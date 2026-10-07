# Fast fire — release 11946 / beta 11943

## Evidence from the supplied builds

Both supplied archives (`dump11946.7z`, `dumpbeta11943.7z`) were checked in `dump.cs` and ARM64 `libil2cpp.so`. The archives and generated disassembly are excluded from Git under `google_drive/`.

SHA-256 of `libil2cpp.so`:

- Release: `172210fb902eb5e04963916c4c1a1c54d156200e250d9227f224f830f0dce419`
- Beta: `bf1a535fe000c0f602ad02bab299b5cb6362d3a2374e9cd162f64857fd27dc43`

`FPWeaponBase + 0x124` is the live attack-period cache; `+0xE8` is the separate reload period. The builds copy per-weapon source values into `+0x124` during initialization/draw:

| Weapon | Source value(s) | Release cache store | Beta cache store |
|---|---|---:|---:|
| FPHitscan, semi | mode `+0x138 == 0`; fire duration `+0x140` | `0x67A1324` | `0x6815F34` |
| FPHitscan, burst | mode `+0x138 == 1`; duration `+0x148` plus pause `+0x14C` | `0x67A1324` | `0x6815F34` |
| FPHitscan, full auto | mode `+0x138 == 2`; `60 / HitscanWeaponConfig.fireRate`, config `+0x130`, field `+0x34` | `0x67A1324` | `0x6815F34` |
| FPBow | minimum-shot period `+0x138` | `0x67866B0` | `0x67FF620` |
| FPCrossbow | minimum-shot period `+0x140` | `0x75591C8` | `0x75B7D2C` |
| FPRocketLauncher | minimum-shot period `+0x140` | `0x67BE8C0` | `0x6849C24` |
| FPSnowball | minimum-throw period `+0x12C` | `0x756090C` | `0x75BDC8C` |
| FPExplosiveCharge | minimum-throw period `+0x12C` | `0x679550C` | `0x680D8BC` |
| FPMelee and melee descendants | `m_TimeBetweenAttacks +0x130` | `0x67B3E44` | `0x683DC08` |
| FPSpear | inherited melee `+0x130`; throw period `+0x168` copied to cache `+0x1A8` | `0x67C0D74` | `0x684CA80` |

These source-field offsets are identical in both dumps. Base cache getter/setter: release `0x67C8868` / `0x67C887C`; beta `0x6853814` / `0x6853AC0`. The implementation changes instance source fields and caches where available; it deliberately does **not** edit the shared full-auto hitscan config. The active `+0x124` cache is still clamped for that mode and re-applied if the game rebuilds it.

For hitscan burst, `+0x1F0` points to `UnityEngine.WaitForSeconds`, whose seconds field is `+0x10`; burst length is at `+0x144`. The game builds the wait from `burstDuration / burstLength`, then stores `burstDuration + burstPause` as the outer cooldown.

The current `FixedTimer` is a sequential struct containing absolute `EndTime` at offset zero. In both dumps its setter computes `Time.time + interval`. The observed attack paths pass the current interval into that setter when starting an attack. This feature does not touch the timer, so enabling it does not rewrite an already-started cooldown; a pending burst can finish under its old timing.

Crossbow and snowball are in `HyperHug.Games.Oxide.By_Namespace.Oxide.Player.First_Person`, **not** `Oxide`, in both builds. The accepted classes are hitscan, bow, crossbow, rocket launcher, snowball, explosive charge, spear, melee, tool, torch, chainsaw and building hammer.

## External-source cross-check

