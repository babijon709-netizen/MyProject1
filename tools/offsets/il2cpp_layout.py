#!/usr/bin/env python3
"""Сравнение раскладки структур в двух il2cpp.h (старый и новый дампы).

Зачем: у этой игры **имена классов и полей переобфусцируются каждый билд**
(`fvp`→`pmi`, `wK`→`ij`, `Mo`→`sR`, ...). Сравнение по именам полей врёт —
оно скажет "moved=0", молча проигнорировав каждое переименованное поле.
Поэтому сравниваем **позиционно**: последовательность (смещение → тип),
а обфусцированные имена типов нормализуются в `OBF_o`.

    # какие из наших структур поменяли раскладку
    python3 tools/offsets/il2cpp_layout.py diff --old old/il2cpp.h --new new/il2cpp.h

    # полный список полей структуры в обоих дампах
    python3 tools/offsets/il2cpp_layout.py show Oxide_LootObject_Fields --old ... --new ...

    # найти переименованный класс по "форме" — набору типов его полей
    python3 tools/offsets/il2cpp_layout.py find --new new/il2cpp.h \
        "WeaponBase_o*" "Oxide_WeaponPiece_o" "UnityEngine_Transform_o*"

Подводные камни, уже наступленные:
  * `awk '/^struct X_Fields/,/^};/'` цепляет forward-декларацию, а не тело;
    поэтому индексируем только строки, заканчивающиеся на `{`.
  * needles у `find` — обычные подстроки, не регэкспы (`\\*` ничего не найдёт).
"""
import argparse
import json
import os
import re
import sys

# Структуры, на которые опирается jni/src/game_offsets.h. Если после апдейта
# какая-то из них печатается как "identical layout" — её константы трогать не надо.
TRACKED = {
    'Oxide_PlayerManager_Fields': 'PlayerManager',
    'Oxide_PlayerManager_StaticFields': 'PlayerManager.static',
    'Oxide_PlayerInventory_Fields': 'PlayerInventory',
    'Oxide_Inventory_Fields': 'Inventory',
    'HyperHug_Games_Oxide_Features_Player_KCC_Fields': 'KCC',
    'HyperHug_Games_Oxide_Features_Player_CharacterAnimation_Fields': 'CharacterAnimation',
    'HyperHug_Games_Oxide_By_Namespace_Oxide_Damage_System_Ragdoll_Fields': 'Ragdoll',
    'HyperHug_Games_Oxide_By_Namespace_Oxide_Damage_System_Ragdoll_BodyPart_Fields': 'Ragdoll.BodyPart',
    'Oxide_HitBox_Fields': 'HitBox',
    'HyperHug_Games_Oxide_Features_Network_HitBoxRecorderRoot_Fields': 'HitBoxRecorderRoot',
    'Oxide_Item_Fields': 'Item',
    'Oxide_ItemData_Fields': 'ItemData',
    'Oxide_LootItem_Fields': 'LootItem',
    'Oxide_LootObject_Fields': 'LootObject',
    'Oxide_ItemPickup_Fields': 'ItemPickup',
    'Oxide_MineableObject_Fields': 'MineableObject',
    'Oxide_FPManager_Fields': 'FPManager',
    'Oxide_FPObject_Fields': 'FPObject',
    'Oxide_FPWeaponBase_Fields': 'FPWeaponBase',
    'HyperHug_Games_Oxide_Features_Weapons_PlayerWeapon_Fields': 'PlayerWeapon',
    'Oxide_WeaponPiece_Fields': 'WeaponPiece',
    'HyperHug_Games_Oxide_Features_Player_PlayerModelInfo_Fields': 'PlayerModelInfo',
    'Oxide_CameraManager_Fields': 'CameraManager',
    'Mirror_NetworkIdentity_Fields': 'NetworkIdentity',
    'Mirror_NetworkClient_StaticFields': 'NetworkClient.static',
    'Oxide_GameControllerBase_StaticFields': 'GameControllerBase.static',
    'Dissonance_VoicePlayerState_Fields': 'VoicePlayerState',
    'UnityEngine_UI_Text_Fields': 'UI.Text',
}

FIELD_RE = re.compile(r'\s*(.+?)\s+([A-Za-z0-9_]+)\s*;\s*/\* (0x[0-9A-Fa-f]+) \*/')
STRUCT_RE = re.compile(rb'struct ([A-Za-z0-9_]+)\s*(?::\s*([A-Za-z0-9_]+)\s*)?\{')


