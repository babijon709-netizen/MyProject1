# Fast reload — release 11946 / beta 11943

## Evidence from the supplied builds

Both supplied archives (`dump11946.7z`, `dumpbeta11943.7z`) were checked in `dump.cs`.

### Reload period chain

`FPWeaponBase` (abstract base of all first-person weapons) stores two reload-related fields:

| Field | Offset | Type | Source |
|---|---:|---|---|
| `effectiveReloadPeriod` (`<rVA>k__BackingField`) | 0xE8 | float | Runtime cache; copied from `reloadPeriod` (0x104) during initialization. This is the value the `ReloadLoop` StateMachineBehaviour reads to time reload steps. |
| `reloadPeriod` | 0x104 | float | Serialized source field from the weapon prefab. |

For `FPHitscan` weapons, there is an additional loop-reload period:

| Field | Offset | Type | Source |
|---|---:|---|---|
| `effectiveLoopReloadPeriod` (`<rZl>k__BackingField`) | 0x2A0 | float | Copied from `HitscanWeaponConfig.LoopReloadPeriod` (config +0x4C) during initialization. Used for magazine-by-magazine reload (shotgun shell loading). |

The `HitscanWeaponConfig` class (referenced from `FPHitscan.config` at +0x130) defines:

| Field | Config Offset | Type |
|---|---:|---|
| `HasFullReload` | 0x44 | bool |
| `HasLoopReload` | 0x45 | bool |
| `FullReloadPeriod` | 0x48 | float |
| `LoopReloadPeriod` | 0x4C | float |

### Implementation behavior

`jni/src/fast_reload_memory.h` reduces `effectiveReloadPeriod` (weapon +0xE8) to 30 ms for all weapons. For FPHitscan weapons, it also reduces `effectiveLoopReloadPeriod` (weapon +0x2A0) to 30 ms. The original values are saved and restored on disable or weapon switch.

Unlike fast_fire, no per-type classification is needed: all FPWeaponBase descendants share the reload field at +0xE8. The FPManager chain (player → FPManager → active weapon → backref) validates the weapon object before writing.

Validation: player identity, NetworkIdentity local-player flag, FPManager class check, active weapon slot, weapon identity/backlink, and heap-write gate are all revalidated before each write. Destroyed objects (identity mismatch) are silently released.

### Accepted weapon types

All FPWeaponBase descendants are accepted:
- FPHitscan (all fire modes: semi, burst, full-auto)
- FPBow, FPCrossbow, FPRocketLauncher
- FPSnowball, FPExplosiveCharge
- FPSpear, FPMelee, FPTool, FPTorch, FPChainsaw, FPBuildingHammer

Melee tools typically have `reloadPeriod == 0` in their prefab. The valid_period gate (`>= 0`) accepts zero, and the `now <= target` check (0 ≤ 0.03) skips the write — so melee tools are silently accepted but not modified.

Crossbow and snowball are in `HyperHug.Games.Oxide.By_Namespace.Oxide.Player.First_Person` (not `Oxide`); their FPManager chain still resolves correctly.

### Validation and limits

`bash scripts/host_tests.sh` runs fake-memory tests for both offset selections and all12 whitelisted weapon classes plus the base FPMelee/FPTool variants. Coverage includes reload period reduction, hitscan loop-reload reduction, no repeated writes, zero-period melee skip, local-player/identity/backref guards, null/unreadable weapon slots, invalid floats, write/readback failures, external updates, destroyed objects, session changes, weapon switching and crossbow/snowball namespace handling.

No device/runtime or server acceptance test was available. The 30 ms value is a local cooldown target; a pending reload coroutine may retain the duration already handed to Unity. A game-side reinitialization can briefly restore the cached period until the watchdog applies the cap again.

### Menu

The toggle already existed in `menu.cpp` (`g_state.fast_reload`). It was a UI-only stub; the config serialization bit (bit 7) was already present. The new controller is wired through `esp_viewmodel_tick` alongside fast_fire.