# Feasibility report: cooperative Story Mode foundation

## Executive conclusion

A two-player replication prototype for RDR2 Story Mode is technically possible,
and this repository demonstrates several required foundations. A complete,
stable cooperative campaign is a substantially different problem and was not
achieved.

The project reached authenticated private networking, remote player and world
state foundations, action and mount contracts, reconnect handling, a safe
launcher, diagnostics, and experimental mission/cinematic presentation. It did
not synchronize Rockstar's complete Story script runtime, merge saves, or make
all mission systems deterministic across two PCs.

## Why the remaining work is difficult

RDR2's campaign assumes one local player, one authoritative Story script
environment, one save owner, and locally streamed world state. A general co-op
layer must reconcile:

- mission script ownership and checkpoint state;
- AI, physics, entity streaming, and dependency order;
- player, mount, weapon, damage, and interaction authority;
- cutscene cameras, actors, AnimScenes, audio, and skip behavior;
- doors, vehicles, props, interiors, prompts, and mission-specific logic;
- reconnects and partial failure without corrupting progress;
- compatibility with game updates and third-party hook changes.

Replicating visible state is not equivalent to synchronizing the underlying
campaign logic. Mission-by-mission patches would also be expensive, fragile,
and difficult to validate safely.

## What the current tester build proves

- A launcher/bridge/sidecar topology can isolate transport from the game-facing
  native layer.
- Authenticated TCP and UDP can carry versioned, bounded replication messages.
- Host authority, logical session generations, interpolation, reconnect replay,
  and dependency-ordered world state are practical foundations.
- Safe install ownership, prerequisite separation, and diagnostic redaction can
  reduce accidental file and privacy problems.
- Synthetic and dependency-free tests can validate large parts of the system
  without launching the game.
- The Protocol 32 tester build also exercises an exact-MissionData barrier,
  bounded dialogue cue/ready coordination, and 50 host-owned ambient encounter
  detections. Those remain validation features, not proof of a shared campaign.

## What it does not prove

- a playable end-to-end shared campaign;
- current RDR2 compatibility;
- safe or permitted public binary distribution;
- identical mission logic, saves, AI, physics, audio, or cutscenes;
- suitability for Red Dead Online, public servers, or monetization.

## Recommended research path

If someone continues the work, the lowest-risk sequence is:

1. preserve and expand dependency-free protocol and authority tests;
2. validate only free-roam player replication on two isolated PCs;
3. stabilize action, mount, world lifecycle, reconnect, and diagnostics;
4. define a narrow mission-presentation scope without claiming shared script
   execution;
5. test a deliberately small set of campaign moments;
6. obtain independent legal and security review before distributing binaries.

## Legal assessment boundary

The MIT license in this repository applies only to original project code that
Lifeely can license. It does not grant rights to Rockstar Games, Take-Two
Interactive, Script Hook RDR2, game assets, trademarks, SDK files, or other
third-party property.

Rockstar's published single-player mod assurance explicitly excludes
multiplayer and online services. This private co-op concept is therefore outside
that assurance. The project must remain unofficial, non-commercial, Story Mode
only, and independently reviewed before any public binary release.

See the root [LEGAL.md](../LEGAL.md) for the complete public notice.
