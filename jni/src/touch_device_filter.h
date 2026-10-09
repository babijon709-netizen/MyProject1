#pragma once

namespace touchfilter {

struct Caps {
    bool mt_x = false;
    bool mt_y = false;
    bool abs_x = false;
    bool abs_y = false;
    bool btn_touch = false;
    bool tool_pen = false;
    bool direct = false;
    bool pointer = false;
};

enum Kind {
    NotTouch = 0,
    MultiTouch = 1,
    SingleTouch = 2
};

inline Kind classify(const Caps& caps) {
    // Android may expose a touchpad and one or more pen digitizers alongside
    // the panel. Reading all of them lets an idle secondary device release
    // ImGui's mouse immediately after the real panel reports a press.
    if (caps.tool_pen) return NotTouch;
    if (caps.pointer && !caps.direct) return NotTouch;
    if (caps.mt_x && caps.mt_y) return MultiTouch;
    if (caps.abs_x && caps.abs_y && caps.btn_touch) return SingleTouch;
    return NotTouch;
}

} // namespace touchfilter
