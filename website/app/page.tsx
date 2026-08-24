const modes = [
  { number: '01', label: 'Solo test', detail: 'Local bot and development checks', tag: '1 PC' },
  { number: '02', label: 'Host session', detail: 'Own the world and invite a friend', tag: 'HOST' },
  { number: '03', label: 'Join session', detail: 'Connect to the host over private LAN', tag: 'GUEST' },
];

const modules = [
  ['Launcher', 'C# / WinForms', 'A dark three-mode launcher, settings, safe installer ownership, lobby, password flow, and redacted diagnostics.'],
  ['Bridge', 'C++20 / ASI', 'The in-process Story Mode bridge, version gate, entity registry, remote presentation, and Script Hook facade.'],
  ['Sidecar', '.NET / TCP + UDP', 'A separate networking process for authentication, peer state, snapshots, reconnects, and diagnostics.'],
  ['Protocol', 'Binary protocol 20', 'Shared contracts, authenticated frames, sequencing, interpolation, animation payloads, and session messages.'],
  ['Self-tests', 'C# + CTest', 'Dependency-light validation for codecs, networking, launcher safety, bridge logic, and failure gates.'],
  ['Documentation', 'Architecture archive', 'Detailed notes covering the replication model, test history, limitations, and unfinished experiments.'],
];

const toolchain = [
  ['Target game file version', 'RDR2 PC 1.0.1491.50'],
  ['Script Hook runtime used', 'Script Hook RDR2 1.0.1491.17'],
  ['Script Hook SDK used', 'Script Hook RDR2 SDK 1.0.1207.73'],
  ['Managed toolchain', '.NET SDK 10.0.203'],
  ['Native toolchain', 'C++20 · CMake 3.25+ · MSVC x64'],
  ['Private network ports', 'TCP 43120 · UDP 43121'],
];

