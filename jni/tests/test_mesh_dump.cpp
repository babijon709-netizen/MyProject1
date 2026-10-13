// Хост-тесты рантайм-дампера мешей (mesh_dump.h). Память игры недоступна,
// поэтому «процесс» — арена байт, а Reader читает из неё: так проверяется ровно
// то, что на устройстве зависит от раскладки — поиск полей нативного Mesh,
// декод вершин/индексов в треугольники, валидация и запись .obj.
//
// Синтетический нативный Mesh повторяет форму настоящего VBO Unity: указатель
// на буфер вершин, рядом число вершин и ШАГ как поле, отдельный указатель на
// индексы и их число. Индексы — u32. Меш — коробка 8 вершин / 12 треугольников,
// вершины interleaved шагом 32 (позиция на 0, хвост — нули).
#include "mesh_dump.h"
#include "vis_check.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int failures = 0;
#define CHECK(cond, name) do { \
    if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", name, __LINE__); } \
} while (0)

// Арена: addr == индекс. 8 МБ хватает, чтобы разнести структуры и буферы.
struct Arena { std::vector<uint8_t> m; Arena(size_t n = 8u << 20) : m(n, 0) {} };

bool arena_read(void* ctx, uint64_t addr, void* out, size_t n) {
    Arena* a = (Arena*)ctx;
    if (addr + n > a->m.size()) return false;
    std::memcpy(out, a->m.data() + addr, n);
    return true;
}
meshdump::Reader reader_of(Arena& a) {
    meshdump::Reader r; r.read = arena_read; r.ctx = &a; return r;
}
void put_u64(Arena& a, uint64_t addr, uint64_t v) { std::memcpy(a.m.data() + addr, &v, 8); }
void put_u32(Arena& a, uint64_t addr, uint32_t v) { std::memcpy(a.m.data() + addr, &v, 4); }
void put_f32(Arena& a, uint64_t addr, float v) { std::memcpy(a.m.data() + addr, &v, 4); }

// Коробка: 8 вершин, 12 треугольников (36 индексов).
const float kBox[8][3] = {
    {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
    {-1, -1,  1}, {1, -1,  1}, {1, 1,  1}, {-1, 1,  1}};
const uint32_t kIdx[36] = {
    0,2,1, 0,3,2, 4,5,6, 4,6,7, 0,1,5, 0,5,4,
    1,2,6, 1,6,5, 2,3,7, 2,7,6, 3,0,4, 3,4,7};

// Собрать валидный нативный Mesh в арене; вернуть базу.
uint64_t build_mesh(Arena& a, uint64_t mb, uint64_t vb, uint64_t ib, uint32_t stride,
                    uint32_t vcount = 8, uint32_t icount = 36) {
    put_u64(a, mb + 0x40, vb);
    put_u32(a, mb + 0x48, vcount);
    put_u32(a, mb + 0x4C, stride);   // поле шага — ярус A поиска
    put_u64(a, mb + 0x60, ib);
    put_u32(a, mb + 0x68, icount);
    for (uint32_t i = 0; i < vcount; ++i) {
        put_f32(a, vb + (uint64_t)i * stride + 0, kBox[i][0]);
        put_f32(a, vb + (uint64_t)i * stride + 4, kBox[i][1]);
        put_f32(a, vb + (uint64_t)i * stride + 8, kBox[i][2]);
    }
    for (uint32_t i = 0; i < icount; ++i) put_u32(a, ib + (uint64_t)i * 4, kIdx[i]);
    return mb;
}
} // namespace

