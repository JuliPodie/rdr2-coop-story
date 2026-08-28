# Private tester downloads

This folder contains timestamped prebuilt private-tester ZIPs for protocol 23.
Use the newest timestamped ZIP only; both players must use that same ZIP and
verify its SHA-256 before testing. The current self-contained ZIP is
`RDR2-CoopStory-Tester-Protocol23-20260828T103737Z.zip` with SHA-256
`F0CC7D02912A7F78E5AAD79CC38296E1C98D8EE0DDA9CB5DDC334DCEE2A03E4C`.

The package contains the project-owned launcher, sidecar, bridge ASI, sample
configuration, test guide, and internal checksums. It deliberately does not
contain ScriptHookRDR2, `dinput8.dll`, an SDK, game files, saves, or files from
other mods. Each tester must obtain the Script Hook runtime separately from its
original author and point the launcher at that extracted folder. The launcher
offers **GET SCRIPT HOOK** only to open the official author page when the
runtime is missing; it never downloads, bundles, or extracts Script Hook.

The launcher and sidecar are self-contained `win-x64` binaries: testers do not
need to install .NET. Test only in Story Mode. Remove the project-owned files
with the launcher before opening Red Dead Online.

The tester profile uses the AnimGraph replica, exposes the F8/F9/F10 diagnostic
controls, and enables Story-VM diagnostic capture. If that capture causes a
repeatable fault, disable only that toggle in the launcher, rerun the case, and
include diagnostics from both PCs.
