#!/usr/bin/env python3
"""Оффсеты БЕТА-версии игры из `dump_beta.7z` — отдельным файлом.

    python3 tools/offsets/beta_offsets.py            # что отличается от релиза
    python3 tools/offsets/beta_offsets.py --apply    # записать файл беты
    python3 tools/offsets/beta_offsets.py --selftest # проверка самого скрипта

Если дамп беты пришёл без `libil2cpp.so`, RVA классов пересчитать нечем — их
можно подсунуть отдельно: `--so libil2cpp.so` (или `--so libil2cpp_beta.7z`,
если он сжат как релизный).

Зачем отдельный скрипт, а не `update_offsets.py`. Тот пересчитывает РЕЛИЗНЫЙ
заголовок по свежему дампу релиза: старый дамп он берёт из истории git, а
значения — из заголовка. Бета же живёт параллельно релизу, и её оффсеты нужно
посчитать ОТ РЕЛИЗНОГО ДАМПА, ничего не переписывая в `game_offsets.h`:

    dump.7z (релиз)  ──эталон──▶  dump_beta.7z (бета)
    game_offsets.h               game_offsets_beta.h (этот скрипт)

Механика сравнения полей — та же, что в `update_offsets.py` (её функции и
переиспользуются): совпадение по читаемому имени, иначе позиционное выравнивание
последовательности типов, а имя класса по указателю — через путь `via`.

Что НЕ выводится из дампа (и потому для беты берётся как в релизе):

  * `kind: runtime` — раскладка IL2CPP/Unity (`MANAGED_CACHED_PTR`,
    `IL2CPP_STRING_*`, `TRANSFORM_*`, `GAMEOBJECT_*`, `IL2CPP_LIST_*`) и нативные
    смещения `UnityEngine.Camera` из libunity.so. Первые не меняются между
    версиями одной Unity; вторые изменятся только если в бете другая сборка
    Unity — тогда в архив беты нужно положить libunity.so, и константы придётся
    перемерить вручную (скрипт скажет об этом в шапке файла);
  * `*_TYPEINFO_RVA` — считаются дизассемблером по `libil2cpp.so` той же версии
    игры. Если в архиве беты есть свой `libil2cpp.so` (или он лежит рядом как
    `libil2cpp_beta.7z` или указан ключом `--so`), RVA пересчитаются; если нет —
    скрипт скажет прямо, в файле беты останутся релизные RVA, а `kRvaFromDump`
    будет false: по релизным RVA бета-клиент классов не находит, поэтому
    `go::BetaAvailable()` такую бету в меню не предложит.

Результат в репозитории:

  * `jni/src/game_offsets_beta.h`  — тот же список констант, что в релизном
    заголовке, но со значениями беты (комментарии и структура сохранены, у
    изменившихся строк стоит пометка «бета: …»);
  * `jni/src/game_offsets_active.inc` — таблица «константа → значение беты» для
    рантайм-переключателя (`game_offsets_active.h`).

Имя структуры беты искать не нужно: обфускатор перекатывает его вместе с полями
(`GKo_Fields` → `RET_Fields`), поэтому пропавшую структуру скрипт подбирает ПО
ФОРМЕ — по смещениям и типам полей, считая перекатанные имена типов одним
токеном. Найденное совпадение печатается в отчёте; если похожих структур
несколько, скрипт молчит и отправляет разбираться руками.

Файл беты пишется ВСЕГДА, даже если архива нет: тогда это копия релиза с
шапкой «не собран из дампа» и `kFromDump = false` — сборка остаётся рабочей, а
в меню бета просто не предлагается (см. `go::BetaAvailable()`).
"""
import argparse
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = subprocess.run(['git', '-C', HERE, 'rev-parse', '--show-toplevel'],
                      capture_output=True, text=True, check=True).stdout.strip()
HEADER = os.path.join(ROOT, 'jni', 'src', 'game_offsets.h')
BETA = os.path.join(ROOT, 'jni', 'src', 'game_offsets_beta.h')
ACTIVE = os.path.join(ROOT, 'jni', 'src', 'game_offsets_active.inc')
MAP = os.path.join(HERE, 'offsets_map.json')

