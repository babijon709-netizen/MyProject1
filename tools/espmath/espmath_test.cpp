// Стенд мировой→экранной математики: собирает НАСТОЯЩИЙ jni/src/esp/math.cpp
// (тот же файл, что уезжает в сборку) и проверяет инварианты проекции.
//
// Зачем. 17 сентября 2026 «упрощение» одной строки в mat_view_from_basis
// (-(-forward*position) -> -(forward*position)) уронило сразу и тач-аим, и
// боксы: глубина кадра уезжала на 2*(forward*position), то есть тем сильнее,
// чем дальше камера от начала мира. Ни syntax, ни hostcheck, ни остальные
// стенды этого не видели: они проверяют сборку и логику, а не числа. Здесь
// проверяются именно числа, и в том числе тот случай, который ломается только
// при камере вдали от начала координат.
//
//   sh tools/espmath/run.sh
#include "esp/math.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

static int g_failures = 0;
static int g_checks = 0;

static void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) {
        std::printf("OK   %s\n", what);
        return;
    }
    ++g_failures;
    std::printf("ПРОВАЛ %s\n", what);
}

static void check_near(float value, float expected, float tolerance, const char* what) {
    const bool ok = std::isfinite(value) && std::isfinite(expected) && std::fabs(value - expected) <= tolerance;
    ++g_checks;
    if (ok) {
        std::printf("OK   %s (%.4f)\n", what, (double)value);
        return;
    }
    ++g_failures;
    std::printf("ПРОВАЛ %s: получено %.4f, ждали %.4f +-%.4f\n", what, (double)value, (double)expected, (double)tolerance);
}

// Камера из рыскания/тангажа в общей конвенции проекта (обратная к
// angles_from_forward в aim_mem.cpp).
struct Pose {
    Vec3 right{}, up{}, forward{}, position{};
};

static Pose pose_from_angles(float yaw_deg, float pitch_deg, const Vec3& position) {
    constexpr float kDegToRad = 0.01745329252F;
    const float yaw = yaw_deg * kDegToRad;
    const float pitch = pitch_deg * kDegToRad;
    Pose pose;
    pose.forward = {sinf(yaw) * cosf(pitch), sinf(pitch), cosf(yaw) * cosf(pitch)};
    Vec3 right = cross_product({0.0F, 1.0F, 0.0F}, pose.forward);
    const float length = sqrtf(right.x * right.x + right.y * right.y + right.z * right.z);
    right = {right.x / length, right.y / length, right.z / length};
    pose.right = right;
    pose.up = cross_product(pose.forward, right);
    pose.position = position;
    return pose;
}

// Кватернион из матрицы поворота: независимая реализация, чтобы связать
// путь «поза -> кватернион -> вид» с путём «поза -> базис -> вид».
static Vec4 quaternion_from_basis(const Vec3& right, const Vec3& up, const Vec3& forward) {
    const float m00 = right.x, m01 = up.x, m02 = forward.x;
    const float m10 = right.y, m11 = up.y, m12 = forward.y;
    const float m20 = right.z, m21 = up.z, m22 = forward.z;
    const float trace = m00 + m11 + m22;
    float x, y, z, w;
    if (trace > 0.0F) {
        const float s = sqrtf(trace + 1.0F) * 2.0F;
        w = 0.25F * s;
        x = (m21 - m12) / s;
        y = (m02 - m20) / s;
        z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = sqrtf(1.0F + m00 - m11 - m22) * 2.0F;
        w = (m21 - m12) / s;
        x = 0.25F * s;
        y = (m01 + m10) / s;
        z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = sqrtf(1.0F + m11 - m00 - m22) * 2.0F;
        w = (m02 - m20) / s;
        x = (m01 + m10) / s;
        y = 0.25F * s;
        z = (m12 + m21) / s;
    } else {
        const float s = sqrtf(1.0F + m22 - m00 - m11) * 2.0F;
        w = (m10 - m01) / s;
        x = (m02 + m20) / s;
        y = (m12 + m21) / s;
        z = 0.25F * s;
    }
    return {x, y, z, w};
}

static constexpr float kScreenWidth = 2400.0F;
static constexpr float kScreenHeight = 1080.0F;

