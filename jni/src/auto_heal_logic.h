#pragma once
// Автохил: РЕШЕНИЕ без ввода и памяти — состояние, таймеры, кому и когда жать.
// Здесь нет ни uinput, ни чтения игры: всё приходит в Inputs, наружу уходят
// только уровни «палец на слоте» / «палец на кнопке использования». Поэтому
// машина целиком проверяется на хосте (jni/tests/test_auto_heal.cpp), а
// jni/src/auto_heal.cpp остаётся тонким драйвером: прочитал HP, отдал пальцы.
//
// Почему «в бою» — не то же самое, что «HP ниже порога». Аптечка тратится
// один раз за бой, а окно урона кончается через несколько секунд: за это
// время надо успеть вылечиться, иначе следующий удар убивает. Поэтому
// главный триггер — СВЕЖИЙ урон (окно combatSec), а порог HP лишь запрещает
// тратить аптечку на царапину. Оба выключателя независимы: combatOnly=false
// лечит по одному порогу (для стелса/строительства вне боя).
//
// Человекоподобность: перед первой попыткой — задержка реакции (reactMs), сам
// тап слота короткий, между тапом и удержанием — пауза на переключение слота
// (selectWaitMs), удержание — holdMs, и только потом оценка результата. Ни одна
// попытка не начинается по одному и тому же кадру: серия перевзводится только
// когда условие ушло (вылечились / бой кончился) или пришёл НОВЫЙ урон.
//
// Оценка «подействовало»: здоровье выросло минимум на successHpRise ИЛИ ушло
// выше порога. Не подействовало — повтор, и так до maxTries; исчерпали —
// backoffSec тишины и ожидание нового урона (без этого битый слот/отсутствие
// аптечки превращались бы в вечное дёрганье кнопок, которое видно и людям, и
// серверу).

namespace autoheal {

// Фазы. IDLE — ждём условие; REACT — задержка реакции; TAP_SLOT — палец на
// слоте аптечки; SELECT_WAIT — пауза на переключение; HOLD_USE — держим кнопку
// использования; COOLDOWN — пауза между попытками; BACKOFF — серия не
// подействовала, ждём нового урона.
enum Stage {
    STAGE_IDLE = 0,
    STAGE_REACT,
    STAGE_TAP_SLOT,
    STAGE_SELECT_WAIT,
    STAGE_HOLD_USE,
    STAGE_COOLDOWN,
    STAGE_BACKOFF,
};

// Почему простаиваем (для строки статуса в окне автохила).
enum Reason {
    REASON_OK = 0,      // не простаиваем
    REASON_OFF,         // выключено
    REASON_NO_GAME,     // игра не привязана / локальный игрок неизвестен
    REASON_NO_INJECT,   // синтетические тачи недоступны (readOnly)
    REASON_NO_DATA,     // здоровье не читается
    REASON_DEAD,        // локальный игрок мёртв
    REASON_HEALTHY,     // здоровье выше порога
    REASON_NO_COMBAT,   // урон старше окна боя
    REASON_REARM,       // серия закрыта: ждём новый урон
    REASON_BACKOFF,     // серия не подействовала, идёт пауза
    REASON_NOT_SET,     // не заданы зоны (слот аптечки / кнопка использования)
};

// Признак боя. Два независимых источника:
//   1. Падение здоровья между кадрами — работает всегда, ничего не стоит и не
//      зависит от семантики чужих полей.
//   2. Изменение m_LastDamageTime (PlayerVitals+0x158) — это ровно тот момент,
//      который зарегистрировала игра, включая урон, не изменивший HP (щит,
//      хитбокс), но семантика поля неизвестна: если это НЕ «время последнего
//      урона», а, скажем, счётчик «сколько прошло с урона», оно меняется каждый
//      кадр. Поэтому изменение метки принимается как урон только пока метка
//      выглядит СОБЫТИЕМ (меняется редко), а не потоком; в противном случае
//      источник отключается сам и остаётся только падение HP.
struct DamageTracker {
    double at = -1.0;            // моно-время последнего урона, -1 = не было
    float  prev_hp = -1.f;       // для обнаружения падения
    float  prev_stamp = -1.f;
    bool   have_stamp = false;
    bool   stamp_useful = true;  // метка ещё ведёт себя как событие
    int    stamp_changes = 0;    // изменения метки в текущем окне наблюдения
    int    window_frames = 0;    // кадров в окне

