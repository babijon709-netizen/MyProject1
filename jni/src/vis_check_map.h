#pragma once
// vis_check_map.h — карта видимости: один раз грузим меш карты, строим BVH и
// дальше отвечаем на вопрос «между камерой и этой точкой тела есть геометрия?».
//
// Ядро обхода — vis_check.h (BVH по треугольникам). Здесь — то, что нужно
// именно нам, внешнему читателю:
//   * фоновая загрузка: меш разбирается в отдельном потоке, кадр оверлея не
//     ждёт его ни на мс (пока дерева нет, проверка молчит — «неизвестно»
//     никогда не гасит визуалы);
//   * кэш дерева на диск (<меш>.bvh): разбор .obj — секунды, а дерево от
//     запуска к запуску не меняется. Дерево можно собрать и на ПК
//     (tools/vis_map/obj2bvh.cpp) и положить на устройство готовым;
//   * гистерезис: одно перекрытие кости — не повод гасить бокс (на границе
//     модели луч мигает), скрытым цель считается после нескольких кадров
//     подряд, а «видна» — сразу;
//   * бюджет лучей на кадр: у проверки есть цена, и она не должна влиять на
//     кадр оверлея даже в толпе.
//
// Чего проверка НЕ знает: динамики. Постройки игроков, двери, турели, чужие
// стены в меше карты не лежат — их никто не экспортировал, они появляются в
// мире в рантайме. Террейн, скалы, деревья, статические строения карты
// перекрывают нормально.
//
// Путь к мешу: <CfgDir>/map.obj по умолчанию, переопределяется боковым файлом
// <CfgDir>/vismap.txt (одна строка — путь), как .mode у режима чтения.

#include "vis_check.h"
#include "config.h"
#include "hud.h"      // ShowToast — о состоянии карты сообщаем тостом с кадра
#include "str.h"      // XS_RU: строки пути шифруются, как и остальные

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sys/stat.h>
#include <time.h>

