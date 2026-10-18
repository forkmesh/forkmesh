# Self-hosted instances

Anyone can start their own ForkMesh instance, a white-label Git host on their
own server, from `forkmesh.com/start`. The app connects to the server over
SSH, runs `www/public/install-instance.sh`, streams progress to the page, and
finishes on the new `<owner>/forkmesh` repository. Every instance shows a
"Powered by ForkMesh" badge.

## Start page (`app/public/start.html`, `start.js`)

Sections: project name, optional logo and colour theme; server (address, SSH
user and port, password or private key, optional host-key fingerprint);
optional domain (without one the instance uses `<ip-with-dashes>.sslip.io`);
admin account and repository owner (organization or user, with optional
extra organization owners); optional mail provider; listing choices
(`ownWorld`, `listOnHomepage`, both on by default).

- **Set up from a file.** A JSON file whose keys are exactly the launch body,
  or a zip holding that JSON and a logo image. It is read in the browser only.
  The user guide is `www/docs/start-config.md`; examples are
  `app/public/start-config.example.json` and `.zip`.
- **Colour theme.** A toolbar of accent swatches (plus a free colour picker),
  three surface families (Slate, the ForkMesh default, Zinc and Stone), a
  dark/light preview switch and Shuffle, over a live mock of a repository
  page drawn with the same tokens `instance_theme.py` accepts. "Customize
  every colour" edits all 26 tokens for the previewed mode. Untouched, the
  page sends `theme: ""` so the instance keeps the ForkMesh colours; a preset
  loaded from a settings file is sent by name until edited; anything else is
  sent as the full `{"dark": {...}, "light": {...}}` object (about 1.2 KB).
  The browser only builds the colours: the launch API cleans them with
  `instance_theme.clean`, the installer checks their shape, and
  `instance-bootstrap` writes the `instance_settings` row on the server, so
  the choice lives only in the instance's own database.
- Secret fields are cleared after sending and on `pagehide`; nothing is kept
  in browser storage. The page polls the launch every second, keeps the
  launch id in the URL, and on failure refills non-secret fields from the
  launch record.
- The page follows the landing style, not the dashboard's: its checkboxes use
  `--ink-strong` (white in dark mode, near-black in light), like its white
  start button. Until October 2026 they used `--accent-bright` and showed
  green, which the landing style keeps for status only.

## API (`app/src/instance_launch.py`)

| Endpoint | Purpose |
|---|---|
| `POST /api/start/launch` | validate, resolve, read the host key, start the run; `202 {id, domain, address, hostKey, steps, url}` |
| `GET /api/start/launches/<id>` | state, steps, message, log, URL; the 128-bit id is the capability |
| `GET /api/start/instances` | public list of listed instances and World slots |
| `GET /api/start/instances/<id>/logo` | an instance's logo, served with a sandboxing CSP and `nosniff` |

- No account needed. Rate limits per client (stored only as a salted hash)
  and a global concurrency cap.
- The host must resolve to public addresses only.
- `instance_launches` stores no password, key or API key; rows expire after
  seven days. A run that stops reporting for 2 minutes is marked lost; the
  setup continues on the server and a new launch with the same details
  follows it.
- The installer prints `FMI {"step":…,"state":…}` lines; success needs exit 0
  and a final `FMI {"done":true,"url":…}` on the instance's own domain.
  Secrets are scrubbed from the log.

## SSH (`app/host/forkmesh_host/ssh_helper.py`)

The sandboxed, socket-activated `fmnext-ssh-helper` (shared with runner SSH
setup, [RUNNERS.md](RUNNERS.md)) has an `instance` operation. It pins the
host key, downloads the installer and runs `bash "$t" --config-stdin` (via
`sudo -n` for a non-root user). Settings, including passwords and mail keys,
travel as one base64 JSON line on standard input, never on the command line.
Runs may take up to 50 minutes.

App settings: `INSTANCE_INSTALL_URL`, `INSTANCE_SOURCE_URL`,
`INSTANCE_SOURCE_REF`.

