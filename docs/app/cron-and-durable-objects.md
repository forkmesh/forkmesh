# Cron and Durable Objects

## Durable Object namespaces

Bound in `app/wrangler.toml` (`[[durable_objects.bindings]]`). On the cores
each object is placed on one live node by rendezvous hash; storage and alarms
are kept in rqlite.

| Binding | Class | Source | Role |
| --- | --- | --- | --- |
| `FORKMESH_MAINNODE_ROOM` | `ForkMeshRoom` | `app/src/entry.py` | Encrypted chat rooms and relay WebSockets |
| `FORKMESH_NODES` | `ForkMeshNodes` | `app/src/entry.py` | One object per account holding the event sockets of its desktops, mirrors and phones; carries payload-free frames |
| `FORKMESH_NOTE_ROOM` | `ForkMeshNoteRoom` | `app/src/entry.py` | Live shared notes |
| `FORKMESH_WORLD` | `ForkMeshWorld` | `world/src/world_durable_objects.py` | World presence |
| `FORKMESH_OFFICE_ROOM` | `ForkMeshOfficeRoom` | `world/src/world_durable_objects.py` | Office meeting rooms |
| `FORKMESH_CRON_RUNNER` | `ForkMeshCronRunner` | `app/src/entry.py` | Cron coordinator (`scheduled-runner-v1`), snapshot republish doorbell (`snapshot-republish-v1`), mirror-wake doorbell, sixteen ingress shards |
| `FORKMESH_CRON_WATCHDOG` | `ForkMeshCronWatchdog` | `app/src/entry.py` | Pushes its alarm forward on each completed tick; emails once on outage and once on recovery |
| `FORKMESH_DISCORD_GATE` | `ForkMeshDiscordGate` | `app/src/entry.py` | Singleton Discord API rate gate |

## The per-minute cron

`Default.scheduled` (`app/src/app_runtime.py`) only rings
`scheduled-runner-v1`; that object's alarm runs every phase due that minute.

- Phases are selected by minute residues (for example `webhook-outbox` at
  `minute % 5 == 4`). The status chain `CRON_STATUS_CHAIN` (`status-sample`,
  `status-alert-pings`, `status-alert-email`, `snapshot-status`) runs first,
  in order.
- Each phase's measured subrequest high-water (`phase_cost_v5`) is reserved
  before it runs under `CRON_BATCH_SUBREQUEST_CAP`; when the next one does not
  fit, progress is saved in `tick_state_v5` and the alarm re-arms. A busy
  minute is a short chain on one object, never a fan-out.
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

## Ingress shards

Only mirror-daemon sync, Git clone reads and a short list of bursty public
reads cross into an ingress shard (`_app_ingress_fetch`). When the shard is
unavailable those requests get a marked 503 with `retry-after: 30`.

## Rules

- Do not reintroduce per-phase objects; retired object names answer 404.
- Do not start detached tasks from a request; use a doorbell object with an
  alarm.
