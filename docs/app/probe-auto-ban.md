# Automatic bans for probing visitors

A visitor whose 404s are for files that only a vulnerability scanner asks
for (`.env`, `.git/config`, `wp-login.php`, `phpinfo`, `database.sql`,
`appsettings.json`, ...) is banned from every ForkMesh site the cores serve.
Added 2026-10-13 (branch 1986).

## Why

On 2026-10-13 the production 404 list ([not-found-tracking.md](not-found-tracking.md))
held 3,503 groups and 18,141 hits. 2,303 of the groups (4,608 hits) were
scanner signatures: 895 credential and secret files (dozens of `.env`
variants alone, cloud keys, `.git`, `.aws`, `.npmrc`, AI-agent configs such
as `.mcp.json` and `claude_desktop_config.json`), 300 server scripts
(`.php`, `.jsp`, `.cfm`, `cgi-bin`), 171 CMS and admin panels (`wp-*`,
`xmlrpc`, `actuator`, `solr`), 383 backups, dumps and logs, and 554 config
files. Most of the rest were the redacted private-repository Git reads, our
own missing assets (favicons, touch icons, `ads.txt`, blog images,
`dashboard-*.js`) and stale links. None of the signature paths exists on any
ForkMesh site, so a visitor asking for several of them is scanning us.

## Code map

| Code | Role |
| --- | --- |
| `app/src/probe_ban.py` | The classifier, scoring, bans, the sweep, Lift and the "Auto bans" card |
| `app/src/not_found_log.py` (`record`) | Hands every 404 (App and the forkmesh.com relay) to `probe_ban.observe`, before the exceptions check |
| `app/src/app_runtime.py` (`_fetch_inner`) | Answers 403 at once on the core that made the ban (`probe_ban.denied`) |
| `app/src/entry.py` | `_probe_ban`, the per-process `_probe_ban_local` table, the `probe-bans` cron phase |
| `app/migrations/0259_probe_bans.sql` | The `probe_visitors` and `probe_bans` tables |
| `app/host/forkmesh_host/request_policy.py`, `deploy/core/request_rules.py` | Enforce the ban fleet-wide (unchanged: a ban is a `request_block_rules` row) |
| `app/tools/sync_request_block_rules.py` | Leaves `auto-probe-*` rows out of the forkmesh.com WAF rule |
| `www/worker.js` (`reportNotFound`) | Relays the visitor's `Sec-Fetch-Site` with each forkmesh.com 404 |

## Classification

`probe_ban.classify(path)` percent-decodes the path (scanners send `%2f.env`
and `config%2ejson`), lower-cases it and checks each segment. The first
match from the top wins for a segment; a path takes the heaviest class any
segment earns.

| Class | Weight | Examples |
| --- | --- | --- |
| `secret` | 3 | any `.env*` segment, a file named with an `env` token (`config.env`, `env.txt`, `runtime-env.js`), `.git`, `.svn`, `.aws`, `.ssh`, `.npmrc`, `.s3cfg`, `.htpasswd`, `.vscode`, `web-inf`, `id_rsa`, `wp-config.php*`, files naming credentials, secrets, private keys, service accounts, tokens or keys (`gcp-credentials.json`, `key.json`), `*.pem`/`*.key`, AI-agent configs (`.mcp.json`, `claude*.json`, `opencode.json`) |
| `script` | 2 | `.php*`, `.phtml`, `.asp(x)`, `.jsp`, `.cgi`, `.cfm`, `.action`, `phpinfo`, `cgi-bin`, `phpunit`, `..` traversal |
| `cms` | 2 | `wp-*`, `wordpress`, `xmlrpc`, `wlwmanifest.xml`, `phpmyadmin`, `pma`, `adminer`, `actuator`, `solr`, `server-status`, `telescope`, `_profiler`, `cfide`, `logonpoint` |
| `backup` | 2 | last segment ending `.sql`, `.bak`, `.old`, `.orig`, `.save`, `.swp`, `~`, `.zip`, `.tar.gz`, `.7z`, `.db`, `.sqlite`, `.log` |
| `config` | 1 | `.yml`, `.ini`, `.conf`, `.toml`, `.properties`, `.dist`, files named `config*`/`settings`/`appsettings*`/`deploy*`/`parameters`, `package.json`, `composer.lock`, `Dockerfile`, `docker-compose*`, `web.config`, `AGENTS.md`, any other dot-segment, a bare `env` or `config` |

