#include "logfile.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <thread>
#include <time.h>
#include <unistd.h>

namespace {

constexpr const char* kPrimaryDir = "/storage/emulated/0/Download";
constexpr const char* kPrimaryPath = "/storage/emulated/0/Download/benzware.log";
constexpr const char* kFallbackDir = "/storage/emulated/0/benzware";
constexpr const char* kFallbackPath = "/storage/emulated/0/benzware/benzware.log";
constexpr long kMaxLogBytes = 16L * 1024L * 1024L;

std::mutex g_log_mutex;
std::condition_variable g_log_cv;
FILE* g_log_file = nullptr;
std::string g_log_path;
std::deque<std::string> g_log_queue;
bool g_writer_run = false;
unsigned g_log_dropped = 0;
std::thread g_writer;
int g_crash_fd = -1;
volatile sig_atomic_t g_handling_crash = 0;
alignas(16) unsigned char g_crash_stack[64 * 1024];
std::atomic<int> g_stage{kStageNone};
std::atomic<unsigned long> g_frame{0};
std::atomic<long long> g_beat_ns{0};
std::atomic<bool> g_watchdog_run{false};
std::thread g_watchdog;

// Заметка для crash_handler. Писатель один — render-поток; читатель —
// обработчик сигнала, возможно на том же потоке. Мьютекса нет намеренно: в
// обработчике он мог быть уже взят мёртвым потоком.
static char s_crash_note[96] = {};
static std::atomic<bool> s_crash_note_valid{false};

long long monotonic_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

const char* stage_name(int stage) {
    switch (stage) {
        case kStageFrameBegin: return "frame-begin";
        case kStageEsp: return "esp";
        case kStageAimSelect: return "aim-select";
        case kStageAimRead: return "aim-read";
        case kStageAimWrite: return "aim-write";
        case kStageAimTouch: return "aim-touch";
        case kStageFarm: return "farm";
        case kStageMenu: return "menu";
        case kStageHud: return "hud";
        case kStageMarkers: return "markers";
        case kStageFreecam: return "freecam";
        case kStageFrameEnd: return "frame-end";
        default: return "none";
    }
}

void write_line_locked(const char* text) {
    if (!g_log_file || !text) return;
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    tm local{};
    localtime_r(&ts.tv_sec, &local);
    const long tid = (long)syscall(SYS_gettid);
    std::fprintf(g_log_file, "%02d:%02d:%02d.%03ld [t%ld] %s\n",
                 local.tm_hour, local.tm_min, local.tm_sec,
                 ts.tv_nsec / 1000000L, tid, text);
}

void crash_handler(int signal_number, siginfo_t* info, void* context) {
    if (g_handling_crash) _exit(128 + signal_number);
    g_handling_crash = 1;

    uintptr_t pc = 0;
#if defined(__aarch64__)
    if (context) pc = (uintptr_t)((ucontext_t*)context)->uc_mcontext.pc;
#elif defined(__x86_64__) && defined(REG_RIP)
    if (context) pc = (uintptr_t)((ucontext_t*)context)->uc_mcontext.gregs[REG_RIP];
#endif
    char message[512];
    const long long age_ns = monotonic_ns() - g_beat_ns.load(std::memory_order_relaxed);
    const int length = std::snprintf(
        message, sizeof(message),
        "\nCRASH: signal=%d code=%d fault=0x%llx pc=0x%llx frame=%lu stage=%s(%d) beat-age=%.3fs note=%s\n",
        signal_number, info ? info->si_code : 0,
        (unsigned long long)(uintptr_t)(info ? info->si_addr : nullptr),
        (unsigned long long)pc, g_frame.load(std::memory_order_relaxed),
        stage_name(g_stage.load(std::memory_order_relaxed)),
        g_stage.load(std::memory_order_relaxed), (double)age_ns / 1e9,
        s_crash_note_valid.load(std::memory_order_acquire) ? s_crash_note : "-");
    if (g_crash_fd >= 0 && length > 0) {
        write(g_crash_fd, message, (size_t)length);
        fsync(g_crash_fd);
    }

    signal(signal_number, SIG_DFL);
#if defined(SYS_tgkill)
    syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), signal_number);
#endif
    _exit(128 + signal_number);
}

void crash_note(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vsnprintf(s_crash_note, sizeof(s_crash_note), format, args);
    va_end(args);
    s_crash_note_valid.store(true, std::memory_order_release);
}

void install_crash_handlers() {
    stack_t stack{};
    stack.ss_sp = g_crash_stack;
    stack.ss_size = sizeof(g_crash_stack);
    sigaltstack(&stack, nullptr);

    struct sigaction action{};
    action.sa_sigaction = crash_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    const int signals[] = {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE};
    for (int value : signals) sigaction(value, &action, nullptr);
}

