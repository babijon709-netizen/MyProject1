#pragma once
// vis_check_dyn.h — ДИНАМИЧЕСКАЯ часть проверки видимости: постройки игроков.
//
// Зачем отдельный слой. Статический меш карты (vis_check_map.h) не знает
// построек: фундаменты, стены, двери, потолки, турели и ящики спавнятся в
// рантайме, их нет ни в одном ассете как «кусок мира» — в ассетах лежит только
// ПРЕФАБ. Поэтому динамика собирается так:
//
//   * префабы выгружаются один раз на ПК (AssetRipper/AssetStudio, Mesh ->
//     Export) в библиотеку .obj — по файлу на имя префаба;
//   * положение каждого поставленного куска читается из памяти: обход
//     Mirror.NetworkClient.spawned -> NetworkIdentity -> GameObject -> имя
//     префаба + мировой TRS из иерархии трансформов (позиция И поворот);
//   * кусок инстансится в мир (AppendObb / TransformTri) и складывается во
//     второе BVH, которое пересобирается проходом по spawned раз в ~0.7 с.
//
// Почему сопоставление по ИМЕНИ, а не по классу: имена игровых объектов —
// контент, обфускатор их не трогает (проект уже читает имена префабов у лута и
// маркеров), а имена классов в этом билде переименованы (Building.BuildingPiece
// известен только как поле LootObject+0xF8). Совпадения имени нет — кусок
// пропускается, проверка НЕ ломается: статическое дерево продолжает работать,
// неизвестность по-прежнему отдаётся как «видна».
//
// Чего здесь сознательно нет: угаданных смещений нативных классов. libunity.so
// в репо обрублен (185 динамических символов, ноль _ZTI*/_ZTS*), а имена
// «MeshRenderer»/«BoxCollider» в .rodata используются только name-based
// поиском компонентов (см. tools/unity_rev/NOTES_VIS_DYN.md) — полевые
// смещения лежат в биндингах libil2cpp.so, которого в репо нет. Поэтому
// габариты куска берутся из библиотеки префабов, а не из нативного
// Renderer::m_AABB.
//
// Зависимости: только vis_check.h (std). Файлы и каталоги — <cstdio>/<dirent.h>.
// Читает память НЕ этот слой: коллектор живёт в game_markers.cpp и зовёт сюда
// publish(). Так библиотеку и сборку дерева можно гонять в host-тестах.

#include "vis_check.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <dirent.h>

namespace vis_dyn {

// ---- Ключ имени ------------------------------------------------------------
// Токены имени. Режем и по разделителям, и по camelCase: «WoodWall_LOD0» ->
// {wood, wall, lod0}, «WoodFoundation» -> {wood, foundation}. Один и тот же
// разбор с обеих сторон обязателен — ключ библиотеки строится из имени ФАЙЛА
// («WoodWall.obj»), а ищем мы по имени объекта в сцене («WoodWall_LOD0»): если
// разбирать их по-разному, сопоставление по подмножеству не работает никогда.
inline std::vector<std::string> Tokenize(const char* name) {
    std::vector<std::string> out;
    std::string cur;
    bool prev_upper = false;
    auto flush = [&]() { if (!cur.empty()) { out.push_back(cur); cur.clear(); } prev_upper = false; };
    for (const char* p = name ? name : ""; *p; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (std::isalnum(c)) {
            const bool upper = (c >= 'A' && c <= 'Z');
            // Новая заглавная начинает слово, кроме подряд идущих заглавных
            // (аббревиатура): «LODWall» -> «LOD» + «Wall», «LOD0» -> «lod0».
            if (upper && !cur.empty() &&
                (!prev_upper || (p[1] && std::isalpha((unsigned char)p[1]) &&
                                 std::islower((unsigned char)p[1])))) {
                flush();
            }
            cur += (char)std::tolower(c);
            prev_upper = upper;
        } else {
            flush();
        }
    }
    flush();
    return out;
}

// Ключ — отсортированные токены слитно: «Wood Foundation» и «Foundation Wood»
// дают один ключ «foundationwood».
inline std::string KeyFromTokens(const std::vector<std::string>& tok) {
    std::vector<std::string> s = tok;
    std::sort(s.begin(), s.end());
    std::string out;
    for (const auto& t : s) out += t;
    return out;
}

inline std::string NormalizeKey(const char* name) { return KeyFromTokens(Tokenize(name)); }

// Склейка токенов БЕЗ сортировки — в том порядке, как они идут в имени. Нужна
// только для ступени 3 сопоставления: «WoodWall_LOD0» -> «woodwalllod0», где
// «woodwall» (имя файла одним токеном) лежит подряд, а в отсортированном ключе
// «lod0wallwood» он уже разорван.
inline std::string GlueTokens(const std::vector<std::string>& tok) {
    std::string out;
    for (const auto& t : tok) out += t;
    return out;
}

// Ключ и токены имени ФАЙЛА — тем же Tokenize (camelCase режется одинаково
// с обеих сторон), поэтому отдельного разбора для имён файлов нет.
inline std::vector<std::string> TokenizeFileName(const char* name) { return Tokenize(name); }

// ---- Библиотека префабов ---------------------------------------------------
struct Entry {
    std::string key;                 // NormalizeKey(имя файла без расширения)
    std::string file;                // имя файла — для диагностики
    std::vector<std::string> tokens; // те же токены, для сопоставления по подмножеству
    std::vector<vis::Tri> tris;      // в локальных координатах префаба
    vis::Vec3 center{0, 0, 0};       // локальный габарит: центр…
    vis::Vec3 extents{0, 0, 0};      // …и полуоси (нужны для коробки-замены)
};

struct Library {
    std::unordered_map<std::string, Entry> by_key;
    std::vector<std::string> keys;   // порядок добавления — для диагностики
    size_t tri_total = 0;

