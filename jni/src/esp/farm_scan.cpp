// farm_scan.cpp — Автофарм: скан узлов реестра.
//
// Модуль разрезан из прежнего монолита jni/src/game.cpp;
// что здесь лежит и кто это зовёт — в шапке farm_scan.h и в docs/CODE_MAP.md.

#include "esp/common.h"
#include "esp/farm_target.h"
#include "esp/game_patch.h"
#include "esp/il2cpp.h"
#include "esp/managed.h"
#include "esp/marker_labels.h"
#include "esp/markers.h"
#include "esp/mem.h"
#include "esp/names.h"
#include "esp/skeleton_cache.h"
#include "esp/skeleton_names.h"
#include "esp/transform.h"
#include "farm_scan.h"

std::vector<FarmEntity> g_farm_entities;

std::unordered_map<uint64_t, int> g_farm_blacklist; // identity -> frames left

// Когда можно начинать следующий проход скана реестра, в mono_seconds().
// Раньше здесь был счётчик КАДРОВ (120, при пустом списке 30), рассчитанный на
// 60 fps: устройство пользователя рисует 118 кадров/с, поэтому скан запускался
// вдвое чаще задуманного — раз в ~1 с, а при пустом списке раз в 0.25 с.
static double g_farm_next_scan = 0.0;

// Определение скана ниже (ему нужны FarmEntity и резолверы реестра), а
// сбрасывать его приходится и отсюда — при перезагрузке мира.
static void farm_scan_abort();

void farm_scan_reset();

// Орудие в руках и что запросили отброшенные узлы (флаги ToolPurpose) — для
// строки статуса в меню: «нужен топор» вместо бессмысленного «все узлы вне
// радиуса».
int g_farm_tool_have = 0;

int g_farm_tool_need = 0;

// Why the picker returned nothing (surfaced in the menu status line):
// 0 ok, 1 off, 2 frame not published, 3 no nodes in registry, 4 none in
// range, 5 camera pose unreadable, 6 nodes are there but the tool in hand
// cannot harvest them (m_RequiredToolPurpose vs FPTool.m_ToolPurposes).
int g_farm_idle_reason = 1;

// Farm kind from a loot item short name. Rank: richer resource wins (metal and
// sulfur nodes drop stones too). Processed items are filtered like the ore
// markers, so barrels never register.
static int farm_kind_for_item_name(const char* item_name, int& kind) {
    if (!item_name || !item_name[0]) return 0;
    char key[40];
    size_t n = 0;
    for (const char* p = item_name; *p && n + 1 < sizeof(key); ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        key[n++] = c;
    }
    key[n] = '\0';
    if (strstr(key, "frag") || strstr(key, "pipe") || strstr(key, "sheet") ||
        strstr(key, "scrap") || strstr(key, "spring") || strstr(key, "gear"))
        return 0;
    if (strstr(key, "sulfur")) { kind = 3; return 4; }
    if (strstr(key, "metal"))  { kind = 2; return 3; }
    if (strstr(key, "stone"))  { kind = 1; return 2; }
    if (strstr(key, "wood"))   { kind = 0; return 1; }
    return 0;
}

// Same two loot lists the markers read, but wood ranks too (trees are the
// whole point here, while the markers skip them to keep the screen clean).
static bool farm_kind_from_loot(uint64_t mineable, int& kind) {
    const uint64_t sources[2] = {MINEABLE_LOOT, MINEABLE_FINISH_BONUS};
    int best = 0;
    for (uint64_t source : sources) {
        uint64_t items[12];
        int count = read_managed_collection(rd_ptr(mineable + source), items, 12);
        for (int i = 0; i < count; ++i) {
            if (!valid_obj(items[i])) continue;
            char name[32] = {};
            if (!read_managed_string(rd_ptr(items[i] + LOOTITEM_ITEM_NAME), name, sizeof(name))) continue;
            int candidate = 0;
            int rank = farm_kind_for_item_name(name, candidate);
            if (rank > best) { best = rank; kind = candidate; }
            if (best == 4) return true;
        }
    }
    return best > 0;
}

