#pragma once
// Отсечение вертикального отрезка тела по ближней плоскости перед проекцией.
//
// В упор (лог 08.10, 23:10:47, игрок 0x77afb4ba80, dist=1.6 м, камера смотрит
// вниз под ~45°) верхняя точка уходила за плоскость: clip_w=-0.059 →
// w2s возвращал false → mark_skip('T') → весь бокс целиком в помойку, хотя
// нижняя точка проектировалась нормально, screen=(1335.7 1017.4) — в кадре.
// Бокс не возвращался 55 кадров (0.92 с). Чуть больше нуля clip_w даёт деление
// на ~0 и экран (19342 -15828), то есть бокс 3369x8422 px и вылет по margin.
//
// Глубина линейна по мировой точке в обоих путях проекции (clip_w = строка 3
// VP · (p,1); camera_space.z = dot(forward, p - cam)), поэтому точка
// пересечения плоскости — обычный lerp вдоль отрезка.
#include <algorithm>

#include "Vector.h"

namespace bodyclip {

// Ближняя плоскость в метрах вдоль взгляда. Меньше 0.2 брать нельзя: экранная
// координата растёт как 1/depth, и на 0.001 (порог w2s) точка улетает на
// 19342 px. Больше 0.3 — начнём резать тело на нормальной дистанции.
constexpr float kNearDepth = 0.20F;

// Плоскость глубины: depth(p) = nx*x + ny*y + nz*z + d.
// Для VP это clip_w = mat_get(vp,3,0..3); Mat4 лежит столбцами
// (m[column * 4 + row], см. mat_get в game.cpp), отсюда индексы 3/7/11/15.
inline Vec4 plane_from_vp(const Mat4& vp) {
    return {vp.m[3], vp.m[7], vp.m[11], vp.m[15]};
}

inline Vec4 plane_from_camera(const Vec3& camera_position, const Vec3& forward) {
    return {forward.x, forward.y, forward.z,
            -(forward.x * camera_position.x + forward.y * camera_position.y +
              forward.z * camera_position.z)};
}

inline float depth(const Vec4& plane, const Vec3& p) {
    return plane.x * p.x + plane.y * p.y + plane.z * p.z + plane.w;
}

struct Segment {
    bool visible = false;      // осталась ли хоть одна точка перед плоскостью
    bool moved_bottom = false; // какую из точек пришлось подтянуть
    bool moved_top = false;
};

// bottom/top меняются на месте. Возвращает описание того, что произошло;
// visible == false означает, что отрезок целиком за плоскостью и рисовать нечего.
inline Segment clip_segment(Vec3& bottom, Vec3& top, const Vec4& plane,
                            float near_depth) {
    Segment out{};
    const float db = depth(plane, bottom);
    const float dt = depth(plane, top);
    const bool ok_b = db >= near_depth;
    const bool ok_t = dt >= near_depth;
    if (ok_b && ok_t) {
        out.visible = true;
        return out;
    }
    if (!ok_b && !ok_t) return out;

    // t, при котором глубина равна near_depth; знаки разные, знаменатель != 0.
    const float t = (near_depth - db) / (dt - db);
    const Vec3 cut = {bottom.x + (top.x - bottom.x) * t,
                      bottom.y + (top.y - bottom.y) * t,
                      bottom.z + (top.z - bottom.z) * t};
    if (!ok_b) { bottom = cut; out.moved_bottom = true; }
    else       { top = cut;    out.moved_top = true; }
    out.visible = true;
    return out;
}

// Подтянутая к плоскости точка проектируется на screen = f(1/near_depth), то
// есть на тысячи пикселей за экраном. Прижимаем только её: обычный бокс, у
// которого обе точки спроецировались честно, трогать нельзя — иначе дальний
// игрок вместо mark_skip('O') начал бы рисоваться прижатым к кромке.
inline Vec2 clamp_to_margin(Vec2 p, float sw, float sh, float margin) {
    p.x = std::min(std::max(p.x, -margin), sw + margin);
    p.y = std::min(std::max(p.y, -margin), sh + margin);
    return p;
}

}  // namespace bodyclip
