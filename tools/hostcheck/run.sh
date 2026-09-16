#!/bin/sh
# Проверка сборки хостовым g++ — без NDK и без Android SDK.
#
# Зачем: ошибки вида «переменная объявлена ниже места использования» ловятся
# NDK-сборкой в CI, но лог CI из песочницы не читается (артефакты и логи лежат
# в чужом blob-хранилище). Этот скрипт даёт ту же проверку за секунду:
# синтаксис и типы точки входа и всех модулей.
#
#   sh tools/hostcheck/run.sh
#
# Что подменяется (каталог stub/):
#   * GLES3/EGL/android-заголовки — минимальные объявления того, что реально
#     зовёт код (текстуры, окно, лог);
#   * sys/system_properties.h — как в Android NDK;
#   * аудио-блок (OpenSLES) в копии app/audio.cpp выключается: его API в
#     хостовом окружении не объявить без сотни строк-заглушек, а к меню и
#     фарму он отношения не имеет. Всё остальное компилируется ровно как в NDK.
#
# Скрипт ничего не меняет в репозитории: копия модуля пишется во временный файл.
#
# Было: одной командой компилировались монолиты game.cpp и main.cpp. Теперь они
# разрезаны на модули jni/src/esp/*.cpp и jni/src/{app,ui,aim,farm}/*.cpp, и
# проверяются все: так же быстро, но ошибка в любом модуле видна отдельной
# строкой.
set -e
cd "$(dirname "$0")/../.."
STUB=tools/hostcheck/stub
TMP=${TMPDIR:-/tmp}

python3 - "$TMP/audio_hostcheck.cpp" <<'PY'
import sys
src = open('jni/src/app/audio.cpp', encoding='utf-8').read()
old = '''#if __has_include("media/audio.h")
#  include "media/audio.h"
#  define AUDIO_AVAILABLE
#endif'''
assert old in src, 'не найден блок подключения аудио в app/audio.cpp'
src = src.replace(old, '// hostcheck: аудио-блок (OpenSLES) выключен', 1)
open(sys.argv[1], 'w', encoding='utf-8').write(src)
PY

INC="-Ijni/include -Ijni/include/ImGui -Ijni/include/ImGui/backends -Ijni/src -I$STUB"
FLAGS="-fsyntax-only -std=gnu++17 -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable"
# shellcheck disable=SC2086
g++ $FLAGS $INC "$TMP/audio_hostcheck.cpp"
echo "app/audio.cpp: ОК (аудио выключено)"
for f in jni/src/main.cpp jni/src/VidAvatar.cpp jni/src/lang.cpp \
         jni/src/esp/*.cpp jni/src/app/*.cpp jni/src/ui/*.cpp \
         jni/src/aim/*.cpp jni/src/farm/*.cpp; do
    case "$f" in *app/audio.cpp) continue ;; esac      # уже проверен выше
    # shellcheck disable=SC2086
    g++ $FLAGS $INC "$f" || exit 1
done
echo "main.cpp + jni/src/{esp,app,ui,aim,farm}/*.cpp: ОК"
echo "сборка хостовым g++ пройдена"