// Точка «прямо перед камерой» обязана попасть в центр экрана; всё, что выше и
// правее камеры, — выше и правее центра; точка за камерой проекции не имеет.
static void check_projection(const Pose& pose, const char* where) {
    const Mat4 view = mat_view_from_basis(pose.right, pose.up, pose.forward, pose.position);
    const Mat4 projection = mat_perspective(60.0F, kScreenWidth / kScreenHeight, 0.1F, 1000.0F);
    const Mat4 vp = mat_mul(projection, view);

    Vec3 ahead = {pose.position.x + pose.forward.x * 10.0F,
                  pose.position.y + pose.forward.y * 10.0F,
                  pose.position.z + pose.forward.z * 10.0F};
    Vec2 center{};
    char what[160];
    std::snprintf(what, sizeof(what), "%s: точка прямо перед камерой — в центре экрана", where);
    const bool centered = w2s(vp, ahead, kScreenWidth, kScreenHeight, center, false);
    check(centered, what);
    if (centered) {
        std::snprintf(what, sizeof(what), "%s: центр по x", where);
        check_near(center.x, kScreenWidth * 0.5F, 1.0F, what);
        std::snprintf(what, sizeof(what), "%s: центр по y", where);
        check_near(center.y, kScreenHeight * 0.5F, 1.0F, what);
    }

    Vec3 above = {ahead.x + pose.up.x * 2.0F, ahead.y + pose.up.y * 2.0F, ahead.z + pose.up.z * 2.0F};
    Vec3 to_right = {ahead.x + pose.right.x * 2.0F, ahead.y + pose.right.y * 2.0F, ahead.z + pose.right.z * 2.0F};
    Vec2 above_screen{}, right_screen{};
    const bool above_ok = w2s(vp, above, kScreenWidth, kScreenHeight, above_screen, false);
    const bool right_ok = w2s(vp, to_right, kScreenWidth, kScreenHeight, right_screen, false);
    std::snprintf(what, sizeof(what), "%s: выше камеры — выше центра", where);
    check(above_ok && above_screen.y < center.y - 10.0F, what);
    std::snprintf(what, sizeof(what), "%s: правее камеры — правее центра", where);
    check(right_ok && right_screen.x > center.x + 10.0F, what);

    Vec3 behind = {pose.position.x - pose.forward.x * 10.0F,
                   pose.position.y - pose.forward.y * 10.0F,
                   pose.position.z - pose.forward.z * 10.0F};
    Vec2 behind_screen{};
    std::snprintf(what, sizeof(what), "%s: точка за камерой не проектируется", where);
    check(!w2s(vp, behind, kScreenWidth, kScreenHeight, behind_screen, false), what);

    std::snprintf(what, sizeof(what), "%s: камера восстанавливается из вида", where);
    Vec3 recovered{};
    const bool recovered_ok = camera_position_from_view(view, recovered);
    check(recovered_ok, what);
    if (recovered_ok) {
        const float dx = recovered.x - pose.position.x, dy = recovered.y - pose.position.y, dz = recovered.z - pose.position.z;
        std::snprintf(what, sizeof(what), "%s: позиция камеры совпадает (%.1f, %.1f, %.1f)",
                      where, (double)pose.position.x, (double)pose.position.y, (double)pose.position.z);
        check(dx * dx + dy * dy + dz * dz < 1e-4F, what);
    }
}

int main() {
    // 1. Тождественная поза: у камеры Unity взгляд идёт по -Z, поэтому в
    // камере-пространстве перевёрнут именно Z (это конвенция проекта, и на ней
    // стоит mat_perspective, который берёт w = -z).
    {
        const Mat4 view = mat_world_to_camera({0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F, 1.0F});
        bool expected_view = true;
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column) {
                const float expected = row == column ? (row == 2 ? -1.0F : 1.0F) : 0.0F;
                if (std::fabs(mat_get(view, row, column) - expected) > 1e-5F) expected_view = false;
            }
        check(expected_view, "тождественная поза — вид с перевёрнутым Z камеры");
        Vec3 origin{};
        check(camera_position_from_view(view, origin) && std::fabs(origin.x) + std::fabs(origin.y) + std::fabs(origin.z) < 1e-5F,
              "позиция камеры у начала мира восстанавливается");
    }

    // 2. Поза из углов: кватернион и базис дают один и тот же вид.
    {
        const Pose pose = pose_from_angles(37.0F, -12.0F, {12.5F, 3.5F, -7.25F});
        const Vec4 quaternion = quaternion_from_basis(pose.right, pose.up, pose.forward);
        const Vec3 forward_by_quaternion = rotate_vector(quaternion, {0.0F, 0.0F, 1.0F});
        check_near(forward_by_quaternion.x, pose.forward.x, 1e-4F, "кватернион из базиса: forward.x");
        check_near(forward_by_quaternion.y, pose.forward.y, 1e-4F, "кватернион из базиса: forward.y");
        check_near(forward_by_quaternion.z, pose.forward.z, 1e-4F, "кватернион из базиса: forward.z");
        const Mat4 by_quaternion = mat_world_to_camera(pose.position, quaternion);
        const Mat4 by_basis = mat_view_from_basis(pose.right, pose.up, pose.forward, pose.position);
        float worst = 0.0F;
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                worst = std::fmax(worst, std::fabs(mat_get(by_quaternion, row, column) - mat_get(by_basis, row, column)));
        check_near(worst, 0.0F, 1e-4F, "вид из кватерниона совпадает с видом из базиса");
    }

    // 3. Проекция у начала мира и — главное — вдали от него. Второй случай
    // ловит ошибку знака в переводе матрицы вида: там глубина уезжает на
    // 2*(forward*position), и при камере вдали от нуля всё разваливается.
    check_projection(pose_from_angles(0.0F, 0.0F, {0.0F, 0.0F, 0.0F}), "камера в начале мира");
    check_projection(pose_from_angles(-118.0F, 4.0F, {312.0F, 41.0F, -184.0F}), "камера вдали от начала мира");
    check_projection(pose_from_angles(75.0F, 63.0F, {-1240.0F, 12.0F, 865.0F}), "камера высоко и далеко");

    // 4. Разбор кеша проекции: своя перспектива узнаётся как есть, её
    // перестановка — как построчная раскладка нативной Matrix4x4f.
    {
        const Mat4 projection = mat_perspective(65.0F, 16.0F / 9.0F, 0.05F, 2000.0F);
        check(perspective_orientation(projection) == 1, "перспектива узнаётся в нашем чтении");
        check(perspective_orientation(mat_transposed(projection)) == 2, "перестановка узнаётся как построчная раскладка");
        Mat4 garbage{};
        for (int i = 0; i < 16; ++i) garbage.m[i] = 3.5F;
        check(perspective_orientation(garbage) == 0, "мусор перспективой не считается");
    }

    if (g_failures == 0) {
        std::printf("\nвсе проверки пройдены: %d\n", g_checks);
        return 0;
    }
    std::printf("\nПРОВАЛЕНО проверок: %d из %d\n", g_failures, g_checks);
    return 1;
}
