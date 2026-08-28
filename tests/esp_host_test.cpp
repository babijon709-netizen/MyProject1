// Host test for the ESP reader (jni/src/game.cpp).
//
// The reader only ever touches the game through process_vm_readv, so this test
// builds a fake il2cpp + Unity object graph inside its own process, points the
// reader at itself (pid == getpid()) and compares the boxes it returns with
// screen coordinates derived here from first principles.
//
// The fake layout is not invented: it mirrors what the shipped binaries do.
//   * Camera::GetProjectionMatrix (libunity.so 0xe1fef4) caches the projection
//     at +0xB0, reads the FOV from +0x40, and writes it column-major with
//     math (3,2) = -1 (see the SetPerspective helper at 0xeed114).
//   * Camera::GetWorldToCameraMatrix (0xe1fe90) caches the view at +0x70 and
//     reaches the transform through *(camera + 0x20) -> GameObject, whose
//     component array is at +0x20 with the count at +0x30 and 16-byte
//     {int32 classID, Component*} entries (helper at 0x640194).
//   * Transform -> TransformHierarchy at +0x38 and the transform index at +0x40;
//     the hierarchy keeps matrices at +0x18 and parent indices at +0x20 as
//     48-byte {translation, rotation, scale} elements, parent < 0 ends the
//     chain (Transform::GetLocalToWorldMatrix, 0x7f2904).
//   * Every managed offset (PlayerManager.worldCameraRoot 0x68, inventory 0x98,
//     lastTickPosition 0x1D0, PlayerInventory._playerInventoryData 0x20,
//     PlayerInventoryData.player 0x10 / .playerModelInfo 0x20,
//     PlayerModelInfo.head 0x20, GameControllerBase statics 0x10 / 0x38,
//     CameraManager.m_Camera 0x20) comes from dump.cs of the shipped build.
//
// Build & run: tests/run_host_tests.sh

#include "game.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

// --- mock offsets (see the header comment for where each one comes from) ------
static const uint64_t MOCK_CLASS_NAME          = 0x10;
static const uint64_t MOCK_CLASS_NAMESPACE     = 0x18;
static const uint64_t MOCK_CLASS_STATIC_FIELDS = 0xB8;
static const uint64_t MOCK_OBJECT_CACHED_PTR   = 0x10;
static const uint64_t MOCK_LIST_ITEMS          = 0x10;
static const uint64_t MOCK_LIST_SIZE           = 0x18;
static const uint64_t MOCK_ARRAY_FIRST         = 0x20;

static const uint64_t MOCK_PM_STATIC_SLEEPING  = 0x0;
static const uint64_t MOCK_PM_STATIC_ACTIVE    = 0x8;
static const uint64_t MOCK_PM_STATIC_CLIENT    = 0x10;
static const uint64_t MOCK_PM_TRANSFORM        = 0x68;
static const uint64_t MOCK_PM_INVENTORY        = 0x98;
static const uint64_t MOCK_PM_POSITION         = 0x1D0;

static const uint64_t MOCK_INV_DATA            = 0x20;
static const uint64_t MOCK_INVDATA_PLAYER      = 0x10;
static const uint64_t MOCK_INVDATA_MODEL       = 0x20;
static const uint64_t MOCK_MODEL_HEAD          = 0x20;

static const uint64_t MOCK_GCB_LOCAL_PLAYER    = 0x10;
static const uint64_t MOCK_GCB_CAMERA_MANAGER  = 0x38;
static const uint64_t MOCK_CAMERA_MANAGER_CAM  = 0x20;

static const uint64_t MOCK_CAM_GAMEOBJECT      = 0x20;
static const uint64_t MOCK_CAM_FOV             = 0x40;
static const uint64_t MOCK_CAM_VIEW            = 0x70;
static const uint64_t MOCK_CAM_PROJECTION      = 0xB0;

static const uint64_t MOCK_GO_COMPONENTS       = 0x20;
static const uint64_t MOCK_GO_COUNT            = 0x30;
static const uint64_t MOCK_COMP_STRIDE         = 0x10;
static const uint64_t MOCK_COMP_PTR            = 0x8;
static const int32_t  MOCK_TRANSFORM_CLASS_ID  = 4;

