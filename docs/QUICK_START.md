# Quick start for source review

This repository is source-only. It does not contain a supported installer,
ready-to-run ASI, Script Hook files, SDK files, or game assets.

## Requirements

- Windows 10/11 x64;
- .NET SDK `10.0.203`;
- CMake 3.25+;
- Visual Studio Build Tools 2022 with MSVC v143 and a Windows SDK;
- a legitimate RDR2 PC installation only for later, separately reviewed game
  testing.

Historical game-facing inputs were RDR2 file version `1.0.1491.50`, Script Hook
runtime `1.0.1491.17`, and Script Hook SDK `1.0.1207.73`.

## Validate the source

```powershell
dotnet build .\CoopStory.slnx -c Release
dotnet run --project .\tests\CoopStory.SelfTest -c Release
dotnet run --project .\tests\CoopStory.Launcher.SelfTest -c Release
```

## Open the launcher UI

```powershell
dotnet run --project .\src\CoopStory.Launcher -c Release
```

Use this only to inspect the interface unless you fully understand the install
ownership and third-party requirements. Closing the UI without applying an
install does not modify the game directory.

## Validate the native simulator

```powershell
cmake --preset bridge-vs2022
cmake --build --preset bridge-vs2022-release
ctest --preset bridge-vs2022-release
```

Building a game-facing ASI requires a separately obtained SDK and an additional
legal/licensing review. See [BUILDING.md](../BUILDING.md).

## Safety

- Do not put Script Hook or game files in this repository.
- Do not disable the game-version or online-mode gates.
- Do not expose project ports to the public internet.
- Never enter Red Dead Online with project-owned mod files installed.
- Review [LEGAL.md](../LEGAL.md) before redistributing anything.
