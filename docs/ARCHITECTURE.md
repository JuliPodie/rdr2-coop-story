# Architecture

RDR2 Coop Story is an archived research prototype for a private, two-PC,
host-authoritative replication layer in Red Dead Redemption 2 Story Mode. It is
not a finished shared campaign and does not synchronize Rockstar's complete
Story script virtual machine.

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
world graph, mission presentation experiments, entity lifecycle, and accepted
interaction mutations. The guest sends local player input/state and consumes
host-approved snapshots or transactions.

The host remains the only owner of campaign progress and its save. The project
does not merge saves or promise deterministic mission script execution on both
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
- interpolation, reconnect replay, impairment simulation, ghost recording, and
  correlated diagnostics.

These systems are incomplete. They do not establish a supported shared Story
campaign, identical AI, identical local audio, or deterministic script state.

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
