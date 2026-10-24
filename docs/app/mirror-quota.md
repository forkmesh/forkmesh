# Mirror quota

All of it is the `mirror-quota` instance plugin (an `OPT_IN` plugin: off on
instances until an admin turns it on under Plugins, and off on forkmesh.com
unless `FORKMESH_CORE_PLUGINS` names it). While it is off there is no card,
no `/<owner>/<repo>/mirror` page, no call for mirrors on the feeds, lite
pages or issue list, and `/api/repo/<o>/<r>/mirror-quota` answers 404.

Part of [the App docs](README.md). A repository can set a goal of healthy
copies kept elsewhere ("we want 10 healthy mirrors running") and list the
copies people run. The repository page shows the count, each copy's health
and how to add one, in a "Mirrors" card at the bottom of the About rail.

The feature was asked for by the example instance
(`forum.example.org`), which mirrors the GitHub forum
`github.com/example/forum` into `example/forum` with
[forge sync](../../www/docs/forge-sync.md) and runs a "let's build 10
mirrors" campaign so the forum survives censorship or a takedown of either
site. Because the issues are committed under `.forkmesh/issues`, a plain
`git clone --mirror` carries the whole forum.

Code: `app/src/mirror_quota.py` (API, health rules, cron job),
`app/public/dashboard/js/09-mirror-quota.js` (card and dialog),
`app/host/forkmesh_host/repo_ssr_mirrors.py` (the same card in the first
view). Tests: `app/tests/test_mirror_quota.py`,
`app/tests/host/test_repo_ssr_mirrors.py`.

## What is counted

