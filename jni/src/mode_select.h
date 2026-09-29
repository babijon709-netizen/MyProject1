#pragma once
// mode_select.h — экран выбора режима работы ПЕРЕД запуском чита.
//
// Экран показывается всегда на старте (после поднятия оверлея и тача, до
// старта потока привязки) и держит собственный цикл drawBegin/drawEnd, пока
// пользователь не выберет способ доступа к памяти игры:
//
//   * Экстернал — /proc/<pid>/mem, как раньше. Работает везде, где чит
//     вообще мог работать до этого.
//   * Кернел (драйвер) — чтение/запись через сокет ядерного драйвера
//     (kernel_io.h): /proc/<pid>/mem не открывается, привязка не зависит от
//     SELinux/yama на /proc.
//
// Выбор сохраняется в .mode рядом с .lang/.accent (каталог конфигов) и
// подсвечивается при следующем запуске; меню всё равно показывается — режим
// меняют под конкретное устройство/запуск (драйвер прошивается отдельно и
// живёт до перезагрузки, поэтому «нашёл/не нашёл» нужно видеть каждый раз).
//
// Пока экран на виду, весь ввод забирает оверлей (Touch_BlockRect на весь
// экран) — в игру не уходит ни одного пальца, тап мимо кнопок не «нажмёт»
// что-то в игре под оверлеем.

#include <sys/stat.h>
#include <unistd.h>                 // geteuid — статус root на экране

#include <fstream>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"          // ImClamp / ImMax / ImMin

#include "app_state.h"               // main_thread_flag
#include "main.h"                    // drawBegin/drawEnd, displayInfo
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
    MODE_EXTERNAL = 0,   // /proc/<pid>/mem
    MODE_KERNEL   = 1,   // сокет драйвера (kernel_io.h)
};

// ---- сохранение выбора (.mode, по образцу .lang/.accent в config.cpp) -----

static std::string ModePath() {
    return std::string(CfgDir()) + XS(".mode");
}

static Mode SavedMode() {
    std::ifstream f(ModePath());
    std::string v;
    if (!f || !std::getline(f, v) || v.empty()) return MODE_EXTERNAL;
    return v == "kernel" ? MODE_KERNEL : MODE_EXTERNAL;
}

static bool HasSavedMode() {
    std::ifstream f(ModePath());
    return f.good();
}

static void RememberMode(Mode m) {
    mkdir(CfgDir(), 0777);
    std::ofstream f(ModePath(), std::ios::trunc);
    if (f) f << (m == MODE_KERNEL ? "kernel" : "external");
}

// ---- одна большая кнопка режима (рисуем сами, как остальное меню) ---------

