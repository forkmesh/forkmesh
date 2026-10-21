# Self-hosted instances

Anyone can start their own ForkMesh instance, a white-label Git host on their
own server, from `forkmesh.com/start`. The app connects to the server over
SSH, runs `www/public/install-instance.sh`, streams progress to the page, and
finishes on the new `<owner>/forkmesh` repository. Instances show a
"Powered by ForkMesh" badge by default; the `poweredBy` setting (`false`)
leaves it out. The installer then writes `INSTANCE_POWERED_BY=0`, passes
`--no-powered-by` to `app/tools/apply_instance_brand.py` (which marks the brand
script with `data-instance-powered-by="off"`), and `/api/instance` answers
`poweredBy: null`.

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
- **Admin console address.** The page fills `adminPath` with a fresh
  `/admin-` plus 20 random `[a-z0-9]` characters from
  `crypto.getRandomValues` each time it opens (about 103 bits), with Copy and
  New buttons and a note to save it. "Download example zip", "Example
  settings only" and "Copy example settings" each put a new random
  `adminPath` into the file they hand out (the zip is rebuilt in the browser,
  stored, from the static example). See [Private admin console
  path](#private-admin-console-path).
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
| `POST /api/start/launch` | validate, resolve, read the host key, start the run; `202 {id, domain, address, hostKey, steps, url, adminPath}` |
| `GET /api/start/launches/<id>` | state, steps, message, log, URL; the 128-bit id is the capability |
| `GET /api/start/instances` | public list of listed instances and World slots |
| `GET /api/start/instances/<id>/logo` | an instance's logo, served with a sandboxing CSP and `nosniff` |
| `POST /api/start/instances/register` | a self-installed instance joins or leaves the directory ([app/instance-federation.md](app/instance-federation.md)) |

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
- `adminPath` is checked with `admin_path.problem`; a launch without one gets
  `admin_path.generate()` (Python `secrets`). It goes to the installer in the
  settings, is answered once in the `202`, and is never written to
  `instance_launches`: it is one of the scrubbed secrets, so a log line that
  contains it is stored as `[hidden]`. The page shows it during the run and
  does not redirect on success while it is shown, so it can be saved.

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
| `source` | shallow clone into `/opt/forkmesh-instance/src`; on `--update`, a shallow fetch of `sourceRef` from `sourceUrl` into that checkout (nothing is checked out yet) |
| `build` | venv and `python -m forkmesh_host build` without ForkMesh-only plugins (`FORKMESH_EXCLUDED_PLUGINS`: their pages, Worker modules and registry rows are left out, [plugins.md](marketplace/plugins.md)), then `app/tools/apply_instance_brand.py`; on a running instance this happens in `src.next` beside the live tree (below) |
| `configure` | `/etc/forkmesh-instance/app.env` with fresh `DATA_KEY`, `ADMIN_PASS` and router keys (kept on re-runs), `ADMIN_PATH` (see below), and `instance.json` without secrets |
| `database` | migrations into a single-node SQLite database |
| `services` | `forkmesh-instance.service` (uvicorn on loopback, sandboxed) |
| `https` | Caddyfile for the app and the Git gateway paths, HSTS; waits for DNS |
| `admin` | installs the Git node daemon (below) and runs `instance-bootstrap` |
| `repo` | waits for `<owner>/forkmesh` to answer `info/refs`, then installs the Actions runner (setting `actions`) |
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
  and admin; local source changes are saved to a patch file first. See
  [Updates build beside the running version](#updates-build-beside-the-running-version).
- **Actions** (setting `actions`, a boolean, default `true`, kept in
  `instance.json`): the `packages` step adds jq, build-essential, unzip and
  nftables for jobs; `app.env` gets `RUNNER_POOL_TARGET_NODES=forkmesh`,
  `ACTIONS_INSTANCE_RUNNER=forkmesh-runner` and a kept `ACTIONS_BOT_SEED`
  (64 hex). After the git node is up, `install-runner.sh --preregistered`
  from the instance's own source installs the runner `forkmesh-runner` with
  the core's daemon binary under `/etc/forkmesh-runner/accounts/<owner>` and
  `/var/lib/forkmesh-runner/accounts/<owner>` (unit
  `forkmesh-runner@<owner>.service`, jobs as `forkmesh-job`), and
  `python -m forkmesh_host instance-runner --name forkmesh-runner
  --public-key <key> --owner <owner>` registers it. A runner that fails to
  install or register only warns: the instance works without Actions and
  `--update` retries. `--update` re-runs the runner installer (same key,
  restarted on the new binary); `actions: false` stops it. An unclaimed new
  instance removes the runner it installed (only its own account folders,
  and the runner roots, firewall unit and `forkmesh-job` user when the
  install created them). See
  [server/instance-actions.md](server/instance-actions.md).
- **HSTS comes from Caddy only.** The App adds
  `Strict-Transport-Security: max-age=31536000; includeSubDomains` to its
  HTML responses (`_BROWSER_SECURITY_HEADERS`). The Caddyfile used to add its
  own `max-age=31536000` with a plain `header` line, so instance pages carried
  the header twice. The Caddyfile now sets that same value in a `header { defer
  ... }` block, which replaces any upstream value when the response is written, the
  same way the core nodes' `(security_headers)` snippet does in
  `deploy/core/render.py`. Every response, including the Git gateway paths,
  carries exactly one value. The `https` step rewrites the Caddyfile and
  reloads Caddy on `--update`. Test:
  `test_caddy_sends_the_only_strict_transport_security_header`.
- An admin can run the same update from the admin console (Platform →
  Update ForkMesh). The `services` step installs
  `forkmesh-instance-update.path` and `.service` and the helper
  `tools/instance-update.sh`; the app only writes a request file. Every build
  writes `/opt/forkmesh-instance/build-info.json` (version, commit, message),
  and `app.env` carries `BUILD_REV`, `APP_VERSION`, `BUILD_INFO_FILE` and the
  request and status paths. See
  [app/server-update.md](app/server-update.md).
  `--config FILE` takes the same JSON as the page for a manual install.

## Updates build beside the running version

Until October 2026 `--update` checked the new commit out in
`/opt/forkmesh-instance/src`, installed Python packages into the live venv and
rebuilt `app/dist` in place while the app kept serving from those same files.
A build that failed half way left the running app serving half-built assets
and, on its next restart, new code that was never built. Now:

1. **Fetch.** The `source` step fetches the target commit into the running
   checkout without touching the work tree. The checkout's `origin` is set to
   `sourceUrl` first (or to `FORKMESH_INSTANCE_SOURCE_DIR` for that run), so
   changing `sourceUrl` in `instance.json` is enough to switch sources; the
   step says so when it changes. A `sourceUrl` on the instance's own domain
   is fetched with `http.curloptResolve=<domain>:443:127.0.0.1`, through the
   local Caddy, so the update does not depend on public DNS or a proxy
   hairpin.
2. **Stage.** When anything has to be built, the build happens in
   `/opt/forkmesh-instance/src.next`. That tree is the previous release
   (`src.prev`) recycled, or a copy of the running tree the first time; it
   fetches the target commit from the running checkout
   (`refs/forkmesh/build`) and checks it out. When `requirements.txt`
   changed (the venv records its digest in `.forkmesh-requirements`), the
   packages go into `venv.next` (the recycled `venv.prev`, or a copy), with
   `python -m pip`, because a moved venv's script shebangs point at the old
   path. A failed build stops here: the live tree, venv, `build-info.json`
   and `build.stamp` are untouched and the app keeps serving. The failed
   tree stays in `src.next` and is reused by the next attempt.
3. **Switch.** `activate_source` renames `src` to `src.prev` and `src.next`
   to `src` (the same for the venv when one was staged), then writes
   `build-info.json` and the stamp. Nothing is deleted: the two trees take
   turns, so the disk holds at most two copies (about 0.9 GB for ForkMesh).
4. **Roll back.** If anything fails after the switch and before the app is
   answering again (settings, migrations, the restart), the `EXIT` trap moves
   the new tree back to `src.next`, `src.prev` back to `src`, restores the
   venv, `build-info.json` and the stamp, and restarts the app if it had been
   restarted. Migrations are not reversed: they are additive, and the old
   code keeps working on the migrated database. Once the app answers
   (`SOURCE_COMMITTED`), later steps (HTTPS, git node, runner) no longer roll
   back.

Between the switch and the restart (a few seconds of settings and migrations)
the running process sees the new files on disk, as it did during the whole
build before.

### The git node program

The server has no Go toolchain, and the coordinator of an instance that runs
its own fork supplies the git node program it built
(`FORKMESH_INSTANCE_DAEMON=/path/to/forkmesh-mirror-node`). An update without
that variable used to download the signed ForkMesh release instead, which on
such an instance silently replaced the fork's program with upstream's. Now:

- With `FORKMESH_INSTANCE_DAEMON`, the binary is installed (atomically) and
  `/opt/forkmesh-instance/daemon-build.json` records `from: supplied`, the
  commit and the tree id of `server/` at that commit.
- An update of an instance whose `sourceUrl` is not the ForkMesh release
  source keeps the installed program.
- Only an instance that follows `https://app.forkmesh.com/forkmesh/forkmesh`
  still fetches the signed release on update (`from: release`).
- `build-info.json` gets `daemon: {state, builtTree, sourceTree,
  builtCommit}`. `state` is `current` when the recorded `server/` tree equals
  the running source's, `stale` when `server/` changed since, `release` or
  `unknown`. The installer warns on `stale`.

Building on the server was rejected: a 1 GB instance would need a ~250 MB
pinned Go toolchain and a memory-hungry build in the middle of an unattended
update, and a broken build would have to be undone while serving. Keeping a
known-good binary and flagging it never breaks the running node; the node
keeps serving with the older program until the operator rebuilds it
(`cd server && go build -trimpath -o forkmesh-mirror-node ./cmd/forkmesh-mirror-node`
from the running commit on a build machine, copy it over, run
`FORKMESH_INSTANCE_DAEMON=<binary> bash install-instance.sh --update`).

## Private admin console path

The admin console has always lived at `/<ADMIN_PATH>` (`app_runtime._fetch_inner`,
`_admin`); its add-on pages (`/admin/theme`, `/admin/acquisition`,
`/admin/hackathon`) had fixed addresses. On an instance (`INSTANCE_NAME`
set) with a valid `ADMIN_PATH`, the whole console moves to that one private
path:

| Page | Address |
|---|---|
| Console and every `?view=` page (Update, Repository addresses, Legal pages, Instances, 404s, Support, Ecosystem, IDS, Marketing email, Database, …) | `<ADMIN_PATH>` |
| Theme switch, Acquisition, Hackathon | `<ADMIN_PATH>/theme`, `<ADMIN_PATH>/acquisition`, `<ADMIN_PATH>/hackathon` |
| `/admin`, `/admin/` and anything under `/admin/` | the instance's normal 404 page (no redirect, no hint) |

- Rules live in `app/src/admin_path.py`. Valid means: starts with `/`, one
  segment of 8 to 64 `A-Z a-z 0-9 - _` (so never a `.` and never a file
  name), and not a built-in address (`admin`, `api`, `dashboard`, every
  `static_routes.RESERVED_ROUTE_PREFIXES` name, `product-analysis`,
  `transparency`, …). Example: `/manage-x7k2q9`.
- Every link, form action and redirect inside the console comes from the
  same helper: `admin_shell.render_page(..., subpages=shell.subpage_base(env))`
  draws the sidebar and the theme form, Acquisition and Hackathon build their
  forms and redirects from `subpage_base(env)`, and the admin notifications
  (`error_group`, signup failures, website feedback, IDS) link to
  `admin_path.notification_href(env)`.
- Console responses on the private path get `cache-control: no-store`,
  `x-robots-tag: noindex, nofollow` and `referrer-policy: no-referrer` (so
  an outbound link from the console never carries the path in `Referer`);
  the pages already had `<meta name="robots" content="noindex, nofollow">`.
- Console page views are not recorded by acquisition attribution or the
  site-referral counter, so the path never lands in those tables. The admin
  path can still appear in the admin-only error log if a console page fails.
- The path is defense in depth, not authentication: sign-in, the admin
  session and the CSRF tokens are checked exactly as before. A signed-out
  visitor who finds the path is sent to `/login`.
- Not leaked: `/api/instance` has no admin field; `robots.txt` and the
  sitemaps do not list it; only an admin's own account answer
  (`_account_public_payload`, sent to the signed-in account at login and
  session refresh, and `POST /api/accounts/admin-session`) carries
  `adminUrl`, which the dashboard shows as **Admin console** in the account
  menu and as the shield button. Error pages and refusals never name it.
- The admin JSON APIs stay at `/api/admin/...` and `/api/accounts/admin-*`:
  they are session-gated, the desktop and phone clients call them by those
  fixed names, and moving them would hide nothing from someone without an
  admin session.
- Short repository addresses (`/<repo>`, single-organization instances) do
  not capture the console: `_fetch_inner` matches the console path before
  the router and its short-address rewrite. A repository with the same name
  as the path is reachable at `/<owner>/<repo>`, never at `/<repo>`; the name
  is not refused, because a refusal would tell anyone who guesses the path
  that it exists.
- Unset or invalid `ADMIN_PATH`, and every core (forkmesh.com, no
  `INSTANCE_NAME`): exactly as before. The console is at `/<ADMIN_PATH>` when
  set, the add-on pages stay under `/admin/`, and an instance without
  `ADMIN_PATH` has no console route.

### Setting it

- New installs: the installer writes `ADMIN_PATH` to `app.env` from the
  `adminPath` setting. Without one, a fresh install generates
  `/admin-` + 20 random `[a-z0-9]` characters (Python `secrets`, the
  kernel's `/dev/urandom` source) and prints it in the final summary with
  the sign-in line. In a detached run that summary is the setup log, so the
  address is printed only when `/var/log/forkmesh-instance-setup.log` is
  owned by root with no group or other access (as the installer creates it);
  otherwise the summary says it is `ADMIN_PATH` in `app.env`. A path that came
  from the settings is never echoed.
- `write_settings` rebuilds `app.env` from scratch on every run. Before
  October 2026 it did not carry `ADMIN_PATH` over, so `--update` (and the
  admin console's Update button) silently dropped a hand-set value and
  `/<path>` fell through to the short repository addresses. It now reads the
  existing value first (`choose_admin_path`).
- `--update` and re-runs keep an existing `ADMIN_PATH` unchanged, even when
  the settings name another one (the summary says so), and never add one to
  an instance that does not have it. `instance.json` never holds it.
- An existing instance: add one line to `/etc/forkmesh-instance/app.env` and
  restart the app (`systemctl restart forkmesh-instance`):

  ```
  ADMIN_PATH=/manage-x7k2q9
  ```

  Use your own random value, for example
  `printf '/admin-%s\n' "$(head -c 64 /dev/urandom | base64 | tr -dc a-z0-9 | head -c 20)"`.
  The dashboard's Admin console link picks it up at the next sign-in or
  session refresh.

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
With `ACTIONS_BOT_SEED` in its environment (or the app env) it also creates
the Actions bot `<owner>-actions` (a node account holding the Ed25519 key
derived from the seed, organization role `maintainer`), and
`python -m forkmesh_host instance-runner --name N --public-key K --owner O`
registers the instance's own trusted pool runner; see
[server/instance-actions.md](server/instance-actions.md). The Actions tab of
a public repository lists its runs to everyone (workflow, event, branch,
commit, actor, status, times); logs, live log tails and the Run, Cancel and
Re-run controls stay with the owner and organization writers, and a private
repository's runs stay hidden ("Public run summary" there).
Admins can serve one owner's repositories at `/<repo>/<folder>` from Platform
→ Repository addresses ([app/server-update.md](app/server-update.md)).

## The platform source mirror (`<owner>/forkmesh`)

`<owner>/forkmesh` (catalog owner `forkmesh`, the instance's git node) is a
read-only mirror of the upstream repository, https://forkmesh.com/forkmesh/forkmesh
by default (the installer's `platformUrl`, else `sourceUrl` unless that is a
repository on the instance itself, with its `app.` host prefix and `.git`
dropped). Nothing typed on the instance can change it: changes go
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
- The mirror's description is the repository's own About text: the git node
  publishes `about` (else `description`) from `.forkmesh/info.json` at the
  served HEAD as the catalog `description` (`catalogDescription` in
  `server/catalog.go`, whitespace collapsed, at most 240 characters). It used
  to publish an empty string, so the card read "No description published."
  This is a `server/` change: it needs a rebuilt node binary. Until then, and
  for any mirror without an About file, the card and the About rail fall back
  to "Read-only mirror of <upstream>" (`repoDescriptionText`). The fallback is
  drawn by the page: the relay never edits a signed catalog field.
  Tests: `TestCatalogDescriptionComesFromTheRepositoryAbout`,
  `test_a_mirror_without_a_published_description_names_its_upstream`.
- A node never labels a repository as a mirror of a page on its own relay
  (`mirrorOfFor` checks `sameRelay`), so a forkmesh.com fleet node that copied
  an instance config cannot mark the canonical repository read-only there.
- The app refuses writes with HTTP 403 `read_only_mirror` (`file-edit`,
  `branches/ops`, `branches/delete`, `pulls`, `pulls/<n>/merge|update`), and a `git push` gets
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
**Single-organization instances** (opt in, off by default). With
`"singleOrganization": true` in the install settings (only with `ownerType`
`org`), the bootstrap writes the `repo_paths` row
`{"canonical":true,"owner":"<org>","short":true}` when it creates the
organization, so every repository of the instance shows only as
`https://<domain>/<repo>`, the platform mirror included
(`https://<domain>/forkmesh`), and the installer reports that address. The
row is written with `ON CONFLICT DO NOTHING` and only when the organization
is new, so a re-run never overrides an admin's choice, and `--update` never
runs the bootstrap. Without the setting nothing changes. An installed
instance is converted with `python -m forkmesh_host instance-single-org --org
<org>` (idempotent, `--off` to switch back); the full command and the rules
are in [app/server-update.md](app/server-update.md).

**Web ports and ufw.** When ufw is active, the installer opens port 80 or
443 only if no ufw rule mentions that port at all. A port the operator has
narrowed, for example `ufw allow proto tcp from <cidr> to any port 80,443`,
is left exactly as it is on every `--update`. With `"webFirewall":
"cloudflare"` in `/etc/forkmesh-instance/instance.json` the installer keeps
80 and 443 open to Cloudflare's published address ranges only: it allows
the current ranges, removes the ones Cloudflare retired and then the plain
`80/tcp` and `443/tcp` allow rules, and refreshes the list weekly
(`forkmesh-web-firewall.timer`). Nothing changes when the list cannot be
fetched. SSH is never touched. The instance must then be reached through
Cloudflare's proxy. Switching back to `"open"` removes the timer but leaves
the narrow rules for the operator to remove.

**Local Caddy and runner firewall changes.** `--update` rewrites
`/etc/caddy/Caddyfile` and the runner's `/etc/forkmesh-runner/jobs.nft`.
Keep local changes in the files they include, which the installers never
touch: `/etc/caddy/conf.d/*.caddy` (top level), `/etc/caddy/site.d/*.caddy`
(directives inside the instance's site block) and
`/etc/forkmesh-runner/jobs.d/*.nft` (for example `add rule inet
forkmesh_runner output ...`).

## Importing a repository on the server (`instance_repo.py`)

`python -m forkmesh_host instance-repo` creates a hosted repository on the
instance and fills it from a git repository or bundle already on the server.
No browser session, password or push credential is involved: the operator's
root shell is the authority. Run it as root from the source checkout:

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

- **Fork link.** `--fork-of <owner>/<repo>` records the repository as a fork
  of a public repository on the instance exactly as the web Fork action does:
  `forkedFrom` in the catalog record (the parent's public identity: an
  organization alias stays the organization, as `_public_repo_owner` keys
  fork edges) and the `repo_forks` edge (`_index_repo_fork_edge`), so the
  page shows "forked from" and the parent's fork count includes it. The
  parent must exist and be public, must not be the repository itself, and
  must share history: its published `commit` or `rootCommit` has to be in the
  source (checked before anything is created when `--from` is a directory)
  and in the staged repository before its objects or refs are migrated. Later runs without `--fork-of` keep the link;
  a rerun with it reports `"forkedFrom": "unchanged"`. Objects are not shared
  with the parent.
- **Switches.** `--protect-default-branch` / `--no-protect-default-branch`
  and `--actions on|off` set the repository's feature settings before the
  fill and are audited as `repository.protection_settings`; see
  [app/branch-protection.md](app/branch-protection.md).

- **Counts and activity.** The fill writes the default branch's whole
  `commitCount` (`git rev-list --count`), `mergeCount`, and the 52-week
  `activityWeeks` / `mergeActivityWeeks` series that the dashboard cards
  draw, bucketed the same way as `server/catalog.go`. The total used to be
  `rev-list --count --max-count=10000`, so every large import (a 45,000-commit
  repository, the 62,000-commit platform source) showed 10,000 and an empty
  activity chart. The 10,000 cap still applies to the per-ref counts of a
  push report. A rerun that moves no refs still refreshes the counts
  (`hosted_git.record_hosted_stats`), so running the command again repairs a
  repository imported before this change.

- **Source.** `--from` is a repository directory or a `git bundle` file. Only
  local `refs/heads/*` and `refs/tags/*` are imported, so make the source with
  `git clone --mirror <url> /root/<repo>.git` (a normal clone has one local
  branch; the rest are remote-tracking refs and are skipped). Pull-request
  refs (`refs/pull/*`) are skipped too.
- **Who does what.** As root, the database work runs in a child process that
  drops to `forkmesh-app` (`--app-user`), so the SQLite files keep their
  owner, and every git command runs as `forkmesh-node` (`--git-user`), the
  owner of `/var/lib/forkmesh/hosted` (`--hosted-root`). The source is first
  written as a bundle into `/var/lib/forkmesh/tmp` and handed to
  `forkmesh-node`, which may not be able to read the operator's folder. That
  bundle holds only what the hosted repository lacks: every hosted head and
  tag the source also has is passed to `git bundle create` as an exclusion,
  so re-running after one new commit bundles one commit. Run as another
  user, everything happens as that user (tests, development).
- **Create.** Same records as the create form for an organization
  repository, through the helpers `_create_core_repository` now shares in
  `hosted_git.py` (`core_repository_target`, `core_catalog_record`,
  `store_core_repository`): the catalog record (`source: hosted-web`,
  `cloneUrl` `<PUBLIC_BASE_URL>/<org>/<repo>`), the `hosted_repos` row with
  creator = owner, the `org_repos` link, cache and catalog revision bumps and
  the `repository.create_hosted` audit row with the owner as actor. The owner
  must be a user account with maintain permission in the organization; no
  first commit is written and the per-account hosted quota is not checked
  (the repository still counts towards it).
- **Fill.** The command creates `<root>/<id>.git` itself when the git node has
  not yet (`git init --bare --initial-branch=main`, mode 0700, then a rename,
  exactly like `server/hosted.go`). It then works in a staging repository, a
  quarantine like the one `git receive-pack` uses: a bare repository in a
  scratch folder (`/var/lib/forkmesh/tmp/instance-repo-*`, else beside the
  hosted root) whose `objects/info/alternates` names the hosted objects. Every
  head and tag is fetched there; a fetch from a directory negotiates with the
  hosted tips, a bundle is indexed whole. The ref plan (fast-forward check)
  runs in the staging repository too. Only then is one pack built with
  `git pack-objects --revs --local` from the new tips minus the hosted tips,
  so a full bundle of a repository the node already holds adds only the
  missing commits (a first fill moves the staged packs as they are). The size
  check adds that pack to the hosted repository's reachable size
  (`git rev-list --all --objects --disk-usage`, which counts each object
  once whatever duplicate packs hold it). A refused ref or a repository that
  would pass `HOSTED_REPO_MAX_BYTES` raises before anything reaches the
  hosted repository, and the scratch folder is removed with
  `forkmesh_safe_delete.remove_tree` either way. On success the pack is
  renamed into `objects/pack` (`.pack`, `.rev`, then `.idx`, the order git
  writes them) and the refs are applied in one `update-ref` transaction:
  new refs are created, branches move only by fast-forward and tags never
  move, unless `--force`. Nothing is ever deleted. A failed `update-ref`
  after the pack moved leaves an unreferenced pack, which the node's
  maintenance keeps (no prune); a rerun reuses it. The fill touches
  `forkmesh-maintenance-due` so the git node compacts the repository next
  (see [HOSTED_REPOS.md](HOSTED_REPOS.md#maintenance)). HEAD points at `main`,
  else `master`, `trunk`, `develop`, else the first branch.

  Before 2026-10-17 the fill fetched straight into the hosted repository under
  `refs/forkmesh/import/*`. As root the source was always a full bundle, and
  a bundle carries no negotiation, so every run wrote the whole history again
  as a new pack. On the Bolt instance a rerun after one commit added a second
  392 MB pack; the size check (directory bytes) then counted both, refused
  the run at 1.2 GB, and left the duplicate pack behind.
- **Record.** As after an accepted web push: `hosted_repos.pushed_at` /
  `pushed_by` (so the node does not fetch over it), the routing pin computed
  from the filled repository with the same canonical form as
  `routing_refs_sha256` (default branch plus tags) written through
  `_pin_hosted_state` (catalog `stateHash`, `lastSync`, `branch`, `commit`,
  `branchCount`, `commitCount`, `rootCommit`, `sizeBytes`, state history),
  and `core_push.record_accepted_push` with the ref updates (default branch
  first, 40 listed): one `git.push` audit row per ref, and, unless
  `--no-hooks`, the push webhooks, code-push ping and Actions queue. An
  import that creates the default branch queues one Actions push job, for
  that branch only (the rule is in
  [server/instance-actions.md](server/instance-actions.md#queue-lease-report));
  before this rule an import queued one job per branch.
- **Idempotent.** A second run finds the `hosted_repos` row and only moves
  refs; with nothing new it records nothing (`refsUpdated: 0`). A repository
  that exists under another organization, or a catalog name a node
  published, is refused.
- **Serving.** The git node learns of a new repository on its next replica
  exchange (at most two minutes; `systemctl restart forkmesh-mirror-node`
  makes it immediate) and then serves it at `/<owner>/<repo>` and
  `/<org>/<repo>`. After updating an existing repository, the gateway may
  answer from its in-memory response cache until the node rewrites the
  repository's gateway entry on that same exchange; restart the node to
  serve the new refs at once.
- **Size.** The installer sets `HOSTED_REPO_MAX_BYTES=1073741824` (1 GiB) in
  `app.env` and `hosted.maxRepositoryBytes` to the same in
  `/etc/forkmesh/mirror-node.json`. The command refuses a fill whose
  reachable objects plus the new pack would pass the app limit; web pushes
  are refused past it too (largest copy the node reports, measured as
  `git count-objects` loose plus pack bytes after the node's maintenance). For a bigger repository raise
  `HOSTED_REPO_MAX_BYTES` in `app.env` (and the node's value, which bounds
  imports and replica fetches) and restart `forkmesh-instance`; the next
  run of the command reads the new value. Later pushes over HTTPS are also
  bounded per request by `FORKMESH_MAX_GIT_PUSH_BODY_BYTES` (256 MiB), which
  incremental pushes do not reach.

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
  The logo always lives at `/instance-logo.<png|jpg|webp|svg>`, and the web
  manifest (`/favicon/site.webmanifest`) lists it, so the phone apps can find
  it on an instance whose `/api/instance` has no `logo` field yet. An SVG
  logo also gets a 512 px `/instance-logo.png`, drawn with `rsvg-convert`
  (the installer installs `librsvg2-bin`), listed first in the manifest; when
  it exists `INSTANCE_LOGO` names the PNG, because phones and most mail
  clients cannot draw SVG. Web pages keep the SVG.
- **Theme** (`app/src/instance_theme.py`): one `instance_settings` row
  (`k = 'theme'`) holding `{"dark": {...}, "light": {...}}` of known token
  names to `#rrggbb` colours, max 8 KB, no raw CSS. Served as
  `/instance-theme.css`; set at setup from a preset name or a full colour
  object (the start page's picker), or later with `PUT /api/instance/theme`
  by an admin. The installer passes an object to `instance-bootstrap` as
  JSON and refuses one with other modes, odd token names or non-hex values.
- **Plugins** (`app/src/instance_features.py`): ForkMesh-company features
  (world, hackathon, ads, rewards, blog, status, developer docs, website
  pages and others) are off on an instance unless turned on. A disabled
  plugin's routes answer 404 before any other route; tagged UI
  (`data-instance-plugin`) is hidden. See [Plugins](#plugins) below.
- **Market packages:** every account on an instance has the
  pre-marketplace packages (repository tabs, Chat, Network, Agents, ...)
  installed until it removes one itself, because an instance has no Market
  onboarding; see "Account state" in
  [marketplace/core-plugins.md](marketplace/core-plugins.md). Nothing is
  stored, so existing accounts get them as soon as the app restarts with the
  change.
- **Pages** (`app/src/instance_pages.py`): an instance renders its own
  `/desktop` and `/docs` guide pages.
- **Legal pages** (`app/src/instance_legal.py`): Terms, Privacy and Security
  are separate settings, all off on a new instance: the links are hidden
  everywhere (sidebar, header menu, footers, the sign-up agreement), `/terms`,
  `/privacy` and `/security-report` answer the instance's 404 page, and there
  is no `/.well-known/security.txt`. An admin turns each one on from Platform
  → Legal pages, as Markdown served at `/terms`, `/privacy` or `/security`
  (sanitized by the host's README renderer) or as a link to another site;
  Security on also publishes `security.txt`. `GET /api/instance` reports them
  as `legal`. The optional install setting `legalPages` turns pages on at
  setup:

  ```json
  "legalPages": {
    "terms": {"markdown": "# Terms of use\n\n..."},
    "privacy": "https://example.org/privacy",
    "security": {"url": "https://example.org/security", "contact": "security@example.org"}
  }
  ```

  Each page takes `markdown` or `url` (a string is a URL), `null`/`false` or
  nothing for off; only `security` takes `contact` (email or https). The
  bootstrap writes it once (`ON CONFLICT DO NOTHING`); `--update` ignores it
  and never turns a page on. forkmesh.com keeps its own pages and links.
  Details: [app/instance-legal-pages.md](app/instance-legal-pages.md).

## Plugins

An admin turns optional plugins on and off from the admin console, Platform →
Plugins (`app/src/instance_console.py`). The choice applies without a
reinstall or restart.

| Plugin | On an instance |
|---|---|
| `world` | The instance's own World at `/world` with the Office and its chat. With a World listing ([Directory](#directory)) a portal joins it to the ForkMesh World. |
| `guestbook` | The agent guestbook in the instance's chat. |
| `genie` | The tip spider at the bottom of each page. |
| `developer-docs` | The dashboard's Developer docs, read from the instance's read-only `forkmesh` mirror. |

Everything else (`hackathon`, `challenges`, `pitch-deck`, `ads`, `patreon`,
`discord`, `microjobs`, `rewards`, `referrals`, `forkbot`, `ecosystem`,
`blog`, `status`, `ios-testers`, `website`, `instance-launcher`) needs
forkmesh.com's own
accounts, ledger, keys or content. The page lists them under "forkmesh.com
services" with the reason (`FORKMESH_ONLY`) and offers no switch;
`/api/instance` reports each plugin's `scope` (`instance` or `forkmesh`) and
`note`. An operator who really wants one can still name it in
`INSTANCE_PLUGINS`; a saved choice keeps it as it is.

- **Where the choice lives.** Saving writes the `instance_settings` row
  `plugins` (`{"enabled": [...]}`), CSRF-checked and audited as
  `admin.console_save_plugins` with the plugins turned on and off. Until the
  first save, `INSTANCE_PLUGINS` (the install setting `plugins`) decides;
  once the row exists it wins. The installer never writes the row and
  `--update` only rewrites `INSTANCE_PLUGINS`, so an update keeps the
  admin's choice.
- **How it applies.** Every request on an instance passes
  `Default._instance_plugins_load` before the plugin gate. It reads the row
  at most every 30 seconds (`CACHE_MS`) into a process cache that
  `enabled_ids` uses; a failed read keeps the last value. The saving process
  updates its cache at once, so the admin sees the result on the next page;
  `/api/instance` (cached 60 s) and the brand script follow within a minute.
- forkmesh.com never reads the row: every plugin stays on there, and the page
  says so instead of showing the form.
- **The most specific prefix wins.** `feature_for` picks the plugin with the
  longest matching path prefix, so `/api/world/store` and `/api/world/wallet`
  belong to `rewards` and `/api/world/community-ads` to `ads` even while
  `world` (which owns `/api/world`) is on. Before, the first listed plugin
  won and turning the World on also opened those forkmesh.com-only routes.

### The World on an instance

The World client lives in `world/public/world`, not in `app/public`, and
forkmesh.com serves it from a separate `world/dist`. The instance build
therefore runs `app/tools/build_site_assets.py instance-world` after
`forkmesh_host build` and before `apply_instance_brand.py`
(`build_app` in the installer). It builds `world/dist` as for
world.forkmesh.com (bundled scene, minified and versioned modules,
three.js r184 vendored under `/world/vendor/`) and copies it into
`app/dist`: all of `world/`, plus the World-only files the app bundle lacks
(bot avatars, marketplace World packages, `security/latest.json`). It never
overwrites an app file and drops the World's `_headers`. The World
document's canonical link to forkmesh.com is removed. The result is about
10 MB on disk (6 MB of it source maps); the build needs the same esbuild the
Office build already installs. The target is not part of a plain
`build_site_assets.py` run, so forkmesh.com's app bundle is unchanged.

The World is always bundled; the plugin decides at request time. The
installer sets `WORLD_ORIGIN` to the instance's own origin, so
`external_site_delivery` answers `local` and `_serve_world_asset`
(`app/src/app_runtime.py`) serves `/world` and `/world/` from
`world/index.html` and every other `/world/...` path from the exact bundled
file (`static_routes.world_asset_path`; dot segments and unknown files are
404). Headers:

- `content-type` from the file (`text/javascript` for modules, `image/webp`
  and so on).
- `cache-control`: `public, max-age=31536000, immutable` for a `?v=` URL and
  for `/world/vendor/`; `no-store, max-age=0, must-revalidate` for the
  document and any unversioned file, as on world.forkmesh.com, so a rebuild
  never mixes module graphs.
- The World CSP (`WORLD_DOCUMENT_SECURITY_HEADERS`): `script-src 'self'
  'unsafe-inline' https://static.cloudflareinsights.com` with no CDN, since
  three.js is local. The Cloudflare origin is the Web Analytics beacon that
  Cloudflare injects into pages of a proxied instance; the site-wide CSP
  already allows it, and without it every World load logged a CSP error.
  `connect-src 'self'
  https: wss:` for the API and the `/api/world/ws` presence socket; frames
  `'self'` for the embedded chat; `frame-ancestors 'none'` (nothing frames
  the World on an instance, and the global HTML layer sets
  `X-Frame-Options: DENY`).

With the plugin off, the plugin gate answers 404 for `/world`, `/world/*`,
`/api/world/*` and `/office` before any of this runs.

Every API the World calls is relative to the page, and `api-client.js` keeps
`location.origin` on any host that is not forkmesh.com, so presence, chat,
the Office, plots, notes and preferences reach the instance's own app and
accounts. The World's server code calls no forkmesh.com address (satellites
read celestrak.org, social boards x.com and reddit.com), so the egress guard
needs no new allowance.

Branding: the brand tool renames the World's title ("Acme Code World") and
adds the brand script, which renames "ForkMesh" in the World's HTML. Text
the scene paints on canvases (signs such as "FORKMESH OFFICE") is renamed by
`world/public/world/world-brand.js`: on a page that carries the brand script
it wraps `fillText`, `strokeText` and `measureText` of the 2D canvas
contexts so measured and painted text match. It leaves "ForkMesh World" (the
portal home), "ForkMesh Desktop", `forkmesh.com` addresses, `@forkmesh`
handles and lower-case repository names alone. The logo comes from
`/assets/logo.svg`, which the brand tool replaces with the instance logo.

What does not work on an instance: the rewards and ledger parts (World
store, wallet, central fund, pending rewards), community ads, the ForkMesh
blog board, the `forkmesh/forkmesh` mirror and agent-bot boards (they read
that repository, which an instance does not have under that name), Discord,
and the relay federation towers. The World shows those boards empty.

The World asks `/api/instance` once (`instanceInfo()` in `world.js`) and
does not request what the instance has switched off, so a visitor's console
stays clean: with `rewards` off it skips `/api/accounts/central-fund` and
`/api/rewards/pending`; with `ads` off the community placement and
`/api/ads/serve`; and on any instance (`branded: true`) the forkmesh.com
directory `/api/start/instances`, which only forkmesh.com fills (only the
home portal is drawn). `instancePluginOff` and `instanceKeepsDirectory` in
`world-instance-portals.js` decide. `/api/accounts/central-fund` belongs to
the `rewards` plugin on the server too, so it answers 404 rather than 503.
The one error left for a signed-out visitor is the 401 from
`/api/accounts/sessions`: the session cookie is HttpOnly, so asking is the
only way to learn there is none, as on forkmesh.com.

The World never changes the page address by itself: it opens repositories,
issues and other sites with `window.open(..., "_blank")` and travels only
when a visitor clicks a portal or the site menu. An embedded browser that
loads `_blank` windows in the same tab, or a tool that drives the active tab
of a shared browser, can look like the World navigating away on its own; a
plain browser left on `/world/` stays there.

### Platform name

The instance name replaces "ForkMesh" wherever the platform itself is meant.
Three layers do it, so that nothing depends on the browser alone:

- **Server.** `instance_features.display_name(env)` is the name (`ForkMesh`
  without `INSTANCE_NAME`) and `instance_features.rename(env, text)` /
  `rename_html` swap it into a string; on forkmesh.com both return the text
  unchanged. They name the repository page's `og:site_name`, `og:title`,
  `twitter:title` and default description, the profile page `<title>`, the
  repository fediverse page, the repository card image's default line, every
  email subject and body (`_send_email`; the "Powered by ForkMesh" footer is
  added after the rename), the hosted-repository limit messages, the
  "unavailable" pages, and the plain-text Git errors (`_git_error_as_text`
  gets the env; on an instance it drops the forkmesh.com help link). The
  founders' day-one feedback email (from founders@forkmesh.com) is not sent
  on an instance.
- **The git node account.** The instance's git node (`INSTANCE_FLAGSHIP_NODE`,
  `forkmesh`) has authored merge and metadata commits, and it owns the platform
  mirror internally. `/api/instance`
  reports it as `gitNode`; the dashboard shows it as the instance name
  wherever a commit author is shown (latest-commit line, history, commit
  detail, contributors, Insights) through `shownAccountName()`, the Actions
  snapshot shows it as the instance name when it is a run's actor, and
  repository cards drop "published from <node>" on an instance.
- **Build.** `apply_instance_brand.py` renames `<title>`, the
  application-name and `og:site_name` metas, and now the platform word inside
  `description`, `og:title`, `og:description`, `twitter:title` and
  `twitter:description`.
- **Dashboard.** `platformName()` in `02-helpers.js` reads the brand tag's
  `data-instance-name` (default `ForkMesh`). Every platform reference in a
  dashboard string goes through it, for example the repository Insights
  panel "<name> traffic", "Could not reach <name>", hosting and import
  notices, webhook and mirror help, the command palette, the profile link
  title and the account-search placeholders. Static partial text that sits
  inside `main` on repository and profile pages (which `instance-brand.js`
  deliberately leaves alone so READMEs and user text are never rewritten) is
  wrapped in `<span data-platform-name>`, which `applyPlatformName()` fills
  at boot. `instance-brand.js` still renames chrome and the other pages; its
  pattern skips identifiers (`X-ForkMesh-Ref`, `ForkMesh-Attestation`) and
  "ForkMesh, Inc.", and drops the word before "desktop".

Beyond the dashboard, a branded instance also names itself in: the `/docs`
and `/desktop` pages (which no longer say "ForkMesh desktop app" or
"forkmesh.com/desktop", and drop the Powered by footer when `poweredBy` is
off), the plugin-off page, the email unsubscribe pages, JSON error messages
(`message`, `hint`, `detail`, `title`, `reason` of any 4xx/5xx JSON answer,
renamed by `HostRuntime._instance_error_text`), the queued-agent notice,
nodeinfo `nodeName`, the instance ActivityPub actor's name, the default
profile and repository actor summaries, the fediverse daily digest, and the
`User-Agent` of requests other servers see (webhook deliveries, fediverse
fetches, workflow HTTP steps and imports, provider mirrors and repository
metadata: `<name-slug>-<kind>/1.0 (+<origin>)`, from
`instance_features.remember`/`user_agent`). The web manifest takes the name
even without a logo. The build-time brand tool renames the platform word in
all page text and shown attributes outside `<script>`/`<style>`, so a page
reads right before `instance-brand.js` runs. "ForkMesh desktop app" reads
"desktop app" on an instance (`desktopBrand()` in the dashboard,
`data-desktop-brand` spans in partials, and both brand passes). The start
page that launches new instances is the `instance-launcher` plugin, off on an
instance.

What stays on purpose: the software's own identity where it is a fact, not
branding ("a ForkMesh node", "runs a ForkMesh version", "other ForkMesh
instances", "a checkout of ForkMesh itself", nodeinfo `software.name`
`forkmesh` and its repository); technical names never shown as prose
(`.forkmesh/` like `.github/`, `forkmesh-actions`, `FORKMESH_*` variables,
the `forkmesh` MCP server and its config path, header and trailer names, the
`forkmesh-profile=` DNS verification record, service and file names); the
`<owner>/forkmesh` platform-source mirror, its description and its "Mirror of
forkmesh.com/forkmesh/forkmesh" label; the upstream installer command
`curl -fsSL https://forkmesh.com/install.sh | bash` on `/desktop` and in
Settings > Nodes; license and credit text, the optional Powered by badge and
email footer; and the strings of ForkMesh-only plugins (ads, hackathon,
links, referrals, rewards, Patreon membership, the Market, developer docs,
the instance launcher), which an instance does not show.
`app/tests/test_instance_platform_name.py` fails on any other platform
literal in dashboard JS (its `KEPT` list is the allow-list) and on any
"forkmesh", in any case, in the shown text of the branded static pages (its
`PAGE_KEPT` and `GATED_PAGES` lists are the allow-list).

### What an instance hides

Besides plugin routes and `data-instance-plugin` UI: the profile Tips card
(`rewards`), the profile "Referred users" panel (`referrals`), the profile
footer's "© ForkMesh, Inc." line and the "Open the dashboard from
forkmesh.com" setting (`website`), and the Patreon connection and
membership cards in Settings (`patreon`). The dashboard does not request the
ads slot (`/api/ads/serve`) or the blog feed (`/rss.xml`) unless
`instancePluginEnabled()` says the plugin is on, so a switched-off plugin no
longer costs a 404 on every page. The availability-proof links to
forkmesh.com docs on the Mirrors tab are left out, and the repository Watch
handle is the instance's own repository actor (`GET .../about`
`fediverse.handle`) instead of `@forkmesh.forkmesh@forkmesh.com`.

## Dashboard on an instance

The dashboard knows it runs on an instance from the
`<script data-instance-brand>` tag that `apply_instance_brand.py` writes into
every page (`dashboardOnInstance()` in `02-helpers.js`). forkmesh.com pages
never carry it.

- **Reconnect node.** On forkmesh.com the header shows "Reconnect node" to a
  signed-in account with no linked node, because nothing else drains its web
  writes. On an instance the instance's own git node serves every repository,
  so a web-only account (no linked node) is never told to reconnect, and the
  Settings > Nodes "No node is linked" callout stays hidden. The header only
  shows "Desktop offline" when the account has linked a node of its own and
  that node's public profile (`GET /api/accounts/<node>`, from its heartbeat)
  says it is offline; the title names the node. An unknown presence stays
  quiet.
- **Served by.** On forkmesh.com a repository can be answered by any of
  several mirrors, so the header pill ("served by <node>", "served by
  mirror", "mirror online/offline"), the repository card's mirror label, the
  home list's serving status and the About rail's "Direct access" mirror list
  say which one answered and where else to go; that is real routing
  information there, so they stay. On an instance one node serves
  everything, so those elements are not rendered (the header pill gets
  `hidden`) and the card reads "online"/"offline" instead of "via mirror".
  The Mirrors tab stays as the full diagnostic view. The "Powered by
  ForkMesh" badge is not a served-by label and stays (see the top of this
  page).

## Directory

After a successful launch, `instance_directory.publish` writes an
`instance_directory` row keyed by domain (name, URL, logo, choices, World
slot). An instance installed directly with the installer joins the same
directory from its admin console (Platform → ForkMesh directory) or with the
install setting `"directory": {"listed": true, "world": true}`: it publishes
`/.well-known/forkmesh-instance.json` and sends a signed announcement that
forkmesh.com checks against that document before writing the row, and it
announces again every six hours; the protocol is in
[app/instance-federation.md](app/instance-federation.md). The World has 12
instance slots. Cron phase `instance-health` probes
`https://<domain>/readyz`; two misses in a row hide the entry, a day offline
pings the admins once per outage.

The forkmesh.com homepage's "Federated instances" section
(`www/templates/pages/index.html`) reads `/api/start/instances` without
credentials and shows each listed instance's logo, name, address and status:
"Online" when its last `/readyz` check answered, "Checking" before the first
check or after one miss (each entry carries `status` and `checkedAt`;
instances that missed two checks are not in the list at all). The list always
starts with a fixed OWASP BLT card (its logo, `www/public/assets/owasp-blt-logo.svg`,
copied from the instance's `/instance-logo.svg`) linking to
`https://git.owaspblt.org/BLT`; a directory entry on `git.owaspblt.org` is
skipped so it does not show twice (the host is the `data-instances-pinned`
value). It always ends with an "Add your instance" box to `/start`, a dashed
card rather than a button because the page's green call to action is its
sign-up button; it replaced the "Start an instance" button and the "No
instance has joined yet." line in October 2026. On forkmesh.com the section
always shows; on an instance (whose own directory is empty) the section and
the add box stay hidden. Tests:
`app/tests/test_homepage_federated_instances.py`. The admin console's Instances view can
hide, show or delete entries (CSRF-protected and audited); deleting does not
touch the instance.

## Commit identities

Commits the git node and the instance runner make themselves (pull merges and
their metadata commits, mirror intake, the issue reconciler, agent workspaces)
are authored as the instance name with `noreply@<domain>`: the installer sets
`FORKMESH_COMMIT_NAME` and `FORKMESH_COMMIT_EMAIL` on
`forkmesh-mirror-node.service` and in a
`forkmesh-runner@<owner>.service.d/identity.conf` drop-in, and
`server/commit_identity.go` reads them (without them: the node name and
`<node>@users.noreply.forkmesh.com`, as before). Pull-rule and sync-status
hints name the platform the same way. The node and runner run the signed
release binary, so this takes effect with the first release that contains it.
Web edits and hosted pushes were already authored by the signed-in account
with `<account>@users.noreply.<host>`, and Actions commit-back by
`<owner>-actions`.

## Mail (`app/src/mail_providers.py`)

`MAIL_PROVIDER` selects `mailtrap`, `sendgrid`, `mailgun` or `none`;
otherwise SMTP or Mailtrap by configuration. `MAIL_SENDER` and
`MAIL_SENDER_NAME` set the sender. Budgets, suppression and the mail log apply
to every provider.

## Limits

- Public sign-up needs Turnstile keys in `app.env`; it fails closed without
  them.
- Turnstile tokens must name the instance's own host. The installer writes
  `TURNSTILE_HOSTNAMES=<domain>` (an existing value is kept), and without it a
  branded instance accepts the host of `APP_ORIGIN`. Before this, an instance
  fell back to forkmesh.com's hostnames and refused every sign-up and password
  reset check with `hostname_mismatch`.
- Without a fingerprint the first host key is trusted (and shown).
- SSH credentials pass through the app once, in memory only.
- The instance app's egress guard refuses forkmesh.com except
  `app.forkmesh.com` (`FORKMESH_EGRESS_PRODUCTION_ALLOW` in the app unit),
  which only the directory announcement uses.
- Server text that is not listed under "Platform name" (admin-only
  operational alerts, the plugin descriptions on the admin console's
  Plugins page, Solana/ledger notices of switched-off plugins, the referral
  preview page) still says ForkMesh.
- The repository card image keeps the ForkMesh mark in its corner.
- Commit identities, the runner name and the Go node's own messages change
  only with a signed release that contains them (see "Commit identities").

## Tests

`app/tests/test_instance_launch.py`, `test_install_instance_script.py`,
`test_install_instance_staged_build.py` (staged build, rollback, source
switch, git node program),
`test_start_instance_page.py`, `test_instance_white_label.py`,
`test_instance_platform_name.py`, `test_git_error_text.py`,
`test_instance_theme.py`, `test_instance_pages.py`,
`test_instance_plugins.py`, `test_instance_registry.py`,
`test_instance_legal_pages.py`, `test_server_update.py`,
`test_instance_directory.py`, `test_instance_git_routing.py`,
`test_instance_node_reconnect_frontend.py`,
`test_instance_served_by_frontend.py`, `test_repo_canonical_addresses.py`,
`test_single_org_contributors.py`, and
`app/host/tests_unit/test_ssh_helper.py`, `test_instance_bootstrap.py`,
`test_instance_repo.py`, and `app/tests/host/test_instance_repo_e2e.py`
(a served app with a temporary SQLite database: a repository created from
the web and one from the command list the same way on the dashboard).
