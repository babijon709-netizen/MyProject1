#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// EXPERIMENTAL, not wired to any loader yet. The library does nothing on its
// own: no threads, scans or writes until arm/probe is called, except when the
// loader sets ARENA_LOOT_AUTOSTART=1 explicitly (LD_PRELOAD-style launch).
// Keep this library loaded until process exit, even after disable (delegate
// copies could still point at its callback). Built with ELF NODELETE as well.
// v1 callers supply a live LootScreenViewImpl and must be on UnityMain.
// v2 discovers the live collect listener by scanning this process, performs the
// single delegate CAS, and is therefore safe to call off the Unity thread: it
// calls no game code. The Unity thread is picked up on the first real click.
enum ArenaLootStatus {
    ARENA_LOOT_OFF=0, ARENA_LOOT_READY=1, ARENA_LOOT_UNSUPPORTED=2,
    ARENA_LOOT_WRONG_THREAD=3, ARENA_LOOT_NOT_READY=4, ARENA_LOOT_BINDING_CHANGED=5,
    ARENA_LOOT_MEMORY_ERROR=6
};
struct ArenaLootReportV1 {
    uint32_t size, status, batch_result, inspected, attempted, returned;
    uint64_t delegate_address;
};
// probe is read-only; it does not prove native callbacks are safe on a device.
int arena_loot_probe_v1(uint64_t il2cpp_base, uint64_t loot_view);
int arena_loot_install_v1(uint64_t il2cpp_base, uint64_t loot_view);
int arena_loot_disable_v1(void);
int arena_loot_report_v1(struct ArenaLootReportV1* out, uint32_t size);
// base_hint may be 0: the il2cpp image is then located through /proc/self/maps.
// Returns READY, NOT_READY (armed, no live loot window yet) or UNSUPPORTED.
int arena_loot_arm_v2(uint64_t il2cpp_base_hint);
// Stops acceleration (also stops the worker) and restores the delegate pointer
// when the original delegate is still reachable and unchanged.
int arena_loot_restore_v2(void);
#ifdef __cplusplus
}
#endif