// enabled=false — режим недоступен (драйвер не найден): кнопка серая, тапы
// игнорируются. Возвращает true, если по кнопке тапнули.
static bool ModeButton(const char* id, float w, float h, const char* title,
                       const char* sub, bool enabled, bool accent, float fs) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, {w, h});

    const bool tapped = enabled && WasTappedHere();
    const bool held   = enabled && ImGui::IsItemActive() && ImGui::IsItemHovered();
    const bool hov    = enabled && ImGui::IsItemHovered();

    auto*  dl = ImGui::GetWindowDrawList();
    ImFont* fn = ImGui::GetFont();

    // Корпус: обычная карточка; у кернела — рамка акцентом, чтобы режимы
    // были различимы с одного взгляда. Нажатие — затемнение корпуса.
    ImVec4 body = enabled ? (accent ? C::Card() : C::LeftBg()) : C::TrkOff();
    if (held) body.w *= 0.82f;
    else if (hov) body.w *= 0.94f;
    dl->AddRectFilled(p, {p.x + w, p.y + h}, C::U(body), R::Btn);
    if (enabled && accent) {
        ImVec4 ring = C::Acc();
        ring.w *= held ? 0.85f : 1.f;
        dl->AddRect(p, {p.x + w, p.y + h}, C::U(ring), R::Btn, 0.f, 3.f);
    }

    // Заголовок и подпись — по центру; шрифт один, крупный кегль задаём
    // вручную (как подписи вкладок в menu.cpp).
    const float tfs = fs * 1.30f;
    const float sfs = fs * 0.92f;
    const ImVec2 ts = fn->CalcTextSizeA(tfs, FLT_MAX, 0.f, title);
    const ImVec2 ss = fn->CalcTextSizeA(sfs, FLT_MAX, 0.f, sub);
    dl->AddText(fn, tfs, {p.x + (w - ts.x) * 0.5f, p.y + h * 0.5f - tfs - 6.f},
                C::U(enabled ? C::Txt() : C::Dim()), title);
    dl->AddText(fn, sfs, {p.x + (w - ss.x) * 0.5f, p.y + h * 0.5f + 8.f},
                C::U(C::Dim()), sub);
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

    const Mode saved = SavedMode();
    const bool hadSaved = HasSavedMode();
    bool drv = false;          // драйвер отвечает на пробу семейства
    int  fam = -1;            // номер семейства сокета драйвера
    drv = memio::driver_available(&fam);

    float fade = 0.f;         // появление карточки 0..1
    int   picked = -1;        // -1 — ещё не выбрали

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
            const float slide = (1.f - t) * 36.f;   // карточка въезжает снизу

            // Фон: сплошной, игры под ним ещё не видно (привязки нет) —
            // полупрозрачность здесь только мешала бы.
            dl->AddRectFilled({0.f, 0.f}, {vw, vh}, C::U(C::Bg()));

            // ---- карточка по центру ------------------------------------
            const float cardW = ImMin(vw - 64.f, 900.f);
            const float btnH  = 148.f;
            const float pad   = 30.f;
            const float titleH = fs * 2.6f;
            const float subH   = fs * 1.5f;
            const float statusH = fs * 2.1f;
            const float hintH  = fs * 1.35f;   // нижняя строка (прошлый режим/подсказка)
            const float cardH = pad + titleH + subH + 18.f +
                                btnH + 14.f + btnH + 20.f + statusH + hintH + pad;
            const float cx = (vw - cardW) * 0.5f;
            const float cy = (vh - cardH) * 0.5f + slide;
            dl->AddRectFilled({cx, cy}, {cx + cardW, cy + cardH},
                              C::U(C::Card()), R::Card);

            float y = cy + pad;
            float x = cx + pad;
            const float iw = cardW - pad * 2.f;

            // Заголовок и подзаголовок.
            {
                const char* title = XS("Режим работы");
                const char* sub   = XS("Выберите способ доступа к памяти игры");
                const float tfs = fs * 1.9f;
                const float sfs = fs * 1.0f;
                const ImVec2 ts = fn->CalcTextSizeA(tfs, FLT_MAX, 0.f, title);
                const ImVec2 ss = fn->CalcTextSizeA(sfs, FLT_MAX, 0.f, sub);
                dl->AddText(fn, tfs, {cx + (cardW - ts.x) * 0.5f, y},
                            C::U(C::Txt()), title);
                y += titleH;
                dl->AddText(fn, sfs, {cx + (cardW - ss.x) * 0.5f, y},
                            C::U(C::Dim()), sub);
                y += subH + 18.f;
            }

            // ---- кнопки режимов ---------------------------------------
            ImGui::SetCursorScreenPos({x, y});
            if (ModeButton("##mode_ext", iw, btnH, XS("Экстернал"),
                           XS("Через /proc/<pid>/mem — работает без драйвера"),
                           true, false, fs)) {
                picked = MODE_EXTERNAL;
                PlaySound(SND_CLICK);
                ShowToast(XS("Экстернал режим"));
            }
            y += btnH + 14.f;

            ImGui::SetCursorScreenPos({x, y});
            if (ModeButton("##mode_kernel", iw, btnH, XS("Кернел (драйвер)"),
                           XS("Через ядерный драйвер — мимо /proc и его запретов"),
                           drv, true, fs)) {
                // Тап по кернелу. Порядок причин:
                //   1) не root — сокет драйвера создаётся только привилегирован-
                //      ным процессом, маячок без root не виден ВООБЩЕ;
                //   2) драйвер не найден — рескан с чистого листа;
                //   3) сокет драйвера поднялся — режим ПРИНИМАЕТСЯ. Живые пробы
                //      по собственной памяти (learn_commands) уточняют только
                //      сообщение: часть сборок драйвера не даёт доступ к самому
                //      себе, и это не приговор — по-настоящему транспорт
                //      проверяется привязкой к игре (bind читает ELF-заголовок
                //      libil2cpp через драйвер, /proc не задействован).
                if (geteuid() != 0) {
                    PlaySound(SND_CLICK);
                    ShowToast(XS("Кернел: нет root — запусти чит от su"));
                } else if (!drv) {
                    memio::kernel_transport().invalidate();
                    drv = memio::driver_available(&fam);
                    PlaySound(SND_CLICK);
                    ShowToast(XS("Драйвер не найден — прошей .sh своей версии ядра"));
                } else {
                    memio::KernelTransport& kt = memio::kernel_transport();
                    kt.learn_commands();
                    const bool readOk  = kt.map_known() &&
                        kt.classify_cmd(kt.read_cmd()).kind == memio::CMD_READ;
                    const bool writeOk = kt.map_known() &&
                        kt.classify_cmd(kt.write_cmd()).kind == memio::CMD_WRITE;
                    picked = MODE_KERNEL;
                    if (readOk && writeOk) {
                        PlaySound(SND_SUCCESS);
                        ShowToast(XS("Кернел режим|драйвер проверен"));
                    } else if (readOk) {
                        PlaySound(SND_SUCCESS);
                        ShowToast(XS("Кернел режим|чтение проверено, запись — нет"));
                    } else if (writeOk) {
                        PlaySound(SND_SUCCESS);
                        ShowToast(XS("Кернел режим|запись проверена, чтение — нет"));
                    } else {
                        PlaySound(SND_CLICK);
                        ShowToast(XS("Кернел режим|проверюсь при привязке к игре"));
                    }
                }
            }
            y += btnH + 20.f;

            // ---- строка состояния драйвера + перепроверка ---------------
            {
                char status[160];
                ImVec4 dotCol;
                const bool root = geteuid() == 0;
                if (!root) {
                    // Не root: маячок драйвера не виден в принципе — это
                    // первая и самая частая причина «серой кнопки».
                    snprintf(status, sizeof(status), "%s",
                             XS("Кернел: нет root — запусти чит от su"));
                    dotCol = C::Red();
                } else if (drv) {
                    snprintf(status, sizeof(status), XS("Драйвер: найден (семейство %d)"), fam);
                    dotCol = ImVec4(0.20f, 0.78f, 0.35f, 1.f);
                } else {
                    snprintf(status, sizeof(status), "%s",
                             XS("Драйвер не найден — прошей .sh своей версии ядра"));
                    dotCol = C::Red();
                }
                const float dfs = fs * 0.95f;
                const ImVec2 dss = fn->CalcTextSizeA(dfs, FLT_MAX, 0.f, status);

                // Кнопка «Проверить снова»: драйвер прошивают и после
                // запуска чита — перезагружать ради этого приложение
                // не нужно. Рескан с чистого листа: сокет мог умереть, и
                // заново прошитый драйвер мог получить другое семейство.
                const char* reLbl = XS("Проверить снова");
                const float rfs = fs * 0.95f;
                const ImVec2 rs = fn->CalcTextSizeA(rfs, FLT_MAX, 0.f, reLbl);
                const float rW = rs.x + 44.f, rH = fs * 2.0f;

                const float dotR = 8.f;
                const float sy = y + statusH * 0.5f;

                dl->AddCircleFilled({x + dotR + 4.f, sy}, dotR, C::U(dotCol), 16);
                dl->AddText(fn, dfs, {x + dotR * 2.f + 18.f, sy - dss.y * 0.5f},
                            C::U(root && drv ? C::Txt() : C::Dim()), status);

                const float rx = x + iw - rW;
                const float ry = y + (statusH - rH) * 0.5f;
                ImGui::SetCursorScreenPos({rx, ry});
                ImGui::InvisibleButton("##mode_recheck", {rW, rH});
                if (WasTappedHere()) {
                    memio::kernel_transport().invalidate();
                    drv = memio::driver_available(&fam);
                    PlaySound(SND_CLICK);
                }
                const bool rHeld = ImGui::IsItemActive() && ImGui::IsItemHovered();
                const bool rHov  = ImGui::IsItemHovered();
                ImVec4 rBg = C::LeftBg();
                if (rHeld) rBg.w *= 0.8f;
                else if (rHov) rBg.w *= 0.92f;
                dl->AddRectFilled({rx, ry}, {rx + rW, ry + rH}, C::U(rBg), rH * 0.5f);
                dl->AddText(fn, rfs, {rx + (rW - rs.x) * 0.5f, ry + (rH - rs.y) * 0.5f},
                            C::U(C::Txt()), reLbl);

                // Нижняя строка: прошлый режим, если устройство уже
                // настраивали (.mode существует), иначе — как проверить
                // драйвер из терминала ещё до входа в игру.
                {
                    char prevBuf[96];
                    const char* prev = nullptr;
                    if (hadSaved) {
                        const char* modeName = (saved == MODE_KERNEL) ? XS("Кернел (драйвер)")
                                                                      : XS("Экстернал");
                        snprintf(prevBuf, sizeof(prevBuf), XS("Прошлый запуск: %s"), modeName);
                        prev = prevBuf;
                    } else {
                        prev = XS("Проверка из терминала: xvcen --check-driver");
                    }
                    const float pfs = fs * 0.85f;
                    const ImVec2 ps = fn->CalcTextSizeA(pfs, FLT_MAX, 0.f, prev);
                    dl->AddText(fn, pfs, {cx + (cardW - ps.x) * 0.5f,
                                          y + statusH + (hintH - ps.y) * 0.5f},
                                C::U(C::Dim()), prev);
                }
            }
        }
        ImGui::End();

        drawEnd();
    }

    Touch_BlockRect(false, 0.f, 0.f, 0.f, 0.f);

    // Применяем выбранный (или дефолтный при выходе через сигнал) транспорт.
    // Достато // Применяем выбранный (или дефолтный при выходе через сигнал) транспорт.
    // Достаточно одного вызова до первого bind — весь код чтения выше по
    // стеку работает через memio::Transport и про режимы не знает (mem_io.h).
    const Mode mode = (picked == MODE_KERNEL) ? MODE_KERNEL : MODE_EXTERNAL;
    if (picked >= 0) RememberMode(mode);
    esp_set_mem_backend(mode == MODE_KERNEL);
    return picked >= 0;
}

}  // namespace modesel
