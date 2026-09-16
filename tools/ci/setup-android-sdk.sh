#!/usr/bin/env bash
# Запасная установка Android cmdline-tools для CI.
#
# Зачем. Шаг `uses: android-actions/setup-android@v3` — сторонний Node-экшен,
# который сам качает cmdline-tools с dl.google.com. 14.09.2026 он трижды подряд
# (прогоны 34893109632, 34893442824, 34894007947) упал за ~20 с ещё ДО
# компиляции, поэтому правки остались непроверенными и без бинарника: NDK
# компилирует точку входа и все модули, включая меню, которое локально не собрать
# (нет android/*.h, EGL, GLESv3). Починить это из агента нельзя:
#   * rerun-failed-jobs — 403 «Resource not accessible by integration»;
#   * push в .github/workflows — «refusing to allow a GitHub App to create or
#     update workflow ... without `workflows` permission».
# Скрипт от экшена не зависит и идемпотентен: если sdkmanager уже есть, он
# ничего не делает, то есть штатный путь сборки не меняется.
#
# Подключение (нужны права на .github/workflows/build.yml — одна новая строка):
#
#   - name: Set up Android SDK
#     uses: android-actions/setup-android@v3
#     continue-on-error: true          # <-- падение экшена больше не рвёт сборку
#
#   - name: Install Android NDK
#     shell: bash
#     run: |
#       source tools/ci/setup-android-sdk.sh   # <-- вот эта строка
#       yes | sdkmanager --licenses >/dev/null || true
#       sdkmanager "ndk;${NDK_VERSION}"
#       echo "ANDROID_NDK_HOME=${ANDROID_SDK_ROOT}/ndk/${NDK_VERSION}" >> "${GITHUB_ENV}"
#       echo "ANDROID_NDK_ROOT=${ANDROID_SDK_ROOT}/ndk/${NDK_VERSION}" >> "${GITHUB_ENV}"
#
# Именно source, а не bash: ANDROID_SDK_ROOT/ANDROID_HOME/PATH нужны тому же
# шагу, где хвост собирает ANDROID_NDK_HOME из ${ANDROID_SDK_ROOT}. Скрипт
# намеренно без `set -u` — source распространяет режимы на весь шаг, а
# ${ANDROID_SDK_ROOT} при упавшем экшене как раз не установлен.
#
# Проверено локально (заглушкой curl): раскладка
# $SDK_ROOT/cmdline-tools/latest/bin/sdkmanager, записи в GITHUB_ENV и
# GITHUB_PATH; ветка «sdkmanager уже есть» выходит сразу.

setup_android_sdk() {
    if command -v sdkmanager >/dev/null 2>&1; then
        echo "sdkmanager уже есть: $(command -v sdkmanager)"
        return 0
    fi

    echo "::warning::sdkmanager не найден — ставим Android cmdline-tools напрямую"
    local sdk_root="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/android-sdk}}"
    mkdir -p "$sdk_root/cmdline-tools"

    # Две версии архива: если Google уберёт одну, вторая ещё жива. Обе — обычные
    # commandlinetools-linux-*_latest.zip с dl.google.com, того же хоста, что и
    # у экшена, только без промежуточного Node-рантайма.
    local ok=0 ver url
    for ver in 11076708 13114758; do
        url="https://dl.google.com/android/repository/commandlinetools-linux-${ver}_latest.zip"
        rm -rf /tmp/cmdline-tools-unpack /tmp/cmdline-tools.zip
        echo "пробую $url"
        if curl -fsSL --retry 3 --retry-delay 5 -o /tmp/cmdline-tools.zip "$url" \
           && unzip -q -o /tmp/cmdline-tools.zip -d /tmp/cmdline-tools-unpack; then
            ok=1
            break
        fi
        echo "::warning::не удалось взять $url"
    done
    if [ "$ok" != 1 ]; then
        echo "cmdline-tools не скачались ни с одного зеркала" >&2
        return 1
    fi

    rm -rf "$sdk_root/cmdline-tools/latest"
    mv /tmp/cmdline-tools-unpack/cmdline-tools "$sdk_root/cmdline-tools/latest"

    # Экспорт — для текущего шага, GITHUB_ENV/GITHUB_PATH — для следующих.
    export ANDROID_SDK_ROOT="$sdk_root"
    export ANDROID_HOME="$sdk_root"
    export PATH="$sdk_root/cmdline-tools/latest/bin:$PATH"
    if [ -n "${GITHUB_ENV:-}" ]; then
        echo "ANDROID_SDK_ROOT=$sdk_root" >> "$GITHUB_ENV"
        echo "ANDROID_HOME=$sdk_root" >> "$GITHUB_ENV"
    fi
    if [ -n "${GITHUB_PATH:-}" ]; then
        echo "$sdk_root/cmdline-tools/latest/bin" >> "$GITHUB_PATH"
    fi

    "$sdk_root/cmdline-tools/latest/bin/sdkmanager" --version || true
    echo "cmdline-tools установлены в $sdk_root/cmdline-tools/latest"
    return 0
}

setup_android_sdk
