# 🛡️ Cheat Kill-Switch - Удаленное управление читом

Система позволяет **в любой момент выключить чит у всех юзеров** через бесплатный хост (Cloudflare Workers или Render.com).

## Как работает

1. Чит при запуске делает запрос на твой сервер `GET /api/check?hwid=...`
2. Сервер отвечает `{"enabled": true/false, "message": "..."}`
3. Если `enabled=false` - чит сразу закрывается, файл `/data/local/tmp/cheat_disabled.txt` с причиной
4. Каждые 60 секунд фоновая проверка - если выключишь во время игры, чит выключится у всех в течение минуты
5. Админ панель `/admin?secret=XXX` - кнопка ВКЛ/ВЫКЛ в один клик

## Варианты хостинга (оба бесплатные)

### Вариант 1: Cloudflare Workers (рекомендуется)
- ✅ 100,000 запросов/день бесплатно
- ✅ Глобальная сеть, <10ms задержка
- ✅ Не засыпает (в отличие от Render)
- ✅ Мгновенное выключение
- ❌ Нужен домен workers.dev (бесплатно)

### Вариант 2: Render.com
- ✅ Полноценный Node.js сервер
- ✅ Бесплатный тариф
- ✅ Простая админка
- ❌ Засыпает через 15 мин неактивности (первый запрос ~30 сек)
- ❌ 750 часов/месяц (хватит на 1 сервис)

**Можно использовать оба одновременно для резерва!** Укажи два URL в `prot.h` - если первый не отвечает, пробует второй.

---

## Быстрый старт - Cloudflare Workers

```bash
cd server/cloudflare-worker
npm install -g wrangler
wrangler login

# Установи секрет (ОБЯЗАТЕЛЬНО поменяй!)
wrangler secret put ADMIN_SECRET
# Введи например: MySuperSecret123!

# Деплой
wrangler deploy
```

После деплоя получишь URL типа:
`https://cheat-killswitch.yourname.workers.dev`

Админка:
`https://cheat-killswitch.yourname.workers.dev/admin?secret=MySuperSecret123!`

API:
`https://cheat-killswitch.yourname.workers.dev/api/status`

### С KV для сохранения состояния (рекомендуется)

```bash
wrangler kv:namespace create "CHEAT_KV"
# Скопируй ID из вывода в wrangler.toml
# Затем
wrangler deploy
```

Без KV тоже работает, но состояние сбрасывается при рестарте воркера (раз в несколько часов).

---

## Быстрый старт - Render.com

1. Залей папку `server/render` на GitHub (или используй этот репозиторий)
2. Иди на https://dashboard.render.com
3. New + -> Web Service -> Connect GitHub repo
4. Настройки:
   - Build Command: `npm install`
   - Start Command: `npm start`
   - Environment: `Node`
   - Free tier
5. Добавь ENV переменные:
   - `ADMIN_SECRET` = твой секрет (например `MySuperSecret123!`)
   - `CHEAT_ENABLED` = `true`
6. Deploy -> получишь URL `https://your-cheat-api.onrender.com`
7. Админка: `https://your-cheat-api.onrender.com/admin?secret=MySuperSecret123!`

---

## Настройка чита (клиент)

### 1. Вставь свой URL в код

Открой `jni/src/protection/prot.h` и поменяй:

```cpp
#define PROT_API_URL "https://cheat-killswitch.yourname.workers.dev/api/status"
#define PROT_API_URL_BACKUP "https://your-cheat-api.onrender.com/api/status" // опционально
```

### 2. Настрой поведение при отсутствии интернета

```cpp
#define PROT_FAIL_CLOSED 0 // 0 = если нет инета - разрешить (для тестов)
                           // 1 = если нет инета - запретить (максимально безопасно)
```

Для продакшена ставь `1`, чтобы юзер не мог обойти блокировку отключив интернет.

### 3. Собери чит

```bash
./build.sh
```

Или через GitHub Actions - бинарник в artifacts.

### 4. Проверь

- Запусти чит - должен работать если сервер enabled=true
- Зайди в админку, нажми ❌ ВЫКЛЮЧИТЬ
- Подожди 60 сек или перезапусти чит - должен выключиться

