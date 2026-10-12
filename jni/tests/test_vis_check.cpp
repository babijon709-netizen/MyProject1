// Хост-тесты проверки видимости: ядро BVH (vis_check.h) и настройка карты
// (vis_check_map.h — загрузка меша, кэш дерева, гистерезис, бюджет).
//
// Сцена та же, что в демо исходника: стена x=100, y[-50,50], z[0,200], глаз в
// (0 0 64). Точки «за стеной» (x=200) обязаны быть перекрыты, точки «в обход»
// (y=120) — открыты. Проверяем и то, что гистерезис не гасит цель на первом
// кадре перекрытия, и что «неизвестно» (нет меша) отдаётся как «видна».
//
// Заглушки хоста: тест проверяет саму проверку, а не меню, тосты и каталог
// конфигов — CfgDir/lang::text/ShowToast подменяются здесь.
#include "vis_check.h"
#include "vis_check_map.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

namespace {
int failures = 0;
#define CHECK(cond, name) do { \
    if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", name, __LINE__); } \
} while (0)

std::string g_dir;
const char* temp_dir() {
    static std::string s;
    if (s.empty()) {
        char tmpl[] = "/tmp/xvcen_vis_XXXXXX";
        s = mkdtemp(tmpl) ? tmpl : "/tmp";
    }
    return s.c_str();
}

void write_file(const std::string& path, const char* text) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { printf("FAIL: cannot write %s\n", path.c_str()); ++failures; return; }
    fputs(text, f);
    fclose(f);
}

// Прямоугольник двумя треугольниками (веером), стена x=x0.
void add_wall(std::vector<vis::Tri>& tris, float x0, float y0, float y1, float z0, float z1) {
    const vis::Vec3 a{x0, y0, z0}, b{x0, y1, z0}, c{x0, y1, z1}, d{x0, y0, z1};
    tris.push_back({a, b, c});
    tris.push_back({a, c, d});
}

bool wait_ready(int max_ms) {
    for (int i = 0; i < max_ms / 10; ++i) {
        const vis_map::Status st = vis_map::status();
        if (st == vis_map::VIS_STATUS_READY || st == vis_map::VIS_STATUS_NOFILE ||
            st == vis_map::VIS_STATUS_FAILED) return st == vis_map::VIS_STATUS_READY;
        usleep(10 * 1000);
    }
    return false;
}

} // namespace

// ---- Заглушки окружения ----------------------------------------------------
const char* CfgDir() { return temp_dir(); }
void ShowToast(const char*) {}
namespace lang { const char* text(const char* ru) { return ru; } }