// ---- Скан реестра: порциями и с отрицательным кешем -------------------------
// Жалоба пользователя: «при включении автофарма минуту лагают все визуалы, потом
// иногда подлагивает». Замер по логу 14.09: кадр оверлея в норме 8.5 мс, но 153
// кадра из 10053 вырастали до 12..34 мс, и интервалы между ними складываются в
// периодический рисунок 8/52/68/112 кадров — это ресканы. Один проход стоил
// столько, сколько записей в NetworkClient.spawned: у каждой читались класс,
// список компонентов и сами компоненты (~10 чтений), а объектов в мире
// тысячи. Десятки тысяч чтений одним кадром — тот самый рывок, который видно
// глазом; на 118 fps скан к тому же запускался вдвое чаще задуманного, потому
// что счётчик был в кадрах из расчёта на 60 fps.
//
// Теперь три вещи сразу:
//   1) отрицательный кеш: запись, у которой в компонентах нет MineableObject,
//      запоминается вместе с указателем её списка компонентов и в следующих
//      проходах проверяется ОДНИМ чтением вместо десяти: пока поле указывает на
//      тот же список, объект тот же и вердикт в силе. Адрес освободившегося
//      объекта игра может отдать новому (в том числе узлу добычи) — тогда поле
//      почти наверняка смотрит на другой список, запись выбрасывается и объект
//      классифицируется заново. Узлов добычи в мире десятки, прочих объектов
//      тысячи, поэтому установившийся проход стоит по одному чтению на запись;
//   2) порционность: расход кадра задан в условных чтениях (kFarmScanBudget),
//      сверка с кешем стоит одно чтение, классификация новой записи — около
//      двенадцати, поэтому даже холодный проход растягивается по кадрам и ни
//      один кадр не платит за весь мир сразу;
//   3) результат прохода подменяет рабочий список ЦЕЛИКОМ в конце: пока проход
//      идёт, бот работает по прежнему списку. Прежняя гарантия («сбой чтения не
//      должен вычищать кеш — иначе маркер пропадал и бот вставал») сохранена и
//      усилена: окна «целей нет» на время скана больше не существует вовсе.
// identity -> указатель её списка компонентов на момент вердикта «не узел».
static std::unordered_map<uint64_t, uint64_t> g_farm_not_node;

static std::vector<uint64_t>   g_farm_scan_ids;       // записи текущего прохода

static size_t                  g_farm_scan_idx = 0;   // курсор прохода

static std::vector<FarmEntity> g_farm_scan_stage;     // накопленный результат

static bool                    g_farm_scan_run = false;

// Сколько условных единиц бюджета скан потратил на прошлом кадре и сколько
// записей реестра в нём всего/осталось. Печатается в строке «кадры оверлея»:
// по этим числам видно, идёт ли проход прямо сейчас и насколько он длинный.
static int                     g_farm_scan_units = 0;

static int                     g_farm_scan_total = 0;

static int                     g_farm_scan_new   = 0;

// Бюджет прохода — в условных чтениях за кадр, а не в записях: сверка с кешем
// стоит одно чтение, классификация новой записи — около двенадцати (класс,
// список, компоненты, трансформ, иногда имя префаба и loot-списки). Пока целей
// нет вовсе (старт фарма, перезагрузка мира), бот просто стоит, поэтому идём
// вчетверо быстрее; когда список есть — спешить некуда, старый список остаётся
// рабочим до конца прохода.
constexpr int    kFarmScanBudget     = 200;  // мягкий бюджет, единицы чтения/кадр

constexpr int    kFarmScanBudgetFast = 600;  // пока рабочий список пуст

constexpr int    kFarmScanCostCached = 1;    // сверка записи из кеша

