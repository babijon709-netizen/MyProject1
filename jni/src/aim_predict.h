#pragma once
// Предиктор цели: альфа-бета фильтр в мировых углах (град), время реальное.
// Мировые углы = угол камеры + смещение цели, поэтому собственный поворот
// камеры не попадает в скорость цели. Скорость и позиция обновляются по
// монотонному времени, а не по кадрам: фпс и просадки на оценку не влияют.
#include <cmath>

namespace aim_predict {

constexpr float  kAlpha      = 0.45f;   // доля невязки в позиции
constexpr float  kBeta       = 0.08f;   // доля невязки в скорости (делится на dt)
constexpr float  kGateDeg    = 6.0f;    // скачок больше — другая точка/телепорт, трек заново
constexpr double kMaxGap     = 0.25;    // пауза чтения дольше — трек заново
constexpr float  kMaxVelDeg  = 720.f;   // град/с, выше — мусор
constexpr float  kMaxLeadDeg = 2.0f;    // потолок упреждения, град

struct Track {
    float  x = 0.f, y = 0.f;     // сглаженная мировая позиция, град
    float  vx = 0.f, vy = 0.f;   // скорость, град/с
    double t = 0.0;
    bool   have = false;
};

inline float wrap180(float d) {
    if (!std::isfinite(d)) return 0.f;
    d = std::fmod(d + 180.f, 360.f);
    if (d < 0.f) d += 360.f;
    return d - 180.f;
}

inline float clampf(float v, float lim) {
    return v > lim ? lim : (v < -lim ? -lim : v);
}

inline void reset(Track& tr) { tr = Track{}; }

// Возвращает true, если трек принял измерение (не сброс на пустом месте).
inline bool update(Track& tr, float mx, float my, double now) {
    if (!std::isfinite(mx) || !std::isfinite(my) || !std::isfinite(now)) return false;
    if (!tr.have || now <= tr.t || now - tr.t > kMaxGap) {
        tr = Track{}; tr.x = mx; tr.y = my; tr.t = now; tr.have = true;
        return true;
    }
    const float dt = (float)(now - tr.t);
    const float px = tr.x + tr.vx * dt;
    const float py = tr.y + tr.vy * dt;
    const float rx = wrap180(mx - px);
    const float ry = wrap180(my - py);
    if (fabsf(rx) > kGateDeg || fabsf(ry) > kGateDeg) {
        tr = Track{}; tr.x = mx; tr.y = my; tr.t = now; tr.have = true;
        return true;
    }
    tr.x = px + kAlpha * rx;
    tr.y = py + kAlpha * ry;
    tr.vx = clampf(tr.vx + (kBeta / dt) * rx, kMaxVelDeg);
    tr.vy = clampf(tr.vy + (kBeta / dt) * ry, kMaxVelDeg);
    tr.t = now;
    return true;
}

// lead — секунды вперёд (задержка применения шага). Возвращает мировые углы.
inline void predict(const Track& tr, float lead, float& outYaw, float& outPitch) {
    float ly = tr.vx * lead, lp = tr.vy * lead;
    const float mag = sqrtf(ly * ly + lp * lp);
    if (mag > kMaxLeadDeg && mag > 0.f) {
        const float s = kMaxLeadDeg / mag;
        ly *= s; lp *= s;
    }
    outYaw = tr.x + ly;
    outPitch = tr.y + lp;
}

} // namespace aim_predict
