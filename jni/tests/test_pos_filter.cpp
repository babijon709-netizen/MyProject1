// Хост-тест временного фильтра позиций (position_filter.h) — без устройства.
#include "position_filter.h"

#include <cstdio>
#include <cmath>

namespace {
int failures = 0;
#define CHECK(cond, name) do { \
    if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", name, __LINE__); } \
} while (0)

using posfilter::State;
using posfilter::Obs;

float dist(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

bool step(State& s, Obs& o, Vec3& pos, bool read_ok, double t) {
    Vec3 vel{};
    return posfilter::filter(s, read_ok, pos, vel, t, o);
}
} // namespace

int main() {
    // 1) Первый кадр: без чтения — не рисуем, с чтением — рисуем сразу.
    {
        State s; Obs o;
        Vec3 p{10, 64, 10};
        CHECK(!step(s, o, p, false, 0.0), "no draw without first read");
        CHECK(step(s, o, p, true, 0.0), "first read draws");
        CHECK(dist(p, {10, 64, 10}) < 1e-5F, "first pos passthrough");
    }
    // 2) Холд: сбои до 5 секунд — бокс живёт (предсказание), больше — гаснет.
    //    Это критично для устройств с длинными сериями EIO/read-gap.
    {
        State s; Obs o;
        Vec3 p{10, 64, 10};
        CHECK(step(s, o, p, true, 0.0), "seed");
        Vec3 q{};
        CHECK(step(s, o, q, false, 0.5), "hold 0.5s alive");
        CHECK(dist(q, {10, 64, 10}) < 1e-5F, "hold keeps last pos");
        CHECK(step(s, o, q, false, 0.9), "hold 0.9s alive");
        CHECK(step(s, o, q, false, 1.5), "hold 1.5s still alive");
        CHECK(step(s, o, q, false, 4.9), "hold 4.9s still alive");
        CHECK(!step(s, o, q, false, 5.5), "hold dies after 5s");
        // Умер = неизвестно где: следующий отсчёт принимаем как первый,
        // без подтверждения скачка (убежавший куда-то игрок не залипает
        // в «неподтверждённом» состоянии навсегда).
        Vec3 far{600, 64, 600};
        CHECK(step(s, o, far, true, 5.55), "reborn first read draws");
        CHECK(dist(far, {600, 64, 600}) < 1e-4F, "reborn at new place");
    }
    // 3) Движение пешком принимается без подтверждений.
    {
        State s; Obs o;
        Vec3 p{10, 64, 10};
        CHECK(step(s, o, p, true, 0.0), "seed");
        for (int i = 1; i <= 10; ++i) {
            Vec3 m{10.0F + 0.3F * i, 64.0F, 10.0F};
            CHECK(step(s, o, m, true, 0.033 * i), "walk accepted");
            CHECK(dist(m, {10.0F + 0.3F * i, 64.0F, 10.0F}) < 1e-4F, "walk passthrough");
        }
        CHECK(o.susp == 0, "walk no suspicions");
    }
    // 4) Мусор: координаты пляшут по карте — не подтверждаются никогда.
    {
        State s; Obs o;
        Vec3 p{10, 64, 10};
        CHECK(step(s, o, p, true, 0.0), "seed");
        const Vec3 junk[] = {{500, 64, 500}, {-320, 64, -120}, {900, 200, -700},
                             {80, -15, 400}, {-50, 64, -610}};
        for (int i = 0; i < 5; ++i) {
            Vec3 m = junk[i];
            CHECK(step(s, o, m, true, 0.033 * (i + 1)), "junk frame still drawn");
            CHECK(dist(m, {10, 64, 10}) < 1e-4F, "junk rejected, keeps old");
        }
        CHECK(o.conf == 0, "junk never confirms");
    }
    // 5) Телепорт: три отсчёта подряд в одной точке — подтверждение.
    //    (Критично: требуется 3 повтора, а не 2, чтобы не гасить на шуме.)
    {
        State s; Obs o;
        Vec3 p{10, 64, 10};
        CHECK(step(s, o, p, true, 0.0), "seed");
        Vec3 m{400, 64, 400};
        step(s, o, m, true, 0.033);
        CHECK(dist(m, {10, 64, 10}) < 1e-4F, "teleport frame1 holds");
        m = {400, 64, 400};  // фильтр перезаписал верхний кадр — подаём заново
        step(s, o, m, true, 0.066);
        CHECK(dist(m, {10, 64, 10}) < 1e-4F, "teleport frame2 still holds (need 3)");
        m = {400, 64, 400};
        step(s, o, m, true, 0.099);
        CHECK(dist(m, {400, 64, 400}) < 1e-4F, "teleport frame3 confirmed");
        CHECK(o.conf == 1, "teleport confirmed once");
    }
    // 6) Перелёт на коптере 100 м/с @30fps: шаг 3.3м, «та же точка» никогда —
    //    но ломаная связная: подтверждение кадра за три, дальше бокс догоняет
    //    реальную позицию в пределах пары шагов (не зависает на месте посадки).
    {
        State s; Obs o;
        Vec3 p{10, 64, 10};
        CHECK(step(s, o, p, true, 0.0), "seed");
        Vec3 m{113.3F, 64, 10.6F};
        step(s, o, m, true, 0.033);
        m = {116.6F, 64, 11.2F};
        step(s, o, m, true, 0.066);
        CHECK(dist(m, {10, 64, 10}) < 1e-4F, "flight frame2 still held");
        m = {119.9F, 64, 11.8F};
        step(s, o, m, true, 0.099);
        CHECK(o.conf >= 1, "flight confirmed by frame3");
        // Догоняющий цикл: на 100+ м/с полёт проходит по цепочке каждые ~2
        // кадра. Проверяем, что бокс не отстаёт больше, чем на два шага.
        double t = 0.099;
        float rx = 119.9F, rz = 11.8F;
        Vec3 last_drawn{};
        for (int i = 0; i < 8; ++i) {
            t += 0.033;
            rx += 3.3F; rz += 0.6F;
            Vec3 q{rx, 64, rz};
            step(s, o, q, true, t);
            last_drawn = q;
        }
        CHECK(dist(last_drawn, {rx, 64, rz}) < 7.5F, "flight keeps up");
    }
    // 7) Восстановление после смерти-холда: умерший трек = «позиции нет»,
    //    первая же живая точка рисуется сразу (как свежий спавн), а дальнейший
    //    полёт подтверждается ломаной — никакого вечного «неподтверждённого».
    {
        State s; Obs o;
        Vec3 p{10, 64, 10};
        CHECK(step(s, o, p, true, 0.0), "seed");
        Vec3 q{};
        CHECK(!step(s, o, q, false, 6.0), "gap kills box after 5s hold");
        // Чтения вернулись: игрок уже далеко и летит прочь.
        Vec3 m{310, 64, 10};
        CHECK(step(s, o, m, true, 6.033), "resume restarts as first draw");
        CHECK(dist(m, {310, 64, 10}) < 1e-4F, "resume at real new place");
        m = {313.3F, 64, 10.6F};
        CHECK(step(s, o, m, true, 6.066), "resume frame2");
        m = {316.6F, 64, 11.2F};
        CHECK(step(s, o, m, true, 6.099), "resume frame3");
        m = {319.9F, 64, 11.8F};
        CHECK(step(s, o, m, true, 6.132), "resume frame4");
        CHECK(o.conf >= 1, "resume confirmed");
        CHECK(dist(m, {319.9F, 64, 11.8F}) < 1e-4F, "resume follows real pos");
    }
    // 9) Холд не дрейфует: дорисовка по скорости ограничена dt<=0.25 с и
    //    лимитом 1.5 м, поэтому после 0.25 с позиция стоит на месте.
    {
        State s; Obs o;
        Vec3 p{10, 64, 10};
        CHECK(step(s, o, p, true, 0.0), "drift seed");
        Obs o2;
        Vec3 h{};
        CHECK(posfilter::filter(s, false, h, Vec3{6, 0, 0}, 0.25, o2), "drift hold 0.25");
        const float at025 = h.x;
        CHECK(posfilter::filter(s, false, h, Vec3{6, 0, 0}, 3.0, o2), "drift hold 3s");
        CHECK(std::fabs(h.x - at025) < 1e-4F, "hold does not drift past 0.25s");
        CHECK(h.x - 10.F <= posfilter::kPosExtrapolateLimit + 1e-4F, "hold within extrapolate limit");
    }
    if (failures) { printf("%d FAILURES\n", failures); return 1; }
    printf("ALL POS-FILTER TESTS PASS\n");
    return 0;
}
