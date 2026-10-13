# World boot latency on the core nodes

Why the World's first requests were slow on ForkMesh Next, and what the App
does about it. Code: `app/src/account_users.py` (roster),
`app/src/entry.py` (profile lookup, wallet, `edge_cache_delete`,
`host_warm_caches`), `app/src/d1_read_guard.py` (`shared_read`),
`app/host/forkmesh_host/background.py` and `runtime.py` (host side). Tests:
`app/tests/test_account_avatar_world_roster.py`,
`app/tests/test_boot_endpoint_caches.py`, `app/tests/test_d1_read_guard.py`.

## What the logs showed (2026-09-29)

The user's load log (RX 580 client): `/api/accounts/jett?viewer=jett`
1.66 s then 2.29 s, `/api/accounts/users?view=world` 1.37 s then 1.56 s,
`/api/world/wallet` 574 ms, and `/api/world/inactive`,
`/api/repo/forkmesh/forkmesh/mirrors`, `.../agent-bots`, `/api/world/visitors`
and `/api/world/floor-notes` at 630 to 820 ms. `/api/world/context` (no
database) takes about 75 ms from the same place.

Each core is one 1-vCPU, 1 GB Vultr VM running one app process (one event
loop), rqlite, Caddy and the rest; the box sat at 0 % idle during boots. A
local rqlite read costs 2 to 7 ms, the cores are 0.7 ms apart.

The core journals (`slow request` lines, `D1 query` lines) gave the cause:

- **The admin World ticket counted every table.** `/api/world/ticket` for an
  administrator builds the System Capacity inventory
  (`_world_system_capacity`, a `SELECT COUNT(*)` over about 256 tables). Its
  shared copy lives in the process cache, so every deploy empties it, and
  nothing stopped concurrent misses: at 21:52:26 on n3 three tickets ran it at
  once (768 statements, 5.4 to 6.5 s each), and every other boot request on
  that core waited behind them (`/api/accounts/jett` 2184 ms, `users` 1505 ms,
  `agent-bots` 771 ms, `mirrors` 698 ms, `inactive` 391 ms). That is the
  common slow prefix of the 600 to 800 ms endpoints: they make 0 to 4 cheap
  queries each (measured locally) and were queueing.
- **Roster rebuilds moved 2.1 MB per build.** The directory scan read
  `u.data` for all 235 users (2,092,208 bytes of ciphertext, mostly avatars)
  on every rebuild: 70 to 200 ms for rqlite alone on a busy core, then JSON
  parsing, 235 AES-GCM decrypts and a 2 MB `json.dumps` for the
  `users-directory` shared copy, which the host cache then refused (entries
  over 2 MB are not kept), so the copy never existed. The world view was
  rebuilt every 30 s and the default roster every 120 s; the World ticket also
  drops the default copy whenever it credits activity. Measured in the
  journals: 250 to 620 ms per rebuild, about 1 s on the first request after a
  deploy (Pillow load and thumbnails).
- **The viewer's own profile made about 20 sequential statements.**
  `?viewer=` responses skipped the 60 s guest cache, and
  `_account_social_counts` re-read and re-decrypted the 47 KB account record,
  counted followers and following separately and read `repositories` once per
  owned node (9 owners for jett).
- **The wallet chip** is cached per process for an hour, so almost every
  visit found it expired on the core it hit and waited on two Solana RPC reads.

Thumbnails were not a per-request cost: `_AVATAR_THUMBS` already keys them by
content hash.

## What changed

- `shared_read`: a reader that has a fallback (the table inventory) never
  scans on the request when the host can run it in the background: it answers
  with the stale copy, or the fallback (an empty inventory, as a conserving
  guard already did), and the host refreshes it. Readers without a fallback
  join one in-flight load per key instead of scanning once per concurrent
  request. On Cloudflare (no host) both paths scan inline as before.
- Roster: the scan reads each user's ciphertext tag (`substr(data,1,24)` and
  `length(data)`; every write re-encrypts with a fresh random IV, so the tag
  changes with every change) and fetches and decrypts only the rows whose tag
  is new, 50 per statement (`_USERS_DIRECTORY_RECORDS`, pruned to the users
  still present). The aggregate join keeps its shared copy, now small enough
  to be kept. Details in [member-roster.md](member-roster.md).
- World view: the server copy is kept 90 s and carries its build time and a
  weak `ETag`; it is served as fresh for 30 s, then served stale while the host
  rebuilds it in the background; `If-None-Match` gets a 304. A purge still
  drops it at once, and a build that started before a purge does not store
  its result (`EDGE_CACHE_EPOCHS`, bumped by `edge_cache_delete`).
