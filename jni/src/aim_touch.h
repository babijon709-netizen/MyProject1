#pragma once
//
// Контроллер тач-аима: остаток в градусах -> сдвиг пальца в пикселях.
//
// Вынесен из aim.cpp целиком (учёт «в полёте», мёртвая зона, полоса дрожи,
// доля шага, потолки, сетка цифровера) по двум причинам:
//
//   1. Стенд tools/aim/touch_sim.cpp гоняет ТОТ ЖЕ код, что идёт на устройство.
//      Раньше модель петли переписывала контроллер своими словами, и «проверено
//      на стенде» означало «проверено на копии».
//   2. Params.soft переключает прежнюю и новую математику, поэтому A/B на стенде
//      — это не два разных файла, а один флаг.
//
// Слой касаний (Touch_Down/Move/Up, перенос пальца, границы зоны) остался в
// aim.cpp: контроллер не знает ни про устройство, ни про поток оверлея.
//
// ---- Что лечит новая математика (soft = true) ------------------------------
//
// Жалоба: «перелетает цель» и «двигается рывками».
//
// Рывки — это форма КОМАНДЫ, а не остатка. Камера в любом случае применяет ввод
// пачками раз в несколько кадров игры (труба ввода Android копится и выливается
// разом), поэтому остаток пилит даже когда камера идёт ровно за целью. А вот
// команда у прежней ветки была ступенчатой по четырём причинам, и все четыре
// убраны:
//
//   * touch_jitter_gate — жёсткий порог: остаток в полосе дрожи даёт ровно 0,
//     на третьем такте — полную величину. Палец идёт «стойка-прыжок-стойка».
//     Заменён непрерывным спадом jitter_taper с отпуском по порогу дрейфа.
//   * доля шага k сжималась до пола 0.40 и обратно скачком за один такт.
//     Заменена S-кривой soft_envelope с нулевой производной на концах.
//   * приказ на такт менялся между 0 и потолком. Теперь команду пропускает
//     ограничитель скорости SoftSlew: последовательность шагов непрерывна.
//   * команда шла одним пакетом, и игра применяла её одним куском. split_substeps
//     дробит её на 4 записи uinput, которые игра разбирает по разным кадрам.
//
// Перелёт. Прежний мягкий старт — линейная доля 0.3 -> 1.0 за 0.2 с — режет
// ПЕРВЫЙ шаг, но не профиль подхода: за время разгона доля выходит на полную и
// приказ на последних градусах прилетает уже после закрытия остатка. Здесь три
// вещи вместо одной:
//
//   1. Время разгона привязано к остатку (soft_envelope_time): на 30 град это
//      полные 0.22 с, на 1 град — 0.05 с. Дальний захват не стал медленнее,
//      короткий довод не тащится через весь разгон.
//   2. Доля согласована с задержкой трубы: k*D держится около kLoopGain.
//      Прежние 0.30 на такт при трубе 0.27-0.33 с давали k*D = 1.0-1.2 — ровно
//      границу устойчивости петли с мёртвым временем, откуда звон на захвате.
//   3. Заказ, который игра ещё не отработала, вычитается из остатка по возрасту
//      приказов и замеренной задержке, а приказ этого такта дополняется
//      упреждением на ту же задержку: цель за D секунд уйдёт, и заказ закрывает
//      остаток с учётом этого ухода.
//
// ---- Что здесь НЕ делается, и почему ---------------------------------------
//
// Пробованы и сняты со стенда (tools/aim/touch_sim.cpp, цифры — оттуда):
//
//   * Полный предиктор Смита: команда считается от остатка, предсказанного на
//     момент прилёта приказа, а не от текущего. Дал перелёт 3.2-10.1 град и
//     7-45 реверсов на захвате. Упреждение, ограниченное долей остатка
//     (kLeadFrac), даёт то же улучшение без права уводить приказ за ноль на
//     ошибке оценки.
//   * Фильтр остатка постоянной, равной времени трубы. Вносит фазовый сдвиг на
//     те же ~D секунд, а петля уже имеет мёртвое время D: перелёт 0.3 -> 2.8°,
//     реверсов 1 -> 69.
//   * Оценка летящего заказа как «поглощение, ограниченное окном приказов».
//     На ведении цели занижала заказ в 4-8 раз (окно держит 0.55-0.75 с, а
//     не применены только последние 0.27 с) — контроллер заказывал поверх
//     летящего и перелетал кость.
//   * Оценка задержки по возрасту приказов. «Возраст старшего приказа в момент
//     первого отклика» почти всегда равен одному такту (0.083 с вместо 0.27 с):
//     приказы ставятся каждый такт. «Взвешенный средний возраст заказов в окне»
//     наоборот уплывает до 0.55 с. Рабочая оценка — очередь: заказ списывается,
//     когда камера реально повернулась, и его возраст на этот момент и есть
//     задержка (стенд: 0.29-0.32 с при трубе 4 кадра по 1/15 с).
//   * Оценка скорости цели по наклону остатка, очищенного от поглощения. Во
//     время захвата остаток падает от нашей же камеры, и наклон выходил
//     -30..-50°/с при настоящих -12°/с: упреждение вставало на предел и уводило
//     приказ за ноль. Рабочий сигнал — остаток плюс поток камеры (собственное
//     движение цели), и только когда летящий заказ мал, окно полное, а разброс
//     остатка в окне невелик.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "aim_control.h"

namespace aim_touch {

// Кадронезависимая доля сглаживания. Копия manes_aim_source::frame_gain:
// заголовок не тянет его, чтобы стенд собирался без остального дерева.
inline float manes_frame_gain(float smooth_fraction, float dt) {
    if (!aim_control::finite(smooth_fraction) || !aim_control::finite(dt) || dt <= 0.0f)
        return 0.0f;
    if (smooth_fraction < 0.0f) smooth_fraction = 0.0f;
    if (smooth_fraction > 1.0f) smooth_fraction = 1.0f;
    if (smooth_fraction == 0.0f) return 0.0f;
    if (smooth_fraction == 1.0f) return 1.0f;
    return 1.0f - std::pow(1.0f - smooth_fraction, dt * 60.0f);
}

// ---- мягкий старт: S-кривая вместо линейного 0.3 -> 1.0 ---------------------
inline float soft_envelope(float t, float duration) {
    if (!(t > 0.0f) || !aim_control::finite(t)) return 0.0f;
    if (!aim_control::finite(duration) || duration < 0.02f) duration = 0.02f;
    if (duration > 0.6f) duration = 0.6f;
    const float x = t / duration;
    if (x >= 1.0f) return 1.0f;
    return x * x * (3.0f - 2.0f * x);   // smoothstep: нулевая производная на концах
}

// Время разгона по остатку: короткий довод не ждёт полного разгона, дальний
// захват выходит на потолок. rate — скорость сведения, град/с.
inline float soft_envelope_time(float error_degrees, float rate_deg_per_s) {
    if (!aim_control::finite(error_degrees)) return 0.0f;
    if (!(rate_deg_per_s > 1.0f) || !aim_control::finite(rate_deg_per_s))
        rate_deg_per_s = 60.0f;
    const float t = std::fabs(error_degrees) / rate_deg_per_s;
    return std::min(0.22f, std::max(0.05f, 1.6f * t));
}

// ---- ограничитель скорости команды ------------------------------------------
// Убирает ступеньки в последовательности приказов: выход не может прыгнуть с 0
// на потолок за такт, он доезжает за accel_limit * dt. Знак меняется мгновенно
// (через ноль), иначе на смене цели палец ехал бы мимо.
struct SoftSlew {
    float out = 0.0f;

