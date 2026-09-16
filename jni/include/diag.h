#pragma once

// Диагностический журнал: зачем, куда и как читать.
//
// Симптом, ради которого модуль появился: «у одного из пользователей не
// работает ни одна функция, но меню запускается». Весь софт стоит на одной
// цепочке — найти процесс игры -> прочитать /proc/<pid>/maps -> читать память
// игры через process_vm_readv -> найти классы по RVA оффсетов -> собрать кадр
// (камера/игроки). Любое звено может отказать только у одного человека
// (SELinux, права, 32-битная игра, другая версия клиента, урезанная прошивка),
// и снаружи все эти случаи выглядели одинаково: меню есть, функций нет.
// Журнал фиксирует каждое звено и пишет в файл рядом с конфигами —
// пользователь просто присылает его целиком.
//
// Файл: <каталог конфигов>/diag.log (переполненный переезжает в diag.log.old).
// Каждая строка дублируется в logcat (тег xvcen.diag): adb logcat -s xvcen.diag.
//
// Как читать присланный файл — см. DIAGNOSTICS.md в корне репозитория.

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <string>

#if defined(__has_include)
#  if __has_include(<android/log.h>)
#    include <android/log.h>
#    define DIAG_HAS_LOGCAT 1
#  endif
#  if __has_include(<sys/system_properties.h>)
#    include <sys/system_properties.h>
#    define DIAG_HAS_PROPERTIES 1
#  endif
#endif

namespace diag {

// ============================ быстрые счётчики ==============================
// Заполняются обёртками чтения/записи памяти в game.cpp. Считать обязательно:
// у «неработающего» пользователя счётчик успехов стоит на нуле, а гистограмма
// errno говорит, ПОЧЕМУ он на нуле. Инкременты — расслабленные атомики: на
// фоне самого syscall это шум.

// Слоты гистограммы по errno. Значения подобраны так, чтобы каждый тип отказа
// указывал на свою причину:
//   EPERM  — чужую память читать не дают вообще (SELinux/права: не поможет
//            ни одна функция и ни одна версия оффсетов);
//   ESRCH  — процесс игры исчез (гонка с его перезапуском, не страшно);
//   EFAULT — адрес не читается: читаем НЕ ТУ память (не та версия игры под
//            оффсеты либо битый указатель по цепочке);
//   EIO    — редкий отказ ядра/устройства;
//   ENOMEM/EINVAL — аномалии, в спокойной системе не встречаются;
//   прочее — всё остальное; встретилось — стоит посмотреть.
enum ErrSlot {
    kErrOther = 0,
    kErrPerm,     // EPERM
    kErrSrch,     // ESRCH
    kErrIo,       // EIO
    kErrFault,    // EFAULT
    kErrNomem,    // ENOMEM
    kErrInval,    // EINVAL
    kErrSlots
};

constexpr int err_slot(int e) {
    switch (e) {
        case EPERM:  return kErrPerm;
        case ESRCH:  return kErrSrch;
        case EIO:    return kErrIo;
        case EFAULT: return kErrFault;
        case ENOMEM: return kErrNomem;
        case EINVAL: return kErrInval;
        default:     return kErrOther;
    }
}

inline const char* err_slot_label(int slot) {
    switch (slot) {
        case kErrPerm:  return "EPERM";
        case kErrSrch:  return "ESRCH";
        case kErrIo:    return "EIO";
        case kErrFault: return "EFAULT";
        case kErrNomem: return "ENOMEM";
        case kErrInval: return "EINVAL";
        default:        return "прочее";
    }
}

inline const char* err_name(int e) { return err_slot_label(err_slot(e)); }

inline std::atomic<unsigned long long> vm_read_ok{0};
inline std::atomic<unsigned long long> vm_read_fail[kErrSlots]{};
inline std::atomic<unsigned long long> vm_write_ok{0};
inline std::atomic<unsigned long long> vm_write_fail[kErrSlots]{};

// Адрес первого сбоя с прошлого среза статистики (0 — сбоев не было).
// Отдельно на чтение и запись: у записи свои запреты, и «читается, но не
// пишется» — отдельная история (x-ray и «всегда день» молча перестают писать).
inline std::atomic<unsigned long long> vm_fail_read_addr{0};
inline std::atomic<unsigned long long> vm_fail_write_addr{0};
inline std::atomic<bool> vm_write_fail_reported{false};

// Чтений сотни-тысячи в кадр, поэтому сбой чтения только считается: в журнал
// он попадает срезом статистики (stats_tick). Печатать каждый — залип бы
// журнал на первом же скане реестра.
inline void note_read_fail(unsigned long long addr, int e) {
    vm_read_fail[err_slot(e)].fetch_add(1, std::memory_order_relaxed);
    unsigned long long expected = 0;
    vm_fail_read_addr.compare_exchange_strong(expected, addr, std::memory_order_relaxed);
}

// ============================ троттлинг сообщений ===========================
// Ключи для due(). Сообщения, которые иначе повторялись бы каждый кадр или
// каждую попытку привязки, печатаются не чаще своего интервала.
enum Key {
    kKeyAttachSearch = 0,  // процесс игры не найден
    kKeyAttachFail,        // привязка не удалась
    kKeyMapsDenied,        // /proc/<pid>/maps не открылся
    kKeyBaseMissing,       // libil2cpp.so нет в maps
    kKeyProbe,             // пробное чтение памяти не прошло
    kKeyApk,               // путь base.apk из maps
    kKeyPm,                // PlayerManager: негативный статус
    kKeyGc,                // GameControllerBase: негативный статус
    kKeyEspWatchdog,       // ESP долго не собирает кадр
    kKeyDay,               // «всегда день»: TOD не находится
    kKeyTouch,             // повтор статуса тач-инъекции
    kKeyCount
};

// true не чаще раза в period_sec на каждый ключ (и всегда — первый раз).
inline bool due(int key, unsigned period_sec) {
    if (key < 0 || key >= kKeyCount) return true;
    static std::atomic<unsigned long long> until_ms[kKeyCount]{};
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const unsigned long long now = (unsigned long long)ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull;
    const unsigned long long want = now + (unsigned long long)period_sec * 1000ull;
    unsigned long long cur = until_ms[key].load(std::memory_order_relaxed);
    while (cur <= now) {
        if (until_ms[key].compare_exchange_weak(cur, want, std::memory_order_relaxed)) return true;
    }
    return false;
}

// ============================ детали реализации =============================
// Не пользуйтесь ими напрямую: это несущие конструкции журнала.

constexpr unsigned long long kRotateBytes = 400ull * 1024ull;

inline std::mutex& impl_mutex() { static std::mutex m; return m; }
inline std::string& impl_path() { static std::string s; return s; }
inline FILE*& impl_file() { static FILE* f = nullptr; return f; }
inline unsigned long long& impl_bytes() { static unsigned long long b = 0; return b; }

inline unsigned long long impl_now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull;
}

