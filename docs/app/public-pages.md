# Public App pages: ledger, network, status, mirror rewards

Standalone public pages in `app/public/` with no sign-in, styled with the
website page kit ([docs/www/README.md](../www/README.md)).

| Page | Data |
| --- | --- |
| `/ledger` (`ledger.js`) | `GET /api/ledger`, `GET /api/rewards/estimates`, `/api/world/wallet` |
| `/network` (`www/public/static-page.js`) | `GET /api/network/stats?payouts=1`, `/api/network/online-history`, `/api/network/leaderboards` |
| `/status` (inline script) | `GET /api/status` and the `/api/status/ws` stream |
| `/mirror-payouts` | `GET /api/network/stats`, `/api/rewards/pool` |
| `/leaderboards` | leaderboard endpoints; "My stats" uses the local session |

Ledger order (wallets, totals, records, integrity rules) is pinned by
`app/tests/test_ledger.py`. Network polling runs every five minutes only while
visible, with backoff on errors. Wallet balances come from an hourly cache;
Refresh requests a fresh on-chain read (`refresh=1`).

## Status page

- The page keeps `id="status-title"` and the `/api/status/ws` reference, which
  the status-page monitor checks.
- Each row shows its `checks` chips (`STATUS_SYSTEM_CHECK_STEPS` in
  `app/src/status_monitoring.py`) and a countdown from `nextCheck`.
- Probe-only rows turn red after two consecutive failing probes
  (`_status_probe_verdict`); visitor evidence turns rows red at once.
- Release rows, one per platform (`RELEASE_PLATFORM_MONITORS`), verify the
  signed manifest carries that platform's build and that it downloads.
- The Nodes section lists cores (`core:<node>`, from `cluster_members`
  heartbeats) and pool runners (`runner:<name>`); personal runners are never
  listed. Core rows page administrators continually by default. See
  [RUNNERS.md](../RUNNERS.md).
- A failing WebSocket constructor falls back to reconnect backoff.
- A row turned red by Worker errors gives only the status code and the route
  family (`_status_route_family`: `/api/<area>`, `/api/repo/[repository]/<kind>`,
  `/[repository]/info/refs`, `/@[account]`, or the first path segment;
  already-redacted private routes stay as they are), plus the extra count.
  The `error_log` message, which holds the exception and traceback, stays in
  the admin console; the one fixed phrase kept is "Exceeded allowed duration"
  for Durable Object aborts (`app/tests/test_status_page.py`).
