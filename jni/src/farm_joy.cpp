#include "farm_joy.h"

#include "benz_log.h"
#include "farm_joy_core.h"   // логика выбора слоя (тестируется на хосте)
#include "game_internal.h"
#include <cmath>
#include <cstring>

using namespace go_active;   // смещения читаются как в game_markers/game_farm

// Реализация. Подробности «почему так» — в farm_joy.h; здесь коротко о
// раскладке, на которую опираемся (дамп 11949):
//   Joystick Pack (неймспейс пустой): handleRange 0x54, deadZone 0x58,
//     axisOptions 0x5C, snapX/Y 0x60/0x61, background 0x68, handle 0x70,
//     baseJoystick 0x78, baseRect 0x80, canvas 0x88, cam 0x90,
//     input Vector2 0x98, путь контроля Input System 0xB0. Компонент лежит на
//     GameObject'ах FixedJoystick (0x78) и FloatingJoystick (0x90) держателя
//     ScreenControlsReferenceHolder;
//   Oxide.PlayerInputHandler: <qNU> PlayerManager backref 0x30, PlayerInput
//     qNa 0x40 (первые 8 байт — MovementInput), Nullable<Vector2> qNf 0x6C и
//     qNP 0x78 (пять публичных сеттеров glO/uov/glb/nh/lE сходятся в эти два
//     поля; в дампе есть NetworkWriter/ReadBlittableNullable<Vector2>, то есть
//     это и есть ввод, уходящий на сервер);
//   PlayerEventHandler (Vsh): manager backref 0xD0, MovementInput
//     Vsf<Vector2> 0x128, LookDirection Vsf<Vector3> 0x140; значение внутри
//     обёртки Vsf — по +0x20 (SYNC_VALUE_OFFSET, тот же слот, куда пишет
//     esp_mem_aim_write_fire_dir для оси выстрела).
//
// Nullable<T> в il2cpp раскладывается по порядку полей метадаты (в дампе:
// hasValue, затем value), то есть hasValue по +0, значение по +4 (для Vector2
// итого 0xC байт — сходится с шагом полей 0x6C -> 0x78). Из дампа это не
// доказать, поэтому для обоих nullable-полей есть по два кандидата: обычная
// раскладка и обратная (значение по +0, hasValue по +8). При неверной
// раскладке байт флага попадает в мантиссу float, то есть игра прочитает
// почти нулевой вектор — худший случай безопасен, а проверка по сближению
// такой кандидат всё равно отбракует.

namespace farmjoy {
namespace {

constexpr float kStickMax  = 1.00f;   // предел магнитуды стика
constexpr float kProbeMax  = 0.60f;   // предел магнитуды непроверенного слота ввода
constexpr float kResolveT  = 1.00f;   // как часто перечитываем цепочку целиком, с
constexpr float kAckAlpha  = 0.25f;   // сглаживание скорости сближения (диагностика)
// Длины и пороги пробы — в farm_joy_core.h (там же вся логика выбора слоя).

enum {
    WHY_OK = 0,
    WHY_NO_STATICS,      // нет статиков GameControllerBase
    WHY_NO_HANDLER,      // нет PlayerInputHandler (статика <qSt>)
    WHY_HANDLER_OWNER,   // хендлер ввода принадлежит другому игроку
    WHY_NO_PLAYER,       // нет локального игрока
    WHY_NO_EVENT,        // нет PlayerEventHandler у локального игрока
    WHY_NO_CHANNEL,      // не сошлась раскладка канала движения (Vsf)
    WHY_WRITE,           // запись не прошла (память отвалилась)
    WHY_NO_SPRINT,       // не сошлась цепочка кнопки бега (LD/InputAction)
};

// Раскладка Nullable<Vector2>: основной вариант (hasValue по +0, значение по
// +4) и обратный (значение по +0, hasValue по +8) — см. комментарий выше.
enum { NUL_NONE = -1, NUL_HV0 = 0, NUL_V0 = 1 };

// Проверенная цель записи. Перед КАЖДОЙ записью сверяем, что объект-владелец
// не освобождён и не переиспользован (по указателю класса) и что его обратная
// ссылка на месте: компонент — на свой GameObject, хендлер — на локального
// игрока (PlayerManager), Vsh — на локального игрока.
struct Target {
    bool     ok = false;
    int      why = WHY_OK;
    int      nul = NUL_NONE;   // NUL_NONE — обычный Vector2, иначе раскладка Nullable
    uint64_t addr = 0;         // куда писать (для Nullable — база)
    uint64_t addr2 = 0;        // второй стик (Fixed и Floating вместе), 0 — нет
    uint64_t owner = 0;        // объект-владелец
    uint64_t klass = 0;        // его класс на момент резолва
    uint64_t back_off = 0;     // смещение обратной ссылки
    uint64_t back_eq = 0;      // 0 — «ссылка непустая», иначе — равна этому
    unsigned sticks = 0;       // биты найденных стиков (1 — Fixed, 2 — Floating)
};

// Атака: байт «кнопка атаки зажата» у хендлера ввода. Своих проверок у него
// не больше, чем у стика: тот же resolve_handler (класс PlayerInputHandler +
// обратная ссылка на локального игрока), а значение — один байт, который сама
// игра пишет тем же способом (strb по +0x68 в gKo) и читает в ветке
// непрерывного удара (0x67E4ADC: if (0x68) Vsh.AttackContinuously.Try()).
unsigned char s_attack_value = 0;   // что мы туда положили (0 — ещё не писали)
int s_attack_why = WHY_OK;

bool vec_ok(const float v[2]) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) &&
           fabsf(v[0]) <= 1.5f && fabsf(v[1]) <= 1.5f;
}

