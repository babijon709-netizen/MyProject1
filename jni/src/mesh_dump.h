#pragma once
// mesh_dump.h — рантайм-дампер мешей: ядро (чистое, std-only, host-тестируемое).
//
// Зачем. Проверке видимости нужны .obj построек. Собирать их на ПК из
// AssetRipper — отдельный ручной шаг на каждый билд игры. Этот дампер читает
// геометрию прямо из памяти работающей игры и пишет те же .obj, которыми
// питается vis_dyn::Library (см. vis_check_dyn.h) и карта vis_map.
//
// Игра — Unity 6000.3.18f1 (ревизия 5ebeb53e4c07), il2cpp. Версия снята из
// .note.unity в libunity.so (tools/unity_rev/NOTES_VIS_DYN.md). Что это даёт и
// чего НЕ даёт:
//   * managed-сторона решаема: MeshFilter/MeshRenderer -> managed Mesh ->
//     native Mesh (m_CachedPtr, MANAGED_CACHED_PTR=0x10, managed_object_native).
//   * нативная раскладка VBO внутри C++-объекта Mesh (указатель на буфер
//     вершин, число вершин, шаг, буфер индексов, их число и размер) НЕ лежит ни
//     в dump.cs, ни в il2cpp-метаданных, а символы/RTTI из libunity.so сняты.
//     Поэтому её нельзя захардкодить без догадки — а догаданные смещения в этот
//     проект не попадают.
//
// Отсюда решение: раскладка ОБНАРУЖИВАЕТСЯ эмпирически, ровно как проект уже
// находит раскладку иерархии трансформов (discover_layout_from_native_transforms).
// Кандиdat годен, только если декодированные вершины проходят жёсткую проверку
// (DecodeValid): конечные координаты, правдоподобный габарит, индексы в диапазоне
// [0, vcount), число индексов кратно 3 и >= 3, почти нет вырожденных
// треугольников. Случайный указатель на кучу такую проверку не проходит.
// Найденная раскладка кэшируется на процесс (билд Unity один — значит и
// раскладка одна на все меши) и логируется, чтобы её можно было закрепить
// именованным блоком на следующий билд.
//
// Этот файл НЕ читает память игры напрямую: поиск и декод идут через
// шаблонизированный Reader, поэтому всё проверяется на хосте синтетическим
// «процессом» (см. jni/tests/test_mesh_dump.cpp). На устройстве Reader — это
// rd_ptr/rd_buf (см. mesh_dump_scan.cpp).

