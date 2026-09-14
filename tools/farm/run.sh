#!/bin/sh
# Стенд контроллера автофарма.
#
# Собирает НАСТОЯЩИЙ код из jni/src/main.cpp — блок констант автофарма,
# namespace farmlog и UpdateFarm/UpdateFarmInner — вокруг заглушек окружения
# (ImGui, тач, игра, настройки) и прогоняет сценарии, в которых задеты все
# места событий лога. NDK не нужен: это обычная сборка g++, поэтому гонять
# можно после каждой правки бота, не дожидаясь CI.
#
#   sh tools/farm/run.sh [кадров]      # по умолчанию 4200 кадров (~70 с игры)
#   BUILD=/tmp/x sh tools/farm/run.sh  # куда складывать собранное и лог
#
# Что проверяет:
#   * компиляцию контроллера с -Wall -Wextra -Wformat=2 — вызовы
#     farmlog::event() помечены format(printf,1,2), поэтому число и типы
#     аргументов в каждом месте события проверяет компилятор;
#   * что лог пишется, колонки ровные, события печатаются человеческим текстом;
#   * что на кадрах без цели (ex 3/4) нет выдуманных переходов «экстеншен не
#     найден» / «точка прицела: корпус»;
#   * что обход перекрытого узла доходит до отказа: #1 -> #2 -> #3 -> #4 -> blacklist.
set -e
cd "$(dirname "$0")/../.."
BUILD=${BUILD:-/tmp/farm_ctrl_test}
FRAMES=${1:-4200}
mkdir -p "$BUILD/cfg"
rm -f "$BUILD/cfg"/*.log

# Регион контроллера вырезается из main.cpp по содержимому, а не по номерам
# строк: правки выше по файлу не должны ломать стенд.
python3 - "$BUILD" <<'PY'
import sys
build = sys.argv[1]
lines = open('jni/src/main.cpp', encoding='utf-8').read().split('\n')

start = None
for i, l in enumerate(lines):
    if l == 'namespace {' and any('kStuckTime' in x for x in lines[i:i + 80]):
        start = i
        break
assert start is not None, 'не найден блок констант автофарма'

inner = lines.index('static void UpdateFarmInner(float dt) {')
depth, end = 0, None
for i in range(inner, len(lines)):
    depth += lines[i].count('{') - lines[i].count('}')
    if depth == 0 and i > inner:
        end = i
        break
assert end is not None, 'не найден конец UpdateFarmInner'
open(build + '/ctrl.inc', 'w', encoding='utf-8').write('\n'.join(lines[start:end + 1]) + '\n')

src = open('jni/src/main.cpp', encoding='utf-8').read()
a = src.index('namespace xp {')
b = src.index('#define XS(s)')
c = src.index('\n', b) + 1
open(build + '/xp.inc', 'w', encoding='utf-8').write(src[a:c])
print('регион контроллера: строки %d..%d main.cpp' % (start + 1, end + 1))
PY

cp jni/include/game.h "$BUILD/game.h"
g++ -std=gnu++17 -O1 -Wall -Wextra -Wformat=2 -Wno-format-nonliteral \
    -Wno-unused-parameter -I"$BUILD" -Ijni/include \
    -o "$BUILD/ctrl_test" tools/farm/ctrl_test.cpp
echo "сборка стенда: ОК"

# Стенд пишет лог в каталог из FARMLOG_DIR (заглушка каталога конфигов).
FARMLOG_DIR="$BUILD/cfg/" "$BUILD/ctrl_test" "$FRAMES"
LOG="$BUILD/cfg/farm_debug.log"
echo "--- лог: $LOG ($(wc -l < "$LOG") строк, $(wc -c < "$LOG") байт)"
echo "--- типы событий:"
grep '^EV' "$LOG" | sed 's/^EV *[0-9.]* //' | sed 's/[0-9][0-9.]*/N/g' | sort | uniq -c | sort -rn
echo "--- проверки:"
fail=0
grep -q "перекрыт: 4 обходов не помогли" "$LOG" || { echo "НЕТ отказа от перекрытого узла"; fail=1; }
# Номера обходов перекрытия должны расти: #1 -> #2 -> #3 -> #4 -> отказ.
# (После отказа стенд показывает тот же узел снова — blacklist в заглушке
# пустой, — поэтому цикл может начаться заново; важна отсутствие повторов
# подряд: именно так выглядел баг с обнулением счётчика в фазе 3.)
grep 'перекрыт: луч игры' "$LOG" | sed 's/.*обход #\([0-9]*\).*/\1/' > "$BUILD/evseq"
awk 'NR>1 && $1==prev {bad=1} {prev=$1} END{exit bad}' "$BUILD/evseq" \
    || { echo "обход перекрытия повторяет один и тот же номер — счётчик обнуляется"; fail=1; }
grep -q 'экстеншен крестика на узле не найден' "$LOG" && { echo "ложное «экстеншен не найден»"; fail=1; }
head -1 "$LOG" | grep -q '^# зоны бота:' || { echo "в шапке нет зон бота"; fail=1; }
[ "$fail" = 0 ] && echo "все проверки пройдены"
exit $fail