    void reset() { out = 0.0f; }

    float apply(float target, float dt, float accel_limit) {
        if (!aim_control::finite(target)) target = 0.0f;
        if (!(dt > 0.0f) || !aim_control::finite(dt)) dt = 1.0f / 60.0f;
        if (dt > 0.1f) dt = 0.1f;
        if (!(accel_limit > 0.0f) || !aim_control::finite(accel_limit)) {
            out = target;
            return out;
        }
        if (target != 0.0f && out != 0.0f && (target > 0.0f) != (out > 0.0f))
            out = 0.0f;                 // реверс: стартуем от нуля, не от прежней скорости
        const float budget = accel_limit * dt;
        const float d = target - out;
        out += (std::fabs(d) <= budget) ? d : (d > 0.0f ? budget : -budget);
        if (!aim_control::finite(out)) out = 0.0f;
        return out;
    }
};

// Непрерывная замена touch_jitter_gate: внутри полосы доля растёт линейно от 0
// до 1, снаружи — 1. Ноль только при смене знака (streak), то есть дрожь кости
// по-прежнему не двигает палец, но медленный дрейф разгоняется плавно, а не
// включается рывком на третьем такте.
//
// Отпуск по порогу дрейфа обязателен: streak набирается только однонаправленным
// остатком, то есть настоящим движением цели, а не дрожью. Без отпуска спад
// доли держался бы и на ведении — команда у цели уменьшена, след за целью
// растягивался (стенд «ведение 8°/с + дрожь»: хвост рысканья 0.19 -> 1.48°).
inline float jitter_taper(float err, int& streak, float band, int required_frames,
                          int release_streak) {
    if (!aim_control::finite(err) || !(band > 0.0f) || required_frames < 1) {
        streak = 0;
        return 0.0f;
    }
    if (err == 0.0f) {
        streak = 0;
        return 0.0f;
    }
    const float m = std::fabs(err);
    if (m >= band) {
        streak = 0;
        return 1.0f;
    }
    const int sign = err > 0.0f ? 1 : -1;
    streak = (streak * sign > 0) ? streak + sign : sign;
    if (streak * sign < required_frames) return 0.0f;
    if (release_streak > required_frames && streak * sign >= release_streak) return 1.0f;
    return std::min(1.0f, m / band);
}

// Разбиение команды такта на несколько пакетов касания. Нужно там, где игра
// обрабатывает ввод чаще, чем тикает оверлей: при 12 fps оверлея и 60 fps игры
// один бросок в 40 px камера применяет целиком за кадр, а четыре по 10 px
// успевают разойтись по разным кадрам. Возвращает НАКОПЛЕННЫЕ позиции.
inline int split_substeps(float dx, float dy, float* outX, float* outY, int maxParts) {
    if (!outX || !outY || maxParts < 1) return 0;
    const float mag = std::max(std::fabs(dx), std::fabs(dy));
    if (!aim_control::finite(mag) || mag <= 0.0f) return 0;
    int parts = (int)std::ceil(mag / 6.0f);   // ~6 px на пакет: мельче — дробление uinput без пользы
    if (parts < 1) parts = 1;
    if (parts > maxParts) parts = maxParts;
    if (parts == 1) {
        outX[0] = dx;
        outY[0] = dy;
        return 1;
    }
    for (int i = 0; i < parts; ++i) {
        const float f = (float)(i + 1) / (float)parts;
        outX[i] = dx * f;
        outY[i] = dy * f;
    }
    return parts;
}

// ---- учёт заказа «в полёте» -------------------------------------------------
// Игра отрабатывает касание не в этом такте: труба ввода Android держит 2-8
// кадров игры. Без учёта летящего заказа контроллер шлёт ту же поправку
// повторно, и по прилёте обоих камера перелетает цель (маятник).
struct Flight {
    // Заказ без отклика дольше этого времени — потерялся (палец перехвачен,
    // экран перекрыт). Порог обязан перекрывать худшую задержку трубы: на
    // 12-15 fps игры это 0.5-0.7 с.
    static constexpr float kStallTime = 1.00f;
    // Предел задержки трубы, с: дольше этого приказ считать летящим нельзя.
    static constexpr float kDelayMax = 1.2f;

    struct Order { float yawDeg, pitchDeg, age; };

    // Очередь заказов на списание. Первый в очереди — самый старый, то есть
    // тот, что игра применит первым: поток камеры списывает очередь с головы.
    // Величины — в градусах камеры (см. place), знак хранится отдельно: камера
    // применяет алгебраическую сумму висящего ввода, поэтому очередь обязана
    // быть однонаправленной (см. pushFifo).
    struct Queued { float mag; float placed; float sign; };

    std::vector<Order> orders;
    std::vector<Queued> fifoYaw, fifoPitch;
    float delay = 0.0f;          // замеренная задержка трубы, с
    float fifoTime = 0.0f;       // часы очереди, с
    float absorbYaw = 0.0f, absorbPitch = 0.0f;
    float stallYaw = 0.0f, stallPitch = 0.0f;
    float prevRawYaw = 0.0f, prevRawPitch = 0.0f;
    bool havePrevRaw = false;
    bool flowOk = false;
    // Остаток скакнул от смены цели/кости, а не от движения камеры.
    int skipFall = 0, skipCross = 0;

    void reset() { *this = Flight{}; }

    // big = true — другой игрок: остаток прыгнул на десятки градусов, и в
    // режиме без потока разница через такую пропасть списывает весь летящий
    // заказ. big = false — та же цель, другая кость: шаг в доли градуса,
    // списание по падению остаётся. Перескок нуля в обоих случаях сделала не
    // камера, а смена цели, поэтому обнулять очередь и поглощение нельзя:
    // вместе с ними теряется замер задержки.
    void onResidualStep(bool big) {
        skipCross = 1;
        if (big) skipFall = 1;
    }

    // Задержка трубы по факту: заказ, применённый камерой в этом такте, лежал в
    // очереди столько-то. Средневзвешенный по величине заказа возраст головы
    // очереди, сглаженный EMA.
    //
    // Прежние две оценки не годились. «Возраст самого старого заказа» в момент
    // первого отклика почти всегда равен одному такту: приказы ставятся каждый
    // такт (стенд: 0.083 с вместо 0.27 с). «Величина-взвешенный средний возраст
    // всех заказов в окне» наоборот уплывает вверх вместе с окном (0.55-0.75 с):
    // на ведении цели приказ мелкий и живёт в окне дольше настоящего (стенд:
    // 0.49-0.55 с), а завышенная задержка раздувает и оценку летящего заказа,
    // и упреждение.
    void measureDelay(std::vector<Queued>& fifo, float applied) {
        if (!(std::fabs(applied) > 1.0e-4f) || fifo.empty()) return;
        // Отклик противоположного знака висящий заказ не списывает.
        if (applied * fifo.front().sign < 0.0f) return;
        double wsum = 0.0, w = 0.0;
        float left = std::fabs(applied);
        while (left > 1.0e-4f && !fifo.empty()) {
            const Queued& q = fifo.front();
            const float take = std::min(q.mag, left);
            wsum += take * (double)(fifoTime - q.placed);
            w += take;
            left -= take;
            if (take >= q.mag - 1.0e-4f) fifo.erase(fifo.begin());
            else { fifo.front().mag = q.mag - take; break; }
        }
        if (w <= 0.0) return;
        const float sample = (float)(wsum / w);
        if (!(sample > 0.0f) || !aim_control::finite(sample)) return;
        delay = (delay > 0.0f) ? 0.7f * delay + 0.3f * sample : sample;
        if (delay > kDelayMax) delay = kDelayMax;
    }

