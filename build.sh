#!/usr/bin/env bash
set -euo pipefail

# Сборка через ndk-build. NDK ставит CI (см. .github/workflows/build.yml);
# локально — экспортировать ANDROID_NDK_HOME/ANDROID_NDK_ROOT.
ROOT="$(cd "$(dirname "$0")" && pwd)"
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"

if [[ -z "${NDK}" ]]; then
    echo "ANDROID_NDK_HOME / ANDROID_NDK_ROOT is not set" >&2
    exit 1
fi
if [[ ! -x "${NDK}/ndk-build" ]]; then
    echo "ndk-build not found at ${NDK}/ndk-build" >&2
    ls -la "${NDK}" >&2 || true
    exit 1
fi

echo "Using NDK at ${NDK}"
"${NDK}/ndk-build" --version || true

set +e
"${NDK}/ndk-build" \
    -C "${ROOT}" \
    NDK_PROJECT_PATH="${ROOT}" \
    NDK_APPLICATION_MK="${ROOT}/jni/Application.mk" \
    APP_BUILD_SCRIPT="${ROOT}/jni/Android.mk" \
    -j"$(nproc)" 2>&1 | tee "${ROOT}/ndk-build.log"
status=${PIPESTATUS[0]}
set -e

if [[ "${status}" -ne 0 ]]; then
    echo "ndk-build failed with exit ${status}" >&2
    tail -n 120 "${ROOT}/ndk-build.log" >&2 || true
    # Хвост ошибок — в аннотации GitHub, чтобы причину было видно без скачивания лога.
    grep -E "error:|fatal error:|undefined reference|Error 1|No such file|unknown argument" \
        "${ROOT}/ndk-build.log" | tail -n 40 | while IFS= read -r line; do
        echo "::error::${line}"
    done
    exit "${status}"
fi

# Модуль в jni/Android.mk зовётся xvcen, артефакт в CI — benzware.sh.
cp -f "${ROOT}/libs/arm64-v8a/xvcen" "${ROOT}/libs/arm64-v8a/benzware.sh"
chmod +x "${ROOT}/libs/arm64-v8a/benzware.sh"
ls -la "${ROOT}/libs/arm64-v8a/benzware.sh"
echo "built ${ROOT}/libs/arm64-v8a/benzware.sh"
