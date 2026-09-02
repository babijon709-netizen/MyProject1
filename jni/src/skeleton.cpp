#include "skeleton.h"
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

// -------------------------------------------------------------------------
// Global Config
// -------------------------------------------------------------------------
bool ESP_Skeleton = false;
static pid_t s_pid = -1;

// You can adjust these offsets via Skeleton_SetOffsets() if they change in your Unity version
static uint64_t offset_gameObject = 0x30;
static uint64_t offset_name = 0x60;
static uint64_t offset_children = 0x70; // NativeTransform -> Child Array
static uint64_t offset_childCount = 0x80; // NativeTransform -> Child Count

void Skeleton_SetPID(pid_t pid) {
    s_pid = pid;
}

void Skeleton_SetOffsets(uint64_t goOffset, uint64_t nameOffset, uint64_t childrenOffset) {
    offset_gameObject = goOffset;
    offset_name = nameOffset;
    offset_children = childrenOffset;
}

// -------------------------------------------------------------------------
// Memory Readers
// -------------------------------------------------------------------------
static ssize_t remote_vm_readv_skel(pid_t pid, const struct iovec* local_iov, unsigned long liovcnt, const struct iovec* remote_iov, unsigned long riovcnt, unsigned long flags) {
    return syscall(__NR_process_vm_readv, pid, local_iov, liovcnt, remote_iov, riovcnt, flags);
}

template<typename T>
static T rd(uint64_t addr) {
    T v{};
    if (s_pid == -1 || !addr) return v;
    struct iovec lv = { &v, sizeof(T) };
    struct iovec rv = { (void*)addr, sizeof(T) };
    remote_vm_readv_skel(s_pid, &lv, 1, &rv, 1, 0);
    return v;
}

static uint64_t rd_ptr(uint64_t addr) {
    return rd<uint64_t>(addr);
}

static std::string read_remote_string(uint64_t address) {
    if (!address || s_pid == -1) return {};
    char buffer[128]{};
    struct iovec local = {buffer, sizeof(buffer) - 1};
    struct iovec remote = {(void*)address, sizeof(buffer) - 1};
    ssize_t count = remote_vm_readv_skel(s_pid, &local, 1, &remote, 1, 0);
    if (count <= 0) return {};
    buffer[sizeof(buffer) - 1] = '\0';
    return std::string(buffer);
}

static std::string Utf16ToUtf8_Ext(uint64_t str_ptr) {
    // If str_ptr points to an Il2CppString (or C# string), the text is at offset 0x14 usually
    // Unity uses 2 bytes per char. For simplicity, we just read the raw buffer and cast to char if it's ascii.
    // Or if the string passed here is just a C string pointer (gameObject -> name is usually a C string at 0x60 in 64-bit Unity)
    // We will just assume it's a C-string if it comes from GameObject.name.
    return read_remote_string(str_ptr);
}

// -------------------------------------------------------------------------
// Unity Hierarchy Helpers
// -------------------------------------------------------------------------
static std::string GetTransformNameExt(uint64_t transform) {
    if (!transform) return "";
    uint64_t gameObject = rd_ptr(transform + offset_gameObject);
    if (!gameObject) return "";
    uint64_t namePtr = rd_ptr(gameObject + offset_name);
    return Utf16ToUtf8_Ext(namePtr);
}

static int GetChildCountExt(uint64_t transform) {
    uint64_t native_transform = rd_ptr(transform + 0x10);
    if (!native_transform) return 0;
    
    // Example for typical Unity version, you might need to adjust offsets.
    // Some Unity versions store child count at 0x58, 0x78 or 0x80 inside Native Transform.
    int count = rd<int>(native_transform + 0x58); // Commonly 0x58 or 0x80
    if (count > 0 && count < 200) return count;
    
    count = rd<int>(native_transform + 0x80);
    if (count > 0 && count < 200) return count;

    return 0;
}

