// Стенд тач-слоя (TouchHelperA): что именно уходит в uinput.
//
// Файл подключает НАСТОЯЩИЙ jni/src/Android_touch/TouchHelperA.cpp и подменяет
// только окружение: вместо /dev/uinput (его в песочнице нет) пакеты пишутся в
// pipe, а из pipe читаются и разбираются — видно каждое событие и каждый пакет.
// Touch_Init() при этом не зовётся вовсе: тест сам выставляет статики слоя так,
// как их выставил бы настоящий init (один тач-девайс, масштаб 1:1).
//
// Что проверяется (это и есть регресс-тест на «аим сломался»):
//   1. первый down уходит с префиксом BTN_TOUCH/BTN_TOOL_FINGER (нажатие);
//   2. палец аимбота: пакет уходит, даже если он байт в байт повторяет прошлый
//      (UpdateAim каждый кадр пересылает ту же точку — «держим тач живым»);
//   3. отпускание пальца аимбота уходит и помечено BTN_TOUCH 0;
//   4. пальцы автофарма: повтор при неизменной позиции НЕ уходит (ради этого
//      пропуск и вводился — на нём держится отсутствие лага оверлея);
//   5. при поднятом пальце аимбота пропуск повторов снова работает (экономия не
//      потеряна), а движение пальцев автофарма всегда уходит.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#include "imgui.h"

// TouchHelperA.cpp зовёт ImGui::GetIO() в потоке чтения тачскрина (раскладка
// окна ImGui), а нам он не нужен. Даём определение, не требуя конструктора
// ImGuiIO (он в imgui.h только объявлен, тела в сборке нет).
namespace ImGui {
ImGuiIO& GetIO() {
    alignas(8) static unsigned char raw[sizeof(ImGuiIO)]{};
    return *reinterpret_cast<ImGuiIO*>(raw);
}
} // namespace ImGui

#include "../../jni/src/Android_touch/TouchHelperA.cpp"

// ---------------------------------------------------------------- окружение --

static int  g_pipe[2] = {-1, -1};

struct Drain {
    int packets   = 0;   // SYN_REPORT'ов, то есть отправленных пакетов
    int events    = 0;   // событий во всех пакетах
    int btnPress  = 0;   // EV_KEY BTN_TOUCH=1 (нажатие синтетического тача)
    int btnFinger = 0;   // EV_KEY BTN_TOOL_FINGER=1
    int btnUp     = 0;   // EV_KEY BTN_TOUCH=0
};

static Drain drain() {
    Drain d;
    struct input_event ev[256];
    for (;;) {
        const ssize_t n = read(g_pipe[0], ev, sizeof(ev));
        if (n <= 0) break;
        const int cnt = (int)(n / (ssize_t)sizeof(struct input_event));
        for (int i = 0; i < cnt; ++i) {
            ++d.events;
            if (ev[i].type == EV_SYN && ev[i].code == SYN_REPORT) ++d.packets;
            if (ev[i].type == EV_KEY && ev[i].code == BTN_TOUCH        && ev[i].value == 1) ++d.btnPress;
            if (ev[i].type == EV_KEY && ev[i].code == BTN_TOOL_FINGER  && ev[i].value == 1) ++d.btnFinger;
            if (ev[i].type == EV_KEY && ev[i].code == BTN_TOUCH        && ev[i].value == 0) ++d.btnUp;
        }
    }
    return d;
}

static void touchSetup() {
    if (pipe(g_pipe) != 0) { perror("pipe"); exit(2); }
    fcntl(g_pipe[0], F_SETFL, O_NONBLOCK);
    nowfd = g_pipe[1];            // «uinput»: всё, что слой пишет, читаем из pipe
    fdNum = 1;                    // один тач-девайс (device 0), как на телефоне
    orientation = 0;
    scale_x = 1.f; scale_y = 1.f; // 1:1, как при масштабе устройства 1.0
    devMaxX = devMaxY = 0;        // без ограничения координат
    screenHeight = 1080.f; screenWidth = 2460.f;
    Touch_initialized = true;
    Touch_readOnly = false;
}

// ------------------------------------------------------------------- тесты --

static int g_fail = 0;

static void check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "[ok]    " : "[ПРОВАЛ]", what);
    if (!ok) ++g_fail;
}

static void checkEq(int got, int want, const char* what) {
    if (got == want) {
        printf("  [ok]    %s: %d\n", what, got);
    } else {
        printf("  [ПРОВАЛ] %s: получено %d, ожидалось %d\n", what, got, want);
        ++g_fail;
    }
}

// 60 «удержаний» пальца аимбота в одной точке: именно этот участок глушился
// пропуском повторов и именно на нём аим перестал держать прицел.
static const int kHoldTicks = 60;

