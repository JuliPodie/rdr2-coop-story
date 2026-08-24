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
Spectator camera control, equipment resync and game-native checkpoint retry
remain fail-closed. The simulator exercises the bridge-side command and state
paths without launching the game.

The deterministic revive state machine enforces the planned 4 s hold, 2 m
range and 35% restored health, and consumes refreshed `ReviveRequest` frames.
Game-input initiation of the first request and restoration of the actual local
RDR2 ped's health are not yet bound to verified natives, so in-game revive is
not claimed as working in this preparation build.

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
