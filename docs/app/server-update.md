# Update ForkMesh and repository addresses (admin console)

Two admin console pages under Platform, both rendered by
`app/src/server_update_admin.py` (views `update` and `repo-addresses`,
dispatched from the admin route in `app/src/app_runtime.py`). Every change is
CSRF-checked and written to the sensitive audit log as
`admin.console_<action>`.

## Update ForkMesh (`?view=update`)

The page shows the build this server runs next to the newest commit on the
source it follows, with a pill saying "Up to date" or "Update available", and
an **Update ForkMesh** button that needs a confirmation tick.

**Running build.** `server_update.running` reads `BUILD_INFO_FILE` (JSON:
`version`, `commit`, `commitTime`, `subject`, `builtAt`) and falls back to
`BUILD_REV` and `APP_VERSION`. The version is the one in
`desktop/CMakeLists.txt`, the same number the releases use.

- Instances: the installer's `write_build_info` writes
  `/opt/forkmesh-instance/build-info.json` on every build.
- Cores: `tools/next_deploy.py` stages `BUILD_INFO.json` in each release and
  points `FORKMESH_VAR_BUILD_INFO_FILE` at `current/BUILD_INFO.json` through
  `release.env`.

The page works in one of two modes (`server_update.mode`):

### Instances

The app runs sandboxed as `forkmesh-app` and cannot rebuild itself, so it
only drops a request file; a root unit does the work.

1. **Check now** or **Update ForkMesh** writes `check` or `update` to
   `INSTANCE_UPDATE_REQUEST` (`/var/lib/forkmesh-instance/update/request`,
   written atomically; the path must be under `/var/lib/forkmesh-instance/`).
2. `forkmesh-instance-update.path` (`PathExists=`) starts
   `forkmesh-instance-update.service`, a root oneshot running
   `/usr/local/lib/forkmesh-instance-update/instance-update.sh`
   (`tools/instance-update.sh`).
3. The helper reads at most 32 bytes of the request, keeps only `a-z`,
   refuses a symlink, and deletes the file. Nothing from the request reaches a
   command.
4. **check** fetches the configured `sourceRef` from `origin` (depth 1) and
   records the fetched commit, date, subject and version.
5. **update** refuses while `forkmesh-instance-setup.service` runs, fetches
   the latest source, extracts that commit's
   `www/public/install-instance.sh` and runs it with `--update`. The installer
   rebuilds, migrates and restarts only what changed, as a manual
   `--update` does, and keeps secrets, data and accounts. Running the new
   installer means installer fixes ship with the update.
6. Progress goes to `/var/lib/forkmesh-instance-update/status.json`
   (root:forkmesh-app, 0640): state, the installer's current `FMI` step, the
   last `Error:` line, the commits before and after, and the upstream commit.
   The full log is `update.log` in the same folder, readable by root only,
   because installer output is not meant for the web.

While a request or run is open the page refreshes every 5 seconds and hides
the buttons. The app restarts during the update, so the page may fail to load
for a moment.

The installer writes the units in its `services` step, so an instance set up
before this change gets the button after one manual
`bash install-instance.sh --update`. Until then the page says so.

### Cores (forkmesh.com)

`FORKMESH_VAR_CORE_ADMIN_UPDATES=1` (rendered when `[self_deploy] enabled`
and `admin_updates` are both true in `deploy/fleet.toml`) turns the button
on. It inserts a `deploy_approvals` row with `kind = 'admin'`,
`target = 'main'` and empty commits; only one admin request may be open at a
time. `deploy/core/self_deploy.py` pins the request to the current `main`
head and deploys it exactly like a phone-approved merge; see
[../deploy/self-deploy.md](../deploy/self-deploy.md). The page lists the last
five admin requests with each core's progress.

"Latest available" on the cores is the `core_upstream` row of
`instance_settings`, which each self-deploy agent rewrites when `main` moves.

## Repository addresses (`?view=repo-addresses`)

An instance with one main owner can drop the owner from repository
addresses:

| Address | Opens |
| --- | --- |
| `/<repo>` | `/<owner>/<repo>` |
| `/<repo>/<folder>/...` | `/<owner>/<repo>/tree/<folder>/...` |
| `/<repo>/issues`, `/pulls/3`, `/blob/...` | that repository tab or file |
| `/<repo>.git/info/refs`, `/<repo>/git-upload-pack` | Git over HTTPS (clone, fetch, push) |

The setting is the `repo_paths` row of `instance_settings`
(`{"short": true, "owner": "acme"}`); the owner must be an existing account
or organization. `app/src/repo_short_urls.py` holds the rules.

- `_route` tries the short form only after every other route has had the
  path, just before the `/<owner>/<repo>` page route, so site pages, assets
  and API paths never change meaning.
- A first segment that is reserved (`RESERVED_ROUTE_PREFIXES`), looks like a
  file (`.php`, `.txt`, `.json` and similar), or names an existing account
  or organization keeps its normal meaning. The full
  `/<owner>/<repo>` address keeps working.
- Git paths are re-dispatched through `_route` with the rewritten path, so
  organization aliases and push rules apply unchanged.
- The repository page is served at the short address with two meta tags
  (`forkmesh-repo-short`, `forkmesh-repo-owner`). `repoRouteParts` in
  `02-helpers.js` maps the short path back to owner, repository, tab and
  folder. Links inside the page keep the full address. Short-address pages
  skip the shared social edge cache because the page differs.
- The setting is cached for 30 seconds per process
  (`repo_short_urls.cached`); a save clears the local cache.
- On the cores Caddy serves static files before the app, so short addresses
  are meant for instances.

### Project addresses

A repository that keeps one project per folder (a monorepo) can give each
folder its own address under the short repository address, and send old
per-project repository addresses there. It needs short addresses on.

| Address | Redirects to |
| --- | --- |
| `/<repo>/<Project>` | `/<repo>/tree/<folder>` |
| `/<repo>/<Project>/blob/<ref>/<path>`, `/tree/<ref>/<path>` | `/<repo>/blob/<folder>/<path>?ref=<ref>` |
| `/<repo>/<Project>/raw/<ref>/<path>` | `/api/repo/<owner>/<repo>/raw?path=<folder>/<path>&ref=<ref>` |
| `/<repo>/<Project>/commits[/<ref>]` | `/<repo>/commits?path=<folder>[&ref=<ref>]` |
| `/<repo>/<Project>/issues` | `/<repo>/issues?label=<folder>` |
| `/<repo>/<Project>/pulls` and other tabs | that tab of the repository |
| `/<repo>/commit/<sha>` | `/<repo>/commits?ref=<sha>` |
| `/<old owner>/<old name>/...` | `/<repo>/<Project>/...` |
| a retired name | `/<repo>` |

The setting is the `repo_projects` row of `instance_settings`:
`{"repo": "Bolt", "table": "<tsv>", "legacy": ["acme-bolt"]}`. The table has
one line per folder: folder, address name and old names separated by commas,
tab-separated; `#` lines are comments. A folder of `-` marks retired names,
which redirect to the repository. The same table can live in the repository
(Bolt keeps it in `scripts/routing/projects.tsv`) and be pasted here.
`app/src/repo_projects.py` holds the rules.

- Names match without regard to case, and the folder name works as well as
  the address name. Unknown names fall through to the short-address rules.
- The redirects run just before the short-address route and answer 302
  with `cache-control: no-store`, because the table can change.
- Redirects land on addresses the repository page already understands. The
  page reads three query parameters once when it opens: `ref` selects the
  branch, `path` narrows the History tab to a folder, and `label` sets the
  Issues label filter.
- The `history` read takes an optional `path`: the gateway runs
  `git log ... -- <path>`, and the app does not cache path-filtered history.
- An old owner whose name is also the short-address owner keeps its real
  repository (`/<owner>/<repo>`); only names in the table redirect. Git
  paths (`.git`) never redirect.
- Each project's issues carry a label named after its folder.

## Tests

`app/tests/test_server_update.py`, `app/tests/test_repo_projects.py`
(project addresses), `app/tests/test_install_instance_script.py`
(updater units, build info), `deploy/tests/test_self_deploy.py` (admin
requests).