    // Списание заказа по факту отработки.
    //   flowOk  — виден поток камеры (aim_camera_flow): камера повернулась на X
    //             град — ровно X заказа она съела, куда бы ни шла цель.
    //   иначе   — запасной признак: остаток упал (дуэль 1 на 1, боксов < 3).
    void update(float dt, float rawYaw, float rawPitch, bool flowOkTick,
                float flowYaw, float flowPitch) {
        if (!(dt > 0.0f) || !aim_control::finite(dt)) dt = 1.0f / 60.0f;
        if (dt > 0.1f) dt = 0.1f;
        flowOk = flowOkTick;
        fifoTime += dt;
        if (havePrevRaw) {
            if (flowOk || skipFall > 0) {
              if (flowOk) {
                const float wasYaw = absorbYaw, wasPitch = absorbPitch;
                drain(absorbYaw, flowYaw);
                drain(absorbPitch, flowPitch);
                if (absorbYaw != wasYaw) stallYaw = 0.0f;
                if (absorbPitch != wasPitch) stallPitch = 0.0f;
                // Камера повернулась — ровно столько заказа она и съела.
                measureDelay(fifoYaw, flowYaw);
                measureDelay(fifoPitch, flowPitch);
              }
            } else {
                const float wasYaw = absorbYaw, wasPitch = absorbPitch;
                drainByFall(absorbYaw, prevRawYaw - rawYaw, stallYaw);
                drainByFall(absorbPitch, prevRawPitch - rawPitch, stallPitch);
                measureDelay(fifoYaw, wasYaw - absorbYaw);
                measureDelay(fifoPitch, wasPitch - absorbPitch);
            }
            // Остаток перескочил ноль — камера уже дальше цели: висящий заказ
            // толкнёт её ещё дальше. Обнуляем не дожидаясь прилёта. На смене
            // цели ноль перескакивает сама цель, и обнуление стоило бы очереди
            // и замера задержки.
            if (skipCross == 0) {
                if (prevRawYaw * rawYaw < 0.0f) { absorbYaw = 0.0f; fifoYaw.clear(); }
                if (prevRawPitch * rawPitch < 0.0f) { absorbPitch = 0.0f; fifoPitch.clear(); }
            }
        }
        if (skipFall > 0) --skipFall;
        if (skipCross > 0) --skipCross;
        prevRawYaw = rawYaw;
        prevRawPitch = rawPitch;
        havePrevRaw = true;

        if (absorbYaw != 0.0f) { stallYaw += dt; if (stallYaw > kStallTime) { absorbYaw = 0.0f; fifoYaw.clear(); } }
        if (absorbPitch != 0.0f) { stallPitch += dt; if (stallPitch > kStallTime) { absorbPitch = 0.0f; fifoPitch.clear(); } }
        ageFifo(fifoYaw, dt);
        ageFifo(fifoPitch, dt);

        // Клэмпа «заказ не больше текущего остатка» здесь нет, и это
        // принципиально. Он ломал поглощение как оценку полёта: остаток на
        // ведении цели равен v_цели * задержка (1.5-3°), заказ в трубе — 2-3°,
        // и клэмп каждый такт срезал поглощение до остатка. Хуже того, он
        // срабатывал в том же такте, что и списание по потоку, и возвращал
        // списанное обратно: контроллер вставал посреди захвата и ждал, пока
        // окно приказов опустеет (стенд trim 1.4: рысканье залипало на 8.6° от
        // цели — на устройстве это «аим не доводит» и бросок одним куском
        // вслед за паузой). От перелёта отвечают списание по потоку и
        // обнуление заказа при перескоке остатка через ноль (выше).
    }

    // Заказ, который игра ещё НЕ отработала: сумма величин заказов моложе
    // задержки трубы. Прежняя эвристика («поглощение, ограниченное окном
    // заказов») на ведении цели занижала летящий заказ в 4-8 раз: окно держит
    // заказы за 0.55-0.75 с, а применены уже все, кроме последних 0.27 с.
    // Занижение означает повторный приказ поверх летящего — перелёт.
    float inflightMag(float delaySec) const {
        double sum = 0.0;
        for (const auto& o : orders)
            if (o.age < delaySec)
                sum += std::fabs((double)o.yawDeg) + std::fabs((double)o.pitchDeg);
        const float v = (float)sum;
        return aim_control::finite(v) ? v : 0.0f;
    }

    // Средневзвешенный возраст приказов в трубе (вес — величина заказа):
    // насколько в среднем опаздывает заказ к моменту прилёта.
    float meanAge() const {
        double sum = 0.0, w = 0.0;
        for (const auto& o : orders) {
            const double m = std::fabs((double)o.yawDeg) + std::fabs((double)o.pitchDeg);
            sum += m * o.age;
            w += m;
        }
        if (w <= 0.0) return 0.0f;
        const float age = (float)(sum / w);
        return aim_control::finite(age) ? age : 0.0f;
    }

    void age(float dt, float responseWindow) {
        for (auto& o : orders) o.age += dt;
        orders.erase(std::remove_if(orders.begin(), orders.end(),
                     [responseWindow](const Order& o) { return o.age >= responseWindow; }),
                     orders.end());
    }

    // Заказ ставится в ГРАДУСАХ КАМЕРЫ: остаток, поток камеры и падение
    // остатка — все в них же. Вызывающий считает приказ в рабочих градусах
    // (шаг пальца × коэффициент с подтяжкой), поэтому пересчёт делается сразу.
    // Подтяжка до 1.4 раздувала приказ: вычитание летящего заказа снимало с
    // остатка на 40% больше, чем камера выдаст на самом деле, а списывание
    // поглощения потоком таяло в 1.4 раза медленнее (стенд «trim 1.4»: хвост
    // рысканья 8.8° вместо 0.3°, захват 2.9 с вместо 1.6 с).
    void place(float yawWork, float pitchWork, float ratioYaw, float ratioPitch) {
        if (!(ratioYaw >= 1.0f) || !aim_control::finite(ratioYaw)) ratioYaw = 1.0f;
        if (!(ratioPitch >= 1.0f) || !aim_control::finite(ratioPitch)) ratioPitch = 1.0f;
        const float yawDeg = yawWork / ratioYaw;
        const float pitchDeg = pitchWork / ratioPitch;
        if (!aim_control::finite(yawDeg) || !aim_control::finite(pitchDeg)) return;
        orders.push_back({yawDeg, pitchDeg, 0.0f});
        absorbYaw += yawDeg;     stallYaw = 0.0f;
        absorbPitch += pitchDeg; stallPitch = 0.0f;
        pushFifo(fifoYaw, yawDeg);
        pushFifo(fifoPitch, pitchDeg);
    }

private:
    // Заказ противоположного знака ГАСИТ уже висящий в трубе, а не ложится
    // рядом: камера применит алгебраическую сумму ввода. Без гашения после
    // смены цели голова очереди держала заказы прошлой цели (в другую
    // сторону), их возраст списывался на новый отклик, и замер задержки уползал
    // 0.32 -> 0.70 с. Дальше по кругу: доля шага падала до пола, окно летящего
    // заказа раздувалось, команда укорачивалась, сведение вставало (стенд
    // «смена цели без потока»: хвост рысканья 4.2° вместо 0.3°).
    void pushFifo(std::vector<Queued>& fifo, float v) {
        if (!(std::fabs(v) > 1.0e-5f)) return;
        float mag = std::fabs(v);
        const float sign = (v > 0.0f) ? 1.0f : -1.0f;
        while (mag > 1.0e-5f && !fifo.empty() && fifo.front().sign != sign) {
            Queued& q = fifo.front();
            const float take = std::min(q.mag, mag);
            q.mag -= take;
            mag -= take;
            if (q.mag <= 1.0e-5f) fifo.erase(fifo.begin());
        }
        if (mag > 1.0e-5f) fifo.push_back({mag, fifoTime, sign});
    }