static uint64_t GetChildExt(uint64_t transform, int index) {
    uint64_t native_transform = rd_ptr(transform + 0x10);
    if (!native_transform) return 0;

    // Children array is typically at 0x70 in Native Transform.
    uint64_t childrenArray = rd_ptr(native_transform + 0x70);
    if (!childrenArray) return 0;

    // The array contains pointers to Native Transform objects.
    uint64_t childNative = rd_ptr(childrenArray + (index * 0x8));
    if (!childNative) return 0;

    // Get the C# Transform object from the Native Transform (usually at 0x20, 0x28, or 0x30).
    // Or if the child array stores C# Transform components directly. Usually it stores NativeTransforms.
    // Many external bases just read the C# component via childNative+0x28 or similar.
    return rd_ptr(childNative + 0x20); // Common offset back to C# Component
}

// -------------------------------------------------------------------------
// Core Caching Logic Adapted for External
// -------------------------------------------------------------------------
void CacheSkeleton(uint64_t transform, PlayerSkeleton& skel, int depth)
{
    if (!transform)
        return;

    if (depth > 40)
        return;

    std::string name = GetTransformNameExt(transform);
    if (name.empty())
        return;

    // Direct mapping logic from original source
    if (name == "Hips") skel.Hips = transform;
    else if (name == "Spine") skel.Spine = transform;
    else if (name == "Spine1") skel.Spine1 = transform;
    else if (name == "Spine2") skel.Spine2 = transform;
    else if (name == "Neck") skel.Neck = transform;
    else if (name == "Head") skel.Head = transform;
    else if (name == "Head_HitBox") skel.Head_HitBox = transform;
    else if (name == "Hips_HitBox") skel.Hips_HitBox = transform;
    else if (name == "Spine_HitBox") skel.Spine_HitBox = transform;
    else if (name == "Spine1_HitBox") skel.Spine1_HitBox = transform;
    else if (name == "Spine2_HitBox") skel.Spine2_HitBox = transform;

    // Left Arm
    else if (name == "Shoulder.L") skel.ShoulderL = transform;
    else if (name == "Arm.L") skel.ArmL = transform;
    else if (name == "ForeArm.L") skel.ForeArmL = transform;
    else if (name == "Hand.L") skel.HandL = transform;

    // Right Arm
    else if (name == "Shoulder.R") skel.ShoulderR = transform;
    else if (name == "Arm.R") skel.ArmR = transform;
    else if (name == "ForeArm.R") skel.ForeArmR = transform;
    else if (name == "Hand.R") skel.HandR = transform;

    // Left Leg
    else if (name == "UpLeg.L") skel.UpLegL = transform;
    else if (name == "Leg.L") skel.LegL = transform;
    else if (name == "Foot.L") skel.FootL = transform;
    else if (name == "ToeBase.L") skel.ToeBaseL = transform;

    // Right Leg
    else if (name == "UpLeg.R") skel.UpLegR = transform;
    else if (name == "Leg.R") skel.LegR = transform;
    else if (name == "Foot.R") skel.FootR = transform;
    else if (name == "ToeBase.R") skel.ToeBaseR = transform;

    int count = GetChildCountExt(transform);
    if (count <= 0 || count > 200)
        return;

    for (int i = 0; i < count; i++)
    {
        uint64_t child = GetChildExt(transform, i);
        if (!child)
            continue;

        CacheSkeleton(child, skel, depth + 1);
    }
}

// -------------------------------------------------------------------------
// Drawing Functions Adapted for External
// -------------------------------------------------------------------------
static void DrawBone(uint64_t a, uint64_t b, W2S_Callback w2s_cb, GetTransformPosition_Callback pos_cb, ImDrawList* drawList, float screenHeight, ImU32 color)
{
    if (!a || !b)
        return;

    Vec3 posA = {};
    Vec3 posB = {};

    if (!pos_cb(a, posA) || !pos_cb(b, posB))
        return;

    Vec2 screenA = {};
    Vec2 screenB = {};

    if (!w2s_cb(posA, screenA) || !w2s_cb(posB, screenB))
        return;

    // Convert Y axis if needed by ImGui (usually ImGui handles this, but matching your source)
    // screenA.y = screenHeight - screenA.y;
    // screenB.y = screenHeight - screenB.y;

    drawList->AddLine(
        ImVec2(screenA.x, screenA.y),
        ImVec2(screenB.x, screenB.y),
        color,
        1.5f
    );
}