namespace vis_map {

// ---- Состояние карты -------------------------------------------------------
enum Status {
    VIS_STATUS_OFF     = 0,  // проверка выключена, дерева нет
    VIS_STATUS_LOADING = 1,  // фоновый поток читает/строит
    VIS_STATUS_READY   = 2,  // дерево в памяти, проверка работает
    VIS_STATUS_NOFILE  = 3,  // файла меша нет — проверка молчит
    VIS_STATUS_FAILED  = 4,  // файл есть, но разобрать/построить не вышло
};

// ---- Настройки под экспорт меша --------------------------------------------
// Единицы и ориентация осей экспортированного меша. Unity — Y-up и метры:
// масштаб 1, свап осей выключен. Если после экспорта геометрия «лежит на боку»
// или расходится по размеру с мировыми координатами игры — правится здесь, а
// не в геометрии.
inline constexpr float kMeshScale    = 1.0f;
inline constexpr bool  kMeshYUpToZUp = false;

// ---- Бюджеты ---------------------------------------------------------------
// Точек на цель: голова, грудь, таз, ноги. Больше не нужно — проверка идёт
// «хотя бы одна точка не перекрыта», и этого хватает, чтобы не терять игрока
// за укрытием по грудь.
inline constexpr int kMaxPointsPerBox   = 4;
// Сколько целей за кадр вообще проверяем и сколько лучей на это тратим. Боксов
// в кадре больше 64 не бывает, а луч по BVH — это сотни наносекунд, так что
// потолок стоит только против патологии (гигантский меш + толпа на экране).
inline constexpr int kMaxBoxesPerFrame  = 64;
inline constexpr int kMaxRaysPerFrame   = 256;
// Кадров подряд с перекрытием, прежде чем гасить. 2 = ~33 мс на 60 к/с:
// мигание на границе модели пропадает, а уход за угол виден без задержки.
inline constexpr int kHiddenFramesToHide = 2;

// ---- Состояние (только чтение из кадра) ------------------------------------
inline std::shared_ptr<const vis::Bvh> g_bvh;
inline std::atomic<int>    g_status{VIS_STATUS_OFF};
inline std::atomic<size_t> g_tris{0};
inline std::atomic<bool>   g_busy{false};
// Готовность/ошибку показываем тостом, но тост живёт в потоке кадра —
// фоновый поток только ставит флаг, поднимает его frame_begin().
inline std::atomic<int>    g_toast_pending{0};

// ---- Путь ------------------------------------------------------------------
inline char g_path[256] = {};
inline bool g_path_ready = false;

inline const char* sidecar_path() {
    static std::string s = std::string(CfgDir()) + XS_RU("vismap.txt");
    return s.c_str();
}

inline const char* default_mesh_path() {
    static std::string s = std::string(CfgDir()) + XS_RU("map.obj");
    return s.c_str();
}

inline void set_mesh_path_quiet(const char* path) {
    if (path == nullptr || path[0] == '\0') return;
    std::snprintf(g_path, sizeof(g_path), "%s", path);
    g_path_ready = true;
}

// Читаем боковой файл один раз за запуск (как .mode у режима чтения).
inline void ensure_path() {
    if (g_path_ready) return;
    g_path_ready = true;
    set_mesh_path_quiet(default_mesh_path());
    FILE* f = std::fopen(sidecar_path(), "r");
    if (f == nullptr) return;
    char line[256] = {};
    if (std::fgets(line, sizeof(line), f) != nullptr) {
        for (char* p = line; *p; ++p) if (*p == '\n' || *p == '\r') { *p = '\0'; break; }
        if (line[0] != '\0') set_mesh_path_quiet(line);
    }
    std::fclose(f);
}

inline const char* mesh_path() {
    ensure_path();
    return g_path;
}

// Смена меша: пишем боковой файл и роняем текущее дерево (если поток не занят).
inline void set_mesh_path(const char* path) {
    if (path == nullptr || path[0] == '\0') return;
    ensure_path();
    if (std::strncmp(path, g_path, sizeof(g_path)) == 0) return;
    set_mesh_path_quiet(path);
    FILE* f = std::fopen(sidecar_path(), "w");
    if (f != nullptr) {
        std::fprintf(f, "%s\n", path);
        std::fclose(f);
    }
    std::atomic_store_explicit(&g_bvh, std::shared_ptr<const vis::Bvh>{}, std::memory_order_release);
    g_tris.store(0, std::memory_order_relaxed);
    g_status.store(VIS_STATUS_OFF, std::memory_order_relaxed);
}

inline Status status() { return static_cast<Status>(g_status.load(std::memory_order_relaxed)); }
inline size_t tri_count() { return g_tris.load(std::memory_order_relaxed); }
inline bool ready() { return status() == VIS_STATUS_READY; }

// Строка состояния для меню. ASCII и цифры: это не подпись интерфейса, а
// показание счётчика, и переводить её нечем (как «PLAYER» у дистанции).
inline const char* status_text() {
    static char s[32];
    switch (status()) {
        case VIS_STATUS_READY: {
            const size_t t = tri_count();
            if (t >= 1000) std::snprintf(s, sizeof(s), "VIS %.0fK", (double)t / 1000.0);
            else           std::snprintf(s, sizeof(s), "VIS %zu", t);
            break;
        }
        case VIS_STATUS_LOADING: std::snprintf(s, sizeof(s), "VIS ...");     break;
        case VIS_STATUS_NOFILE:  std::snprintf(s, sizeof(s), "VIS --");      break;
        case VIS_STATUS_FAILED:  std::snprintf(s, sizeof(s), "VIS ERR");     break;
        default:                 std::snprintf(s, sizeof(s), "VIS off");     break;
    }
    return s;
}

// ---- Фоновый поток ---------------------------------------------------------
// Тост только на смену состояния: «нет файла» исторгается один раз, а не
// каждые пять секунд, пока проверка включена, а меша нет.
inline std::atomic<int> g_reported{VIS_STATUS_OFF};
inline void report_status(int st) {
    const int prev = g_reported.exchange(st, std::memory_order_relaxed);
    if (prev != st) g_toast_pending.store(st, std::memory_order_relaxed);
}

inline double now_seconds() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

inline void publish(std::shared_ptr<const vis::Bvh> bvh) {
    if (!bvh) return;
    g_tris.store(bvh->TriCount(), std::memory_order_relaxed);
    std::atomic_store_explicit(&g_bvh, std::move(bvh), std::memory_order_release);
    g_status.store(VIS_STATUS_READY, std::memory_order_relaxed);
}

inline void worker(std::string path) {
    const std::string cache = path + XS_RU(".bvh");
    struct stat so {};
    struct stat sc {};
    const bool have_obj   = ::stat(path.c_str(), &so) == 0 && so.st_size > 0;
    const bool have_cache = ::stat(cache.c_str(), &sc) == 0 && sc.st_size > 0;

    // Кэш годится, если он не старше меша. Меша может не быть вовсе — тогда
    // дерево всё равно грузим: .bvh самодостаточен, его и кладут на устройство.
    if (have_cache && (!have_obj || sc.st_mtime >= so.st_mtime)) {
        auto cached = std::make_shared<vis::Bvh>();
        if (cached->LoadBinary(cache.c_str()) && !cached->Empty()) {
            publish(std::move(cached));
            report_status(VIS_STATUS_READY);
            return;
        }
    }

    if (!have_obj) {
        g_status.store(VIS_STATUS_NOFILE, std::memory_order_relaxed);
        report_status(VIS_STATUS_NOFILE);
        return;
    }

    std::vector<vis::Tri> tris;
    if (!vis::LoadObj(path, tris, kMeshScale, kMeshYUpToZUp) || tris.empty()) {
        g_status.store(VIS_STATUS_FAILED, std::memory_order_relaxed);
        report_status(VIS_STATUS_FAILED);
        return;
    }
    auto built = std::make_shared<vis::Bvh>();
    built->Build(std::move(tris));
    if (built->Empty()) {
        g_status.store(VIS_STATUS_FAILED, std::memory_order_relaxed);
        report_status(VIS_STATUS_FAILED);
        return;
    }
    built->SaveBinary(cache.c_str());   // следующие запуски — одно чтение
    publish(std::move(built));
    report_status(VIS_STATUS_READY);
}

inline void start_load() {
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) return;
    ensure_path();
    std::string path = g_path;
    g_status.store(VIS_STATUS_LOADING, std::memory_order_relaxed);
    std::thread([path]() {
        worker(path);
        g_busy.store(false, std::memory_order_release);
    }).detach();
}