- Profile lookup: a `?viewer=` request for a public profile reuses the guest
  copy and computes only `isFollowing` (one statement, none for your own
  profile); a miss stores the guest copy too. Social counts are one statement,
  mirror counts one `IN (...)` read, and the already-decrypted record is passed
  down. A follow or unfollow now purges both profiles' lookup copies.
- Wallet: `world_wallet_swr_handler` keeps each address's last good reading
  for 24 h per process; once the hourly copy expires it answers with that
  reading (`Cache-Control: public, max-age=60`) and refreshes in the
  background. `refresh=1`, bad addresses and failed reads pass straight
  through. The handler itself (`world/src/world_routes.py`) is unchanged.
- Host: `env.HOST_BACKGROUND` (`BackgroundRefresh`) runs one background task
  per key (at most 16) and `join` shares one load between waiters. After the
  vault unseals, `warm_caches` calls `entry.host_warm_caches`, which builds the
  world roster (Pillow, thumbnails, decrypt memo) and starts the table
  inventory, so the first visitor after a deploy does not pay for them.
  `FORKMESH_HOST_WARM=0` turns the warm-up off.

The App's rule against concurrent tasks (`test_worker_task_concurrency.py`)
is about Pyodide. App code still creates no task: it hands a factory to
`HOST_BACKGROUND`, which exists only on the CPython host.

Not changed: Caddy already compresses every response (zstd), so the roster is
not pre-compressed. `fleet.toml` `[app] d1_log = true` journals every
statement (about 360 lines a minute on n3), which costs journald CPU on these
boxes; that is an operator choice for the desktop DB view.

## Measurements

Local host, sqlite, 235 synthetic users with 8 avatars:

| | before | after |
|---|---|---|
| world view rebuild after expiry | 50 ms | 27 ms, 4 statements |
| world view, stale copy | rebuild | 0.6 ms, rebuild in background |
| world view with a matching `If-None-Match` | 200, full body | 304, 0.6 ms |
| `/api/accounts/jett?viewer=jett` statements | about 20 | 1 (guest copy warm), 7 (cold) |

On the cores the rebuild saving is larger than locally, because the 2.1 MB
ciphertext read (70 to 200 ms there) is gone after the first build.

## Rollout

App and host code only: no migration. It takes effect on the next core roll
(`tools/next_deploy.py`); each core warms itself after unsealing.

## Round 5 (2026-09-30)

### What the logs showed

A signed-in admin's World load (account jett) hit core n3 at 07:52:59 to
07:53:05 UTC (`~/.cache/world-r5-load/n3-app.log`). The server times were:

- `/api/accounts/users?view=world`: 704 ms.
- `/api/accounts/jett?viewer=jett`: 436 ms.
- `/api/world/ticket`: 558 ms.
- `/api/world/wallet`: 490 ms.
- `/api/world/preferences`: 587 ms for GET and 501 ms for POST.
- `/api/accounts/jett/contributions`: 1049 ms.
- `.../agent-bots`: 589 ms.

On a 1-vCPU core, every statement in that window waited 15 to 60 ms on the
shared event loop. So each query a request skips saves tens of
milliseconds, not the 2 to 7 ms that rqlite itself takes.

- **Round 3's caches were cold because of time, not keys.** The process
  (pid 2086014) started and warmed at 07:34:08, 19 minutes before the visit.
  - The world roster copy was kept only 90 s, so the visit rebuilt it inline.
    That rebuild ran the referrer scan, the read-guard state, the joined scan,
    two `_users_directory_records` reads and `_world_user_activity_buckets`.
  - The guest profile copy was kept 60 s, so `?viewer=jett` ran the cold
    7-statement path.
  - The wallet memo is per process, and nothing warms it. The cores rolled
    four times between 07:27 and 08:06 that morning, which reset it each time.
  - The cache keys themselves were fine: `view=world` and `?viewer=` both
    reach the cached paths.
- **Every authenticated request ran two linearizable "primary" reads.** One
  reads `account_sessions` joined with `account_devices`, the other reads
  `users.data`. On n3 that day there were 1106 such lookups, and a World boot
  runs about 15 of them within 3 s using the same token. The World ticket,
  agent-bots, runners/attention and the cold profile each added
  `SELECT is_admin`.
- **Contributions re-verified everything on every request.** The endpoint
  ran:
  - the account read;
  - the revision CTE, which hashes every public source for the profile;
  - four heavy CTEs;
  - the revision CTE again.

  Its internal cache is keyed by that revision, so even a cache hit paid for
  two heavy CTEs. Its 30 s lifetime also meant the first visit after a pause
  missed it.
- `_world_user_activity_buckets` runs only when the roster is rebuilt, not
  on every roster read. With a warm roster it costs nothing.

### What changed

