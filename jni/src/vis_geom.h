#pragma once
// vis_geom.h — хранилище собранной геометрии мира и запрос «перекрыт ли отрезок».
//
// Здесь нет ни чтения памяти, ни Unity: только вершины, треугольники, сетка
// и луч. Всё, что достаёт геометрию из игры, лежит в vis_mesh.cpp; этот файл
// собирается и тестируется отдельно (jni/tests/test_vis_mesh.cpp), потому что
// именно на нём держится correctness: неправильный обход сетки или
// неправильное пересечение луча с треугольником дадут «вижу сквозь стену» или
// «не вижу в чистом поле», и debugging этого по устройству — часы.
//
// Устройство.
//   * Геометрия — набор узлов. Узел = мировой AABB + диапазон треугольников
//     (пустой диапазон — «грубый» узел: боком считается сам AABB, так идут
//     меши, которые не влезли в бюджет треугольников).
//   * Сетка — столбцы по X/Z (cell × cell), по Y не делим: стены вертикальны,
//     а деление по высоте множит число ячеек без выигрыша. Узел попадает во
//     все столбцы, которые накрывает его AABB по X/Z, поэтому проверка
//     столбца — это уже проверка AABB по двум осям; третья досчитывается
//     slab-тестом.
//   * Запрос — 2D-DDA (Amanatides–Woo) по столбцам, в каждом столбце slab по
//     AABB узла, потом Möller–Trumbore по его треугольникам. Выход — на ПЕРВОМ
//     пересечении: вопрос «перекрыт ли» не требует ближайшего.
//
// Единицы — метры, мировые координаты игры (Y вверх).

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include "Vector.h"

namespace vis {

class World {
public:
    // Размер столбца, метры. Меньше — точнее отсев, но больше ячеек в обходе
    // луча; 6 м близко к габариту стены/двери в постройках.
    static constexpr float kCell = 6.0F;
    // Метры от начала луча, в которых попадание не считается: своё тело,
    // оружие в руках, ближняя поверхность, на которой стоит прицел.
    static constexpr float kSelfIgnore = 0.80F;
    // Метры до конца отрезка, в которых попадание не считается: поверхность
    // самой цели (кость почти всегда внутри/на границе меша).
    static constexpr float kRayPad = 0.40F;

    void clear() {
        nodes_.clear();
        verts_.clear();
        tris_.clear();
        start_.clear();
        items_.clear();
        stamp_.clear();
        stamp_id_ = 0;
        ready_ = false;
    }

    // Грубый узел: один AABB, треугольников нет. Возвращает false, если бокс
    // вырожден или не является числом.
    bool add_box(const float mn[3], const float mx[3]) {
        if (!sane(mn, mx)) return false;
        Node n{};
        copy3(n.mn, mn);
        copy3(n.mx, mx);
        n.first = (uint32_t)tris_.size();
        n.count = 0;
        nodes_.push_back(n);
        return true;
    }

    // Узел с треугольниками. verts — мировые вершины меша, idx — тройки
    // индексов в verts (не в общий пул). Возвращает false при мусоре на входе.
    bool add_mesh(const float mn[3], const float mx[3], const Vec3* verts,
                  size_t vert_count, const uint32_t* idx, size_t idx_count) {
        if (!sane(mn, mx) || !verts || !idx) return false;
        if (idx_count == 0 || (idx_count % 3) != 0) return false;
        if (vert_count == 0 || vert_count > (size_t)1u << 31) return false;
        for (size_t i = 0; i < idx_count; ++i)
            if (idx[i] >= vert_count) return false;   // индекс вне буфера — это мусор
        Node n{};
        copy3(n.mn, mn);
        copy3(n.mx, mx);
        n.first = (uint32_t)tris_.size();
        n.count = (uint32_t)(idx_count / 3);
        nodes_.push_back(n);
        const uint32_t base = (uint32_t)verts_.size();
        verts_.insert(verts_.end(), verts, verts + vert_count);
        for (size_t i = 0; i < idx_count; ++i) tris_.push_back(base + idx[i]);
        return true;
    }