static const uint64_t MOCK_TR_HIERARCHY        = 0x38;
static const uint64_t MOCK_TR_INDEX            = 0x40;
static const uint64_t MOCK_HIER_MATRICES       = 0x18;
static const uint64_t MOCK_HIER_INDICES        = 0x20;

// --- arena --------------------------------------------------------------------
static uint8_t* g_arena = nullptr;
static size_t   g_arena_size = 64u * 1024u * 1024u;
static size_t   g_bump = 0;

static void arena_reset() { g_bump = 0; if (g_arena) memset(g_arena, 0, g_arena_size); }

static uint64_t alloc_zeroed(size_t bytes) {
    size_t aligned = (bytes + 15u) & ~(size_t)15u;
    if (g_bump + aligned > g_arena_size) { fprintf(stderr, "arena exhausted\n"); exit(2); }
    uint64_t address = (uint64_t)(g_arena + g_bump);
    g_bump += aligned;
    return address;
}

static void put64(uint64_t address, uint64_t value) { memcpy((void*)address, &value, 8); }
static void put32(uint64_t address, uint32_t value) { memcpy((void*)address, &value, 4); }
static void putf (uint64_t address, float value)    { memcpy((void*)address, &value, 4); }
static void putvec3(uint64_t address, float x, float y, float z) { putf(address, x); putf(address + 4, y); putf(address + 8, z); }
static void putquat(uint64_t address, float x, float y, float z, float w) {
    putf(address, x); putf(address + 4, y); putf(address + 8, z); putf(address + 12, w);
}
static uint64_t put_string(const char* text) {
    size_t length = strlen(text) + 1;
    uint64_t address = alloc_zeroed(length + 8);
    memcpy((void*)address, text, length);
    return address;
}

// --- libil2cpp.so stand-in ----------------------------------------------------
// esp_init() resolves the il2cpp base by scanning /proc/self/maps for a mapping
// named libil2cpp.so, so the mock has to be a real file mapping of that name.
static uint64_t g_il2cpp_base = 0;
static const uint64_t PLAYER_MANAGER_TYPEINFO_RVA   = 0xD2BBAD8;
static const uint64_t GAME_CONTROLLER_TYPEINFO_RVA  = 0xD2B6ED8;

static void map_fake_il2cpp() {
    if (g_il2cpp_base) return;
    const char* dir = "/tmp/xvcen_esp_test";
    mkdir(dir, 0777);
    std::string path = std::string(dir) + "/libil2cpp.so";
    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { perror("open libil2cpp.so"); exit(2); }
    uint64_t size = 0xD2C0000;
    if (ftruncate(fd, (off_t)size) != 0) { perror("ftruncate"); exit(2); }
    void* mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (mapped == MAP_FAILED) { perror("mmap libil2cpp.so"); exit(2); }
    g_il2cpp_base = (uint64_t)mapped;
}

// --- Unity transform hierarchy ------------------------------------------------
struct MockHierarchy {
    uint64_t hierarchy = 0;
    uint64_t matrices = 0;
    uint64_t indices = 0;
    int32_t  next_index = 0;
    int32_t  capacity = 0;
};
static MockHierarchy g_hierarchy;

static void hierarchy_create(int32_t capacity) {
    g_hierarchy.capacity = capacity;
    g_hierarchy.next_index = 0;
    g_hierarchy.hierarchy = alloc_zeroed(0x40);
    g_hierarchy.matrices = alloc_zeroed((size_t)capacity * 48);
    g_hierarchy.indices = alloc_zeroed((size_t)capacity * 4);
    put64(g_hierarchy.hierarchy + MOCK_HIER_MATRICES, g_hierarchy.matrices);
    put64(g_hierarchy.hierarchy + MOCK_HIER_INDICES, g_hierarchy.indices);
    // Root at index 0: identity transform, no parent.
    putquat(g_hierarchy.matrices + 0x10, 0.0F, 0.0F, 0.0F, 1.0F);
    putvec3(g_hierarchy.matrices + 0x20, 1.0F, 1.0F, 1.0F);
    put32(g_hierarchy.indices, (uint32_t)(int32_t)-1);
    g_hierarchy.next_index = 1;
}

