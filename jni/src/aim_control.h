#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace aim_control {

inline bool finite(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

inline float wrap_degrees(float value) {
    if (!finite(value) || !(value > -1.0e6f && value < 1.0e6f)) return 0.0f;
    if (value > -180.0f && value <= 180.0f) return value;
    return value - 360.0f * std::floor((value + 180.0f) / 360.0f);
}

// The configured bone is exact; an unavailable slot is not substituted.
inline int configured_bone_slot(int requested, const bool valid[3]) {
    return requested >= 0 && requested < 3 && valid[requested] ? requested : -1;
}

struct AngleFilter {
    float yaw = 0.0f;
    float pitch = 0.0f;
    bool ready = false;

    void reset() { *this = AngleFilter{}; }

    void update(float& input_yaw, float& input_pitch, float dt, float time_constant) {
        if (!finite(input_yaw) || !finite(input_pitch)) {
            reset();
            input_yaw = input_pitch = 0.0f;
            return;
        }
        if (!(dt > 0.0f) || !finite(dt)) dt = 1.0f / 60.0f;
        if (dt > 0.1f) dt = 0.1f;
        if (!(time_constant > 0.0f) || !finite(time_constant)) time_constant = 0.02f;
        if (time_constant < 0.004f) time_constant = 0.004f;
        if (time_constant > 0.2f) time_constant = 0.2f;
        if (!ready) {
            yaw = wrap_degrees(input_yaw);
            pitch = input_pitch;
            ready = true;
        } else {
            const float alpha = 1.0f - std::exp(-dt / time_constant);
            float yaw_delta = wrap_degrees(input_yaw - yaw);
            float pitch_delta = input_pitch - pitch;
            if (std::fabs(yaw_delta) < 0.006f) yaw_delta = 0.0f;
            if (std::fabs(pitch_delta) < 0.006f) pitch_delta = 0.0f;
            yaw = wrap_degrees(yaw + yaw_delta * alpha);
            pitch += pitch_delta * alpha;
        }
        input_yaw = yaw;
        input_pitch = pitch;
    }
};

inline bool target_within_range_window(float range_metres, float nearest_metres) {
    if (!finite(range_metres) || !finite(nearest_metres) ||
        range_metres < 0.0f || nearest_metres < 0.0f) return false;
    // Keep crosshair-first selection within +5 m nearby, scaling to 1.5x at range.
    const float limit = std::max(nearest_metres + 5.0f, nearest_metres * 1.5f);
    return range_metres <= limit;
}

// Lower scores win: smart balances log-distance with aim error; the other two
// make either crosshair alignment or world distance the primary criterion.
inline float target_priority_score(int priority, float aim_error,
                                   float range_metres, bool range_valid,
                                   bool sticky) {
    if (!finite(aim_error) || aim_error < 0.0f) aim_error = 1.0e6f;
    const bool valid_range = range_valid && finite(range_metres) && range_metres >= 0.0f;
    const float range_term = std::log1p(valid_range ? range_metres : 10000.0f);
    float score;
    switch (priority) {
        case 1:  score = aim_error * 8.0f + range_term * 0.01f; break;
        case 2:  score = (valid_range ? range_metres : 1.0e6f) * 4.0f + aim_error; break;
        default: score = range_term * 4.0f + aim_error; break;
    }
    if (sticky) score -= 0.03f;
    return score;
}

inline void memory_command_angles(float accumulator_yaw, float accumulator_pitch,
                                  bool have_camera, float camera_yaw, float camera_pitch,
                                  float delta_yaw, float delta_pitch,
                                  float& out_yaw, float& out_pitch) {
    float base_yaw = accumulator_yaw;
    float base_pitch = accumulator_pitch;
    if (have_camera && finite(camera_yaw) && finite(camera_pitch)) {
        base_yaw = wrap_degrees(camera_yaw);
        base_pitch = wrap_degrees(-camera_pitch);
    }
    out_yaw = wrap_degrees(base_yaw + delta_yaw);
    out_pitch = wrap_degrees(base_pitch - delta_pitch);
}

// ---- Компенсация отдачи -----------------------------------------------------
//
// Обе петли наводки пропорциональные: тач считает шаг от пиксельной ошибки,
// память — от градусной, и обе ограничены скоростью. Против ПОСТОЯННОГО
// возмущения пропорциональное звено оставляет статическую ошибку: пока держится
// кнопка огня, ствол поднимает камеру, петля её возвращает, и прицел стоит не
// на кости, а на величину подъёма за такт выше неё. Ошибка угловая, поэтому
// промах от неё растёт линейно с дистанцией, а угловой размер модели падает
// обратно пропорционально ей: вблизи подъём ещё ложится на тело, на сотне
// метров уходит за макушку. Это и есть «на дистанции стреляет выше».
//
// Подъём измеряется, а не моделируется. RecoilConfig — ассет на
// HitscanWeaponConfig (+0x70, рядом с разбросом +0x68) — несёт только размах:
// minRecoil/maxRecoil (Vector2, 0x18/0x20) и боковые дельты (0x28/0x2C); ни
// нарастания по выстрелам, ни множителя прицеливания, ни кривой возврата там
// нет (см. no_recoil_memory.h — он гасит ровно эти четыре числа). Собирать по
// ним подъём значило бы угадывать форму под каждый ствол и каждый патч.
// Петля и так видит всё нужное: когда кнопка держится, а прицел уже сел на
// кость, ошибка, которая ПРОДОЛЖАЕТ появляться, — это ствол двигает камеру.
// Интеграл ровно по такой ошибке снимает её в установившемся режиме, а очередь
// живёт именно там.
//
// Знак один на оба режима: команда в обеих петлях подписана как ошибка
// (команда того же знака ошибку уменьшает), поэтому член просто прибавляется к
// команде и отдельных знаков на режим не нужно. Отдача — поворот камеры, так
// что её величина не зависит от дистанции до цели ни в пикселях, ни в градусах.
struct RecoilControl {
    // Постоянная стекания, с. Ствол возвращается между очередями, а заряд,
    // оставшийся от прошлой цели, выкинул бы первые выстрелы следующей.
    static constexpr float kTau = 0.60f;
    // Коэффициент интеграла, 1/с. Установившийся остаток при подъёме climb за
    // такт: e = climb * (1 - exp(-dt/kTau)) / (kKi * dt + (1 - exp(-dt/kTau)) * kp).
    // На 12 Гц, подъёме 0.4 град/так и kp=0.7 это ~0.04 град вместо 0.57 у
    // чистого пропорционального звена — в пикселях экрана остаток уходит под
    // мёртвую зону тач-петли.
    static constexpr float kKi = 14.0f;

    float integral = 0.0f;

    void reset() { integral = 0.0f; }
    float term() const { return integral; }

    // error     — ошибка наводки в области вызывающего (тач: пиксели пальца,
    //             память: градусы), подписана как команда, которая её гасит.
    // firing    — кнопка огня держится и игра свою отдачу ещё не погасила.
    // band      — полоса «прицел сел». Вне неё ошибка принадлежит наведению на
    //             цель, а не отдаче, и заряд там наматывает интеграл, который
    //             потом перелетает на приходе.
    // limit     — кламп члена в той же области (анти-виндап).
    // saturated — прошлая команда упёрлась в ограничение скорости: заряд держим,
    //             иначе петля накопит то, что не смогла отдать.
    float update(float error, bool firing, float dt, float band, float limit,
                 bool saturated = false) {
        if (!finite(error) || !(limit > 0.0f)) { reset(); return 0.0f; }
        if (!(dt > 0.0f) || !finite(dt)) dt = 1.0f / 60.0f;
        if (dt > 0.1f) dt = 0.1f;
        // Стекает каждый такт, держится кнопка или нет.
        integral *= std::exp(-dt / kTau);
        if (firing && !saturated && band > 0.0f && std::fabs(error) <= band)
            integral += kKi * error * dt;
        if (!finite(integral)) integral = 0.0f;
        if (integral >  limit) integral =  limit;
        if (integral < -limit) integral = -limit;
        return integral;
    }
};

inline float memory_step(float error_degrees, float dt, float speed,
                         float range_metres) {
    if (!finite(error_degrees)) return 0.0f;
    if (!(dt > 0.0f) || !finite(dt)) dt = 1.0f / 60.0f;
    if (dt > 0.1f) dt = 0.1f;
    if (!(speed >= 1.0f)) speed = 1.0f;
    if (speed > 10.0f) speed = 10.0f;
    float deadband = 0.015f;
    if (range_metres > 0.0f && finite(range_metres)) {
        const float bone_tolerance = std::atan(0.004f / range_metres) * 57.2957795f;
        deadband = std::max(deadband, bone_tolerance);
    }
    if (std::fabs(error_degrees) <= deadband) return 0.0f;

    const float smooth = 0.25f + (speed - 1.0f) / 9.0f * 0.75f;
    const float gain = 1.0f - std::pow(1.0f - smooth, dt * 60.0f);
    float max_rate = 120.0f + 48.0f * speed;
    if (range_metres > 0.0f && range_metres < 12.0f) max_rate *= 1.15f;
    float max_step = max_rate * dt;
    if (max_step > 12.0f) max_step = 12.0f;
    const float step = error_degrees * gain;
    return std::max(-max_step, std::min(max_step, step));
}


} // namespace aim_control