    // Построить сетку вокруг origin радиусом radius (метры). Обязательно перед
    // первым запросом и после любой правки геометрии.
    void build(const Vec3& origin, float radius) {
        ready_ = false;
        start_.clear();
        items_.clear();
        stamp_.assign(nodes_.size(), 0);
        stamp_id_ = 0;
        if (nodes_.empty() || !(radius >= 1.0F) || !std::isfinite(radius)) return;

        const float half = radius + kCell;
        ox_ = origin.x - half;
        oz_ = origin.z - half;
        nx_ = (int)((2.0F * half) / kCell) + 1;
        nz_ = nx_;
        if (nx_ > 256) nx_ = 256;          // 256*6 = 1536 м — дальше не стреляют
        if (nz_ > 256) nz_ = 256;

        const size_t cells = (size_t)nx_ * (size_t)nz_;
        start_.assign(cells + 1, 0);
        for (const Node& n : nodes_) {
            int x0, x1, z0, z1;
            span(n.mn[0], n.mx[0], ox_, nx_, x0, x1);
            span(n.mn[2], n.mx[2], oz_, nz_, z0, z1);
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x)
                    ++start_[(size_t)z * (size_t)nx_ + (size_t)x + 1];
        }
        for (size_t i = 0; i < cells; ++i) start_[i + 1] += start_[i];
        items_.resize(start_[cells]);
        std::vector<uint32_t> cursor(start_.begin(), start_.begin() + cells);
        for (uint32_t id = 0; id < nodes_.size(); ++id) {
            const Node& n = nodes_[id];
            int x0, x1, z0, z1;
            span(n.mn[0], n.mx[0], ox_, nx_, x0, x1);
            span(n.mn[2], n.mx[2], oz_, nz_, z0, z1);
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x) {
                    const size_t cell = (size_t)z * (size_t)nx_ + (size_t)x;
                    items_[cursor[cell]++] = id;
                }
        }
        ready_ = true;
    }

    bool ready() const { return ready_; }
    size_t node_count() const { return nodes_.size(); }
    size_t triangle_count() const { return tris_.size() / 3; }
    size_t vertex_count() const { return verts_.size(); }

    // Перекрыт ли отрезок геометрией. false — либо не перекрыт, либо геометрии
    // нет (сетка не построена): незнание НЕ считается препятствием, иначе аим
    // ослеп бы на время сбора. hit_t — доля длины отрезка (0..1) первого
    // найденного пересечения; это ПЕРВОЕ, не обязательно ближайшее.
    bool blocked(const Vec3& from, const Vec3& to, float* hit_t = nullptr) {
        if (!ready_) return false;
        float dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
        const float len = sqrtf(dx * dx + dy * dy + dz * dz);
        if (!(len > 1e-3F) || !std::isfinite(len)) return false;
        const float dmin = kSelfIgnore;
        const float dmax = len - kRayPad;
        if (dmax <= dmin) return false;
        dx /= len;
        dy /= len;
        dz /= len;

        float px = (from.x - ox_) / kCell;
        float pz = (from.z - oz_) / kCell;
        if (!std::isfinite(px) || !std::isfinite(pz)) return false;
        int ix = clampi((int)floorf(px), 0, nx_ - 1);
        int iz = clampi((int)floorf(pz), 0, nz_ - 1);
        const int step_x = dx >= 0.0F ? 1 : -1;
        const int step_z = dz >= 0.0F ? 1 : -1;
        // Расстояния вдоль луча (в метрах, не в долях) до ближайших границ.
        const float bx = (dx > 0.0F ? (float)(ix + 1) : (float)ix) * kCell + ox_;
        const float bz = (dz > 0.0F ? (float)(iz + 1) : (float)iz) * kCell + oz_;
        float t_max_x = (dx != 0.0F) ? (bx - from.x) / dx : 1e30F;
        float t_max_z = (dz != 0.0F) ? (bz - from.z) / dz : 1e30F;
        const float t_delta_x = (dx != 0.0F) ? fabsf(kCell / dx) : 1e30F;
        const float t_delta_z = (dz != 0.0F) ? fabsf(kCell / dz) : 1e30F;
        if (t_max_x < 0.0F) t_max_x = 0.0F;   // старт внутри клетки
        if (t_max_z < 0.0F) t_max_z = 0.0F;

        if (stamp_id_ == 0xFFFFFFFFu) {
            std::fill(stamp_.begin(), stamp_.end(), 0u);
            stamp_id_ = 0;
        }
        ++stamp_id_;

        for (int guard = 0; guard < 4096; ++guard) {
            if (ix >= 0 && ix < nx_ && iz >= 0 && iz < nz_) {
                float t = 0.0F;
                if (test_cell(ix, iz, from, dx, dy, dz, dmin, dmax, t)) {
                    if (hit_t) *hit_t = t / len;
                    return true;
                }
            }
            if (t_max_x < t_max_z) {
                if (t_max_x > dmax) break;
                t_max_x += t_delta_x;
                ix += step_x;
            } else {
                if (t_max_z > dmax) break;
                t_max_z += t_delta_z;
                iz += step_z;
            }
            if (ix < -1 || ix > nx_ || iz < -1 || iz > nz_) break;
        }
        return false;
    }