    // Заказ, который никто не применил дольше предела задержки, — потерянный
    // ввод (палец перехвачен игрой, экран перекрыт).
    void ageFifo(std::vector<Queued>& fifo, float dt) {
        (void)dt;
        while (!fifo.empty() && fifoTime - fifo.front().placed > kDelayMax)
            fifo.erase(fifo.begin());
    }

public:

    // Что вычитается из остатка: заказ, который игра ещё не отработала.
    //
    // Когда поток камеры НЕ виден (дуэль 1 на 1, боксов меньше трёх), списать
    // заказ по факту нечем, и вместо расчётного возраста берётся поглощение —
    // оно сливается падением остатка и потому на длинной трубе даёт не меньше,
    // чем расчёт.
    void subtract(float& errYaw, float& errPitch, float delaySec) const {
        auto pick = [](float byAge, float byAbsorb) {
            return (std::fabs(byAbsorb) > 1.0e-4f) ? byAbsorb : byAge;
        };
        const float inflYaw = pick(inFlightAxis(delaySec, true), absorbYaw);
        const float inflPitch = pick(inFlightAxis(delaySec, false), absorbPitch);
        // Насыщение, а не вычитание: оценка летящего заказа ограничивается
        // остатком, поэтому err не меняет знак от одного только учёта полёта и
        // отдельная защита знака не нужна. Вычитание без насыщения на ведении
        // цели давало err = raw - inFlight = v*D - v*D = 0 каждый такт: приказ
        // не выдавался вовсе, цель уходила, остаток вырастал, приказ
        // выдавался — и так по кругу (стенд: хвост рысканья 4.5 -> 8.7°).
        // С насыщением команда равна дефициту летящего заказа: сколько ещё не
        // заказано из того, что нужно к моменту прилёта.
        errYaw = saturate(errYaw, inflYaw);
        errPitch = saturate(errPitch, inflPitch);
    }

private:
    // Сумма ЗНАКОВАЯ: камера применит алгебраическую сумму всего, что висит в
    // трубе. Сумма модулей со знаком последнего приказа годилась, пока все
    // заказы шли в одну сторону, и ломалась на смене цели: заказы прошлой цели
    // (в другую сторону) прибавлялись по величине к новым, оценка летящего
    // заказа вырастала вдвое-втрое и насыщение обнуляло команду вовсе — прицел
    // вставал и ждал, пока труба опустеет (стенд «смена цели без потока»: хвост
    // рысканья 4.7° вместо 0.5°).
    float inFlightAxis(float delaySec, bool yawAxis) const {
        double sum = 0.0;
        for (const auto& o : orders) {
            if (o.age >= delaySec) continue;
            sum += (double)(yawAxis ? o.yawDeg : o.pitchDeg);
        }
        const float v = (float)sum;
        return aim_control::finite(v) ? v : 0.0f;
    }

public:

    static float minmag(float a, float b) { return std::fabs(a) < std::fabs(b) ? a : b; }

    // raw - inflight, но не дальше нуля: летящий заказ не может «закрыть»
    // больше, чем есть в остатке.
    static float saturate(float raw, float inflight) {
        const float e = raw - inflight;
        return (raw * e < 0.0f) ? 0.0f : e;
    }

    static void drain(float& absorb, float flow) {
        if (absorb > 0.0f && flow > 0.0f)
            absorb = (flow >= absorb) ? 0.0f : absorb - flow;
        else if (absorb < 0.0f && flow < 0.0f)
            absorb = (-flow >= -absorb) ? 0.0f : absorb - flow;
    }

