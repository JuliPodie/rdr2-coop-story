# Friend test package contract

This document describes the private two-PC test artifact. It is a packaging
contract, not permission to redistribute third-party files.

## Launcher package and password-protected lobby

The final launcher ZIP is identical on both PCs. It contains no role-specific
configuration, `sessionToken`, clear-text password, or `.coopjoin` invitation.
Both testers may copy the same project-owned archive and verify the same
`SHA256SUMS.txt`.

After extraction, the host selects **Host** in the launcher, clicks **HOST**,
and enters the private session password twice. The guest selects **Guest**,
clicks **JOIN**, and enters the host IPv4 address and the same password. Share
the IPv4 and password privately; there is no normal launcher session-code or
invite-file step in V29.4.

The launcher derives the protocol credential locally from that password and
the canonical host IPv4. It never stores the clear-text password in settings,
sidecar configuration, logs, diagnostics, or the common launcher ZIP. The
launcher writes the derived active machine-specific sidecar configuration
under:

```text
%LOCALAPPDATA%\RDR2CoopStory\launcher\
```

The common package remains reusable; only LocalAppData and the values entered
by the testers determine Host or Guest role.

## Headless/development session generation

For development without the launcher, the sidecar can generate a matched pair:

```text
CoopStory.Sidecar.exe create-session --output <new-session-directory> --host-address <host-LAN-IPv4-or-DNS>
```

This command atomically publishes `host.config.json` and `guest.config.json`
with one randomly generated `sessionToken`; the token is not printed to
stdout. These are private headless-test files, not inputs to the common friend
ZIP. Give `guest.config.json` directly to the invited tester only when using
the headless workflow.

`create-session` rejects loopback, wildcard, broadcast and multicast
destinations. Protocol 23 supports IPv4. The normal Host sidecar listens on
all IPv4 LAN/Hamachi interfaces on TCP `43120` and UDP `43121` by default. `local-test`
and `simulate` bind only to `127.0.0.1`.

## Package contents

Recommended layout:

```text
RDR2-CoopStory-Friend-Test/
  CoopStory.Launcher.exe
  CoopStoryBridge.asi
  sidecar/
    CoopStory.Sidecar.exe
    CoopStory.Sidecar.dll
    CoopStory.Sidecar.deps.json
    CoopStory.Sidecar.runtimeconfig.json
    CoopStory.Protocol.dll
  README.txt
  TESTING.md
  START_COOP.bat
  BUILD_INFO.json
  SHA256SUMS.txt
```

`BUILD_INFO.json` is part of the signed allowlist. For the private tester build
it must report `protocol: 32`, `engineVersion: tester-protocol32`,
`animSceneRuntimeCaptureEnabled: true` and
`animSceneNativeCreateEnabled: true`. These fields describe an opt-in capability,
the tester-profile default. The launcher setting `STORY VM CAPTURE` remains
local and can be turned off for an isolation retry. Exact capture must stay
pinned to the supported game/ScriptHook layout,
fail closed on any RVA/prologue mismatch, and never delete a game-owned scene.
Only the Host installs Story VM detours. The Guest performs the same read-only
handler validation and may create a bridge-owned replica, but leaves all
game-owned Story VM handlers untouched.

The archive may contain only project-owned binaries, launcher/install scripts
and documentation. It must not contain:

- `ScriptHookRDR2.dll`, `dinput8.dll`, `NativeTrainer.asi`, or the ScriptHook
  SDK;
- Rockstar executables, saves, scripts, models, textures or other assets;
- LML, RedM, trainers or third-party VPN software.
- `host.config.json`, `guest.config.json`, `.coopjoin`, `sessionToken`, launcher
  settings, diagnostics, logs, or other machine-specific state.

Script Hook RDR2 remains a separate prerequisite obtained by each tester from
its author. The private tester ZIP is self-contained and needs no separately
installed .NET runtime. Both PCs need the supported legal PC game build and
must use Story Mode only.

For a direct LAN or Hamachi IPv4 test, the host may need to allow the host application on
Windows **Private networks**. The package must not silently create firewall
rules. Protocol 23 has no NAT traversal; an Internet test needs networking
arranged by the testers outside this project.

## Diagnostics

Each normal run appends structured JSON Lines:

```text
Launcher: %LOCALAPPDATA%\RDR2CoopStory\launcher\logs\sidecar.jsonl
Headless Host:  %LOCALAPPDATA%\RDR2CoopStory\logs\host-sidecar.jsonl
Headless Guest: %LOCALAPPDATA%\RDR2CoopStory\logs\guest-sidecar.jsonl
```

Every five seconds `diagnostics.streaming` records aggregate PlayerState
counters in both directions, connection state and role. It does not log every
frame. `diagnostics.streaming-final` records totals during shutdown.

Create a shareable support archive on each PC:

```text
CoopStory.Sidecar.exe export-diagnostics --config <that-PC-config.json> --output <new-file.zip>
```

The ZIP contains:

```text
summary.json
config.redacted.json
sidecar.jsonl
```

If no log exists, `sidecar-log-missing.txt` replaces `sidecar.jsonl`. The
exporter removes the exact configured credential and any token-shaped values
from copied text. `sessionToken` is always `[REDACTED]`.

The host and guest should each send their own ZIP after the same test window.
The filenames should identify only the role and UTC test time, not the token.