    size_t size() const { return by_key.size(); }

    // Сопоставление имени объекта в сцене с префабом библиотеки, три ступени:
    //   1) точное совпадение ключа (имя файла = имя объекта, порядок слов не
    //      важен: «Wood Foundation» == «Foundation Wood»);
    //   2) токены ключа — ПОДМНОЖЕСТВО токенов имени. Так «Foundation Wood
    //      Damaged» находит «WoodFoundation.obj». Работает, только если имя
    //      файла разбилось на токены (camelCase или разделители);
    //   3) запасной путь для имён файлов, которые не разбились: «woodwall.obj»
    //      одним токеном всё равно находится у «WoodWall_LOD0» — слитный ключ
    //      библиотеки ищется подстрокой в слитном ключе имени.
    // Среди кандидатов 2 и 3 побеждает САМЫЙ ДЛИННЫЙ ключ, поэтому «wall» не
    // выигрывает у «woodwall», а «wood» не забирает «woodenboxlarge». Ключ
    // короче 4 символов в нечётких ступенях не участвует вовсе.
    //
    // Всё это — только про ИМЕНА: ни одного смещения в памяти, поэтому
    // переименование классов обфускатором сопоставление не ломает.
    const Entry* Find(const char* name) const {
        if (!name || !*name || by_key.empty()) return nullptr;
        const std::vector<std::string> tok = Tokenize(name);
        if (tok.empty()) return nullptr;
        const std::string key = KeyFromTokens(tok);
        auto it = by_key.find(key);
        if (it != by_key.end()) return &it->second;

        std::vector<std::string> sorted = tok;
        std::sort(sorted.begin(), sorted.end());
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
        const std::string glued = GlueTokens(tok);

        const Entry* best = nullptr;
        size_t best_len = 0;
        for (const auto& kv : by_key) {
            const Entry& e = kv.second;
            if (e.key.size() <= best_len || e.key.size() < 4) continue;
            bool ok = false;
            if (e.tokens.size() <= sorted.size()) {
                ok = true;
                for (const auto& t : e.tokens) {
                    if (!std::binary_search(sorted.begin(), sorted.end(), t)) { ok = false; break; }
                }
            }
            // Имя файла одним токеном («woodwall.obj»): сравнивать не с чем,
            // поэтому ищем его подстрокой в имени объекта, в исходном порядке
            // токенов. Только целиком — «wall» внутри «stonewall» не совпадение.
            if (!ok && e.tokens.size() == 1) ok = glued.find(e.key) != std::string::npos;
            if (!ok) continue;
            best = &e;
            best_len = e.key.size();
        }
        return best;
    }
};

inline std::shared_ptr<const Library> g_lib;
inline std::atomic<size_t> g_lib_entries{0};
inline std::atomic<size_t> g_lib_tris{0};

// Один .obj в библиотеку. scale/yUpToZUp — те же, что у меша карты: префаб
// экспортируется тем же инструментом, значит и единицы те же.
inline bool AddObj(Library& lib, const std::string& path, float scale, bool yUpToZUp) {
    const size_t slash = path.find_last_of('/');
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    // Ключ и токены — из имени ФАЙЛА с разбиением camelCase: «WoodFoundation.obj»
    // даёт {foundation, wood}, и объект «Foundation Wood Damaged» его находит.
    std::vector<std::string> tokens = TokenizeFileName(base.c_str());
    if (tokens.empty()) return false;
    std::sort(tokens.begin(), tokens.end());
    tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
    const std::string key = KeyFromTokens(tokens);
    if (key.empty()) return false;

    std::vector<vis::Tri> tris;
    if (!vis::LoadObj(path, tris, scale, yUpToZUp) || tris.empty()) return false;

    Entry e;
    e.key = key;
    e.file = base;
    e.tokens = tokens;
    if (!vis::LocalBounds(tris.data(), tris.size(), e.center, e.extents)) return false;
    e.tris = std::move(tris);
    if (lib.by_key.count(key) == 0) lib.keys.push_back(key);
    lib.tri_total += e.tris.size();
    lib.by_key[key] = std::move(e);
    return true;
}

// Каталог с .obj. Возвращает число добавленных файлов; каталога нет — 0, и это
// не ошибка: динамика просто молчит, статика работает.
inline size_t LoadDir(Library& lib, const char* dir, float scale, bool yUpToZUp) {
    if (!dir || !*dir) return 0;
    DIR* d = ::opendir(dir);
    if (!d) return 0;
    size_t added = 0;
    std::vector<std::string> names;
    while (struct dirent* ent = ::readdir(d)) {
        const std::string n = ent->d_name;
        if (n.size() < 5) continue;
        if (n.compare(n.size() - 4, 4, ".obj") != 0 && n.compare(n.size() - 4, 4, ".OBJ") != 0)
            continue;
        names.push_back(n);
    }
    ::closedir(d);
    std::sort(names.begin(), names.end());           // детерминированный порядок
    for (const auto& n : names)
        if (AddObj(lib, std::string(dir) + "/" + n, scale, yUpToZUp)) ++added;
    return added;
}

inline void publish_library(std::shared_ptr<const Library> lib) {
    const size_t entries = lib ? lib->keys.size() : 0;
    const size_t tris = lib ? lib->tri_total : 0;
    g_lib_entries.store(entries, std::memory_order_relaxed);
    g_lib_tris.store(tris, std::memory_order_relaxed);
    std::atomic_store_explicit(&g_lib, std::move(lib), std::memory_order_release);
}

inline std::shared_ptr<const Library> library() {
    return std::atomic_load_explicit(&g_lib, std::memory_order_acquire);
}

inline size_t lib_entries() { return g_lib_entries.load(std::memory_order_relaxed); }
inline size_t lib_tris() { return g_lib_tris.load(std::memory_order_relaxed); }

// ---- Динамическое дерево ---------------------------------------------------
// Кусок, из которого собирается дерево. Заполняется коллектором из памяти игры.
struct Piece {
    vis::Vec3 pos{0, 0, 0};     // мировая позиция трансформа
    vis::Quat rot{0, 0, 0, 1};  // мировой поворот (иерархия даёт именно мировой)
    float scale = 1.0f;         // масштаб иерархии (обычно 1)
    std::string name;           // имя GameObject/префаба — по нему ищем меш
};

// Собрать дерево из списка кусков. Чистая геометрия: ни файлов, ни публикации —
// дерево возвращается, а публикует его vis_map (владелец снимка кадра).
// Пустой список — пустое дерево, а не «оставить старое»: разгруженную базу
// нельзя продолжать считать стеной. Возвращает nullptr, если геометриии нет.
inline std::shared_ptr<const vis::Bvh> build(const std::vector<Piece>& pieces,
                                             const Library& lib,
                                             size_t* instanced = nullptr,
                                             size_t* unmatched = nullptr) {
    std::vector<vis::Tri> tris;
    size_t inst = 0, miss = 0;
    for (const auto& p : pieces) {
        const Entry* e = lib.Find(p.name.c_str());
        if (!e || e->tris.empty()) { ++miss; continue; }
        const float s = (p.scale > 1e-3f && p.scale < 1e3f) ? p.scale : 1.0f;
        tris.reserve(tris.size() + e->tris.size());
        for (const auto& t : e->tris)
            tris.push_back(vis::TransformTri(t, p.pos, p.rot, s));
        ++inst;
    }
    if (instanced) *instanced = inst;
    if (unmatched) *unmatched = miss;
    if (tris.empty()) return nullptr;
    auto built = std::make_shared<vis::Bvh>();
    built->Build(std::move(tris));
    if (built->Empty()) return nullptr;
    return built;
}

// Счётчики для меню и диагностики. Заполняет владелец прохода (коллектор в
// game_markers.cpp) через set_counts — сам build() только возвращает дерево.
inline std::atomic<size_t> g_tris{0};
inline std::atomic<size_t> g_pieces{0};
inline std::atomic<size_t> g_unmatched{0};
inline void set_counts(size_t tris, size_t pieces, size_t unmatched) {
    g_tris.store(tris, std::memory_order_relaxed);
    g_pieces.store(pieces, std::memory_order_relaxed);
    g_unmatched.store(unmatched, std::memory_order_relaxed);
}
inline size_t tri_count() { return g_tris.load(std::memory_order_relaxed); }
inline size_t piece_count() { return g_pieces.load(std::memory_order_relaxed); }
inline size_t unmatched_count() { return g_unmatched.load(std::memory_order_relaxed); }

// Строка состояния для меню: сколько кусков стало геометрией и сколько имён не
// совпало с библиотекой (несовпадение — это сигнал «дополни каталог», а не
// ошибка: проверка продолжает работать по статике).
inline const char* status_text() {
    static char s[48];
    const size_t t = tri_count();
    const size_t u = unmatched_count();
    if (t == 0 && u == 0) { std::snprintf(s, sizeof(s), "OBJ --"); return s; }
    if (t >= 1000) std::snprintf(s, sizeof(s), "OBJ %.1fK/%zu", (double)t / 1000.0, u);
    else           std::snprintf(s, sizeof(s), "OBJ %zu/%zu", t, u);
    return s;
}

} // namespace vis_dyn
