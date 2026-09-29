#include "kernel_memory.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

namespace kernel_memory {
namespace {

// ABI общий с driver/include/gc_protocol.h. Значения намеренно не используют
// ioctl и не создают /dev-узел: модуль регистрирует свободное семейство сокетов.
constexpr int kFirstFamily = 12;       // AF_DECnet; дальше сканируется до NPROTO
constexpr int kLastFamily = 63;
constexpr int kSocketProtocol = 0x090419;
constexpr uint32_t kAbiVersion = 0x00020001U;
constexpr size_t kMaxTransfer = 256U * 1024U;

enum Operation : int {
    kPing = 0x4700,
    kAttach = 0x4701,
    kAlive = 0x4702,
    kRead = 0x4703,
    kWrite = 0x4704,
};

struct Request {
    uint32_t version;
    uint32_t flags;
    uint64_t address;
    uint64_t buffer;
    uint64_t size;
};

std::atomic<int> g_mode{static_cast<int>(Mode::Auto)};

bool equals_ignore_case(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return *a == *b;
}

}  // namespace

void set_mode(Mode value) { g_mode.store(static_cast<int>(value), std::memory_order_relaxed); }
Mode mode() { return static_cast<Mode>(g_mode.load(std::memory_order_relaxed)); }

const char* mode_name(Mode value) {
    switch (value) {
        case Mode::Kernel: return "kernel";
        case Mode::Proc: return "proc";
        default: return "auto";
    }
}

void configure_from_environment() {
    const char* value = getenv("XVCEN_MEMORY");
    if (!value || !*value) return;
    if (equals_ignore_case(value, "kernel") || equals_ignore_case(value, "driver"))
        set_mode(Mode::Kernel);
    else if (equals_ignore_case(value, "proc") || equals_ignore_case(value, "user"))
        set_mode(Mode::Proc);
    else if (equals_ignore_case(value, "auto"))
        set_mode(Mode::Auto);
}

Client::~Client() { close(); }

bool Client::connect(pid_t target) {
    close();
    if (target <= 0) {
        last_error_.store(EINVAL);
        return false;
    }

    // Семейство выбирается динамически драйвером. Неверное семейство либо не
    // создаст SOCK_RAW, либо отвергнет секретный protocol; успешный ping
    // дополнительно исключает случайное совпадение с чужим протоколом.
    for (int family = kFirstFamily; family <= kLastFamily; ++family) {
        int fd = ::socket(family, SOCK_RAW | SOCK_CLOEXEC, kSocketProtocol);
        if (fd < 0) continue;

        uint32_t version = 0;
        socklen_t length = sizeof(version);
        if (::getsockopt(fd, 0, kPing, &version, &length) != 0 ||
            length != sizeof(version) || version != kAbiVersion) {
            ::close(fd);
            continue;
        }

        int value = target;
        length = sizeof(value);
        if (::getsockopt(fd, target, kAttach, &value, &length) != 0) {
            const int error = errno;
            ::close(fd);
            last_error_.store(error);
            continue;
        }
        family_.store(family, std::memory_order_relaxed);
        pid_.store(target, std::memory_order_relaxed);
        fd_.store(fd, std::memory_order_release);
        last_error_.store(0, std::memory_order_relaxed);
        return true;
    }
    if (!last_error_.load(std::memory_order_relaxed)) last_error_.store(ENODEV);
    return false;
}

void Client::close() {
    const int fd = fd_.exchange(-1, std::memory_order_acq_rel);
    if (fd >= 0) ::close(fd);
    family_.store(-1, std::memory_order_relaxed);
    pid_.store(-1, std::memory_order_relaxed);
}

bool Client::request(int operation, uint64_t address, void* buffer, size_t size) {
    if (!buffer || !size) {
        last_error_.store(EINVAL);
        return false;
    }
    const int fd = fd_.load(std::memory_order_acquire);
    if (fd < 0) {
        last_error_.store(ENOTCONN);
        return false;
    }

    Request request{kAbiVersion, 0, address,
                    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(buffer)), size};
    socklen_t length = sizeof(request);
    if (::getsockopt(fd, 0, operation, &request, &length) != 0) {
        last_error_.store(errno, std::memory_order_relaxed);
        return false;
    }
    last_error_.store(0, std::memory_order_relaxed);
    return true;
}

bool Client::read(uint64_t address, void* buffer, size_t size) {
    auto* output = static_cast<unsigned char*>(buffer);
    while (size) {
        const size_t chunk = size > kMaxTransfer ? kMaxTransfer : size;
        if (!request(kRead, address, output, chunk)) return false;
        address += chunk;
        output += chunk;
        size -= chunk;
    }
    return true;
}

bool Client::write(uint64_t address, const void* buffer, size_t size) {
    auto* input = static_cast<const unsigned char*>(buffer);
    while (size) {
        const size_t chunk = size > kMaxTransfer ? kMaxTransfer : size;
        // ABI содержит userspace-указатель; const снимается только для передачи.
        if (!request(kWrite, address, const_cast<unsigned char*>(input), chunk)) return false;
        address += chunk;
        input += chunk;
        size -= chunk;
    }
    return true;
}

bool Client::alive() {
    const int fd = fd_.load(std::memory_order_acquire);
    if (fd < 0) return false;
    int value = 0;
    socklen_t length = sizeof(value);
    if (::getsockopt(fd, 0, kAlive, &value, &length) != 0) {
        last_error_.store(errno, std::memory_order_relaxed);
        return false;
    }
    return value == 1;
}

}  // namespace kernel_memory
