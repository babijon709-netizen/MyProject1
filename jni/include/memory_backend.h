#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <sys/types.h>

namespace memaccess {

// The reader intentionally has two independent transports. External mode is
// the existing /proc/<pid>/mem implementation; kernel mode never falls back to
// it, so selecting kernel mode cannot silently run with the old access path.
enum class AccessMode : int {
    External = 0,
    Kernel = 1,
};

void SetAccessMode(AccessMode mode);
AccessMode GetAccessMode();
const char* AccessModeName(AccessMode mode);

class KernelDriverManager {
public:
    static KernelDriverManager& Instance();

    // Starts the version-matched FTDriver payload when necessary and verifies
    // that a supported device endpoint is available. The opaque files under
    // driver/ are user-space launchers for the kernel component; the memory ABI
    // is kept in the client below and is deliberately not mixed with the
    // external transport.
    bool Start(std::string* error = nullptr);
    void Stop();
    bool Running() const;
    const char* DevicePath() const;
    const char* SelectedScript() const;
    const char* LastError() const;

private:
    KernelDriverManager() = default;
    KernelDriverManager(const KernelDriverManager&) = delete;
    KernelDriverManager& operator=(const KernelDriverManager&) = delete;

    bool running_ = false;
    int child_pid_ = -1;
    std::string device_path_;
    std::string selected_script_;
    std::string last_error_;
};

namespace detail {

class Transport {
public:
    virtual ~Transport() = default;
    virtual bool Bind(pid_t pid) = 0;
    virtual void Unbind() = 0;
    virtual bool Bound() const = 0;
    virtual ssize_t Read(uint64_t address, void* out, size_t size) = 0;
    virtual ssize_t Write(uint64_t address, const void* in, size_t size) = 0;
    virtual const char* Name() const = 0;
    virtual int LastError() const = 0;
};

std::shared_ptr<Transport> CreateTransport(AccessMode mode);

} // namespace detail
} // namespace memaccess
