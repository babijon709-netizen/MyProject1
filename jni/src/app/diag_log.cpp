// app/diag_log.cpp — Журнал здоровья: запись событий в файл.
//
// Модуль новый (не из монолита): появился вместе с разбором «чит сам
// выключается». Что и зачем пишется — в шапке app/diag_log.h.
//
// Как устроено — и почему именно так.
//
// Журнал лежит на внешнем хранилище (/storage/emulated/0/benzhack), а это на
// Android не «просто файл», а FUSE: write() туда может встать на секунды, если
// система занята (медиа-скан, MTP, да и просто занятое хранилище). Первая версия
// писала строку прямо из потока, который её позвал, под общим мьютексом. Это
// давало сразу две возможности заморозить чит: (1) поток кадра сам упирается в
// write() на внешнее хранилище; (2) любой другой поток в этот момент ждёт мьютекс
// журнала, а его держит тот, кто застрял в write(). Симптомы совпадают с
// жалобой «чит в один момент полностью завис»: в логе последний пульс есть, а
// дальше ничего — ни «выход», ни «ПАДЕНИЕ», ни следующих пульсов.
//
// Поэтому запись теперь АСИНХРОННАЯ: тот, кто зовёт diag_log(), только копирует
// готовую строку в кольцевой буфер и идёт дальше — без диска, без ожидания
// мьютекса (мьютекс берётся trylock'ом: не взяли — событие отброшено и посчитано).
// Файл пишет отдельный поток. Он может встать на внешнем хранилище сколько
// угодно: поток кадра этого уже не почувствует.
//
// Отброшенные события считаются, и счётчик попадает в следующую строку: видно,
// что журнал «подтормаживал». Из обработчика падения файл по-прежнему пишется
// напрямую (write без мьютекса) — там ждать нельзя, да и терять строку нельзя.
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
constexpr size_t kQueueSize = 64 * 1024;    // кольцевой буфер: ~200 строк (события, которым не хватило места, считаются)

int    g_fd = -1;
char   g_path[256] = {};
bool   g_enabled = false;
double g_start = 0.0;

// Очередь событий (одна строка = одна запись, поэтому кольцо и хвост с длиной).
pthread_mutex_t g_queue_lock = PTHREAD_MUTEX_INITIALIZER;
char   g_queue[kQueueSize];
size_t g_qhead = 0;              // откуда читает поток записи
size_t g_qtail = 0;              // куда пишут зовущие (под мьютексом)
unsigned long long g_dropped = 0;

// Схлопывание повторов: держим последнюю строку и счётчик подавленных.
char   g_last[224] = {};   // последнее событие (для схлопывания повторов)
int    g_repeat = 0;

pthread_t g_writer = 0;

double now_seconds() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// Сколько байт свободно в кольце. Читается под мьютексом.
size_t queue_used_locked() {
    return (g_qtail + kQueueSize - g_qhead) % kQueueSize;
}

// Положить готовую строку (без перевода строки) в очередь.
// Зовётся из любого потока. Мьютекс здесь держится ТОЛЬКО на копирование
// строки: диск под ним не трогается, поэтому ждать его безопасно и правильно
// (попытка «не ждать» через trylock роняла события на ровном месте — терялись
// обычные строки при живой нагрузке). Единственный случай, когда событие
// теряется, — очередь действительно переполнена; тогда оно считается.
void queue_line(const char* text) {
    if (g_fd < 0 || !text || !*text) return;
    const size_t len = strlen(text);
    if (len == 0 || len > kLineMax) return;
    pthread_mutex_lock(&g_queue_lock);
    const size_t used = queue_used_locked();
    if (used + len + 2 > kQueueSize) {   // +2: перевод строки и запас на кольцо
        ++g_dropped;
        pthread_mutex_unlock(&g_queue_lock);
        return;
    }
    const size_t part1 = kQueueSize - g_qtail;
    if (len <= part1) {
        memcpy(g_queue + g_qtail, text, len);
    } else {
        memcpy(g_queue + g_qtail, text, part1);
        memcpy(g_queue, text + part1, len - part1);
    }
    g_qtail = (g_qtail + len) % kQueueSize;
    g_queue[g_qtail] = '\n';
    g_qtail = (g_qtail + 1) % kQueueSize;
    pthread_mutex_unlock(&g_queue_lock);
}

// Забрать одну строку из очереди. false — очередь пуста.
bool pop_line(char* out, size_t out_size) {
    pthread_mutex_lock(&g_queue_lock);
    if (g_qhead == g_qtail) {
        pthread_mutex_unlock(&g_queue_lock);
        return false;
    }
    size_t n = 0;
    while (g_qhead != g_qtail && n + 1 < out_size) {
        const char c = g_queue[g_qhead];
        g_qhead = (g_qhead + 1) % kQueueSize;
        if (c == '\n') break;
        out[n++] = c;
    }
    out[n] = 0;
    pthread_mutex_unlock(&g_queue_lock);
    return true;
}

