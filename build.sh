#!/usr/bin/env bash
# Сборка libs/arm64-v8a/xvcen (arm64-v8a) из jni/Android.mk.
#
# Хост-тесты (jni/tests, без NDK) запускаются отдельно — scripts/host_tests.sh.
# В CI это отдельный шаг: падение теста и падение компиляции должны читаться
# по-разному.
#
# NDK берётся из ANDROID_NDK_HOME / ANDROID_NDK_ROOT, иначе из
# ${ANDROID_SDK_ROOT}/ndk/${NDK_VERSION}.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
LOG="${ROOT}/ndk-build.log"
OUT="${ROOT}/libs/arm64-v8a/xvcen"

NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
if [[ -z "${NDK}" && -n "${ANDROID_SDK_ROOT:-}" && -n "${NDK_VERSION:-}" ]]; then
    NDK="${ANDROID_SDK_ROOT}/ndk/${NDK_VERSION}"
fi
if [[ ! -x "${NDK}/ndk-build" ]]; then
    echo "ndk-build не найден (ANDROID_NDK_HOME='${ANDROID_NDK_HOME:-}' ANDROID_NDK_ROOT='${ANDROID_NDK_ROOT:-}')" >&2
    exit 1
fi

echo "Using NDK at ${NDK}"
"${NDK}/ndk-build" --version || true

# Прошлый бинарь сносим: иначе «сборка прошла» может означать артефакт
# предыдущего прогона, а upload-artifact кладёт именно то, что лежит здесь.
if [[ -d "${ROOT}/libs/arm64-v8a" ]]; then
    rm -f "${ROOT}/libs/arm64-v8a/"*
fi

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
    echo "ndk-build завершился с кодом ${status}" >&2
    tail -n 80 "${LOG}" >&2 || true
    # Raw-лог run'а доступен не всегда (артефактный хост режется фильтром),
    # а аннотации отдаёт REST API — поэтому дублируем самое полезное туда.
    if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
        grep -E "error:|fatal error:|undefined reference|Error 1|No such file" "${LOG}" \
            | tail -n 40 | cut -c1-200 | while IFS= read -r line; do
            echo "::error::${line}"
        done
        if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
            {
                echo '## ndk-build failed'
                echo
                echo '```'
                tail -n 60 "${LOG}"
                echo '```'
            } >> "${GITHUB_STEP_SUMMARY}"
        fi
    fi
    exit "${status}"
fi

# BUILD_EXECUTABLE кладёт бинарь в libs/<abi>/<module>. Подстраховка — копия из
# obj/local на случай, если install-шаг не отработал (strip уже в LOCAL_LDFLAGS).
if [[ ! -f "${OUT}" && -f "${ROOT}/obj/local/arm64-v8a/xvcen" ]]; then
    cp -f "${ROOT}/obj/local/arm64-v8a/xvcen" "${OUT}"
fi

test -f "${OUT}"
ls -la "${OUT}"
du -h "${OUT}"
if command -v file >/dev/null 2>&1; then
    file "${OUT}"
fi
sha256sum "${OUT}" 2>/dev/null || true
echo "built ${OUT}"
