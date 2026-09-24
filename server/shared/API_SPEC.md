# API Spec - Cheat Kill-Switch

Единый API для обоих бэкендов (Render и Cloudflare Workers).

## Endpoints

### GET /api/status
Главный эндпоинт для чита. Без параметров.

Response 200:
```json
{
  "enabled": true,
  "message": "Cheat active",
  "version": "1.0.0",
  "min_version": "1.0.0",
  "maintenance": false,
  "timestamp": 1700000000,
  "server_time": "2024-01-01T00:00:00.000Z"
}
```

- `enabled`: bool - если false, чит должен выключиться
- `message`: string - сообщение для юзера (показывается в логах/файле)
- `version`: string - текущая версия чита на сервере
- `min_version`: string - минимальная требуемая версия
- `maintenance`: bool - режим техработ
- `timestamp`: int - unix time

### GET /api/check?hwid=XXX&version=1.0.0&user=...
То же что /api/status, но с логированием.

Query params:
- `hwid`: string - HWID устройства
- `version`: string - версия чита
- `user`: string - имя юзера (опционально)

Response такой же как /api/status.

Можно добавить логику бана:
- Если hwid в бан листе -> `{"enabled": false, "message": "HWID banned"}`

### POST /api/admin/status
Управление состоянием. Требует secret.

Headers:
- `Content-Type: application/json`
- `X-Admin-Secret: YOUR_SECRET` (или в body)

Body:
```json
{
  "secret": "YOUR_SECRET",
  "enabled": false,
  "message": "Cheat disabled - maintenance",
  "version": "1.1.0",
  "min_version": "1.0.0",
  "maintenance": true
}
```

Все поля опциональные кроме secret. Можно менять только одно.

Response 200:
```json
{
  "success": true,
  "state": {
    "enabled": false,
    "message": "Cheat disabled - maintenance",
    "version": "1.1.0",
    "min_version": "1.0.0",
    "maintenance": true,
    "updated_at": "2024-01-01T00:00:00.000Z",
    "total_checks": 123,
    "disabled_count": 5
  }
}
```

Response 403 если неверный secret.

### GET /api/admin/state?secret=XXX
Получить текущее состояние (для админки).

Query:
- `secret`: string

Response 200: state object
Response 403: invalid secret

### GET /admin?secret=XXX
HTML админ панель с кнопками ВКЛ/ВЫКЛ.

### GET /
HTML инфо страница с текущим статусом.

## Коды ошибок

- 200 OK - успех
- 403 Forbidden - неверный secret
- 404 Not Found - неизвестный endpoint
- 400 Bad Request - невалидный JSON

## Примеры curl

Включить:
```bash
curl -X POST https://your-domain/api/admin/status \
  -H "Content-Type: application/json" \
  -d '{"secret":"SECRET","enabled":true,"message":"Cheat active"}'
```

Выключить:
```bash
curl -X POST https://your-domain/api/admin/status \
  -H "Content-Type: application/json" \
  -d '{"secret":"SECRET","enabled":false,"message":"Maintenance"}'
```

Проверить:
```bash
curl https://your-domain/api/status
curl "https://your-domain/api/check?hwid=test123&version=1.0.0"
```

## Клиентская логика (C++)

1. При старте: GET /api/check?hwid=HWID&version=VER (timeout 8 sec)
2. Парс JSON: ищи "enabled": true/false
3. Если false -> KillCheat(message)
4. Если true -> StartBackgroundChecker (каждые 60 sec)
5. В фоне: если enabled=false -> KillCheat

Fail behavior:
- FAIL_OPEN (PROT_FAIL_CLOSED=0): если сервер недоступен, разрешить работу (для тестов)
- FAIL_CLOSED (PROT_FAIL_CLOSED=1): если сервер недоступен, запретить (для продакшена)
