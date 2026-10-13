#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Извлечение статической геометрии из ассетов Unity ПРЯМО НА УСТРОЙСТВЕ (без ПК).

Зачем: рантайм-дампер (jni/src/mesh_dump.h) берёт только сетевые постройки из
Mirror.NetworkClient.spawned. Статическая карта (террейн, скалы, деревья,
статические здания) лежит в ассет-бандлах игры (APK/OBB) и в spawned не
попадает. Этот скрипт разбирает ассеты через UnityPy и отдаёт Wavefront .obj,
который vis_map кладёт в <CfgDir>/map.obj (или в pieces/ для библиотеки).

Работает под Termux на самом телефоне:
    pkg install python
    pip install UnityPy
    pm path com.catsbit.oxidesurvivalisland          # пути к base.apk/split
    python extract_assets.py /data/app/.../base.apk ... --out-dir /sdcard/ox

Формат выхода — только `v`/`f` (геометрия), потому что vis::LoadObj читает
именно их: у `f a/b/c` берётся первое число, полигоны триангулируются веером.
vt/vn намеренно отбрасываются.

Ориентация: UnityPy/Unity отдают меши в тех же осях, что и AssetRipper
(правая, Z вверх после конверсии), поэтому на устройстве kMeshYUpToZUp=false.
Если в игре карта повёрнута на 90° вокруг X или зеркальна по X —either
поменяйте kMeshYUpToZUp в jni/src/vis_check_map.h, либо передайте yUpToZUp=1
в obj2bvh при предпосчёте .bvh.
"""
import argparse
import os
import sys

try:
    import UnityPy
    from UnityPy.enums import ClassIDType
except Exception as e:  # pragma: no cover
    sys.stderr.write("Нужен UnityPy:  pip install UnityPy   (%s)\n" % e)
    sys.exit(2)

MESH = int(ClassIDType.Mesh)            # 43
TERRAIN_DATA = int(ClassIDType.TerrainData)  # 156


def log(*a):
    sys.stderr.write(" ".join(str(x) for x in a) + "\n")
    sys.stderr.flush()


def sanitize(name, fallback="mesh"):
    out = []
    for ch in name or "":
        if ch.isalnum() and ord(ch) < 128 or ch in "_-.":
            out.append(ch)
        elif ch == " ":
            out.append("_")
    s = "".join(out).strip(".")
    if not s:
        s = fallback
    return s[:60]


def mesh_extent(m):
    """Полуоси локального AABB меша (для фильтра «это мировая геометрия»)."""
    try:
        e = m.m_LocalAABB.m_Extent
        return max(abs(float(e.x)), abs(float(e.y)), abs(float(e.z)))
    except Exception:
        return 0.0


def export_mesh_obj(m):
    """Wavefront из меша: только v/f (几何), 1-based, fan-триангуляция."""
    raw = m.export(format="obj")  # str: v/vt/vn/f a/b/c ...
    verts, faces = [], []
    for line in raw.splitlines():
        if not line:
            continue
        tag = line.split(" ", 1)[0]
        if tag == "v":
            parts = line.split()
            if len(parts) >= 4:
                try:
                    verts.append((float(parts[1]), float(parts[2]), float(parts[3])))
                except ValueError:
                    pass
        elif tag == "f":
            ids = []
            for tok in line.split()[1:]:
                num = tok.split("/")[0]
                try:
                    i = int(num)
                except ValueError:
                    continue
                if i < 0:
                    i = len(verts) + i  # отрицательный индекс — от конца
                else:
                    i -= 1
                ids.append(i)
            if len(ids) >= 3:
                faces.append(ids)
    if not verts or not faces:
        return None
    return verts, faces


def write_obj(path, verts, faces, base=0, comment=None):
    """Пишем один меш. base — сдвиг индексов вершин (для склейки)."""
    with open(path, "a" if base else "w", encoding="ascii", errors="ignore") as f:
        if comment and not base:
            f.write("# %s\n" % comment)
        for v in verts:
            f.write("v %.5f %.5f %.5f\n" % v)
        for ids in faces:
            # веер: (0,k,k+1); индексы 1-based со сдвигом base
            for k in range(1, len(ids) - 1):
                f.write("f %d %d %d\n" % (ids[0] + base + 1, ids[k] + base + 1,
                                          ids[k + 1] + base + 1))


def terrain_to_grid(td, step):
    """
    Best-effort: Unity Terrain — это карта высот, не готовый меш. Строим сетку
    из m_Heightmap.m_Heights (нормированные 0..1) и мирового размера m_Size.
    step — прореживание (полный рез = миллионы вершин, телефон не потянет).
    Структура полей может отличаться между версиями Unity/играми — поэтому
    обёрнуто в try и помечено как экспериментальное.
    """
    try:
        hm = td.m_Heightmap
        heights = list(hm.m_Heights)
        res = int(getattr(hm, "m_Width", 0)) or int(getattr(hm, "m_Resolution", 0))
        if res < 2 or len(heights) < res * res:
            # иногда высоты лежат одним плоским массивом без явного res
            import math
            res = int(math.isqrt(len(heights)))
            if res < 2:
                return None
        size = td.m_Size  # Vector3: мировые размеры террейна
        sx, sy, sz = float(size.x), float(size.y), float(size.z)
        step = max(1, int(step))
        n = (res - 1) // step + 1
        if n < 2:
            return None

        def h(r, c):
            return heights[r * res + c]

        verts = []
        for r in range(0, res, step):
            for c in range(0, res, step):
                x = (c / float(res - 1)) * sx
                z = (r / float(res - 1)) * sz
                y = h(r, c) * sy
                verts.append((x, y, z))
        faces = []
        for r in range(n - 1):
            for c in range(n - 1):
                i0 = r * n + c
                i1 = i0 + 1
                i2 = i0 + n
                i3 = i2 + 1
                faces.append([i0, i2, i1])
                faces.append([i1, i2, i3])
        return verts, faces
    except Exception as e:
        log("  terrain: не удалось построить сетку (%s)" % e)
        return None


def main():
    ap = argparse.ArgumentParser(description="Извлечь меши/террейн Unity из APK/OBB в .obj (на устройстве).")
    ap.add_argument("inputs", nargs="+", help="base.apk, split_*.apk, main.obb, каталог — что удалось прочитать")
    ap.add_argument("--out-dir", required=True, help="каталог для отдельных <имя>.obj")
    ap.add_argument("--combine", metavar="MAP_OBJ", default=None,
                    help="склеить мировую геометрию в один map.obj (для <CfgDir>/map.obj)")
    ap.add_argument("--min-extent", type=float, default=2.0,
                    help="в combine брать меши с полуосью AABB >= этого (метры); отсекает предметы/оружие (по умолч. 2.0)")
    ap.add_argument("--name-contains", default=None, help="брать только меши с подстрокой в имени")
    ap.add_argument("--terrain", action="store_true", help="также строить сетку террейна из TerrainData")
    ap.add_argument("--terrain-step", type=int, default=8, help="прореживание карты высот (по умолч. 8)")
    ap.add_argument("--max-verts", type=int, default=2_000_000, help="потолок вершин на combine (защита от переполнения)")
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)

    seen_names = {}
    n_mesh = n_written = n_terr = 0
    combined_v = 0
    combine_f = None
    if args.combine:
        combine_f = open(args.combine, "w", encoding="ascii", errors="ignore")
        combine_f.write("# static map, extracted on-device via UnityPy\n")

    for path in args.inputs:
        if not os.path.exists(path):
            log("пропуск (нет файла):", path)
            continue
        log("разбор:", path)
        try:
            env = UnityPy.load(path)
        except Exception as e:
            log("  не открылся (%s)" % e)
            continue

        for obj in env.objects:
            try:
                t = int(obj.type)
            except Exception:
                continue

            if t == MESH:
                n_mesh += 1
                try:
                    m = obj.read()
                except Exception as e:
                    log("  mesh read fail:", e)
                    continue
                name = sanitize(getattr(m, "m_Name", "") or "mesh")
                if args.name_contains and args.name_contains.lower() not in name.lower():
                    continue
                got = export_mesh_obj(m)
                if not got:
                    continue
                verts, faces = got
                # уникальное имя файла
                base_name = name
                k = seen_names.get(name, 0)
                seen_names[name] = k + 1
                if k:
                    name = "%s_%d" % (base_name, k)
                outp = os.path.join(args.out_dir, name + ".obj")
                write_obj(outp, verts, faces, base=0,
                          comment="%s (%d verts)" % (base_name, len(verts)))
                n_written += 1

                ext = mesh_extent(m)
                if combine_f and ext >= args.min_extent and len(verts) >= 3:
                    if combined_v + len(verts) > args.max_verts:
                        log("  combine: достигнут потолок вершин, дальше пропускаю")
                    else:
                        write_obj(args.combine, verts, faces, base=combined_v)
                        combined_v += len(verts)

            elif t == TERRAIN_DATA and args.terrain:
                try:
                    td = obj.read()
                except Exception as e:
                    log("  terrain read fail:", e)
                    continue
                got = terrain_to_grid(td, args.terrain_step)
                if not got:
                    continue
                verts, faces = got
                name = sanitize(getattr(td, "m_Name", "") or "terrain")
                outp = os.path.join(args.out_dir, name + "_terrain.obj")
                write_obj(outp, verts, faces, base=0,
                          comment="%s terrain (%d verts)" % (name, len(verts)))
                n_terr += 1
                n_written += 1
                if combine_f and combined_v + len(verts) <= args.max_verts:
                    write_obj(args.combine, verts, faces, base=combined_v)
                    combined_v += len(verts)

    if combine_f:
        combine_f.close()

    log("готово: мешей найдено %d, записано .obj %d, террейнов %d" % (n_mesh, n_written, n_terr))
    log("отдельные файлы:", args.out_dir)
    if args.combine:
        log("склейка (%d вершин):" % combined_v, args.combine)
        log("дальше:  adb push %s /data/local/benzware/map.obj  (или скопировать файлом)" % args.combine)


if __name__ == "__main__":
    main()
