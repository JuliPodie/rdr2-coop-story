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

## One-PC live animation mirror

The launcher **SOLO TEST** mode runs one RDR2 Story Mode process plus an
authenticated loopback guest. After loading a safe free-roam save, open `F9`
and select **Live mirror: start / stop**. A replica named `LIVE MIRROR` appears
three metres beside the local player and consumes the same player-state,
equipment, interpolation, animation-presentation, and remote-ped code used by
a real peer.

Walk, run, sprint, stop, turn, crouch, aim, fire, reload, enter/leave cover,
jump, climb, swim, fall, and recover while watching the replica. The mirror
keeps a fixed world-space offset so both paths remain comparable. Mount state
is deliberately omitted until the separate mount entity/relationship lane can
also be mirrored; use a two-PC session for mount animation validation.

For NPC and encounter presentation, keep Live mirror running and select
**World view: host / co-op** in `F9`. In co-op view, the authenticated host
world graph is replayed through the normal guest proxy renderer. Entity IDs,
parent dependencies, positions, and task targets receive the same fixed offset
as `LIVE MIRROR`. The source population remains faintly visible and keeps its
original collision in this one-process mode so host sampling cannot feed back
proxy corrections or make actors fall through terrain. Select the command
again to delete the guest graph and restore normal presentation. This exception
is loopback-only and cannot enable guest-authored world entities in a normal
two-PC session.

Use the toggle to compare NPC count, models, relative placement, weapons,
health/death state, locomotion/combat tasks, mounted parent ordering, and
despawn cleanup. Guest replicas are invulnerable presentation actors and never
fire local damaging bullets. Combat contribution and encounter authority still
require their separate authenticated intent paths.

On a real two-PC guest, deterministic Story and camp actors reported as
script-owned scenarios are matched by model and a tight position tolerance to
the guest's existing game-owned actor. RDR2 remains the animation owner for a
match, preserving its authored scenario graph, current phase, props, and IK.
The log reports a match as `[WORLD_SCENARIO_RECONCILE]` and includes the count
as `exact-local-scenario` in `[WORLD_PROXY_PHYSICS]`. An unmatched actor first
tries the guest's nearest authored scenario point, then uses a stable idle
fallback instead of repeatedly clearing tasks.

This test validates the local sender and production receiver in one process,
but it cannot reproduce two independent game clocks, frame rates, streaming
state, collision worlds, or physics. In particular, the three-metre offset has
no authored scenario point, so it validates population, placement, task
classification, and stability—not the exact camp chore clip. The current SDK
also cannot enumerate an arbitrary ped's active animation dictionary, clip, or
phase; exact generic clip streaming would require a separate version-specific
game-memory integration. Repeat important scenario fixes on two PCs when a
tester is available.

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

The tester build contains 94 reviewed free-roam action-script detections: 65
hostile roadside actions, 15 rescues, six wagon defenses, three animal attacks,
and five camp clear-outs. These are host-owned bridge scenes, not synchronized
runs of Rockstar's ambient scripts. Non-action ambient vignettes continue
through the ordinary world mirror and are not converted into invented fights.

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