# Обе формы объявлений из заголовка: смещения-uint64 и «переменные» float
# (PLAYER_HEIGHT и т.п.). Всё, что объявлено так, попадает в файл беты.
DECL_RE = re.compile(
    r'^(inline constexpr (?:std::uint64_t|float)\s+(\w+)\s*=\s*)'
    r'(0x[0-9A-Fa-f]+|\d+(?:\.\d+)?[Ff]?)(\s*;.*)$', re.M)
# Значение из строки объявления → int (смещения) или float (не смещения).
NUM_RE = re.compile(r'0x[0-9A-Fa-f]+|\d+(?:\.\d+)?')


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


u = load_module('update_offsets', os.path.join(HERE, 'update_offsets.py'))
lay = u.lay


def num_of(text):
    """Число из строки объявления: int для 0x…/десятичных, float для 1.8F."""
    raw = NUM_RE.search(text).group(0)
    if raw.lower().startswith('0x') or '.' not in raw:
        return int(raw, 16) if raw.lower().startswith('0x') else int(raw)
    return float(raw.rstrip('Ff'))


# ------------------------------------------------------------------ дампы ----
def extract_beta(outdir, archive):
    """Распаковать архив беты: il2cpp.h + script.json + libil2cpp.so (если есть)."""
    os.makedirs(outdir, exist_ok=True)
    if os.path.exists(os.path.join(outdir, 'il2cpp.h')):
        return outdir
    if not os.path.exists(archive):
        return None

    import py7zr
    raw = os.path.join(outdir, '_raw')
    os.makedirs(raw, exist_ok=True)
    with py7zr.SevenZipFile(archive, 'r') as z:
        # Каталоги не распаковываем: py7zr спотыкается на них, если каталог уже есть.
        z.extract(path=raw, targets=[n for n in z.getnames() if not n.endswith('/')])

    def find(name):
        for root, _dirs, files in os.walk(raw):
            for f in files:
                if f.lower() == name:
                    return os.path.join(root, f)
        return None

    # Всё, что нужно, — в один каталог, как их ждёт остальная обвязка.
    for want in ('il2cpp.h', 'dump.cs', 'script.json'):
        got = find(want)
        if not got:
            print(f'   в архиве нет {want}')
            continue
        if os.path.abspath(got) != os.path.abspath(os.path.join(outdir, want)):
            os.replace(got, os.path.join(outdir, want))

    # libil2cpp.so: либо лежит в архиве файлом, либо вложенным архивом (.7z с
    # LZMA2+ARM64 BCJ — py7zr такой фильтр не тянет, распаковываем через xz).
    so = os.path.join(outdir, 'libil2cpp.so')
    if not os.path.exists(so):
        packed = find('libil2cpp.so')
        if packed:
            os.replace(packed, so)
        else:
            nested = find('libil2cpp_beta.7z') or find('libil2cpp.7z')
            if nested:
                unpack_so(nested, so)
    if os.path.exists(so):
        print(f'   libil2cpp.so беты: {os.path.getsize(so)} байт')
    else:
        print('   libil2cpp.so беты нет — TYPEINFO_RVA пересчитать не получится')
    return outdir


