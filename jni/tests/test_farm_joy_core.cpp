// Логика выбора слоя ввода для памяти-джойстика автофарма (farm_joy_core.h):
// проба по сближению, ротация по слоям, отбраковка «ведёт не туда».
// Памяти здесь нет: проверяем само решение, по которому farm_joy.cpp уже
// двигает стик в памяти игры.
#include "farm_joy_core.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using farmjoy::TrialState;

namespace {

constexpr float kDt = 1.0f / 60.0f;
constexpr int   kLayers = 7;

// Прогнать окно: дистанция меняется как speed м/с (плюс — сближаемся).
TrialState::Decision run(TrialState& state, float seconds, float start_dist, float speed) {
    TrialState::Decision last = TrialState::NONE;
    float dist = start_dist;
    const int frames = static_cast<int>(seconds / kDt + 0.5f);
    for (int i = 0; i < frames; ++i) {
        dist -= speed * kDt;
        last = state.observe(kDt, dist, true);
        if (last != TrialState::NONE) break;
    }
    return last;
}

} // namespace

int main() {
    // 1. Сближение подтверждает слой.
    {
        TrialState state(kLayers);
        state.set_armed(true, true);
        const TrialState::Decision d = run(state, 0.5f, 12.f, 4.f);
        assert(d == TrialState::CONFIRMED);
        assert(state.confirmed());
        assert(state.current() == 0);
        assert(state.trials() == 1);
        // После подтверждения пробы больше не идут.
        assert(run(state, 0.5f, 12.f, 0.f) == TrialState::NONE);
        assert(state.trials() == 1);
    }

    // 2. Стоит на месте — провал и переход к следующему доступному слою.
    {
        TrialState state(kLayers);
        state.set_available(1, false);          // слой 1 не резолвится
        state.set_armed(true, true);
        const TrialState::Decision d = run(state, 0.5f, 12.f, 0.f);
        assert(d == TrialState::NEXT);
        assert(state.fails(0) == 1);
        assert(!state.rejected(0));             // просто не подтвердился
        assert(state.current() == 2);           // 1 пропущен: недоступен
        assert(!state.confirmed());
    }

    // 3. Поехали назад — слой отбраковывается и больше не пробуется.
    {
        TrialState state(kLayers);
        state.set_armed(true, true);
        const TrialState::Decision d = run(state, 0.5f, 12.f, -6.f);
        assert(d == TrialState::NEXT);
        assert(state.rejected(0));
        assert(state.fails(0) == 1);
        assert(state.current() == 1);
        // Круг по остальным и возврат: отвергнутый 0 пропускается.
        for (int i = 0; i < kLayers - 1; ++i) {
            const TrialState::Decision dd = run(state, 0.5f, 12.f, 0.f);
            assert(dd == TrialState::NEXT);
            assert(state.current() != 0);
        }
    }

    // 4. Магнитуда ниже половины (и «не чистая» ходьба) пробу не запускают.
    {
        TrialState state(kLayers);
        state.set_armed(true, false);
        assert(run(state, 1.0f, 12.f, 0.f) == TrialState::NONE);
        assert(state.trials() == 0);
        state.set_armed(true, true);
        assert(run(state, 0.2f, 12.f, 0.f) == TrialState::NONE);   // окно не набрано
        state.observe(kDt, 11.f, false);                            // грязный кадр
        assert(state.trials() == 0);
        assert(state.current() == 0);
    }

    // 5. У самой точки подхода (и по мусорной дистанции) проба не идёт.
    {
        TrialState state(kLayers);
        state.set_armed(true, true);
        assert(run(state, 1.0f, 1.5f, 0.f) == TrialState::NONE);
        assert(state.trials() == 0);
        assert(state.observe(kDt, NAN, true) == TrialState::NONE);
        assert(state.observe(kDt, 12.f, true) == TrialState::NONE);
    }

    // 6. release() останавливает пробы, закрепление сохраняется.
    {
        TrialState state(kLayers);
        state.set_armed(true, true);
        assert(run(state, 0.5f, 12.f, 4.f) == TrialState::CONFIRMED);
        state.release();
        assert(state.confirmed());
        assert(run(state, 1.0f, 12.f, 0.f) == TrialState::NONE);
    }

    // 7. Недоступные слои не берутся даже при полном круге отбраковки.
    {
        TrialState state(kLayers);
        for (int c = 0; c < kLayers; ++c) state.set_available(c, c == 3);
        state.set_armed(true, true);
        for (int i = 0; i < 4; ++i) {
            const TrialState::Decision d = run(state, 0.5f, 12.f, -6.f);
            assert(d == TrialState::NEXT);
            assert(state.current() == 3);       // единственный доступный
        }
    }

    // 8. Порог подтверждения: чуть-чуть сближения недостаточно, ощутимое — да.
    {
        TrialState state(kLayers);
        state.set_armed(true, true);
        assert(run(state, 0.5f, 12.f, 0.1f) == TrialState::NEXT);   // ~0.05 м за окно
        assert(!state.confirmed());
        assert(run(state, 0.5f, 12.f, 1.0f) == TrialState::CONFIRMED);
        assert(state.confirmed());
    }

    std::printf("test_farm_joy_core: ok\n");
    return 0;
}
