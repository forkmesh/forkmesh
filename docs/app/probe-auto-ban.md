# Automatic bans for probing visitors

A visitor whose 404s are for files only a vulnerability scanner asks for
(`.env`, `.git/config`, `wp-login.php`, `phpinfo`, `database.sql`, ...) is
banned from every site the cores serve. See
[not-found-tracking.md](not-found-tracking.md) for the 404 log that feeds it.

## Code map

| Code | Role |
| --- | --- |
| `app/src/probe_ban.py` | Classifier, scoring, bans, sweep, Lift, the "Auto bans" card |
| `app/src/not_found_log.py` (`record`) | Hands every 404 to `probe_ban.observe`, before the exceptions check |
| `app/src/app_runtime.py` (`_fetch_inner`) | Answers 403 at once on the core that made the ban (`probe_ban.denied`) |
| `app/src/entry.py` | `_probe_ban`, the per-process `_probe_ban_local` table, the `probe-bans` cron phase |
| Tables | `probe_visitors`, `probe_bans` |
| `app/host/forkmesh_host/request_policy.py`, `deploy/core/request_rules.py` | Fleet-wide enforcement: a ban is a `request_block_rules` row |
| `app/tools/sync_request_block_rules.py` | Leaves `auto-probe-*` rows out of the edge WAF rule |

## Classification

`probe_ban.classify(path)` percent-decodes the path (twice when it was
double-encoded, as in `status%252ephp`), lower-cases it, checks the whole path
against product signatures and then each segment; the path takes the heaviest
class it earns.

| Class | Weight | Examples |
| --- | --- | --- |
| `secret` | 3 | `.env*`, `.git`, `.svn`, `.aws`, `.ssh`, `.npmrc`, `.htpasswd`, `id_rsa`, `wp-config.php*`, credential/key files (`*-key.txt`, `*_token.txt`, `creds.json`, `<provider>.json` for mail, payment, auth, database and AI providers), `*.pem`/`*.key`, AI-agent configs; a last segment named like a credential variable (`MAILGUN_API_KEY`, `MAIL_PASSWORD`, `aws_keys`, `*-credentials`, `service-accounts`); cloud metadata (`/latest/meta-data`, `/computeMetadata/v1`, `/metadata/identity`), Vault `/v1/sys/`, Rails `config/{environments,initializers}/*.rb`, Magento `app/etc/local.xml` |
| `script` | 2 | `.php*`, `.asp(x)`, `.jsp`, `.cgi`, `.cfm`, `.rsp`, `phpinfo*`, `cgi-bin`, `/api/<x>.php`, `..` traversal (also double-encoded), invalid UTF-8 (`%c0`), Windows drive paths |
| `cms` | 2 | `wp-*`, `xmlrpc`, `phpmyadmin`, `adminer*`, `actuator` (also under `/api/`), `solr`, `server-status`; first-segment products (`jenkins`, `grafana`, `kibana`, `rabbitmq`, `airflow`, `jupyter`, `administrator`, debug toolbars, `fpm-status`); infrastructure APIs: pprof, Consul `/v1/{agent,catalog,status,health}/`, etcd `/v3/kv/`, Docker `/v1.NN/`, Elasticsearch `/_cat`, `/_search`, ..., Sentry `/api/0/`, GitLab `/api/v4/ci/`, `/rails/` |
| `backup` | 2 | last segment ending `.sql`, `.bak`, `.old`, `.swp`/`.swn`, `~`, `.zip`, `.tar.gz`, `.db`, `.log`, `.log.N`; `db`/`database`/`dump`/`backup`/`users` `.csv`/`.json`/`.xml`; anything under `/var/log/` |
| `config` | 1 | `.yml`, `.ini`, `.conf`, `.toml`, `config*`, `appsettings*`, `Dockerfile`, `web.config`, build and dependency files (`go.sum`, `requirements.txt`, `Makefile`, `pom.xml`, `manage.py`, ...), `node_modules`, `venv`, API-doc discovery (`swagger*`, `api-docs`, `openapi.json`, `graphiql`), other dot-segments |