#include "vis_check.h"   // Vec3, Tri, AABB, LocalBounds

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace meshdump {

// ---- Найденная раскладка нативного Mesh ------------------------------------
// Все смещения — от базы нативного объекта Mesh (того, на что указывает
// m_CachedPtr managed-обёртки). Нули = «не найдено».
struct Layout {
    // Буфер вершин: указатель на данные, число вершин, шаг вершины в байтах и
    // смещение позиции (Vector3) внутри шага (Unity хранит interleaved:
    // position, normal, tangent, uv...). Позиция почти всегда на 0.
    uint32_t vbuf_ptr_off   = 0;   // -> void* (указатель на массив вершин)
    uint32_t vcount_off     = 0;   // -> uint32 (число вершин)
    uint32_t vstride_off    = 0;   // -> uint32 (шаг вершины, байт); 0 = фикс. stride
    uint32_t vstride_fixed  = 0;   // если vstride_off==0, использовать это
    uint32_t vpos_sub_off   = 0;   // смещение Vector3 внутри шага (обычно 0)
    // Буфер индексов.
    uint32_t ibuf_ptr_off   = 0;   // -> void*
    uint32_t icount_off     = 0;   // -> uint32 (число индексов)
    uint32_t isize_off      = 0;   // -> uint32 (2 или 4); 0 = фикс. isize_fixed
    uint32_t isize_fixed    = 0;   // 2 (u16) или 4 (u32)
    bool valid = false;
};

// Ограничения поиска/декода. Верхняя грань числа вершин — не «меш карты», а
// здравый смысл: меш постройки — тысячи вершин, не миллионы.
inline constexpr uint32_t kMinVerts   = 3;
inline constexpr uint32_t kMaxVerts   = 400000;
inline constexpr uint32_t kMinIndices = 3;
inline constexpr uint32_t kMaxIndices = 3000000;
inline constexpr uint32_t kMinStride  = 12;   // минимум: одна Vector3
inline constexpr uint32_t kMaxStride  = 256;  // с запасом на все UV-наборы
inline constexpr float    kMaxExtent  = 2000.0f;   // метры; больше — не меш, а мусор
inline constexpr float    kMinExtent  = 0.005f;
inline constexpr float    kMaxDegenerateRatio = 0.5f;  // не более половины нулевых

// ---- Reader ----------------------------------------------------------------
// Читатель памяти процесса. На устройстве — rd_ptr/rd_buf; на хосте — карта
// байт. Поиск и декод ничего не знают про транспорт.
struct Reader {
    // Прочитать size байт по адресу addr в out. false = не читается.
    bool (*read)(void* ctx, uint64_t addr, void* out, size_t size) = nullptr;
    void* ctx = nullptr;
    uint64_t ptr(uint64_t addr) const {
        uint64_t v = 0;
        if (read && read(ctx, addr, &v, sizeof(v))) return v;
        return 0;
    }
    bool u32(uint64_t addr, uint32_t& out) const {
        return read && read(ctx, addr, &out, sizeof(out));
    }
    bool bytes(uint64_t addr, void* out, size_t n) const {
        return read && read(ctx, addr, out, n);
    }
};

inline bool looks_like_heap_ptr(uint64_t p) {
    return p >= 0x10000 && p < 0x0001000000000000ULL;
}

// ---- Декод вершин/индексов в треугольники ----------------------------------
// raw_v: vcount вершин шагом stride, позиция на pos_sub (Vector3, float).
// raw_i: icount индексов размером isize (2 или 4). Возвращает локальные
// треугольники. Пусто — если параметры несовместны или буфер не читается.
inline std::vector<vis::Tri> DecodeTris(const uint8_t* raw_v, uint32_t vcount, uint32_t stride,
                                        uint32_t pos_sub,
                                        const uint8_t* raw_i, uint32_t icount, uint32_t isize) {
    std::vector<vis::Tri> out;
    if (!raw_v || !raw_i || vcount < kMinVerts || vcount > kMaxVerts) return out;
    if (stride < kMinStride || stride > kMaxStride) return out;
    if (pos_sub + 12 > stride) return out;
    if (isize != 2 && isize != 4) return out;
    if (icount < kMinIndices || icount > kMaxIndices || (icount % 3) != 0) return out;

    auto vert = [&](uint32_t i, vis::Vec3& v) -> bool {
        if (i >= vcount) return false;
        const uint8_t* p = raw_v + (size_t)i * stride + pos_sub;
        float f[3];
        std::memcpy(f, p, sizeof(f));
        v = vis::Vec3{f[0], f[1], f[2]};
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    };
    auto index = [&](uint32_t k) -> uint32_t {
        if (isize == 2) { uint16_t v; std::memcpy(&v, raw_i + (size_t)k * 2, 2); return v; }
        uint32_t v; std::memcpy(&v, raw_i + (size_t)k * 4, 4); return v;
    };

    out.reserve(icount / 3);
    for (uint32_t k = 0; k + 2 < icount; k += 3) {
        const uint32_t ia = index(k), ib = index(k + 1), ic = index(k + 2);
        if (ia >= vcount || ib >= vcount || ic >= vcount) return {};   // мусорный индекс
        vis::Vec3 a, b, c;
        if (!vert(ia, a) || !vert(ib, b) || !vert(ic, c)) return {};   // нечитаемая/неконечная
        out.push_back({a, b, c});
    }
    return out;
}

// Жёсткая проверка декодированного меша. Ею отсеиваются случайные указатели:
// у настоящего меша габарит правдоподобный, индексы в диапазоне, и почти все
// треугольники невырожденные. Возвращает true и заполняет center/extents.
inline bool DecodeValid(const std::vector<vis::Tri>& tris, vis::Vec3& center, vis::Vec3& extents) {
    if (tris.empty()) return false;
    if (!vis::LocalBounds(tris.data(), tris.size(), center, extents)) return false;
    for (int i = 0; i < 3; ++i) {
        const float e = extents[i] * 2.0f;
        if (!(e >= 0.0f) || e > kMaxExtent) return false;
    }
    // Хотя бы одна ось должна быть ненулевой (плоская пластина допустима: две оси).
    const float maxe = std::fmax(extents.x, std::fmax(extents.y, extents.z)) * 2.0f;
    if (maxe < kMinExtent) return false;
    // Вырожденные треугольники (нулевая площадь) — признак неверного шага/порядка.
    size_t degen = 0;
    for (const auto& t : tris) {
        const vis::Vec3 n = vis::Cross(t.b - t.a, t.c - t.a);
        if (vis::Length(n) < 1e-9f) ++degen;
    }
    if ((float)degen > (float)tris.size() * kMaxDegenerateRatio) return false;
    return true;
}

// ---- Поиск раскладки в нативном Mesh ---------------------------------------
// Сканируем первые scan_bytes нативного объекта в поисках указателя на буфер
// вершин, рядом — числа вершин и шага, затем указателя на индексы и их числа.
// Каждая комбинация немедленно декодируется и проверяется (DecodeValid); первая
// прошедшая — ответ. Стоимость ограничена (runs once per distinct mesh), а
// ложные срабатывания отсекает валидация.
//
// Вершины читаем одним блоком (cap_vert_bytes), индексы — одним блоком: транспорт
// сам режет запрос по страницам, но держать гигабайты нельзя, поэтому потолок.
inline constexpr size_t kScanBytes     = 0x400;    // область нативного Mesh, где ищем поля
inline constexpr size_t kCapVertBytes  = 48u << 20; // 48 МБ вершин максимум
inline constexpr size_t kCapIndexBytes = 24u << 20; // 24 МБ индексов максимум
inline constexpr uint32_t kFieldWin    = 0x40;     // окно поиска count/stride вокруг указателя

// Прочитать весь буфер по адресу/числу/шагу. false, если размер absurd или не
// читается. caller задаёт предел в байтах.
inline bool ReadBuffer(const Reader& rd, uint64_t addr, size_t bytes, std::vector<uint8_t>& out,
                       size_t cap) {
    if (addr == 0 || bytes == 0 || bytes > cap) return false;
    out.resize(bytes);
    return rd.bytes(addr, out.data(), bytes);
}

// Правдоподобные шаги вершины (Unity interleaved: position + normal + tangent +
// UV-наборы). Фиксированный набор вместо «любое u32 в окне» режет число
// комбинаций на порядки и не даёт мусорному полю сойти за шаг.
inline const uint32_t kStrideSet[] = {12, 16, 20, 24, 28, 32, 36, 40, 44, 48,
                                      52, 56, 60, 64, 72, 80, 96, 128};
inline constexpr size_t kStrideSetN = sizeof(kStrideSet) / sizeof(kStrideSet[0]);

// Потолок попыток полного декода: поиск идёт один раз на нативный меш и
// кэшируется, но и разовый проход не должен подвешивать кнопку дампа.
inline constexpr int kMaxDecodeAttempts = 4096;

// Превалидация ТОЛЬКО вершин (без индексов): прочитать буфер, снять габарит по
// позициям, отсеять бесконечное/абсурдное. Дёшево относительно полного декода и
// отсекает почти все неверные (vcount, stride) ещё до перебора индексов.
inline bool VerticesPlausible(const Reader& rd, uint64_t vptr, uint32_t vcount, uint32_t stride,
                              std::vector<uint8_t>& vbuf) {
    if (!ReadBuffer(rd, vptr, (size_t)vcount * stride, vbuf, kCapVertBytes)) return false;
    vis::Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (uint32_t i = 0; i < vcount; ++i) {
        float f[3];
        std::memcpy(f, vbuf.data() + (size_t)i * stride, sizeof(f));
        if (!std::isfinite(f[0]) || !std::isfinite(f[1]) || !std::isfinite(f[2])) return false;
        lo = vis::Min3(lo, vis::Vec3{f[0], f[1], f[2]});
        hi = vis::Max3(hi, vis::Vec3{f[0], f[1], f[2]});
    }
    const vis::Vec3 ext = hi - lo;
    const float maxe = std::fmax(ext.x, std::fmax(ext.y, ext.z));
    return maxe >= kMinExtent && maxe <= kMaxExtent;
}

// Применить ИЗВЕСТНУЮ раскладку к нативному Mesh и декодировать. Это быстрый
// путь: раскладка в пределах одного билда Unity одна на все меши, поэтому после
// первой сходимости DiscoverLayout каждый следующий меш читается без поиска —
// и заодно сам служит дискриминатором («это нативный Mesh, если ApplyLayout на
// нём сошёлся и DecodeValid прошёл»).
inline bool ApplyLayout(const Reader& rd, uint64_t mesh, const Layout& lay,
                        std::vector<vis::Tri>& tris, vis::Vec3& center, vis::Vec3& extents) {
    if (!lay.valid || mesh == 0) return false;
    const uint64_t vptr = rd.ptr(mesh + lay.vbuf_ptr_off);
    if (!looks_like_heap_ptr(vptr)) return false;
    uint32_t vcount = 0;
    if (!rd.u32(mesh + lay.vcount_off, vcount)) return false;
    if (vcount < kMinVerts || vcount > kMaxVerts) return false;
    uint32_t stride = lay.vstride_fixed;
    if (lay.vstride_off) { if (!rd.u32(mesh + lay.vstride_off, stride)) return false; }
    if (stride < kMinStride || stride > kMaxStride) return false;
    if ((size_t)vcount * stride > kCapVertBytes) return false;

    const uint64_t iptr = rd.ptr(mesh + lay.ibuf_ptr_off);
    if (!looks_like_heap_ptr(iptr)) return false;
    uint32_t icount = 0;
    if (!rd.u32(mesh + lay.icount_off, icount)) return false;
    if (icount < kMinIndices || icount > kMaxIndices || (icount % 3) != 0) return false;
    uint32_t isize = lay.isize_fixed;
    if (lay.isize_off) { uint32_t v = 0; if (rd.u32(mesh + lay.isize_off, v) && (v == 2 || v == 4)) isize = v; }
    if (isize != 2 && isize != 4) return false;
    if ((size_t)icount * isize > kCapIndexBytes) return false;

    std::vector<uint8_t> vb, ib;
    if (!ReadBuffer(rd, vptr, (size_t)vcount * stride, vb, kCapVertBytes)) return false;
    if (!ReadBuffer(rd, iptr, (size_t)icount * isize, ib, kCapIndexBytes)) return false;
    tris = DecodeTris(vb.data(), vcount, stride, lay.vpos_sub_off, ib.data(), icount, isize);
    return DecodeValid(tris, center, extents);
}

// Полный поиск раскладки. Двухфазный, чтобы не читать буферы впустую:
//   фаза 1 — кандидаты-указатели на кучу в голове объекта + правдоподобные
//            (vcount, stride) рядом; каждый проверяется превалидацией вершин и
//            попадает в короткий список;
//   фаза 2 — для каждого прошедшего кандидата вершин перебираются кандидаты
//            индексов, полный декод и DecodeValid.
// Возвращает первую валидную раскладку и треугольники. Порядок размера индекса
// — u32 раньше u16 (иначе u16-трактовка u32-буфера может случайно пройти).
inline bool DiscoverLayout(const Reader& rd, uint64_t mesh, Layout& layout,
                           std::vector<vis::Tri>& tris, vis::Vec3& center, vis::Vec3& extents) {
    if (mesh == 0) return false;
    std::vector<uint8_t> head(kScanBytes);
    if (!rd.bytes(mesh, head.data(), head.size())) return false;

    // Кандидаты-указатели на кучу в голове объекта.
    std::vector<uint32_t> ptrs;
    for (uint32_t off = 8; off + 8 <= kScanBytes; off += 8) {
        uint64_t p = 0;
        std::memcpy(&p, head.data() + off, 8);
        if (looks_like_heap_ptr(p)) ptrs.push_back(off);
    }
    if (ptrs.size() < 2) return false;   // нужны минимум вершины и индексы

    auto u32_at = [&](uint32_t off, uint32_t& out) -> bool {
        if (off + 4 > kScanBytes) return false;
        std::memcpy(&out, head.data() + off, 4);
        return true;
    };

    // Фаза 1: короткий список кандидатов вершин. Шаг вершины в настоящем VBO
    // Unity — это ПОЛЕ, поэтому ярус A читает его из окна рядом с vcount; ярус B
    // (фиксированный набор) — только запасной, когда поля не нашлось. Порядок
    // важен: угаданный шаг может дать misaligned-вершины, которые всё ещё
    // конечны и проходят валидацию, поэтому полевой кандидат пробуется первым.
    struct VCand { uint32_t vo, vc, vs, stride; uint64_t vptr; };
    std::vector<VCand> field_cands, guess_cands;
    std::vector<uint8_t> vbuf;
    for (uint32_t vo : ptrs) {
        uint64_t vptr = 0;
        std::memcpy(&vptr, head.data() + vo, 8);
        for (uint32_t vc = vo; vc + 4 <= kScanBytes && vc < vo + kFieldWin; vc += 4) {
            uint32_t vcount = 0;
            if (!u32_at(vc, vcount)) continue;
            if (vcount < kMinVerts || vcount > kMaxVerts) continue;
            // Ярус A: шаг как поле в окне рядом с vcount.
            bool found_field = false;
            for (uint32_t vs = vc; vs + 4 <= kScanBytes && vs < vc + kFieldWin; vs += 4) {
                if (vs == vc) continue;
                uint32_t stride = 0;
                if (!u32_at(vs, stride)) continue;
                if (stride < kMinStride || stride > kMaxStride) continue;
                if ((size_t)vcount * stride > kCapVertBytes) continue;
                if (!VerticesPlausible(rd, vptr, vcount, stride, vbuf)) continue;
                field_cands.push_back({vo, vc, vs, stride, vptr});
                found_field = true;
                break;
            }
            if (found_field) continue;
            // Ярус B: поле шага не найдено — пробуем правдоподобный набор.
            for (size_t si = 0; si < kStrideSetN; ++si) {
                const uint32_t stride = kStrideSet[si];
                if ((size_t)vcount * stride > kCapVertBytes) continue;
                if (!VerticesPlausible(rd, vptr, vcount, stride, vbuf)) continue;
                guess_cands.push_back({vo, vc, 0, stride, vptr});
            }
        }
    }
    std::vector<VCand> vcands;
    vcands.reserve(field_cands.size() + guess_cands.size());
    for (const auto& c : field_cands) vcands.push_back(c);
    for (const auto& c : guess_cands) vcands.push_back(c);
    if (vcands.size() > 96) vcands.resize(96);
    if (vcands.empty()) return false;

    // Фаза 2: индексы + полный декод (через ApplyLayout — та же проверка, что и
    // на быстром пути, поэтому раскладка не «двоится» между поиском и применением).
    int attempts = 0;
    for (const auto& vcd : vcands) {
        for (uint32_t io : ptrs) {
            if (io == vcd.vo) continue;
            for (uint32_t ic = io; ic + 4 <= kScanBytes && ic < io + kFieldWin; ic += 4) {
                uint32_t icount = 0;
                if (!u32_at(ic, icount)) continue;
                if (icount < kMinIndices || icount > kMaxIndices || (icount % 3) != 0) continue;
                for (uint32_t isize : {4u, 2u}) {
                    if (++attempts > kMaxDecodeAttempts) return false;
                    Layout cand{};
                    cand.vbuf_ptr_off = vcd.vo;
                    cand.vcount_off = vcd.vc;
                    if (vcd.vs) cand.vstride_off = vcd.vs;   // шаг прочитан из поля
                    else        cand.vstride_fixed = vcd.stride;  // запасной набор
                    cand.vpos_sub_off = 0;
                    cand.ibuf_ptr_off = io;
                    cand.icount_off = ic;
                    cand.isize_fixed = isize;
                    cand.valid = true;
                    std::vector<vis::Tri> t;
                    if (ApplyLayout(rd, mesh, cand, t, center, extents)) {
                        layout = cand;
                        tris = std::move(t);
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

// ---- Запись .obj -----------------------------------------------------------
// Пишем локальные координаты (инстанс в мир делает vis_dyn по TRS куска). Формат
// минимальный: только v и f, как понимает vis::LoadObj. Возвращает текст.
inline std::string WriteObj(const std::vector<vis::Tri>& tris, const char* comment) {
    std::string s;
    s.reserve(tris.size() * 96 + 128);
    s += "# dumped at runtime from Unity 6000.3.18f1 (il2cpp), external read\n";
    if (comment && *comment) { s += "# "; s += comment; s += "\n"; }
    s += "# local coordinates; vis_dyn instances by the piece transform\n";
    // вершины
    char buf[96];
    for (const auto& t : tris) {
        for (const vis::Vec3* v : {&t.a, &t.b, &t.c}) {
            std::snprintf(buf, sizeof(buf), "v %.5f %.5f %.5f\n", v->x, v->y, v->z);
            s += buf;
        }
    }
    // грани: индексация 1-based, каждый треугольник — своя тройка вершин
    uint32_t base = 1;
    for (size_t i = 0; i < tris.size(); ++i, base += 3) {
        std::snprintf(buf, sizeof(buf), "f %u %u %u\n", base, base + 1, base + 2);
        s += buf;
    }
    return s;
}

// Ключ дедупликации: один и тот же нативный Mesh переиспользуется многими
// экземплярами префаба (все деревянные стены — один меш). Дампим по одному .obj
// на нативный меш, а не на каждый кусок в мире.
inline std::string DedupKey(uint64_t native_mesh, uint32_t vcount, uint32_t icount) {
    char b[64];
    std::snprintf(b, sizeof(b), "%llx_%u_%u",
                  (unsigned long long)native_mesh, vcount, icount);
    return std::string(b);
}

// ---- Перечисление всей сцены (статика) ------------------------------------
// Иерархия трансформов Unity общая на сцену: из любого нативного Transform
// достаются data (структура иерархии) и index этого трансформа в ней. Внутри
// data лежит массив указателей на трансформы, пронумерованный тем же индексом.
// Его смещение заранее неизвестно (нет dump.cs), но якорь самопроверяемый:
// transforms[index_нашего] == наш_transform. Ищем поле-указатель в data, у
// которого slot[index] совпал с известным трансформом (прямо или через один
// уровень косвенности — как matrices/indices в некоторых сборках).
inline bool FindTransformsArray(const Reader& rd, uint64_t data, uint64_t anchorT,
                                int32_t anchorI, uint64_t& out) {
    if (!looks_like_heap_ptr(data) || !looks_like_heap_ptr(anchorT)) return false;
    if (anchorI < 0 || anchorI > 1000000) return false;
    const uint64_t slot = (uint64_t)anchorI * 8u;
    for (uint32_t off = 0; off + 8 <= kScanBytes; off += 8) {
        const uint64_t p = rd.ptr(data + off);
        if (!looks_like_heap_ptr(p)) continue;
        if (rd.ptr(p + slot) == anchorT) { out = p; return true; }        // массив напрямую
        const uint64_t p2 = rd.ptr(p);                                     // один уровень косвенности
        if (looks_like_heap_ptr(p2) && rd.ptr(p2 + slot) == anchorT) { out = p2; return true; }
    }
    return false;
}

// Число трансформов в иерархии: идём по transforms[] и сверяем, что у каждого
// указателя его собственный индекс (по indexOff) равен позиции k. Это строгий
// якорь: случайный мусор не даст index==k. «Дырки» (освобождённые слоты)
// терпим в пределах tolerance подряд. Возвращает наибольший связный индекс +1.
inline int32_t CountTransforms(const Reader& rd, uint64_t transforms, uint64_t indexOff,
                               int32_t cap, int32_t tolerance) {
    if (!looks_like_heap_ptr(transforms) || cap <= 0) return 0;
    if (tolerance < 0) tolerance = 0;
    int32_t n = 0, misses = 0;
    for (int32_t k = 0; k < cap; ++k) {
        const uint64_t x = rd.ptr(transforms + (uint64_t)k * 8u);
        uint32_t idx = 0xFFFFFFFFu;
        const bool ok = looks_like_heap_ptr(x) && rd.u32(x + indexOff, idx) && (int32_t)idx == k;
        if (ok) { n = k + 1; misses = 0; }
        else if (++misses > tolerance) break;
    }
    return n;
}

// Дешёвая самопроверка найденного массива: вокруг якоря каждый слот должен быть
// указателем на трансформ с собственным индексом == позиции. Не требует O(N).
inline bool SceneArraysLookValid(const Reader& rd, uint64_t transforms, uint64_t indexOff,
                                 int32_t anchorI, int32_t window) {
    if (!looks_like_heap_ptr(transforms) || anchorI < 0) return false;
    if (window < 1) window = 1;
    int good = 0;
    for (int32_t k = anchorI - window; k <= anchorI + window; ++k) {
        if (k < 0) continue;
        const uint64_t x = rd.ptr(transforms + (uint64_t)k * 8u);
        uint32_t idx = 0xFFFFFFFFu;
        if (looks_like_heap_ptr(x) && rd.u32(x + indexOff, idx) && (int32_t)idx == k) ++good;
    }
    return good > 0;   // хотя бы сам якорь сошёлся
}

// Запекание локальных треугольников меша в мировые и дописка в .obj. В отличие
// от WriteObj (локальные координаты для инстансинга vis_dyn), статика пишется
// сразу в мир: у карты нет второго прохода инстансинга, vis_map грузит map.obj
// как есть. vbase — сквозной счётчик вершин файла (растёт на 3 с треугольником),
// чтобы грани всех инстансов ссылались на свои вершины.
inline void AppendWorldObj(std::string& out, const std::vector<vis::Tri>& local,
                           const vis::Vec3& pos, const vis::Quat& rot, float scale,
                           uint32_t& vbase) {
    char buf[96];
    for (const auto& t : local) {
        const vis::Tri w = vis::TransformTri(t, pos, rot, scale);
        for (const vis::Vec3* v : {&w.a, &w.b, &w.c}) {
            std::snprintf(buf, sizeof(buf), "v %.5f %.5f %.5f\n", v->x, v->y, v->z);
            out += buf;
        }
        std::snprintf(buf, sizeof(buf), "f %u %u %u\n", vbase + 1, vbase + 2, vbase + 3);
        out += buf;
        vbase += 3;
    }
}

} // namespace meshdump
