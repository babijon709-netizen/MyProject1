#include "mem_io.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace {
struct FakeTransport final : memio::Transport {
    bool available = true;
    bool opened = false;
    int opens = 0;
    int closes = 0;

    const char* name() const override { return "fake"; }
    bool open(pid_t) override {
        ++opens;
        opened = available;
        if (!opened) errno = EIO;
        return opened;
    }
    void close() override { ++closes; opened = false; }
    ssize_t read_once(uint64_t, void* out, size_t len) override {
        if (!opened || !available) { errno = EIO; return -1; }
        unsigned char bytes[4] = {0x7f, 'E', 'L', 'F'};
        if (len > sizeof(bytes)) { errno = EIO; return -1; }
        std::memcpy(out, bytes, len);
        return static_cast<ssize_t>(len);
    }
    ssize_t write_once(uint64_t, const void*, size_t len) override {
        return opened && available ? static_cast<ssize_t>(len) : -1;
    }
};

int failures = 0;
#define CHECK(condition, label) do { \
    if (!(condition)) { ++failures; std::printf("FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)
} // namespace

int main() {
    FakeTransport transport;
    memio::Reader reader;
    reader.set_transport(&transport);
    CHECK(reader.bind(4242, 0x10000), "initial bind succeeds");
    CHECK(transport.opens == 1 && reader.bound(), "initial transport is bound");

    // A transport outage closes the failed session but keeps pid/probe data.
    transport.available = false;
    CHECK(!reader.verify(), "unavailable transport fails liveness");
    CHECK(!reader.bound(), "failed probe leaves no stale open session");

    // No process scan/rebind is needed after the driver/fd comes back: verify
    // retries the same selected transport with the remembered pid and probe.
    transport.available = true;
    CHECK(reader.verify(), "verify recovers transport in place");
    CHECK(reader.bound(), "recovered transport is bound");
    CHECK(transport.opens >= 3, "verify reopened transport after outage");

    if (failures) {
        std::printf("%d FAILURES\n", failures);
        return 1;
    }
    std::puts("ALL MEMIO-VERIFY TESTS PASS");
    return 0;
}