bool alive(const Target& t) {
    if (!t.ok || t.owner < 0x10000 || t.klass < 0x10000) return false;
    if (rd_ptr(t.owner) != t.klass) return false;   // освобождён/переиспользован
    if (!t.back_off) return true;
    const uint64_t link = rd_ptr(t.owner + t.back_off);
    return t.back_eq ? (link == t.back_eq) : (link >= 0x10000);
}

// --- запись ---------------------------------------------------------------

bool write_target(const Target& t, float x, float y) {
    if (!t.ok || !t.addr) return false;
    if (t.nul == NUL_NONE) {
        const float v[2] = {x, y};
        bool written = wr_buf(t.addr, v, sizeof(v));
        if (t.addr2) written |= wr_buf(t.addr2, v, sizeof(v));
        return written;
    }
    // Nullable<Vector2>: 12 байт — флаг и два float по раскладке.
    unsigned char buf[0xC] = {};
    const float v[2] = {x, y};
    if (t.nul == NUL_HV0) {
        buf[0] = 1;                        // hasValue
        memcpy(buf + 4, v, sizeof(v));     // value
    } else {
        memcpy(buf, v, sizeof(v));         // value
        buf[8] = 1;                        // hasValue
    }
    return wr_buf(t.addr, buf, sizeof(buf));
}

// Обнулить цель: у Nullable вместе с флагом — состояние «стик отпущен».
void zero_target(const Target& t) {
    if (!alive(t)) return;
    write_target(t, 0.0f, 0.0f);
}

// --- резолв ---------------------------------------------------------------

// Компонент-стик на GameObject: сперва точные имена классов ассета (они
// уникальны), затем базовый "Joystick". Имя сверяет gameobject_component_by_class;
// базовый требует ещё проверки раскладки: у игрового Oxide.InputSystem.Joystick
// с тем же именем поля лежат по 0x20..0x48, и 0x98 там — чужая память.
uint64_t stick_component_of(uint64_t native_go) {
    static const char* kExact[] = {"FixedJoystick", "FloatingJoystick",
                                   "DynamicJoystick", "VariableJoystick"};
    for (const char* name : kExact) {
        const uint64_t component = gameobject_component_by_class(native_go, name);
        if (component) return component;
    }
    const uint64_t component = gameobject_component_by_class(native_go, "Joystick");
    if (!component) return 0;
    const float handle_range = rd<float>(component + JOYSTICK_HANDLE_RANGE);
    const float dead_zone    = rd<float>(component + JOYSTICK_DEAD_ZONE);
    if (!std::isfinite(handle_range) || !(handle_range > 0.01f && handle_range <= 2.0f)) return 0;
    if (!std::isfinite(dead_zone) || !(dead_zone >= 0.0f && dead_zone < 1.0f)) return 0;
    if (rd_ptr(component + JOYSTICK_BACKGROUND) < 0x10000) return 0;
    if (rd_ptr(component + JOYSTICK_HANDLE) < 0x10000) return 0;
    float value[2] = {};
    if (!rd_buf(component + JOYSTICK_INPUT_VALUE, value, sizeof(value)) || !vec_ok(value)) return 0;
    return component;
}

