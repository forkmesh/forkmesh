# Repositories hosted on the core nodes

A user with no desktop node and no external forge account can create a
repository on the website and use it with plain `git` over HTTPS. Every core
keeps a full replica. Later the user can move the repository to their own
node.

Code: `app/src/hosted_git.py` (create, push routing, quotas, private reads),
`app/src/hosted_repos.py` (replica exchange, templates), migration
`0199_hosted_repositories`, daemon `server/hosted.go` and
`server/hosted_push_mirror.go`, the `restricted` visibility in
`tools/mirror_gateway.py`, and the create page `/dashboard/repos/new`. On a self-hosted instance the
operator command `instance-repo` creates and fills one from a repository on
the server through the same create helpers ([INSTANCES.md](INSTANCES.md)).

## What a user gets

**New repository** fields: name (`[A-Za-z0-9._-]`, at most 64 characters, not
starting with `.` or ending in `.git`), owner (self or an organization where
the user can maintain), description, visibility (public or private), README,
`.gitignore` template, license. A public `https://` URL on a host in
`HOSTED_IMPORT_HOSTS` imports a cloneable copy.

After creation the page shows the remote URL
`https://app.forkmesh.com/<owner>/<name>`, the commands, and a username and
password. The password is the session-bound push credential (`fmp1.…`): 7
days, never stored, revoked with the session, renewable with
`POST /api/hosted-repositories/push-credential`. It also clones private
repositories. There are no personal access tokens and no SSH access.

**Quotas** (`app/wrangler.toml [vars]`): `HOSTED_REPO_MAX_PER_ACCOUNT`
(default 10) and `HOSTED_REPO_MAX_BYTES` (default 200 MiB, compared with the
largest copy any core reports; over it a push answers
`413 repository_quota_exceeded`). Push bodies are streamed, up to
`FORKMESH_MAX_GIT_PUSH_BODY_BYTES` (256 MiB). The creating account must have a
verified email.

**Git rules** (gateway receive-pack): fast-forward only, no ref deletes, new
tags allowed but never moved, only `refs/heads/*` and `refs/tags/*`, objects
fsck-checked, no hooks, every accepted push audited per ref.

## Design

```
browser ── POST /api/hosted-repositories ──► app (any core; rqlite)
                                             │ catalog record + hosted_repos row
                                             └─ "hosted" node event ──► every core daemon
core daemon ── POST /api/hosted-repositories/replicas (node-signed) ──► app
            ◄── desired set: {id, owner, name, visibility, pushedAt, primary?, init?, sources[]}
    ├─ create <root>/<id>.git + gateway entry ("public" | "restricted")
    ├─ primary + empty: deterministic first commit, or a bounded import
    └─ behind / empty: fetch from a peer with a router-signed read grant, fast-forward only
git push ─► app (Basic credential) ─► last pusher, else rendezvous-first live core
git clone ─► app ─► public: a core whose signed refs proof matches the pin
                    private: owner or organization reader, then a core via capability
```

- **State** lives in rqlite: `hosted_repos` (spec, `pushed_at`, `pushed_by`)
  and `hosted_repo_copies` (each node writes only its own rows).
- **Primary:** the rendezvous-first live core for the repository id. Only it
  writes the first commit (deterministic, so two would-be primaries agree) or
  runs an import.
- **Pushes** go to the last pusher when live, else rendezvous order.
- **Replicas** behind `pushed_at` fetch from the pusher first using a
  runner-format read grant (at most 1 h) and apply refs with the same
  fast-forward-only reconciliation as upstream sync.
- **Wake-ups:** the payload-free `hosted` node event after create, push or
  first pin; a 2-minute safety reconcile; `POST /v1/control/sync`.
- **Gateway config** is rewritten by the daemon, one entry per repository and
  per organization alias. Public entries also get web merge operations when
  the node runs the merge executor; restricted entries do not.
- **Routing pin:** public reads go only to cores whose signed proof matches
  the pinned `stateHash`, recorded after each push or web merge
  (`record_hosted_push_state`).