Логи смотри:
```bash
adb logcat -s PROT
cat /data/local/tmp/cheat_disabled.txt
```

---

## Админка - как выключать

### Через браузер (просто)
Открой `https://твой-домен/admin?secret=ТВОЙ_СЕКРЕТ`
Нажми кнопку ВЫКЛЮЧИТЬ - мгновенно у всех перестанет работать.

### Через curl (для ботов/скриптов)

Выключить:
```bash
curl -X POST https://твой-домен/api/admin/status \
  -H "Content-Type: application/json" \
  -d '{"secret":"MySuperSecret123!","enabled":false,"message":"Cheat disabled - maintenance"}'
```

Включить:
```bash
curl -X POST https://твой-домен/api/admin/status \
  -H "Content-Type: application/json" \
  -d '{"secret":"MySuperSecret123!","enabled":true,"message":"Cheat active"}'
```

### Проверить статус
```bash
curl https://твой-домен/api/status
```

---

## Продвинутые фишки

### Бан по HWID (скоро)
В `server.js` и `index.js` есть место для бана по HWID. Можешь добавить массив забаненных и проверять в `/api/check`.

### Проверка версии
Установи `min_version` в админке. Если версия клиента ниже - можешь в клиенте показать "нужно обновить".

### Несколько резервных серверов
В `prot.h` укажи `PROT_API_URL_BACKUP` - если основной не отвечает, пробует резервный.

### Логирование юзеров
Все запросы с `hwid` логируются в консоль сервера. На Render смотри в Logs, на Cloudflare в `wrangler tail`.

---

## Безопасность

- **Поменяй ADMIN_SECRET** - по дефолту `CHANGE_ME_SUPER_SECRET_12345`, любой может выключить/включить
- **PROT_FAIL_CLOSED=1** для продакшена - иначе юзер отключит интернет и обойдет
- **Обфускация URL** - в продакшене зашифруй URL (base64, xor), чтобы не нашли в бинарнике через strings
- **HTTPS** - всегда используй https, чтобы не перехватили
- **Не храни секрет в коде** - используй env переменные

---

## Структура файлов

```
server/
├── cloudflare-worker/   # Cloudflare Workers backend
│   ├── src/index.js     # Воркер
│   ├── wrangler.toml    # Конфиг
│   └── package.json
├── render/              # Render.com backend
│   ├── server.js        # Express сервер
│   ├── public/admin.html# Админка
│   └── package.json
└── README.md            # Этот файл

jni/src/protection/      # Клиентская часть (в чите)
├── prot.h               # Конфиг и API
├── prot.cpp             # Логика kill-switch
└── http_client.h/.cpp   # HTTP клиент (curl/wget/socket)
```

---

## FAQ

**Q: Что если юзер пропатчит проверку?**
A: Это native binary, проверка в нескольких местах. Для серьезной защиты добавь обфускацию, проверку целостности, анти-отладку. Но для 99% юзеров текущего хватит.

**Q: Render засыпает, чит не работает 30 сек?**
A: Используй Cloudflare как основной, Render как бэкап. Или платный тариф Render $7/мес без сна.

**Q: Можно ли сделать чтобы чит работал только у определенных людей?**
A: Да, добавь whitelist по HWID в сервере. В `/api/check` проверяй если hwid в списке разрешенных.

**Q: Как часто проверять?**
A: Сейчас 60 сек. Можно уменьшить до 10 сек в `prot.h` -> `PROT_CHECK_INTERVAL_SEC`. Но больше нагрузка на сервер.

**Q: Что если Cloudflare забанит?**
A: Это обычный Workers, не нарушает TOS. Но если боишься - используй Render или свой VPS.

---

## Поддержка

Если что-то не работает:
1. `adb logcat -s PROT` - логи защиты
2. `curl https://твой-домен/api/status` - проверь сервер
3. Проверь что URL в `prot.h` правильный и без опечаток
4. Проверь что ADMIN_SECRET поменял

Удачного хостинга! 🚀
