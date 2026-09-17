#!/bin/sh
# Стенд мемори-режимов аима.
#
# Собирает НАСТОЯЩИЙ код режимов из jni/src/aim.cpp — общий выбор цели,
# «память» и «сайлент» — вокруг заглушек игры (MouseLook, камера, ось
# выстрела) и прогоняет сценарии, которых на устройстве не увидеть глазами:
# потерянные записи, отсутствие оси камеры, отсутствие объекта, предел
# отклонения оси выстрела. NDK не нужен: обычная сборка g++.
#
#   sh tools/aim/run.sh
#   BUILD=/tmp/x sh tools/aim/run.sh
#
# Регион вырезается из aim.cpp по содержимому, а не по номерам строк: правки
# выше по файлу не должны ломать стенд.
set -e
cd "$(dirname "$0")/../.."
BUILD=${BUILD:-/tmp/aim_mem_test}
mkdir -p "$BUILD"

python3 - "$BUILD" <<'PY'
import sys
build = sys.argv[1]
lines = open('jni/src/aim.cpp', encoding='utf-8').read().split('\n')

def find(pred, what):
    for i, l in enumerate(lines):
        if pred(l):
            return i
    raise SystemExit('не найдено: %s' % what)

# A: структура цели.
a0 = find(lambda l: l == 'struct AimTarget {', 'struct AimTarget')
a1 = a0
while lines[a1] != '};':
    a1 += 1
# B: общее для всех режимов (выбор цели) — до тач-аима.
b0 = find(lambda l: l.startswith('// ============================ Общее для всех режимов'), 'общий блок')
b1 = find(lambda l: l.startswith('// ============================ Тач-аим'), 'тач-аим')
# C: мемори-режимы и диспетчер — до автофарма.
c0 = find(lambda l: l.startswith('// ====================== Мемори-аим: «памя'), 'мемори-аим')
c1 = find(lambda l: l.startswith('// ============================ Автофарм'), 'автофарм')

region = lines[a0:a1 + 1] + [''] + lines[b0:b1] + [''] + lines[c0:c1]
open(build + '/ctrl.inc', 'w', encoding='utf-8').write('\n'.join(region) + '\n')

src = open('jni/include/str.h', encoding='utf-8').read()
a = src.index('namespace xp {')
b = src.index('#define XS(s)')
c = src.index('\n', b) + 1
open(build + '/xp.inc', 'w', encoding='utf-8').write(src[a:c])
print('регион режимов aim.cpp: AimTarget + общий блок + мемори/сайлент (%d строк)' % len(region))
PY

g++ -std=gnu++17 -O1 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
    -I"$BUILD" -Ijni/include/ImGui -Ijni/include -Ijni/src \
    -o "$BUILD/mem_test" tools/aim/mem_test.cpp
echo "сборка стенда: ОК"

"$BUILD/mem_test"
