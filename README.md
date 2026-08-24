# RDR2 Coop Story

An archived, non-commercial research prototype that explored a two-player,
host-authoritative replication layer for Red Dead Redemption 2 PC Story Mode.

> **Source foundation only — not a finished co-op campaign.**
>
> I am no longer developing this project because the remaining work became too
> complex for me to continue. I am sharing the foundation so the player
> replication work can be studied, tested safely, or continued by someone who
> understands the technical and legal risks.

## What is included

- a C# Windows Forms launcher with a dark three-mode UI, settings, safe install
  ownership, lobby, password flow, and redacted diagnostics;
- a C++20 Story Mode bridge and Script Hook facade;
- a separate .NET sidecar for authenticated TCP/UDP networking;
- binary protocol 20, interpolation, entity and animation replication contracts;
- reconnect, world mirror, mission presentation, player action, mount, and
  diagnostics experiments;
- C# and C++ self-tests;
- architecture, protocol, test, and development-history documentation.

The launcher UI source is present under `src/CoopStory.Launcher`. Its original
interface is Polish; the English project website contains a translated,
privacy-safe browser reconstruction of the UI.

## What is not included

This repository and its downloadable source archive intentionally exclude:

- `ScriptHookRDR2.dll`, `dinput8.dll`, `NativeTrainer.asi`, and Script Hook SDK
  files;
- Rockstar Games executables, scripts, models, textures, audio, saves, or other
  assets;
- prebuilt ASI binaries or a supported ready-to-install release;
- files from other mods, public server components, matchmaking, or online tools.

Every third-party prerequisite must be obtained independently from its original
author. Never upload those files to this repository or attach them to a release.

## Historical development versions

| Component | Version used |
|---|---|
| Target RDR2 PC file version | `1.0.1491.50` |
| Script Hook RDR2 runtime | `1.0.1491.17` |
| Script Hook RDR2 SDK | `1.0.1207.73` |
| .NET SDK | `10.0.203` |
| Native build | C++20, CMake 3.25+, MSVC x64 |
| Last internal project build | `V31.10 Alpha`, protocol `20` |

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

The project was developed by **Lifeely** with support from **OpenAI Codex**,
which helped with architecture, programming, testing, documentation, and
preparation of the project website.

The project is not created, supported, endorsed, or authorized by Rockstar
Games, Take-Two Interactive, Alexander Blade, or any other third-party tool
author.

Contact: [u/Lifeely_ on Reddit](https://www.reddit.com/user/Lifeely_/)

## License

Original project code is available under the [MIT License](LICENSE). That
license applies only to rights Lifeely can grant in the original code. It does
not license or grant rights to any game, trademark, third-party software, SDK,
or other intellectual property. See [NOTICE.md](NOTICE.md).

Some historical research and test notes remain in Polish because they document
the original development process. The public-facing website, this README, build
guide, legal notice, and release instructions are in English. Follow
[PUBLISHING.md](PUBLISHING.md) to publish the repository and website through
GitHub Pages.
