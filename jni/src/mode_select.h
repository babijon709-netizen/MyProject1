#pragma once
// mode_select.h — экран выбора способа чтения ПЕРЕД запуском чита.
//
// Минималистичный экран: заголовок «Способ чтения» по центру сверху и четыре
// кнопки сеткой 2х2 — «Прямое чтение», «FT-Драйвер», «RT-Dev» (случайный
// `/dev/[A-GNY]{1,6}`, ioctl 0x801/0x802) и «RT-Hook» (syscall ioctl 0x259/0x25a).
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
    MODE_RT_DEV   = 2,   // /dev/[A-GNY]{1,6} RT-Dev — «RT-Dev»
    MODE_RT_HOOK  = 3,   // syscall ioctl hookdd/gynhdd — «RT-Hook»
};

// ---- сохранение выбора (.mode, по образцу .lang/.accent в config.cpp) -----

static std::string ModePath() {
    return std::string(CfgDir()) + XS(".mode");
}

static void RememberMode(Mode m) {
    mkdir(CfgDir(), 0777);
    std::ofstream f(ModePath(), std::ios::trunc);
    if (f) f << (m == MODE_KERNEL ? "kernel" : m == MODE_RT_DEV ? "rt-dev"
                       : m == MODE_RT_HOOK ? "rt-hook" : "external");
}

// Между версиями «rt» на короткое время означал только RT-Dev. Экран выбора
// показывается всегда, но мигрируем запись сразу, чтобы CLI dump прочитал
// однозначное имя даже до следующего выбора пользователем.
static void MigrateLegacyRtMode() {
    std::ifstream in(ModePath());
    std::string value;
    if (!in || !std::getline(in, value) || value != "rt") return;
    RememberMode(MODE_RT_DEV);
}

