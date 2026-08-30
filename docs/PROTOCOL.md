# Protocol 32 overview

Protocol 32 is the current private-tester wire revision. The implementation in
`src/CoopStory.Protocol` and the matching native bridge code are the source of
truth; this document records the supported design, not a public compatibility
promise. Host and guest must always use the same tester package and protocol
revision.

## Session establishment

1. The launcher derives a private protocol credential from the session password
   and validated host address.
2. Host and guest sidecars complete a versioned authenticated handshake.
3. The peers bind the UDP path to that authenticated session.
4. The local bridge connects to its sidecar and negotiates role, protocol,
   motion mode, and logical session generation.
5. Replication begins only after every role and generation gate passes.

Mismatched protocol or motion modes fail closed. Credentials are never written
to normal logs or exported diagnostics.

## Transport split

Authenticated TCP carries ordering-sensitive data, including:

- handshake, role, lobby, reconnect, goodbye, and session-menu control;
- entity lifecycle, reliable player actions, and interaction transactions;
- mission state, objective, camera, cinematic, progression, and dialogue
  control; and
- authoritative replay batches and ambient-encounter state.

Authenticated UDP carries high-frequency replaceable data, including player
transforms, motion and AnimGraph snapshots, and camera/world snapshots for
which a newer state supersedes an older one. UDP validation rejects
unauthenticated, replayed, malformed, oversized, and wrong-session datagrams.

## Current message families

Protocol 32 contains versioned contracts for:

- session hello/goodbye, identity, and lobby state;
- player transform, motion, animation graph, action, traversal, damage,
  downed, revive, restraint, mount, and interaction state;
- host-owned world entity lifecycle and dependency graph updates;
- mission state, objective, camera, cinematic, MetaPed, AnimScene, and exact
  MissionData progression barriers;
- mission dialogue cue/ready messages scoped to a mission epoch and checkpoint;
- host-authoritative ambient-encounter proposal and state messages; and
- peer resynchronization, reconnect replay, diagnostics, and session controls.

Every decoder validates payload bounds, flags, enum values, lengths, sequence,
and authority before a payload reaches game-facing logic.

## Ordering, reconnect, and authority rules

- Sequence comparison is wrap-safe.
- Logical session generations invalidate queued work from earlier connections.
- The sidecar bounds and coalesces high-frequency delivery instead of allowing
  unbounded queues.
- Reliable host state is replayed in dependency order after reconnect.
- Tombstones prevent a late snapshot from recreating an entity already removed
  by the host.
- Bridge sends and receives remain generation-bound across named-pipe reconnects.
- The host accepts only validated guest intents; it owns world lifecycle,
  encounter phase/outcome, and authoritative interaction mutations.

## Mission and dialogue boundary

The host publishes an exact MissionData ID, mission epoch, phase, safe anchor,
and bounded objective/camera presentation. A guest may use its own matching
vanilla mission prompt only through the short exact-ID barrier. The initial
MissionData/global-start probe is only a candidate check; save authority is
created only after the exact local instance starts and the host releases it. An unavailable, mismatched,
or timed-out guest remains companion-only and receives no save change.

Completion replication is deny-by-default. It carries a host rating only for a
matching released run, and the guest applies only the reviewed idempotent
MissionData/reward records in the catalog. Cash is not inferred from total
wallet movement. Pending completion events are durably journaled for reconnect
replay until acknowledgement. This
does not synchronize Rockstar's mission VM, checkpoints, AI, or save files.

Dialogue cues identify only catalogue-owned roots and an observed line. A
matching guest reports whether its own vanilla root/line is ready; a
companion-only guest can use only a reviewed bridge-owned audio presentation.
Stale, mismatched, unavailable, retry, reconnect, cinematic, and mission-end
cues are rejected or cleared. No message asks the peer to guess or advance a
Rockstar conversation.

## Ambient encounter boundary

Fifty reviewed local free-roam script IDs map to five bridge-owned profiles:
roadside ambush, hostage rescue, wagon defense, animal attack, and camp
clear-out. A host detection or guest proposal becomes one shared instance only
after host validation of session state, safety, distance, anchor, and profile.
The host creates the physical roster and decides success, failure, abandonment,
and cleanup; the guest contributes through validated combat intent and receives
the host world presentation.

These messages never make the underlying Rockstar ambient script shared.
Original script state, law, wagon physics, dialogue, Honor, money, campaign
progress, unique rewards, and inventories remain local. Ordinary local corpse
loot is not represented by the protocol.

## Network defaults

| Transport | Default port | Intended scope |
|---|---:|---|
| TCP | `43120` | Trusted private network only |
| UDP | `43121` | Trusted private network only |

Do not forward these ports to the public internet. There is no public server,
matchmaking service, relay, NAT traversal, or supported WAN security model.

## Compatibility boundary

Historical development and the current tester gate use:

- RDR2 PC file version `1.0.1491.50`;
- Script Hook RDR2 runtime `1.0.1491.17`;
- Script Hook RDR2 SDK `1.0.1207.73`; and
- project protocol `32`.

These values document a pinned private-test context, not a broad compatibility
promise. Do not disable game-version, online-mode, or protocol gates to force a
test.
