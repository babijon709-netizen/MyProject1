// aim/memory.cpp — Такт мемори-аима: абсолютные углы вместо пальца.
//
// Модуль разрезан из прежнего монолита jni/src/main.cpp? Нет — он новый.
// Появился вместе с режимом «Мемори» (вкладка «Аим»): прицел доводится не
// пальцем через uinput, а записью поворота в память игры (esp/aim_mem.cpp).
//
// Почему контроллер получился в разы проще тач-аима. В тач-режиме есть петля с
// задержкой: мы двигаем палец, игра отвечает не в том же кадре, поэтому шаг
// считается по остатку, каким он СТАНЕТ, есть ожидание подтверждения
// (s_ackTimeouts), «шаги в полёте», квант цифровера и обучение град/px. У
// мемори-режима всего этого нет:
//   * запись абсолютная — мы говорим «прицел смотрит на такие-то углы», а не
//     «сдвинься на столько-то пикселей»;
//   * текущие углы известны точно, поэтому ошибка — это разница углов, а не
//     измерение по экрану;
//   * цена ошибки не растёт за кадр: если игра применит нашу запись на кадр
//     позже, следующий шаг просто снова посчитает остаток от того, что реально
//     в памяти (никакого перелёта, потому что шаг — доля ТЕКУЩЕЙ ошибки).
// Поэтому здесь только политика: мёртвая зона, доля ошибки за такт и потолок
// шага. Всё это проверяется стендом tools/aim/run_mem.sh на настоящем коде этого
// файла (искусственная задержка отклика, шум чтения, движение цели).
#include "app/common.h"
#include "aim/memory.h"

namespace {

constexpr float kRadToDeg = 57.29577951f;

// Мёртвая зона. Нижняя граница — не квант ввода (его тут нет), а шум чтения
// углов: у STATE_* это ровно то, что игра положила в поле (шума нет), у
// TRANSFORM и у углов камеры — доли градуса. 0.05° хватает, чтобы не дрожать,
// и не видно глазу (0.05° на 100 м — 9 см, меньше головы).
constexpr float kMinDeadDeg = 0.05f;

// Мёртвая зона «по цели»: 3 см в мире цели. Вблизи она шире шумовой — так
// прицел не дёргается от дыхания цели, — но не шире самой головы.
constexpr float kTargetDeadMeters = 0.03f;

// Потолок шага за такт. Слайдер «Скорость» и так даёт не больше половины
// остатка, а потолок нужен на случай мусорной цели (кость мигнула в другую
// сторону): 12° за кадр — это уже резкий разворот, дальше нельзя.
constexpr float kMaxStepDeg = 12.f;

}  // namespace

bool AimMemoryStep(const AimTarget& target, float dt, float speed, float world_dist,
                   float yaw_now, float pitch_now, float& yaw_out, float& pitch_out) {
    if (!target.valid) return false;
    if (!std::isfinite(yaw_now) || !std::isfinite(pitch_now)) return false;
    if (!std::isfinite(target.yaw) || !std::isfinite(target.pitch)) return false;

    if (!(dt > 0.f) || !std::isfinite(dt)) dt = 1.f / 60.f;
    if (dt > 0.1f) dt = 0.1f;

    // Остаток: цель задана смещением от прицела, значит абсолютные углы цели —
    // это текущие плюс смещение. yaw переводим на короткую дугу: иначе на
    // переходе через ±180 аим развернулся бы «через весь экран».
    float error_yaw = target.yaw;
    while (error_yaw > 180.f) error_yaw -= 360.f;
    while (error_yaw < -180.f) error_yaw += 360.f;
    const float error_pitch = target.pitch;

    // Мёртвая зона: шум чтения плюс 3 см в мире цели.
    float dead = kMinDeadDeg;
    if (world_dist > 1.f && std::isfinite(world_dist)) {
        const float world_dead = atanf(kTargetDeadMeters / world_dist) * kRadToDeg;
        if (world_dead > dead) dead = world_dead;
    }
    if (fabsf(error_yaw) < dead && fabsf(error_pitch) < dead) return false;

    // «Скорость» из меню: 1 — мягко, 10 — вплотную к остатку, ровно та же
    // шкала, что у тач-режима, чтобы переключение режима не меняло привычку.
    // Нижняя граница здесь 0.5, а не 0.25, как у пальца: команда абсолютная, и
    // даже «мягкий» шаг возвращает половину ошибки — плавность сохраняется, а
    // отставание остаётся вдвое от физического минимума, а не вчетверо.
    float slider = speed;
    if (!(slider >= 1.f)) slider = 1.f;
    if (slider > 10.f) slider = 10.f;
    const float per_frame = 0.5f + (slider - 1.f) / 9.f * 0.5f;       // 0.50 .. 1.00
    float share = 1.f - powf(1.f - per_frame, dt * 60.f);             // независимо от fps

    // Верхний предел — вся ошибка за такт. Больше брать нечего: команда
    // абсолютная, и «больше ошибки» — это уже промах в другую сторону.
    //
    // Важное свойство этой шкалы: при share = 1 запись ставит прицел ровно на
    // цель (new = прочитанный угол + ошибка = угол цели), и шум чтения при этом
    // сокращается сам собой. Остаётся только отставание по задержке применения
    // записи: на 40°/с и двух кадрах это 1.3° (замер — tools/aim/mem_loop.cpp),
    // и его добирает упреждение из aim/update.cpp. Меньший share (ползунок вниз)
    // отставание увеличивает как 1/share — это и есть «плавнее».
    if (share > 1.f) share = 1.f;
    if (share < 0.02f) share = 0.02f;

    float step_yaw = error_yaw * share;
    float step_pitch = error_pitch * share;
    if (step_yaw > kMaxStepDeg) step_yaw = kMaxStepDeg;
    if (step_yaw < -kMaxStepDeg) step_yaw = -kMaxStepDeg;
    if (step_pitch > kMaxStepDeg) step_pitch = kMaxStepDeg;
    if (step_pitch < -kMaxStepDeg) step_pitch = -kMaxStepDeg;

    // Шаг меньше мёртвой зоны смысла не имеет: цель и так считается наведённой.
    if (fabsf(step_yaw) < dead * 0.25f && fabsf(step_pitch) < dead * 0.25f) return false;

    yaw_out = yaw_now + step_yaw;
    pitch_out = pitch_now + step_pitch;
    // Тангаж за вертикаль не заводим: за ±90° камера переворачивается, и
    // следующее чтение углов даёт скачок на 180°.
    if (pitch_out > 89.f) pitch_out = 89.f;
    if (pitch_out < -89.f) pitch_out = -89.f;
    while (yaw_out > 180.f) yaw_out -= 360.f;
    while (yaw_out < -180.f) yaw_out += 360.f;
    return true;
}
