#pragma once
// Временной фильтр позиции игрока (ESP): сглаживает одиночные сбои чтения,
// отсекает мусорные отсчёты и подтверждает настоящие телепорты/перелёты.
// Логика чистая (без чтения памяти игры), поэтому покрыта хост-тестом
// jni/tests/test_pos_filter.cpp. game.cpp подаёт монотонное время и
// собирает статистику Obs в журнал.
#include "Vector.h"
#include <cmath>
#include <cstdint>

namespace posfilter {

// Значения подобраны под верхние скорости игры: бег ~8, машина ~50,
// коптер до ~120 м/с. Мусорный отсчёт скачет случайно по карте, а игрок —
// никогда быстрее транспорта.
constexpr double kPosHoldSeconds      = 2.0;   // холд без успешного чтения (время, не кадры!)
constexpr int    kPosJumpConfirmFrames = 3;    // нужен повтор в несколько кадров, чтобы не гасить ESP на шуме
constexpr float  kPosJumpMeters        = 1.5F; // базовый допуск, метры
constexpr float  kPosJumpSpeed         = 28.0F;// плюс м/с на секунду простоя
constexpr float  kPosExtrapolateLimit  = 1.5F; // потолок дорисовки по скорости
constexpr float  kPosJumpSameSpot      = 1.5F; // «та же» подозрительная точка
constexpr float  kTransportMaxSpeed    = 130.0F; // предел связного перелёта

// Подтвережденная фильтром часть трека игрока.
struct State {
    Vec3   drawn{};
    bool   has_drawn = false;
    double drawn_t = 0.0;
    int    hold_frames = 0;   // сколько кадров живём без успешного чтения
    Vec3   jump{};            // подозрительный отсчёт, ждущий подтверждения
    bool   has_jump = false;
    int    jump_frames = 0;
    // Последний шаг подозрительной ломаной (транспортный перелёт): направление
    // и длина, чтобы следующий отсчёт признать продолжением полёта.
    Vec3   jump_step{};
    float  jump_step_len = 0.0F;
    bool   have_jump_step = false;
};

// Наблюдатель: инкременты, которые game.cpp периодически сводит в журнал.
struct Obs {
    int susp = 0;             // отсчётов за пределами правдоподобия
    int conf = 0;             // из них подтвердилось как настоящий скачок
    uint64_t last = 0;        // последний игрок с подтверждением
    float last_d = 0.0F;      // на сколько метров
    bool vel_reset = false;   // скачок подтверждён: скоростная оценка невалидна
};

// now — монотонные секунды вызывающего. vel — измеренная скорость игрока
// (для экстраполяции на сбойном кадре). pos in/out: сырое -> рисуемое.
inline bool filter(State& track, bool read_ok, Vec3& pos, const Vec3& vel,
                    double now, Obs& obs) {
    if (!track.has_drawn) {
        if (!read_ok) return false;
        track.drawn = pos; track.drawn_t = now; track.has_drawn = true;
        track.hold_frames = 0; track.has_jump = false; track.jump_frames = 0;
        track.have_jump_step = false;
        return true;
    }

    double dt = now - track.drawn_t;
    if (!(dt > 0.0)) dt = 0.0;
    if (dt > 0.25) dt = 0.25;
    const float fdt = (float)dt;

    // Последняя принятая позиция, продвинутая по измеренной скорости; дальше
    // метра-полутора не продлеваем, чтобы не унести бокс самим фильтром.
    Vec3 predicted = track.drawn;
    {
        const float mx = vel.x * fdt, my = vel.y * fdt, mz = vel.z * fdt;
        const float step2 = mx * mx + my * my + mz * mz;
        const float limit2 = kPosExtrapolateLimit * kPosExtrapolateLimit;
        const float scale = (step2 > limit2 && step2 > 0.0F) ? sqrtf(limit2 / step2) : 1.0F;
        predicted.x += mx * scale; predicted.y += my * scale; predicted.z += mz * scale;
    }

    if (!read_ok) {
        ++track.hold_frames;
        // Важно не гасить ESP сразу на временном окне в 1s: некоторые игроки
        // "проваливаются" на 1–2 секунды из-за кратковременных read-gap, а это
        // выглядит как рандомное пропадание в бою. Держим последнее валидное
        // место до expiry, а потом уже сбрасываем трек.
        if (now - track.drawn_t > kPosHoldSeconds) {
            track.has_drawn = false;
            return false;
        }
        pos = predicted;
        return true;
    }

    const float dx = pos.x - predicted.x, dy = pos.y - predicted.y, dz = pos.z - predicted.z;
    const float dev = sqrtf(dx * dx + dy * dy + dz * dz);
    const float limit = kPosJumpMeters + kPosJumpSpeed * fdt;
    if (std::isfinite(dev) && dev <= limit) {
        track.drawn = pos; track.drawn_t = now;
        track.hold_frames = 0; track.has_jump = false; track.jump_frames = 0;
        track.have_jump_step = false;
        return true;
    }

    ++obs.susp;    // рваный отсчёт (подтвердится ли — см. ниже)
    // Скачок за пределы правдоподобия. Два способа подтвердить:
    //   1) та же точка подряд — настоящий телепорт (иконка «стоит на месте»);
    //   2) связная ломаная — пассажир транспорта: каждый кадр новая точка
    //      (на коптере 60-100+ м/с в точку «та же» не вернуться никогда),
    //      но шаг за кадр не быстрее транспорта и примерно в ту же сторону.
    // Мусор (чужие координаты из пула/полупрочитанные слова) летает по карте
    // случайно и не проходит ни одну из двух проверок.
    bool same = false;
    Vec3 pending_step{};
    float pending_len = 0.0F;
    bool pending_valid = false;
    if (track.has_jump) {
        const float jx = pos.x - track.jump.x, jy = pos.y - track.jump.y, jz = pos.z - track.jump.z;
        const float step2 = jx * jx + jy * jy + jz * jz;
        same = std::isfinite(step2) && step2 < kPosJumpSameSpot * kPosJumpSameSpot;
        const float step = std::isfinite(step2) ? sqrtf(step2) : 0.0F;
        const float step_limit = kPosJumpSameSpot + kTransportMaxSpeed * fdt;
        if (step > 0.0001F && step <= step_limit) {
            pending_step = {jx, jy, jz}; pending_len = step; pending_valid = true;
            if (!same && !track.have_jump_step) {
                // Первый ограниченный шаг уже доказывает, что новая точка не
                // одиночный выброс. Он задаёт курс; следующий кадр обязан его
                // подтвердить направлением, прежде чем трек будет принят.
                same = true;
            } else if (!same && track.jump_step_len > 0.0001F) {
                const float dotn = (jx * track.jump_step.x + jy * track.jump_step.y +
                                    jz * track.jump_step.z) / (step * track.jump_step_len);
                if (dotn > 0.25F) same = true;  // тот же курс — продлеваем ломаную
            }
        }
    }
    if (same) {
        ++track.jump_frames;
        if (pending_valid) {
            track.jump_step = pending_step;
            track.jump_step_len = pending_len;
            track.have_jump_step = true;
        }
        track.jump = pos;
    } else {
        track.jump = pos;
        track.jump_frames = 1;
        // Шаг запоминаем и при «не совпало»: он станет курсом для следующего
        // отсчёта. Без этого ломаная никогда не начиналась — транспортный
        // бокс зависал на месте посадки до полной остановки игрока.
        if (pending_valid) {
            track.jump_step = pending_step;
            track.jump_step_len = pending_len;
            track.have_jump_step = true;
        } else {
            track.have_jump_step = false;
        }
    }
    track.has_jump = true;

    if (track.jump_frames >= kPosJumpConfirmFrames) {
        track.drawn = pos; track.drawn_t = now;
        track.has_jump = false; track.jump_frames = 0; track.hold_frames = 0;
        track.have_jump_step = false;
        // Скорость через телепорт не измеряется — вызывающий обязан сбросить
        // свою оценку скорости, иначе упреждение будет на пару кадров смотреть
        // в старую сторону.
        obs.conf += 1; obs.last_d = dev; obs.vel_reset = true;
        return true;
    }
    pos = predicted;
    return true;
}

} // namespace posfilter
