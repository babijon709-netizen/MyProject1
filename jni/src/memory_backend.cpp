#include "memory_backend.h"
#include "ftdriver_abi.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace memaccess {
namespace {

std::atomic<int> g_mode{static_cast<int>(AccessMode::External)};

const char* env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return value && value[0] ? value : "";
}


bool path_readable(const char* path) {
    return path && path[0] && access(path, R_OK | W_OK) == 0;
}

bool path_executable(const char* path) {
    return path && path[0] && access(path, R_OK | X_OK) == 0;
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

std::string kernel_release() {
    struct utsname uts{};
    if (uname(&uts) != 0) return {};
    return uts.release;
}

std::string find_driver_script(const std::string& release) {
    const char* explicit_payload = env_or_empty("FTDRIVER_PAYLOAD");
    if (explicit_payload[0] && path_executable(explicit_payload)) return explicit_payload;

    const char* configured_dir = env_or_empty("FTDRIVER_DIR");
    const char* dirs[] = {
        configured_dir,
        "/data/local/tmp/FTDriver",
        "/data/local/tmp/ftdriver",
        "/sdcard/FTDriver",
    };

    std::string base = release;
    const size_t dash = base.find('-');
    if (dash != std::string::npos) base.resize(dash);

    std::string family = base;
    const size_t dot = family.find('.');
    const size_t dot2 = dot == std::string::npos ? std::string::npos : family.find('.', dot + 1);
    if (dot2 != std::string::npos) family.resize(dot2);

    // Prefer the full kernel release, then the release without Android's
    // vendor suffix, and only then an explicitly supplied major.minor payload.
    // ColorOS/OnePlus variants are never guessed from a generic prefix.
    const std::string names[] = {
        release + ".sh",
        base + ".sh",
        family + ".sh",
    };
    for (const char* dir : dirs) {
        if (!dir || !dir[0]) continue;
        for (const std::string& name : names) {
            const std::string path = std::string(dir) + "/" + name;
            if (path_executable(path.c_str())) return path;
        }
    }
    return {};
}

bool launch_driver_script(const std::string& path) {
    const pid_t launcher = fork();
    if (launcher < 0) return false;
    if (launcher == 0) {
        // Double-fork so the app never owns or stops the manually installed
        // driver process. su is required because payloads are kernel helpers.
        const pid_t worker = fork();
        if (worker == 0) {
            if (access("/system/bin/su", X_OK) == 0) {
                execl("/system/bin/su", "su", "-c", path.c_str(), static_cast<char*>(nullptr));
            }
            execl("/system/bin/sh", "sh", path.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        _exit(worker < 0 ? 127 : 0);
    }
    int status = 0;
    waitpid(launcher, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
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
    selected_script_.clear();

    // First accept an already-running endpoint. Otherwise start the exact
    // payload installed by the user, using root only for the launch step.
    device_path_ = find_device();
    if (device_path_.empty()) {
        const std::string release = kernel_release();
        selected_script_ = find_driver_script(release);
        if (selected_script_.empty()) {
            last_error_ = "Не найден FTDriver payload для ядра " +
                          (release.empty() ? std::string("unknown") : release) +
                          ". Установи нужный .sh в /data/local/tmp/FTDriver.";
            set_error(error, last_error_);
            running_ = false;
            return false;
        }
        if (!launch_driver_script(selected_script_)) {
            last_error_ = "Не удалось запустить FTDriver payload: " + selected_script_;
            set_error(error, last_error_);
            running_ = false;
            return false;
        }

        // Payloads may create their endpoint asynchronously. The app waits;
        // it never kills the payload if the endpoint is not found.
        for (int i = 0; i < 500; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            device_path_ = find_device();
            if (!device_path_.empty()) break;
        }
    }

    if (device_path_.empty()) {
        last_error_ = "FTDriver payload запущен, но его endpoint не обнаружен. "
                      "Проверь реальный device node и протокол драйвера.";
        set_error(error, last_error_);
        running_ = false;
        return false;
    }

    running_ = true;
    return true;
}

void KernelDriverManager::Stop() {
    running_ = false;
    device_path_.clear();
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
} // namespace memaccess
