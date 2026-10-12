// Хостовый прогон логики автохила (jni/src/auto_heal_logic.h): без игры, без
// uinput — только состояние, таймеры и решение. Проверяем ровно те свойства,
// из-за которых автохил может стать вредным: лечение «на ровном месте», вечное
// дёрганье кнопок после неудачной серии, тапы при недоступной инъекции и
// отпускание пальца при любой потере условий.

#include "auto_heal_logic.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace autoheal;

static int g_fail = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);      \
            ++g_fail;                                                       \
        }                                                                   \
    } while (0)

namespace {

// Прогон кадров: вход один и тот же, пока не кончится время; собираем
// суммарное время «палец на слоте» и «палец на кнопке», число начатых попыток.
struct Run {
    Machine m;
    Inputs in;
    Settings s;
    double slot_ms = 0.0;
    double use_ms = 0.0;
    int started = 0;
    int finished = 0;
    int successes = 0;
    int gave_up = 0;
    bool keep_damage = false;   // бой продолжается: урон обновляется каждый кадр

    void frame(double dt = 1.0 / 60.0) {
        in.dt = (float)dt;
        if (keep_damage) in.damageAt = in.now;
        Outputs out;
        m.tick(in, s, out);
        if (out.slotDown) slot_ms += dt * 1000.0;
        if (out.useDown) use_ms += dt * 1000.0;
        if (out.attemptStarted) ++started;
        if (out.attemptFinished) ++finished;
        if (out.success) ++successes;
        if (out.gaveUp) ++gave_up;
        in.now += dt;
    }

    void frames(double seconds, double dt = 1.0 / 60.0) {
        for (int i = 0; i < (int)(seconds / dt + 0.5); ++i) frame(dt);
    }
};

Inputs MakeInputs() {
    Inputs in;
    in.enabled = true;
    in.gameReady = true;
    in.inject = true;
    in.health = 100.f;
    in.healthMax = 100.f;
    in.damageAt = -1.0;
    in.now = 0.0;
    return in;
}

}  // namespace

