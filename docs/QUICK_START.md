# Quick start for private testers and source review

The source tree does not contain Script Hook files, SDK files, game assets,
or saves. Private tester releases contain a prebuilt launcher, sidecar, and
project-owned ASI, so testers do not need to compile or install .NET. Obtain
the separate Script Hook prerequisite from its original author.

## Run the tester ZIP

1. Download the same tester ZIP on both PCs from the GitHub Releases page and
   extract it to a normal folder. For the Protocol 32 source line, the release
   tag and `BUILD_INFO.json` must both report protocol `32`.
2. Run `START_COOP.bat`, choose `RDR2.exe`, and use **GET SCRIPT HOOK** if the
   launcher reports it missing. Obtain and extract Script Hook yourself, then
   select that folder with **BROWSE**.
3. The host chooses **HOST**; the guest chooses **GUEST**. Share only the host's
   private IPv4 address and the chosen session password.
4. Load backed-up local saves in Story Mode. Never run the installed project in
   Red Dead Online.

The tester package is experimental. See [STATUS.md](STATUS.md) and
[TESTING.md](TESTING.md) for supported test scope and stop conditions.

## Requirements for a source build

- Windows 10/11 x64;
- .NET SDK `10.0.203`;
- CMake 3.25+;
- Visual Studio Build Tools 2022 or Visual Studio 2026 with MSVC x64 and a
  Windows SDK;
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
cmake --preset bridge-vs2026
cmake --build --preset bridge-vs2026-release
ctest --preset bridge-vs2026-release
```

Building a game-facing ASI requires a separately obtained SDK and an additional
legal/licensing review. See [BUILDING.md](../BUILDING.md).

## Safety

- Do not put Script Hook or game files in this repository.
- Do not disable the game-version or online-mode gates.
- Do not expose project ports to the public internet.
- Never enter Red Dead Online with project-owned mod files installed.
- Review [LEGAL.md](../LEGAL.md) before redistributing anything.
