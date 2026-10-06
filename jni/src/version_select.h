#pragma once
// version_select.h — экран выбора версии игры ПЕРЕД привязкой.
//
// Релизный и бета-клиент (dump11945 / dumpbeta11943) ставятся под ОДНИМ
// именем пакета, поэтому автоопределения по package name не существует —
// только ручной выбор. От выбора зависит набор оффсетов (go_active):
// managed-layouts у билдов одинаковые, отличаются TypeInfo inner-slots,
// окно TOD и одно поле PlayerInventoryData.
//
// Экран минималистичный, по образцу mode_select.h: заголовок «Версия игры»
// по центру и две кнопки — «Release» и «Beta». Выбранный на прошлом запуске
// вариант подсвечен акцентной рамкой и хранится в .gversion рядом с
// .mode/.lang: экран показывается на каждом запуске, но последний выбор
// виден сразу, без угадывания.
//
// Пока экран на виду, весь ввод забирает оверлей (Touch_BlockRect на весь
// экран) — в игру не уходит ни одного пальца.

#include <sys/stat.h>

#include <fstream>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"          // ImMin

#include "app_state.h"               // main_thread_flag
#include "main.h"                    // drawBegin/drawEnd
#include "game_offsets_active.h"     // go_active::UseRelease/UseBeta
#include "config.h"                  // CfgDir(), RestoreLang(), RestoreAccent()
#include "theme.h"                   // C::/R::, ApplyTheme
#include "hud.h"                     // VisibleScreen
#include "audio.h"                   // PlaySound
#include "str.h"                     // XS()
#include "ui_util.h"                 // EaseInOut, WasTappedHere

namespace versionsel {

// ---- сохранение выбора (.gversion, по образцу .mode в mode_select.h) ------

static std::string VersionPath() {
    return std::string(CfgDir()) + XS(".gversion");
}

static bool RememberedBeta() {
    std::ifstream in(VersionPath());
    std::string value;
    return in && std::getline(in, value) && value == "beta";
}

static void RememberVersion(bool beta) {
    mkdir(CfgDir(), 0777);
    std::ofstream f(VersionPath(), std::ios::trunc);
    if (f) f << (beta ? "beta" : "release");
}

// Применить выбранную версию к go_active и запомнить для следующего запуска.
// Дёргается и из экрана выбора, и из esp_init — когда fingerprint libil2cpp
// показал, что пользователь ткнул не в ту кнопку (имя пакета у версий
// одинаковое, поэтому автодетект им невозможен и нужен этот двухкнопочный
// выбор, а неверный выбор ломал бы резолвинг ВСЕХ классов).
inline void ApplyVersion(bool beta) {
    RememberVersion(beta);
    if (beta) go_active::UseBeta();
    else      go_active::UseRelease();
}

// ---- кнопка версии (корпус как у карточек mode_select) ---------------------

static bool VersionButton(const char* id, float w, float h, const char* title,
                          bool accent, float fs) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, {w, h});

    const bool tapped = WasTappedHere();
    const bool held   = ImGui::IsItemActive() && ImGui::IsItemHovered();
    const bool hov    = ImGui::IsItemHovered();

    auto*  dl = ImGui::GetWindowDrawList();
    ImFont* fn = ImGui::GetFont();

    ImVec4 body = accent ? C::Card() : C::LeftBg();
    if (held) body.w *= 0.82f;
    else if (hov) body.w *= 0.94f;
    dl->AddRectFilled(p, {p.x + w, p.y + h}, C::U(body), R::Btn);
    if (accent) {
        ImVec4 ring = C::Acc();
        ring.w *= held ? 0.85f : 1.f;
        dl->AddRect(p, {p.x + w, p.y + h}, C::U(ring), R::Btn, 0.f, 3.f);
    }

    const float tfs = fs * 1.25f;
    const ImVec2 ts = fn->CalcTextSizeA(tfs, FLT_MAX, 0.f, title);
    dl->AddText(fn, tfs, {p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f},
                C::U(C::Txt()), title);
    return tapped;
}

