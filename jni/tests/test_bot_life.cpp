#include "bot_life.h"
#include "esp_box_hold.h"
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

    EspMarker bot{}, ore{};
    bot.id = 1; bot.kind = ESP_MARKER_BOT;
    ore.id = 2; ore.kind = ESP_MARKER_ORE;
    std::vector<espboxhold::Entry<EspMarker>> hold;
    std::vector<EspMarker> out;
    // Кадр с матрицей вида: удержанная метка перепроецируется мировым якорем.
    auto frame_ok = [](EspMarker&) { return true; };
    espboxhold::update(hold, std::vector<EspMarker>{bot, ore}, false, 0.0, 0.35, frame_ok, out);
    remote = 1;
    assert(life.refresh(read));
    espboxhold::erase_if(hold, [&](const EspMarker& marker) {
        return marker.id == bot.id && life.dead;
    });
    espboxhold::update(hold, std::vector<EspMarker>{}, true, 0.01, 0.35, frame_ok, out);
    assert(out.size() == 1 && out[0].id == ore.id);
    espboxhold::update(hold, std::vector<EspMarker>{}, true, 0.4, 0.35, frame_ok, out);
    assert(out.empty());
    remote = 0;
    assert(!life.refresh(read));
    espboxhold::update(hold, std::vector<EspMarker>{bot}, false, 0.5, 0.35, frame_ok, out);
    assert(out.size() == 1 && out[0].id == bot.id);
    std::puts("bot life tests passed");
}