// Статики GameControllerBase — общий вход для всех цепочек ниже.
uint64_t statics_or_why(int& why) {
    const uint64_t statics = game_controller_statics();
    if (!statics) why = WHY_NO_STATICS;
    return statics;
}

// UI-стик: статики -> ScreenControlsManager -> ScreenControlsReferenceHolder
// -> GameObject'ы FixedJoystick/FloatingJoystick -> компонент стика.
Target resolve_ui_stick() {
    Target t;
    int why = WHY_OK;
    const uint64_t statics = statics_or_why(why);
    if (!statics) { t.why = why; return t; }

    uint64_t holder = 0;
    const uint64_t manager = rd_ptr(statics + GAME_CONTROLLER_SCREEN_CONTROLS_FIELD);
    if (manager && object_class_name_is(manager, "ScreenControlsManager", true))
        holder = rd_ptr(manager + SCREEN_CONTROLS_HOLDER_FIELD);
    if (!holder || !object_class_name_is(holder, "ScreenControlsReferenceHolder", true)) {
        // Запасной путь: тот же holder держит сам хендлер ввода (+0x28).
        const uint64_t handler = rd_ptr(statics + GAME_CONTROLLER_INPUT_HANDLER_FIELD);
        if (handler && object_class_name_is(handler, "PlayerInputHandler", true))
            holder = rd_ptr(handler + SCREEN_CONTROLS_HOLDER_FIELD);
    }
    if (!holder || !object_class_name_is(holder, "ScreenControlsReferenceHolder", true)) {
        t.why = WHY_NO_CHANNEL;
        return t;
    }

    const uint64_t fields[2] = {SCREEN_CONTROLS_HOLDER_FIXED, SCREEN_CONTROLS_HOLDER_FLOATING};
    for (int i = 0; i < 2; ++i) {
        const uint64_t managed = rd_ptr(holder + fields[i]);
        if (managed < 0x10000) continue;
        const uint64_t go = managed_object_native(managed);
        if (go < 0x10000) continue;
        const uint64_t component = stick_component_of(go);
        if (!component) continue;
        t.sticks |= (1u << i);
        if (!t.addr) {
            t.addr = component + JOYSTICK_INPUT_VALUE;
            t.owner = component;
            t.klass = rd_ptr(component);
            t.back_off = COMPONENT_GAMEOBJECT;   // компонент знает свой GameObject
            t.back_eq = 0;                       // 0 — «ссылка непустая»
        } else if (!t.addr2) {
            t.addr2 = component + JOYSTICK_INPUT_VALUE;
        }
    }
    if (!t.addr) { t.why = WHY_NO_CHANNEL; return t; }
    t.ok = true;
    return t;
}

// Хендлер ввода: общая проверка для трёх кандидатов (оба Nullable и PlayerInput).
bool resolve_handler(uint64_t& handler, uint64_t& klass, uint64_t& owner, int& why) {
    handler = klass = owner = 0;
    const uint64_t statics = statics_or_why(why);
    if (!statics) return false;
    const uint64_t found = rd_ptr(statics + GAME_CONTROLLER_INPUT_HANDLER_FIELD);
    if (!found || !object_class_name_is(found, "PlayerInputHandler", true)) {
        why = WHY_NO_HANDLER;
        return false;
    }
    const uint64_t player = local_player_checked();
    const uint64_t backref = rd_ptr(found + PLAYER_INPUT_HANDLER_OWNER_BACKREF);
    if (backref && player && backref != player) { why = WHY_HANDLER_OWNER; return false; }
    handler = found;
    klass = rd_ptr(found);
    owner = backref;
    return true;
}