void writer_loop() {
    std::unique_lock<std::mutex> lock(g_log_mutex);
    while (g_writer_run || !g_log_queue.empty()) {
        g_log_cv.wait_for(lock, std::chrono::milliseconds(250), [] {
            return !g_writer_run || !g_log_queue.empty();
        });
        std::vector<std::string> batch;
        batch.reserve(g_log_queue.size() + (g_log_dropped ? 1 : 0));
        while (!g_log_queue.empty()) {
            batch.push_back(std::move(g_log_queue.front()));
            g_log_queue.pop_front();
        }
        const unsigned dropped = g_log_dropped;
        g_log_dropped = 0;
        lock.unlock();
        for (const std::string& line : batch) write_line_locked(line.c_str());
        if (dropped) {
            char message[96];
            std::snprintf(message, sizeof(message), "лог: отброшено %u строк из-за переполнения очереди", dropped);
            write_line_locked(message);
        }
        if (g_log_file && (!batch.empty() || dropped)) std::fflush(g_log_file);
        lock.lock();
        if (g_log_file && std::ftell(g_log_file) >= kMaxLogBytes) {
            std::fclose(g_log_file);
            g_log_file = nullptr;
            if (g_crash_fd >= 0) { close(g_crash_fd); g_crash_fd = -1; }
            const std::string old_path = g_log_path + ".old";
            unlink(old_path.c_str());
            rename(g_log_path.c_str(), old_path.c_str());
            g_log_file = std::fopen(g_log_path.c_str(), "w");
            if (g_log_file) {
                chmod(g_log_path.c_str(), 0666);
                setvbuf(g_log_file, nullptr, _IOFBF, 64 * 1024);
                write_line_locked("лог продолжен после ротации 16 МБ; предыдущая часть в benzware.log.old");
                std::fflush(g_log_file);
                g_crash_fd = open(g_log_path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
            }
        }
    }
}

} // namespace

void LogOpen() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_file) return;

    mkdir(kPrimaryDir, 0777);
    g_log_file = std::fopen(kPrimaryPath, "w");
    if (g_log_file) {
        g_log_path = kPrimaryPath;
    } else {
        mkdir(kFallbackDir, 0777);
        g_log_file = std::fopen(kFallbackPath, "w");
        if (g_log_file) g_log_path = kFallbackPath;
    }
    if (!g_log_file) return;

    chmod(g_log_path.c_str(), 0666);
    setvbuf(g_log_file, nullptr, _IOFBF, 64 * 1024);
    g_beat_ns.store(monotonic_ns(), std::memory_order_relaxed);
    write_line_locked("лог открыт");
    write_line_locked((std::string("файл: ") + g_log_path).c_str());
    std::fflush(g_log_file);
    g_crash_fd = open(g_log_path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    install_crash_handlers();
    g_writer_run = true;
    g_writer = std::thread(writer_loop);
}

void LogClose() {
    g_watchdog_run.store(false, std::memory_order_release);
    if (g_watchdog.joinable()) g_watchdog.join();

    LogLine("лог закрыт");
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        g_writer_run = false;
    }
    g_log_cv.notify_all();
    if (g_writer.joinable()) g_writer.join();

    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (!g_log_file) return;
    std::fflush(g_log_file);
    std::fclose(g_log_file);
    g_log_file = nullptr;
    if (g_crash_fd >= 0) {
        close(g_crash_fd);
        g_crash_fd = -1;
    }
    g_log_path.clear();
    g_log_queue.clear();
}

void LogLine(const char* format, ...) {
    if (!format) return;
    char message[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        if (!g_log_file || !g_writer_run) return;
        if (g_log_queue.size() >= 4096) {
            ++g_log_dropped;
            return;
        }
        g_log_queue.emplace_back(message);
    }
    g_log_cv.notify_one();
}

void LogStage(int stage) {
    if (stage < kStageNone || stage >= kStageCount) stage = kStageNone;
    g_stage.store(stage, std::memory_order_relaxed);
}

void LogFrameBeat(unsigned long frame) {
    g_frame.store(frame, std::memory_order_relaxed);
    g_beat_ns.store(monotonic_ns(), std::memory_order_release);
}

void LogWatchdogStart() {
    bool expected = false;
    if (!g_watchdog_run.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    g_beat_ns.store(monotonic_ns(), std::memory_order_release);
    g_watchdog = std::thread([] {
        long long last_report_ns = 0;
        while (g_watchdog_run.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (!g_watchdog_run.load(std::memory_order_acquire)) break;
            const long long now_ns = monotonic_ns();
            const long long age_ns = now_ns - g_beat_ns.load(std::memory_order_acquire);
            if (age_ns >= 2000000000LL &&
                (last_report_ns == 0 || now_ns - last_report_ns >= 2000000000LL)) {
                LogLine("WATCHDOG: кадр %lu не завершён %.1fс, стадия=%s(%d)",
                        g_frame.load(std::memory_order_relaxed),
                        (double)age_ns / 1e9,
                        stage_name(g_stage.load(std::memory_order_relaxed)),
                        g_stage.load(std::memory_order_relaxed));
                last_report_ns = now_ns;
            } else if (age_ns < 2000000000LL) {
                last_report_ns = 0;
            }
        }
    });
}
