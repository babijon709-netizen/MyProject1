#!/bin/bash
# Быстрый деплой скрипт

echo "=== Cheat Kill-Switch Deploy ==="
echo "1) Cloudflare Workers"
echo "2) Render.com (инструкция)"
echo "3) Оба"
read -p "Выбери (1/2/3): " choice

if [ "$choice" = "1" ] || [ "$choice" = "3" ]; then
  echo "--- Деплой Cloudflare Workers ---"
  cd cloudflare-worker
  if ! command -v wrangler &> /dev/null; then
    echo "Устанавливаю wrangler..."
    npm install -g wrangler
  fi
  echo "Логин в Cloudflare..."
  wrangler login
  echo "Установи секрет:"
  wrangler secret put ADMIN_SECRET
  echo "Деплой..."
  wrangler deploy
  echo "Готово! URL выше ^^^"
  cd ..
fi

if [ "$choice" = "2" ] || [ "$choice" = "3" ]; then
  echo "--- Render.com ---"
  echo "1. Залей папку server/render на GitHub"
  echo "2. Иди на https://dashboard.render.com"
  echo "3. New -> Web Service -> выбери репо"
  echo "4. Build: npm install, Start: npm start, Free plan"
  echo "5. ENV: ADMIN_SECRET=твой_секрет, CHEAT_ENABLED=true"
  echo "6. Deploy"
  echo "Подробнее в server/render/README.md"
fi

echo "После деплоя не забудь вставить URL в jni/src/protection/prot.h"
echo "  #define PROT_API_URL \"https://твой-домен/api/status\""
