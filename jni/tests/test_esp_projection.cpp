#include "esp_projection.h"

#include <cmath>
#include <cstdio>

namespace {
int failures = 0;

#define CHECK(condition, name) do { \
    if (!(condition)) { ++failures; std::printf("FAIL: %s (line %d)\n", name, __LINE__); } \
} while (0)

Mat4 multiply(const Mat4& left, const Mat4& right) {
    Mat4 result{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            float value = 0.0F;
            for (int k = 0; k < 4; ++k)
                value += espprojection::get(left, row, k) * espprojection::get(right, k, column);
            espprojection::set(result, row, column, value);
        }
    }
    return result;
}

bool near(float a, float b, float tolerance = 0.01F) {
    return std::fabs(a - b) <= tolerance;
}
} // namespace

int main() {
    Mat4 view{}, projection{};
    CHECK(espprojection::make_view({0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F, 1.0F}, view),
          "identity camera view is valid");
    CHECK(espprojection::make_perspective(90.0F, 1.0F, 0.1F, 1000.0F, projection),
          "valid perspective is built");
    const Mat4 vp = multiply(projection, view);

    Vec2 screen{};
    CHECK(espprojection::project(vp, {0.0F, 0.0F, 10.0F}, 200.0F, 100.0F, screen),
          "forward point projects");
    CHECK(near(screen.x, 100.0F) && near(screen.y, 50.0F), "forward point is centered");
    CHECK(espprojection::project(vp, {10.0F, 0.0F, 10.0F}, 200.0F, 100.0F, screen, false),
          "unclipped point projects outside viewport");
    CHECK(near(screen.x, 200.0F) && near(screen.y, 50.0F), "right edge follows perspective");
    CHECK(!espprojection::project(vp, {0.0F, 0.0F, -1.0F}, 200.0F, 100.0F, screen),
          "point behind camera is rejected");
    CHECK(!espprojection::project(vp, {0.0F, 0.0F, 10.0F}, 0.0F, 100.0F, screen),
          "invalid viewport is rejected");

    constexpr float half_sqrt = 0.70710678118F;
    Mat4 yaw_view{};
    CHECK(espprojection::make_view({0.0F, 0.0F, 0.0F},
                                    {0.0F, half_sqrt, 0.0F, half_sqrt}, yaw_view),
          "rotated camera view is valid");
    const Mat4 yaw_vp = multiply(projection, yaw_view);
    CHECK(espprojection::project(yaw_vp, {10.0F, 0.0F, 0.0F}, 200.0F, 100.0F, screen),
          "yaw rotates the forward direction");
    CHECK(near(screen.x, 100.0F) && near(screen.y, 50.0F), "yaw target stays centered");

    Mat4 invalid{};
    CHECK(!espprojection::make_perspective(0.0F, 1.0F, 0.1F, 1000.0F, invalid),
          "invalid field of view is rejected");
    CHECK(failures == 0, "all projection checks passed");
    if (failures == 0) std::puts("ESP projection tests passed");
    return failures == 0 ? 0 : 1;
}
