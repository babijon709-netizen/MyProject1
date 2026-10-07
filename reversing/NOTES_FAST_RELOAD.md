# Fast reload — release 11946 / beta 11943

## Evidence from the supplied builds

Field offsets verified by disassembling `libil2cpp.so` from `dump11946.7z`
and cross-checked against `dumpbeta11943.7z` (both archives, plus the loose
older `dump.cs`, live in gitignored `google_drive/`; SHA-256 values are in
`NOTES_FAST_FIRE.md`). The beta `dump.cs` (custom `il2cpp_dump.py`, metadata
v39) carries no byte offsets, so each beta confirmation below comes from the
beta binary itself.

| Field | Offset | Release evidence | Beta evidence |
|---|---:|---|---|
| `FPWeaponBase.reloadPeriod` (source) | `0x104` | `sry()` reads `+0x104` and multiplies by `WeaponBase.wLG(modEffectType=5)`; RVA `0x67C8094` | `FPWeaponBase.XZu` (Slot 30) reads `+0x104`; RVA `0x68539DC` |
| `FPWeaponBase.rVA` (effective full-reload cache) | `0xE8` | draw init `FPWeaponBase.sVS` stores the virtual `sry()` result into `+0xE8` (`str s0, [x19, #0xe8]` at `0x67C857C`); getter `seH` `0x67C8870` returns it | `FPWeaponBase.Xiw` (Slot 16) stores `+0xE8`; getters `XwH`/`XJm` read `+0xE8` |
| `FPHitscan` loop-reload cache | `0x2A0` | `FPHitscan.sVS` (`0x679E46C`): `ldr s0, [config, #0x4c]; str s0, [weapon, #0x2a0]` | `FPHitscan.Xiw` (Slot 16): reads config `+0x4C`, stores `+0x2A0` |
| `HitscanWeaponConfig.FullReloadPeriod` | `0x48` | field dump; `FPHitscan.sry` override `0x679FE78` returns `config+0x48 * wLG(5)` | `FPHitscan.XZu` reads config `+0x48` |
| `HitscanWeaponConfig.LoopReloadPeriod` | `0x4C` | field dump; read by `FPHitscan.sVS` | read by `FPHitscan.Xiw` |

`FPWeaponBase.sVS` flow: `FPObject.sVS()` (FOV caches, unrelated) → virtual
`sry()` → `str` into `rVA (+0xE8)` → `sey()` (maxAmmo × mod factor) → `+0xE4`.
For FPHitscan the `sry` override sources from the shared config; for every
other FP weapon it sources from the per-instance `reloadPeriod (+0x104)`.

## How reload consumes these fields

- `FPHitscanAnimator.{srb,wVe,Lh,Vc}` compute the animator rate parameters:
  `rate_full = clipDuration / weapon.rVA` and
  `rate_loop = (a + b * weapon.rVX + c) / weapon.rZl`, then
  `Animator.SetFloat`. The reload animation therefore runs in wall time
  proportional to `rVA` (full reload) and `rZl` (per-bullet loop reload).
- `FPHitscanAnimator.Reload(int)` (RVA `0x67A6EAC`, fallback path) schedules
  `MonoBehaviour.Invoke(methodName, duration)` with
  `duration = max(rVA - field58 - field50, field58)` — a delayed ammo event
  proportional to `rVA`.
- `ReloadLoop` (`StateMachineBehaviour`, RVA `0x67C8F6C`/`0x67C8FD4`) ticks
  `m_NextReloadTime = Time.time + stateInfo.length` and decrements an
  animator integer (remaining reload steps) via
  `Animator.GetInteger/SetInteger`. `stateInfo.length` scales with the rate
  parameters above, so the whole loop follows `rZl`.
- `FPWeaponBase.seQ` (`rVA - reloadSkip`) is called from
  `PlayerWeapon.MwW/NAB/Oo/NVy` — the client weapon messaging path; it sees
  the capped value transitively.
- The auto-reload gate `FPWeaponBase.NnA(bool)` compares reserve ammo against
  `minReloadAmmo (+0x100)` and clip against `rVX (+0xE4)`; it gates *whether*
  a reload starts, not its duration, and is deliberately untouched.

## Implementation behavior

`jni/src/fast_reload_memory.h` mirrors `fast_fire_memory.h`: one controller
ticked from `esp_viewmodel_tick` on the existing 100 ms weapon watchdog and
session mutex. When the toggle is on it caps, never lengthens, three
per-instance floats at `kPeriod = 0.03f`:

1. `reloadPeriod (+0x104)` — the instance source for non-hitscan `sry()`;
2. `rVA (+0xE8)` — the effective full-reload cache read by the animators,
   the `Invoke` duration and `seQ`;
3. `rZl (+0x2A0)` — the hitscan loop-reload cache (FPHitscan only).

Write order is source first, caches last (the draw init copies sources into
the caches, so a re-init is re-capped by the next watchdog pass). The shared
`HitscanWeaponConfig` asset is never written, so other weapons and any
game-side config reload keep their originals. Identity, local network state,
FPManager active slot, typed classification (12 whitelisted FP classes from
fast fire), player backlink, managed/native identity, lifetime and the
anonymous-heap write gate are revalidated before every write; restores only
fire while the field still holds the controller's own last value.

`g_state.fast_reload` and config bit 7 already existed; the menu toggle now
triggers `esp_viewmodel_tick` immediately (same as fast fire) and
`FPWEAPON_RELOAD_PERIOD = 0x104` was added to all three offset tables.

## Validation and limits

`bash scripts/host_tests.sh` runs `test_fast_reload_memory` for both offset
selections and all 12 classes: cap/restore of all three fields, no-lengthen
semantics, config asset untouched, invalid floats, null/unreadable slots,
identity guards, write/readback/restore failures, external updates, weapon
switching, session changes and stage/status reporting. Host syntax checks
use `scripts/host_stubs.sh` plus `g++ -fsyntax-only` on the edited TUs.

No device/runtime or server acceptance test was available. A reload already
in flight when the toggle flips keeps its remaining `Invoke` delay; the
animator rates and loop steps re-derive from the capped fields on the next
draw/refresh. The animator fallback path floors the delayed event at the
weapon's `FPObject` field `+0x58`, so its floor is that constant, not zero.
Ammo counts, server-side reload validation, animations and input are not
touched: the server still decides when ammo actually moves.
