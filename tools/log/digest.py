#!/usr/bin/env python3
"""Разбор лога оверлея: что писала игра, где подвисало, что с осью сайлента.

    python3 tools/log/digest.py [файл.log]

Без аргумента берётся самый свежий *.log в корне репозитория. Формат строки:

    MM-DD HH:MM:SS.mmm (+X.X) текст

Понимает и новый лог (сайлент / память / СТОПОР / стадии), и старый
(сводка / мир / боксы / привязка) — по старым логам тоже можно снять сводку.
"""

import glob
import os
import re
import sys

LINE = re.compile(r'^(\d\d)-(\d\d) (\d\d):(\d\d):(\d\d)\.(\d\d\d) \(\+[\d.]+\) (.*)$')

# Классы строк: что искать в тексте -> как назвать в сводке.
CLASSES = [
    ('СТОПОР',            ('СТОПОР:',)),
    ('кадры пошли снова', ('кадры пошли снова',)),
    ('сайлент: ось',      ('ось до записи',)),
    ('сайлент: цепь',     ('сайлент: цепь',)),
    ('память: MouseLook', ('память: MouseLook',)),
    ('аим (новый)',       ('аим: память', 'аим: сайлент', 'аим: тач')),
    ('аим (старый)',      ('аим: не активен', 'аим: работает')),
    ('сводка',            ('сводка:',)),
    ('мир',               ('мир:',)),
    ('боксы',             ('боксы:',)),
    ('привязка',          ('привязка:',)),
    ('отказы чтения',     ('отказы чтения',)),
    ('режим аима',        ('выбран режим', 'аим: выключен', 'аим: включён')),
    ('перезапуск лога',   ('лог открыт', 'лог перезаписан')),
]

MODE_RE = re.compile(r'выбран режим (\d+) \(([^)]*)\)')
STOP_RE = re.compile(r'СТОПОР: кадр (\d+) не идёт ([\d.]+) с, стадия: (.*)$')
RESUME_RE = re.compile(r'кадры пошли снова \(кадр (\d+)\)')
AXIS_RE = re.compile(r'кадр=(\d+) ось до записи рысканье=(-?[\d.]+) тангаж=(-?[\d.]+) '
                     r'\(писали (-?[\d.]+)/(-?[\d.]+)\) — (НАШЕ|игры); '
                     r'отклонение (-?[\d.]+)/(-?[\d.]+)(; был выстрел)?')
CHAIN_RE = re.compile(r'цепь адрес=0x[0-9a-f]+ значение=\((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\) '
                      r'длина=([\d.]+)')
WRITE_FAIL_RE = re.compile(r'запись оси не прошла, адрес=0x[0-9a-f]+ errno=(\d+)')
SUMMARY_RE = re.compile(r'чтений (\d+), отказов (\d+), переоткрытий (\d+)')


def seconds(m):
    """Секунды от начала суток: без года, только для разниц внутри прогона."""
    return ((int(m.group(1)) * 31 + int(m.group(2))) * 24 + int(m.group(3))) * 3600 \
        + int(m.group(4)) * 60 + int(m.group(5)) + int(m.group(6)) / 1000.0


