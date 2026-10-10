#include "fast_loot_memory.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

using go_active::IL2CPP_ARRAY_FIRST_ELEMENT;
using go_active::IL2CPP_ARRAY_LENGTH;
using go_active::IL2CPP_CLASS_STATIC_FIELDS_OFFSET;

struct FakeIO {
    std::map<uint64_t, uint8_t> mem;
    std::map<uint64_t, std::pair<std::string, std::string>> klass_names;
    unsigned writes = 0;

    template<class T> void put(uint64_t a, T v) {
        auto* bytes = reinterpret_cast<const uint8_t*>(&v);
        for (size_t i = 0; i < sizeof(v); ++i) mem[a + i] = bytes[i];
    }
    template<class T> bool read(uint64_t a, T& v) {
        auto* bytes = reinterpret_cast<uint8_t*>(&v);
        for (size_t i = 0; i < sizeof(v); ++i) {
            auto it = mem.find(a + i);
            if (it == mem.end()) return false;
            bytes[i] = it->second;
        }
        return true;
    }
    bool read_range(uint64_t a, void* out, size_t size) {
        auto* bytes = reinterpret_cast<uint8_t*>(out);
        for (size_t i = 0; i < size; ++i) {
            auto it = mem.find(a + i);
            if (it == mem.end()) return false;
            bytes[i] = it->second;
        }
        return true;
    }
    template<class T> bool write_heap(uint64_t a, T v) {
        ++writes; put(a, v); return true;
    }
    bool named_klass(uint64_t klass, const char* name, const char* ns) {
        auto it = klass_names.find(klass);
        return it != klass_names.end() && it->second.first == name && it->second.second == ns;
    }
    float value(uint64_t a) { float v = 0; assert(read(a, v)); return v; }
};

const uint64_t base = 0x400000;
const uint64_t helper_klass = 0x500000;
const uint64_t helper_statics = 0x510000;
const uint64_t runners_array = 0x520000;
const uint64_t dp_klass = 0x530000;   // DelayPromise
const uint64_t di_klass = 0x531000;   // DelayIgnoreTimeScalePromise

// Типичный объект промиса: класс в первом поле, дальше тройка полей.
void make_promise(FakeIO& io, uint64_t obj, uint64_t klass, float timespan, float elapsed, bool scaled) {
    io.put<uint64_t>(obj, klass);
    if (scaled) { // initialFrame 0x18, delayTimeSpan 0x1C, elapsed 0x20
        io.put<int32_t>(obj + 0x18, 1234);
        io.put<float>(obj + go_active::DELAYPROMISE_SCALED_TIMESPAN, timespan);
        io.put<float>(obj + go_active::DELAYPROMISE_SCALED_ELAPSED, elapsed);
    } else {      // delayFrameTimeSpan 0x18, elapsed 0x1C, initialFrame 0x20
        io.put<float>(obj + go_active::DELAYPROMISE_TIMESPAN, timespan);
        io.put<float>(obj + go_active::DELAYPROMISE_ELAPSED, elapsed);
        io.put<int32_t>(obj + go_active::DELAYPROMISE_INITIAL_FRAME, 1234);
    }
}

struct Fixture {
    FakeIO io;
    fast_loot::Controller<FakeIO> c{io};

    Fixture() {
        // TypeInfo-слоты (адреса в «образе» базы) и классы с именами.
        io.put<uint64_t>(base + go_active::PLAYER_LOOP_HELPER_TYPEINFO_RVA, helper_klass);
        io.put<uint64_t>(base + go_active::FASTLOOT_DELAYPROMISE_TYPEINFO_RVA, dp_klass);
        io.put<uint64_t>(base + go_active::FASTLOOT_IGNOREDELAY_TYPEINFO_RVA, di_klass);
        io.klass_names[helper_klass] = {"PlayerLoopHelper", "Cysharp.Threading.Tasks"};
        io.klass_names[dp_klass] = {"DelayPromise", ""};
        io.klass_names[di_klass] = {"DelayIgnoreTimeScalePromise", ""};
        io.put<uint64_t>(helper_klass + IL2CPP_CLASS_STATIC_FIELDS_OFFSET, helper_statics);
        io.put<uint64_t>(helper_statics + go_active::PLAYERLOOPHELPER_RUNNERS, runners_array);
    }

