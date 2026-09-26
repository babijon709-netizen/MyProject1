#pragma once

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <cerrno>
#include <cstring>

enum LogStageId {
    kStageNone = 0,
    kStageFrameBegin,
    kStageEsp,
    kStageAimSelect,
    kStageAimRead,
    kStageAimWrite,
    kStageAimTouch,
    kStageFarm,
    kStageMenu,
    kStageFreecam,
    kStageFrameEnd,
    kStageCount
};

// Device builds write diagnostics to shared storage so they can be retrieved
// without adb/logcat. Host-only tests keep logging disabled unless explicitly
// enabled by tools/logfile_test.sh.
#if defined(__ANDROID__) || defined(BENZWARE_ENABLE_FILE_LOGGING)
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace logfile_detail {
inline constexpr std::size_t kMaxLine = 3072;
inline constexpr off_t kMaxFileBytes = 2 * 1024 * 1024;

inline std::mutex& mutex() {
    static std::mutex value;
    return value;
}
inline int& fd() {
    static int value = -1;
    return value;
}
inline int& fd_mirror() {
    static int value = -1;
    return value;
}
inline bool& attempted() {
    static bool value = false;
    return value;
}
inline int& last_error() {
    static int value = 0;
    return value;
}
inline char* active_path() {
    static char value[160] = {};
    return value;
}
#ifdef BENZWARE_LOG_TESTING
inline char* test_path() {
    static char value[160] = {};
    return value;
}
#endif

inline bool write_all(int out, const char* data, std::size_t size) {
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n = ::write(out, data + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

inline void write_text_locked(const char* text) {
    const int out = fd();
    if (out < 0 || !text) return;

    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    std::tm local{};
    const bool have_time = ::localtime_r(&ts.tv_sec, &local) != nullptr;

    char line[kMaxLine + 128] = {};
    int prefix = 0;
    if (have_time) {
        prefix = std::snprintf(line, sizeof(line),
            "%04d-%02d-%02d %02d:%02d:%02d.%03ld  ",
            local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
            local.tm_hour, local.tm_min, local.tm_sec,
            ts.tv_nsec / 1000000L);
    }
    if (prefix < 0 || static_cast<std::size_t>(prefix) >= sizeof(line)) return;

    const std::size_t room = sizeof(line) - static_cast<std::size_t>(prefix) - 2;
    const std::size_t text_len = strnlen(text, room);
    std::memcpy(line + prefix, text, text_len);
    std::size_t total = static_cast<std::size_t>(prefix) + text_len;
    if (total == 0 || line[total - 1] != '\n') line[total++] = '\n';

    struct stat st{};
    if (::fstat(out, &st) == 0 && st.st_size + static_cast<off_t>(total) > kMaxFileBytes) {
        // Keep the current session's log bounded. LogOpen() truncates the file
        // on every launch, so no valuable previous-session data is lost here.
        if (::ftruncate(out, 0) == 0) {
            static constexpr char marker[] = "--- log rotated after 2 MiB ---\n";
            write_all(out, marker, sizeof(marker) - 1);
        }
    }
    write_all(out, line, total);
    ::fsync(out);

    const int m = fd_mirror();
    if (m >= 0) {
        struct stat mst{};
        if (::fstat(m, &mst) == 0 && mst.st_size + static_cast<off_t>(total) > kMaxFileBytes) {
            if (::ftruncate(m, 0) == 0) {
                static constexpr char marker[] = "--- log rotated after 2 MiB ---\n";
                write_all(m, marker, sizeof(marker) - 1);
            }
        }
        write_all(m, line, total);
        ::fsync(m);
    }
}

inline bool open_path_locked(const char* path, int flags) {
    if (!path || !path[0]) return false;
    const int opened = ::open(path, flags | O_CLOEXEC, 0666);
    if (opened < 0) {
        last_error() = errno;
        return false;
    }
    ::fchmod(opened, 0644);
    fd() = opened;
    std::snprintf(active_path(), 160, "%s", path);
    return true;
}

inline bool open_locked(bool truncate) {
    if (fd() >= 0) return true;
    last_error() = 0;
    const int flags = O_WRONLY | O_CREAT | (truncate ? O_TRUNC : O_APPEND);

#ifdef BENZWARE_LOG_TESTING
    if (test_path()[0]) return open_path_locked(test_path(), flags);
#endif

    // Ensure common folders exist before trying to create log files
    ::mkdir("/storage/emulated/0", 0777);
    ::mkdir("/storage/emulated/0/Download", 0777);
    ::mkdir("/sdcard/Download", 0777);
    ::mkdir("/storage/emulated/0/benzware", 0777);

    // Prefer Android's public Downloads folder. The aliases cover devices where
    // /sdcard is mounted separately; config storage is a visible fallback.
    static const char* const paths[] = {
        "/storage/emulated/0/Download/benzware.log",
        "/sdcard/Download/benzware.log",
        "/storage/emulated/0/Download/benzware_offsets.log",
        "/sdcard/Download/benzware_offsets.log",
        "/storage/emulated/0/benzware/benzware.log",
        "/data/local/tmp/benzware.log",
    };
    bool ok = false;
    for (const char* path : paths) {
        if (open_path_locked(path, flags)) {
            ok = true;
            break;
        }
    }

    if (ok) {
        attempted() = true;
        if (fd_mirror() < 0) {
            static const char* const mirror_paths[] = {
                "/storage/emulated/0/Download/benzware_offsets.log",
                "/sdcard/Download/benzware_offsets.log",
            };
            for (const char* mp : mirror_paths) {
                if (std::strcmp(mp, active_path()) == 0) continue;
                const int mfd = ::open(mp, flags | O_CLOEXEC, 0666);
                if (mfd >= 0) {
                    ::fchmod(mfd, 0644);
                    fd_mirror() = mfd;
                    break;
                }
            }
        }
    }
    return ok;
}
}  // namespace logfile_detail

inline bool LogOpen() {
    std::lock_guard<std::mutex> guard(logfile_detail::mutex());
    if (!logfile_detail::open_locked(true)) {
        std::fprintf(stderr, "benzware: cannot open diagnostic log (errno=%d: %s)\n",
                     logfile_detail::last_error(), std::strerror(logfile_detail::last_error()));
        return false;
    }
    logfile_detail::write_text_locked("=== benzware diagnostic session started ===");
    return true;
}

inline void LogClose() {
    std::lock_guard<std::mutex> guard(logfile_detail::mutex());
    if (logfile_detail::fd() >= 0) {
        logfile_detail::write_text_locked("=== benzware diagnostic session ended ===");
        ::fsync(logfile_detail::fd());
        ::close(logfile_detail::fd());
        logfile_detail::fd() = -1;
    }
    if (logfile_detail::fd_mirror() >= 0) {
        ::fsync(logfile_detail::fd_mirror());
        ::close(logfile_detail::fd_mirror());
        logfile_detail::fd_mirror() = -1;
    }
}

inline const char* LogPath() {
    return logfile_detail::active_path()[0] ? logfile_detail::active_path() : "unavailable";
}

inline int LogLastError() {
    return logfile_detail::last_error();
}

#ifdef BENZWARE_LOG_TESTING
inline void LogSetPathForTesting(const char* path) {
    std::lock_guard<std::mutex> guard(logfile_detail::mutex());
    if (logfile_detail::fd() >= 0) {
        ::close(logfile_detail::fd());
        logfile_detail::fd() = -1;
    }
    logfile_detail::attempted() = false;
    logfile_detail::last_error() = 0;
    std::snprintf(logfile_detail::test_path(), 160, "%s", path ? path : "");
}
#endif

inline void LogLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline void LogLine(const char* fmt, ...) {
    if (!fmt) return;
    char message[logfile_detail::kMaxLine] = {};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    std::lock_guard<std::mutex> guard(logfile_detail::mutex());
    if (logfile_detail::fd() < 0) {
        static timespec last_retry{};
        timespec now{};
        ::clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - last_retry.tv_sec >= 1) {
            last_retry = now;
            logfile_detail::open_locked(false);
        }
        if (logfile_detail::fd() < 0) return;
    }
    logfile_detail::write_text_locked(message);
}
#else
inline bool LogOpen() { return false; }
inline void LogClose() {}
inline const char* LogPath() { return "disabled (host build)"; }
inline int LogLastError() { return 0; }
inline void LogStage(int) {}
inline void LogFrameBeat(unsigned long) {}
inline void LogWatchdogStart() {}
inline void LogLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline void LogLine(const char*, ...) {}
#endif

#if defined(__ANDROID__) || defined(BENZWARE_ENABLE_FILE_LOGGING)
inline void LogStage(int) {}
inline void LogFrameBeat(unsigned long) {}
inline void LogWatchdogStart() {}
#endif
