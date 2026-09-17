// app/diag_log.cpp — Журнал здоровья: запись событий в файл.
//
// Модуль новый (не из монолита): появился вместе с разбором «чит сам
// выключается». Что и зачем пишется — в шапке app/diag_log.h.
//
// Как устроено. Файл открывается один раз при старте и живёт открытым: события
// редкие, но в момент поломки важно, чтобы запись не могла провалиться из-за
// занятости файла. Строка собирается целиком в буфере и уходит одним write(),
// под мьютексом: зовут журнал и поток кадра, и поток привязки, и одноразовые
// потоки тача.
#include "app/common.h"
#include "app/diag_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdint.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>

namespace {

constexpr size_t kLineMax   = 320;          // одна строка события
constexpr off_t  kFileLimit = 1 << 20;      // 1 МБ — дальше начинаем заново
constexpr double kDedupWindow = 2.0;        // одинаковые события внутри окна схлопываем

int    g_fd = -1;
char   g_path[256] = {};
bool   g_enabled = false;
double g_start = 0.0;

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

// Схлопывание повторов: держим последнюю строку и счётчик подавленных.
char   g_last[192] = {};
int    g_repeat = 0;

double now_seconds() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// Запись готовой строки. Вызывать под мьютексом.
void write_locked(const char* text) {
    if (g_fd < 0) return;
    const size_t len = strlen(text);
    ssize_t written = write(g_fd, text, len);
    (void)written;
}

void emit(const char* tag, const char* text) {
    char line[kLineMax];
    pthread_mutex_lock(&g_lock);
    if (g_repeat > 0) {
        // Перед новым событием отдаём долг: сколько раз предыдущее повторилось.
        char tail[224];
        snprintf(tail, sizeof(tail), "%8.1f %-7s (предыдущее повторилось ×%d)\n",
                 now_seconds() - g_start, "repeat", g_repeat);
        write_locked(tail);
        g_repeat = 0;
    }
    snprintf(line, sizeof(line), "%8.1f %-7s %s\n", now_seconds() - g_start, tag, text);
    write_locked(line);
    pthread_mutex_unlock(&g_lock);
}

// ---- Аварийная строка из обработчика сигнала --------------------------------
// Здесь нельзя ни мьютекса, ни snprintf, ни stdio: только write() по уже
// открытому дескриптору. Поэтому строка собирается руками, а числа — вручную.
char* crash_append(char* out, const char* text) {
    while (*text) *out++ = *text++;
    return out;
}

char* crash_append_uint(char* out, unsigned long long value) {
    char digits[24];
    int n = 0;
    if (value == 0) digits[n++] = '0';
    while (value > 0) { digits[n++] = (char)('0' + (value % 10)); value /= 10; }
    while (n > 0) *out++ = digits[--n];
    return out;
}

char* crash_append_hex(char* out, unsigned long long value) {
    static const char* kDigits = "0123456789abcdef";
    char digits[24];
    int n = 0;
    if (value == 0) digits[n++] = '0';
    while (value > 0) { digits[n++] = kDigits[value & 0xF]; value >>= 4; }
    while (n > 0) *out++ = digits[--n];
    return out;
}

void crash_handler(int sig, siginfo_t* info, void*) {
    if (g_fd >= 0) {
        char line[160];
        char* out = line;
        out = crash_append(out, "\n=== ПАДЕНИЕ: сигнал ");
        out = crash_append_uint(out, (unsigned long long)sig);
        out = crash_append(out, ", адрес 0x");
        out = crash_append_hex(out, info ? (unsigned long long)(uintptr_t)info->si_addr : 0ULL);
        out = crash_append(out, " ===\n");
        ssize_t w = write(g_fd, line, (size_t)(out - line));
        (void)w;
    }
    // Дальше — как будто обработчика не было: система должна увидеть падение.
    signal(sig, SIG_DFL);
    raise(sig);
}

}  // namespace

void diag_install_crash_handler() {
    static char alt_stack[32768];   // свой стек: переполнение основного не мешает
    stack_t ss{};
    ss.ss_sp = alt_stack;
    ss.ss_size = sizeof(alt_stack);
    sigaltstack(&ss, nullptr);

    struct sigaction sa{};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);
}

void diag_self_stats(char* out, size_t size) {
    if (!out || size == 0) return;
    unsigned long long rss_mb = 0, threads = 0, fds = 0;
    int oom = 0;
    char buf[512];

    int fd = open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = 0;
            unsigned long long total_pages = 0, resident_pages = 0;
            if (sscanf(buf, "%llu %llu", &total_pages, &resident_pages) == 2) {
                (void)total_pages;
                rss_mb = resident_pages * (unsigned long long)(sysconf(_SC_PAGESIZE) / 1024) / 1024ULL;
            }
        }
    }
    fd = open("/proc/self/status", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = 0;
            const char* t = strstr(buf, "Threads:");
            if (t) threads = strtoull(t + 8, nullptr, 10);
        }
    }
    fd = open("/proc/self/oom_score_adj", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) { buf[n] = 0; oom = atoi(buf); }
    }
    // Дескрипторы считаем по каталогу: их число — самый быстрый признак утечки.
    DIR* dir = opendir("/proc/self/fd");
    if (dir) {
        while (readdir(dir)) ++fds;
        closedir(dir);
        if (fds > 0) fds -= 1;   // сама запись «.», «..» считается один раз
    }
    snprintf(out, size, "память %llu МБ, fd %llu, потоков %llu, oom_adj %d",
             rss_mb, fds, threads, oom);
}

void diag_init(const char* dir) {
    if (g_fd >= 0) return;
    if (!dir || !dir[0]) return;
    snprintf(g_path, sizeof(g_path), "%s%s", dir, "xvcen_health.log");
    struct stat st{};
    if (stat(g_path, &st) == 0 && st.st_size > kFileLimit) {
        // Дорос до предела: прошлый журнал уходит в .1 (его и смотреть, если
        // поломка была раньше), а новый начинается с нуля.
        char old[280];
        snprintf(old, sizeof(old), "%s.1", g_path);
        remove(old);
        rename(g_path, old);
    }
    g_fd = open(g_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    g_enabled = (g_fd >= 0);
    g_start = now_seconds();
    if (!g_enabled) return;
    pthread_mutex_lock(&g_lock);
    char head[kLineMax];
    snprintf(head, sizeof(head), "%8.1f %-7s журнал открыт: %s\n", 0.0, "health", g_path);
    write_locked(head);
    pthread_mutex_unlock(&g_lock);
}

bool diag_enabled() { return g_enabled; }

const char* diag_path() { return g_path; }

void diag_log(const char* tag, const char* fmt, ...) {
    if (!g_enabled || !tag || !fmt) return;
    char text[224];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    // Повтор того же события в пределах окна — не строка, а счётчик.
    pthread_mutex_lock(&g_lock);
    if (g_last[0] && strcmp(g_last, text) == 0) {
        ++g_repeat;
        pthread_mutex_unlock(&g_lock);
        return;
    }
    snprintf(g_last, sizeof(g_last), "%s", text);
    pthread_mutex_unlock(&g_lock);

    emit(tag, text);
}
