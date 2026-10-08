#include "vischeck.hpp"

#include <cassert>
#include <cmath>
#include <limits>

namespace {

bool near(float a, float b, float epsilon = 0.02f) {
    return std::fabs(a - b) <= epsilon;
}

} // namespace

int main() {
    using namespace manes;

    const float fov = 70.f * kPi / 180.f;
    Vec2 projected{};
    assert(is_visible({13.7f, 2.1f, 45.f}, projected, fov, 1080.f, 2.5f));
    assert(near(projected.x, 576.504f));
    assert(near(projected.y, -4.345f));

    Vec2 unchanged{12.f, 34.f};
    assert(!is_visible({0.f, 0.f, 1.0999f}, unchanged, fov, 1080.f, 2.5f));
    assert(unchanged.x == 12.f && unchanged.y == 34.f);
    assert(is_visible({0.f, 0.f, kNearPlane}, projected, fov, 1080.f, 1.f));

    const float tiny_fov = 0.005f;
    assert(project_point({0.f, 0.f, 20.f}, projected, tiny_fov, 1080.f, 1.f) > 0.f);
    assert(!is_visible({0.f, 0.f, 20.f}, unchanged, tiny_fov, 1080.f, 1.f));
    assert(unchanged.x == 12.f && unchanged.y == 34.f);

    const float half_fov = kPi * 0.5f;
    const float scale = 180.f / (kPi * half_fov);
    const float x_for_sentinel = -1.1f / scale;
    assert(is_visible({x_for_sentinel, 0.f, 2.f}, projected, half_fov, 2.f, 1.f));
    assert(near(projected.x, kInvisibleSentinel, 0.0001f));

    const float nan = std::numeric_limits<float>::quiet_NaN();
    assert(!is_visible({nan, 0.f, 5.f}, unchanged, fov, 1080.f, 1.f));
    assert(!is_visible({0.f, 0.f, 5.f}, unchanged, nan, 1080.f, 1.f));
    assert(unchanged.x == 12.f && unchanged.y == 34.f);
    return 0;
}