constexpr int    kFarmScanCostNew    = 12;   // классификация новой записи

constexpr double kFarmScanPeriod   = 2.0;   // с между проходами, когда узлы есть

constexpr double kFarmScanRetry    = 0.5;   // с, когда список пуст (мир грузится)

constexpr size_t kFarmNegCacheMax  = 16384; // потолок кеша, записей

// Бросить незавершённый проход (смена маски ресурсов, перезагрузка мира).
static void farm_scan_abort() {
    g_farm_scan_run = false;
    g_farm_scan_idx = 0;
    g_farm_scan_ids.clear();
    g_farm_scan_stage.clear();
}

// Полный сброс скана: рабочий список чистит вызывающий, здесь — кеш и проход.
void farm_scan_reset() {
    farm_scan_abort();
    g_farm_not_node.clear();
    g_farm_next_scan = 0.0;
}

// Вердикт по одной записи реестра:
//   0 — структурно не узел добычи; behaviours — её список компонентов (по нему
//       запись потом сверяется одним чтением вместо полной классификации);
//   1 — узел есть, но сейчас он не наш: маска ресурсов, сбой чтения, ещё не
//       изучена раскладка трансформов. Запоминать НЕЛЬЗЯ — условие временное;
//   2 — наш узел, entity заполнена (transform, required_purpose, pos);
//   3 — запись вообще не NetworkIdentity: кешировать нечего, повторная проверка
//       и так стоит одно чтение за проход.
static int farm_classify_identity(uint64_t identity, uint64_t identity_class,
                                  FarmEntity& entity, uint64_t& behaviours) {
    behaviours = 0;
    if (identity_class && rd_ptr(identity) != identity_class) return 3;
    uint64_t array = rd_ptr(identity + NETID_BEHAVIOURS);
    if (!valid_obj(array)) return 3;
    behaviours = array;
    int32_t behaviour_count = rd<int32_t>(array + IL2CPP_ARRAY_LENGTH);
    if (behaviour_count <= 0) return 0;
    if (behaviour_count > 32) behaviour_count = 32;
    uint64_t comps[32];   // список компонентов (behaviours — выходной параметр)
    if (!rd_buf(array + IL2CPP_ARRAY_FIRST_ELEMENT, comps,
                (size_t)behaviour_count * sizeof(uint64_t)))
        return 1;   // словарь переписывают на ходу: не вердикт, а повод повторить

    // Kind: the entityType enum first (cheap and exact), loot second.
    uint64_t component = 0;
    int kind = -1;
    for (int32_t b = 0; b < behaviour_count && kind < 0; ++b) {
        const uint64_t cand = comps[b];
        if (!valid_obj(cand)) continue;
        if (marker_class_of(rd_ptr(cand)) != MARKER_CLASS_MINEABLE) continue;
        component = cand;
        switch ((MineableEntityType)rd<int32_t>(component + MINEABLE_ENTITY_TYPE)) {
            case MineableEntityType::Tree:   kind = 0; break;
            case MineableEntityType::Stone:  kind = 1; break;
            case MineableEntityType::Iron:   kind = 2; break;
            case MineableEntityType::Sulfur: kind = 3; break;
            default: break;
        }
        if (kind < 0 && !farm_kind_from_loot(component, kind)) kind = -1;
    }
    if (!component) return 0;   // в компонентах нет MineableObject — не узел
    if (kind < 0) return 1;     // узел, но ресурс не опознан (сбой чтения)
    if (!(g_farm_mask & (1u << kind))) return 1;   // маску меняют на лету

    entity.identity = identity;
    entity.component = component;
    entity.kind = kind;
    entity.transform = native_component_transform(managed_object_native(component));
    if (!entity.transform)
        entity.transform = native_component_transform(managed_object_native(identity));
    if (!entity.transform) return 1;   // раскладка трансформов ещё не изучена

    // Fallen logs register as "Tree" but cannot be chopped the same way — the
    // bot just circles them. Filter them out by prefab name (log / fallen /
    // dead / driftwood variants). Вердикт структурный, поэтому бревно уходит в
    // отрицательный кеш: чтение имени (несколько syscall'ов плюс строка) больше
    // не повторяется на каждом проходе.
    if (kind == 0 && g_go_name_offset_valid) {
        char go_name[48];
        if (read_transform_name(entity.transform, go_name, sizeof(go_name))) {
            for (char* p = go_name; *p; ++p)
                if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
            if (strstr(go_name, "log") || strstr(go_name, "fallen") ||
                strstr(go_name, "dead") || strstr(go_name, "driftwood") ||
                strstr(go_name, "stump"))
                return 0;
        }
    }

    // Чем этот узел вообще можно взять. Мусорное значение (не из набора флагов)
    // считаем отсутствием требования, чтобы ошибка чтения не оставила фарм без
    // целей.
    {
        const int32_t known = (int32_t)ToolPurpose::CutWood |
                              (int32_t)ToolPurpose::BreakRocks |
                              (int32_t)ToolPurpose::CutAnimals;
        const int32_t purpose = rd<int32_t>(component + MINEABLE_REQUIRED_TOOL_PURPOSE);
        entity.required_purpose = ((purpose & ~known) == 0) ? (int)purpose : 0;
    }

    entity.pos_valid = marker_world_position(entity.transform, entity.pos);
    return 2;
}

