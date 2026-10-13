// vis_mesh.cpp — рантайм-сбор мешей игры и проверка видимости по ним.
// Публичное описание — в vis_mesh.h, геометрия запроса — в vis_geom.h.
//
// Порядок работы:
//   1. раскладка нативного Mesh (число вершин, буфер вершин, шаг вершины,
//      число индексов, буфер индексов, ширина индекса) — берётся из
//      game_offsets.h, а если там прочерк, ищется рантаймом: сначала
//      структурный якорь VertexData, иначе полный перебор;
//   2. обход дерева трансформов сцены от корня (от трансформа локального
//      игрока поднимаемся по массиву родителей) с отсечением по расстоянию и
//      пропуском поддеревьев игроков;
//   3. у каждого узла — GameObject, его компоненты, поиск PPtr<Mesh>;
//   4. вершины меша разворачиваются в мировые координаты и вместе с
//      треугольниками уходят в vis::World.
//
// Всё чтение идёт через g_mem (mem_io.h): тот же транспорт и тот же кадровый
// кэш блоков, что у ESP. Крупные буферы (вершины, индексы) читаются одним
// pread — mem_io сам пускает чтение больше блока мимо кэша.

#include "vis_mesh.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include "game_internal.h"
#include "game_offsets_active.h"
#include "vis_geom.h"

