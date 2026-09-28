# OneSource weapon-speed port: release-only implementation

## Scope

Fast Fire and Fast Reload are independent max-only toggles. The UI has no sliders; enabled means the verified `20×` preset.

All chains use named compile-time constants from `go_active`, which is a view of `game_offsets.h`. There is no build selector, mutable offset table, build-specific fallback, or live offset discovery.

## Why config-only writes were insufficient

`HitscanWeaponConfig` is read while the first-person weapon is created/equipped. The game copies periods into live `FPWeaponBase` / `FPHitscan` caches, so changing only the shared config does not reliably affect an already-created weapon.

`jni/src/weapon_speed_memory.h` therefore owns both layers:

- config fire/full/loop reload periods;
- live fire period;
- live full/loop reload caches;
- burst wait-object cadence when the current weapon uses one.

The authoritative active object comes from `FPManager + FPMANAGER_CURRENT_OBJECT`; typed slots are accepted only after player/object backlinks and class identity checks.

## Lifecycle and restoration

- enable captures original values before the first write;
- selected periods are original / 20;
- game writes are re-captured when they replace our last value;
- weapon switch, unequip, death and respawn restore only addresses still owned by the same validated object;
- process loss forgets stale addresses without writing to a reused address;
- Fire and Reload restore independently.

Live reads validate object ownership and value ranges. They never choose another field offset.

## Verification

```sh
sh tools/weapons/run.sh
sh tools/offsets/run.sh
sh tools/hostcheck/run.sh
```

The host fixtures cover existing weapon, switch, melee, unequip, respawn, independent toggles, foreign game updates, write failures, conditional restoration and the fixed `20×` preset. Device verification remains required because only the running client proves that its live cache update timing matches the audited ARM64 consumers.
