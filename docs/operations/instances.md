# Self-hosted instances

Part of [Operations](README.md).

Anyone can start their own ForkMesh instance, a white-label Git host on their
own server, from `forkmesh.com/start`. The app connects to the server over
SSH, runs `www/public/install-instance.sh`, streams progress to the page, and
finishes on the new `<owner>/forkmesh` repository. The user guide is
[Self-hosting](../self-hosting.md) and
[Start your own instance](../start-your-own-instance.md); how an
instance fits the network is in [Architecture](../architecture.md).

Instances show a "Powered by ForkMesh" badge by default. `poweredBy: false`
makes the installer write `INSTANCE_POWERED_BY=0` and pass `--no-powered-by`
to `app/tools/apply_instance_brand.py` (which marks the brand script
`data-instance-powered-by="off"`), and `/api/instance` answers
`poweredBy: null`.

## Start page (`app/public/start.html`, `start.js`)

Sections: project name, optional logo and colour theme; server (address, SSH
user and port, password or private key, optional host-key fingerprint);
optional domain (else `<ip-with-dashes>.sslip.io`); admin account and
repository owner (organization or user, optional extra organization owners);
optional mail provider; listing choices (`ownWorld`, `listOnHomepage`, both
on by default).

- **Set up from a file.** A JSON file whose keys are exactly the launch body,
  or a zip with that JSON and a logo, read in the browser only. Guide:
  [start config](../start-config.md); examples
  `app/public/start-config.example.json` and `.zip`.
- **Let your coding agent do it.** A card under "Set up from a file" holds a
  read-only one-line prompt with a Copy button (`data-start-agent-prompt`,
  `data-start-agent-copy`) pointing an agent at `app/public/forkmesh-agent.md`
  (served at `/forkmesh-agent.md`), with links to read it, download it as
  `FORKMESH.md`, and [coding agents](../coding-agents.md). The
  agent writes the settings file and the person uploads it, so only they
  type SSH secrets. The card has no section number.
