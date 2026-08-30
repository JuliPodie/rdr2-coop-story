# Architecture

RDR2 Coop Story is a private, two-PC, host-authoritative replication layer for
Red Dead Redemption 2 Story Mode. The current Protocol 32 tester build is not
a finished shared campaign and does not synchronize Rockstar's complete Story
script virtual machine, save files, AI, or physics.

## System topology

```text
Host RDR2 process                       Guest RDR2 process
  CoopStoryBridge.asi                    CoopStoryBridge.asi
          | named pipe                          | named pipe
  CoopStory.Sidecar  <--- TCP / UDP --->  CoopStory.Sidecar
          ^                                      ^
          | launcher-owned configuration         |
  CoopStory.Launcher                      CoopStory.Launcher
```

Each PC runs three project components:

1. `CoopStory.Launcher` owns configuration, package validation, safe install
   ownership, process startup, lobby state, and diagnostic export.
2. `CoopStoryBridge.asi` runs inside the Story Mode process and translates
   between game-native state and versioned project messages.
3. `CoopStory.Sidecar` owns authentication, TCP/UDP transport, reconnects,
   buffering, diagnostics, and delivery to the local bridge.

`CoopStory.Protocol` contains the shared managed contracts and codecs. Native
bridge equivalents are kept wire-compatible and are exercised by separate
self-tests.

## Authority model

The host is authoritative for the session generation, replicated peer state,
world graph, mission presentation, bridge-owned ambient encounters, entity
lifecycle, and accepted interaction mutations. The guest sends local player
input/state and consumes host-approved snapshots or transactions.

The host remains the owner of its campaign save. A guest's save can change only
through the separate exact-MissionData progression gate: the guest proves the
same incomplete mission startable locally, verifies its own completion write,
and receives only an allow-listed idempotent reward mapping. The project does
not merge saves or promise deterministic mission script execution on both
machines.

## Data paths

- Reliable control and lifecycle messages use authenticated TCP frames.
- High-frequency player/world snapshots use authenticated UDP datagrams.
- The bridge and sidecar communicate through a local named pipe.
- Sequence numbers, logical session generations, bounded queues, and replay
  gates prevent messages from an earlier connection from crossing a reconnect.
- Diagnostics record bounded metadata and redact credentials, private paths,
  and connection data before export.

## Replication experiments

The source contains foundations for:

- remote player transform, locomotion, aim, weapon, and identity state;
- reliable action transactions for combat, grapple, lasso, and recovery;
- mount ownership and rider relationships;
- host-owned world entities and dependency-ordered lifecycle updates;
- mission state, camera, objective, cinematic, MetaPed, and AnimScene
  presentation experiments;
- epoch/checkpoint-scoped mission dialogue cue/ready coordination with a small
  bridge-owned companion-audio fallback;
- five host-owned ambient profiles backed by 94 reviewed local action-script
  detections: roadside ambush, hostage rescue, wagon defense, animal attack,
  and camp clear-out;
- interpolation, reconnect replay, impairment simulation, ghost recording, and
  correlated diagnostics.

These systems are incomplete. They do not establish a supported shared Story
campaign, identical AI/audio, or deterministic script state. Ambient profiles
do not execute Rockstar's original event scripts or transfer their law, reward,
Honor, inventory, or progression state.

## Safety boundaries

- The bridge is Story Mode only and must fail closed when online mode is
  detected.
- The launcher validates the pinned game build before installing or starting.
- Script Hook, its SDK, loaders, trainers, game files, and other mods are never
  part of the repository or public source archive.
- Install and uninstall operations use a machine-bound manifest and remove only
  project-owned files.
- Test ports are intended for a trusted private network, never the public
  internet.

## Important source locations

| Area | Location |
|---|---|
| Launcher UI and safe install ownership | `src/CoopStory.Launcher` |
| Managed wire contracts and codecs | `src/CoopStory.Protocol` |
| External networking process | `src/CoopStory.Sidecar` |
| Native Story Mode bridge and simulator | `src/CoopStory.Bridge` |
| Managed validation | `tests/CoopStory.SelfTest` |
| Launcher validation | `tests/CoopStory.Launcher.SelfTest` |
| Native simulator validation | `tests/Bridge.SelfTest` |

Read [PROTOCOL.md](PROTOCOL.md), [STATUS.md](STATUS.md), and
[TESTING.md](TESTING.md) before changing authority or transport behavior.
