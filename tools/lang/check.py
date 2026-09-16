#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Проверка таблиц перевода РУ/EN (tools/lang/run.sh зовёт этот скрипт).

Русские строки живут в коде (XS("...") в модулях jni/src/{app,ui,aim,farm},
таблицы имён — в esp/weapons.cpp), а
переводы — в jni/src/lang_tables.inc, который собирает gen_tables.py. Разъехаться
стороны могут молча: добавили пункт меню, забыли пару — и он остаётся русским в
английском меню, а заметить это можно только на устройстве. Поэтому здесь:

  * lang_tables.inc должен совпадать с тем, что собирает gen_tables.py (то есть
    каждая строка из кода переведена, а лишних пар нет);
  * таблицы отсортированы по байтам UTF-8 и без дублей — по этому порядку идёт
    бинарный поиск в lang.cpp: при сдвинутой сортировке часть подписей молча
    осталась бы русской;
  * английские имена оружия совпадают с тем, как называет его игра
    (kWeaponNames в esp/weapons.cpp, колонка `en`), кроме подписей, заведённых
    руками в gen_tables.py.

Строки мини-лога (mlog::line / mlog::every) не проверяются — как и прежний лог
автофарма: они намеренно остаются русскими, лог разбирают по подписям.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import gen_tables  # noqa: E402  (таблицы и правило выбора имён оружия)

REPO = os.path.join(HERE, '..', '..')
INC = os.path.join(REPO, 'jni/src/lang_tables.inc')


def parse_tables(text):
    """Прочитать lang_tables.inc: {имя таблицы: {ключ: перевод}}."""
    tables, name = {}, None
    for line in text.split('\n'):
        m = re.search(r'static const LangPair (\w+)\[\] = \{', line)
        if m:
            name = m.group(1); tables[name] = {}
            continue
        if name is None:
            continue
        if line.strip() == '};':
            name = None; continue
        if not line.strip():
            continue
        m = re.match(r'\s*\{"((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)"\},\s*$', line)
        if not m:
            raise SystemExit('%s: не разобрана строка таблицы: %r' % (INC, line))
        ru, en = m.group(1), m.group(2)
        if ru in tables[name]:
            raise SystemExit('%s: ключ «%s» в %s повторяется' % (INC, ru, name))
        tables[name][ru] = en
    return tables


def sorted_ok(table):
    """Таблица отсортирована по байтам UTF-8 — по этому порядку идёт поиск."""
    keys = list(table.keys())
    for a, b in zip(keys, keys[1:]):
        if a.encode('utf-8') >= b.encode('utf-8'):
            return False, (a, b)
    return True, None


def main():
    problems = []

    # 1. Файл собран из текущего кода: и полнота, и отсутствие лишнего.
    try:
        expected, _visual = gen_tables.build_text()
    except SystemExit as e:
        print('сборка таблиц перевода: ПРОВАЛ')
        print('  %s' % e)
        return 1
    have = open(INC, encoding='utf-8').read()
    if have != expected:
        problems.append('lang_tables.inc разошёлся с gen_tables.py — '
                        'запусти: python3 tools/lang/gen_tables.py')

    tables = parse_tables(have)
    ui = tables.get('kUiEn')
    visual = tables.get('kVisualEn')
    if ui is None or visual is None:
        problems.append('в lang_tables.inc нет таблиц kUiEn/kVisualEn')
        print('проверка таблиц перевода: ПРОВАЛ')
        for p in problems:
            print('  ' + p)
        return 1

    # 2. Сортировка: бинарный поиск промахнётся молча.
    for name, table in tables.items():
        ok, pair = sorted_ok(table)
        if not ok:
            problems.append('%s не отсортирована: «%s» идёт после «%s» '
                            '(бинарный поиск промахнётся)' % (name, pair[1], pair[0]))

    # 3. Оружие: имя должно быть одним из тех, что игра пишет в своём UI.
    #    Одной подписи игра может давать несколько имён (у «Топора» это Axe и
    #    Hatchet), а подписи, заведённые руками в gen_tables.VISUAL_EN, вольны
    #    не совпадать вовсе — они и так наши.
    game_src = gen_tables.strip_comments(gen_tables.game_sources())
    allowed = {}
    for _key, en, ru in re.findall(
            r'\{"([a-z0-9_\.]+)",\s*"((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)"\}', game_src):
        allowed.setdefault(ru, set()).add(en)
    for ru, names in sorted(allowed.items()):
        if ru in gen_tables.VISUAL_EN or ru in ui or ru not in visual:
            continue
        pick = gen_tables.WEAPON_EN_PICK.get(ru)
        if pick is not None and visual[ru] != pick:
            problems.append('оружие «%s»: в таблице «%s», а gen_tables.py держит «%s»'
                            % (ru, visual[ru], pick))
        elif pick is None and visual[ru] not in names:
            problems.append('оружие «%s»: в таблице «%s», а игра называет его %s'
                            % (ru, visual[ru], '/'.join(sorted(names))))

    if problems:
        print('проверка таблиц перевода: ПРОВАЛ')
        for p in problems:
            print('  ' + p)
        return 1
    print('таблицы перевода: меню %d пар, визуалов %d пар — покрытие полное, '
          'сортировка верна' % (len(ui), len(visual)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