Target resolve_handler_field(int candidate) {
    Target t;
    int why = WHY_OK;
    uint64_t handler = 0, klass = 0, owner = 0;
    if (!resolve_handler(handler, klass, owner, why)) { t.why = why; return t; }
    const bool swapped = (candidate == C_OVR_A_S || candidate == C_OVR_B_S);
    const bool second  = (candidate == C_OVR_B || candidate == C_OVR_B_S);
    t.ok = true;
    t.owner = handler;
    t.klass = klass;
    if (owner) { t.back_off = PLAYER_INPUT_HANDLER_OWNER_BACKREF; t.back_eq = owner; }
    if (candidate == C_INPUT) {
        t.addr = handler + PLAYER_INPUT_MOVEMENT_INPUT;
    } else {
        t.addr = handler + (second ? PLAYER_INPUT_HANDLER_OVERRIDE_B
                                   : PLAYER_INPUT_HANDLER_OVERRIDE_A);
        t.nul = swapped ? NUL_V0 : NUL_HV0;
    }
    return t;
}

// Канал движения в PlayerEventHandler (Vsh) локального игрока.
Target resolve_channel() {
    Target t;
    const uint64_t player = local_player_checked();
    if (!player) { t.why = WHY_NO_PLAYER; return t; }
    const uint64_t handler = local_event_handler_checked();   // с проверкой владельца
    if (!handler) { t.why = WHY_NO_EVENT; return t; }
    const uint64_t channel = rd_ptr(handler + EVENT_HANDLER_MOVEMENT_INPUT);
    if (channel < 0x10000) { t.why = WHY_NO_CHANNEL; return t; }
    float value[2] = {};
    if (!rd_buf(channel + SYNC_VALUE_OFFSET, value, sizeof(value)) || !vec_ok(value)) {
        t.why = WHY_NO_CHANNEL;
        return t;
    }
    // Раскладку сверяем по соседнему LookDirection: он у нас уже читается как
    // нормированная ось выстрела (esp_mem_aim_read_fire_dir), значит по нему
    // видно, что это тот самый объект и те самые смещения.
    const uint64_t look = rd_ptr(handler + EVENT_HANDLER_LOOK_DIRECTION);
    if (look < 0x10000) { t.why = WHY_NO_CHANNEL; return t; }
    const Vec3 dir = rd_v3(look + SYNC_VALUE_OFFSET);
    const float len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (!std::isfinite(len) || !(len > 0.5f && len < 2.0f)) { t.why = WHY_NO_CHANNEL; return t; }
    t.ok = true;
    t.addr = channel + SYNC_VALUE_OFFSET;
    t.owner = handler;
    t.klass = rd_ptr(handler);
    t.back_off = EVENT_HANDLER_MANAGER_BACKREF;
    t.back_eq = player;
    return t;
}

Target resolve(int candidate) {
    switch (candidate) {
        case C_UI_STICK: return resolve_ui_stick();
        case C_OVR_A:
        case C_OVR_B:
        case C_INPUT:
        case C_OVR_A_S:
        case C_OVR_B_S:  return resolve_handler_field(candidate);
        case C_CHANNEL:  return resolve_channel();
        default: {
            Target t;
            t.why = WHY_NO_CHANNEL;
            return t;
        }
    }
}

