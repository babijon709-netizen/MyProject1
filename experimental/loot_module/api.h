#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// EXPERIMENTAL, not wired to the external app. No constructor/autoload/injector.
// install/disable must be called on UnityMain by an authorized in-process host.
// Keep this library loaded until process exit, even after disable (delegate
// copies could still point at its callback). Built with ELF NODELETE as well.
// The caller supplies a live LootScreenViewImpl, NOT a remotely opened container.
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
#ifdef __cplusplus
}
#endif
