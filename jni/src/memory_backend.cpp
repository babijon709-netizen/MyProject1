#include "memory_backend.h"
#include "ftdriver_abi.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
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

    // FTDriver is deliberately started by the user with the payload matching
    // the device kernel. The app must not guess a payload, execute it, or stop
    // it on exit: the supplied files are opaque launchers and their device/IO
    // protocol is not part of the app.
    device_path_ = find_device();
    if (device_path_.empty()) {
        last_error_ =
            "FTDriver не найден. Запусти вручную payload для своей версии ядра "
            "и укажи его device node через MEMIO_KERNEL_DEVICE.";
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