// style_on=false — транспорт не отвечает: карточка приглушена, но остаётся
// кликабельной для повторной проверки после загрузки драйвера.
static bool ModeButton(const char* id, float w, float h, const char* title,
                       bool style_on, bool driver, float fs) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, {w, h});

    const bool tapped = WasTappedHere();
    const bool held = ImGui::IsItemActive() && ImGui::IsItemHovered();
    const bool hov = ImGui::IsItemHovered();
    auto* dl = ImGui::GetWindowDrawList();
    ImFont* fn = ImGui::GetFont();

    ImVec4 body = style_on ? C::Card() : C::TrkOff();
    if (held) {
        body.x *= 0.88f; body.y *= 0.88f; body.z *= 0.88f;
    } else if (hov) {
        const ImVec4 raised = C::Raised();
        body = {Lerpf(body.x, raised.x, 0.65f), Lerpf(body.y, raised.y, 0.65f),
                Lerpf(body.z, raised.z, 0.65f), 1.f};
    }
    dl->AddRectFilled({p.x + 1.f, p.y + 3.f}, {p.x + w + 1.f, p.y + h + 3.f},
                      IM_COL32(0, 0, 0, g_darkTheme ? 34 : 16), R::Btn);
    dl->AddRectFilled(p, {p.x + w, p.y + h}, C::U(body), R::Btn);
    dl->AddRect(p, {p.x + w, p.y + h}, C::UA(C::Sep(), 0.9f), R::Btn, 0, 1.f);
    if (driver)
        dl->AddRectFilled({p.x, p.y}, {p.x + 4.f, p.y + h},
                          C::UA(style_on ? C::Acc() : C::Dim(), 0.95f), 2.f);

    const float tfs = fs * 1.05f;
    const ImVec2 ts = fn->CalcTextSizeA(tfs, FLT_MAX, 0.f, title);
    dl->AddText(fn, tfs, {p.x + 20.f, p.y + (h - ts.y) * 0.5f},
                C::U(style_on ? C::Txt() : C::Dim()), title);

    const float sx = p.x + w - 22.f, sy = p.y + 18.f;
    dl->AddCircleFilled({sx, sy}, 5.f, C::U(style_on ? C::Acc() : C::Dim()), 16);
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
    MigrateLegacyRtMode();

    bool drv = false;          // FT-драйвер отвечает на пробу семейства
    int  fam = -1;             // номер семейства сокета FT-драйвера
    drv = memio::driver_available(&fam);
    bool rt_dev = memio::rt_dev_available();     // RT-Dev найден и отвечает
    bool rt_hook = memio::rt_hook_available();   // RT-Hook: хук ioctl отвечает

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
            const float slide = (1.f - t) * 24.f;

            dl->AddRectFilled({0.f, 0.f}, {vw, vh}, C::UA(C::Bg(), 0.96f * t));

            const char* title = XS("Способ чтения");
            const float tfs = fs * 1.50f;
            const ImVec2 ts = fn->CalcTextSizeA(tfs, FLT_MAX, 0.f, title);
            const float gap = 12.f;
            const float btnH = fs * 3.4f;
            const float panelW = ImMin(vw - 32.f, 690.f);
            const float btnW = (panelW - 64.f - gap) * 0.5f;
            const float panelH = 72.f + tfs + 24.f + btnH * 2.f + gap + 30.f;
            const float panelX = (vw - panelW) * 0.5f;
            const float panelY = (vh - panelH) * 0.5f + slide;

            dl->AddRectFilled({panelX + 2.f, panelY + 5.f},
                              {panelX + panelW + 2.f, panelY + panelH + 5.f},
                              IM_COL32(0, 0, 0, int(48 * t)), R::Sheet);
            dl->AddRectFilled({panelX, panelY}, {panelX + panelW, panelY + panelH},
                              C::U(C::LeftBg()), R::Sheet);
            dl->AddRect({panelX, panelY}, {panelX + panelW, panelY + panelH},
                        C::UA(C::Sep(), t), R::Sheet, 0, 1.f);
            dl->AddRectFilled({panelX, panelY}, {panelX + 5.f, panelY + panelH},
                              C::UA(C::Acc(), t), 2.f);

            dl->AddText(fn, tfs, {panelX + 32.f, panelY + 30.f},
                        C::UA(C::Txt(), t), title);
            dl->AddLine({panelX + 32.f, panelY + 54.f + ts.y},
                        {panelX + panelW - 32.f, panelY + 54.f + ts.y},
                        C::UA(C::Sep(), 0.85f * t), 1.f);

            const float rowW = btnW * 2.f + gap;
            const float bx = panelX + (panelW - rowW) * 0.5f;
            const float by = panelY + 76.f + ts.y;
            const float by2 = by + btnH + gap;

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
                // RT-Dev создаёт random root-only char-узел из точного
                // паттерна /dev/[A-GNY]{1,6} и принимает ioctl 0x801/0x802.
                // Повторный тап пересканирует узлы: модуль можно прошить после старта.
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
            if (ModeButton("##mode_rt_hook", btnW, btnH,
                           XS("RT-Hook"), rt_hook, true, fs)) {
                // RT-Hook не создаёт /dev: модуль перехватывает syscall ioctl.
                // Повторный тап заново проверяет чтение своей памяти командами
                // 0x259/0x25a через fd=-1.
                if (geteuid() != 0) {
                    PlaySound(SND_CLICK);
                    ShowToast(XS("RT-Hook: нет root — запусти чит от su"));
                } else if (!rt_hook) {
                    memio::rt_hook_transport().invalidate();
                    rt_hook = memio::rt_hook_available();
                    if (rt_hook) {
                        picked = MODE_RT_HOOK;
                        PlaySound(SND_SUCCESS);
                    } else {
                        PlaySound(SND_CLICK);
                        ShowToast(XS("RT-Hook не найден — прошей .sh своей версии ядра"));
                    }
                } else {
                    picked = MODE_RT_HOOK;
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
                     : (picked == MODE_RT_DEV)  ? MODE_RT_DEV
                     : (picked == MODE_RT_HOOK) ? MODE_RT_HOOK
                                                : MODE_EXTERNAL;
    if (picked >= 0) RememberMode(mode);
    // Ошибка от нажатия на другую карточку могла появиться перед успешным
    // выбором RT-Hook/RT-Dev/FT. Не переносим этот устаревший тост в игру.
    if (picked >= 0 && mode != MODE_EXTERNAL) ClearToast();
    esp_set_mem_backend(mode == MODE_KERNEL ? ESP_MEM_FT
                        : mode == MODE_RT_DEV  ? ESP_MEM_RT_DEV
                        : mode == MODE_RT_HOOK ? ESP_MEM_RT_HOOK
                                                : ESP_MEM_EXTERNAL);
    return picked >= 0;
}

}  // namespace modesel
