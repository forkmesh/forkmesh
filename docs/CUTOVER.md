# Production cutover: Cloudflare Worker stack -> ForkMesh Next

This runbook moves production (`app.forkmesh.com`, `world.forkmesh.com`, and the
app behind `forkmesh.com`) from the Cloudflare Python Worker `forkmesh-app` and D1
`forkmesh` to the three Next core nodes. It follows the user's decisions of
2026-09-24:

1. **Data.** Production goes read-only. Then a final D1 export is taken,
   re-keyed under Next's `DATA_KEY` and loaded into Next, logins are checked,
   and traffic switches over. No write is lost.
2. **Hostnames.**
   - `app.forkmesh.com` and `world.forkmesh.com` become unproxied round-robin
     A records for the three cores.
   - `forkmesh.com` stays on Cloudflare. Its www Worker fetches the Next
     origin with the signed edge header.
   - The `next.*` names redirect to production.
   - No client needs an update.
3. **Timing.** Right after the rehearsal passes.
4. **Rollback.** The old Worker and D1 stay intact but unrouted for 14 days.

Everything below was rehearsed except the steps marked **LIVE**. The
rehearsal used local files, a throwaway local rqlite, Pebble as the ACME
server, dry-runs against the live Cloudflare API, and a read-only D1 export
("Rehearsal results" at the end has the details). The coordinator runs the
LIVE steps.

Conventions:
- The Next repo is `NX=~/projects/forkmesh/src` and the production repo
  is `PR=~/projects/forkmesh/src`.
- `CFG=~/projects/forkmesh/config/fleet`, `K=~/projects/forkmesh/ssh/fleet/next_fleet_ed25519`.
- `CORES` is the three core addresses and `N1_IP` is n1's, read from the
  operator file (the repository holds no node address, docs/deploy/README.md):
  ```sh
  CORES=$(python3 -c 'import json,os; d=json.load(open(os.path.expanduser("~/projects/forkmesh/config/fleet/fleet-instances.json"))); print(" ".join(d["fmnext-core-%d" % n]["ip"] for n in (1, 2, 3)))')
  N1_IP=${CORES%% *}
  ```
- Every tool dry-runs without `--execute`. Always run the dry-run first and
  read it.
- No command prints a secret. Never `source` an env file, and never pass a
  secret on argv.

## 0. What changes, and why (the research this runbook rests on)

### A. Production identity for Next (deploy/fleet.toml `[production]`)

`deploy/core/render.py` gained an optional `[production]` table (branch
`cutover/production-identity`, commit `cdf6b48`, one file: `deploy/fleet.toml`).
Without the table, the render is byte-identical to today's (deploy/tests
snapshots). With it:

| What | Before (Next) | After (production) |
|---|---|---|
| Caddy sites | `app.next`, `world.next` | `app.forkmesh.com`, `world.forkmesh.com` with the app and world routes. `app.next`/`world.next` answer 308 to the production name, except `/readyz`, `/health` and `/api/version` on `app.next`, which `next_deploy`, `next_dns` and the node DNS reconciler still probe. Private Git `/git/*` is served on both app names. |
| certsync names | 2 | 4 (production names first) |
| `FORKMESH_PUBLIC_HOSTS` | next + nN | production + next + nN |
| `/etc/hosts` pin for the daemon | `app.next` | `app.forkmesh.com app.next.forkmesh.com` |
| daemon `catalogUrl`, runner `appUrl`, world `forkmesh-api-origin` meta, `--init` upstream | `app.next` | `app.forkmesh.com` |
| egress guard (`app/host/forkmesh_host/egress.py`) | refuses `*.forkmesh.com` by name | `FORKMESH_SERVES_PRODUCTION=1`: the name rule is off (Next must reach its own names and `mirror35–38`). The address rules stay. |
| app variables | `app/wrangler.toml [vars]` | `[production.vars]` rendered into `app.env` as `FORKMESH_VAR_*`, which overrides wrangler vars. render.py refuses any name that looks like a secret, is not a wrangler var, or is multi-line. |

`[production.vars]` holds production's exact values: `PUBLIC_BASE_URL`,
`API_ORIGIN`, `APP_ORIGIN`, `WWW_ORIGIN`, `WORLD_ORIGIN`,
`CORS_ALLOWED_ORIGINS`, `DISCORD_OAUTH_REDIRECT_URI`, `DISCORD_BUGS_*`,
`PATREON_OAUTH_REDIRECT_URI`, `COMMUNITY_REWARD_NETWORK=mainnet-beta`,
`COMMUNITY_REWARD_RPC_URL`, and `AP_OUTBOUND=1` (Next ships with
ActivityPub delivery off; production federates as forkmesh.com). It also
held `FLAGSHIP_RECOVERY_NODE=mirror29` until 2026-09-27, when that setting
was retired (`docs/MIRROR_RETIREMENT.md`, "Old tunnel mirrors").

**Names inside the code.** `tools/rehost.py` rewrote production hostnames
into Next names in the code: session cookie `Domain=.next.forkmesh.com`, the
dashboard's official-host sets, CORS fallbacks, the ActivityPub origin, and
mail links. Served under production names unchanged, web login breaks
(browsers reject the cookie domain). `tools/cutover_rehost.py` is the inverse
(`next.` → apex, `app.next`/`world.next`/`ssh.next` → production,
`.next.forkmesh.com` suffix → `.forkmesh.com`). It keeps the `nN.next` node
names and restores `www.forkmesh.com` where rehost folded it into a doubled
apex entry. It is applied to a **release worktree**, never to `main`, and
refuses a `main` checkout. `--compare` against production's checkout leaves
283 differing hostname lines in 56 files: mostly Next's own test fixtures
and comments, a few single-entry `www.`/`api.` lists, and `b.forkmesh.com`
in one list. Production's CORS and `WWW_ORIGIN` come back exactly through
`[production.vars]`, and `api.forkmesh.com` has no DNS record.

**Secrets** (`tools/cutover_identity.py`, reading production's
`app/.env.production` in memory). Compared against Next's `secrets.env`
without printing values:

| Name | Next today | Action |
|---|---|---|
| `MIRROR_ROUTER_SIGNING_SEED`, `MIRROR_ROUTER_PUBLIC_KEY` | Next's own | **adopt production's** (section B) |
| `DISCORD_BOT_TOKEN`, `TREASURY_SOLANA_ADDRESS`, `SSH_GATEWAY_{HOST,PORT,TOKEN,REPOSITORIES,NODE_HOSTS}` | missing | adopt; added to `optional_secrets` on the branch |
| `SENTRY_DSN` | same value, but not in the vault list | adopt (uncommented on the branch) |
| `DISCORD_CLIENT_*`, `APNS_*`, `MAILTRAP_API_TOKEN`, `ADMIN_PATH`, `PRODUCT_ANALYSIS_*` | same as production | re-checked, unchanged |
| `WWW_EDGE_SECRET` | Next's own | rotated to a new random value shared by the cores and production's `forkmesh-www` (`put-www-secret`) |
| `DATA_KEY`, `ADMIN_PASS` | Next's own | **kept** (the user chose re-key; section B) |
| `MAILTRAP_WEBHOOK_SECRET` | Next's own webhook | production's exists only as a Worker secret and cannot be read back; see step 16 |
| `PATREON_*` | none | **production has none either**: no Patreon credentials in `.env.production` or on the Worker, so there is nothing to switch |
| `TURNSTILE_SECRET_KEY` | Next's widget `forkmesh-next-signup` | production has no Turnstile. Next fails closed without it, so the widget must allow `forkmesh.com` and `app.forkmesh.com` (step 3) |

### B. Keys clients pin

| Key | Signer | Verified by | Pinned how | Next action |
|---|---|---|---|---|
| Mirror router Ed25519 (`MIRROR_ROUTER_*`) | the app (entry.py `_https_mirror_router_seed`: portable sessions, every mirror proxy capability, migration reports, Actions control) | Go gateway `server/identity.go:278` (`--verify-mirror-capability`), `actions_control.go:155`, `actions.go:1011`; `tools/mirror_gateway.py`; desktop QSettings `control/directMirrorRouterPublicKey` | fetched once at `--init` and pinned in `/var/lib/forkmesh/mirror-gateway/config.json` `routerPublicKey`; never re-fetched | **adopt production's pair** via `cutover_identity apply` + `next_vault sync`. Next's own cores pinned Next's key: `next_fleet install nN --steps daemon` now re-pins it from `secrets.env` and restarts the daemon (tools/next_fleet.py) |
| Mirror, desktop and phone node keys | the clients | the app, against D1 (`nodes`, `account_devices`, endpoints) | in D1 | come with the data (the re-key recomputes `node_bi`/`device_bi`) |
| ActivityPub instance and actor RSA keys | the app | remote servers (cached `keyId`) | D1 `ap_service_keys`/`ap_actors`, encrypted | come with the data, re-encrypted; ids stay `https://forkmesh.com/...` through `PUBLIC_BASE_URL` |
| relay identity (`relay_self`) | the app | the main relay | D1 | comes with the data |
| APNs | the app | Apple | – | already production's values |
| release signing | offline / desktop | install.sh | pinned in `install.sh` | not on the server; unaffected |
| TLS | – | – | **no client pins a certificate or CA** (Go system roots, Qt default, Android without a pin-set, iOS without pinning) | a Let's Encrypt certificate is fine |

**DATA_KEY.** Next keeps its own key (key_id `974e5c3f`). Production's
(`03325227`) is read only into the re-key process from `.env.production:DATA_KEY`
(`next_rekey.py --old-key-env`). It is never written into the Next config.
Nothing a client *verifies* is derived from `DATA_KEY`. What the re-key
changes (docs/REKEY.md 1.3) is the set of things clients *hold*:

- **Every session ends.**
  - Browser, desktop and phone users log in again.
  - Phones treat the 401 as a revoked device. The native apps'
    `handleUnauthorized` clears the session, **rotates the phone's Ed25519
    key** and shows the "revoked" banner. Expect a support wave and new
    device rows. (Decision D1 at the end.)
- **Room chat passphrases** (`SHA-256(DATA_KEY+suffix)`) are cached by
  running clients. Desktops keep the old one until restart and mobile until
  app restart. Until then those clients cannot read new room messages.
- **Re-issue:**
  - hosted-git push passwords;
  - repository invitation, password-reset and email-verify links;
  - org bot tokens and channel webhook URLs whose secret was not stored.
- **Unaffected:** Go mirror daemons authenticate only with their node key,
  so they keep working.