- **Colour theme.** Accent swatches plus a free picker, three surface
  families (Slate, the ForkMesh default, Zinc and Stone), a dark/light
  preview switch and Shuffle, over a live repository-page mock drawn with the
  tokens `instance_theme.py` accepts. "Customize every colour" edits all 26
  tokens for the previewed mode. Untouched, the page sends `theme: ""` (the
  ForkMesh colours); a preset from a settings file is sent by name until
  edited; anything else as the full `{"dark": {...}, "light": {...}}` object
  (about 1.2 KB). Theme cards from `app/public/instance-themes/themes.json`
  ([Named themes](#named-themes)) offer Default and every ready theme
  (planned ones "Coming soon"); picking one hides the colour toolbar, paints
  the preview and sends its id (`"blt"`). If the catalogue cannot be fetched
  only the toolbar shows. The launch API cleans colours with
  `instance_theme.clean`, the installer checks their shape, and
  `instance-bootstrap` writes the `instance_settings` row, so the choice
  lives only in the instance's database.
- **Admin console address.** Each page open fills `adminPath` with `/admin-`
  plus 20 random `[a-z0-9]` characters from `crypto.getRandomValues` (about
  103 bits), with Copy and New buttons and a note to save it. "Download
  example zip", "Example settings only" and "Copy example settings" each put
  a fresh `adminPath` in the file (the zip is rebuilt in the browser). See
  [Private admin console path](#private-admin-console-path).
- Secret fields are cleared after sending and on `pagehide`; nothing goes in
  browser storage. The page polls every second, keeps the launch id in the
  URL, and on failure refills non-secret fields from the launch record.
- The page uses the landing style: checkboxes use `--ink-strong` (white in
  dark, near-black in light), like its start button; green is kept for
  status.

## API (`app/src/instance_launch.py`)

| Endpoint | Purpose |
|---|---|
| `POST /api/start/launch` | validate, resolve, read the host key, start the run; `202 {id, domain, address, hostKey, steps, url, adminPath}` |
| `GET /api/start/launches/<id>` | state, steps, message, log, URL; the 128-bit id is the capability |
| `GET /api/start/instances` | public list of listed instances and World slots |
| `GET /api/start/instances/<id>/logo` | an instance's logo, served with a sandboxing CSP and `nosniff` |
| `POST /api/start/instances/register` | a self-installed instance joins or leaves the directory ([instance federation](../app/instance-federation.md)) |

- No account needed. Per-client rate limits (client stored only as a salted
  hash) and a global concurrency cap. The host must resolve to public
  addresses only.
- `instance_launches` stores no password, key or API key; rows expire after
  seven days. A run silent for 2 minutes is marked lost; setup continues on
  the server and a new launch with the same details follows it.
- The installer prints `FMI {"step":…,"state":…}` lines; success needs exit 0
  and a final `FMI {"done":true,"url":…}` on the instance's own domain.
  Secrets are scrubbed from the log.
- `adminPath` is checked with `admin_path.problem`; a launch without one gets
  `admin_path.generate()` (Python `secrets`). It reaches the installer in the
  settings, is answered once in the `202`, and is never written to
  `instance_launches`: it is a scrubbed secret (a log line containing it is
  stored as `[hidden]`). The page does not redirect on success while it shows
  the path.

## SSH (`app/host/forkmesh_host/ssh_helper.py`)

The sandboxed, socket-activated `fmnext-ssh-helper` (shared with runner SSH
setup, [runners](runners.md)) has an `instance` operation. It pins the
host key, downloads the installer and runs `bash "$t" --config-stdin` (via
`sudo -n` for a non-root user). Settings, including passwords and mail keys,
travel as one base64 JSON line on stdin, never on the command line. Runs may
take up to 50 minutes. App settings: `INSTANCE_INSTALL_URL`,
`INSTANCE_SOURCE_URL`, `INSTANCE_SOURCE_REF`.

## Installer (`www/public/install-instance.sh`)

| Step | What happens |
|---|---|
| `system` | supported Debian/Ubuntu, x86_64 or arm64, at least 1 GB RAM and 6 GB disk, ports 80/443 free |
| `packages` | swap on small machines; git, Python, Node, Caddy, sqlite3, age; service users |
| `source` | shallow clone into `/opt/forkmesh-instance/src`; on `--update`, a shallow fetch of `sourceRef` from `sourceUrl` into that checkout (nothing checked out yet) |
| `build` | venv and `python -m forkmesh_host build` without ForkMesh-only plugins (`FORKMESH_EXCLUDED_PLUGINS`: their pages, app modules and registry rows are left out, [plugins](../marketplace/plugins.md)), then `app/tools/apply_instance_brand.py`; on a running instance this happens in `src.next` (below) |
| `configure` | `/etc/forkmesh-instance/app.env` with fresh `DATA_KEY`, `ADMIN_PASS` and router keys (kept on re-runs), `ADMIN_PATH`, and `instance.json` without secrets |
| `database` | migrations into a single-node SQLite database |
| `services` | `forkmesh-instance.service` (uvicorn on loopback, sandboxed) |
| `https` | Caddyfile for the app and the Git gateway paths, HSTS; waits for DNS |
| `admin` | installs the Git node daemon (below) and runs `instance-bootstrap` |
| `repo` | waits for `<owner>/forkmesh` to answer `info/refs`, then installs the Actions runner (setting `actions`) |
| `done` | prints the final URL |

- Setup runs as a detached systemd service; the SSH session only follows its
  log, so a dropped connection does not stop it. A lock prevents concurrent
  runs; each step skips finished work, so a re-run resumes.
- DNS is ready when public resolvers or `getent` return the form's address,
  an address on a global interface, or the server's outbound address. A
  Cloudflare-proxied domain (the operator's own) is detected and needs
  SSL/TLS mode Full (strict); Caddy trusts `Cf-Connecting-Ip` only from
  Cloudflare's published ranges.
- An instance launched from the start page (settings carry `launchId`)
  removes itself unless the admin signs in within five minutes
  (`arm_first_login_timeout`, `forkmesh-instance-first-login-cleanup.timer`),
  so an abandoned launch does not leave a server running. A manual
  `--config` install has no `launchId` and is never armed, because it often
  finishes unattended.
- `--update` pulls, rebuilds, migrates and restarts, keeping secrets, data
  and admin; local source changes are saved to a patch file first. See
  [Updates build beside the running version](#updates-build-beside-the-running-version).
  `--config FILE` takes the same JSON as the page for a manual install.
- **Actions** (setting `actions`, boolean, default `true`, kept in
  `instance.json`): `packages` adds jq, build-essential, unzip and nftables;
  `app.env` gets `RUNNER_POOL_TARGET_NODES=forkmesh`,
  `ACTIONS_INSTANCE_RUNNER=forkmesh-runner` and a kept `ACTIONS_BOT_SEED`
  (64 hex). After the git node is up, `install-runner.sh --preregistered`
  from the instance's own source installs runner `forkmesh-runner` with the
  core's daemon binary under `/etc/forkmesh-runner/accounts/<owner>` and
  `/var/lib/forkmesh-runner/accounts/<owner>` (unit
  `forkmesh-runner@<owner>.service`, jobs as `forkmesh-job`), and
  `python -m forkmesh_host instance-runner --name forkmesh-runner
  --public-key <key> --owner <owner>` registers it. A failed install or
  registration only warns; `--update` retries and re-runs the runner
  installer (same key, new binary); `actions: false` stops it. An unclaimed
  instance removes the runner it installed (its own account folders, and the
  runner roots, firewall unit and `forkmesh-job` user when it created them).
  See [instance actions](../server/instance-actions.md).
- **HSTS comes from Caddy only.** The app adds
  `Strict-Transport-Security: max-age=31536000; includeSubDomains` to HTML
  (`_BROWSER_SECURITY_HEADERS`). The Caddyfile sets the same value in a
  `header { defer ... }` block, which replaces any upstream value, as the
  core nodes' `(security_headers)` snippet in `deploy/core/render.py` does,
  so every response (Git gateway paths included) carries exactly one value.
  The `https` step rewrites the Caddyfile and reloads Caddy on `--update`.
  Test: `test_caddy_sends_the_only_strict_transport_security_header`.
- **Console update.** Platform → Update ForkMesh runs the same update. The
  `services` step installs `forkmesh-instance-update.path` and `.service`
  and `tools/instance-update.sh`; the app only writes a request file. Every
  build writes `/opt/forkmesh-instance/build-info.json` (version, commit,
  message), and `app.env` carries `BUILD_REV`, `APP_VERSION`,
  `BUILD_INFO_FILE` and the request and status paths. See
  [server update](../app/server-update.md).

## Tor onion service

Setting `onionService` (boolean, default `false`, kept in `instance.json`)
also serves the instance over Tor, so blocking its domain or IP does not
block it ([forum reach](../app/forum-reach.md),
[Censorship resilience](../censorship-resilience.md)). On a running
instance, add `"onionService": true` to `/etc/forkmesh-instance/instance.json`
and run `install-instance.sh --update`; `false` (or removing the key) turns
it off.

| Piece | Where |
| --- | --- |
| Package | `tor` joins the `packages` step |
| Hidden service | `/etc/tor/torrc.d/forkmesh-instance.conf`: `HiddenServiceDir /var/lib/tor/forkmesh-instance/`, `HiddenServicePort 80 127.0.0.1:8091`; `/etc/tor/torrc` gets one `%include /etc/tor/torrc.d/*.conf` line if it has none |
| Onion site | `/etc/caddy/conf.d/forkmesh-onion.caddy`: `http://<addr>.onion:8091`, bound to `127.0.0.1`, the same gateway paths and app as the HTTPS site, `Host` set to the onion name |
| Advertisement | `/etc/caddy/site.d/forkmesh-onion-location.caddy`: `Onion-Location: http://<addr>.onion{uri}` on the HTTPS site, which Tor Browser offers to switch to |
| App | `FORKMESH_PUBLIC_HOSTS=<domain>,<addr>.onion` (the host allowlist in `request_guard.py` answers the onion name) and `FORKMESH_VAR_INSTANCE_ONION=<addr>.onion` in the unit; `/api/instance` answers `onion: "<addr>.onion"` (`null` without one) and `instance-brand.js` adds a "Tor .onion" link to the footer badge |

- The step runs between `configure` and `services`, inside the `configure`
  progress step, so the start page's step list is unchanged. The address is
  read from `/var/lib/tor/forkmesh-instance/hostname` (v3, 56 base32
  characters); setup stops if Tor writes none within two minutes.
- **Plain HTTP on purpose.** Tor encrypts and authenticates the connection
  end to end, and no public CA issues `.onion` certificates cheaply. Caddy
  listens only on loopback, so the site is reachable only through Tor. It
  drops the app's `upgrade-insecure-requests` CSP directive (it would send
  Tor Browser to `https://` for every asset) and the HSTS header.
- **Idempotent.** Each run rewrites the torrc snippet and both Caddy files;
  Tor restarts only when its config changed or it is not running, Caddy
  reloads only when its files changed. `--update` keeps the address.
- **The key is the address.** `/var/lib/tor/forkmesh-instance/` holds
  `hs_ed25519_secret_key`; whoever has it can serve the address. Back the
  folder up (root only, mode 0700, owner `debian-tor`) to move the address.
  Turning the setting off removes the site files but keeps the key.
- **Egress.** The app makes no outbound calls for this:
  `FORKMESH_EGRESS_PRODUCTION_ALLOW` is unchanged and only the host allowlist
  gains the onion name. `webFirewall` is unaffected: Tor dials out and port
  8091 is loopback only.
- **Visitors share one address.** Every onion visitor reaches the app from
  `127.0.0.1` (`Cf-Connecting-Ip`), so per-IP rate limits are shared; probe
  bans never apply to non-global addresses.
- Tests: `app/tests/test_install_instance_onion.py`.

## Updates build beside the running version

`--update` never builds in the tree the app is serving from, so a build that
fails half way cannot leave the app on half-built assets or unbuilt code.

1. **Fetch.** The `source` step fetches the target commit into the running
   checkout without touching the work tree. `origin` is set to `sourceUrl`
   first (or `FORKMESH_INSTANCE_SOURCE_DIR` for that run), so changing
   `sourceUrl` in `instance.json` switches sources; the step says when it
   does. A `sourceUrl` on the instance's own domain is fetched with
   `http.curloptResolve=<domain>:443:127.0.0.1` through the local Caddy, so
   the update needs no public DNS or proxy hairpin. The forge's git mirror
   can answer 503 briefly after an import, so the fetch (and the first clone)
   retries after 5, 10, 20, 40 and 60 seconds (`source_retry`, waits from
   `FORKMESH_SOURCE_RETRY_WAITS`) before stopping with "Could not fetch". A
   failed clone leaves no `src.partial` (`git clone` removes its folder).
2. **Stage.** Builds happen in `/opt/forkmesh-instance/src.next`: the
   previous release (`src.prev`) recycled, or a copy of the running tree the
   first time. It fetches the target from the running checkout
   (`refs/forkmesh/build`) and checks it out. When `requirements.txt`
   changed (the venv records its digest in `.forkmesh-requirements`),
   packages go into `venv.next` (recycled `venv.prev`, or a copy) with
   `python -m pip`, because a moved venv's script shebangs point at the old
   path. A failed build stops here with the live tree, venv,
   `build-info.json` and `build.stamp` untouched; the failed tree stays in
   `src.next` for the next attempt.
3. **Switch.** `activate_source` renames `src` to `src.prev` and `src.next`
   to `src` (likewise the venv when staged), then writes `build-info.json`
   and the stamp. Nothing is deleted: the two trees take turns, so the disk
   holds at most two copies (about 0.9 GB for ForkMesh).
4. **Roll back.** A failure after the switch and before the app answers
   again (settings, migrations, restart) makes the `EXIT` trap move the new
   tree back to `src.next` and `src.prev` back to `src`, restore the venv,
   `build-info.json` and stamp, and restart the app if it had been
   restarted. Migrations are not reversed: they are additive and old code
   works on the migrated database. Once the app answers
   (`SOURCE_COMMITTED`), later steps (HTTPS, git node, runner) no longer roll
   back. Only the working copy of the installer defines `rollback_source`;
   the copy that starts the detached setup calls it only when it exists.

Between switch and restart (a few seconds) the running process sees the new
files on disk.

### The git node program

The server has no Go toolchain, and an instance running its own fork
supplies the git node program it built
(`FORKMESH_INSTANCE_DAEMON=/path/to/forkmesh-mirror-node`). An update must
never silently replace a fork's program with upstream's, so:

- With `FORKMESH_INSTANCE_DAEMON`, the binary is installed atomically and
  `/opt/forkmesh-instance/daemon-build.json` records `from: supplied`, the
  commit and the tree id of `server/` at that commit.
- An instance whose `sourceUrl` is not the ForkMesh release source keeps the
  installed program on update.
- Only an instance following `https://app.forkmesh.com/forkmesh/forkmesh`
  fetches the signed release on update (`from: release`).
- `build-info.json` gets `daemon: {state, builtTree, sourceTree,
  builtCommit}`. `state` is `current` when the recorded `server/` tree equals
  the running source's, `stale` when `server/` changed since, `release` or
  `unknown`. The installer warns on `stale`; the console's Update page says
  "The git node program was built from older server code".

Building on the server was rejected: a 1 GB instance would need a ~250 MB Go
toolchain and a memory-hungry build in an unattended update. A known-good
binary keeps serving until the operator rebuilds it on a build machine from
the running commit
(`cd server && go build -trimpath -o forkmesh-mirror-node ./cmd/forkmesh-mirror-node`),
copies it over and runs
`FORKMESH_INSTANCE_DAEMON=<binary> bash install-instance.sh --update`.

## Building the instance from its own repository

An instance can update from a repository it hosts, so a merged pull request
deploys. All opt-in:

1. **Publish the platform code as an organization repository** linked as a
   fork of the platform mirror (`/<owner>/forkmesh`), default branch
   protected and Actions off (its workflows are the platform's, not jobs for
   this server):

   ```sh
   git clone --bare --single-branch -b <integration-branch> \
     /root/platform-src /root/acme-platform.git
   git -C /root/acme-platform.git branch -m <integration-branch> main
   cd /opt/forkmesh-instance/src/app/host
   /opt/forkmesh-instance/venv/bin/python -m forkmesh_host instance-repo \
     --owner alice --organization acme --name acme-platform \
     --from /root/acme-platform.git --fork-of acme/forkmesh \
     --protect-default-branch --actions off --no-hooks \
     --env-file /etc/forkmesh-instance/app.env \
     --db sqlite:/var/lib/forkmesh-instance/app.db \
     --data-dir /var/lib/forkmesh-instance/data
   systemctl restart forkmesh-mirror-node
   ```

   It must be public: the updater fetches without credentials.
2. **Point the instance at it.** In `/etc/forkmesh-instance/instance.json`
   set `"sourceUrl": "https://code.example.org/acme/acme-platform.git"` and
   `"sourceRef": "main"`, then run one update (console button, or
   `bash /opt/forkmesh-instance/src/www/public/install-instance.sh --update`).
   The platform mirror keeps its own upstream: `platformUrl` when set, else
   the ForkMesh release source whenever `sourceUrl` is on the instance's
   domain.
3. **Turn on "Update when a pull request is merged"** on the console's Update
   page ([server update](../app/server-update.md#update-when-a-pull-request-is-merged)).

Then a contributor opens a pull request, an owner or admin who is not the
author approves it, an owner merges it, and the instance updates;
`build-info.json` and the Update page show the merged commit. The protected
branch ([branch protection](../app/branch-protection.md)) keeps
unreviewed code from deploying: the console refuses automatic updates, and a
merge files none, while the branch is unprotected. The operator's
`instance-repo` stays a root-only bypass and never triggers an update.

## Named themes

An instance can wear a whole named theme: colours for both modes plus a
stylesheet that changes shape and weight (radius, headings, labels, active
navigation row). Themes ship with the code; nobody can upload CSS.

- **Catalogue:** `app/public/instance-themes/themes.json`, one entry per
  theme: `id`, `name`, `description`, `status` (`ready` or `planned`),
  `palette`, and for a ready theme `colors` (`dark` and `light`, any
  `instance_theme.THEME_TOKENS` name to `#rrggbb`, adding `cta`,
  `cta-foreground`, `success` and `attention` to the custom tokens) and
  `stylesheet`, a file beside it (`blt.css`). `palette` is read in order
  primary, background, surface, second accent, text, then extras, eight at
  most. Cards draw only the palette, as one block with primary three times
  as wide. Planned themes are "Coming soon" and refused by the API. Ready:
  `default` (ForkMesh colours), `blt`, `cloudflare`, `google`, `github`. An
  entry may name `colorMode` (`dark`, `light`, `system`; see
  [Colour mode](#colour-mode)); default `dark`.
- **Storage:** choosing a theme stores `{"preset": "<id>"}` in the
  `instance_settings` theme row (optionally with `dark`/`light` overrides),
  not its colours, so catalogue fixes reach every instance on its next
  update. `"default"` stores `{}`. `instance_theme.clean` accepts the id
  wherever a preset name was accepted (settings file, launch API, installer,
  bootstrap). `preset` plus overrides is accepted by the launch API and
  `PUT /api/instance/theme`, not by the installer's settings file (a theme
  name or a `{dark, light}` object).
- **Apps:** `GET /api/instance` returns `themeColors`
  (`instance_theme.colours`): the theme's colours per mode with overrides on
  top, `{}` for default. The desktop and phone apps use it for "Match the
  instance's theme", so they need neither catalogue nor stylesheet.
- **Rendering:** `/instance-theme.css` writes the colour tokens, then the
  stylesheet, then overrides. `/instance-theme.css?preset=<id>` renders a
  ready theme without saving it (settings preview); an unknown or planned id
  is 404. The catalogue loads once when `instance_theme` is imported; a
  missing or broken file leaves only the colour presets. A stylesheet over
  64 KB or containing `</` is ignored.
- **Who can change it:** the start page at launch (a settings file naming a
  theme waits for the catalogue read). Then an admin in Settings → Instance
  theme (shown only to an admin on a branded instance), with cards from
  `GET /api/instance` (`themes`, `theme.preset`). Clicking a ready card
  previews it on that page by pointing `link[data-instance-theme]` at
  `/instance-theme.css?preset=<id>`; leaving or "Cancel preview" restores
  the sheet. "Apply to instance" sends `PUT /api/instance/theme` with
  `{theme, password}` and updates the cached instance document. The password
  field is cleared on success, failure, cancel, leaving and `pagehide`, and a
  hidden `autocomplete="username"` field stops browsers saving a password
  without an account name. The route checks the session is an admin, then
  the password with its own attempt budget (`instance-theme-v1` blind index,
  login lockout rules), and records `instance.theme` in the sensitive-action
  audit log either way. The admin console offers the same under Platform →
  **Site theme** (`?view=site-theme`: a radio per ready theme, "Keep the
  custom colours" when the stored theme has its own, and the colour mode),
  behind its own sign-in and CSRF check, audited as
  `admin.console_save_theme`; picking a named theme there drops custom
  overrides. Repository share cards take their accent from the stored
  colours, else the named theme's.
- **The ready themes.** Exact colours are in `themes.json` and each
  stylesheet; the intent: **BLT** after the OWASP BLT API explorer
  (charcoal surfaces, signal red `#dc2626` for primary, links, focus and the
  create button, 800-weight headings, 8/12/16 px radii). **Cloudflare**
  after the Cloudflare dashboard (true-black flat surfaces, orange
  `#f38020` for selection, blue for links, focus and the create button as on
  "Add record", 6/8/10 px radii). **Google** after Drive, Gmail and Gemini
  (Material 3 tonal surfaces, blue `#8ab4f8` / `#0b57d0` filled buttons
  (the pastel `#a8c7fa` read as dull), Google's green, yellow and red as
  tonal status labels, pill buttons and active row, Drive-style borderless
  menus with 40 px rows, `#1a73e8` letter avatars, Google Sans only if
  installed). **GitHub** (`colorMode: system`) GitHub's default light and
  dark themes, system font stack, 6 px radii, so a GitHub forum mirror reads
  like the forum its visitors know. No theme downloads a font.
- **Adding a theme:** add its entry and stylesheet, set `status: ready` when
  finished, check `/instance-theme.css?preset=<id>`.
  `app/tests/test_instance_theme.py` requires every ready theme to define
  every token in both modes.

### When an update restarts the git node

`--update` restarts `forkmesh-mirror-node` only when `daemon_digest` changes.
It hashes the binary, the systemd unit, the gateway tools in
`/usr/local/share/forkmesh/tools`, the node config without its `version`
field, and only the gateway config fields the installer writes
(`routerPublicKey`, `publicOrigin`, `dashboard.canonicalOrigin`, the
`forkmesh` repository entries). The node config's `version` is the source
commit and the daemon rewrites the gateway config on every ref change, so
hashing either raw restarted the node on every deploy (pull request pages
showed "unavailable until a reachable mirror host serves …" for about 40 s on
a single server). The cost: the catalog's `version` label stays at the commit
of the last real restart when an update changed nothing the node runs.

## Colour mode

The dashboard opens dark unless the visitor picked light. An instance can
instead open light, or follow each visitor's `prefers-color-scheme`, without
a flash of the wrong theme.

- **Setting:** the theme document's `mode` (`dark`, `light`, `system`), else
  the preset's `colorMode`, else `dark` (`instance_theme.color_mode`).
  `clean` keeps `mode` only when it differs from the preset's, so
  `{"preset": "github"}` already follows the system. The installer's
  settings file takes `"theme": "github"`; an object with `mode` goes through
  the console page, the launch API or `PUT /api/instance/theme`.
- **Visitor's choice wins:** Settings, Appearance offers System, Dark and
  Light. Dark or Light is stored as before (`forkmesh.dashboard.theme`,
  `forkmesh.theme`, the `forkmesh_theme` cookie); System stores `system`.
  With nothing stored, the instance's mode decides. The public site reads
  the same keys: `site-header.js` and `home-theme.js` follow the system for
  `system` (the homepage's light default applies only when nothing is
  stored), and `static-page.js` resolves it without writing a fixed theme
  back.
- **First paint:** the shell's head script resolves a stored choice
  (`system` via `matchMedia`) and sets `data-dashboard-theme` and
  `data-dashboard-theme-resolved`. `/instance-theme.css` (render-blocking,
  linked by `tools/apply_instance_brand.py` at the end of `<head>`) writes
  `--instance-color-mode` when the mode is not dark, and the brand tool puts
  a small inline script (`data-instance-color-mode`, `MODE_TAG`) right after
  the link. A script after a stylesheet runs only once it has loaded, so it
  can read the variable and switch before paint, and it follows later system
  changes. `02-helpers.js` `readDashboardTheme` reads the same variable.
- **Without JavaScript** (Tor Browser's safest level, script blockers):
  `data-dashboard-theme-resolved` is never set, so the stylesheet's
  `@media (prefers-color-scheme: light){html:not([data-dashboard-theme-resolved]){...}}`
  block applies the light tokens: `theme.json`'s light values, the shell's
  light link colours (`BASE_LIGHT_HEX`), then the preset's and instance's.
  The server-rendered first views (`repo_ssr*.py`) draw only with those
  tokens; rules keyed on `[data-dashboard-theme="light"]` wait for the
  script. The forum home and lite pages (`read_surfaces.py`) carry their own
  GitHub light and dark palette behind a media query and ignore the stored
  choice, because they are cached by URL.

## Private admin console path

The admin console lives at `/<ADMIN_PATH>` (`app_runtime._fetch_inner`,
`_admin`). On an instance (`INSTANCE_NAME` set) with a valid `ADMIN_PATH`,
the add-on pages move under it too:

| Page | Address |
|---|---|
| Console and every `?view=` page (Update, Repository addresses, Legal pages, Instances, 404s, Support, Ecosystem, IDS, Marketing email, Database, …) | `<ADMIN_PATH>` |
| Theme switch, Acquisition, Hackathon | `<ADMIN_PATH>/theme`, `<ADMIN_PATH>/acquisition`, `<ADMIN_PATH>/hackathon` |
| `/admin`, `/admin/` and anything under `/admin/` | the instance's normal 404 page (no redirect, no hint) |

- Rules: `app/src/admin_path.py`. Valid: starts with `/`, one segment of 8
  to 64 `A-Z a-z 0-9 - _` (never a `.`, never a file name), not a built-in
  address (`admin`, `api`, `dashboard`, every
  `static_routes.RESERVED_ROUTE_PREFIXES` name, `product-analysis`,
  `transparency`, …). Example: `/manage-x7k2q9`.
- Every console link, form action and redirect comes from one helper:
  `admin_shell.render_page(..., subpages=shell.subpage_base(env))` draws the
  sidebar and theme form, Acquisition and Hackathon use `subpage_base(env)`,
  and admin notifications (`error_group`, signup failures, website feedback,
  IDS) link to `admin_path.notification_href(env)`.
- Console responses get `cache-control: no-store`,
  `x-robots-tag: noindex, nofollow` and `referrer-policy: no-referrer` (an
  outbound link never carries the path in `Referer`), plus
  `<meta name="robots" content="noindex, nofollow">`.
- Console views are not recorded by acquisition attribution or the
  site-referral counter. The path can still appear in the admin-only error
  log if a console page fails.
- The path is defence in depth, not authentication: sign-in, admin session
  and CSRF tokens are checked as before. A signed-out visitor who finds it
  is sent to `/login`.
- Not leaked: `/api/instance` has no admin field; `robots.txt` and sitemaps
  omit it; only an admin's own account answer (`_account_public_payload`, at
  login and session refresh, the `account` block of
  `GET /api/accounts/sessions`, and `POST /api/accounts/admin-session`)
  carries `adminUrl`, shown as **Admin console** in the account menu and as
  the shield button. Error pages and refusals never name it.
- The admin JSON APIs stay at `/api/admin/...` and `/api/accounts/admin-*`:
  they are session-gated, clients call them by fixed names, and moving them
  hides nothing from someone without an admin session.
- Short repository addresses (`/<repo>`) do not capture the console:
  `_fetch_inner` matches the console path before the router. A repository
  with the same name is reachable only at `/<owner>/<repo>`; the name is not
  refused, since a refusal would confirm the path to a guesser.
- Unset or invalid `ADMIN_PATH`, and every core (no `INSTANCE_NAME`): the
  console is at `/<ADMIN_PATH>` when set, add-on pages stay under `/admin/`,
  and an instance without `ADMIN_PATH` has no console route.

### Setting it

- New installs: the installer writes `ADMIN_PATH` to `app.env` from
  `adminPath`. Without one, a fresh install generates `/admin-` + 20 random
  `[a-z0-9]` characters (Python `secrets`) and prints it in the final summary
  with the sign-in line. In a detached run that summary is the setup log, so
  it is printed only when `/var/log/forkmesh-instance-setup.log` is owned by
  root with no group or other access; otherwise the summary says it is
  `ADMIN_PATH` in `app.env`. A path from the settings is never echoed.
- `write_settings` rebuilds `app.env` on every run and reads the existing
  value first (`choose_admin_path`), so `--update` and the console's Update
  button never drop a hand-set value. Re-runs keep an existing `ADMIN_PATH`
  even when the settings name another (the summary says so), and never add
  one to an instance without it. `instance.json` never holds it.
- Existing instance: add one line to `/etc/forkmesh-instance/app.env` and
  run `systemctl restart forkmesh-instance`:

  ```
  ADMIN_PATH=/manage-x7k2q9
  ```

  Use your own random value, for example
  `printf '/admin-%s\n' "$(head -c 64 /dev/urandom | base64 | tr -dc a-z0-9 | head -c 20)"`.
  The Admin console link picks it up at the next sign-in or session refresh.

## Bootstrap (`instance_bootstrap.py`)

`python -m forkmesh_host instance-bootstrap` reads JSON on stdin and creates
or updates the admin (verified, admin, two-factor required at first login),
other owners, the organization, the `forkmesh` node account and the
`<owner>/forkmesh` alias. `INSTANCE_FLAGSHIP_NODE=forkmesh` lets the single
node's own signed refs digest count as the flagship proof;
`HOSTED_GIT_CORE_NODES=forkmesh` makes it the core for new repositories. The
app does not redirect Git clients to its own origin.

- Issues, Pull requests and Discussions start off on the `<owner>/forkmesh`
  mirror; the owner can turn them on in Settings
  ([repository features](../app/repo-settings.md#features-issues-pull-requests-discussions)).
- With `ACTIONS_BOT_SEED` in its environment (or the app env) it creates the
  Actions bot `<owner>-actions` (a node account with the Ed25519 key derived
  from the seed, organization role `maintainer`);
  `python -m forkmesh_host instance-runner --name N --public-key K --owner O`
  registers the instance's trusted pool runner
  ([instance actions](../server/instance-actions.md)). A public
  repository's Actions tab lists runs to everyone (workflow, event, branch,
  commit, actor, status, times); logs, live tails and Run, Cancel and Re-run
  stay with owners and organization writers, and a private repository's runs
  stay hidden ("Public run summary" there).
- Admins can serve one owner's repositories at `/<repo>/<folder>` from
  Platform → Repository addresses ([server update](../app/server-update.md)).

## The platform source mirror (`<owner>/forkmesh`)

`<owner>/forkmesh` (catalog owner `forkmesh`, the instance's git node) is a
read-only mirror of the upstream, https://forkmesh.com/forkmesh/forkmesh by
default (the installer's `platformUrl`, else `sourceUrl` unless that is on
the instance itself, with its `app.` host prefix and `.git` dropped). Nothing
typed on the instance can change it: changes go upstream or into a fork. See
[Mirroring](../mirroring.md) for how mirrors work in general.

How it stays current (`server/upstreamwatch.go`,
[server](../server/README.md)):

- The git node fetches `https://app.forkmesh.com/forkmesh/forkmesh` (the
  `upstreams` entry of `/etc/forkmesh/mirror-node.json`).
- That upstream is in `eventDrivenUpstreams` and served by another relay, so
  the node opens a public watch socket on the upstream relay:
  `wss://app.forkmesh.com/api/nodes/events?watch=forkmesh/forkmesh`. It is
  unsigned and read-only: accepted for public repositories only, no writable
  frames, carrying the same payload-free `commits` doorbell (refs digest and
  generation, no repository bytes) the relay sends its own mirrors. The node
  fetches at once on every doorbell and reconnect.
- `syncInterval` (15 s) stays as a poll for a lost doorbell or down socket.
  (The node's own relay never hears of a forkmesh.com push, so relying on it
  alone left the mirror up to 30 minutes behind.)

An upstream push usually reaches the mirror in 2 to 5 seconds (source
publish, about 25 ms doorbell fan-out, one fetch); without the socket, about
15 s plus the fetch. The upstream side is `app/src/entry.py`
`_node_events_watch_object`, `ForkMeshNodes`, `notify_repo_watchers`; when
the upstream refuses the watch upgrade the node logs `upstream watch ...`,
backs off up to five minutes and the 15 s poll keeps the mirror current.
`curl -s 127.0.0.1:8791/v1/status` reports `upstreamWatches`; the journal
logs each connect and disconnect.

Labelling and read-only:

- The node config carries `mirrorOf: {"forkmesh/forkmesh":
  "https://forkmesh.com/forkmesh/forkmesh"}`. The node publishes `mirrorOf`,
  `upstreamCommit` (served HEAD) and `lastSyncedAt` (ms of the last good
  fetch) in its signed catalog record, and drops the write operations
  (`merge-pull`, `update-pull-branch`, the agent-branch merges,
  `git-receive-pack`) from every gateway entry of that Git directory,
  including the `<owner>/forkmesh` alias.
- `app.env` carries `INSTANCE_UPSTREAM`; the website treats the flagship
  node's `forkmesh` repository as a read-only mirror of it even when the node
  release does not publish `mirrorOf`.
- `GET /api/repo/<owner>/forkmesh/about` returns `mirrorOf`, `readOnly`,
  `lastSyncedAt` and `upstreamCommit`. The catalog record is republished at
  least every ten minutes and the about answer cached for ten, so "synced"
  can read up to about twenty minutes old; any upstream change republishes
  at once.
- The Code tab shows "Mirror of forkmesh.com/forkmesh/forkmesh · synced
  <time>" above the latest-commit row (hover: date and served commit), the
  notice "Read-only. Send changes upstream or into a fork." and a Fork button
  opening the fork menu. The label comes from the about answer, so it reads
  the same at `/<owner>/forkmesh` and, single-organization, at `/forkmesh`.
  File editing is disabled; branch creation, merging and new pull requests
  are hidden. Issues, Pull requests and Discussions stay off: the bootstrap
  writes them off and the about answer reports them off for any read-only
  mirror.
- Description: the git node publishes `about` (else `description`) from
  `.forkmesh/info.json` at the served HEAD as the catalog `description`
  (`catalogDescription` in `server/catalog.go`, whitespace collapsed, at most
  240 characters; needs a rebuilt node binary). Without one, the card and
  About rail show "Read-only mirror of <upstream>" (`repoDescriptionText`),
  drawn by the page: the relay never edits a signed catalog field. Tests:
  `TestCatalogDescriptionComesFromTheRepositoryAbout`,
  `test_a_mirror_without_a_published_description_names_its_upstream`.
- A node never labels a repository as a mirror of a page on its own relay
  (`mirrorOfFor` checks `sameRelay`), so a forkmesh.com fleet node with a
  copied instance config cannot mark the canonical repository read-only.
- The app refuses writes with HTTP 403 `read_only_mirror` (`file-edit`,
  `branches/ops`, `branches/delete`, `pulls`, `pulls/<n>/merge|update`), and
  `git push` gets `remote: This repository is a read-only mirror of
  https://forkmesh.com/forkmesh/forkmesh. Send changes upstream there, or
  fork this repository and change your fork.` (git prints a `text/plain`
  error body as `remote:` lines). The gateway does not offer
  `git-receive-pack` for that directory, so an SSH push is refused by
  authorization, without the text.

New installs get all of this. `--update` rewrites `mirror-node.json` (adds
`mirrorOf` and `eventDrivenUpstreams`) and `app.env` (`INSTANCE_UPSTREAM`)
and restarts changed services. A node release that fails `--check` with
`mirrorOf` gets the config without it (the installer says so); website
labelling and refusals still apply.

**Single-organization instances** (opt in). With
`"singleOrganization": true` (only with `ownerType` `org`), the bootstrap
writes the `repo_paths` row `{"canonical":true,"owner":"<org>","short":true}`
when it creates the organization, so every repository shows only as
`https://<domain>/<repo>`, the platform mirror included
(`https://<domain>/forkmesh`). The row is written `ON CONFLICT DO NOTHING`
and only for a new organization, so a re-run never overrides an admin, and
`--update` never runs the bootstrap. Convert an installed instance with
`python -m forkmesh_host instance-single-org --org <org>` (idempotent,
`--off` to switch back); see [server update](../app/server-update.md).

**Web ports and ufw.** With ufw active, the installer opens 80 or 443 only
if no ufw rule mentions that port, so a narrowed rule (for example
`ufw allow proto tcp from <cidr> to any port 80,443`) survives every
`--update`. With `"webFirewall": "cloudflare"` in
`/etc/forkmesh-instance/instance.json` (for an operator who puts their
instance behind Cloudflare's proxy), the installer keeps 80 and 443 open to
Cloudflare's published ranges only: it allows the current ranges, removes
retired ones and then the plain `80/tcp` and `443/tcp` rules, and refreshes
weekly (`forkmesh-web-firewall.timer`). Nothing changes when the list cannot
be fetched; SSH is never touched. Switching back to `"open"` removes the
timer but leaves the narrow rules for the operator.

**Local Caddy and runner firewall changes.** `--update` rewrites
`/etc/caddy/Caddyfile` and `/etc/forkmesh-runner/jobs.nft`. Keep local
changes in the included files the installers never touch:
`/etc/caddy/conf.d/*.caddy` (top level), `/etc/caddy/site.d/*.caddy` (inside
the site block) and `/etc/forkmesh-runner/jobs.d/*.nft`. A runner rule
letting a job reach a private address must precede the built-in rejects, so
use `insert rule inet forkmesh_runner output ...`; an `add rule` never
matches. If a `jobs.d` file does not parse, the firewall loads
`jobs-base.nft` alone and logs it.

## Importing a repository on the server (`instance_repo.py`)

`python -m forkmesh_host instance-repo` creates a hosted repository and fills
it from a git repository or bundle on the server. The operator's root shell
is the authority: no browser session, password or push credential. Run as
root from the source checkout:

```
cd /opt/forkmesh-instance/src/app/host
/opt/forkmesh-instance/venv/bin/python -m forkmesh_host instance-repo \
  --owner <account> --organization <org> --name <repo> \
  [--description TEXT] [--visibility public|private] \
  --from /root/<repo>.git [--force] [--no-hooks] \
  [--fork-of <owner>/<repo>] [--protect-default-branch] [--actions on|off] \
  --env-file /etc/forkmesh-instance/app.env \
  --db sqlite:/var/lib/forkmesh-instance/app.db \
  --data-dir /var/lib/forkmesh-instance/data
```

It prints one JSON line: `ok`, `owner`, `organization`, `name`, `id`,
`created`, `branches`, `tags`, `defaultBranch`, `commit`, `refsUpdated`,
`sizeBytes`, `stateHash`, `gitDir`, `forkedFrom` and `settings` (or
`ok: false` and `error`, exit 1).

- **Fork link.** `--fork-of <owner>/<repo>` records a fork of a public
  repository on the instance as the web Fork action does: `forkedFrom` in
  the catalog record (the parent's public identity; an organization alias
  stays the organization, as `_public_repo_owner` keys fork edges) and the
  `repo_forks` edge (`_index_repo_fork_edge`). The parent must exist, be
  public, not be the repository itself, and share history: its `commit` or
  `rootCommit` must be in the source (checked before anything is created
  when `--from` is a directory) and in the staged repository before objects
  or refs move. Later runs without `--fork-of` keep the link; a rerun with
  it reports `"forkedFrom": "unchanged"`. Objects are not shared.
- **Switches.** `--protect-default-branch` / `--no-protect-default-branch`
  and `--actions on|off` set feature settings before the fill, audited as
  `repository.protection_settings`
  ([branch protection](../app/branch-protection.md)).
- **Counts and activity.** The fill writes the default branch's full
  `commitCount` (`git rev-list --count`), `mergeCount`, and the 52-week
  `activityWeeks` / `mergeActivityWeeks` series, bucketed as in
  `server/catalog.go` (no 10,000 cap; that cap applies only to per-ref
  counts in a push report). A rerun that moves no refs still refreshes them
  (`hosted_git.record_hosted_stats`), which repairs older imports.
- **Source.** `--from` is a repository directory or `git bundle` file. Only
  local `refs/heads/*` and `refs/tags/*` are imported, so create it with
  `git clone --mirror <url> /root/<repo>.git` (a normal clone's other
  branches are remote-tracking refs and are skipped). `refs/pull/*` is
  skipped.
- **Who does what.** As root, database work runs in a child that drops to
  `forkmesh-app` (`--app-user`), so SQLite files keep their owner, and every
  git command runs as `forkmesh-node` (`--git-user`), owner of
  `/var/lib/forkmesh/hosted` (`--hosted-root`). The source is first written
  as a bundle into `/var/lib/forkmesh/tmp` (`forkmesh-node` may not read the
  operator's folder), holding only what the hosted repository lacks: every
  hosted head and tag the source has is a `git bundle create` exclusion, so a
  rerun after one commit bundles one commit. As another user, everything runs
  as that user (tests, development).
- **Create.** The same records as the create form for an organization
  repository, through helpers shared in `hosted_git.py`
  (`core_repository_target`, `core_catalog_record`,
  `store_core_repository`): catalog record (`source: hosted-web`, `cloneUrl`
  `<PUBLIC_BASE_URL>/<org>/<repo>`), `hosted_repos` row with creator = owner,
  `org_repos` link, cache and catalog revision bumps and the
  `repository.create_hosted` audit row. The owner must be a user with
  maintain permission in the organization; no first commit is written and
  the hosted quota is not checked (the repository still counts).
- **Fill.** The command creates `<root>/<id>.git` if the node has not
  (`git init --bare --initial-branch=main`, mode 0700, then a rename, like
  `server/hosted.go`). It works in a staging repository, a quarantine like
  `git receive-pack`'s: a bare repository in
  `/var/lib/forkmesh/tmp/instance-repo-*` (else beside the hosted root) whose
  `objects/info/alternates` names the hosted objects. Heads and tags are
  fetched there (a directory fetch negotiates with the hosted tips; a bundle
  is indexed whole) and the fast-forward plan runs there. Then one pack is
  built with `git pack-objects --revs --local` from the new tips minus the
  hosted tips, so a full bundle of a repository the node holds adds only the
  missing commits. The size check adds that pack to the hosted reachable
  size (`git rev-list --all --objects --disk-usage`, counting each object
  once). A refused ref or passing `HOSTED_REPO_MAX_BYTES` raises before
  anything reaches the hosted repository; the scratch folder is removed with
  `forkmesh_safe_delete.remove_tree` either way. On success the pack is
  renamed into `objects/pack` (`.pack`, `.rev`, then `.idx`, git's order) and
  refs applied in one `update-ref` transaction: new refs created, branches
  fast-forward only, tags never move, unless `--force`. Nothing is deleted.
  A failed `update-ref` after the move leaves an unreferenced pack the node's
  maintenance keeps (no prune) and a rerun reuses. The fill touches
  `forkmesh-maintenance-due` so the node compacts next
  ([hosted repositories](core-hosted-repositories.md#maintenance)). HEAD points at `main`,
  else `master`, `trunk`, `develop`, else the first branch. (Fetching
  straight into the hosted repository rewrote the whole history as a new
  pack each run, because a bundle has no negotiation.)
- **Record.** As after an accepted web push: `hosted_repos.pushed_at` /
  `pushed_by` (so the node does not fetch over it), the routing pin from the
  filled repository in `routing_refs_sha256`'s canonical form (default
  branch plus tags) through `_pin_hosted_state` (catalog `stateHash`,
  `lastSync`, `branch`, `commit`, `branchCount`, `commitCount`, `rootCommit`,
  `sizeBytes`, state history), and `core_push.record_accepted_push` with the
  ref updates (default branch first, 40 listed): one `git.push` audit row per
  ref and, unless `--no-hooks`, push webhooks, code-push ping and Actions
  queue. An import that creates the default branch queues one Actions push
  job, for that branch only
  ([instance actions](../server/instance-actions.md#queue-lease-report)).
- **Idempotent.** A second run finds the `hosted_repos` row and only moves
  refs; with nothing new it records nothing (`refsUpdated: 0`). A repository
  under another organization, or a catalog name a node published, is
  refused.
- **Serving.** The git node learns of a new repository on its next replica
  exchange (at most two minutes; `systemctl restart forkmesh-mirror-node`
  makes it immediate) and serves it at `/<owner>/<repo>` and
  `/<org>/<repo>`. After an update the gateway may answer from its response
  cache until that exchange; restart the node to serve new refs at once.
- **Size.** The installer sets `HOSTED_REPO_MAX_BYTES=1073741824` (1 GiB) in
  `app.env` and `hosted.maxRepositoryBytes` to the same in
  `/etc/forkmesh/mirror-node.json`. The command refuses a fill whose
  reachable objects plus the new pack pass the app limit; web pushes are
  refused past it too (largest copy the node reports, `git count-objects`
  loose plus pack bytes after maintenance). For a bigger repository raise
  `HOSTED_REPO_MAX_BYTES` in `app.env` (and the node's value, which bounds
  imports and replica fetches) and restart `forkmesh-instance`. HTTPS pushes
  are also bounded per request by `FORKMESH_MAX_GIT_PUSH_BODY_BYTES`
  (256 MiB).

## White label

- `INSTANCE_NAME` names the instance: email sender and card, page titles,
  and `app/public/instance-brand.js`, which renames the product in visible UI
  text (never user content) and hides switched-off plugin UI.
- `GET /api/instance` returns name, branding, logo, plugins, offsite hosts
  and theme. `logo` is the `INSTANCE_LOGO` path, `/assets/logo-96.png` when
  unbranded, or empty; the phone and desktop apps save it as the project
  logo when a project attaches the instance.
- **Logo:** a `data:` URL (PNG, JPEG, WebP or SVG, 200 KB max, type detected
  from bytes, unsafe SVG refused). The brand tool writes it over the logo,
  favicon and web manifest assets; `INSTANCE_LOGO` points email at it. It
  always lives at `/instance-logo.<png|jpg|webp|svg>` and the manifest
  (`/favicon/site.webmanifest`) lists it. An SVG logo also gets a 512 px
  `/instance-logo.png` from `rsvg-convert` (`librsvg2-bin`), listed first,
  and `INSTANCE_LOGO` names the PNG, because phones and most mail clients
  cannot draw SVG. Web pages keep the SVG.
- **Theme** (`app/src/instance_theme.py`): one `instance_settings` row
  (`k = 'theme'`) of `{"dark": {...}, "light": {...}}` known token names to
  `#rrggbb`, max 8 KB, no raw CSS, served as `/instance-theme.css`. Set at
  setup from a preset name or colour object, or later with
  `PUT /api/instance/theme` by an admin. The installer passes an object to
  `instance-bootstrap` as JSON and refuses other modes, odd token names or
  non-hex values. See [Named themes](#named-themes).
- **Plugins** (`app/src/instance_features.py`): ForkMesh-company features
  (world, hackathon, ads, rewards, blog, status, developer docs, website
  pages and others) are off unless turned on; the platform changelog is on
  unless turned off. A disabled plugin's routes answer 404 before any other
  route; tagged UI (`data-instance-plugin`) is hidden. See [Plugins](#plugins).
- **Market packages:** every account on an instance has the pre-marketplace
  packages (repository tabs, Chat, Network, Agents, ...) installed until it
  removes one, because an instance has no Market onboarding ("Account state"
  in [core plugins](../marketplace/core-plugins.md)). Nothing is
  stored, so existing accounts get them on the next app restart.
- **Pages** (`app/src/instance_pages.py`): the instance renders its own
  `/desktop` and `/docs` guide pages.
- **Legal pages** (`app/src/instance_legal.py`): Terms, Privacy and Security
  are separate settings, all off on a new instance: links hidden everywhere
  (sidebar, header menu, footers, sign-up agreement), `/terms`, `/privacy`
  and `/security-report` answer 404, no `/.well-known/security.txt`. An admin
  turns each on in Platform → Legal pages, as Markdown served at `/terms`,
  `/privacy` or `/security` (sanitized by the README renderer) or as a link;
  Security on also publishes `security.txt`. `GET /api/instance` reports them
  as `legal`. The install setting `legalPages` turns pages on at setup:

  ```json
  "legalPages": {
    "terms": {"markdown": "# Terms of use\n\n..."},
    "privacy": "https://example.org/privacy",
    "security": {"url": "https://example.org/security", "contact": "security@example.org"}
  }
  ```

  Each takes `markdown` or `url` (a string is a URL), `null`/`false` or
  nothing for off; only `security` takes `contact` (email or https). The
  bootstrap writes it once (`ON CONFLICT DO NOTHING`); `--update` ignores it.
  forkmesh.com keeps its own pages.
  Details: [instance legal pages](../app/instance-legal-pages.md).
- **ForkMesh's own sites** (`"whiteLabel": true`, off by default;
  `apply_instance_brand --white-label`, `INSTANCE_WHITE_LABEL=1`;
  `apply_instance_brand.offsite_chrome`, `instance_features.OFFSITE_CHROME`):
  the brand tool removes header and sidebar links to forkmesh.com pages
  (Marketplace `forkmesh.com/market`, Pricing, Membership), app.forkmesh.com
  and Patreon from every built page, points the sidebar's Home at `/` and the
  World links at `/world` (hidden while the World plugin is off), so the
  links are gone at first paint and without JavaScript.
  `/instance-theme.css` also hides any such link the dashboard draws later in
  `header`, `nav`, `aside` or `footer`, except inside the Powered by badge
  (kept while `poweredBy` is on). forkmesh.com is untouched.

## Home page

An admin picks a home repository in Platform → Home page
(`instance_console.render_home`, `save_home`; `app/src/instance_home.py`):
one `instance_settings` row (`k = 'home'`, `{"repository": "owner/name"}`),
CSRF-checked and audited as `admin.console_save_home`; empty turns it off.
`GET /api/instance` reports `home` (`{"repository": ...}` or `null`; always
`null` on forkmesh.com).

With one set, `/` is a forum front page rendered by the host
(`app/host/forkmesh_host/read_surfaces.py`): the README with one
`dir="auto"` section per heading, the latest threads from its issue index,
its labels, Atom feed and lite view links, and a slot for the mirror line.
No JavaScript, no request to another site. Without one, or while the
repository is not public, `/` redirects to `/dashboard`.
Details: [feeds and lite](../app/feeds-and-lite.md).

## Plugins

An admin turns optional plugins on and off in Platform → Plugins
(`app/src/instance_console.py`), without reinstall or restart.

| Plugin | On an instance |
|---|---|
| `world` | The instance's own World at `/world` with the Office and its chat. With a World listing ([Directory](#directory)) a portal joins it to the ForkMesh World. |
| `guestbook` | The agent guestbook in the instance's chat. |
| `genie` | The tip spider at the bottom of each page. |
| `developer-docs` | The dashboard's Developer docs, read from the instance's read-only `forkmesh` mirror. |
| `issue-ai-prompt` | The "Fix <url>: read the issue, make the change…" box under an open issue's title, which readers copy into a coding agent. On by default. |
| `fediverse-ui` | The Fediverse thread panel under issues, pull requests and discussions, and the repository's Watchers row with its "Watch on the fediverse" menu. Off also answers 404 to `GET /api/repo/<o>/<r>/fedi-comments`. On by default. |
| `changelog` | The platform changelog at `/changelog`: what changed in the platform version the instance runs. On by default. |
| `forum-list` | Forum-style issue lists: newest first, with each row's last few commenters ([repo views](../app/repo-views.md)). Opt-in (`OPT_IN`): off unless chosen, on forkmesh.com too. |

Everything else (`hackathon`, `challenges`, `pitch-deck`, `ads`, `patreon`,
`discord`, `microjobs`, `rewards`, `referrals`, `forkbot`, `ecosystem`,
`blog`, `status`, `ios-testers`, `website`, `instance-launcher`) needs
forkmesh.com's accounts, ledger, keys or content. The page lists them under
"forkmesh.com services" with the reason (`FORKMESH_ONLY`) and no switch;
`/api/instance` reports each plugin's `scope` (`instance` or `forkmesh`) and
`note`. An operator can still name one in `INSTANCE_PLUGINS`; a saved choice
keeps it.

- **Where the choice lives.** Saving writes the `instance_settings` row
  `plugins` (`{"enabled": [...]}`), CSRF-checked and audited as
  `admin.console_save_plugins`. Until the first save, `INSTANCE_PLUGINS`
  (install setting `plugins`) decides; then the row wins. The installer never
  writes the row and `--update` only rewrites `INSTANCE_PLUGINS`, so updates
  keep the admin's choice.
- **On by default (`DEFAULT_ON`).** `changelog`, `issue-ai-prompt` and
  `fediverse-ui` stay on until an admin switches them off, including on an
  instance that saved its choice before they existed: `parse` adds every
  `DEFAULT_ON` plugin the row does not name in `disabled`, and saving writes
  ones left off as `disabled` (`{"enabled": [...], "disabled":
  ["fediverse-ui"]}`). `INSTANCE_PLUGINS` need not name them; `-fediverse-ui`
  turns one off. A future plugin that changes something instances already
  show joins `DEFAULT_ON`, so no update silently removes it.
- **Opt-in (`OPT_IN`).** forkmesh.com (an unbranded core) normally runs every
  plugin. An `OPT_IN` plugin changes pages enough to be a choice everywhere:
  off on instances, and on a core only when `FORKMESH_CORE_PLUGINS` (comma
  list in the core's environment) names it.
- **The changelog on an instance.** The page is the static
  `www/public/changelog.html`, which loads `/changelog.js` and reads
  `/changelog.json`, the `changelog/changelog.json` of the running commit
  ([changelog](../engineering/changelog.md)), so it needs nothing from forkmesh.com.
  `build_site_assets.stage_app` copies page and script into `app/dist`
  (`BACKEND_ASSETS`, like `blog.html`) with the JSON beside them;
  `/changelog.json` is served even with the plugin off (public release
  notes). With `WWW_ORIGIN` set to the instance's origin,
  `external_site_delivery` answers `local` and `_serve_site_document` serves
  the copy. forkmesh.com's app origin still 308s `/changelog` to
  forkmesh.com. The brand pass renames the platform word.
- **The Company group.** The sidebar's `sidebarGroupCompany`
  (`app/public/dashboard/partials/sidebar.html`) and the site header menu's
  Company group (`www/public/site-header.js`) carry
  `data-instance-plugin="website"`, so `chrome_css` hides the whole group,
  title included, from first paint while `website` is off.
- **Not only hidden.** The dashboard asks `instancePluginOff(id)`
  (`02-helpers.js`, from the page's `/api/instance`) and leaves the prompt,
  Fediverse thread and watch menu out of its markup; `loadFederatedReplies`
  never calls `fedi-comments` while `fediverse-ui` is off. The SSR issue page
  and About rail ask `repo_ssr_issue.plugins_off()`
  (`instance_features.switched_off()` for the env remembered by
  `remember_instance`), and the issue render cache is keyed by it. Until the
  dashboard has the instance document, `chrome_css` (`PLUGIN_ELEMENTS`)
  hides the same elements, so nothing flashes in.
- **How it applies.** Every instance request passes
  `Default._instance_plugins_load` before the plugin gate. It reads the row at
  most every 30 seconds (`CACHE_MS`) into a process cache that `enabled_ids`
  uses; a failed read keeps the last value. The saving process updates its
  cache at once; `/api/instance` (cached 60 s) and the brand script follow
  within a minute. forkmesh.com never reads the row: every plugin stays on
  there, and the page says so.
- **The most specific prefix wins.** `feature_for` picks the plugin with the
  longest matching prefix, so `/api/world/store` and `/api/world/wallet`
  belong to `rewards` and `/api/world/community-ads` to `ads` even while
  `world` (owner of `/api/world`) is on.

### The World on an instance

The World client lives in `world/public/world`, and forkmesh.com serves it
from a separate `world/dist`. The instance build runs
`app/tools/build_site_assets.py instance-world` after `forkmesh_host build`
and before `apply_instance_brand.py` (`build_app` in the installer). It
builds `world/dist` as for world.forkmesh.com (bundled scene, minified and
versioned modules, three.js r184 vendored under `/world/vendor/`) and copies
it into `app/dist`: all of `world/`, plus World-only files the app lacks (bot
avatars, marketplace World packages, `security/latest.json`). It never
overwrites an app file, drops the World's `_headers` and removes the canonical
link to forkmesh.com. About 10 MB on disk (6 MB source maps); it needs the
esbuild the Office build installs. A plain `build_site_assets.py` run skips
this target.

The World is always bundled; the plugin decides per request. The installer
sets `WORLD_ORIGIN` to the instance's origin, so `external_site_delivery`
answers `local` and `_serve_world_asset` (`app/src/app_runtime.py`) serves
`/world` and `/world/` from `world/index.html` and other `/world/...` paths
from the exact bundled file (`static_routes.world_asset_path`; dot segments
and unknown files 404). Headers:

- `content-type` from the file (`text/javascript` for modules, `image/webp`
  and so on).
- `cache-control`: `public, max-age=31536000, immutable` for `?v=` URLs and
  `/world/vendor/`; `no-store, max-age=0, must-revalidate` for the document
  and unversioned files, so a rebuild never mixes module graphs.
- The World CSP (`WORLD_DOCUMENT_SECURITY_HEADERS`): `script-src 'self'
  'unsafe-inline' https://static.cloudflareinsights.com`, no CDN since
  three.js is local. The Cloudflare origin is the Web Analytics beacon
  Cloudflare injects when the operator proxies the instance through
  Cloudflare; the site-wide CSP allows it too. `connect-src 'self' https:
  wss:` for the API and the `/api/world/ws` presence socket; frames `'self'`
  for the embedded chat; `frame-ancestors 'none'` (the global HTML layer
  sets `X-Frame-Options: DENY`).

With the plugin off, the gate answers 404 for `/world`, `/world/*`,
`/api/world/*` and `/office` first.

Every World API call is page-relative, and `api-client.js` keeps
`location.origin` on any host but forkmesh.com, so presence, chat, the
Office, plots, notes and preferences reach the instance's own app. The
World's server code calls no forkmesh.com address (satellites read
celestrak.org, social boards x.com and reddit.com), so the egress guard
needs no new allowance.

Branding: the brand tool renames the World's title ("Acme Code World") and
adds the brand script. Canvas text (signs such as "FORKMESH OFFICE") is
renamed by `world/public/world/world-brand.js`, which on a branded page
wraps `fillText`, `strokeText` and `measureText` of 2D contexts so measured
and painted text match. It leaves "ForkMesh World" (the portal home),
"ForkMesh Desktop", `forkmesh.com` addresses, `@forkmesh` handles and
lower-case repository names. The logo is `/assets/logo.svg`, replaced by the
instance logo.

Not on an instance: rewards and ledger parts (World store, wallet, central
fund, pending rewards), community ads, the ForkMesh blog board, the
`forkmesh/forkmesh` mirror and agent-bot boards (an instance has no
repository by that name), Discord, and the relay federation towers. They
are hidden in the instance's hexagon (below).

The World asks `/api/instance` once (`instanceInfo()` in `world.js`) and
skips what the instance switched off, so the console stays clean: with
`rewards` off it skips `/api/accounts/central-fund` and
`/api/rewards/pending`; with `ads` off the community placement and
`/api/ads/serve`; on any instance (`branded: true`) the forkmesh.com
directory `/api/start/instances` (the ForkMesh World next door is drawn from
`/api/instance`). `instancePluginOff` and `instanceKeepsDirectory` in
`world-instance-portals.js` decide. `/api/accounts/central-fund` belongs to
`rewards` on the server too, so it answers 404, not 503. The one error left
for a signed-out visitor is the 401 from `/api/accounts/sessions`: the
session cookie is HttpOnly, so asking is the only way to learn there is
none.

The World never changes the page address by itself: it opens repositories,
issues and other sites with `window.open(..., "_blank")` and travels only on
a portal or site-menu click. An embedded browser that loads `_blank` in the
same tab, or a tool driving a shared browser's active tab, can look like the
World navigating away.

### An instance World is its own hexagon

An instance World is one hexagon of its own, placed where the directory put
it, with the ForkMesh World across the road, so visitors do not take it for
a copy of world.forkmesh.com.

**What is drawn.** `territoryFromInstance()` (`world-territory.js`) turns
`/api/instance` into the hexagon's settings whenever `branded` is true. The
World hides every ForkMesh town element (Office tower, beach, coaster,
districts, boards, bots, mirror-node yard, desert plots: the
`TERRITORY_HIDDEN_ELEMENTS` list, passed with the visitor's own choices as
`initialDisabledElements` to the Elements panel's registry) and keeps what
comes from the instance's data: the Members Circle and the repository portal
ring. `scene/instance-territory.js` builds a starter world: ground, a road
around the hexagon, a plaza with a sign carrying the instance name and logo
(`/instance-logo.*`, else `/assets/logo.svg`), benches, lamps and trees.
Fresh visitors arrive on the plaza (`TERRITORY_FRESH_SPAWN`). The Office
stays at `/office`; its tower walls and door zone are off
(`territoryWalkActive()`).

**Style.** `style` is `meadow` (green ground, stone plaza) or `security`
(dark ground, red accents, padlocks and a shield over the sign, terminal
green lettering). `accent` overrides the accent colour; empty takes the
instance theme's dark `primary`, then the style's own (`TERRITORY_STYLES`).

**Buildings that link out.** `objects` lists up to 12 houses
`{type: "house", label, url, position: [x, z], color}`; `position` is from
the plaza centre, within 300 on each axis, and at most 330 from the hexagon
centre (the plaza sits 85 in front of it), the reach the ForkMesh World draws
a neighbour scene to (`NEIGHBOUR_SCENE_REACH`). Clicking a house leaves for
`url` (https only, no credentials) in the same tab after the "Entering …"
notice (`openTerritoryLink`). Nothing opens by walking.

The neighbour scene (`/api/world/neighbour-scene`) is served from the
instance's own `PUBLIC_BASE_URL` and answers 404 without one, so a request's
`Host` never picks the address the server fetches the bug list from. Until an
operator saves World settings the route falls through to the existing
handler, so `WORLD_NEIGHBOUR_SCENE` and the default scene still apply.
Answers are edge-cached (600 s, or 60 s when the bug list could not be
read), so anonymous requests do not repeat the reads and fetch.

**Bug terrarium.** `bugs: {repo, ref, path, title, limit, position}` draws a
glass enclosure with up to 60 crawling bugs. The list is a JSON file in one
of the instance's repositories, read through
`/api/repo/<owner>/<name>/raw?path=...&ref=...`: an array of
`{domain, name, url, favicon, count}` (`count` optional). `favicon` is a
path next to the list (`favicons/<domain>.png`; png, ico, gif, jpg or webp)
drawn on the bug's back, so the browser loads only same-origin files and no
third-party favicon service sees who looks. A path that leaves the
repository, an absolute address or an SVG favicon is dropped
(`territoryFaviconURL`). Clicking a bug shows its domain, name and count with
a link to `url` (https only).

**Settings.** The `instance_settings` row `world`
(`app/src/instance_world.py`, cleaned on save, defaults when missing or
invalid), reported by `/api/instance` as `world`. Admins edit it as JSON in
Platform → World (`save_world`, CSRF-checked, audited as
`admin.console_save_world`).

**The ForkMesh World next door is live.** The hexagon stands on one of the
ForkMesh town's desert petals: the one the hub publishes for it in
`/api/world/mesh`, else slot n's petal n + 1 (`territoryPetal`). ForkMesh's
town, petals, roads, Office tower, coaster, beach and other scenery
(`TERRITORY_NEIGHBOUR_ELEMENTS`) are moved beside the hexagon, walkable and
solid, with no reload at the border ([federation](../world/federation.md)).
Stepping onto ForkMesh's ground moves the walker's presence to ForkMesh's
relay over a visit socket with a visitor token (`name@<this host>` there);
the Office runs against ForkMesh's API (`setOfficeOwner`). Pieces that would
show the instance's data inside ForkMesh's town stay off
(`TERRITORY_HIDDEN_ELEMENTS`) while the walker stands in the instance.

**Stepping into the ForkMesh World.** Crossing the road's midline onto
ForkMesh's ground (`territoryOwnerAt()`) puts the page in the ForkMesh World
without a reload, as world.forkmesh.com enters an instance: the address
stays, and the walker stays signed in here, visiting ForkMesh with a visa.
`enterMeshWorld()` calls `leaveOwnTerritory()`, which moves ForkMesh's
scenery back, hides the hexagon and its clicks, switches on the hidden
ForkMesh pieces the visitor had not switched off, and restores the town's
walk surface. The walker is recentred by the hub's offset, the mesh is laid
out from the hub (`placeWorldMesh`, the instance as the hub's neighbour),
town reads go to ForkMesh through the federation client (`meshRoot`),
residents arrive over the visit socket, and the notice reads
`Entering ForkMesh · world.forkmesh.com`. Stepping back calls
`placeTerritoryNeighbour()` → `enterOwnTerritory()`, restoring the hexagon,
clicks, hidden list and ForkMesh next door. The saved position stays in the
instance's frame, so a reload on ForkMesh's ground switches again.

**What ForkMesh's World draws.** From a distance forkmesh.com draws the
hexagon from `GET /api/world/neighbour-scene`
(`app/src/instance_world_scene.py`: ground, plaza, sign, lamps, houses with
links, the terrarium with up to 32 bugs). A ForkMesh walker who steps onto
it, without a reload, stands in this World as it draws itself: built with
the same code from this instance's `/api/instance`, ForkMesh's town next
door, and Members Circle, portals and town content read from this instance
([World mesh](../world/mesh.md#standing-in-an-instances-hexagon)). They
open a visit socket to this relay with a visa and appear as
`name@world.forkmesh.com`.

**Crossing the road (address arrivals).** The instance installs no
crossings (`crossings` stays empty), so walking never navigates; the
in-place switch above replaces it. `checkTerritoryCrossing()` would navigate
on entering a listed crossing tile (`crossedInto`) to
`<home>?from=<instance domain>&slot=<n>&pos=<x>,<z>` (`home` is the
directory's `portal.home` or `https://world.forkmesh.com/world/`; `pos`
relative to the hexagon centre, rounded to a tenth, within 400). Walking from
world.forkmesh.com into an instance is the world-mesh flowers' job
([World mesh](../world/mesh.md)); the territory turns off the mesh
neighbours (`placeWorldMesh`) so the layouts never overlap. On arrival
`territoryArrival()` validates the parameters (a domain or `forkmesh`, slot
0-11, two short numbers), places the visitor on the matching road edge 14
units in, facing away (`edgeArrival`), and strips them from the address.
Sign-ins do not cross: cookies are per origin, so the URL carries no token
and the visitor sees "Visiting from <origin> as a guest" unless signed in
there. A crossing waits four seconds before it can fire again.

### Platform name

The instance name replaces "ForkMesh" wherever the platform itself is meant,
in three layers so nothing depends on the browser alone:

- **Server.** `instance_features.display_name(env)` is the name (`ForkMesh`
  without `INSTANCE_NAME`); `instance_features.rename(env, text)` /
  `rename_html` swap it in (no-ops on forkmesh.com). They cover the
  repository page's `og:site_name`, `og:title`, `twitter:title` and default
  description, the profile `<title>`, the repository fediverse page, the
  repository card's default line, every email subject and body
  (`_send_email`; the "Powered by ForkMesh" footer is added after the
  rename), hosted-repository limit messages, "unavailable" pages, and
  plain-text Git errors (`_git_error_as_text` gets the env and drops the
  forkmesh.com help link). The founders' day-one feedback email is not sent
  on an instance.
- **The git node account.** The instance's git node (`INSTANCE_FLAGSHIP_NODE`,
  `forkmesh`) authors merge and metadata commits and owns the platform
  mirror. `/api/instance` reports it as `gitNode`; the dashboard shows it as
  the instance name wherever a commit author is shown (latest-commit line,
  history, commit detail, contributors, Insights) via `shownAccountName()`,
  the Actions snapshot does the same for a run's actor, and repository cards
  drop "published from <node>".
- **Build.** `apply_instance_brand.py` renames `<title>`, the
  application-name and `og:site_name` metas, and the platform word in
  `description`, `og:title`, `og:description`, `twitter:title` and
  `twitter:description`. It writes the name beside the header logo
  (`<span data-instance-name-label>`) so it is there at first paint without a
  layout shift; re-branding replaces the label.
- **Switched-off plugins at first paint.** `/instance-theme.css` ends with
  `instance_features.chrome_css`: one rule per switched-off plugin hiding its
  `[data-instance-plugin~=id]` elements and links to its pages in `header`,
  `nav`, `aside` and `footer` (and `world.forkmesh.com` links when World is
  off), with `li:has(...)` wrappers in a separate rule so a browser without
  `:has` keeps the rest. It starts with `--instance-plugins: ready`; seeing
  that, `instance-brand.js` no longer hides every plugin element until
  `/api/instance` answers.
- **Dashboard.** `platformName()` in `02-helpers.js` reads the brand tag's
  `data-instance-name` (default `ForkMesh`); every platform reference in a
  dashboard string goes through it (Insights "<name> traffic", "Could not
  reach <name>", hosting and import notices, webhook and mirror help, the
  command palette, the profile link title, account-search placeholders).
  Static partial text inside `main` on repository and profile pages (which
  `instance-brand.js` leaves alone so READMEs and user text are never
  rewritten) is wrapped in `<span data-platform-name>`, filled by
  `applyPlatformName()` at boot. `instance-brand.js` renames chrome and other
  pages; it skips identifiers (`X-ForkMesh-Ref`, `ForkMesh-Attestation`) and
  "ForkMesh, Inc.", and drops the word before "desktop". Elements marked
  `data-user-content` keep their own words.

A branded instance also names itself in: the `/docs` and `/desktop` pages
(no "ForkMesh desktop app" or "forkmesh.com/desktop"; no Powered by footer
when `poweredBy` is off), the plugin-off page, email unsubscribe pages, JSON
error messages (`message`, `hint`, `detail`, `title`, `reason` of any 4xx/5xx
JSON answer, via `HostRuntime._instance_error_text`), the queued-agent
notice, nodeinfo `nodeName`, the instance ActivityPub actor's name, default
profile and repository actor summaries, the fediverse daily digest, and the
`User-Agent` other servers see (webhooks, fediverse fetches, workflow HTTP
steps and imports, provider mirrors, repository metadata:
`<name-slug>-<kind>/1.0 (+<origin>)`, from
`instance_features.remember`/`user_agent`). The web manifest takes the name
even without a logo. The build-time brand tool renames the platform word in
all page text and shown attributes outside `<script>`/`<style>`. "ForkMesh
desktop app" reads "desktop app" (`desktopBrand()` in the dashboard,
`data-desktop-brand` spans in partials, both brand passes). The start page
that launches instances is the `instance-launcher` plugin, off on an
instance.

Kept on purpose: the software's identity where it is a fact ("a ForkMesh
node", "runs a ForkMesh version", "other ForkMesh instances", "a checkout of
ForkMesh itself", nodeinfo `software.name` `forkmesh` and its repository);
technical names never shown as prose (`.forkmesh/` like `.github/`,
`forkmesh-actions`, `FORKMESH_*` variables, the `forkmesh` MCP server and its
config path, header and trailer names, the `forkmesh-profile=` DNS record,
service and file names); the `<owner>/forkmesh` mirror, its description and
"Mirror of forkmesh.com/forkmesh/forkmesh" label; the upstream installer
command `curl -fsSL https://forkmesh.com/install.sh | bash` on `/desktop` and
in Settings > Nodes; license and credit text, the optional Powered by badge
and email footer; and strings of ForkMesh-only plugins (ads, hackathon,
links, referrals, rewards, Patreon membership, the Market, developer docs,
the instance launcher). `app/tests/test_instance_platform_name.py` fails on
any other platform literal in dashboard JS (`KEPT` is the allow-list) and on
any "forkmesh", in any case, in the shown text of branded static pages
(`PAGE_KEPT` and `GATED_PAGES`).

### What an instance hides

Besides plugin routes and `data-instance-plugin` UI: the profile Tips card
(`rewards`), the profile "Referred users" panel (`referrals`), the profile
footer's "© ForkMesh, Inc." line and the "Open the dashboard from
forkmesh.com" setting (`website`), and the Patreon connection and membership
cards (`patreon`). The dashboard requests the ads slot (`/api/ads/serve`) or
blog feed (`/rss.xml`) only when `instancePluginEnabled()` says the plugin
is on, so a switched-off plugin costs no 404 per page. The Mirrors tab's
availability-proof links to forkmesh.com docs are left out, and the
repository Watch handle is the instance's own repository actor
(`GET .../about` `fediverse.handle`), not `@forkmesh.forkmesh@forkmesh.com`.

## Dashboard on an instance

The dashboard knows it runs on an instance from the
`<script data-instance-brand>` tag `apply_instance_brand.py` writes into
every page (`dashboardOnInstance()` in `02-helpers.js`); forkmesh.com pages
never carry it.

- **Reconnect node.** On forkmesh.com the header shows "Reconnect node" to a
  signed-in account with no linked node, because nothing else drains its web
  writes. On an instance the git node serves every repository, so a web-only
  account is never told to reconnect and the Settings > Nodes "No node is
  linked" callout stays hidden. The header shows "Desktop offline" only when
  the account linked a node of its own and that node's public profile
  (`GET /api/accounts/<node>`, from its heartbeat) says offline; the title
  names the node. Unknown presence stays quiet.
- **Served by.** On forkmesh.com any of several mirrors can answer, so the
  header pill ("served by <node>", "served by mirror", "mirror
  online/offline"), the card's mirror label, the home list's serving status
  and the About rail's "Direct access" list are real routing information. On
  an instance one node serves everything, so they are not rendered (the pill
  gets `hidden`) and the card reads "online"/"offline" instead of "via
  mirror". The Mirrors tab stays as the diagnostic view; the Powered by
  badge stays.
- **Mirror goal.** The rail's "Mirrors" card counts plain git copies kept
  elsewhere (this instance, the original named by
  `.forkmesh/forge-source.json`, and registered copies) against a goal the
  maintainers set, and shows on instances too
  ([mirror goals](../mirror-quota.md#how-it-is-built)).

## Accounts, sign-up and moderation

An admin chooses under People → **Sign-up & moderation**
(`app/src/instance_console.py`, view `accounts-moderation`); the choice is
the `instance_settings` row `accounts` and applies within 30 seconds without
a restart. Defaults: email required, people pick usernames, no review, no
limits. Details, APIs and storage:
[accounts and moderation](../app/accounts-and-moderation.md).

| Setting | Choices |
| --- | --- |
| Sign-up | open with email required; open with email optional (no address stored, a one-time recovery code instead); invite code only (the Controls code, email optional); closed |
| Usernames | people choose; suggest a random name like `quiet-otter-7f3k`; random names only |
| Only signed-in accounts can post | off / on |
| First-post approval | off / on: a new account's posts wait in the organization's Moderation queue until a moderator approves one |
| New-account period, threads and comments per day | 7 days, 0 (no limit), 0 (no limit) |
| Hide the posts of accounts blocked with "hide their posts" | on |

The page also lists instance moderators (they moderate every repository),
server-wide blocked accounts and the moderation log. Organization owners and
admins name their own moderators and block accounts from the organization
settings.

For a public forum: email optional, suggest random names, only signed-in
accounts can post, first-post approval on, 3 threads and 20 comments a day
for the first 7 days. Sign-up still needs the Turnstile keys
([Limits](#limits)).

## Directory

After a successful launch, `instance_directory.publish_new` writes an
`instance_directory` row keyed by domain (name, URL, logo, choices, World
slot), only when no row exists for that domain and the domain is the
launch's own `sslip.io` name or resolves only to the address the launch
connected to. A launch never updates or removes an entry: the launched
server reports its own success, so anyone with an SSH server could otherwise
overwrite or delete any domain's entry. Later changes go through the
instance's signed announcement. An instance installed directly joins from
its admin console (Platform → ForkMesh directory) or with
`"directory": {"listed": true, "world": true}`: it publishes
`/.well-known/forkmesh-instance.json` and sends a signed announcement that
forkmesh.com checks against that document before writing the row, again
every six hours ([instance federation](../app/instance-federation.md)).
The World has 12 instance slots. Cron phase `instance-health` probes
`https://<domain>/readyz`; two misses in a row hide the entry, a day offline
pings the admins once per outage.

The forkmesh.com homepage's "Federated instances" section
(`www/templates/pages/index.html`) reads `/api/start/instances` without
credentials and shows each listed instance's logo, name, address and status:
"Online" when its last `/readyz` check answered, "Checking" before the first
check or after one miss (entries carry `status` and `checkedAt`; two misses
drop an instance from the list). The list starts with a fixed OWASP BLT card
(logo `www/public/assets/owasp-blt-logo.svg`, from the instance's
`/instance-logo.svg`) linking to `https://git.owaspblt.org/BLT`; a directory
entry on `git.owaspblt.org` (the `data-instances-pinned` value) is skipped so
it does not show twice. It ends with a dashed "Add your instance" card to
`/start` (a card, not a button, because the page's green call to action is
sign-up). The section always shows on forkmesh.com and stays hidden on an
instance. Tests: `app/tests/test_homepage_federated_instances.py`. The admin
console's Instances view can hide, show or delete entries (CSRF-protected,
audited); deleting does not touch the instance.

## Commit identities

Commits the git node and instance runner make themselves (pull merges and
their metadata commits, mirror intake, the issue reconciler, agent
workspaces) are authored as the instance name with `noreply@<domain>`: the
installer sets `FORKMESH_COMMIT_NAME` and `FORKMESH_COMMIT_EMAIL` on
`forkmesh-mirror-node.service` and in a
`forkmesh-runner@<owner>.service.d/identity.conf` drop-in, read by
`server/commit_identity.go` (without them: the node name and
`<node>@users.noreply.forkmesh.com`). Pull-rule and sync-status hints name
the platform the same way. Node and runner run the signed release binary, so
this needs a release that contains it. Web edits and hosted pushes are
authored by the signed-in account with `<account>@users.noreply.<host>`, and
Actions commit-back by `<owner>-actions`.

## Mail (`app/src/mail_providers.py`)

`MAIL_PROVIDER` selects `mailtrap`, `sendgrid`, `mailgun` or `none`;
otherwise SMTP or Mailtrap by configuration. `MAIL_SENDER` and
`MAIL_SENDER_NAME` set the sender. Budgets, suppression and the mail log
([mail log](mail-log.md)) apply to every provider.

## Privacy mode

An admin can make the instance keep no visitor records beyond what it needs
to run: Platform → Privacy mode (`instance_settings` key `privacy`), or the
install setting `privacyMode: true` (`INSTANCE_PRIVACY_MODE=1` in `app.env`).
Only the install setting also reaches Caddy, uvicorn, the git node and the
journal; for an existing instance add `"privacyMode": true` to
`/etc/forkmesh-instance/instance.json` and run `install-instance.sh --update`.
[privacy mode](../app/privacy-mode.md) audits what is recorded normally,
what privacy mode stops, and what remains and why.

## Limits

- Public sign-up needs Turnstile keys in `app.env`; it fails closed without
  them.
- Turnstile tokens must name the instance's own host. The installer writes
  `TURNSTILE_HOSTNAMES=<domain>` (an existing value is kept); without it a
  branded instance accepts the host of `APP_ORIGIN`, never forkmesh.com's
  hostnames (which would fail every check with `hostname_mismatch`).
- Without a fingerprint the first host key is trusted (and shown).
- SSH credentials pass through the app once, in memory only.
- The instance app's egress guard refuses forkmesh.com except
  `app.forkmesh.com` (`FORKMESH_EGRESS_PRODUCTION_ALLOW` in the app unit),
  used only by the directory announcement, and the instance's own domain, so
  an instance on a `*.forkmesh.com` name can still register its own endpoint
  (otherwise: "production host … is off limits for Next").
- Server text not listed under "Platform name" (admin-only operational
  alerts, plugin descriptions on the Plugins page, Solana/ledger notices of
  switched-off plugins, the referral preview page) still says ForkMesh.
- The repository card image keeps the ForkMesh mark in its corner.
- Commit identities, the runner name and the Go node's messages change only
  with a signed release that contains them ([Commit identities](#commit-identities)).

## Tests

`app/tests/test_instance_launch.py`, `test_install_instance_script.py`,
`test_install_instance_staged_build.py` (staged build, rollback, source
switch, git node program), `test_start_instance_page.py`,
`test_instance_white_label.py`, `test_instance_platform_name.py`,
`test_git_error_text.py`, `test_instance_theme.py`, `test_instance_pages.py`,
`test_instance_plugins.py`, `test_instance_registry.py`,
`test_instance_legal_pages.py`, `test_server_update.py`,
`test_instance_directory.py`, `test_instance_git_routing.py`,
`test_accounts_moderation.py`, `test_instance_node_reconnect_frontend.py`,
`test_instance_served_by_frontend.py`, `test_repo_canonical_addresses.py`,
`test_single_org_contributors.py`, and
`app/host/tests_unit/test_ssh_helper.py`, `test_instance_bootstrap.py`,
`test_instance_repo.py`, and `app/tests/host/test_instance_repo_e2e.py` (a
served app with a temporary SQLite database: a repository created from the
web and one from the command list the same way on the dashboard).
