#!/bin/sh
# Хост-проверка синтаксиса ВСЕХ TU из jni/Android.mk — без NDK и без устройства
# (заглушки ставит scripts/host_stubs.sh в .stubs/).
#
# Зачем: NDK-сборка в CI идёт минуты и качает тулчейн, а эта проверка — секунды
# и ловит тот же класс ошибок (необъявленный идентификатор, разъехавшийся
# заголовок, ошибка в сигнатуре). Гонять перед пушем.
#
# Использование: sh scripts/host_syntax.sh          (компиляторы: CC/CXX)
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
cd "${DIR}/.."

sh "${DIR}/host_stubs.sh" >/dev/null

INCLUDES="-I.stubs -Ijni/include -Ijni/include/ImGui -Ijni/include/ImGui/backends -Ijni/src"
DEFINES="-DUSE_OPENGL -DIMGUI_IMPL_OPENGL_ES3 -DANDROID"

# Список TU берём из Android.mk, чтобы он не разъезжался с реальной сборкой.
SRC="$(sed -n '/LOCAL_SRC_FILES/,/^$/p' jni/Android.mk | grep -oE '[^ ]+\.(cpp|cc|c)')"
total=0
fail=0

for f in ${SRC}; do
    case "${f}" in
        *.c) cc="${CC:-gcc}";  std="-std=c11" ;;
        *)   cc="${CXX:-g++}"; std="-std=gnu++17" ;;
    esac
    total=$((total + 1))
    log="$(mktemp)"
    if ${cc} ${std} -fsyntax-only ${DEFINES} ${INCLUDES} "jni/${f}" >"${log}" 2>&1; then
        echo "ok   jni/${f}"
    else
        echo "FAIL jni/${f}"
        grep -E "error:" "${log}" | head -10
        fail=1
    fi
    rm -f "${log}"
done

if [ "${fail}" -ne 0 ]; then
    echo "syntax check: есть ошибки (${total} TU)" >&2
    exit 1
fi
echo "syntax check: чисто (${total} TU)"
