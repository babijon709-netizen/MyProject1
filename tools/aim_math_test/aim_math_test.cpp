// Регрессия математики мемори-аима (game.cpp: esp_mem_aim_point_at).
//
// Функции ниже скопированы БУКВАЛЬНО из jni/src/game.cpp — конвенции
// (Vec4 = x,y,z,w; world = parent * local) должны совпадать; если их
// меняют в game.cpp, меняют и здесь. Запуск: g++ -O2 aim_math_test.cpp && ./a.out
//
// Сценарий повторяет пайплайн:
//   f0  = rot(Qw, (0,0,-1))                      — ось камеры
//   d   = f0, повёрнутый на err вокруг случайной оси
//   ΔQ  = axis_angle(cross(f0,d), step)          — мировой доворот
//   Ql' = Qp⁻¹ · ΔQ · Qp · Ql                     — в систему родителя
//   f1  = rot(Qp·Ql', (0,0,-1))
// Ожидание: угол f0→f1 = step; угол f1→d = err - step; при step = err — f1 = d.
#include <cmath>
#include <cstdio>
#include <random>

struct Vec3 { float x, y, z; };
struct Vec4 { float x, y, z, w; };

static Vec3 cross_product(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static Vec3 rotate_vector(const Vec4& q, const Vec3& v) {
    Vec3 qq = {q.x, q.y, q.z};
    Vec3 c1 = cross_product(qq, v);
    Vec3 doubled = {c1.x * 2.f, c1.y * 2.f, c1.z * 2.f};
    Vec3 c2 = cross_product(qq, doubled);
    return {v.x + q.w * doubled.x + c2.x, v.y + q.w * doubled.y + c2.y, v.z + q.w * doubled.z + c2.z};
}
static Vec4 multiply_quaternion(const Vec4& a, const Vec4& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
    };
}
static bool normalize_quaternion(Vec4& q) {
    float l2 = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
    if (!std::isfinite(l2) || l2 < 1e-6F) return false;
    float inv = 1.f / sqrtf(l2);
    q.x *= inv; q.y *= inv; q.z *= inv; q.w *= inv;
    return true;
}
static Vec4 quat_from_axis_angle(const Vec3& axis, float ang) {
    float s = sinf(ang * 0.5F);
    return {axis.x * s, axis.y * s, axis.z * s, cosf(ang * 0.5F)};
}

static float vlen(const Vec3& v) { return sqrtf(v.x*v.x + v.y*v.y + v.z*v.z); }
static Vec3 vnorm(const Vec3& v) { float l = vlen(v); return {v.x/l, v.y/l, v.z/l}; }
static float ang_deg(const Vec3& a, const Vec3& b) {
    float d = a.x*b.x + a.y*b.y + a.z*b.z;
    d = d < -1.f ? -1.f : (d > 1.f ? 1.f : d);
    return std::acos(d) * 57.29577951308232F;
}

// Один такт аима из esp_mem_aim_point_at: входы — позы, выход — новый
// локальный поворот камеры (как бы записанный в слот).
static bool one_tick(const Vec4& qp, const Vec4& ql, const Vec3& fwd0,
                     const Vec3& target_dir, float max_step_deg, Vec4& q_l_out) {
    Vec3 d = vnorm(target_dir);
    Vec3 axis = cross_product(fwd0, d);
    const float alen = vlen(axis);
    const float dot = fwd0.x*d.x + fwd0.y*d.y + fwd0.z*d.z;
    const float err = std::atan2(alen, dot < -1.f ? -1.f : (dot > 1.f ? 1.f : dot));
    if (err * 57.29577951308232F < 0.15F) return false;      // мёртвая зона
    if (alen < 1e-6F) return false;
    axis = vnorm(axis);
    float step = err;
    const float cap = max_step_deg * 0.017453292519943295F;
    if (step > cap) step = cap;
    const Vec4 dq = quat_from_axis_angle(axis, step);
    Vec4 qpn = qp; normalize_quaternion(qpn);
    const Vec4 qpinv = {-qpn.x, -qpn.y, -qpn.z, qpn.w};
    Vec4 dq_local = multiply_quaternion(multiply_quaternion(qpinv, dq), qpn);
    Vec4 q_local = multiply_quaternion(dq_local, ql);
    if (!normalize_quaternion(q_local)) return false;
    q_l_out = q_local;
    return true;
}

int main() {
    std::mt19937 rng(20260919u);
    std::uniform_real_distribution<float> u(-1.f, 1.f);
    auto rand_quat = [&]() {
        Vec4 q{u(rng), u(rng), u(rng), u(rng)};
        normalize_quaternion(q);
        return q;
    };
    int fails = 0;
    for (int iter = 0; iter < 20000; ++iter) {
        const bool root = (iter % 2 == 0);   // половина — корень (родитель = identity)
        Vec4 qp = root ? Vec4{0.f, 0.f, 0.f, 1.f} : rand_quat();
        Vec4 ql = rand_quat();
        Vec4 qw = multiply_quaternion(qp, ql);      // world = parent * local
        normalize_quaternion(qw);
        Vec3 fwd0 = vnorm(rotate_vector(qw, {0.f, 0.f, -1.f}));
        // Цель: f0, повёрнутая на случайный угол 1..175° вокруг случайной оси.
        Vec3 rax{u(rng), u(rng), u(rng)};
        rax = vnorm(rax);
        float err_deg = 1.f + std::fabs(u(rng)) * 174.f;   // 1..175°
        Vec3 d = vnorm(rotate_vector(quat_from_axis_angle(rax, err_deg * 0.017453292519943295F), fwd0));

        // Такты до схождения к цели (80 — защитный потолок: при битой
        // математике остаток бы не сходил).
        const float max_step = 8.f + (iter % 10 + 1) * 8.f;   // 16..88°, как в игре
        Vec4 q_l = ql;
        float remain = err_deg;
        int ticks = 0;
        for (; ticks < 80; ++ticks) {
            Vec4 qw2 = multiply_quaternion(qp, q_l);
            normalize_quaternion(qw2);
            Vec3 f = vnorm(rotate_vector(qw2, {0.f, 0.f, -1.f}));
            const float before = ang_deg(f, d);
            Vec4 next;
            if (!one_tick(qp, q_l, f, d, max_step, next)) { remain = before; break; }  // мёртвая зона
            q_l = next;
            Vec4 qw3 = multiply_quaternion(qp, q_l);
            normalize_quaternion(qw3);
            Vec3 f2 = vnorm(rotate_vector(qw3, {0.f, 0.f, -1.f}));
            remain = ang_deg(f2, d);   // остаток до цели после такта
            // После такта остаток ДО цели не должен вырасти — иначе доворот
            // шёл мимо (битый знак оси или порядок умножения).
            if (remain > before + 0.01F) {
                printf("FAIL iter=%d tick=%d: остаток вырос %.2f° -> %.2f° (root=%d)\n",
                       iter, ticks, before, remain, (int)root);
                if (++fails > 5) return 1;
            }
            if (remain < 1.0F) break;   // в мёртвой зоне / у цели
        }
        if (remain > 1.0F) {
            printf("FAIL iter=%d: не сошлось за %d тактов, остаток %.2f° (root=%d, err0=%.1f°, step<=%.0f°)\n",
                   ticks, ticks, remain, (int)root, err_deg, max_step);
            if (++fails > 5) return 1;
        }
    }
    if (fails) { printf("Всего падений: %d\n", fails); return 1; }
    printf("ОК: 20000 сценариев (корень+родитель, err 1..175°, шаг 16..88°) — сходение к цели, остаток не растёт\n");
    return 0;
}