Never classified:

- `/.well-known/…`, `/docs/…`, `/_host/…` and the redacted
  `/private-or-unpublished-repository/…`. `/docs/` because its own broken
  links (`/docs/ssh-gateway-config.schema.json/`) looked like config files.
- Repository routes: a third segment that is a route family (`blob`, `tree`,
  `raw`, `commit`, `releases`, `issues`, ...), so a missing `config.php` in a
  public repository never counts.
- `/api/…` deeper than three segments, or under `/api/repo`, `/api/repos`,
  `/api/orgs`, `/api/users`, `/api/raw`, `/api/git`. `/api/.env`,
  `/api/v1/.env`, `/api/staging/.env` and `/api/phpinfo.php` still count.
- Our own asset names: `.js`, `.css`, `.png`, `.webp`, `.ico`, `.txt`,
  `.map`, `.md` (other than `AGENTS.md`/`CLAUDE.md`) carry no weight by
  themselves.

`app/tests/test_probe_ban.py` pins both lists: the scanner families taken
from the production 404 list, and our own 404s and repository routes.

## Scoring and bans

The visitor is the address in `cf-connecting-ip` (the TCP peer through
Caddy, or the verified forkmesh.com visitor), as an IPv4 address or the
IPv6 `/64` around it, since one IPv6 host rotates through its `/64`. It is
stored only as `blind_index("probe-visitor:" + address)`.

- Each **distinct** probe path adds its class weight; the same path again
  adds nothing, so a broken client retrying one URL never bans itself. The
  window is an hour from the first probe; older probes stop counting.
- At **6 points** the visitor is banned: two secret files, three scripts,
  CMS paths or backups, or any mix (secret + CMS + config). A single `/.env`
  or a stray `/config.json` does not ban. The threshold is two kinds of
  evidence on purpose: one address can be a whole carrier NAT, an office or
  a VPN exit.
- The ban lasts **a day**, or **a week** when the same visitor was banned in
  the previous 30 days.
- A ban writes `probe_bans` (rule id, sealed visitor, classes, the paths,
  score, expiry) and a `request_block_rules` row: rule id
  `auto-probe-<first 32 hex of the blind index>`, `match_type` `ip` (`cidr`
  for an IPv6 `/64`), priority 0, note `auto probe ban: <classes>, score N`.
  The insert is `ON CONFLICT DO NOTHING`, so an operator's own rule for the
  same address is left as it is.
- The core that made the ban answers that visitor 403 at once for two
  minutes from memory (`_probe_ban_local`, before any route). Every core's
  `request_policy` reloads the rules within 60 s and Caddy's include follows
  (docs/host/README.md, "Request rules in Caddy"), so after that the
  visitor is refused before Python on every core. `/health`, `/readyz` and
  `/api/fleet/*` stay exempt as they are for every rule.
- Operator rules always win the 1,000-rule load: they have priority 100,
  auto bans 0. At most 500 bans are enforced at once; past that a ban is
  recorded as "Observed" and blocks nothing. An observed ban only restarts
  the visitor's window: it does not count as a previous ban, and the
  visitor is banned for real at its next 6 points once there is room.

### A page cannot get its visitors banned

Any page could make a visitor's browser ask for `/.env`: an `<img>` on
another site, or one in a public issue rendered on app.forkmesh.com. A 404
is therefore scored only when its `Sec-Fetch-Site` header is absent (curl,
scripts and scanners send none) or `none` (a URL typed or opened from a
bookmark). `cross-site`, `same-site` and `same-origin` requests are still
logged as 404s but never scored. The forkmesh.com Worker copies the header
into the relay body as `fetchSite` (`www/worker.js`, `reportNotFound`); until
that Worker is deployed, relayed forkmesh.com 404s count as header-less.
Browsers too old to send the header (Safari before 16.4) are the residual
case.

