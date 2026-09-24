# 🛡️ Как поставить чит на бесплатный хост и выключать в любой момент

Готовая система kill-switch для твоего чита. Один клик в админке - и ни один юзер не может пользоваться читом.

## Что уже сделано в этом репо

✅ Бэкенд для **Cloudflare Workers** (100k запросов/день бесплатно, не засыпает, <10ms)
✅ Бэкенд для **Render.com** (Node.js, бесплатно, 750 часов/месяц)
✅ Клиентская защита в чите (`jni/src/protection/` - уже интегрирована в `main.cpp` и `Android.mk`)
✅ Админ панель с кнопками ВКЛ/ВЫКЛ
✅ API с логированием HWID

## Пошаговая инструкция (5 минут)

### Шаг 1: Выбери хост

**Рекомендую Cloudflare Workers** - быстрее, не засыпает, бесплатно.

#### Cloudflare Workers (рекомендуется)

```bash
# 1. Установи wrangler
npm install -g wrangler

# 2. Логин
wrangler login

# 3. Перейди в папку
cd server/cloudflare-worker

# 4. Установи секрет (придумай свой!)
wrangler secret put ADMIN_SECRET
# Введи: MySuperSecret123!@#

# 5. Деплой
wrangler deploy
```

Готово! Получишь URL типа:
`https://cheat-killswitch.yourname.workers.dev`

Админка:
`https://cheat-killswitch.yourname.workers.dev/admin?secret=MySuperSecret123!@#`

#### Render.com (альтернатива)

1. Залей `server/render` на GitHub
2. https://dashboard.render.com -> New Web Service -> выбери репо
3. Build: `npm install`, Start: `npm start`, Free plan
4. ENV: `ADMIN_SECRET=MySuperSecret123!@#`, `CHEAT_ENABLED=true`
5. Deploy -> получишь `https://xxx.onrender.com`

### Шаг 2: Вставь URL в чит

Открой `jni/src/protection/prot.h`:

```cpp
#define PROT_API_URL "https://cheat-killswitch.yourname.workers.dev/api/status"
#define PROT_API_URL_BACKUP "https://xxx.onrender.com/api/status" // опционально второй
```

Поменяй `YOURNAME` на свой URL после деплоя!

### Шаг 3: Настрой безопасность

В том же файле:

```cpp
#define PROT_FAIL_CLOSED 1 // 1 = если нет инета, чит НЕ работает (безопасно)
                            // 0 = если нет инета, чит работает (для тестов)

#define PROT_CHECK_INTERVAL_SEC 60 // как часто проверять (сек)
```

Для продакшена ставь `1`, чтобы юзер не обошел отключив интернет.

### Шаг 4: Собери чит

```bash
./build.sh
```

Или через GitHub Actions (артефакт `xvcen.sh`).

### Шаг 5: Проверь

1. Запусти чит - должен работать
2. Открой админку `https://.../admin?secret=...`
3. Нажми ❌ ВЫКЛЮЧИТЬ
4. Перезапусти чит или подожди 60 сек - чит должен закрыться

Логи:
```bash
adb logcat -s PROT
cat /data/local/tmp/cheat_disabled.txt
```

## Как выключать чит

### Через браузер (проще всего)
Открой админку и нажми кнопку. Мгновенно у всех выключается.

### Через телефон (Termux)
```bash
curl -X POST https://твой-домен/api/admin/status \
  -H "Content-Type: application/json" \
  -d '{"secret":"ТВОЙ_СЕКРЕТ","enabled":false,"message":"Тех работы"}'
```

### Через бота (Telegram/Discord)
Сделай бота который делает POST запрос - сможешь выключать из ТГ одной командой.

## Что происходит у юзера когда выключено

1. При запуске чита - сразу закрывается, в `/data/local/tmp/cheat_disabled.txt` причина
2. Если выключишь во время игры - через 60 сек чит закроется у всех
3. В логах `adb logcat -s PROT` пишет "Cheat disabled by admin"

Юзер не может обойти если `PROT_FAIL_CLOSED=1` и URL зашифрован.

## Продвинуто

### Шифрование URL (чтобы не нашли через strings)
В `prot.h` зашифруй URL base64 или xor. Пример:

```cpp
// Вместо открытого URL
#define PROT_API_URL "https://..."

// Сделай функцию которая расшифровывает
std::string GetApiUrl() {
    // base64 decode или xor
    return decode("aHR0cHM6Ly4uLg==");
}
```

### Бан по HWID
В `server/render/server.js` и `cloudflare-worker/src/index.js` добавь:

```js
const BANNED_HWIDS = ["hwid1", "hwid2"];
if (BANNED_HWIDS.includes(hwid)) {
    return jsonResponse({enabled: false, message: "HWID banned"});
}
```

### Whitelist (только для своих)
```js
const ALLOWED_HWIDS = ["твой hwid", "друга hwid"];
if (!ALLOWED_HWIDS.includes(hwid)) {
    return jsonResponse({enabled: false, message: "Not whitelisted"});
}
```

### Логи в Telegram
Добавь в сервер отправку в ТГ при каждой проверке:

```js
fetch(`https://api.telegram.org/bot${BOT_TOKEN}/sendMessage?chat_id=${CHAT_ID}&text=Check from ${hwid}`);
```

## FAQ

**Q: Юзер может пропатчить проверку?**
A: Да, если умеет реверсить native. Но 99% не умеют. Для защиты добавь обфускацию, проверку целостности.

**Q: Render засыпает?**
A: Да, через 15 мин. Используй Cloudflare как основной.

**Q: Сколько юзеров выдержит бесплатно?**
A: Cloudflare 100k запросов/день = 1000 юзеров по 100 проверок. Хватит. Render 750 часов = 1 сервис на месяц.

**Q: Можно ли сделать чтобы чит работал только у кого есть ключ?**
A: Да, сделай whitelist по HWID или добавь проверку ключа в `/api/check?key=XXX`.

## Файлы

```
server/
├── cloudflare-worker/ - бэкенд Cloudflare
├── render/ - бэкенд Render
├── shared/API_SPEC.md - документация API
└── README.md - подробная дока

jni/src/protection/
├── prot.h - конфиг (URL, версия)
├── prot.cpp - логика kill-switch
└── http_client.cpp/h - HTTP клиент

CHEAT_HOSTING_GUIDE.md - этот файл
```

## Поддержка

Если не работает:
1. `curl https://твой-домен/api/status` - проверь сервер
2. `adb logcat -s PROT` - логи чита
3. Проверь URL в `prot.h` без опечаток
4. Проверь что `ADMIN_SECRET` поменял

Удачного хостинга! 🚀

P.S. Не забудь поменять `ADMIN_SECRET` с дефолтного!
