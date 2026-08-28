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
mission epoch, phase, safe anchor and companion objective marker. Protocol 30
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

Protocol 30 adds a deny-by-default per-mission progression handshake and
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
