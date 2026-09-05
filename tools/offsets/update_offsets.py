#!/usr/bin/env python3
"""Пересчёт `jni/src/game_offsets.h` после апдейта игры — одной командой.

    python3 tools/offsets/update_offsets.py                 # показать, что изменилось
    python3 tools/offsets/update_offsets.py --apply         # и записать в заголовок

Что делает:

  1. распаковывает новые дампы из рабочего дерева и старые из коммита,
     в котором лежали предыдущие (`--old`, по умолчанию определяется сам);
  2. читает `offsets_map.json` — какая константа какому полю какой структуры
     соответствует, и сверяет карту со старым дампом (если поле уехало ещё
     до этого апдейта, значит константа была неверной — об этом скажет);
  3. находит то же поле в новом дампе и печатает новое смещение;
  4. пересчитывает `*_TYPEINFO_RVA` дизассемблером (они меняются всегда);
  5. с `--apply` переписывает заголовок и обновляет карту.

Почему поле нельзя искать просто по имени: обфускатор перекатывает имена
классов и полей каждый билд (`fvp`->`pmi`, `wK`->`ij`). Поэтому сначала
пробуем совпадение по имени — читаемые имена (`lastSavedPosition`, `m_Name`)
переживают апдейт, — а если имя обфусцировано, выравниваем список полей
позиционно по последовательности типов и берём поле с тем же номером.

Зависимости: py7zr, capstone, xz (см. README.md).
"""
import argparse
import difflib
import importlib.util
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = subprocess.run(['git', '-C', HERE, 'rev-parse', '--show-toplevel'],
                      capture_output=True, text=True, check=True).stdout.strip()
HEADER = os.path.join(ROOT, 'jni', 'src', 'game_offsets.h')
MAP = os.path.join(HERE, 'offsets_map.json')

CONST_RE = re.compile(
    r'^(inline constexpr std::uint64_t\s+(\w+)\s*=\s*)(0x[0-9A-Fa-f]+|\d+)(\s*;.*)$', re.M)


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


lay = load_module('il2cpp_layout', os.path.join(HERE, 'il2cpp_layout.py'))

OBF_RE = re.compile(r'^[A-Za-z]{1,4}(_[A-Za-z0-9]{1,4})*$')


def obfuscated(name):
    """Имя, которое обфускатор перекатит на следующем билде."""
    return bool(OBF_RE.match(name)) or name.startswith('<')


# ----------------------------------------------------------------- дампы ----
def extract(outdir, ref=None):
    if os.path.exists(os.path.join(outdir, 'il2cpp.h')):
        return outdir
    cmd = [os.path.join(HERE, 'extract_dumps.sh'), outdir] + ([ref] if ref else [])
    res = subprocess.run(cmd, stdout=subprocess.DEVNULL)
    if res.returncode != 0 or not os.path.exists(os.path.join(outdir, 'il2cpp.h')):
        return None
    return outdir


def previous_dump_ref():
    """Ближайший коммит, где dump.7z ЕСТЬ и отличается от текущего.

    Просто взять второй коммит из истории файла нельзя: дампы удаляли и
    заливали заново, поэтому в истории попадаются коммиты удаления, где
    файла нет вовсе.
    """
    cur = subprocess.run(['git', '-C', ROOT, 'hash-object', os.path.join(ROOT, 'dump.7z')],
                         capture_output=True, text=True).stdout.strip()
    refs = subprocess.run(['git', '-C', ROOT, 'log', '--format=%H', '--', 'dump.7z'],
                          capture_output=True, text=True).stdout.split()
    for ref in refs:
        blob = subprocess.run(['git', '-C', ROOT, 'rev-parse', f'{ref}:dump.7z'],
                              capture_output=True, text=True)
        if blob.returncode == 0 and blob.stdout.strip() and blob.stdout.strip() != cur:
            return ref
    return None


# --------------------------------------------------- сопоставление полей ----
def align(old_fields, new_fields):
    """old_offset -> (new_offset, как найдено). Имя, иначе позиция."""
    by_name = {}
    for fn, off, ty in new_fields:
        by_name.setdefault(fn, []).append((off, ty))

    out = {}
    for fn, off, ty in old_fields:
        if not obfuscated(fn) and len(by_name.get(fn, [])) == 1:
            out[off] = (by_name[fn][0][0], 'имя')

    # Остальные — позиционно: выравниваем последовательности типов.
    so = [lay.norm(t) for _, _, t in old_fields]
    sn = [lay.norm(t) for _, _, t in new_fields]
    for a, b, n in difflib.SequenceMatcher(None, so, sn, autojunk=False).get_matching_blocks():
        for k in range(n):
            off = old_fields[a + k][1]
            if off not in out:
                out[off] = (new_fields[b + k][1], 'позиция')
    return out


