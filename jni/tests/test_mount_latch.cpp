// Host tests for the mounted-player box hold (mount_latch.h).
//
// Сценарий из лога 08.10, 23:09:07: два пассажира одного транспорта (ид=509113)
// ушли в вечное удержание — «удержаний=59» при кадрах=59..60, «отстал» рос
// 9.4 → 173.4 м за 8.08 с (20.3 м/с), «пропусков=0» всё это время, то есть
// живой отсчёт был каждый кадр. Вышли только когда оба вылетели из реестра
// (23:09:15.252 «визуал: состав 1: +0 -2»). Ключевое: «заморозка» (=saved_still)
// весь инцидент была 0..4 — lastSavedPosition ехал вместе с транспортом, а обе
// ветки защиты написаны под замороженный saved.
#include "mount_latch.h"

#include <cstdio>

namespace {
int failures = 0;
#define CHECK(cond, name) do { \
    if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", name, __LINE__); } \
} while (0)

// 20.3 м/с при 60 fps — темп роста «отстал» из лога.
constexpr float kStepM = 20.3F / 60.0F;

Vec3 along(float meters) { return {meters, 0.0F, 0.0F}; }

// Предикат защиты до правки: saved_still не проверялся, потолка удержания не
// было, а latch.last обновлялся только когда защита не срабатывала. Нужен,
// чтобы тест различал «починено» и «никогда не срабатывало».
bool old_step(MountLatch& latch, const Vec3& saved, const Vec3& live, Vec3& feet) {
    if (!latch.last_ok) { latch.last = live; latch.last_ok = true; feet = live; return false; }
    const float live_h2 = mountlatch::horiz2(live, saved);
    const float last_h2 = mountlatch::horiz2(latch.last, saved);
    const bool guard =
        (live_h2 < 1.25F * 1.25F && last_h2 > 2.5F * 2.5F) ||
        (last_h2 > 4.0F && mountlatch::horiz2(live, latch.last) > 4.0F * 4.0F &&
         live_h2 < last_h2 * 0.25F);
    if (guard) { feet = latch.last; return true; }
    latch.last = live;
    feet = live;
    return false;
}
}  // namespace

int main() {
    // 1. Инцидент из лога: saved живой (едет с транспортом), бокс отстал на
    //    9.4 м. Старая логика держала бы всю секунду — новая обязана догнать.
    {
        MountLatch old_latch{}, latch{};
        old_latch.engaged = true;
        old_latch.last = along(0.0F);   // бокс отстал: запомненная точка позади
        old_latch.last_ok = true;
        latch.engaged = true;
        latch.last = along(0.0F);
        latch.last_ok = true;
        float live_m = 9.4F;
        int old_holds = 0, new_holds = 0;
        for (int frame = 0; frame < 60; ++frame) {
            const Vec3 live = along(live_m);
            const Vec3 saved = live;    // lastSavedPosition едет вместе с транспортом
            const float live_h2 = mountlatch::horiz2(live, saved);
            Vec3 old_feet{}, feet{};
            if (old_step(old_latch, saved, live, old_feet)) ++old_holds;
            if (mountlatch::apply_hold(latch, saved, live, live_h2, /*saved_still=*/2, feet))
                ++new_holds;
            live_m += kStepM;
        }
        CHECK(old_holds == 60, "pre-fix logic holds every frame of that second");
        CHECK(mountlatch::lag_meters(old_latch, along(live_m)) > 29.0F,
              "pre-fix box falls ~20 m behind, as the log shows");
        CHECK(new_holds == 0, "live saved: no hold at all");
        CHECK(mountlatch::lag_meters(latch, along(live_m)) < 0.5F,
              "box rides with the vehicle");
    }

    // 2. Регрессия: тот самый щелчок, ради которого защита написана. saved
    //    заморожен в точке посадки, трансформ на кадр прыгает обратно в неё —
    //    бокс держим ровно этот кадр и отпускаем на следующем.
    {
        MountLatch latch{};
        latch.engaged = true;
        const Vec3 saved = along(0.0F);
        float vehicle_m = 30.0F;

        Vec3 feet{};
        for (int frame = 0; frame < 10; ++frame) {
            const Vec3 live = along(vehicle_m);
            CHECK(!mountlatch::apply_hold(latch, saved, live,
                                          mountlatch::horiz2(live, saved), 500, feet),
                  "riding away from a frozen saved is not a flicker");
            vehicle_m += kStepM;
        }
        const float before_flicker = feet.x;

        const Vec3 snap = along(0.0F);   // щелчок: отсчёт лёг в точку посадки
        CHECK(mountlatch::apply_hold(latch, saved, snap,
                                     mountlatch::horiz2(snap, saved), 500, feet),
              "snap back to the frozen boarding point is held");
        CHECK(feet.x == before_flicker, "held frame keeps the previous box");

        const Vec3 live = along(vehicle_m);
        CHECK(!mountlatch::apply_hold(latch, saved, live,
                                      mountlatch::horiz2(live, saved), 500, feet),
              "hold lasts exactly one frame");
        CHECK(feet.x == live.x, "box is back on the fresh sample");
        CHECK(latch.hold_streak == 0, "hold streak resets after release");
    }

    // 3. Аварийный выход: saved заморожен и защита срабатывает каждый кадр.
    //    Без потолка latch.last не обновляется никогда (он пишется только когда
    //    защита не сработала) — ровно то, что и произошло 08.10.
    {
        MountLatch latch{};
        latch.engaged = true;
        latch.last = along(30.0F);
        latch.last_ok = true;
        const Vec3 saved = along(0.0F);
        const Vec3 live = along(0.0F);   // отсчёт навсегда лёг в точку посадки

        int held = 0;
        Vec3 feet{};
        for (int frame = 0; frame < mountlatch::kMaxHoldStreak + 5; ++frame) {
            if (mountlatch::apply_hold(latch, saved, live,
                                       mountlatch::horiz2(live, saved), 500, feet))
                ++held;
        }
        CHECK(held == mountlatch::kMaxHoldStreak,
              "hold is capped at kMaxHoldStreak consecutive frames");
        CHECK(feet.x == live.x, "after the cap the box resyncs to the live sample");
        CHECK(latch.hold_count == mountlatch::kMaxHoldStreak,
              "hold_count still reports every held frame for the log");
    }

    // 4. Живой отсчёт гасит miss_streak: «пропусков» в логе считается отдельно.
    {
        MountLatch latch{};
        latch.engaged = true;
        latch.miss_streak = 7;
        Vec3 feet{};
        const Vec3 live = along(5.0F);
        mountlatch::apply_hold(latch, live, live, 0.0F, 0, feet);
        CHECK(latch.miss_streak == 0, "live sample clears the miss streak");
        CHECK(!latch.last_ok == false, "first live sample arms the latch");
    }

    if (failures) { printf("test_mount_latch: %d failure(s)\n", failures); return 1; }
    printf("test_mount_latch: ok\n");
    return 0;
}
