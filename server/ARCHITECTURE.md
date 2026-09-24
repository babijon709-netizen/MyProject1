# Архитектура Kill-Switch

```
┌─────────────┐          HTTPS          ┌──────────────────────┐
│  Чит (C++)  │  ──GET /api/check?hwid──▶│  Cloudflare Workers  │
│  Android    │  ◀──{"enabled":false}── │  или Render.com      │
│  binary     │                         │  (бесплатный хост)   │
└─────────────┘                         └──────────────────────┘
       │                                          ▲
       │  каждые 60 сек                           │ POST /api/admin/status
       │  фоновая проверка                        │ {"enabled": false}
       │                                          │
       │                                          │
       │                                   ┌─────────────┐
       │                                   │  Ты (админ) │
       │                                   │  Браузер    │
       │                                   │  /admin?secret │
       └───────────────────────────────────┴─────────────┘

Поток:
1. Юзер запускает чит
2. prot::Init() -> GET https://your-worker.workers.dev/api/check?hwid=xxx
3. Сервер отвечает {"enabled": true} -> чит работает
4. Ты заходишь в админку https://.../admin?secret=SECRET и жмешь ВЫКЛЮЧИТЬ
5. Сервер сохраняет enabled=false
6. Через 60 сек фоновая проверка в чите видит enabled=false и убивает процесс
7. При следующем запуске чит сразу не запустится

Компоненты:
- jni/src/protection/prot.h/cpp - клиентская часть
- http_client - пытается curl, wget, raw socket
- server/cloudflare-worker - бэкенд CF
- server/render - бэкенд Render
- Админка - HTML + JS, один клик

Безопасность:
- ADMIN_SECRET - только ты знаешь, без него нельзя выключить/включить
- PROT_FAIL_CLOSED=1 - если юзер отключит интернет, чит не будет работать
- HWID - логируется, можно банить
- HTTPS - трафик шифруется
```

## Failover (резерв)

Если указать два URL в prot.h:
```cpp
#define PROT_API_URL "https://cf-worker.workers.dev/api/status"
#define PROT_API_URL_BACKUP "https://render.onrender.com/api/status"
```

Клиент:
1. Пробует основной
2. Если не отвечает 8 сек - пробует бэкап
3. Если оба не отвечают - смотрит PROT_FAIL_CLOSED

## Масштабирование

- Cloudflare Workers: 100k req/day free, $5 за 10M
- Render: 750 часов/мес free (1 сервис), $7/мес без сна
- Можно добавить свой VPS как третий бэкап
- Можно добавить Firebase Remote Config как четвертый (бесплатно)

## Обход защиты юзером

Как юзер может обойти:
1. Патч бинарника (убрать вызов prot::Init) - нужен реверс скилл
2. Подменить DNS/hosts чтобы api указывал на свой сервер с enabled=true - решается зашифрованным URL + проверкой SSL pinning
3. Отключить интернет если FAIL_OPEN - решается FAIL_CLOSED=1
4. Найти URL через strings и заблокировать через firewall - решается обфускацией URL

Для 99% юзеров текущей защиты хватит. Для параноиков добавь:
- Шифрование URL (xor/base64)
- Проверку целостности бинарника
- Анти-отладку
- Несколько точек проверки (не только в main, но и в esp_draw, aim и тд)
- Проверку времени (если юзер перевел время назад)

