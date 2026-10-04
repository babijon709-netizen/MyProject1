// Хост-тест опознания libunity и контроллера анлока FPS (без устройства).
// Собирается scripts/host_tests.sh обычным g++.
#include "fps_memory.h"

#include <cstdio>
#include <cstring>
#include <map>

namespace {

// Поддельная память процесса: слово -> значение (читаемая область).
struct FakeIO {
    std::map<uint64_t, uint32_t> mem;
    template<class T> bool read(uint64_t addr, T& out) {
        std::memset(&out, 0, sizeof(T));
        bool ok = true;
        for (size_t i = 0; i < sizeof(T); i += 4) {
            auto it = mem.find(addr + i);
            if (it == mem.end()) { ok = false; continue; }
            uint32_t v = it->second;
            std::memcpy(reinterpret_cast<char*>(&out) + i, &v, 4);
        }
        return ok;
    }
    template<size_t N> bool read(uint64_t addr, std::array<uint32_t, N>& out) {
        bool ok = true;
        for (size_t i = 0; i < N; ++i) {
            auto it = mem.find(addr + i * 4);
            if (it == mem.end()) { ok = false; out[i] = 0; continue; }
            out[i] = it->second;
        }
        return ok;
    }
    template<class T> bool write(uint64_t addr, const T& in) {
        for (size_t i = 0; i < sizeof(T); i += 4)
            mem[addr + i] = *reinterpret_cast<const uint32_t*>(
                reinterpret_cast<const char*>(&in) + i);
        return true;
    }
    uint64_t ptr(uint64_t addr) {
        uint64_t v = 0;
        read(addr, v);
        return v;
    }
};

int failures = 0;
#define CHECK(cond, name) do { \
    if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", name, __LINE__); } \
} while (0)

constexpr uint64_t UNITY = 0x7000000000;
// Текущая сборка libunity (см. kFpsLayouts): build id, getter/target/pacing.
using viewmodel::kFpsLayouts;
const viewmodel::FpsLayout& current_layout() { return kFpsLayouts[2]; }

void seed_image(FakeIO& io, const viewmodel::FpsLayout& layout,
                int32_t target_value, const char*) {
    io.mem[UNITY + 0x318]     = (uint32_t)(layout.build_id & 0xFFFFFFFFu);
    io.mem[UNITY + 0x318 + 4] = (uint32_t)(layout.build_id >> 32);
    // getter: adrp x8, <страница поля> ; ldr w0,[x8,#imm] ; ret
    // word[1] берём из таблицы, adrp собираем руками под текущий getteer.
    const uint64_t pc = UNITY + layout.getter;
    const uint64_t target_page = (UNITY + layout.target) & ~0xFFFULL;
    const int64_t delta = (int64_t)(target_page - (pc & ~0xFFFULL));
    const int32_t imm21 = (int32_t)(delta >> 12);
    const uint32_t adrp = 0x90000000u | 8u /*rd*/ |
        ((uint32_t)(imm21 & 3) << 29) | ((uint32_t)(imm21 >> 2) << 5);
    io.mem[pc]     = adrp;
    io.mem[pc + 4] = layout.load_instruction;
    io.mem[pc + 8] = 0xD65F03C0u;  // ret
    // pacing update: характерная голова
    io.mem[UNITY + layout.pacing_update] = 0xF81E0FFEu;
    // живые значения
    io.mem[UNITY + layout.target]      = (uint32_t)target_value;
    io.mem[UNITY + layout.pacing_type] = 1;  // native pacing включён
}

} // namespace

int main() {
    // 1) Опознание текущей сборки: таблица выдаёт layout.
    {
        FakeIO io;
        seed_image(io, current_layout(), 30, nullptr);
        const auto* layout = viewmodel::fps_layout(io, UNITY);
        CHECK(layout == &current_layout(), "fps_layout matches current build");
    }
    // 2) Битый getter -> не опознано (fail closed).
    {
        FakeIO io;
        seed_image(io, current_layout(), 30, nullptr);
        io.mem[UNITY + current_layout().getter + 8] = 0xDEADBEEFu;  // вместо ret
        CHECK(viewmodel::fps_layout(io, UNITY) == nullptr, "corrupt getter rejected");
    }
    // 3) Чужой build id -> не опознано.
    {
        FakeIO io;
        seed_image(io, current_layout(), 30, nullptr);
        io.mem[UNITY + 0x318] ^= 0x12345678u;
        CHECK(viewmodel::fps_layout(io, UNITY) == nullptr, "unknown build id rejected");
    }
    // 4) Контроллер: 144 применяется, pacing глушится, статус Active.
    {
        FakeIO io;
        seed_image(io, current_layout(), 30, nullptr);
        viewmodel::FpsController<FakeIO> ctl(io);
        ctl.tick(1, UNITY, 144);
        CHECK(ctl.status == viewmodel::Status::Active, "tick -> Active");
        CHECK(io.mem[UNITY + current_layout().target] == 144, "target written 144");
        CHECK(io.mem[UNITY + current_layout().pacing_type] == 0, "pacing muted");
        CHECK(ctl.field_seen() == 144 && ctl.pace_seen() == 0, "seen mirrors memory");
        // устойчивое состояние: второй тик не пишет заново, статус держится
        ctl.tick(1, UNITY, 144);
        CHECK(ctl.status == viewmodel::Status::Active, "steady tick stays Active");
        // выключение: оригиналы возвращаются
        ctl.tick(1, UNITY, 0);
        CHECK(ctl.status == viewmodel::Status::Off, "disable -> Off");
        CHECK(io.mem[UNITY + current_layout().target] == 30, "target restored 30");
        CHECK(io.mem[UNITY + current_layout().pacing_type] == 1, "pacing restored");
    }
    // 5) Без базы — Waiting и никакой записи.
    {
        FakeIO io;
        viewmodel::FpsController<FakeIO> ctl(io);
        ctl.tick(1, 0, 144);
        CHECK(ctl.status == viewmodel::Status::Waiting, "no base -> Waiting");
        CHECK(!ctl.has_state(), "no base -> no state");
    }
    // 6) Неизвестная сборка — Failed.
    {
        FakeIO io;
        seed_image(io, current_layout(), 30, nullptr);
        io.mem[UNITY + 0x318] ^= 0xDEADBEEFu;
        viewmodel::FpsController<FakeIO> ctl(io);
        ctl.tick(1, UNITY, 144);
        CHECK(ctl.status == viewmodel::Status::Failed, "unknown build -> Failed");
    }
    if (failures) { printf("%d FAILURES\n", failures); return 1; }
    printf("ALL FPS TESTS PASS\n");
    return 0;
}
