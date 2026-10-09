// Host-тест альфа-бета предиктора цели.
#include "aim_predict.h"
#include <cmath>
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

int main() {
    using namespace aim_predict;

    // Постоянная скорость 30 град/с по yaw, 60 Гц: скорость сходится, предикт попадает.
    {
        Track tr; double t = 0.0; float yaw = 0.f;
        for (int i = 0; i < 120; ++i) {
            update(tr, yaw, 5.f, t);
            t += 1.0 / 60.0; yaw += 30.f / 60.f;
        }
        CHECK(std::fabs(tr.vx - 30.f) < 1.5f);
        float py, pp; predict(tr, 1.0f / 60.f, py, pp);
        const float truth = yaw;                  // позиция через один кадр
        CHECK(std::fabs(py - (truth + 0.5f) ) < 0.8f);
        CHECK(std::fabs(pp - 5.f) < 0.05f);
    }

    // Дрожь ±0.3 град, нулевое среднее: скорость и предиктор не должны гулять.
    {
        Track tr; double t = 0.0;
        for (int i = 0; i < 300; ++i) {
            const float j = (i & 1) ? 0.3f : -0.3f;
            update(tr, 10.f + j, 2.f - j, t);
            t += 1.0 / 60.0;
        }
        CHECK(std::fabs(tr.vx) < 6.f);
        CHECK(std::fabs(tr.vy) < 6.f);
        float py, pp; predict(tr, 1.0f / 60.f, py, pp);
        CHECK(std::fabs(py - 10.f) < 0.5f);
    }

    // Скачок больше порога — трек заново, скорость обнулена.
    {
        Track tr; double t = 0.0;
        for (int i = 0; i < 60; ++i) { update(tr, 0.f + i * 0.5f, 0.f, t); t += 1.0 / 60.0; }
        CHECK(std::fabs(tr.vx) > 10.f);
        update(tr, 90.f, 0.f, t);
        CHECK(tr.vx == 0.f);
        CHECK(std::fabs(tr.x - 90.f) < 1e-4f);
    }

    // Пауза чтения больше kMaxGap — трек заново.
    {
        Track tr; update(tr, 1.f, 1.f, 0.0);
        update(tr, 1.2f, 1.f, 0.5);
        CHECK(tr.vx == 0.f);
    }

    // Переход через 180: скорость остаётся корректной.
    {
        Track tr; double t = 0.0; float yaw = 175.f;
        for (int i = 0; i < 60; ++i) {
            update(tr, wrap180(yaw), 0.f, t);
            t += 1.0 / 60.0; yaw += 0.5f;
        }
        CHECK(std::fabs(tr.vx - 30.f) < 3.f);
    }

    // Потолок упреждения.
    {
        Track tr; tr.have = true; tr.vx = 500.f; tr.vy = 0.f; tr.t = 0.0;
        float py, pp; predict(tr, 0.2f, py, pp);
        CHECK(std::fabs(py - tr.x) <= kMaxLeadDeg + 1e-4f);
    }

    // Мусорные входы не портят трек.
    {
        Track tr; update(tr, 1.f, 1.f, 0.0);
        CHECK(!update(tr, NAN, 1.f, 0.01));
        CHECK(std::fabs(tr.x - 1.f) < 1e-6f);
    }

    if (fails) { std::printf("test_aim_predict: %d FAILED\n", fails); return 1; }
    std::printf("test_aim_predict: ok\n");
    return 0;
}