    static void drainByFall(float& absorb, float fall, float& stall) {
        if (fall > 0.0f && absorb > 0.0f) {
            absorb = (fall >= absorb) ? 0.0f : absorb - fall;
            stall = 0.0f;
        } else if (fall < 0.0f && absorb < 0.0f) {
            absorb = (-fall >= -absorb) ? 0.0f : absorb - fall;
            stall = 0.0f;
        }
    }
};

// ---- параметры такта --------------------------------------------------------
struct Params {
    float screen_height = 1080.0f;
    float units_per_px = 1.0f;   // Touch_DeviceUnitsPerPixel
    float base_gain = 0.10f;     // коэффициент из настройки чувствительности, град/px
    float trim = 1.0f;           // подтяжка коэффициента замером (>= 1.0, см. trimTo в aim.cpp)
    float trim_pitch = 1.0f;     // то же по тангажу: оси учатся независимо
    bool  gain_from_sens = true;
    bool  learned_yaw = false, learned_pitch = false;
    bool  lsq_ready_yaw = false, lsq_ready_pitch = false;
    // Коэффициент известен ТОЛЬКО по настройке чувствительности (ни замера
    // камеры, ни оценки по реакции прицела). Тогда рабочий коэффициент может
    // быть завышен подтяжкой, и потолки шага обязаны считаться в градусах
    // камеры, а не в рабочих: иначе потолок пропускает больше, чем камера
    // доворачивает, и захват не сходится (стенд: trim 1.4, цель не захвачена).
    bool  estimate_only = false;
    float speed = 5.0f;          // слайдер 1..10
    float deg_per_px = 0.055f;   // град на пиксель экрана (проекция)
    float world_dist = 20.0f;    // м до кости
    bool  have_camera_axis = false;
    bool  soft = true;           // false — прежняя математика (для A/B на стенде)
    int   substeps = 4;          // пакетов касания на такт (1 — один бросок)
};

enum Action : int {
    kHold = 0,     // палец не двигаем (Touch_Move той же точкой)
    kMove = 1,     // dx/dy — сдвиг пальца в пикселях оверлея
};

struct Output {
    Action action = kHold;
    float dx = 0.0f, dy = 0.0f;           // суммарный сдвиг такта, px
    int parts = 1;                        // на сколько пакетов разбить
    float partX[4] = {}, partY[4] = {};   // накопленные позиции пакетов
    float err_yaw = 0.0f, err_pitch = 0.0f;
    float k_gain = 0.0f;
    float envelope = 1.0f;
};

struct Controller {
    // Полоса «дрожь кости» у цели и порог устойчивого дрейфа в ней.
    static constexpr float kJitterBand = 0.45f;
    static constexpr int   kDriftStreak = 2;
    // Порог отпуска спада: столько тактов однонаправленного остатка внутри
    // полосы — это уже не дрожь, а ведение цели, доля возвращается к 1.
    static constexpr int   kDriftRelease = 4;
    // Прежняя доля шага у цели: спад с 2.5 град до пола 0.40.
    static constexpr float kRampBand = 2.5f;
    static constexpr float kRampMin = 0.40f;
    // Устойчивый предел доли шага при запаздывающем касании: петля
    // err(t+1) = err(t) - k*err(t-1) расходится при k -> 1 (|z| = sqrt(k)),
    // 0.30 даёт затухание за 2-3 такта.
    static constexpr float kStableFraction = 0.30f;
    // Скорость сведения, град/с, на концах слайдера: задаёт время разгона
    // мягкой ветки (soft_envelope_time) и потолок скорости команды.
    static constexpr float kRateMin = 45.0f;
    static constexpr float kRateMax = 240.0f;
    // Ускорение ограничителя команды: во сколько раз потолок скорости в секунду.
    // 8 — это разгон от нуля до потолка за 1/8 с: достаточно быстро, чтобы не
    // есть скорость захвата, и достаточно медленно, чтобы команда не прыгала.
    static constexpr float kSlewAccel = 8.0f;
    // Петля «остаток -> заказ -> камера через D секунд» расходится, когда
    // произведение коэффициента на задержку подходит к единице: err(t+1) =
    // err(t) - k*err(t-D) даёт корни на единичной окружности при k*D = 1.
    // Прежний коэффициент (доля 0.30 на такт 0.083 с = 3.6 1/с) при трубе
    // 0.27-0.33 с давал k*D = 1.0-1.2 — ровно границу, откуда звон на захвате
    // и пила на ведении цели. Цель k*D ~ 0.5: петля затухает за 2-3 такта, а
    // скорость сведения добирается упреждением и потолком шага.
    static constexpr float kLoopGain = 0.7f;
    // Пол коэффициента, 1/с: ниже него захват ползёт даже при длинной трубе.
    static constexpr float kGainMin = 1.2f;
    // Предел задержки, по которой считается коэффициент: длиннее 1 с труба
    // не бывает, а замер на провале ФПС иначе уронил бы коэффициент до нуля.
    static constexpr float kGainDelayMax = 1.0f;
    // Окно оценки собственной скорости цели, секунд, и предел в тактах.
    //
    // Сигнал — остаток, очищенный от заказа в трубе (raw - absorb): это
    // положение цели относительно камеры с поправкой на то, что камера ещё
    // довернёт. Секунда окна — это четыре-пять применений ввода при трубе
    // 0.27 с: на коротком окне пила применения не усредняется и наклон
    // оказывается шумом (стенд «ведение 8°/с»: окно 5 тактов давало
    // -3.6..+3.9 град/с вместо +8).
    static constexpr float kTargetWindowTime = 1.0f;
    static constexpr int   kTargetWindowMax = 32;
    // Меньше этой скорости упреждение не выдаётся: ниже шума замера.
    static constexpr float kTargetRateMin = 0.8f;
    // Ограничить СУММАРНЫЙ заказ в трубе (чтобы бросок камеры за пачку ввода не
    // превышал потолка команды за такт) пробовали: бросок падает 12.1 -> 4.2°,
    // но команда начинает хлопать между нулём и остатком места, рывок пальца
    // растёт 9.2 -> 21.8 px, а захват 30° замедляется 1.33 -> 2.33 с. Не годится:
    // плавность команды важнее плавности применения ввода, которое делает игра.
    // Предел упреждения, град, и предел времени упреждения, с.
    static constexpr float kLeadMaxDegrees = 12.0f;
    static constexpr float kLeadMaxTime = 0.8f;
    // Оценка скорости цели считается только когда заказ в трубе мал: иначе в
    // окно попадают такты захвата, где остаток падает от нашей же камеры, и
    // наклон выходит -30..-50°/с при настоящих -12°/с (упреждение вставало на
    // предел и уводило приказ за ноль). Во время захвата оценка просто держит
    // прежнее значение, а на удержании — пересчитывается каждый такт.
    static constexpr float kLeadSettledBand = 6.0f;
    // Разброс остатка за окно, при котором оценка скорости цели ещё считается.
    // Захват и выход из него дают разброс в десятки градусов, и наклон по этим
    // тактам переживал захват ещё на секунду (стенд: упреждение 11.5° при
    // настоящих 8°/с цели).
    static constexpr float kLeadSpreadMax = 8.0f;
    // Доля остатка, которую упреждению разрешено снять. Предел нужен не от
    // хорошей жизни: на переходных тактах (выход из захвата, пачка применённого
    // ввода) оценка скорости цели даёт -4°/с при стоящей цели, и упреждение без
    // ограничения уводило приказ за ноль — контроллер вставал и ждал, пока
    // оценка протухнет (стенд «труба8»: рысканье не сходилось 1.5 с).
    // С долей 0.35 приказ остаётся направленным на цель, а упреждение лишь
    // укорачивает его.
    static constexpr float kLeadFrac = 0.5f;

    struct Sample { float v; };

    static float targetRate(const Sample* h, int n, float dt, int cap) {
        // Окно обязано быть полным: наклон по четырём тактам переходного
        // процесса — это скорость нашего сведения, а не цели.
        if (n < cap || cap < 4 || !(dt > 0.0f)) return 0.0f;
        // Устойчивость остатка: пока он скачет, окно держит переходные такты и
        // наклон по ним — это скорость нашего сведения, а не цели.
        float lo = h[0].v, hi = h[0].v;
        for (int i = 1; i < n; ++i) {
            if (h[i].v < lo) lo = h[i].v;
            if (h[i].v > hi) hi = h[i].v;
        }
        if (hi - lo > kLeadSpreadMax) return 0.0f;
        double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
        for (int i = 0; i < n; ++i) {
            const double x = (double)i * dt;
            const double y = h[i].v;
            sx += x; sy += y; sxx += x * x; sxy += x * y;
        }
        const double denom = (double)n * sxx - sx * sx;
        if (std::fabs(denom) < 1.0e-9) return 0.0f;
        const double v = ((double)n * sxy - sx * sy) / denom;
        if (!std::isfinite(v)) return 0.0f;
        const float rate = (float)v;
        return std::fabs(rate) < kTargetRateMin ? 0.0f : rate;
    }

    static void pushHistory(Sample* h, int& n, float v, float dt) {
        const int cap = std::min(kTargetWindowMax,
                                 std::max(4, (int)(kTargetWindowTime / dt) + 1));
        for (int i = 1; i < cap; ++i) h[i - 1] = h[i];
        h[cap - 1] = {v};
        n = (n < cap) ? n + 1 : cap;
    }

    static float clampLead(float lead) {
        if (!aim_control::finite(lead)) return 0.0f;
        if (std::fabs(lead) > kLeadMaxDegrees)
            lead = (lead > 0.0f) ? kLeadMaxDegrees : -kLeadMaxDegrees;
        return lead;
    }

    Flight flight;
    SoftSlew slewYaw, slewPitch;
    int streakYaw = 0, streakPitch = 0;
    float acquire = 0.0f;
    float softTime = 0.0f;
    float envelopeDuration = 0.12f;
    float flipDamp = 1.0f;
    float prevCtlYaw = 0.0f, prevCtlPitch = 0.0f;
    bool haveCtlErr = false;
    float ackCameraYaw = 0.0f, ackCameraPitch = 0.0f;
    int ackTimeouts = 0;
    float ackDisabled = 0.0f;
    bool camMovedPrev = false;
    Sample histYaw[kTargetWindowMax] = {}, histPitch[kTargetWindowMax] = {};
    int histN = 0;
    float targetRateYaw = 0.0f, targetRatePitch = 0.0f;
    float trackYaw = 0.0f, trackPitch = 0.0f;

