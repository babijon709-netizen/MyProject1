#pragma once
#include <cmath>
// Готовые анимации из интернета (не наши, взяты как есть):
//  1. easeOutBack — шпаргалка easings.net, файл src/easings.yml:
//     https://github.com/ai/easings.net/blob/master/src/easings.yml
//     (уравнения Роберта Пеннера; Math.pow(x-1,3/2) записан умножением —
//     математика та же, без вызова pow).
//  2. Second-order dynamics — «желе»-пружина из доклада t3ssel8r
//     (https://youtu.be/KPoeNZZ6H4s), порт gist metaphore:
//     https://gist.github.com/metaphore/51aa5bfb699365f7701cfb38f402cd3d
//     (Java переписана на C++ один в один, помеченное «glue» — наша обвязка:
//     параметры по умолчанию и конструктор).
//
// Что: окно выскакивает из кнопки (back) и догоняет палец желешкой (sod).

// easeOutBack(net): x = 0..1, возвращает 0..~1.1 — с перелётом в конце.
inline float NetEaseOutBack(float x) {
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.f;
    const float u = x - 1.f;
    return 1.f + c3 * u * u * u + c1 * u * u;
}

// Second-order dynamics 2D: состояние + configure/moveInstant/update —
// дословно из gist, коэффициенты k1/k2/k3 и стабилизатор k2stable — тоже.
struct NetSecondOrder2D {
    float f = 3.0f, z = 0.35f, r = 1.0f; // glue: частота, затухание (<1 = желе), отклик
    float k1 = 0.f, k2 = 0.f, k3 = 0.f;  // динамические константы
    float pDstX = 0.f, pDstY = 0.f;      // предыдущая цель
    float posX = 0.f, posY = 0.f;        // состояние: позиция
    float accX = 0.f, accY = 0.f;        // состояние: «ускорение» (как в gist)
    float velX = 0.f, velY = 0.f;        // состояние: шаг позиции за кадр

    NetSecondOrder2D() { configure(f, z, r); } // glue
    NetSecondOrder2D(float nf, float nz, float nr) : f(nf), z(nz), r(nr) { configure(f, z, r); } // glue

    void configure(float nf, float nz, float nr) {
        f = nf; z = nz; r = nr; // glue: запоминаем параметры
        const float pi = 3.14159265f;
        k1 = z / (pi * f);
        k2 = 1.f / ((2.f * pi * f) * (2.f * pi * f));
        k3 = r * z / (2.f * pi * f);
    }
    void reset(float dstX, float dstY) { configure(f, z, r); moveInstant(dstX, dstY); } // glue
    void moveInstant(float dstX, float dstY) {
        pDstX = posX = dstX;
        pDstY = posY = dstY;
        accX = accY = 0.f;
    }
    void update(float deltaTime, float dstX, float dstY) {
        // Оценка скорости цели.
        float xd_x = (dstX - pDstX) / deltaTime;
        float xd_y = (dstY - pDstY) / deltaTime;
        pDstX = dstX;
        pDstY = dstY;

        // Кламп k2 гарантирует стабильность без джиттера.
        float a = deltaTime * deltaTime / 2.f + deltaTime * k1 / 2.f;
        float b = deltaTime * k1;
        float m = a > b ? a : b;
        float k2Stable = k2 > m ? k2 : m;

        // Интеграция позиции скоростью.
        velX = deltaTime * accX;
        velY = deltaTime * accY;
        posX = posX + velX;
        posY = posY + velY;

        // Интеграция скорости ускорением.
        accX = accX + deltaTime * (dstX + k3 * xd_x - posX - k1 * accX) / k2Stable;
        accY = accY + deltaTime * (dstY + k3 * xd_y - posY - k1 * accY) / k2Stable;
    }
    void setPos(float x, float y) { posX = x; posY = y; } // glue: возврат клампа в состояние
};

// 3. Mesh Deformer — Catlike Coding (Jasper Flick), 2D-порт:
//    https://catlikecoding.com/unity/tutorials/mesh-deformation/
//    У каждой вершины сетки: пружина к покою + демпфер + скорость
//    (AddForceToVertex / UpdateVertex из туториала). Соседи тянут друг друга
//    (стандартная ткань), жёсткость чуть разная по сетке — иначе все точки
//    отстают одинаково и рёбра остаются прямыми. Glue: размер сетки, map().
constexpr int kJellyNX = 11;
constexpr int kJellyNY = 9;
struct NetJellyMesh {
    struct Vert { float x = 0.f, y = 0.f, vx = 0.f, vy = 0.f; };
    Vert  p[kJellyNY][kJellyNX];
    float rx = 0.f, ry = 0.f, rw = 1.f, rh = 1.f;
    bool  inited = false;