int main() {
    // ---- 1. Ядро BVH -------------------------------------------------------
    {
        std::vector<vis::Tri> tris;
        add_wall(tris, 100.f, -50.f, 50.f, 0.f, 200.f);
        vis::Bvh bvh;
        bvh.Build(std::move(tris));
        CHECK(bvh.TriCount() == 2, "wall has 2 triangles");
        CHECK(!bvh.Empty(), "tree built");

        const vis::Vec3 eye{0, 0, 64};
        CHECK(bvh.Occluded(eye, {200, 0, 72}), "behind wall is occluded");
        CHECK(bvh.Occluded(eye, {101, 0, 64}), "just past the wall is occluded");
        // Точка НА поверхности стены перекрытой не считается: конец луча
        // отодвигается (endPad), иначе цель, стоящая спиной к стене, читалась бы
        // как «за стеной» из-за самой этой стены.
        CHECK(!bvh.Occluded(eye, {100, 0, 64}), "point on the surface is not blocked");
        CHECK(!bvh.Occluded(eye, {200, 120, 72}), "around the wall is clear");
        CHECK(!bvh.Occluded(eye, {99.5f, 0, 64}, 0.25f), "in front of the wall is clear");

        float t = 0.f;
        CHECK(bvh.Raycast(eye, {1, 0, 0}, 1000.f, t) && std::fabs(t - 100.f) < 0.01f,
              "raycast distance to the wall");

        // Точки цели: одна за стеной, одна в обход — видима (любая открытая).
        const vis::Vec3 pts[2] = {{200, 0, 72}, {200, 120, 72}};
        CHECK(bvh.IsVisible(eye, pts, 2), "one clear point is enough");
        const vis::Vec3 hidden[2] = {{200, 0, 72}, {200, 0, 36}};
        CHECK(!bvh.IsVisible(eye, hidden, 2), "both points behind the wall");

        // ---- 2. Кэш дерева на диск -----------------------------------------
        const std::string obj = std::string(temp_dir()) + "/map.obj";
        const std::string bvh_path = obj + ".bvh";
        CHECK(bvh.SaveBinary(bvh_path.c_str()), "binary cache written");
        vis::Bvh loaded;
        CHECK(loaded.LoadBinary(bvh_path.c_str()), "binary cache read back");
        CHECK(loaded.TriCount() == bvh.TriCount(), "cache keeps triangle count");
        CHECK(loaded.Occluded(eye, {200, 0, 72}), "cache keeps occlusion");
        CHECK(!loaded.Occluded(eye, {200, 120, 72}), "cache keeps clearance");
        vis::Bvh broken;
        CHECK(!broken.LoadBinary("/tmp/definitely_missing.bvh"), "missing cache rejected");
    }

    // ---- 3. Разбор .obj ----------------------------------------------------
    {
        const std::string obj = std::string(temp_dir()) + "/small.obj";
        // Квад с отрицательными индексами (веер), координаты в метрах.
        write_file(obj,
                   "# test quad at x=5\n"
                   "v 5 0 0\nv 5 4 0\nv 5 4 6\nv 5 0 6\n"
                   "f -4 -3 -2\nf -4 -2 -1\n");
        std::vector<vis::Tri> tris;
        CHECK(vis::LoadObj(obj, tris, 1.0f, false), "obj parsed");
        CHECK(tris.size() == 2, "quad -> 2 triangles");
        CHECK(tris.size() == 2 && std::fabs(tris[0].a.x - 5.f) < 1e-5f, "obj x kept");
        std::vector<vis::Tri> scaled;
        CHECK(vis::LoadObj(obj, scaled, 0.01f, false) && !scaled.empty() &&
              std::fabs(scaled[0].a.x - 0.05f) < 1e-6f, "obj scale applied");
        std::vector<vis::Tri> missing;
        CHECK(!vis::LoadObj(std::string(temp_dir()) + "/nope.obj", missing), "missing obj rejected");
    }

    // ---- 4. Карта видимости (загрузка, гистерезис, бюджет, неизвестность) ---
    {
        const std::string obj = std::string(temp_dir()) + "/map.obj";
        write_file(obj,
                   "v 100 -50 0\nv 100 50 0\nv 100 50 200\nv 100 -50 200\n"
                   "f 1 2 3\nf 1 3 4\n");
        ::remove((obj + ".bvh").c_str());

        vis_map::set_mesh_path(obj.c_str());
        vis_map::init();
        vis_map::request(true);
        CHECK(wait_ready(5000), "mesh loaded in the background thread");
        CHECK(vis_map::status() == vis_map::VIS_STATUS_READY, "status is READY");
        CHECK(vis_map::tri_count() == 2, "map has 2 triangles");
        CHECK(std::strcmp(vis_map::status_text(), "VIS 2") == 0, "status text for a tiny map");

        const float hidden_pts[1][3] = {{200.f, 0.f, 72.f}};
        const float clear_pts[1][3]  = {{200.f, 120.f, 72.f}};
        const float eye[3] = {0.f, 0.f, 64.f};

        // Первое перекрытие цель не гасит: гистерезис ждёт второй кадр подряд.
        CHECK(vis_map::visible(7, eye, hidden_pts, 1) == true, "first hidden frame keeps the target");
        vis_map::frame_begin();
        CHECK(vis_map::visible(7, eye, hidden_pts, 1) == false, "second hidden frame hides the target");
        vis_map::frame_begin();
        CHECK(vis_map::visible(7, eye, clear_pts, 1) == true, "visible again immediately");
        vis_map::frame_begin();
        CHECK(vis_map::visible(8, eye, clear_pts, 1) == true, "clear target stays visible");

        // Один и тот же кадр: повторный вызов не тратит лучи и не двигает
        // гистерезис вперёд (FrameBoxes зовут несколько модулей на кадр).
        vis_map::frame_begin();
        CHECK(vis_map::visible(9, eye, hidden_pts, 1) == true, "frame A: still visible");
        CHECK(vis_map::visible(9, eye, hidden_pts, 1) == true, "frame A repeated: no second streak");
        vis_map::frame_begin();
        CHECK(vis_map::visible(9, eye, hidden_pts, 1) == false, "frame B: hidden");

        // Пустые точки и битый глаз — «неизвестно», то есть видна.
        CHECK(vis_map::visible(10, eye, nullptr, 0), "no points -> visible");
        const float bad_pts[1][3] = {{0.f, 0.f, 0.f}};
        CHECK(vis_map::visible(11, eye, bad_pts, 1), "coincident point -> visible");

        // Меш пропал: путь сменили на несуществующий — проверка молчит.
        vis_map::set_mesh_path((std::string(temp_dir()) + "/gone.obj").c_str());
        vis_map::request(true);
        usleep(200 * 1000);
        CHECK(vis_map::status() == vis_map::VIS_STATUS_NOFILE, "missing mesh -> NOFILE");
        vis_map::frame_begin();
        CHECK(vis_map::visible(12, eye, hidden_pts, 1) == true, "no map -> nothing is filtered");

        // Меш перечитан со свежего пути: сначала .obj, потом готовый .bvh.
        vis_map::set_mesh_path(obj.c_str());
        vis_map::request(true);
        CHECK(wait_ready(5000), "mesh reloaded");
        vis_map::set_mesh_path(obj.c_str());   // тот же путь — дерево не роняем
        CHECK(vis_map::status() == vis_map::VIS_STATUS_READY, "same path keeps the tree");

        // Только готовое дерево, без .obj: так и кладут меш на устройство —
        // собранный на ПК tools/vis_map/obj2bvh.cpp .bvh рядом с (не)мешем.
        const std::string only_obj = std::string(temp_dir()) + "/only.obj";
        CHECK(access((obj + ".bvh").c_str(), F_OK) == 0, "cache exists after the build");
        CHECK(rename((obj + ".bvh").c_str(), (only_obj + ".bvh").c_str()) == 0, "cache moved aside");
        CHECK(access(only_obj.c_str(), F_OK) != 0, "source mesh is really gone");
        vis_map::set_mesh_path(only_obj.c_str());
        vis_map::request(true);
        CHECK(wait_ready(5000), "cache-only map loads without the .obj");
        vis_map::frame_begin();
        CHECK(vis_map::visible(20, eye, hidden_pts, 1) == true, "cache-only frame A");
        vis_map::frame_begin();
        CHECK(vis_map::visible(20, eye, hidden_pts, 1) == false, "cache-only map occludes");
    }

    if (failures == 0) printf("test_vis_check: all checks passed\n");
    else printf("test_vis_check: %d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