    void reset() { *this = Controller{}; }

    // Палец отпущен или переставлен — игра летящее не отработает.
    // keepDelay: замер задержки принадлежит трубе ввода, а не заказам, и после
    // переноса пальца труба никуда не девается. Без него следующий захват шёл с
    // полной долей шага (предел k по задержке включается только при
    // pipeDelay > 0) и с пустым окном летящего заказа — снова пачка приказов
    // поверх друг друга и бросок камеры. Стенд A/B оставляет прежнее поведение:
    // там сброс полный, по умолчанию.
    void clearOrders(bool keepDelay = false) {
        const float savedDelay = flight.delay;
        flight.reset();
        if (keepDelay) flight.delay = savedDelay;
        streakYaw = streakPitch = 0;
        ackCameraYaw = ackCameraPitch = 0.0f;
    }

    // Смена цели. Отдельный вход, а не clearOrders + resetApproach, которыми
    // это делалось раньше: труба ввода принадлежит устройству, а не цели.
    //
    // Замеренная задержка обязана переживать смену: по ней считается и доля
    // шага (предел k включается только при pipeDelay > 0), и окно летящего
    // заказа. Обнулённая задержка оставляла окно пустым, летящий заказ не
    // вычитался, и контроллер заказывал полный остаток каждый такт — за
    // мёртвое время трубы набиралось три приказа по 4.2°, которые игра
    // применяла одной пачкой: бросок камеры 12.6° за кадр (стенд «смена цели
    // -25°»). Заказы, уже ушедшие в игру, она всё равно применит — палец со
    // экрана не снимался, и забыть их значит заказать тот же поворот ещё раз.
    //
    //   hard = true  — другой игрок. Остаток скачет на десятки градусов,
    //                  скорость прошлой цели и разгон подхода смысла не имеют.
    //   hard = false — та же цель, другая кость (голова мигнула, слот упал на
    //                  шею или грудь): остаток смещается на доли градуса, а
    //                  aim.cpp считает это сменой выбора. Сбрасывать разгон и
    //                  скорость цели здесь нельзя — на ведении это повторяется
    //                  каждые доли секунды.
    void onTargetSwitch(bool hard) {
        flight.onResidualStep(hard);
        streakYaw = streakPitch = 0;
        ackCameraYaw = ackCameraPitch = 0.0f;
        // Гаситель качелей и прошлый остаток принадлежат старой цели: иначе
        // первый же такт на новой принимает скачок за перелёт и режет долю
        // шага до 0.45 на треть секунды.
        flipDamp = 1.0f;
        haveCtlErr = false;
        prevCtlYaw = prevCtlPitch = 0.0f;
        if (!hard) return;
        slewYaw.reset();
        slewPitch.reset();
        acquire = 0.0f;
        softTime = 0.0f;
        std::memset(histYaw, 0, sizeof(histYaw));
        std::memset(histPitch, 0, sizeof(histPitch));
        histN = 0;
        targetRateYaw = targetRatePitch = 0.0f;
    }

    // Новый захват: разгон начинается заново, скорость команды с нуля.
    void resetApproach() {
        slewYaw.reset();
        slewPitch.reset();
        acquire = 0.0f;
        softTime = 0.0f;
        flipDamp = 1.0f;
        haveCtlErr = false;
        prevCtlYaw = prevCtlPitch = 0.0f;
        std::memset(histYaw, 0, sizeof(histYaw));
        std::memset(histPitch, 0, sizeof(histPitch));
        histN = 0;
        targetRateYaw = targetRatePitch = 0.0f;
    }

