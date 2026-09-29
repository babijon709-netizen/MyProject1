#include "memory_backend.h"
#include "ftdriver_abi.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace memory {
namespace {

std::atomic<int> g_mode{static_cast<int>(AccessMode::External)};

const char* env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return value && value[0] ? value : "";
}

std::string kernel_release() {
    struct utsname uts{};
    if (uname(&uts) != 0) return {};
    std::string release(uts.release);
    const size_t dash = release.find('-');
    if (dash != std::string::npos) release.resize(dash);
    return release;
}

bool path_readable(const char* path) {
    return path && path[0] && access(path, R_OK | W_OK) == 0;
}

const char* const kDeviceNames[] = {
    "/dev/ftdriver",
    "/dev/FTDriver",
    "/dev/ft_mem",
    "/dev/ftmem",
    "/dev/kernel_mem",
    "/dev/memory_driver",
};

std::string find_device() {
    const char* configured = env_or_empty("MEMIO_KERNEL_DEVICE");
    if (configured[0] && path_readable(configured)) return configured;
    for (const char* path : kDeviceNames) {
        if (path_readable(path)) return path;
    }
    return {};
}

const char* shell_path() {
    if (access("/system/bin/sh", X_OK) == 0) return "/system/bin/sh";
    return "/bin/sh";
}

const char* script_name_for_release(const std::string& release, int index) {
    // The payload set is intentionally explicit. A driver compiled for a
    // different kernel must never be selected just because its prefix looks
    // close enough; the generic major.minor script is only the payloads that
    // are actually present in the repository.
    static const char* const names[] = {
        "4.9.186.sh", "4.14.117.sh", "4.14.141.sh", "4.14.141b.sh",
        "4.14.180.sh", "4.14.180b.sh", "4.14.186.sh", "4.14.186b.sh",
        "4.14.186c.sh", "4.19.81.sh", "4.19.109.sh", "4.19.113.sh",
        "4.19.113b.sh", "4.19.157.sh", "4.19.157b.sh", "4.19.191.sh",
        "4.19.191b-ColorOS.sh", "4.19.191-ColorOS.sh", "5.4.61.sh",
        "5.4.86.sh", "5.4.147.sh", "5.4.191oneplus.sh",
        "5.4.210-ColorOS.sh", "5.4.233.sh", "5.4.233oneplus.sh",
        "5.4.233-ColorOS.sh", "5.10.136.sh",
        "5.10.sh", "5.15.sh", "6.1.sh", "6.6.sh", "6.12.sh",
        "oppo4.19.81.sh", "oppoa92s4.14.186.sh",
    };
    if (index < 0 || index >= static_cast<int>(sizeof(names) / sizeof(names[0]))) return nullptr;
    const char* name = names[index];
    const std::string exact = release + ".sh";
    if (exact == name) return name;
    return nullptr;
}

std::string find_driver_script(const std::string& release) {
    const char* configured_dir = env_or_empty("FTDRIVER_DIR");
    const char* dirs[] = {
        configured_dir,
        "/data/local/tmp/FTDriver",
        "/data/local/tmp/ftdriver",
        "/sdcard/FTDriver",
    };

    // Prefer an exact kernel build payload.
    for (const char* dir : dirs) {
        if (!dir || !dir[0]) continue;
        for (int i = 0; ; ++i) {
            const char* name = script_name_for_release(release, i);
            if (!name) {
                if (i >= 40) break;
                continue;
            }
            std::string path = std::string(dir) + "/" + name;
            if (access(path.c_str(), R_OK) == 0) return path;
        }
    }

    // Generic payloads are explicitly named in the supplied set. Select one
    // only for an exact major.minor release, never for a different family.
    const size_t dot = release.find('.');
    const size_t dot2 = dot == std::string::npos ? std::string::npos : release.find('.', dot + 1);
    if (dot == std::string::npos || dot2 == std::string::npos) return {};
    const std::string family = release.substr(0, dot2);
    const std::string generic = family + ".sh";
    const char* dirs2[] = {configured_dir, "/data/local/tmp/FTDriver", "/data/local/tmp/ftdriver", "/sdcard/FTDriver"};
    for (const char* dir : dirs2) {
        if (!dir || !dir[0]) continue;
        const std::string path = std::string(dir) + "/" + generic;
        if (access(path.c_str(), R_OK) == 0) return path;
    }
    return {};
}

