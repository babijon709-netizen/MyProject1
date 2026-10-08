// Host tests for the near-plane clip of the player body segment (body_clip.h).
//
// Числа — из лога 08.10, игрок 0x77afb4ba80, dist=1.6 м, камера смотрит вниз:
//   23:10:47.552  bottom clip=(0.103 -1.064 0.905 1.204)
//                 top    clip=(0.103  0.213 -0.293 0.007) → screen=(19342.2 -15827.8),
//                 box=(10339.0 -7405.2 3369.0 8422.6) → reason=offscreen
//   23:10:47.605  bottom clip=(0.104 -1.238 0.723 1.023) → screen=(1355.6 1193.8)
//                 top    clip=(0.104  0.137 -0.360 -0.059) → behind-camera,
//                 box_valid=0 → reason=top-projection
// Бокс не возвращался 55 кадров (0.92 с) — «пропали все визуалы на секунду».
#include "body_clip.h"

#include <cmath>
#include <cstdio>

namespace {
int failures = 0;
#define CHECK(cond, name) do { \
    if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", name, __LINE__); } \
} while (0)

bool near_eq(float a, float b, float eps = 1e-4F) { return std::fabs(a - b) <= eps; }

// Глубина вдоль +Z, чтобы depth() совпал с clip_w из лога.
const Vec4 kPlane = {0.0F, 0.0F, 1.0F, 0.0F};

// viewport=2460x1080, screen_margin = max(sw,sh)*0.25 — как в game.cpp.
constexpr float kSw = 2460.0F, kSh = 1080.0F, kMargin = 615.0F;

// Тот же тест на пересечение, что и в game.cpp перед mark_skip('O').
bool box_intersects_margin(Vec2 bottom, Vec2 top) {
    const float height = std::fabs(top.y - bottom.y);
    const float cx = (bottom.x + top.x) * 0.5F;
    const float cy = (bottom.y + top.y) * 0.5F;
    const float half_h = height * 0.5F;
    const float half_w = height * 0.45F * 0.5F;   // PLAYER_BOX_WIDTH_RATIO порядка 0.45
    if (!(height > 0.001F)) return false;
    return !(cx + half_w < -kMargin || cx - half_w > kSw + kMargin ||
             cy + half_h < -kMargin || cy - half_h > kSh + kMargin);
}
}  // namespace