class Dump:
    """il2cpp.h + индекс "имя структуры -> байтовый диапазон её тела"."""

    def __init__(self, path):
        self.path = path
        self.index = self._index()

    def _index(self):
        cache = self.path + '.idx.json'
        try:
            if os.path.getmtime(cache) >= os.path.getmtime(self.path):
                return json.load(open(cache))
        except OSError:
            pass
        idx, pos, start, name = {}, 0, None, None
        with open(self.path, 'rb') as f:
            for line in f:
                if line.startswith(b'struct ') and line.rstrip().endswith(b'{'):
                    m = STRUCT_RE.match(line)
                    if m:
                        start, name = pos, m.group(1).decode()
                elif line.startswith(b'};') and start is not None:
                    idx[name] = (start, pos + len(line))
                    start = name = None
                pos += len(line)
        try:
            json.dump(idx, open(cache, 'w'))
        except OSError:
            pass
        return idx

    def body(self, struct):
        if struct not in self.index:
            return None
        a, b = self.index[struct][0], self.index[struct][1]
        with open(self.path, 'rb') as f:
            f.seek(a)
            return f.read(b - a).decode('utf8', 'replace')

    def fields(self, struct):
        """-> [(имя, смещение, тип)] или None, если структуры нет."""
        blob = self.body(struct)
        if blob is None:
            return None
        out = []
        for ln in blob.split('\n')[1:]:
            m = FIELD_RE.match(ln)
            if m:
                out.append((m.group(2), int(m.group(3), 16), m.group(1).strip()))
        return out


def norm(t):
    """Обфусцированные имена типов (`fvp_o*`, `Mo_o`) -> `OBF_o`."""
    return re.sub(r'\b[A-Za-z]{1,4}(_[A-Za-z0-9]{1,4})*_o\b', 'OBF_o', t.strip())


def cmd_diff(a):
    old, new = Dump(a.old), Dump(a.new)
    names = a.structs or list(TRACKED)
    changed = 0
    for st in names:
        label = TRACKED.get(st, st)
        fo, fn = old.fields(st), new.fields(st)
        if fo is None or fn is None:
            print(f"[{label:26}] ОТСУТСТВУЕТ: old={fo is not None} new={fn is not None}")
            changed += 1
            continue
        do = {o: norm(t) for _, o, t in fo}
        dn = {o: norm(t) for _, o, t in fn}
        diffs = [o for o in sorted(set(do) | set(dn)) if do.get(o) != dn.get(o)]
        if not diffs:
            print(f"[{label:26}] раскладка не изменилась ({len(fo)} полей)")
            continue
        changed += 1
        print(f"[{label:26}] расходятся {len(diffs)} смещений:")
        for o in diffs[:a.limit]:
            print(f"       0x{o:03X}  old={do.get(o, '-'):<38} new={dn.get(o, '-')}")
        if len(diffs) > a.limit:
            print(f"       ... ещё {len(diffs) - a.limit}")
    print(f"\nструктур с изменённой раскладкой: {changed} из {len(names)}")
    return 0


def cmd_show(a):
    for tag, path in (('OLD', a.old), ('NEW', a.new)):
        if not path:
            continue
        fl = Dump(path).fields(a.struct)
        if fl is None:
            print(f"  {tag}: <нет такой структуры>")
            continue
        print(f"  {tag} {a.struct} ({len(fl)} полей)")
        for n, o, t in fl:
            print(f"     0x{o:03X}  {n:<42} {t}")
    return 0


def cmd_find(a):
    """Поиск структуры по форме: все *_Fields, содержащие все указанные типы."""
    d = Dump(a.new if a.new else a.old)
    for name in d.index:
        if not name.endswith('_Fields'):
            continue
        lo, hi = d.index[name]
        if hi - lo > a.maxbytes:
            continue
        blob = d.body(name)
        if all(nd in blob for nd in a.types):
            print("==", name)
            for n, o, t in d.fields(name):
                print(f"   0x{o:03X} {n:<26}{t}")
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--old', help='старый il2cpp.h')
    p.add_argument('--new', help='новый il2cpp.h')
    sub = p.add_subparsers(dest='cmd', required=True)

    d = sub.add_parser('diff', help='позиционный diff раскладки (по умолчанию — все наши структуры)')
    d.add_argument('structs', nargs='*')
    d.add_argument('--limit', type=int, default=14)
    d.set_defaults(func=cmd_diff)

    s = sub.add_parser('show', help='печать всех полей структуры')
    s.add_argument('struct')
    s.set_defaults(func=cmd_show)

    f = sub.add_parser('find', help='найти переименованный класс по типам его полей')
    f.add_argument('types', nargs='+', help='подстроки типов, НЕ регэкспы')
    f.add_argument('--maxbytes', type=int, default=12000)
    f.set_defaults(func=cmd_find)

    a = p.parse_args()
    if a.cmd in ('diff',) and not (a.old and a.new):
        p.error('diff требует --old и --new')
    if a.cmd == 'find' and not (a.new or a.old):
        p.error('find требует --new (или --old)')
    return a.func(a)


if __name__ == '__main__':
    sys.exit(main())
