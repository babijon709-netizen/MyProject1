#!/usr/bin/env bash
# Сборка под Android arm64-v8a через ndk-build.
#
# Артефакт: libs/arm64-v8a/benzware.sh (исполняемый файл, LOCAL_MODULE = xvcen,
# BUILD_EXECUTABLE) плюс совместимый алиас xvcen-arm64-v8a — имена закреплены в
# KERNEL.md, п. 9.
#
# Логи шагов GitHub Actions не всегда доступны (raw-логи отдаёт отдельный хост,
# а он может быть закрыт), поэтому ошибки ndk-build дополнительно выводятся
# аннотациями ::error:: — их видно в интерфейсе и через REST API.
set -euo pipefail

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

LOG="${ROOT}/ndk-build.log"
set +e
"${NDK}/ndk-build" \
    -C "${ROOT}" \
    NDK_PROJECT_PATH="${ROOT}" \
    NDK_APPLICATION_MK="${ROOT}/jni/Application.mk" \
    APP_BUILD_SCRIPT="${ROOT}/jni/Android.mk" \
    -j"$(nproc)" 2>&1 | tee "${LOG}"
status=${PIPESTATUS[0]}
set -e

if [[ "${status}" -ne 0 ]]; then
    echo "ndk-build failed with exit ${status}" >&2
    tail -n 120 "${LOG}" >&2 || true
    grep -E "error:|fatal error:|undefined reference|Error 1|No such file|unknown argument" "${LOG}" \
        | tail -n 40 | cut -c1-200 | while IFS= read -r line; do
        echo "::error::${line}"
    done
    exit "${status}"
fi

OUT="${ROOT}/libs/arm64-v8a"
test -f "${OUT}/xvcen"

mv -f "${OUT}/xvcen" "${OUT}/benzware.sh"
chmod +x "${OUT}/benzware.sh"
cp -f "${OUT}/benzware.sh" "${OUT}/xvcen-arm64-v8a"

ls -la "${OUT}/benzware.sh"
du -h "${OUT}/benzware.sh"
echo "built ${OUT}/benzware.sh"