- **Server-Timing** (`app/host/forkmesh_host/asgi.py`, `d1_caller.py`,
  `d1_rqlite.py`, `d1_sqlite.py`):
  - Every `/api/*` response gets `Server-Timing: app;dur=<ms>` plus
    `db;dur=<ms>;desc="<n> queries"`. `app` is the time until the App
    returned its response object. `db` is the wall time the request spent
    waiting on D1 statements, counted per request in the `d1_caller` holder.
  - Background refreshes run in their own scope, so their queries do not
    count toward the request that started them.
  - The header is added to the ASGI header list, so the App's response
    object is never modified. Streamed bodies are unaffected. WebSockets and
    the repo fast path are unchanged.
  - The World (world.forkmesh.com) calls app.forkmesh.com cross-origin: its
    `/api/*` redirects with a 308. So the response also carries
    `Timing-Allow-Origin` set to the origin that CORS already allowed, which
    lets browsers expose the timing to it.
  - Caddy passes both headers through and does not modify them. The header
    reveals no more than the response's total latency already does.
- **Session memo** (`_account_session_lookup_memo`, `account_users.py`):
  - On the host only, a GET or HEAD `_account_session_record` reuses a
    successful lookup for 5 s.
  - The memo is keyed by the session token's HMAC digest, and each caller
    gets its own copy of the record.
  - Concurrent lookups of one token share a single flight.
  - Unsafe methods, the Cloudflare path and all direct
    `_account_session_lookup` callers still read the primary every time.
  - `_is_admin` gets the same 5 s memo on the host.
  - Both memos are cleared on this process whenever a statement writes
    `users`, `account_sessions` (other than the session touch) or
    `account_devices`. The hook is `_d1_write_watch`, which `_log_d1_query`
    runs for every statement. A lookup that was in flight during such a write
    is not stored.
  - A logout, a revocation, an admin change or a record write therefore takes
    effect at once on the core that handled it, and within 5 s on the other
    cores.
- **Guest profile** (`_account_lookup_cached` and `_account_lookup_store`):
  - On the host, the copy is kept 15 min and records its build time. It is
    served as fresh for 60 s.
  - After that it is served stale while `_account_lookup_refresh` re-runs
    `accounts_handler` in the background. That refresh bypasses the cache and
    does not store its result if a purge happened while it ran.
  - Browsers still receive `public, max-age=60`.
  - The privacy check on the current record still runs before any cached
    copy is used.
  - On Cloudflare the behavior is unchanged: a 60 s copy, and a miss after
    that.
- **World roster:** the server copy is now kept 1 h instead of 90 s. It is
  still fresh for 30 s, then served stale while the host rebuilds it, so the
  first visitor after an idle period gets a copy at most an hour old. The
  finest activity bucket is also an hour. Purges still drop the copy at once.
- **Contributions** (`_contribution_view_*` in `route_handlers.py`):
  - On the host, each cacheable range's final response is kept for 10 min
    under a key that includes the profile, the range and a contributions
    generation. It is served as fresh for 60 s.
  - After that it is served stale while the host re-runs
    `_contribution_profile_api(..., view_refresh=True)`, which keeps the full
    revision checks.
  - Any write to `repositories` or `profile_contribution_*` on this process
    advances the generation, which retires every stored view.
  - The account and privacy check still runs first. The response body and
    headers are unchanged.
  - A repository made private on another core can still show in that core's
    contribution view for up to 60 s plus one stale answer.
- **Wallets:** after the roster warms, the host warms the wallet readings for
  the addresses the world roster publishes (13 today, at most 32). It runs in
  the background through `_world_wallet_remember`, so after a roll the World's
  wallet chip is answered from memory.

New code lives in the lazy modules. `entry.py` gains about 3.5 KB of startup
source: the write watch, the memo containers, and the proxies and call sites.
The `test_world_worker_footprint.py` startup budget already failed before
round 5: 1,741,518 bytes against a budget of 1,730,000. That budget was
retired on 2026-09-30 ([worker-footprint.md](worker-footprint.md)).

Tests: `app/tests/test_boot_api_latency_round5.py` and
`app/tests/host/test_host_server_timing.py`.

### Expected effect on a warm core

These are the statements each request still runs:

| Request | Statements |
|---|---|
| Roster | 0 |
| `?viewer=jett` | 1 (the privacy read) |
| World ticket | Its own writes, plus session and admin reads when the memo has expired |
| Preferences GET | 0 within 5 s of another authenticated GET |
| Contributions | 1 (the account read) |

### Rollout

App and host code only: no migration and no Caddy change. It takes effect on
the next core roll (`tools/next_deploy.py`). The Server-Timing header needs the
new host code, which ships in the same roll.
