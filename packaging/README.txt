RDR2 COOP STORY - SOURCE/TEST PACKAGE
====================================

PROJECT STATUS
--------------
This is an experimental player-replication foundation, not a finished co-op
campaign. Development by Lifeely has concluded because the remaining work is
too complex to continue as a supported release.

The project is unofficial, non-commercial, and intended for Story Mode only.
It is not created, supported, authorized, or endorsed by Rockstar Games or
Take-Two Interactive.

IMPORTANT LIMITATIONS
---------------------
- No supported ready-to-install public release is provided.
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
- Protocol: 20

Obtain every third-party prerequisite independently from its original author.
Do not redistribute those files with this project.

STARTING A BUILT TEST PACKAGE
-----------------------------
1. Extract the complete test ZIP to a new folder.
2. Run START_COOP.bat.
3. Select the correct RDR2.exe and separately extracted Script Hook folder.
4. Use identical project builds and motion settings on both private PCs.
5. The host enters a session password and shares only the private IPv4 address
   and password with the guest.
6. Both players select Story Mode only and load safe local saves.
7. Export diagnostics after a problem, then stop the session.
8. Uninstall all project-owned game files before opening Red Dead Online.

Do not expose TCP 43120 or UDP 43121 to the public internet. Use only a trusted
private LAN or a deliberately configured private overlay network.

SOURCE BUILD AND SAFETY
-----------------------
Read README.md, BUILDING.md, LEGAL.md, docs/QUICK_START.md, and
docs/TESTING.md in the source repository before building or testing.

The MIT license covers only original project code that Lifeely can license. It
does not grant rights to any game, trademark, Script Hook component, SDK, or
other third-party intellectual property.