void set_error(std::string* out, const std::string& value) {
    if (out) *out = value;
}

} // namespace

void SetAccessMode(AccessMode mode) {
    g_mode.store(static_cast<int>(mode), std::memory_order_release);
}

AccessMode GetAccessMode() {
    return g_mode.load(std::memory_order_acquire) == static_cast<int>(AccessMode::Kernel)
               ? AccessMode::Kernel : AccessMode::External;
}

const char* AccessModeName(AccessMode mode) {
    return mode == AccessMode::Kernel ? "Ядро" : "External";
}

KernelDriverManager& KernelDriverManager::Instance() {
    static KernelDriverManager manager;
    return manager;
}

bool KernelDriverManager::Start(std::string* error) {
    last_error_.clear();
    if (running_ && !device_path_.empty() && path_readable(device_path_.c_str())) return true;

    if (child_pid_ > 0) {
        int status = 0;
        if (waitpid(child_pid_, &status, WNOHANG) == child_pid_) child_pid_ = -1;
    }

    device_path_ = find_device();
    if (device_path_.empty()) {
        const std::string release = kernel_release();
        selected_script_ = find_driver_script(release);
        if (selected_script_.empty()) {
            last_error_ = "Не найден драйвер для ядра " + (release.empty() ? "unknown" : release) +
                          ". Укажи FTDRIVER_DIR или запусти драйвер отдельно.";
            set_error(error, last_error_);
            running_ = false;
            return false;
        }

        const pid_t child = fork();
        if (child < 0) {
            last_error_ = std::string("fork: ") + std::strerror(errno);
            set_error(error, last_error_);
            return false;
        }
        if (child == 0) {
            execl(shell_path(), shell_path(), selected_script_.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        child_pid_ = static_cast<int>(child);

        // A launcher can need a short moment to create its device node. Do not
        // keep the app in kernel mode if the selected payload exits or exposes
        // no endpoint; the caller can then choose External explicitly.
        for (int i = 0; i < 150; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            device_path_ = find_device();
            int status = 0;
            if (waitpid(child_pid_, &status, WNOHANG) == child_pid_) {
                child_pid_ = -1;
                break;
            }
            if (!device_path_.empty()) break;
        }
    }

    if (device_path_.empty()) {
        last_error_ = "Драйвер запущен, но устройство FTDriver не появилось";
        set_error(error, last_error_);
        Stop();
        return false;
    }
    running_ = true;
    return true;
}

void KernelDriverManager::Stop() {
    running_ = false;
    device_path_.clear();
    if (child_pid_ > 0) {
        kill(static_cast<pid_t>(child_pid_), SIGTERM);
        int status = 0;
        for (int i = 0; i < 50; ++i) {
            if (waitpid(static_cast<pid_t>(child_pid_), &status, WNOHANG) == child_pid_) {
                child_pid_ = -1;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (child_pid_ > 0) {
            kill(static_cast<pid_t>(child_pid_), SIGKILL);
            waitpid(static_cast<pid_t>(child_pid_), &status, 0);
            child_pid_ = -1;
        }
    }
}

bool KernelDriverManager::Running() const { return running_; }
const char* KernelDriverManager::DevicePath() const { return device_path_.c_str(); }
const char* KernelDriverManager::SelectedScript() const { return selected_script_.c_str(); }
const char* KernelDriverManager::LastError() const { return last_error_.c_str(); }

namespace detail {
namespace {

class ExternalTransport final : public Transport {
public:
    bool Bind(pid_t pid) override {
        Unbind();
        if (pid <= 0) { last_error_ = EINVAL; return false; }
        char path[48];
        std::snprintf(path, sizeof(path), "/proc/%d/mem", static_cast<int>(pid));
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) { last_error_ = errno; return false; }
        pid_.store(static_cast<int>(pid), std::memory_order_release);
        fd_.store(fd, std::memory_order_release);
        return true;
    }

    void Unbind() override {
        const int fd = fd_.exchange(-1, std::memory_order_acq_rel);
        if (fd >= 0) close(fd);
        pid_.store(-1, std::memory_order_release);
    }

    bool Bound() const override { return fd_.load(std::memory_order_acquire) >= 0 && pid_.load(std::memory_order_acquire) > 0; }
    ssize_t Read(uint64_t address, void* out, size_t size) override {
        const int fd = fd_.load(std::memory_order_acquire);
        if (fd < 0) { last_error_ = EBADF; return -1; }
        const ssize_t result = pread(fd, out, size, static_cast<off_t>(address));
        if (result < 0) last_error_ = errno;
        return result;
    }
    ssize_t Write(uint64_t address, const void* in, size_t size) override {
        const int fd = fd_.load(std::memory_order_acquire);
        if (fd < 0) { last_error_ = EBADF; return -1; }
        const ssize_t result = pwrite(fd, in, size, static_cast<off_t>(address));
        if (result < 0) last_error_ = errno;
        return result;
    }
    const char* Name() const override { return "/proc/<pid>/mem"; }
    int LastError() const override { return last_error_; }

private:
    std::atomic<int> fd_{-1};
    std::atomic<int> pid_{-1};
    std::atomic<int> last_error_{0};
};

// The request carries a user pointer because the driver copies directly to or
// from the caller buffer; no process address is ever dereferenced in the app.
class KernelTransport final : public Transport {
public:
    bool Bind(pid_t pid) override {
        Unbind();
        if (pid <= 0) { last_error_ = EINVAL; return false; }
        const char* configured = env_or_empty("MEMIO_KERNEL_DEVICE");
        std::string path = configured[0] ? configured : KernelDriverManager::Instance().DevicePath();
        if (path.empty()) path = find_device();
        if (path.empty()) { last_error_ = ENODEV; return false; }
        const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) { last_error_ = errno; return false; }
        fd_.store(fd, std::memory_order_release);
        pid_.store(static_cast<int>(pid), std::memory_order_release);
        int32_t target = static_cast<int32_t>(pid);
        if (ioctl(fd, FTDRIVER_IOCTL_ATTACH, &target) < 0 && errno != ENOTTY && errno != EINVAL) {
            last_error_ = errno;
            Unbind();
            return false;
        }
        path_ = path;
        return true;
    }

    void Unbind() override {
        const int fd = fd_.exchange(-1, std::memory_order_acq_rel);
        const pid_t pid = static_cast<pid_t>(pid_.load(std::memory_order_acquire));
        if (fd >= 0) {
            if (pid > 0) {
                int32_t target = static_cast<int32_t>(pid);
                ioctl(fd, FTDRIVER_IOCTL_DETACH, &target);
            }
            close(fd);
        }
        pid_.store(-1, std::memory_order_release);
        path_.clear();
    }

    bool Bound() const override { return fd_.load(std::memory_order_acquire) >= 0 && pid_.load(std::memory_order_acquire) > 0; }

    ssize_t Read(uint64_t address, void* out, size_t size) override {
        return Request(FTDRIVER_IOCTL_READ, address, out, size, false);
    }
    ssize_t Write(uint64_t address, const void* in, size_t size) override {
        return Request(FTDRIVER_IOCTL_WRITE, address, const_cast<void*>(in), size, true);
    }
    const char* Name() const override { return "FTDriver kernel ioctl"; }
    int LastError() const override { return last_error_; }

private:
    ssize_t Request(unsigned long command, uint64_t address, void* buffer, size_t size, bool write) {
        (void)write;
        const int fd = fd_.load(std::memory_order_acquire);
        const int pid = pid_.load(std::memory_order_acquire);
        if (fd < 0 || pid <= 0) { last_error_ = EBADF; return -1; }
        FTDriverMemoryRequest request{};
        request.pid = static_cast<int32_t>(pid);
        request.address = address;
        request.buffer = reinterpret_cast<uint64_t>(buffer);
        request.size = static_cast<uint64_t>(size);
        if (ioctl(fd, command, &request) < 0) {
            last_error_ = errno;
            return -1;
        }
        if (request.size > size) { last_error_ = EOVERFLOW; return -1; }
        return static_cast<ssize_t>(request.size ? request.size : size);
    }

    std::atomic<int> fd_{-1};
    std::atomic<int> pid_{-1};
    std::string path_;
    std::atomic<int> last_error_{0};
};

} // namespace

std::shared_ptr<Transport> CreateTransport(AccessMode mode) {
    if (mode == AccessMode::Kernel) return std::make_shared<KernelTransport>();
    return std::make_shared<ExternalTransport>();
}

} // namespace detail
} // namespace memory
