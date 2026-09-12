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

# ---- Сборка своего драйвера bw_mem.ko (Linux 5.10.255, arm64) ---------------
# Модуль встраивается в бинарник массивом байт (jni/src/bw_ko_data.h):
# команда --loadmod без пути извлекает и загружает его на устройстве.
# Ядро 5.10.255 = версия устройства (SuiKernel); vermagic/CRC расходятся со
# стоком, поэтому загрузка идёт с MODULE_INIT_IGNORE_MODVERSIONS|IGNORE_VERMAGIC.
KVER="5.10.255"
KDIR="${ROOT}/linux-${KVER}"
KO_OUT="${ROOT}/kernel_driver/bw_mem.ko"
KO_HDR="${ROOT}/jni/src/bw_ko_data.h"

build_ko() {
    if ! command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
        sudo apt-get update -qq || true
        sudo apt-get install -y -qq gcc-aarch64-linux-gnu bison flex bc libelf-dev || return 1
    fi
    command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 || return 1

    if [[ ! -d "${KDIR}" ]]; then
        curl -fsSL --retry 3 -o "linux-${KVER}.tar.xz" \
             "https://cdn.kernel.org/pub/linux/kernel/v5.x/linux-${KVER}.tar.xz" \
        || curl -fsSL --retry 3 -o "linux-${KVER}.tar.xz" \
             "https://mirrors.edge.kernel.org/pub/linux/kernel/v5.x/linux-${KVER}.tar.xz" \
        || return 1
        tar -xf "linux-${KVER}.tar.xz" || return 1
    fi

    [[ -f "${KDIR}/.config" ]] || \
        make -C "${KDIR}" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig || return 1
    [[ -f "${KDIR}/scripts/module.lds" ]] || \
        make -C "${KDIR}" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)" modules_prepare || return 1
    touch "${KDIR}/Module.symvers"

    make -C "${KDIR}" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
         M="${ROOT}/kernel_driver" modules || return 1
    [[ -f "${KO_OUT}" ]]
}

if build_ko; then
    file "${KO_OUT}"
    python3 - "${KO_OUT}" "${KO_HDR}" <<'PYEOF'
import sys
data = open(sys.argv[1], 'rb').read()
with open(sys.argv[2], 'w') as f:
    f.write("// Сгенерировано build.sh из kernel_driver/bw_mem.ko — НЕ редактировать.\n")
    f.write("// Встроенный драйвер benzware; извлекается командой --loadmod.\n")
    f.write("static const unsigned char bw_ko_data[] = {\n")
    for i in range(0, len(data), 20):
        f.write("  " + ", ".join(str(b) for b in data[i:i+20]) + ",\n")
    f.write("};\n")
    f.write("static const unsigned long bw_ko_size = sizeof(bw_ko_data); // %d байт\n" % len(data))
print("embedded bw_mem.ko: %d bytes" % len(data))
PYEOF
else
    echo "::warning::bw_mem.ko не собрался — бинарник будет без встроенного драйвера"
    cat > "${KO_HDR}" <<'EOF'
// bw_mem.ko не собрался (см. warning в логе CI) — встроенного драйвера нет.
static const unsigned char bw_ko_data[] = { 0 };
static const unsigned long bw_ko_size = 0;
EOF
fi

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
