# Update ForkMesh and repository addresses (admin console)

Part of [the App docs](README.md). Admin console pages under Platform,
rendered by `app/src/server_update_admin.py` (views `update`, `repo-addresses`
and `legal-pages`, dispatched from the admin route in `app/src/app_runtime.py`).
Legal pages are in [instance-legal-pages.md](instance-legal-pages.md). Every
change is CSRF-checked and written to the sensitive audit log as
`admin.console_<action>`. Running your own instance is covered in
[self-hosting](../self-hosting.md) and
[instances](../operations/instances.md).

The pages live at `<ADMIN_PATH>?view=update` and so on. With a private
`ADMIN_PATH` every console page (theme switch, Acquisition and Hackathon
included) is served only under that path and `/admin/...` answers the normal
404; links and form actions come from `admin_shell.subpage_base(env)` and
`console_base(env)`, never a fixed `/admin`. Setting it on a live instance,
the installer's `adminPath` setting, and what stays at `/api/admin/...` are in
[instances](../operations/instances.md#private-admin-console-path).

## Update ForkMesh (`?view=update`)

One **Version** card shows the running build, the newest commit on the
followed source (only when it differs), when that was last checked, and an
"Up to date" or "Update available" pill. Below it: the **Update** card (an
**Update ForkMesh** button behind a confirmation tick) and, on instances, an
**Automatic updates** card. While an update runs, the Update card shows the
old and new commit once the new one is known.

`server_update.freshness` says "Update available" when the newest commit is
not the running one, unless it is older than the running build's commit time:
a check made before the last deploy names an older commit, and the running
build is newer, not behind. A newer check replaces it.

**Running build.** `server_update.running` reads `BUILD_INFO_FILE` (JSON:
`version`, `commit`, `commitTime`, `subject`, `builtAt`) and falls back to
`BUILD_REV` and `APP_VERSION`. The version is the one in
`desktop/CMakeLists.txt`, the number releases use.

- Instances: the installer's `write_build_info` writes
  `/opt/forkmesh-instance/build-info.json` on every build.
- Cores: `tools/next_deploy.py` stages `BUILD_INFO.json` in each release and
  points `FORKMESH_VAR_BUILD_INFO_FILE` at `current/BUILD_INFO.json` through
  `release.env`.

The page has two modes (`server_update.mode`).

### Instances

The app runs sandboxed as `forkmesh-app` and cannot rebuild itself, so it
only drops a request file; a root unit does the work.

1. **Check now** or **Update ForkMesh** writes `check` or `update` to
   `INSTANCE_UPDATE_REQUEST` (`/var/lib/forkmesh-instance/update/request`,
   atomically; the path must be under `/var/lib/forkmesh-instance/`). A
   merged pull request writes `auto` (below).
2. `forkmesh-instance-update.path` (`PathExists=`) starts
   `forkmesh-instance-update.service`, a root oneshot running
   `/usr/local/lib/forkmesh-instance-update/instance-update.sh`
   (`tools/instance-update.sh`).
3. The helper reads at most 32 bytes of the request, keeps only `a-z`,
   refuses a symlink, and deletes the file. Nothing from the request reaches a
   command.
4. **check** fetches the configured `sourceRef` from the configured
   `sourceUrl` (depth 1; not the checkout's `origin`, so editing
   `instance.json` is enough) and records the fetched commit, date, subject,
   version and source. A `sourceUrl` on the instance's own `domain` is
   fetched through the local Caddy (`http.curloptResolve`). Only `https://`
   (and `file:///` for tests) is accepted.
5. **update** refuses while `forkmesh-instance-setup.service` runs, fetches
   the latest source, extracts that commit's
   `www/public/install-instance.sh` and runs it with `--update`, so installer
   fixes ship with the update. Like a manual `--update`, it rebuilds,
   migrates and restarts only what changed and keeps secrets, data and
   accounts. It builds beside the running version, switches only after a
   good build and rolls back if the restart fails, so a failed update leaves
   the old version serving
   ([instances](../operations/instances.md#updates-build-beside-the-running-version)).
   **auto** is the same run with `trigger: merge`, except it ends at once
   (`done`, nothing restarted) when the fetched commit is already running
   (for example two coalesced merges).
6. Progress goes to `/var/lib/forkmesh-instance-update/status.json`
   (root:forkmesh-app, 0640): state, `trigger` (`admin` or `merge`), the
   installer's current `FMI` step, the last `Error:` line, the commits before
   and after, and the upstream commit. The full log, `update.log` in the same
   folder, is root-only: installer output is not meant for the web.

While a request or run is open the page refreshes every 5 seconds and hides
the buttons; it may fail to load for a moment while the app restarts.

The Version card also warns when `build-info.json` says the git node program
was built from older `server/` code than the running source
(`daemon.state` = `stale`); see
[instances](../operations/instances.md#the-git-node-program).

### Update when a pull request is merged

A switch in the **Automatic updates** card (instances only) turns automatic
updates on and off; its description names the repository and branch
(`save_auto_update`, audited as `admin.console_save_auto_update`). The setting
is the `self_update` row of `instance_settings` (`{"auto": true}`);
`app/src/self_update.py` holds the rules.

- **Which repository.** The one the running build came from:
  `build-info.json` `source` and `ref` (rewritten on every installer run,
  even when the commit is already built, so switching `sourceUrl` takes
  effect after one update). The source must be
  `<PUBLIC_BASE_URL>/<owner>/<repo>[.git]`, or `/<repo>[.git]` with short
  addresses on; anything else (the ForkMesh release, another host) cannot be
  turned on (`auto_no_target`). Repositories are compared by storage owner,
  so the organization alias and creator address match; a fork never does.
- **Which event.** Only a successful web merge
  (`repository_pull_merge_handler`, the `merge` mutation, after
  `repo_merge_jobs` moves to `succeeded`, once per merge) whose pull request
  base (`pull.md` `base`) is the source branch. Pushes, "update branch" (it
  moves the pull request's own branch), agent merges, branch operations and
  operator imports never trigger it.
- **Only a protected branch.** The installer runs as root from whatever the
  source branch holds, so whoever can merge into it can deploy code to the
  server. The branch must be protected
  ([branch-protection.md](branch-protection.md): merged only after a
  non-author owner or admin approves, never pushed to directly): turning the
  card on is refused otherwise (`auto_unprotected`), and a merge arriving
  after protection was switched off, or while the setting cannot be read,
  files nothing (outcome `unprotected`, audited as refused). Protection also
  stops a direct push riding along with the next merge, since the update
  fetches the branch tip, not the merged commit.
- **What it does.** Files the button's request with action `auto`. The
  request is one file: a waiting `update` or `auto` absorbs the merge
  (`queued`); a waiting `check` is replaced by `auto`, so a check never
  swallows a merge's update; during a run, the new file makes the path unit
  run exactly one more update afterwards, which fetches the latest commit, so
  any number of merges during a run cost one extra update. The oneshot never
  runs twice at once.
- **Audit and display.** Each trigger writes `admin.auto_update` (pull
  number, branch, merged commit, outcome) and the `self_update_last` row,
  which the card shows ("Last merge: pull request #12 by alice, Update
  requested"). The update's progress shows "Started by: Merged pull request"
  and the commits it moved between.

The installer writes the units in its `services` step, so an older instance
gets the button after one manual `bash install-instance.sh --update`; until
then the page says so.

### Cores (forkmesh.com)

`FORKMESH_VAR_CORE_ADMIN_UPDATES=1` (rendered when `[self_deploy] enabled`
and `admin_updates` are both true in `deploy/fleet.toml`) turns the button
on. It inserts a `deploy_approvals` row with `kind = 'admin'`,
`target = 'main'` and empty commits; one admin request may be open at a time.
`deploy/core/self_deploy.py` pins it to the current `main` head and deploys it
like a phone-approved merge ([../deploy/self-deploy.md](../operations/self-deploy.md)).
The page lists the last five admin requests with each core's progress.

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

- `_route` tries the short form only after every other route, just before
  the `/<owner>/<repo>` page route, so site pages, assets and API paths never
  change meaning.
- A first segment that is reserved (`RESERVED_ROUTE_PREFIXES`), looks like a
  file (`.php`, `.txt`, `.json` and similar), or names an existing account or
  organization keeps its normal meaning. The full `/<owner>/<repo>` address
  keeps working.
- Git paths are re-dispatched through `_route` with the rewritten path, so
  organization aliases and push rules apply unchanged.
- The repository page is served at the short address with two meta tags
  (`forkmesh-repo-short`, `forkmesh-repo-owner`), placed right after
  `<head>` so the early-read script (`partials/repo-early-reads.html`), which
  runs inside the head, maps the short path too (placed later, every
  short-address tab fired two 404 reads for `/api/repo/<repo>/<tab>/...`).
  `repoRouteParts` in `02-helpers.js` maps the short path back to owner,
  repository, tab and folder. Links in the page keep the full address unless
  the owner is canonical (below). Short-address pages skip the shared social
  edge cache because the page differs.
- The setting is cached 30 seconds per process (`repo_short_urls.cached`); a
  save clears the local cache.
- On the cores Caddy serves static files before the app, so short addresses
  are meant for instances.

### Single-organization instances (canonical short addresses)

The second switch on the Short addresses card, "Show repositories without
the owner (single-organization instance)", stores `"canonical": true` in the
same row (`{"canonical":true,"owner":"acme-bolt","short":true}`); it counts
only while `short` is on. That owner's repositories then show only as
`/<repo>`: `https://code.example.org/Bolt`, `/Bolt/issues`,
`/Bolt/tree/bolt-c`, clone `https://code.example.org/Bolt.git`.

Organization repositories are stored under the creating account and linked
in `org_repos` (Bolt is hosted as `drew/Bolt`, aliased as `acme-bolt/Bolt`);
the creator address is treated like the organization address.

| Request | Answer |
| --- | --- |
| `GET`/`HEAD /<owner>/<repo>[/tab/...]` | `308` to `/<repo>[/...]`, query kept, `cache-control: no-store` so turning the switch off takes effect at once |
| `GET /<creator>/<repo>[/...]`, the repository linked to the owner (the creating account, or the `forkmesh` node for the platform mirror) | the same `308`, public repositories only (a redirect for a private one would reveal it exists) |
| `/<owner>/<repo>/info/refs`, `git-upload-pack`, `git-receive-pack`, `/api/repo/...` | unchanged at both addresses; git and the API never redirect |
| a name that is reserved, looks like a file, ends in `.git`, equals the owner, or is a person's account or an organization | keeps `/<owner>/<repo>` (the short rewrite refuses those names) |
| a name that is only an internal node account (`forkmesh`, `<org>-actions`) | goes to the root: internal accounts have no address of their own, so the platform mirror is `/forkmesh` |

Where the short form is produced:

- **Server redirect**: `_repo_canonical_path` in `entry.py`, called from
  `_route` just before the `/<owner>/<repo>` page, after every git and API
  route. `repo_short_urls.canonical_redirect` holds the pure rule: the short
  path must rewrite back to exactly the requested full path.
- **Head tags**: a page served at the short address with canonical on gets
  `<link rel="canonical">` and `og:url` of `https://<host>/<repo>` and a
  `forkmesh-repo-canonical` meta tag naming the owner.
- **Browser**: dashboard pages are static files, so the setting arrives
  through `GET /api/instance` (`repoAddresses`: owner, canonical, the owner's
  public repository names that must keep the owner, public creator aliases
  `{repo: creator}`, and the reserved route names). Boot (`10-boot.js`)
  starts the read at once and waits for it (at most 1.5 s) when no answer is
  cached or the cached one is older than ten minutes
  (`REPO_ADDRESSES_TTL_MS`); the answer is kept in `localStorage`
  (`forkmesh.repoAddresses`, saved time in `forkmesh.repoAddressesSavedAt`)
  so later pages render short links at once. A cached answer can be wrong
  right after an admin switches the mode, so when the fresh answer differs,
  `applyRepoAddresses` fires `forkmesh:repo-addresses` and boot runs
  `refreshRepoAddressViews`: repository lists, sidebar and header are
  redrawn, every same-site link is rewritten (`refreshRepoLinks`, either
  direction) and a long address in the address bar is replaced with
  `history.replaceState` (`replaceLongRepoLocation`). The core hosts never
  ask. `repoPathUrl`, and through it `repoLinkUrl`, `groupLinkUrl`, record
  and compare links, `navigateHistory` targets and the repository lists,
  return `/<repo>/...` for the owner, its creator aliases and rows whose
  public owner is the organization. `cloneUrl` adds `.git` to a one-segment
  address. `repoPublicHref`/`repoPublicKey` shorten notification links and
  labels. The "forked from" line (`repoProvenanceHtml`) names the parent
  through `repoDisplayName`, not the stored `forkedFrom` key (test:
  `test_single_org_fork_line_names_the_parent_by_its_short_address`). Short
  paths the page made are remembered so Back/Forward parse them again
  (`repoRouteParts`). `parseCloneUrlIdentity` reads a one-segment clone URL
  of this site as `<owner>/<repo>`.
- **Clone URLs in API answers**: the hosted-create and push-credential
  answers (`remoteUrl` and the commands) use `_repo_clone_url`. Catalog rows
  keep their stored, signed `cloneUrl` (the server reads identities from
  it); `GET /api/repositories` and `/api/repositories/search` answers are
  rewritten on the way out (`repo_short_urls.short_urls`): same-site URLs of
  the owner or a linked creator become `/<repo>` (`/<repo>.git` for
  `cloneUrl`, `clone.https` and `git clone` commands); mirror URLs on other
  hosts and names that must keep the owner are left alone.
- **Notifications**: `enqueue_notification` stores the short `href`.
- **Git over the short address**: `/<repo>.git/info/refs` and
  `/<repo>.git/git-receive-pack` are re-dispatched as the full path. The
  request still carries the short URL, so organization-alias recovery
  (`_git_push_alias_owner`, `_https_mirror_proxy`) maps it back through
  `_git_request_path`. The instance host's 256 MiB push body limit
  (`request_guard.GIT_PUSH_RE`) also matches the one-segment push path.

Root server pages (`/admin`, `/health`, `/readyz`, `/oauth`, `/developers`,
`/mcp`, `/nodeinfo`, `/ap`, `/v1`, `/ws` and the rest of
`static_routes.CORE_RESERVED_ROUTE_PREFIXES`) are reserved, so a one-segment
address never turns them into a repository page (unlisted, `/admin` rendered
an empty repository "admin" and the console was unreachable). A repository
with a reserved name keeps its `/<owner>/<repo>` address.

Short addresses also skip the dashboard page names (`repos`, `settings`,
`agents`, `runners`, ... from `DASHBOARD_PAGE_ASSETS`), `profile`, `explore`
and `forgot` (`static_routes.SHORT_NAME_RESERVED`, sent to the browser as
`reserved`). These are reserved only for short addresses, not owner names, so
an account with one of these names keeps its `/<name>/<repo>` pages.

Page addresses kept out of the short-repository router and the JSON 404:

- `/dashboard/profile/<name>` and `/profile/<name>` (browsers cached an old
  308) 308 to `/@<name>` (`profile_page_redirect`), and
  `dashboard_repo_redirect` never turns `/dashboard/<page>/...` into a
  repository path (so `/profile/<name>` is never read as the repository
  `profile`).
- `/<name>` of an organization or an active, non-node account 302s to
  `/@<name>` (`_account_page_redirect`).
- `/explore` 308s to `/dashboard/repos`, `/forgot` to `/forgot-password`
  (`LEGACY_PAGE_PATHS`).
- `/<owner>/<repo>/commit/<sha>` and `/<repo>/commit/<sha>` 308 to
  `.../commits/<sha>` (`REPO_ROUTE_ALIASES`, `repo_short_urls.alias_redirect`).
  The repository page opens that commit from the URL, and clicking a commit
  pushes the same shareable URL.
- A short address the visitor cannot read (missing, or private to someone
  else) gets the repository page saying "Repository not found" with status
  404. The check is `_repository_access_context`, so a reader of a private
  repository gets 200 and nobody else learns it exists.
- An unowned route answers the styled 404 page to a browser page load
  (`_is_page_navigation`) and JSON `{"error": "not_found"}` to API and other
  clients.

Internal accounts are node accounts (`_account_kind` = `node`): the git node
`forkmesh` and the Actions bot `<org>-actions`. They keep their organization
role (the bot stays a maintainer so workflows can push), but the roster
(`GET /api/orgs/<org>/members`) leaves them out while the mode is on
(`_org_internal_members`). The owners shown are the people configured at
install.

Signing up and contributing are unaffected: `/signup` is unchanged, and an
issue, pull request or discussion on an organization repository needs only a
signed event and the feature switch (`_repo_feature_off`), not membership
(`app/tests/test_single_org_contributors.py`).

The owner's repository names and creator links come from `org_repos` and the
owner's `repositories` rows, cached five minutes per process
(`_repo_short_links`; an admin save clears it). A new repository whose name
collides with an account can show a short link for up to that long; the
server redirect checks accounts on every request.

With canonical on, the repository page names only the repository:

- **Header**: `renderHeaderContext` (`04-account.js`) shows just the
  repository link when `repoLinkUrl` gives a one-segment address, instead of
  `acme-bolt / Bolt`.
- **`og:title`, `twitter:title`** and the fallback `og:description` read
  `Bolt - ForkMesh` (`_serve_repo_page`, only at the canonical owner's short
  address).
- **Server-rendered first view** (`app/host/forkmesh_host/repo_ssr.py`): a
  one-segment GET (`short_target`) gets a `ShortTicket`; the owner is unknown
  until the app's page arrives, so `finish` reads the `forkmesh-repo-short` /
  `forkmesh-repo-owner` meta tags from it (`short_page`), then renders the
  remembered first view of `<owner>/<repo>` within the same budget as a full
  address. With `forkmesh-repo-canonical` present, the title is the
  repository alone and file, folder and README links are `/<repo>/...`. The
  early-read script still reads only two-segment paths, so on a short page
  the dashboard reads the tree itself at boot.

Labels follow the same rule. `repoDisplayName(repo)` (`02-helpers.js`)
returns only the name for a repository with a short address (the owner, a
creator alias, or a row whose public owner is the organization) and
`owner/name` otherwise; a string key goes through `repoPublicKey`.
`groupDisplayName(group)` does the same for a mirror group,
`repoReferenceLabel` gives `Repo#12`, and `repoPublicText` shortens
`owner/name` inside server-written text (`Bounty funded on acme/Repo` becomes
`Bounty funded on Repo`). They cover the sidebar, home list, repositories
page, global search, profile activity, activity feed, notifications, agent,
task and note lists, the repository page's loading header and not-found
message, pull request compose and detail labels, contribution tips, commit
detail, copy-path menu, code map and agent repository picker. The sidebar and
home list deduplicate on that label, so a repository stored as `alice/Repo`
and linked as `acme/Repo` appears once, as `Repo`. A "published from" note
that would name an internal node account (the platform mirror under
`forkmesh`) is dropped. The browser title of a repository page becomes
`<repo> - <instance>` (`renderHeaderContext`, restored on leaving). The
network page's largest-repositories board names a short repository by name
(`_short_repo_board` in `entry.py`). Without canonical every label keeps its
`owner/name` form, passed as the fallback by each call site.

Still shown with the owner: ActivityPub actor URLs and the fediverse profile,
mirror and runner clone URLs on other hosts, and the materialized public
catalog snapshot on the cores (instances do not use it).

New instances opt in with `"singleOrganization": true` in the install
settings (default `false`, organization owners only); see
[instances](../operations/instances.md). For an existing instance, run the operator
command as the app user from the instance's source:

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
the root, and is safe to rerun (`"changed": false`). `--off` sets `canonical`
back to `false`. No catalog row is rewritten: stored `cloneUrl` values are
signed identity data, so the short form is applied to answers instead. The
admin page (Platform → Repository addresses, both switches, owner = the
organization) does the same without the alias repair, or write the row
directly:

```sql
INSERT INTO instance_settings (k, doc, updated_at)
VALUES ('repo_paths', '{"canonical":true,"owner":"acme-bolt","short":true}',
        CAST(strftime('%s','now') AS INTEGER) * 1000)
ON CONFLICT(k) DO UPDATE SET doc=excluded.doc, updated_at=excluded.updated_at;
```

A row written by SQL takes effect within 30 seconds (the per-process cache).

### Project addresses

A repository with one project per folder (a monorepo) can give each folder
its own address under the short repository address and send old per-project
repository addresses there. Needs short addresses on.

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
one tab-separated line per folder: folder, address name, comma-separated old
names; `#` lines are comments. A folder of `-` marks retired names, which
redirect to the repository. The table can live in the repository (Bolt keeps
it in `scripts/routing/projects.tsv`) and be pasted here.
`app/src/repo_projects.py` holds the rules. Project redirects are checked
before the short route and land on `/<repo>/...`, which canonical mode never
redirects again.

- Names match case-insensitively, and the folder name works as well as the
  address name. Unknown names fall through to the short-address rules.
- Redirects answer 302 with `cache-control: no-store`, because the table can
  change.
- Targets are addresses the repository page understands. It reads three
  query parameters once on open: `ref` selects the branch, `path` narrows the
  History tab to a folder, `label` sets the Issues label filter.
- The `history` read takes an optional `path`: the gateway runs
  `git log ... -- <path>`, and the app does not cache path-filtered history.
  On a node the host's repository fast path forwards `path` for `history`
  too ([docs/host/README.md](../host/README.md));
  `test_repo_fastpath_history_path.py` reads a folder's history through the
  fast path and a real gateway.
- An old owner whose name is also the short-address owner keeps its real
  repository (`/<owner>/<repo>`); only names in the table redirect. Git
  paths (`.git`) never redirect.
- Each project's issues carry a label named after its folder.

## Tests

`app/tests/test_server_update.py`, `app/tests/test_repo_projects.py`
(project addresses), `app/tests/test_install_instance_script.py`
(updater units, build info), `deploy/tests/test_self_deploy.py` (admin
requests).