// --- бег -------------------------------------------------------------------
// Персонаж бежит, только когда игра считает кнопку бега нажатой: оценщик
// спринта хендлера (glv, RVA 0x67E3574, зовётся из glI на 0x67E47D4) каждый
// кадр переписывает PlayerInput.Sprint (хендлер+0x59) как «стойка стика &&
// кнопка_бега», где кнопка — InputAction (LD+0x48). Отклонение стика бег НЕ
// включает: оно лишь масштабирует скорость (и при |x| >= 0.9 или y <= -0.1
// игра бег ещё и запрещает). Поэтому состояние кнопки ведём в памяти: пишем
// фазу акции в TriggerState (InputActionPhase.Started), дальше игра сама
// ставит PlayerInput.Sprint, а контроллер персонажа (KCC, RVA 0x7B95830)
// зеркалит его в PlayerEventHandler.Run (Vsh+0x280). Если фазу игра не
// подхватила за kSprintMissFrames — пишем и сам байт PlayerInput.Sprint, тот
// же, что пишет оценщик; это страховка от сброса/переезда акции, включается
// не более одного раза за ходьбу.
constexpr int   kSprintMissFrames = 30;     // ~0.5 с
constexpr float kSprintGateX      = 0.9f;   // порог glv: |x| >= 0.9 — бег запрещён
constexpr float kSprintGateY      = -0.1f;  // порог glv: y <= -0.1 — бег запрещён

// Цепочка кнопки бега: хендлер -> LD -> InputAction -> TriggerState. Раскладка
// Input System не наша, поэтому перед КАЖДОЙ записью сверяем имена классов,
// диапазон индекса, ненулевые указатели и фазу в пределах enum (см. sprint_alive).
struct SprintTarget {
    bool     ok = false;
    int      why = WHY_NO_SPRINT;
    uint64_t handler = 0;
    uint64_t klass = 0;
    uint64_t ld = 0;         // обёртка нового Input System у хендлера
    uint64_t action = 0;     // InputAction кнопки бега (то, что читает glv)
    uint64_t phase = 0;      // байт фазы в TriggerState — куда пишем
};

bool resolve_sprint(SprintTarget& t) {
    t = SprintTarget();
    uint64_t handler = 0, klass = 0, owner = 0;
    int why = WHY_OK;
    if (!resolve_handler(handler, klass, owner, why)) { t.why = why; return false; }
    const uint64_t ld = rd_ptr(handler + PLAYER_INPUT_HANDLER_INPUT_ACTIONS);
    if (ld < 0x10000 || !object_class_name_is(ld, "LD", false)) return false;
    const uint64_t action = rd_ptr(ld + LD_SPRINT_ACTION);
    if (action < 0x10000 || !object_class_name_is(action, "InputAction", false)) return false;
    const int index = (int)rd<std::int32_t>(action + INPUT_ACTION_INDEX);
    if (index < 0 || index > 0x4000) return false;
    const uint64_t holder = rd_ptr(action + INPUT_ACTION_STATE_HOLDER);
    const uint64_t state = holder >= 0x10000 ? rd_ptr(holder + INPUT_ACTION_STATE_HOLDER_STATE) : 0;
    const uint64_t states = state >= 0x10000 ? rd_ptr(state + INPUT_ACTION_STATE_STATES) : 0;
    if (states < 0x10000) return false;
    const uint64_t phase = states + (uint64_t)index * INPUT_ACTION_STATE_STRIDE;
    if (rd<unsigned char>(phase) > 4) return false;   // фаза вне InputActionPhase
    t.ok = true;
    t.why = WHY_OK;
    t.handler = handler;
    t.klass = klass;
    t.ld = ld;
    t.action = action;
    t.phase = phase;
    return true;
}

// --- состояние ------------------------------------------------------------

Target   s_targets[C_COUNT];
TrialState s_trial(C_COUNT);      // выбор слоя: проба -> подтверждение/ротация
float    s_age = kResolveT;       // возраст резолва, с
float    s_dt = 1.0f / 60.0f;
bool     s_touched[C_COUNT] = {}; // в этот кандидат уже писали (есть что обнулять)
float    s_x = 0.0f, s_y = 0.0f;  // последний записанный вектор
float    s_prev_dist = 0.0f;      // дистанция в прошлом кадре (для скорости)
bool     s_prev_ok = false;
float    s_ack = 0.0f;            // измеренная скорость сближения, м/с
int      s_writes = 0;            // удачных записей за сессию
int      s_why = WHY_OK;
// Лог why-кодов: только на СМЕНЕ кода. Кнопки атаки/бега пишутся раз в кадр,
// один устойчивый сбой не должен затопить бензваровский лог.
static int s_attack_why_log = WHY_OK;
static int s_sprint_why_log = WHY_OK;
static void joyWhyLog(const char* what, int why, int& last) {
    if (last == why) return;
    last = why;
    benz_log("JOY %s: why=%d (%s)", what, why, why_text(why));
}
unsigned s_sticks = 0;            // какие стики найдены (диагностика)

