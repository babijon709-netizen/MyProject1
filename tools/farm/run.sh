#!/bin/sh
# Стенд контроллера и памяти автофарма.
# Проверяет:
# 1. Применение и сброс модификаций в памяти без патча кода (alwaysCrit, durability, reach, anim suppression, LookDirection)
# 2. Совместимость с Release и Beta версиями оффсетов
# 3. Полную сборку всех компонентов
set -e
cd "$(dirname "$0")/../.."
BUILD=${BUILD:-/tmp/farm_test}
mkdir -p "$BUILD"

echo "=== Сборка и прогон unit-тестов farm_memory ==="
g++ -std=c++17 -O2 -Wall -Wextra \
    -Ijni/include -Ijni/src \
    -o "$BUILD/farm_memory_test" tools/farm/farm_memory_test.cpp

"$BUILD/farm_memory_test"
echo "farm_memory: все проверки пройдены"