## Installer (`www/public/install-instance.sh`)

| Step | What happens |
|---|---|
| `system` | supported Debian/Ubuntu, x86_64 or arm64, at least 1 GB RAM and 6 GB disk, ports 80/443 free |
| `packages` | swap on small machines; git, Python, Node, Caddy, sqlite3, age; service users |
| `source` | shallow clone into `/opt/forkmesh-instance/src` |
| `build` | venv and `python -m forkmesh_host build` without ForkMesh-only packages (`FORKMESH_EXCLUDED_PLUGINS`), then `app/tools/apply_instance_brand.py` |
| `configure` | `/etc/forkmesh-instance/app.env` with fresh `DATA_KEY`, `ADMIN_PASS` and router keys (kept on re-runs) and `instance.json` without secrets |
| `database` | migrations into a single-node SQLite database |
| `services` | `forkmesh-instance.service` (uvicorn on loopback, sandboxed) |
| `https` | Caddyfile for the app and the Git gateway paths, HSTS; waits for DNS |
| `admin` | installs the signed Git node daemon and runs `instance-bootstrap` |
| `repo` | waits for `<owner>/forkmesh` to answer `info/refs` |
| `done` | prints the final URL |

- The setup runs as a detached systemd service; the SSH session only follows
  its log, so a dropped connection does not stop it. A lock prevents two
  concurrent runs. Each step skips work already done, so a re-run resumes.
- DNS counts as ready when public resolvers or `getent` return the form's
  address, an address on a global interface, or the server's outbound
  address. A Cloudflare-proxied name is detected; the SSL/TLS mode must be
  Full (strict). Caddy trusts `Cf-Connecting-Ip` only from Cloudflare's
  published ranges.
- `--update` pulls, rebuilds, migrates and restarts, keeping secrets, data
  and admin; local source changes are saved to a patch file first.
- An admin can run the same update from the admin console (Platform →
  Update ForkMesh). The `services` step installs
  `forkmesh-instance-update.path` and `.service` and the helper
  `tools/instance-update.sh`; the app only writes a request file. Every build
  writes `/opt/forkmesh-instance/build-info.json` (version, commit, message),
  and `app.env` carries `BUILD_REV`, `APP_VERSION`, `BUILD_INFO_FILE` and the
  request and status paths. See
  [app/server-update.md](app/server-update.md).
  `--config FILE` takes the same JSON as the page for a manual install.

## Bootstrap (`instance_bootstrap.py`)

`python -m forkmesh_host instance-bootstrap` reads JSON on stdin and creates
or updates the admin (verified, admin, two-factor required at first login),
other owners, the organization, the `forkmesh` node account and the
`<owner>/forkmesh` alias. `INSTANCE_FLAGSHIP_NODE=forkmesh` lets the single
node's own signed refs digest count as the flagship proof;
`HOSTED_GIT_CORE_NODES=forkmesh` makes it the core for new repositories. The
app does not redirect Git clients to its own origin.
The bootstrap also switches Issues, Pull requests and Discussions off on the
`<owner>/forkmesh` mirror; the owner can turn them back on in the
repository's Settings tab ([app/repo-features.md](app/repo-features.md)).
Admins can serve one owner's repositories at `/<repo>/<folder>` from Platform
→ Repository addresses ([app/server-update.md](app/server-update.md)).

## The platform source mirror (`<owner>/forkmesh`)