### C. Read-only and decommission switches on the production Worker

There was none (research: no `MAINTENANCE`/`READ_ONLY`/`DECOMMISSION` anywhere
in production). Removing the crons alone would **not** stop the old stack:
`ForkMeshCronRunner._coordinator_alarm` re-arms itself every minute, and
`ForkMeshCronWatchdog` would send an outage alert about 3 minutes after the
last tick. The patch (production repo, branch `cutover/readonly-and-www-origin`,
**not deployed**) adds:

- `FORKMESH_READ_ONLY=1` (Worker var):
  - GET/HEAD/OPTIONS and git clone reads (`git-upload-pack`) keep working.
  - Every other request gets 503 with `Retry-After: 120` (expected-degraded
    headers, so it is not logged as an error). WebSocket upgrades are refused.
  - Writes that GET handlers make are skipped: session touch, attribution,
    referral and blog counters, api metrics, DO traffic flush, error logging,
    schema DDL.
  - A `_d1_write_guard` in every D1 helper is the backstop.
  - Background work stops: cron, queue, every DO alarm, watchdog, Discord gate.
- `FORKMESH_DECOMMISSIONED=1`: everything answers 503 and does nothing. Alarms
  delete themselves.
- A decommission wrangler variant with no `[[routes]]`, `crons = []` and no
  queue consumer.
  - **Never deploy the stock `app/wrangler.toml` or `world/wrangler.toml`
    after the DNS flip.** Their `custom_domain = true` routes would try to
    re-claim `app.forkmesh.com`/`world.forkmesh.com`.

### D. TLS before the flip (`tools/cutover_tls.py`)

Today both names are Worker custom domains, so HTTP-01 for them reaches the
Worker, not a core. Without a certificate in place, certsync serves
`internal` (self-signed) until it issues one after the flip, and every client
fails TLS for those minutes.

Choice: **DNS-01 from the desktop.**
- The existing zone token is used through a narrow guard: TXT only, exactly
  `_acme-challenge.app.forkmesh.com` / `_acme-challenge.world.forkmesh.com`,
  and only records this run created, re-checked by id before delete.
- One certificate per name, since certsync keeps one pair per name.
- Each pair is checked with certsync's own validator: key matches, chains
  to a system CA, covers the name, not after, and 100 days at most.
- Each pair is pushed over SSH stdin into `/etc/forkmesh-next/tls/<name>.{key,crt}`
  (0600 caddy) on every core. That path is IDS-excluded (render.py
  `IDS_EXCLUDE`), because certsync writes it itself.

Caddy 2.6.2 plus certsync load it as-is: `local_pairs()` reads `tls_dir`, and
the pair wins `freshest`. Once `[production]` adds the names, the mode is
`files` (tested: `test_pushed_pair_is_served_by_certsync_before_the_flip`).
Renewal 30 days before expiry is ordinary leaderless HTTP-01, because by
then the names point at the cores.

Rejected alternatives:
- Worker-forwarded `/.well-known/acme-challenge`: needs a Worker deploy, and
  Workers cannot fetch a core by IP with the right Host.
- A brief proxied phase: Cloudflare would terminate TLS, and websocket and
  git-body limits would change.

### E. DNS flip (`tools/cutover_dns.py`)

Today (read-only `plan`, 2026-09-24):
- `app.forkmesh.com` is a custom domain of `forkmesh-app` and
  `world.forkmesh.com` of `forkmesh-world`. Each has a managed, proxied
  `AAAA 100::` record (TTL auto = 300 s).
- `forkmesh.com`/`www` belong to `forkmesh-www` (and stay).
- There is no `api.forkmesh.com` record.
- Zone SOA `min_ttl` = **1800 s**.

`flip` detaches each custom domain (Cloudflare removes the managed record)
and immediately creates three unproxied A records (TTL 60). The detach and
create calls are back to back, and a resolver asking in that sub-second gap
caches "no record" for the SOA minimum, so step 5 lowers it to 60 first.
- **Guards:** only those two names; A content must be a core IP from
  `deploy/fleet.toml`; custom domains only on `forkmesh-app`/`forkmesh-world`;
  it refuses unexpected records.
- **State:** saved in `$CFG/cutover-dns-state.json`, which `rollback` uses.
- **Credentials:** the same account token production deploys with.
  `app/deploy.sh` reads `CLOUDFLARE_API_TOKEN`/`CLOUDFLARE_ACCOUNT_ID` from
  `app/.env.production`, and `$CFG/cloudflare.env` holds the same token
  (checked equal). Wrangler OAuth is not used (`FORKMESH_USE_WRANGLER_OAUTH`
  unset).

### F. forkmesh.com (www)

