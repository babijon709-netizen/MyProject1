#!/bin/sh
# Локальная проверка синтаксиса и семантики без NDK.
#
# Полноценную сборку делает CI (ndk-build, arm64-v8a), но ждать его на каждую
# правку дорого. Здесь те же перевод-юниты прогоняются через обычный g++
# -fsyntax-only: системные заголовки Android (native_window, EGL, GLES3,
# OpenSLES, system_properties) подменяются заглушками из stub/ — в них ровно те
# типы и функции, которыми пользуется код, поэтому ошибки в самом коде
# (необъявленное имя, неверный формат printf, несовпадение аргументов)
# находятся локально за секунды. Заглушки НЕ участвуют в ndk-build: они лежат
# отдельно и подключаются только ключом -I из этого скрипта.
#
# Использование: sh tools/syntax/check.sh [файл ...]
set -e
cd "$(dirname "$0")/../.."
STUB=tools/syntax/stub
INC="-I$STUB -Ijni/include -Ijni/include/ImGui -Ijni/include/ImGui/backends -Ijni/src"
FILES="${*:-jni/src/main.cpp jni/src/game.cpp jni/src/Android_draw/draw.cpp \
    jni/src/Android_touch/TouchHelperA.cpp jni/src/Blur/Blur.cpp jni/src/VidAvatar.cpp}"
rc=0
for f in $FILES; do
    CC=g++; STD=-std=c++17
    case "$f" in *.c) CC=gcc; STD=-std=c11 ;; esac
    if $CC $STD -fsyntax-only -Wall $INC "$f" 2> /tmp/syntax_err.txt; then
        echo "OK   $f"
    else
        # -Wcomment в game_offsets.h:346 — давний и известный, не ошибка сборки.
        if grep -q "error:" /tmp/syntax_err.txt; then
            echo "ОШИБКА $f"; grep -v "multi-line comment" /tmp/syntax_err.txt | head -30; rc=1
        else
            echo "OK   $f (только предупреждения)"
        fi
    fi
done
# C-источник декодера JPEG (собран своим компилятором в Android.mk)
if [ -f jni/src/third_party/tjpgd/tjpgd.c ]; then
    if gcc -std=c11 -fsyntax-only -I$STUB -Ijni/src/third_party/tjpgd jni/src/third_party/tjpgd/tjpgd.c 2> /tmp/syntax_err.txt; then
        echo "OK   jni/src/third_party/tjpgd/tjpgd.c"
    else
        echo "ОШИБКА jni/src/third_party/tjpgd/tjpgd.c"; head -20 /tmp/syntax_err.txt; rc=1
    fi
fi
exit $rc
