#!/usr/bin/env bash
#
# Сборка нативного исполняемого файла xvcen.sh (НЕ APK).
#
# Требуется Android NDK. Путь задаётся одним из способов:
#   ./build.sh                      # если установлен NDK= / ANDROID_NDK_HOME / ANDROID_NDK_ROOT
#   NDK=/path/to/android-ndk ./build.sh
#
# Результат: libs/arm64-v8a/xvcen.sh
#
set -euo pipefail
cd "$(dirname "$0")"

# --- ищем NDK ---
NDK="${NDK:-${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}}"
if [ -z "$NDK" ]; then
    echo "Ошибка: не найден Android NDK." >&2
    echo "Укажи путь: NDK=/путь/к/android-ndk ./build.sh" >&2
    echo "Скачать можно тут: https://developer.android.com/ndk/downloads" >&2
    exit 1
fi

# ndk-build бывает в корне NDK или в подпапке build/
if [ -x "$NDK/ndk-build" ]; then
    NDK_BUILD="$NDK/ndk-build"
elif [ -x "$NDK/build/ndk-build" ]; then
    NDK_BUILD="$NDK/build/ndk-build"
else
    echo "Ошибка: ndk-build не найден в $NDK" >&2
    exit 1
fi

export ANDROID_NDK_HOME="$NDK"
export ANDROID_NDK_ROOT="$NDK"

echo ">> NDK:      $NDK"
echo ">> Module:   xvcen.sh (executable, arm64-v8a, android-21)"
echo
"$NDK_BUILD" "$@"

echo
echo "Готово. Бинарь: libs/arm64-v8a/xvcen.sh"
