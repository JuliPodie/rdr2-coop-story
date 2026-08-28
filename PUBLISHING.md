# Publishing on GitHub

This folder is the reviewed source package. Publish its **contents as the root
of a new repository**; do not publish the parent development workspace.

## First publication

1. Create a new public GitHub repository, for example `rdr2-coop-story`.
2. Upload or push everything from this folder, including `.github`, to the
   repository's `main` branch.
3. Open **Settings → Pages** in that repository.
4. Under **Build and deployment**, select **GitHub Actions** as the source.
5. Open the **Actions** tab and wait for
   `Deploy project website to GitHub Pages` to finish.
6. Open the URL shown in the deployment job.

The workflow installs the pinned website toolchain, exports a static site,
creates `RDR2-CoopStory-Source.zip` from the exact committed revision, and
publishes both through GitHub Pages. The download does not contain generated
build folders or uncommitted local files.

To verify the Pages export locally, run `pnpm run build` inside the `website`
directory. `pnpm run dev` starts the local preview.

## Before every public update

Run these checks from the repository root:

```powershell
dotnet build .\CoopStory.slnx -c Release
dotnet run --project .\tests\CoopStory.SelfTest -c Release
dotnet run --project .\tests\CoopStory.Launcher.SelfTest -c Release
```

Then review the complete diff, confirm no personal data or third-party binaries
were added, and push to `main`. Never commit `ScriptHookRDR2.dll`, `dinput8.dll`,
SDK files, game assets, saves, secrets, logs, or local network details.

For private tester couples, build `artifacts\releases\RDR2-CoopStory-Tester-Protocol23-*.zip`
with `scripts\Build-FriendPackage.ps1` and attach that ZIP to a private GitHub
Release. It may include only the project-owned launcher, sidecar, ASI, test
guide, and checksums. Do not attach Script Hook, SDK, game, trainer, save, or
other-mod files. Testers can then download a matching prebuilt package without
compiling the repository.

## Repository settings worth enabling

- Require pull-request review before changes reach `main` if collaborators are
  added later.
- Enable secret scanning and dependency alerts where GitHub offers them.
- Keep Issues disabled if Lifeely does not intend to provide support or continue
  development.
- Do not enable Discussions or publish a support promise unless someone agrees
  to maintain the project.

Read [LEGAL.md](LEGAL.md) before publication. The project disclaimer and MIT
license cannot grant rights to Rockstar Games, Take-Two Interactive, Script Hook
RDR2, or any other third-party property.
