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

    // Verifies an already-running FTDriver endpoint or starts the exact
    // manually installed payload for the current kernel. The app never stops a
    // payload that it started; the payload must still match the device kernel
    // and its real device/ABI must be configured for the target device.
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
