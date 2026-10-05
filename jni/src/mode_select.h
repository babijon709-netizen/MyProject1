#pragma once
// mode_select.h — экран выбора способа чтения ПЕРЕД запуском чита.
//
// Минималистичный экран: заголовок «Способ чтения» по центру сверху и четыре
// кнопки сеткой 2х2 — «Прямое чтение» (/proc/<pid>/mem, как раньше),
// «FT-Драйвер» (сокет ядерного драйвера, kernel_io.h), «QX-Драйвер»
// (/dev-узел драйвера QX) и «RT-Драйвер» (хук syscall ioctl, RT-hook).
// Без фона, подписей и строк состояния: причины отказа приходят тостом по
// тапу, повторный тап по кнопке драйвера перепроверяет драйвер (прошить
// .sh можно и после запуска).
//
// Выбор сохраняется в .mode рядом с .lang/.accent (каталог конфигов), но
// экран всё равно показывается на каждом запуске — драйвер живёт до
// перезагрузки, и «нашёл/не нашёл» нужно видеть каждый раз.
//
// Пока экран на виду, весь ввод забирает оверлей (Touch_BlockRect на весь
// экран) — в игру не уходит ни одного пальца, тап мимо кнопок не «нажмёт»
// что-то в игре под оверлеем.

#include <sys/stat.h>
#include <unistd.h>                 // geteuid — статус root на экране

#include <fstream>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"          // ImMin

#include "app_state.h"               // main_thread_flag
#include "main.h"                    // drawBegin/drawEnd
#include "mem_io.h"                  // procfs_transport (дефолт)
#include "kernel_io.h"               // kernel_transport, driver_available
#include "game.h"                    // esp_set_mem_backend
#include "config.h"                  // CfgDir(), RestoreLang(), RestoreAccent()
#include "theme.h"                   // C::/R::, ApplyTheme
#include "hud.h"                     // VisibleScreen
#include "audio.h"                   // PlaySound
#include "str.h"                     // XS()
#include "ui_util.h"                 // EaseInOut, WasTappedHere

namespace modesel {

enum Mode {
    MODE_EXTERNAL = 0,   // /proc/<pid>/mem — «Прямое чтение»
    MODE_KERNEL   = 1,   // сокет драйвера bing-класса — «FT-Драйвер»
    MODE_RT_DEV   = 2,   // /dev-узел драйвера RT-Dev — «RT-Dev»
    MODE_RT       = 3,   // хук syscall ioctl (RT-hook) — «RT-Драйвер»
};

// ---- сохранение выбора (.mode, по образцу .lang/.accent в config.cpp) -----

static std::string ModePath() {
    return std::string(CfgDir()) + XS(".mode");
}

static void RememberMode(Mode m) {
    mkdir(CfgDir(), 0777);
    std::ofstream f(ModePath(), std::ios::trunc);
    if (f) f << (m == MODE_KERNEL ? "kernel" : m == MODE_RT_DEV ? "rt_dev"
                       : m == MODE_RT ? "rt" : "external");
}

// ---- кнопка режима (только текст, без подписи) -----------------------------
//
// style_on=false — драйвер не отвечает: корпус приглушён, но тап всё равно
// возвращается вызывающему (тот покажет причину тостом и перепроверит).
static bool ModeButton(const char* id, float w, float h, const char* title,
                       bool style_on, bool accent, float fs) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, {w, h});

    const bool tapped = WasTappedHere();
    const bool held   = ImGui::IsItemActive() && ImGui::IsItemHovered();
    const bool hov    = ImGui::IsItemHovered();

    auto*  dl = ImGui::GetWindowDrawList();
    ImFont* fn = ImGui::GetFont();

    // Корпус: обычная карточка; у FT-Драйвера — рамка акцентом. Нажатие —
    // затемнение корпуса.
    ImVec4 body = style_on ? (accent ? C::Card() : C::LeftBg()) : C::TrkOff();
    if (held) body.w *= 0.82f;
    else if (hov) body.w *= 0.94f;
    dl->AddRectFilled(p, {p.x + w, p.y + h}, C::U(body), R::Btn);
    if (style_on && accent) {
        ImVec4 ring = C::Acc();
        ring.w *= held ? 0.85f : 1.f;
        dl->AddRect(p, {p.x + w, p.y + h}, C::U(ring), R::Btn, 0.f, 3.f);
    }

    const float tfs = fs * 1.25f;
    const ImVec2 ts = fn->CalcTextSizeA(tfs, FLT_MAX, 0.f, title);
    dl->AddText(fn, tfs, {p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f},
                C::U(style_on ? C::Txt() : C::Dim()), title);
    return tapped;
}