The public [UltimateSurvival source at a pinned revision](https://github.com/blacksailfornasao/UltimateSurvival/tree/3c8655a9ac4b49dc655f98be0c45a108d30f36ed/Scripts/By%20Namespace/UltimateSurvival/Player/First%20Person) is an older/different implementation, not source for these supplied Oxide builds. Its offsets and gates were not copied. It was useful to identify behavior that the cache-only plan missed:

- [`FPHitscan.cs`](https://github.com/blacksailfornasao/UltimateSurvival/blob/3c8655a9ac4b49dc655f98be0c45a108d30f36ed/Scripts/By%20Namespace/UltimateSurvival/Player/First%20Person/FPHitscan.cs): burst shots yield a shared `WaitForSeconds`; `TryAttackContinuously` rejects semi-auto. The outer cooldown and the per-shot burst wait are separate clocks.
- [`FPMelee.cs`](https://github.com/blacksailfornasao/UltimateSurvival/blob/bf0d97f77bc8292b4a964effc6bbd646b3e1d522/Scripts/By%20Namespace/UltimateSurvival/Player/First%20Person/FPMelee.cs): `m_TimeBetweenAttacks` gates attacks and the continuous method delegates to a single attack attempt. The supplied builds retain the matching source field at `+0x130` and copy it into `FPWeaponBase +0x124`.
- [`FPBow.cs`](https://github.com/blacksailfornasao/UltimateSurvival/blob/fc4260fa100fb8a4fbe5a5bd8087e1da8f902889/Scripts/By%20Namespace/UltimateSurvival/Player/First%20Person/FPBow.cs): firing requires active aim, and a shot stops aim. The period change does not bypass these gates.
- [`FPSpear.cs`](https://github.com/blacksailfornasao/UltimateSurvival/blob/80e13c36da1a03e3d585a664f432bc055c6f8ab6/Scripts/By%20Namespace/UltimateSurvival/Player/First%20Person/FPSpear.cs): throwing requires aim and a throw-ready gate, then uses a spawn-delay coroutine. It is a different class hierarchy from the supplied builds, so only the general gate/delay lesson was used; current offsets come from the supplied builds.
- [Unity `WaitForSeconds.cs`](https://github.com/Unity-Technologies/UnityCsReference/blob/d5a388dbe5253100d5778cccb467fdb42af6a643/Runtime/Export/Scripting/WaitForSeconds.cs): the managed `m_Seconds` field is an internal float. This C# reference source does not specify native coroutine scheduling; a wait already yielded may have had its duration consumed by the engine.

## Implementation behavior

`jni/src/fast_fire_memory.h` caps individual source/cache periods at 30 ms and never lengthens a value already below that. For burst hitscan it also reduces the instance burst duration, pause and `WaitForSeconds` value consistently. It sets the outer burst interval to no less than `burstLength * newWait + newPause`, so the next newly-started burst is not scheduled before the prior burst's wait sequence plus pause. If the current cached interval is already shorter than this required cycle, the controller fails closed instead of creating a faster overlapping burst.

The controller keeps each original and the value it wrote. It restores source fields first and the runtime cache last, and only if the field still contains its own last value. It revalidates exact type/namespace, local network identity, active FPManager object, player backlink, managed/native identity and heap-write gate before each write. It restores on disable/equip changes, retries failed restores before applying to a replacement weapon, and does not reuse the stale typed weapon slot. Shared config, code, timers, ammo, reload, input and animation state are not written.

The existing `g_state.fast_fire` and config bit remain the setting. The menu toggle applies immediately; watchdog checks share the existing 100 ms cadence and session mutex. Semi-auto remains semi-auto: no input automation is added. Aim requirements, bow draw/release behavior, spear spawn delay, reload/ammo, animations, frame scheduling and server validation still limit accepted fire rate.

## Validation and limits

`bash scripts/host_tests.sh` runs fake-memory tests for both offset selections and all 12 whitelisted classes. Coverage includes source/cache changes, burst timing and overlap guards, fire-mode switching, spear throw source/cache, no repeated writes, local-owner/type guards, null/unreadable slots, invalid floats, write/readback/restore failures, external updates, destroyed objects, session changes and weapon switching. The new tests also pass with `-O3 -ffast-math`. Host syntax checks use `scripts/host_stubs.sh`.

No device/runtime or server acceptance test was available. The 30 ms value is a local cooldown target, not a guarantee of 33 accepted shots per second. A pending `WaitForSeconds` may retain the duration already handed to Unity; an already-started timer is not rewritten. Full-auto hitscan's shared config is intentionally untouched, so a game-side reinitialization can briefly restore its cache until the watchdog applies the local cap again.
