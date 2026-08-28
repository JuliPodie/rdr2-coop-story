# RDR2 Coop Story

An experimental, non-commercial private-tester build of a two-player,
host-authoritative replication layer for Red Dead Redemption 2 PC Story Mode.
Big thanks to [u/Lifeely\_ on Reddit](https://www.reddit.com/user/Lifeely_/) for
creating the inital project.

## Install the tester build — no compiling required

1. Put both PCs on the same trusted private network. Prefer a normal LAN; if
   that is not practical, create a private Hamachi network. Never expose the
   project ports to the public internet.
2. Download the tester ZIP from the [GitHub Releases page](https://github.com/JuliPodie/rdr2-coop-story/releases) on both PCs, extract it to a normal folder, and run
   `START_COOP.bat`. Do not run it inside the ZIP.
3. In the launcher, select your `RDR2.exe` with **BROWSE**.
4. If Script Hook is missing, click **GET SCRIPT HOOK**, download and extract it
   yourself from the original author's page, then select that extracted folder
   with **BROWSE**. The launcher verifies it before installing anything.
5. Back up both local saves. For full mission, dialogue, and progression tests,
   keep separate saves at the same campaign progress, with the same upcoming
   mission available and incomplete on both PCs.
6. Load and keep using your own character and your own local save. Save normally
   in each game to retain your progress: the mod never copies, merges,
   substitutes, or saves one player's file for the other. A verified matching
   mission update is written only to the eligible guest's own local game.
7. Choose **HOST** on one PC and **GUEST** on the other. The host enters a
   private session password and shares their LAN or Hamachi IPv4 address plus
   that password with the guest.
8. Start RDR2 in **Story Mode only**, load the prepared saves, and begin the
   session from the launcher. A guest whose save cannot start the host's exact
   mission remains a companion, so matching-mission features stay off.
9. Use the launcher to uninstall the project-owned files before ever opening
    Red Dead Online.

## What is included

- a C# Windows Forms launcher with a dark three-mode UI, settings, safe install
  ownership, lobby, password flow, and redacted diagnostics;
- a C++20 Story Mode bridge and Script Hook facade;
- a separate .NET sidecar for authenticated TCP/UDP networking;
- binary protocol 32, interpolation, entity and animation replication contracts;
- reconnect, world mirror, mission presentation, player action, mount, and
  diagnostics experiments;
- 50 reviewed ambient-event detections mapped to five bridge-owned co-op
  profiles: 35 roadside ambushes, five rescues, three wagon defenses, three
  animal attacks, and four camp clear-outs;
- C# and C++ self-tests;
- architecture, protocol, test, and development-history documentation.

The launcher UI source is present under `src/CoopStory.Launcher`. The launcher,
errors, scripts, and public documentation are English. The project website also
contains a privacy-safe browser reconstruction of the interface.

## What is not included

This repository and its downloadable source archive intentionally exclude:

- `ScriptHookRDR2.dll`, `dinput8.dll`, `NativeTrainer.asi`, and Script Hook SDK
  files;
- Rockstar Games executables, scripts, models, textures, audio, saves, or other
  assets;
- third-party loader/runtime binaries or a turnkey public release;
- files from other mods, public server components, matchmaking, or online tools.

Every third-party prerequisite must be obtained independently from its original
author. Never upload those files to this repository or attach them to a release.

## Historical development versions

| Component                   | Version used                 |
| --------------------------- | ---------------------------- |
| Target RDR2 PC file version | `1.0.1491.50`                |
| Script Hook RDR2 runtime    | `1.0.1491.17`                |
| Script Hook RDR2 SDK        | `1.0.1207.73`                |
| .NET SDK                    | `10.0.203`                   |
| Native build                | C++20, CMake 3.25+, MSVC x64 |
| Private tester build        | protocol `32`                |

The Script Hook runtime and SDK are not vendored. The original author’s page is
[dev-c.com/rdr2/scripthookrdr2](http://www.dev-c.com/rdr2/scripthookrdr2/).

## Build the safe source validation

Install .NET SDK 10.0.203, CMake 3.25 or newer, and either Visual Studio Build
Tools 2022 or Visual Studio 2026 with MSVC x64 and the Windows SDK.

```powershell
dotnet build .\CoopStory.slnx -c Release
dotnet run --project .\tests\CoopStory.SelfTest -c Release
dotnet run --project .\tests\CoopStory.Launcher.SelfTest -c Release

cmake --preset bridge-vs2026
cmake --build --preset bridge-vs2026-release
ctest --preset bridge-vs2026-release
```

These commands build and test the managed projects, SDK-free bridge simulator,
and self-tests. Building an ASI requires a separately obtained Script Hook SDK
and an additional legal/licensing review. See [BUILDING.md](BUILDING.md). For
the exact tester scope, progression boundary, and two-PC checklist, see
[docs/STATUS.md](docs/STATUS.md) and [docs/TESTING.md](docs/TESTING.md).

### Fast local development refresh

For a local developer test installation, close RDR2 and the sidecar, then run:

```powershell
.\scripts\Refresh-DevTest.ps1
```

It builds the managed projects, runs managed self-tests, builds and tests the
private ASI, stages a fresh development package, updates only manifest-owned
project files in the game directory, and verifies/installs the separately
obtained local Script Hook runtime. It never installs Native Trainer.

Available flags:

- `-GamePath 'D:\Games\Red Dead Redemption 2'` selects the RDR2 installation.
- `-SdkPath 'D:\Tools\ScriptHookRDR2_SDK_1.0.1207.73'` selects the separately
  extracted Script Hook SDK.
- `-BridgePreset bridge-asi-vs2026` selects the native build preset (the
  default); `bridge-asi-vs2022` is also supported.
- `-SkipNativeBuild` reuses the already-built ASI at that preset's expected
  path; use it only when that ASI is current.
- `-Launch` starts the local Story Mode test after the refresh completes.

For example:

```powershell
.\scripts\Refresh-DevTest.ps1 `
  -Launch
```

## Safety and legal status

- Story Mode only. Never use the project in Red Dead Online.
- Do not disable the online guard, game-version gate, or safety checks.
- Do not expose the test ports to the public internet.
- Do not monetize access, bundle prerequisites, or claim official support.
- A legitimate PC game copy and separate third-party prerequisites are required.

Rockstar’s published PC mod statement generally concerns non-commercial
single-player projects and explicitly excludes multiplayer or online services.
This private co-op design is therefore outside that assurance. A disclaimer is
not a license or permission. Read [LEGAL.md](LEGAL.md) and obtain qualified legal
advice before public distribution or continued development.

## Author and attribution

The project was initally developed by **Lifeely** with support from **OpenAI Codex**,
which helped with architecture, programming, testing, documentation, and
preparation of the project website.

The project is not created, supported, endorsed, or authorized by Rockstar
Games, Take-Two Interactive, Alexander Blade, or any other third-party tool
author.

## License

Original project code is available under the [MIT License](LICENSE). It does
not license or grant rights to any game, trademark, third-party software, SDK,
or other intellectual property. See [NOTICE.md](NOTICE.md).
