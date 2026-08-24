# Testing guide

This guide validates the archived source foundation. It does not certify a
finished co-op campaign or authorize public distribution of binaries.

## Managed build and self-tests

From the repository root on Windows:

```powershell
dotnet build .\CoopStory.slnx -c Release
dotnet run --project .\tests\CoopStory.SelfTest -c Release
dotnet run --project .\tests\CoopStory.Launcher.SelfTest -c Release
```

Expected results for the archived source are:

- solution build: zero errors;
- protocol/sidecar self-test: `46/46`;
- launcher self-test: `28/28`.

Test counts may change if the project is extended; a failure must be understood
before any in-game test.

## Native SDK-free simulator

With CMake 3.25+ and Visual Studio Build Tools 2022/MSVC v143:

```powershell
cmake --preset bridge-vs2022
cmake --build --preset bridge-vs2022-release
ctest --preset bridge-vs2022-release
```

This exercises native bridge logic without Script Hook or the game.

## Launcher UI check

```powershell
dotnet run --project .\src\CoopStory.Launcher -c Release
```

Verify that:

- all visible labels and errors are English;
- paths remain local and are not printed into exported diagnostics;
- the example/browser preview never contains a real local address;
- Script Hook is selected separately and is never bundled;
- install, update, and uninstall actions remain manifest-owned;
- Story Mode warnings and the Red Dead Online prohibition are visible.

Do not perform an install operation against a game directory during a UI-only
check.

## Two-PC research test prerequisites

An in-game test requires an independently obtained Script Hook runtime and SDK,
a legitimate PC game copy on each machine, matching builds, safe local saves,
and a trusted private network. Never commit those prerequisites or game files.

Before starting:

1. back up saves independently on both PCs;
2. verify the pinned RDR2 file version and project build;
3. confirm identical protocol and motion settings;
4. keep TCP `43120` and UDP `43121` private;
5. load Story Mode only;
6. confirm the guest is not inside an active local mission or cutscene;
7. plan how to remove all project-owned files before opening Red Dead Online.

## Suggested progression

Test one feature group at a time:

1. connection, identity, ping, disconnect, and reconnect;
2. idle, walk, run, sprint, crouch, aim, and weapon state;
3. begin/end cleanup for combat and interaction actions;
4. separate mount ownership and dismount cases;
5. world entity spawn, update, dependency, and tombstone behavior;
6. mission objective and camera presentation without assuming shared script
   execution;
7. AnimScene/MetaPed experiments only after the earlier layers are stable.

Place diagnostic markers before and after a visible problem, continue running
long enough to capture the post-event window, then export diagnostics from both
PCs. Do not publish raw logs without reviewing them for private data.

## Stop conditions

Stop immediately if:

- either PC enters or attempts to enter Red Dead Online;
- the game or Script Hook version differs from the expected development input;
- authentication, protocol, or motion-mode negotiation fails;
- unknown ASI/loaders are present in the game directory;
- the launcher cannot prove install ownership;
- a test requires disabling an online, version, or safety gate.