// ---- Кадр ------------------------------------------------------------------
// Зовётся каждый кадр с флагом «проверка нужна» (ESP «Видимость» или аим
// «Только видимых»). Включение поднимает загрузку один раз; выключение дерево
// не выбрасывает — повторное включение мгновенное.
//
// Повтор после отказа — не чаще раза в kRetrySeconds: файл могли положить на
// место уже после первого отказа, но поднимать поток на КАЖДОМ кадре нельзя —
// это и спавн потока в кадре, и тост на каждую попытку.
inline constexpr double kRetrySeconds = 5.0;
inline std::string g_last_path;
inline double      g_last_attempt = -1e9;

inline void request(bool on) {
    if (!on) return;
    ensure_path();
    const Status st = status();
    if (st == VIS_STATUS_READY || st == VIS_STATUS_LOADING) return;
    const double now = now_seconds();
    if (g_last_path == g_path && now - g_last_attempt < kRetrySeconds) return;
    g_last_path = g_path;
    g_last_attempt = now;
    start_load();
}

// Счётчик кадров проверки. Начинается с 1: у новой записи кэша поле frame=0
// означает «ещё не считалась», и при старте с нуля первая же цель получила бы
// ответ из пустой записи, не сделав ни одного луча.
inline uint32_t g_frame = 1;
inline int      g_rays_this_frame = 0;
inline int      g_boxes_this_frame = 0;

