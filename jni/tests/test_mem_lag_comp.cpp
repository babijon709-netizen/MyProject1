// Host-тест компенсации задержки записи мемори-аима.
#include "mem_lag_comp.h"
#include <cmath>
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)
static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

int main() {
    using namespace mem_lag_comp;

    CHECK(near(wrap180(370.f), 10.f));
    CHECK(near(wrap180(-190.f), 170.f));
    CHECK(near(wrap180(0.f), 0.f));

    CHECK(pending(true, false, 1));
    CHECK(!pending(true, true, 1));
    CHECK(!pending(false, false, 0));
    CHECK(!pending(true, false, kPendingFrames + 1));

    CHECK(near(base(10.f, 12.f, true), 12.f));
    CHECK(near(base(10.f, 12.f, false), 10.f));

    // Шаг прошлого такта ещё не в камере: вычитаем его из ошибки.
    CHECK(near(error(1.0f, 0.4f, true), 0.6f));
    CHECK(near(error(-1.0f, -0.4f, true), -0.6f));
    // Без ожидания записи ошибка не меняется, знак шага не влияет.
    CHECK(near(error(1.0f, 0.4f, false), 1.0f));
    CHECK(near(error(1.0f, NAN, true), 1.0f));

    if (fails) { std::printf("test_mem_lag_comp: %d FAILED\n", fails); return 1; }
    std::printf("test_mem_lag_comp: ok\n");
    return 0;
}
