#!/bin/sh
# Проверка сборки хостовым g++ — без NDK и без Android SDK.
#
# Зачем: ошибки вида «переменная объявлена ниже места использования» ловятся
# NDK-сборкой в CI, но лог CI из песочницы не читается (артефакты и логи лежат
# в чужом blob-хранилище). Этот скрипт даёт ту же проверку за секунду:
# синтаксис и типы main.cpp, game.cpp и VidAvatar.cpp.
#
#   sh tools/hostcheck/run.sh
#
# Что подменяется (каталог stub/):
#   * GLES3/EGL/android-заголовки — минимальные объявления того, что реально
#     зовёт код (текстуры, окно, лог);
#   * sys/system_properties.h — как в Android NDK;
#   * аудио-блок (OpenSLES) в копии main.cpp выключается: его API в хостовом
#     окружении не объявить без сотни строк-заглушек, а к меню и фарму он
#     отношения не имеет. Всё остальное компилируется ровно как в NDK.
#
# Скрипт ничего не меняет в репозитории: копия main.cpp пишется во временный файл.
#
# Было: одной командой компилировался монолит game.cpp. Теперь он разрезан на
# модули jni/src/esp/*.cpp, и проверяются все: так же быстро, но ошибка в любом
# модуле видна отдельной строкой.
set -e
cd "$(dirname "$0")/../.."
STUB=tools/hostcheck/stub
TMP=${TMPDIR:-/tmp}

python3 - "$TMP/main_hostcheck.cpp" <<'PY'
import sys
src = open('jni/src/main.cpp', encoding='utf-8').read()
old = '''#if __has_include("media/audio.h")
#  include "media/audio.h"
#  define AUDIO_AVAILABLE
#endif'''
assert old in src, 'не найден блок подключения аудио в main.cpp'
src = src.replace(old, '// hostcheck: аудио-блок (OpenSLES) выключен', 1)
open(sys.argv[1], 'w', encoding='utf-8').write(src)
PY

INC="-Ijni/include -Ijni/include/ImGui -Ijni/include/ImGui/backends -Ijni/src -I$STUB"
FLAGS="-fsyntax-only -std=gnu++17 -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable"
# shellcheck disable=SC2086
g++ $FLAGS $INC "$TMP/main_hostcheck.cpp"
echo "main.cpp: ОК"
# shellcheck disable=SC2086
g++ $FLAGS $INC jni/src/VidAvatar.cpp
echo "VidAvatar.cpp: ОК"
# shellcheck disable=SC2086
for f in jni/src/esp/*.cpp; do
    g++ $FLAGS $INC "$f" || exit 1
done
echo "jni/src/esp/*.cpp: ОК"
# shellcheck disable=SC2086
g++ $FLAGS $INC jni/src/lang.cpp
echo "lang.cpp: ОК"
echo "сборка хостовым g++ пройдена"