void revalidate(int candidate) {
    s_targets[candidate] = resolve(candidate);
    s_trial.set_available(candidate, alive(s_targets[candidate]));
}

// Состояние кнопки бега. base_* — что было в памяти до нашей первой записи,
// чтобы отпускание вернуло игру в исходное состояние, а не в «ноль».
SprintTarget  s_sprint_chain;
unsigned char s_sprint_base_phase = 0;
unsigned char s_sprint_base_sprint = 0;
bool          s_sprint_base_known = false;
bool          s_sprint_on = false;       // бег ведём мы (есть что возвращать)
bool          s_sprint_direct = false;   // запасной путь: ведём сам байт Sprint
int           s_sprint_miss = 0;
int           s_sprint_why = WHY_OK;

bool sprint_alive() {
    const SprintTarget& t = s_sprint_chain;
    if (!t.ok || !t.phase) return false;
    if (rd_ptr(t.handler) != t.klass) return false;   // хендлер освобождён/переиспользован
    const uint64_t ld = rd_ptr(t.handler + PLAYER_INPUT_HANDLER_INPUT_ACTIONS);
    if (ld != t.ld || ld < 0x10000) return false;
    if (rd_ptr(ld + LD_SPRINT_ACTION) != t.action) return false;
    const uint64_t holder = rd_ptr(t.action + INPUT_ACTION_STATE_HOLDER);
    const uint64_t state = holder >= 0x10000 ? rd_ptr(holder + INPUT_ACTION_STATE_HOLDER_STATE) : 0;
    const uint64_t states = state >= 0x10000 ? rd_ptr(state + INPUT_ACTION_STATE_STATES) : 0;
    const int index = (int)rd<std::int32_t>(t.action + INPUT_ACTION_INDEX);
    if (states < 0x10000 || index < 0 || index > 0x4000) return false;
    if (states + (uint64_t)index * INPUT_ACTION_STATE_STRIDE != t.phase) return false;
    return rd<unsigned char>(t.phase) <= 4;
}

// Раз в кадр: подхватила ли игра нашу фазу. Признак — PlayerInput.Sprint,
// который оценщик спринта переписывает сам. Ноль там законен, когда стойка
// стика бег запрещает (|x| >= 0.9 или y <= -0.1 — пороги из glv), поэтому
// «промахом» считаем только кадры с разрешающей стойкой.
void sprint_check() {
    if (!s_sprint_on || s_sprint_direct) { s_sprint_miss = 0; return; }
    if (!(fabsf(s_x) < kSprintGateX && s_y > kSprintGateY)) { s_sprint_miss = 0; return; }
    if (!sprint_alive()) { s_sprint_miss = 0; return; }
    const uint64_t press = s_sprint_chain.handler + PLAYER_INPUT_SPRINT;
    if (rd<unsigned char>(press) != 0) { s_sprint_miss = 0; return; }
    if (++s_sprint_miss < kSprintMissFrames) return;
    s_sprint_miss = 0;
    s_sprint_direct = true;   // фазу игра не подхватывает — ведём сам байт бега
}

} // namespace

void tick(float dt) {
    if (!std::isfinite(dt) || dt <= 0.0f) dt = 1.0f / 60.0f;
    if (dt > 0.25f) dt = 0.25f;
    s_dt = dt;
    s_age += dt;

    const int cur = s_trial.current();
    if (s_age >= kResolveT || !alive(s_targets[cur])) {
        s_age = 0.0f;
        for (int c = 0; c < C_COUNT; ++c) revalidate(c);
        s_sticks = s_targets[C_UI_STICK].sticks;
    }

    sprint_check();   // бег: подхватила ли игра нашу фазу кнопки (см. ниже)
}

