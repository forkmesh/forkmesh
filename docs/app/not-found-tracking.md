# 404 tracking and automatic probe bans

Part of the [App docs](README.md). Every 404 on a ForkMesh site is counted in
one grouped list in the admin console (Reliability → "404s",
`?view=not-found`). When a group reaches 10 hits, administrators get one Ping.
A visitor whose 404s are for files only a vulnerability scanner asks for is
banned from every site the cores serve ([Automatic probe bans](#automatic-probe-bans)).

## Code map

| Code | Role |
| --- | --- |
| `app/src/not_found_log.py` | Recording, pruning, the alert, the `/api/not-found` relay endpoint, the admin page |
| `app/src/app_runtime.py` (`_fetch_inner`) | Calls `record_response` for every 404 |
| `app/src/admin_shell.py` | Sidebar entry and badge |
| `www/worker.js` (`reportNotFound`) | Relays the static site's own 404s to the App |
| Tables | `not_found_paths`, `not_found_hours`, `not_found_ignores` |

## Grouping

One `not_found_paths` row per (site, method, path):

- **site**: request host, lower-cased, `www.` removed; relayed hits always
  record under the host of `WWW_ORIGIN`.
- **method**: `HEAD` counts as `GET`; unusual verbs are `OTHER`.
- **path**: no query; repository routes pass through
  `_privacy_safe_log_path` so private names never appear; id-like or
  token-like segments become `[id]`; cut at 300 characters.
- **referrer**: host and path for ForkMesh sites, host only otherwise.
- **last visitor** (`last_client`): IP, country and User-Agent of the most
  recent hit, sealed with `encrypt_row` so no plaintext IP is stored.

Each 404 is one upsert on the group and one on its hour in
`not_found_hours`, which backs a 24-hour chart and per-group sparklines.

## Exceptions

An exception (`not_found_ignores`, per site and path, optionally `prefix`)
stops a path from being logged, for content removed on purpose. A matching
request only bumps the rule's `hits` and `last_seen`. A prefix of `/` is
refused. Rules are cached per process for a minute; at most 500. Adding and
removing are CSRF-checked and audited.

## Alert

When a hit takes an unalerted group to 10, the row is claimed with
`UPDATE … SET alerted_at=? WHERE … AND alerted_at=0` (sent once), and each
administrator gets an `operational_alert` Ping "404 reached 10 hits: <path>".
At most 5 groups alert per hour; held-back groups stay "Pending".

## Admin page

KPIs, the 24-hour chart, and groups sortable by hits, recent activity or
recency (50 per page), each with source label, sparkline, referrer, last
visitor, alert state, and Ignore / Clear actions (Clear is audited).

## Bounds

Groups unseen for 90 days are deleted; at most 5000 groups; pruning runs on
the first and every 50th new group per process; hourly rows older than 48
hours are deleted; read-only mode records nothing.

## Static-site relay

The static forkmesh.com site is the `forkmesh-www` Worker. When its asset
lookup answers 404 for GET or HEAD,
`reportNotFound` posts `{path, referrer, userAgent, fetchSite}` to
`POST /api/not-found` on `APP_ORIGIN` in the background. The endpoint is
unauthenticated by design: it can only record a 404 for a site path. The
visitor's IP arrives through the signed edge-client header.

## Tests (404 log)

`app/tests/test_not_found_log.py`, `www/tests/not_found_relay.test.mjs`,
`app/tests/test_admin_shell.py`.

## Automatic probe bans

A visitor whose 404s are for files only a vulnerability scanner asks for
(`.env`, `.git/config`, `wp-login.php`, `phpinfo`, `database.sql`, ...) is
banned from every site the cores serve. Every 404 is passed to
`probe_ban.observe` from `not_found_log.record`.

### Code map

| Code | Role |
| --- | --- |
| `app/src/probe_ban.py` | Classifier, scoring, bans, sweep, Lift, the "Auto bans" card |
| `app/src/not_found_log.py` (`record`) | Hands every 404 to `probe_ban.observe`, before the exceptions check |
| `app/src/app_runtime.py` (`_fetch_inner`) | Answers 403 at once on the core that made the ban (`probe_ban.denied`) |
| `app/src/entry.py` | `_probe_ban`, the per-process `_probe_ban_local` table, the `probe-bans` cron phase |
| Tables | `probe_visitors`, `probe_bans` |
| `app/host/forkmesh_host/request_policy.py`, `deploy/core/request_rules.py` | Fleet-wide enforcement: a ban is a `request_block_rules` row |
| `app/tools/sync_request_block_rules.py` | Leaves `auto-probe-*` rows out of the Cloudflare zone WAF rule it publishes |

### Classification

`probe_ban.classify(path)` percent-decodes the path (twice when it was
double-encoded, as in `status%252ephp`), lower-cases it, checks the whole path
against product signatures and then each segment; the path takes the heaviest
class it earns. A path that spells a dot as `%2e` (`/CHANGELOG%2emd`,
`/%2eeslintrc`) is a `script` probe whatever it names: no browser or link
writes a dot that way, and in the 404 log every such path came from a scanner.

| Class | Weight | Examples |
| --- | --- | --- |
| `secret` | 3 | `.env*`, `.git`, `.svn`, `.aws`, `.ssh`, `.npmrc`, `.htpasswd`, `_netrc`, `_environment`, `wgetrc`, shell and database `*_history`, `id_rsa`, `wp-config.php*`, `sftp-config.json`, `*.tfvars`, `GoogleService-Info.plist`, `google-services.json`, credential/key files (`*-key.txt`, `*_token.txt`, `creds.json`, `<provider>.json` for mail, payment, auth, database, cloud, chat and AI providers), `*.pem`/`*.key`, AI-agent configs; database dumps (`*.sql`, `*.sqlite`, `*.db`, `*.dump`, also compressed); a last segment named like a credential variable (`MAILGUN_API_KEY`, `MAIL_PASSWORD`, `aws_keys`, `*-credentials`, `service-accounts`); a last segment written as an environment variable (`MAIL_HOST`, `SES_REGION`, `ADM_SESSIONID`) in a path of one segment or three or more; a `.git/`, `.svn/`, `.hg/` or `.bzr/` directory anywhere, also under `/api/`; `/v1/secrets`, cloud metadata (`/latest/meta-data`, `/computeMetadata/v1`, `/metadata/identity`), Vault `/v1/sys/`, Rails `config/{environments,initializers}/*.rb`, Magento `app/etc/local.xml` |
| `script` | 3 | `.php*` (also `index.php.txt`), `.asp(x)`, `.jsp`, `.do`, `.action`, `.cgi`, `.cfm`, `.rsp`, `phpinfo*`, `cgi-bin`, `/api/<x>.php`, a `%2e` dot, `..` traversal (also double-encoded), invalid UTF-8 (`%c0`), Windows drive paths; WordPress and PHP admin tools as the first segment (`wp`, `wp-*`, `wordpress`, `xmlrpc`, `phpmyadmin`, `pma`, `adminer`, `_profiler`, `_ignition`, `boaform`, `hnap1`) |
| `cms` | 2 | `wp-*`, `xmlrpc`, `phpmyadmin`, `adminer*` deeper in a path, `actuator` (also under `/api/`), `solr`, `server-status`; first-segment products (`jenkins`, `grafana`, `kibana`, `rabbitmq`, `airflow`, `jupyter`, `administrator`, `elmah`, `nacos`, `zabbix`, `webmin`, `portainer`, debug toolbars, `fpm-status`); infrastructure APIs: pprof, Consul `/v1/{agent,catalog,status,health}/`, etcd `/v3/kv/`, Docker `/v1.NN/`, Elasticsearch `/_cat`, `/_search`, ..., Sentry `/api/0/`, GitLab `/api/v4/ci/`, `/rails/`, Metabase `api/session/properties` (also under `/metabase/`, `/bi/`, ...), XWiki `/bin/{get,view}/Main/`, Atlassian `/rest/api/1.0/`, Joomla `/media/system/js/`, Strapi `/api/users-permissions`, CKAN `/api/action/datastore_search*`, SQL endpoints `/api/{db,data,rds}/{query,execute}`, Tomcat `/manager/html`, LLM proxies (`bedrock-runtime`, `/v1/models`) |
| `backup` | 2 | last segment ending `.bak`, `.old`, `.swp`/`.swn`, `~`, `.zip`, `.tar.gz`, `.tbz2`, `.zst`, `.war`, `.log`, `.log.N`; `db`/`database`/`dump`/`backup`/`users` `.csv`/`.json`/`.xml`; anything under `/var/log/` |
| `config` | 1 | `.yml`, `.ini`, `.conf`, `.toml`, `config*`, `appsettings*`, `Dockerfile`, `Jenkinsfile`, `web.config`, `vercel.json`, build and dependency files (`go.sum`, `requirements.txt`, `Makefile`, `CMakeLists.txt`, `build.sh`, `pom.xml`, `manage.py`, ...), `node_modules`, `venv`, API-doc discovery (`swagger*`, `api-docs`, `openapi.json`, `graphiql`, `graphql`, `redoc`, `altair`), other dot-segments |
| `recon` | 1 | First segment only: directory guesses (`backup/`, `db/`, `keys`, `secrets`, `certs/`, `mysql`, `redis`, `kafka`, `console`, `debug`, `shell`, `terminal`, `proxy`, `fetch`, `render`, `screenshot`, `metrics`, `whoami`, `vendor/`, `uploads/`, `themes/`, `_next/`, `terraform/`, `ansible/`, ...), mail-provider names (`mailgun`, `sendgrid`, `ses`, `smtp`, ...), version files (`README.md`, `CHANGELOG.txt`, `LICENSE.txt`, `version.json`, `build-info`, `MANIFEST.in`), `reconx-*` canaries |

Product names and recon words count only as the first segment and
credential-variable names only as the last, so a marketplace package or
repository called `jenkins`, `kafka` or `aws-credentials` is never a probe, and
an `/owner/MY_REPO` path is never read as an environment variable. Repository
routes win over signatures (`/rails/rails/blob/...` is a repository). Words a
person types (`/admin`, `/login`, `/settings`, `/contact`, `/mail`, `/security`)
stay unclassified. A recon word weighs 1, so it bans only alongside two more
probe paths.

Never classified: `/.well-known/…`, `/docs/…`, `/_host/…`, the redacted
private-repository path, repository routes (`blob`, `tree`, `raw`, ...),
deep or repository `/api/…` paths, and ordinary asset extensions.
`app/tests/test_probe_ban.py` pins scanner paths and legitimate paths.

### Scoring and bans

- The visitor is the address the host puts in `cf-connecting-ip` (the TCP
  peer, or the verified visitor a trusted proxy names), IPv4 or the IPv6
  `/64`, stored only as
  `blind_index("probe-visitor:" + address)`.
- Each **distinct** probe path adds its weight within a one-day window;
  repeats add nothing.
- At **3 points** the visitor is banned, so one direct secret or server-script
  probe (`/.env`, `/xmlrpc.php`, `/wp-json/`) bans on its own; a CMS or backup
  path needs one more probe path, a config or recon path two more.
- These weights come from replaying the stored 404 log: one direct `/.env`,
  `/xmlrpc.php`, `/wp-login.php`, `/wp-json/...` or `.sql` probe is something
  only a scanner sends (a page cannot, see Safety rules), and two-path
  WordPress scans and paired `.php` probes are not ordinary traffic either.
  Add new scanner paths to the table rather than lowering the line.
- A ban lasts a week, or 30 days for a repeat within 90 days. It writes
  `probe_bans` and a `request_block_rules` row
  `auto-probe-<first 32 hex of the blind index>` (`ip` or `cidr`, priority 0),
  `ON CONFLICT DO NOTHING` so operator rules are untouched.
- The banning core refuses the visitor from memory immediately; every core's
  `request_policy` reloads rules within 60 s and Caddy then refuses before
  Python. `/health`, `/readyz` and `/api/fleet/*` stay exempt.
- Operator rules (priority 100) win the 1,000-rule load; at most 500 auto
  bans are enforced, beyond that a ban is recorded as "Observed".

#### Safety rules

- **Cross-site requests are never scored.** Any page can make a browser
  request `/.env`, so a 404 is scored only when `Sec-Fetch-Site` is absent or
  `none`. The static-site relay forwards it as `fetchSite`.
- **Never banned.** A job on a shared runner or a workflow on a mirror can
  reach the public App, and one probe from it would otherwise ban the host's
  address, cutting off its own claims or syncs and every other owner it
  serves ([runners](../operations/runners.md)). The full list of exempt
  sources, for operator review:

  | Source | Where it comes from | Scope |
  | --- | --- | --- |
  | Non-routable addresses | loopback, private, link-local, carrier NAT, documentation, the mesh range (`ipaddress` `is_global` false) | the address |
  | Cloudflare | `CLOUDFLARE_RANGES`, the static forkmesh.com Worker relays from there; a copy of the list in `deploy/core/render.py`, kept equal by a test | the ranges |
  | Operator allow list | `FORKMESH_PROBE_BAN_ALLOW` in the app environment (IPs or CIDRs, comma separated) | as written |
  | Fleet runners | `deploy/core/render.py` writes every `[[runners]]` address from `deploy/fleet.toml` into `FORKMESH_PROBE_BAN_ALLOW` in each core's `app.env` | IPv4 /32, IPv6 /64 |
  | Autoscaled nodes | `autoscale_nodes.public_ipv4` for every node not in state `removing` (both pools) | IPv4 /32 |
  | Live mirrors | `mirror_https_endpoints.source_ip` where `healthy=1`, `abuse_blocked=0` and the last signed health proof (`checked_at`) is under a day old | IPv4 /32, IPv6 /64 |

  `source_ip` is the edge-verified address (`cf-connecting-ip`) the mirror's
  last signed endpoint registration came from, written only when it changes
  so an unchanged renewal stays write-free; it is the host's own egress,
  unlike the DNS of its URL, which is often a Cloudflare tunnel. The
  autoscaled and mirror lists are read from the database at most once a
  minute per core (`HOST_EXEMPT_TTL_MS`), with no DNS lookup. Requiring a
  healthy, fresh, unblocked mirror means an account cannot exempt an address
  by registering a mirror that serves nothing; the cost is that a live mirror
  operator can probe from that address unbanned, as an allow-listed host
  can. Community pool runners (`runner_nodes`, scope `pool`) and user SSH
  runner setups store no address and are not exempt.
- An address that becomes one of these hosts while banned is lifted on the
  next check: its next probe 404 (once the minute cache refreshes), or the
  next `probe-bans` sweep, which lifts every active ban whose target is in
  the list (audited as `probe_ban.lift_fleet_or_mirror_host` by
  `probe-ban:fleet-or-mirror-host`).
- Bans block the App, World and proxied site routes, not the static
  forkmesh.com files.

### Modes

`FORKMESH_PROBE_BAN`, set for the cores under `[production.vars]` in
`deploy/fleet.toml` (the name must also be a `[vars]` name in
`app/wrangler.toml`, which `deploy/core/render.py` checks):

| Value | Effect |
| --- | --- |
| `ban` (default, also any unknown value) | Score, ban and block |
| `observe` | Score and record bans ("Observed") without a rule or a 403 |
| `off` | Nothing is scored |

Outside `ban`, the next sweep deletes every `auto-probe-*` rule.

### Sweep

Cron phase `probe-bans`, every 5 minutes: delete `auto-probe-*` rules without
an active enforced ban; delete `probe_bans` rows expired over 7 days ago;
delete `probe_visitors` unseen for 90 days and not banned; prune the memory
table; lift active bans on autoscaled nodes and live mirrors (see Safety
rules).

### Admin page

The 404s page marks probe paths with a "Probe" pill and has an "Auto bans"
card (mode, threshold, counts, last 100 bans). **Lift** (CSRF-checked,
audited as `admin.console_lift_probe_ban`) deletes the rule and resets the
visitor's score and ban count. Ignoring a 404 path stops logging it but not
scoring it.

### Re-key

A re-key ([rekey](../operations/rekey.md)) empties both tables (they are keyed by
blind index); the next sweep then lifts all automatic bans.

### Tests

`app/tests/test_probe_ban.py`, `app/tests/test_not_found_log.py`,
`www/tests/not_found_relay.test.mjs`, the `probe-bans` slot in
`app/tests/test_cron_runner.py`.