    // Регистрирует раннер с набором предметов; возвращает адрес раннера.
    uint64_t runner(uint64_t addr, int32_t tail, const std::vector<uint64_t>& items, int32_t capacity = -1) {
        io.put<int32_t>(addr + go_active::LOOPRUNNER_TAIL, tail);
        io.put<uint32_t>(addr + go_active::LOOPRUNNER_TAIL + 4, 0); // pad до указателя
        const uint64_t items_array = addr + 0x100;
        io.put<uint64_t>(addr + go_active::LOOPRUNNER_ITEMS, items_array);
        io.put<int32_t>(items_array + IL2CPP_ARRAY_LENGTH,
                        capacity >= 0 ? capacity : (int32_t)items.size());
        for (size_t i = 0; i < items.size(); ++i)
            io.put<uint64_t>(items_array + IL2CPP_ARRAY_FIRST_ELEMENT + i * 8, items[i]);
        return addr;
    }
    void runners(std::initializer_list<uint64_t> list) {
        io.put<int32_t>(runners_array + IL2CPP_ARRAY_LENGTH, (int32_t)list.size());
        size_t i = 0;
        for (uint64_t r : list)
            io.put<uint64_t>(runners_array + IL2CPP_ARRAY_FIRST_ELEMENT + (i++) * 8, r);
    }
};

