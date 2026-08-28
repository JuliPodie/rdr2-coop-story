RDR2 COOP STORY - PRIVATE TESTER PACKAGE
=========================================

PROJECT STATUS
--------------
This package contains a prebuilt launcher, sidecar, and Story Mode bridge for
private tester pairs. It is an experimental player-replication build, not a
finished co-op campaign or a shared-save mod.

The project is unofficial, non-commercial, and intended for Story Mode only.
It is not created, supported, authorized, or endorsed by Rockstar Games or
Take-Two Interactive.

IMPORTANT LIMITATIONS
---------------------
- No shared Story script VM, shared save, matchmaking, or public server exists.
- Mission, cutscene, world, combat, mount, and animation replication remain
  experimental and may fail or desynchronize.
- Never use the project in Red Dead Online.

THIRD-PARTY REQUIREMENTS
------------------------
The package must never contain ScriptHookRDR2.dll, dinput8.dll,
NativeTrainer.asi, Script Hook SDK files, Rockstar assets, game executables,
saves, or files from other mods.

Historical development inputs:
- RDR2 PC file version: 1.0.1491.50
- Script Hook RDR2 runtime: 1.0.1491.17
- Script Hook RDR2 SDK: 1.0.1207.73
- .NET SDK: 10.0.203
- Protocol: 32

Obtain every third-party prerequisite independently from its original author.
Do not redistribute those files with this project.

STARTING A BUILT TEST PACKAGE
-----------------------------
1. Extract the complete test ZIP to a new folder. Do not run it from inside
   the ZIP.
2. Run START_COOP.bat.
3. Select the correct RDR2.exe. If Script Hook is missing, use **GET SCRIPT
   HOOK** to open the author's official page, download/extract it yourself,
   then select its extracted folder with **BROWSE**.
4. Use the identical package build on both private PCs. The tester profile
   enables the AnimGraph replica and Story-VM diagnostics by default.
5. The host enters a session password and shares only the private IPv4 address
   and password with the guest.
6. Both players select Story Mode only and load safe local saves.
7. Export diagnostics after a problem, then stop the session.
8. Uninstall all project-owned game files before opening Red Dead Online.

TESTER CONTROLS
---------------
- F8: open the in-game bootstrap menu when it is enabled.
- F9: open the diagnostic command menu.
- F10: hide or restore the top status bar.
- Use the launcher to export diagnostics from both PCs after a defect.

The Story-VM capture switch is intentionally enabled in this private tester
profile so build-specific faults can be found. Turn it off in the launcher if
a test is unstable, record that choice in the report, and retry the same case.

AMBIENT ENCOUNTER COVERAGE
--------------------------
This build detects 50 reviewed free-roam script IDs and replaces them, when
safe, with one host-owned co-op scene: 35 roadside ambushes, five rescues,
three wagon defenses, three animal attacks, and four camp clear-outs. The
guest receives host-owned replicas and can contribute combat; the host decides
success, failure, abandonment, and cleanup.

This does not make Rockstar's original event script, wagon physics, law state,
dialogue, Honor, money, unlocks, or progression shared. Native source actors
are only masked during a replacement scene. Each player may use ordinary local
corpse loot where RDR2 offers it, but that loot is deliberately never copied to
the peer. Report the internal event name shown in diagnostics with any issue.

Do not expose TCP 43120 or UDP 43121 to the public internet. Use only a trusted
private LAN or a deliberately configured private overlay network.

SOURCE BUILD AND SAFETY
-----------------------
Read README.md, BUILDING.md, LEGAL.md, docs/QUICK_START.md, and
docs/TESTING.md in the source repository before building or testing.

The MIT license covers only original project code that Lifeely can license. It
does not grant rights to any game, trademark, Script Hook component, SDK, or
other third-party intellectual property.
