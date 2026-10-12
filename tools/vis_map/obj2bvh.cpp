// obj2bvh — сборка BVH карты видимости НА ПК: на телефоне остаётся только
// прочитать готовый файл (одно чтение вместо разбора .obj в секундах).
//
//   g++ -O2 -std=gnu++17 -I jni/src tools/vis_map/obj2bvh.cpp -o obj2bvh
//   ./obj2bvh map.obj            -> map.obj.bvh   (метры, оси как в меше)
//   ./obj2bvh map.obj 0.01       -> тот же файл для меша в сантиметрах
//   ./obj2bvh map.obj 1 1        -> меш Y-up, мир Z-up (Unity -> Z-up)
//
// Проверка каталога префабов (динамика, постройки игроков):
//   ./obj2bvh --check-pieces pieces/ "WoodWall_LOD0" "Foundation Wood Damaged"
// Показывает, какие ключи получились из имён файлов и какой префаб найдётся для
// каждого указанного имени объекта из сцены. Тот же код, что на устройстве
// (vis_dyn::LoadDir / Library::Find), поэтому совпадение здесь = совпадение там.
//
// Положить результат рядом с мешем на устройство: <CfgDir>/map.obj.bvh
// (<CfgDir> по умолчанию /data/local/benzware). Дерево самодостаточно: .obj на
// телефоне может и не лежать — тогда меш обновлять придётся вместе с .bvh.
// Формат один и тот же на ПК и на устройстве: little-endian POD (x86-64 ->
// arm64 переносится побайтово).
#include "vis_check.h"
#include "vis_check_dyn.h"

#include <cstdio>
#include <cstdlib>
#include <string>

// Каталог префабов: что загрузилось и как сопоставляется с именами из сцены.
static int check_pieces(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: obj2bvh --check-pieces <dir> [scene name...]\n");
        return 2;
    }
    vis_dyn::Library lib;
    const size_t added = vis_dyn::LoadDir(lib, argv[2], 1.0f, false);
    std::printf("%s: %zu mesh(es), %zu triangle(s)\n", argv[2], added, lib.tri_total);
    if (added == 0) {
        std::printf("nothing loaded: нужны .obj в этом каталоге\n");
        return 1;
    }
    for (const auto& key : lib.keys) {
        const vis_dyn::Entry& e = lib.by_key[key];
        std::printf("  key %-28s file %-24s tris %-6zu ext (%.2f %.2f %.2f)\n",
                    e.key.c_str(), e.file.c_str(), e.tris.size(),
                    e.extents.x * 2.f, e.extents.y * 2.f, e.extents.z * 2.f);
    }
    for (int i = 3; i < argc; ++i) {
        const vis_dyn::Entry* e = lib.Find(argv[i]);
        std::printf("  find \"%s\" -> %s\n", argv[i], e ? e->file.c_str() : "NO MATCH");
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--check-pieces") return check_pieces(argc, argv);
    if (argc < 2) {
        std::printf("usage: obj2bvh map.obj [scale] [yUpToZUp(0/1)]\n"
                    "       obj2bvh --check-pieces <dir> [scene name...]\n");
        return 2;
    }
    const std::string in = argv[1];
    const float scale = (argc > 2) ? static_cast<float>(std::atof(argv[2])) : 1.0f;
    const bool  yup   = (argc > 3) && std::atoi(argv[3]) != 0;
    if (!(scale > 0.0f)) {
        std::printf("scale must be > 0\n");
        return 2;
    }

    std::vector<vis::Tri> tris;
    if (!vis::LoadObj(in, tris, scale, yup)) {
        std::printf("failed to load %s (no vertices/faces?)\n", in.c_str());
        return 1;
    }
    std::printf("triangles: %zu\n", tris.size());

    vis::Bvh bvh;
    bvh.Build(std::move(tris));
    if (bvh.Empty()) {
        std::printf("empty tree\n");
        return 1;
    }

    const std::string out = in + ".bvh";
    if (!bvh.SaveBinary(out.c_str())) {
        std::printf("failed to write %s\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s: %zu tris, %zu nodes\n", out.c_str(), bvh.TriCount(), bvh.NodeCount());
    return 0;
}
