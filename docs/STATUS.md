# Archived project status

## Final state

Development has concluded. Lifeely is sharing the source because it contains a
substantial first foundation for player and world replication, but completing
and supporting a shared Story campaign became too complex.

The last internal label was **V31.10 Alpha** with **protocol 20**. This is not a
stable release or a compatibility guarantee.

## Confirmed in dependency-free validation

- managed protocol, authentication, codec, interpolation, authority, reconnect,
  diagnostics, and simulation self-tests;
- launcher settings, password, invite, install ownership, update, uninstall,
  runtime separation, and redaction self-tests;
- SDK-free native bridge simulator tests when a supported C++ toolchain is
  available;
- successful Windows Forms launcher compilation with a system-provided Georgia
  display font and no redistributed font file.

## Implemented foundations

- authenticated private TCP/UDP sidecar transport;
- host/guest session generations and reconnect gates;
- remote player state interpolation and identity;
- action, traversal, mount, equipment, and world-entity contracts;
- host-authoritative interaction and world graph registries;
- mission state, camera, objective, cinematic, MetaPed, and AnimScene
  experiments;
- launcher UI, safe package validation, manifest-owned install/uninstall, and
  redacted diagnostic export;
- synthetic peer, network impairment, ghost recording, and correlated timeline
  diagnostics.

## Not completed or supported

- a finished playable cooperative campaign;
- synchronized Rockstar Story scripts or merged save progress;
- deterministic AI, physics, audio, cutscenes, doors, vehicles, or mission
  objects across both PCs;
- compatibility with current or future RDR2 builds;
- public servers, matchmaking, NAT traversal, or WAN deployment;
- a ready-to-install public binary release;
- ongoing maintenance, issue support, or security response.

## Known risk areas

- game updates can invalidate native addresses, layouts, and hooks;
- mission and cinematic presentation may desynchronize or fall back;
- action, mount, world, and physics corrections can conflict with local game
  behavior;
- Script Hook and SDK redistribution is outside this project license;
- Rockstar's published single-player mod assurance does not cover this private
  multiplayer design.

## Recommended continuation order

Anyone continuing the work should first:

1. preserve Story Mode and game-version fail-closed behavior;
2. keep third-party binaries and SDK files outside the repository;
3. run every dependency-free self-test before touching game hooks;
4. verify transport and player replication on two isolated test PCs;
5. treat mission/cinematic work as experimental presentation, not shared script
   execution;
6. obtain an independent legal and security review before distributing binaries.

See [ARCHITECTURE.md](ARCHITECTURE.md), [PROTOCOL.md](PROTOCOL.md),
[TESTING.md](TESTING.md), and the root [LEGAL.md](../LEGAL.md).
