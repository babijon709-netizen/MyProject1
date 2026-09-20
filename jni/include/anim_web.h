#pragma once
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
