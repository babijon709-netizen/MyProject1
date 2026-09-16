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

Часть констант лежит не в именованной структуре, а в классе ПО указателю из
неё (`PlayerManager.playerEventHandler -> Aim`), и имя того класса тоже
ротирует. Для таких записей в карте есть `via` — путь от стабильной структуры
по читаемым именам полей; скрипт разрешает его в имена структур обоих дампов и
дальше проверяет константу как обычное поле. Запись без `via` в такой цепочке
проверяла бы «у PlayerManager есть поле 0x268» — всегда правда, поэтому сдвиг
внутри хендлера (Aim 0x268 -> 0x270 после вставки KnockDoor) прошёл бы молча.

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
BACKING_RE = re.compile(r'^_(.+)_k__BackingField$')
# «Слово» в обычном регистре: Aim, Jump, KnockDoor, manager, nicklabel.
WORD_RE = re.compile(r'^[A-Z][a-z]+([A-Z][a-z]+)*$|^[a-z][a-z0-9]*([A-Z][a-z0-9]+)*$')
# Сериализованные поля Unity не обфусцируются и не ротируют: m_Text, m_Name.
UNITY_RE = re.compile(r'^m_[A-Z][A-Za-z0-9]*$')


def obfuscated(name):
    """Имя, которое обфускатор перекатит на следующем билде.

    Автосвойства компилятор заворачивает в `_XXXX_k__BackingField`, и XXXX
    там такой же перекатываемый мусор (`_ukT_`, `_QCF_`, `_LPj_` — каждый
    билд свой). Без разворачивания этой обёртки такие поля выглядят
    «читаемыми», и сверка со старым дампом ложно стопорит весь пересчёт:
    имя уехало, а смещение-то осталось прежним.

    Короткое имя ещё не значит мусорное. `Aim`, `Jump`, `Voice` — нормальные
    имена полей в PlayerEventHandler, а `uRP`, `ccW`, `nfD` — перекатываемые.
    Различаем по регистру: слово/camelCase читаемо, смесь регистров внутри
    короткого имени — нет. Цена ошибки здесь высокая: приняв `Aim` за мусор,
    скрипт вообще снимает его с проверки (в --verify) или отдаёт на
    позиционное выравнивание, а оно в классе с перекатываемыми именами типов
    (`norm()` сводит их к одному токену) вставку поля не видит. Именно так
    сдвиг Aim 0x268 -> 0x270 прошёл незамеченным.
    """
    if name.startswith('<'):
        return True
    m = BACKING_RE.match(name)
    if m:
        name = m.group(1)
    if UNITY_RE.match(name):
        return False
    if not OBF_RE.match(name):
        return False
    return not WORD_RE.match(name)


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


def rva_candidates(sofile, scriptfile, classes, top=6, methods=3000):
    """{класс: [(RVA, обращений, отпечаток статик-полей), ...]} — ВСЕ кандидаты.

    Формат вывода typeinfo_rva.py:
        Oxide.PlayerManager  (методов просканировано: 645)
           0xD7AAAF8   обращений=5    читаемые статик-поля[0x1A0x1]

    `--methods 3000` обязателен: со значением по умолчанию (400) у
    PlayerManager в новом билде не находится ни одного кандидата — нужные
    методы просто не попадают в первую четыреста.
    """
    if not classes:
        return {}
    cmd = [sys.executable, os.path.join(HERE, 'typeinfo_rva.py'),
           '--so', sofile, '--script', scriptfile,
           '--top', str(top), '--methods', str(methods)] + classes
    res = subprocess.run(cmd, capture_output=True, text=True)
    found, cur = {}, None
    for line in (res.stdout or '').splitlines():
        head = re.match(r'^(\S+)\s+\(', line)
        if head:
            cur = head.group(1)
            found.setdefault(cur, [])
            continue
        m = re.match(r'^\s+0x([0-9A-Fa-f]{5,})\s+обращений=(\d+)\s+'
                     r'читаемые статик-поля\[([^\]]*)\]', line)
        if cur and m:
            # Смещение статик-поля, БЕЗ счётчика обращений: typeinfo_rva.py
            # печатает их вместе («0x1A0x1» — поле 0x1A, одно чтение), и если
            # оставить счётчик в отпечатке, то сверка «отпечаток статик-полей»
            # ломается от любой правки кода: те же поля читаются другое число
            # раз (у PlayerManager в бете 5->9) — и верный слот теряется.
            fp = tuple(sorted(h.group(0) for h in
                              (re.match(r'0x[0-9A-Fa-f]+', t.strip()) for t in m.group(3).split(','))
                              if h))
            found[cur].append((int(m.group(1), 16), int(m.group(2)), fp))
    return found