// Метка времени строки: секунды от первого использования журнала.
inline void impl_stamp(char* out, size_t cap, const char* msg) {
    static std::atomic<unsigned long long> start_ms{0};
    const unsigned long long now = impl_now_ms();
    unsigned long long zero = 0;
    start_ms.compare_exchange_strong(zero, now);
    snprintf(out, cap, "[%7.1f] %s", (double)(now - start_ms.load()) / 1000.0, msg);
}

inline bool impl_reopen_locked() {
    if (impl_path().empty()) return false;
    FILE* f = fopen(impl_path().c_str(), "a");
    if (!f) return false;
    if (impl_file()) fclose(impl_file());
    impl_file() = f;
    setvbuf(f, nullptr, _IOFBF, 8192);
    struct stat st{};
    impl_bytes() = (::stat(impl_path().c_str(), &st) == 0) ? (unsigned long long)st.st_size : 0;
    return true;
}

// Дописать строку в файл (уже под мьютексом) и следить за размером:
// затянувшийся сеанс переезжает в .old, чтобы файл не рос вечно.
inline void impl_write_locked(const char* line) {
    FILE* f = impl_file();
    if (!f) return;
    fputs(line, f);
    fputc('\n', f);
    fflush(f);
    impl_bytes() += strlen(line) + 1;
    if (impl_bytes() > kRotateBytes) {
        fclose(f);
        impl_file() = nullptr;
        const std::string old = impl_path() + ".old";
        ::unlink(old.c_str());
        ::rename(impl_path().c_str(), old.c_str());
        if (impl_reopen_locked()) {
            char msg[128], line2[192];
            snprintf(msg, sizeof(msg),
                     "журнал переполнился (%llu КБ) — продолжаю здесь, прежний: diag.log.old",
                     kRotateBytes / 1024ull);
            impl_stamp(line2, sizeof(line2), msg);
            fputs(line2, impl_file());
            fputc('\n', impl_file());
            fflush(impl_file());
        }
    }
}

// ============================ журнал ========================================

// Строка в журнал: метка времени, текст, немедленный сброс на диск и зеркало
// в logcat. Потокобезопасно.
inline void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline void logf(const char* fmt, ...) {
    char msg[480];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    char line[560];
    impl_stamp(line, sizeof(line), msg);
#ifdef DIAG_HAS_LOGCAT
    __android_log_print(ANDROID_LOG_INFO, "xvcen.diag", "%s", line);
#endif
    std::lock_guard<std::mutex> lk(impl_mutex());
    impl_write_locked(line);
}

