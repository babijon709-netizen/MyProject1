#!/usr/bin/env python3
# Подстановка адреса сервера и секрета подписи в jni/src/license.cpp перед
# сборкой. Значения приходят из GitHub Secrets (BZW_HOST, BZW_SECRET) — в
# репозитории остаются плейсхолдеры, секрет нигде не светится.
#
#   BZW_SECRET=... [BZW_HOST=...] python3 tools/protect/inject_settings.py [путь]
#
# Без BZW_SECRET сборка останавливается: бинарь с плейсхолдером секрета не
# пройдёт ни одну проверку подписи — лучше упасть здесь, чем отдать юзерам
# кирпич.

import os
import sys

PATH = sys.argv[1] if len(sys.argv) > 1 else "jni/src/license.cpp"
DEFAULT_HOST = "benzware-license-server.fly.dev"
PLACEHOLDER_SECRET = "CHANGE_ME_same_secret_as_on_server_7f3a9c21"

src = open(PATH, encoding="utf-8").read()

host = os.environ.get("BZW_HOST", "").strip()
secret = os.environ.get("BZW_SECRET", "").strip()

if not secret:
    print("::error::BZW_SECRET не задан. Добавь его в репозитории: "
          "Settings -> Secrets and variables -> Actions -> New repository secret", file=sys.stderr)
    sys.exit(1)

if any(c in secret for c in '"\\\n\r\t') or not all(32 <= ord(c) < 127 for c in secret):
    print("::error::BZW_SECRET: только печатаемые ASCII-символы, без кавычек и слешей", file=sys.stderr)
    sys.exit(1)

if PLACEHOLDER_SECRET not in src:
    print("::error::плейсхолдер секрета не найден в license.cpp — файл уже правили?", file=sys.stderr)
    sys.exit(1)

src = src.replace(PLACEHOLDER_SECRET, secret)
print("inject: SECRET подставлен из окружения (в логе значение не печатается)")

if host and host != DEFAULT_HOST:
    if DEFAULT_HOST not in src:
        print(f"::error::дефолтный хост '{DEFAULT_HOST}' не найден в license.cpp", file=sys.stderr)
        sys.exit(1)
    src = src.replace(DEFAULT_HOST, host)
    print(f"inject: хост -> {host}")
else:
    print(f"inject: хост по умолчанию ({DEFAULT_HOST})")

open(PATH, "w", encoding="utf-8").write(src)
