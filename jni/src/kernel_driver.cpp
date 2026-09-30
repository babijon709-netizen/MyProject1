#include "kernel_driver.h"

#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

namespace kernel_driver {
namespace {

constexpr const char* kKnownNames[] = {
    "wanbai", "CheckMe", "Ckanri", "lanran", "video188",
};
constexpr size_t kMaxHiddenName = 7;

// Standard character devices that satisfy the "short lowercase, root-owned,
// zero-sized" convention the module uses (/dev/null is the obvious one). They
// were the reason a device was always "found": the node scan matched them and
// nothing afterwards proved the ioctl protocol, so kernel mode came up on
// phones without the module installed.
constexpr const char* kExcludedNames[] = {
    "null", "zero", "full", "random", "urandom", "tty", "console", "ptmx",
    "kmsg", "mem", "kmem", "port", "fuse", "ashmem", "binder", "hwbinder",
    "vndbinder", "rtc", "watchdog", "loop", "ram", "sda", "mmcblk", "zram",
};

bool all_lowercase_letters(const char* name) {
    if (!name || !name[0]) return false;
    size_t length = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name);
         *p; ++p) {
        if (!std::islower(*p)) return false;
        if (++length > kMaxHiddenName) return false;
    }
    return length != 0;
}

bool is_excluded_name(const char* name) {
    if (!name) return true;
    for (const char* excluded : kExcludedNames)
        if (std::strcmp(name, excluded) == 0) return true;
    return false;
}

bool contains(const char* name, const char* needle) {
    return name && needle && std::strstr(name, needle) != nullptr;
}

void append_unique(std::vector<std::string>& nodes, const std::string& path) {
    if (std::find(nodes.begin(), nodes.end(), path) == nodes.end())
        nodes.push_back(path);
}

}  // namespace

namespace {

// Value read back through the driver during the pid-less probe. Reading our
// OWN memory is the only self-check available before a game process exists,
// and it is a real end-to-end test of the protocol: a device that is not the
// module either rejects the ioctl (ENOTTY/EINVAL) or leaves the buffer
// untouched, and in both cases the pattern does not come back.
volatile uint64_t g_self_probe_value = 0x5175696E67687561ULL;  // "Qinghua"

bool speaks_protocol(int fd) {
    uint64_t readback = 0;
    CopyMemory request{getpid(),
                       reinterpret_cast<uintptr_t>(const_cast<uint64_t*>(&g_self_probe_value)),
                       &readback, sizeof(readback)};
    errno = 0;
    if (ioctl(fd, OP_READ_MEM, &request) != 0) return false;
    return readback == g_self_probe_value;
}

}  // namespace

Availability probe() {
    Availability result;
    const std::vector<std::string> nodes = Driver::discover_nodes();
    for (const std::string& candidate : nodes) {
        errno = 0;
        int fd = open(candidate.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) fd = open(candidate.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            // Only a node with one of the module's published names counts as
            // evidence when it cannot be opened: the anonymous-node heuristic
            // also matches ordinary root-only devices, and treating those as
            // "driver present, run as root" was misleading.
            const char* leaf = std::strrchr(candidate.c_str(), '/');
            if (!result.present && leaf && Driver::is_known_node(leaf + 1)) {
                result.present = true;
                result.path = candidate;
                result.error = errno;
            }
            continue;
        }
        const bool verified = speaks_protocol(fd);
        close(fd);
        if (!verified) continue;  // some other character device, not the module
        result.present = true;
        result.openable = true;
        result.error = 0;
        result.path = candidate;
        return result;
    }
    if (!result.present) result.error = ENODEV;
    return result;
}

Driver::~Driver() { unbind(); }

bool Driver::is_known_node(const char* name) {
    if (!name) return false;
    for (const char* known : kKnownNames)
        if (std::strcmp(name, known) == 0) return true;
    return false;
}

bool Driver::looks_like_hidden_node(const char* name, const struct stat& info) {
    // This is the same identifying convention used by the reference source:
    // an unnamed-looking, root-owned, zero-sized character node with a normal
    // creation time. It keeps ordinary /dev entries out of the ioctl probe.
    if (!all_lowercase_letters(name)) return false;
    if (is_excluded_name(name)) return false;
    if (!S_ISCHR(info.st_mode) && !S_ISBLK(info.st_mode)) return false;
    if (info.st_uid != 0 || info.st_gid != 0 || info.st_size != 0) return false;
    if (contains(name, "gpiochip")) return false;
    if (info.st_ctime == 0 || info.st_atime != info.st_ctime) return false;
    return true;
}