// Слоты ввода (Nullable<Vector2> хендлера) могут оказаться и «взглядом»:
// пока кандидат не подтверждён, ведём их мягче. Для проверки по сближению
// этого хватает с запасом (нужно около 0.27 м/с, а ходьба даёт метры в
// секунду), зато камеру, если слот окажется взглядом, тянет в разы слабее.
bool probe_slot(int candidate) {
    return candidate == C_OVR_A || candidate == C_OVR_B ||
           candidate == C_OVR_A_S || candidate == C_OVR_B_S;
}

bool command(float x, float y) {
    if (!std::isfinite(x) || !std::isfinite(y)) { release(); return false; }
    const int cur = s_trial.current();
    const float limit = (!s_trial.confirmed() && probe_slot(cur)) ? kProbeMax : kStickMax;
    const float len = sqrtf(x * x + y * y);
    if (len > limit && len > 0.0f) {
        const float k = limit / len;
        x *= k;
        y *= k;
    }
    if (!std::isfinite(x) || !std::isfinite(y)) { release(); return false; }
    s_x = x;
    s_y = y;
    s_trial.set_armed(true, len >= 0.5f);

    if (!alive(s_targets[cur])) revalidate(cur);
    if (!alive(s_targets[cur])) {
        s_why = s_targets[cur].why;
        return false;
    }
    if (!write_target(s_targets[cur], x, y)) {
        s_why = WHY_WRITE;
        return false;
    }
    s_why = WHY_OK;
    s_touched[cur] = true;
    ++s_writes;
    return true;
}

bool attack(bool on) {
    uint64_t handler = 0, klass = 0, owner = 0;
    int why = WHY_OK;
    if (!resolve_handler(handler, klass, owner, why)) {
        s_attack_why = why;
        joyWhyLog("атака", s_attack_why, s_attack_why_log);
        return false;                 // класть 1 было некуда — и не надо
    }
    const unsigned char value = on ? 1u : 0u;
    if (!wr_buf(handler + PLAYER_INPUT_HANDLER_ATTACK_HELD, &value, sizeof(value))) {
        s_attack_why = WHY_WRITE;
        joyWhyLog("атака", s_attack_why, s_attack_why_log);
        return false;                 // состояние не меняем: попробуем ещё
    }
    s_attack_why = WHY_OK;
    joyWhyLog("атака", s_attack_why, s_attack_why_log);
    s_attack_value = value;
    return true;
}

bool sprint(bool on) {
    if (!on && !s_sprint_on) return true;          // возвращать нечего
    if (!sprint_alive()) {
        if (!resolve_sprint(s_sprint_chain)) {
            s_sprint_why = s_sprint_chain.why;
            joyWhyLog("бег", s_sprint_why, s_sprint_why_log);
            return false;
        }
        s_sprint_base_known = false;               // объект мог смениться
    }
    const uint64_t phase_addr = s_sprint_chain.phase;
    const uint64_t press_addr = s_sprint_chain.handler + PLAYER_INPUT_SPRINT;
    if (!s_sprint_base_known) {
        s_sprint_base_phase  = rd<unsigned char>(phase_addr);
        s_sprint_base_sprint = rd<unsigned char>(press_addr);
        s_sprint_base_known  = true;
    }
    const unsigned char phase = on ? INPUT_ACTION_PHASE_STARTED : s_sprint_base_phase;
    if (!wr_buf(phase_addr, &phase, sizeof(phase))) {
        s_sprint_why = WHY_WRITE;
        joyWhyLog("бег", s_sprint_why, s_sprint_why_log);
        return false;
    }
    if (s_sprint_direct) {
        const unsigned char press = on ? 1u : s_sprint_base_sprint;
        if (!wr_buf(press_addr, &press, sizeof(press))) {
            s_sprint_why = WHY_WRITE;
            joyWhyLog("бег", s_sprint_why, s_sprint_why_log);
            return false;
        }
        if (!on) s_sprint_direct = false;
    }
    s_sprint_on = on;
    if (!on) s_sprint_miss = 0;
    s_sprint_why = WHY_OK;
    joyWhyLog("бег", s_sprint_why, s_sprint_why_log);
    return true;
}

