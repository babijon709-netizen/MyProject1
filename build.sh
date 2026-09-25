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

# Public endpoint only: never embed the administrative token in the binary.
# Refuse to publish an accidentally unconfigured (non-starting) artifact.
if [[ -z "${BENZWARE_STATUS_URL:-}" ]]; then
    echo "BENZWARE_STATUS_URL is missing: set the repository Actions variable to your public https://.../status Worker URL" >&2
    echo "::error title=Missing BENZWARE_STATUS_URL::Set Settings > Secrets and variables > Actions > Variables > BENZWARE_STATUS_URL to your public HTTPS Worker URL ending in /status" >&2
    exit 1
fi
python3 - "${BENZWARE_STATUS_URL}" "${ROOT}/jni/include/benzware_remote_local.h" <<'PY_CONFIG'
import json, sys
from urllib.parse import urlsplit
url, output = sys.argv[1:]
parts = urlsplit(url)
if (parts.scheme != 'https' or not parts.hostname or parts.username or parts.password
        or parts.path != '/status' or parts.query or parts.fragment
        or any(c.isspace() or ord(c) < 32 for c in url)):
    raise SystemExit('BENZWARE_STATUS_URL must be a plain https://host/status URL')
with open(output, 'w') as f:
    f.write('#pragma once\n#define BENZWARE_STATUS_URL ' + json.dumps(url) + '\n')
PY_CONFIG

echo "Testing viewmodel memory controller"
_run_suite() {
    # Вывод тестов пишется в лог; при падении хвост выносится в GitHub-аннотации
    # (::error::) — raw-логи не всегда доступны локально, а аннотации отдаются
    # REST API. Стилистика — как у диагностики ndk-build ниже.
    local label="$1" script="$2" log status=0
    log="$(mktemp)"
    sh "${script}" >"${log}" 2>&1 || status=$?
    if [[ "${status}" -eq 0 ]]; then
        echo "${label}: OK"
        tail -n 2 "${log}" || true
        rm -f "${log}"
        return 0
    fi
    echo "${label}: FAILED (exit ${status})" >&2
    tail -n 40 "${log}" >&2 || true
    grep -E "Assertion|error:|fatal error:|terminate|what\(\)|Aborted|core dumped|Segmentation|internal compiler error|Stack dump" \
        "${log}" | tail -n 15 | cut -c1-180 | while IFS= read -r _line; do
        echo "::error title=${label}::${_line}"
    done
    rm -f "${log}"
    return "${status}"
}
_run_suite "viewmodel tests" "${ROOT}/tools/viewmodel/run.sh"
echo "Testing object-data weapon controller"
_run_suite "weapon tests" "${ROOT}/tools/weapons/run.sh"
echo "Testing silent aim adapter"
_run_suite "silent aim tests" "${ROOT}/tools/silent/run.sh"

# Экспериментальный внутрипроцессный модуль подбора: отдельная библиотека, в
# benzware она не попадает. Ошибка её сборки НЕ ломает обычную сборку — печатается
# предупреждение (в CI по логу видно, собрался ли модуль под текущий NDK).
_loot_log="$(mktemp)"
if sh "${ROOT}/tools/loot_module/build_android.sh" >"${_loot_log}" 2>&1; then
    echo "experimental loot module built (separate library, not part of benzware)"
    echo "::notice title=experimental loot module::compiled under the current NDK (not part of benzware)"
else
    echo "experimental loot module did not build; benzware is unaffected" >&2
    echo "::warning title=experimental loot module::build failed; benzware is unaffected"
    grep -E "error:|fatal error:|undefined reference" "${_loot_log}" | head -8 | cut -c1-180 | while IFS= read -r _line; do
        echo "::warning title=experimental loot module::${_line}"
    done
fi
rm -f "${_loot_log}"

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

# Keep the native executable named benzware. Bundle it with the standalone
# memory dumper in a single self-extracting distribution script.
if [[ ! -f "${ROOT}/libs/arm64-v8a/benzware" || ! -f "${ROOT}/libs/arm64-v8a/benzhack_dump" ]]; then
    echo "Missing benzware or benzhack_dump output" >&2
    exit 1