std::vector<std::string> Driver::discover_nodes() {
    std::vector<std::string> result;
    DIR* dir = opendir("/dev");
    if (!dir) return result;

    // Stable names first. Older modules use one of these names, while newer
    // builds remove the name from the visible device lists.
    for (const char* name : kKnownNames) {
        char path[PATH_MAX];
        std::snprintf(path, sizeof(path), "/dev/%s", name);
        struct stat info{};
        if (stat(path, &info) == 0 && (S_ISCHR(info.st_mode) || S_ISBLK(info.st_mode)))
            append_unique(result, path);
    }

    rewinddir(dir);
    while (struct dirent* entry = readdir(dir)) {
        if (!entry->d_name[0] || std::strcmp(entry->d_name, ".") == 0 ||
            std::strcmp(entry->d_name, "..") == 0 || is_known_node(entry->d_name))
            continue;
        char path[PATH_MAX];
        std::snprintf(path, sizeof(path), "/dev/%s", entry->d_name);
        struct stat info{};
        if (lstat(path, &info) != 0 || !looks_like_hidden_node(entry->d_name, info))
            continue;
        append_unique(result, path);
    }
    closedir(dir);
    return result;
}

bool Driver::read_is_elf(int fd, pid_t pid, uintptr_t address) {
    if (fd < 0 || pid <= 0 || !address) return false;
    unsigned char magic[4] = {};
    CopyMemory request{pid, address, magic, sizeof(magic)};
    errno = 0;
    if (ioctl(fd, OP_READ_MEM, &request) != 0) return false;
    return magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
}

static uintptr_t module_base_from_fd(int fd, pid_t pid, const char* name) {
    if (fd < 0 || pid <= 0 || !name || !name[0]) return 0;
    char buffer[256] = {};
    std::strncpy(buffer, name, sizeof(buffer) - 1);
    ModuleBase request{pid, buffer, 0};
    if (ioctl(fd, OP_MODULE_BASE, &request) != 0) return 0;
    return request.base;
}

bool Driver::bind(pid_t pid, uintptr_t probe_address) {
    unbind();
    if (pid <= 0) {
        errno = EINVAL;
        return false;
    }

    const std::vector<std::string> nodes = discover_nodes();
    for (const std::string& candidate : nodes) {
        int fd = open(candidate.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) fd = open(candidate.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;

        // The reference driver exposes both read/write and module-base ioctl
        // operations. A zero probe is only used by the maps-less fallback and
        // therefore checks the module operation to avoid attaching to another
        // character device with a coincidentally permissive ioctl number.
        const bool valid = probe_address
                               ? read_is_elf(fd, pid, probe_address)
                               : module_base_from_fd(fd, pid, "libil2cpp.so") != 0;
        if (!valid) {
            close(fd);
            continue;
        }
        fd_ = fd;
        pid_ = pid;
        path_ = candidate;
        return true;
    }
    errno = ENODEV;
    return false;
}

void Driver::unbind() {
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
    pid_ = -1;
    path_.clear();
}

ssize_t Driver::read(uintptr_t address, void* out, size_t size) const {
    if (!bound() || !address || !out || !size) {
        errno = EINVAL;
        return -1;
    }
    CopyMemory request{pid_, address, out, size};
    errno = 0;
    if (ioctl(fd_, OP_READ_MEM, &request) != 0) {
        if (errno == 0) errno = EIO;
        return -1;
    }
    return static_cast<ssize_t>(size);
}

ssize_t Driver::write(uintptr_t address, const void* in, size_t size) const {
    if (!bound() || !address || !in || !size) {
        errno = EINVAL;
        return -1;
    }
    CopyMemory request{pid_, address, const_cast<void*>(in), size};
    errno = 0;
    if (ioctl(fd_, OP_WRITE_MEM, &request) != 0) {
        if (errno == 0) errno = EIO;
        return -1;
    }
    return static_cast<ssize_t>(size);
}

uintptr_t Driver::module_base(const char* name) const {
    if (!bound()) return 0;
    return module_base_from_fd(fd_, pid_, name);
}

}  // namespace kernel_driver