int main() {
    // 1. Кадр 23:10:47.605: верх за плоскостью (clip_w=-0.059), низ в кадре.
    {
        Vec3 bottom = {0.0F, 0.0F, 1.023F};    // depth == clip_w из лога
        Vec3 top = {0.0F, 1.8F, -0.059F};      // тело 1.8 м, как body_h=1.75..1.8
        CHECK(near_eq(bodyclip::depth(kPlane, bottom), 1.023F), "bottom depth matches the log");
        CHECK(near_eq(bodyclip::depth(kPlane, top), -0.059F), "top depth matches the log");

        const bodyclip::Segment seg =
            bodyclip::clip_segment(bottom, top, kPlane, bodyclip::kNearDepth);
        CHECK(seg.visible, "one point in front: the box survives");
        CHECK(seg.moved_top && !seg.moved_bottom, "only the offending end is moved");
        CHECK(near_eq(bodyclip::depth(kPlane, top), bodyclip::kNearDepth),
              "clipped end lands exactly on the near plane");
        CHECK(near_eq(bottom.z, 1.023F) && near_eq(bottom.y, 0.0F),
              "visible end is untouched");
        CHECK(top.y > 0.0F && top.y < 1.8F, "cut stays on the body segment");
    }

    // 2. Кадр 23:10:47.552: clip_w=0.007 — формально «спереди», но деление даёт
    //    screen=(19342 -15828) и бокс 3369x8422 px, который уносит по margin.
    {
        Vec3 bottom = {0.0F, 0.0F, 1.204F};
        Vec3 top = {0.0F, 1.8F, 0.007F};
        const bodyclip::Segment seg =
            bodyclip::clip_segment(bottom, top, kPlane, bodyclip::kNearDepth);
        CHECK(seg.visible && seg.moved_top, "near-zero w is clipped too, not divided by");
        CHECK(near_eq(bodyclip::depth(kPlane, top), bodyclip::kNearDepth),
              "clipped end sits at the near plane, not at 0.007");

        // Что бы ни спроецировалось, после прижимания точка в пределах зоны,
        // а низ (1355.6 1193.8) из лога остаётся в кадре — бокс проходит тест.
        const Vec2 clamped = bodyclip::clamp_to_margin({19342.2F, -15827.8F}, kSw, kSh, kMargin);
        CHECK(near_eq(clamped.x, kSw + kMargin) && near_eq(clamped.y, -kMargin),
              "exploded projection is pulled to the margin box");
        const Vec2 feet = {1355.6F, 1193.8F};
        CHECK(box_intersects_margin(feet, clamped), "clipped box passes the margin test");
        const float height = std::fabs(clamped.y - feet.y);
        CHECK(height > 100.0F && height <= 2.0F * (kSh + 2.0F * kMargin),
              "box height stays finite and bounded");
    }

    // 3. Обе точки перед плоскостью — ничего не трогаем.
    {
        Vec3 bottom = {1.0F, 2.0F, 5.0F}, top = {1.0F, 3.8F, 5.0F};
        const bodyclip::Segment seg =
            bodyclip::clip_segment(bottom, top, kPlane, bodyclip::kNearDepth);
        CHECK(seg.visible && !seg.moved_bottom && !seg.moved_top,
              "far player is left alone");
        CHECK(near_eq(bottom.z, 5.0F) && near_eq(top.z, 5.0F), "coordinates unchanged");
    }

    // 4. Обе за плоскостью — рисовать нечего. Отдельно вырожденный случай:
    //    одинаковая глубина обеих точек (dt - db == 0) не должен давать деление
    //    на ноль — ветка «обе сзади» отрабатывает раньше.
    {
        Vec3 bottom = {0.0F, 0.0F, -0.5F}, top = {0.0F, 1.8F, -0.1F};
        const bodyclip::Segment seg =
            bodyclip::clip_segment(bottom, top, kPlane, bodyclip::kNearDepth);
        CHECK(!seg.visible, "segment fully behind the plane is dropped");

        Vec3 flat_bottom = {0.0F, 0.0F, -0.4F}, flat_top = {0.0F, 1.8F, -0.4F};
        const bodyclip::Segment flat =
            bodyclip::clip_segment(flat_bottom, flat_top, kPlane, bodyclip::kNearDepth);
        CHECK(!flat.visible, "equal depths behind the plane do not divide by zero");
        CHECK(near_eq(flat_bottom.z, -0.4F) && near_eq(flat_top.z, -0.4F),
              "degenerate segment is left untouched");
    }

    // 5. Взгляд снизу: за плоскостью нижняя точка.
    {
        Vec3 bottom = {0.0F, 0.0F, -0.30F}, top = {0.0F, 1.8F, 0.90F};
        const bodyclip::Segment seg =
            bodyclip::clip_segment(bottom, top, kPlane, bodyclip::kNearDepth);
        CHECK(seg.visible && seg.moved_bottom && !seg.moved_top,
              "bottom end is clipped when the camera is under the player");
        CHECK(near_eq(bodyclip::depth(kPlane, bottom), bodyclip::kNearDepth),
              "clipped bottom lands on the near plane");
        CHECK(near_eq(top.z, 0.90F), "top end untouched");
    }

    // 6. Плоскости глубины для обоих путей проекции.
    {
        Mat4 vp{};
        // Mat4 столбцами: mat_get(m,row,col) == m[col*4+row]; clip_w — строка 3.
        vp.m[3] = 0.1F; vp.m[7] = 0.2F; vp.m[11] = 0.3F; vp.m[15] = 0.4F;
        const Vec4 p = bodyclip::plane_from_vp(vp);
        CHECK(near_eq(p.x, 0.1F) && near_eq(p.y, 0.2F) && near_eq(p.z, 0.3F) &&
              near_eq(p.w, 0.4F), "plane_from_vp reads the clip_w row");
        CHECK(near_eq(bodyclip::depth(p, {1.0F, 1.0F, 1.0F}), 1.0F),
              "vp depth of (1,1,1) is 0.1+0.2+0.3+0.4");

        const Vec4 c = bodyclip::plane_from_camera({1.0F, 2.0F, 3.0F}, {1.0F, 0.0F, 0.0F});
        CHECK(near_eq(bodyclip::depth(c, {4.0F, 2.0F, 3.0F}), 3.0F),
              "camera depth is the distance along forward");
    }

    if (failures) { printf("test_body_clip: %d failure(s)\n", failures); return 1; }
    printf("test_body_clip: ok\n");
    return 0;
}