// Один шаг скана. Вызывается каждый кадр; сам решает, пора ли начинать проход и
// не пора ли его закончить. Дороже kFarmScanBudget записей за кадр не делает.
void farm_scan_tick() {
    const double now = mono_seconds();
    if (!g_farm_scan_run && now < g_farm_next_scan) return;

    if (!g_farm_scan_run) {
        // Начало прохода. Сам словарь читается одним куском (это дёшево), а вот
        // классификация записей растягивается по кадрам. Сбой чтения здесь не
        // вердикт: рабочий список не тронут, повторим через kFarmScanRetry.
        uint64_t dictionary = resolve_network_client_spawned();
        if (!dictionary) { g_farm_next_scan = now + kFarmScanRetry; return; }
        uint64_t entries = rd_ptr(dictionary + DICT_ENTRIES);
        int32_t count = rd<int32_t>(dictionary + DICT_COUNT);
        if (!valid_obj(entries) || count <= 0) {
            g_farm_next_scan = now + kFarmScanRetry;
            return;
        }
        if (count > 4096) count = 4096;
        std::vector<uint8_t> buffer((size_t)count * DICT_ENTRY_STRIDE);
        if (!rd_buf(entries + IL2CPP_ARRAY_FIRST_ELEMENT, buffer.data(), buffer.size())) {
            g_farm_next_scan = now + kFarmScanRetry;
            return;
        }
        g_farm_scan_ids.resize((size_t)count);
        g_farm_scan_total = count;
        for (int32_t i = 0; i < count; ++i)
            memcpy(&g_farm_scan_ids[(size_t)i],
                   buffer.data() + (size_t)i * DICT_ENTRY_STRIDE + DICT_ENTRY_VALUE,
                   sizeof(uint64_t));
        g_farm_scan_idx = 0;
        g_farm_scan_stage.clear();
        g_farm_scan_run = true;
        // Фильтр поваленных брёвен нуждается в смещении имени GameObject.
        // Ищем его раз на проход (а не каждый кадр прохода): попытка стоит
        // обхода поддерева трансформов, см. ensure_gameobject_name_offset.
        if (!g_go_name_offset_valid && g_local_player)
            ensure_gameobject_name_offset(g_local_player);
    }

    const uint64_t identity_class = resolve_network_identity_class();

    int budget = g_farm_entities.empty() ? kFarmScanBudgetFast : kFarmScanBudget;
    g_farm_scan_units = 0;
    g_farm_scan_new = 0;
    while (g_farm_scan_idx < g_farm_scan_ids.size()) {
        const uint64_t identity = g_farm_scan_ids[g_farm_scan_idx++];
        if (!valid_obj(identity)) continue;
        const auto cached = g_farm_not_node.find(identity);
        const int cost = (cached != g_farm_not_node.end()) ? kFarmScanCostCached
                                                           : kFarmScanCostNew;
        if (budget < cost) { --g_farm_scan_idx; break; }  // доработаем в следующем кадре
        budget -= cost;
        g_farm_scan_units += cost;
        if (cost == kFarmScanCostNew) ++g_farm_scan_new;

        if (cached != g_farm_not_node.end()) {
            // Одно чтение: список компонентов на месте и тот же — объект не
            // сменился, вердикт «не узел добычи» остаётся в силе.
            if (rd_ptr(identity + NETID_BEHAVIOURS) == cached->second) continue;
            g_farm_not_node.erase(cached);   // адрес переиспользовали
        }

        FarmEntity entity;
        uint64_t behaviours = 0;
        const int verdict = farm_classify_identity(identity, identity_class, entity, behaviours);
        if (verdict == 0) {
            if (behaviours && g_farm_not_node.size() < kFarmNegCacheMax)
                g_farm_not_node.emplace(identity, behaviours);
            continue;
        }
        if (verdict != 2) continue;
        g_farm_scan_stage.push_back(entity);
        if (g_farm_scan_stage.size() >= 512) {
            g_farm_scan_idx = g_farm_scan_ids.size();
            break;
        }
    }
    if (g_farm_scan_idx < g_farm_scan_ids.size()) return;   // проход не закончен

    g_farm_entities = std::move(g_farm_scan_stage);
    g_farm_scan_stage.clear();
    g_farm_scan_ids.clear();
    g_farm_scan_idx = 0;
    g_farm_scan_run = false;
    // Пустой результат означает «реестр ещё не читается / мир грузится после
    // респауна» — повторяем чаще, но всё равно не каждый кадр.
    g_farm_next_scan = now + (g_farm_entities.empty() ? kFarmScanRetry : kFarmScanPeriod);
}

