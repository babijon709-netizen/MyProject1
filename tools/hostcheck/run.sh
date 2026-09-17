#!/bin/sh
# Проверка сборки хостовым g++ — без NDK и без Android SDK.
#
# Зачем: ошибки вида «переменная объявлена ниже места использования» ловятся
# NDK-сборкой в CI, но лог CI из песочницы не читается (артефакты и логи лежат
# в чужом blob-хранилище). Этот скрипт даёт ту же проверку за секунду:
# синтаксис и типы всех модулей jni/src.
#
#   sh tools/hostcheck/run.sh
#
# Что подменяется (каталог stub/):
#   * GLES3/EGL/android-заголовки — минимальные объявления того, что реально
#     зовёт код (текстуры, окно, лог);
#   * sys/system_properties.h — как в Android NDK;
#   * аудио-блок (OpenSLES) в копии audio.cpp выключается: его API в хостовом
#     окружении не объявить без сотни строк-заглушек, а к меню и фарму он
#     отношения не имеет. Всё остальное компилируется ровно как в NDK.
#
# Скрипт ничего не меняет в репозитории: копия audio.cpp пишется во временный
# файл.
set -e
cd "$(dirname "$0")/../.."
STUB=tools/hostcheck/stub
TMP=${TMPDIR:-/tmp}

python3 - "$TMP/audio_hostcheck.cpp" <<'PY'
import sys
src = open('jni/src/audio.cpp', encoding='utf-8').read()
old = '''#if __has_include("media/audio.h")
#  include "media/audio.h"
#  define AUDIO_AVAILABLE
#endif'''
assert old in src, 'не найден блок подключения аудио в audio.cpp'
src = src.replace(old, '// hostcheck: аудио-блок (OpenSLES) выключен', 1)
open(sys.argv[1], 'w', encoding='utf-8').write(src)
PY

INC="-Ijni/include -Ijni/include/ImGui -Ijni/include/ImGui/backends -Ijni/src -I$STUB"
FLAGS="-fsyntax-only -std=gnu++17 -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable"
for f in jni/src/main.cpp \
         "$TMP/audio_hostcheck.cpp" \
         jni/src/theme.cpp jni/src/process.cpp jni/src/hud.cpp jni/src/esp_draw.cpp \
         jni/src/aim.cpp jni/src/farm.cpp jni/src/widgets.cpp jni/src/menu.cpp \
         jni/src/config.cpp \
         jni/src/VidAvatar.cpp jni/src/game.cpp jni/src/game_markers.cpp \
         jni/src/game_farm.cpp jni/src/lang.cpp; do
    # shellcheck disable=SC2086
    g++ $FLAGS $INC "$f" || exit 1
    echo "${f#jni/src/}: ОК"
done
echo "сборка хостовым g++ пройдена"