    // Порог падения: аптечка/реген поднимает HP плавно, а дробный шум
    // сотые доли не считаем уроном.
    static constexpr float kDropStep = 0.5f;
    // Окно наблюдения за меткой, кадры (~2 с при 60 Гц).
    static constexpr int kWindow = 120;

    void reset() {
        at = -1.0;
        prev_hp = -1.f;
        prev_stamp = -1.f;
        have_stamp = false;
        stamp_useful = true;
        stamp_changes = 0;
        window_frames = 0;
    }

    void observe(bool hp_ok, float hp, bool stamp_ok, float stamp, double now) {
        if (!hp_ok) {
            // Потеря чтения (перепривязка, загрузка мира): прошлое значение не
            // годится для сравнения — иначе первый же честный HP после паузы
            // выглядел бы уроном.
            prev_hp = -1.f;
        } else {
            if (prev_hp >= 0.f && hp < prev_hp - kDropStep) at = now;
            prev_hp = hp;
        }
        if (!stamp_ok) {
            have_stamp = false;
            return;
        }
        if (have_stamp) {
            const float d = stamp - prev_stamp;
            ++window_frames;
            if (d > 1e-4f) {
                ++stamp_changes;
                if (stamp_useful) at = now;
            } else if (d < -1e-4f) {
                // Метка поехала назад — она не «время урона»: источник гасим.
                stamp_useful = false;
            }
            if (window_frames >= kWindow) {
                // Больше половины кадров с изменением — это поток, а не события.
                if (stamp_changes * 2 >= window_frames) stamp_useful = false;
                stamp_changes = 0;
                window_frames = 0;
            }
        }
        prev_stamp = stamp;
        have_stamp = true;
    }
};

struct Settings {
    float hpPct = 60.f;        // лечиться, когда здоровье <= этого процента
    float combatSec = 6.f;     // урон не старше этого — «в бою»
    float reactMs = 180.f;     // задержка реакции перед первой попыткой
    float tapMs = 70.f;        // сколько палец лежит на слоте
    float selectWaitMs = 220.f;// пауза после тапа: игра переключает слот
    float holdMs = 2000.f;     // сколько держим кнопку использования
    float cooldownSec = 1.6f;  // пауза между попытками
    float backoffSec = 12.f;   // тишина после исчерпанной серии
    int   maxTries = 3;        // попыток на одну серию
    bool  combatOnly = true;   // лечить только когда урон свежий
    float successHpRise = 1.0f;// рост HP, считающийся успехом
};

struct Inputs {
    float  dt = 1.f / 60.f;
    bool   enabled = false;      // тумблер «Автохил»
    bool   gameReady = false;    // привязка жива и локальный игрок разрешён
    bool   inject = false;       // синтетические тачи реально уходят в игру
    float  health = -1.f;        // текущее HP, -1 = не прочитано
    float  healthMax = -1.f;     // максимум HP, <= 0 = неизвестен
    double damageAt = -1.0;      // моно-время последнего замеченного урона
    double now = 0.0;            // моно-время кадра, с
};

// Уровни пальцев + события кадра (события — только на переходах, для журнала
// и статуса; решение от них не зависит).
struct Outputs {
    bool slotDown = false;       // палец на слоте аптечки
    bool useDown = false;        // палец на кнопке использования
    bool attemptStarted = false; // начали попытку (палец пошёл на слот)
    bool attemptFinished = false;// попытка закончилась (удержание отпущено)
    bool success = false;        // ...и она подействовала
    bool gaveUp = false;         // ...и серия исчерпана, ушли в backoff
};

struct Machine {
    int    stage = STAGE_IDLE;
    int    reason = REASON_OFF;
    float  timer = 0.f;          // остаток текущей фазы, с
    int    tries = 0;            // попыток в текущей серии
    bool   armed = true;         // можно начинать серию
    bool   needFreshDamage = false; // после backoff — только на новый урон
    double freshDamageRef = -1.0;   // какой damageAt уже «израсходован»
    float  hpAtAttempt = -1.f;   // здоровье на момент начала удержания
    float  hpPct = -1.f;         // последний посчитанный процент (для статуса)
    int    doneAttempts = 0;     // сколько попыток всего сделано (статус)
    int    successes = 0;        // сколько раз подействовало (статус)
    int    aborts = 0;           // сколько попыток прервано (статус)

