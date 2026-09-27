#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "game.h"
#include "game_offsets_active.h"
#include "farm_memory.h"

// Mock memory provider for testing farm_tuning::Controller
struct MockMemory {
    std::map<uint64_t, std::vector<uint8_t>> storage;
    std::map<uint64_t, std::pair<std::string, std::string>> class_names;

    template<class T> bool read(uint64_t address, T& value) {
        if (address < 0x10000) return false;
        auto it = storage.find(address & ~0x7ULL);
        if (it == storage.end()) return false;
        size_t offset = address & 0x7ULL;
        if (offset + sizeof(T) > it->second.size()) return false;
        std::memcpy(&value, it->second.data() + offset, sizeof(T));
        return true;
    }

    template<class T> bool write_heap(uint64_t address, const T& value) {
        if (address < 0x10000) return false;
        uint64_t aligned = address & ~0x7ULL;
        auto& buf = storage[aligned];
        if (buf.size() < 16) buf.resize(16, 0);
        size_t offset = address & 0x7ULL;
        if (offset + sizeof(T) > buf.size()) buf.resize(offset + sizeof(T), 0);
        std::memcpy(buf.data() + offset, &value, sizeof(T));
        return true;
    }

    bool named(uint64_t object, const char* name, const char* namespaze) {
        if (object < 0x10000) return false;
        auto it = class_names.find(object);
        if (it == class_names.end()) return false;
        return it->second.first == name && it->second.second == namespaze;
    }

    bool named_klass(uint64_t klass, const char* name, const char* namespaze) {
        if (klass < 0x10000) return false;
        auto it = class_names.find(klass);
        if (it == class_names.end()) return false;
        return it->second.first == name && it->second.second == namespaze;
    }

    void set_ptr(uint64_t address, uint64_t val) {
        write_heap(address, val);
    }
    void set_float(uint64_t address, float val) {
        write_heap(address, val);
    }
    void set_byte(uint64_t address, uint8_t val) {
        write_heap(address, val);
    }
};

// Global stubs for esp functions referenced by farm_memory.h
static FarmTarget s_test_target{};
static bool s_have_test_target = false;
static float s_test_eye[3] = { 100.f, 20.f, 100.f };

bool esp_farm_get_target(FarmTarget& out) {
    if (!s_have_test_target) return false;
    out = s_test_target;
    return true;
}

bool esp_local_eye_position(float& x, float& y, float& z) {
    x = s_test_eye[0]; y = s_test_eye[1]; z = s_test_eye[2];
    return true;
}

