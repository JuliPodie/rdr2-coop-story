# Condensed development history

This English summary replaces the original Polish internal test notes. Version
labels describe research milestones, not stable public releases.

- **V8-V13:** synthetic peer, ghost recording/replay, interpolation, and early
  marker-locked remote movement experiments.
- **V14-V17:** experimental AnimGraph visual driving, traversal hand-off,
  jumping, climbing, falling, and continuous locomotion work.
- **V18-V20:** combat/action transactions, lasso, pause/session controls, mount
  relationships, and host-authoritative action cleanup.
- **V21-V25:** host-owned NPC/world graphs, mission state buffering, objective
  presentation, camera/spectator behavior, and reconnect replay.
- **V26-V28:** cutscene/combat diagnostics, camera synchronization experiments,
  guest mission isolation, and safer presentation fallbacks.
- **V29:** MetaPed appearance, AnimScene discovery/presentation, launcher-owned
  host/join flow, session passwords, lobby identity, and reduced reliance on
  in-game emergency menus.
- **V30:** expanded diagnostics, problem markers, AnimGraph motion fixes,
  water/swimming state, action recovery, and additional two-PC regression work.
- **V31:** experimental Story VM capture and AnimScene definition replay,
  dependency-ordered cast binding, readiness/commit phases, and stricter safe
  fallback behavior.
- **Protocol 30:** exact MissionData progression barrier, deny-by-default
  completion/reward catalog, and mission dialogue cue/ready coordination.
- **Protocol 32:** host-owned ambient-event coordinator, later expanded to 94 reviewed
  free-roam script detections spanning five bounded co-op profiles.

The current private tester line is **Protocol 32**. It remains experimental and
does not claim a supported shared campaign. See [STATUS.md](STATUS.md) for the
current capability boundary and [TESTING.md](TESTING.md) for the tester process.
