#pragma once
// vis_check.h — геометрическая проверка видимости: есть ли между глазом и
// точкой тела геометрия карты.
//
// Это не обращение к игре: ни il2cpp, ни Physics.Linecast, ни костей — только
// треугольники меша карты и мировые координаты, которые кадр ESP уже посчитал.
// Значит, работает и на внешнем чтении (наш случай): меш грузится один раз,
// BVH строится один раз, дальше это обычный CPU-луч по дереву.
//
// Конвенция: ЕДИНАЯ мировая система координат с позициями игроков, что у
// источника, что у меша. Единицы — метры игры. Если меш экспортирован в других
// единицах или в Y-up/Z-up (Unity — Y-up, Source — Z-up), это правится
// масштабом/свап-осью при загрузке (LoadObj), а не здесь.
//
// Источник: vis_check.hpp из архива vischeck (BVH-часть висибл чека), логика
// сохранена; убрана SpottedByMask (маска CS2, привязывать её тут не к чему) и
// добавлены SaveBinary/LoadBinary — собранное дерево кэшируется на диск, чтобы
// на телефоне не разбирать .obj на каждом запуске.
//
// Что откуда брать (внешний процесс, без инъекции):
//   * меш карты  — экспорт ассетов игры в .obj (Unity: AssetStudio/AssetRipper,
//                  Mesh -> Export), либо собственный экспорт коллизии;
//   * точки тела — мировые кости кадра ESP (game.cpp -> EspBox.anchor_bone);
//   * глаз       — позиция камеры кадра (esp_frame_camera_position).
// Динамику (постройки игроков, двери, мешки) статический меш не знает: их в нём
// просто нет. Террейн, скалы, деревья и статические строения карты — перекрывает
// он; постройки — второе дерево, собранное из префабов и мировых трансформов
// (vis_check_dyn.h, слой vis_map). Этот файл остаётся std-only: повороты и
// инстанс меша в мир нужны ровно для того, чтобы динамика собиралась снаружи,
// не трогая ядро обхода.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace vis {

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float X, float Y, float Z) : x(X), y(Y), z(Z) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
};
inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float Length(const Vec3& v) { return std::sqrt(Dot(v, v)); }
inline Vec3 Min3(const Vec3& a, const Vec3& b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
inline Vec3 Max3(const Vec3& a, const Vec3& b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }

struct Tri { Vec3 a, b, c; };


struct AABB {
    Vec3 mn{ std::numeric_limits<float>::max(),  std::numeric_limits<float>::max(),  std::numeric_limits<float>::max()};
    Vec3 mx{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
    void Grow(const Vec3& p) { mn = Min3(mn, p); mx = Max3(mx, p); }
    void Grow(const Tri& t) { Grow(t.a); Grow(t.b); Grow(t.c); }
};

// ---- Поворот/перенос: инстанс меша в мир -----------------------------------
// Зачем. Динамика (постройки игроков) живёт в мире как ПРЕФАБ + мировой TRS:
// сам префаб экспортируется один раз, а положение каждого куска читается из
// иерархии трансформов игры (read_transform_hierarchy_layout даёт и позицию, и
// мировой поворот). Поэтому меш библиотеки разворачивается в мир здесь, а не
// правится в файле.
struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
    Quat() = default;
    Quat(float X, float Y, float Z, float W) : x(X), y(Y), z(Z), w(W) {}
};

inline Quat QuatNormalized(const Quat& q) {
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(n > 1e-6f) || !std::isfinite(n)) return Quat{0, 0, 0, 1};
    const float inv = 1.0f / n;
    return Quat{q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

// v' = q * v * q^-1, разложенное (тот же результат, что у проекта в game.cpp,
// но без зависимости от него: vis_check.h остаётся std-only).
inline Vec3 RotateByQuat(const Quat& q, const Vec3& v) {
    const Vec3 u{q.x, q.y, q.z};
    const float s = q.w;
    const Vec3 t = Cross(u, v) * 2.0f;
    return v + t * s + Cross(u, t);
}

inline Vec3 TransformPoint(const Vec3& p, const Vec3& pos, const Quat& rot, float scale = 1.0f) {
    return RotateByQuat(rot, p * scale) + pos;
}

inline Tri TransformTri(const Tri& t, const Vec3& pos, const Quat& rot, float scale = 1.0f) {
    return {TransformPoint(t.a, pos, rot, scale),
            TransformPoint(t.b, pos, rot, scale),
            TransformPoint(t.c, pos, rot, scale)};
}

// Локальный габарит набора треугольников: по нему делается и коробка-замена,
// когда меша этого имени в библиотеке нет, и проверка «похоже ли на правду».
inline bool LocalBounds(const Tri* tris, size_t n, Vec3& center, Vec3& extents) {
    if (tris == nullptr || n == 0) return false;
    AABB b;
    for (size_t i = 0; i < n; ++i) b.Grow(tris[i]);
    if (!(b.mn.x <= b.mx.x)) return false;   // пустой/неинициализированный AABB
    center  = (b.mx + b.mn) * 0.5f;
    extents = (b.mx - b.mn) * 0.5f;
    return std::isfinite(center.x) && std::isfinite(center.y) && std::isfinite(center.z) &&
           std::isfinite(extents.x) && std::isfinite(extents.y) && std::isfinite(extents.z);
}

// 12 треугольников из oriented box: центр/полуоси в локальных координатах,
// разворот и перенос — мировые. Это и есть инстанс куска постройки.
inline void AppendObb(const Vec3& center, const Vec3& extents,
                      const Vec3& pos, const Quat& rot, float scale,
                      std::vector<Tri>& out) {
    const float ex = extents.x * scale, ey = extents.y * scale, ez = extents.z * scale;
    if (!(ex > 1e-4f) || !(ey > 1e-4f) || !(ez > 1e-4f)) return;
    if (!std::isfinite(ex) || !std::isfinite(ey) || !std::isfinite(ez)) return;
    Vec3 c[8];
    for (int i = 0; i < 8; ++i) {
        const Vec3 local{
            center.x * scale + ((i & 1) ? ex : -ex),
            center.y * scale + ((i & 2) ? ey : -ey),
            center.z * scale + ((i & 4) ? ez : -ez)};
        // scale уже применён к полуосям и к центру: второй раз умножать нельзя.
        c[i] = TransformPoint(local, pos, rot, 1.0f);
    }
    // 6 граней по 2 треугольника. Направления граней здесь не важны: RayTri
    // двусторонний (det может быть отрицательным), луч всё равно ловится.
    static const int kFaces[6][4] = {
        {0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1},
        {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
    for (const auto& f : kFaces) {
        out.push_back({c[f[0]], c[f[1]], c[f[2]]});
        out.push_back({c[f[0]], c[f[2]], c[f[3]]});
    }
}

// Moller-Trumbore, двусторонний. Возвращает расстояние попадания в t.
inline bool RayTri(const Vec3& o, const Vec3& d, const Tri& tr, float tMax, float& t) {
    constexpr float EPS = 1e-7f;
    const Vec3 e1 = tr.b - tr.a, e2 = tr.c - tr.a;
    const Vec3 p = Cross(d, e2);
    const float det = Dot(e1, p);
    if (std::fabs(det) < EPS) return false;
    const float inv = 1.0f / det;
    const Vec3 s = o - tr.a;
    const float u = Dot(s, p) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    const Vec3 q = Cross(s, e1);
    const float v = Dot(d, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    const float tt = Dot(e2, q) * inv;
    if (tt < 1e-4f || tt > tMax) return false;
    t = tt;
    return true;
}

// Slab-тест. invD = 1/d по каждой оси.
inline bool RayBox(const Vec3& o, const Vec3& invD, const AABB& b, float tMax) {
    float t0 = 0.0f, t1 = tMax;
    for (int i = 0; i < 3; ++i) {
        float n = (b.mn[i] - o[i]) * invD[i];
        float f = (b.mx[i] - o[i]) * invD[i];
        if (n > f) std::swap(n, f);
        t0 = std::max(t0, n);
        t1 = std::min(t1, f);
        if (t0 > t1) return false;
    }
    return true;
}

class Bvh {
public:
    void Build(std::vector<Tri> tris) {
        tris_ = std::move(tris);
        nodes_.clear();
        if (tris_.empty()) return;
        std::vector<uint32_t> idx(tris_.size());
        for (uint32_t i = 0; i < idx.size(); ++i) idx[i] = i;
        nodes_.reserve(tris_.size() * 2);
        BuildRec(idx, 0, static_cast<uint32_t>(idx.size()));
        // Треугольники переставляются в порядок листьев — обход читает их подряд.
        std::vector<Tri> sorted(tris_.size());
        for (size_t i = 0; i < idx.size(); ++i) sorted[i] = tris_[idx[i]];
        tris_ = std::move(sorted);
    }

    bool Empty() const { return nodes_.empty(); }
    size_t TriCount() const { return tris_.size(); }
    size_t NodeCount() const { return nodes_.size(); }

    // Ближайшее попадание: true + tHit, если что-то пересеклось в пределах maxT.
    bool Raycast(const Vec3& o, const Vec3& dir, float maxT, float& tHit) const {
        return Trace(o, dir, maxT, false, &tHit);
    }

    // true — отрезок from->to перекрыт геометрией. endPad отодвигает конец луча
    // от цели, чтобы не поймать поверхность, на которой цель стоит/лежит.
    bool Occluded(const Vec3& from, const Vec3& to, float endPad = 0.5f) const {
        const Vec3 d = to - from;
        const float len = Length(d);
        if (len < 1e-4f) return false;
        const Vec3 dn = d * (1.0f / len);
        return Trace(from, dn, std::max(0.0f, len - endPad), true, nullptr);
    }

    // Видима, если хотя бы одна точка (голова, грудь, таз, ноги, кости) не перекрыта.
    bool IsVisible(const Vec3& eye, const Vec3* points, size_t n) const {
        for (size_t i = 0; i < n; ++i)
            if (!Occluded(eye, points[i])) return true;
        return false;
    }
    bool IsVisible(const Vec3& eye, const std::vector<Vec3>& pts) const {
        return IsVisible(eye, pts.data(), pts.size());
    }

    // ---- Кэш собранного дерева на диск -------------------------------------
    // Разбор .obj на телефоне — секунды (текстовый парсер), а дерево от запуска
    // к запуску не меняется. Формат: заголовок + треугольники + узлы, всё POD
    // little-endian, поэтому .bvh можно собрать и на ПК (tools/vis_map) и
    // положить на устройство готовым.
    bool SaveBinary(const char* path) const {
        if (path == nullptr || nodes_.empty()) return false;
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        Header h{};
        h.tri_count = tris_.size();
        h.node_count = nodes_.size();
        f.write(reinterpret_cast<const char*>(&h), sizeof(h));
        f.write(reinterpret_cast<const char*>(tris_.data()),
                static_cast<std::streamsize>(tris_.size() * sizeof(Tri)));
        f.write(reinterpret_cast<const char*>(nodes_.data()),
                static_cast<std::streamsize>(nodes_.size() * sizeof(Node)));
        return f.good();
    }

    bool LoadBinary(const char* path) {
        if (path == nullptr) return false;
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        Header h{};
        if (!f.read(reinterpret_cast<char*>(&h), sizeof(h))) return false;
        if (std::memcmp(h.magic, kMagic, 4) != 0 || h.version != kVersion) return false;
        if (h.tri_count == 0 || h.tri_count > (1u << 24)) return false;   // 16M треугольников — уже не карта
        if (h.node_count == 0 || h.node_count > h.tri_count * 2u + 8u) return false;
        std::vector<Tri> tris(h.tri_count);
        std::vector<Node> nodes(h.node_count);
        if (!f.read(reinterpret_cast<char*>(tris.data()),
                    static_cast<std::streamsize>(h.tri_count * sizeof(Tri)))) return false;
        if (!f.read(reinterpret_cast<char*>(nodes.data()),
                    static_cast<std::streamsize>(h.node_count * sizeof(Node)))) return false;
        tris_ = std::move(tris);
        nodes_ = std::move(nodes);
        return true;
    }

private:
    struct Node {
        AABB box;
        uint32_t left = 0;   // внутренний: левый потомок (правый — не обязательно left+1)
        uint32_t right = 0;  // внутренний: правый потомок
        uint32_t first = 0;  // лист: первый треугольник
        uint32_t count = 0;  // лист: сколько треугольников (0 = внутренний)
    };

    static constexpr char kMagic[4] = {'B', 'V', 'H', '1'};
    static constexpr uint32_t kVersion = 1;
    struct Header {
        char     magic[4] = {'B', 'V', 'H', '1'};
        uint32_t version  = kVersion;
        uint64_t tri_count = 0;
        uint64_t node_count = 0;
    };

    uint32_t BuildRec(std::vector<uint32_t>& idx, uint32_t lo, uint32_t hi) {
        const uint32_t ni = static_cast<uint32_t>(nodes_.size());
        nodes_.emplace_back();
        AABB box, cbox;
        for (uint32_t i = lo; i < hi; ++i) {
            const Tri& t = tris_[idx[i]];
            box.Grow(t);
            cbox.Grow((t.a + t.b + t.c) * (1.0f / 3.0f));
        }
        nodes_[ni].box = box;
        const uint32_t n = hi - lo;
        if (n <= 4) {
            nodes_[ni].first = lo;
            nodes_[ni].count = n;
            return ni;
        }
        const Vec3 ext = cbox.mx - cbox.mn;
        int axis = (ext.x > ext.y && ext.x > ext.z) ? 0 : (ext.y > ext.z ? 1 : 2);
        const uint32_t mid = lo + n / 2;
        std::nth_element(idx.begin() + lo, idx.begin() + mid, idx.begin() + hi,
            [&](uint32_t a, uint32_t b) {
                const Tri& ta = tris_[a]; const Tri& tb = tris_[b];
                return (ta.a[axis] + ta.b[axis] + ta.c[axis]) < (tb.a[axis] + tb.b[axis] + tb.c[axis]);
            });
        const uint32_t l = BuildRec(idx, lo, mid);
        const uint32_t r = BuildRec(idx, mid, hi);
        nodes_[ni].left = l;
        nodes_[ni].right = r;
        nodes_[ni].count = 0;
        return ni;
    }

    bool Trace(const Vec3& o, const Vec3& d, float maxT, bool anyHit, float* outT) const {
        if (nodes_.empty()) return false;
        const Vec3 invD{
            std::fabs(d.x) < 1e-12f ? 1e12f : 1.0f / d.x,
            std::fabs(d.y) < 1e-12f ? 1e12f : 1.0f / d.y,
            std::fabs(d.z) < 1e-12f ? 1e12f : 1.0f / d.z};
        uint32_t stack[128];
        int sp = 0;
        stack[sp++] = 0;
        float best = maxT;
        bool hit = false;
        while (sp) {
            const Node& nd = nodes_[stack[--sp]];
            if (!RayBox(o, invD, nd.box, best)) continue;
            if (nd.count) {
                for (uint32_t i = 0; i < nd.count; ++i) {
                    float t;
                    if (RayTri(o, d, tris_[nd.first + i], best, t)) {
                        if (anyHit) return true;
                        best = t;
                        hit = true;
                    }
                }
            } else if (sp < 126) {
                stack[sp++] = nd.left;
                stack[sp++] = nd.right;
            }
        }
        if (hit && outT) *outT = best;
        return hit;
    }

    std::vector<Tri> tris_;
    std::vector<Node> nodes_;
};

// Минимальный загрузчик .obj: v / f (n-gon разбивается веером, поддержаны
// v/vt/vn и отрицательные индексы).
// scale: множитель координат (например меш карты в других единицах).
// yUpToZUp: меш Y-up, мир Z-up -> (x, y, z) в (x, -z, y). Unity — Y-up, так что
// для нашей карты это false, а масштаб — только под единицы экспорта.
inline bool LoadObj(const std::string& path, std::vector<Tri>& out, float scale = 1.0f, bool yUpToZUp = false) {
    std::ifstream f(path);
    if (!f) return false;
    std::vector<Vec3> v;
    std::string line;
    while (std::getline(f, line)) {
        if (line.size() < 2) continue;
        std::istringstream ss(line);
        std::string tag;
        ss >> tag;
        if (tag == "v") {
            float x, y, z;
            if (!(ss >> x >> y >> z)) continue;
            if (yUpToZUp) { const float ny = -z, nz = y; y = ny; z = nz; }
            v.emplace_back(x * scale, y * scale, z * scale);
        } else if (tag == "f") {
            std::vector<int> ids;
            std::string tok;
            while (ss >> tok) {
                int i = std::atoi(tok.c_str());
                if (i < 0) i = static_cast<int>(v.size()) + i; else i -= 1;
                if (i < 0 || i >= static_cast<int>(v.size())) { ids.clear(); break; }
                ids.push_back(i);
            }
            for (size_t k = 1; k + 1 < ids.size(); ++k)
                out.push_back({v[ids[0]], v[ids[k]], v[ids[k + 1]]});
        }
    }
    return !out.empty();
}

} // namespace vis