`<owner>/forkmesh` (catalog owner `forkmesh`, the instance's git node) is a
read-only mirror of the upstream repository, https://forkmesh.com/forkmesh/forkmesh
by default (the installer's `sourceUrl` with its `app.` host prefix and
`.git` dropped). Nothing typed on the instance can change it: changes go
upstream, or into a fork.

How it stays current (`server/upstreamwatch.go`, `docs/server/README.md`):

- The git node fetches `https://app.forkmesh.com/forkmesh/forkmesh` (the
  `upstreams` entry of `/etc/forkmesh/mirror-node.json`).
- Because that upstream is listed in `eventDrivenUpstreams` and is served by
  another relay than the instance's own catalog, the node opens a public
  watch socket on the upstream relay:
  `wss://app.forkmesh.com/api/nodes/events?watch=forkmesh/forkmesh`. It is
  unsigned and read-only: the upstream relay accepts it for public
  repositories only, advertises no writable frames on it, and sends the same
  payload-free `commits` doorbell (refs digest and generation, no repository
  bytes) it sends its own mirrors when the source publishes. The node answers
  every doorbell, and every reconnect, with an immediate fetch.
- The configured `syncInterval` (15 s) remains a poll of the upstream as the
  fallback for a lost doorbell or a down socket.

Until 2026-10-17 `eventDrivenUpstreams` only told the node "this upstream is
announced on your own relay's socket". On an instance that socket is the
instance's own relay, which never hears of a push to forkmesh.com, and the
listing also replaced the 15 s poll with the 30-minute relay safety cadence.
The live instance therefore updated only when some unrelated wake-up (a
reconnect catch-up, a collaboration event) happened to run a fetch: minutes
in practice, up to 30 minutes in the worst case.

Latency now: an upstream push reaches the mirror after the source node
publishes its new refs to forkmesh.com (seconds), the doorbell fan-out
(about 25 ms queueing), and one fetch from the upstream (one or two seconds
for a typical push), so usually within 2 to 5 seconds. With the watch socket
down, or a doorbell lost, the 15 s poll bounds it at about 15 s plus the
fetch. The upstream side needs the watch channel deployed on forkmesh.com
(`app/src/entry.py` `_node_events_watch_object`, `ForkMeshNodes`,
`notify_repo_watchers`); until then the watch upgrade is refused, the node
logs `upstream watch ...` with a backoff of up to five minutes, and the 15 s
poll alone keeps the mirror current. `curl -s 127.0.0.1:8791/v1/status`
reports `upstreamWatches` and the journal logs each connect and disconnect.

Labelling and read-only:

- The node config carries `mirrorOf: {"forkmesh/forkmesh":
  "https://forkmesh.com/forkmesh/forkmesh"}`. The node then publishes
  `mirrorOf`, `upstreamCommit` (the served HEAD) and `lastSyncedAt` (ms of the
  last successful fetch) in its signed catalog record, and drops the write
  operations (`merge-pull`, `update-pull-branch`, the agent-branch merges,
  `git-receive-pack`) from every gateway entry of that Git directory,
  including the `<owner>/forkmesh` alias.
- `app.env` carries `INSTANCE_UPSTREAM`. The website treats the flagship
  node's `forkmesh` repository as a read-only mirror of it even when the
  node is a release that does not publish `mirrorOf` yet.
- `GET /api/repo/<owner>/forkmesh/about` returns `mirrorOf`, `readOnly`,
  `lastSyncedAt` and `upstreamCommit`. The catalog record is republished at
  least every ten minutes and the about answer is cached for ten, so
  "synced" can read up to about twenty minutes old while the mirror is
  current; any upstream change republishes at once.
- The repository page shows "Mirror of forkmesh.com/forkmesh/forkmesh ·
  synced <time>" in the Code tab, just above the latest-commit row, so the header stays compact (hovering the time shows the date and the
  served commit), the notice "Read-only. Send changes upstream or into a
  fork." and a Fork button that opens the existing fork menu. The label comes
  from the about answer, not the address, so it reads the same at
  `/<owner>/forkmesh` and, in single-organization mode, at `/forkmesh`. File
  editing is disabled and branch creation, merging and new pull requests are
  hidden there. Issues, Pull requests and Discussions stay off: the bootstrap
  writes them off, and the about answer reports them off for any read-only
  mirror even if an admin turned one on.
- A node never labels a repository as a mirror of a page on its own relay
  (`mirrorOfFor` checks `sameRelay`), so a forkmesh.com fleet node that copied
  an instance config cannot mark the canonical repository read-only there.
