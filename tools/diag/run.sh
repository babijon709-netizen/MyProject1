#!/bin/sh
# Стенд журнала здоровья (app/diag_log.cpp).
#
#   sh tools/diag/run.sh
#   BUILD=/tmp/diag sh tools/diag/run.sh
#
# Что проверяет и почему именно это. Журнал пишется на внешнее хранилище, где
# write() может встать на секунды; если писать оттуда, откуда журнал позвали,
# встанет поток кадра — а это и есть жалоба «чит в один момент полностью завис»,
# после которой в файле не остаётся ни одной новой строки. Поэтому проверки
# такие:
#   1) файл открывается по пути <каталог>/xvcen_health.log, строки — формата
#      «секунды тег текст»;
#   2) все события доходят до файла (diag_flush ждёт хвост);
#   3) повтор того же события схлопывается в одну строку со счётчиком;
#   4) поток, который зовёт diag_log, не упирается в диск: 20000 вызовов подряд
#      укладываются в отведённое время, а если буфер переполнился — в файле
#      появляется строка «потеряно событий», то есть потеря не молчаливая.
#
# NDK не нужен: собираем diag_log.cpp обычным g++ с заглушками Android.
set -e
cd "$(dirname "$0")/../.."
BUILD=${BUILD:-/tmp/diag_test}
mkdir -p "$BUILD"

g++ -std=gnu++17 -O1 -Wall -Wextra -Wno-unused-parameter \
    -Itools/syntax/stub -Ijni/include -Ijni/include/ImGui -Ijni/src \
    -o "$BUILD/diag_test" tools/diag/diag_queue_test.cpp jni/src/app/diag_log.cpp
echo "--- стенд журнала:"
rm -rf "$BUILD/log"; mkdir -p "$BUILD/log"
"$BUILD/diag_test" "$BUILD/log"