// ---- экран ---------------------------------------------------------------
//
// Возвращает true, если режим выбран и применён к g_mem. false — приложение
// закрыли (сигнал/терминал), не дав выбрать: вызывающий выходит тем же
// порядком teardown, что и обычный выход из main().
static bool RunModeSelect() {
    // Язык и акцент применяем заранее: в main() это делается позже (после
    // потока привязки), а на этом экране подписи уже должны быть на месте.
    RestoreLang();
    RestoreAccent();
    ApplyTheme();

    bool drv = false;          // FT-драйвер отвечает на пробу семейства
    int  fam = -1;             // номер семейства сокета FT-драйвера
    drv = memio::driver_available(&fam);
    bool rt_dev = memio::rt_dev_available(); // RT-Dev драйвер найден и отвечает
    bool rt = memio::rt_available();         // RT-драйвер: хук ioctl отвечает

    float fade = 0.f;          // появление 0..1
    int   picked = -1;         // -1 — ещё не выбрали

    while (main_thread_flag.load() && picked < 0) {
        drawBegin();
        auto& io = ImGui::GetIO();
        float dt = io.DeltaTime;
        if (dt > 0.1f) dt = 0.1f;
        fade = ImMin(fade + dt * 1.6f, 1.f);

        float vw = 0.f, vh = 0.f;
        VisibleScreen(vw, vh);
        // Экран выбора забирает весь ввод: в игру под оверлеем не уходит
        // ни одного пальца, пока режим не выбран.
        Touch_BlockRect(true, 0.f, 0.f, vw, vh);

        // ---- окно на весь видимый экран (как меню, без заголовка) -------
        ImGuiWindowFlags wf = ImGuiWindowFlags_NoResize    | ImGuiWindowFlags_NoCollapse
                            | ImGuiWindowFlags_NoTitleBar  | ImGuiWindowFlags_NoScrollbar
                            | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground
                            | ImGuiWindowFlags_NoMove      | ImGuiWindowFlags_NoNav
                            | ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::SetNextWindowPos({0.f, 0.f}, ImGuiCond_Always);
        ImGui::SetNextWindowSize({vw, vh}, ImGuiCond_Always);
        ImGui::Begin("##mode_select", nullptr, wf);
        {
            auto*  dl = ImGui::GetWindowDrawList();
            ImFont* fn = ImGui::GetFont();
            const float fs = ImGui::GetFontSize();

            const float t = EaseInOut(fade);
            const float slide = (1.f - t) * -30.f;   // въезжает сверху

            // Заголовок + кнопки — единый блок ровно по центру экрана
            // (по горизонтали и по вертикали): без фона, только текст
            // с тенью для читаемости поверх экрана под оверлеем.
            // Кнопок четыре — сетка 2х2 (в один ряд на телефоне уже не
            // влезают), порядок: экстернал и FT сверху, QX и RT снизу.
            const char* title = XS("Способ чтения");
            const float tfs = fs * 1.55f;
            const ImVec2 ts = fn->CalcTextSizeA(tfs, FLT_MAX, 0.f, title);
            const float gap  = 14.f;
            const float btnH = fs * 3.4f;
            const float btnW = ImMin((vw - 36.f - gap) / 2.f, 300.f);
            const float tGap = fs * 1.3f;                 // заголовок -> кнопки
            const float blockH = tfs + tGap + btnH * 2.f + gap;
            const float ty = (vh - blockH) * 0.5f + slide;   // центр по вертикали
            const float tx = (vw - ts.x) * 0.5f;
            dl->AddText(fn, tfs, {tx + 2.f, ty + 2.f}, IM_COL32(0, 0, 0, 110), title);
            dl->AddText(fn, tfs, {tx, ty}, C::U(C::Txt()), title);

            // Сетка 2х2 под заголовком.
            const float rowW = btnW * 2.f + gap;
            const float by   = ty + tfs + tGap;
            const float bx   = (vw - rowW) * 0.5f;
            const float by2  = by + btnH + gap;           // второй ряд

            ImGui::SetCursorScreenPos({bx, by});
            if (ModeButton("##mode_ext", btnW, btnH,
                           XS("Прямое чтение"), true, false, fs)) {
                picked = MODE_EXTERNAL;
                PlaySound(SND_CLICK);
                ShowToast(XS("Прямое чтение"));
            }

            ImGui::SetCursorScreenPos({bx + btnW + gap, by});
            if (ModeButton("##mode_ft", btnW, btnH,
                           XS("FT-Драйвер"), drv, true, fs)) {
                // Тап по FT-Драйверу. Никаких проб команд здесь НЕТ (слепая
                // проба неизвестного кода может потревожить драйвер перед
                // привязкой). Порядок:
                //   1) не root — сокет драйвера создаётся только
                //      привилегированным процессом, маячок без root не виден;
                //   2) драйвер не найден — рескан с чистого листа (прошить
                //      .sh можно и после запуска чита), нашёлся — выбираем;
                //   3) драйвер отвечает — режим принят. Настоящая проверка
                //      транспорта — привязка к игре: bind читает ELF-заголовок
                //      libil2cpp ЧЕРЕЗ ДРАЙВЕР, результат виден тостом.
                if (geteuid() != 0) {
                    PlaySound(SND_CLICK);
                    ShowToast(XS("FT-Драйвер: нет root — запусти чит от su"));
                } else if (!drv) {
                    memio::kernel_transport().invalidate();
                    drv = memio::driver_available(&fam);
                    if (drv) {
                        picked = MODE_KERNEL;
                        PlaySound(SND_SUCCESS);
                    } else {
                        PlaySound(SND_CLICK);
                        ShowToast(XS("Драйвер не найден — прошей .sh своей версии ядра"));
                    }
                } else {
                    picked = MODE_KERNEL;
                    PlaySound(SND_SUCCESS);
                }
            }

            ImGui::SetCursorScreenPos({bx, by2});
            if (ModeButton("##mode_rt_dev", btnW, btnH,
                           XS("RT-Dev"), rt_dev, true, fs)) {
                // Тап по RT-Dev Драйверу. Порядок:
                //   1) не root — узел драйвера открывается только привилегированным;
                //   2) не найден — рескан с чистого листа (прошить .sh можно и
                //      после запуска чита, имя узла при этом новое);
                //   3) отвечает — режим принят, настоящий экзамен — привязка
                //      к игре (bind читает ELF-заголовок libil2cpp через RT-Dev).
                if (geteuid() != 0) {
                    PlaySound(SND_CLICK);
                    ShowToast(XS("RT-Dev: нет root — запусти чит от su"));
                } else if (!rt_dev) {
                    memio::rt_dev_transport().invalidate();
                    rt_dev = memio::rt_dev_available();
                    if (rt_dev) {
                        picked = MODE_RT_DEV;
                        PlaySound(SND_SUCCESS);
                    } else {
                        PlaySound(SND_CLICK);
                        ShowToast(XS("RT-Dev не найден — прошей .sh своей версии ядра"));
                    }
                } else {
                    picked = MODE_RT_DEV;
                    PlaySound(SND_SUCCESS);
                }
            }

            ImGui::SetCursorScreenPos({bx + btnW + gap, by2});
            if (ModeButton("##mode_rt", btnW, btnH,
                           XS("RT-Драйвер"), rt, true, fs)) {
                // Тап по RT-Драйверу. Драйвер этого класса не имеет ни
                // сокета, ни /dev-узла — он подменяет syscall ioctl
                // (sys_call_table[29]), поэтому «рескан» здесь — просто
                // повторная проба чтения своей памяти через ioctl(-1,
                // 0x259). Порядок прежний:
                //   1) не root — команды 0x259 гейтятся (uid/euid/capable);
                //   2) хук не отвечает — перепробовать (прошить .sh можно и
                //      после запуска чита; хук снимается командой 0x25e);
                //   3) отвечает — режим принят, настоящий экзамен —
                //      привязка к игре (bind читает ELF-заголовок
                //      libil2cpp через перехваченный ioctl).
                if (geteuid() != 0) {
                    PlaySound(SND_CLICK);
                    ShowToast(XS("RT: нет root — запусти чит от su"));
                } else if (!rt) {
                    memio::rt_transport().invalidate();
                    rt = memio::rt_available();
                    if (rt) {
                        picked = MODE_RT;
                        PlaySound(SND_SUCCESS);
                    } else {
                        PlaySound(SND_CLICK);
                        ShowToast(XS("RT не найден — прошей .sh своей версии ядра"));
                    }
                } else {
                    picked = MODE_RT;
                    PlaySound(SND_SUCCESS);
                }
            }
        }
        ImGui::End();

        drawEnd();
    }

    Touch_BlockRect(false, 0.f, 0.f, 0.f, 0.f);

    // Применяем выбранный (или дефолтный при выходе через сигнал) транспорт.
    // Достаточно одного вызова до первого bind — весь код чтения выше по
    // стеку работает через memio::Transport и про режимы не знает (mem_io.h).
    const Mode mode = (picked == MODE_KERNEL) ? MODE_KERNEL
                     : (picked == MODE_RT_DEV) ? MODE_RT_DEV
                     : (picked == MODE_RT)     ? MODE_RT
                                               : MODE_EXTERNAL;
    if (picked >= 0) RememberMode(mode);
    esp_set_mem_backend(mode == MODE_KERNEL ? ESP_MEM_FT
                        : mode == MODE_RT_DEV ? ESP_MEM_RT_DEV
                        : mode == MODE_RT   ? ESP_MEM_RT
                                            : ESP_MEM_EXTERNAL);
    return picked >= 0;
}

}  // namespace modesel
