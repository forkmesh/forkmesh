# ForkMesh-hosted repositories on the Next core

A new user with no desktop node and no GitHub, GitLab or Codeberg account
can create a repository on the website and use it with plain `git` over
HTTPS. The three core nodes host it, and each node keeps a full replica.
Later the user can move the repository to their own node.

Code:

- App: `app/src/hosted_git.py` (create, push routing, quotas, private reads)
  and `app/src/hosted_repos.py` (replica exchange, templates, quotas).
- Database: migration `0199_hosted_repositories`.
- Daemon: `server/hosted.go` and `server/hosted_push_mirror.go` (push mirrors).
- Gateway: the `restricted` visibility in `tools/mirror_gateway.py`.
- Website: the **New repository** source on the create page,
  `/dashboard/repos/new` (`app/public/dashboard/partials/views/new-repo.html`,
  `app/public/dashboard/js/05-repo-list-explorer.js`).

## What a user gets

**Repositories → New repository** (`/dashboard/repos/new`):

| Field | Notes |
|---|---|
| Name | `[A-Za-z0-9._-]`, at most 64 characters. It may not start with `.` or end in `.git`. |
| Owner | You, or an organization where you hold **maintain** or better. |
| Description | Optional, at most 240 characters. |
| Visibility | **Public** means anyone can browse and clone it. **Private** means only you (and the organization's readers) can clone or read it. |
| Initialize with a README | On by default. |
| .gitignore | Python, Node, Go, Rust, Java, C / C++, or none. |
| License | MIT, BSD 3-Clause, ISC, Unlicense, or none. |

The **Remote clone URL** source also hosts a copy when the URL is a public
`https://` repository on `github.com`, `gitlab.com` or `codeberg.org`. The
list comes from `HOSTED_IMPORT_HOSTS`. Any other URL still leads to the
desktop handoff.

After creation the modal shows:

- The remote URL: `https://app.forkmesh.com/<owner>/<name>`, or
  `/<organization>/<name>` for an organization repository.
- The commands. For a repository with a first commit or an import, these are
  `git clone …`. For an empty one, they are `git remote add` and `git push`.
- A **username** (your account name) and a **password**. The password is the
  existing session-bound push credential (`fmp1.…`). It lasts 7 days, is
  never stored, and dies when the browser session is signed out. To get a new
  one, call `POST /api/hosted-repositories/push-credential`.

The same credential clones a private repository. ForkMesh has no
personal access tokens and no SSH access to hosted repositories.

**Quotas.** The app enforces these, and they are set in `app/wrangler.toml`
`[vars]`:

| Variable | Default |
|---|---|
| `HOSTED_REPO_MAX_PER_ACCOUNT` | 10 live hosted repositories per account. The count covers repositories the account created, organization ones included. |
| `HOSTED_REPO_MAX_BYTES` | 200 MiB per repository. The largest copy any core reports is compared with this limit. |

- At or over the size limit, a push answers `413 repository_quota_exceeded`.
  A single push with a `Content-Length` over the limit is refused the same
  way.
- A push body through the App may be up to 256 MiB. That limit is
  `FORKMESH_MAX_GIT_PUSH_BODY_BYTES` in `request_guard`, and Caddy uses the
  same number. The body is streamed and never buffered.
- The creating session must be verified. Every POST session check already
  refuses an unverified email (`email_not_verified`), and Turnstile guards
  signup.

**Git rules.** These are the gateway's receive-pack rules:

- Fast-forward only: no force pushes.
- No ref deletes.
- A new tag may be pushed; an existing tag never moves (the owner tag move in
  `docs/CORE_PUSH.md` is offered only on core-held repositories).
- Only `refs/heads/*` and `refs/tags/*` are accepted.
- Objects are fsck-checked.
- Hooks never run.
- Every accepted push is audited per ref (`git.push`, `docs/CORE_PUSH.md`).

## Design

```
browser ── POST /api/hosted-repositories ──► app (any core; rqlite)
                                             │ catalog record (source hosted-web)
                                             │ hosted_repos row (+ first-commit spec)
                                             └─ "hosted" node event ──► every core daemon
core daemon (each) ── POST /api/hosted-repositories/replicas (node-signed) ──► app
                  ◄── desired set: {id, owner, name, visibility, pushedAt,
                                    primary?, init?, sources[{node,url,grant}]}
    ├─ create <root>/<id>.git + gateway entry ("public" | "restricted")
    ├─ primary + empty: deterministic first commit, or a bounded import
    └─ behind / empty: git fetch <peer>/v1/runner/<owner>/<repo>.git
                       (router-signed read grant) → fast-forward only
git push ─► app (Basic credential) ─► the last pusher, else the rendezvous-first
            live core (router capability) ─► record pin + pushed_at ─► "hosted" event
git clone ─► app ─► public: any core whose signed refs proof matches the pin
                    private: the owner or an org reader, then a core via capability
```

### Replication: desired state plus a primary

The replicated source of truth is **rqlite**, which the app already uses
(Raft). It stores:

- `hosted_repos`: the repository, its encrypted spec, `pushed_at`, and
  `pushed_by` (the node that accepted the last push).
- `hosted_repo_copies`: what each core holds. Each node writes only its own
  rows.

Every core daemon reconciles its disk against this state. No node is special:

- **Primary.** The primary is the rendezvous-first *live* core for the
  repository id (`sha256("forkmesh-hosted-v1\n" + node + "\n" + id)`,
  highest wins). Only the primary writes the first commit or runs an import.
  The first commit is deterministic: fixed files, author, message and
  timestamp. So if two nodes both believe they are primary, they produce the
  same commit id.
- **Pushes.** A push goes to `pushed_by` when that node is live, otherwise to
  the rendezvous order. The node that took the last push holds every pushed
  ref. The others may still be fetching.
- **Replicas.** A core whose copy is empty or behind `pushed_at` receives
  sources: the pusher first, then the rendezvous order, each with a
  router-signed **read grant**. The grant uses the runner-grant format
  (`forkmesh-runner-grant-v1`, node = source, runner = asking core, at most
  1 h). The core fetches the source gateway's `/v1/runner/<owner>/<repo>.git`
  into a staging namespace and applies it with `reconcileUpstreamRefs`, the
  same fast-forward-only, deterministic-divergence merge the upstream sync
  uses. A stale source can never rewind a branch.
- **Wake-ups.** After a create, a push, or the first content pin, the app
  sends the payload-free `hosted` node event (`notify_repo_host`) to every
  core. The daemon also reconciles every 2 minutes (`hostedSafetyInterval`)
  and on `POST /v1/control/sync`.
- **Gateway configuration.** Nothing is edited by hand. The daemon rewrites
  the gateway configuration with one entry per hosted repository, plus one per
  organization alias on the same `gitDir`. The entries carry default read
  operations plus `git-receive-pack`. The ordinary sync cycle keeps their
  integrity pins current and adopts the new gateway generation.
- **Routing pin.** A public read is routed only to a core whose signed
  repository proof matches the pin (`stateHash`, which covers the default
  branch and tags). Replicas that are still catching up are skipped until they
  converge. The pin comes from the hosting node's signed proof after each push
  (`record_hosted_push_state`), or from the first content a core reports
  (initial commit or import).

Alternatives considered:

- *Primary only, others pull through the existing `upstreams` config.* This
  needs static configuration per repository, and the upstream URL would have
  to follow primary failover.
- *The app fans each push out to all three nodes.* A streamed pack can be
  read only once, and a failed leg would leave the copies diverged.

The desired-state loop reuses the grant, gateway, and reconciliation
machinery that already exists.

### Private repositories

A **private** hosted repository becomes a `restricted` gateway entry:

- It is configured like a public one, but it lives in
  `restricted_repositories`.
- It is reachable only by a verified router capability
  (`/v1/repositories/...`) or a runner grant.
- `/v1/public/...`, `/<owner>/<repo>.git`, the legacy branches read, the
  mirror dashboard, and the public repository count never see it.

The app serves it through `hosted_private_read`. This covers the named Git
URL (`info/refs`, `git-upload-pack`) and the `/api/repo/<owner>/<repo>/<read>`
routes. The caller must be the owner, or an organization reader
(`org_repos`, read permission or better):

- Git authenticates with the Basic hosted credential.
- The website authenticates with its session.
- A caller who is not a reader gets `401` with a Basic challenge (Git) or
  `404` (the website). A `404` never reveals that the repository exists.

**This is access control, not end-to-end encryption.** The core nodes hold
the bytes in plaintext, as any hosted forge does. A repository that must be
owner-sealed still needs a desktop or mobile node. The create modal and the
push panel say so.

### Organizations

With **Owner = organization**:

- The repository is stored under the creator's namespace.
- It is linked in `org_repos`, the existing alias model, so `/<org>/<name>`
  works.
- The organization name becomes an alias entry on every core gateway.
- Pushes use the existing organization RBAC (`_org_write_allowed`, write or
  better).
- Reads of a private repository need read permission or better.

### Imports

With an `importUrl`, the primary core runs:

```
git fetch <url> +refs/heads/*:refs/heads/* +refs/tags/*:refs/tags/*
```

Limits:

- `https` only. The host must be on the list. No credentials, port or query
  are allowed.
- The fetch is stopped when the repository grows past `maxRepositoryBytes`
  (256 MiB on the node) or runs for more than 10 minutes.
- HEAD points at `main`, `master`, `trunk` or `develop`, in that order of
  preference.

The replicas then fetch it from the primary. Private sources, which need a
provider token, are not supported.

## Moving to your own node

**Implemented.** The takeover is detected automatically. Once the owner's
own node (desktop or headless mirror) publishes the repository through the
normal signed catalog path, the relay-authored record (`source: hosted-web`)
is replaced. The next replica exchange then:

- marks the hosted row deleted,
- frees the quota,
- stops routing pushes to the cores,
- tells every core to move its copy to `.trash`.

A catalog row deleted on the website ends hosting the same way.

**Steps for the user.** The website does not automate these:

1. `git clone --mirror https://app.forkmesh.com/<owner>/<name>`. Use
   the hosted credential for a private repository.
2. Add it on the node:
   - Desktop: **Repos → Add local repository**.
   - Headless: `www/public/install.sh`, then serve it as that node's
     repository (www/docs/operations/headless-mirror-refresh.md).
   - Private: a private mirror is sealed by the node with the owner's key.
3. Publish it from the node. The signed publication takes the record over
   within one exchange (at most 2 minutes). Push to the node's own remote
   from then on.

Not implemented: a one-click "move" button, and keeping the cores as extra
mirrors after the takeover. Volunteer mirrors can still follow the
repository through the existing mirror mechanisms.

## Push mirrors to GitHub and GitLab

The owner of a hosted repository can keep a copy on GitHub or GitLab. They
configure a push mirror with a personal access token. The primary core then
pushes every head and tag to that remote. Only the primary pushes; replicas
never do. Daemon code: `server/hosted_push_mirror.go`.

**Desired set (app → primary).** The replica exchange response item gains an
optional `pushMirror`, sent only to the primary:

```json
"pushMirror": {
  "revision": "<opaque; changes when the owner edits the settings>",
  "provider": "github" | "gitlab",
  "url": "https://github.com/<owner>/<name>.git",
  "username": "x-access-token" | "oauth2",
  "token": "<PAT>",
  "prune": false
}
```

**Report (daemon → app).** The exchange request gains `pushMirrors`. It holds
the last attempt for every id with a known result, on every exchange, and is
left out when there are none:

```json
"pushMirrors": {
  "hr0123...": {"revision": "...", "digest": "<routing refs sha256 at push time>",
                "ok": true, "at": 1790000000000, "error": "", "refs": 3}
}
```

`digest` is `routingRefsSHA256`, the same value as the copy report's
`routingDigest`. `refs` counts the refs the porcelain output lists as
created, updated, forced or deleted. Up-to-date refs are not counted. After
an attempt the daemon asks for another exchange at once, so the app sees the
result within one loop tick instead of the 2-minute safety interval.

**Validation.** An invalid `pushMirror` is reported as a failed attempt with
error `invalid push mirror`, and nothing is pushed. It is valid only when:

- `provider` is `github` or `gitlab`, and `username` is `x-access-token` or
  `oauth2`.
- `url` is exactly `https://<host>/<owner>/<name>.git`. The host must be on
  `hosted.pushHosts`. No userinfo, port, query, fragment, percent escapes,
  subgroups or `..` are allowed.
- `token` is not empty, is at most 512 characters, and has no whitespace or
  control characters.

`hosted.pushHosts` (JSON `pushHosts`) defaults to `github.com` and
`gitlab.com`. It is validated like `importHosts`: lower-case host names only.
Add a self-hosted GitLab host there to allow it.

**When the primary pushes.** After the apply rounds, for each non-empty
repository it is primary for:

- There is no earlier attempt, or `revision` changed: push.
- The last attempt failed: retry only after 10 minutes. A new `revision`
  retries at once. A failure with a bad token therefore does not hammer the
  forge on every push.
- The last attempt succeeded: push when the routing digest changed, when any
  head or tag changed (the full `refsSHA256`, so a new feature branch is
  mirrored as well), or 6 hours after the last success. The 6-hour push
  repairs a remote someone edited by hand.

Pushes run one after another, after replication. Each is limited to 10
minutes. A failed push is logged (`hosted <id>: push mirror to <url> failed:
…`) and reported. It is not one of the replicator's failures, so it never
marks the node unhealthy. The state lives in memory. A daemon restart
therefore pushes once more, which is harmless. State is dropped when the id
appears in `removed` or arrives without a `pushMirror`.

**The command and the token.** The primary runs:

```
git -C <gitDir> push --porcelain [--prune] <url> +refs/heads/*:refs/heads/* +refs/tags/*:refs/tags/*
```

`--prune` is added only when `prune` is true. The token never appears on a
command line or in the URL. It travels as
`http.extraHeader: Authorization: Basic base64(username:token)` through
`GIT_CONFIG_COUNT`/`GIT_CONFIG_KEY_n`/`GIT_CONFIG_VALUE_n`, the same way
`fetchFromPeers` passes its grant. The same environment also sets:

- `credential.helper=` (no stored credentials are used or written)
- `http.followRedirects=false`
- `GIT_TERMINAL_PROMPT=0`
- empty `GIT_ASKPASS`/`SSH_ASKPASS`
- `GIT_ALLOW_PROTOCOL=https`

**Redaction.** Error text comes from git's stderr (or stdout when stderr is
empty). Every occurrence of the token and of its base64 header value is
replaced with `***`. The text is then folded to one line of at most 300
characters. That is what is logged and reported as `error`.