int main() {
    // 1. Обнуляются только точные 0.35f, в обоих раскладках полей.
    {
        Fixture f;
        const uint64_t dp_ok = 0x600000, dp_other = 0x601000, di_ok = 0x602000, r = 0x700000;
        make_promise(f.io, dp_ok, dp_klass, 0.35f, 0.10f, true);
        make_promise(f.io, dp_other, dp_klass, 0.50f, 0.10f, true);
        make_promise(f.io, di_ok, di_klass, 0.35f, 0.05f, false);
        f.runners({f.runner(r, 3, {dp_ok, dp_other, di_ok})});

        const int zeroed = f.c.tick(base, true);
        assert(zeroed == 2);
        assert(f.c.zeroed_total() == 2);
        assert(f.c.state() == fast_loot::State::Active);
        assert(f.io.value(dp_ok + go_active::DELAYPROMISE_SCALED_TIMESPAN) == 0.f);
        assert(f.io.value(di_ok + go_active::DELAYPROMISE_TIMESPAN) == 0.f);
        assert(f.io.value(dp_other + go_active::DELAYPROMISE_SCALED_TIMESPAN) == 0.5f);
        assert(f.io.writes == 2);
        std::printf("fast_loot: exact-cadence zeroing ok\n");
    }

    // 2. tail=0 — элементы за хвостом не трогаются.
    {
        Fixture f;
        const uint64_t stale = 0x610000, r = 0x700000;
        make_promise(f.io, stale, dp_klass, 0.35f, 0.1f, true);
        f.runners({f.runner(r, 0, {stale})});
        assert(f.c.tick(base, true) == 0);
        assert(f.io.value(stale + go_active::DELAYPROMISE_SCALED_TIMESPAN) == 0.35f);
        std::printf("fast_loot: empty tail untouched ok\n");
    }

    // 3. Мусорный elapsed (бесконечность / большой) блокирует запись.
    {
        Fixture f;
        const uint64_t nan_item = 0x620000, big_item = 0x621000, r = 0x700000;
        make_promise(f.io, nan_item, dp_klass, 0.35f, std::numeric_limits<float>::infinity(), true);
        make_promise(f.io, big_item, dp_klass, 0.35f, 100.f, true);
        f.runners({f.runner(r, 2, {nan_item, big_item})});
        assert(f.c.tick(base, true) == 0);
        assert(f.io.writes == 0);
        std::printf("fast_loot: elapsed guard ok\n");
    }

    // 4. Чужой класс в элементе — запись не делается, резолв не портится.
    {
        Fixture f;
        const uint64_t foreign_klass = 0x532000, item = 0x630000, r = 0x700000;
        f.io.klass_names[foreign_klass] = {"DelayFramePromise", ""};
        make_promise(f.io, item, foreign_klass, 0.35f, 0.f, true);
        f.runners({f.runner(r, 1, {item})});
        assert(f.c.tick(base, true) == 0);
        assert(f.io.writes == 0);
        assert(f.c.state() == fast_loot::State::Active);
        std::printf("fast_loot: foreign class ignored ok\n");
    }

    // 5. Имя PlayerLoopHelper не сошлось — Failed, записей нет.
    {
        Fixture f;
        f.io.klass_names[helper_klass] = {"SomeOtherClass", "Oxide"};
        const uint64_t item = 0x640000, r = 0x700000;
        make_promise(f.io, item, dp_klass, 0.35f, 0.f, true);
        f.runners({f.runner(r, 1, {item})});
        assert(f.c.tick(base, true) == 0);
        assert(f.c.state() == fast_loot::State::Failed);
        assert(f.c.stage() == 1);
        assert(f.io.writes == 0);
        std::printf("fast_loot: helper identity gate ok\n");
    }

    // 6. Бета 11957: слоты точные (TypeInfoPointers), запасного нет — класс берётся с основного слота.
    {
        go_active::Select(go_active::GameVersion::Beta);
        Fixture f;
        const uint64_t item = 0x650000, r = 0x700000;
        f.io.put<uint64_t>(base + go_active::FASTLOOT_DELAYPROMISE_TYPEINFO_RVA, dp_klass);
        make_promise(f.io, item, dp_klass, 0.35f, 0.f, true);
        f.runners({f.runner(r, 1, {item})});
        assert(go_active::FASTLOOT_DELAYPROMISE_TYPEINFO_RVA_ALT == 0);
        assert(f.c.tick(base, true) == 1);
        assert(f.io.value(item + go_active::DELAYPROMISE_SCALED_TIMESPAN) == 0.f);
        go_active::Select(go_active::GameVersion::Release);
        std::printf("fast_loot: beta exact typeinfo slot ok\n");
    }

    // 7. Выключенный тумблер: состояние Off, память не читается на запись.
    {
        Fixture f;
        const uint64_t item = 0x660000, r = 0x700000;
        make_promise(f.io, item, dp_klass, 0.35f, 0.f, true);
        f.runners({f.runner(r, 1, {item})});
        assert(f.c.tick(base, false) == 0);
        assert(f.c.state() == fast_loot::State::Off);
        assert(f.io.writes == 0);
        std::printf("fast_loot: disabled idle ok\n");
    }

    // 8. capacity меньше tail — читаем только в пределах массива.
    {
        Fixture f;
        const uint64_t ok = 0x670000, beyond = 0x671000, r = 0x700000;
        make_promise(f.io, ok, dp_klass, 0.35f, 0.f, true);
        make_promise(f.io, beyond, dp_klass, 0.35f, 0.f, true);
        f.runners({f.runner(r, 2, {ok}, 1)}); // tail=2, но в массиве 1 слот
        assert(f.c.tick(base, true) == 1);
        assert(f.io.value(beyond + go_active::DELAYPROMISE_SCALED_TIMESPAN) == 0.35f);
        std::printf("fast_loot: array capacity clamp ok\n");
    }

    std::printf("ALL FAST-LOOT TESTS PASS\n");
    return 0;
}
