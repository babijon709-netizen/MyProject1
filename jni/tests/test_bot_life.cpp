#include "bot_life.h"
#include <cassert>
#include <cstdio>

int main() {
    botlife::State life;
    unsigned reads = 0;
    bool readable = true;
    uint8_t remote = 0;
    auto read = [&](uint64_t address, uint8_t& value) {
        ++reads;
        assert(address == 0x1000 + 0xE8);
        value = remote;
        return readable;
    };
    assert(!life.refresh(read));
    assert(reads == 0);
    life.handler = 0x1000;
    assert(!life.refresh(read));
    readable = false;
    remote = 1;
    assert(!life.refresh(read));
    readable = true;
    assert(life.refresh(read));
    readable = false;
    remote = 0;
    assert(life.refresh(read));
    readable = true;
    remote = 2;
    assert(life.refresh(read));
    remote = 0;
    assert(!life.refresh(read)); // Same component reused for a live spawn.

    std::puts("bot life tests passed");
}