These are not the ForkMesh mirror fleet. The Mirrors tab and the "Direct
access" list (`GET /api/repo/<o>/<r>/mirrors`) are ForkMesh nodes that serve
the repository with signed proofs and are routed to; on an instance they are
hidden because one node serves everything ([../INSTANCES.md](../INSTANCES.md#dashboard-on-an-instance)).
The quota counts plain git copies anywhere, which the instance never routes
to: it only checks that they answer and are current.

The card's rows:

- **This instance**, built in and always green while it serves the page.
- **The original**, built in, when the repository carries
  `.forkmesh/forge-source.json` (`forkmesh-forge-source-v1`, written by
  `tools/forge_sync.py`): "GitHub (original)" for `provider: github`
  (`https://github.com/<repository>`), "<host> (original)" for Gitea or
  Forgejo (`baseUrl`). The file is read from the repository's own raw route at
  most every six hours; a failed read keeps the last answer and is retried
  after 30 minutes, and a missing file removes the row.
- **Registered mirrors**, stored in the database.

"healthy" is the number of green rows, built-in rows included, so
"3 / 10 healthy" with the instance and GitHub green means one external copy is
healthy.

## Storage

Migration `0283_repo_mirror_quota.sql`:

- `repo_mirror_quota` (`repo_bi` = the repository's `key_bi`, `target`,
  `next_check_at`, `updated_at`, `data`). `data` is encrypted (`encrypt_row`)
  and holds the storing owner and name, the repository's default-branch heads
  of the last 24 hours (`heads`: commit, first and last seen, at most 200),
  the parsed forge source and its last check. A row exists once a goal is set
  or a mirror is added.
- `repo_mirror_registry` (`repo_bi`, `mirror_id`, `added_by_bi`,
  `created_at`, `next_check_at`, `data`). `mirror_id` is the first 16 hex
  digits of SHA-256 of host plus lowercase path without `.git`, so the https,
  git:// and ssh forms of one copy are one row. `data` is encrypted: `url`,
  `host`, `scheme`, `checkable`, `label`, `kind`, `addedBy` and `check`
  (`lastCheck {at, ok, headCommit, error, latencyMs}`, `lastOkAt`,
  `lastOkHead`, `failures`, `checks`).

Both tables are in `_REPO_SCOPED_TABLES` (purged with the repository) and in
`tools/next_rekey.py` (`BI_COLUMNS`, `CIPHER_COLUMNS`); see
[../REKEY.md](../REKEY.md).

## API

`/api/repo/<owner>/<repo>/mirror-quota`, routed from the
`REPO_API_PREFIX_RE` block in `app/src/app_runtime.py` after the organization
alias rewrite. Public repositories only: a private or unknown repository
answers 404 to every method.

- `GET` → `{ok, target, healthy, total, registered, max, maxTarget, cloneUrl,
  mirrors: [{id, label, kind, url, host, builtin, status, statusLabel,
  lastCheck, addedBy, addedAt, checkable}]}`. Anonymous answers are
  `public, max-age=30, stale-while-revalidate=60`; a signed-in request also
  gets `viewer {signedIn, name, canSetTarget, canAdd}` and per-row
  `canRemove`, and is `private, no-store`. Both vary on `authorization,
  cookie`.
- `PUT {target}` sets the goal, 0 to 100 (0 turns it off). Maintainers only.
- `POST {url, label?, kind?}` adds a mirror. Any signed-in account. 201 with
  the new payload and `added`.
- `DELETE {id}` (or `?id=`) removes one. The account that added it, or a
  maintainer.

Mutations use `_account_session_record`, so a cookie session also needs an
allowed `Origin` (the App's CSRF rule) and a verified email. A maintainer is
the owner of the storing node (`_account_owns_node`), an account with
`maintain` or `admin` on the linked organization repository
(`_org_linked_repo_permission`), or an instance administrator (`_is_admin`).

**Addresses.** `https://host[:port]/path`, `git://host[:port]/path`,
`ssh://[user@]host[:port]/path` or `user@host:path`, at most 300 printable
ASCII characters, a dotted DNS name (no IP literal, no `localhost`, `.local`,
`.internal`, `.lan`, `.home.arpa`, `.test`, `.example`, ...), path characters
`A-Za-z0-9._~-` and `/`, no query, no fragment, no password, and no user name
on https. The instance's own host is refused (`same_instance`), and so is the
original's address. Only https addresses are checked; git://, ssh and
`.onion` / `.i2p` addresses are listed with status `unknown` ("Not checked").
`kind` is inferred from the host (`github.com`, `gitlab.com` or `gitlab.*`,
`codeberg.org`, `forkmesh.com` and its subdomains, `git` for git:// and ssh);
a `kind` sent by the client is used only when the host says nothing.

**Limits.** 100 mirrors per repository, 3 adds per account per minute and 10
per day (counted on `added_by_bi, created_at`; 429 with `Retry-After: 60`).

## Health checks

Cron phase `mirror-quota` runs at minute 3 of every five
(`_cron_due_phases`, `CRON_PHASE_CATALOG`):

1. Up to 4 due `repo_mirror_quota` rows: the repository's catalog record
   gives the current default-branch commit, which is added to `heads`; the
   forge source is reread when six hours old, and the original is checked
   when its jittered 15-minute slot is due. The row is due again in five
   minutes.
2. Up to 6 due `repo_mirror_registry` rows, oldest first, each checked and
   then due again in 15 minutes plus up to 3 minutes of jitter.

The run stops starting checks after 15 seconds: the cron tick has a 35-second
budget (`CRON_DISPATCH_TIMEOUT_MS`) shared with the other phases, so each
check gets 10 seconds rather than the 20 a standalone checker would allow.
At ten checks every five minutes, 24 mirrors keep a 15-minute cadence; more
are checked less often, oldest first.

**One check** is `git ls-remote` without git: `GET <url>/info/refs?service=git-upload-pack`
through the App's `fetch`, so the host's egress guard applies (public
addresses only, every redirect hop re-checked, `*.forkmesh.com` refused
except this instance's own domain). The body is read through a stream reader
and dropped past 4 MiB. Smart-HTTP pkt-lines and the dumb-HTTP `info/refs`
format (`<sha>\t<ref>`) are both parsed; the head is `refs/heads/<default
branch>`, else `HEAD` (or its `symref`).

**Status.**

| Status | Rule |
| --- | --- |
| green | last check reached it and its head is a commit this repository's default branch had in the last 24 hours |
| yellow | reachable but the head is another commit or the branch is missing, or the last success is older than 24 hours |
| red | never reached, or two failed checks in a row |
| unknown | not checked yet, or not checkable |

Status is worked out when the API is read, from the stored check and the
stored heads, so it ages without a write. A single failed check keeps the
previous answer. The original is green when reachable; when the catalog
record carries `upstreamCommit` (the commit this copy last took from it) and
the original's head has differed from it for 24 hours, it is yellow ("Our
copy is behind").

## Card

`renderRepoMirrorQuotaCard(data, viewer, helpHref)` draws, under a hairline
like the rail's other sections:

- "Mirrors" and "N / goal healthy" (or "N healthy" without a goal), then a
  slim progress bar filled in `success` green when there is a goal.
- Below its goal (a goal set and the overall status not green), a muted line
  "Needs N more mirrors · How to help →" linking to `/<o>/<r>/mirror`
  (`repoMirrorHelpHref`; `repo_ssr_mirrors.card_html(..., help_href)` in the
  first view, from `repo_ssr._about_rail`), the how-to-help page described in
  [feeds-and-lite.md](feeds-and-lite.md#asking-for-mirrors).
- One row per copy: a status dot (`bg-success`, `bg-attention`,
  `bg-destructive`, `bg-muted-foreground/40`) with `role="img"` and the word
  (Healthy, Behind, Unreachable, Not checked) as its label, the name as a
  `primary` link for https addresses (new tab, `noopener noreferrer
  nofollow`), and a muted line with the host, the detail and "checked 5
  minutes ago". Nothing is loaded from the mirrors or the original: no
  favicons, no pictures.
- "Add a mirror" (neutral `fm-btn-sm`; the page's green button stays "Create
  fork"). The dialog explains why copies matter, gives
  `git clone --mirror <this repository>.git`, `git update-server-info` and a
  15-minute cron line, says the issues are in `.forkmesh/issues`, links
  `https://forkmesh.com/start` to start an instance, links "Read the full
  guide" to `/<o>/<r>/mirror`, and for a signed-in
  visitor has the address and name fields; a visitor is asked to sign in.
- Maintainers get a number field and "Set target" ("Set a mirror goal" when
  there is none). The adder or a maintainer gets "Remove" on a row, with a
  confirmation.

The card is hidden when there is no goal and no registered mirror, except
for maintainers, who see the goal field. Private repositories have no card.

**First view.** The host micro-caches anonymous `GET .../mirror-quota` like
`/about` (`MICRO_CACHE_RE` in `repo_ssr.py`, 15 s fresh, 5 min stale, a write
through the node drops it) and remembers the last anonymous answer
(`_quota_memo`, `cached_quota`, warmed like `/about`). When it has one, the
first view draws the card with `repo_ssr_mirrors.card_html` at the bottom of
the rail and seeds it as `mirrorQuota` in `forkmesh-repo-ssr`;
`repoMirrorQuotaSection` draws the same markup from that seed, so the rail
does not change when the dashboard takes over, and `loadRepoMirrorQuota`
(from `loadRepoAboutRail`) refreshes it. Without one, both draw an empty
hidden section. The parity test runs the dashboard functions under node and
compares them with the Python port; a markup change goes in both.

## Setting the goal for example/forum

As the instance administrator or an owner/maintainer of the `example`
organization, open `https://forum.example.org/example/forum`, find
"Set a mirror goal" at the bottom of the right rail, enter 10 and press
"Set target". From a shell with a session token:

```sh
curl -X PUT https://forum.example.org/api/repo/example/forum/mirror-quota \
  -H "Authorization: Bearer $FORKMESH_SESSION" -H 'content-type: application/json' \
  -d '{"target": 10}'
```

The first cron run after that (within five minutes) reads the forge source and
adds the "GitHub (original)" row.

## Overall status

The card's heading dot and progress bar are coloured by `status` in the API
answer (`overall_status` in `app/src/mirror_quota.py`, `mirrorQuotaOverall` in
the dashboard, `overall_status` in `repo_ssr_mirrors.py`): green when the
healthy count reaches the goal, yellow when it reaches at least half of it, red
below half (2 of 10 is red). Every green row counts, including "This instance"
and the original on GitHub, which is one of the mirrors. With no goal the card
is green when anything is healthy.

