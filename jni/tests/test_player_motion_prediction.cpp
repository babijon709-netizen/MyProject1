#include "player_motion_prediction.h"
#include <cassert>
#include <cmath>

static bool near(float a, float b, float eps = 0.001F) {
    return std::fabs(a - b) <= eps;
}

int main() {
    const Vec3 sample{10.0F, 2.0F, -4.0F};
    const Vec3 velocity{6.0F, 0.0F, -2.0F};

    Vec3 before = playermotion::predict(sample, velocity, 10.0, 10.005);
    assert(near(before.x, sample.x));
    assert(near(before.z, sample.z));

    Vec3 mid = playermotion::predict(sample, velocity, 10.0, 10.058);
    assert(near(mid.x, 10.3F));
    assert(near(mid.z, -4.1F));

    Vec3 capped = playermotion::predict(sample, velocity, 10.0, 11.0);
    assert(near(capped.x, 10.6F));
    assert(near(capped.z, -4.2F));

    Vec3 bounded = playermotion::predict(sample, {1000.0F, 0.0F, 0.0F}, 10.0, 11.0);
    assert(near(bounded.x, 11.2F));

    Vec3 invalid = playermotion::predict(sample, {NAN, 0.0F, 0.0F}, 10.0, 10.05);
    assert(near(invalid.x, sample.x));
}
