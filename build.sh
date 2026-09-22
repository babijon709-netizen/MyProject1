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

# Экспериментальный внутрипроцессный модуль подбора: отдельная библиотека, в
# xvcen она не попадает. Ошибка её сборки НЕ ломает обычную сборку — печатается
# предупреждение (в CI по логу видно, собрался ли модуль под текущий NDK).
_loot_log="$(mktemp)"
if sh "${ROOT}/tools/loot_module/build_android.sh" >"${_loot_log}" 2>&1; then
    echo "experimental loot module built (separate library, not part of xvcen)"
    echo "::notice title=experimental loot module::compiled under the current NDK (not part of xvcen)"
else
    echo "experimental loot module did not build; xvcen is unaffected" >&2
    echo "::warning title=experimental loot module::build failed; xvcen is unaffected"
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

if [[ -f "${ROOT}/libs/arm64-v8a/xvcen" && ! -f "${ROOT}/libs/arm64-v8a/xvcen.sh" ]]; then
    cp -f "${ROOT}/libs/arm64-v8a/xvcen" "${ROOT}/libs/arm64-v8a/xvcen.sh"
fi

test -f "${ROOT}/libs/arm64-v8a/xvcen.sh"

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
ls -la "${ROOT}/libs/arm64-v8a/xvcen.sh"
du -h "${ROOT}/libs/arm64-v8a/xvcen.sh"
echo "built ${ROOT}/libs/arm64-v8a/xvcen.sh"
