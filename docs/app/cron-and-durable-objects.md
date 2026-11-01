# Cron and Durable Objects

Part of [the App docs](README.md). "Durable Objects" here are the App's
stateful room objects, emulated on the core nodes by the host
(`app/host/forkmesh_host/durable.py`, `durable_cluster.py`); the code keeps
the Durable Object API shape. How the cores share work and survive failures:
[architecture](../architecture.md).

## Durable Object namespaces

Listed in `app/wrangler.toml` (`[[durable_objects.bindings]]`; the file is not
deployed, the host's `env.py` reads its binding lists). Each object is placed
on one live core by rendezvous hash; storage and alarms are kept in rqlite
([d1.md](d1.md)).

| Binding | Class | Source | Role |
| --- | --- | --- | --- |
| `FORKMESH_MAINNODE_ROOM` | `ForkMeshRoom` | `app/src/entry.py` | Encrypted chat rooms and relay WebSockets |
| `FORKMESH_NODES` | `ForkMeshNodes` | `app/src/entry.py` | One object per account holding the event sockets of its desktops, mirrors and phones; carries payload-free frames. `watch:<owner>/<repo>` objects hold the unsigned, ping-only watch sockets of other relays' mirrors of a public repository and receive its `commits` doorbells ([instances](../operations/instances.md#the-platform-source-mirror-ownerforkmesh)) |
| `FORKMESH_NOTE_ROOM` | `ForkMeshNoteRoom` | `app/src/entry.py` | Live shared notes |
| `FORKMESH_WORLD` | `ForkMeshWorld` | `world/src/world_durable_objects.py` | World presence |
| `FORKMESH_OFFICE_ROOM` | `ForkMeshOfficeRoom` | `world/src/world_durable_objects.py` | Office meeting rooms |
| `FORKMESH_CRON_RUNNER` | `ForkMeshCronRunner` | `app/src/entry.py` | Cron coordinator (`scheduled-runner-v1`), snapshot republish doorbell (`snapshot-republish-v1`), mirror-wake doorbell, sixteen ingress shards |
| `FORKMESH_CRON_WATCHDOG` | `ForkMeshCronWatchdog` | `app/src/entry.py` | Pushes its alarm forward on each completed tick; emails once on outage and once on recovery |
| `FORKMESH_DISCORD_GATE` | `ForkMeshDiscordGate` | `app/src/entry.py` | Singleton Discord API rate gate |
| `FORKMESH_ORG_OFFICE` | `ForkMeshOrgOffice` | `app/src/org_office_room.py` (class in `app/src/entry.py`) | One organization office per organization: sockets, desks, whiteboard and attendance ([../office/README.md](../office/README.md)) |

## The per-minute cron

Each minute the host calls `Default.scheduled` (`app/src/app_runtime.py`),
which only rings `scheduled-runner-v1`; that object (placed on one live core)
runs every phase due that minute in its alarm, and `host_cron_runs` records
which node ran which minute.

- Phases are selected by minute residues (for example `webhook-outbox` at
  `minute % 5 == 4`). The status chain `CRON_STATUS_CHAIN` (`status-sample`,
  `status-alert-pings`, `status-alert-email`, `snapshot-status`) runs first,
  in order.
- Each phase's measured subrequest high-water (`phase_cost_v5`, outbound
  fetches and database calls) is reserved before it runs under
  `CRON_BATCH_SUBREQUEST_CAP`; when the next one does not fit, progress is
  saved in `tick_state_v5` and the alarm re-arms. A busy minute is a short
  chain on one object, never a fan-out.
- An exhausted phase budget (`_cron_budget_exhausted`) is recorded in
  `cron_tick`, not logged as an error; the next minute delivers what is due.
- A phase interrupted mid-run is reported incomplete, not replayed, unless it
  is in `CRON_PHASE_REPLAY_SAFE`.
- Every tick writes a `cron_tick` row (six-hour retention), readable at
  `GET /api/accounts/admin-cron-report`.

## Event-driven snapshot republish

A mutation that changes an anonymous catalog projection rings
`snapshot-republish-v1` (`schedule_public_catalog_republish`). Its alarm
republishes the affected public snapshot paths, at most one trailing rebuild
per path every `SNAPSHOT_REPUBLISH_MIN_INTERVAL_MS`. The cron is the
freshness floor.

A snapshot build runs under the same 45-call guard as a cron phase, so its
cost must not grow with the number of rows it shows. On 2026-11-01
`/api/network/overview` failed every republish with "cron phase subrequest
budget exhausted": the contributors board looked up each contributor's
profile on its own (one or two D1 reads per row, added for the private
profile label), and the largest-repositories board resolved each row's
organization address even on an instance without short addresses, where
that can never change a row. The contributors board now reads all of its
accounts in at most two queries (`_account_identity_recs_by_bi`), and
`_short_repo_board` returns at once unless a canonical short owner is set.
The whole build costs about 34 calls whatever the board sizes;
`app/tests/host/test_network_overview_budget.py` builds it on the host
runtime with full boards and checks it fits `CRON_BATCH_SUBREQUEST_CAP`
minus the reserve headroom.

## Ingress shards

Only mirror-daemon sync, Git clone reads and a short list of bursty public
reads cross into an ingress shard (`_app_ingress_fetch`). When the shard is
unavailable those requests get a marked 503 with `retry-after: 30`.

## Rules

- Do not reintroduce per-phase objects; retired object names answer 404.
- Do not start detached tasks from a request; use a doorbell object with an
  alarm.
