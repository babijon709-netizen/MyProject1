// obj2bvh — сборка BVH карты видимости НА ПК: на телефоне остаётся только
// прочитать готовый файл (одно чтение вместо разбора .obj в секундах).
//
//   g++ -O2 -std=gnu++17 -I jni/src tools/vis_map/obj2bvh.cpp -o obj2bvh
//   ./obj2bvh map.obj            -> map.obj.bvh   (метры, оси как в меше)
//   ./obj2bvh map.obj 0.01       -> тот же файл для меша в сантиметрах
//   ./obj2bvh map.obj 1 1        -> меш Y-up, мир Z-up (Unity -> Z-up)
//
// Положить результат рядом с мешем на устройство: <CfgDir>/map.obj.bvh
// (<CfgDir> по умолчанию /data/local/benzware). Дерево самодостаточно: .obj на
// телефоне может и не лежать — тогда меш обновлять придётся вместе с .bvh.
// Формат один и тот же на ПК и на устройстве: little-endian POD (x86-64 ->
// arm64 переносится побайтово).
#include "vis_check.h"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: obj2bvh map.obj [scale] [yUpToZUp(0/1)]\n");
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
