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
| `source` | shallow clone into `/opt/forkmesh-instance/src`; on `--update`, a shallow fetch of `sourceRef` from `sourceUrl` into that checkout (nothing is checked out yet) |
| `build` | venv and `python -m forkmesh_host build` without ForkMesh-only packages (`FORKMESH_EXCLUDED_PLUGINS`), then `app/tools/apply_instance_brand.py`; on a running instance this happens in `src.next` beside the live tree (below) |
| `configure` | `/etc/forkmesh-instance/app.env` with fresh `DATA_KEY`, `ADMIN_PASS` and router keys (kept on re-runs) and `instance.json` without secrets |
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
  --env-file /etc/forkmesh-instance/app.env \
  --db sqlite:/var/lib/forkmesh-instance/app.db \
  --data-dir /var/lib/forkmesh-instance/data
```

It prints one JSON line: `ok`, `owner`, `organization`, `name`, `id`,
`created`, `branches`, `tags`, `defaultBranch`, `commit`, `refsUpdated`,
`sizeBytes`, `stateHash`, `gitDir` (or `ok: false` and `error`, exit 1).

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
  pages and others) are off on an instance unless listed in
  `INSTANCE_PLUGINS`. A disabled plugin's routes answer 404 before any other
  route; tagged UI (`data-instance-plugin`) is hidden.
- **Market packages:** every account on an instance has the
  pre-marketplace packages (repository tabs, Chat, Network, Agents, ...)
  installed until it removes one itself, because an instance has no Market
  onboarding; see "Account state" in
  [marketplace/core-plugins.md](marketplace/core-plugins.md). Nothing is
  stored, so existing accounts get them as soon as the app restarts with the
  change.
- **Pages** (`app/src/instance_pages.py`): an instance renders its own
  `/desktop` and `/docs` guide pages.

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
`test_install_instance_staged_build.py` (staged build, rollback, source
switch, git node program),
`test_start_instance_page.py`, `test_instance_white_label.py`,
`test_instance_theme.py`, `test_instance_pages.py`, `test_server_update.py`,
`test_instance_directory.py`, `test_instance_git_routing.py`,
`test_instance_node_reconnect_frontend.py`,
`test_instance_served_by_frontend.py`, `test_repo_canonical_addresses.py`,
`test_single_org_contributors.py`, and
`app/host/tests_unit/test_ssh_helper.py`, `test_instance_bootstrap.py`,
`test_instance_repo.py`, and `app/tests/host/test_instance_repo_e2e.py`
(a served app with a temporary SQLite database: a repository created from
the web and one from the command list the same way on the dashboard).
