#include "farm.h"
#include "cfg.h"
#include "main.h"
#include "game.h"
#include "Android_touch/TouchHelperA.h"
#include "app_state.h"
#include "ui_util.h"
#include "esp_draw.h"            // FrameBoxes
#include "widgets.h"             // g_sheet, g_pop (пауза при открытом окне)
#include "menu.h"                // g_calibMode
#include "process.h"             // g_esp_attached
#include "logfile.h"             // LogLine (no-op, пока лог выключен)
#include <cmath>

// ============================== Magic-Farm ===================================
//
// Переработка автофарма. Прежний контроллер был тач-ботом: он сам крутил
// камеру свайпами, сам держал джойстик движения, подходил к дереву, обходил
// препятствия и только потом бил. Отсюда и весь его код — обучение
// коэффициента град/px, дебаунсы смены цели, манёвры обхода, watchdog'и
// застревания. Magic-Farm ничего этого не делает: ходит игрок, а чит работает
// только с памятью.
//
// Как это устроено (подробности — «Magic-Farm: ядро» в game.cpp):
//
//   Silent Melee. FPMelee.ZkX не бросает луч в момент удара — он берёт готовый
//   GKo из активностей PlayerEventHandler (RaycastData, при null AimRaycast) и
//   смотрит только на его поля: GameObject попадания, коллайдер, точку и
//   дистанцию. Мы держим в этой структуре выбранный узел, поэтому штатный удар
//   приходится в ресурс независимо от того, куда смотрит камера и достаёт ли
//   до узла настоящий луч. Урон, кулдаун и выдача ресурса при этом остаются
//   серверными: мы не выдумываем попадание, а переадресуем настоящее.
//
//   Удары по «крестикам». Точкой попадания всегда ставится живой маркер серии
//   (OreHitstreaks.MoW у руды, отрезок TreeHitstreaks.MTQ..MTu у дерева) —
//   ровно то, по чему игра меряет серию (радиус допуска 15 см). Целиться
//   руками больше не нужно вовсе: координаты крестика читаются из памяти, и
//   каждый удар ложится в его центр, пока крестик жив.
//
//   Melee TP. Дальность удара (FPMelee.m_MaxReach + hitRadius) и геометрия
//   луча взаимодействия (RaycastManager.m_RayLength / m_AimRayLength /
//   m_SphereCastRadius) переписываются на заданный радиус. Два следствия:
//   удар засчитывается по узлам в нескольких метрах вокруг игрока, а толстая
//   сфера луча сама приносят нам шаблоны попадания (GameObject + коллайдер)
//   тех узлов, мимо которых игрок просто прошёл.
//
// Единственный синтетический ввод — тап по кнопке атаки, и тот отключаемый:
// с выключенными «Авто-ударами» чит просто обслуживает удары самого игрока.

bool  g_farmActive = false;
int   g_farmPhase = 0;
bool  g_farmPaused = false;
int   g_farmNodes = 0;
int   g_farmScanned = 0;
int   g_farmReason = 1;
float g_farmTgtDist = 0.f;
float g_farmReach = 0.f;
float g_farmTpReach = 0.f;
bool  g_farmTpActive = false;
int   g_farmTgtKind = 0;
int   g_farmSpot = 0;
int   g_farmStreak = 0;
float g_farmHpPct = -1.f;
float g_farmSpotLife = -1.f;
float g_farmAttackPeriod = 0.f;
int   g_farmToolHave = 0;
int   g_farmToolNeed = 0;
int   g_farmXp = 0;
int   g_farmTemplates = 0;
int   g_farmIndexed = 0;
int   g_farmNodesWorld = 0;
int   g_farmLearn = 0;
int   g_farmHits = 0;
int   g_farmSwings = 0;
int   g_farmRayStage = 0;        // где обрывается цепочка рейкаста игры
int   g_farmRayBest = 0;         // лучшая стадия за сессию
int   g_farmPinHold = -1;        // % проверок, где наш рейкаст уцелел (-1 нет данных)

