// Стенд Touch_GetActive — снимка мультитача для фрикама.
//
// Как и touch_repeat_test.cpp, подключает НАСТОЯЩИЙ
// jni/src/Android_touch/TouchHelperA.cpp и выставляет его статики вручную
// (Touch_Init() не зовётся: /dev/input в песочнице нет).
//
// Что проверяется:
//   1. без пальцев и с невалидными аргументами — 0;
//   2. настоящие пальцы возвращаются с id и координатами, а синтетика
//      аима (60000) и фарма (61000..61002) отсекается — иначе injected-тач
//      уводил бы джойстик и камеру фрикама;
//   3. поднятый палец пропадает из снимка, id удержанного не меняется;
//   4. поворот и масштаб устройства считаются той же математикой, что и
//      ImGui MousePos (иначе хит-тесты фрикама разъехались бы с пальцами).
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "imgui.h"

// TouchHelperA.cpp зовёт ImGui::GetIO() в потоке чтения тачскрина, а нам он
// не нужен. Даём определение, не требуя конструктора ImGuiIO.
namespace ImGui {
ImGuiIO& GetIO() {
    alignas(8) static unsigned char raw[sizeof(ImGuiIO)]{};
    return *reinterpret_cast<ImGuiIO*>(raw);
}
} // namespace ImGui

#include "../../jni/src/Android_touch/TouchHelperA.cpp"

static int g_fail = 0;

static void check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "[ok]    " : "[ПРОВАЛ]", what);
    if (!ok) ++g_fail;
}

int main() {
    printf("Стенд Touch_GetActive: снимок мультитача\n");
    fdNum = 1;                    // один тач-девайс (device 0), как на телефоне
    orientation = 0;
    other_touch = false;
    scale_x = 1.f; scale_y = 1.f; // 1:1
    devMaxX = 2460; devMaxY = 1080;
    screenWidth = 2460.f; screenHeight = 1080.f;
    Touch_initialized = true;
    Touch_readOnly = false;
    memset(Finger, 0, sizeof(Finger));

    printf("\n1. Пусто и невалидные аргументы\n");
    {
        float x[4], y[4]; int id[4];
        check(Touch_GetActive(x, y, id, 4) == 0, "без пальцев — 0");
        check(Touch_GetActive(nullptr, y, id, 4) == 0, "xs=null — 0");
        check(Touch_GetActive(x, y, nullptr, 4) == 0, "ids=null — 0");
        check(Touch_GetActive(x, y, id, 0) == 0, "maxCount=0 — 0");
    }

    printf("\n2. Два настоящих пальца + синтетика аима/фарма\n");
    {
        Finger[0][0].isDown = true; Finger[0][0].x = 200;  Finger[0][0].y = 300; Finger[0][0].id = 10;
        Finger[0][1].isDown = true; Finger[0][1].x = 2000; Finger[0][1].y = 500; Finger[0][1].id = 11;
        Finger[0][9].isDown = true; Finger[0][9].x = 1200; Finger[0][9].y = 600; Finger[0][9].id = 60000;
        Finger[0][6].isDown = true; Finger[0][6].x = 500;  Finger[0][6].y = 800; Finger[0][6].id = 61000;
        float x[4], y[4]; int id[4];
        const int n = Touch_GetActive(x, y, id, 4);
        check(n == 2, "синтетика отсечена, настоящих двое");
        const bool ok0 = (id[0] == 10 && x[0] == 200.f && y[0] == 300.f);
        const bool ok1 = (id[1] == 11 && x[1] == 2000.f && y[1] == 500.f);
        check(ok0 && ok1, "id и координаты настоящих пальцев на месте");
    }

    printf("\n3. Подъём пальца\n");
    {
        Finger[0][0].isDown = false;
        float x[4], y[4]; int id[4];
        const int n = Touch_GetActive(x, y, id, 4);
        check(n == 1 && id[0] == 11, "остался один палец с тем же id");
        Finger[0][0].isDown = true;
    }

    printf("\n4. Поворот (orientation=1): та же математика, что у MousePos\n");
    {
        orientation = 1;
        float x[4], y[4]; int id[4];
        const int n = Touch_GetActive(x, y, id, 4);
        // x = yt = 300, y = screenHeight - xt = 1080 - 200 = 880
        check(n == 2 && x[0] == 300.f && y[0] == 880.f, "палец 10 -> (300, 880)");
        orientation = 0;
    }

    printf("\n5. Масштаб устройства 2:1\n");
    {
        scale_x = 2.f; scale_y = 2.f;
        float x[4], y[4]; int id[4];
        const int n = Touch_GetActive(x, y, id, 4);
        check(n == 2 && x[0] == 100.f && y[0] == 150.f, "палец 10 -> (100, 150)");
        scale_x = 1.f; scale_y = 1.f;
    }

    if (g_fail == 0) printf("\nСТЕНД ПРОЙДЕН\n");
    else printf("\nПРОВАЛОВ: %d\n", g_fail);
    return g_fail ? 1 : 0;
}
