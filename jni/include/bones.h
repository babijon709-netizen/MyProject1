#pragma once
#include "Vector.h"
#include "skeleton.h"

// Real per-player bone positions (world space) read from the game's Animator
// skin matrix array (Unity 6). When the native data is unavailable or fails
// validation, `valid` stays 0 and the caller should fall back to the
// procedural skeleton.
struct BoneSet {
    Vec3 p[skeleton::BONE_COUNT];
    bool v[skeleton::BONE_COUNT];
    int  valid; // number of bones that were read successfully
};