namespace vis {
namespace {

// ---- бюджет --------------------------------------------------------------
constexpr float  kRebuildDistance = 25.0F;   // метры от точки сбора
constexpr double kRebuildAge      = 20.0;    // секунд на сборку без движения
constexpr float  kHarvestRadius   = 140.0F;  // метры: радиус сбора вокруг глаза
constexpr int    kMaxNodes        = 3000;    // узлов дерева в одной сборке
constexpr int    kMaxTriangles    = 150000;
constexpr int    kMaxVertices     = 400000;
constexpr int    kNodesPerFrame   = 120;     // узлов дерева за кадр
constexpr int    kMeshPerFrame    = 12;      // узлов, у которых читаем меш, за кадр
constexpr int    kProbePerFrame   = 8;       // кандидатов-мешей на раскладку за кадр
constexpr int    kProbeBudget     = 600;     // чтений за кадр на поиск раскладки
constexpr double kProbeTimeout    = 40.0;    // секунд, после чего поиск бросаем
constexpr float  kCoarseMax       = 25.0F;   // бокс крупнее этого не берём вовсе
constexpr int    kConfirmMeshes   = 2;       // подтверждений раскладки после первого
constexpr size_t kHeadSize        = 0x180;   // сколько байт Mesh читаем на кандидата
constexpr size_t kScratchBytes    = 4u << 20;  // 4 МБ на один буфер вершин/индексов
constexpr uint32_t kMaxVertexCount = 400000;
constexpr uint32_t kMaxIndexCount  = 2000000;
constexpr uint64_t kMaxBuffer      = 64ULL << 20;
// Смещения PPtr<Mesh> внутри компонента: Component занимает 0x20, дальше поля
// наследника. Перебираем с запасом — лишняя проверка стоит одно чтение блока,
// а найденный меш проверяется по содержимому, так что ложного срабатывания нет.
constexpr int kFields[] = {0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x70};
// Нативный GameObject: те же слоты, что в game.cpp.
constexpr uint64_t kGoCount  = 0x30;
constexpr uint64_t kGoStride = 0x10;

// ---- мелкая арифметика ---------------------------------------------------
static inline uint32_t ld16(const uint8_t* p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}
static inline uint32_t ld32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static inline uint64_t ld64(const uint8_t* p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

// Те же формулы, что в game.cpp (rotate_vector / multiply_quaternion):
// единичный кватернион и порядок «родитель * локальный».
static Vec3 qrot(const Vec4& q, const Vec3& v) {
    const Vec3 u = {q.x, q.y, q.z};
    const Vec3 t = {2.0F * (u.y * v.z - u.z * v.y), 2.0F * (u.z * v.x - u.x * v.z),
                    2.0F * (u.x * v.y - u.y * v.x)};
    return {v.x + q.w * t.x + (u.y * t.z - u.z * t.y),
            v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
            v.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
}

static Vec4 qmul(const Vec4& a, const Vec4& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

static bool qnorm(Vec4& q) {
    const float l2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (!std::isfinite(l2) || l2 < 1e-6F || l2 > 1e6F) return false;
    const float s = 1.0F / sqrtf(l2);
    q.x *= s;
    q.y *= s;
    q.z *= s;
    q.w *= s;
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
}

struct Pose {
    Vec3 pos{};
    Vec4 rot{0, 0, 0, 1};
    Vec3 scale{1, 1, 1};
};

// world = parentWorld * localTRS (Unity: сначала масштаб, потом поворот, потом
// перенос) — то же, что делает read_transform_world_trs в game.cpp.
static Pose compose(const Pose& p, const Matrix34& m) {
    const Vec3 lt = {m.translation.x, m.translation.y, m.translation.z};
    const Vec3 ls = {m.scale.x, m.scale.y, m.scale.z};
    const Vec3 scaled = {lt.x * p.scale.x, lt.y * p.scale.y, lt.z * p.scale.z};
    const Vec3 turned = qrot(p.rot, scaled);
    Pose out;
    out.pos = {p.pos.x + turned.x, p.pos.y + turned.y, p.pos.z + turned.z};
    out.rot = qmul(p.rot, m.rotation);
    if (!qnorm(out.rot)) out.rot = {0, 0, 0, 1};
    out.scale = {p.scale.x * ls.x, p.scale.y * ls.y, p.scale.z * ls.z};
    return out;
}

static bool matrix_ok(const Matrix34& m) {
    if (!std::isfinite(m.translation.x) || !std::isfinite(m.translation.y) ||
        !std::isfinite(m.translation.z))
        return false;
    if (fabsf(m.translation.x) > 1e6F || fabsf(m.translation.y) > 1e6F ||
        fabsf(m.translation.z) > 1e6F)
        return false;
    const float l2 = m.rotation.x * m.rotation.x + m.rotation.y * m.rotation.y +
                     m.rotation.z * m.rotation.z + m.rotation.w * m.rotation.w;
    if (!(l2 > 0.25F) || l2 > 4.0F) return false;
    return std::isfinite(m.scale.x) && std::isfinite(m.scale.y) && std::isfinite(m.scale.z) &&
           fabsf(m.scale.x) > 1e-4F && fabsf(m.scale.x) < 1e4F;
}

static float dist3(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    const float d2 = dx * dx + dy * dy + dz * dz;
    return std::isfinite(d2) ? sqrtf(d2) : 1e30F;
}

// ---- раскладка нативного Mesh -------------------------------------------
struct Layout {
    int vcount = -1, vptr = -1, stride = 0;
    int icount = -1, iptr = -1, width = 0;
    int field = -1;   // смещение PPtr<Mesh> в компоненте (-1 = любой слот)
    int via = 0;      // 0 из офсетов, 1 структурный якорь, 2 перебор
    bool valid = false;
};

struct MeshInfo {
    uint32_t nv = 0, ni = 0;
    uint64_t vp = 0, ip = 0;
    int width = 0, stride = 0;
};

// Прочитать поля меша по раскладке. Здесь же дешёвая проверка «похоже на меш»:
// числа в разумных пределах, указатели живые, буферы не гигантские.
static bool mesh_info(uint64_t mesh, const Layout& L, MeshInfo& out) {
    if (!valid_obj(mesh) || !L.valid) return false;
    out.nv = rd<uint32_t>(mesh + (uint64_t)L.vcount);
    out.ni = rd<uint32_t>(mesh + (uint64_t)L.icount);
    out.vp = rd_ptr(mesh + (uint64_t)L.vptr);
    out.ip = rd_ptr(mesh + (uint64_t)L.iptr);
    out.width = L.width;
    out.stride = L.stride;
    if (out.nv < 3 || out.nv > kMaxVertexCount) return false;
    if (out.ni < 3 || out.ni > kMaxIndexCount || (out.ni % 3) != 0) return false;
    if (!valid_obj(out.vp) || !valid_obj(out.ip)) return false;
    if ((uint64_t)out.nv * (uint64_t)out.stride > kMaxBuffer) return false;
    if ((uint64_t)out.ni * (uint64_t)(out.width / 8) > kMaxBuffer) return false;
    return true;
}

// Все прочитанные индексы обязаны быть меньше числа вершин. Это и есть основной
// отсев мусора: по случайному указателю «индексы» — что угодно, и почти всегда
// значение выходит за границы буфера вершин.
static bool index_sample(uint64_t ip, uint32_t ni, int width, uint8_t* buf, size_t buf_size,
                         uint32_t& out_max, uint32_t& out_count) {
    const size_t ws = (size_t)(width / 8);
    size_t want = (size_t)(ni < 256 ? ni : 256) * ws;
    if (want < 48 || want > buf_size) return false;
    if (!rd_buf(ip, buf, want)) return false;
    uint32_t maxidx = 0;
    const uint32_t n = (uint32_t)(want / ws);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t v = width == 16 ? ld16(buf + (size_t)i * 2) : ld32(buf + (size_t)i * 4);
        if (v > maxidx) maxidx = v;
    }
    out_max = maxidx;
    out_count = n;
    return true;
}

// Вершины: читаем начало буфера, считаем бокс, и главное — связность. Настоящий
// меш — поверхность: среднее ребро много меньше габарита. У неправильного шага
// «треугольники» соединяют случайные точки облака (нормали, UV, касательные), и
// среднее ребро выходит сравнимым с габаритом — по этому признаку шаг и берётся.
static bool vertex_probe(uint64_t vp, uint32_t nv, int stride, const uint8_t* ibuf,
                         uint32_t idx_count, int width, float& out_ratio) {
    if ((uint64_t)nv * (uint64_t)stride < 128) return false;
    uint8_t vbuf[4096];
    const size_t want = std::min<size_t>(sizeof(vbuf), (size_t)nv * (size_t)stride);
    if (want < 128) return false;
    if (!rd_buf(vp, vbuf, want)) return false;
    const uint32_t k = (uint32_t)(want / (uint32_t)stride);
    if (k < 8) return false;
    float mn[3] = {1e30F, 1e30F, 1e30F};
    float mx[3] = {-1e30F, -1e30F, -1e30F};
    for (uint32_t i = 0; i < k; ++i) {
        float v[3];
        memcpy(v, vbuf + (size_t)i * (size_t)stride, sizeof(v));
        for (int c = 0; c < 3; ++c) {
            if (!std::isfinite(v[c]) || fabsf(v[c]) > 1e5F) return false;
            if (v[c] < mn[c]) mn[c] = v[c];
            if (v[c] > mx[c]) mx[c] = v[c];
        }
    }
    const float ext = std::max(std::max(mx[0] - mn[0], mx[1] - mn[1]), mx[2] - mn[2]);
    if (!(ext >= 0.01F) || ext > 5000.0F) return false;

    float sum = 0.0F;
    uint32_t edges = 0;
    const uint32_t tris = idx_count / 3;
    for (uint32_t t = 0; t < tris && edges < 96; ++t) {
        const size_t base = (size_t)t * (size_t)(width / 8) * 3;
        const uint32_t ids[3] = {width == 16 ? ld16(ibuf + base) : ld32(ibuf + base),
                                 width == 16 ? ld16(ibuf + base + 2) : ld32(ibuf + base + 4),
                                 width == 16 ? ld16(ibuf + base + 4) : ld32(ibuf + base + 8)};
        if (ids[0] >= k || ids[1] >= k || ids[2] >= k) continue;
        float p[3][3];
        for (int j = 0; j < 3; ++j)
            memcpy(p[j], vbuf + (size_t)ids[j] * (size_t)stride, sizeof(p[j]));
        for (int e = 0; e < 3; ++e) {
            const float* u = p[e];
            const float* w = p[(e + 1) % 3];
            const float dx = u[0] - w[0], dy = u[1] - w[1], dz = u[2] - w[2];
            sum += sqrtf(dx * dx + dy * dy + dz * dz);
            ++edges;
        }
    }
    if (edges < 24) return false;
    out_ratio = (sum / (float)edges) / ext;
    return true;
}

// Структурный якорь: VertexData.m_Channels[0] (вершины: stream 0, offset 0,
// размерность 3) и m_Channels[1] (нормали: stream 0, offset 8..64, размерность
// 3) лежат рядом. Дальше по ним же идут массив потоков (4 × 16 байт, в
// StreamInfo[0] лежит шаг вершины), число вершин и указатель на данные.
static bool find_vertex_data(const uint8_t* head, int& vcount, int& vptr, int& stride) {
    for (int off = 0x10; off + 0x78 <= (int)kHeadSize; off += 4) {
        const uint32_t c0 = ld32(head + off);
        const uint32_t c1 = ld32(head + off + 4);
        if ((c0 >> 24) != 3 || (c0 & 0xFF) != 0 || ((c0 >> 8) & 0xFF) != 0) continue;
        if ((c1 >> 24) != 3 || (c1 & 0xFF) != 0) continue;
        const uint32_t normal_offset = (c1 >> 8) & 0xFF;
        if (normal_offset < 8 || normal_offset > 64) continue;
        const int streams = off + 32;                   // 8 каналов × 4 байта
        const uint32_t st = ld32(head + streams + 8);   // StreamInfo[0].stride
        if (st < normal_offset + 12 || st > 256) continue;
        const int vc = streams + 64;                    // 4 потока × 16 байт
        const uint32_t nv = ld32(head + vc);
        if (nv < 3 || nv > kMaxVertexCount) continue;
        if ((uint64_t)nv * (uint64_t)st > kMaxBuffer) continue;
        const int vp = vc + 8;                          // число потоков + выравнивание
        if (!valid_obj(ld64(head + vp))) continue;
        vcount = vc;
        vptr = vp;
        stride = (int)st;
        return true;
    }
    return false;
}

// Годен ли меш под эту раскладку: используется и при подтверждении раскладки,
// и при обходе сцены, и при поиске PPtr<Mesh> в компоненте.
static bool mesh_ok(uint64_t mesh, const Layout& L) {
    MeshInfo info;
    if (!mesh_info(mesh, L, info)) return false;
    uint8_t ibuf[1024];
    uint32_t maxidx = 0, count = 0;
    if (!index_sample(info.ip, info.ni, info.width, ibuf, sizeof(ibuf), maxidx, count))
        return false;
    if (maxidx >= info.nv) return false;
    float ratio = 1e30F;
    return vertex_probe(info.vp, info.nv, info.stride, ibuf, count, info.width, ratio);
}

// Полный перебор: ищем буфер индексов, по его максимуму — число вершин, потом
// буфер вершин и шаг (шаг — с минимальным отношением «среднее ребро / габарит»).
static bool probe_brute(const uint8_t* head, Layout& out, int& budget) {
    uint8_t ibuf[1024];
    for (int ic = 0x10; ic + 4 <= (int)kHeadSize && budget > 0; ic += 4) {
        const uint32_t ni = ld32(head + ic);
        if (ni < 900 || ni > kMaxIndexCount || (ni % 3) != 0) continue;
        for (int ip = 0x10; ip + 8 <= (int)kHeadSize && budget > 0; ip += 8) {
            const uint64_t p = ld64(head + ip);
            if (!valid_obj(p)) continue;
            for (int width = 16; width <= 32; width += 16) {
                if (budget-- <= 0) return false;
                uint32_t maxidx = 0, count = 0;
                if (!index_sample(p, ni, width, ibuf, sizeof(ibuf), maxidx, count)) continue;
                if (maxidx < ni / 12 || maxidx > ni * 3 / 2 || maxidx > kMaxVertexCount) continue;
                int best_vc = -1;
                uint32_t best_nv = 0;
                for (int vc = 0x10; vc + 4 <= (int)kHeadSize; vc += 4) {
                    const uint32_t nv = ld32(head + vc);
                    if (nv < maxidx + 1 || nv > kMaxVertexCount) continue;
                    if (best_vc < 0 || nv < best_nv) {
                        best_vc = vc;
                        best_nv = nv;
                    }
                }
                if (best_vc < 0) continue;
                float best_ratio = 1e30F;
                int best_vp = -1, best_stride = 0;
                for (int vp = 0x10; vp + 8 <= (int)kHeadSize; vp += 8) {
                    const uint64_t q = ld64(head + vp);
                    if (!valid_obj(q) || q == p) continue;
                    for (int stride = 12; stride <= 64; stride += 4) {
                        float ratio = 1e30F;
                        if (!vertex_probe(q, best_nv, stride, ibuf, count, width, ratio))
                            continue;
                        if (ratio >= best_ratio) continue;
                        best_ratio = ratio;
                        best_vp = vp;
                        best_stride = stride;
                    }
                }
                if (best_vp < 0 || best_ratio > 0.45F) continue;
                out.vcount = best_vc;
                out.vptr = best_vp;
                out.stride = best_stride;
                out.icount = ic;
                out.iptr = ip;
                out.width = width;
                out.via = 2;
                out.valid = true;
                return true;
            }
        }
    }
    return false;
}

// Попытка найти раскладку на одном меше: сначала структурный якорь, потом
// перебор. Обе ветки заканчиваются одной и той же проверкой вершин.
static bool probe_one(uint64_t mesh, Layout& out, int& budget) {
    uint8_t head[kHeadSize];
    if (!rd_buf(mesh, head, sizeof(head))) return false;

    int vc = -1, vp = -1, stride = 0;
    if (find_vertex_data(head, vc, vp, stride)) {
        const uint32_t nv = ld32(head + vc);
        const uint64_t vptr = ld64(head + vp);
        uint8_t ibuf[1024];
        // Буфер индексов ищем по уже известному числу вершин: каждый
        // прочитанный индекс обязан быть меньше nv.
        for (int ic = 0x10; ic + 4 <= (int)kHeadSize && budget > 0; ic += 4) {
            const uint32_t ni = ld32(head + ic);
            if (ni < 12 || ni > kMaxIndexCount || (ni % 3) != 0) continue;
            for (int ip = 0x10; ip + 8 <= (int)kHeadSize && budget > 0; ip += 8) {
                const uint64_t p = ld64(head + ip);
                if (!valid_obj(p)) continue;
                for (int width = 16; width <= 32; width += 16) {
                    if (budget-- <= 0) return false;
                    uint32_t maxidx = 0, count = 0;
                    if (!index_sample(p, ni, width, ibuf, sizeof(ibuf), maxidx, count))
                        continue;
                    if (maxidx >= nv) continue;
                    float ratio = 1e30F;
                    if (!vertex_probe(vptr, nv, stride, ibuf, count, width, ratio)) continue;
                    if (ratio > 0.45F) continue;
                    out.vcount = vc;
                    out.vptr = vp;
                    out.stride = stride;
                    out.icount = ic;
                    out.iptr = ip;
                    out.width = width;
                    out.via = 1;
                    out.valid = true;
                    return true;
                }
            }
        }
    }
    return probe_brute(head, out, budget);
}

// ---- состояние -----------------------------------------------------------
struct Scan {
    bool active = false;
    int phase = 0;   // 0 обход дерева, 1 чтение мешей, 2 сборка сетки
    Vec3 origin{};
    double started = 0.0;
    uint64_t entry = 0;
    uint64_t hier_data = 0, matrices = 0, indices = 0;
    std::vector<uint64_t> tf;
    std::vector<Pose> pose;
    std::vector<uint32_t> near_nodes;   // узлы в радиусе сбора
    std::vector<uint64_t> exclude;      // трансформы поддеревьев игроков
    size_t cursor = 0;
    size_t near_cursor = 0;
    int scanned = 0, meshes = 0, coarse = 0;
    World world;
};

struct Probe {
    bool active = false;
    int phase = 0;   // 1 поиск, 2 подтверждение
    double started = 0.0;
    std::vector<uint64_t> meshes;
    size_t cursor = 0;
    Layout found;
    int confirmed = 0;
};

bool g_enabled = false;
World g_world;
Scan g_scan;
Probe g_probe;
Layout g_layout;
int g_layout_state = 0;   // 0 не искали, 1 ищем, 2 есть, 3 нет
Stats g_stats;
double g_built_at = 0.0;
std::vector<uint8_t> g_scratch;

// Массив указателей Transform, параллельный массивам матриц и родителей: без
// него до корня сцены не подняться (родитель в иерархии — индекс, а не
// указатель). Ищется по известному трансформу: base[slot] — массив, у которого
// arr[index] обязан быть этим трансформом.
uint64_t g_tf_base = 0;
int g_tf_slot = -1;
uint64_t g_tf_data = 0;

uint64_t transform_by_index(int32_t index) {
    if (g_tf_slot < 0 || index < 0) return 0;
    return rd_ptr(rd_ptr(g_tf_base + (uint64_t)g_tf_slot) + (uint64_t)index * 8);
}

void find_transform_array(uint64_t hier_data, uint64_t known_transform, int32_t known_index) {
    g_tf_slot = -1;
    g_tf_data = hier_data;
    // Три толкования: сам блок данных иерархии, указатель из его слота матриц
    // (раскладка с разыменованием) и ещё один разыменованный шаг.
    const uint64_t direct = rd_ptr(hier_data + go_active::TRANSFORM_HIERARCHY_MATRICES);
    const uint64_t bases[3] = {hier_data, direct, rd_ptr(direct)};
    for (uint64_t base : bases) {
        if (!valid_obj(base)) continue;
        for (int slot = 0; slot <= 0x40; slot += 8) {
            const uint64_t arr = rd_ptr(base + (uint64_t)slot);
            if (!valid_obj(arr)) continue;
            if (rd_ptr(arr + (uint64_t)known_index * 8) != known_transform) continue;
            g_tf_base = base;
            g_tf_slot = slot;
            return;
        }
    }
}

// Компоненты GameObject. Возвращает число годных слотов.
int object_components(uint64_t gameobject, uint64_t* out, int max_components) {
    if (!valid_obj(gameobject)) return 0;
    const uint64_t pairs = rd_ptr(gameobject + go_active::GAMEOBJECT_COMPONENT_ARRAY);
    if (!valid_obj(pairs)) return 0;
    const int32_t count = rd<int32_t>(gameobject + kGoCount);
    if (count <= 0 || count > 32) return 0;
    const int n = count < max_components ? count : max_components;
    int found = 0;
    for (int i = 0; i < n; ++i) {
        const uint64_t c = rd_ptr(pairs + (uint64_t)i * kGoStride + go_active::COMPONENT_PAIR_PTR);
        if (!valid_obj(c) || c == gameobject) continue;
        if (rd_ptr(c + go_active::COMPONENT_GAMEOBJECT) != gameobject) continue;  // чужой слот
        out[found++] = c;
    }
    return found;
}

// Меш из компонентов GameObject.
bool mesh_in_object(uint64_t gameobject, const Layout& L, uint64_t& out_mesh, int& out_field) {
    uint64_t comps[16];
    const int n = object_components(gameobject, comps, 16);
    uint8_t block[128];
    for (int i = 0; i < n; ++i) {
        if (!rd_buf(comps[i], block, sizeof(block))) continue;
        for (int f = 0; f < (int)(sizeof(kFields) / sizeof(kFields[0])); ++f) {
            if (L.field >= 0 && L.field != kFields[f]) continue;
            const uint64_t m = memio::untag(ld64(block + kFields[f]));
            if (!valid_obj(m)) continue;
            if (!mesh_ok(m, L)) continue;
            out_mesh = m;
            out_field = kFields[f];
            return true;
        }
    }
    return false;
}

// Прочитать один меш в мировые координаты и положить в World.
bool add_mesh_geometry(uint64_t mesh, const Layout& L, const Pose& pose, World& world,
                       int& coarse_nodes) {
    MeshInfo info;
    if (!mesh_info(mesh, L, info)) return false;
    if (g_scratch.size() < kScratchBytes) g_scratch.resize(kScratchBytes);
    uint8_t* raw = g_scratch.data();
    const size_t isize = (size_t)info.ni * (size_t)(info.width / 8);

    // Крупный меш в бюджет треугольников не лезет — берём только его бокс, и то
    // если он не огромный: AABB рельефа в двести метров перекрыл бы полкарты.
    const bool coarse = info.nv > 20000 || info.ni > 60000 || isize > kScratchBytes ||
                        world.triangle_count() + info.ni / 3 > (size_t)kMaxTriangles ||
                        world.vertex_count() + info.nv > (size_t)kMaxVertices;
    uint32_t take = info.nv;
    if ((size_t)take * (size_t)info.stride > kScratchBytes)
        take = (uint32_t)(kScratchBytes / (size_t)info.stride);
    if (coarse && take > 4096) take = 4096;
    if (take < 3) return false;
    const size_t vread = (size_t)take * (size_t)info.stride;
    if (!rd_buf(info.vp, raw, vread)) return false;

    std::vector<Vec3> verts;
    verts.reserve(take);
    float mn[3] = {1e30F, 1e30F, 1e30F};
    float mx[3] = {-1e30F, -1e30F, -1e30F};
    for (uint32_t i = 0; i < take; ++i) {
        float v[3];
        memcpy(v, raw + (size_t)i * (size_t)info.stride, sizeof(v));
        if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) return false;
        const Vec3 local = {v[0] * pose.scale.x, v[1] * pose.scale.y, v[2] * pose.scale.z};
        const Vec3 turned = qrot(pose.rot, local);
        const Vec3 p = {pose.pos.x + turned.x, pose.pos.y + turned.y, pose.pos.z + turned.z};
        if (!vec3_is_finite(p)) return false;
        verts.push_back(p);
        const float c[3] = {p.x, p.y, p.z};
        for (int a = 0; a < 3; ++a) {
            if (c[a] < mn[a]) mn[a] = c[a];
            if (c[a] > mx[a]) mx[a] = c[a];
        }
    }
    const float ext = std::max(std::max(mx[0] - mn[0], mx[1] - mn[1]), mx[2] - mn[2]);
    if (!(ext >= 0.05F)) return false;
    if (coarse) {
        if (ext > kCoarseMax) return false;
        if (world.add_box(mn, mx)) {
            ++coarse_nodes;
            return true;
        }
        return false;
    }
    std::vector<uint32_t> idx(info.ni);
    if (!rd_buf(info.ip, raw, isize)) return false;
    for (uint32_t i = 0; i < info.ni; ++i)
        idx[i] = info.width == 16 ? ld16(raw + (size_t)i * 2) : ld32(raw + (size_t)i * 4);
    return world.add_mesh(mn, mx, verts.data(), verts.size(), idx.data(), idx.size());
}

// ---- обход сцены ---------------------------------------------------------
bool start_scan(const Vec3& eye, uint64_t entry, const std::vector<uint64_t>& players,
                double now) {
    Scan& s = g_scan;
    s = Scan{};
    uint64_t matrices = 0, indices = 0;
    int32_t index = -1;
    if (!valid_obj(entry) || !transform_hierarchy_arrays(entry, matrices, indices, index))
        return false;
    const uint64_t hier = rd_ptr(entry + go_active::TRANSFORM_HIERARCHY_DATA);
    if (!valid_obj(hier)) return false;

    // Подъём к корню: родитель в иерархии — индекс, поэтому нужен параллельный
    // массив указателей Transform (ищется по самому entry).
    if (g_tf_data != hier || g_tf_slot == -1) find_transform_array(hier, entry, index);

    int32_t root = index;
    for (int d = 0; d < 128; ++d) {
        int32_t parent = -1;
        if (!rd_exact(indices + (uint64_t)root * 4, parent)) break;
        if (parent < 0 || parent > 100000 || parent == root) break;
        root = parent;
    }
    // Массива указателей может не быть (другая раскладка иерархии). Тогда до
    // корня не подняться, а обход поддерева самого игрока бессмыслен: оно целиком
    // в исключениях. Отказываем честно — диагностика покажет «нет входа».
    uint64_t root_tf = (root == index) ? entry : transform_by_index(root);
    if (!valid_obj(root_tf)) return false;

    Matrix34 m{};
    if (!rd_exact(matrices + (uint64_t)root * sizeof(Matrix34), m) || !matrix_ok(m)) {
        // Матрица корня не читается (индекс есть, массива уже нет — объект
        // успели удалить): обходим от самого игрока, раз уж другого входа нет.
        if (!rd_exact(matrices + (uint64_t)index * sizeof(Matrix34), m) || !matrix_ok(m))
            return false;
        root_tf = entry;
        root = index;
    }
    Pose base;
    base.pos = {m.translation.x, m.translation.y, m.translation.z};
    base.rot = m.rotation;
    if (!qnorm(base.rot)) return false;
    base.scale = {m.scale.x, m.scale.y, m.scale.z};

    s.active = true;
    s.phase = 0;
    s.origin = eye;
    s.started = now;
    s.entry = entry;
    s.hier_data = hier;
    s.matrices = matrices;
    s.indices = indices;
    s.tf.push_back(root_tf);
    s.pose.push_back(base);
    for (uint64_t player : players) {
        const uint64_t t = rd_ptr(rd_ptr(player + go_active::PLAYER_TRANSFORM) +
                                  go_active::MANAGED_CACHED_PTR);
        if (valid_obj(t)) s.exclude.push_back(t);
    }
    // Локальный игрок тоже игрок: его тело едет вместе с камерой, и baked-меш
    // торса перекрыл бы полэкрана. Исключение, впрочем, не нужно, если вход в
    // сцену — он сам (корень поднимается из него же): иначе обход нашёл бы
    // только его поддерево, да и то пустым.
    if (root_tf != entry) s.exclude.push_back(entry);
    return true;
}

bool is_excluded(const Scan& s, uint64_t transform) {
    for (uint64_t t : s.exclude)
        if (t == transform) return true;
    return false;
}

// Обход в ширину, но дети узла уходят в очередь БЛИЖНИМИ ПЕРВЫМИ. Порядок
// массива детей произвольный, а предел kMaxNodes на дереве в несколько десятков
// тысяч узлов иначе оставил бы первые попавшиеся три тысячи — например, одну
// группу рельефа и ни одной постройки. Матрица ребёнка читается здесь же (её
// всё равно нужно для мировой позы), так что сортировка стоит только сравнений.
void scan_tree(Scan& s, int budget) {
    for (int done = 0; done < budget && s.cursor < s.tf.size(); ++done) {
        const size_t at = s.cursor++;
        const uint64_t node = s.tf[at];
        if (!valid_obj(node)) continue;
        const int32_t count = rd<int32_t>(node + go_active::TRANSFORM_CHILD_COUNT);
        if (count <= 0 || count > 128) continue;
        const uint64_t array = rd_ptr(node + go_active::TRANSFORM_CHILDREN_ARRAY);
        if (!valid_obj(array)) continue;
        uint64_t kids[128];
        if (!rd_buf(array, kids, (size_t)count * sizeof(uint64_t))) continue;

        struct Kid {
            uint64_t tf;
            int32_t index;
            Pose pose;
            float distance;
        };
        Kid pending[128];
        int pending_count = 0;
        for (int i = 0; i < count; ++i) {
            const uint64_t child = memio::untag(kids[i]);
            if (!valid_obj(child) || child == node) continue;
            if (is_excluded(s, child)) continue;
            const uint64_t data = rd_ptr(child + go_active::TRANSFORM_HIERARCHY_DATA);
            const int32_t index = rd<int32_t>(child + go_active::TRANSFORM_HIERARCHY_INDEX);
            if (data != s.hier_data || index < 0 || index > 100000) continue;
            Matrix34 m{};
            if (!rd_exact(s.matrices + (uint64_t)index * sizeof(Matrix34), m)) continue;
            if (!matrix_ok(m)) continue;
            const Pose p = compose(s.pose[at], m);
            if (!vec3_is_finite(p.pos)) continue;
            Kid& k = pending[pending_count++];
            k.tf = child;
            k.index = index;
            k.pose = p;
            k.distance = dist3(p.pos, s.origin);
        }
        std::sort(pending, pending + pending_count,
                  [](const Kid& a, const Kid& b) { return a.distance < b.distance; });
        for (int i = 0; i < pending_count; ++i) {
            if (s.tf.size() >= (size_t)kMaxNodes) return;
            s.tf.push_back(pending[i].tf);
            s.pose.push_back(pending[i].pose);
            ++s.scanned;
            if (pending[i].distance <= kHarvestRadius)
                s.near_nodes.push_back((uint32_t)(s.tf.size() - 1));
        }
    }
}

void scan_meshes(Scan& s, int budget) {
    for (int done = 0; done < budget && s.near_cursor < s.near_nodes.size(); ++done) {
        const uint32_t at = s.near_nodes[s.near_cursor++];
        if (at >= s.tf.size()) continue;
        const uint64_t go = rd_ptr(s.tf[at] + go_active::COMPONENT_GAMEOBJECT);
        if (!valid_obj(go)) continue;
        uint64_t mesh = 0;
        int field = -1;
        if (!mesh_in_object(go, g_layout, mesh, field)) continue;
        if (add_mesh_geometry(mesh, g_layout, s.pose[at], s.world, s.coarse)) ++s.meshes;
    }
}

// ---- поиск раскладки -----------------------------------------------------
void probe_collect(uint64_t entry, double now) {
    g_probe = Probe{};
    std::vector<uint64_t> nodes;
    collect_transform_subtree(entry, nodes, 128);
    for (uint64_t node : nodes) {
        const uint64_t go = rd_ptr(node + go_active::COMPONENT_GAMEOBJECT);
        uint64_t comps[16];
        const int n = object_components(go, comps, 16);
        uint8_t block[128];
        for (int i = 0; i < n && g_probe.meshes.size() < 256; ++i) {
            if (!rd_buf(comps[i], block, sizeof(block))) continue;
            for (int f = 0; f < (int)(sizeof(kFields) / sizeof(kFields[0])); ++f) {
                const uint64_t m = memio::untag(ld64(block + kFields[f]));
                if (valid_obj(m)) g_probe.meshes.push_back(m);
            }
        }
    }
    if (g_probe.meshes.empty()) {
        g_layout_state = 3;
        return;
    }
    g_probe.active = true;
    g_probe.phase = 1;
    g_probe.started = now;
}

bool layout_from_offsets(Layout& out) {
    if (go_active::MESH_VERTEX_COUNT < 0 || go_active::MESH_VERTEX_PTR < 0 ||
        go_active::MESH_VERTEX_STRIDE < 0 || go_active::MESH_INDEX_COUNT < 0 ||
        go_active::MESH_INDEX_PTR < 0 || go_active::MESH_INDEX_WIDTH < 0)
        return false;
    out.vcount = go_active::MESH_VERTEX_COUNT;
    out.vptr = go_active::MESH_VERTEX_PTR;
    out.stride = go_active::MESH_VERTEX_STRIDE;
    out.icount = go_active::MESH_INDEX_COUNT;
    out.iptr = go_active::MESH_INDEX_PTR;
    out.width = (go_active::MESH_INDEX_WIDTH == 32) ? 32 : 16;
    out.field = go_active::COMPONENT_MESH_PTR;
    out.via = 0;
    out.valid = true;
    return true;
}

void layout_tick(uint64_t entry, double now) {
    if (g_layout_state == 0) {
        Layout seeded;
        if (layout_from_offsets(seeded)) {
            g_layout = seeded;
            g_layout_state = 2;
            return;
        }
        g_layout_state = 1;
        probe_collect(entry, now);
        return;
    }
    if (g_layout_state != 1 || !g_probe.active) return;

    // Затянувшийся поиск — не повод висеть: сцена за это время сменится.
    if (now - g_probe.started > kProbeTimeout) {
        g_layout_state = 3;
        g_probe.active = false;
        return;
    }

    int budget = kProbeBudget;
    for (int done = 0; done < kProbePerFrame && g_probe.cursor < g_probe.meshes.size(); ++done) {
        const uint64_t mesh = g_probe.meshes[g_probe.cursor++];
        if (g_probe.phase == 1) {
            Layout found;
            if (probe_one(mesh, found, budget)) {
                g_probe.found = found;
                g_probe.phase = 2;
                g_probe.confirmed = 1;
            }
        } else if (mesh_ok(mesh, g_probe.found)) {
            if (++g_probe.confirmed >= kConfirmMeshes + 1) {
                g_layout = g_probe.found;
                g_layout_state = 2;
                g_probe.active = false;
                return;
            }
        }
    }
    if (g_probe.cursor >= g_probe.meshes.size()) {
        // Кандидаты кончились: если раскладка нашлась, берём её и без нужного
        // числа подтверждений — иначе следующий шанс будет только после смены
        // мира, а толку от молчащей фичи ноль.
        if (g_probe.phase == 2) {
            g_layout = g_probe.found;
            g_layout_state = 2;
        } else {
            g_layout_state = 3;
        }
        g_probe.active = false;
    }
}

}  // namespace

// ---- публичное API -------------------------------------------------------

void set_enabled(bool on) {
    if (g_enabled == on) return;
    g_enabled = on;
    if (!on) {
        g_world.clear();
        g_scan = Scan{};
        g_stats = Stats{};
    }
}

bool enabled() { return g_enabled; }

void reset() {
    g_world.clear();
    g_scan = Scan{};
    g_probe = Probe{};
    g_layout = Layout{};
    g_layout_state = 0;
    g_stats = Stats{};
    g_tf_base = 0;
    g_tf_slot = -1;
    g_tf_data = 0;
    g_built_at = 0.0;
}

void frame_tick(const Vec3& eye, uint64_t entry_transform,
                const std::vector<uint64_t>& players, double now) {
    g_stats = Stats{};
    if (!g_enabled) return;
    if (g_pid <= 0 || !valid_obj(entry_transform) || !vec3_is_finite(eye)) {
        g_stats.state = 6;
        return;
    }

    layout_tick(entry_transform, now);
    if (g_layout_state == 1) {
        g_stats.state = 1;
        g_stats.via = g_probe.phase == 2 ? g_probe.found.via : 0;
        return;
    }
    if (g_layout_state != 2) {
        // Раскладку не нашли: либо движок другой, либо CPU-копии мешей
        // выгружены (Read/Write выключен в импортере). Молчим и не мешаем —
        // незнание не считается препятствием.
        g_stats.state = 5;
        return;
    }
    g_stats.vcount = g_layout.vcount;
    g_stats.vptr = g_layout.vptr;
    g_stats.stride = g_layout.stride;
    g_stats.icount = g_layout.icount;
    g_stats.iptr = g_layout.iptr;
    g_stats.width = g_layout.width;
    g_stats.field = g_layout.field;
    g_stats.via = g_layout.via;

    const bool moved = g_world.ready() && dist3(eye, g_scan.origin) > kRebuildDistance;
    const bool stale = (now - g_built_at) > kRebuildAge;
    const bool entry_changed = g_scan.entry != 0 && g_scan.entry != entry_transform;
    if (!g_scan.active && (moved || stale || entry_changed || !g_world.ready())) {
        g_built_at = now;
        if (!start_scan(eye, entry_transform, players, now)) {
            g_stats.state = 6;
            return;
        }
    }

    Scan& s = g_scan;
    if (s.active) {
        if (s.phase == 0) {
            scan_tree(s, kNodesPerFrame);
            if (s.cursor >= s.tf.size() || s.tf.size() >= (size_t)kMaxNodes) {
                s.phase = 1;
                // Ближние вперёд: если бюджет треугольников кончится, останутся
                // стены вокруг игрока, а не дальний лес.
                const Vec3& o = s.origin;
                std::sort(s.near_nodes.begin(), s.near_nodes.end(),
                          [&](uint32_t a, uint32_t b) {
                              return a < s.pose.size() && b < s.pose.size() &&
                                     dist3(s.pose[a].pos, o) < dist3(s.pose[b].pos, o);
                          });
            }
            g_stats.state = 2;
        } else if (s.phase == 1) {
            scan_meshes(s, kMeshPerFrame);
            if (s.near_cursor >= s.near_nodes.size()) s.phase = 2;
            g_stats.state = 3;
        }
        if (s.phase == 2) {
            s.world.build(s.origin, kHarvestRadius);
            g_world = std::move(s.world);
            s.active = false;
            g_built_at = now;
            g_stats.state = 4;
        }
    } else if (g_world.ready()) {
        g_stats.state = 4;
    }
    g_stats.nodes = s.meshes;
    g_stats.scanned = s.scanned;
    g_stats.coarse = s.coarse;
    g_stats.triangles = (int)g_world.triangle_count();
    g_stats.vertices = (int)g_world.vertex_count();
    g_stats.origin[0] = g_scan.origin.x;
    g_stats.origin[1] = g_scan.origin.y;
    g_stats.origin[2] = g_scan.origin.z;
    g_stats.built_age = now - g_built_at;
}

void annotate(std::vector<EspBox>& boxes, const Vec3& eye) {
    if (!g_enabled || !g_world.ready() || !vec3_is_finite(eye)) {
        for (EspBox& b : boxes) b.occluded = false;
        return;
    }
    for (EspBox& b : boxes) {
        // Голова и грудь — мировые кости, посчитанные для аима; середина тела —
        // якорь бокса. Любая неперекрытая точка делает цель видимой: один кривой
        // обломок геометрии не должен гасить игрока целиком.
        Vec3 points[3];
        int count = 0;
        for (int slot = 0; slot <= 2; slot += 2) {
            if (!b.aim_world_valid[slot]) continue;
            const Vec3 p = {b.aim_world[slot][0], b.aim_world[slot][1], b.aim_world[slot][2]};
            if (vec3_is_finite(p)) points[count++] = p;
        }
        if (b.anchor_valid) {
            const Vec3 feet = {b.anchor_feet[0], b.anchor_feet[1], b.anchor_feet[2]};
            const float rise = (b.anchor_top_y - b.anchor_bottom_y) * 0.5F;
            if (vec3_is_finite(feet) && std::isfinite(rise))
                points[count++] = {feet.x, feet.y + rise, feet.z};
        }
        if (count == 0) {
            b.occluded = false;
            continue;
        }
        bool seen = false;
        for (int i = 0; i < count && !seen; ++i)
            if (!g_world.blocked(eye, points[i])) seen = true;
        b.occluded = !seen;
    }
}

bool segment_blocked(const Vec3& from, const Vec3& to, float* hit_fraction) {
    return g_world.blocked(from, to, hit_fraction);
}

const Stats& stats() { return g_stats; }

size_t diag(char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    const Stats& s = g_stats;
    static const char* kState[] = {"выкл",         "ищем раскладку", "обходим сцену",
                                   "читаем меши", "готово",         "данных нет",
                                   "нет входа"};
    size_t n = 0;
    auto put = [&](const char* fmt, ...) {
        if (n + 1 >= cap) return;
        va_list ap;
        va_start(ap, fmt);
        const int w = vsnprintf(out + n, cap - n, fmt, ap);
        va_end(ap);
        if (w > 0) n += (size_t)w;
        if (n >= cap - 1) n = cap - 1;
    };
    put("== меши мира (визибл чек) ==\n");
    put("тумблер   : %s\n", g_enabled ? "вкл" : "выкл");
    put("состояние : %s (%d)\n", kState[(s.state >= 0 && s.state <= 6) ? s.state : 0], s.state);
    put("раскладка : %s\n",
        s.via == 0 ? "из офсетов" : (s.via == 1 ? "структурный якорь" : "перебор"));
    put("  вершины : число +0x%X, буфер +0x%X, шаг %d\n", s.vcount, s.vptr, s.stride);
    put("  индексы : число +0x%X, буфер +0x%X, ширина %d\n", s.icount, s.iptr, s.width);
    put("  слот    : PPtr<Mesh> в компоненте +0x%X\n", s.field);
    put("сборка    : мешей %d (обойдено узлов %d), треугольников %d, вершин %d, боксов %d\n",
        s.nodes, s.scanned, s.triangles, s.vertices, s.coarse);
    put("центр     : %.1f %.1f %.1f, возраст %.1f с\n", s.origin[0], s.origin[1], s.origin[2],
        s.built_age);
    if (g_world.ready()) {
        // Контрольные лучи: вверх и вбок от центра сбора. Под крышей первый
        // обязан упираться, в чистом поле — нет. По ним же видно вырождение
        // геометрии в один гигантский бокс, который перекрывает всё подряд.
        const Vec3 from = {s.origin[0], s.origin[1] + 1.5F, s.origin[2]};
        const Vec3 up = {s.origin[0], s.origin[1] + 60.0F, s.origin[2]};
        const Vec3 side = {s.origin[0] + 60.0F, s.origin[1] + 1.5F, s.origin[2]};
        float hit = 0.0F;
        const bool up_blocked = g_world.blocked(from, up, &hit);
        put("луч вверх : %s (доля %.3f)\n", up_blocked ? "перекрыт" : "чисто", hit);
        const bool side_blocked = g_world.blocked(from, side, &hit);
        put("луч вбок  : %s (доля %.3f)\n", side_blocked ? "перекрыт" : "чисто", hit);
    } else {
        put("лучи      : геометрия не собрана\n");
    }
    out[n < cap ? n : cap - 1] = '\0';
    return n;
}

}  // namespace vis
