#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Дописать в карту оффсетов источник каждой константы.

    python3 tools/offsets/build_map.py /tmp/dump         # обновить offsets_map.json
    python3 tools/offsets/build_map.py /tmp/dump --check # только проверить, не писать

Карта (tools/offsets/offsets_map.json) — машиночитаемый источник истины про
оффсеты. До этой правки в ней лежали число и пояснение «что это», но не было
главного: **где в дампах это искать**, когда игра обновится. Скрипт добавляет
каждой записи блок `src`:

    "src": {"file": "il2cpp.h", "struct": "...", "field": "...", "how": "..."}

 * `kind: field` — смещение поля: файл il2cpp.h, структура, имя поля, тип.
   Проверяется автоматически (verify_map.py): 136 из 137 сошлись с дампом.
   Имя поля обфусцировано и ротирует от билда к билду, поэтому в `how`
   лежит подсказка, чем_field_ опознать, если имя переехало (тип, соседи,
   цепочка via).
 * `kind: typeinfo_rva` — слот глобального Il2CppClass* в .data.rel.ro
   libil2cpp.so: уезжает КАЖДЫЙ билд, ищется typeinfo_rva.py по обращениям
   из методов класса и отпечатку (сколько обращений, есть ли статик-поля).
 * `kind: runtime` — раскладка Unity/IL2CPP-объектов: из dump.cs не выводится
   вовсе. Живёт либо в libunity.so (нативные Camera/Transform/GameObject),
   либо в ABI il2cpp (System.String, массивы, generic-обёртки), либо
   выведена из ПОРЯДКА полей в il2cpp.h + заголовка объекта 0x10. Уезжает
   только при смене версии Unity/IL2CPP — поэтому первым делом при
   обновлении сравнивается версия движка (поле `_build.unity`).

Кроме того, скрипт:
 * добавляет в карту константы, которых в ней не было (сверяет с
   jni/src/game_offsets.h);
 * пишет блок `_build` — отпечаток текущих дампов (версия Unity, версия
   метаданных, число типов/методов/полей, размеры и SHA1 файлов). По нему
   после обновления игры видно, что именно приехало: новая версия движка
   значит пересчитывать и класс C, те же цифры при новом билде игры —
   только поля и RVA.