def pick_rva(cls, old_c, new_c, cur):
    """Выбор нового RVA по отпечатку старого значения (см. docstring
    typeinfo_rva.py: верхний кандидат верен НЕ всегда — у GameControllerBase
    его стабильно обгоняет чужой слот).

    Возвращает (RVA|None, как_нашли, предупреждение|None).
    """
    if not new_c:
        return None, '', ('кандидатов нет — попробуй --methods больше или другой класс-якорь')
    if not old_c or cur is None:
        return new_c[0][0], 'верхний (сверки со старым не было)', (
            f'{cls}: нет старого дампа/значения — взят верхний кандидат, проверь руками')
    # отпечаток того кандидата, который в СТАРОМ билде и был верным значением
    mine = [(n, s) for r, n, s in old_c if r == cur]
    if not mine:
        return new_c[0][0], 'верхний (старое значение среди кандидатов не нашлось)', (
            f'{cls}: текущее {cur:#x} не воспроизводится на старом дампе — '
            f'взят верхний кандидат {new_c[0][0]:#x}, проверь руками')
    o_refs, o_fp = mine[0]
    exact = [(r, n) for r, n, s in new_c if s == o_fp and n == o_refs]
    if exact:
        return exact[0][0], f'отпечаток (обращений={o_refs}, статик-поля[{", ".join(o_fp) or "-"}]', None
    same_fp = [(r, n) for r, n, s in new_c if s == o_fp]
    if len(same_fp) > 1:
        return same_fp[0][0], 'отпечаток (неоднозначно)', (
            f'{cls}: под отпечаток подходит несколько кандидатов '
            f'{[hex(r) for r, _ in same_fp]} — проверь руками')
    if same_fp:
        return same_fp[0][0], f'отпечаток статик-полей[{", ".join(o_fp) or "-"}]', (
            f'{cls}: число обращений изменилось {o_refs}->{same_fp[0][1]}, '
            f'отпечаток статик-полей совпал')
    return new_c[0][0], 'верхний (отпечаток не совпал)', (
        f'{cls}: отпечаток старого значения [{", ".join(o_fp) or "-"}] в новом дампе '
        f'не встретился — взят верхний кандидат {new_c[0][0]:#x}, ОБЯЗАТЕЛЬНО проверь руками')


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

    # Непрямые цепочки (`via`). Часть констант живёт не в той структуре, где
    # лежит указатель, а в классе ПО этому указателю:
    # PlayerManager.playerEventHandler -> Aim. Имя такого класса обфускатор
    # перекатывает каждый билд (DqO -> Gum), поэтому в карте пишется путь от
    # стабильной структуры по читаемым именам полей, а имя структуры
    # разрешается здесь — отдельно для нового и для старого дампа.
    #
    # Без этого проверка вырождается в «у PlayerManager есть поле по 0x268»,
    # что верно всегда, и сдвиг внутри самого хендлера проходит молча: в билд
    # добавили поле KnockDoor (0x188), Aim уехал 0x268 -> 0x270, а «Только в
    # прицеле» начало читать флаг прыжка. Вдобавок --apply «освежил» имена этих
    # записей по чужой структуре, и provenance превратился в мусор.
    def resolve_via(dump, via):
        st = None
        for i, hop in enumerate(via):
            st = hop.get('struct') or st
            want = [f for f in (dump.fields(st) or []) if f[0] == hop['field']]
            if len(want) != 1 or not want[0][2].endswith('_o*'):
                return None, f'шаг {i + 1}: {st}.{hop["field"]}'
            st = want[0][2][:-len('_o*')] + '_Fields'
        return st, None

    for const, ent in omap.items():
        if ent.get('kind') != 'field' or not ent.get('via'):
            continue
        st_new, bad = resolve_via(new, ent['via'])
        if st_new is None:
            sys.exit(f'{const}: путь via не разрешился в новом дампе ({bad}) — '
                     f'поле переименовали или тип перестал быть указателем; поправь карту')
        ent['_struct'] = st_new
        ent['_struct_old'] = resolve_via(old, ent['via'])[0] if old is not None else None

    src = open(HEADER, encoding='utf-8').read()
    header_vals = {m.group(2): int(m.group(3), 0) for m in CONST_RE.finditer(src)}

    # по структурам, чтобы поля одной структуры выравнивать один раз; ключ —
    # пара (имя в новом дампе, имя в старом): у via-записей они разные
    per_struct = {}
    for const, ent in omap.items():
        if ent.get('kind') == 'field':
            key = (ent.get('_struct') or ent['struct'],
                   ent.get('_struct_old') or ent['struct'])
            per_struct.setdefault(key, []).append(const)

    # Карта описывает то состояние заголовка, которое есть сейчас, а оно
    # получено из ПРЕДЫДУЩЕГО дампа. Если подсунуть дамп через поколение,
    # смещения в карте будут указывать на чужие поля и «изменения» окажутся
    # выдумкой — поэтому сначала проверяем, тот ли это дамп.
    if old is not None:
        ok = bad = 0
        for const, ent in omap.items():
            if ent.get('kind') != 'field':
                continue
            fold = old.fields(ent.get('_struct_old') or ent['struct'])
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

    # Защита от повторного --apply. Карта описывает тот дамп, из которого её
    # последний раз обновляли: запущенная дважды на одном и том же новом дампе,
    # она сдвинет значения ЕЩЁ раз (реальный случай этого апдейта: PIECE
    # 0x100 -> 0x110 -> 0x120). Признак — имена полей в карте совпадают с
    # новым дампом чаще, чем со старым.
    if old is not None:
        mo = mn = 0
        for ent in omap.values():
            if ent.get('kind') != 'field':
                continue
            fo = old.fields(ent.get('_struct_old') or ent['struct'])
            fn = new.fields(ent.get('_struct') or ent['struct'])
            if not fo or not fn:
                continue
            ho = [f for f in fo if f[1] == ent['offset']]
            hn = [f for f in fn if f[1] == ent['offset']]
            if ho and ho[0][0] == ent.get('field'): mo += 1
            if hn and hn[0][0] == ent.get('field'): mn += 1
        if mn >= mo + 3:
            print(f'СТОП: карта уже описывает НОВЫЙ дамп ({mn} имён полей совпадают '
                  f'с новым против {mo} со старым) — значит --apply на этом дампе '
                  f'уже отработал.\n      Повторный прогон сдвинет значения ещё раз. '
                  f'Если заголовок испорчен: git checkout -- '
                  f'{os.path.relpath(HEADER, ROOT)} {os.path.relpath(a.map, ROOT)} '
                  f'и запусти один раз.')
            return 2

    changes, warnings, checked, unverifiable = {}, [], 0, []
    for (struct, struct_old), consts in sorted(per_struct.items()):
        ent0 = omap[consts[0]]
        label = lay.TRACKED.get(struct, struct)
        if ent0.get('via'):
            label = f'{label} via {".".join(h["field"] for h in ent0["via"])}'
        fnew = new.fields(struct)
        fold = old.fields(struct_old) if old else None
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
                # Имя без старого дампа не восстановить, но проверить, что по
                # этому смещению всё ещё поле того же типа, можно: этого
                # хватает, чтобы заметить вставку/удаление поля выше по классу.
                at = [f for f in fnew if f[1] == ent['offset']]
                if not at:
                    warnings.append(f'{const}: в новом дампе у {label} нет поля по '
                                    f'0x{ent["offset"]:X}')
                elif ent.get('type') and lay.norm(at[0][2]) != lay.norm(ent['type']):
                    warnings.append(f'{const}: {label} по 0x{ent["offset"]:X} теперь '
                                    f'{at[0][2]}, а карта ждёт {ent["type"]}')
                else:
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

    # TYPEINFO_RVA — подбираем по отпечатку со старого дампа, не «верхним»
    rva_consts = [c for c, e in omap.items() if e.get('kind') == 'typeinfo_rva']
    if rva_consts and not a.no_rva:
        classes = [omap[c].get('class') for c in rva_consts if omap[c].get('class')]
        print(f'пересчёт RVA для {len(classes)} классов (дизассемблирование, ~минута)...')
        new_c = rva_candidates(os.path.join(newdir, 'libil2cpp.so'),
                               os.path.join(newdir, 'script.json'), classes)
        old_c = rva_candidates(os.path.join(olddir, 'libil2cpp.so'),
                               os.path.join(olddir, 'script.json'), classes) if olddir else {}
        for const in rva_consts:
            cls = omap[const].get('class')
            cur = header_vals.get(const)
            got, how, warn = pick_rva(cls, old_c.get(cls, []), new_c.get(cls, []), cur)
            if warn:
                warnings.append(f'{const}: {warn}')
            if got is None:
                warnings.append(f'{const}: RVA для {cls} не найден — запусти typeinfo_rva.py вручную')
            elif cur != got:
                changes[const] = (cur, got, f'TypeInfo {cls}', how)
            else:
                print(f'  {const}: {got:#x} — без изменений ({how})')

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

    if a.apply and (changes or not a.verify):
        if changes:
            def sub(m):
                c = m.group(2)
                return m.group(1) + ('0x%X' % changes[c][1]) + m.group(4) if c in changes else m.group(0)
            open(HEADER, 'w', encoding='utf-8').write(CONST_RE.sub(sub, src))
            for c, (o, n, _, _) in changes.items():
                if omap[c].get('kind') == 'field':
                    omap[c]['offset'] = n
        # Карта обязана описывать ТЕКУЩИЙ дамп, а не тот, из которого её
        # последний раз меняли: обфусцированные имена полей ротируют каждый
        # билд (MoW -> lzD), и устаревшее имя в карте — это проваленный поиск
        # «по имени» при следующем апдейте. Поэтому имена/типы перечитываются
        # для всех полевых записей, а не только для изменившихся.
        renamed = 0
        fl_cache = {}
        for c, ent in omap.items():
            if ent.get('kind') != 'field':
                continue
            st = ent.get('_struct') or ent['struct']
            if st not in fl_cache:
                fl_cache[st] = new.fields(st) or []
            for fn, off, ty in fl_cache[st]:
                if off == ent['offset'] and (fn != ent.get('field') or ty != ent.get('type')):
                    ent['field'], ent['type'] = fn, ty
                    renamed += 1
                    break
            if ent.get('via'):
                # класс по указателю ротирует — пишем имя из текущего дампа,
                # а путь via остаётся, чтобы в следующий раз разрешить заново
                ent['struct'] = st
        for ent in omap.values():
            ent.pop('_struct', None)
            ent.pop('_struct_old', None)
        json.dump(omap, open(a.map, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
        print(f'\nзаписано в {os.path.relpath(HEADER, ROOT)}'
              f'{"" if not changes else " и карту"}'
              f'{"" if changes else " (карта: имена полей освежены)"}'
              f'{f" (карта: имён полей обновлено {renamed})" if changes and renamed else ""}. '
              f'Проверь сборку и добавь строку в OFFSETS_UPDATE.md.\n'
              f'Если у игры есть бета-версия — пересобери её файл оффсетов от нового '
              f'дампа релиза: python3 tools/offsets/beta_offsets.py --apply')
    elif changes:
        print('\nчтобы записать: тот же вызов с --apply')
    return 1 if warnings else 0


if __name__ == '__main__':
    sys.exit(main())
