#!/usr/bin/env bash
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

export NDK_PROJECT_PATH="${ROOT}"
export NDK_APPLICATION_MK="${ROOT}/jni/Application.mk"

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
    if [[ -f "${ROOT}/ndk-build.log" ]]; then
        grep -E "error:|fatal error:|undefined reference|Error 1|No such file|unknown argument" "${ROOT}/ndk-build.log" | tail -n 40 | while IFS= read -r line; do
            echo "::error::${line}"
        done
    fi
    exit "${status}"
fi

if [[ -f "${ROOT}/libs/arm64-v8a/xvcen" && ! -f "${ROOT}/libs/arm64-v8a/xvcen.sh" ]]; then
    cp -f "${ROOT}/libs/arm64-v8a/xvcen" "${ROOT}/libs/arm64-v8a/xvcen.sh"
fi

# Распаковываем скрипты FT-драйвера рядом с бинарем: софт в KERNEL-режиме
# ищет их в <папка бинаря>/drivers/ft/ (на телефоне это /data/local/tmp/drivers/ft).
if [[ -f "${ROOT}/FTDriver.zip" ]]; then
    DRV_DIR="${ROOT}/libs/arm64-v8a/drivers/ft"
    mkdir -p "${DRV_DIR}"
    if command -v unzip >/dev/null 2>&1; then
        unzip -o -j -q "${ROOT}/FTDriver.zip" -d "${DRV_DIR}"
    else
        python3 - "${ROOT}/FTDriver.zip" "${DRV_DIR}" <<'PYEOF'
import os, sys, zipfile
zf = zipfile.ZipFile(sys.argv[1])
for name in zf.namelist():
    if name.endswith('/'):
        continue
    with open(os.path.join(sys.argv[2], os.path.basename(name)), 'wb') as f:
        f.write(zf.read(name))
PYEOF
    fi
    chmod 755 "${DRV_DIR}"/*.sh 2>/dev/null || true
    echo "FT driver scripts: $(ls -1 "${DRV_DIR}" | wc -l) files -> ${DRV_DIR}"
fi

test -f "${ROOT}/libs/arm64-v8a/xvcen.sh"
echo "built ${ROOT}/libs/arm64-v8a/xvcen.sh"