    // Полный сброс — как при выключении фичи.
    void reset() {
        stage = STAGE_IDLE;
        reason = REASON_OFF;
        timer = 0.f;
        tries = 0;
        armed = true;
        needFreshDamage = false;
        freshDamageRef = -1.0;
        hpAtAttempt = -1.f;
        hpPct = -1.f;
    }

    void tick(const Inputs& in, const Settings& s, Outputs& out) {
        out = Outputs{};
        float dt = in.dt;
        if (!(dt > 0.f) || dt > 0.25f) dt = 1.f / 60.f;

        const bool hpKnown = in.health >= 0.f;
        const float pct = (hpKnown && in.healthMax > 1.f)
            ? (in.health / in.healthMax) * 100.f
            : -1.f;
        hpPct = pct;
        const bool dead = pct >= 0.f && in.health <= 0.5f;
        const bool below = pct >= 0.f && !dead && pct <= s.hpPct;
        const bool inCombat = !s.combatOnly ||
            (in.damageAt >= 0.0 && (in.now - in.damageAt) <= (double)s.combatSec &&
             in.now >= in.damageAt);
        const bool trigger = below && inCombat;
        // Новый урон относительно израсходованного: по нему перевзводится серия
        // после backoff (само поле может быть недоступно — тогда работает
        // только «условие ушло», см. ниже).
        const bool newDamage = in.damageAt >= 0.0 &&
            (freshDamageRef < 0.0 || in.damageAt > freshDamageRef + 1e-4);

        // --- Жёсткие остановки: отпускаем палец немедленно ---------------------
        const bool holding = stage == STAGE_REACT || stage == STAGE_TAP_SLOT ||
                             stage == STAGE_SELECT_WAIT || stage == STAGE_HOLD_USE;
        const bool hardStop = !in.enabled || !in.gameReady || !in.inject ||
                              !hpKnown || dead;
        if (hardStop) {
            if (holding) {
                out.attemptFinished = true;
                ++aborts;
            }
            if (!in.enabled)                          reason = REASON_OFF;
            else if (!in.gameReady)                   reason = REASON_NO_GAME;
            else if (!in.inject)                      reason = REASON_NO_INJECT;
            else if (!hpKnown)                        reason = REASON_NO_DATA;
            else                                      reason = REASON_DEAD;
            stage = STAGE_IDLE;
            timer = 0.f;
            tries = 0;
            armed = true;
            needFreshDamage = false;
            return;
        }

        // --- Перевзвод серии ---------------------------------------------------
        // Условие ушло (вылечились или бой кончился) — серия закрыта, следующая
        // начинается заново. После исчерпанной серии то же условие ушло, но
        // начинать заново можно ТОЛЬКО на новом уроне: иначе одна и та же рана
        // крутила бы попытки до бесконечности.
        if (!armed) {
            if (needFreshDamage) {
                if (newDamage || !trigger) {
                    armed = true;
                    needFreshDamage = false;
                    tries = 0;
                }
            } else if (!trigger) {
                armed = true;
                tries = 0;
            }
        }

        switch (stage) {
            case STAGE_IDLE: {
                if (!trigger) {
                    reason = below ? REASON_NO_COMBAT : REASON_HEALTHY;
                    return;
                }
                if (!armed) {
                    reason = needFreshDamage ? REASON_REARM : REASON_BACKOFF;
                    return;
                }
                if (tries >= s.maxTries) {   // страховка: сюда попадать не должны
                    stage = STAGE_BACKOFF;
                    timer = s.backoffSec;
                    reason = REASON_BACKOFF;
                    return;
                }
                armed = false;
                stage = STAGE_REACT;
                timer = s.reactMs * 0.001f;
                reason = REASON_OK;
                return;
            }

            case STAGE_REACT: {
                if (!trigger) {              // вылечился сам, пока думали
                    reason = below ? REASON_NO_COMBAT : REASON_HEALTHY;
                    stage = STAGE_IDLE;
                    out.attemptFinished = true;
                    return;
                }
                timer -= dt;
                if (timer > 0.f) return;
                stage = STAGE_TAP_SLOT;
                timer = s.tapMs * 0.001f;
                out.slotDown = true;
                out.attemptStarted = true;   // палец пошёл на слот — попытка начата
                return;
            }

            case STAGE_TAP_SLOT: {
                timer -= dt;
                if (timer > 0.f) { out.slotDown = true; return; }
                // Палец отпущен, слот переключается.
                stage = STAGE_SELECT_WAIT;
                timer = s.selectWaitMs * 0.001f;
                return;
            }

            case STAGE_SELECT_WAIT: {
                timer -= dt;
                if (timer > 0.f) return;
                stage = STAGE_HOLD_USE;
                timer = s.holdMs * 0.001f;
                hpAtAttempt = hpKnown ? in.health : -1.f;
                ++doneAttempts;
                out.useDown = true;
                return;
            }

            case STAGE_HOLD_USE: {
                // Игра поднимает HP в конце применения: если здоровье уже ушло
                // выше порога — держать больше нечего, отпускаем и закрываем
                // попытку как успешную.
                if (pct > s.hpPct) {
                    out.attemptFinished = true;
                    out.success = true;
                    ++successes;
                    tries = 0;
                    stage = STAGE_IDLE;
                    armed = false;       // перевзвод — когда условие уйдёт
                    reason = REASON_HEALTHY;
                    return;
                }
                timer -= dt;
                if (timer > 0.f) { out.useDown = true; return; }
                stage = STAGE_COOLDOWN;
                timer = s.cooldownSec;
                ++tries;
                out.attemptFinished = true;
                return;
            }

            case STAGE_COOLDOWN: {
                timer -= dt;
                if (timer > 0.f) return;
                const bool applied = hpKnown && (
                    (pct > s.hpPct) ||
                    (hpAtAttempt >= 0.f && in.health >= hpAtAttempt + s.successHpRise));
                if (applied) {
                    out.success = true;
                    ++successes;
                    tries = 0;
                    stage = STAGE_IDLE;
                    armed = false;
                    reason = REASON_HEALTHY;
                    return;
                }
                if (tries >= s.maxTries) {
                    stage = STAGE_BACKOFF;
                    timer = s.backoffSec;
                    reason = REASON_BACKOFF;
                    // Новый урон — единственный вход обратно. Израсходованным
                    // считаем тот, что привёл к этой серии.
                    needFreshDamage = true;
                    freshDamageRef = in.damageAt;
                    out.gaveUp = true;
                    return;
                }
                if (!trigger) {          // рана закрылась (забинтовали/отступили)
                    stage = STAGE_IDLE;
                    tries = 0;
                    armed = false;
                    reason = below ? REASON_NO_COMBAT : REASON_HEALTHY;
                    return;
                }
                stage = STAGE_REACT;     // повтор: та же задержка реакции
                timer = s.reactMs * 0.001f;
                return;
            }

            case STAGE_BACKOFF: {
                timer -= dt;
                if (timer > 0.f) return;
                stage = STAGE_IDLE;
                reason = needFreshDamage ? REASON_REARM : REASON_HEALTHY;
                return;
            }
        }
    }
};

}  // namespace autoheal