void release() {
    s_trial.release();
    s_x = 0.0f;
    s_y = 0.0f;
    // Зажатие атаки не оставляем игре: пока байт 1, персонаж машет сам.
    if (s_attack_value) attack(false);
    // И кнопку бега возвращаем в то состояние, что было до нас.
    if (s_sprint_on) sprint(false);
    for (int c = 0; c < C_COUNT; ++c) {
        if (!s_touched[c]) continue;
        zero_target(s_targets[c]);
        s_touched[c] = false;
    }
}

void observe(float walk_dist, bool clean) {
    // Скорость сближения — только для строки статуса: по «чистым» кадрам.
    const bool usable = clean && std::isfinite(walk_dist) && walk_dist > 0.0f;
    if (!usable) {
        s_prev_ok = false;
    } else {
        if (s_prev_ok && s_dt > 0.0f) {
            const float rate = (s_prev_dist - walk_dist) / s_dt;   // >0 — сближаемся
            if (std::isfinite(rate) && rate > -30.0f && rate < 30.0f)
                s_ack += (rate - s_ack) * kAckAlpha;
        }
        s_prev_dist = walk_dist;
        s_prev_ok = true;
    }

    // Решение по пробе — в ядре (farm_joy_core.h): здесь только его последствия.
    const int from = s_trial.current();
    const TrialState::Decision dec = s_trial.observe(s_dt, walk_dist, clean);
    if (dec == TrialState::CONFIRMED) {
        // Подтверждение — событие однократное (confirmed_ дальше не сбрасывается,
        // пока не release): один лог на застёгнутый слой стика.
        benz_log("JOY стик: кандидат %d (%s) ПОДТВЕРЖДЁН trials=%d сближение=%.2f м/с",
                 from, candidate_name(from), s_trial.trials(), s_ack);
        return;
    }
    if (dec != TrialState::NEXT) return;
    if (!s_touched[from]) return;
    zero_target(s_targets[from]);   // слой не подтвердился: стик не оставляем нажатым
    s_touched[from] = false;
    s_ack = 0.0f;
    benz_log("JOY стик: кандидат %d (%s) отклонён fails=%d why=%d (%s)",
             from, candidate_name(from), s_trial.fails(from),
             s_targets[from].why, why_text(s_targets[from].why));
}

Status status() {
    Status out;
    const int cur = s_trial.current();
    out.candidate = cur;
    out.confirmed = s_trial.confirmed();
    out.armed = s_trial.armed();
    out.x = s_x;
    out.y = s_y;
    out.closing_mps = s_ack;
    out.writes = s_writes;
    out.trials = s_trial.trials();
    for (int c = 0; c < C_COUNT; ++c) out.fails[c] = s_trial.fails(c);
    out.why = s_why != WHY_OK ? s_why : s_targets[cur].why;
    out.sticks = s_sticks;
    out.attacking = s_attack_value != 0;
    out.attack_why = s_attack_why;
    out.sprinting = s_sprint_on;
    out.sprint_why = s_sprint_why;
    return out;
}

const char* candidate_name(int candidate) {
    switch (candidate) {
        case C_UI_STICK: return "стик UI";
        case C_OVR_A:    return "слот A";
        case C_OVR_B:    return "слот B";
        case C_CHANNEL:  return "канал";
        case C_INPUT:    return "ввод игрока";
        case C_OVR_A_S:  return "слот A*";
        case C_OVR_B_S:  return "слот B*";
        default:         return "?";
    }
}

const char* why_text(int why) {
    switch (why) {
        case WHY_OK:            return "ок";
        case WHY_NO_STATICS:    return "нет статиков";
        case WHY_NO_HANDLER:    return "нет хендлера";
        case WHY_HANDLER_OWNER: return "чужой игрок";
        case WHY_NO_PLAYER:     return "нет игрока";
        case WHY_NO_EVENT:      return "нет обработчика";
        case WHY_NO_CHANNEL:    return "нет стика";
        case WHY_WRITE:         return "нет записи";
        case WHY_NO_SPRINT:     return "нет кнопки бега";
        default:                return "?";
    }
}

} // namespace farmjoy