Production's `forkmesh-www` reaches the app through a **service binding**
(`www/wrangler.toml` `APP` → `forkmesh-app`). A service binding ignores DNS,
so after the flip `forkmesh.com/api/*` would still hit the old Worker and D1
(split brain). The production patch adds `APP_UPSTREAM=origin`: fetch
`APP_ORIGIN` with the HMAC-signed `x-forkmesh-edge-client` header
(`WWW_EDGE_SECRET`, byte-compatible with the Next host's verifier), fall back
to the `nN.next` nodes, and keep `binding` as the default and the rollback.

`next.forkmesh.com`: `www/wrangler.redirect.toml` deployed
`www/redirect_worker.js` under the same script name (`forkmesh-next-www`);
both files were removed on 2026-09-29 (below, "forkmesh-next-www deleted
again"). It
has no assets and sends a 308 for every request to the same path on
`https://forkmesh.com`. Rollback is a plain `wrangler deploy`.

Since 2026-09-25 forkmesh.com deploys from `main` with
`www/wrangler.production.toml`. It is the same Worker name (`forkmesh-www`)
and the same custom domains (`forkmesh.com`, `www.forkmesh.com`), with
`APP_UPSTREAM=origin` and the `nN.next` fallback origins written into the
file instead of passed as `--var`s. Main's `worker.js` fetches the cores
directly, so it needs no `forkmesh-app` service binding. Before this,
production www was deployed by hand from `cutover/deploy-readonly`, so www
work merged into `main` (the Ecosystem pages, the #704 homepage note) never
went live.

Checked 2026-09-27: `cutover/deploy-readonly` is 11 commits ahead of `main`
and nothing live depends on them.

- `main` already has the blog post (`4b939eb3f`) and the manual-only
  `deploy.yml` (`5f252f6d3`).
- The App Worker read-only and decommission switches (`e34abe819`,
  `f5810b6b2`) only mattered for the retired Cloudflare Workers. `main`'s www
  `worker.js` reaches the cores through `APP_ORIGIN` by its own code.
- `c9d64bf73` is a macOS diagnostic, not for merge.
- The four native/ios macOS test fixes (`625eefc04`, `c6be40528`,
  `67d2a3cc7`, `ed437f3a7`) are not in `main`. They belong to native/ios work,
  not to the served stack.

The branch can be retired once someone has taken or dropped those iOS fixes.

Done 2026-09-27: the four iOS fixes were cherry-picked onto the fleet
reliability branch (`463-5412cf3b…`), with the transport fix merged by hand
around main's `surfaceRedirects` and `onBodyBytes`. On Linux the
`LocalEngineTransportTests`, `HuggingFaceSnapshotTests` and
`LocalModelManagerTests` pass (30 tests). Their worktrees were removed.
Local tags `archive/cutover-deploy-readonly` (`5f252f6d3`) and
`archive/cutover-readonly-and-www-origin` (`f5810b6b2`) keep the commits.

Rechecked 2026-09-27 for `cutover/readonly-and-www-origin`. The one piece
`main` lacked was the cross-check between the www Worker's signed
`x-forkmesh-edge-client` header and the App's verifier. On the branch it loaded
the verifier from the retired `forkmesh-next` checkout. It now runs against
`app/host` as `app/tests/host/test_edge_client_crosscheck.py`
(`deploy/core/edge_client.md`). Both `cutover/*` branches are still on
`origin` and in the local clone. Deleting them is left to the user.

Checked again later on 2026-09-27: `git ls-remote` against
`n1.next`, `n2.next` and `n3.next` still lists `cutover/deploy-readonly`
(`5f252f6d3`) and `cutover/readonly-and-www-origin` (`f5810b6b2`) as
branches, next to the `archive/*` tags on the same commits. The deletion did
not hold, or a clone that still has them pushed them back. Nothing in them is
missing from `main` or the fleet branch above, so the operator can delete both
branches from the mirror again (the tags keep the commits) and check which
clone re-pushes them.

```sh
cd www && npx --yes wrangler@4.136.2 deploy --config wrangler.production.toml \
  --var DEPLOY_FINGERPRINT:$(python3 ../app/tools/deploy_targets.py fingerprint www)
```

Wrangler 4.136 needs Node 22 or newer. `WWW_EDGE_SECRET` stays a Worker
secret and carries across deploys. To roll back: `npx wrangler rollback --name
forkmesh-www`.

### G. Mirrors, desktops, relays after the flip

- **Go daemon (mirror35–38).**
  - It holds one `/api/nodes/events` WebSocket and re-dials by name on every
    reconnect (1 s → 60 s backoff). Go does not cache DNS.
  - Its HTTP calls use the default transport, which keeps idle connections
    for 90 s. A daemon that calls more often than that keeps reusing its
    connection to Cloudflare.
  - The read-only deploy restarts the Durable Objects and drops the sockets,
    and the upgrades are then refused until the flip.
  - **Restart `forkmesh-mirror-node` on mirror35–38 after the flip** (step 20).
    Hosts → Update is **not** needed: it is a full reinstall, and the router
    key is production's, so nothing has to be re-pinned on a production mirror.
- **Desktop relay.**
  - It uses a hand-built WebSocket with 25 s pings and reconnect backoff up
    to 300 s, resolving through Qt's resolver (about 60 s cache).
  - It reconnects on its own. Users log in again (re-key).
  - Desktops still running keep the old room-chat passphrase until restart.
- **Phones.** They reconnect on their own. There is no TLS pinning. The re-key
  logs them out (see B).
- **Repo serving and catalog.**
  - The production export carries mirror35–38's `nodes` and
    `mirror_https_endpoints` rows. Next's cores (`fmnext-n1..3`, owner
    account `fmnext`) and runner `fmnext-r1` exist only in Next, so they
    would vanish with the load: `cutover_data carry` copies exactly those
    rows (plus the Next-only tables) into the final file.
  - Cores are routed only for hosted repositories (`hosted_repos`, Next-only,
    carried). Production repositories keep routing to production mirrors by
    their signed repository proofs.
  - No conflict: the cores never claim a production repository.

### H. Capacity (Cloudflare GraphQL, read-only, 2026-09-24)

| Worker / host | 7 days | average | peak hour |
|---|---|---|---|
| `forkmesh-app` | 99,045 requests | 0.16 req/s | 2,934/h = 0.81 req/s |
| `forkmesh-www` | 17,582 | 0.03 req/s | 0.15 req/s |
| `forkmesh-world` | 3,020 | – | 0.04 req/s |
| zone, all hosts (3 days) | ~83,000 | ~0.32 req/s | – |
| D1, 24 h | 174k read queries, 24k write queries | – | – |

Next on one 1 GB core (docs/PHASE0_MEASUREMENTS.md):

| Measure | Result |
|---|---|
| API reads p50 | 1.2–10.7 ms |
| `/api/ledger` p50 (slowest route) | 32 ms |
| login (PBKDF2) | 128 ms |
| rqlite write p50 | 1.7 ms |
| 300 WebSockets | 140 MB RSS |
| memory available | 638 MB |

Even the slowest measured route gives each node about 30 req/s, so three
nodes handle over 100x production's peak. **Recommendation: do not resize
the cores before the cutover.** Keep `next_fleet.py replace-core` ready
(one core at a time, quorum-safe) if memory or event-loop lag alarms.

### I. Data move (measured, see "Rehearsal results")

1. `wrangler d1 export`: read-only. It **blocks D1 for other requests while
   it runs** (Cloudflare docs); 8.6 s at 37 MB.
2. Load into SQLite.
3. `next_rekey` from production's key (env file, memory only) to Next's key.
4. Load into a local rqlite, run `rqlite_migrate --app-schema`, take `/db/backup`.
5. `cutover_data carry` the Next-owned rows.
6. `cutover_data verify` (every account by name and e-mail, every node by
   name, against the pre-re-key baseline).
7. `cutover_data login-probe` (real HTTP logins, locally).
8. Load through the Raft leader.

### J. Status page, monitoring, IDS

- **Status page.** `/status` is served by the app. The cron status probes
  run on the cores (rotating coordinator), so status continues from the new
  data. `x-forkmesh-expected-degraded` 503s from the old Worker are not logged.
- **Sentry** becomes production's DSN (vault). Watch it for the first hour.
- **IDS.** Every node change goes through `next_fleet`/`next_deploy`, which
  push signed expected-change manifests:
  - the `files` step: exact sha256 of the new Caddyfile, `app.env`,
    `certsync.json`, `hosts.fragment` and overrides;
  - the `daemon` step;
  - `next_deploy restart`.
  - The TLS push writes only the IDS-excluded `tls/`. SSH logins by the
    fleet tools show up as `ssh_tool_session` (info).
  - After node work: `python3 tools/next_ids.py status n1 n2 n3 r1`, with
    no new `alert`/`lockdown` incidents.

## 1. Preconditions (day before or hours before; no production impact)

Go/no-go for all of them: every check passes, or the cutover waits.

1. **Production patches reviewed and merged** into production `main`
   (branch `cutover/readonly-and-www-origin`, commit `e34abe8`, plus its
   comment-strip follow-up), and CI passes.
   - The patch also adds `app/tests/test_maintenance_switches.py` (65
     tests), `www/tests/app_origin.test.mjs`, and a cross-check that the
     production www signature verifies with Next's `EdgeClientVerifier`.
   - Check: `cd $PR && git log --oneline -3 origin/main` shows the merge.
2. **Port the production feature Next lacks.** Production has 29 commits
   since Next's seed (`c48046db2`). Of these, only `d578fbc2a` ("agents:
   image attachments on website-queued jobs"; migration
   `0191_org_agent_attachments.sql`) touches app code or migrations. The
   production export records that migration, and its tables arrive with the
   data but unused by Next's code.
   - Port it to Next main under the **next free migration number** (0200 is
     taken by `0200_apps.sql`; the SQL is idempotent `CREATE ... IF NOT EXISTS`).
   - Check: `ls $NX/app/migrations | grep org_agent_attachments`.
   - Or accept the gap (decision D5).
3. **Turnstile.** Add `forkmesh.com` and `app.forkmesh.com` to the widget
   `forkmesh-next-signup` (Cloudflare dashboard → Turnstile, or the API with
   the account token).
   - Expected: the widget's domains list 4 names.
   - Without this, signups and password resets fail after the flip, because
     the VPS fails closed.
4. **Rehearsal passes on the day's data.** Run it locally; production is
   touched only by the read-only 9 s export:
   ```sh
   cd $NX && umask 077
   W=~/projects/forkmesh-backup/data/cutover-rehearsal/$(date -u +%Y%m%dT%H%M%SZ)
   .venv/bin/python tools/next_backup.py run --execute        # fresh Next backup (nightly job; takes ~30 s)
   B=$(ls -1 ~/projects/forkmesh/data/fleet/backups/rqlite-*.sqlite3.gz.age | tail -1)
   mkdir -p -m 0700 $W && .venv/bin/python tools/next_backup.py restore "$B" --out $W/next.sqlite3 --identity $CFG/backup-age.key
   .venv/bin/python tools/cutover_data.py rehearse --work $W/run --execute-export \
       --next-backup $W/next.sqlite3 --seed-probe
   ```
   - Expected: `verify: OK`, four `login-probe: ... OK` lines, and
     `rehearsal: ~140 s of data work`.
   - No-go if the re-key aborts: read the error, fix the tool, never
     `--allow-orphans` blindly.
5. **Build the production release** (not activated). Build from a release
   worktree: Next `main` plus the fleet switch plus the inverse rehost.
   ```sh
   cd $NX && git fetch -q . && REL=~/projects/forkmesh-backup/worktrees/release-production
   git worktree add -b release/production-$(date -u +%Y%m%d) $REL main
   cd $REL && git cherry-pick cdf6b48                                  # [production] in fleet.toml
   python3 tools/cutover_rehost.py --apply --root .                     # production names in the code
   git commit -qam "release: production names (tools/cutover_rehost.py)"
   python3 tools/cutover_rehost.py --check                              # expect: no "would rewrite" lines
   (cd app && python3 -m pytest -q tests/host/test_host_e2e.py)         # the release itself works
   python3 tools/next_deploy.py build --execute && python3 tools/next_deploy.py upload --execute
   ```
   - Expected: `upload` prints the release dir on n1..n3. Nothing restarts.
   - **All later `next_fleet`/`next_deploy` commands run from `$REL`**, whose
     `deploy/fleet.toml` has `[production]`.
6. **Certificates** (DNS-01; creates and deletes two TXT records, touches no
   serving record):
   ```sh
   cd $REL && python3 tools/cutover_tls.py issue                         # dry-run
   python3 tools/cutover_tls.py issue --execute --contact <ops e-mail>
   ```
   - Expected: `issue: app.forkmesh.com: notAfter <~90 days>, issuer CN=R1x/E7...`
     and the same for world.
   - Then push, which is harmless: certsync ignores names outside its
     `names` until the `files` step:
     ```sh
     python3 tools/cutover_tls.py push && python3 tools/cutover_tls.py push --execute
     python3 tools/cutover_tls.py status
     ```
   - Expected: each node lists both names with `notAfter=...`.
   - Go/no-go: all three cores hold both pairs.
7. **Identity values into `secrets.env`** (desktop file only; the vault is
   synced inside the window):
   ```sh
   python3 tools/cutover_identity.py plan
   python3 tools/cutover_identity.py apply --execute
   ```
   - Expected: `plan` shows the router pair `replaced by production's`,
     `DISCORD_BOT_TOKEN new`, and so on. `apply` reports
     `secrets.env: 20 value(s) written ...; previous copy in secrets.env.pre-cutover`.
8. **Lower the negative-caching TTL** at least 30 minutes before the flip
   (zone-wide; affects only how long "no such record" is cached):
   ```sh
   python3 tools/cutover_dns.py soa-min-ttl 60 && python3 tools/cutover_dns.py soa-min-ttl 60 --execute
   ```
   - Expected: `zone SOA min_ttl 1800 -> 60`.
   - If the API refuses the PATCH, set it in the dashboard (DNS → Settings
     → SOA).
9. **Announce** a 15-minute window in which writes pause and everyone must
   log in again afterwards.

## 2. The cutover (read-only window)

Timing is from the rehearsal and live estimates; see "Window". T = the
moment production turns read-only.

10. **LIVE: production read-only** (T+0). Deploy the patched Worker with the
    switch. `FORKMESH_EXTRA_VARS` adds `--var` flags to the upload.
    `FORKMESH_RECOVER_UNAVAILABLE_HTTP=1` skips the snapshot-freshness wait,
    which cannot pass once the cron is off:
    ```sh
    cd $PR/app && FORKMESH_EXTRA_VARS="FORKMESH_READ_ONLY:1" FORKMESH_FORCE_DEPLOY=1 FORKMESH_RECOVER_UNAVAILABLE_HTTP=1 ./deploy.sh app
    curl -s -o /dev/null -w '%{http_code}\n' -X POST https://app.forkmesh.com/api/accounts/login   # 503
    curl -s -o /dev/null -w '%{http_code}\n' https://app.forkmesh.com/api/version                  # 200
    ```
    - Go/no-go: POST → 503 with `Retry-After`, and GET → 200.
    - Wait 60 s: the cron runner and queue must be idle. `wrangler tail
      forkmesh-app` must show no `cron phase` lines.
11. **Stop Next's apps and timers** (T+1 min). rqlite keeps running:
    ```sh
    systemctl --user stop fmnext-unseal.timer fmnext-backup.timer
    for ip in $CORES; do ssh -i $K root@$ip systemctl stop forkmesh-app; done
    cd $REL && python3 tools/next_backup.py run --execute
    B=$(ls -1 ~/projects/forkmesh/data/fleet/backups/rqlite-*.sqlite3.gz.age | tail -1)
    W=~/projects/forkmesh-backup/data/cutover-$(date -u +%Y%m%dT%H%M%SZ); mkdir -m 0700 $W
    python3 tools/next_backup.py restore "$B" --out $W/next.sqlite3 --identity $CFG/backup-age.key
    ```
    - Expected: `restored ...: d1_migrations=..., repositories=...`.
    - This backup is also the rollback point for Next's own state.
12. **Final export and data move** (T+2 → T+5 min; measured 136 s of data work):
    ```sh
    python3 tools/cutover_data.py rehearse --work $W/move --execute-export --next-backup $W/next.sqlite3
    ```
    - This runs the same pipeline as the rehearsal, on the real final
      export, and produces `$W/move/final.sqlite3`.
    - Expected: `phase export ~9 s`, `phase rekey ~130 s`, `verify: OK`
      (the NOTE about 2 pre-existing e-mail index mismatches is fine), and
      `login-probe: fmnext by name/email: OK`.
    - Go/no-go: `verify: OK` and every `login-probe` OK. Otherwise
      **rollback R1**.
13. **Load through the leader** (T+6 min):
    ```sh
    ssh -i $K root@$N1_IP "curl -fsS -K /etc/forkmesh-next/rqlite.curlrc 'http://127.0.0.1:4001/nodes?pretty'" | grep -B2 '"leader": true'
    LEADER=<leader ip>
    scp -i $K $W/move/final.sqlite3 root@$LEADER:/root/final.sqlite3
    ssh -i $K root@$LEADER "curl -fsS -K /etc/forkmesh-next/rqlite.curlrc -XPOST -H 'Content-Type: application/octet-stream' \
       --data-binary @/root/final.sqlite3 http://127.0.0.1:4001/db/load && shred -u /root/final.sqlite3"
    for ip in $CORES; do ssh -i $K root@$ip "curl -fsS -K /etc/forkmesh-next/rqlite.curlrc \
       -G 'http://127.0.0.1:4001/db/query?level=none' --data-urlencode 'q=SELECT COUNT(*) FROM users'"; done
    ```
    - Go/no-go: the same user count on all three nodes, equal to
      `verify: users=`.
14. **Vault and node files** (T+7 → T+11 min). Apps are stopped. Each
    command below pushes its IDS expectation first:
    ```sh
    cd $REL
    python3 tools/next_vault.py sync --prune --no-reload --execute       # production router key, Discord bot, Sentry, ... under Next's key
    for n in n1 n2 n3; do python3 tools/next_fleet.py install $n --steps files --execute; done
    for n in n1 n2 n3; do python3 tools/next_fleet.py install $n --steps daemon --execute; done   # re-pins routerPublicKey
    python3 tools/next_deploy.py restart --execute                       # activates the uploaded release, gates on /readyz + /api/version, unseals
    python3 tools/next_vault.py status
    for n in n1 n2 n3; do python3 tools/next_ids.py status $n; done
    ```
    - Expected:
      - `sync` lists the added names;
      - every node `unsealed`, with rows `(matches)` key_id `974e5c3f`;
      - the daemon step prints `router public key re-pinned from the vault
        value` once per core;
      - no new IDS alert.
    - Go/no-go: all three nodes ready. Otherwise rollback R1 (production is
      still read-only and untouched).
15. **Probe the cores directly, before DNS** (T+12 min):
    ```sh
    for ip in $CORES; do
      curl -sS --resolve app.forkmesh.com:443:$ip https://app.forkmesh.com/readyz -o /dev/null -w "$ip %{http_code} %{ssl_verify_result}\n"
      echo | openssl s_client -connect $ip:443 -servername app.forkmesh.com 2>/dev/null | openssl x509 -noout -issuer -enddate
    done
    curl -sS --resolve app.forkmesh.com:443:$N1_IP -X POST https://app.forkmesh.com/api/accounts/login \
      -H 'content-type: application/json' -H 'origin: https://app.forkmesh.com' --data @<(python3 - <<'PY'
    import json, pathlib
    v = dict(l.split("=", 1) for l in pathlib.Path.home().joinpath("projects/forkmesh/config/fleet/next-admin.env").read_text().split() if "=" in l)
    print(json.dumps({"nodeName": v["NEXT_ADMIN_EMAIL"], "password": v["NEXT_ADMIN_PASSWORD"]}))
    PY
    ) -o /dev/null -w 'login by email %{http_code}\n'
    ```
    - Expected: `200 0` from each node, a Let's Encrypt issuer, and
      `login by email 200`.
    - Also log in with your own production account by name through the
      same `--resolve`.
    - Go/no-go: all pass. Otherwise rollback R1.
16. **Mailtrap webhook** (T+13 min). Production's webhook points at
    `app.forkmesh.com`, and its secret is unreadable (Worker-only), so after
    the flip its events would fail verification on Next.
    - In Mailtrap, change Next's webhook (id in `$CFG/mailtrap-webhook.json`)
      to the production URL (same path, host `app.forkmesh.com`).
    - Delete production's webhook.
    - Next verifies with its own `MAILTRAP_WEBHOOK_SECRET`, which is already
      in the vault.
17. **LIVE: DNS flip** (T+14 min):
    ```sh
    cd $REL && python3 tools/cutover_dns.py flip          # dry-run: expect "flip -> detach ..., then A <3 IPs>" for both names
    python3 tools/cutover_dns.py flip --execute
    python3 tools/cutover_dns.py plan                     # "already flipped to the cores" x2
    for r in 1.1.1.1 8.8.8.8 9.9.9.9; do dig +short @$r app.forkmesh.com A; done
    ```
    - Expected: three core IPs from every resolver within about 5 minutes
      (the old records' TTL was 300 s).
    - Write traffic starts arriving on Next: **the window closes** (about
      T+15 min).
18. **www origin mode and secret** (T+15 min, right after the flip; until
    the vars change the service binding keeps serving the old, read-only
    Worker):
    ```sh
    cd $REL && python3 tools/cutover_identity.py put-www-secret && python3 tools/cutover_identity.py put-www-secret --execute
    cd $PR/www && npx --yes wrangler@4.136.2 deploy --config wrangler.toml --var APP_UPSTREAM:origin \
      --var APP_FALLBACK_ORIGINS:https://n1.next.forkmesh.com,https://n2.next.forkmesh.com,https://n3.next.forkmesh.com \
      --var DEPLOY_FINGERPRINT:$(python3 ../app/tools/deploy_targets.py fingerprint www)
    curl -s https://forkmesh.com/api/version | head -c 200          # "rev": the Next release's BUILD_REV
    ```
    - Go/no-go: `forkmesh.com/api/version` shows the Next rev.
    - Why after the flip: a Worker's `fetch("https://app.forkmesh.com")`
      follows the zone's DNS. Before the flip it would reach the old
      Worker's custom domain; after it, the cores' A records.
19. **Next homepage redirect**:
    ```sh
    cd $NX/www && npx wrangler deploy --config wrangler.redirect.toml
    curl -sI https://next.forkmesh.com/blog | grep -i '^location'   # https://forkmesh.com/blog
    curl -sI https://app.next.forkmesh.com/dashboard | grep -i '^location'   # https://app.forkmesh.com/dashboard
    ```
20. **LIVE: mirrors reconnect** (coordinator; production mirrors):
    `systemctl restart forkmesh-mirror-node` on mirror35..38, one at a time.
    This drops pooled keep-alive connections to Cloudflare.
    - Expected: `journalctl -u forkmesh-mirror-node -n 20` shows the node
      events socket connected.
    - Status → mirrors shows all four with fresh `last_seen`.
21. **Old stack off** (T+30 min, after the DNS tail). Deploy the
    decommission variant: no routes, `crons = []`, no queue consumer,
    `FORKMESH_DECOMMISSIONED=1`.
    - Never through `deploy.sh`: its verify steps would hit Next.
    - **Never the stock wrangler.toml** (its custom-domain route would
      re-claim `app.forkmesh.com`). The same goes for `./deploy.sh world`
      and `world.forkmesh.com`.
    - Run it in a subshell with `CLOUDFLARE_API_TOKEN`/`CLOUDFLARE_ACCOUNT_ID`
      exported from `.env.production`, read with Python, not `source`:
    ```sh
    cd $PR/app && python3 tools/decommission_wrangler_toml.py -o wrangler.decommissioned.toml
    . ./pywrangler.sh && pywrangler deploy --config wrangler.decommissioned.toml --env "" --name forkmesh-app
    ```
    - Leave `forkmesh-world` as it is. It has no crons and is unrouted after
      the flip.
    - Keep D1 `forkmesh` and both Workers for 14 days (decision 4).
22. **Timers back on and the SOA back up**:
    ```sh
    systemctl --user start fmnext-unseal.timer fmnext-backup.timer
    cd $REL && python3 tools/cutover_dns.py soa-min-ttl 1800 --execute
    shred -u $W/move/export.sql $W/move/export.sqlite3 $W/move/rekeyed.sqlite3 $W/next.sqlite3; rm -rf $W/move/rekey-work
    ```
    - Keep `$W/move/final.sqlite3` (Next-key ciphertext) until the first
      nightly backup verifies, then delete it by name.

### Window

| Phase | Measured (rehearsal) | Live estimate |
|---|---|---|
| read-only deploy + idle check | – | 1–2 min |
| stop apps, Next backup + restore | backup 34 MB | 1 min |
| export (D1 blocked for its duration) | 8.6 s | 10 s |
| SQL → SQLite | 1.5 s | 2 s |
| re-key (61k rows, 3,951 ciphertexts, 59,578 blind indexes; verify with 4.28 M probes) | 127 s | 130 s |
| local rqlite load / migrate / backup / carry / verify / reload / login probe | 0.9 / 4.4 / 0.5 / 0.1 / 0.2 / 0.6 / 1.3 s | 10 s |
| upload 34 MB to the leader + `/db/load` + counts | 0.8 s local load | 30 s |
| vault sync, `files` ×3, `daemon` ×3, rolling restart with gates | – | 4–5 min |
| direct probes, Mailtrap | – | 1–2 min |
| DNS flip, then www origin mode | sub-second per name | 2 min |
| **Writes paused** | | **about 12–15 min** (target 15, abort threshold 25) |
| DNS tail: resolvers holding the old record | | up to 5 min more. Those clients keep reading, and their writes get 503 + `Retry-After` (not lost) |

## 3. Post-cutover checks (J)

| Check | How | Expected |
|---|---|---|
| login by name | website login with a production account name | dashboard loads; `Set-Cookie ... Domain=.forkmesh.com` |
| login by e-mail | the same with its e-mail | same |
| push and clone through a production mirror | `git clone https://mirror35.forkmesh.com/forkmesh/forkmesh /tmp/c && git -C /tmp/c log -1`; push a branch to a test repo through the app (desktop or `git push` via the app URL) | clone OK; the push is accepted. It needs a router-signed capability, which proves mirrors accept Next's (production's) router key |
| desktop relay | a desktop shows connected; a second device sees its presence | connected within 5 min (backoff) |
| world | `https://world.forkmesh.com/world/` loads; `wss://world.forkmesh.com` socket welcomes a guest | `{"type":"ready"}` |
| chat WebSockets | `python3 - <<'PY'` with `websockets.sync.client.connect("wss://app.forkmesh.com/api/status/ws")` | `ready`, ping/pong |
| Mailtrap e-mail | request a password reset for your own account | mail arrives; `mailtrap_email_sends` gains a row; the Mailtrap webhook delivers `delivery` (200) |
| APNs | mention your own account from another account with an iPhone signed in | push arrives |
| Discord bot | post a test report in the bugs channel `1531718105124831304` | an issue appears in forkmesh/forkmesh within a minute (cron phase `discord-bugs`) |
| ActivityPub | `curl -s -H 'Accept: application/activity+json' 'https://forkmesh.com/.well-known/webfinger?resource=acct:forkmesh@forkmesh.com'`, then fetch the `self` link; search `@forkmesh@forkmesh.com` from a Mastodon account | JRD with `https://forkmesh.com/...` ids; the remote sees the actor and posts |
| status page | `https://forkmesh.com/status` | all components green within 5 minutes; mirrors listed |
| IDS | `python3 tools/next_ids.py status n1 n2 n3 r1` | `ok`, no new incidents |
| Sentry | production project | no new error spike |
| old Worker quiet | `wrangler tail forkmesh-app` after step 21 | only 503s from stragglers; no cron, queue or alarm lines |

## 4. ROLLBACK

The router key is production's on both stacks, and the old Worker and D1
are untouched (D1 received no write after step 10). So a rollback never needs
a client change. Pick by how far the cutover got.

### R1: before the DNS flip (steps 10–17 failed)

Production data is unchanged and only paused.
1. `cd $PR/app && FORKMESH_FORCE_DEPLOY=1 ./deploy.sh app`: the normal deploy, without
   `FORKMESH_READ_ONLY`. Production writes again. The custom domains were
   never detached.
2. (www is still on its service binding: step 18 comes after the flip.)
3. Put Next back on its own identity. Do it before its apps run with
   production's secrets against production data:
   ```sh
   cd $NX                                    # main: fleet.toml without [production]
   python3 tools/cutover_identity.py rollback --execute
   for ip in $CORES; do ssh -i $K root@$ip systemctl stop forkmesh-app; done
   # restore Next's own database from step 11 (docs/REKEY.md step 5 commands, with $W/next.sqlite3)
   python3 tools/next_vault.py sync --prune --no-reload --execute
   for n in n1 n2 n3; do python3 tools/next_fleet.py install $n --steps files,daemon --execute; done
   python3 tools/next_deploy.py all --execute   # main's release (Next names)
   ```
4. `cutover_dns.py soa-min-ttl 1800 --execute`.

### R2: after the flip, minutes in, nothing important written yet (emergency)

Accept losing Next's writes since the flip, or replay them by hand from the
logs:
```sh
cd $REL && python3 tools/cutover_dns.py rollback && python3 tools/cutover_dns.py rollback --execute
cd $PR/www && npx --yes wrangler@4.136.2 deploy --config wrangler.toml --var APP_UPSTREAM:binding
cd $PR/app && FORKMESH_FORCE_DEPLOY=1 ./deploy.sh app   # normal, writable; custom domains are back from the DNS rollback
```
- `rollback` deletes exactly the A records the flip created and re-attaches
  `app`→`forkmesh-app` and `world`→`forkmesh-world` from the saved state.
- Since 2026-09-24 the `fmnext-dns-health` loop and `next_dns.py` also write
  these names. They have replaced several of the flip's A records and added
  AAAA records. Before a DNS rollback, run
  `systemctl --user stop fmnext-dns-health`, then delete the names' A and
  AAAA records that `rollback` reports missing or leaves behind, before the
  custom domains are re-attached.
- Then do R1 step 3 for Next.
- Users whose sessions were issued by Next must log in again. Their
  production sessions were never deleted in D1.

### R3: after the flip, with writes on Next (within the 14 days)

A full reverse data move, about 15–20 minutes of downtime.
1. Stop Next's apps: `systemctl stop forkmesh-app` on each core. This is a
   hard pause; Next has no read-only switch.
   - Before relying on R3, port the production read-only patch to Next.
     It is the same `app/src` code.
2. Take a verified backup:
   `python3 tools/next_backup.py run --execute` and `restore` it to `$W/back.sqlite3`.
3. Re-key back to production's key:
   ```sh
   python3 tools/next_rekey.py run --in $W/back.sqlite3 --out $W/back-prod.sqlite3 \
     --old-key $CFG/DATA_KEY --new-key-env $PR/app/.env.production:DATA_KEY
   ```
4. Drop the Next-only tables (`cutover_data.NEXT_TABLES` plus
   `runner_request_nonces`, `runner_login_*`) and the Next-only `d1_migrations`
   rows 0191_chat…–0199. Then dump:
   `sqlite3 $W/back-prod.sqlite3 .dump > $W/back.sql`.
5. D1: `wrangler d1 export forkmesh --remote` first (the rollback's rollback),
   then empty and import:
   `wrangler d1 execute forkmesh --remote --file $W/drop-all.sql` followed by
   `--file $W/back.sql`. Production's credentials come in the environment,
   exactly as `cutover_data export` does.
6. Run R2: DNS rollback, www binding, normal app deploy. Then R1 step 3.

Everyone logs in again (the re-key). Hosted repositories and runner
registrations exist only on Next and stay there.

## 5. Decisions and blockers for the user

- **D1: Re-key means mass logout, and phones rotate their device keys.**
  - The re-key logs out every browser, desktop and phone. Phones show the
    "revoked" banner and rotate their Ed25519 device keys. Some org bot
    tokens and webhook URLs must be re-issued, and outstanding links die.
  - The alternative avoids all of it: adopt production's `DATA_KEY` and
    `ADMIN_PASS` into the vault and skip the re-key. After the cutover, Next
    *is* production, so the audit's concern (a second deployment holding
    production's key) no longer applies.
  - The runbook implements the user's choice (re-key).
- **D2: Mailtrap webhook.** Re-point Next's webhook and delete production's
  (step 16), or rotate production's secret into the vault. This is a
  Mailtrap-account action.
- **D3: Turnstile.** Production never used it. Next fails closed without it.
  Add the production domains to the widget (precondition 3), or give
  production its own widget.
- **D4: SOA minimum TTL.** Lower it from 1800 to 60 for the flip. This is a
  zone-wide setting, restored afterwards.
- **D5: Port the production feature Next lacks** (agents image attachments,
  migration 0191 → next free number) before the cutover, or accept the gap.
- **D6: The `fmnext` admin account is carried into production.** It is
  Next's operator admin and owns the core nodes. The core node rows require
  it, so the alternative is to re-own the cores under the `forkmesh`
  account.
- **D7: Where production names live in code.** Keep building releases
  through `release/production-*` (inverse rehost at build time), or switch
  `main` to production names once and retire `tools/rehost.py`'s guard test.
- **D8: DNS failover for production names.** Done 2026-09-24.
  - `next_dns.py` manages exactly `app.forkmesh.com` and
    `world.forkmesh.com`, A and AAAA records only, next to the `next.*`
    names.
  - The `fmnext-dns-health` user unit runs on the operator box. A core that
    fails `/readyz` 3 times in a row leaves both names. It returns after
    2 minutes healthy. The last record is never removed, and at most one
    change is made per 5 minutes (OPERATIONS.md, "DNS health failover").
  - The node reconciler still manages only `next.*` and still runs `dry`.
- **D9: The Cloudflare token is account-wide.** Still open.
  - Production and Next share one account token (`winter-wave-4b2e`) with
    Account API Tokens Write, billing, Workers, D1 and zone-wide DNS. Rotate
    it and split it.
  - `next_dns.py` already reads only `config/cloudflare-dns.env`, which is a
    symlink to `cloudflare.env` today. Replace the symlink with a file (0600)
    holding a Zone → DNS → Edit token for forkmesh.com, then
    `systemctl --user restart fmnext-dns-health`.
  - The cutover tools keep reading `cloudflare.env`.
- **D10: IPv6.** Done 2026-09-24.
  - n1–n3 have Vultr IPv6 (`ipv6` in the operator's `fleet-instances.json`, loaded as `public_ipv6`).
  - `app.forkmesh.com`, `world.forkmesh.com`, `app.next`, `world.next` and
    `nN.next` carry AAAA records next to the A records (TTL 60, DNS only).
  - The firewall, IDS egress and lockdown rules, and fail2ban cover IPv6
    (OPERATIONS.md, "IPv6").
- **Data findings in production (pre-existing, not caused by the move):**
  - 12 `repositories` rows are sealed under a key that is neither
    production's nor Next's. Production cannot read them today. The re-key
    now pseudonymises their identity blind index (reported as `NOTE 12 ...
    dead rows`) instead of aborting.
  - 2 unverified accounts have an e-mail index that does not match their
    stored e-mail. They cannot log in by e-mail today, and will not after
    either.

## Rehearsal results (2026-09-24, dress rehearsal)

All steps were local or dry-run, with no write to production, D1, DNS, the
mirrors or a live Next node.

| Step | Result |
|---|---|
| `wrangler d1 export forkmesh --remote` (read-only; output swallowed, file 0600) | 8.6 s, 30.5 MB SQL, 309 tables, 238 users |
| SQL → SQLite | 1.5 s, `integrity_check ok`, no FK violations |
| re-key, production key (in memory) → Next key `974e5c3f` | 127 s. 61,006 rows in, 60,320 out (638 invalidated, as REKEY.md 1.3). 3,951 ciphertexts, 59,578 blind indexes recomputed, 162 chat envelopes re-sealed. `verify: OK`, 4.28 M old-key probes |
| local rqlite 10.3.6: `/db/load`, `rqlite_migrate --app-schema` (0191–0199 Next, 198 already applied), `/db/backup` | 0.9 s, 4.4 s, 0.5 s |
| carry (Next backup of 01:18) | 3 cores + 3 endpoints + `fmnext` + runner + vault/cluster/DO state, 0.1 s |
| verify vs. pre-re-key baseline | 240/240 by name, 238/238 by e-mail (+2 pre-existing mismatches), 28/28 nodes |
| login probe (app under uvicorn on the final file, HTTP) | a production row signed up before the re-key, and `fmnext`, by name and by e-mail: 200. Wrong password: 401 |
| final file loads into a fresh rqlite | 0.6 s |
| `cutover_tls issue` against Pebble 2.8.0 + challtestsrv (DNS-01) | 2 certificates in 0.8 s, both pass certsync's validator |
| `cutover_dns plan` / `flip` (dry-run) against the live API | as in section E |
| `cutover_identity plan` against the real files | as in section A |
| production Caddyfile (`[production]`) adapts on Caddy 2.6.2 in `files`/`acme`/`internal` | yes |
| app test suite (`app/tests`, excluding `tests/host`) on the inverse-rehosted tree vs. unmodified HEAD | 121 failures only on the rehosted tree (314 vs 193), in 29 files. None is a served App/World file still naming Next. They come from four causes. (a) Tests that compare the App with `www/` (Next's homepage Worker, which production does not deploy): `_redirects`, `_headers`, installer, header, blog, market. (b) Generated `conformance/` fixtures ("fixture.json is out of date"). (c) `tools/`/`server/` text. (d) Mixed-case literals such as `Next.ForkMesh.COM`, which the lowercase mapping leaves alone. `app/tests/host/test_host_e2e.py` passes on the release tree |

Re-run the data rehearsal any time with precondition 4. It needs no node
and no DNS, and touches production only through the 9 s read-only export.
Afterwards shred the rehearsal files (`shred -u` by name, never `rm -rf`
on a path built from a variable).

## Tool reference

The tools carry no comments (AGENTS.md: explanations live here). Every
command dry-runs without `--execute`. None prints a secret value.

### tools/cutover_data.py

| Command | What it does |
|---|---|
| `export --out F [--execute]` | `wrangler d1 export forkmesh --remote`. `CLOUDFLARE_API_TOKEN`/`CLOUDFLARE_ACCOUNT_ID` are read from production's `app/.env.production` (must be 0600) into the child's environment only. stdout/stderr are discarded because they carry a one-hour presigned URL to the whole database. The output is 0600. The export blocks D1 while it runs. |
| `sqlite --in F.sql --out F.sqlite3` | loads the dump into a new 0600 SQLite file and runs `integrity_check` |
| `carry --from NEXT --into FINAL [--account N] [--execute]` | copies rows Next owns into the final file. Tables in `NEXT_TABLES` (vault, cluster membership, DO state, runners, IDS, hosted repos, marketplace reviews) replace the target's. Row rules: `users` named by `--account` (default `NEXT_ADMIN_USER`) and their `account_devices`, `nodes`/`mirror_https_endpoints` named `fmnext-*`, and `repositories` referenced by live `hosted_repos`. Ephemeral tables (`runner_request_nonces`, `runner_login_*`) are skipped. A key that already exists aborts, and so does any foreign-key violation. It reports how many other accounts exist only in Next. Both files must be under the same `DATA_KEY`. |
| `verify --in F [--key K] [--baseline B] [--baseline-key-env FILE:NAME]` | every `users` row resolves by name (`user_bi`) and by e-mail (`email_bi`), and every `nodes` row by name, under the key. With a baseline (the pre-re-key export under production's key), mismatches already present there are notes, and only regressions fail. |
| `login-probe --in F [--accounts-env F]` | runs the app locally (`python -m forkmesh_host serve`, sqlite backend on a private copy, offline, no cron, sandboxed HOME/XDG, no `FORKMESH_HOME`). It logs in over HTTP by name and by e-mail with the right password (expects 200 and a session token) and a wrong one (expects 401/403). Accounts come as `<PREFIX>_USER/_EMAIL/_PASSWORD` triples. |
| `rehearse --work DIR [--dump F \| --execute-export] [--next-backup F] [--seed-probe]` | the whole move with per-phase timings: sqlite, re-key, local rqlite load/migrate/backup, carry, verify, reload into a fresh rqlite, login probe. `--seed-probe` first signs a throwaway account up on the pre-re-key copy through the app's own signup route, under production's key in memory, so the probe proves a re-keyed production row. |

### tools/cutover_tls.py

- `issue [--execute] [--staging | --directory URL] [--contact E]`
  - Runs ACME (RFC 8555) with an in-memory ES256 account key, one order per
    name, DNS-01.
  - `CloudflareTxt` writes exactly `_acme-challenge.app.forkmesh.com` /
    `_acme-challenge.world.forkmesh.com`, TXT, TTL 60. It waits until
    1.1.1.1 (DoH) sees the value, and deletes only record ids it created,
    re-checked by type and name.
  - The certificate key is EC P-256, generated locally. Each pair is
    validated with `certsync.openssl_validate` (max 100 days) and written
    0600 to `~/projects/forkmesh-backup/data/cutover-tls/`.
  - Rehearsal options: `--challtestsrv URL` (Pebble's mock DNS),
    `--ca-bundle` (ACME server trust; relaxes only the strict-X.509 flag)
    and `--validate-ca`.
- `push [--execute] [nN ...]`
  - Validates each pair again, then over SSH stdin installs the key first,
    then the certificate, 0600 caddy, into `/etc/forkmesh-next/tls/`.
  - Then starts `fmnext-cert-sync.service` and prints each name's `notAfter`.
- `status [nN ...]`: the `notAfter` of each pair on each core.

### tools/cutover_dns.py

- `plan`: read-only. For the two names it shows the custom domains, the
  A/AAAA/CNAME records and the zone SOA `min_ttl`.
- `flip [--execute]`
  - Refuses unexpected records, or a custom domain on any other Worker.
  - Saves state before the first change. Per name it detaches the custom
    domain, then creates one A record per core IP (TTL 60, unproxied); these
    two calls are back to back.
  - Idempotent.
- `rollback [--execute]`: deletes the recorded A records (each re-checked as
  type A, the right name and a core IP), then re-attaches the recorded
  custom domain to its Worker.
- `soa-min-ttl N [--execute]`: PATCHes the zone `dns_settings.soa.min_ttl`
  (30..86400).

### tools/cutover_identity.py

- `plan`: for each name it adopts, prints `same` / `new` /
  `replaced by production's` / `absent in production`. Comparison is in
  memory, and no value is printed.
- `apply [--execute]`
  - Writes production's values of the adopted names into
    `config/secrets.env`, in place and keeping comments and order.
  - Rotates `WWW_EDGE_SECRET`. Never touches `DATA_KEY`, `ADMIN_PASS` or the
    Next-only secrets.
  - Keeps the previous file as `secrets.env.pre-cutover` (0600) and refuses
    a second apply.
- `rollback [--execute]`: restores that file.
- `put-www-secret [--execute]`: runs `wrangler secret put WWW_EDGE_SECRET
  --name forkmesh-www`, with the value on stdin and production's Cloudflare
  credentials in the environment.

### tools/cutover_rehost.py

- `--check`, `--apply --root DIR` (refuses a `main` checkout), and
  `--compare --production DIR` (lists hostname lines that still differ from
  production).
- **Roots:** `app/src`, `app/public`, `app/tools`, `app/tests`, `app/browser_tests`,
  `app/wrangler.toml`, `world`, `marketplace`. `tools/`, `deploy/` and
  `app/host` keep the Next names.
- **Mapping:**
  - `next.` → apex;
  - `app.next`/`world.next`/`ssh.next` → production;
  - the `.next.forkmesh.com` suffix → `.forkmesh.com`;
  - a doubled `"forkmesh.com", "forkmesh.com"` becomes `..., "www.forkmesh.com"`;
  - every other `*.next.forkmesh.com` name is kept.

### tools/next_rekey.py additions

- `--old-key-env FILE:NAME` / `--new-key-env FILE:NAME` read a key from one
  `NAME=` line of a 0600 env file, into memory only.
- **Dead rows.** A strict identity blind index whose every row carries a
  `data` record that the old key cannot open is pseudonymised instead of
  aborting the run. Production cannot read such a row either. The summary
  reports it as `dead_row_strict_pseudonymised`.
- The 0198/0199 tables are registered.

### deploy/core/render.py `[production]`

- `check_production`:
  - hosts must be exactly `app.forkmesh.com`/`world.forkmesh.com`;
  - `vars` names must be wrangler `[vars]` names or `AP_OUTBOUND`;
  - names that look like secrets (`SECRET|TOKEN|PASS|SEED|PRIVATE|DSN|_KEY`,
    except `TURNSTILE_SITE_KEY`) and multi-line values are refused.
- Helpers:
  - `served_*` are the production names when the table is present;
  - `legacy_hosts` are the Next names that redirect;
  - `tls_hosts` are the certsync names;
  - `production_settings` renders the `app.env` lines.
- `egress.serves_production` reads `FORKMESH_SERVES_PRODUCTION`.
- The `next_fleet` daemon step re-pins `routerPublicKey` in
  `/var/lib/forkmesh/mirror-gateway/config.json` to the `secrets.env`
  value, keeping the file's mode and owner.

### www/redirect_worker.js

Removed 2026-09-29. It was deployed with `www/wrangler.redirect.toml` under the script name
`forkmesh-next-www`. It answers 308 to `REDIRECT_ORIGIN` (a bare https
origin) plus the request's path and query, for every request.

## Variant used on 2026-09-24: keep production's DATA_KEY

The user decided on this variant after the rehearsal. Next adopts
production's `DATA_KEY` and `ADMIN_PASS`, so the final export is **not**
re-keyed. Sessions, room-chat keys and emailed links keep working, and
nobody is logged out. The rows only Next has (core nodes, runner, `fmnext`,
vault, cluster state) are sealed under Next's key, so they are moved to
production's key before being carried in. `main` switches to production
names once, through the release branch
(`release/production-20260924`, pinned at `e22d283`: `e86a8b0` plus the
cutover, keep-key, attachments migration (0201) and code-push/APNs
(0202, from production's `5e79aa2e4`/`1edb99d56`) commits and
`tools/cutover_rehost.py` output). The Apps slice (`a414bcd`) and
`5f6e186` are excluded.

Changes to the numbered steps:

- **Step 7 (staging):** `cutover_identity.py apply --keep-production-key
  --execute` (it also adopts `ADMIN_PASS`).
- **Step 8:** Cloudflare refuses custom SOA values on this zone (error
  1003), so `min_ttl` stays 1800 s. The flip's sub-second gap is accepted.
- **Step 11:** as written. In addition, `cutover_identity.py
  install-data-key --execute` switches `config/DATA_KEY` to production's
  key and keeps Next's key as `config/DATA_KEY.next-pre-cutover` (0400,
  never deleted; the user moves it offline). Run it after the apps stop and
  before the vault sync.
- **Step 12:**
  `cutover_data.py rehearse --keep-production-key --execute-export --work $W/move --next-backup $W/next.sqlite3`
  - Export, then load, migrate and back up.
  - `extract` the Next-owned rows, then `reseal` them from Next's key to
    production's (`next_rekey` on the small file).
  - Carry, then verify every account by name and e-mail under production's
    key, check that every carried value opens under production's key and
    none under Next's, and run the HTTP login probe.
  - Measured: 15 s of data work plus the export (9–76 s).
- **Step 14:** `next_vault.py sync --prune --no-reload --execute` now seals
  under production's key (key_id `03325227`).
- **Step 16 (Mailtrap):** Next's webhook 4524 is re-pointed to
  `https://app.forkmesh.com/api/integrations/mailtrap/webhook`.
  Production's webhook 3788 (already inactive) is deleted.
- **Rollback R1 step 3:** additionally `cutover_identity.py
  restore-data-key --execute` and `cutover_identity.py rollback --execute`.
  - The Next copy restored from step 11 is under Next's key again.
- **Window estimate:** writes pause for about 8–10 minutes.
- **Production Worker deploys** are built from `cutover/deploy-readonly` in
  the production repo: the deployed rev `c9d64bf73` merged with
  `cutover/readonly-and-www-origin`. Production `main` does not contain the
  APNs change that production serves.

## After the cutover: core gateways' dashboard replica (found 2026-09-24)

Each core's Git gateway (`nN.next.forkmesh.com`) keeps a public dashboard
replica (`/var/lib/forkmesh/mirror-gateway/dashboard-replica.json`: the
catalog, status, and forkmesh/forkmesh's star, forks, about and mirrors).
It refreshes from `dashboard.canonicalOrigin` in the gateway config, which
the daemon's one-time `--init` set to `https://app.next.forkmesh.com`.
After the flip that name answers 308 to `app.forkmesh.com`, and
`mirror_replica` refuses a response that changes origin. So the replica
kept its last pre-cutover copy, taken from the quieted Next data:

- 0 fediverse followers, with the handle `@forkmesh.forkmesh@next.forkmesh.com`;
- the Next-era catalog and mirrors list.

The repo page reads `about`, `mirrors`, `tree` and `blobs` from a mirror
origin (sorted by last sync) until it knows the page's origin reads the
repository locally. Whenever an `fmnext-nN` core sorted first, the Watch
button and the "fediverse watchers" line showed 0. The same happened on
every page served by `nN.next.forkmesh.com`.

- **Fix:** `next_fleet install nN --steps daemon` now re-pins
  `dashboard.canonicalOrigin` to `https://<served app host>`, next to the
  router key. It runs through the tool, so IDS gets its signed window.
- **What happens on restart:** a replica whose source changed is discarded
  and refetched when the daemon restarts.
- **After a future rename:** run `install nN --steps daemon` on every core
  whenever the served app host changes.

The About rail's "Latest release" fix (`042cadf`) edited only the built
`app/public/dashboard.js`. `3be0bd7` then rebuilt the bundle from
`app/public/dashboard/js/*.js`, which dropped the fix, and the release showed
v0.7.95 again. The picker now lives in the source fragments
(`06-repo-content.js`, and the Releases tab's sort in
`07-repo-compose-branch.js`).

**Rule:** edit the fragments and run
`app/tools/build_dashboard_assets.py`. Never edit `dashboard.js` directly.

### Core gateways' dashboard pages and assets (found 2026-09-24)

`tools/mirror_gateway.py` builds its website from git blobs of the mirrored
`forkmesh/forkmesh` repository: `app/public/dashboard.js`, the HTML under
`app/public/dashboard/`, and so on.

- **On `nN.next.forkmesh.com` that is the wrong source.** It is production
  ForkMesh's own tree, not the Next release the cores deploy. Its checked-in
  HTML carries the unstamped `?v=${version}` placeholder, which only the build
  (`dashboard_shell.stamp_asset_versions`) fills in. Those pages therefore ran
  production-repo code (latest release v0.7.95) under a literal
  `?v=${version}` URL.
- **New gateway setting:** `dashboard.assetsRoot`, an absolute directory
  holding a built site. When it is set, the gateway serves pages, the
  asset routes, and any other built asset with a known extension (for example
  `/user-status.js` and `/dashboard-page-*.js`) from that directory. HTML is
  served already stamped.
  - **Caching:** the cache key is the resolved real path plus mtime and size.
    A deploy that swaps the `current` symlink takes effect on the next
    request, with no daemon restart.
  - **Safety:** symlinks that resolve outside the root are refused.
- **Cores:** `next_fleet install nN --steps daemon` sets `assetsRoot` to
  `<current>/app/dist`.
- **Independent mirrors** keep the git-blob source. They now replace
  `?v=${version}"` in served HTML with the assets commit (12 hex digits), so
  a new commit changes every asset URL.

### Caddy 2.6.2 reload panic (2026-09-24, n2)

Debian's Caddy 2.6.2 panicked during a routes reload with
`context: internal error: missing cancel error`. Its unit has `Restart=no`,
so the node lost HTTPS until Caddy was started by hand. Two changes cover it:

- The drop-in now sets `Restart=on-failure` (`RestartSec=2s`), so any crash
  restarts Caddy. It is applied with `install nN --steps files`, which opens
  the signed IDS window.
- `next_deploy`'s activate and rollback scripts check
  `systemctl is-active caddy` after each reload and restart Caddy if it died.

The fix is to move off 2.6.2 to a pinned upstream Caddy binary. See
docs/OPERATIONS.md, "Caddy version": `next_fleet.py` steps `caddy` and
`caddy-debian`. The deploy gate now also waits for Caddy to serve `/readyz`,
so a Caddy restart no longer rolls a node back.

## After the cutover: old-stack data and /status history (2026-09-24)

### /status showed only 08-26, 08-27 and today

The history was never lost. Every status batch row (`system_status_{minute,hourly,daily}_batch`) came over in the D1 export. Next's `fb02cd7` (18:06 UTC) then added mirror35–38 to `STATUS_RETIRED_MIRRORS`. `_status_batch_decode` treated any retired id as a poisoned row and returned `{}` for the **whole** row, so every day from 08-28 to 09-23, every hour up to 09-24 17:00 and every minute up to 18:40 read as "no data".

- **Fix:** the decoder skips a retired mirror id and keeps the rest of the row. Any other invalid id still rejects the row. It is in branch `fix/status-retired-mirror-rows` of the Next repo, and it needs an app deploy.
- **Collateral:** the hourly compactor rebuilt the 18:00 hour from minutes that decoded as empty, and stored 23 of its 60 checks. The daily compactor at 00:00 UTC does the same to the previous day if the fix is not live by then.

### tools/status_history_repair.py

This tool recomputes hour batches from the retained minute batches (30 h), and day batches from 24 present hour batches. The retired list comes from `--entry`, which must be the **deployed** tree's `app/src/entry.py`: a retired id in a written row breaks the decoder that is running before the fix.

- A row is written only when the recomputed row covers **at least** as many checks for every system and more in total.
- An existing row is replaced with `UPDATE … WHERE data=<the value read>`, so a concurrent app write wins. A missing row is added with `INSERT OR IGNORE`.
- The current hour and day are left to the app.
- The tool dry-runs by default and reaches rqlite through the fleet SSH tunnel (IDS logs `ssh_tool_session`).

The 2026-09-24 run restored hour 18:00 (299 → 780 checks). If the fix goes live after 00:05 UTC on 09-25, run the tool again to restore day 09-24.

### Old-stack inventory

The old stack was taken read-only on 2026-09-24 at 20:20 UTC. The encrypted copy is `~/projects/forkmesh-backup/data/old-stack-export/old-stack-20260924T202758Z.tar.gz.age` (to `backup-age.pub`). It holds:

- a fresh `wrangler d1 export`;
- the KV keys and values;
- the Durable Object instance list;
- the per-table counts and the key-level diff.

| Store | Old | Result |
|---|---|---|
| D1 `forkmesh` | 309 tables, 61,008 rows | Every table exists in rqlite. By primary key, 1,160 old rows are absent. All of them are app retention on Next: `cron_tick` (6 h), status minutes and claims (30 h), `chat_history` (7 d), `online_hourly*` (48 h), world visit HLL buckets, attribution IP buckets, expired sessions and signup intents, and 188 read notifications of one account trimmed by `MAX_NOTIFICATIONS_PER_RECIPIENT`. The rest is the mirror35–38 retirement (docs/MIRROR_RETIREMENT.md): nodes mirror36–38 and their `repositories`, `profile_contribution_projects`, `catalog_rate`, `mirror_https_endpoints` and two finished `org_agent_jobs` on mirror38. Nothing was backfilled. |
| D1 writes after the export | 15:10–15:20 UTC | Only probe/scheduler rows: mirror heartbeats, status minutes, snapshots, `cron_tick`, `usage_budget_state`. No user writes. |
| KV `REPOSITORY_METADATA` | 5,420 keys (`repository-metadata-fallback/v1/*`) | Last-known-good repo-page cache, rebuilt from mirrors; Next keeps KV per node. Archived, not imported. |
| KV `WORLD_CACHE_META` | 0 keys | Unused; deleted 2026-10-04 (see "Workers retired"). |
| Durable Objects | 8 classes. Objects with stored data: Room 8, CronRunner 75, CronWatchdog 1, DiscordGate 1; the others 0 | Only rate windows, scheduler, doorbell, wake and rate-limit state; no class uses `storage.sql`. Chat, notes and world totals were always in D1. No export endpoint was deployed. |
| Queue `forkmesh-notification-email` | backlog 0 | Payload-free doorbells, 24 h retention. |
| R2 | not enabled on the account | — |

### The old Worker was redeployed at 17:28 UTC

`forkmesh-app` was redeployed at 17:28:56 UTC from production main (`36b1d0cf9`), with the stock `app/wrangler.toml`. The decommission variant is gone: a `* * * * *` cron and the queue consumer are back, and `FORKMESH_DECOMMISSIONED` is unset. The domains were not re-claimed: the Worker has no custom domain, no zone route and no workers.dev subdomain, and the deploy recorded `failed` in `world_deploy_status`. As of 20:30 UTC the old D1 has had no write since 15:20, and the Worker has had no invocation since 16:00. Redeploy the decommission variant (step 21), and stop whatever deploys production main to Cloudflare.

**Cause:** ForkMesh Actions run 491 on the operator desktop. `.forkmesh/deploy.yml` ran `on: [push, workflow_dispatch]` with no branch filter. The in-app merge of `36b1d0cf9` into main queued it, and `./deploy.sh changed` deployed `forkmesh-app` with the stock manifest. Wrangler set the cron and the queue consumer. The custom-domain claim for `app.forkmesh.com` failed only because the hostname already had DNS records (code 100117). Pushes after the Next merge (`ce149153a`) queued the same workflow: runs 525–567 would have created `forkmesh-next-app` (cron `* * * * *`, queue consumer, custom domain `app.next.forkmesh.com`) and replaced the `next.forkmesh.com` redirect Worker. They stopped only because deploy.sh could not read that Worker's secret names. Pushes to other branches queued it too (for example `diag/native-cancel-probe`, run 451).

**Fix (2026-09-24, 20:55 UTC):**

- Main and `cutover/deploy-readonly` carry a `deploy.yml` that runs only on `workflow_dispatch`, and its first step exits 1. The production www Worker is deployed by hand from `cutover/deploy-readonly` (`cd app && ./deploy.sh www`).
- Older branches still carry the push-triggered copy. Untick "Deploy Cloudflare Worker" in the repository's Actions settings on the operator desktop, so a stale branch push cannot run it either.
- The decommission variant was redeployed (version `1a2f4f34`). `wrangler deploy` does not detach a queue consumer that is missing from the manifest, so the `forkmesh-app` consumer on `forkmesh-notification-email` was deleted through the Queues API.
- Verified with the API: `forkmesh-app` and `forkmesh-world` have no cron schedules, no workers.dev subdomain, no custom domain and no zone route. The queue has no consumer. `FORKMESH_DECOMMISSIONED=1` and `FORKMESH_READ_ONLY=1` are set.

## D1 deleted (2026-09-27)

The Cloudflare D1 database `forkmesh` (`2d33ad9e-…`) was deleted on
2026-09-27 at about 01:55 UTC, after a final backup and a row-level check
against rqlite.

**Why it was still busy.** Pushes to the old branch `native/ios-desktop-parity`
(14:26 and 14:30 EDT on 09-26) ran its push-triggered "Deploy Cloudflare
Worker" workflow (Actions runs #914 and #927). This is the 09-24 trap again:
old branches still carry the pre-fix `deploy.yml`, and the workflow was never
unticked in the desktop's Actions settings. `forkmesh-app` (build
`053f1af8bc9f`) came back with a `* * * * *` cron and the
`forkmesh-notification-email` queue consumer, both bound to D1: about 15,000
reads and 1,800 writes a day. It never got a custom domain
(`app.forkmesh.com` already had DNS records, code 100117), so no user reached
it. Mailtrap's log shows no mail sent after 09-24 07:49, so it sent nothing.
Its status monitor wrote false "needs attention" notifications into D1.

**What was done:**
1. Removed the cron (`PUT …/workers/scripts/forkmesh-app/schedules []`) and the
   queue consumer. The Worker kept running every minute on its Durable Object
   alarms, so removing the cron alone did not stop it.
2. Backup: `cutover_data.py export` (wrangler 4.136.3 needs Node 22:
   `~/.nvm/versions/node/v22.23.1/bin` on `PATH`) →
   `data/fleet/backups/d1-forkmesh-final-20260927T014737Z.sql` (0600, 28.9 MB,
   57,790 rows, sha256 `e8a1bc65d20671f0…`), plus an age copy (`.sql.gz.age`, to
   `backup-age.pub`) beside it. The 09-24 old-stack archive in
   `~/projects/forkmesh-backup` is unchanged.
3. Compared with a fresh plain `/db/backup` of rqlite (n1). All 309 D1 tables
   exist in rqlite. Every row was matched by primary key (the whole row when a
   table has none). 937 of 57,774 D1 rows are not in rqlite, and every one is
   accounted for:
   - retired mirrors (`docs/MIRROR_RETIREMENT.md`): nodes mirror17 and
     mirror35–38; their `forkmesh` repositories; `profile_contribution_projects`;
     `mirror_https_endpoints`; `catalog_rate`; `product_clone_daily`; 19
     finished `org_agent_jobs` (17 on mirror35, 2 on mirror38); mirror35's
     163-row notification inbox; mirror35's `issue_seq` (729, safe:
     `forkbot` seeds the counter from the highest issue number in the
     repository, and git holds up to 728); and mirror35's one repo webhook, a
     Codemagic trigger last delivered 09-17 (Next triggers Codemagic through
     `tools/next_codemagic.py`);
   - retention on Next: read notifications over `MAX_NOTIFICATIONS_PER_RECIPIENT`
     (500), expired or revoked sessions and signup intents, `inbox_drain_log`,
     `repo_state_history`, `attribution_clone_snapshots`,
     `flagship_read_failure_minute`, world visit HLL buckets, `cron_tick`;
   - rows the zombie Worker wrote after the cutover: `operational_alert`
     notifications from 09-26 18:34 on, and one `cron_tick`.

   No user data existed only in D1.
4. Deleted the database through the API. The account has no D1 database left.

**Nothing calls D1.** The Next app's `DB` binding is rqlite
(`app/host/forkmesh_host/d1_rqlite.py`). `forkmesh_mcp_server._d1_query` now
reads rqlite over the fleet SSH path (`rqlite_curl` on a healthy core)
instead of `pywrangler d1 execute`, and so does
`app/sync_request_block_rules.sh` since 2026-09-28
([www/README.md](www/README.md), Security). The legacy production scripts
(`app/deploy.sh`, `app/migrate.sh`,
`app/tools/sync_contributor_rewards.py`, `app/tools/sync_discord_accounts.py`)
are manual-only and now fail at the missing database. `app/src/usage_reporting.py`
reads Cloudflare's D1 analytics, not the database, and reports zero. A stale
branch that redeploys the old Worker now fails on its `DB` binding, so the old
stack cannot come back with its crons. The `forkmesh-app` script itself is
still on the account, with no cron, queue consumer, route or domain. Its
Durable Object alarms now fail at the missing database. Deleting the script
(and its Durable Object state, archived on 09-24) is the remaining step.

## Workers retired (2026-09-27)

`forkmesh-app`, `forkmesh-world` and `forkmesh-next-www` were deleted through
the API (`DELETE …/workers/scripts/<name>?force=true`). `forkmesh-world` went
first: it had a service binding to `forkmesh-app`. `forkmesh-www` (forkmesh.com
and www.forkmesh.com) is the only Worker left serving traffic; it reaches the
App over the internet (`APP_ORIGIN`) and had no binding to the deleted ones.

- **Export first.** Every Worker's settings (plain variable values included),
  secret names, code as uploaded, schedules, versions and deployments, plus the
  zone routes, custom domains and email routing rules, are in
  `~/projects/forkmesh/config/fleet/retired-workers/2026-09-27/` (never erase;
  README there lists where each secret value lives). Cloudflare never returns
  secret values; all of them are in `config/fleet/secrets.env` or
  `src/app/.env.production`.
- **What went with them.** `forkmesh-app`'s eight Durable Object namespaces
  (state archived 09-24, above). Its KV namespace `REPOSITORY_METADATA` (5,420
  keys, every value exported to `kv-REPOSITORY_METADATA.json` in the folder
  above) and queue `forkmesh-notification-email` (no producers or consumers
  left) were deleted the same day. KV `WORLD_CACHE_META` was not touched then.
- **KV `WORLD_CACHE_META` deleted (2026-10-04).** Namespace
  `3fbdd29f5d8b43ef9ae6754225ac651c` held the old Worker's cache-diagnostics
  snapshot, a feature added and reverted on 2026-07-29. Before deletion it had
  0 keys, `forkmesh-www` (the only Worker left) had no KV binding, the account
  had no Pages projects, and nothing in git or `config/fleet` named it. The
  empty export is `kv-WORLD_CACHE_META.json` in
  `~/projects/forkmesh/config/fleet/retired-workers/2026-10-04/`. The account
  has no KV namespaces left.
- **`next.forkmesh.com`** was `forkmesh-next-www`'s custom domain and only
  redirected to forkmesh.com; it no longer resolves.
- **`email-worker` deleted too.** It fanned `founders@forkmesh.com` out to two
  addresses through Cloudflare Email Routing, but forkmesh.com's MX is Zoho, so
  Email Routing receives nothing: GraphQL showed 0 invocations and 0 routing
  events from 2026-08-29 to 2026-09-27. Its `founders@` rule was removed first;
  both are in the export.
- **Tracing.** `forkmesh-www` traces are on with `head_sampling_rate = 1`, set
  through `PATCH …/script-settings` (no new version) and in
  `www/wrangler.production.toml` so a redeploy keeps it.

## forkmesh-next-www deleted again (2026-09-29)

A `wrangler` upload re-created `forkmesh-next-www` on 2026-09-27 at 20:18 UTC,
about an hour after it was deleted, with the full homepage build of
`www/wrangler.toml` (assets, `APP_ORIGIN=https://app.forkmesh.com`, a
`DEPLOY_FINGERPRINT` var, no secrets) and its custom domain
`next.forkmesh.com`. The fingerprint var points at `app/deploy.sh www`
(`www/deploy.sh`, or the `www` step of `deploy.sh deploy`/`changed`), which
still deployed `www/wrangler.toml` and then checked forkmesh.com. It served
1,376 requests in two days, all scanners and crawlers (`/.env*`, `/.aws/*`,
`/robots.txt`), and 308'd everything else to app.forkmesh.com.

It was exported to
`~/projects/forkmesh/config/fleet/retired-workers/2026-09-29/forkmesh-next-www/`
(settings, versions, deployments, uploaded content, custom domain, DNS) and
deleted with `DELETE …/workers/scripts/forkmesh-next-www?force=true`, which
also removed the custom domain and its read-only `next.forkmesh.com` AAAA
record. `forkmesh-www` is the only Worker left; the `*.next.forkmesh.com` core
records are separate and untouched.

So it cannot come back:

- `app/deploy.sh` deploys www (and its `dry-run www`) with
  `wrangler.production.toml`, the same config as `tools/next_autodeploy.py`.
  `app/tools/deploy_targets.py` fingerprints that file instead of
  `www/wrangler.toml`.
- `www/wrangler.toml` lost its `name` and its `next.forkmesh.com` route.
  Wrangler refuses to deploy a config without a name, so a plain
  `npx wrangler deploy` in `www/` fails. The file stays for the tests and
  build tooling that read its assets and `run_worker_first`.
- `www/wrangler.redirect.toml`, `www/redirect_worker.js` and its test were
  removed; they deployed under the same script name.

