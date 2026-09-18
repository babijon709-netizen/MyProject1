#!/bin/sh
# Стенд переключателя версий игры (релиз/бета) — на хосте, без NDK и устройства.
#
# Зачем: от выбора версии зависят все оффсеты сразу, и ошибка здесь не видна
# глазом — она выглядит как «чит ничего не читает» на устройстве. Стенд берёт
# НАСТОЯЩИЙ jni/src/game_offsets_active.h (вместе с релизным заголовком, файлом
# беты и сгенерированной таблицей), прогоняет выбор версии туда-сюда и сверяет
# каждое поле.
#
#   sh tools/offsets/run.sh
set -e
cd "$(dirname "$0")/../.."
BUILD=${BUILD:-/tmp/offsets_switch}
mkdir -p "$BUILD"

g++ -std=gnu++17 -O1 -g -Wall -Wextra \
    -Ijni/src \
    -o "$BUILD/switch_test" tools/offsets/switch_test.cpp

"$BUILD/switch_test"
