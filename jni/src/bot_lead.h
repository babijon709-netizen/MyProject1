#pragma once
// Скорость бота для упреждения аима.
//
// У игроков оценка скорости своя (PlayerTrack в game.cpp), а бот брал
// g_aim_lead, оставшийся от последнего обработанного игрока кадра: в
// мемори-режиме упреждение там не меньше 50 мс (esp_draw.cpp), поэтому
// бегущий игрок сдвигал прицел на бота по СВОЕЙ скорости — точка уезжала в
// сторону от бота. Здесь своя оценка по той же схеме, что у игроков: шаг
// меряется между принятыми отсчётами позиции, сглаживание 0.5, телепорт и
// смена владельца пула отсекаются пределом скорости, стоянка обнуляет.
#include "Vector.h"

#include <cmath>

namespace botlead {

constexpr float  kMinStepMeters   = 0.02F;  // шаг меньше — позицию игра не меняла
constexpr double kMinSpanSeconds  = 0.02;   // мельче — это шум dt, а не движение
// Дальние боты перечитываются раз в 6/15 кадров, то есть 0.25 с на 60 к/с и до
// 0.5 с на 30 — окно должно это покрывать, иначе оценка не набиралась бы вовсе.
constexpr double kMaxSpanSeconds  = 0.9;    // разрыв больше — шаг не измеряем
constexpr float  kMaxSpeed        = 12.0F;  // быстрее человек не бегает: телепорт
constexpr double kFreshSeconds    = 0.3;    // столько без движения — оценка мертва

struct Track {
    Vec3   vel{};          // сглаженная скорость, м/с
    Vec3   ref{};          // позиция, от которой меряется текущий шаг
    double ref_t = 0.0;
    double moved_t = 0.0;  // когда позиция в последний раз менялась
    bool   have_ref = false;
};

inline bool finite_position(const Vec3& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

inline void update(Track& track, const Vec3& position, double now) {
    if (!finite_position(position) || !std::isfinite(now)) return;   // мусорный отсчёт не двигает оценку
    if (!track.have_ref) {
        track.ref = position; track.ref_t = now; track.moved_t = now;
        track.have_ref = true;
        return;
    }
    const float mx = position.x - track.ref.x;
    const float my = position.y - track.ref.y;
    const float mz = position.z - track.ref.z;
    const double span = now - track.ref_t;
    if (mx * mx + my * my + mz * mz > kMinStepMeters * kMinStepMeters) {
        if (span > kMinSpanSeconds && span < kMaxSpanSeconds) {
            const Vec3 v = {(float)(mx / span), (float)(my / span), (float)(mz / span)};
            const float speed = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
            if (std::isfinite(speed) && speed < kMaxSpeed) {
                track.vel.x = track.vel.x * 0.5F + v.x * 0.5F;
                track.vel.y = track.vel.y * 0.5F + v.y * 0.5F;
                track.vel.z = track.vel.z * 0.5F + v.z * 0.5F;
            }
        }
        track.ref = position; track.ref_t = now; track.moved_t = now;
    } else if (span > kFreshSeconds) {
        track.vel = {};
        track.ref = position; track.ref_t = now;
    }
}

// Годится ли оценка для упреждения прямо сейчас: позиция менялась недавно, а
// скорость конечна и правдоподобна. Стоящий бот даёт нулевую скорость — это
// тоже честный ответ, просто без упреждения. Оценку, набранную до долгого
// разрыва в чтениях, сохраняем: далёкие боты перечитываются редко, и обнулять
// её на каждом пропуске значило бы не упреждать их вообще.
inline bool fresh(const Track& track, double now) {
    if (!track.have_ref) return false;
    if (!(now - track.moved_t < kFreshSeconds)) return false;
    const float speed2 = track.vel.x * track.vel.x + track.vel.y * track.vel.y +
                         track.vel.z * track.vel.z;
    return std::isfinite(speed2) && speed2 < kMaxSpeed * kMaxSpeed;
}

} // namespace botlead