// Adds a transform to the hierarchy and returns a managed Transform object
// (m_CachedPtr -> native transform) pointing at it.
static uint64_t make_transform(float x, float y, float z, float qx, float qy, float qz, float qw, int32_t parent = 0) {
    int32_t index = g_hierarchy.next_index++;
    if (index >= g_hierarchy.capacity) { fprintf(stderr, "hierarchy exhausted\n"); exit(2); }
    uint64_t matrix = g_hierarchy.matrices + (uint64_t)index * 48;
    putvec3(matrix, x, y, z);
    putquat(matrix + 0x10, qx, qy, qz, qw);
    putvec3(matrix + 0x20, 1.0F, 1.0F, 1.0F);
    put32(g_hierarchy.indices + (uint64_t)index * 4, (uint32_t)parent);

    uint64_t native = alloc_zeroed(0x60);
    put64(native + MOCK_TR_HIERARCHY, g_hierarchy.hierarchy);
    put32(native + MOCK_TR_INDEX, (uint32_t)index);
    uint64_t managed = alloc_zeroed(0x40);
    put64(managed + MOCK_OBJECT_CACHED_PTR, native);
    return managed;
}

// --- world used by the scenarios ---------------------------------------------
struct MockPlayer {
    uint64_t manager = 0;
    float x = 0, y = 0, z = 0;
    bool skeleton = true;      // inventory chain + head bone present
    bool field_ok = true;      // lastTickPosition holds the real feet
    bool field_garbage = false; // lastTickPosition holds NaN / huge values
    bool rig_ok = true;        // worldCameraRoot present
};

struct MockWorld {
    uint64_t native_camera = 0;
    float cam_x = 0, cam_y = 1.7F, cam_z = -5.0F;
    float fov = 60.0F;
    // true: the transform hangs off the camera's GameObject, the way the engine
    // reaches it. false: it only exists as a raw pointer inside the camera
    // object, which is what the offset scan fallback is for.
    bool camera_component_path = true;
    std::vector<MockPlayer> players;
};

static uint64_t make_il2cpp_class(const char* name, const char* ns, uint64_t static_fields) {
    uint64_t klass = alloc_zeroed(0x200);
    put64(klass + MOCK_CLASS_NAME, put_string(name));
    put64(klass + MOCK_CLASS_NAMESPACE, put_string(ns));
    put64(klass + MOCK_CLASS_STATIC_FIELDS, static_fields);
    return klass;
}

static uint64_t native_of(uint64_t managed) { return *(uint64_t*)(uintptr_t)(managed + MOCK_OBJECT_CACHED_PTR); }

