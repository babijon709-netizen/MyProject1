#pragma once
#include <atomic>
#include <string>

namespace prot {

// Конфиг - ПОМЕНЯЙ НА СВОЙ URL!
// Получишь после деплоя на Render или Cloudflare
// Примеры:
//   Cloudflare: https://cheat-killswitch.yourname.workers.dev/api/status
//   Render:     https://your-cheat-api.onrender.com/api/status
// Можно использовать несколько URL для резерва (если первый не отвечает - пробует второй)

// Твой деплой на Cloudflare - ГОТОВ
#ifndef PROT_API_URL
#define PROT_API_URL "https://cheat-killswitch.viktor-dictor1337.workers.dev/api/status"
#endif

// Второй резервный URL (опционально) - можешь добавить Render как бэкап
#ifndef PROT_API_URL_BACKUP
#define PROT_API_URL_BACKUP ""
#endif

// Как часто проверять в фоне (секунды)
#define PROT_CHECK_INTERVAL_SEC 60

// Что делать если сервер недоступен:
// 0 = разрешить работу (fail-open, удобно для тестов)
// 1 = запретить работу (fail-closed, максимально безопасно - юзер не обойдет отключив инет)
#define PROT_FAIL_CLOSED 1

// Версия чита (проверяется сервером если min_version задана)
#define PROT_CHEAT_VERSION "1.0.0"

struct Status {
    bool enabled = true;
    bool maintenance = false;
    std::string message = "OK";
    std::string version = PROT_CHEAT_VERSION;
    std::string min_version = "1.0.0";
    long long timestamp = 0;
    bool valid = false; // удалось ли получить ответ
};

// Проверить сервер синхронно (блокирует на ~5 сек)
Status CheckServer();

// Инициализация защиты - вызывать в начале main()
// Если чит выключен - завершает процесс или ставит флаг
void Init();

// Запустить фоновую проверку (каждые PROT_CHECK_INTERVAL_SEC)
void StartBackgroundChecker();

// Получить последний статус
Status GetLastStatus();

// Принудительно выключить чит (вызывается изнутри если enabled=false)
void KillCheat(const std::string& reason);

// Для отладки
extern std::atomic<bool> g_prot_enabled;
extern std::atomic<bool> g_prot_checked;
extern std::string g_prot_last_message;

} // namespace prot