// Ротация: журнал дорос до предела — прошлый уходит в .1.
void rotate_if_needed() {
    struct stat st{};
    if (stat(g_path, &st) == 0 && st.st_size > kFileLimit) {
        char old[280];
        snprintf(old, sizeof(old), "%s.1", g_path);
        remove(old);
        if (g_fd >= 0) close(g_fd);
        rename(g_path, old);
        g_fd = open(g_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    }
}

// Поток записи: берёт строки из очереди и пишет в файл. Единственный, кто
// трогает диск, поэтому его тормоза никого больше не касаются.
void* writer_thread(void*) {
    char line[kLineMax + 8];
    int idle_ticks = 0;
    for (;;) {
        if (!pop_line(line, sizeof(line))) {
            // Пусто: спим коротко. Пустой журнал почти не просыпается, а
            // всплеск событий разбирается пачкой без задержек.
            struct timespec req{};
            req.tv_nsec = (++idle_ticks > 20 ? 40 : 2) * 1000000L;
            nanosleep(&req, nullptr);
            continue;
        }
        idle_ticks = 0;
        pthread_mutex_lock(&g_queue_lock);
        const unsigned long long lost = g_dropped;
        g_dropped = 0;
        pthread_mutex_unlock(&g_queue_lock);
        if (lost > 0) {
            char note[96];
            snprintf(note, sizeof(note), "%8.1f %-7s (потеряно событий: %llu)\n",
                     now_seconds() - g_start, "health", lost);
            if (g_fd >= 0) {
                ssize_t w = write(g_fd, note, strlen(note));
                (void)w;
            }
        }
        char text[kLineMax + 12];
        snprintf(text, sizeof(text), "%s\n", line);
        if (g_fd >= 0) {
            ssize_t w = write(g_fd, text, strlen(text));
            (void)w;
        }
        rotate_if_needed();
    }
    return nullptr;
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
    // Каталог может прийти и без косой черты на конце — путь склеиваем с
    // проверкой, иначе журнал лёг бы рядом с каталогом («benzhackxvcen_health.log»).
    const size_t dir_len = strlen(dir);
    snprintf(g_path, sizeof(g_path), "%s%s%s", dir,
             (dir_len > 0 && dir[dir_len - 1] == '/') ? "" : "/", "xvcen_health.log");
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

    // Поток записи. Отдельный именно потому, что диск на этом пути может
    // задуматься, а зовущие diag_log() — это поток кадра, поток привязки и
    // одноразовые потоки тача: никто из них ждать не должен.
    pthread_create(&g_writer, nullptr, writer_thread, nullptr);

    char head[kLineMax];
    snprintf(head, sizeof(head), "%8.1f %-7s журнал открыт: %s", 0.0, "health", g_path);
    queue_line(head);
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

    // Повтор того же события — не строка, а счётчик. Схлопывание делаем здесь,
    // в зовущем потоке: диску достаётся уже готовое решение, а не поток строк.
    if (g_last[0] && strcmp(g_last, text) == 0) {
        __atomic_add_fetch(&g_repeat, 1, __ATOMIC_RELAXED);
        return;
    }
    snprintf(g_last, sizeof(g_last), "%s", text);
    const int suppressed = __atomic_exchange_n(&g_repeat, 0, __ATOMIC_RELAXED);

    char line[kLineMax];
    if (suppressed > 0) {
        char tail[224];
        snprintf(tail, sizeof(tail), "%8.1f %-7s (предыдущее повторилось ×%d)",
                 now_seconds() - g_start, "repeat", suppressed);
        queue_line(tail);
    }
    snprintf(line, sizeof(line), "%8.1f %-7s %s", now_seconds() - g_start, tag, text);
    queue_line(line);
}

void diag_flush(int wait_ms) {
    if (g_fd < 0) return;
    // Выход: даём потоку записи время разобрать очередь. Ждём не бесконечно —
    // если хранилище уже стоит, уйти всё равно надо.
    struct timespec req{};
    req.tv_nsec = 20 * 1000000L;
    int waited = 0;
    while (waited < wait_ms) {
        pthread_mutex_lock(&g_queue_lock);
        const bool empty = (g_qhead == g_qtail);
        pthread_mutex_unlock(&g_queue_lock);
        if (empty) return;
        nanosleep(&req, nullptr);
        waited += 20;
    }
}