Product names count only as the first segment and credential-variable names
only as the last, so a marketplace package or repository called `jenkins` or
`aws-credentials` is never a probe. Repository routes win over signatures
(`/rails/rails/blob/...` is a repository). Generic words (`/admin`, `/mail`,
`/redis`, `/secrets`) stay unclassified.

Never classified: `/.well-known/…`, `/docs/…`, `/_host/…`, the redacted
private-repository path, repository routes (`blob`, `tree`, `raw`, ...),
deep or repository `/api/…` paths, and ordinary asset extensions.
`app/tests/test_probe_ban.py` pins scanner paths and legitimate paths.

## Scoring and bans

- The visitor is the address in `cf-connecting-ip` (the TCP peer, or the
  verified edge visitor), IPv4 or the IPv6 `/64`, stored only as
  `blind_index("probe-visitor:" + address)`.
- Each **distinct** probe path adds its weight within a one-hour window;
  repeats add nothing.
- At **4 points** the visitor is banned. The heaviest path weighs 3, so a
  ban always needs at least two distinct probe paths, since one address can be
  a shared NAT or VPN exit.
- The threshold was 6 until 2026-10-16. That week production had 39 bans but
  32 visitors left below 6, most of them two-path WordPress scans
  (`//xmlrpc.php` + `//wordpress/` = 4) and paired `.php` probes. A
  visitor's own browser never scores (see Safety rules), so two direct probe
  paths in an hour are not ordinary traffic. The same review found about 340
  scanner paths (credential-variable names, cloud metadata, pprof, Consul,
  Vault, Elasticsearch, Jenkins, double-encoded files) that matched no class;
  the table above now covers them.
- A ban lasts a day, or a week for a repeat within 30 days. It writes
  `probe_bans` and a `request_block_rules` row
  `auto-probe-<first 32 hex of the blind index>` (`ip` or `cidr`, priority 0),
  `ON CONFLICT DO NOTHING` so operator rules are untouched.
- The banning core refuses the visitor from memory immediately; every core's
  `request_policy` reloads rules within 60 s and Caddy then refuses before
  Python. `/health`, `/readyz` and `/api/fleet/*` stay exempt.
- Operator rules (priority 100) win the 1,000-rule load; at most 500 auto
  bans are enforced, beyond that a ban is recorded as "Observed".

### Safety rules

- **Cross-site requests are never scored.** Any page can make a browser
  request `/.env`, so a 404 is scored only when `Sec-Fetch-Site` is absent or
  `none`. The static-site relay forwards it as `fetchSite`.
- **Never banned:** non-routable addresses (loopback, private, link-local,
  carrier NAT, documentation, the mesh range) and every edge-provider range
  (a copy of `CLOUDFLARE_RANGES` in `deploy/core/render.py`, kept equal by a
  test). `FORKMESH_PROBE_BAN_ALLOW` adds more.
- Bans block the App, World and proxied site routes, not static files served
  by the edge.

## Modes

`FORKMESH_PROBE_BAN` (set under `[vars]` in `app/wrangler.toml` to persist
across deploys):

| Value | Effect |
| --- | --- |
| `ban` (default, also any unknown value) | Score, ban and block |
| `observe` | Score and record bans ("Observed") without a rule or a 403 |
| `off` | Nothing is scored |

Outside `ban`, the next sweep deletes every `auto-probe-*` rule.

## Sweep

Cron phase `probe-bans`, every 5 minutes: delete `auto-probe-*` rules without
an active enforced ban; delete `probe_bans` rows expired over 7 days ago;
delete `probe_visitors` unseen for 30 days and not banned; prune the memory
table.

## Admin page

The 404s page marks probe paths with a "Probe" pill and has an "Auto bans"
card (mode, threshold, counts, last 100 bans). **Lift** (CSRF-checked,
audited as `admin.console_lift_probe_ban`) deletes the rule and resets the
visitor's score and ban count. Ignoring a 404 path stops logging it but not
scoring it.

## Re-key

A re-key ([../REKEY.md](../REKEY.md)) empties both tables (they are keyed by
blind index); the next sweep then lifts all automatic bans.

## Tests

`app/tests/test_probe_ban.py`, `app/tests/test_not_found_log.py`,
`www/tests/not_found_relay.test.mjs`, the `probe-bans` slot in
`app/tests/test_cron_runner.py`.
