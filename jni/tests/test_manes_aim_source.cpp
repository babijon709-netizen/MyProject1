#include "manes_aim_source.h"

#include <cassert>
#include <cmath>
#include <limits>

int main() {
    using manes_aim_source::EntityName;

    assert(manes_aim_source::classify_entity_name("Elite Crate(Clone)") == EntityName::EliteCrate);
    assert(manes_aim_source::classify_entity_name("elitecrate") == EntityName::EliteCrate);
    assert(manes_aim_source::classify_entity_name("Military Crate") == EntityName::MilitaryCrate);
    assert(manes_aim_source::classify_entity_name("ItemPickup") == EntityName::ItemPickup);
    assert(manes_aim_source::classify_entity_name("MineableCompositeStone(Clone)") == EntityName::CompositeStone);
    assert(manes_aim_source::classify_entity_name("MineableCompositeMetal") == EntityName::None);
    assert(manes_aim_source::classify_entity_name(nullptr) == EntityName::None);

    assert(std::fabs(manes_aim_source::angular_distance_degrees(0.0f, 0.0f)) < 0.001f);
    assert(std::fabs(manes_aim_source::angular_distance_degrees(10.0f, 0.0f) - 10.0f) < 0.001f);
    assert(std::fabs(manes_aim_source::angular_distance_degrees(0.0f, 15.0f) - 15.0f) < 0.001f);
    assert(manes_aim_source::angular_distance_degrees(10.0f, 10.0f) > 14.0f);
    assert(manes_aim_source::angular_distance_degrees(10.0f, 10.0f) < 15.0f);
    assert(!std::isfinite(manes_aim_source::angular_distance_degrees(
        std::numeric_limits<float>::quiet_NaN(), 0.0f)));

    assert(manes_aim_source::fov_radius_degrees(80.0f) == 40.0f);
    assert(manes_aim_source::inside_angular_fov(40.0f, 0.0f, 80.0f));
    assert(!manes_aim_source::inside_angular_fov(41.0f, 0.0f, 80.0f));
    assert(manes_aim_source::inside_angular_fov(45.0f, 0.0f, 80.0f, 5.0f));
    assert(!manes_aim_source::inside_angular_fov(
        std::numeric_limits<float>::infinity(), 0.0f, 80.0f));

    assert(std::fabs(manes_aim_source::frame_gain(0.5f, 1.0f / 60.0f) - 0.5f) < 0.001f);
    assert(std::fabs(manes_aim_source::frame_gain(0.5f, 2.0f / 60.0f) - 0.75f) < 0.001f);
    assert(manes_aim_source::frame_gain(0.0f, 1.0f / 60.0f) == 0.0f);
    assert(manes_aim_source::frame_gain(1.0f, 1.0f / 60.0f) == 1.0f);
    assert(manes_aim_source::frame_gain(0.5f, 0.0f) == 0.0f);
    return 0;
}
