# RDR2 Coop Story

An experimental, non-commercial private-tester build of a two-player,
host-authoritative replication layer for Red Dead Redemption 2 PC Story Mode.
Big thanks to [u/Lifeely\_ on Reddit](https://www.reddit.com/user/Lifeely_/) for
creating the inital project.

## Install the tester build — no compiling required

This is for two private testers on Windows with legitimate RDR2 PC copies.
Both players must use the exact same current tester ZIP from the
[GitHub Releases page](https://github.com/JuliPodie/rdr2-coop-story/releases).
The package is self-contained, so it does not require a separate .NET
installation.

1. Download the current tester ZIP, extract it to a normal folder, and run
   `START_COOP.bat`. Do not run it inside the ZIP.
2. In the launcher, select your `RDR2.exe` with **BROWSE**.
3. If Script Hook is missing, click **GET SCRIPT HOOK**. It opens the original
   author's page; download and extract it yourself, then select that extracted
   folder with **BROWSE**. The launcher verifies it before installing anything.
4. Choose **HOST** on one PC and **GUEST** on the other. The host enters a
   private session password and shares their private IPv4 address plus that
   password with the guest.
5. Start RDR2 in **Story Mode only**, load safe local saves, and begin the
   session from the launcher. Export diagnostics from both PCs after a problem.
6. Use the launcher to uninstall the project-owned files before ever opening
   Red Dead Online.

The current tester profile enables AnimGraph replication and diagnostic
controls. It is experimental: campaign scripts and saves are not fully shared,
so test one feature group at a time and keep backup saves.

## What is included

- a C# Windows Forms launcher with a dark three-mode UI, settings, safe install
  ownership, lobby, password flow, and redacted diagnostics;
- a C++20 Story Mode bridge and Script Hook facade;
- a separate .NET sidecar for authenticated TCP/UDP networking;
- binary protocol 26, interpolation, entity and animation replication contracts;
- reconnect, world mirror, mission presentation, player action, mount, and
  diagnostics experiments;
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
| Private tester build        | protocol `23`                |

The Script Hook runtime and SDK are not vendored. The original author’s page is
[dev-c.com/rdr2/scripthookrdr2](http://www.dev-c.com/rdr2/scripthookrdr2/).

## Build the safe source validation

Install .NET SDK 10.0.203, CMake 3.25 or newer, and Visual Studio Build Tools
2022 with MSVC v143 and the Windows SDK.

```powershell
dotnet build .\CoopStory.slnx -c Release
dotnet run --project .\tests\CoopStory.SelfTest -c Release
dotnet run --project .\tests\CoopStory.Launcher.SelfTest -c Release

cmake --preset bridge-vs2022
cmake --build --preset bridge-vs2022-release
ctest --preset bridge-vs2022-release
```

These commands build and test the managed projects, SDK-free bridge simulator,
and self-tests. Building an ASI requires a separately obtained Script Hook SDK
and an additional legal/licensing review. See [BUILDING.md](BUILDING.md).

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
