#include "esp_draw.h"
#include "aim.h"                 // AIM_MODE_MEMORY
#include "main.h"              // displayInfo, native_window_screen_*
#include "app_state.h"         // g_state
#include "cfg.h"
#include "process.h"           // g_esp_attached
#include "ui_util.h"           // ColU32
#include "game_offsets_active.h" // game_offsets::PLAYER_BOX_WIDTH_RATIO
#include <cfloat>
#include <cmath>

// Radius (px) of the aim FOV circle. Original logic: cfg::aim::fov is an
// angle against a fixed 60° reference (not the live camera FOV), so the circle
// stays put on screen when the player zooms in. 180° = whole screen.
float AimFovRadiusPx(float sw, float sh) {
    float fov = cfg::aim::fov;
    if (fov <= 0.f) return 0.f;
    if (fov >= 180.f) return sw + sh;
    float t = tanf(fov * 0.5f * (float)M_PI / 180.f) / tanf(30.f * (float)M_PI / 180.f);
    float r = t * (sh * 0.5f);
    if (!std::isfinite(r) || r > sw + sh) r = sw + sh;
    return r;
}

// One remote snapshot per frame, shared by the ESP overlay and the aimbot.
// Сколько боксов нарисовано в последнем кадре — для сводки в мини-логе.

const std::vector<EspBox>& FrameBoxes(float sw, float sh) {
    static std::vector<EspBox> s_boxes;
    static int s_frame = -1;
    int frame = ImGui::GetFrameCount();
    if (frame != s_frame) {
        s_frame = frame;
        esp_set_skeleton_enabled(g_state.esp_skeleton);
        // Кости нужны аиму в обоих режимах: и касание, и память считают ошибку
        // по точке на шее, взятой из костей. Раньше запрос включался только от
        // касания, и в режиме «память» при выключенном скелете в ESP кости не
        // читались вовсе — тогда аим уходил на запасной расчёт по росту, а
        // признак смерти по позе вообще нечем было мерить.
        esp_set_aim_bones_enabled(g_state.aim_touch || g_state.aim_mode == AIM_MODE_MEMORY);
        esp_set_markers_enabled(g_state.esp_ore, g_state.esp_animal,
                                g_state.esp_loot, g_state.esp_pickup);
        esp_set_always_day(g_state.always_day);
        esp_set_marker_max_distance(g_state.marker_dist);
        esp_set_xray(g_state.xray_on ? g_state.xray_range : 0.f);
        s_boxes = esp_get_boxes((int)sw, (int)sh);
    }
    return s_boxes;
}