static void build_world(MockWorld& world) {
    arena_reset();

    // --- transform hierarchy: root + camera + 2 transforms per player
    hierarchy_create(1 + 1 + (int32_t)world.players.size() * 2);
    uint64_t camera_transform = make_transform(world.cam_x, world.cam_y, world.cam_z, 0.0F, 0.0F, 0.0F, 1.0F);

    // --- native camera + its GameObject (component array holds the Transform)
    uint64_t camera_component_entry = alloc_zeroed(MOCK_COMP_STRIDE * 2);
    put32(camera_component_entry, (uint32_t)MOCK_TRANSFORM_CLASS_ID);
    put64(camera_component_entry + MOCK_COMP_PTR, native_of(camera_transform));
    put32(camera_component_entry + MOCK_COMP_STRIDE, 20); // Camera class id
    put64(camera_component_entry + MOCK_COMP_STRIDE + MOCK_COMP_PTR, alloc_zeroed(0x40));

    uint64_t game_object = alloc_zeroed(0x60);
    put64(game_object + MOCK_GO_COMPONENTS, camera_component_entry);
    put32(game_object + MOCK_GO_COUNT, 2);

    world.native_camera = alloc_zeroed(0x1000);
    if (world.camera_component_path) {
        put64(world.native_camera + MOCK_CAM_GAMEOBJECT, game_object);
    } else {
        put64(world.native_camera + 0x100, native_of(camera_transform));
        (void)game_object;
    }
    putf(world.native_camera + MOCK_CAM_FOV, world.fov);

    // Column-major GL perspective, exactly as SetPerspective writes it:
    // mem[0] = cot/aspect, mem[5] = cot, mem[10] = -(f+n)/(f-n),
    // mem[11] = -1 (math (3,2)), mem[14] = -2fn/(f-n).
    const float near_z = 0.3F, far_z = 1000.0F, aspect = 1920.0F / 1080.0F;
    float cot = 1.0F / tanf(world.fov * 3.14159265F / 180.0F * 0.5F);
    putf(world.native_camera + MOCK_CAM_PROJECTION + 0x00, cot / aspect);
    putf(world.native_camera + MOCK_CAM_PROJECTION + 0x14, cot);
    putf(world.native_camera + MOCK_CAM_PROJECTION + 0x28, -(far_z + near_z) / (far_z - near_z));
    putf(world.native_camera + MOCK_CAM_PROJECTION + 0x2C, -1.0F);
    putf(world.native_camera + MOCK_CAM_PROJECTION + 0x38, -2.0F * far_z * near_z / (far_z - near_z));
    // +0x70 (worldToCameraMatrix) stays all-zero: the engine only fills it when
    // managed code asks, so the reader must build the view from the transform.

    // --- managed camera chain: GameControllerBase statics -> CameraManager
    uint64_t managed_camera = alloc_zeroed(0x40);
    put64(managed_camera + MOCK_OBJECT_CACHED_PTR, world.native_camera);
    uint64_t camera_manager = alloc_zeroed(0x60);
    put64(camera_manager + MOCK_CAMERA_MANAGER_CAM, managed_camera);

    // --- players
    uint64_t player_static_fields = alloc_zeroed(0x40);
    for (MockPlayer& player : world.players) {
        player.manager = alloc_zeroed(0x400);
        if (player.rig_ok)
            put64(player.manager + MOCK_PM_TRANSFORM, make_transform(player.x, player.y + 1.6F, player.z, 0, 0, 0, 1.0F));
        if (player.field_ok)
            putvec3(player.manager + MOCK_PM_POSITION, player.x, player.y, player.z);
        if (player.field_garbage) {
            float nan_value = std::nanf("");
            putf(player.manager + MOCK_PM_POSITION, nan_value);
            putf(player.manager + MOCK_PM_POSITION + 4, 1.0e30F);
            putf(player.manager + MOCK_PM_POSITION + 8, -1.0e30F);
        }
        if (player.skeleton) {
            uint64_t head = make_transform(player.x, player.y + 1.58F, player.z, 0, 0, 0, 1.0F);
            uint64_t model_info = alloc_zeroed(0x60);
            put64(model_info + MOCK_MODEL_HEAD, head);
            uint64_t inventory_data = alloc_zeroed(0x40);
            put64(inventory_data + MOCK_INVDATA_PLAYER, player.manager);
            put64(inventory_data + MOCK_INVDATA_MODEL, model_info);
            uint64_t inventory = alloc_zeroed(0x40);
            put64(inventory + MOCK_INV_DATA, inventory_data);
            put64(player.manager + MOCK_PM_INVENTORY, inventory);
        }
    }

    // --- PlayerManager class + clientPlayerList
    uint64_t player_class = make_il2cpp_class("PlayerManager", "Oxide", player_static_fields);
    for (MockPlayer& player : world.players) put64(player.manager, player_class);

    uint64_t list_class = make_il2cpp_class("List`1", "System.Collections.Generic", 0);
    uint64_t items = alloc_zeroed(MOCK_ARRAY_FIRST + 8 * (world.players.size() + 1));
    put64(items, list_class);
    for (size_t i = 0; i < world.players.size(); ++i)
        put64(items + MOCK_ARRAY_FIRST + i * 8, world.players[i].manager);
    uint64_t client_list = alloc_zeroed(0x40);
    put64(client_list, list_class);
    put64(client_list + MOCK_LIST_ITEMS, items);
    put32(client_list + MOCK_LIST_SIZE, (uint32_t)world.players.size());
    put64(player_static_fields + MOCK_PM_STATIC_SLEEPING, 0);
    put64(player_static_fields + MOCK_PM_STATIC_ACTIVE, 0);
    put64(player_static_fields + MOCK_PM_STATIC_CLIENT, client_list);

    // --- GameControllerBase class + statics
    uint64_t controller_static_fields = alloc_zeroed(0x60);
    put64(controller_static_fields + MOCK_GCB_LOCAL_PLAYER, world.players.empty() ? 0 : world.players[0].manager);
    put64(controller_static_fields + MOCK_GCB_CAMERA_MANAGER, camera_manager);
    uint64_t controller_class = make_il2cpp_class("GameControllerBase", "Oxide", controller_static_fields);
    (void)controller_class;

    // --- TypeInfo pointers, where the reader looks for them
    put64(g_il2cpp_base + PLAYER_MANAGER_TYPEINFO_RVA, player_class);
    put64(g_il2cpp_base + GAME_CONTROLLER_TYPEINFO_RVA, controller_class);
}

