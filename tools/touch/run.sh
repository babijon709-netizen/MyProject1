#!/bin/sh
# Стенд тач-слоя: что именно уходит в uinput при инъекции синтетических касаний.
#
# Зачем: автофарм-правки добавили в Upload() пропуск пакетов-повторов (ради
# отсутствия лага оверлея), и этот пропуск заодно заглушил удержание пальца
# аимботом — «верхний аим» перестал держать прицел на устройстве. Здесь тот же
# код гоняется на хосте: вместо /dev/uinput пакеты пишутся в pipe, читаются и
# разбираются по событиям, поэтому видно, сколько пакетов реально ушло в простой
# автофарма и сколько — в удержание аимбота.
#
#   sh tools/touch/run.sh
#
# Собирается НАСТОЯЩИЙ jni/src/Android_touch/TouchHelperA.cpp (подключается как
# включаемый файл), заглушка только одна — ImGui::GetIO(): она нужна link'у, но
# в тесте не используется. NDK не нужен.
set -e
cd "$(dirname "$0")/../.."
BUILD=${BUILD:-/tmp/touch_test}
mkdir -p "$BUILD"

# -Wno-*: предупреждения самого тач-слоя (пустые поля input_event, «может быть
# не инициализировано» в разборе /dev/input) к стенду отношения не имеют и
# забивают его вывод; ошибки компиляции при этом видны как обычно.
g++ -std=gnu++17 -O1 -g -Wall -Wextra -Wno-unused-parameter \
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-maybe-uninitialized \
    -pthread \
    -Ijni/include -Ijni/include/ImGui -Ijni/src \
    -o "$BUILD/touch_repeat_test" tools/touch/touch_repeat_test.cpp
"$BUILD/touch_repeat_test"
