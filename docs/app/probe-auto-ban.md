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
class it earns. A path that spelled a dot as `%2e` (`/CHANGELOG%2emd`,
`/%2eeslintrc`) is a `script` probe whatever it names: no browser or link
writes a dot that way, and in the October 2026 log every such path came from a
scanner.

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

## Scoring and bans

- The visitor is the address in `cf-connecting-ip` (the TCP peer, or the
  verified edge visitor), IPv4 or the IPv6 `/64`, stored only as
  `blind_index("probe-visitor:" + address)`.
- Each **distinct** probe path adds its weight within a one-day window;
  repeats add nothing.
- At **3 points** the visitor is banned, so one direct secret or server-script
  probe (`/.env`, `/xmlrpc.php`, `/wp-json/`) bans on its own; a CMS or backup
  path needs one more probe path, a config or recon path two more.
- Until 2026-10-22 the line was 4 points within an hour, scripts weighed 2,
  and a ban lasted a day (a week for a repeat). The 404 log then held 4,109
  paths: 1,226 matched no class, and 66 visitors had scored without a ban,
  almost all with one direct `/.env`, `/xmlrpc.php`, `/wp-login.php`,
  `/wp-json/...` or `.sql` probe. Only a scanner sends those (a page cannot,
  see Safety rules), so the line moved to 3, scripts to 3, database dumps to
  `secret`, the window to a day, and the `recon` class and the rest of the
  table's new entries were added from those 1,226 paths. Replayed against the
  stored visitors, 62 of the 66 are now banned.
- The threshold was 6 until 2026-10-16. That week production had 39 bans but
  32 visitors left below 6, most of them two-path WordPress scans
  (`//xmlrpc.php` + `//wordpress/` = 4) and paired `.php` probes. A
  visitor's own browser never scores (see Safety rules), so two direct probe
  paths in an hour are not ordinary traffic. The same review found about 340
  scanner paths (credential-variable names, cloud metadata, pprof, Consul,
  Vault, Elasticsearch, Jenkins, double-encoded files) that matched no class;
  the table above now covers them.
- A ban lasts a week, or 30 days for a repeat within 90 days. It writes
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
  test). `FORKMESH_PROBE_BAN_ALLOW` adds more. `deploy/core/render.py`
  writes it into each core's `app.env` with every `[[runners]]` address from
  `deploy/fleet.toml` (the IPv4 address and the IPv6 /64): a job on a shared
  runner can reach the public App, and one probe from it would otherwise ban
  the runner's address, cutting off its own claims and every other owner's
  jobs. Autoscaled pool runners and user-run mirrors are not in the fleet
  file and are not covered.
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
delete `probe_visitors` unseen for 90 days and not banned; prune the memory
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