    void snap(float x, float y, float w, float h) {
        rx = x; ry = y; rw = w > 1.f ? w : 1.f; rh = h > 1.f ? h : 1.f;
        for (int j = 0; j < kJellyNY; ++j)
            for (int i = 0; i < kJellyNX; ++i) {
                p[j][i].x = restX(i); p[j][i].y = restY(j);
                p[j][i].vx = p[j][i].vy = 0.f;
            }
        inited = true;
    }
    float restX(int i) const { return rx + rw * (float)i / (float)(kJellyNX - 1); }
    float restY(int j) const { return ry + rh * (float)j / (float)(kJellyNY - 1); }

    void tick(float dt, float x, float y, float w, float h) {
        if (dt < 1e-4f) dt = 1e-4f;
        if (dt > 0.05f) dt = 0.05f;
        if (!inited) { snap(x, y, w, h); return; }
        rx = x; ry = y; rw = w > 1.f ? w : 1.f; rh = h > 1.f ? h : 1.f;
        // Catlike: velocity -= displacement * springForce * dt;
        //          velocity *= 1 - damping * dt;
        //          displaced += velocity * dt;
        const float springForce = 28.f;
        const float damping     = 6.5f;
        const float dampMul     = 1.f - (damping * dt < 0.9f ? damping * dt : 0.9f);
        for (int j = 0; j < kJellyNY; ++j) {
            const float uy = (float)j / (float)(kJellyNY - 1);
            for (int i = 0; i < kJellyNX; ++i) {
                const float ux = (float)i / (float)(kJellyNX - 1);
                // Жёсткость разная: верх (хватка) туже, низ жиже; вдоль ребра
                // синус, чтобы середина стороны отставала от углов.
                const float vary = 0.62f + 0.28f * (1.f - uy)
                                 + 0.16f * sinf(ux * 6.28318f + uy * 2.4f);
                Vert& v = p[j][i];
                v.vx -= (v.x - restX(i)) * springForce * vary * dt;
                v.vy -= (v.y - restY(j)) * springForce * vary * dt;
                v.vx *= dampMul;
                v.vy *= dampMul;
            }
        }
        // Соседи: сила пропорциональна растяжению относительно покоя (ткань).
        const float kN = 22.f;
        for (int j = 0; j < kJellyNY; ++j)
            for (int i = 0; i < kJellyNX; ++i) {
                Vert& v = p[j][i];
                auto pull = [&](int ni, int nj) {
                    const float rlx = restX(ni) - restX(i);
                    const float rly = restY(nj) - restY(j);
                    const float cx = p[nj][ni].x - v.x;
                    const float cy = p[nj][ni].y - v.y;
                    v.vx += (cx - rlx) * kN * dt;
                    v.vy += (cy - rly) * kN * dt;
                };
                if (i + 1 < kJellyNX) pull(i + 1, j);
                if (j + 1 < kJellyNY) pull(i, j + 1);
                if (i > 0)            pull(i - 1, j);
                if (j > 0)            pull(i, j - 1);
            }
        for (int j = 0; j < kJellyNY; ++j)
            for (int i = 0; i < kJellyNX; ++i) {
                p[j][i].x += p[j][i].vx * dt;
                p[j][i].y += p[j][i].vy * dt;
            }
    }
    // Точка покоя (прямоугольник окна) → смещённая сеткой.
    void map(float px, float py, float& ox, float& oy) const {
        float u = (px - rx) / rw;
        float v = (py - ry) / rh;
        u = u < -0.15f ? -0.15f : (u > 1.15f ? 1.15f : u);
        v = v < -0.15f ? -0.15f : (v > 1.15f ? 1.15f : v);
        const float fx = u * (float)(kJellyNX - 1);
        const float fy = v * (float)(kJellyNY - 1);
        int i0 = (int)fx, j0 = (int)fy;
        if (i0 < 0) i0 = 0; if (i0 > kJellyNX - 2) i0 = kJellyNX - 2;
        if (j0 < 0) j0 = 0; if (j0 > kJellyNY - 2) j0 = kJellyNY - 2;
        const float tx = fx - (float)i0, ty = fy - (float)j0;
        const Vert& a = p[j0][i0];
        const Vert& b = p[j0][i0 + 1];
        const Vert& c = p[j0 + 1][i0];
        const Vert& d = p[j0 + 1][i0 + 1];
        ox = (a.x * (1.f - tx) + b.x * tx) * (1.f - ty) + (c.x * (1.f - tx) + d.x * tx) * ty;
        oy = (a.y * (1.f - tx) + b.y * tx) * (1.f - ty) + (c.y * (1.f - tx) + d.y * tx) * ty;
    }
};
