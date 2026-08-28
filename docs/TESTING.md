# Testing guide

This guide validates the current private tester build. It does not certify a
finished co-op campaign or authorize use in Red Dead Online.

## Managed build and self-tests

From the repository root on Windows:

```powershell
dotnet build .\CoopStory.slnx -c Release
dotnet run --project .\tests\CoopStory.SelfTest -c Release
dotnet run --project .\tests\CoopStory.Launcher.SelfTest -c Release
```

Expected results for the current Protocol 32 source are:

- solution build: zero errors;
- protocol/sidecar self-test: `49/49`;
- launcher self-test: `28/28`.

Test counts may change if the project is extended; a failure must be understood
before any in-game test.

## Native entitlement probes

The guarded Repeating Shotgun developer probe validates whether a live Story
Mode `UNLOCK` record can make a shop item available without granting the item
or rewriting a save. Follow [SHOP_UNLOCK_PROBE.md](SHOP_UNLOCK_PROBE.md) on a
throwaway save before any co-op entitlement replication is added.

The Poison Throwing Knife pamphlet uses the same native family. Its guarded
host-side result is documented in [RECIPE_UNLOCK_PROBE.md](RECIPE_UNLOCK_PROBE.md).
It has passed one disposable Story Mode test, but must still be applied and
acknowledged by a real guest before it is considered a co-op entitlement.

## Native SDK-free simulator

With CMake 3.25+ and the installed Visual Studio 2026 toolchain:

```powershell
cmake --preset bridge-asi-vs2026
cmake --build build\bridge-asi-vs2026 --config Release --parallel
ctest --test-dir build\bridge-asi-vs2026 -C Release --output-on-failure
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

## Ambient encounter pass

The tester build contains 50 reviewed free-roam script detections: 35 roadside
ambushes, five rescues, three wagon defenses, three animal attacks, and four
camp clear-outs. These are host-owned bridge scenes, not synchronized runs of
Rockstar's ambient scripts.

For each profile, use disposable free-roam saves and confirm:

1. host detection creates one shared scene rather than a duplicate fight;
2. guest replicas appear and guest damage contributes to the host outcome;
3. host success, failure, distance abandonment, disconnect, and 30-second
   cleanup all remove bridge-created actors cleanly;
4. neither player receives copied money, Honor, weapon, unlock, collectible,
   campaign, map-pickup, or capability state;
5. any ordinary corpse loot remains private to the player whose local game
   offers it.

Wagon-defense profiles do not share a native wagon or carriage simulation, and
the original Rockstar event script, law response, and dialogue remain local.
Animal scenes deliberately decline if the host cannot observe a local animal
source model. Include the event name and both diagnostic exports in every
report.

The tester package defaults to `animgraph_replica` and enables the Story-VM
diagnostic capture. Keep those settings for normal coverage. If either causes
a repeatable failure, disable only that switch, repeat the case, and include
both diagnostic exports with the report.

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
