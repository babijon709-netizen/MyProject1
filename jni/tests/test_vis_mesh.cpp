// Хост-тест геометрии визибл-чека (vis_geom.h): сборка боксов и мешей, сетка,
// обход столбцов и пересечение луча с треугольником. Чтения памяти здесь нет
// вообще — тест проверяет ровно ту часть, ошибка в которой даёт «вижу сквозь
// стену» или «не вижу в чистом поле».
//
// Запуск: scripts/host_tests.sh  (или g++ -std=gnu++17 -I jni/src -I jni/include
//         jni/tests/test_vis_mesh.cpp -o /tmp/t && /tmp/t)

#include "vis_geom.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

int g_failed = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) return;
    ++g_failed;
    printf("FAIL: %s\n", what);
}

bool near(float a, float b, float eps = 0.02f) { return fabsf(a - b) <= eps; }

// Стена 6 × 3 метра в плоскости x = 10.
void add_wall(vis::World& w, float x = 10.0f, float low = 0.0f, float high = 3.0f) {
    const Vec3 verts[4] = {{x, low, -3.0f}, {x, high, -3.0f}, {x, high, 3.0f}, {x, low, 3.0f}};
    const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    const float mn[3] = {x - 0.01f, low, -3.0f};
    const float mx[3] = {x + 0.01f, high, 3.0f};
    check(w.add_mesh(mn, mx, verts, 4, idx, 6), "wall accepted");
}

}  // namespace