namespace {

// Сколько узлов разбирается за кадр. Больше десятка в радиусе удара не бывает
// даже в поле с бочками, а каждый узел стоит чтений памяти (крестик, здоровье,
// остаток).
constexpr int kMaxSwarm = 12;

// Ритм ударов, когда орудие не прочиталось. Настоящие значения всегда берутся
// из FPMelee.m_TimeBetweenAttacks + pauseAfterAttack: тап чаще кулдауна игра
// просто ставит в очередь, а потом выпускает пачкой.
constexpr float kTapDownMsDefault = 85.f;
constexpr float kTapUpMsDefault   = 230.f;

// Доля дальности удара, которую пишем в RaycastHit.distance. Игра сравнивает
// её с m_MaxReach + hitRadius строгим «меньше», поэтому запас обязателен:
// дальность меняется в тот же момент, когда орудие достают из рук.
constexpr float kDistanceFraction = 0.55f;
constexpr float kDistanceFallback = 0.45f;   // м, когда дальность не прочиталась

// Остаток, ниже которого узел считается выбранным. Дебаунса, как в старом
// контроллере, не нужно: мы не теряем на смене цели ни кадра — просто со
// следующего удара бьём по следующему узлу.
constexpr float kDepletedFrac = 0.03f;

// Радиус поиска целей, когда Melee TP выключен: бьём только то, до чего и так
// дотягивается орудие.
constexpr float kPlainRadiusPad = 0.35f;

// Сфера луча взаимодействия как доля радиуса TP. Радиус нужен не для урона (за
// него отвечает дальность), а чтобы собственный рейкаст игры цеплял узлы
// рядом с линией взгляда и приносил их шаблоны попадания.
// Слишком толстая сфера вредна: пущенная из глаз, она начинает цеплять стены,
// пол и собственный корпус, попадание возвращается с дистанцией около нуля —
// и ломается не только Magic-Farm, но и обычные удары с подсказками игрока.
constexpr float kSphereFraction = 0.15f;
constexpr float kSphereMin = 0.10f;
constexpr float kSphereMax = 0.50f;

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Цвет метки узла: зелёная — бьём в крестик, оранжевая — в корпус,
// приглушённая — шаблон попадания ещё не выучен (по такому узлу бить нечем,
// пока луч игры не упрётся в него хотя бы раз).
ImU32 node_colour(const FarmSwarmNode& node, bool target) {
    const ImVec4& base = node.at_spot ? cfg::esp::farm_spot_col : cfg::esp::farm_body_col;
    const float a = node.have_template ? (target ? 1.00f : 0.55f) : 0.30f;
    return IM_COL32((int)(base.x * 255), (int)(base.y * 255), (int)(base.z * 255),
                    (int)(235 * a));
}

void draw_node(const FarmSwarmNode& node, bool target) {
    if (!node.on_screen) return;
    auto* fg = ImGui::GetForegroundDrawList();
    const ImU32 col = node_colour(node, target);
    const float r = node.at_spot ? 13.f : 17.f;
    const float w = target ? 3.f : 1.8f;
    fg->AddCircle({node.sx, node.sy}, r, col, 24, w);
    if (target) {
        fg->AddLine({node.sx - r * 1.6f, node.sy}, {node.sx - r * 0.5f, node.sy}, col, w);
        fg->AddLine({node.sx + r * 0.5f, node.sy}, {node.sx + r * 1.6f, node.sy}, col, w);
        fg->AddLine({node.sx, node.sy - r * 1.6f}, {node.sx, node.sy - r * 0.5f}, col, w);
        fg->AddLine({node.sx, node.sy + r * 0.5f}, {node.sx, node.sy + r * 1.6f}, col, w);
    }
    if (node.at_spot) fg->AddCircleFilled({node.sx, node.sy}, 2.5f, col, 12);
}

// Оценка цели: в первую очередь крестик (попадание в него растит серию и даёт
// бонус), затем близость. Выбитые узлы уходят в конец и тут же в чёрный список.
float node_score(const FarmSwarmNode& node) {
    float score = node.dist;
    // Узел с выученным шаблоном предпочтительнее, но узел без шаблона целью
    // быть ТОЖЕ обязан: удар по нему идёт без подмены рейкаста (бьёт то, во
    // что упёрся удлинённый луч игры) и как раз этим шаблон и выучивается.
    // Пока эта проверка была жёстким «continue», на свежей местности цель не
    // находилась никогда и чит молча простаивал.
    if (!node.have_template) score += 1.5f;
    if (node.at_spot) score -= 2.0f;
    if (node.kind == 4) score -= 0.5f;   // бочка ломается в пару ударов
    return score;
}

void reset_status() {
    g_farmActive = false;
    g_farmPhase = 0;
    g_farmPaused = false;
    g_farmNodes = 0;
    g_farmTgtDist = 0.f;
    g_farmTgtKind = 0;
    g_farmSpot = 0;
    g_farmStreak = 0;
    g_farmHpPct = -1.f;
    g_farmSpotLife = -1.f;
    g_farmXp = 0;
}

} // namespace