def rva_scan(sofile, scriptfile, classes):
    """{класс: RVA} через typeinfo_rva.py — берём верхнего кандидата.

    Формат вывода того скрипта:
        Oxide.PlayerManager  (методов просканировано: 400)
           0xD7E4310   обращений=7  ...
    Верхний кандидат почти всегда верен, но не всегда (см. предупреждение
    самого typeinfo_rva.py про GameControllerBase), поэтому значение,
    отличающееся от текущего, стоит перепроверить руками на старом дампе.
    """
    if not classes:
        return {}
    cmd = [sys.executable, os.path.join(HERE, 'typeinfo_rva.py'),
           '--so', sofile, '--script', scriptfile, '--top', '1'] + classes
    res = subprocess.run(cmd, capture_output=True, text=True)
    found, cur = {}, None
    for line in (res.stdout or '').splitlines():
        head = re.match(r'^(\S+)\s+\(', line)
        if head:
            cur = head.group(1)
            continue
        m = re.match(r'^\s+0x([0-9A-Fa-f]{5,})\s', line)
        if cur and m:
            found[cur] = int(m.group(1), 16)
            cur = None
    if not found:
        print('   typeinfo_rva.py ничего не вернул:', (res.stderr or '').strip()[:200])
    return found


# ------------------------------------------------------------------ main ----
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--old', help='коммит с прошлыми дампами (по умолчанию — сам найдёт)')
    ap.add_argument('--work', default='/tmp/offsets_update', help='куда распаковывать')
    ap.add_argument('--apply', action='store_true', help='записать новые значения в заголовок')
    ap.add_argument('--no-rva', action='store_true', help='пропустить пересчёт TYPEINFO_RVA (долго)')
    ap.add_argument('--verify', action='store_true',
                    help='сверить заголовок только с текущим дампом, без прошлого билда')
    ap.add_argument('--map', default=MAP)
    a = ap.parse_args()

    if not os.path.exists(a.map):
        sys.exit(f'нет карты {a.map} — без неё не понять, какая константа какому полю отвечает')
    omap = json.load(open(a.map, encoding='utf-8'))

    old_ref = None if a.verify else (a.old or previous_dump_ref())
    newdir = extract(os.path.join(a.work, 'new'))
    if newdir is None:
        sys.exit('не удалось распаковать dump.7z из рабочего дерева')
    olddir = extract(os.path.join(a.work, 'old'), old_ref) if old_ref else None
    if old_ref and olddir is None:
        print(f'старый дамп из {old_ref} не распаковался — сверка с прошлым билдом пропущена')
        old_ref = None
    print(f'новые дампы: рабочее дерево')
    print(f'старые дампы: {old_ref or "НЕТ — сверка с прошлым билдом пропущена"}\n')

    new = lay.Dump(os.path.join(newdir, 'il2cpp.h'))
    old = lay.Dump(os.path.join(olddir, 'il2cpp.h')) if olddir else None

    src = open(HEADER, encoding='utf-8').read()
    header_vals = {m.group(2): int(m.group(3), 0) for m in CONST_RE.finditer(src)}

    # по структурам, чтобы поля одной структуры выравнивать один раз
    per_struct = {}
    for const, ent in omap.items():
        if ent.get('kind') == 'field':
            per_struct.setdefault(ent['struct'], []).append(const)

    # Карта описывает то состояние заголовка, которое есть сейчас, а оно
    # получено из ПРЕДЫДУЩЕГО дампа. Если подсунуть дамп через поколение,
    # смещения в карте будут указывать на чужие поля и «изменения» окажутся
    # выдумкой — поэтому сначала проверяем, тот ли это дамп.
    if old is not None:
        ok = bad = 0
        for const, ent in omap.items():
            if ent.get('kind') != 'field':
                continue
            fold = old.fields(ent['struct'])
            if not fold or obfuscated(ent.get('field', '')):
                continue
            here = [f for f in fold if f[1] == ent['offset']]
            if here:
                ok += here[0][0] == ent['field']
                bad += here[0][0] != ent['field']
        # Читаемые имена между соседними билдами не переименовывают. Три
        # несовпадения — это уже другой дамп, а не апдейт.
        if bad >= 3:
            print(f'СТОП: старый дамп ({old_ref}) не тот, под который сделан заголовок '
                  f'— {bad} из {ok + bad} читаемых полей не совпали по имени.\n'
                  f'      Укажи нужный коммит через --old, либо запусти с --verify '
                  f'(сверка только с текущим дампом).')
            return 2

    changes, warnings, checked, unverifiable = {}, [], 0, []
    for struct, consts in sorted(per_struct.items()):
        label = lay.TRACKED.get(struct, struct)
        fnew = new.fields(struct)
        fold = old.fields(struct) if old else None
        if fnew is None:
            warnings.append(f'[{label}] структуры нет в новом дампе — переименована? '
                            f'ищи форму: il2cpp_layout.py find')
            continue
        amap = align(fold, fnew) if fold else None

        for const in sorted(consts):
            ent = omap[const]
            checked += 1
            want = header_vals.get(const)
            # 1. карта против старого дампа: было ли значение верным ДО апдейта
            if fold is not None:
                names_old = [f for f in fold if f[1] == ent['offset']]
                if not names_old:
                    warnings.append(f'{const}: в старом дампе у {label} нет поля по '
                                    f'0x{ent["offset"]:X} — карта устарела')
                elif ent.get('field') and names_old[0][0] != ent['field'] and not obfuscated(ent['field']):
                    warnings.append(f'{const}: в старом дампе по 0x{ent["offset"]:X} лежит '
                                    f'{names_old[0][0]}, а карта ждёт {ent["field"]}')
            # 2. то же поле в новом дампе
            hit = None
            if a.verify and obfuscated(ent.get('field', '')):
                unverifiable.append(const)
                continue
            if ent.get('field') and not obfuscated(ent['field']):
                same = [f for f in fnew if f[0] == ent['field']]
                if len(same) == 1:
                    hit = (same[0][1], 'имя')
            if hit is None and amap is not None:
                hit = amap.get(ent['offset'])
            if hit is None:
                warnings.append(f'{const}: поле не найдено в новом дампе — проверь вручную')
                continue
            # Сравниваем с тем, что стоит в заголовке: так одной и той же
            # проверкой ловятся и уехавшее после апдейта поле, и константа,
            # которая была неверной с самого начала.
            if want is None or hit[0] != want:
                changes[const] = (want, hit[0], f'{label}.{ent.get("field")}', hit[1])

    # TYPEINFO_RVA
    rva_consts = [c for c, e in omap.items() if e.get('kind') == 'typeinfo_rva']
    if rva_consts and not a.no_rva:
        classes = [omap[c].get('class') for c in rva_consts if omap[c].get('class')]
        print(f'пересчёт RVA для {len(classes)} классов (дизассемблирование, ~минута)...')
        found = rva_scan(os.path.join(newdir, 'libil2cpp.so'),
                         os.path.join(newdir, 'script.json'), classes)
        for const in rva_consts:
            cls = omap[const].get('class')
            got = found.get(cls)
            cur = header_vals.get(const)
            if got is None:
                warnings.append(f'{const}: RVA для {cls} не найден — запусти typeinfo_rva.py вручную')
            elif cur != got:
                changes[const] = (cur, got, f'TypeInfo {cls}', 'дизасм')

    # ------------------------------------------------------------- отчёт ---
    if unverifiable:
        print(f'без прошлого дампа не проверить (имя поля обфусцировано): '
              f'{len(unverifiable)} шт.\n')
    print(f'проверено констант: {checked} полей + {len(rva_consts)} RVA'
          f'  (не из дампа игры и потому не проверяются: '
          f'{sum(1 for e in omap.values() if e.get("kind") == "runtime")})\n')
    if changes:
        print(f'{"константа":42s} {"было":>10s} {"стало":>10s}  источник')
        for c, (o, n, what, how) in sorted(changes.items()):
            print(f'{c:42s} {("0x%X" % o) if o is not None else "-":>10s} '
                  f'{"0x%X" % n:>10s}  {what} ({how})')
    else:
        print('изменений нет — все константы на месте.')
    if warnings:
        print('\nтребует внимания:')
        for w in warnings:
            print('  ! ' + w)

    if a.apply and changes:
        def sub(m):
            c = m.group(2)
            return m.group(1) + ('0x%X' % changes[c][1]) + m.group(4) if c in changes else m.group(0)
        open(HEADER, 'w', encoding='utf-8').write(CONST_RE.sub(sub, src))
        for c, (o, n, _, _) in changes.items():
            if omap[c].get('kind') == 'field':
                omap[c]['offset'] = n
                fl = new.fields(omap[c]['struct']) or []
                for fn, off, ty in fl:
                    if off == n:
                        omap[c]['field'], omap[c]['type'] = fn, ty
        json.dump(omap, open(a.map, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
        print(f'\nзаписано в {os.path.relpath(HEADER, ROOT)} и карту. '
              f'Проверь сборку и добавь строку в OFFSETS_UPDATE.md.')
    elif changes:
        print('\nчтобы записать: тот же вызов с --apply')
    return 1 if warnings else 0


if __name__ == '__main__':
    sys.exit(main())
