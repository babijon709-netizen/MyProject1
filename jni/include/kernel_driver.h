#pragma once

#include <sys/types.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stddef.h>

#include <string>
#include <vector>

namespace kernel_driver {

// Protocol used by the bundled QX/Qinghuan-compatible Android modules. The
// driver intentionally exposes a dynamic /dev node, so discovery is kept in a
// separate transport instead of being mixed into the game's memory layer.
constexpr unsigned long OP_INIT_KEY    = 0x800;
constexpr unsigned long OP_READ_MEM    = 0x801;
constexpr unsigned long OP_WRITE_MEM   = 0x802;
constexpr unsigned long OP_MODULE_BASE = 0x803;

struct CopyMemory {
    pid_t pid;
    uintptr_t addr;
    void* buffer;
    size_t size;
};

struct ModuleBase {
    pid_t pid;
    char* name;
    uintptr_t base;
};

class Driver {
public:
    Driver() = default;
    ~Driver();

    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;

    // Finds the driver's /dev node and verifies it by reading the ELF header at
    // the target's known library mapping. Passing probe_address == 0 selects a
    // node through the driver's module-base operation instead; that fallback
    // is used when /proc/<pid>/maps is unavailable.
    bool bind(pid_t pid, uintptr_t probe_address);
    void unbind();

    bool bound() const { return fd_ >= 0 && pid_ > 0; }
    const char* path() const { return path_.empty() ? "kernel driver" : path_.c_str(); }
    pid_t pid() const { return pid_; }

    ssize_t read(uintptr_t address, void* out, size_t size) const;
    ssize_t write(uintptr_t address, const void* in, size_t size) const;
    uintptr_t module_base(const char* name) const;

private:
    static std::vector<std::string> discover_nodes();
    static bool is_known_node(const char* name);
    static bool looks_like_hidden_node(const char* name, const struct stat& info);
    static bool read_is_elf(int fd, pid_t pid, uintptr_t address);

    int fd_ = -1;
    pid_t pid_ = -1;
    std::string path_;
};

}  // namespace kernel_driver
