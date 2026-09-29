#include "mem_io.h"

#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

bool write_all(int fd, const void* data, size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    while (size) {
        const ssize_t n = write(fd, bytes, size);
        if (n > 0) { bytes += n; size -= static_cast<size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool read_all(int fd, void* data, size_t size) {
    auto* bytes = static_cast<unsigned char*>(data);
    while (size) {
        const ssize_t n = read(fd, bytes, size);
        if (n > 0) { bytes += n; size -= static_cast<size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

}  // namespace

int main() {
    setenv("RWMEM_BACKEND", "proc", 1);

    int address_pipe[2] = {-1, -1};
    int command_pipe[2] = {-1, -1};
    int result_pipe[2] = {-1, -1};
    if (pipe(address_pipe) || pipe(command_pipe) || pipe(result_pipe)) {
        perror("pipe");
        return 1;
    }

    const pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return 1;
    }
    if (child == 0) {
        close(address_pipe[0]);
        close(command_pipe[1]);
        close(result_pipe[0]);
        auto* page = static_cast<unsigned char*>(mmap(nullptr, 4096,
            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (page == MAP_FAILED) _exit(2);
        page[0] = 0x7f; page[1] = 'E'; page[2] = 'L'; page[3] = 'F';
        const uint64_t initial = 0x1122334455667788ULL;
        memcpy(page + 64, &initial, sizeof(initial));
        const uint64_t address = reinterpret_cast<uint64_t>(page);
        if (!write_all(address_pipe[1], &address, sizeof(address))) _exit(3);
        unsigned char command = 0;
        if (!read_all(command_pipe[0], &command, sizeof(command))) _exit(4);
        uint64_t value = 0;
        memcpy(&value, page + 64, sizeof(value));
        const unsigned char ok =
            command == 1 && value == 0xaabbccddeeff0011ULL ? 1 : 0;
        write_all(result_pipe[1], &ok, sizeof(ok));
        _exit(ok ? 0 : 5);
    }

    close(address_pipe[1]);
    close(command_pipe[0]);
    close(result_pipe[1]);

    uint64_t address = 0;
    if (!read_all(address_pipe[0], &address, sizeof(address))) {
        fprintf(stderr, "did not receive child mapping\n");
        kill(child, SIGKILL);
        return 1;
    }

    memio::Reader reader;
    if (!reader.bind(child, address)) {
        fprintf(stderr, "bind failed: errno=%d\n", reader.last_error());
        kill(child, SIGKILL);
        return 1;
    }
    if (strcmp(reader.backend_name(), "/proc/<pid>/mem") != 0) {
        fprintf(stderr, "unexpected backend: %s\n", reader.backend_name());
        kill(child, SIGKILL);
        return 1;
    }

    uint64_t value = 0;
    if (!reader.read(address + 64, &value, sizeof(value)) ||
        value != 0x1122334455667788ULL) {
        fprintf(stderr, "remote read mismatch: 0x%llx\n",
                static_cast<unsigned long long>(value));
        kill(child, SIGKILL);
        return 1;
    }

    const uint64_t replacement = 0xaabbccddeeff0011ULL;
    if (!reader.write(address + 64, &replacement, sizeof(replacement))) {
        fprintf(stderr, "remote write failed: errno=%d\n", errno);
        kill(child, SIGKILL);
        return 1;
    }
    value = 0;
    if (!reader.read(address + 64, &value, sizeof(value)) || value != replacement) {
        fprintf(stderr, "read-after-write mismatch\n");
        kill(child, SIGKILL);
        return 1;
    }

    std::string maps;
    if (!memio::read_process_maps(child, maps) || maps.empty()) {
        fprintf(stderr, "process maps fallback failed\n");
        kill(child, SIGKILL);
        return 1;
    }

    // На host /dev/rwmem отсутствует: auto обязан бесшовно выбрать procfs.
    reader.unbind();
    setenv("RWMEM_BACKEND", "auto", 1);
    if (!reader.bind(child, address) ||
        strcmp(reader.backend_name(), "/proc/<pid>/mem") != 0) {
        fprintf(stderr, "automatic driver-to-proc fallback failed\n");
        kill(child, SIGKILL);
        return 1;
    }

    const unsigned char command = 1;
    write_all(command_pipe[1], &command, sizeof(command));
    unsigned char child_ok = 0;
    read_all(result_pipe[0], &child_ok, sizeof(child_ok));
    int status = 0;
    waitpid(child, &status, 0);
    if (!child_ok || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "child did not observe the remote write\n");
        return 1;
    }

    puts("memio proc backend: OK");
    return 0;
}
