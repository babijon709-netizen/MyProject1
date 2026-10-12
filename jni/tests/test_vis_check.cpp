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
#include <sys/stat.h>
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

    // ==== Динамика: библиотека префабов и второе дерево =====================
    // Постройки спавнятся в рантайме, поэтому их геометрия собирается как
    // ПРЕФАБ (файл в каталоге) + мировой TRS (память игры). Здесь проверяются
    // обе половины, которые не требуют памяти игры: сопоставление имени и
    // инстанс в мир.
    CHECK(vis_dyn::NormalizeKey("Wood_Wall") == "wallwood", "normalize key");
    CHECK(vis_dyn::NormalizeKey("WoodWall_LOD0") == "lod0wallwood", "camelCase and suffix split");
    CHECK(vis_dyn::NormalizeKey("wall wood") == vis_dyn::NormalizeKey("Wood Wall"),
          "word order does not matter");
    CHECK(vis_dyn::NormalizeKey("") .empty(), "empty key");
    CHECK(vis::RotateByQuat(vis::QuatNormalized(vis::Quat{0, 0, 0.70710678f, 0.70710678f}),
                            vis::Vec3{1, 0, 0}).y > 0.99f, "quat yaw rotates +X to +Y");

    {
        // Библиотека: три префаба, имена как их отдаёт экспорт.
        const std::string dir = std::string(temp_dir()) + "/pieces";
        mkdir(dir.c_str(), 0755);
        const char* wall_obj =
            "v -1.5 -0.1 0\nv 1.5 -0.1 0\nv 1.5 0.1 0\nv -1.5 0.1 0\nf 1 2 3\nf 1 3 4\n";
        write_file(dir + "/woodwall.obj", wall_obj);
        write_file(dir + "/WoodFoundation.obj", wall_obj);
        write_file(dir + "/wooddoorway.obj", wall_obj);
        write_file(dir + "/notes.txt", "not a mesh");

        vis_dyn::Library lib;
        CHECK(vis_dyn::LoadDir(lib, dir.c_str(), 1.0f, false) == 3, "three meshes loaded");
        CHECK(lib.size() == 3 && lib.tri_total == 6, "library keeps triangles");
        const vis_dyn::Entry* wall = lib.Find("woodwall");
        CHECK(wall != nullptr && wall->file == "woodwall", "exact match");
        const vis_dyn::Entry* lod = lib.Find("WoodWall_LOD0");
        CHECK(lod != nullptr && lod->file == "woodwall", "LOD suffix finds the prefab");
        const vis_dyn::Entry* reordered = lib.Find("Foundation Wood Damaged");
        CHECK(reordered != nullptr && reordered->file == "WoodFoundation",
              "tokens as a subset, order-free");
        CHECK(lib.Find("stonewall") == nullptr, "no match is no match");
        CHECK(lib.Find(nullptr) == nullptr && lib.Find("") == nullptr, "empty name is safe");

        // Ступень 3: имя файла ОДНИМ токеном («woodwall.obj», всё строчными) не
        // бьётся на токены, поэтому подмножество не сработает — такой префаб
        // находится подстрокой в имени объекта, в исходном порядке токенов.
        // Целиком: «stonewall» и «WoodenBoxLarge» его найти не должны.
        {
            const std::string dir2 = std::string(temp_dir()) + "/pieces_lc";
            mkdir(dir2.c_str(), 0755);
            write_file(dir2 + "/woodwall.obj", wall_obj);
            vis_dyn::Library lc;
            CHECK(vis_dyn::LoadDir(lc, dir2.c_str(), 1.0f, false) == 1, "lowercase library loads");
            CHECK(lc.Find("WoodWall_LOD0") != nullptr, "glued substring finds the one-token prefab");
            CHECK(lc.Find("BigWoodWall") != nullptr, "prefix in the scene name is fine");
            CHECK(lc.Find("stonewall") == nullptr, "substring must be whole, not 'wall'");
            CHECK(lc.Find("WoodenBoxLarge") == nullptr, "wooden is not wood");
            CHECK(lc.Find("woodwall") != nullptr, "exact match still works");
        }

        // Инстанс в мир. Ключевая тонкость, из-за которой сцена именно такая:
        // AppendObb вращает вокруг ПОЗИЦИИ КУСКА (пивота), а не вокруг его
        // центра — центр задан в локальных координатах префаба и уезжает вместе
        // с поворотом. Так и в игре: пивот стены — её трансформ, а геометрия
        // префаба смещена относительно него.
        //
        // Пластина 4x2x0.2 с центром на +1.5 по Z от пивота. Луч идёт вдоль Z.
        //   без поворота: тонкая ось — Z, мир y in [-1, 1];
        //   поворот 90° вокруг X: центр уезжает на y=-1.5, тонкая ось — Y,
        //                         мир y in [-1.6, -1.4], z in [-1, 1].
        // Луч на y=-1.5 попадает ТОЛЬКО в повёрнутый кусок: «блокирует»
        // появляется именно от применения кватерниона, а не от коробки как таковой.
        const vis::Vec3 ctrLocal{0, 0, 1.5f};
        const vis::Vec3 pivot{0, 0, 0};
        // Полуоси РАЗНЫЕ по всем осям: у куба поворот ничего бы не менял.
        const vis::Vec3 ext{2, 1, 0.1f};
        const vis::Quat noRot{0, 0, 0, 1};
        const vis::Quat pitch90 = vis::QuatNormalized(vis::Quat{0.70710678f, 0, 0, 0.70710678f});
        const vis::Vec3 rayFrom{0, -1.5f, -20}, rayTo{0, -1.5f, 20};

        std::vector<vis::Tri> flat;
        vis::AppendObb(ctrLocal, ext, pivot, noRot, 1.0f, flat);
        CHECK(flat.size() == 12, "obb is 12 triangles");
        vis::Bvh flat_bvh; flat_bvh.Build(flat);
        CHECK(!flat_bvh.Occluded(rayFrom, rayTo), "unrotated piece does not reach the ray");

        std::vector<vis::Tri> turned;
        vis::AppendObb(ctrLocal, ext, pivot, pitch90, 1.0f, turned);
        CHECK(turned.size() == 12, "rotated obb is still 12 triangles");
        vis::Bvh turned_bvh; turned_bvh.Build(turned);
        CHECK(turned_bvh.Occluded(rayFrom, rayTo), "the same piece turned by 90 deg blocks");
        {   // габариты разъехались: тонкая ось переехала с Z на Y, центр — на -1.5
            vis::AABB b;
            for (const auto& t : turned) b.Grow(t);
            CHECK((b.mx.y - b.mn.y) < 0.5f && (b.mx.z - b.mn.z) > 1.5f &&
                  (b.mx.x - b.mn.x) > 3.0f &&
                  std::fabs((b.mn.y + b.mx.y) * 0.5f + 1.5f) < 1e-3f,
                  "rotation moved the thin axis onto Y and carried the center");
        }

        // Центр/полуоси инстансятся вместе с остальным: AppendObb обязан уехать
        // на pos+center, а не остаться в начале координат.
        std::vector<vis::Tri> off;
        vis::AppendObb(vis::Vec3{0, 0, 0}, vis::Vec3{0.5f, 0.5f, 0.5f},
                       vis::Vec3{10, 20, 30}, noRot, 1.0f, off);
        vis::Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
        for (const auto& t : off) for (const vis::Vec3* v : {&t.a, &t.b, &t.c}) {
            lo = vis::Min3(lo, *v); hi = vis::Max3(hi, *v);
        }
        CHECK(std::fabs(lo.x - 9.5f) < 1e-3f && std::fabs(hi.x - 10.5f) < 1e-3f &&
              std::fabs(lo.y - 19.5f) < 1e-3f && std::fabs(hi.z - 30.5f) < 1e-3f,
              "obb lands on pos, not on the origin");

        // build()/rebuild_dynamic(): дерево из кусков и снятие дерева при
        // пустом списке — разгруженную базу нельзя продолжать считать стеной.
        std::vector<vis_dyn::Piece> pieces;
        vis_dyn::Piece p;
        p.pos = vis::Vec3{5, 0, 0};      // пластина перпендикулярна лучу rayZ
        p.rot = noRot;
        p.name = "woodwall";
        pieces.push_back(p);
        const vis::Vec3 rayZFrom{5, 0, -20}, rayZTo{5, 0, 20};
        size_t inst = 0, miss = 0;
        auto built = vis_dyn::build(pieces, lib, &inst, &miss);
        CHECK(inst == 1 && miss == 0 && built != nullptr, "one piece instanced");
        CHECK(built && built->TriCount() == 2, "piece keeps its own triangles");
        CHECK(built && built->Occluded(rayZFrom, rayZTo), "instanced piece blocks the ray");
        CHECK(vis_map::rebuild_dynamic(pieces, lib) == 1, "rebuild publishes the tree");
        CHECK(vis_dyn::tri_count() == 2 && vis_dyn::unmatched_count() == 0,
              "counters follow the rebuild");
        CHECK(vis_map::dyn_tree() && vis_map::dyn_tree()->Occluded(rayZFrom, rayZTo),
              "published tree is readable by the frame");

        pieces.clear();
        vis_dyn::Piece other;
        other.name = "stonewall";
        pieces.push_back(other);
        CHECK(vis_dyn::build(pieces, lib, &inst, &miss) == nullptr && inst == 0 && miss == 1,
              "unmatched piece adds no geometry");
        CHECK(vis_map::rebuild_dynamic(pieces, lib) == 0, "unmatched rebuild publishes nothing");
        CHECK(vis_map::dyn_tree() == nullptr && vis_dyn::tri_count() == 0,
              "empty list takes the tree down");

        // visible(): статика молчит (дерева нет), динамика перекрывает.
        // Неизвестность = «видна», поэтому первое решение обязано быть true,
        // а гасить цель можно только после kHiddenFramesToHide кадров.
        std::vector<vis::Tri> wallTris;
        vis::AppendObb(vis::Vec3{0, 0, 0}, ext, vis::Vec3{5, 0, 0}, noRot, 1.0f, wallTris);
        auto dyn = std::make_shared<vis::Bvh>();
        dyn->Build(wallTris);
        vis_map::set_tree_for_test(nullptr);
        vis_map::set_dyn_for_test(dyn);
        const float eye[3] = {rayZFrom.x, rayZFrom.y, rayZFrom.z};
        const float pts[1][3] = {{rayZTo.x, rayZTo.y, rayZTo.z}};
        vis_map::frame_begin();
        CHECK(vis_map::visible(30, eye, pts, 1) == true, "dyn-only frame A: hysteresis holds");
        vis_map::frame_begin();
        CHECK(vis_map::visible(30, eye, pts, 1) == false, "dyn-only occludes");

        // Статика есть, но НЕ перекрывает: решение всё равно «перекрыто»,
        // потому что хватает любого из двух деревьев.
        auto stat = std::make_shared<vis::Bvh>();
        std::vector<vis::Tri> far_wall;
        add_wall(far_wall, 500.f, -50.f, 50.f, 0.f, 200.f);
        stat->Build(far_wall);
        vis_map::set_tree_for_test(stat);
        vis_map::set_dyn_for_test(dyn);
        vis_map::frame_begin();
        CHECK(vis_map::visible(31, eye, pts, 1) == true, "two trees frame A");
        vis_map::frame_begin();
        CHECK(vis_map::visible(31, eye, pts, 1) == false,
              "clear static does not excuse a blocking dynamic");

        // Проверка по двум деревьям вместе: статика закрывает, динамика нет —
        // цель всё равно скрыта (достаточно ЛЮБОГО перекрытия).
        {
            std::vector<vis::Tri> nearWall;
            add_wall(nearWall, 5.f, -50.f, 50.f, -50.f, 50.f);   // плоскость x=5, ось Z
            auto stat2 = std::make_shared<vis::Bvh>();
            stat2->Build(nearWall);
            vis_map::set_tree_for_test(stat2);
            vis_map::set_dyn_for_test(nullptr);
            const float e2[3] = {-20, 0, 0};
            const float p2[1][3] = {{20, 0, 0}};
            vis_map::frame_begin();
            CHECK(vis_map::visible(32, e2, p2, 1) == true, "static-only frame A");
            vis_map::frame_begin();
            CHECK(vis_map::visible(32, e2, p2, 1) == false, "static-only occludes");
        }

        // Каталог префабов подхватывается фоном из <CfgDir>/pieces/ и
        // переопределяется директивой pieces= в боковом файле — тот же
        // vismap.txt, что задаёт путь меша.
        {
            const std::string cfg = std::string(temp_dir());
            const std::string side = cfg + "/vismap.txt";
            write_file(side, ("mesh=" + cfg + "/map.obj\npieces=" + dir + "\nradius=75\n").c_str());
            // ensure_path() уже отработал раньше на этом каталоге, поэтому
            // боковой файл перечитывается новым процессом; здесь проверяем
            // сам разбор директив через публичный путь.
            vis_map::apply_sidecar_line("radius=75");
            CHECK(vis_map::radius() == 75.f, "radius directive");
            vis_map::apply_sidecar_line("radius=1");
            CHECK(vis_map::radius() == 75.f, "absurd radius is rejected");
            vis_map::apply_sidecar_line("radius=99999");
            CHECK(vis_map::radius() == 75.f, "absurd radius is rejected (high)");
            vis_map::apply_sidecar_line(("pieces=" + dir).c_str());
            CHECK(std::string(vis_map::pieces_dir()) == dir, "pieces directive");
            vis_map::apply_sidecar_line("# comment");
            CHECK(std::string(vis_map::pieces_dir()) == dir, "comment line changes nothing");
        }

        // Обеих деревьев нет — проверка молчит и отдаёт «видна».
        vis_map::set_tree_for_test(nullptr);
        vis_map::set_dyn_for_test(nullptr);
        vis_map::frame_begin();
        CHECK(vis_map::visible(32, eye, pts, 1) == true, "no trees means unfiltered");
        CHECK(vis_map::status() == vis_map::VIS_STATUS_OFF, "test hook leaves status OFF");
    }

    if (failures == 0) printf("test_vis_check: all checks passed\n");
    else printf("test_vis_check: %d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
