#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <atomic>

namespace kernel_memory {

// Выбор транспорта памяти. По умолчанию Auto: сначала скрытый socket-интерфейс
// драйвера, затем обычный /proc/<pid>/mem, если модуль не загружен.
enum class Mode : int { Auto = 0, Kernel = 1, Proc = 2 };

void set_mode(Mode mode);
Mode mode();
const char* mode_name(Mode mode);
// XVCEN_MEMORY=auto|kernel|proc. Не меняет режим, если переменной нет.
void configure_from_environment();

class Client {
public:
    Client() = default;
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool connect(pid_t pid);
    void close();
    bool connected() const { return fd_.load(std::memory_order_acquire) >= 0; }
    pid_t pid() const { return (pid_t)pid_.load(std::memory_order_relaxed); }

    bool read(uint64_t address, void* buffer, size_t size);
    bool write(uint64_t address, const void* buffer, size_t size);
    bool alive();
    int last_error() const { return last_error_.load(std::memory_order_relaxed); }

private:
    bool request(int operation, uint64_t address, void* buffer, size_t size);

    std::atomic<int> fd_{-1};
    std::atomic<int> family_{-1};
    std::atomic<int> pid_{-1};
    std::atomic<int> last_error_{0};
};

}  // namespace kernel_memory
