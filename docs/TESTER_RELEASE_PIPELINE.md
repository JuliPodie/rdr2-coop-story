# Tester release pipeline

Every push to `main` (including a merged pull request or squash merge) starts
the `Publish tester build` GitHub Actions workflow. It builds the managed
projects, runs both managed test suites, builds and tests the native bridge,
creates the self-contained tester ZIP, retains it as a workflow artifact, and
publishes it as a GitHub pre-release.

## One-time maintainer setup

The native bridge cannot be safely built on a standard GitHub-hosted runner:
the official ScriptHookRDR2 SDK is intentionally neither committed nor
redistributed by this project. Register the maintainer's Windows machine as a
self-hosted GitHub Actions runner and give it these labels:

- `self-hosted`
- `Windows`
- `X64`
- `rdr2-coop-story-release`

Install Visual Studio 2022 C++ build tools, CMake, and the official SDK on that
machine. In the repository's Actions variables, set
`SCRIPT_HOOK_RDR2_SDK_DIR` to the SDK directory on that runner. The default
native preset is `bridge-asi-vs2022`; set `COOPSTORY_ASI_PRESET` if the runner
uses the VS 2026 preset instead.

The workflow fails closed when the SDK, CMake, native tests, package checks, or
release upload cannot complete. It never bundles ScriptHookRDR2 or a trainer.

Use **Run workflow** from GitHub Actions to create a tester pre-release for the
current `main` revision without another merge.
