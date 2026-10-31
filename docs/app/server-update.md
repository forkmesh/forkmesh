# Update ForkMesh and repository addresses (admin console)

Admin console pages under Platform, rendered by
`app/src/server_update_admin.py` (views `update`, `repo-addresses` and
`legal-pages`, dispatched from the admin route in `app/src/app_runtime.py`).
Legal pages (Terms, Privacy and Security on an instance) are described in
[instance-legal-pages.md](instance-legal-pages.md). Every change is
CSRF-checked and written to the sensitive audit log as
`admin.console_<action>`.

The pages are `?view=` pages of the console, so they live at
`<ADMIN_PATH>?view=update` (and so on). On an instance with a private
`ADMIN_PATH` every console page, including the theme switch, Acquisition and
Hackathon, is served only under that path and `/admin/...` answers the normal
404 page; links and form actions inside these pages come from
`admin_shell.subpage_base(env)` and `console_base(env)`, never a fixed
`/admin`. Setting it on a live instance (one line in `app.env`, then
`systemctl restart forkmesh-instance`), the installer's `adminPath` setting,
and what stays at `/api/admin/...` are in
[../INSTANCES.md](../INSTANCES.md#private-admin-console-path).

## Update ForkMesh (`?view=update`)

The page shows one **Version** card: the build this server runs, the newest
commit on the source it follows (only when it differs), when that was last
checked, and a pill saying "Up to date" or "Update available". Under it are
the **Update** card, with an **Update ForkMesh** button that needs a
confirmation tick, and on instances one **Automatic updates** card. While an
update runs, the Update card shows the old and new commit only once the new
one is known, so it no longer repeats the running build.

`server_update.freshness` says "Update available" when the newest commit is
not the running one, unless that commit is older than the running build's
commit time: a check made before the last deploy names an older commit, and
the build running here is newer, not behind. A newer check replaces it.

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
   A merged pull request writes `auto` (below).
2. `forkmesh-instance-update.path` (`PathExists=`) starts
   `forkmesh-instance-update.service`, a root oneshot running
   `/usr/local/lib/forkmesh-instance-update/instance-update.sh`
   (`tools/instance-update.sh`).
3. The helper reads at most 32 bytes of the request, keeps only `a-z`,
   refuses a symlink, and deletes the file. Nothing from the request reaches a
   command.
4. **check** fetches the configured `sourceRef` from the configured
   `sourceUrl` (depth 1; not from the checkout's `origin`, so editing
   `instance.json` is enough) and records the fetched commit, date, subject,
   version and source. A `sourceUrl` on the instance's own `domain` is
   fetched through the local Caddy (`http.curloptResolve`). Only `https://`
   (and `file:///` for tests) addresses are accepted.
5. **update** refuses while `forkmesh-instance-setup.service` runs, fetches
   the latest source, extracts that commit's
   `www/public/install-instance.sh` and runs it with `--update`. The installer
   rebuilds, migrates and restarts only what changed, as a manual
   `--update` does, and keeps secrets, data and accounts. Running the new
   installer means installer fixes ship with the update. The installer builds
   beside the running version and switches only after a good build, rolling
   back if the restart fails, so a failed update leaves the old version
   serving ([../INSTANCES.md](../INSTANCES.md#updates-build-beside-the-running-version)).
   **auto** is the same run with `trigger: merge` recorded, except that it
   ends at once (`done`, nothing restarted) when the fetched commit is the
   one already running, for example when two merges were coalesced.
6. Progress goes to `/var/lib/forkmesh-instance-update/status.json`
   (root:forkmesh-app, 0640): state, `trigger` (`admin` or `merge`), the
   installer's current `FMI` step, the last `Error:` line, the commits before
   and after, and the upstream commit.
   The full log is `update.log` in the same folder, readable by root only,
   because installer output is not meant for the web.

While a request or run is open the page refreshes every 5 seconds and hides
the buttons. The app restarts during the update, so the page may fail to load
for a moment.

The Version card also warns when `build-info.json` says the git node
program was built from older `server/` code than the running source
(`daemon.state` = `stale`); the installer keeps the installed program on an
instance that follows its own source, see
[../INSTANCES.md](../INSTANCES.md#the-git-node-program).

### Update when a pull request is merged

A switch in the **Automatic updates** card (instances only) turns automatic
updates on and off; its one-line description names the repository and branch
(`save_auto_update`, CSRF-checked, audited as
`admin.console_save_auto_update`). The setting is the `self_update` row of
`instance_settings` (`{"auto": true}`); `app/src/self_update.py` holds the
rules.

- **Which repository.** The one the running build came from:
  `build-info.json` `source` and `ref` (rewritten on every installer run,
  also when the commit is already built, so switching `sourceUrl` to the
  running commit's repository takes effect after one update). The source must be
  `<PUBLIC_BASE_URL>/<owner>/<repo>[.git]`, or `/<repo>[.git]` when short
  addresses are on. Anything else (the ForkMesh release, another host) cannot
  be turned on (`auto_no_target`). The repository is compared by its storage
  owner, so the organization alias and the creator address are the same
  repository; a fork is a different repository and never matches.
- **Which event.** Only a successful web merge (`repository_pull_merge_handler`,
  the `merge` mutation, after `repo_merge_jobs` moved to `succeeded`, which
  happens once per merge) whose pull request base (`pull.md` `base`) is the
  source branch. Pushes, the "update branch" mutation (it moves the pull
  request's own branch), agent merges, branch operations and operator imports
  never trigger it; on a protected branch the merge is the only way in.
- **Only a protected branch.** The installer runs as root from whatever the
  source branch holds, so whoever can merge into it can deploy code to this
  server. The source branch must be protected
  ([branch-protection.md](branch-protection.md): merged only after an owner
  or admin who is not the author approves, never pushed to directly):
  turning the card on is refused otherwise (`auto_unprotected`), and a merge
  arriving after protection was switched off, or while the setting cannot be
  read, files nothing (outcome `unprotected`, audited as refused). Protection
  is also what keeps a direct push from riding along with the next merge,
  since the update fetches the branch tip, not the merged commit.
- **What it does.** The same request the button files, with action `auto`.
  The request is one file: if an `update` or `auto` is already waiting, the
  merge joins it (`queued`); a waiting `check` is replaced by `auto`, so a
  check never swallows a merge's update; if an update is running, the new file makes the path unit run
  exactly one more update when it finishes, and that run fetches the latest
  commit, so any number of merges during a run cost one extra update. The
  oneshot service never runs twice at once.
- **Audit and display.** Each trigger writes `admin.auto_update` (pull
  number, branch, merged commit, outcome) and the `self_update_last` row,
  which the card shows ("Last merge: pull request #12 by alice, Update
  requested"). The update's own progress shows "Started by: Merged pull
  request" and the commits it moved between.

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

The newest commit on the cores is the `core_upstream` row of
`instance_settings`, which each self-deploy agent rewrites when `main` moves.

## Repository addresses (`?view=repo-addresses`)

An instance with one main owner can drop the owner from repository
addresses:

| Address | Opens |
| --- | --- |
| `/<repo>` | `/<owner>/<repo>` |
| `/<repo>/<folder>/...` | `/<owner>/<repo>/tree/<folder>/...` |
| `/<repo>/issues`, `/pull/3`, `/blob/...` | that repository tab or file |
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
  folder. The tags go right after `<head>` so the early-read script
  (`partials/repo-early-reads.html`), which runs inside the head, maps the
  short path the same way; before October 2026 they sat before `</head>` and
  every short-address tab page fired two 404 reads for
  `/api/repo/<repo>/<tab>/...`. Links inside the page keep the full address
  unless the owner is canonical (below). Short-address pages
  skip the shared social edge cache because the page differs.
- The setting is cached for 30 seconds per process
  (`repo_short_urls.cached`); a save clears the local cache.
- On the cores Caddy serves static files before the app, so short addresses
  are meant for instances.

### Single-organization instances (canonical short addresses)

The second switch on the Short addresses card, "Show repositories without
the owner (single-organization instance)", stores `"canonical": true` in the
same row: `{"canonical":true,"owner":"acme-bolt","short":true}`. It only
counts while `short` is on. The repositories of that owner then show only as
`/<repo>`: `https://code.example.org/Bolt`, `/Bolt/issues`,
`/Bolt/tree/bolt-c`, clone `https://code.example.org/Bolt.git`.

Organization repositories are stored under the account that created them
and linked to the organization in `org_repos` (Bolt is hosted as
`drew/Bolt` and aliased as `acme-bolt/Bolt`). That creator address is
treated like the organization address.

| Request | Answer |
| --- | --- |
| `GET`/`HEAD /<owner>/<repo>[/tab/...]` | `308` to `/<repo>[/...]`, query kept, `cache-control: no-store` so turning the switch off takes effect at once |
| `GET /<creator>/<repo>[/...]`, the repository linked to the owner (the creating account, or the `forkmesh` node for the platform mirror) | the same `308`, for a public repository only (a redirect for a private one would tell a visitor that it exists) |
| `/<owner>/<repo>/info/refs`, `git-upload-pack`, `git-receive-pack`, `/api/repo/...` | unchanged at both addresses; git and the API are never redirected |
| a name that is reserved, looks like a file, ends in `.git`, equals the owner, or is a person's account or an organization | keeps `/<owner>/<repo>` (the short rewrite refuses those names, so the full address is the only one that works) |
| a name that is only an internal node account (`forkmesh`, `<org>-actions`) | goes to the root: in this mode internal accounts have no address of their own, so the instance's platform mirror is `/forkmesh` |

Where the short form is produced:

- **Server redirect**: `_repo_canonical_path` in `entry.py` (called from
  `_route` just before the `/<owner>/<repo>` page is served, after every git
  and API route, so those never redirect). `repo_short_urls.canonical_redirect`
  holds the pure rule: the short path must rewrite back to exactly the
  requested full path.
- **Head tags**: a page served at the short address with canonical on gets
  `<link rel="canonical">` and `og:url` of `https://<host>/<repo>` and a
  `forkmesh-repo-canonical` meta tag naming the owner.
- **Browser**: the dashboard pages are static files, so the setting reaches
  them through `GET /api/instance` (`repoAddresses`: owner, canonical, the
  owner's public repository names that must keep the owner, public creator
  aliases `{repo: creator}`, and the reserved route names). Dashboard boot
  (`10-boot.js`) starts the read at once and waits for it (at most 1.5 s)
  when no answer is cached or the cached one is older than ten minutes
  (`REPO_ADDRESSES_TTL_MS`); the answer is kept in `localStorage`
  (`forkmesh.repoAddresses`, saved time in `forkmesh.repoAddressesSavedAt`)
  so later pages render short links straight away. A page drawn from a cached
  answer can be wrong right after an admin switches the mode, so when the
  fresh answer differs from the one the page already used,
  `applyRepoAddresses` fires `forkmesh:repo-addresses` and boot runs
  `refreshRepoAddressViews`: the repository lists and sidebar are drawn
  again, the header is redrawn, every same-site link is rewritten
  (`refreshRepoLinks`: long to short, or back to the long form when the mode
  was turned off) and a long address in the address bar is replaced with
  `history.replaceState` (`replaceLongRepoLocation`). An identical answer
  changes nothing.
  The core hosts never ask. `repoPathUrl`, and through it `repoLinkUrl`,
  `groupLinkUrl`, record and compare links, `navigateHistory` targets and the
  repository lists, return `/<repo>/...` for the owner, its creator aliases
  and rows whose public owner is the organization. `cloneUrl` adds `.git` to a
  one-segment address. `repoPublicHref`/`repoPublicKey` shorten notification
  links and labels. The "forked from" line (`repoProvenanceHtml`) names the parent
  through `repoDisplayName`; it printed the stored `forkedFrom` key, so a
  fork of the platform mirror read `<org>/forkmesh` while every other label
  read `forkmesh` (test:
  `test_single_org_fork_line_names_the_parent_by_its_short_address`). Short paths the page itself made are remembered so
  Back/Forward parse them again (`repoRouteParts`). `parseCloneUrlIdentity`
  reads a one-segment clone URL of this site as `<owner>/<repo>`.
- **Clone URLs in API answers**: the hosted-create and push-credential
  answers (`remoteUrl` and the commands) use `_repo_clone_url`. The catalog
  rows keep their stored, signed `cloneUrl` (the server reads identities from
  it), and `GET /api/repositories` and `/api/repositories/search` answers are
  rewritten on the way out (`repo_short_urls.short_urls`): same-site URLs of
  the owner or of a linked creator become `/<repo>` (`/<repo>.git` for
  `cloneUrl`, `clone.https` and `git clone` commands); mirror URLs on other
  hosts and names that must keep the owner are left alone.
- **Notifications**: `enqueue_notification` stores the short `href`.
- **Git over the short address**: `/<repo>.git/info/refs` and
  `/<repo>.git/git-receive-pack` are re-dispatched as the full path. The
  request object still carries the short URL, so the organization-alias
  recovery (`_git_push_alias_owner`, `_https_mirror_proxy`) maps it back
  through `_git_request_path`. The instance host's 256 MiB push body limit
  (`request_guard.GIT_PUSH_RE`) also matches the one-segment push path.

Server pages that live at the root (`/admin`, `/health`, `/readyz`,
`/oauth`, `/developers`, `/mcp`, `/nodeinfo`, `/ap`, `/v1`, `/ws` and the
others in `static_routes.CORE_RESERVED_ROUTE_PREFIXES`) are reserved, so a
one-segment address never turns them into a repository page. Before they were
listed, turning canonical mode on made `/admin` render an empty repository
page named "admin" and the admin console unreachable. A repository whose name
is reserved keeps its `/<owner>/<repo>` address.

Short addresses also skip the dashboard page names (`repos`, `settings`,
`agents`, `runners`, ... from `DASHBOARD_PAGE_ASSETS`), `profile`, and the
forge page names `explore` and `forgot`
(`static_routes.SHORT_NAME_RESERVED`, sent to the browser as `reserved`).
They are reserved only for short addresses, not as owner names, so an account
with one of these names keeps its `/<name>/<repo>` pages on the shared site.

Page addresses that used to fall into the short-repository router or the
JSON 404 (seen on an instance in October 2026):

- `/dashboard/profile/<name>` was bounced by `dashboard_repo_redirect` to
  `/profile/<name>`, which canonical mode read as the repository `profile`.
  `/dashboard/profile/<name>` and `/profile/<name>` (browsers cached the old
  308) now 308 to `/@<name>` (`profile_page_redirect`), and
  `dashboard_repo_redirect` never turns `/dashboard/<page>/...` into a
  repository path.
- `/<name>` of an organization or an active, non-node account 302s to its
  profile `/@<name>` (`_account_page_redirect`).
- `/explore` 308s to `/dashboard/repos` and `/forgot` to `/forgot-password`
  (`LEGACY_PAGE_PATHS`).
- `/<owner>/<repo>/commit/<sha>` and `/<repo>/commit/<sha>` 308 to
  `.../commits/<sha>` (`REPO_ROUTE_ALIASES`, `repo_short_urls.alias_redirect`).
  The repository page opens that commit's detail from the URL, and a click
  on a commit pushes the same URL so it can be shared.
- A short address whose repository the visitor cannot read (missing, or
  private to someone else) still gets the repository page, which says
  "Repository not found", but with status 404. The check is
  `_repository_access_context` for the visitor, so a reader of a private
  repository keeps status 200 and nobody else learns that it exists.
- A route nothing owns answers the styled 404 page to a browser page load
  (`_is_page_navigation`) and the JSON `{"error": "not_found"}` to API and
  other clients.

Internal accounts are node accounts (`_account_kind` = `node`): the git
node `forkmesh`, the Actions bot `<org>-actions`. They keep their
organization role (the bot stays a maintainer so workflows can push), but the
organization roster (`GET /api/orgs/<org>/members`) leaves them out while
the mode is on (`_org_internal_members`). The organization owners shown are
the people configured at install.

Signing up and contributing are not affected: `/signup` is unchanged, and
opening an issue, pull request or discussion on an organization repository
needs only a signed event and the feature switch (`_repo_feature_off`), not
organization membership (`app/tests/test_single_org_contributors.py`).

The owner's repository names and creator links are read from `org_repos`
and the owner's `repositories` rows and cached for five minutes per process
(`_repo_short_links`); a save on the admin page clears it. A new repository
whose name collides with an account can show a short link for up to that
long; the server redirect itself checks accounts on every request.

With canonical on, the repository page names only the repository:

- **Header**: `renderHeaderContext` (`04-account.js`) shows just the
  repository link when `repoLinkUrl` gives a one-segment address, instead of
  `acme-bolt / Bolt`.
- **`og:title`, `twitter:title`** and the fallback `og:description` read
  `Bolt - ForkMesh` (`_serve_repo_page`, only for a page served at the short
  address of the canonical owner).
- **Server-rendered first view** (`app/host/forkmesh_host/repo_ssr.py`): a
  one-segment GET (`short_target`) gets a `ShortTicket`; the owner is not
  known until the Worker's page arrives, so `finish` reads the
  `forkmesh-repo-short` / `forkmesh-repo-owner` meta tags from it
  (`short_page`), then renders the remembered first view of
  `<owner>/<repo>` within the same budget as a full address. With
  `forkmesh-repo-canonical` present, the title is the repository alone and
  the file, folder and README links are `/<repo>/...`. The early-read script
  still only reads two-segment paths, so on a short page the dashboard reads
  the tree itself when it boots.

Labels follow the same rule. `repoDisplayName(repo)` (`02-helpers.js`)
returns only the name for a repository that has a short address (the owner,
a creator alias, or a row whose public owner is the organization) and the
usual `owner/name` otherwise; a string key goes through `repoPublicKey`.
`groupDisplayName(group)` does the same for a mirror group,
`repoReferenceLabel` gives `Repo#12`, and `repoPublicText` shortens
`owner/name` inside server-written text (notification and activity titles,
for example `Bounty funded on acme/Repo` becomes `Bounty funded on Repo`).
They are used by the sidebar, the dashboard home list, the repositories
page, global search, profile activity, the activity feed, notifications,
the agent, task and note lists, the repository page's loading header and
its not-found message, the pull request compose and detail
labels, the contribution tips, the commit detail, the copy-path menu, the
code map and the agent repository picker. The sidebar and home list
deduplicate on that label, so a repository stored under its creator
(`alice/Repo`) and linked to the organization (`acme/Repo`) appears once,
as `Repo`. Where a label would name an internal node account (the platform
mirror under `forkmesh`), the "published from" note is dropped with the
owner. With canonical on the browser title of a repository page becomes
`<repo> - <instance>` (`renderHeaderContext`, restored when the page leaves
the repository). The network page's largest-repositories board names a
short repository by its name (`_short_repo_board` in `entry.py`). Without
canonical, every label keeps its previous form; the call sites that used
the physical `owner/name` pass it as the fallback.

Still shown with the owner: ActivityPub actor URLs and the fediverse
profile, mirror and runner clone URLs on other hosts, and the materialized
public catalog snapshot on the cores (instances do not use it).

New instances opt in with `"singleOrganization": true` in the install
settings (default `false`, organization owners only); see
[../INSTANCES.md](../INSTANCES.md). To turn it on for an existing instance,
run the operator command as the app user, from the instance's source:

```sh
cd /opt/forkmesh-instance/src/app/host && runuser -u forkmesh-app -- \
  env HOME=/var/lib/forkmesh-instance /opt/forkmesh-instance/venv/bin/python \
  -m forkmesh_host instance-single-org --org acme-bolt \
  --env-file /etc/forkmesh-instance/app.env \
  --db sqlite:/var/lib/forkmesh-instance/app.db \
  --data-dir /var/lib/forkmesh-instance/data
```

It writes the `repo_paths` row, links the `forkmesh` platform mirror to the
organization if that alias is missing, prints the repositories now served at
the root, and is safe to run again (`"changed": false`). `--off` sets
`canonical` back to `false`. No catalog row is rewritten: stored `cloneUrl`
values are signed identity data the server reads, so the short form is
applied to answers instead. The admin page (Platform → Repository addresses,
both switches, owner = the organization) does the same as the command
without the alias repair, or the row can be written directly:

```sql
INSERT INTO instance_settings (k, doc, updated_at)
VALUES ('repo_paths', '{"canonical":true,"owner":"acme-bolt","short":true}',
        CAST(strftime('%s','now') AS INTEGER) * 1000)
ON CONFLICT(k) DO UPDATE SET doc=excluded.doc, updated_at=excluded.updated_at;
```

A row written by SQL takes effect within 30 seconds (the per-process cache).

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
Project addresses already target `/<repo>/...`, so they work unchanged with
canonical short addresses: a project redirect is checked before the short
route and lands on an address that canonical mode never redirects again.

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
  On a node the host's repository fast path forwards `path` for `history`
  too (`docs/host/README.md`); `test_repo_fastpath_history_path.py` reads a
  folder's history through the fast path and a real gateway.
- An old owner whose name is also the short-address owner keeps its real
  repository (`/<owner>/<repo>`); only names in the table redirect. Git
  paths (`.git`) never redirect.
- Each project's issues carry a label named after its folder.

## Tests

`app/tests/test_server_update.py`, `app/tests/test_repo_projects.py`
(project addresses), `app/tests/test_install_instance_script.py`
(updater units, build info), `deploy/tests/test_self_deploy.py` (admin
requests).