def classify(text):
    for name, keys in CLASSES:
        if any(k in text for k in keys):
            return name
    return 'прочее'


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else None
    if not path:
        root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        cands = sorted(glob.glob(os.path.join(root, '*.log')), key=os.path.getmtime)
        if not cands:
            sys.exit('в корне репозитория нет ни одного *.log')
        path = cands[-1]

    rows = []           # (время или None, текст, класс)
    for raw in open(path, encoding='utf-8', errors='replace'):
        raw = raw.rstrip('\n')
        m = LINE.match(raw)
        if m:
            rows.append((seconds(m), m.group(7), classify(m.group(7)), raw[:19]))
        elif raw.strip():
            rows.append((None, raw.strip(), 'заголовок', ''))

    print('файл:  %s' % os.path.basename(path))
    print('строк: %d' % len(rows))
    timed = [r for r in rows if r[0] is not None]
    if timed:
        print('период: %s — %s (%d записей со временем)'
              % (timed[0][3], timed[-1][3], len(timed)))
    print()

    # --- что попадалось -----------------------------------------------------
    counts = {}
    for _t, _text, cls, _s in rows:
        counts[cls] = counts.get(cls, 0) + 1
    print('что попадалось:')
    for cls, n in sorted(counts.items(), key=lambda kv: -kv[1]):
        print('    %-20s %d' % (cls, n))
    print()

    # --- паузы: кандидаты во фризы -----------------------------------------
    # Периодические строки (сводка раз в 5 с, строка аима) — не паузы, а ритм
    # лога: по ним замерять подвисание бессмысленно.
    periodic = {'сводка', 'аим (старый)', 'аим (новый)'}
    pauses = []
    for i in range(1, len(timed)):
        if timed[i][0] is None or timed[i - 1][0] is None:
            continue
        if timed[i][2] in periodic or timed[i - 1][2] in periodic:
            continue
        dt = timed[i][0] - timed[i - 1][0]
        if 0.5 <= dt < 3600:
            pauses.append((dt, timed[i - 1], timed[i]))
    if pauses:
        print('паузы между записями больше 0.5 с (кандидаты во фриз):')
        for dt, prev, cur in sorted(pauses, reverse=True)[:10]:
            print('    %.1f с  %s' % (dt, prev[3]))
            print('        было:  %s' % prev[1][-90:])
            print('        далее: %s' % cur[1][-90:])
        print()

    # --- сторож -------------------------------------------------------------
    stops = [(t, x) for t, x, c, _s in rows if c == 'СТОПОР']
    resumes = [(t, x) for t, x, c, _s in rows if c == 'кадры пошли снова']
    if stops or resumes:
        print('сторож:')
        for t, x in stops:
            m = STOP_RE.search(x)
            print('    СТОПОР кадр %s, %.1f с, стадия «%s»'
                  % (m.group(1), float(m.group(2)), m.group(3)) if m else '    ' + x)
        for t, x in resumes:
            m = RESUME_RE.search(x)
            print('    кадры пошли снова (кадр %s)' % m.group(1) if m else '    ' + x)
        print()

    # --- сайлент: ось -------------------------------------------------------
    own = game = shots = 0
    devs = []
    for _t, x, c, _s in rows:
        if c != 'сайлент: ось':
            continue
        m = AXIS_RE.search(x)
        if not m:
            continue
        if m.group(6) == 'НАШЕ':
            own += 1
        else:
            game += 1
        if m.group(9):
            shots += 1
        devs.append((abs(float(m.group(7))), abs(float(m.group(8)))))
    if own or game:
        print('сайлент, ось перед записью:')
        print('    наша запись дожила до следующего кадра (НАШЕ): %d' % own)
        print('    игру перебила игра (игры):                    %d' % game)
        print('    замеров с меткой «был выстрел»:                %d' % shots)
        if devs:
            print('    отклонение макс: рысканье %.1f, тангаж %.1f град'
                  % (max(d[0] for d in devs), max(d[1] for d in devs)))
        print()

    lens = [float(m.group(4)) for _t, x, c, _s in rows if (m := CHAIN_RE.search(x))]
    if lens:
        print('сайлент, цепь оси: замеров %d, длина от %.4f до %.4f'
              % (len(lens), min(lens), max(lens)))
        lost = sum(1 for _t, x, c, _s in rows if 'цепь оси потеряна' in x)
        if lost:
            print('    потеря цепи: %d раз' % lost)
        print()

    fails = [m.group(1) for _t, x, c, _s in rows if (m := WRITE_FAIL_RE.search(x))]
    if fails:
        print('сайлент, запись оси не прошла: %d раз, errno: %s'
              % (len(fails), ', '.join(sorted(set(fails)))))
        print()

    # --- режимы аима --------------------------------------------------------
    modes = [(t, x) for t, x, c, _s in rows if c == 'режим аима']
    if modes:
        print('переключения аима:')
        for t, x in modes:
            m = MODE_RE.search(x)
            print('    %s' % (('режим %s (%s)' % (m.group(1), m.group(2))) if m else x))
        print()

    # --- сводка старого лога -------------------------------------------------
    sums = [m.groups() for _t, x, c, _s in rows if (m := SUMMARY_RE.search(x))]
    if sums:
        last = tuple(int(v) for v in sums[-1])
        print('счётчики чтения (последняя сводка): чтений %d, отказов %d, '
              'переоткрытий %d' % last)
        print()

    print('последние строки:')
    for _t, x, _c, _s in rows[-12:]:
        print('    %s' % x[:150])


if __name__ == '__main__':
    main()
