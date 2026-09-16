#!/bin/sh
# Стенд переключателя языка РУ/EN.
#
#   sh tools/lang/run.sh
#   BUILD=/tmp/lang sh tools/lang/run.sh     # куда складывать собранное
#
# Что делает:
#   1) check.py сверяет таблицы перевода с кодом: все строки из XS("...") и все
#      подписи визуалов из esp/*.cpp переведены, лишних пар нет, таблицы
#      отсортированы (по этому порядку идёт бинарный поиск), имена оружия — как
#      их пишет сама игра;
#   2) собирает НАСТОЯЩИЙ jni/src/lang.cpp (с таблицами) вместе с тестом
#      tools/lang/lang_test.cpp и прогоняет его: русский по умолчанию, перевод
#      меню и визуалов при переключении, возврат на русский, неизвестная строка
#      не пропадает, поиск находит ВСЕ пары таблиц.
#
# NDK не нужен: обычная сборка g++ на хосте, поэтому проверку можно гонять после
# каждой правки подписей, не дожидаясь устройства.
set -e
cd "$(dirname "$0")/../.."
BUILD=${BUILD:-/tmp/lang_test}
mkdir -p "$BUILD"

echo "--- таблицы перевода:"
python3 tools/lang/check.py

# Тесту нужны те же пары, что и в сборке, но с определением LangPair: сам
# lang.cpp держит его в анонимном namespace и наружу не отдаёт.
{
    echo '// Собрано tools/lang/run.sh из jni/src/lang_tables.inc — только для теста.'
    echo 'struct LangPair { const char* ru; const char* en; };'
    cat jni/src/lang_tables.inc
} > "$BUILD/lang_test_tables.inc"

g++ -std=gnu++17 -O1 -Wall -Wextra -Wno-unused-parameter \
    -I"$BUILD" -Ijni/include -Ijni/src \
    -o "$BUILD/lang_test" tools/lang/lang_test.cpp jni/src/lang.cpp
echo "--- стенд языка:"
"$BUILD/lang_test"