private:
    struct Node {
        float mn[3], mx[3];
        uint32_t first, count;
    };

    static void copy3(float* out, const float* in) {
        out[0] = in[0];
        out[1] = in[1];
        out[2] = in[2];
    }
    static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
    static bool sane(const float mn[3], const float mx[3]) {
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(mn[i]) || !std::isfinite(mx[i])) return false;
            if (mx[i] < mn[i]) return false;
            if (fabsf(mn[i]) > 1e6F || fabsf(mx[i]) > 1e6F) return false;
        }
        return true;
    }
    void span(float lo, float hi, float origin, int count, int& out_lo, int& out_hi) const {
        const float a = (lo - origin) / kCell;
        const float b = (hi - origin) / kCell;
        out_lo = clampi((int)floorf(a), 0, count - 1);
        out_hi = clampi((int)floorf(b), 0, count - 1);
        if (out_hi < out_lo) std::swap(out_lo, out_hi);
    }

    // Луч против AABB: slab-тест на отрезке [dmin, dmax] вдоль луча.
    static bool ray_aabb(const Vec3& o, const Vec3& d, const float mn[3], const float mx[3],
                         float dmin, float dmax, float& t_enter) {
        const float oo[3] = {o.x, o.y, o.z};
        const float dd[3] = {d.x, d.y, d.z};
        float t0 = dmin, t1 = dmax;
        for (int a = 0; a < 3; ++a) {
            if (fabsf(dd[a]) < 1e-8F) {
                if (oo[a] < mn[a] || oo[a] > mx[a]) return false;
                continue;
            }
            const float inv = 1.0F / dd[a];
            float ta = (mn[a] - oo[a]) * inv;
            float tb = (mx[a] - oo[a]) * inv;
            if (ta > tb) std::swap(ta, tb);
            if (ta > t0) t0 = ta;
            if (tb < t1) t1 = tb;
            if (t0 > t1) return false;
        }
        t_enter = t0;
        return true;
    }

    // Möller–Trumbore, двусторонний: отсекать по ориентации нельзя — меши
    // построек игроки часто выворачивают, да и тонкая стена одна.
    bool ray_tri(const Vec3& o, const Vec3& d, const Vec3& a, const Vec3& b, const Vec3& c,
                 float dmin, float dmax, float& t) const {
        const Vec3 e1 = {b.x - a.x, b.y - a.y, b.z - a.z};
        const Vec3 e2 = {c.x - a.x, c.y - a.y, c.z - a.z};
        const Vec3 p = {d.y * e2.z - d.z * e2.y, d.z * e2.x - d.x * e2.z,
                        d.x * e2.y - d.y * e2.x};
        const float det = e1.x * p.x + e1.y * p.y + e1.z * p.z;
        if (fabsf(det) < 1e-12F) return false;
        const float inv = 1.0F / det;
        const Vec3 s = {o.x - a.x, o.y - a.y, o.z - a.z};
        const float u = (s.x * p.x + s.y * p.y + s.z * p.z) * inv;
        if (u < -1e-4F || u > 1.0001F) return false;
        const Vec3 q = {s.y * e1.z - s.z * e1.y, s.z * e1.x - s.x * e1.z,
                        s.x * e1.y - s.y * e1.x};
        const float v = (d.x * q.x + d.y * q.y + d.z * q.z) * inv;
        if (v < -1e-4F || u + v > 1.0001F) return false;
        const float tt = (e2.x * q.x + e2.y * q.y + e2.z * q.z) * inv;
        if (tt < dmin || tt > dmax) return false;
        t = tt;
        return true;
    }

    bool test_cell(int ix, int iz, const Vec3& o, float dx, float dy, float dz, float dmin,
                   float dmax, float& hit) {
        const size_t cell = (size_t)iz * (size_t)nx_ + (size_t)ix;
        const uint32_t lo = start_[cell], hi = start_[cell + 1];
        const Vec3 d = {dx, dy, dz};
        for (uint32_t k = lo; k < hi; ++k) {
            const uint32_t id = items_[k];
            if (stamp_[id] == stamp_id_) continue;   // узел накрывает несколько столбцов
            stamp_[id] = stamp_id_;
            const Node& n = nodes_[id];
            float t_enter = 0.0F;
            if (!ray_aabb(o, d, n.mn, n.mx, dmin, dmax, t_enter)) continue;
            if (n.count == 0) {          // грубый узел: сам бокс и есть препятствие
                hit = t_enter;
                return true;
            }
            const uint32_t* tri = tris_.data() + n.first;
            for (uint32_t t = 0; t < n.count; ++t) {
                if (ray_tri(o, d, verts_[tri[t * 3]], verts_[tri[t * 3 + 1]],
                            verts_[tri[t * 3 + 2]], dmin, dmax, hit))
                    return true;
            }
        }
        return false;
    }

    std::vector<Node> nodes_;
    std::vector<Vec3> verts_;
    std::vector<uint32_t> tris_;
    std::vector<uint32_t> start_, items_;
    std::vector<uint32_t> stamp_;
    uint32_t stamp_id_ = 0;
    float ox_ = 0.0F, oz_ = 0.0F;
    int nx_ = 0, nz_ = 0;
    bool ready_ = false;
};

}  // namespace vis
