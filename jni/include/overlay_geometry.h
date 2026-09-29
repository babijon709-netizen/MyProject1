#pragma once

// One geometry contract shared by EGL/ImGui and world-space projection.
//
// Android rotates/crops a Surface according to display orientation. A square
// max-side Surface therefore covers portrait and landscape without having to
// destroy/recreate EGL on every 90-degree turn. The game itself is projected
// in landscape coordinates (long side x short side), while menus clamp to the
// physical visible width/height reported by DisplayInfo.
struct OverlayGeometry {
    int visible_width = 0;
    int visible_height = 0;
    int scene_width = 0;
    int scene_height = 0;
    int surface_side = 0;
};

inline OverlayGeometry MakeOverlayGeometry(int display_width, int display_height,
                                           int fallback_width = 1920,
                                           int fallback_height = 1080) {
    const bool display_valid = display_width >= 100 && display_height >= 100;
    int width = display_valid ? display_width : fallback_width;
    int height = display_valid ? display_height : fallback_height;
    if (width < 100) width = 1920;
    if (height < 100) height = 1080;

    OverlayGeometry result{};
    result.visible_width = width;
    result.visible_height = height;
    result.scene_width = width > height ? width : height;
    result.scene_height = width > height ? height : width;
    result.surface_side = result.scene_width;
    return result;
}