// --- independent expectation --------------------------------------------------
// Perspective projection done by hand: rotate the world offset into camera
// space, divide by the forward distance, scale by cot(fov/2). No matrices from
// game.cpp are involved, so this really cross-checks its pipeline.
struct ScreenPoint { float x, y; bool visible; };

static ScreenPoint project(const MockWorld& world, float sw, float sh, float x, float y, float z) {
    ScreenPoint out{0, 0, false};
    float rx = x - world.cam_x, ry = y - world.cam_y, rz = z - world.cam_z;
    // Camera basis: identity rotation -> right = +X, up = +Y, forward = +Z.
    float forward = rz;
    if (forward <= 0.001F) return out;
    float cot = 1.0F / tanf(world.fov * 3.14159265F / 180.0F * 0.5F);
    float aspect = sw / sh;
    float ndc_x = (cot / aspect) * rx / forward;
    float ndc_y = cot * ry / forward;
    out.x = (ndc_x + 1.0F) * 0.5F * sw;
    out.y = (1.0F - ndc_y) * 0.5F * sh;
    out.visible = true;
    return out;
}

// --- assertions ---------------------------------------------------------------
static int g_failures = 0;

static void check(bool condition, const std::string& what) {
    if (condition) { printf("  ok   %s\n", what.c_str()); }
    else { printf("  FAIL %s\n", what.c_str()); ++g_failures; }
}

static void check_near(float actual, float expected, float tolerance, const std::string& what) {
    bool ok = std::isfinite(actual) && fabsf(actual - expected) <= tolerance;
    if (ok) printf("  ok   %s (%.1f ~ %.1f)\n", what.c_str(), actual, expected);
    else { printf("  FAIL %s (got %.2f, want %.2f +- %.2f)\n", what.c_str(), actual, expected, tolerance); ++g_failures; }
}

// Runs every scenario against the same world shape: three remote players in
// front of the camera, the local player behind them. What changes is which
// position source is still alive.
struct Scenario {
    const char* name;
    bool skeleton, field, garbage, rig;
    bool camera_components;
    bool expect_boxes;
};