int main() {
    // ---- 1. Выключено: ничего не делаем ни при каких условиях --------------
    {
        Run r;
        r.in = MakeInputs();
        r.in.enabled = false;
        r.in.health = 20.f;
        r.in.damageAt = 0.0;
        r.frames(3.0);
        CHECK(r.started == 0, "disabled: no attempts");
        CHECK(r.slot_ms == 0.0 && r.use_ms == 0.0, "disabled: finger stays up");
        CHECK(r.m.reason == REASON_OFF, "disabled: reason OFF");
    }

    // ---- 2. Только порог (combatOnly=false): полный цикл и успех -----------
    {
        Run r;
        r.s = Settings{};
        r.s.combatOnly = false;
        r.in = MakeInputs();
        r.in.health = 40.f;            // ниже порога 60
        r.frames(0.05);                // кадр-два до реакции
        CHECK(r.started == 0, "react: no tap before the reaction delay");
        r.frames(0.30);                // реакция 0.18 с
        CHECK(r.m.stage == STAGE_TAP_SLOT || r.m.stage == STAGE_SELECT_WAIT ||
              r.m.stage == STAGE_HOLD_USE, "cycle: reached the tap/hold phase");
        r.frames(1.0);
        CHECK(r.slot_ms > 50.0 && r.slot_ms < 120.0, "tap: slot finger ~70 ms");
        CHECK(r.m.stage == STAGE_HOLD_USE, "hold: holding the use button");
        r.in.health = 75.f;            // аптечка сработала выше порога
        r.frames(0.2);
        CHECK(r.successes == 1, "success: attempt counted as successful");
        CHECK(r.m.stage == STAGE_IDLE, "success: back to idle");
        CHECK(r.use_ms > 200.0, "hold: use button was really held");
        r.frames(3.0);
        CHECK(r.started == 1, "success: no new series while healthy");
    }

    // ---- 3. Только в бою: без свежего урона не лечимся ---------------------
    {
        Run r;
        r.in = MakeInputs();
        r.in.health = 30.f;            // рана есть, урона не видели
        r.frames(5.0);
        CHECK(r.started == 0, "combat: nothing without damage");
        CHECK(r.m.reason == REASON_NO_COMBAT, "combat: reason NO_COMBAT");

        r.in.damageAt = 5.0;          // урон только что
        r.frames(0.5);
        CHECK(r.started == 1, "combat: series starts on fresh damage");

        // Урон старше окна — новая серия не начинается.
        Run r2;
        r2.in = MakeInputs();
        r2.in.health = 30.f;
        r2.in.damageAt = 0.0;
        r2.in.now = 10.0;             // 10 с > окна 6 с
        r2.frames(1.0);
        CHECK(r2.started == 0, "combat: stale damage does not trigger");
    }

    // ---- 4. Мёртв / нет данных / нет инъекции ------------------------------
    {
        Run r;
        r.in = MakeInputs();
        r.in.health = 0.f;
        r.in.damageAt = 0.0;
        r.frames(2.0);
        CHECK(r.started == 0, "dead: no attempts");
        CHECK(r.m.reason == REASON_DEAD, "dead: reason DEAD");

        Run r2;
        r2.in = MakeInputs();
        r2.in.health = -1.f;          // чтение не удалось
        r2.in.damageAt = 0.0;
        r2.frames(2.0);
        CHECK(r2.started == 0, "no data: no attempts");
        CHECK(r2.m.reason == REASON_NO_DATA, "no data: reason NO_DATA");

        Run r3;
        r3.in = MakeInputs();
        r3.in.inject = false;
        r3.in.health = 20.f;
        r3.in.damageAt = 0.0;
        r3.frames(2.0);
        CHECK(r3.started == 0, "no inject: no attempts");
        CHECK(r3.m.reason == REASON_NO_INJECT, "no inject: reason NO_INJECT");
    }

    // ---- 5. Инъекция пропала посреди серии: палец отпущен ------------------
    {
        Run r;
        r.in = MakeInputs();
        r.in.health = 40.f;
        r.in.damageAt = 0.0;
        r.frames(0.70);               // дошло до удержания
        CHECK(r.use_ms > 0.0, "abort: holding before the injection is lost");
        const double held = r.use_ms;
        r.in.inject = false;
        r.frames(0.2);
        CHECK(r.use_ms == held, "abort: hold stopped immediately");
        CHECK(r.m.stage == STAGE_IDLE, "abort: back to idle");
    }

    // ---- 6. Не подействовало: ограниченное число попыток и backoff ---------
    {
        // 6a. Бой идёт (урон обновляется), аптечка не срабатывает: серия
        // обязана кончиться на maxTries и уйти в тишину, а не крутиться вечно.
        Run r;
        r.s.maxTries = 3;
        r.in = MakeInputs();
        r.in.health = 40.f;           // HP не растёт: аптечки нет/слот пустой
        r.in.damageAt = 0.0;
        r.keep_damage = true;
        r.frames(13.0);               // три попытки по ~4 с + пауза перед сдачей
        CHECK(r.started == 3, "fail: exactly maxTries attempts per burst");
        CHECK(r.gave_up == 1, "fail: gave up once");
        CHECK(r.m.stage == STAGE_BACKOFF, "fail: sitting in backoff");
        r.frames(6.0);                // пауза: попыток нет
        CHECK(r.started == 3, "fail: silence during backoff");
        r.frames(7.0);                // пауза кончилась — новая серия на новом уроне
        CHECK(r.started == 4, "fail: new burst after backoff");

        // 6b. Бой кончился (урон старый): серия закрывается сама и НЕ
        // перезапускается на той же ране — иначе кнопки дёргались бы вечно.
        Run r2;
        r2.s.maxTries = 3;
        r2.in = MakeInputs();
        r2.in.health = 40.f;
        r2.in.damageAt = 0.0;
        r2.frames(20.0);
        const int once = r2.started;
        CHECK(once >= 1 && once <= 3, "fail: attempts under way inside the fight");
        CHECK(r2.m.stage == STAGE_IDLE && r2.m.reason == REASON_NO_COMBAT,
              "fail: wait idle when the fight is over");
        r2.frames(30.0);
        CHECK(r2.started == once, "fail: no new series on the same old wound");
    }

    // ---- 7. Условие ушло — серия закрывается, новый урон начинает заново ---
    {
        Run r;
        r.in = MakeInputs();
        r.in.health = 40.f;
        r.in.damageAt = 0.0;
        r.frames(0.5);
        const int started = r.started;
        CHECK(started == 1, "rearm: series started");
        r.in.health = 95.f;           // вылечился сам
        r.frames(0.5);
        r.in.health = 40.f;           // снова рана, но без нового урона
        r.in.damageAt = -1.0;
        r.frames(2.0);
        CHECK(r.started == started, "rearm: threshold alone does not restart");
        r.in.damageAt = r.in.now;     // новый бой
        r.frames(0.5);
        CHECK(r.started == started + 1, "rearm: fresh damage restarts the series");
    }

    // ---- 8. Успех определяется и по одному факту роста HP ------------------
    {
        Run r;
        r.s.hpPct = 95.f;             // порог высокий: до него аптечка не дотянет
        r.in = MakeInputs();
        r.in.health = 60.f;
        r.in.damageAt = 0.0;
        r.frames(0.70);               // дошли до удержания
        CHECK(r.m.stage == STAGE_HOLD_USE, "rise: holding");
        r.in.health = 62.f;           // +2 HP, но всё ещё ниже порога
        r.frames(4.0);                // удержание и пауза прошли
        CHECK(r.successes == 1, "rise: counted as success by HP rise");
        CHECK(r.started == 1, "rise: no retry after a successful heal");
    }

    // ---- 9. Трекер урона ---------------------------------------------------
    {
        DamageTracker t;
        // Падение HP — урон.
        t.observe(true, 100.f, false, -1.f, 0.0);
        t.observe(true, 100.f, false, -1.f, 1.0);
        CHECK(t.at < 0.0, "tracker: no damage yet");
        t.observe(true, 92.f, false, -1.f, 2.0);
        CHECK(t.at == 2.0, "tracker: HP drop registers damage");
        // Метка урона: изменение вверх — тоже урон.
        DamageTracker t2;
        t2.observe(true, 100.f, true, 10.0f, 0.0);
        t2.observe(true, 100.f, true, 10.0f, 1.0);
        CHECK(t2.at < 0.0, "tracker: unchanged stamp is not damage");
        t2.observe(true, 100.f, true, 10.5f, 2.0);
        CHECK(t2.at == 2.0, "tracker: stamp change registers damage");
        // Метка «счётчиком каждый кадр» — источник сам выключается.
        DamageTracker t3;
        float stamp = 0.f;
        for (int i = 0; i < 200; ++i) {
            stamp += 0.1f;
            t3.observe(true, 100.f, true, stamp, (double)i / 60.0);
        }
        CHECK(!t3.stamp_useful, "tracker: streaming stamp is rejected");
        // Метка, поехавшая назад, тоже не «время урона».
        DamageTracker t4;
        t4.observe(true, 100.f, true, 50.0f, 0.0);
        t4.observe(true, 100.f, true, 20.0f, 0.1);
        CHECK(!t4.stamp_useful, "tracker: going backwards disables the stamp");
        // Потеря чтения HP не выдаёт скачок за урон.
        DamageTracker t5;
        t5.observe(true, 100.f, false, -1.f, 0.0);
        t5.observe(false, -1.f, false, -1.f, 1.0);
        t5.observe(true, 30.f, false, -1.f, 2.0);
        CHECK(t5.at < 0.0, "tracker: first HP after a read gap is not damage");
    }

    if (g_fail == 0) std::printf("ALL AUTO-HEAL TESTS PASS\n");
    return g_fail == 0 ? 0 : 1;
}