export default function Home() {
  return (
    <main>
      <header className="site-header">
        <a className="brand" href="#top" aria-label="RDR2 Coop Story home">
          <span className="brand-mark">R2</span>
          <span>Coop Story</span>
        </a>
        <nav aria-label="Primary navigation">
          <a href="#launcher">Launcher UI</a>
          <a href="#source">Inside the source</a>
          <a href="#build">Build</a>
          <a href="#legal">Legal</a>
          <a className="nav-cta" href="https://www.reddit.com/user/Lifeely_/" target="_blank" rel="noreferrer">
            Lifeely on Reddit
          </a>
        </nav>
      </header>

      <section className="hero" id="top">
        <div className="hero-copy">
          <div className="eyebrow"><span /> Archived technical alpha</div>
          <h1>A first foundation for two-player replication in RDR2 Story Mode.</h1>
          <p className="hero-lead">
            RDR2 Coop Story is a non-commercial research prototype that explored a host-authoritative
            player, world, and presentation replication layer for two private PCs.
          </p>
          <div className="hero-actions">
            <a className="button button-primary" href="#launcher">Explore the build</a>
            <a className="button button-secondary" href="#project-status">Read the project note</a>
          </div>
          <p className="hero-credit">
            Created by <strong>Lifeely</strong> with support from <strong>OpenAI Codex</strong>.
          </p>
        </div>

        <aside className="hero-note" id="project-status">
          <p className="note-label">Project status</p>
          <h2>Foundation shared. Development concluded.</h2>
          <p>
            I am no longer developing this project because the remaining work became too complex for
            me to continue. I am publishing the foundation so others can study the player replication
            work and decide where to take it next.
          </p>
          <div className="status-row">
            <span>Last internal build</span>
            <strong>V31.10 Alpha</strong>
          </div>
          <div className="status-row">
            <span>Protocol</span>
            <strong>20</strong>
          </div>
        </aside>
      </section>

      <section className="launcher-section" id="launcher" aria-labelledby="launcher-title">
        <div className="section-heading">
          <p className="eyebrow"><span /> Included in the source</p>
          <h2 id="launcher-title">The launcher UI is part of the public code.</h2>
          <p>
            This safe browser reconstruction mirrors the Windows Forms launcher without exposing any
            local settings, addresses, game files, or third-party binaries.
          </p>
        </div>

        <div className="launcher-shell" role="img" aria-label="Preview of the RDR2 Coop Story launcher interface">
          <div className="launcher-topbar">
            <div>
              <p className="launcher-title">RDR2 COOP STORY</p>
              <p className="launcher-build">V31.10 ALPHA · LAUNCHER 11.4.0</p>
            </div>
            <div className="launcher-nav"><span className="active">START</span><span>SETTINGS</span></div>
          </div>

          <div className="launcher-grid">
            <section className="launcher-panel modes-panel">
              <div className="panel-heading"><b>01</b><div><h3>CHOOSE MODE</h3><p>One launcher for local tests and private LAN sessions.</p></div></div>
              <div className="mode-list">
                {modes.map((mode, index) => (
                  <div className={`mode-card ${index === 1 ? 'selected' : ''}`} key={mode.label}>
                    <div><strong>{mode.label}</strong><p>{mode.detail}</p></div><span>{mode.tag}</span>
                  </div>
                ))}
              </div>
              <p className="safety-line">STORY MODE ONLY<br /><span>Keep the project out of Red Dead Online.</span></p>
            </section>

            <section className="launcher-panel platform-panel">
              <div className="panel-heading"><b>02</b><div><h3>PLATFORM</h3><p>The main action follows the selected launcher.</p></div></div>
              <div className="platform-card selected"><strong>STEAM</strong><span>steam://rungameid/1174180</span></div>
              <div className="platform-card"><strong>ROCKSTAR</strong><span>Rockstar Games Launcher</span></div>
              <button className="launch-orb" type="button" tabIndex={-1}><strong>HOST</strong><span>VIA STEAM</span></button>
              <p className="launch-hint">HOST · SET A SESSION PASSWORD</p>
            </section>

            <section className="launcher-panel session-panel">
              <h3>HOST SESSION</h3>
              <p>Set a session password and wait for the second player.</p>
              <label>IN-GAME NICK<input value="Lifeely" readOnly /></label>
              <div className="lobby-label">SESSION LOBBY</div>
              <div className="lobby-grid">
                <div className="player host"><span>● HOST</span><strong>Lifeely</strong><small>CONFIGURING</small></div>
                <div className="player guest"><span>● GUEST</span><strong>Waiting…</strong><small>OFFLINE</small></div>
              </div>
              <label>PRIVATE LAN HOST ADDRESS<input value="192.168.50.10" readOnly /></label>
              <div className="password-note"><strong>SESSION PASSWORD</strong><span>Entered only when hosting or joining. Never shown here.</span></div>
              <div className="ready-line">✓ mode & platform &nbsp; ✓ nick &nbsp; ✓ safe example data</div>
            </section>
          </div>
          <div className="launcher-footer"><span>□ Ready.</span><span>○ SESSION STOPPED</span></div>
        </div>
      </section>

      <section className="content-section source-section" id="source" aria-labelledby="source-title">
        <div className="content-heading">
          <div>
            <p className="eyebrow"><span /> What is being shared</p>
            <h2 id="source-title">A source foundation, not a finished co-op campaign.</h2>
          </div>
          <p>
            The repository preserves the original technical direction: a host-authoritative session,
            a local game bridge, an external network sidecar, and a UI that keeps third-party
            prerequisites separate. It is released for study and possible continuation.
          </p>
        </div>

        <div className="module-grid">
          {modules.map(([name, tech, description], index) => (
            <article className="module-card" key={name}>
              <span className="module-number">{String(index + 1).padStart(2, '0')}</span>
              <p>{tech}</p>
              <h3>{name}</h3>
              <div>{description}</div>
            </article>
          ))}
        </div>

        <div className="honest-grid">
          <article>
            <p className="small-label good">Foundation present</p>
            <h3>What the code explores</h3>
            <ul>
              <li>Authenticated two-PC LAN/Hamachi session transport.</li>
              <li>Remote player transform, movement, action, mount, and animation replication paths.</li>
              <li>Host-owned world entities, mission presentation experiments, reconnect, and diagnostics.</li>
              <li>A safe launcher, version checks, install ownership, and a hard Story Mode/RDO guard.</li>
            </ul>
          </article>
          <article>
            <p className="small-label warning">Not completed</p>
            <h3>What this release does not claim</h3>
            <ul>
              <li>No finished shared campaign, shared save state, or synchronized Story script VM.</li>
              <li>No stable release support, compatibility promise, public server, or matchmaking.</li>
              <li>No bundled Script Hook, SDK, ASI loader, trainer, Rockstar asset, executable, or save.</li>
              <li>No permission or endorsement from Rockstar Games, Take-Two, or third-party authors.</li>
            </ul>
          </article>
        </div>
      </section>

      <section className="content-section toolchain-section" aria-labelledby="toolchain-title">
        <div className="content-heading compact">
          <div>
            <p className="eyebrow"><span /> Reproducible context</p>
            <h2 id="toolchain-title">The exact versions used during development.</h2>
          </div>
          <p>
            These are historical build inputs, not a compatibility guarantee. Script Hook runtime and
            SDK files must be obtained independently from their original author and are never part of
            this repository or its downloads.
          </p>
        </div>
        <div className="spec-list">
          {toolchain.map(([label, value]) => (
            <div className="spec-row" key={label}><span>{label}</span><strong>{value}</strong></div>
          ))}
        </div>
        <p className="source-link-line">
          Third-party prerequisite: <a href="http://www.dev-c.com/rdr2/scripthookrdr2/" target="_blank" rel="noreferrer">Script Hook RDR2 by Alexander Blade ↗</a>
        </p>
      </section>

      <section className="build-section" id="build" aria-labelledby="build-title">
        <div className="build-wrap">
          <div className="build-intro">
            <p className="eyebrow"><span /> Source release</p>
            <h2 id="build-title">Download the code. Build only what you understand.</h2>
            <p>
              The public download is source-only. It includes the launcher UI source, bridge, sidecar,
              protocol, tests, and documentation. It intentionally excludes ready-to-run mod binaries
              and every third-party or game-owned file.
            </p>
            <a className="button button-primary download-button" href="downloads/RDR2-CoopStory-Source.zip" download>
              Download source ZIP
            </a>
            <small>The ZIP is generated from the repository by the GitHub Pages workflow.</small>
          </div>

          <div className="build-steps">
            <article className="build-step">
              <span>01</span><div><h3>Install the development prerequisites</h3><p>Windows 10/11 x64, .NET SDK 10.0.203, CMake 3.25+, and Visual Studio Build Tools 2022 with MSVC v143 and the Windows SDK.</p></div>
            </article>
            <article className="build-step">
              <span>02</span><div><h3>Build the managed solution</h3><pre><code>dotnet build .\CoopStory.slnx -c Release</code></pre></div>
            </article>
            <article className="build-step">
              <span>03</span><div><h3>Run the dependency-free validation</h3><pre><code>{`dotnet run --project .\\tests\\CoopStory.SelfTest -c Release\ndotnet run --project .\\tests\\CoopStory.Launcher.SelfTest -c Release\ncmake --preset bridge-vs2022\ncmake --build --preset bridge-vs2022-release\nctest --preset bridge-vs2022-release`}</code></pre></div>
            </article>
            <article className="build-step">
              <span>04</span><div><h3>Build the ASI only after a separate SDK review</h3><p>Point <code>SCRIPT_HOOK_RDR2_SDK_DIR</code> at an independently extracted SDK 1.0.1207.73, then use the <code>bridge-asi-vs2022</code> preset. Do not commit or redistribute the SDK.</p></div>
            </article>
          </div>
        </div>
      </section>

      <section className="content-section legal-section" id="legal" aria-labelledby="legal-title">
        <div className="legal-lead">
          <p className="eyebrow"><span /> Important legal notice</p>
          <h2 id="legal-title">A disclaimer reduces confusion. It does not create permission.</h2>
          <p>
            Rockstar&apos;s published PC mod statement generally concerns non-commercial single-player
            projects and explicitly excludes multiplayer or online services. This project&apos;s private
            Story Mode co-op design is therefore outside that assurance. Public use or continued
            distribution should be reviewed independently by a qualified lawyer.
          </p>
          <a href="https://support.rockstargames.com/articles/5NVOAYjcTomO8v6SX2k76k/pc-single-player-mods" target="_blank" rel="noreferrer">
            Read Rockstar&apos;s current PC Single-Player Mods statement ↗
          </a>
        </div>
        <div className="legal-cards">
          <article><h3>Project code</h3><p>The MIT license applies only to original project code authored for this repository. It grants no rights to Rockstar, Take-Two, Script Hook, or other third-party intellectual property.</p></article>
          <article><h3>Required separation</h3><p>Users must supply their own legitimate PC game copy and independently obtain permitted prerequisites. Never upload game files, saves, Script Hook archives, loaders, trainers, or SDK files here.</p></article>
          <article><h3>Strict scope</h3><p>Story Mode only. No Red Dead Online, public servers, DRM or anti-cheat bypass, monetization, donations tied to access, or claims of official compatibility.</p></article>
          <article><h3>Trademarks</h3><p>Red Dead Redemption, RDR2, Rockstar Games, and Take-Two names and marks belong to their respective owners and are used only to identify the project&apos;s technical context.</p></article>
        </div>
      </section>

      <section className="author-section" aria-labelledby="author-title">
        <div className="author-monogram">L</div>
        <div>
          <p className="eyebrow"><span /> From the project</p>
          <h2 id="author-title">Built by Lifeely, with help from OpenAI Codex.</h2>
          <blockquote>
            “I will not continue developing this project because the remaining work became too
            complicated for me. I did, however, build the first foundation for player replication,
            and I am sharing it so the work does not simply disappear.”
          </blockquote>
          <p>
            The project was developed by <strong>Lifeely</strong> with support from <strong>OpenAI Codex</strong>,
            which helped with architecture, programming, testing, documentation, and preparation of
            this website. This project is not created, supported, or endorsed by Rockstar Games or
            Take-Two Interactive.
          </p>
          <a className="reddit-link" href="https://www.reddit.com/user/Lifeely_/" target="_blank" rel="noreferrer">Contact Lifeely on Reddit ↗</a>
        </div>
      </section>

      <footer>
        <span>RDR2 Coop Story · archived source foundation · 2026</span>
        <span>Original project code: MIT · third-party rights reserved by their owners</span>
      </footer>
    </main>
  );
}