struct Hit {
    bool     visible = true;    // последнее принятое решение
    uint8_t  hidden_streak = 0; // кадров подряд с перекрытием
    uint32_t frame = 0;         // кадр, на котором решение посчитано
};

inline std::unordered_map<uint64_t, Hit>& cache() {
    static std::unordered_map<uint64_t, Hit> m;
    return m;
}

// Начало кадра проверки: счётчики бюджета, тосты, чистка кэша целей.
inline void frame_begin() {
    ++g_frame;
    g_rays_this_frame = 0;
    g_boxes_this_frame = 0;

    const int pending = g_toast_pending.exchange(0, std::memory_order_relaxed);
    if (pending == VIS_STATUS_READY) {
        char msg[64];
        std::snprintf(msg, sizeof(msg), "VIS %.0fK", (double)tri_count() / 1000.0);
        ShowToast(msg);
    } else if (pending == VIS_STATUS_NOFILE) {
        ShowToast(XS("Карта видимости не найдена|проверка молчит"));
    } else if (pending == VIS_STATUS_FAILED) {
        ShowToast(XS("Карта видимости не разобрана|проверка молчит"));
    }

    if (cache().size() > 256) cache().clear();
}

// true — цель НЕ перекрыта (или проверка невозможна: дерева нет, бюджет
// кончился, точки невалидны). Неизвестность всегда отдаётся как «видна»:
// молчащая проверка не должна гасить визуалы и выкидывать цель из аима.
inline bool visible(uint64_t id, const float eye[3], const float (*pts)[3], int n) {
    std::shared_ptr<const vis::Bvh> bvh =
        std::atomic_load_explicit(&g_bvh, std::memory_order_acquire);
    if (!bvh || bvh->Empty() || pts == nullptr || n <= 0 || eye == nullptr) return true;

    Hit& hit = cache()[id];
    // Цель уже проверена в этом кадре (FrameBoxes зовётся и оверлеем, и аимом,
    // и пилюлей счётчика) — второй раз лучи не гоняем.
    if (hit.frame == g_frame) return hit.visible;
    if (g_boxes_this_frame >= kMaxBoxesPerFrame || g_rays_this_frame >= kMaxRaysPerFrame)
        return hit.visible;
    hit.frame = g_frame;

    ++g_boxes_this_frame;
    const vis::Vec3 eye_v{eye[0], eye[1], eye[2]};
    bool clear_any = false;
    for (int i = 0; i < n && i < kMaxPointsPerBox; ++i) {
        const vis::Vec3 p{pts[i][0], pts[i][1], pts[i][2]};
        const vis::Vec3 d = p - eye_v;
        const float len = vis::Length(d);
        if (!(len > 0.05f) || !std::isfinite(len)) continue;
        // Отступ от цели: не больше половины метра и не больше четверти самой
        // дистанции — иначе в упор (цель в метре) луч обрезался бы в ноль.
        const float pad = len * 0.25f < 0.5f ? len * 0.25f : 0.5f;
        ++g_rays_this_frame;
        if (!bvh->Occluded(eye_v, p, pad)) { clear_any = true; break; }
    }

    if (clear_any) {
        hit.visible = true;
        hit.hidden_streak = 0;
    } else if (hit.visible) {
        // Погасить можно только после kHiddenFramesToHide кадров подряд.
        if (hit.hidden_streak < 255) ++hit.hidden_streak;
        if (hit.hidden_streak >= kHiddenFramesToHide) hit.visible = false;
    }
    return hit.visible;
}

// Настройка при старте (если меш задан в боковом файле — дерево поднимется при
// первом включении проверки, здесь только читаем путь).
inline void init() { ensure_path(); }

} // namespace vis_map