// Учёт сбоя записи памяти. Записи — единицы на кадр (x-ray, «всегда день»),
// поэтому первый сбой печатаем сразу и один раз за запуск, дальше — счётчики.
inline void note_write_fail(unsigned long long addr, int e) {
    vm_write_fail[err_slot(e)].fetch_add(1, std::memory_order_relaxed);
    unsigned long long expected = 0;
    const bool first = vm_fail_write_addr.compare_exchange_strong(expected, addr,
                                                                  std::memory_order_relaxed);
    if (first && !vm_write_fail_reported.exchange(true, std::memory_order_relaxed)) {
        logf("запись памяти: ПЕРВЫЙ СБОЙ addr=0x%llx errno=%d (%s) — функции с записью "
             "(x-ray, всегда день) у этого пользователя не заработают",
             addr, e, err_name(e));
    }
}

// Текст из чужой памяти для печати: непечатаемые байты — в точки, хвост обрезаем.
inline void clean_text(char* dst, size_t cap, const char* src) {
    if (!cap) return;
    size_t o = 0;
    for (size_t i = 0; src && src[i] && o + 1 < cap; ++i) {
        const unsigned char c = (unsigned char)src[i];
        dst[o++] = (char)(c >= 0x20 && c < 0x7f ? c : '.');
    }
    dst[o] = '\0';
}