- The app refuses writes with HTTP 403 `read_only_mirror` (`file-edit`,
  `branches/ops`, `pulls`, `pulls/<n>/merge|update`), and a `git push` gets
  `remote: This repository is a read-only mirror of
  https://forkmesh.com/forkmesh/forkmesh. Send changes upstream there, or
  fork this repository and change your fork.` (git prints a `text/plain`
  error body as `remote:` lines). The node gateway no longer offers
  `git-receive-pack` for that Git directory either, so an SSH push is refused
  by authorization too, without the explanatory text.

New installs get all of this from the installer. `--update` rewrites
`mirror-node.json` (adds `mirrorOf` and `eventDrivenUpstreams`), `app.env`
(`INSTANCE_UPSTREAM`) and restarts the changed services. A node release that
predates `mirrorOf` fails `--check` with it, so the installer then writes the
config without it and says so; the website labelling and refusals still
apply.

## White label

- `INSTANCE_NAME` names the instance: email sender and card, page titles,
  and `app/public/instance-brand.js`, which renames the product in visible UI
  text (never user content) and hides switched-off plugin UI.
- `GET /api/instance` returns name, branding, logo, plugins, offsite hosts
  and theme. `logo` is the `INSTANCE_LOGO` path, `/assets/logo-96.png` on an
  unbranded instance, or empty; the phone and desktop apps save it as the
  project logo when a project attaches the instance.
- **Logo:** a `data:` URL (PNG, JPEG, WebP or SVG, 200 KB max, type detected
  from bytes, unsafe SVG refused). The brand tool writes it over the logo and
  favicon assets and the web manifest; `INSTANCE_LOGO` points email at it.
- **Theme** (`app/src/instance_theme.py`): one `instance_settings` row
  (`k = 'theme'`) holding `{"dark": {...}, "light": {...}}` of known token
  names to `#rrggbb` colours, max 8 KB, no raw CSS. Served as
  `/instance-theme.css`; set at setup from a preset name or a full colour
  object (the start page's picker), or later with `PUT /api/instance/theme`
  by an admin. The installer passes an object to `instance-bootstrap` as
  JSON and refuses one with other modes, odd token names or non-hex values.
- **Plugins** (`app/src/instance_features.py`): ForkMesh-company features
  (world, hackathon, ads, rewards, blog, status, developer docs, website
  pages and others) are off on an instance unless listed in
  `INSTANCE_PLUGINS`. A disabled plugin's routes answer 404 before any other
  route; tagged UI (`data-instance-plugin`) is hidden.
- **Pages** (`app/src/instance_pages.py`): an instance renders its own
  `/desktop` and `/docs` guide pages.

## Directory

After a successful launch, `instance_directory.publish` writes an
`instance_directory` row keyed by domain (name, URL, logo, choices, World
slot). The World has 12 instance slots. Cron phase `instance-health` probes
`https://<domain>/readyz`; two misses in a row hide the entry, a day offline
pings the admins once per outage. The admin console's Instances view can
hide, show or delete entries (CSRF-protected and audited); deleting does not
touch the instance.

## Mail (`app/src/mail_providers.py`)

`MAIL_PROVIDER` selects `mailtrap`, `sendgrid`, `mailgun` or `none`;
otherwise SMTP or Mailtrap by configuration. `MAIL_SENDER` and
`MAIL_SENDER_NAME` set the sender. Budgets, suppression and the mail log apply
to every provider.

## Limits

- Public sign-up needs Turnstile keys in `app.env`; it fails closed without
  them.
- Without a fingerprint the first host key is trusted (and shown).
- SSH credentials pass through the app once, in memory only.
- The instance app's egress guard refuses forkmesh.com; nothing needs it.
- Brand renaming happens in the browser; plain-text server messages still
  say ForkMesh.

## Tests

`app/tests/test_instance_launch.py`, `test_install_instance_script.py`,
`test_start_instance_page.py`, `test_instance_white_label.py`,
`test_instance_theme.py`, `test_instance_pages.py`, `test_server_update.py`,
`test_instance_directory.py`, `test_instance_git_routing.py`, and
`app/host/tests_unit/test_ssh_helper.py`, `test_instance_bootstrap.py`.
