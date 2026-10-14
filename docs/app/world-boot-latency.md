# World boot latency on the core nodes

How the App keeps the World's first requests fast. Each core runs one App
process on a single event loop, so every statement a boot request skips
shortens the queue for all others. Code: `app/src/account_users.py`,
`app/src/entry.py` (`edge_cache_delete`, `host_warm_caches`),
`app/src/d1_read_guard.py` (`shared_read`), `app/host/forkmesh_host/`.

- **Background refresh**: `env.HOST_BACKGROUND` runs one task per key and
  shares in-flight loads; on Cloudflare reads run inline.
- **Stale-while-revalidate copies**: world roster (fresh 30 s, kept 1 h, weak
  `ETag`), guest profiles (fresh 60 s), contributions (fresh 60 s,
  generation-keyed), wallet readings (last good value for 24 h).
  `edge_cache_delete` bumps `EDGE_CACHE_EPOCHS` so purges win over in-flight
  builds. Privacy checks always run before a cached copy is served.
- **Roster**: only changed user rows are fetched and decrypted; see
  [member-roster.md](member-roster.md).
- **Session memo**: on the host, GET/HEAD session and `_is_admin` lookups are
  reused for 5 s and cleared on writes to `users`, `account_sessions` or
  `account_devices` (`_d1_write_watch`).
- **Warm-up**: after the vault unseals, `host_warm_caches` builds the roster,
  table inventory and wallet readings. `FORKMESH_HOST_WARM=0` disables it.
- **Server-Timing**: every `/api/*` response carries `app` and `db` durations
  plus `Timing-Allow-Origin` for the allowed CORS origin.

Tests: `app/tests/test_boot_endpoint_caches.py`,
`app/tests/test_boot_api_latency_round5.py`,
`app/tests/host/test_host_server_timing.py`.