// ---- экран ---------------------------------------------------------------
//
// Возвращает true, если версия выбрана и применена к go_active. false —
// приложение закрыли сигналом до выбора: вызывающий выходит тем же порядком
// teardown, что и при отмене выбора режима.
static bool RunVersionSelect() {
    // Язык и акцент применяем заранее (в main() их восстановление идёт позже),
    // как это делает экран выбора режима.
    RestoreLang();
    RestoreAccent();
    ApplyTheme();
    const bool remembered_beta = RememberedBeta();

    float fade = 0.f;
    int   picked = -1;         // 0 — Release, 1 — Beta

    while (main_thread_flag.load() && picked < 0) {
        drawBegin();
        auto& io = ImGui::GetIO();
        float dt = io.DeltaTime;
        if (dt > 0.1f) dt = 0.1f;
        fade = ImMin(fade + dt * 1.6f, 1.f);

        float vw = 0.f, vh = 0.f;
        VisibleScreen(vw, vh);
        Touch_BlockRect(true, 0.f, 0.f, vw, vh);

        ImGuiWindowFlags wf = ImGuiWindowFlags_NoResize    | ImGuiWindowFlags_NoCollapse
                            | ImGuiWindowFlags_NoTitleBar  | ImGuiWindowFlags_NoScrollbar
                            | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground
                            | ImGuiWindowFlags_NoMove      | ImGuiWindowFlags_NoNav
                            | ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::SetNextWindowPos({0.f, 0.f}, ImGuiCond_Always);
        ImGui::SetNextWindowSize({vw, vh}, ImGuiCond_Always);
        ImGui::Begin("##version_select", nullptr, wf);
        {
            auto*  dl = ImGui::GetWindowDrawList();
            ImFont* fn = ImGui::GetFont();
            const float fs = ImGui::GetFontSize();

            const float t = EaseInOut(fade);
            const float slide = (1.f - t) * -30.f;   // въезжает сверху

            // Заголовок + две кнопки — единый блок ровно по центру экрана.
            const char* title = XS("Версия игры");
            const float tfs = fs * 1.55f;
            const ImVec2 ts = fn->CalcTextSizeA(tfs, FLT_MAX, 0.f, title);
            const float gap  = 14.f;
            const float btnH = fs * 3.4f;
            const float btnW = ImMin((vw - 36.f - gap) / 2.f, 300.f);
            const float tGap = fs * 1.3f;
            const float blockH = tfs + tGap + btnH;
            const float ty = (vh - blockH) * 0.5f + slide;
            const float tx = (vw - ts.x) * 0.5f;
            dl->AddText(fn, tfs, {tx + 2.f, ty + 2.f}, IM_COL32(0, 0, 0, 110), title);
            dl->AddText(fn, tfs, {tx, ty}, C::U(C::Txt()), title);

            const float rowW = btnW * 2.f + gap;
            const float by   = ty + tfs + tGap;
            const float bx   = (vw - rowW) * 0.5f;

            ImGui::SetCursorScreenPos({bx, by});
            if (VersionButton("##ver_release", btnW, btnH,
                              "Release", !remembered_beta, fs)) {
                picked = 0;
                PlaySound(SND_CLICK);
                ShowToast(XS("Release"));
            }

            ImGui::SetCursorScreenPos({bx + btnW + gap, by});
            if (VersionButton("##ver_beta", btnW, btnH,
                              "Beta", remembered_beta, fs)) {
                picked = 1;
                PlaySound(SND_CLICK);
                ShowToast(XS("Beta"));
            }
        }
        ImGui::End();

        drawEnd();
    }

    Touch_BlockRect(false, 0.f, 0.f, 0.f, 0.f);

    // Выбор задаёт набор оффсетов один раз, до привязки и до старта
    // какого-либо чтения — потребители go_active дальше не знают про версии.
    if (picked >= 0) ApplyVersion(picked == 1);
    return picked >= 0;
}

}  // namespace versionsel