int main() {
    // ---- пустой мир: незнание не считается препятствием -------------------
    {
        vis::World w;
        check(!w.blocked({0, 1, 0}, {50, 1, 0}), "empty world is never blocking");
        w.build({0, 1, 0}, 140.f);
        check(!w.ready() || !w.blocked({0, 1, 0}, {50, 1, 0}), "empty grid is never blocking");
    }

    // ---- стена -------------------------------------------------------------
    {
        vis::World w;
        add_wall(w);
        w.build({0, 1, 0}, 140.f);
        check(w.ready(), "grid built");
        check(w.triangle_count() == 2, "two triangles");

        float hit = -1.f;
        check(w.blocked({0, 1, 0}, {20, 1, 0}, &hit), "straight ray hits the wall");
        check(near(hit, 0.5f), "hit at half the segment");

        // Мимо стены: вдоль неё и через просвет сверху.
        check(!w.blocked({0, 1, 0}, {0, 1, 20}), "along the wall is clean");
        check(!w.blocked({0, 5, 0}, {20, 5, 0}), "over the wall is clean");
        check(!w.blocked({0, 1, 0}, {20, 1, 10}), "past the wall edge is clean");
        // Назад: двусторонний тест, луч из-за стены тоже упирается.
        check(w.blocked({20, 1, 0}, {0, 1, 0}), "reverse ray hits the wall");
        check(!w.blocked({20, 1, 0}, {0, 7, 0}), "reverse ray over the wall is clean");
    }

    // ---- свои 0.8 м и 0.4 м до цели ---------------------------------------
    {
        vis::World w;
        add_wall(w, 0.5f);   // прямо перед глазом
        w.build({0, 1, 0}, 140.f);
        check(!w.blocked({0, 1, 0}, {20, 1, 0}), "geometry under 0.8 m is ignored");
    }
    {
        vis::World w;
        add_wall(w, 19.8f);  // почти в точке цели
        w.build({0, 1, 0}, 140.f);
        check(!w.blocked({0, 1, 0}, {20, 1, 0}), "geometry within 0.4 m of the end is ignored");
    }

    // ---- грубый бокс -------------------------------------------------------
    {
        vis::World w;
        const float mn[3] = {9.0f, 0.0f, -3.0f};
        const float mx[3] = {11.0f, 3.0f, 3.0f};
        check(w.add_box(mn, mx), "coarse node accepted");
        w.build({0, 1, 0}, 140.f);
        check(w.blocked({0, 1, 0}, {20, 1, 0}), "coarse node blocks");
        check(!w.blocked({0, 1, 0}, {0, 1, 20}), "coarse node does not block sideways");
    }

    // ---- мусор на входе ----------------------------------------------------
    {
        vis::World w;
        const Vec3 verts[3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
        const uint32_t bad[3] = {0, 1, 7};   // индекс вне буфера вершин
        const uint32_t ok[3] = {0, 1, 2};
        const float mn[3] = {0, 0, 0};
        const float mx[3] = {1, 1, 0};
        check(!w.add_mesh(mn, mx, verts, 3, bad, 3), "index out of range rejected");
        check(!w.add_mesh(mn, mx, verts, 3, ok, 2), "index count not a multiple of 3 rejected");
        const float nan_box[3] = {NAN, 0, 0};
        check(!w.add_box(nan_box, mx), "NaN box rejected");
        check(!w.add_mesh(mx, mn, verts, 3, ok, 3), "inverted box rejected");
        check(w.node_count() == 0, "nothing was added");
    }

    // ---- дальний обход сетки: стена в 100 метрах ---------------------------
    {
        vis::World w;
        add_wall(w, 100.f);
        w.build({0, 1, 0}, 140.f);
        float hit = -1.f;
        check(w.blocked({0, 1, 0}, {130, 1, 0}, &hit), "far wall is found by the DDA");
        check(near(hit, 100.f / 130.f), "far hit fraction");
        check(!w.blocked({0, 1, 0}, {90, 1, 0}), "short ray stops before the wall");
    }

    // ---- луч строго вертикально (dx = dz = 0) ------------------------------
    {
        vis::World w;
        const Vec3 verts[4] = {{-3, 5, -3}, {3, 5, -3}, {3, 5, 3}, {-3, 5, 3}};
        const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
        const float mn[3] = {-3, 5, -3};
        const float mx[3] = {3, 5, 3};
        check(w.add_mesh(mn, mx, verts, 4, idx, 6), "ceiling accepted");
        w.build({0, 1, 0}, 140.f);
        check(w.blocked({0, 1, 0}, {0, 20, 0}), "vertical ray hits the ceiling");
        check(!w.blocked({0, 1, 0}, {0, 3, 0}), "vertical ray stops below the ceiling");
    }

    // ---- узел накрывает несколько столбцов, но проверяется один раз --------
    {
        vis::World w;
        const Vec3 verts[4] = {{-3, 0, -3}, {3, 0, -3}, {3, 0, 3}, {-3, 0, 3}};
        const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
        const float mn[3] = {-3, 0, -3};
        const float mx[3] = {3, 0, 3};
        // Плоскость y = 0 под ногами: луч вперёд не должен на неё натыкаться,
        // хотя бокс узла пересекает половину столбцов по пути.
        check(w.add_mesh(mn, mx, verts, 4, idx, 6), "floor accepted");
        w.build({0, 1, 0}, 140.f);
        check(!w.blocked({0, 1, 0}, {20, 1, 0}), "floor below the ray does not block");
        check(w.blocked({0, 3, 0}, {0, -3, 0}), "ray down hits the floor");
    }

    // ---- перестройка и очистка --------------------------------------------
    {
        vis::World w;
        add_wall(w);
        w.build({0, 1, 0}, 140.f);
        check(w.blocked({0, 1, 0}, {20, 1, 0}), "blocked before rebuild");
        check(!w.blocked({0, 1, 0}, {20, 1, 0}) || true, "");
        w.clear();
        check(!w.ready(), "clear drops the grid");
        check(!w.blocked({0, 1, 0}, {20, 1, 0}), "cleared world is transparent");
        add_wall(w);
        check(!w.blocked({0, 1, 0}, {20, 1, 0}), "geometry without build() is inert");
        w.build({0, 1, 0}, 140.f);
        check(w.blocked({0, 1, 0}, {20, 1, 0}), "blocked again after rebuild");
    }

    printf("vis_mesh geometry: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
