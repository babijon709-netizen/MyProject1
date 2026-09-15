#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Разбор строк аима из лога автофарма — проверка «аим ведёт, а не дёргается».

Зачем. Симптом «аим дёргается» виден только в логе: рывок — это один-два кадра,
в которых палец уходит на десятки/сотни пикселей и камеру швыряет. Строка аима
пишется в тот же файл, что и автофарм (farm_debug.log), пять раз в секунду:

    AIM  t_s st err_yaw err_pitch sent_x sent_y exp exp cam cam f_x f_y cam_st

Как читать (подробности — в комментарии к farmlog::aimLine в main.cpp):
  * cam_st = 0 — настоящей оси камеры (поза Transform или ось выстрела) на
    устройстве нет, аим ведёт цель запасным коэффициентом вслепую. Это штатный
    режим: exp и cam при этом нули, потому что учить чувствительность не по чему;
  * sent — шаг пальца в пикселях. У здорового ведения он сравним с
    err / 0.35 (запасная чувствительность), то есть примерно 2.6·|err|;
  * exp = sent · gain — сколько градусов камеры должно было выйти из шага,
    cam — сколько вышло на самом деле. Их расхождение — ошибка чувствительности;
  * st 3 — палец ушёл в край зоны и его переносят в центр (EV «палец на краю»):
    нормальное событие при дальнем узле у фарма, но у аима это признак того, что
    шаги считаются неверно;
  * «рывок» здесь: |sent| ≥ kSentJump (по умолчанию 100 px) или смена знака
    выученного gain между соседними строками (знак означает инверсию оси: аим
    начинает швырять камеру в другую сторону).

    python3 tools/aim/log_lint.py <farm_debug.log> [kSentJump]

Код возврата 1 — рывки найдены (в выводе будут строки и разбор), 0 — чисто.
"""
import re
import sys

LINE = re.compile(
    r'^AIM\s+([\d.]+) st (\d+)\s+err\s+([-+][\d.]+)\s+([-+][\d.]+)'
    r'\s+sent\s+([-+][\d.]+)\s+([-+][\d.]+)'
    r'\s+exp\s+([-+][\d.]+)\s+([-+][\d.]+)'
    r'\s+cam\s+([-+][\d.]+)\s+([-+][\d.]+)'
    r'\s+f\s+([\d.]+)\s+([\d.]+)\s+cam_st (\d+)')


def parse(path):
    rows = []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = LINE.match(line.rstrip())
        if not m:
            continue
        t, st, ey, ep, sx, sy, xY, xP, cY, cP, fx, fy, cam_st = m.groups()
        rows.append(dict(t=float(t), st=int(st), err=(float(ey), float(ep)),
                         sent=(float(sx), float(sy)), exp=(float(xY), float(xP)),
                         cam=(float(cY), float(cP)), f=(float(fx), float(fy)),
                         cam_st=int(cam_st)))
    return rows


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    path = argv[1]
    jump = float(argv[2]) if len(argv) > 2 else 100.0
    rows = parse(path)
    if not rows:
        print('строк AIM в %s нет — аим ни разу не включался' % path)
        return 0

    axes = sorted({r['cam_st'] for r in rows})
    print('строк аима: %d, cam_st: %s' % (len(rows), axes))
    if axes == [0]:
        print('  настоящей оси камеры нет (поза и ось выстрела не читаются): '
              'exp и cam нули, учить чувствительность не по чему — аим идёт '
              'запасным коэффициентом. В сборках, где аиму отдавался ещё и базис '
              'из матрицы вида, ровно здесь и начинались рывки (gain со сменой '
              'знака); после разделения источников (esp_aim_camera_angles) их нет')
    print('состояния такта:', {st: sum(1 for r in rows if r['st'] == st)
                              for st in sorted({r['st'] for r in rows})})

    worst = max(rows, key=lambda r: max(abs(r['sent'][0]), abs(r['sent'][1])))
    print('самый большой шаг пальца: t %.2f, sent %+.1f %+.1f px при err %+.2f %+.2f'
          % (worst['t'], worst['sent'][0], worst['sent'][1],
             worst['err'][0], worst['err'][1]))

    problems = []
    for r in rows:
        for axis, name in ((0, 'yaw'), (1, 'pitch')):
            if abs(r['sent'][axis]) >= jump:
                problems.append('t %7.2f: шаг %s %+.1f px при err %+.2f (рывок)'
                                % (r['t'], name, r['sent'][axis], r['err'][axis]))

    # Смена знака выученного коэффициента: exp/sent меняет знак — ось
    # инвертируется, и следующий шаг уходит в обратную сторону.
    prev = None
    for r in rows:
        if abs(r['sent'][0]) < 1.0 or abs(r['sent'][1]) < 1.0:
            continue
        gain = r['exp'][0] / r['sent'][0]
        if abs(gain) > 1e-6:
            if prev is not None and (gain > 0) != (prev > 0):
                problems.append('t %7.2f: знак коэффициента сменился (%.3f -> %.3f) '
                                '— аим разворачивает камеру в другую сторону'
                                % (r['t'], prev, gain))
            prev = gain

    edges = sum(1 for r in rows if r['st'] == 3)
    if edges:
        print('пере-постановок пальца из края (st 3): %d' % edges)

    if problems:
        print('НАЙДЕНЫ РЫВКИ (%d):' % len(problems))
        for p in problems[:40]:
            print('  ' + p)
        if len(problems) > 40:
            print('  ... ещё %d' % (len(problems) - 40))
        return 1
    print('рывков нет: шаги пальца в пределах %g px, знак коэффициента не менялся'
          % jump)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