static void run_scenario(const Scenario& scenario, const ScreenPoint* expected_centers, size_t expected_count) {
    printf("[%s]\n", scenario.name);
    MockWorld world;
    world.players.push_back(MockPlayer{});                                   // local, skipped
    world.players[0].x = 0.0F; world.players[0].y = 0.0F; world.players[0].z = 2.0F;
    const float zs[3] = {5.0F, 15.0F, 30.0F};
    for (int i = 0; i < 3; ++i) {
        MockPlayer player{};
        player.x = 0.0F; player.y = 0.0F; player.z = zs[i];
        player.skeleton = scenario.skeleton;
        player.field_ok = scenario.field;
        player.field_garbage = scenario.garbage;
        player.rig_ok = scenario.rig;
        world.players.push_back(player);
    }
    for (MockPlayer& player : world.players) {
        player.skeleton = scenario.skeleton;
        player.field_ok = scenario.field;
        player.field_garbage = scenario.garbage;
        player.rig_ok = scenario.rig;
    }

    world.camera_component_path = scenario.camera_components;
    build_world(world);
    esp_reset();
    if (!esp_init((pid_t)getpid())) { check(false, "esp_init"); return; }

    std::vector<EspBox> boxes = esp_get_boxes(1920, 1080);
    if (!scenario.expect_boxes) {
        check(boxes.empty(), "no boxes when nothing is readable");
        return;
    }
    check(boxes.size() == expected_count, "box count == " + std::to_string(expected_count) +
          " (got " + std::to_string(boxes.size()) + ")");
    if (boxes.size() != expected_count) return;

    // esp_get_boxes walks the snapshot in order, so box i is remote player i.
    for (size_t i = 0; i < boxes.size() && i < expected_count; ++i) {
        float center_x = (boxes[i].x1 + boxes[i].x2) * 0.5F;
        float center_y = (boxes[i].y1 + boxes[i].y2) * 0.5F;
        check_near(center_x, expected_centers[i].x, 2.0F, "player " + std::to_string(i) + " box center x");
        check_near(center_y, expected_centers[i].y, 2.0F, "player " + std::to_string(i) + " box center y");
        check(boxes[i].distance > 0.0F, "player " + std::to_string(i) + " distance resolved");
    }
    check(esp_get_aim_targets().size() == boxes.size(), "aim targets match boxes");
}

int main() {
    map_fake_il2cpp();
    g_arena = (uint8_t*)mmap(nullptr, g_arena_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_arena == MAP_FAILED) { perror("mmap arena"); return 2; }

    const float sw = 1920.0F, sh = 1080.0F;
    // Expected box centers: the middle of feet (y=0) and head top (y=1.76).
    MockWorld reference;
    reference.cam_x = 0.0F; reference.cam_y = 1.7F; reference.cam_z = -5.0F;
    ScreenPoint expected[3];
    const float zs[3] = {5.0F, 15.0F, 30.0F};
    for (int i = 0; i < 3; ++i) {
        ScreenPoint bottom = project(reference, sw, sh, 0.0F, 0.0F, zs[i]);
        ScreenPoint top = project(reference, sw, sh, 0.0F, 1.76F, zs[i]);
        expected[i].x = (bottom.x + top.x) * 0.5F;
        expected[i].y = (bottom.y + top.y) * 0.5F;
        expected[i].visible = bottom.visible && top.visible;
    }

    // 1. Everything healthy: the managed field is bound and used.
    run_scenario({"managed field + skeleton", true, true, false, true, true, true}, expected, 3);
    // 2. The regression this test exists for: the installed build never fills
    //    lastTickPosition in for remote players, so the field is dead weight
    //    and the skeleton has to carry the boxes.
    run_scenario({"dead managed field, skeleton alive", true, false, false, true, true, true}, expected, 3);
    // 3. Models not streamed in yet: no head bone, the field still works.
    run_scenario({"no skeleton, managed field alive", false, true, false, true, true, true}, expected, 3);
    // 4. Neither the field nor the skeleton: only the worldCameraRoot rig.
    run_scenario({"rig only", false, false, false, true, true, true}, expected, 3);
    // 5. The field holds NaN/huge garbage (stale object, mid-streaming read):
    //    it must be rejected, not projected.
    run_scenario({"garbage managed field, skeleton alive", true, false, true, true, true, true}, expected, 3);
    // 6. Camera transform not reachable through the GameObject: the offset scan
    //    fallback still has to find it, otherwise every box lands in the wrong
    //    place (the old code silently used the local player's rig as camera).
    run_scenario({"camera via object scan", true, true, false, true, false, true}, expected, 3);
    // 7. Nothing at all is readable: no crash, no boxes.
    run_scenario({"nothing readable", false, false, false, false, true, false}, expected, 0);
    esp_reset();
    printf(g_failures == 0 ? "\nALL PASS\n" : "\nFAILURES: %d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
