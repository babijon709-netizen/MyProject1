#include "overlay_geometry.h"

#include <cassert>
#include <iostream>

static void expect_geometry(int width, int height,
                            int visible_width, int visible_height,
                            int scene_width, int scene_height,
                            int surface_side) {
    const OverlayGeometry value = MakeOverlayGeometry(width, height);
    assert(value.visible_width == visible_width);
    assert(value.visible_height == visible_height);
    assert(value.scene_width == scene_width);
    assert(value.scene_height == scene_height);
    assert(value.surface_side == surface_side);
}

int main() {
    // The EGL/ImGui canvas is rotation invariant, but world projection remains
    // in the game's landscape coordinate system.
    expect_geometry(2460, 1080, 2460, 1080, 2460, 1080, 2460);
    expect_geometry(1080, 2460, 1080, 2460, 2460, 1080, 2460);

    // 180-degree orientations report the same dimensions and must not change
    // either contract.
    expect_geometry(1920, 1080, 1920, 1080, 1920, 1080, 1920);
    expect_geometry(1080, 1920, 1080, 1920, 1920, 1080, 1920);

    const OverlayGeometry fallback = MakeOverlayGeometry(0, 0, 1280, 720);
    assert(fallback.visible_width == 1280);
    assert(fallback.visible_height == 720);
    assert(fallback.scene_width == 1280);
    assert(fallback.scene_height == 720);
    assert(fallback.surface_side == 1280);

    std::cout << "overlay orientation geometry: OK\n";
    return 0;
}