def unpack_libil2cpp(archive, dest):
    """Распаковка LZMA2+ARM64 BCJ архива так же, как это делает extract_dumps.sh."""
    import py7zr
    a = py7zr.SevenZipFile(archive)
    pi = a.header.main_streams.packinfo
    fo = a.header.main_streams.unpackinfo.folders[0]
    props = next(c['properties'] for c in fo.coders if c.get('properties'))
    dict_sz = (2 | (props[0] & 1)) << (props[0] // 2 + 11)
    off = a.afterheader + pi.packpos
    size = pi.packsizes[0]
    pack = dest + '.pack'
    with open(archive, 'rb') as f, open(pack, 'wb') as o:
        f.seek(off)
        o.write(f.read(size))
    with open(dest, 'wb') as o:
        subprocess.run(['xz', '--format=raw', '--arm64', f'--lzma2=dict={dict_sz}', '-dc', pack],
                       stdout=o, check=True)
    os.remove(pack)


def unpack_so(archive, dest):
    """libil2cpp.so из архива — в обоих форматах, в которых его присылают.

    Бета приходит обычным 7z с `libil2cpp.so` внутри (так и было 16.09.2026), а
    релизный `libil2cpp.7z` — LZMA2 с фильтром ARM64-BCJ, который py7zr не
    тянет: его распаковывает `unpack_libil2cpp` через `xz --format=raw`.
    Пробуем первый путь, на любой осечке уходим во второй.
    """
    print(f'   libil2cpp.so беты: распаковываю {os.path.basename(archive)}')
    try:
        import py7zr
        with py7zr.SevenZipFile(archive, 'r') as z:
            names = [n for n in z.getnames() if not n.endswith('/')]
            so = [n for n in names if n.lower().endswith('.so')]
            if so:
                outdir = os.path.dirname(dest) or '.'
                z.extract(path=outdir, targets=so)
                got = os.path.join(outdir, so[0])
                with open(got, 'rb') as f:
                    elf = f.read(4) == b'\x7fELF'
                if elf:
                    os.replace(got, dest)
                    print(f'   libil2cpp.so беты: {os.path.getsize(dest)} байт')
                    return dest
                os.remove(got)
                print('   внутри оказался не ELF — пробую путь с xz')
    except Exception as e:  # noqa: BLE001 — причина не важна, есть второй путь
        print(f'   py7zr не осилил ({e.__class__.__name__}) — пробую путь с xz')
    unpack_libil2cpp(archive, dest)
    print(f'   libil2cpp.so беты: {os.path.getsize(dest)} байт (xz raw)')
    return dest


def place_so(src, dest):
    """Положить libil2cpp.so беты на место: файлом или упакованным (.7z)."""
    if src.lower().endswith('.7z'):
        unpack_so(src, dest)
    else:
        shutil.copyfile(src, dest)
    print(f'   libil2cpp.so беты: {os.path.getsize(dest)} байт '
          f'(из {os.path.basename(src)})')


def dump_fingerprint(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()[:16]


# Окно скана TOD_Sky (esp/game_patch.cpp ищет в нём класс небесного цикла по сигнатуре).
# В карте оно помечено как runtime — «из дампа не выводится», и это верно: оно
# зависит не от раскладки структур, а от того, ГДЕ в .data.rel.ro лежит таблица
# metadata-usage. У беты она уезжает вместе с RVA классов, поэтому окно едет на
# ту же дельту: иначе скан уходит в пустоту и «Всегда день» молча не работает.
RVA_SHIFTED = ('TOD_SCAN_RVA_BEGIN', 'TOD_SCAN_RVA_END')


# ------------------------------------------------- форма вместо имени ----
def type_norm(t):
    """Тип для сравнения структур: у перекатанных имён остаётся только форма.

    `GKw_o*` в релизе и `REh_o*` в бете — один и тот же класс: обфускатор
    перегенерировал имя, и совпасть по имени они не могут, а по форме (указатель
    на класс) — могут. Читаемые типы (`Oxide_PlayerManager_o*`, `float`,
    `UnityEngine_RaycastHit_o`) сравниваются как есть.
    """
    star = '*' if t.endswith('*') else ''
    base = t[:-1] if star else t
    return ('обф' + star) if u.obfuscated(base) else t


def find_beta_struct(beta, rel_fields, need_offsets):
    """Структура беты по форме — когда её имя перегенерировали.

    Возвращает `(имя, пояснение)` или `(None, причина)`. Считаем долю полей,
    совпавших по смещению и нормированному типу; кандидат обязан содержать все
    нужные нам смещения. Победитель должен быть заметно впереди второго: если
    структур-близнецов несколько, выбор наугад хуже отказа.
    """
    if not rel_fields or not need_offsets:
        return None, 'нечего сравнивать'
    want = {o: type_norm(t) for _, o, t in rel_fields}
    scored = []
    for name in beta.index:
        if not name.endswith('_Fields'):
            continue
        got = {o: type_norm(t) for _, o, t in (beta.fields(name) or [])}
        if any(want.get(o) not in (None, got.get(o)) for o in need_offsets):
            continue
        score = sum(1 for o, t in want.items() if got.get(o) == t) / len(want)
        scored.append((score, name))
    if not scored:
        return None, 'по форме ничего похожего'
    scored.sort(reverse=True)
    best = scored[0]
    second = scored[1] if len(scored) > 1 else (0.0, '—')
    if best[0] < 0.8:
        return None, f'ближайшее по форме — {best[1]}, и то всего {best[0]:.0%}'
    if best[0] - second[0] < 0.15:
        return None, f'похожих структур несколько: {best[1]} и {second[1]}'
    return best[1], f'{best[0]:.0%} полей совпало'


# ------------------------------------------------------- пересчёт значений ----
def resolve_via(dump, via):
    """Путь `via` (стабильные имена полей) → имя структуры в этом дампе."""
    st = None
    for i, hop in enumerate(via):
        st = hop.get('struct') or st
        want = [f for f in (dump.fields(st) or []) if f[0] == hop['field']]
        if len(want) != 1 or not want[0][2].endswith('_o*'):
            return None, f'шаг {i + 1}: {st}.{hop["field"]}'
        st = want[0][2][:-len('_o*')] + '_Fields'
    return st, None


def compute_beta(rel, beta, omap, rel_vals, with_rva, rel_dir, beta_dir):
    """Значения беты для всех констант карты: {const: значение}."""
    # via-цепочки разрешаем в обоих дампах: имя класса по указателю ротирует.
    for const, ent in omap.items():
        if ent.get('kind') != 'field' or not ent.get('via'):
            continue
        st_rel, bad = resolve_via(rel, ent['via'])
        st_beta, bad2 = resolve_via(beta, ent['via'])
        if st_rel is None:
            sys.exit(f'{const}: путь via не разрешился в релизном дампе ({bad})')
        if st_beta is None:
            sys.exit(f'{const}: путь via не разрешился в дампе беты ({bad2}) — '
                     f'поле переименовали; поправь карту')
        ent['_struct_rel'], ent['_struct_beta'] = st_rel, st_beta

    per_struct = {}
    for const, ent in omap.items():
        if ent.get('kind') == 'field':
            key = (ent.get('_struct_beta') or ent['struct'],
                   ent.get('_struct_rel') or ent['struct'])
            per_struct.setdefault(key, []).append(const)

    values, changes, warnings = {}, {}, []
    for (st_beta, st_rel), consts in sorted(per_struct.items()):
        ent0 = omap[consts[0]]
        label = lay.TRACKED.get(st_beta, st_beta)
        if ent0.get('via'):
            label = f'{label} via {".".join(h["field"] for h in ent0["via"])}'
        f_beta = beta.fields(st_beta)
        f_rel = rel.fields(st_rel)
        if f_beta is None:
            # Имя структуры перекатано — ищем её по форме (смещения + типы).
            need = [omap[c]['offset'] for c in consts]
            found, why = find_beta_struct(beta, f_rel, need)
            if found is None:
                warnings.append(f'[{label}] структуры нет в дампе беты ({why}) — ищи '
                                f'форму: il2cpp_layout.py find')
                continue
            print(f'   {st_beta} → {found}: структура найдена по форме ({why})')
            f_beta = beta.fields(found)
            for c in consts:
                omap[c]['_struct_beta'] = found
        amap = u.align(f_rel, f_beta)

        for const in sorted(consts):
            ent = omap[const]
            want = rel_vals.get(const)
            hit = None
            # Читаемое имя переживает переобфускацию — пробуем его первым.
            if ent.get('field') and not u.obfuscated(ent['field']):
                same = [f for f in f_beta if f[0] == ent['field']]
                if len(same) == 1:
                    hit = (same[0][1], 'имя')
            if hit is None:
                hit = amap.get(ent['offset'])
                if hit is not None:
                    hit = (hit[0], 'позиция')
            if hit is None:
                warnings.append(f'{const}: поле не найдено в дампе беты — проверь вручную')
                continue
            values[const] = hit[0]
            if want is not None and hit[0] != want:
                changes[const] = (want, hit[0], f'{label}.{ent.get("field")}', hit[1])

    # runtime-константы: из дампа не выводятся, остаются релизными.
    runtime = [c for c, e in omap.items() if e.get('kind') == 'runtime']
    for const in runtime:
        if const in rel_vals:
            values[const] = rel_vals[const]

    # TYPEINFO_RVA: только по libil2cpp.so САМОЙ беты.
    rva_consts = [c for c, e in omap.items() if e.get('kind') == 'typeinfo_rva']
    rva_note = ''
    rva_from_dump = False
    so_beta = os.path.join(beta_dir, 'libil2cpp.so')
    script_beta = os.path.join(beta_dir, 'script.json')
    if rva_consts and not with_rva:
        rva_note = 'RVA не пересчитывались (--no-rva)'
        for const in rva_consts:
            if const in rel_vals:
                values[const] = rel_vals[const]
    elif rva_consts and os.path.exists(so_beta) and os.path.exists(script_beta):
        classes = [omap[c].get('class') for c in rva_consts if omap[c].get('class')]
        print(f'пересчёт RVA беты для {len(classes)} классов (дизассемблирование, ~минута)...')
        cand_beta = u.rva_candidates(so_beta, script_beta, classes)
        cand_rel = u.rva_candidates(os.path.join(rel_dir, 'libil2cpp.so'),
                                    os.path.join(rel_dir, 'script.json'), classes) \
            if os.path.exists(os.path.join(rel_dir, 'libil2cpp.so')) else {}
        rva_from_dump = True
        for const in rva_consts:
            cls = omap[const].get('class')
            cur = rel_vals.get(const)
            got, how, warn = u.pick_rva(cls, cand_rel.get(cls, []), cand_beta.get(cls, []), cur)
            if warn:
                warnings.append(f'{const}: {warn}')
            if got is None:
                rva_from_dump = False
                warnings.append(f'{const}: RVA для {cls} в бете не найден — '
                                f'запусти typeinfo_rva.py вручную')
                continue
            values[const] = got
            if cur != got:
                changes[const] = (cur, got, f'TypeInfo {cls}', how)
    elif rva_consts:
        rva_note = 'libil2cpp.so беты не было — RVA остались релизные (классы не найдутся!)'
        warnings.append(rva_note)
        for const in rva_consts:
            if const in rel_vals:
                values[const] = rel_vals[const]

    if not rva_consts:
        rva_from_dump = True  # пересчитывать нечего — все RVA уже из дампа

    # Окно скана TOD_Sky едет вместе с RVA (см. RVA_SHIFTED).
    deltas = sorted(values[c] - rel_vals[c] for c in rva_consts
                    if c in values and c in rel_vals and values[c] != rel_vals[c])
    if len(deltas) >= 2 and deltas[-1] - deltas[0] <= 0x10000:
        delta = deltas[len(deltas) // 2]
        for const in RVA_SHIFTED:
            if const in rel_vals and omap.get(const, {}).get('kind') == 'runtime':
                values[const] = rel_vals[const] + delta
                changes[const] = (rel_vals[const], values[const], 'окно скана RVA',
                                  f'сдвиг {delta:+#x}')
    elif len(deltas) >= 2:
        warnings.append('RVA классов разъехались на разные дельты '
                        f'({deltas[0]:+#x}..{deltas[-1]:+#x}) — окно скана TOD '
                        f'осталось релизным, проверь «Всегда день» на бете')

    return values, changes, warnings, runtime, rva_consts, rva_note, rva_from_dump


# ------------------------------------------------------------- генерация ----
def read_header(path):
    """({имя: значение}, [(вид, имя)]) по текущему виду заголовка.

    Вид — "field" для смещений (std::uint64_t) и "const" для остального
    (float-геометрия вроде PLAYER_HEIGHT): смещения переключаются между
    версиями игры, остальное одинаково везде.
    """
    text = open(path, encoding='utf-8').read()
    vals = {m.group(2): num_of(m.group(3)) for m in DECL_RE.finditer(text)}
    decls = [('field' if 'std::uint64_t' in m.group(1) else 'const', m.group(2))
             for m in DECL_RE.finditer(text)]
    return vals, decls


def fmt_value(v):
    if isinstance(v, float):
        # Целые float'ы печатаем с точкой: «0F» — не литерал, а «0.0F» — да.
        txt = '%g' % v
        if '.' not in txt and 'e' not in txt:
            txt += '.0'
        return txt + 'F'
    return '0x%X' % v


def write_beta_header(rel_text, values, changes, runtime, rva_consts, note, stamp,
                     dump_src, from_dump, rva_from_dump, n_map):
    """Файл беты: та же структура и комментарии, что у релиза, со значениями беты."""
    out = []
    for line in rel_text.split('\n'):
        m = DECL_RE.match(line)
        if not m:
            out.append(line)
            continue
        name = m.group(2)
        if name not in values:
            # Не из карты (например, PLAYER_HEIGHT): значение одинаково везде.
            out.append(line)
            continue
        val = values[name]
        tail = m.group(4)
        if name in changes:
            tail = tail.rstrip() + f'  // бета: было {fmt_value(changes[name][0])}'
        out.append(m.group(1) + fmt_value(val) + tail)
    # Зеркалим релизный заголовок: комментарии и порядок те же, чтобы файлы
    # можно было сравнивать построчно. Меняем только то, что обязано отличаться:
    # имя namespace (иначе это был бы тот же самый набор констант), убираем
    # #pragma once/#include (они уже есть в шапке этого файла) и подставляем
    # значения беты с пометкой у изменившихся строк.
    skip = {'#pragma once', '#include <cstdint>'}
    body = '\n'.join(l for l in out if l.strip() not in skip)
    # «Это оффсеты релизной версии» из релизного заголовка здесь неверно: этот
    # файл — бета. Заменяем заметку на свою, чтобы не путать читателя.
    body = re.sub(r'// ВНИМАНИЕ: это оффсеты РЕЛИЗНОЙ.*?\n\n', '', body, count=1, flags=re.S)
    body = body.replace('namespace game_offsets {', 'namespace game_offsets_beta {', 1)

    # Маркер «собран из дампа или нет» и строка-источник для меню: по ним
    # переключатель решает, предлагать ли бету (go::BetaAvailable()).
    # Строка для меню: пока бета доступна — это источник оффсетов, иначе причина,
    # по которой её нельзя выбрать (пустая строка = доступна).
    if not from_dump:
        reason = 'Бета недоступна: файл оффсетов не собран'
    elif not rva_from_dump:
        reason = 'Бета недоступна: не пересчитаны RVA классов'
    else:
        reason = ''
    mark = ('inline constexpr bool kFromDump = %s;\n'
            'inline constexpr bool kRvaFromDump = %s;\n'
            'inline constexpr const char* kReason = "%s";\n'
            'inline constexpr const char* kSource = "%s";\n\n'
            % ('true' if from_dump else 'false',
               'true' if rva_from_dump else 'false', reason, dump_src))
    body = body.replace('namespace game_offsets_beta {\n',
                        'namespace game_offsets_beta {\n' + mark, 1)

    # Константы раскладки, которые так и остались релизными (окно TOD_Sky
    # может быть сдвинуто — тогда оно не «взято как есть», а пересчитано).
    copied = [c for c in runtime if c not in changes]
    banner = f'''// Оффсеты БЕТА-версии игры. Файл собран tools/offsets/beta_offsets.py — правь
// скрипт и дампы, а не здесь.
//
// Источник: {dump_src}, дата сборки файла {stamp}.
// Эталон, от которого считались отличия: релизный dump.7z и jni/src/game_offsets.h.
// Отличий от релиза: {len(changes)} из {len(values)} объявленных констант
// (в карте оффсетов {n_map} записей).
// Взято из релиза как есть: {len(copied)} констант раскладки IL2CPP/Unity
// (из дампа игры они не выводятся — об этом ниже) и {len(rva_consts)} RVA.
// Окно скана TOD_Sky сдвинуто на дельту RVA беты (см. RVA_SHIFTED в скрипте):
// оно зависит от того, где лежит таблица metadata-usage, а не от структур.
// {note or 'TYPEINFO_RVA пересчитаны по libil2cpp.so беты.'}
//
// Как пользоваться: выбор версии — в меню клиента («Версия игры», вкладка
// «Опции») и на стартовом экране; переключатель — go::SelectBuild() в
// game_offsets_active.h, он подставляет значения из этого файла. Пока
// kFromDump == false, бета в меню не предлагается: файл не собран из дампа.
// kRvaFromDump == false — не пересчитаны RVA классов (нет libil2cpp.so беты):
// по релизным RVA бета-клиент классов не находит, поэтому такая бета тоже не
// предлагается. Лечится ключом --so или файлом libil2cpp_beta.7z в корне.

#pragma once
#include <cstdint>

'''
    return banner + body


def write_active_inc(rel_decls, stamp):
    """Таблица для переключателя: пара «имя → значение беты».

    Для каждой константы заголовка пишется одна строка, и вид строки зависит
    от того, что это за константа:

      * GO_FIELD — смещение (std::uint64_t): попадает и в таблицу переключения
        (значение релиза берётся из game_offsets.h, значение беты — из
        game_offsets_beta.h), и в активный набор;
      * GO_CONST — не смещение (float-геометрия вроде PLAYER_HEIGHT): значение
        одинаково у обеих версий, в таблицу не идёт.

    Пересобирать таблицу нужно только при смене ДАМПА БЕТЫ: релизные значения
    подставляются из заголовка, а не из неё.
    """
    lines = [f'''// Активные оффсеты для переключателя версии (go::SelectBuild).
// Сгенерировано tools/offsets/beta_offsets.py — правь скрипт, а не здесь.
//
// GO_FIELD(имя, значение_беты) — смещение: релиз берётся из game_offsets.h,
// бета — из game_offsets_beta.h. GO_CONST(имя, значение) — не смещение
// (геометрия боксов и т.п.), у обеих версий одинаково.
//
// Собрано: {stamp}.

''']
    for kind, name in rel_decls:
        lines.append(f'{"GO_FIELD" if kind == "field" else "GO_CONST"}'
                     f'({name}, game_offsets_beta::{name})\n')
    return ''.join(lines)


# ------------------------------------------------------------------ main ----
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--archive', default=os.path.join(ROOT, 'dump_beta.7z'),
                    help='архив с дампом беты (по умолчанию dump_beta.7z в корне)')
    ap.add_argument('--work', default='/tmp/offsets_beta', help='куда распаковывать дампы')
    ap.add_argument('--apply', action='store_true', help='записать файл беты и таблицу')
    ap.add_argument('--selftest', action='store_true',
                    help='проверка скрипта: релиз против релиза (отличий быть не должно)')
    ap.add_argument('--no-rva', action='store_true', help='не пересчитывать TYPEINFO_RVA')
    ap.add_argument('--so', help='libil2cpp.so беты (файлом или .7z), если его нет в архиве')
    ap.add_argument('--map', default=MAP)
    a = ap.parse_args()

    omap = json.load(open(a.map, encoding='utf-8'))
    rel_dir = u.extract(os.path.join(a.work, 'rel'))
    if rel_dir is None:
        sys.exit('не удалось распаковать релизный dump.7z из рабочего дерева')
    if a.selftest:
        beta_dir, src = rel_dir, 'РЕЛИЗ (для самопроверки)'
    else:
        beta_dir = extract_beta(os.path.join(a.work, 'beta'), a.archive)
        src = os.path.basename(a.archive)
        if beta_dir is None:
            print(f'архива {a.archive} нет — пишу файл беты как копию релиза '
                  f'(kFromDump = false, в меню бета не предлагается)')
        elif not os.path.exists(os.path.join(beta_dir, 'libil2cpp.so')):
            # .so можно дать ключом или положить рядом с дампом — имена
            # фиксированные, чтобы не искать его по всему диску.
            for cand in (a.so, os.path.join(ROOT, 'libil2cpp_beta.7z'),
                         os.path.join(ROOT, 'libil2cpp_beta.so')):
                if cand and os.path.exists(cand):
                    place_so(os.path.abspath(cand),
                             os.path.join(beta_dir, 'libil2cpp.so'))
                    break

    rel = lay.Dump(os.path.join(rel_dir, 'il2cpp.h'))
    rel_text = open(HEADER, encoding='utf-8').read()
    rel_vals, rel_decls = read_header(HEADER)
    stamp = time.strftime('%d.%m.%Y')

    if beta_dir is None:
        values, changes, warnings = dict(rel_vals), {}, []
        runtime = [c for c, e in omap.items() if e.get('kind') == 'runtime']
        rva_consts = [c for c, e in omap.items() if e.get('kind') == 'typeinfo_rva']
        note = 'Файл НЕ собран из дампа беты: значения совпадают с релизом, ' \
               'в меню бета не предлагается (go::BetaAvailable() == false).'
        from_dump = False
        rva_from_dump = False
        dump_src = f'архива {src} нет'
    else:
        beta = lay.Dump(os.path.join(beta_dir, 'il2cpp.h'))
        values, changes, warnings, runtime, rva_consts, note, rva_from_dump = \
            compute_beta(rel, beta, omap, rel_vals, not a.no_rva,
                         rel_dir, beta_dir if not a.selftest else rel_dir)
        from_dump = True
        fp = dump_fingerprint(os.path.join(beta_dir, 'il2cpp.h'))
        dump_src = f'{src}, il2cpp.h sha256:{fp}'

    # --- отчёт ---
    print(f'\nэталон: релизный дамп + {os.path.relpath(HEADER, ROOT)}')
    print(f'бета:   {dump_src}')
    print(f'проверено констант: {len(values)} '
          f'(не выводятся из дампа: {len(runtime)} раскладки + '
          f'{len(rva_consts)} RVA{r" (пересчитаны)" if from_dump and not a.no_rva else ""})\n')
    if changes:
        print(f'{"константа":42s} {"релиз":>10s} {"бета":>10s}  источник')
        for c, (o, n, what, how) in sorted(changes.items()):
            print(f'{c:42s} {fmt_value(o):>10s} {fmt_value(n):>10s}  {what} ({how})')
    else:
        print('отличий от релиза нет — раскладка структур беты совпадает с релизом.'
              if from_dump else 'отличий нет (самопроверка).')
    if warnings:
        print('\nтребует внимания:')
        for w in warnings:
            print('  ! ' + w)
    if from_dump and not rva_from_dump:
        print('\nбета в меню предлагаться НЕ будет: RVA классов не пересчитаны.')
        print('  нужен libil2cpp.so той же сборки, что и дамп:')
        print('    python3 tools/offsets/beta_offsets.py --apply --so <файл>')
        print('  или положи libil2cpp_beta.7z в корень репозитория ')
        print('  (обычный 7z с libil2cpp.so внутри; релизный формат с xz тоже понимаем).')

    if a.selftest:
        bad = {c for c in rel_vals if c in values and values[c] != rel_vals[c]}
        print('\nсамопроверка: ' + ('ПРОВАЛ, разошлись ' + ', '.join(sorted(bad)) if bad
                                    else 'ПРОЙДЕНА — все значения совпали с заголовком'))
        return 1 if bad else 0

    if a.apply:
        header = write_beta_header(rel_text, values, changes, runtime, rva_consts,
                                   note, stamp, dump_src, from_dump, rva_from_dump,
                                   len(omap))
        open(BETA, 'w', encoding='utf-8').write(header)
        open(ACTIVE, 'w', encoding='utf-8').write(write_active_inc(rel_decls, stamp))
        print(f'\nзаписано: {os.path.relpath(BETA, ROOT)} '
              f'({len(values)} констант, отличий {len(changes)}) и '
              f'{os.path.relpath(ACTIVE, ROOT)} ({len(rel_decls)} строк)')
    else:
        print('\nчтобы записать: тот же вызов с --apply')
    return 1 if warnings and from_dump and any('не найден' in w for w in warnings) else 0


if __name__ == '__main__':
    sys.exit(main())