fi
_payload="$(mktemp)"
trap 'rm -f "${_payload}"' EXIT
tar -czf "${_payload}" -C "${ROOT}/libs/arm64-v8a" benzware benzhack_dump
_benz_hash="$(sha256sum "${ROOT}/libs/arm64-v8a/benzware" | cut -d ' ' -f 1)"
_dump_hash="$(sha256sum "${ROOT}/libs/arm64-v8a/benzhack_dump" | cut -d ' ' -f 1)"
cat > "${ROOT}/libs/arm64-v8a/benzware.sh" << 'SH_EOF'
#!/bin/sh
# ./benzware.sh [args]      -> benzware
# ./benzware.sh dump [args] -> standalone memory dumper
set -e
umask 077
DIR=$(mktemp -d "${TMPDIR:-/data/local/tmp}/benzware.XXXXXXXX")
trap 'rm -rf "$DIR"' 0
PAYLOAD_LINE=$(awk '/^__PAYLOAD_BELOW__/ {print NR + 1; exit 0; }' "$0")
tail -n +"$PAYLOAD_LINE" "$0" | tar -xzf - -C "$DIR"
chmod 755 "$DIR/benzware" "$DIR/benzhack_dump"
# Detect an accidentally altered or simply repacked payload before execution.
# This checksum is NOT a signature: someone who edits this launcher can update it.
hash_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1"
    elif [ -x /system/bin/toybox ]; then
        /system/bin/toybox sha256sum "$1"
    else
        echo "benzware: sha256sum/toybox is required to verify the payload" >&2
        return 1
    fi
}
EXPECTED_BENZWARE=__BENZWARE_SHA256__
EXPECTED_DUMPER=__DUMPER_SHA256__
ACTUAL_BENZWARE=$(hash_file "$DIR/benzware" | cut -d ' ' -f 1)
ACTUAL_DUMPER=$(hash_file "$DIR/benzhack_dump" | cut -d ' ' -f 1)
if [ "$EXPECTED_BENZWARE" != "$ACTUAL_BENZWARE" ] || [ "$EXPECTED_DUMPER" != "$ACTUAL_DUMPER" ]; then
    echo "benzware: bundled executable checksum mismatch" >&2
    exit 1
fi
if [ "$1" = "dump" ]; then
    shift
    "$DIR/benzhack_dump" "$@"
else
    "$DIR/benzware" "$@"
fi
exit $?
__PAYLOAD_BELOW__
SH_EOF
sed -i "s/__BENZWARE_SHA256__/${_benz_hash}/; s/__DUMPER_SHA256__/${_dump_hash}/" "${ROOT}/libs/arm64-v8a/benzware.sh"
cat "${_payload}" >> "${ROOT}/libs/arm64-v8a/benzware.sh"
chmod +x "${ROOT}/libs/arm64-v8a/benzware.sh"
rm -f "${_payload}"
trap - EXIT

# Переводы РУ/EN: строка меню без перевода в английском меню просто остаётся
# русской — на устройстве это видно, а в сборке нет. Проверка сверяет таблицы с
# кодом и роняет сборку с подсказкой, что запустить.
if command -v python3 >/dev/null 2>&1; then
    python3 "${ROOT}/tools/lang/check.py"
else
    echo "python3 не найден — проверка переводов пропущена" >&2
fi
# Вес бинарника — виден прямо в логе сборки (в нём кадры аватарки ~300 КБ,
# см. tools/video/gen_avatar.py; ролик целиком в бинарник не вшивается).
ls -la "${ROOT}/libs/arm64-v8a/benzware.sh"
du -h "${ROOT}/libs/arm64-v8a/benzware.sh"
echo "built ${ROOT}/libs/arm64-v8a/benzware.sh"

if [[ -f "${ROOT}/libs/arm64-v8a/benzhack_dump" ]]; then
    ls -la "${ROOT}/libs/arm64-v8a/benzhack_dump"
    echo "built ${ROOT}/libs/arm64-v8a/benzhack_dump"
fi