### Private repositories

A private repository is a `restricted` gateway entry reachable only by a
router capability or runner grant; public routes, dashboards and counts never
see it. The app serves Git and `/api/repo/...` reads through
`hosted_private_read` to the owner or organization readers; others get `401`
(Git) or `404` (web). This is access control, not end-to-end encryption: the
cores hold plaintext. Owner-sealed repositories need the owner's own node.

### Organizations

The repository is stored under the creator's namespace, linked in `org_repos`
so `/<org>/<name>` works, and pushes use organization roles (write or better;
private reads need read or better).

`org_repos.repo` is always the lowercase name: every reader (the git and API
alias rewrite, org read and push permissions) looks it up lowercased, and the
organization link endpoint stores it that way. Hosted creation used to store
the name as typed, so a mixed-case repository such as `Bolt` had a link no
lookup could find: `/<org>/Bolt.git` and the short `/Bolt.git` answered
`not_found` and organization members other than the creator could not push.
`link_organization` now stores the lowercase name and renames an existing
mixed-case link of the same repository, and re-running `instance-repo` on an
existing repository relinks it, which repairs a link written before the fix.

### Imports

The primary fetches heads and tags from an allowed `https` host (no
credentials, port or query), stops past the size limit or 10 minutes, and
points HEAD at `main`, `master`, `trunk` or `develop`. Private sources are not
supported. The separate metadata importer (`app/src/repository_imports.py`)
imports issues, pulls and releases only.

## Moving to your own node

When the owner's node publishes the repository through the signed catalog,
the hosted record is replaced; the next exchange marks the row deleted, frees
the quota, stops routing pushes and moves core copies to `.trash`. Steps:
`git clone --mirror`, add it on the node, publish from the node.

## Push mirrors

The owner can mirror a hosted repository to GitHub or GitLab with a personal
access token. Only the primary pushes. The exchange carries an optional
`pushMirror` (`revision`, `provider`, `url`, `username`, `token`, `prune`) to
the primary, and the daemon reports `pushMirrors` results (`revision`,
`digest`, `ok`, `at`, `error`, `refs`).

- Valid only for `github`/`gitlab`, URL exactly
  `https://<host>/<owner>/<name>.git` on `hosted.pushHosts`, a token of at most
  512 characters without whitespace.
- Push when settings change, when refs change, or every 6 hours; retry
  failures after 10 minutes; each push is limited to 10 minutes.
- `git push --porcelain [--prune] <url> +refs/heads/*:refs/heads/* +refs/tags/*:refs/tags/*`
  with the token only in an `http.extraHeader` passed through `GIT_CONFIG_*`
  environment variables, no credential helper, no redirects, HTTPS only.
  Errors are redacted (token and header value replaced) and folded to 300
  characters.

## Operations

- Root on each core: `/var/lib/forkmesh/hosted/<id>.git` (owned by the daemon
  user, 0700). Removed repositories go to `.trash/<id>-<unix>.git`; nothing
  deletes them automatically. Empty it by exact name after the retention
  window.
- Logs: `journalctl -u forkmesh-mirror-node | grep hosted`.
- Database: `hosted_repos`, `hosted_repo_copies`.
- Watch disk use; lower the per-account quota if needed.
- `deploy/core/render.py` writes `FORKMESH_VAR_HOSTED_GIT_CORE_NODES` (every
  core, or empty with `[hosted] enabled = false`) and
  `FORKMESH_VAR_HOSTED_GIT_CORE_OWNERS` (plus `[hosted] retired_cores`) into
  `app.env`. An empty list makes the create form fall back to the node flow.
- Rollout order: node files (`files,start`), then daemon, then the app
  release, one core at a time.

## Known gaps

- Branch deletion and force pushes are refused.
- A core offline longer than the removal window keeps an orphaned copy.
- Concurrent divergent pushes on two cores during failover converge to the
  newer commit.