void DrawEspOverlay() {
    float sw = (float)native_window_screen_x;
    float sh = (float)native_window_screen_y;
    if (displayInfo.width > displayInfo.height && displayInfo.width >= 100 && displayInfo.height >= 100) {
        sw = (float)displayInfo.width;
        sh = (float)displayInfo.height;
    } else if (displayInfo.height > displayInfo.width && displayInfo.height >= 100 && displayInfo.width >= 100) {
        sw = (float)displayInfo.height;
        sh = (float)displayInfo.width;
    }
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    if (!g_esp_attached) return;

    if (g_state.aim_touch && g_state.aim_special) {
        float fovR = AimFovRadiusPx(sw, sh);
        if (fovR > 1.f) {
            ImVec4 fc = ImVec4(cfg::aim::fov_color[0], cfg::aim::fov_color[1],
                               cfg::aim::fov_color[2], cfg::aim::fov_color[3]);
            dl->AddCircle(ImVec2(sw * 0.5f, sh * 0.5f), fovR,
                          ImGui::ColorConvertFloat4ToU32(fc), 72, 1.5f);
        }
    }

    if (!g_state.esp_box && !g_state.esp_chams && !g_state.esp_wall && !g_state.esp_tracer && !g_state.esp_skeleton && !g_state.esp_name && !g_state.esp_weapon && !g_state.esp_ore && !g_state.esp_animal && !g_state.esp_loot && !g_state.esp_pickup) return;

    const std::vector<EspBox>& boxes = FrameBoxes(sw, sh);
    constexpr int BOX_EDGES[][2] = {
        {0,1},{1,2},{2,3},{3,0},
        {4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7}
    };
    float thick = g_state.esp_thick;
    if (thick < 0.5f) thick = 0.5f;

    // Thin dark-gray outline used across all visuals (no bold black strokes).
    const ImU32 kVisOutline = IM_COL32(45, 45, 52, 220);

    // ESP labels in the GUI style: compact rounded pill (light translucent fill
    // + thin gray outline) + colored text. The text is drawn with the same font
    // and size it is measured at (espFont/espFs) so it never spills out of the
    // pill — the old call drew at the full-size font while sizing at 0.8×, which
    // made the distance/weapon text overflow the pill.
    ImFont* espFont = ImGui::GetFont();
    float espFs = ImGui::GetFontSize() * 0.8f;
    auto PillH = [&](const char* text, float scale = 1.f) {
        if (!text || !text[0]) return 0.f;
        ImVec2 tsz = espFont->CalcTextSizeA(espFs * scale, FLT_MAX, 0, text);
        return tsz.y + 4.f * scale;
    };
    auto EspPill = [&](float cx, float y, const char* text, ImU32 textCol, float scale = 1.f) {
        if (!text || !text[0]) return;
        float fsz = espFs * scale;
        ImVec2 tsz = espFont->CalcTextSizeA(fsz, FLT_MAX, 0, text);
        const float padX = 5.f * scale, padY = 2.f * scale;
        float x0 = cx - tsz.x * 0.5f - padX;
        float x1 = cx + tsz.x * 0.5f + padX;
        float y1 = y + tsz.y + padY * 2.f;
        // Light translucent fill (not a dense black block), same for the name,
        // weapon and distance pills so they read consistently over any scene.
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y1), IM_COL32(30, 30, 36, 40), 5.f * scale);
        dl->AddRect(ImVec2(x0, y), ImVec2(x1, y1), kVisOutline, 5.f * scale, 0, 1.0f);
        dl->AddText(espFont, fsz, ImVec2(cx - tsz.x * 0.5f, y + padY), textCol, text);
    };
    // A far away player projects to a couple of pixels, which used to make the
    // box degenerate (corners gone, top/bottom strokes fused into one line).
    // Every visual therefore works on a rect that is grown around its own
    // centre up to a stroke-aware minimum.
    //
    // The growth is a single uniform scale, not a per-axis clamp: clamping the
    // axes separately pushed the width of a distant player up to the same
    // minimum as the height and the box turned into a square. Scaling keeps the
    // player's proportions, so a far away box stays a small tall rectangle.
    const float kMinBoxSide = 3.0f * (thick + 1.0f) + 8.0f;      // vertical floor
    const float kMinBoxWidth = 2.0f * 1.5f + (thick + 1.5f);     // 2 stubs + a gap
    auto NormRect = [&](float& x1, float& y1, float& x2, float& y2) {
        if (x2 < x1) { float t = x1; x1 = x2; x2 = t; }
        if (y2 < y1) { float t = y1; y1 = y2; y2 = t; }
        float cx = (x1 + x2) * 0.5f, cy = (y1 + y2) * 0.5f;
        float w = x2 - x1, h = y2 - y1;
        if (!(h > 0.01f)) { h = kMinBoxSide; w = kMinBoxSide * game_offsets::PLAYER_BOX_WIDTH_RATIO; }
        if (h < kMinBoxSide) { float s = kMinBoxSide / h; h *= s; w *= s; }
        if (w < kMinBoxWidth) w = kMinBoxWidth; // only for absurdly narrow rects
        x1 = cx - w * 0.5f; x2 = cx + w * 0.5f;
        y1 = cy - h * 0.5f; y2 = cy + h * 0.5f;
    };
    // Corner box: the middle of every edge is cut out, only corners remain.
    // The corner length is capped per axis so the two segments of one edge can
    // never meet — at any distance a visible gap stays in the middle of the
    // top/bottom (and left/right) edges, which is what makes it read as a
    // corner box instead of a plain rectangle or a single fused line.
    auto CornerBox = [&](float x1, float y1, float x2, float y2, ImU32 col) {
        if (!std::isfinite(x1) || !std::isfinite(y1) || !std::isfinite(x2) || !std::isfinite(y2)) return;
        NormRect(x1, y1, x2, y2);
        float w = x2 - x1, h = y2 - y1;

        float len = (w < h ? w : h) * 0.28f;
        if (len > 42.f) len = 42.f;

        // Keep at least ~30% of every edge (and never less than one stroke
        // plus a pixel) empty in its middle.
        float gapX = w * 0.30f; if (gapX < thick + 1.5f) gapX = thick + 1.5f;
        float gapY = h * 0.30f; if (gapY < thick + 1.5f) gapY = thick + 1.5f;
        float lx = (w - gapX) * 0.5f; if (lx > len) lx = len;
        float ly = (h - gapY) * 0.5f; if (ly > len) ly = len;
        if (lx < 1.5f) lx = 1.5f;
        if (ly < 1.5f) ly = 1.5f;

        const ImVec2 pts[8][2] = {
            {{x1, y1}, {x1 + lx, y1}}, {{x1, y1}, {x1, y1 + ly}},
            {{x2 - lx, y1}, {x2, y1}}, {{x2, y1}, {x2, y1 + ly}},
            {{x1, y2 - ly}, {x1, y2}}, {{x1, y2}, {x1 + lx, y2}},
            {{x2 - lx, y2}, {x2, y2}}, {{x2, y2 - ly}, {x2, y2}},
        };
        for (int i = 0; i < 8; ++i)
            dl->AddLine(pts[i][0], pts[i][1], kVisOutline, thick + 1.0f);
        for (int i = 0; i < 8; ++i)
            dl->AddLine(pts[i][0], pts[i][1], col, thick);
    };

    for (const EspBox& box : boxes) {
        if (!std::isfinite(box.x1) || !std::isfinite(box.y1) || !std::isfinite(box.x2) || !std::isfinite(box.y2)) continue;

        // Rect every visual (box, tracer, labels) is anchored to: never smaller
        // than the stroke-aware minimum, so nothing collapses at long range.
        float bx1 = box.x1, by1 = box.y1, bx2 = box.x2, by2 = box.y2;
        NormRect(bx1, by1, bx2, by2);

        // Союзники: выключатель «Союзники» в визуалах. Выключен — на союзнике
        // не рисуется НИЧЕГО: ни бокс, ни скелет, ни линия, ни ник, ни оружие.
        // Раньше признак своего влиял только на цвет, и убрать союзников с
        // экрана было нечем (просьба 19.09).
        if (box.ally && !g_state.esp_team) continue;
        // Team mates / clan mates get their own colour so they read as
        // friendly at a glance (and the aimbot leaves them alone).
        const bool ally = box.ally;

        if (g_state.esp_chams) {
            bool all_valid = true;
            for (int c = 0; c < 8; ++c) {
                if (!box.corner_visible[c] || !std::isfinite(box.corners[c][0]) || !std::isfinite(box.corners[c][1]) || box.corners[c][0] < 0 || box.corners[c][1] < 0) {
                    all_valid = false;
                    break;
                }
            }
            if (all_valid) {
                ImU32 col = ColU32(cfg::esp::box_col_invis);
                // 3D box edges collapse to a single line at long range because the
                // projected top/bottom (and left/right) faces sit within a pixel or
                // two. Measure the on-screen extent and, when it is below a stroke
                // aware minimum, draw a flat, centred box with that minimum span so
                // the horizontal rows stay visibly separated (same guarantee as the
                // corner box) instead of fusing into one line.
                float mnX = FLT_MAX, mnY = FLT_MAX, mxX = -FLT_MAX, mxY = -FLT_MAX;
                for (int c = 0; c < 8; ++c) {
                    float x = box.corners[c][0], y = box.corners[c][1];
                    if (x < mnX) mnX = x;
                    if (x > mxX) mxX = x;
                    if (y < mnY) mnY = y;
                    if (y > mxY) mxY = y;
                }
                float dMin = kMinBoxSide;
                if ((mxX - mnX) >= dMin && (mxY - mnY) >= dMin) {
                    for (const auto& edge : BOX_EDGES) {
                        int a = edge[0], b = edge[1];
                        dl->AddLine(
                            ImVec2(box.corners[a][0], box.corners[a][1]),
                            ImVec2(box.corners[b][0], box.corners[b][1]),
                            col, thick
                        );
                    }
                } else {
                    dl->AddRect(ImVec2(bx1, by1), ImVec2(bx2, by2), kVisOutline, 0.f, 0, thick + 1.0f);
                    dl->AddRect(ImVec2(bx1, by1), ImVec2(bx2, by2), col, 0.f, 0, thick);
                }
            }
        }

        if (g_state.esp_box) {
            CornerBox(bx1, by1, bx2, by2, ColU32(ally ? cfg::esp::ally_col : cfg::esp::box_col));
        }

        if (g_state.esp_skeleton && box.has_skeleton) {
            ImU32 skelCol = ColU32(cfg::esp::skeleton_col);
            auto validPt = [&](int b) {
                return b >= 0 && box.bone_valid[b] &&
                       std::isfinite(box.bones[b][0]) && std::isfinite(box.bones[b][1]);
            };
            auto lineTo = [&](int a, int b) {
                dl->AddLine(ImVec2(box.bones[a][0], box.bones[a][1]),
                            ImVec2(box.bones[b][0], box.bones[b][1]),
                            skelCol, thick);
            };
            // Draw each chain connecting consecutive valid bones, skipping
            // missing ones (0..5 torso, 6..9/10..13 arms, 14..17/18..21 legs).
            auto drawChain = [&](const int* chain, int n, int anchor) {
                int prev = validPt(anchor) ? anchor : -1;
                for (int i = 0; i < n; ++i) {
                    int b = chain[i];
                    if (!validPt(b)) continue;
                    if (prev >= 0) lineTo(prev, b);
                    prev = b;
                }
            };
            // Arms hang off the highest available spine bone.
            int chest = -1;
            for (int c : {3, 2, 1, 0}) { if (validPt(c)) { chest = c; break; } }

            static const int torso[] = {1, 2, 3, 4, 5};
            static const int armL[]  = {6, 7, 8, 9};
            static const int armR[]  = {10, 11, 12, 13};
            static const int legL[]  = {14, 15, 16, 17};
            static const int legR[]  = {18, 19, 20, 21};
            drawChain(torso, 5, 0);
            drawChain(armL, 4, chest);
            drawChain(armR, 4, chest);
            drawChain(legL, 4, 0);
            drawChain(legR, 4, 0);
        }



        if (g_state.esp_tracer) {
            // From the middle of the top edge of the screen to the head area.
            // Team mates get a green line so a glance at the tracer is enough.
            float tx = (bx1 + bx2) * 0.5f;
            ImU32 tracerCol = ColU32(ally ? cfg::esp::ally_tracer_col : cfg::esp::tracer_col);
            dl->AddLine(ImVec2(sw * 0.5f, 0.0f), ImVec2(tx, by1),
                        kVisOutline, thick + 1.0f);
            dl->AddLine(ImVec2(sw * 0.5f, 0.0f), ImVec2(tx, by1), tracerCol, thick);
        }

        // Name above the box; distance right under the box, weapon under the
        // distance.
        {
            float cx = (bx1 + bx2) * 0.5f;
            const float gap = 5.f;
            if (g_state.esp_name && box.has_name && box.name[0]) {
                // Clan tag in front of the nick, the way the game shows it.
                char label[56];
                if (box.has_tag && box.tag[0])
                    snprintf(label, sizeof(label), "[%s] %s", box.tag, box.name);
                else
                    snprintf(label, sizeof(label), "%s", box.name);
                float h = PillH(label);
                EspPill(cx, by1 - h - gap, label,
                        ColU32(ally ? cfg::esp::ally_col : cfg::esp::name_col));
            }
            float belowY = by2 + gap;
            if (g_state.esp_wall) {
                char label[32];
                if (box.distance >= 0.0f) snprintf(label, sizeof(label), "%.1fm", box.distance);
                else snprintf(label, sizeof(label), "PLAYER");
                EspPill(cx, belowY, label, ColU32(cfg::esp::distance_col));
                belowY += PillH(label) + 4.f;
            }
            if (g_state.esp_weapon && box.has_weapon && box.weapon[0]) {
                EspPill(cx, belowY, box.weapon, ColU32(cfg::esp::weapon_col));
                belowY += PillH(box.weapon) + 4.f;
            }
        }
    }

    // ---- World markers: ore nodes and animals ----------------------------
    // One pill with the resource / animal name at the object's position; the
    // scan itself is done by the game layer and reuses this frame's camera.
    if (g_state.esp_ore || g_state.esp_animal || g_state.esp_loot || g_state.esp_pickup) {
        // Smaller than the player labels (there are many more of them), with the
        // distance on a second line underneath.
        constexpr float kMarkerScale = 0.78f;
        // Elite crates pulse through the spectrum: one hue for all of them per
        // frame (a full turn every two seconds) so they cannot be missed.
        const float rainbow_hue = fmodf((float)ImGui::GetTime() * 0.5f, 1.0f);
        float rr = 1.f, rg = 1.f, rb = 1.f;
        ImGui::ColorConvertHSVtoRGB(rainbow_hue, 0.85f, 1.0f, rr, rg, rb);
        const ImU32 rainbow_col = IM_COL32((int)(rr * 255.f), (int)(rg * 255.f), (int)(rb * 255.f), 255);
        for (const EspMarker& marker : esp_get_markers()) {
            if (!marker.name[0]) continue;
            if (!std::isfinite(marker.x) || !std::isfinite(marker.y)) continue;
            ImU32 col = marker.rainbow ? rainbow_col
                : marker.has_color
                ? IM_COL32(marker.color_rgb[0], marker.color_rgb[1], marker.color_rgb[2], 255)
                : ColU32(marker.kind == ESP_MARKER_LOOT   ? cfg::esp::loot_col
                       : marker.kind == ESP_MARKER_PICKUP ? cfg::esp::pickup_col
                                                          : cfg::esp::animal_col);
            EspPill(marker.x, marker.y, marker.name, col, kMarkerScale);
            char label[24];
            snprintf(label, sizeof(label), "%.0fm", marker.distance);
            EspPill(marker.x, marker.y + PillH(marker.name, kMarkerScale) + 2.f, label,
                    ColU32(cfg::esp::distance_col), kMarkerScale);
        }
    }
}