"""

import argparse
import collections
import hashlib
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
MAP = os.path.join(HERE, 'offsets_map.json')
HEADER = os.path.join(ROOT, 'jni', 'src', 'game_offsets.h')

CONST_RE = re.compile(r'^inline constexpr std::uint64_t (\w+)\s*=\s*(0x[0-9A-Fa-f]+);\s*(.*)$')

# ---------------------------------------------------------------------------
# Источники констант класса «runtime»: куда смотреть в дампах.
# Формат: имя -> (файл, как найти/чем проверено)
# ---------------------------------------------------------------------------
RUNTIME_SRC = {
    # --- нативная Camera (libunity.so) -------------------------------------
    'CAMERA_VIEW_MATRIX': ('libunity.so',
        'get_worldToCameraMatrix_Injected @ 0x5ac514 -> хелпер 0xe1fe08 отдаёт cam+0x70 '
        '(записано в комментарии game_offsets.h); кеш ленивый, живёт по dirty-байту +0x502'),
    'CAMERA_PROJECTION_MATRIX': ('libunity.so',
        'get_projectionMatrix_Injected @ 0x5ac53c -> хелпер 0xe1fe6c отдаёт cam+0xB0'),
    'CAMERA_WORLD_TO_CLIP': ('libunity.so',
        'worldToClip идёт следом за projection; в коде не используется, только замер для лога'),
    'CAMERA_PREV_VIEW_PROJ': ('libunity.so',
        'матрица прошлого кадра (motion vectors); в коде не используется'),
    'CAMERA_NATIVE_TRANSFORM': ('libunity.so',
        'тот Transform, по которому Unity пересобирает кеш view при dirty-флаге; '
        'этим же указателем пользуется ESP (read_native_camera_matrices) и фрикам'),
    'CAMERA_FOV_DEGREES': ('libunity.so',
        'get_fieldOfView; проверено поведением: круг FOV в оверлее совпадает с прицелом игры'),
    'CAMERA_ASPECT': ('libunity.so',
        'get_aspect; проверено совпадением с sw/sh экрана'),
    'CAMERA_NEAR_CLIP': ('libunity.so',
        'get_nearClipPlane; проверено самой надёжной проверкой — иксрей пишет сюда, '
        'и картинка меняется, а при выключении восстанавливается'),
    'CAMERA_FAR_CLIP': ('libunity.so',
        'get_farClipPlane; лежит рядом с near (+4), проверено чтением'),
    'CAMERA_VIEW_DIRTY': ('libunity.so',
        'dirty-байт кеша view: при движении камеры меняется, без движения — нет'),
    'CAMERA_PROJ_DIRTY': ('libunity.so',
        'dirty-байт кеша projection'),
    # --- нативные Transform / Component / GameObject -----------------------
    'TRANSFORM_CHILDREN_ARRAY': ('libunity.so',
        'нативный Transform: массив детей; проверено обходом иерархии (маркеры, кости, '
        'поиск трансформа камеры)'),
    'TRANSFORM_CHILD_COUNT': ('libunity.so',
        'нативный Transform: int32 число детей, проверено тем же обходом'),
    'COMPONENT_GAMEOBJECT': ('libunity.so',
        'нативный Component.m_GameObject; проверено переходом компонент -> объект'),
    'GAMEOBJECT_COMPONENT_ARRAY': ('libunity.so',
        'нативный GameObject.m_Component: массив пар {GameObject*, Component*}'),
    'COMPONENT_PAIR_PTR': ('libunity.so',
        'второй указатель в паре элемента m_Component'),
    'GAMEOBJECT_NAME_GUESS': ('libunity.so',
        'core::string с SSO; уточняется в рантайме перебором кандидатов по именам'),
    # --- ABI il2cpp (в dump.cs нет, раскладкаRuntime-объектов) -------------
    'MANAGED_CACHED_PTR': ('ABI + il2cpp.h',
        'UnityEngine.Object: за заголовком объекта (klass 0x0, monitor 0x8) первым идёт '
        'm_CachedPtr -> 0x10; проверено на всех managed-обёртках'),
    'IL2CPP_ARRAY_LENGTH': ('ABI il2cpp',
        'Il2CppArray: klass 0x0, monitor 0x8, bounds* 0x10, max_length 0x18, данные 0x20'),
    'IL2CPP_ARRAY_FIRST_ELEMENT': ('ABI il2cpp',
        'данные массива начинаются с 0x20'),
    'IL2CPP_CLASS_NAME': ('libil2cpp.so (ABI, metadata v39)',
        'Il2CppClass.name; проверяется в рантайме: код читает имя класса и сверяет со '
        'строкой — пока имена совпадают, смещение верное'),
    'IL2CPP_CLASS_NAMESPACE': ('libil2cpp.so (ABI, metadata v39)',
        'Il2CppClass.namespaze, следующим полем'),
    # --- generic-обёртки: смещений в дампе нет, считаем по порядку полей ---
    'IL2CPP_STRING_LENGTH': ('il2cpp.h',
        'struct System_String_Fields: _stringLength /* 0x10 */; ПРОВЕРЕНО по дампу'),
    'IL2CPP_STRING_CHARS': ('il2cpp.h',
        'struct System_String_Fields: _firstChar /* 0x14 */; ПРОВЕРЕНО по дампу'),
    'IL2CPP_LIST_ITEMS': ('il2cpp.h + ABI',
        'System_Collections_Generic_List_1_T__Fields: порядок (_items, _size, _version, '
        '_syncRoot) плюс заголовок объекта 0x10 -> 0x10 и 0x18. В дампе у generic '
        'смещений нет, поэтому считается по порядку'),
    'IL2CPP_LIST_SIZE': ('il2cpp.h + ABI',
        'тот же порядок: _size вторым -> 0x18'),
    'DICT_ENTRIES': ('il2cpp.h + ABI',
        'System_Collections_Generic_Dictionary_2_TKey_TValue__Fields: порядок (_buckets, '
        '_entries, ...) плюс заголовок 0x10 -> 0x18'),
    'DICT_ENTRY_STRIDE': ('il2cpp.h + ABI',
        'Entry{TKey,TValue}: (hashCode i4, next i4, key ptr, value ptr) = 0x18. '
        'Внутри массива заголовка объекта нет — это важно, иначе stride был бы 0x28'),
    'DICT_ENTRY_VALUE': ('il2cpp.h + ABI',
        'value — четвёртое поле Entry: 0x0+4+8 = 0x10'),
    'SYNC_VALUE_OFFSET': ('il2cpp.h + ABI',
        'Mirror SyncVar<T>: обёртка из двух полей, значение лежит в 0x20; найдено по '
        'содержимому (по этому смещению читается само значение SyncVar)'),
    'GUI_VALUE': ('il2cpp.h + ABI',
        'обёртка GuI`1<T>: в il2cpp.h у GuI_1_Fields полей нет (generic), 0x20 '
        'подтверждено дизассемблером геттера'),
    # --- обфусцированные классы игры: имя ротирует, ищем по составу полей --
    'WEAPONVIEW_WEAPON_BASE': ('il2cpp.h + dump.cs',
        'обёртка weapon view: имя класса обфусцировано и ротирует. Искать класс, у '
        'которого рядом лежат WeaponBase*, WeaponPiece* и Transform*'),
    'WEAPONVIEW_PIECE': ('il2cpp.h + dump.cs',
        ' WeaponPiece — второе поле того же класса, сразу за WeaponBase'),
    'WEAPONVIEW_ROOT_TRANSFORM': ('il2cpp.h + dump.cs',
        'корневой Transform модели оружия в том же классе'),
    'WEAPONVIEW_INNER': ('il2cpp.h + dump.cs',
        'внутренний объект обёртки (0x10) — сразу за заголовком'),
    'TOD_SKY_CYCLE': ('il2cpp.h + dump.cs',
        'TOD_Sky.Cycle: имя класса ротирует (IY -> UV). Искать класс с полем типа Cycle; '
        'проверяется «всегда день» — время суток возвращается в полдень'),
    'TOD_SCAN_RVA_BEGIN': ('libil2cpp.so',
        'начало окна .data.rel.ro со слотами глобальных Il2CppClass*, которое '
        'перебирает always_day_tick(); уезжает вместе с бинарём'),
    'TOD_SCAN_RVA_END': ('libil2cpp.so',
        'конец окна скана = BEGIN + 0xA0000'),
}

# Константы, которых не было в карте: что это и откуда.
NEW_FIELDS = {
    'EVENT_HANDLER_LAST_HIT_POINT': dict(
        struct='Gum_Fields', field='LastLocalHitPoint', type='Il2CppObject*',
        via=[dict(struct='Oxide_PlayerManager_Fields', field='playerEventHandler')],
        note='последняя точка попадания (обёртка SyncVar) — замер для лога'),
    'EVENT_HANDLER_LAST_HIT_TIME': dict(
        struct='Gum_Fields', field='LastLocalHitTime', type='Il2CppObject*',
        via=[dict(struct='Oxide_PlayerManager_Fields', field='playerEventHandler')],
        note='время последнего попадания (обёртка SyncVar) — замер для лога'),
    'VITALS_MAX_HEALTH': dict(
        struct='Oxide_GenericVitals_Fields', field='m_MaxHealth', type='float',
        note='GenericVitals: максимум здоровья — знаменатель полоски здоровья в ESP'),
    'MINEABLE_HIT_ANCHOR': dict(
        struct='Oxide_MineableObject_Fields', field='LXX', type='UnityEngine_Transform_o*',
        note='якорь точки удара по руде (ZgL() отдаёт его мировую позицию); имя LXX ротирует'),
    'ARP_LATEST_VALUE': dict(
        kind='runtime', file='il2cpp.h + ABI',
        how='Cysharp_Threading_Tasks_AsyncReactiveProperty_1_Fields: порядок (triggerEvent, '
            'latestValue) плюс заголовок 0x10 -> 0x18',
        note='AsyncReactiveProperty<T>.latestValue — реактивное свойство (здоровье у сущностей)'),
    'PMP_ENTITY': dict(
        kind='runtime', file='? (класс ротирует)',
        how='не используется в коде (только в переключателе версий). Класс «pmK» в '
            'текущем дампе не опознан: имя обфусцировано и сменилось',
        note='НЕ ИСПОЛЬЗУЕТСЯ. При обновлении можно не переносить; если понадобится — '
             'искать класс, у которого на 0x98 лежит AsyncReactiveProperty_1_o*'),
    'PMK_HEALTH': dict(
        kind='runtime', file='? (класс ротирует)',
        how='не используется в коде. Если понадобится: в том же классе, что PMP_ENTITY, '
            'поле типа AsyncReactiveProperty<float> — проверить по 0x98',
        note='НЕ ИСПОЛЬЗУЕТСЯ. AsyncReactiveProperty<float> здоровья сущности'),
}


def sha1(path, short=12):
    h = hashlib.sha1()
    with open(path, 'rb') as fh:
        for chunk in iter(lambda: fh.read(1 << 22), b''):
            h.update(chunk)
    return h.hexdigest()[:short]


def read_header(path):
    """Константы релизного заголовка: имя -> (значение, комментарий строки)."""
    out = collections.OrderedDict()
    for line in open(path, encoding='utf-8'):
        m = CONST_RE.match(line)
        if m:
            out[m.group(1)] = (int(m.group(2), 16), m.group(3).strip())
    return out


def build_dump_info(dumpdir):
    """Отпечаток дампов: чем отличать «приехала игра» от «приехал движок»."""
    info = collections.OrderedDict()
    for name in ('dump.cs', 'il2cpp.h', 'script.json', 'libil2cpp.so', 'libunity.so'):
        p = os.path.join(dumpdir, name)
        if os.path.exists(p):
            info[name] = collections.OrderedDict(
                size=os.path.getsize(p), sha1_12=sha1(p))

    # версия движка — по ней решается судьба класса C (runtime/ABI)
    unity = None
    for so in ('libil2cpp.so', 'libunity.so'):
        p = os.path.join(dumpdir, so)
        if not os.path.exists(p):
            continue
        out = subprocess.run(['strings', '-n', '8', p], capture_output=True, text=True).stdout
        found = sorted(set(re.findall(r'\b(\d{4}\.\d+\.\d+[a-z0-9]*)\b', out)))
        if found:
            unity = found[0]
            break
    info['unity'] = unity

    # версия метаданных и размеры таблиц — из шапки dump.cs
    head = ''
    with open(os.path.join(dumpdir, 'dump.cs'), encoding='utf-8', errors='replace') as fh:
        for _ in range(6):
            head += fh.readline()
    m = re.search(r'metadata v(\d+)', head)
    info['metadata_version'] = m.group(1) if m else None
    m = re.search(r'types (\d+), methods (\d+), fields (\d+)', head)
    if m:
        info['counts'] = collections.OrderedDict(types=int(m.group(1)),
                                                 methods=int(m.group(2)),
                                                 fields=int(m.group(3)))
    return info


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dumpdir', nargs='?', default='/tmp/dump')
    ap.add_argument('--check', action='store_true', help='не писать карту, только показать, что изменилось')
    a = ap.parse_args()

    d = json.load(open(MAP, encoding='utf-8'), object_pairs_hook=collections.OrderedDict)
    consts = read_header(HEADER)

    added, patched = [], []
    # 1. новые константы из заголовка
    for name, spec in NEW_FIELDS.items():
        if name in d:
            continue
        rec = collections.OrderedDict()
        if spec.get('kind') == 'runtime':
            rec['kind'] = 'runtime'
            rec['offset'] = consts.get(name, (0,))[0]
            rec['note'] = spec.get('note', '')
            rec['src'] = collections.OrderedDict(file=spec['file'], how=spec['how'])
        else:
            rec['kind'] = 'field'
            rec['class'] = spec.get('class', '')
            rec['struct'] = spec['struct']
            rec['field'] = spec['field']
            rec['type'] = spec['type']
            rec['offset'] = consts[name][0]
            if spec.get('via'):
                rec['via'] = [collections.OrderedDict(s) for s in spec['via']]
            rec['note'] = spec.get('note', '')
            rec['src'] = collections.OrderedDict(
                file='il2cpp.h', struct=spec['struct'], field=spec['field'],
                how='смещение из комментария /* 0x%X */ рядом с полем' % consts[name][0])
        d[name] = rec
        added.append(name)

    # 2. источники
    for name, rec in d.items():
        kind = rec.get('kind')
        if kind == 'field':
            src = collections.OrderedDict(file='il2cpp.h')
            if rec.get('struct'):
                src['struct'] = rec['struct']
            if rec.get('field'):
                src['field'] = rec['field']
            src['how'] = ('смещение — из комментария /* 0x%X */ рядом с полем; имя поля '
                          'обфусцировано и ротирует от билда к билду, поэтому при '
                          'обновлении поле ищется по смещению и типу «%s»%s'
                          % (rec.get('offset', 0), rec.get('type', ''),
                             ', цепочка via — по читаемым именам полей' if rec.get('via') else ''))
        elif kind == 'typeinfo_rva':
            src = collections.OrderedDict(
                file='libil2cpp.so', section='.data.rel.ro',
                how='слот глобального Il2CppClass*: ищется typeinfo_rva.py по обращениям '
                    'adrp/ldr из методов самого класса с последующим чтением '
                    'static_fields (ldr x8,[klass,#0xB8]). Уезжает КАЖДЫЙ билд — '
                    'отпечаток см. в PROVENANCE.md, §B')
        else:
            if name in RUNTIME_SRC:
                file_, how = RUNTIME_SRC[name]
                src = collections.OrderedDict(file=file_, how=how)
            elif 'src' in rec:
                src = rec['src']          # уже заполнен выше (новые константы)
            else:
                src = collections.OrderedDict(file='?', how='источник не записан — заполнить вручную')
        if rec.get('src') != src:
            rec['src'] = src
            patched.append(name)

    # 3. отпечаток дампов
    d['_build'] = build_dump_info(a.dumpdir)

    print('добавлено констант: %d (%s)' % (len(added), ', '.join(added)))
    print('источников дописано/обновлено: %d' % len(patched))
    unknown = [n for n, r in d.items() if not n.startswith('_') and r.get('src', {}).get('file') == '?']
    if unknown:
        print('без источника:', unknown)
    print('версия движка: %s, metadata v%s, типов %s' % (
        d['_build'].get('unity'), d['_build'].get('metadata_version'),
        d['_build'].get('counts', {}).get('types')))

    if a.check:
        return 0
    with open(MAP, 'w', encoding='utf-8') as fh:
        json.dump(d, fh, ensure_ascii=False, indent=1)
        fh.write('\n')
    print('записано:', MAP)
    return 0


if __name__ == '__main__':
    sys.exit(main())