void UpdateFarm(float dt) {
    static bool  s_tapDown = false;     // палец 2 держит кнопку атаки
    static int   s_tapTimer = 0;        // мс до следующего переключения пальца
    static bool  s_pinned = false;      // запись попадания активна
    static unsigned long long s_rayData = 0;  // последний живой GKo игры
    static unsigned long long s_rayPrimary = 0, s_rayFallback = 0;
    static int   s_sweepIdx = 0;        // курсор «веера» по узлам
    static bool  s_tpOn = false;        // Melee TP включён нашей записью
    static float s_tpRadius = 0.f;      // радиус, который уже опубликован
    static float s_tpTime = 0.f;        // с до следующей публикации задания

    auto releaseTap = [&]() {
        if (s_tapDown) { Touch_Up_N(2); s_tapDown = false; }
        s_tapTimer = 0;
    };
    auto releasePin = [&]() {
        if (s_pinned) { esp_magic_hit_release(); s_pinned = false; }
    };
    auto releaseTp = [&]() {
        if (s_tpOn) { esp_melee_tp_update(false, 0.f, 0.f); s_tpOn = false; s_tpRadius = 0.f; }
        g_farmTpActive = false;
        g_farmTpReach = 0.f;
    };

    if (dt <= 0.f || !std::isfinite(dt)) dt = 1.f / 60.f;
    if (dt > 0.1f) dt = 0.1f;

    unsigned mask = 0;
    if (g_state.farm_on) {
        if (g_state.farm_wood)   mask |= 1u;
        if (g_state.farm_stone)  mask |= 2u;
        if (g_state.farm_metal)  mask |= 4u;
        if (g_state.farm_sulfur) mask |= 8u;
        if (g_state.farm_barrel) mask |= 16u;
    }
    esp_farm_set_resources(mask);
    esp_farm_set_range(g_state.farm_range);

    if (mask == 0 || !g_esp_attached) {
        releaseTap();
        releasePin();
        releaseTp();
        reset_status();
        return;
    }

    float sw = (float)native_window_screen_x;
    float sh = (float)native_window_screen_y;
    if (displayInfo.width > displayInfo.height && displayInfo.width >= 100 && displayInfo.height >= 100) {
        sw = (float)displayInfo.width;  sh = (float)displayInfo.height;
    } else if (displayInfo.height > displayInfo.width && displayInfo.height >= 100 && displayInfo.width >= 100) {
        sw = (float)displayInfo.height; sh = (float)displayInfo.width;
    }
    if (sw < 100.f || sh < 100.f) { releaseTap(); releasePin(); reset_status(); return; }

    // Снимок камеры этого кадра: по нему меряются дистанции и проекция меток.
    FrameBoxes(sw, sh);

    // ---- Melee TP --------------------------------------------------------------
    // Пишем дальность ДО выбора цели: радиус поиска узлов — это и есть та
    // дальность, которую игра теперь считает своей. Запись держит отдельный
    // поток (игра возвращает прежние значения каждый раз, когда орудие
    // достают), здесь только публикуем задание.
    const float tpRadius = clampf(g_state.farm_tp_radius, 0.5f, 12.f);
    if (g_state.farm_tp) {
        // Задание публикуем не каждый кадр: сама запись живёт в потоке
        // (он переписывает поля каждые 8 мс), а публикация стоит полного
        // прохода «игрок -> FPManager -> оружие». Радиус изменился — применяем
        // немедленно, иначе раз в 100 мс, чтобы поймать смену орудия в руках.
        s_tpTime -= dt;
        if (!s_tpOn || fabsf(tpRadius - s_tpRadius) > 0.01f || s_tpTime <= 0.f) {
            const float sphere = clampf(tpRadius * kSphereFraction, kSphereMin, kSphereMax);
            esp_melee_tp_update(true, tpRadius, sphere);
            s_tpOn = true;
            s_tpRadius = tpRadius;
            s_tpTime = 0.1f;
        }
    } else {
        releaseTp();
    }
    bool tpActive = false;
    float tpReach = 0.f, tpSphere = 0.f;
    esp_melee_tp_status(tpReach, tpSphere, tpActive);
    (void)tpSphere;   // радиус сферы нужен только самой записи
    g_farmTpActive = tpActive;
    g_farmTpReach = tpActive ? tpReach : 0.f;

    // ---- Узлы в радиусе --------------------------------------------------------
    FarmSwarmNode nodes[kMaxSwarm];
    FarmSwarmInfo info;
    // Без TP радиус поиска равен настоящей дальности орудия: смысла держать в
    // списке то, до чего удар всё равно не дотянется, нет. Дальность ещё не
    // прочиталась (орудие не в руках) — берём скромные 2 м, список всё равно
    // пойдёт в метки, а бить будет нечем.
    float radius = g_state.farm_tp ? tpRadius : 2.0f;
    if (!g_state.farm_tp && g_farmReach > 0.3f) radius = g_farmReach + kPlainRadiusPad;
    const int count = esp_farm_get_swarm(nodes, kMaxSwarm, radius, info);
    esp_farm_debug(g_farmScanned, g_farmReason);
    g_farmNodes = count;
    g_farmReach = info.melee_reach;
    g_farmAttackPeriod = info.attack_period;
    g_farmToolHave = info.tool_purposes;
    g_farmToolNeed = info.tool_need;
    g_farmTemplates = info.templates;
    g_farmIndexed = info.indexed;
    g_farmNodesWorld = info.nodes_total;
    g_farmLearn = info.learn_state;
    g_farmRayStage = info.ray_stage;
    g_farmRayBest = info.ray_stage_best;
    {
        unsigned held = 0, lost = 0;
        esp_magic_hit_stats(held, lost);
        const unsigned total = held + lost;
        g_farmPinHold = total ? (int)((held * 100ULL) / total) : -1;
    }

    // Раз в секунду пишем состояние в лог: без этой строки «включил, подошёл,
    // ничего не происходит» неотличимо от «узлы не найдены», «нет орудия»,
    // «шаблон не выучен» и «кнопка атаки не там».
    {
        static float s_logTime = 0.f;
        s_logTime -= dt;
        if (s_logTime <= 0.f) {
            s_logTime = 1.f;
            LogLine("[farm] nodes=%d/%d near=%d tpl=%d idx=%d learn=%d reach=%.2f tp=%d/%.2f "
                    "tool=%d/%d reason=%d swings=%d pins=%d",
                    info.nodes_near, info.nodes_total, count, info.templates, info.indexed,
                    info.learn_state, info.melee_reach, tpActive ? 1 : 0, tpReach,
                    info.tool_purposes, info.tool_need, g_farmReason, g_farmSwings, g_farmHits);
        }
    }

    // Живой GKo запоминаем: он нужен, даже когда в этом кадре луч игры ушёл в
    // небо (обёртка пуста). Сам объект при этом никуда не девается — он
    // остаётся в поле, в которое мы его прописали.
    if (info.ray_ready) {
        s_rayData = info.ray_data;
        if (info.primary) s_rayPrimary = info.primary;
        if (info.fallback) s_rayFallback = info.fallback;
    }

    // Пауза: открытое меню и калибровка зон. Удары самого игрока мы не трогаем
    // (в отличие от старого бота, он тут ни с кем камеру не делит), поэтому ни
    // палец аима, ни активный аимбот Magic-Farm больше не останавливают.
    const bool paused = g_sheet.visible || (g_pop.visible && !g_pop.closing) ||
                        g_calibMode != 0;
    g_farmPaused = paused;

    // Метки по всем найденным узлам рисуем всегда: по ним видно и что чит
    // нашёл, и по какому узлу он бьёт прямо сейчас.
    int best = -1;
    float bestScore = 1e18f;
    for (int i = 0; i < count; ++i) {
        const FarmSwarmNode& node = nodes[i];
        // Выбитый узел больше не цель: убираем его из реестра сразу, иначе он
        // продолжал бы перетягивать на себя удары как ближайший.
        if (node.fraction >= 0.f && node.fraction < kDepletedFrac) {
            esp_farm_blacklist(node.id, 120.f);
            continue;
        }
        const float score = node_score(node);
        if (score < bestScore) { bestScore = score; best = i; }
    }

    // «Веер»: каждый удар уходит в следующий узел по кругу — то самое «иду, а
    // всё вокруг разваливается». По умолчанию выключен: добивать узел по
    // одному быстрее, чем размазывать урон по всей поляне.
    int target = best;
    if (g_state.farm_sweep && count > 0) {
        for (int step = 0; step < count; ++step) {
            const int idx = (s_sweepIdx + step) % count;
            const FarmSwarmNode& node = nodes[idx];
            if (node.fraction >= 0.f && node.fraction < kDepletedFrac) continue;
            target = idx;
            break;
        }
    }

    for (int i = 0; i < count; ++i) draw_node(nodes[i], i == target);

    if (target < 0 || paused) {
        releaseTap();
        releasePin();
        g_farmActive = (count > 0);
        g_farmPhase = 0;
        if (target < 0) {
            g_farmTgtDist = 0.f; g_farmSpot = 0; g_farmStreak = 0;
            g_farmHpPct = -1.f; g_farmSpotLife = -1.f; g_farmXp = 0;
        }
        return;
    }

    const FarmSwarmNode& tgt = nodes[target];
    g_farmActive = true;
    g_farmTgtDist = tgt.dist;
    g_farmTgtKind = tgt.kind;
    g_farmSpot = tgt.at_spot ? tgt.spot_source : 0;
    g_farmStreak = tgt.streak;
    g_farmHpPct = (tgt.health >= 0.f && tgt.health_max > 0.f)
                ? 100.f * tgt.health / tgt.health_max : -1.f;
    g_farmSpotLife = tgt.at_spot ? tgt.spot_life : -1.f;
    g_farmXp = tgt.experience;

    // ---- Запись попадания ------------------------------------------------------
    // GKo с результатом рейкаста игра использует не только для удара: из него
    // же берутся подсказки взаимодействия (подобрать, открыть, залезть), и
    // пока пин держится, они показывают выбранный узел, а не то, на что
    // смотрит игрок. Это сознательный размен: пин, который снимался между
    // ударами, игра успевала перебить своим рейкастом, и удар уходил по
    // направлению взгляда. Пин живёт, пока есть цель, и снимается при её
    // пропаже, паузе меню и выключении чита.
    const bool canPin = g_state.farm_silent && tgt.have_template && s_rayData != 0 &&
                        (s_rayPrimary != 0 || s_rayFallback != 0);
    auto pinTarget = [&](bool hot) {
        if (!canPin) { if (s_pinned) releasePin(); return; }
        MagicHit hit;
        hit.hot = hot;
        hit.ray_data = s_rayData;
        hit.primary = s_rayPrimary;
        hit.fallback = s_rayFallback;
        hit.hit_object = tgt.hit_object;
        hit.collider = tgt.collider;
        hit.x = tgt.x; hit.y = tgt.y; hit.z = tgt.z;
        // Дистанцию пишем свою: игра сравнивает именно её с дальностью удара,
        // и настоящие метры до узла нас в этой проверке только ограничивают.
        const float limit = (info.melee_reach > 0.2f)
                          ? info.melee_reach * kDistanceFraction : kDistanceFallback;
        hit.distance = (tgt.dist > 0.05f && tgt.dist < limit) ? tgt.dist : limit;
        if (esp_magic_hit_pin(hit)) s_pinned = true;
    };

    // Удар без шаблона имеет смысл ровно в одном случае: когда собственный луч
    // игры прямо сейчас упирается в ресурс (learn_state == 2) — тогда штатный
    // удар и так попадёт куда надо, а заодно выучится шаблон. Во всех
    // остальных случаях махать нечем: это и был тот самый «бот просто бьёт
    // туда, куда смотрит». Метки узлов при этом рисуются, состояние в меню
    // показывает, чего не хватает.
    // Когда игра вообще не публикует GKo (стадия ниже 4), подменять нечего и
    // пин невозможен — но штатный удар в упор всё ещё работает. Поэтому у
    // самой цели (до 3.5 м) бьём как обычный автофарм, а не стоим столбом.
    const bool rayUsable = info.ray_stage >= 4;
    const bool closeBlind = !rayUsable && tgt.dist <= 3.5f;
    if (!tgt.have_template && info.learn_state != 2 && !closeBlind) {
        releaseTap();
        releasePin();
        g_farmPhase = 1;
        return;
    }

    // ---- Авто-удары ------------------------------------------------------------
    // Единственный синтетический ввод Magic-Farm. Такт берётся из самого
    // орудия: удар раньше кулдауна игра ставит в очередь и потом выпускает
    // пачкой — в этот момент пин уже может указывать на другой узел.
    if (!g_state.farm_swing) {
        // Авто-ударов нет — бьёт сам игрок, и момент удара нам неизвестен.
        // Значит, пин всегда горячий: только так подмена доживает до чужого
        // удара, в какой бы миг кадра он ни случился.
        releaseTap();
        pinTarget(true);
        g_farmPhase = 2;
        return;
    }

    float tapDownMs = kTapDownMsDefault, tapUpMs = kTapUpMsDefault;
    if (info.attack_period > 0.05f && info.attack_period < 5.f) {
        const float periodMs = clampf(info.attack_period * 1020.f, 250.f, 2000.f);
        tapDownMs = clampf(periodMs * 0.10f, 40.f, 120.f);
        tapUpMs = fmaxf(periodMs - tapDownMs, 60.f);
    }

    s_tapTimer -= (int)roundf(dt * 1000.f);

    // Пин держится ПОСТОЯННО, пока есть цель, а не только вокруг удара.
    //
    // Раньше он взводился ровно перед тапом и снимался сразу после. Но игра
    // переписывает свой рейкаст каждым кадром, и между её записью и чтением в
    // FPMelee проходят доли миллисекунды: наша разовая запись к этому моменту
    // была уже затёрта, и удар уходил туда, куда смотрит камера. Теперь пин
    // живёт всё время, а за 150 мс до удара и на всём нажатии переходит в
    // горячий режим (писатель долбит каждые 120 мкс вместо 2 мс).
    const bool hot = s_tapDown || s_tapTimer <= 150;
    pinTarget(hot);

    if (s_tapTimer <= 0) {
        if (!s_tapDown) {
            if (s_pinned) ++g_farmHits;   // счётчик перенаправленных ударов
            const float fx = (g_state.farm_fire_x >= 0.f) ? sw * g_state.farm_fire_x : sw * 0.88f;
            const float fy = (g_state.farm_fire_y >= 0.f) ? sh * g_state.farm_fire_y : sh * 0.66f;
            Touch_Down_N(2, fx, fy);
            ++g_farmSwings;
            s_tapDown = true;
            s_tapTimer = (int)tapDownMs;
            g_farmPhase = 3;
        } else {
            Touch_Up_N(2);
            s_tapDown = false;
            s_tapTimer = (int)tapUpMs;
            g_farmPhase = 2;
            // Пин НЕ снимаем: до следующего удара он должен успеть
            // закрепиться, а снятие и повторный взвод каждые 300 мс — это
            // как раз та пауза, в которую игра возвращала свой рейкаст.
            // Отпускается он при пропаже цели, паузе и выключении чита.
            // Удар отпущен — «веер» переводит прицел на следующий узел.
            if (g_state.farm_sweep && count > 0) s_sweepIdx = (target + 1) % count;
        }
    } else {
        g_farmPhase = s_tapDown ? 3 : 2;
    }
}