int main() {
    using namespace meshdump;

    // ---- 1. Поиск раскладки на валидном меше --------------------------------
    {
        Arena a;
        const uint64_t mb = build_mesh(a, 0x20000, 0x100000, 0x400000, 32);
        Reader rd = reader_of(a);
        Layout lay;
        std::vector<vis::Tri> tris;
        vis::Vec3 c, e;
        CHECK(DiscoverLayout(rd, mb, lay, tris, c, e), "layout discovered");
        CHECK(lay.valid, "layout marked valid");
        CHECK(lay.vbuf_ptr_off == 0x40, "vertex pointer offset");
        CHECK(lay.vcount_off == 0x48, "vertex count offset");
        CHECK(lay.vstride_off == 0x4C, "stride read from a field, not guessed");
        CHECK(lay.ibuf_ptr_off == 0x60, "index pointer offset");
        CHECK(lay.icount_off == 0x68, "index count offset");
        CHECK(lay.isize_fixed == 4, "index size u32");
        CHECK(tris.size() == 12, "12 triangles decoded");
        CHECK(std::fabs(e.x * 2.f - 2.f) < 1e-3f && std::fabs(e.y * 2.f - 2.f) < 1e-3f &&
              std::fabs(e.z * 2.f - 2.f) < 1e-3f, "box extents are 2x2x2");
        // Декод совпадает с исходной коробкой: первая грань 0,2,1.
        CHECK(std::fabs(tris[0].a.x - kBox[0][0]) < 1e-4f &&
              std::fabs(tris[0].b.z - kBox[2][2]) < 1e-4f, "vertices map back to the box");
    }

    // ---- 2. Шаг 12 (позиция без interleaving) тоже находится ----------------
    {
        Arena a;
        const uint64_t mb = build_mesh(a, 0x20000, 0x100000, 0x400000, 12);
        Reader rd = reader_of(a);
        Layout lay; std::vector<vis::Tri> tris; vis::Vec3 c, e;
        CHECK(DiscoverLayout(rd, mb, lay, tris, c, e), "tight stride discovered");
        CHECK(lay.vstride_off == 0x4C && tris.size() == 12, "tight stride decodes");
    }

    // ---- 3. Запись .obj и обратное чтение -----------------------------------
    {
        Arena a;
        const uint64_t mb = build_mesh(a, 0x20000, 0x100000, 0x400000, 32);
        Reader rd = reader_of(a);
        Layout lay; std::vector<vis::Tri> tris; vis::Vec3 c, e;
        CHECK(DiscoverLayout(rd, mb, lay, tris, c, e), "discover for obj");
        const std::string obj = WriteObj(tris, "WoodWall");
        CHECK(obj.find("v -1.00000 -1.00000 -1.00000") != std::string::npos, "obj has vertices");
        CHECK(obj.find("f 1 2 3") != std::string::npos, "obj has faces");
        CHECK(obj.find("# dumped at runtime") != std::string::npos, "obj has provenance header");
        // Сохраним и прочитаем тем же парсером, что грузит меш на устройстве.
        const char* path = "/tmp/xvcen_dump_test.obj";
        FILE* f = fopen(path, "w");
        CHECK(f != nullptr, "obj writable");
        if (f) { fwrite(obj.data(), 1, obj.size(), f); fclose(f); }
        std::vector<vis::Tri> back;
        CHECK(vis::LoadObj(path, back) && back.size() == 12, "obj round-trips to 12 tris");
        // Дедуп: один нативный меш -> один ключ, разные меши -> разные.
        CHECK(DedupKey(0xABC, 8, 36) == DedupKey(0xABC, 8, 36), "same mesh same key");
        CHECK(DedupKey(0xABC, 8, 36) != DedupKey(0xABC, 9, 36), "different vcount differs");
        CHECK(DedupKey(0xABC, 8, 36) != DedupKey(0xDEF, 8, 36), "different mesh differs");
    }

    // ---- 4. Мусор отвергается ------------------------------------------------
    {
        Arena a;
        // «Меш», у которого буфер вершин — весь 0xFF (NaN): конечных позиций нет.
        const uint64_t mb = 0x20000, vb = 0x100000, ib = 0x400000;
        put_u64(a, mb + 0x40, vb);
        put_u32(a, mb + 0x48, 8);
        put_u32(a, mb + 0x4C, 32);
        put_u64(a, mb + 0x60, ib);
        put_u32(a, mb + 0x68, 36);
        for (uint64_t i = 0; i < 8u * 32; ++i) a.m[vb + i] = 0xFF;   // NaN-позиции
        for (uint32_t i = 0; i < 36; ++i) put_u32(a, ib + (uint64_t)i * 4, kIdx[i]);
        Reader rd = reader_of(a);
        Layout lay; std::vector<vis::Tri> tris; vis::Vec3 c, e;
        CHECK(!DiscoverLayout(rd, mb, lay, tris, c, e), "NaN vertex buffer is rejected");
    }
    {
        Arena a;
        // Индексы вне диапазона вершин: декод обязан вернуть пусто.
        const uint64_t mb = build_mesh(a, 0x20000, 0x100000, 0x400000, 32);
        for (uint32_t i = 0; i < 36; ++i) put_u32(a, 0x400000 + (uint64_t)i * 4, 9999);
        Reader rd = reader_of(a);
        Layout lay; std::vector<vis::Tri> tris; vis::Vec3 c, e;
        CHECK(!DiscoverLayout(rd, mb, lay, tris, c, e), "out-of-range indices rejected");
    }

    // ---- 5. ApplyLayout: быстрый путь найденной раскладкой ------------------
    {
        Arena a;
        const uint64_t mb = build_mesh(a, 0x20000, 0x100000, 0x400000, 32);
        Reader rd = reader_of(a);
        Layout lay; std::vector<vis::Tri> tris; vis::Vec3 c, e;
        CHECK(DiscoverLayout(rd, mb, lay, tris, c, e), "discover for apply");
        std::vector<vis::Tri> again; vis::Vec3 c2, e2;
        CHECK(ApplyLayout(rd, mb, lay, again, c2, e2), "apply known layout");
        CHECK(again.size() == tris.size(), "apply gives the same triangle count");
        CHECK(std::fabs(c2.x - c.x) < 1e-4f && std::fabs(e2.z - e.z) < 1e-4f,
              "apply gives the same geometry");
        // Та же раскладка на НЕ-меше (мусорный указатель) обязана не сойтись —
        // это и есть дискриминатор «нативный Mesh vs что угодно ещё».
        Layout bad = lay; 
        std::vector<vis::Tri> none; vis::Vec3 c3, e3;
        CHECK(!ApplyLayout(rd, 0x700000, bad, none, c3, e3), "apply on non-mesh fails");
        Layout invalid;
        CHECK(!ApplyLayout(rd, mb, invalid, none, c3, e3), "invalid layout is refused");
    }

    // ---- 6. Декод/валидация как чистые функции ------------------------------
    {
        // icount не кратен 3 -> пусто.
        std::vector<uint8_t> vb(8 * 12), ib(6 * 4);
        for (uint32_t i = 0; i < 8; ++i) {
            const float p[3] = {kBox[i][0], kBox[i][1], kBox[i][2]};
            std::memcpy(vb.data() + i * 12, p, 12);
        }
        for (uint32_t i = 0; i < 6; ++i) { const uint32_t v = kIdx[i]; std::memcpy(ib.data() + i * 4, &v, 4); }
        CHECK(DecodeTris(vb.data(), 8, 12, 0, ib.data(), 5, 4).empty(), "icount%3 != 0 -> empty");
        CHECK(DecodeTris(vb.data(), 8, 12, 0, ib.data(), 6, 3).empty(), "bad isize -> empty");
        CHECK(DecodeTris(vb.data(), 8, 8, 0, ib.data(), 6, 4).empty(), "stride<12 -> empty");
        CHECK(DecodeTris(vb.data(), 8, 12, 0, ib.data(), 2, 4).empty(), "icount<kMinIndices -> empty");
        CHECK(DecodeTris(vb.data(), 8, 12, 0, ib.data(), 6, 4).size() == 2, "clean decode gives 2 tris");
        std::vector<vis::Tri> empty;
        vis::Vec3 c, e;
        CHECK(!DecodeValid(empty, c, e), "empty mesh invalid");
    }

    // ---- 7. Перечисление сцены и запекание в мир ----------------------------
    {
        Arena a;
        Reader rd = reader_of(a);
        const uint64_t indexOff = 0x10;              // стенд для TRANSFORM_HIERARCHY_INDEX
        // 5 трансформов: у каждого по indexOff лежит его собственный индекс.
        uint64_t X[5];
        for (int32_t k = 0; k < 5; ++k) {
            X[k] = 0x10000 + (uint64_t)k * 0x100;   // >= 0x10000: проходит looks_like_heap_ptr
            put_u32(a, X[k] + indexOff, (uint32_t)k);
        }
        // transforms[]: TA[k*8] == X[k].
        const uint64_t TA = 0x100000;
        for (int32_t k = 0; k < 5; ++k) put_u64(a, TA + (uint64_t)k * 8, X[k]);
        // data: декой-поле и настоящее поле-указатель на TA.
        const uint64_t D = 0x200000;
        put_u64(a, D + 0x08, 0x300000);              // декой: не совпадёт с якорем
        put_u64(a, D + 0x28, TA);                    // настоящее поле
        const int32_t anchorI = 2;
        const uint64_t anchorT = X[2];

        uint64_t found = 0;
        CHECK(FindTransformsArray(rd, D, anchorT, anchorI, found) && found == TA,
              "FindTransformsArray: прямой массив найден по якорю");
        CHECK(CountTransforms(rd, TA, indexOff, 100, 2) == 5,
              "CountTransforms: 5 связных трансформов");
        CHECK(SceneArraysLookValid(rd, TA, indexOff, anchorI, 1),
              "SceneArraysLookValid: якорь самопроверяется");

        // Косвенный вариант: data -> структура -> массив.
        const uint64_t D2 = 0x210000, S = 0x220000;
        put_u64(a, S + 0x00, TA);                    // S[0] = TA (массив)
        put_u64(a, D2 + 0x40, S);                    // data-поле -> S
        uint64_t found2 = 0;
        CHECK(FindTransformsArray(rd, D2, anchorT, anchorI, found2) && found2 == TA,
              "FindTransformsArray: один уровень косвенности");

        // Мусорный якорь: массив не находится.
        uint64_t nope = 0;
        CHECK(!FindTransformsArray(rd, D, 0xDEADBEEF, anchorI, nope),
              "FindTransformsArray: чужой якорь не совпадает");

        // Запекание в мир: сдвиг + масштаб, сквозной счётчик вершин.
        std::vector<vis::Tri> local = { vis::Tri{ {0,0,0}, {1,0,0}, {0,1,0} } };
        std::string obj = "# static map\n";
        uint32_t vbase = 0;
        const vis::Vec3 pos{10, 20, 30};
        const vis::Quat ident{0, 0, 0, 1};
        AppendWorldObj(obj, local, pos, ident, 1.0f, vbase);
        CHECK(vbase == 3, "AppendWorldObj: vbase вырос на 3");
        CHECK(obj.find("v 10.00000 20.00000 30.00000") != std::string::npos, "вершина сдвинута в мир");
        CHECK(obj.find("f 1 2 3") != std::string::npos, "грань ссылается на свои вершины");
        // Второй инстанс: тот же меш, другая позиция, scale=2, сквозная индексация.
        AppendWorldObj(obj, local, vis::Vec3{0,0,0}, ident, 2.0f, vbase);
        CHECK(vbase == 6, "AppendWorldObj: второй инстанс -> vbase 6");
        CHECK(obj.find("f 4 5 6") != std::string::npos, "вторая грань со сдвигом");
        CHECK(obj.find("v 2.00000 0.00000 0.00000") != std::string::npos, "scale=2 применён");
        // Обратное чтение тем же парсером, что грузит map.obj на устройстве.
        const char* path = "/tmp/xvcen_static_test.obj";
        FILE* f = fopen(path, "w");
        CHECK(f != nullptr, "static obj writable");
        if (f) { fwrite(obj.data(), 1, obj.size(), f); fclose(f); }
        std::vector<vis::Tri> back;
        CHECK(vis::LoadObj(path, back) && back.size() == 2, "world obj round-trips to 2 tris");
        if (back.size() == 2) {
            CHECK(std::fabs(back[0].a.x - 10.f) < 1e-3f && std::fabs(back[0].a.z - 30.f) < 1e-3f,
                  "первый инстанс в мире на (10,20,30)");
            CHECK(std::fabs(back[1].b.x - 2.f) < 1e-3f, "второй инстанс масштабирован x2");
        }
    }

    // ---- 8. Террейн: открытие раскладки и построение сетки ------------------
    {
        Arena a;
        Reader rd = reader_of(a);
        const uint64_t td = 0x300000;   // TerrainData
        const uint64_t hp = 0x400000;   // массив высот
        const int32_t  R  = 65;         // 2^6+1
        put_u32(a, td + 0x14, (uint32_t)R);            // разрешение
        put_f32(a, td + 0x30, 512.0f);                 // size.x (метры)
        put_f32(a, td + 0x34, 200.0f);                 // size.y (масштаб высоты)
        put_f32(a, td + 0x38, 512.0f);                 // size.z (метры)
        put_u64(a, td + 0x48, hp);                     // указатель на высоты
        for (int32_t r = 0; r < R; ++r)
            for (int32_t c = 0; c < R; ++c)
                put_f32(a, hp + (uint64_t)(r * R + c) * 4,
                        0.5f + 0.4f * std::sin(r * 0.2f) * std::cos(c * 0.2f));  // [0.1,0.9]

        TerrainLayout lay;
        CHECK(DiscoverTerrain(rd, td, lay), "DiscoverTerrain на синтетическом TerrainData");
        CHECK(lay.valid && lay.res == R, "террейн: разрешение 65");
        CHECK(lay.res_off == 0x14 && lay.size_off == 0x30 && lay.heights_off == 0x48,
              "террейн: смещения res/size/heights");
        vis::Vec3 size{};
        CHECK(TerrainSize(rd, td, lay, size) &&
              std::fabs(size.x - 512.f) < 1e-3f && std::fabs(size.y - 200.f) < 1e-3f,
              "террейн: size читается");

        std::vector<vis::Tri> grid;
        const uint64_t heights = rd.ptr(td + lay.heights_off);
        CHECK(BuildTerrainGrid(rd, heights, lay.res, size, grid), "BuildTerrainGrid строит сетку");
        CHECK(grid.size() == (size_t)(R - 1) * (R - 1) * 2, "террейн: 64*64*2 треугольника");
        // Локальные координаты в пределах [0,size.x]×[0,size.z], y = h*size.y.
        bool inrange = !grid.empty();
        for (const auto& t : grid)
            for (const vis::Vec3* v : {&t.a, &t.b, &t.c})
                if (v->x < -1e-3f || v->x > size.x + 1e-3f ||
                    v->z < -1e-3f || v->z > size.z + 1e-3f ||
                    v->y < -1e-3f || v->y > size.y + 1e-3f) inrange = false;
        CHECK(inrange, "террейн: сетка в пределах размера");

        // Запекание в мир и обратное чтение.
        std::string obj = "# terrain\n";
        uint32_t vbase = 0;
        AppendWorldObj(obj, grid, vis::Vec3{1000, 0, 1000}, vis::Quat{0,0,0,1}, 1.0f, vbase);
        CHECK(vbase == grid.size() * 3, "террейн: vbase = 3*треугольника");
        const char* path = "/tmp/xvcen_terrain_test.obj";
        FILE* f = fopen(path, "w");
        CHECK(f != nullptr, "terrain obj writable");
        if (f) { fwrite(obj.data(), 1, obj.size(), f); fclose(f); }
        std::vector<vis::Tri> back;
        CHECK(vis::LoadObj(path, back) && back.size() == grid.size(), "террейн: round-trip");
        if (!back.empty())
            CHECK(std::fabs(back[0].a.x - 1000.f) < 1e-2f, "террейн: сдвинут в мир на 1000");

        // Отрицательные гейты: плоские высоты и неровное разрешение отвергаются.
        {
            Arena b;
            Reader rb = reader_of(b);
            const uint64_t td2 = 0x300000, hp2 = 0x400000;
            put_u32(b, td2 + 0x14, 65u);
            put_f32(b, td2 + 0x30, 512.f); put_f32(b, td2 + 0x34, 200.f); put_f32(b, td2 + 0x38, 512.f);
            put_u64(b, td2 + 0x48, hp2);
            for (int32_t i = 0; i < 65 * 65; ++i) put_f32(b, hp2 + (uint64_t)i * 4, 0.5f);  // плоско
            TerrainLayout l2;
            CHECK(!DiscoverTerrain(rb, td2, l2), "террейн: плоские высоты отвергнуты");
        }
        {
            Arena c;
            Reader rc = reader_of(c);
            const uint64_t td3 = 0x300000;
            put_u32(c, td3 + 0x14, 64u);   // не 2^n+1
            TerrainLayout l3;
            CHECK(!DiscoverTerrain(rc, td3, l3), "террейн: разрешение 64 отвергнуто");
        }
    }

    if (failures == 0) printf("test_mesh_dump: all checks passed\n");
    else printf("test_mesh_dump: %d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
