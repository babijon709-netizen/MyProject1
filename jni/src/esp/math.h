#pragma once
// math.h — Вектор/кватернион/матрица и мировое→экранное.
//
// Всё, что модуль отдаёт наружу: сюда смотрят те модули, которым нужны эти
// типы, данные и функции. Реализация — в math.cpp.
#include "esp/common.h"

// ---- Функции, которые видят другие модули ----

bool vec3_is_finite(const Vec3& value);

Vec3 cross_product(const Vec3& left, const Vec3& right);

Vec3 rotate_vector(const Vec4& quaternion, const Vec3& vector);

Vec4 multiply_quaternion(const Vec4& left, const Vec4& right);

bool normalize_quaternion(Vec4& quaternion);

bool matrix34_is_valid(const Matrix34& matrix);

float mat_get(const Mat4& matrix, int row, int column);

bool matrix_is_finite(const Mat4& matrix);

Mat4 mat_mul(const Mat4& a, const Mat4& b);

Mat4 mat_perspective(float fov_degrees, float aspect, float z_near, float z_far);

Mat4 mat_world_to_camera(const Vec3& position, const Vec4& rotation);

bool camera_position_from_view(const Mat4& view, Vec3& position);

bool w2s(const Mat4& vp, const Vec3& world, float sw, float sh, Vec2& out, bool clip_to_screen = true);
