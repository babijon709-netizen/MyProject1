#!/bin/sh
# Стенд мемори-режимов аима.
#
# Собирает НАСТОЯЩИЙ код режимов из jni/src/aim.cpp — общий выбор цели и
# «память» — вокруг заглушек игры (MouseLook, камера) и прогоняет сценарии,
# которых на устройстве не увидеть глазами: потерянные записи, отсутствие оси
# камеры, отсутствие объекта, дрожь на идущей цели. NDK не нужен: обычная
# сборка g++.
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
c0 = find(lambda l: l.startswith('// ---- диагностика режимов'), 'диагностика режимов')
c1 = find(lambda l: l.startswith('// ============================ Автофарм'), 'автофарм')

region = lines[a0:a1 + 1] + [''] + lines[b0:b1] + [''] + lines[c0:c1]
open(build + '/ctrl.inc', 'w', encoding='utf-8').write('\n'.join(region) + '\n')

# Тач-петля (tools/aim/touch_loop_test.cpp): НАСТОЯЩИЙ UpdateAimTouch с его
# окружением — дробь экрана пальца, коэффициент из чувствительности (БЕЗ
# MonoNow: часы у стенда свои, время в прогоне — симулированное), AimTarget,
# общий блок (выбор цели + поток камеры) и весь тач-режим.
f0 = find(lambda l: l.startswith('static constexpr float kAimTouchDefX'), 'kAimTouchDefX')
f1 = find(lambda l: l.startswith('float AimTouchFracY('), 'AimTouchFracY')
while lines[f1] != '}':
    f1 += 1
s0 = find(lambda l: l.startswith('static constexpr float kAimRefSensitivity'), 'kAimRefSensitivity')
m0 = find(lambda l: l.startswith('double MonoNow()'), 'MonoNow')
m1 = m0
while lines[m1] != '}':
    m1 += 1
s1 = find(lambda l: l.startswith('float AimSensitivityGain('), 'AimSensitivityGain')
while lines[s1] != '}':
    s1 += 1
t0 = find(lambda l: l.startswith('// ---- диагностика тач-аима'), 'диагностика тач-аима')
touch_region = (lines[f0:f1 + 1] + [''] + lines[s0:m0] + [''] + lines[m1 + 1:s1 + 1] + [''] +
                lines[a0:a1 + 1] + [''] + lines[b0:t0] + [''] + lines[t0:c0])
open(build + '/touch_ctrl.inc', 'w', encoding='utf-8').write('\n'.join(touch_region) + '\n')
print('регион тач-петли aim.cpp: %d строк' % len(touch_region))

src = open('jni/include/str.h', encoding='utf-8').read()
a = src.index('namespace xp {')
b = src.index('#define XS(s)')
c = src.index('\n', b) + 1
open(build + '/xp.inc', 'w', encoding='utf-8').write(src[a:c])
print('регион режимов aim.cpp: AimTarget + общий блок + мемори (%d строк)' % len(region))
PY

g++ -std=gnu++17 -O1 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
    -I"$BUILD" -Ijni/include/ImGui -Ijni/include -Ijni/src \
    -o "$BUILD/mem_test" tools/aim/mem_test.cpp
echo "сборка стенда: ОК"

g++ -std=gnu++17 -O1 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
    -I"$BUILD" -Ijni/include/ImGui -Ijni/include -Ijni/src \
    -o "$BUILD/touch_loop_test" tools/aim/touch_loop_test.cpp
echo "сборка стенда тач-петли: ОК"

"$BUILD/mem_test"
"$BUILD/touch_loop_test"
sh tools/team_membership_test.sh
sh tools/logfile_test.sh
