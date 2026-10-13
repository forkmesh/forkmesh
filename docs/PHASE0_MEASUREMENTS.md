# Phase 0 measurements (2026-09-23)

Measured on `fmnext-core-1`: Vultr vc2-1c-1gb (1 vCPU, 961 MB usable RAM), ewr, Debian 13, CPython 3.13.5.
- App: the unchanged Worker app on the CPython host shim, run with `python -OO` and `MALLOC_ARENA_MAX=2`.
- Database: a single rqlite v10.3.6 node on local disk with a fresh schema (201 migrations plus the app DDL, applied in 5.6 s).
- Measured from localhost, so these are node-side latencies with no network.

| What | Result | Plan gate |
|---|---|---|
| App RSS after a warm restart | 50 MB | ≤ 260 MB |
| App RSS after a sweep of the main routes | 58 MB | |
| App RSS on the first start after deploy (compiling `.pyc`) plus 300 open WebSockets | 132–140 MB | |
| rqlite RSS | 44–46 MB | ≤ 150 MB |
| Free memory (`available`) with both running | 638 MB | ≥ 150 MB |
| Time from startup to `/health` | 0.6 s | |
| `/api/version` p50 / p95 | 1.2 / 1.3 ms | |
| `/api/repositories` p50 / p95 | 4.0 / 4.4 ms | reads p50 ≤ 15 ms |
| `/api/status` p50 / p95 | 7.8 / 8.8 ms | |
| `/api/accounts/sessions` (authenticated) p50 / p95 | 10.7 / 11.8 ms | |
| `/api/ledger` p50 / p95 | 31.9 / 35.2 ms | slowest route measured |
| Login p50 (PBKDF2) | 128 ms | ≤ 300 ms |
| Signup | 351 ms | |
| rqlite single-row write p50 / p95 (1 node) | 1.7 / 2.7 ms | |

**Verdict:** the 1 GB core plan passes with a wide margin. Still to measure:
- write latency on the 3-node cluster;
- the k6 mix across round-robin DNS.