    // rawYaw/rawPitch — остаток до цели в градусах (вправо/вверх положительно).
    // pendDx/pendDy/pendTime — накопленный сдвиг пальца, который камера ещё не
    // отработала; принадлежат вызывающему, он же ведёт подтверждение такта.
    Output tick(const Params& p, float fingerX, float fingerY,
                float dt, float rawYaw, float rawPitch,
                bool camMoved, float camYawDelta, float camPitchDelta,
                bool flowOk, float flowYaw, float flowPitch,
                float& pendDx, float& pendDy, float& pendTime) {
        Output out;
        if (!(dt > 0.0f) || !aim_control::finite(dt)) dt = 1.0f / 60.0f;
        if (dt > 0.1f) dt = 0.1f;

        float sm = p.speed;
        if (!(sm >= 1.0f)) sm = 1.0f;
        if (sm > 10.0f) sm = 10.0f;
        const float frac = 0.25f + (sm - 1.0f) / 9.0f * 0.75f;
        const float trim = (p.trim > 1.0f && aim_control::finite(p.trim)) ? p.trim : 1.0f;
        const float trimP = (p.trim_pitch > 1.0f && aim_control::finite(p.trim_pitch))
                          ? p.trim_pitch : trim;
        const float gy = p.base_gain * trim;
        const float gp = p.base_gain * trimP;
        if (!(std::fabs(gy) > 1.0e-5f)) {
            out.action = kHold;
            return out;
        }

        const float responseWindow = aim_control::touch_response_window(dt);
        const bool useFlight = !p.have_camera_axis || ackDisabled > 0.0f;

        // ---- такт с подтверждением (есть настоящая ось камеры) ----
        const float expectedYaw = pendDx * gy;
        const float expectedPitch = -pendDy * gp;
        if (!p.have_camera_axis || camMoved) {
            ackCameraYaw = ackCameraPitch = 0.0f;
        } else {
            ackCameraYaw = aim_control::touch_accumulate_camera_response(
                ackCameraYaw, camYawDelta, expectedYaw);
            ackCameraPitch = aim_control::touch_accumulate_camera_response(
                ackCameraPitch, camPitchDelta, expectedPitch);
        }
        const bool partialAck =
            aim_control::touch_camera_response_acknowledged(expectedYaw, ackCameraYaw) ||
            aim_control::touch_camera_response_acknowledged(expectedPitch, ackCameraPitch);
        const bool camAnswered = camMoved || camMovedPrev || partialAck;
        camMovedPrev = camMoved;
        if (ackDisabled > 0.0f) ackDisabled -= dt;
        if (ackDisabled <= 0.0f && p.have_camera_axis && !camAnswered &&
            (std::fabs(pendDx) >= 1.0f || std::fabs(pendDy) >= 1.0f)) {
            pendTime += dt;
            if (pendTime < responseWindow) {
                out.action = kHold;      // держим тач живым, ничего не двигаем
                return out;
            }
            pendDx = pendDy = 0.0f;      // ввод потерялся — продолжаем
            pendTime = 0.0f;
            ackCameraYaw = ackCameraPitch = 0.0f;
            if (++ackTimeouts >= 3) { ackTimeouts = 0; ackDisabled = 5.0f; }
        } else if (camAnswered) {
            ackTimeouts = 0;
            pendTime = 0.0f;
            ackCameraYaw = ackCameraPitch = 0.0f;
        }

        // ---- заказ «в полёте» ----
        float errYaw = rawYaw, errPitch = rawPitch;
        float pipeDelay = 0.0f;
        if (useFlight) {
            flight.update(dt, rawYaw, rawPitch, flowOk, flowYaw, flowPitch);
            flight.age(dt, responseWindow);
            // Пока задержка не замерена (первые такты, потерянный ввод),
            // берём взвешенный возраст заказов в окне: это оценка сверху, и
            // насыщение не даёт ей превратиться в лишние приказы.
            pipeDelay = (flight.delay > 0.0f) ? flight.delay : flight.meanAge();
            flight.subtract(errYaw, errPitch, pipeDelay);
        } else {
            // Есть настоящая ось — работает такт с подтверждением, а не полёт.
            // Прошлый сырой остаток забываем: вернётся режим без оси, и первый
            // же его такт не должен списывать заказ по разнице через пропасть.
            flight.havePrevRaw = false;
        }

        // ---- собственная скорость цели и упреждение ----
        // Остаток падает не только от нашего приказа: цель идёт навстречу и
        // закрывает его сама, а приказ прилетит через трубу ввода. Без
        // упреждения заказ на закрытие сегодняшнего остатка прилетает уже
        // после того, как остаток закрылся, и камера проскакивает кость. На
        // убегającej цели наоборот: заказ всегда меньше нужного, и прицел
        // тащится позади на v_цели * D.
        //
        // Наклон берётся от СОБСТВЕННОГО движения цели, а не от остатка:
        // остаток во время захвата падает в основном от нашей же камеры, и его
        // наклон — это скорость сведения (~30°/с), а не цели (стенд: оценка
        // выходила -33°/с при настоящих -12°/с, упреждение вставало на предел
        // и уводило приказ за ноль). Когда поток камеры виден, его поворот
        // прибавляется к остатку — остаётся чистое движение цели. Когда потока
        // нет (дуэль 1 на 1, боксов меньше трёх), вычитать собственное
        // движение нечем, и сигнал берётся очищенным от заказа в трубе: на
        // захвате он почти стоит, поэтому наклон мал и упреждение не мешает.
        const float sigYaw = flight.flowOk ? rawYaw + flowYaw : rawYaw - flight.absorbYaw;
        const float sigPitch = flight.flowOk ? rawPitch + flowPitch : rawPitch - flight.absorbPitch;
        const float inflightMag = std::max(std::fabs(flight.absorbYaw), std::fabs(flight.absorbPitch));
        if (inflightMag < kLeadSettledBand) {
            pushHistory(histYaw, histN, sigYaw, dt);
            pushHistory(histPitch, histN, sigPitch, dt);
            const int cap = std::min(kTargetWindowMax,
                                     std::max(4, (int)(kTargetWindowTime / dt) + 1));
            // Сглаживание оценки обязательно: дрожь кости 0.4° на 7 рад/с даёт
            // в производной ±2.8°/с, и упреждение без сглаживания качалось от
            // нуля до предела каждый такт.
            targetRateYaw = 0.85f * targetRateYaw + 0.15f * targetRate(histYaw, histN, dt, cap);
            targetRatePitch = 0.85f * targetRatePitch + 0.15f * targetRate(histPitch, histN, dt, cap);
        } else {
            // История обязана сбрасываться, а не «замораживаться»: за захват в
            // окне остаются такты, где остаток падал от нашей же камеры
            // (30-50°/с), и наклон по ним переживал захват ещё на секунду —
            // упреждение выходило 12° при настоящих 8°/с цели.
            histN = 0;
            targetRateYaw = targetRatePitch = 0.0f;
        }

        // leadTime — насколько приказ опаздывает к моменту прилёта; упреждение
        // ровно на это время. Знак ПЛЮС: наклон — это скорость остатка, то есть
        // куда цель уйдёт, и заказ закрывает остаток с учётом этого ухода.
        float leadTime = pipeDelay;
        if (leadTime > kLeadMaxTime) leadTime = kLeadMaxTime;
        float leadYaw = clampLead(targetRateYaw * leadTime);
        float leadPitch = clampLead(targetRatePitch * leadTime);
        const float capYaw = kLeadFrac * std::fabs(errYaw);
        const float capPitch = kLeadFrac * std::fabs(errPitch);
        if (std::fabs(leadYaw) > capYaw) leadYaw = (leadYaw > 0.0f) ? capYaw : -capYaw;
        if (std::fabs(leadPitch) > capPitch) leadPitch = (leadPitch > 0.0f) ? capPitch : -capPitch;

        // ---- квант ввода и мёртвая зона ----
        float unitsPerPx = p.units_per_px;
        if (!(unitsPerPx >= 0.25f && unitsPerPx <= 16.0f)) unitsPerPx = 1.0f;
        const float gridPx = 1.0f / unitsPerPx;
        const bool gainKnownYaw = p.learned_yaw || p.gain_from_sens || p.lsq_ready_yaw;
        const bool gainKnownPitch = p.learned_pitch || p.gain_from_sens || p.lsq_ready_pitch;
        const float qYaw = gainKnownYaw ? std::fabs(gy) * gridPx : 0.0f;
        const float qPitch = gainKnownPitch ? std::fabs(gp) * gridPx : 0.0f;
        const float deadYaw = aim_control::touch_lock_deadband(p.deg_per_px, p.world_dist, qYaw);
        const float deadPitch = aim_control::touch_lock_deadband(p.deg_per_px, p.world_dist, qPitch);
        if (std::fabs(errYaw) < deadYaw && std::fabs(errPitch) < deadPitch) {
            pendDx = pendDy = 0.0f;
            pendTime = 0.0f;
            ackCameraYaw = ackCameraPitch = 0.0f;
            out.action = kHold;
            return out;
        }

        // ---- полоса дрожи у цели ----
        if (p.soft) {
            errYaw *= jitter_taper(errYaw, streakYaw, kJitterBand, kDriftStreak, kDriftRelease);
            errPitch *= jitter_taper(errPitch, streakPitch, kJitterBand, kDriftStreak, kDriftRelease);
        } else {
            errYaw = aim_control::touch_jitter_gate(errYaw, streakYaw, kJitterBand, kDriftStreak);
            errPitch = aim_control::touch_jitter_gate(errPitch, streakPitch, kJitterBand, kDriftStreak);
        }
        if (errYaw == 0.0f && errPitch == 0.0f) {
            out.action = kHold;          // дрожь: палец стоит, камера не ходит за костью
            return out;
        }

        // ---- доля шага ----
        float k = manes_frame_gain(frac, dt);
        if (k > kStableFraction) k = kStableFraction;
        const float fracMid = 0.25f + (5.0f - 1.0f) / 9.0f * 0.75f;   // слайдер 5
        if (frac < fracMid) k *= frac / fracMid;
        if (k > 1.0f) k = 1.0f;
        if (k < 0.05f) k = 0.05f;

        // В мягкой ветке доля согласуется с задержкой трубы: k*D держится около
        // kLoopGain, иначе при трубе 0.3 с и доле 0.30/такт петля звенит.
        // Пол kGainMin не даёт захвату превратиться в ползание, когда замер
        // задержки завышен (провал ФПС, потерянные приказы).
        //
        // До первого отклика предела нет вовсе: камера ещё не двигалась,
        // обратной связи в петле нет и звенеть нечему, а заниженная доля
        // только растягивала захват (стенд «труба8»: 1.58 -> 2.67 с).
        if (p.soft && pipeDelay > 0.0f) {
            float d = pipeDelay;
            if (!aim_control::finite(d)) d = dt;
            if (d > kGainDelayMax) d = kGainDelayMax;
            const float kSoft = std::max(kGainMin, kLoopGain / d) * dt;
            if (kSoft < k) k = kSoft;
        }

        // Гаситель «качелей»: остаток перескочил ноль и набрал заметную
        // величину обратного знака — это перелёт от нашей поправки.
        const bool flipYaw = haveCtlErr && prevCtlYaw * errYaw < 0.0f &&
                             std::fabs(errYaw) > std::fabs(prevCtlYaw) * 0.35f;
        const bool flipPitch = haveCtlErr && prevCtlPitch * errPitch < 0.0f &&
                               std::fabs(errPitch) > std::fabs(prevCtlPitch) * 0.35f;
        if (flipYaw || flipPitch) flipDamp = 0.45f;
        else if (flipDamp < 1.0f) {
            flipDamp += (1.0f - flipDamp) * (dt * 2.5f);
            if (flipDamp > 1.0f) flipDamp = 1.0f;
        }
        prevCtlYaw = errYaw;
        prevCtlPitch = errPitch;
        haveCtlErr = true;
        k *= flipDamp;

        float envelope = 1.0f;
        if (p.soft) {
            // Мягкий старт: S-кривая, время разгона по остатку.
            const float rate = kRateMin + (sm - 1.0f) / 9.0f * (kRateMax - kRateMin);
            const float mag = std::max(std::fabs(rawYaw), std::fabs(rawPitch));
            envelopeDuration = soft_envelope_time(mag, rate);
            softTime += dt;
            envelope = soft_envelope(softTime, envelopeDuration);
            k *= envelope;
        } else {
            acquire += dt;
            if (acquire > 1.0f) acquire = 1.0f;
            k *= 0.3f + 0.7f * std::min(1.0f, acquire / 0.20f);

            // Спад доли шага у цели: чем меньше остаток, тем меньшую его долю
            // заказываем за такт, иначе поздние прилёты длинной трубы
            // перелетают кость.
            const float maxEff = std::max(std::fabs(errYaw), std::fabs(errPitch));
            float ramp = maxEff / kRampBand;
            if (ramp > 1.0f) ramp = 1.0f;
            if (ramp < kRampMin) ramp = kRampMin;
            k *= ramp;
        }

        // ---- команда в градусах, затем в пикселях ----
        // В мягкой ветке команда считается от остатка с упреждением. Знаковой
        // защиты здесь нет нарочно: на встречной цели упреждение ОБЯЗАНО
        // уводить приказ за ноль — это и есть приказ не перелетать кость,
        // которую цель уже прошла. Защита оставляла прицел гнаться за целью,
        // которая уходит в другую сторону (стенд «встречная цель 12°/с»:
        // перелёт 0.9°, хвост 5.1°).
        float cmdYaw = errYaw, cmdPitch = errPitch;
        if (p.soft) {
            cmdYaw = errYaw + leadYaw;
            cmdPitch = errPitch + leadPitch;
        }
        float wantYaw = cmdYaw * k;
        float wantPitch = cmdPitch * k;

        // Ограничитель скорости команды: главная часть «мягкого хода». Профиль
        // подхода остаётся прежним (доля шага × остаток), но приказ не может
        // прыгнуть с нуля на потолок за такт — только доехать за ускорение.
        bool slewed = false;
        if (p.soft) {
            const float rate = kRateMin + (sm - 1.0f) / 9.0f * (kRateMax - kRateMin);
            // Ускорение ограничителя, град/с². Пол у половины такта — иначе на
            // редких тактах (12 fps и просадки) разгон от нуля до первого
            // приказа съедал бы заметную часть захвата.
            const float halfTick = 0.5f * (1.0f + 0.65f * sm) / dt;
            const float accel = std::max(rate * kSlewAccel, halfTick / dt);
            wantYaw = slewYaw.apply(wantYaw, dt, accel);
            wantPitch = slewPitch.apply(wantPitch, dt, accel);
            slewed = true;
        }

        float dx = wantYaw / gy;
        float dy = -wantPitch / gp;

        // Финальный подход: в нескольких квантах ввода от цели сглаживание
        // выключается и палец встаёт на ближайшую достижимую позицию сетки.
        // Сглаживание здесь либо ползло бы много тактов, либо после округления
        // колебалось на целый шаг вокруг кости.
        if (gainKnownYaw && std::fabs(errYaw) < qYaw * 3.0f)
            dx = roundf((errYaw / gy) * unitsPerPx) / unitsPerPx;
        if (gainKnownPitch && std::fabs(errPitch) < qPitch * 3.0f)
            dy = roundf((-errPitch / gp) * unitsPerPx) / unitsPerPx;

        // Шаг не больше команды этого такта: иначе собственный шаг перелетает
        // цель, и следующий считается уже по ошибке другого знака.
        if (std::fabs(dx * gy) > std::fabs(cmdYaw)) dx = cmdYaw / gy;
        if (std::fabs(dy * gp) > std::fabs(cmdPitch)) dy = -cmdPitch / gp;

        // Потолок в градусах за такт и в пикселях экрана. Когда коэффициент —
        // догадка из настройки, потолок считается по истинным градусам камеры:
        // рабочий завышен подтяжкой (до 1.4x), и пересчёт «рабочие градусы ->
        // пиксели» пропускал шаг в полтора раза больше, чем камера доворачивает.
        const float maxAnglePerFrame = 1.0f + 0.65f * sm;
        const float capGainYaw = p.estimate_only ? gy / trim : gy;
        const float capGainPitch = p.estimate_only ? gp / trimP : gp;
        const float maxYawStep = aim_control::touch_step_limit_pixels(p.screen_height, capGainYaw, maxAnglePerFrame);
        const float maxPitchStep = aim_control::touch_step_limit_pixels(p.screen_height, capGainPitch, maxAnglePerFrame);
        dx = std::max(-maxYawStep, std::min(maxYawStep, dx));
        dy = std::max(-maxPitchStep, std::min(maxPitchStep, dy));

        // Целые единицы цифровера: скрытый остаток подпикселя позже вылез бы
        // незапланированным шагом, а замер коэффициента видел бы не то, что ушло.
        const float nx = roundf((fingerX + dx) * unitsPerPx) / unitsPerPx;
        const float ny = roundf((fingerY + dy) * unitsPerPx) / unitsPerPx;
        dx = nx - fingerX;
        dy = ny - fingerY;
        if (!aim_control::finite(dx) || !aim_control::finite(dy) || (dx == 0.0f && dy == 0.0f)) {
            pendDx = pendDy = 0.0f;
            pendTime = 0.0f;
            ackCameraYaw = ackCameraPitch = 0.0f;
            if (slewed) { slewYaw.reset(); slewPitch.reset(); }   // команда не ушла — скорость не копим
            out.action = kHold;
            return out;
        }

        pendDx += dx;
        pendDy += dy;
        if (useFlight) {
            // Рысканье: шаг dx поворачивает камеру на dx*gy к цели (остаток
            // упадёт на столько); тангаж: сдвиг пальца вверх (dy < 0) поднимает
            // камеру, остаток упадёт на -dy*gp.
            flight.place(dx * gy, -dy * gp, trim, trimP);
        }

        out.action = kMove;
        out.dx = dx;
        out.dy = dy;
        out.err_yaw = errYaw;
        out.err_pitch = errPitch;
        out.k_gain = k;
        out.envelope = envelope;
        const int maxParts = (p.soft && p.substeps > 1) ? std::min(p.substeps, 4) : 1;
        out.parts = split_substeps(dx, dy, out.partX, out.partY, maxParts);
        if (out.parts < 1) {
            out.parts = 1;
            out.partX[0] = dx;
            out.partY[0] = dy;
        }
        return out;
    }

};

} // namespace aim_touch
