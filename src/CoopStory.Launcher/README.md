# CoopStory Launcher

Windows Forms launcher for the current private Coop Story test package. The
launcher reads the visible build name from `BUILD_INFO.json`, so the UI is not
tied to one engine revision. It provides:

- a single context-aware START/HOST/JOIN action for solo, HOST, and CLIENT tests;
- a compact colored HOST/GUEST lobby with nicknames, peer/bridge state and
  ICMP ping in milliseconds;
- a HOST password/confirmation prompt and a JOIN prompt for the host IPv4 plus
  the same password; PBKDF2 derives the existing protocol-19 credential without
  persisting the clear-text password;
- Steam and Rockstar platform cards with matching start-button colors;
- safe selection, common-location detection, and verification of `RDR2.exe`;
- separate selection of the user-downloaded Script Hook RDR2 runtime;
- host save selection on the Settings page rather than the session start card;
- install, one-operation package update, and uninstall ownership manifests;
- nickname, local save, LAN/Hamachi IPv4, and password-derived session configuration;
- a separate settings page whose values survive release-folder replacement;
- an experimental AnimGraph/Direct Replica motion engine enabled by default;
  both endpoints must select the same mode, and Task/Navmesh remains available;
- sidecar lifecycle and an activity log;
- pre-warmed, composited page switching and a throttled start-button animation
  to avoid black placeholder rectangles on slower GPUs;
- one-click redacted diagnostic export to a configurable folder. The stable
  `RDR2-Coop-Diagnostics.zip` name safely replaces the previous archive only
  after the new ZIP has been created successfully. The export includes a
  chronological timeline, anomaly analysis and `MARKER_WINDOWS.md/json` with
  10 seconds before and 15 seconds after every correlated F7 marker;
- the Windows-provided Georgia display font, with no third-party font file
  bundled or redistributed by the project.

The distributable package must never contain `ScriptHookRDR2.dll`,
`dinput8.dll`, `NativeTrainer.asi`, SDK headers, or Rockstar assets. The
launcher may copy the two runtime files only from the folder explicitly
selected by the user during their own local installation.

This is a Story Mode-only technical alpha. It does not claim shared campaign
scripts, a shared save, synchronized mission triggers, or support for RDO.