void esp_farm_set_resources(unsigned mask) {
    if (g_farm_mask != mask) {
        g_farm_mask = mask;
        g_farm_entities.clear();
        // Проход скана, если он шёл, собран под старой маской — выбрасываем его
        // и начинаем заново. Отрицательный кеш при этом жив: «нет компонента
        // MineableObject» от выбранных галочек ресурсов не зависит.
        farm_scan_abort();
        g_farm_next_scan = 0.0;
    }
    if (!mask) g_farm_blacklist.clear();
}

// Search radius for farm nodes, metres. Set from the menu slider.
float g_farm_max_distance = 100.0F;

void esp_farm_set_range(float meters) {
    if (!std::isfinite(meters)) return;
    if (meters < 10.0F) meters = 10.0F;
    if (meters > 300.0F) meters = 300.0F;
    g_farm_max_distance = meters;
}

void esp_farm_blacklist(unsigned long long id, float seconds) {
    if (!id) return;
    int frames = (int)(seconds * 60.0F);
    if (frames < 60) frames = 60;
    g_farm_blacklist[(uint64_t)id] = frames;
    // Drop it from the cache right away and rescan soon: keeping a dead
    // node around until the next 2 s rescan is what made the bot jerk the
    // camera at garbage coordinates after finishing a tree.
    for (auto it = g_farm_entities.begin(); it != g_farm_entities.end(); ++it) {
        if (it->identity == (uint64_t)id) { g_farm_entities.erase(it); break; }
    }
    const double soon = mono_seconds() + 0.25;
    if (g_farm_next_scan > soon) g_farm_next_scan = soon;
}

