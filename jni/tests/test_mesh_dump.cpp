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

    if (failures == 0) printf("test_mesh_dump: all checks passed\n");
    else printf("test_mesh_dump: %d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
