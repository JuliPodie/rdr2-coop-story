# CoopStory C++ bridge

This directory contains the x64 C++20 process bridge. It does not install
anything into Red Dead Redemption 2 and does not vendor ScriptHookRDR2.

## Targets

- `CoopStory.Bridge.Core`: SDK-independent protocol, state machines, security
  gate, telemetry, entity registry and named-pipe client.
- `CoopStory.Bridge.Sim`: deterministic, SDK-free loopback simulation.
- `CoopStory.Bridge.SelfTest`: dependency-free protocol and behavior tests.
- `CoopStoryBridge.asi`: opt-in ScriptHook target, built only when the official
  SDK directory and import library are supplied.

The default configuration builds only the simulator and self-tests. Enabling
`COOPSTORY_BUILD_ASI` without a complete SDK fails at CMake configure time with
an actionable error.

## Safety boundary

- The bridge accepts only `RDR2.exe` file version `1.0.1491.50` with the pinned
  SHA-256 from `VersionGate.hpp`.
- Runtime activation fails closed unless Story Mode is positively identified
  and no online session is active.
- The named-pipe path contains the current user's SID and the client verifies
  that the server process has the same SID.
- Pipe connect attempts use `NMPWAIT_NOWAIT` (1 ms) and are retried once per
  second, so a stopped sidecar cannot freeze the game script thread for seconds.
- Wire messages contain stable `NetEntityId` values (`epoch:u32`,
  `counter:u32`), never RDR2 handles or pointers.
- After each connection the bridge waits for a one-byte pipe `HelloAck`
  (`0=Host`, `1=Guest`); it emits no player telemetry before that role is
  negotiated.
- There is no pattern scanning, memory patching or direct memory write.

## Native integration status

`IScriptHookFacade` keeps all game-specific calls out of the core. The SDK
implementation uses only ScriptHook's public registration, wait and typed
`natives.h` entry points from the locally supplied SDK. Because that SDK
predates the pinned 1491.50 executable, the game behavior remains marked
`UNVERIFIED_NATIVE_BINDING`, compiled out by default and guarded by
`COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS=OFF`.

Consequently, the default `.asi` configuration deliberately refuses runtime
activation. It must not be enabled until those bindings are checked against
the exact installed official SDK and game build. The explicit opt-in uses the
SDK's typed `natives.h` wrappers to sample player/mission/cutscene state and to
clone, move and delete a non-networked remote ped; all replicas are deleted on
reconnect/unload. Ordinary remote updates use bounded physical velocity and
smooth heading correction. Coordinate snapping is reserved for discontinuities
of at least 8 m, and spawn/snap ground probes may correct height only within
3 m so another floor or a river bed cannot be selected accidentally. A native
startup overlay under `F8` offers `HOST` and `JOIN` using a compact invite code
passed only through the current-user IPC pipe and the Windows clipboard. `F10`
hides or restores the top status bar, while `F9` opens the diagnostic command
menu. The invite secret is never written to bridge logs.
The host is the sole Story-mission authority. The guest receives the host's
mission epoch, phase, safe anchor and companion objective marker. Protocol 32
can briefly permit the guest's own vanilla prompt only when the host is already
in the exact catalog MissionData entry and the guest independently proved that
same entry startable: guest activation and host release are both explicit. A
wrong private Story mission (for example, Chapter 3 while the host is in
Chapter 2), a refusal, or a timeout is quarantined and the native HUD tells the
guest to exit it before following the host again. The guest prompt stays
guarded for a 250 ms verified-idle interval after the barrier arrives, so a
private mission that was already entering is rejected rather than adopted.
This experimental barrier does not yet make Rockstar's mission VM, checkpoint
script state, or dialogue deterministic. A downed pair never auto-retries: revive takes
priority, and only the host can explicitly choose **Retry checkpoint** from the
diagnostic menu. The actual RDR2 checkpoint-retry native remains unverified and
therefore fail-closed. The simulator exercises these bridge-side authority and
isolation paths without launching the game.

## Co-op ambient encounters

Protocol 32 also defines a host-authoritative coordinator for bridge-owned
ambient profiles: roadside ambush, hostage rescue, wagon defense, animal attack
and camp clear-out. A host can start a profile; a guest can initiate a profile
proposal or answer an exact-event preflight, but only the host publishes an
accepted instance and outcome.
The host checks session readiness, mission/cinematic safety, a bounded anchor,
participant distance and whether another encounter is active. It then sends a
stable instance ID, deterministic roster seed, phase and outcome. An exact
event may additionally use a short guest preflight before activation. Terminal
states are retained briefly, then all bridge-created presentation is cleaned up;
separation beyond 160 m abandons the activity. Loot, collectibles, random-event
scripts, law state and campaign rewards remain local/private.