// ---- Крестик: читаем из памяти, а не угадываем ------------------------------
// Старая реализация искала X перебором детей узла (по именам, LOD-мешам и
// «прыгающему» трансформу) и потому регулярно целилась в ствол, в спящий
// шаблон на пивоте или в корень под землёй — отсюда половина багов автофарма.
//
// Игра хранит крестик сама, и хранит его там, где его же и проверяет:
//   MineableObject.QWD (0xE8) — массив JE-экстеншенов узла, заполняется в
//   MineableObject.cik() (GetComponents<JE> на GameObject узла), который
//   вызывается из OnStartClient -> ciQ() и при каждом попадании (SRl -> cik).
//     руда   -> MineableObjectExtension_OreHitstreaks, живой клон в MoW (0x30);
//               мировая позиция его transform'а и есть точка крестика —
//               именно её читает проверка попадания OreHitstreaks.os().
//     дерево -> MineableObjectExtension_TreeHitstreaks, живой клон в MTn (0x50),
//               а точка попадания на коре — Vector3 в MTQ (0x88); проверка
//               попадания (giL и обвязка rRS/DMU/Dne/DfW) меряет дистанцию от
//               точки удара до отрезка MTQ..MTu с радиусом 0.15 м.
//
// X существует не всегда: игра создаёт его при ПЕРВОМ попадании по узлу
// (JE.gir) и уничтожает через 15 секунд простоя (OreHitstreaks.giq /
// TreeHitstreaks.gie обнуляют MoW/MTn и вызывают Object.Destroy на маркере).
// Поэтому «крестика нет» — штатное состояние: бьём по корпусу, первый удар
// создаёт X, и дальше все удары идут уже в него.
static constexpr const char* kFarmExtOreClass  = "MineableObjectExtension_OreHitstreaks";

static constexpr const char* kFarmExtTreeClass = "MineableObjectExtension_TreeHitstreaks";

// Крестик обязан лежать на узле: у руды его ставит Collider.ClosestPoint
// (поверхность камня), у дерева — рейкаст по стволу. Всё, что
// дальше нескольких метров от пивота, — мусор чтения или маркер соседнего узла.
bool farm_spot_on_node(const Vec3& spot, const Vec3& node, int kind) {
    const float dx = spot.x - node.x, dy = spot.y - node.y, dz = spot.z - node.z;
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz)) return false;
    const float max_h  = (kind == 0) ? 6.0F : 4.0F;   // крона/ствол vs камень
    const float max_up = (kind == 0) ? 26.0F : 9.0F;
    return (dx * dx + dz * dz) <= max_h * max_h && dy >= -8.0F && dy <= max_up;
}

// JE-экстеншен крестика для узла. QWD заполняется игрой при OnStartClient,
// поэтому обычно находится с первого раза; если кеш ещё пуст — повторяем раз в
// полсекунды (чтение имён классов дорогое, гонять его каждый кадр нельзя).
void farm_resolve_extension(FarmEntity& entity) {
    if (entity.ext_kind != FARM_EXT_NONE && valid_obj(entity.ext)) return;
    if (entity.ext_age > 0) { --entity.ext_age; return; }
    entity.ext_age = 30;                       // ~0.5 с до следующей попытки
    entity.ext = 0;
    entity.ext_kind = FARM_EXT_NONE;
    if (!valid_obj(entity.component)) return;

    uint64_t items[8];
    const int count = read_managed_collection(rd_ptr(entity.component + MINEABLE_EXTENSIONS),
                                              items, 8);
    for (int i = 0; i < count; ++i) {
        const uint64_t obj = items[i];
        if (!valid_obj(obj)) continue;
        if (object_class_name_is(obj, kFarmExtOreClass)) {
            entity.ext = obj; entity.ext_kind = FARM_EXT_ORE; return;
        }
        if (object_class_name_is(obj, kFarmExtTreeClass)) {
            entity.ext = obj; entity.ext_kind = FARM_EXT_TREE; return;
        }
    }
}