## Provider imports (GitHub, GitLab, Codeberg): the reported side claim

The report says those imports don't complete real mirroring. That is
accurate, and it is by design:

- `app/src/repository_imports.py` imports provider metadata and
  collaboration history only: issues, pull requests, releases and selected
  documents.
- The Git contents need a mirror node ("never cloneable through ForkMesh
  until an independently verified mirror is linked to it").
- The modal said so already. The entry stays under *External repositories*.

The fix for the missing path is the hosted import above: paste the public
URL into **Remote clone URL**, and the core hosts a real, cloneable copy.
The metadata importer is unchanged. Linking an existing metadata import to a
hosted copy is not done.

## The create page

Creating or mirroring a repository used to be a popup on the Repositories
page. It is now its own page, `/dashboard/repos/new` (`new-repo` in
`dashboard_shell.PAGES`), so it has a real URL and room for the form. The
**New repository** button, the home rail's **New** and the command palette
link there, and the old `/dashboard/repos?new=1` marker redirects to it.
The form keeps every `data-new-repo-*` hook, so the flow code in
`05-repo-list-explorer.js` is shared; `prepareNewRepoForm` does what opening
the popup did. Its styles are scoped to the view (`.nr-*`, built on the
`--dashboard-*-rgb` colours), so the page needs no Tailwind rebuild. The
popup (`new-repo-modal`) stays on the Repositories page only for the
"mirror this repository from your desktop node" handoff
(`openExternalMirrorHandoff`).

## The repository page right after creation

The public catalog (`/api/repositories`) is a prebuilt snapshot. The
republish doorbell rebuilds it at most every two minutes
(`SNAPSHOT_REPUBLISH_MIN_INTERVAL_MS`), and the page keeps the last catalog
in `localStorage`. So for up to two minutes after **Create**, a reload of
`/<owner>/<name>` read a catalog without the new repository and replaced the
server-rendered first view with **Repository not found** (2026-09-26,
`jett/test`).

`initRepoPage` now does one fresh catalog read (cache-busted, so a live
read) before it renders the 404, but only when the server already rendered
the repository (`[data-repo-ssr]`) or the visitor is signed in. An anonymous
visitor to a name that does not exist still gets the 404 from the snapshot,
so crawlers never cause a live catalog read.

## The repository list right after a delete

After **Delete repository** the page goes to `/dashboard/repos`, which drew
the list from the catalog saved in `localStorage` (up to 5 minutes old) and
from the browser's HTTP cache of `/api/repositories` (`max-age=300`). For
about four minutes after a delete the snapshot also answers
`503 privacy-transition`, so the background refresh failed and nothing
replaced the stale copy. The deleted repository stayed listed (2026-09-26).

The page now records a successful delete under
`forkmesh.repositoryDeleted.v1` for 10 minutes. `renderRepositories` and
`storeRepositoryCatalog` drop those names, whichever cache the list came
from, and creating a repository with the same name clears the record.

## Issues and pulls on a hosted repository

The core daemons drain web-submitted issues and pulls with the signed
`forkmesh-issues-pull-v1` read, and the app admits a signer only if it is in
the repository's `groupNodes`. For a hosted repository the cores were added
to `nodes` (serving) but not to `groupNodes`, so every drain of a new hosted
repository answered 401 ("collaboration jett/test/issues: relay returned
HTTP 401", 2026-09-26). `_https_mirror_public_context` now adds the hosted
core nodes to both sets.

## Web merges on a hosted repository (2026-10-04)

A hosted repository already lives on the cores, which run the merge executor,
so its pull requests merge on the core itself, like any other public
repository ([server/merge-executor.md](server/merge-executor.md)). Asked as
"This is already on a mirror so it should be able to merge on itself": the
owner of `jett/test` pressed **Merge** and the page read "An online mirror is
merging the exact reviewed commits." for its whole 150 s wait, while no core
ever took the job (no `repo_merge_jobs` row, no `repository.pull_merge` audit
line).

- **Operations.** The replicator rebuilds hosted gateway entries on every
  pass, with only the read operations and `git-receive-pack`. The daemon's
  startup upgrade (`ensureGatewayCapabilities`) gave them `merge-pull` for a
  moment, and the next pass took it away again, so the App found no core that
  advertised the merge for the repository. `hostedGatewayOperations`
  (`server/hosted.go`) now gives a **public** entry `merge-pull` and
  `update-pull-branch` whenever the node runs the executor, plus the
  agent-branch operations when the daemon enabled them
  (`Daemon.agentBranchOps`). A restricted (private) entry gets none: the App
  merges only public repositories.
- **Replication.** A merge written on one core is recorded like a push. After
  a pull mutation (`repository_pull_merge_handler`) or an agent-branch merge
  (`agent_branches._merge`) succeeds, the App calls
  `record_hosted_push_state` with the core that ran it. That sets `pushed_by`
  and `pushed_at`, wakes every core with the `hosted` event, and pins the
  core's freshly signed refs digest. The other cores are then behind
  `pushed_at` and fetch from that core first. The merge only moves `main`, the
  pull ledger and the pull's own branch forward, so the fast-forward-only
  reconciliation takes it. The call does nothing for a repository that a node
  publishes itself.
- **The page.** While the App answers with a retryable refusal and no core
  has accepted the job, the merge panel says "Waiting for an online mirror to
  take this merge. No mirror has accepted it yet." It says a mirror "is
  merging" only once a core answers `202 processing`.

## Operations

- **On each core**, the root is `/var/lib/forkmesh/hosted/<id>.git`. It is
  owned by `forkmesh-node`, mode 0700, and sits inside the daemon's own state
  directory.
- Removed repositories are in `/var/lib/forkmesh/hosted/.trash/<id>-<unix>.git`.
  **Nothing deletes them.** Empty `.trash` by hand once the 30-day window is
  over, and only by exact name.
- Logs: `journalctl -u forkmesh-mirror-node | grep 'hosted'`, for failures to
  fetch, initialize or import.
- In the database (rqlite):
  - `SELECT id,visibility,pushed_at,pushed_by,deleted_at FROM hosted_repos`
  - `SELECT * FROM hosted_repo_copies`
- Disk: 3 × 10 repositories × 200 MiB per active account is the worst case,
  on 25 GB nodes. Watch `du -sh /var/lib/forkmesh/hosted` and lower
  `HOSTED_REPO_MAX_PER_ACCOUNT` if it grows.
- Turn it off:
  - On the cores the app's `HOSTED_GIT_CORE_NODES` comes from the fleet:
    `deploy/core/render.py` writes `FORKMESH_VAR_HOSTED_GIT_CORE_NODES` (every
    core in `fleet.toml`, or empty when `[hosted] enabled = false`) into
    `app.env`, which overrides `app/wrangler.toml` (2026-10-13, so a core
    added or removed is in or out of hosting with the next render). Setting
    the wrangler value to `""` turns hosting off only where no `app.env`
    sets it. With the list empty the create form falls back to the
    single-node or desktop flow.
  - On the nodes: set `[hosted] enabled = false` in `deploy/fleet.toml`, then
    run the rollout's steps 1 and 2. Existing entries stay served as
    configured repositories until they are removed.

## Rollout checklist (operator)

Do one node at a time, and dry-run each command first by leaving out
`--execute`. **The order matters.** The daemons and gateways must know the
`hosted` config and `restricted` visibility before the app starts creating
repositories.

1. **Node files.** For each core:

   ```sh
   python3 tools/next_fleet.py install nN --steps files,start --execute
   ```

   This writes:
   - the Caddyfile: the App host takes Git pushes up to 256 MB;
   - `/etc/forkmesh-next/mirror-node.overrides.json`, which gains
     `"hosted": {"enabled": true, "root": "/var/lib/forkmesh/hosted", …}`.

   Caddy reloads.
2. **Daemon and gateway.** For each core:

   ```sh
   python3 tools/next_fleet.py install nN --steps daemon --execute
   ```

   This installs the new binary and `tools/mirror_gateway.py`, merges the
   overrides, and restarts the daemon. Until step 3, the daemon logs
   `hosted replicas: HTTP 404` every 2 minutes. That is harmless.
   Check: `journalctl -u forkmesh-mirror-node -n 50`, and
   `curl -fsS https://nN.next.forkmesh.com/readyz`.
3. **App release.**

   ```sh
   python3 tools/next_deploy.py all --execute
   ```

   This applies migration 0199 (`hosted_repos`, `hosted_repo_copies`) and
   ships the new `[vars]`: `HOSTED_GIT_CORE_NODES`, `HOSTED_REPO_*` and
   `HOSTED_IMPORT_HOSTS`. No new secrets: grants and capabilities use the
   existing router key.
4. **Verify.**
   - `curl -fsS https://app.forkmesh.com/api/hosted-repositories` answers
     `"mode":"core"`.
   - Create a test repository from the website, with a README.
   - Within about a minute, every core has
     `/var/lib/forkmesh/hosted/<id>.git` with the same `main`:
     `git --git-dir=… rev-parse main`.
   - Clone it with the shown credential, push a commit, then clone again.
   - Repeat with a private repository. An anonymous
     `git ls-remote https://app.forkmesh.com/<you>/<name>` must ask for
     a password.
   - Delete the test repositories. Their copies move to `.trash`.
5. **Rollback.**
   - App: `python3 tools/next_deploy.py rollback nN --execute`. The 0199
     tables can stay.
   - Nodes: `[hosted] enabled = false`, then run steps 1 and 2.

No new units, ports, firewall rules or credentials are involved. The daemon
already reaches `app.next` (through `/etc/hosts`) and the other cores'
`nN.next:443`.

## Known gaps

- The website's repository page was not changed for private hosted
  repositories. The session-authorized `/api/repo/...` reads it uses are
  served, but the page's private-archive UI was not reworked or verified in a
  browser.
- Branch deletion and force pushes are refused (gateway rules).
- A core that is offline for more than the 30-day `removed` window keeps an
  orphaned copy until someone removes it by hand.
- Concurrent divergent pushes to the same branch on two cores during a
  failover converge to the newer commit (`reconcileUpstreamRefs`). The older
  push is then not on the branch.
