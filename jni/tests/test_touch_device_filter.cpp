#include "touch_device_filter.h"
#include <cassert>

int main() {
    using namespace touchfilter;

    Caps panel{};
    panel.mt_x = panel.mt_y = panel.direct = true;
    assert(classify(panel) == MultiTouch);

    Caps xiaomi_touch{};
    xiaomi_touch.mt_x = xiaomi_touch.mt_y = true;
    xiaomi_touch.pointer = true;
    assert(classify(xiaomi_touch) == NotTouch);

    Caps pen{};
    pen.abs_x = pen.abs_y = pen.btn_touch = pen.direct = pen.tool_pen = true;
    assert(classify(pen) == NotTouch);

    Caps vm{};
    vm.abs_x = vm.abs_y = vm.btn_touch = true;
    assert(classify(vm) == SingleTouch);

    Caps joystick{};
    joystick.abs_x = joystick.abs_y = true;
    assert(classify(joystick) == NotTouch);
}