int main() {
    printf("Стенд тач-слоя: что уходит в uinput\n");
    touchSetup();

    printf("\n1. Первый down аимбота\n");
    Touch_Down(1200.f, 600.f);
    {
        const Drain d = drain();
        checkEq(d.packets, 1, "пакетов");
        check(d.btnPress == 1 && d.btnFinger == 1,
              "нажатие ушло вместе с первым пакетом (BTN_TOUCH 1 + BTN_TOOL_FINGER 1)");
    }

    printf("\n2. Удержание аимбота: та же точка кадр за кадром\n");
    {
        Drain d;
        for (int i = 0; i < kHoldTicks; ++i) {
            Touch_Move(1200.f, 600.f);
            const Drain one = drain();
            d.packets += one.packets; d.events += one.events;
        }
        checkEq(d.packets, kHoldTicks, "пакетов на 60 удержаний");
        checkEq(d.events, kHoldTicks * 7, "событий (6 MT + SYN_REPORT на пакет)");
        printf("          (до правки простоя здесь было 0 пакетов — камера «выпадала» из удержания)\n");
    }

    printf("\n3. Отпускание пальца аимбота\n");
    {
        Touch_Up();
        const Drain d = drain();
        checkEq(d.packets, 1, "пакетов на отпускание");
        check(d.btnUp == 1, "отпускание помечено BTN_TOUCH 0");
    }

    printf("\n4. Пальцы автофарма: постановка и простой (пропуск повторов)\n");
    {
        unsigned long long c0 = 0, s0 = 0, c1 = 0, s1 = 0;
        double ms = 0.0;
        Touch_UploadStats(c0, ms, s0);
        Touch_Down_N(0, 300.f, 900.f);      // стик
        Touch_Down_N(1, 1200.f, 600.f);     // камера
        Touch_Down_N(2, 1500.f, 640.f);     // удар
        const Drain set = drain();
        checkEq(set.packets, 3, "пакетов на постановку трёх пальцев");

        Drain rep;
        for (int i = 0; i < 20; ++i) {
            Touch_Down_N(0, 300.f, 900.f);
            Touch_Down_N(1, 1200.f, 600.f);
            Touch_Down_N(2, 1500.f, 640.f);
            const Drain one = drain();
            rep.packets += one.packets; rep.events += one.events;
        }
        Touch_UploadStats(c1, ms, s1);
        checkEq(rep.packets, 0, "пакетов на 60 повторов в простое");
        checkEq(rep.events, 0, "событий на 60 повторов в простое");
        checkEq((int)(c1 - c0), 63, "вызовов Upload за участок");
        checkEq((int)(s1 - s0), 60, "из них пропущено как повтор");
    }

    printf("\n5. Автофарм реально водит пальцами — пакеты уходят\n");
    {
        Drain d;
        for (int i = 1; i <= 10; ++i) {
            Touch_Down_N(1, 1200.f + i, 600.f);   // камера едет по 1 px за такт
            d.packets += drain().packets;
        }
        checkEq(d.packets, 10, "пакетов на 10 сдвигов камеры");
        // Вызов с уже стоящей на месте точкой пакет не порождает — это и есть
        // пропуск повторов, он виден и здесь.
        Touch_Down_N(1, 1210.f, 600.f);
        checkEq(drain().packets, 0, "пакетов на вызов без сдвига");
    }

    printf("\n6. Аимбот снова взял камеру (палец вниз, точка прежняя)\n");
    {
        Touch_Down(1200.f, 600.f);
        const Drain d = drain();
        checkEq(d.packets, 1, "пакетов на постановку пальца");
        // BTN_TOUCH уже нажат (его выставили пальцы автофарма), поэтому второй
        // раз префикс не пишется — устройство и так числится нажатым.
        check(d.btnPress == 0, "повторного BTN_TOUCH 1 не пишем: он уже нажат");
    }

    printf("\n7. Аимбот держит, автофарм в это время на паузе\n");
    {
        Drain d;
        for (int i = 0; i < kHoldTicks; ++i) {
            Touch_Move(1200.f, 600.f);
            const Drain one = drain();
            d.packets += one.packets; d.events += one.events;
        }
        checkEq(d.packets, kHoldTicks, "пакетов на 60 удержаний (палец аимбота не глушится)");
        check(d.events >= kHoldTicks * 7, "каждое удержание — полный пакет");
    }

    printf("\n8. Аимбот отпущен — экономия на простое автофарма возвращается\n");
    {
        Touch_Up();
        drain();
        Drain rep;
        for (int i = 0; i < 30; ++i) {
            Touch_Down_N(0, 300.f, 900.f);
            Touch_Down_N(1, 1210.f, 600.f);
            Touch_Down_N(2, 1500.f, 640.f);
            const Drain one = drain();
            rep.packets += one.packets; rep.events += one.events;
        }
        checkEq(rep.packets, 0, "пакетов на 90 повторов без пальца аимбота");
        checkEq(rep.events, 0, "событий на 90 повторов");
        Touch_Up_N(0); Touch_Up_N(1); Touch_Up_N(2);
        checkEq(drain().packets, 3, "пакетов на снятие трёх пальцев автофарма");
    }

    if (g_fail) {
        printf("\nСТЕНД НЕ ПРОЙДЕН: провалов %d\n", g_fail);
        return 1;
    }
    printf("\nСТЕНД ПРОЙДЕН\n");
    return 0;
}