int main() {
    printf("--- Запуск тестов farm_memory.h ---\n");

    MockMemory mem;
    farm_tuning::Controller<MockMemory> ctrl(mem);

    // Setup mock game heap layout
    const uint64_t kPlayer = 0x100000;
    const uint64_t kPlayerClass = 0x200000;
    const uint64_t kFpManager = 0x300000;
    const uint64_t kWeapon = 0x400000;
    const uint64_t kWeaponClass = 0x500000;
    const uint64_t kItem = 0x600000;
    const uint64_t kItemData = 0x700000;
    const uint64_t kItemConfig = 0x800000;
    const uint64_t kItemState = 0x900000;
    const uint64_t kRayman = 0xA00000;
    const uint64_t kEvByr = 0xB00000;
    const uint64_t kEvByi = 0xB10000;
    const uint64_t kEvByt = 0xB20000;
    const uint64_t kEvBbk = 0xB30000;
    const uint64_t kDelegateAct = 0xC00000;
    const uint64_t kAnimator = 0xD00000;
    const uint64_t kDrawEv = 0xE00000;
    const uint64_t kDrawAct = 0xE10000;
    const uint64_t kMotion = 0xE20000;
    const uint64_t kAttackOffset = 0xE30000;
    const uint64_t kEventHandler = 0xF00000;
    const uint64_t kLookWrapper = 0xF10000;

    // Set class types
    mem.class_names[kPlayer] = { "BasePlayer", "Oxide" };
    mem.class_names[kWeapon] = { "FPTool", "Oxide" };
    mem.class_names[kMotion] = { "FPMotion", "Oxide" };
    mem.class_names[kAnimator] = { "FPMeleeAnimator", "Oxide" };

    // Player fields
    mem.set_ptr(kPlayer, kPlayerClass);
    mem.set_ptr(kPlayer + go_active::MANAGED_CACHED_PTR, kPlayer + 0x10);
    mem.set_ptr(kPlayer + go_active::PLAYER_FP_MANAGER, kFpManager);
    mem.set_ptr(kPlayer + go_active::PLAYER_EVENT_HANDLER, kEventHandler);

    // FPManager fields
    mem.set_ptr(kFpManager, 0x200010);
    mem.set_ptr(kFpManager + go_active::FPMANAGER_CURRENT_WEAPON, kWeapon);

    // Weapon fields
    mem.set_ptr(kWeapon, kWeaponClass);
    mem.set_ptr(kWeapon + go_active::MANAGED_CACHED_PTR, kWeapon + 0x10);
    mem.set_ptr(kWeapon + go_active::FPOBJECT_PLAYER_BACKREF, kPlayer);
    mem.set_byte(kWeapon + go_active::FPTOOL_ALWAYS_CRIT, 0);
    mem.set_float(kWeapon + go_active::FPMELEE_MAX_REACH, 1.5f);
    mem.set_float(kWeapon + go_active::FPMELEE_HIT_RADIUS, 0.3f);
    mem.set_float(kWeapon + go_active::FPMELEE_TIME_BETWEEN_ATTACKS, 0.5f);
    mem.set_float(kWeapon + go_active::FPMELEE_PAUSE_AFTER_ATTACK, 0.2f);
    mem.set_float(kWeapon + go_active::FPMELEE_IMPACT_FORCE, 10.0f);
    mem.set_ptr(kWeapon + go_active::FPOBJECT_ITEM, kItem);
    mem.set_ptr(kWeapon + go_active::FPOBJECT_RAYCAST_MANAGER, kRayman);
    mem.set_ptr(kWeapon + go_active::FPMELEE_EVENT_ATTACK, kEvByr);
    mem.set_ptr(kWeapon + go_active::FPMELEE_EVENT_WOOSH, kEvByi);
    mem.set_ptr(kWeapon + go_active::FPMELEE_EVENT_HIT, kEvByt);
    mem.set_ptr(kWeapon + go_active::FPWEAPONBASE_EVENT_ATTACK, kEvBbk);
    mem.set_ptr(kWeapon + go_active::FPOBJECT_DRAW_EVENT, kDrawEv);

    // Item & Durability fields
    mem.set_ptr(kItem, 0x600010);
    mem.set_ptr(kItem + go_active::ITEM_DATA, kItemData);
    mem.set_ptr(kItem + 0x28, kItemState);
    mem.set_ptr(kItemData, 0x700010);
    mem.set_ptr(kItemData + go_active::ITEMDATA_CONFIG_VALUES, kItemConfig);
    mem.set_ptr(kItemConfig, 0x800010);
    mem.set_byte(kItemConfig + go_active::ITEMCONFIG_DURABILITY_UNBREAKABLE, 0);
    mem.set_float(kItemState + 0x10, 45.0f); // current durability damaged
    mem.set_float(kItemState + 0x1C, 100.0f); // max durability

    // RaycastManager fields
    mem.set_ptr(kRayman, 0xA00010);
    mem.set_ptr(kRayman + go_active::MANAGED_CACHED_PTR, kRayman + 0x20);
    mem.set_ptr(kRayman + go_active::RAYCASTMAN_PLAYER, kPlayer);
    mem.set_float(kRayman + go_active::RAYCASTMAN_RAY_LENGTH, 1.5f);
    mem.set_float(kRayman + go_active::RAYCASTMAN_AIM_RAY_LENGTH, 1.5f);
    mem.set_float(kRayman + go_active::RAYCASTMAN_SPHERE_RADIUS, 0.3f);
    mem.set_float(kRayman + go_active::RAYCASTMAN_TOO_CLOSE, 0.2f);

    // Event delegates (eDt at BYR, BYI, BYT, BBk)
    mem.set_ptr(kEvByr, 0xB00010);
    mem.set_ptr(kEvByr + go_active::EDT_ACTION, kDelegateAct);
    mem.set_ptr(kEvByi, 0xB10010);
    mem.set_ptr(kEvByi + go_active::EDT_ACTION, kDelegateAct);
    mem.set_ptr(kEvByt, 0xB20010);
    mem.set_ptr(kEvByt + go_active::EDT_ACTION, kDelegateAct);
    mem.set_ptr(kEvBbk, 0xB30010);
    mem.set_ptr(kEvBbk + go_active::EDT_ACTION, kDelegateAct);
    mem.set_ptr(kDelegateAct, 0xC00010);
    mem.set_ptr(kDelegateAct + go_active::DELEGATE_TARGET, kAnimator);

    // Animator fields
    mem.set_ptr(kAnimator, 0xD00010);
    mem.set_ptr(kAnimator + go_active::MANAGED_CACHED_PTR, kAnimator + 0x20);
    mem.set_float(kAnimator + go_active::FPMELEE_ANIM_ATTACK_CLIP_LEN, 0.75f);
    mem.set_ptr(kAnimator + go_active::FPMELEE_ANIM_WOOSH_SHAKE, 0xD10000);
    mem.set_ptr(kAnimator + go_active::FPMELEE_ANIM_HIT_SHAKE, 0xD20000);

    // Draw / Motion / TransformOffset fields
    mem.set_ptr(kDrawEv, 0xE00010);
    mem.set_ptr(kDrawEv + go_active::FPDRAW_ACTION, kDrawAct);
    mem.set_ptr(kDrawAct, 0xE10010);
    mem.set_ptr(kDrawAct + go_active::DELEGATE_TARGET, kMotion);
    mem.set_ptr(kMotion, 0xE20010);
    mem.set_ptr(kMotion + go_active::MANAGED_CACHED_PTR, kMotion + 0x20);
    mem.set_ptr(kMotion + go_active::FPMOTION_OBJECT, kWeapon);
    mem.set_ptr(kMotion + go_active::FPMOTION_PLAYER, kEventHandler);
    mem.set_ptr(kMotion + go_active::FPMOTION_ATTACK_OFFSET, kAttackOffset);
    mem.set_float(kAttackOffset + go_active::TRANSFORMOFFSET_LERP_SPEED, 8.0f);

    // PlayerEventHandler & LookDirection fields
    mem.set_ptr(kEventHandler, 0xF00010);
    mem.set_ptr(kEventHandler + go_active::EVENT_HANDLER_LOOK_DIRECTION, kLookWrapper);
    mem.set_float(kLookWrapper + go_active::GUI_VALUE + 0, 0.f);
    mem.set_float(kLookWrapper + go_active::GUI_VALUE + 4, 0.f);
    mem.set_float(kLookWrapper + go_active::GUI_VALUE + 8, 1.f);

    // Target setup: Tree weakpoint at (102.0, 20.0, 100.0) -> eye is at (100.0, 20.0, 100.0) -> dir = (1, 0, 0)
    s_test_target.valid = true;
    s_test_target.id = 0x998877;
    s_test_target.kind = 0;
    s_test_target.has_spot = true;
    s_test_target.aim_x = 102.0f;
    s_test_target.aim_y = 20.0f;
    s_test_target.aim_z = 100.0f;
    s_test_target.aim_dist = 2.0f;
    s_test_target.melee_reach = 20.0f;
    s_have_test_target = true;

    // 1. Tick Controller with farm enabled
    printf("1. Тест применения модификаций в памяти...\n");
    ctrl.tick(1, kPlayer, true);
    assert(ctrl.status == viewmodel::Status::Active);

    // Verify alwaysCrit == 1
    uint8_t crit = 0;
    mem.read(kWeapon + go_active::FPTOOL_ALWAYS_CRIT, crit);
    assert(crit == 1);
    printf("  [+] alwaysCrit = 1 успешно установлен в памяти\n");

    // Verify durabilityUnbreakable == 1 and current durability restored to max
    uint8_t unbrk = 0;
    mem.read(kItemConfig + go_active::ITEMCONFIG_DURABILITY_UNBREAKABLE, unbrk);
    assert(unbrk == 1);
    float cur_dur = 0.f;
    mem.read(kItemState + 0x10, cur_dur);
    assert(cur_dur == 100.0f);
    printf("  [+] durabilityUnbreakable = 1 и прочность восстановлена\n");

    // Verify reach & hit radius
    float reach = 0.f, radius = 0.f, attack_time = 0.f;
    mem.read(kWeapon + go_active::FPMELEE_MAX_REACH, reach);
    mem.read(kWeapon + go_active::FPMELEE_HIT_RADIUS, radius);
    mem.read(kWeapon + go_active::FPMELEE_TIME_BETWEEN_ATTACKS, attack_time);
    assert(reach == 20.0f);
    assert(radius == 15.0f);
    assert(attack_time == 0.08f);
    printf("  [+] MaxReach=20.0, HitRadius=15.0, TimeBetweenAttacks=0.08 применены\n");

    // Verify RaycastManager
    float ray_len = 0.f, aim_ray = 0.f, sphere_rad = 0.f;
    mem.read(kRayman + go_active::RAYCASTMAN_RAY_LENGTH, ray_len);
    mem.read(kRayman + go_active::RAYCASTMAN_AIM_RAY_LENGTH, aim_ray);
    mem.read(kRayman + go_active::RAYCASTMAN_SPHERE_RADIUS, sphere_rad);
    assert(ray_len == 20.0f);
    assert(aim_ray == 20.0f);
    assert(sphere_rad == 15.0f);
    printf("  [+] RaycastManager ray_length=20.0, sphere_radius=15.0 применены\n");

    // Verify event delegates zeroed (animation suppression)
    uint64_t act_byr = 0, act_byi = 0, act_byt = 0, act_bbk = 0;
    mem.read(kEvByr + go_active::EDT_ACTION, act_byr);
    mem.read(kEvByi + go_active::EDT_ACTION, act_byi);
    mem.read(kEvByt + go_active::EDT_ACTION, act_byt);
    mem.read(kEvBbk + go_active::EDT_ACTION, act_bbk);
    assert(act_byr == 0);
    assert(act_byi == 0);
    assert(act_byt == 0);
    assert(act_bbk == 0);
    printf("  [+] Делегаты анимации BYR, BYI, BYT, BBk обнулены (нет анимации удара)\n");

    // Verify Animator clip len & shake zeroed
    float clip_len = 0.f;
    uint64_t woosh_shake = 0, hit_shake = 0;
    mem.read(kAnimator + go_active::FPMELEE_ANIM_ATTACK_CLIP_LEN, clip_len);
    mem.read(kAnimator + go_active::FPMELEE_ANIM_WOOSH_SHAKE, woosh_shake);
    mem.read(kAnimator + go_active::FPMELEE_ANIM_HIT_SHAKE, hit_shake);
    assert(clip_len == 0.0f);
    assert(woosh_shake == 0);
    assert(hit_shake == 0);
    printf("  [+] FPMeleeAnimator attackClipLength=0 и тряска камеры отключены\n");

    // Verify LookDirection targeting towards (1, 0, 0)
    farm_tuning::Position3 ldir{};
    mem.read(kLookWrapper + go_active::GUI_VALUE, ldir);
    assert(std::fabs(ldir.x - 1.0f) < 0.01f);
    assert(std::fabs(ldir.y) < 0.01f);
    assert(std::fabs(ldir.z) < 0.01f);
    printf("  [+] PlayerEventHandler.LookDirection нацелен точно на крестик (1,0,0)\n");

    // 2. Test clean restoration on disable
    printf("2. Тест чистого восстановления оригинальных значений...\n");
    ctrl.tick(1, kPlayer, false);
    assert(ctrl.status == viewmodel::Status::Off);

    mem.read(kWeapon + go_active::FPTOOL_ALWAYS_CRIT, crit);
    assert(crit == 0);
    mem.read(kItemConfig + go_active::ITEMCONFIG_DURABILITY_UNBREAKABLE, unbrk);
    assert(unbrk == 0);
    mem.read(kWeapon + go_active::FPMELEE_MAX_REACH, reach);
    assert(reach == 1.5f);
    mem.read(kWeapon + go_active::FPMELEE_HIT_RADIUS, radius);
    assert(radius == 0.3f);
    mem.read(kEvByr + go_active::EDT_ACTION, act_byr);
    assert(act_byr == kDelegateAct);
    mem.read(kAnimator + go_active::FPMELEE_ANIM_ATTACK_CLIP_LEN, clip_len);
    assert(clip_len == 0.75f);
    mem.read(kAnimator + go_active::FPMELEE_ANIM_WOOSH_SHAKE, woosh_shake);
    assert(woosh_shake == 0xD10000);
    printf("  [+] Все оригинальные значения корректно восстановлены\n");

    // 3. Test beta offset switching
    printf("3. Тест совместимости с переключением на Beta-версию...\n");
    go::SelectBuild(go::Build::Beta); // switch to Beta layout
    ctrl.reset(false);
    ctrl.tick(2, kPlayer, true);
    assert(ctrl.status == viewmodel::Status::Active);
    printf("  [+] Controller успешно работает на Beta-раскладке оффсетов\n");

    ctrl.tick(2, kPlayer, false);
    go::SelectBuild(go::Build::Release); // return to Release

    printf("\n=== ВСЕ ТЕСТЫ farm_memory.h УСПЕШНО ПРОЙДЕНЫ ===\n");
    return 0;
}