### Never banned

`probe_ban.ban_target` refuses any address that is not globally routable
(loopback, private, link-local, carrier NAT, documentation ranges, the mesh
`10.77.0.0/16`) and every Cloudflare range. Cloudflare matters: a
forkmesh.com request reaches the App from a Cloudflare address, and if its
signed visitor header ever failed to verify, `cf-connecting-ip` would be
that Cloudflare address; Caddy's `remote_ip` matcher would then refuse our
own forkmesh.com fetches. The list is a copy of `CLOUDFLARE_RANGES` in
`deploy/core/render.py`; a test keeps them equal.
`FORKMESH_PROBE_BAN_ALLOW` (comma-separated addresses or networks) adds
more, for a monitor or a pentest.

### forkmesh.com

forkmesh.com's own static 404s count (the relay carries the verified visitor),
and the ban blocks that visitor on app.forkmesh.com, world.forkmesh.com and
on every forkmesh.com page the homepage Worker fetches from the App. It does
not reach forkmesh.com's static files: those are served by Cloudflare, and
`sync_request_block_rules.py` leaves `auto-probe-*` rows out of the WAF
expression (its 3,500-byte limit would not hold hundreds of addresses, and
the WAF is published by hand). A static 404 costs us nothing.

## Modes

`FORKMESH_PROBE_BAN` in the App's environment. To keep a setting, add it
(and `FORKMESH_PROBE_BAN_ALLOW`) under `[vars]` in `app/wrangler.toml` and
deploy: `/etc/forkmesh-next/app.env` is rendered whole by
`deploy/core/render.py`, so a line added there by hand works after an app
restart but is gone at the next deploy. Lift on the admin page is the quick
way to undo one ban.

| Value | Effect |
| --- | --- |
| `ban` (default, also any unknown value) | Score, ban and block |
| `observe` | Score and record bans ("Observed") without a rule or a 403 |
| `off` | Nothing is scored |

In `observe` and `off` the next sweep deletes every `auto-probe-*` rule, so
switching away from `ban` lifts all automatic bans within five minutes.

## Sweep

Cron phase `probe-bans`, every 5 minutes (minute residue 0, the lightest
slot of the cron schedule):

1. delete `auto-probe-*` rules with no active enforced, unlifted ban (expired,
   lifted, orphaned, or all of them when the mode is not `ban`);
2. delete `probe_bans` rows that expired more than 7 days ago (they stay on
   the admin page as "Expired" until then);
3. delete `probe_visitors` not seen for 30 days and not banned;
4. drop expired entries from this process's memory table.

## Admin page

The console's 404s page (`?view=not-found`) marks each group whose path is a
probe with a red "Probe" pill, and has an "Auto bans" card between the list
and the exceptions: the mode, the threshold, active and last-24-hour counts,
and the last 100 bans with the visitor (address or `/64`, country, agent),
the classes, the first four paths, the score, when, until when and the
status (Banned, Observed, Expired, Lifted). **Lift** (CSRF-checked, audited as
`admin.console_lift_probe_ban`) deletes the rule, marks the ban lifted and
resets the visitor's score and ban count, so its next ban is a one-day ban
again; other cores stop refusing it within a minute.

Ignoring a path on the 404s page stops logging it but not scoring it: a
scanner's `/.env` still counts toward its ban.

## Re-key

`probe_visitors` and `probe_bans` are emptied by a re-key (docs/REKEY.md):
both are keyed by a blind index. The next sweep then deletes every orphaned
`auto-probe-*` rule, so a re-key lifts all automatic bans.

## Tests

`app/tests/test_probe_ban.py` (classifier, targets, scoring, bans, IPv6,
modes, cap, sweep, Lift, admin page, the 404-log hook, embedded probes,
the WAF SQL),
`app/tests/test_not_found_log.py` (its harness binds the real module),
`www/tests/not_found_relay.test.mjs` (`fetchSite`) and the `probe-bans`
slot in `app/tests/test_cron_runner.py`.
