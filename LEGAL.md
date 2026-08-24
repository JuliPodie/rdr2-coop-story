# Legal and distribution notice

This document is a project safety notice, not legal advice.

## Rockstar and Take-Two policy context

Rockstar's current public statement says Take-Two generally does not take legal
action against certain third-party PC projects that are single-player,
non-commercial, and respectful of third-party rights. The same statement
explicitly excludes multiplayer or online services and tools that may affect
them. It is not a license, approval, or waiver, and it can be changed or
withdrawn.

RDR2 Coop Story was designed as private two-PC co-op. Even though it targets
Story Mode and includes a hard Red Dead Online guard, its multiplayer design is
outside that public assurance. A private LAN reduces exposure; it does not grant
permission or legal protection.

- Rockstar PC Single-Player Mods statement:
  https://support.rockstargames.com/articles/5NVOAYjcTomO8v6SX2k76k/pc-single-player-mods
- Rockstar legal terms: https://www.rockstargames.com/legal

Obtain qualified legal advice before distributing binaries, promoting a public
release, monetizing the project, or continuing multiplayer development.

## Distribution boundary

This public repository is a source archive of original project work. It must not
contain:

- Rockstar executables, assets, saves, decompiled scripts, models, textures,
  maps, audio, or other game content;
- Script Hook RDR2 runtime or SDK files, an ASI loader, Native Trainer, or files
  from another mod;
- tools for Red Dead Online, public matchmaking, public servers, DRM bypass,
  anti-cheat bypass, or ban evasion;
- unreviewed logs, captures, invitations, passwords, tokens, e-mail addresses,
  user paths, machine identifiers, or private IP addresses;
- a font, image, library, or other asset without a redistribution license.

The repository uses only system typography for the launcher and an original
social-preview image for the website. It does not use official game art, logos,
screenshots, or character assets.

## Script Hook RDR2

Historical development used:

- Script Hook RDR2 runtime `1.0.1491.17`;
- Script Hook RDR2 SDK `1.0.1207.73`.

Their archives state that redistribution is not allowed. Users must obtain them
independently from the original author's page:
http://www.dev-c.com/rdr2/scripthookrdr2/

The meaning of the SDK's offline-only language for a private LAN experiment is
not clarified by the author. Do not interpret the existence of source code in
this repository as permission to redistribute the SDK or a built ASI.

## License scope

The repository's MIT license applies only to original project code and
documentation for which Lifeely can grant rights. It does not license:

- Red Dead Redemption, RDR2, Rockstar Games, or Take-Two names, trademarks, or
  copyrighted material;
- Script Hook RDR2, its SDK, loader, trainer, or the rights of its author;
- Windows, .NET, Visual Studio, Steam, Rockstar Games Launcher, Hamachi, or any
  other third-party product;
- any game-native interface or third-party material beyond what applicable law
  independently allows.

Red Dead Redemption, RDR2, Rockstar Games, and Take-Two names and marks belong
to their respective owners and are used only to identify technical context.

## Privacy review

The prepared source archive was scanned for common e-mail, private-key, secret,
absolute user-path, and identity patterns. The Word report's author metadata was
blank at the time of review. Test IP addresses are documentation-only examples.

This review cannot guarantee that every future commit is clean. Before each
publication, inspect the exact release archive again and never rely on a
disclaimer or `.gitignore` as the only control.
