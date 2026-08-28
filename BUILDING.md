# Building RDR2 Coop Story from source

This guide validates the source foundation. It does not promise a playable,
supported co-op campaign and it does not authorize redistribution of game or
third-party files.

## 1. Requirements

- Windows 10 or Windows 11 x64;
- .NET SDK `10.0.203`;
- CMake `3.25` or newer;
- Visual Studio Build Tools 2022 `17.14` or Visual Studio 2026 with MSVC x64,
  the Windows SDK, and CMake tools;
- for an ASI build only: a separately obtained Script Hook RDR2 SDK
  `1.0.1207.73`.

The historical runtime used for game tests was Script Hook RDR2
`1.0.1491.17`. The runtime archive states that redistribution is not allowed,
so every user must obtain it independently from the original author.

## 2. Managed build and tests

From the repository root:

```powershell
dotnet build .\CoopStory.slnx -c Release
dotnet run --project .\tests\CoopStory.SelfTest -c Release
dotnet run --project .\tests\CoopStory.Launcher.SelfTest -c Release
```

This builds the WinForms launcher, protocol, sidecar, and both managed self-test
programs. No game or Script Hook file is required for this validation.

## 3. SDK-free C++ bridge validation

Use the preset matching your installed Visual Studio version. For example,
Visual Studio 2026:

```powershell
cmake --preset bridge-vs2026
cmake --build --preset bridge-vs2026-release
ctest --preset bridge-vs2026-release
```

The SDK-free preset builds the bridge core, simulator, and self-tests without an
ASI and without third-party headers or libraries.

## 4. Optional ASI compilation

Do this only after reading [LEGAL.md](LEGAL.md), reviewing the Script Hook SDK
terms, and accepting that the private co-op design is outside Rockstar's
single-player mod assurance.

Obtain and extract the official Script Hook RDR2 SDK `1.0.1207.73` yourself,
then set a local environment variable that points to the extracted SDK root:

```powershell
$env:SCRIPT_HOOK_RDR2_SDK_DIR = 'D:\path\to\your\extracted\ScriptHookRDR2_SDK'
cmake --preset bridge-asi-vs2026
cmake --build --preset bridge-asi-vs2026-release
ctest --preset bridge-asi-vs2026-release
```

Never copy the SDK into this repository. Never commit or release its headers,
library, samples, runtime, ASI loader, or trainer.

## 5. Launcher UI

The launcher is included in the managed solution:

```powershell
dotnet run --project .\src\CoopStory.Launcher\CoopStory.Launcher.csproj -c Release
```

The launcher expects a complete locally built package beside it before the main
window can operate. The private tester ZIP is self-contained for testers; .NET
is required only when building the source.

The launcher now uses the Windows-provided Georgia typeface. No external TTF is
loaded, bundled, or redistributed.

## 6. Test boundaries

- Use only private LAN or a private virtual LAN.
- Never forward TCP `43120` or UDP `43121` to the public internet.
- Never enter Red Dead Online with the mod installed.
- Do not disable the RDO guard or target-version gate.
- Use two legitimate game copies and two legitimate accounts for any two-PC
  experiment.
- Do not publish diagnostics until you have manually reviewed them for local
  paths, IP addresses, identifiers, and session data.
