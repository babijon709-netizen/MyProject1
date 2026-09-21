#!/bin/sh
# Explicit opt-in, never builds/injects through the ordinary application build.
set -eu
cd "$(dirname "$0")/../.."
ROOT=$(pwd)
NDK=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
if [ -z "$NDK" ] || [ ! -x "$NDK/ndk-build" ]; then
    echo 'Android NDK not available; no Android module produced.' >&2
    exit 1
fi
sh tools/loot_module/run.sh
"$NDK/ndk-build" NDK_PROJECT_PATH=null \
    APP_BUILD_SCRIPT="$ROOT/experimental/loot_module/Android.mk" \
    NDK_APPLICATION_MK="$ROOT/experimental/loot_module/Application.mk" \
    NDK_OUT="$ROOT/.tools/loot_module/obj" \
    NDK_LIBS_OUT="$ROOT/.tools/loot_module/libs" "$@"
echo 'Built experimental module ONLY. No injection or game execution performed.'