static void DrawSkeleton(PlayerSkeleton& s, W2S_Callback w2s_cb, GetTransformPosition_Callback pos_cb, ImDrawList* drawList, float screenHeight, ImU32 color)
{
    // Body
    DrawBone(s.Hips, s.Spine, w2s_cb, pos_cb, drawList, screenHeight, color);
    DrawBone(s.Spine, s.Spine1, w2s_cb, pos_cb, drawList, screenHeight, color);
    DrawBone(s.Spine1, s.Spine2, w2s_cb, pos_cb, drawList, screenHeight, color);
    DrawBone(s.Spine2, s.Neck, w2s_cb, pos_cb, drawList, screenHeight, color);
    DrawBone(s.Neck, s.Head, w2s_cb, pos_cb, drawList, screenHeight, color);

    // Left Arm
    DrawBone(s.Spine2, s.ShoulderL, w2s_cb, pos_cb, drawList, screenHeight, color);
    DrawBone(s.ShoulderL, s.ArmL, w2s_cb, pos_cb, drawList, screenHeight, color);
    DrawBone(s.ArmL, s.ForeArmL, w2s_cb, pos_cb, drawList, screenHeight, color);
    DrawBone(s.ForeArmL, s.HandL, w2s_cb, pos_cb, drawList, screenHeight, color);

    // Right Arm
    DrawBone(s.Spine2, s.ShoulderR, w2s_cb, pos_cb, drawList, screenHeight, color);
    DrawBone(s.ShoulderR, s.ArmR, w2s_cb, pos_cb, drawList, screenHeight, color);
    DrawBone(s.ArmR, s.ForeArmR, w2s_cb, pos_cb, drawList, screenHeight, color);
    DrawBone(s.ForeArmR, s.HandR, w2s_cb, pos_cb, drawList, screenHeight, color);

    // Left Leg
    DrawBone(s.Hips, s.UpLegL, w2s_cb, pos_cb, drawList, screenHeight, color);
    DrawBone(s.UpLegL, s.LegL, w2s_cb, pos_cb, drawList, screenHeight, color);
    DrawBone(s.LegL, s.FootL, w2s_cb, pos_cb, drawList, screenHeight, color);
    DrawBone(s.FootL, s.ToeBaseL, w2s_cb, pos_cb, drawList, screenHeight, color);

    // Right Leg
    DrawBone(s.Hips, s.UpLegR, w2s_cb, pos_cb, drawList, screenHeight, color);
    DrawBone(s.UpLegR, s.LegR, w2s_cb, pos_cb, drawList, screenHeight, color);
    DrawBone(s.LegR, s.FootR, w2s_cb, pos_cb, drawList, screenHeight, color);
    DrawBone(s.FootR, s.ToeBaseR, w2s_cb, pos_cb, drawList, screenHeight, color);
}

void DrawSkeletonESP(std::unordered_map<uint64_t, PlayerSkeleton>& skeletonCache, std::vector<uint64_t>& activePlayers, W2S_Callback w2s_cb, GetTransformPosition_Callback pos_cb, float screenHeight, ImU32 color)
{
    if (!ESP_Skeleton)
        return;

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    if (!drawList)
        return;

    for (uint64_t player : activePlayers)
    {
        auto it = skeletonCache.find(player);
        if (it != skeletonCache.end()) {
            DrawSkeleton(it->second, w2s_cb, pos_cb, drawList, screenHeight, color);
        }
    }
}
