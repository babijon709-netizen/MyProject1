# Render.com Backend - Cheat Kill-Switch

Бесплатный хост для управления читом.

## Деплой за 2 минуты

### 1. Подготовка репо
Залей папку `server/render` на GitHub как отдельный репо, или используй этот репо (укажи Root Directory = `server/render` в настройках Render).

### 2. Создание сервиса
- Иди на https://dashboard.render.com
- New + -> Web Service
- Connect твой GitHub репо
- Settings:
  - Name: `cheat-killswitch` (любое)
  - Region: любой ближе к юзерам (Singapore для Азии, Frankfurt для EU)
  - Branch: main
  - Root Directory: `server/render` (если деплоишь из этого репо) или пусто если отдельный репо
  - Runtime: Node
  - Build Command: `npm install`
  - Start Command: `npm start`
  - Plan: Free

### 3. ENV переменные
В разделе Environment добавь:
- `ADMIN_SECRET` = `ТВОЙ_СЕКРЕТ_12345` (придумай сложный!)
- `CHEAT_ENABLED` = `true`

### 4. Deploy
Нажми Create Web Service -> жди 2-3 минуты.

Получишь URL типа: `https://cheat-killswitch-xxxx.onrender.com`

### 5. Проверка
- Открой `https://твой-url.onrender.com/api/status` - должен вернуть JSON `{"enabled": true, ...}`
- Открой `https://твой-url.onrender.com/admin?secret=ТВОЙ_СЕКРЕТ` - админка

## Использование

В `jni/src/protection/prot.h` вставь:
```cpp
#define PROT_API_URL "https://cheat-killswitch-xxxx.onrender.com/api/status"
```

## Важно про Free Tier

- Сервер засыпает через 15 мин без запросов. Первый запрос после сна ~30-50 сек (cold start).
- Клиент в чите ждет 8 сек, так что первый запуск после сна может быть долгим, но второй уже быстрый.
- Решение: используй Cloudflare Workers как основной (не засыпает), Render как бэкап.
- Или платный тариф $7/мес - не засыпает.

## Логи

В Render Dashboard -> Logs видишь все проверки:
```
Check from HWID=xxx version=1.0.0 -> enabled=true
ADMIN UPDATE: enabled=false by IP ...
```

## API

- `GET /api/status` - статус
- `GET /api/check?hwid=XXX&version=1.0` - статус с логом
- `POST /api/admin/status` - управление (нужен secret)
- `GET /admin?secret=XXX` - админ панель
