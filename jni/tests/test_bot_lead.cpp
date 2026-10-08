// Хост-тест оценки скорости бота для упреждения аима (jni/src/bot_lead.h).
// Проверяет то, из-за чего жаловались на «аим уводит в сторону от бота»:
// упреждение должно считаться по скорости самого бота, не браться у чужой
// цели, не разгоняться телепортом/пулом и гаснуть, когда бот стоит.
#include "bot_lead.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

using botlead::Track;

static Vec3 V(float x, float y, float z) { return Vec3{x, y, z}; }

static bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

int main() {
    // Первый отсчёт: скорость нулевая, но окно оценки открыто.
    {
        Track t;
        assert(!botlead::fresh(t, 0.0));
        botlead::update(t, V(10.0F, 2.0F, -3.0F), 1.0);
        assert(botlead::fresh(t, 1.0));
        assert(t.vel.x == 0.0F && t.vel.y == 0.0F && t.vel.z == 0.0F);
    }

    // Ровный бег 10 м/с по -Z: сглаживание 0.5 должно вывести на измеренную.
    {
        Track t;
        double now = 0.0;
        for (int i = 0; i < 12; ++i) {
            botlead::update(t, V(0.0F, 0.0F, -1.0F * (float)i), now);
            now += 0.1;
        }
        assert(t.vel.z < 0.0F);
        assert(near(t.vel.z, -10.0F, 0.6F));
        assert(near(t.vel.x, 0.0F, 0.001F));
        assert(botlead::fresh(t, now));
    }

    // Диагональный бег: знаки и модуль по осям.
    {
        Track t;
        double now = 0.0;
        for (int i = 0; i < 12; ++i) {
            const float s = 0.4F * (float)i;   // 4 м/с по X и 4 м/с по Y
            botlead::update(t, V(s, s, 0.0F), now);
            now += 0.1;
        }
        assert(near(t.vel.x, 4.0F, 0.4F));
        assert(near(t.vel.y, 4.0F, 0.4F));
    }

    // Телепорт/смена владельца пула: 40 м за кадр — скорость остаётся прежней.
    {
        Track t;
        double now = 0.0;
        for (int i = 0; i < 12; ++i) {
            botlead::update(t, V(0.0F, 0.0F, -1.0F * (float)i), now);
            now += 0.1;
        }
        const float vel_z = t.vel.z;
        now += 0.05;
        botlead::update(t, V(40.0F, 0.0F, 60.0F), now);   // ~1440 м/с
        assert(t.vel.z == vel_z);
        assert(t.vel.x == 0.0F);
        assert(botlead::fresh(t, now));                    // позиция менялась только что
    }

    // Микрошаг (<2 см, сетевой дребезг) движением не считается и не сдвигает
    // опорную точку, пока пауза короче окна стоянки.
    {
        Track t;
        botlead::update(t, V(0.0F, 0.0F, 0.0F), 0.0);
        botlead::update(t, V(0.005F, 0.0F, 0.0F), 0.05);
        assert(t.vel.x == 0.0F && t.vel.y == 0.0F && t.vel.z == 0.0F);
        assert(t.ref.x == 0.0F && t.ref.z == 0.0F);
    }

    // Стоящий бот: через 0.3 с без движения скорость обнуляется, оценка мертва.
    {
        Track t;
        double now = 0.0;
        for (int i = 0; i < 12; ++i) {
            botlead::update(t, V(0.0F, 0.0F, -1.0F * (float)i), now);
            now += 0.1;
        }
        assert(t.vel.z < 0.0F);
        for (int i = 0; i < 6; ++i) {                       // стоим на месте 0.6 с
            botlead::update(t, V(0.0F, 0.0F, -11.0F), now);
            now += 0.1;
        }
        assert(t.vel.x == 0.0F && t.vel.y == 0.0F && t.vel.z == 0.0F);
        assert(!botlead::fresh(t, now));                    // движения не было давно
    }

    // Долгий разрыв в чтениях (>0.9 с): шаг не измеряется, старая оценка
    // сохраняется — далёкие боты перечитываются редко, терять её нельзя.
    {
        Track t;
        double now = 0.0;
        for (int i = 0; i < 12; ++i) {
            botlead::update(t, V(0.0F, 0.0F, -1.0F * (float)i), now);
            now += 0.1;
        }
        const float vel_z = t.vel.z;
        now += 1.5;
        botlead::update(t, V(0.0F, 0.0F, -20.0F), now);
        assert(t.vel.z == vel_z);
        assert(botlead::fresh(t, now));
    }

    // Мусорный отсчёт (NaN/бесконечность) и мусорное время не двигают оценку.
    {
        Track t;
        botlead::update(t, V(0.0F, 0.0F, 0.0F), 0.0);
        botlead::update(t, V(1.0F, 0.0F, 0.0F), 0.1);
        const Vec3 vel = t.vel;
        botlead::update(t, V(std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F), 0.2);
        botlead::update(t, V(5.0F, 0.0F, 0.0F), std::numeric_limits<double>::infinity());
        assert(t.vel.x == vel.x && t.vel.y == vel.y && t.vel.z == vel.z);
    }

    // Оценка «свежая» только в пределах окна: даже без обнуления скорости
    // (движение микрошагами) она перестаёт годиться.
    {
        Track t;
        botlead::update(t, V(0.0F, 0.0F, 0.0F), 0.0);
        assert(botlead::fresh(t, 0.1));
        assert(!botlead::fresh(t, 0.5));
    }
    std::puts("bot-lead tests passed");
    return 0;
}
