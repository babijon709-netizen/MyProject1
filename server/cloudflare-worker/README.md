# Cloudflare Workers Backend - Cheat Kill-Switch

Самый быстрый и надежный бесплатный хост.

## Преимущества
- 100,000 запросов/день бесплатно
- Глобальная сеть Cloudflare, <10ms
- Не засыпает
- Мгновенное выключение
- Бесплатный домен `*.workers.dev`

## Деплой

### 1. Установи wrangler
```bash
npm install -g wrangler
```

### 2. Логин
```bash
wrangler login
# Откроется браузер, логинишься в Cloudflare (бесплатный акк)
```

### 3. Настрой секрет
```bash
cd server/cloudflare-worker
wrangler secret put ADMIN_SECRET
# Введи свой секрет, например: MySuperSecret123!
```

### 4. Деплой (без KV - проще)
```bash
wrangler deploy
```

Получишь URL: `https://cheat-killswitch.<your-subdomain>.workers.dev`

### 5. (Опционально) С KV для сохранения

Без KV состояние хранится в памяти и сбрасывается при рестарте воркера (раз в несколько часов). С KV - сохраняется навсегда.

```bash
wrangler kv:namespace create "CHEAT_KV"
# Вывод:
# { id: "abc123...", preview_id: "xyz..." }

# Вставь ID в wrangler.toml:
# [[kv_namespaces]]
# binding = "CHEAT_KV"
# id = "abc123..."

wrangler deploy
```

### 6. Проверка

```bash
curl https://cheat-killswitch.<your-subdomain>.workers.dev/api/status
# {"enabled": true, "message": "Cheat active", ...}
```

Админка:
`https://cheat-killswitch.<your-subdomain>.workers.dev/admin?secret=MySuperSecret123!`

## Настройка чита

В `jni/src/protection/prot.h`:
```cpp
#define PROT_API_URL "https://cheat-killswitch.<your-subdomain>.workers.dev/api/status"
```

## Команды

```bash
wrangler dev          # Локальный тест
wrangler deploy       # Деплой
wrangler tail         # Логи в реальном времени
wrangler secret list  # Список секретов
```

## Лимиты Free

- 100k запросов/день (хватит на 1000 юзеров по 100 проверок)
- 10ms CPU на запрос
- 128MB RAM
- Если нужно больше - $5/мес за 10M запросов

## Кастомный домен (опционально)

В Cloudflare Dashboard -> Workers -> твой воркер -> Settings -> Triggers -> Add Custom Domain
Можешь привязать свой домен типа `api.yourdomain.com`