// Строка в буфер с контролем переполнения (для сборки составных сообщений).
inline void buf_add(char* buf, size_t cap, int& n, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));
inline void buf_add(char* buf, size_t cap, int& n, const char* fmt, ...) {
    if (!buf || n < 0 || n >= (int)cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int written = vsnprintf(buf + n, cap - (size_t)n, fmt, ap);
    va_end(ap);
    n = (written < 0) ? (int)cap : (n + written >= (int)cap ? (int)cap - 1 : n + written);
}

// Название архитектуры по e_machine из ELF-заголовка (для строки attach
// в esp_init). Живёт здесь по той же причине, что и farm_reason_text: строки
// журнала не переводятся, а все литералы game.cpp собираются для переводов.
inline const char* elf_arch(unsigned machine) {
    switch (machine) {
        case 183: return "aarch64";   // EM_AARCH64
        case 40:  return "arm";       // EM_ARM
        case 62:  return "x86_64";    // EM_X86_64
        case 3:   return "x86";       // EM_386
        default:  return "прочая";
    }
}

// Человеческие подписи причин простоя автофарма (значения g_farm_idle_reason
// из game.cpp). Живут здесь, а не в game.cpp: строки журнала сознательно не
// переводятся (их разбирают по русским подписям), а harvesting переводов
// собирает все кириллические литералы game.cpp — см. tools/lang/gen_tables.py.
inline const char* farm_reason_text(int reason) {
    static const char* const kTexts[] = {
        "работает",                        // 0
        "ресурсы не выбраны",              // 1
        "нет привязки к игре",             // 2
        "узлов в реестре нет",             // 3
        "узлы не прошли отбор",            // 4
        "нет позиции/углов камеры",        // 5
        "узлам нужен другой инструмент",   // 6
    };
    return reason >= 0 && reason < (int)(sizeof(kTexts) / sizeof(kTexts[0]))
        ? kTexts[reason] : "?";
}

// ============================ инициализация =================================

// Открыть файл журнала (dir — каталог конфигов с завершающим '/') и записать
// шапку окружения. Повторный вызов игнорируется.
inline void init(const char* dir) {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;

    if (dir && dir[0]) {
        impl_path() = std::string(dir) + "diag.log";
        // Переполненный в прошлый раз файл — в .old, пишем с чистого.
        struct stat st{};
        if (::stat(impl_path().c_str(), &st) == 0 && (unsigned long long)st.st_size > kRotateBytes) {
            const std::string old = impl_path() + ".old";
            ::unlink(old.c_str());
            ::rename(impl_path().c_str(), old.c_str());
        }
    }
    const bool file_ok = impl_reopen_locked();

    logf("=== журнал диагностики, бинарник собран " __DATE__ " " __TIME__ " ===");
    if (!file_ok) {
        logf("файл журнала НЕ ОТКРЫЛСЯ%s%s errno=%d (%s) — журнал будет только в logcat; "
             "верояная причина: нет записи в каталог конфигов (тогда и конфигы не сохраняются)",
             impl_path().empty() ? " (каталог не задан)" : ": ",
             impl_path().c_str(), errno, err_name(errno));
    }
    logf("легенда: чтений 0 ок при живой привязке — чужая память не читается ВООБЩЕ "
         "(ни одна функция не заработает); EPERM — запрет (SELinux/права); "
         "EFAULT — адреса не те (не та версия игры под оффсеты)");

    // ---- окружение устройства: без него «один пользователь» не отличить ----
#ifdef DIAG_HAS_PROPERTIES
    char val[256];
    struct Prop { const char* name; const char* title; };
    static const Prop props[] = {
        { "ro.product.manufacturer",    "устройство"     },
        { "ro.product.model",           ""               },
        { "ro.build.version.release",   "Android"        },
        { "ro.build.version.sdk",       "SDK"            },
        { "ro.product.cpu.abi",         "ABI"            },
    };
    std::string dev;
    for (const Prop& p : props) {
        val[0] = '\0';
        __system_property_get(p.name, val);
        if (!val[0]) continue;
        dev += p.title[0] ? std::string(" ") + p.title + "=" : std::string(" ") + val;
        if (p.title[0]) dev += val;
    }
    if (!dev.empty()) logf("устройство:%s", dev.c_str());
#endif

    if (FILE* f = fopen("/sys/fs/selinux/enforce", "r")) {
        const int c = fgetc(f);
        fclose(f);
        logf("SELinux: %s", c == '1' ? "enforcing (режим запретов)"
                                     : c == '0' ? "permissive" : "неизвестно");
    } else {
        logf("SELinux: статус не читается (%s)", err_name(errno));
    }
    if (FILE* f = fopen("/proc/sys/kernel/yama/ptrace_scope", "r")) {
        const int c = fgetc(f);
        fclose(f);
        if (c >= 0) logf("yama/ptrace_scope: %c", (char)c);
    }

    const time_t wall = time(nullptr);
    char tmbuf[64];
    strftime(tmbuf, sizeof(tmbuf), "%Y-%m-%d %H:%M:%S", localtime(&wall));
    logf("запуск: %s, pid=%d uid=%d%s", tmbuf, (int)getpid(), (int)getuid(),
         getuid() == 0 ? " (root)" : "");
}

// ============================ срез счётчиков ================================
// Вызывается из медленного потока (поток привязки, каждые 1.5 с); сам печатает
// не чаще раза в kStatsPeriodSec и только если счётчики двигались.
// В покое журнал молчит, поэтому каждая его строка — событие.
constexpr unsigned kStatsPeriodSec = 10;

inline void stats_tick() {
    static std::atomic<bool> busy{false};
    static std::atomic<unsigned long long> next_ms{0};

    bool expected = false;
    if (!busy.compare_exchange_strong(expected, true)) return;

    const unsigned long long now = impl_now_ms();
    if (now < next_ms.load(std::memory_order_relaxed)) { busy.store(false); return; }
    next_ms.store(now + (unsigned long long)kStatsPeriodSec * 1000ull, std::memory_order_relaxed);

    const unsigned long long ro = vm_read_ok.exchange(0, std::memory_order_relaxed);
    const unsigned long long wo = vm_write_ok.exchange(0, std::memory_order_relaxed);
    unsigned long long rf[kErrSlots] = {}, wf[kErrSlots] = {};
    for (int i = 0; i < kErrSlots; ++i) {
        rf[i] = vm_read_fail[i].exchange(0, std::memory_order_relaxed);
        wf[i] = vm_write_fail[i].exchange(0, std::memory_order_relaxed);
    }

    unsigned long long rf_sum = 0, wf_sum = 0;
    for (int i = 0; i < kErrSlots; ++i) { rf_sum += rf[i]; wf_sum += wf[i]; }
    if (!ro && !wo && !rf_sum && !wf_sum) { busy.store(false); return; }

    char msg[520];
    int n = 0;
    buf_add(msg, sizeof(msg), n, "память игры за %u с: чтений %llu ок", kStatsPeriodSec, ro);
    if (rf_sum) {
        buf_add(msg, sizeof(msg), n, " / %llu сбоев (", rf_sum);
        bool first = true;
        for (int i = 0; i < kErrSlots; ++i) {
            if (!rf[i]) continue;
            buf_add(msg, sizeof(msg), n, "%s%s x%llu", first ? "" : ", ",
                    err_slot_label(i), rf[i]);
            first = false;
        }
        buf_add(msg, sizeof(msg), n, ")");
        const unsigned long long a = vm_fail_read_addr.exchange(0, std::memory_order_relaxed);
        if (a) buf_add(msg, sizeof(msg), n, ", первый сбой чтения addr=0x%llx", a);
    }
    if (wo || wf_sum) {
        buf_add(msg, sizeof(msg), n, "; записей %llu ок / %llu сбоев", wo, wf_sum);
        bool first = true;
        for (int i = 0; i < kErrSlots; ++i) {
            if (!wf[i]) continue;
            buf_add(msg, sizeof(msg), n, "%s%s x%llu", first ? " (" : ", ",
                    err_slot_label(i), wf[i]);
            first = false;
        }
        if (!first) buf_add(msg, sizeof(msg), n, ")");
    }
    logf("%s", msg);
    busy.store(false);
}

} // namespace diag
