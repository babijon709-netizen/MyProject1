#include "auto_heal.h"
#include "auto_heal_logic.h"
#include "app_state.h"
#include "game.h"
#include "hud.h"                    // menu_open, VisibleScreen
#include "menu.h"                   // g_calibMode
#include "process.h"                // g_esp_attached
#include "Android_touch/TouchHelperA.h"
#include <cmath>
#include <ctime>

// Драйвер автохила. Логика целиком в auto_heal_logic.h — здесь только то, что
// нельзя проверить на хосте: чтение живой игры и синтетические касания.
//
// Палец: слот 1. Слоты 0 (джойстик) и 2 (удар) держит автофарм, у аима свой
// слот, поэтому автохил никому не мешает и работает в бою одновременно с
// аимом — это и есть его смысл: лечиться, не прекращая стрелять.
//
// Что НЕ делается:
//   * нет ни одной записи в память игры — аптечка применяется обычным тапом по
//     слоту и удержанием кнопки использования, все проверки (есть ли предмет,
//     кулдаун, анимация) остаются у игры;
//   * нет тапов при калибровке зон (иначе калибровка записала бы наш же тап) и
//     пока не заданы обе точки;
//   * нет «лечения по расписанию»: серия начинается только по свежему урону и
//     только если здоровье ниже порога.

float g_healHpPct = -1.f;
int   g_healStage = 0;
int   g_healReason = 0;
int   g_healTries = 0;
int   g_healAttempts = 0;
int   g_healSuccesses = 0;
float g_healSinceHit = -1.f;

namespace {

autoheal::Machine s_machine;
autoheal::DamageTracker s_damage;
autoheal::Settings s_settings;

// Палец драйвера: -1 отпущен, 0 на слоте, 1 на кнопке использования.
int  s_finger = -1;
bool s_attached_was = false;
double s_attached_at = 0.0;

// Первые секунды после привязки не лечим: мир ещё грузится, здоровье может
// прочитаться нулём (персонаж не заспавнен), и «HP 0» ушло бы в аптечку.
constexpr double kAttachSettleSec = 4.0;

double NowSeconds() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

float ClampF(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void ReleaseFinger() {
    if (s_finger >= 0) {
        Touch_Up_N(1);
        s_finger = -1;
    }
}

// Точка в пикселях: доли ЗАПИСАНЫ по VisibleScreen (см. калибровку), поэтому и
// пересчитываем через неё — иначе на устройстве с другим соотношением сторон
// слот аптечки уехал бы вместе с растягом экрана.
void ZonePoint(float fx, float fy, float& px, float& py) {
    float w = 0.f, h = 0.f;
    VisibleScreen(w, h);
    if (w < 100.f) w = 1080.f;
    if (h < 100.f) h = 1920.f;
    px = ClampF(fx, 0.f, 1.f) * w;
    py = ClampF(fy, 0.f, 1.f) * h;
}

}  // namespace

void UpdateAutoHeal(float dt) {
    const double now = NowSeconds();

    // --- Привязка ------------------------------------------------------------
    const bool attached = g_esp_attached;
    if (attached && !s_attached_was) s_attached_at = now;
    if (!attached && s_attached_was) {
        // Перепривязка: состояние серии не переносим (новый мир/новая рана).
        ReleaseFinger();
        s_machine.reset();
        s_damage.reset();
        s_machine.armed = true;
    }
    s_attached_was = attached;

    // --- Настройки из состояния (меню правит их на ходу) ---------------------
    s_settings.hpPct = ClampF(g_state.autoheal_hp, 5.f, 95.f);
    s_settings.combatSec = ClampF(g_state.autoheal_combat_s, 0.f, 30.f);
    s_settings.holdMs = ClampF(g_state.autoheal_hold_ms, 150.f, 6000.f);
    s_settings.cooldownSec = ClampF(g_state.autoheal_cooldown_s, 0.3f, 10.f);
    s_settings.reactMs = ClampF(g_state.autoheal_react_ms, 0.f, 800.f);
    s_settings.combatOnly = g_state.autoheal_combat_only;
    s_settings.maxTries = g_state.autoheal_tries < 1 ? 1
                        : (g_state.autoheal_tries > 6 ? 6 : g_state.autoheal_tries);

    // --- Здоровье ------------------------------------------------------------
    float hp = -1.f, hp_max = -1.f, stamp = -1.f;
    const bool vitals_ok = esp_local_vitals(hp, hp_max, stamp);
    const bool hp_ok = vitals_ok && hp >= 0.f;
    s_damage.observe(hp_ok, hp, vitals_ok && stamp >= 0.f, stamp, now);

    // --- Гейты ---------------------------------------------------------------
    const bool zones_set = g_state.autoheal_slot_x >= 0.f && g_state.autoheal_use_x >= 0.f;
    const bool settled = attached && (now - s_attached_at) >= kAttachSettleSec;
    const bool can_inject = Touch_CanInject() && g_calibMode == 0;

    autoheal::Inputs in;
    in.dt = dt;
    in.enabled = g_state.autoheal_on;
    in.gameReady = settled;
    in.inject = can_inject && zones_set;
    in.health = hp;
    in.healthMax = hp_max;
    in.damageAt = s_damage.at;
    in.now = now;

    autoheal::Outputs out;
    s_machine.tick(in, s_settings, out);

    // --- Палец ---------------------------------------------------------------
    float sx = 0.f, sy = 0.f, ux = 0.f, uy = 0.f;
    if (zones_set) {
        ZonePoint(g_state.autoheal_slot_x, g_state.autoheal_slot_y, sx, sy);
        ZonePoint(g_state.autoheal_use_x, g_state.autoheal_use_y, ux, uy);
    }
    if (out.slotDown && zones_set) {
        if (s_finger != 0) {
            if (s_finger >= 0) Touch_Up_N(1);   // смена точки без «залипания»
            Touch_Down_N(1, sx, sy);
            s_finger = 0;
        }
    } else if (out.useDown && zones_set) {
        if (s_finger != 1) {
            if (s_finger >= 0) Touch_Up_N(1);
            Touch_Down_N(1, ux, uy);
            s_finger = 1;
        }
    } else {
        ReleaseFinger();
    }

    // --- Статус --------------------------------------------------------------
    g_healHpPct = s_machine.hpPct;
    g_healStage = s_machine.stage;
    g_healTries = s_machine.tries;
    g_healSinceHit = s_damage.at > 0.0 ? (float)(now - s_damage.at) : -1.f;

    // Причина простоя: у драйвера есть свои, о которых машина не знает.
    int reason = s_machine.reason;
    if (g_state.autoheal_on) {
        if (!zones_set)       reason = autoheal::REASON_NOT_SET;
        else if (!settled)    reason = autoheal::REASON_NO_GAME;
        else if (!can_inject) reason = autoheal::REASON_NO_INJECT;
    } else {
        reason = autoheal::REASON_OFF;
    }
    g_healReason = reason;

    // Счётчики сессии — из машины (она считает попытки и успехи).
    g_healAttempts = s_machine.doneAttempts;
    g_healSuccesses = s_machine.successes;
}