With the explicit unverified-native-binding switch enabled, the native facade
detects the 50 reviewed 1491.50 free-roam script IDs in
`BridgeOwnedEncounterCatalog.hpp`: 35 roadside ambushes, five hostage rescues,
three wagon defenses, three animal attacks, and four camp clear-outs. Each one
maps only to one of the five bounded bridge profiles, never to a remote run of
Rockstar's source script. The host creates the sole physical actor roster and
the guest receives it through the host-authoritative world-entity lane; guest
shots use the existing validated damage-intent path. Native source actors are
masked only for the replacement scene and restored at cleanup. The bridge
does not pause, delete, reward, or progress those native scripts. A generic
guest-originated proposal can use the host's fallback Story model when no
matching host script is loaded; an animal scene fails closed until the host can
observe an actual local animal source model.

The experimental exact-ID **Valentine Extortion** adaptation
(`beat_odriscoll_town_encounter`) adds a two-second guest preflight. A matching
guest is recorded as a **participant**; a save where the beat is unavailable
(or a missed reply) is explicitly recorded as a **companion**. It uses the
same host-owned three-ped rescue profile, but still has no special reward or
Honor mapping.

Extortion has no bridge reward mapping or corpse-loot tracking. The bridge
leaves ordinary interaction with the bridge-created/mirrored generic bandit
corpses to each local vanilla game for a 30-second cleanup window. A companion
may therefore receive only whatever generic money, ammunition, or provisions
their own local corpse roll offers; this needs live two-PC validation. The
bridge never grants a weapon variant, document, recipe, collectible, mission
item, cash packet, Honor, or campaign progress. During every bridge-owned
encounter window it also discards the otherwise itemless map-pickup and
capability telemetry, so corpse interaction cannot become a side-channel for
either player. Unknown ambient scripts remain local.

### Extortion two-PC validation

Use two disposable Story saves in calm free roam, keep the pair within 120 m,
and let the host approach the Valentine Extortion beat. The host log must show
`host detected Extortion; awaiting guest preflight`; the guest then logs either
`guest preflight=participant` when that save also exposes the exact beat, or
`guest preflight=companion` otherwise. Both cases receive the same
host-authoritative generic three-ped scene and may shoot its bandits. After the
host reports success, each player checks their own local bandit corpse during
the 30-second retention window. Any offered money, ammunition, or provisions
must stay local; no special revolver, document, recipe, collectible, mission
item, Honor change, cash packet, or progression update is expected. The host
then despawns the scene after the retention window.

Protocol 32 adds a deny-by-default per-mission progression handshake and
matching-instance start barrier. When an
allow-listed Story mission becomes active, the host sends its exact MissionData
hash. The guest accepts only an exact valid, required, incomplete and unrated
Story MissionData record while RDR2 permits mission start; the host then binds
that approval to that mission run
and may send its completion event. The catalog contains the supplied Story
mission script list, including `FUD1` (**The New South**) and `HNT1` (**Exit
Pursued by a Bruised Ego**). The F9 **Arm FUD1** and **Arm HNT1** commands are
optional controlled test paths for those two missions, not required for normal
catalog detection. A matching eligible guest receives only MissionData's
host's actual MissionData rating (normal completion, bronze, silver, or gold),
then `MISSIONDATA_WAS_COMPLETED` is verified. The positive cash-balance delta
over that exact host mission run is also transferred, capped at $100,000 and
applied once only after the guest completion succeeds. Explicit catalogue
records are then applied idempotently: `HNT1` grants the Legendary Animals map
plus its challenge unlock, while `FUD1` grants the permanent Fishing Rod. The
guest catalogue also includes the permanent `SAD3` Carcano and `MAR8`
Binoculars fallback grants plus the `AB21` Sadie telegram. The fishing rod
used by `RABI1` is a temporary mission loan and is therefore not copied as a
permanent weapon reward. The
inventory path uses the public Character-GUID/slot grant sequence and verifies
the resulting item count. Horses and any weapon, unlock, document, or recipe
without an exact tested mission mapping are not copied. The guest retains its
own preflight decision locally: a completion can be applied only when it
matches that offer and positive local decision, and an accepted completion
event is applied at most once.

The deterministic revive state machine enforces a 4 s hold, 2 m range and 35%
restored health. When an alive player is near a downed peer, the native HUD
shows a hold-to-revive prompt using the normal Story context control; releasing
or leaving range cancels it. The host validates the interaction and each game
restores only its own local RDR2 ped. This remains a two-PC live-test gate
under the explicit unverified-native-binding switch.

## Direct build after prerequisites are installed

```powershell
cmake -S src/CoopStory.Bridge -B out/bridge -A x64
cmake --build out/bridge --config Debug
ctest --test-dir out/bridge -C Debug --output-on-failure
```

To configure the ASI target later:

```powershell
cmake -S src/CoopStory.Bridge -B out/bridge-asi -A x64 `
  -DCOOPSTORY_BUILD_ASI=ON `
  -DSCRIPT_HOOK_RDR2_SDK_DIR=D:/path/to/extracted/sdk
```

`COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS=ON` is intentionally not shown in
the normal build command. It is a developer validation switch, not a supported
installation default.

No build target copies output to the game directory.
