# Manually supplied prerequisites

Do not commit third-party archives or extracted files.

Expected local inputs:

1. Script Hook RDR2 runtime `1.0.1491.17`.
2. Script Hook RDR2 SDK `1.0.1207.73`.
3. Visual Studio Build Tools 2022 17.14 with MSVC v143, Windows SDK and CMake.

Downloaded archives may be placed in `prereqs/incoming/`. Extracted SDK/runtime
directories may remain elsewhere and be passed explicitly to the build and
deployment scripts. The project never installs `NativeTrainer.asi`.

