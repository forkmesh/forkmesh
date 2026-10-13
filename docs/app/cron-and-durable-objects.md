# Cron and Durable Objects

## Durable Object namespaces

All classes are bound in `app/wrangler.toml` (`[[durable_objects.bindings]]`,
with the class history in `[[migrations]]` tags v1 to v16).

| Binding | Class | Source | Role |
| --- | --- | --- | --- |
| `FORKMESH_MAINNODE_ROOM` | `ForkMeshRoom` | `app/src/entry.py` | Encrypted chat rooms and relay WebSockets |
| `FORKMESH_NODES` | `ForkMeshNodes` | `app/src/entry.py` | One object per account holding the hibernated node event sockets of its desktops, mirrors and phones; the relay pushes payload-free frames through it |
| `FORKMESH_NOTE_ROOM` | `ForkMeshNoteRoom` | `app/src/entry.py` | Live shared notes |
| `FORKMESH_WORLD` | `ForkMeshWorld` | `world/src/world_durable_objects.py` | World presence |
| `FORKMESH_OFFICE_ROOM` | `ForkMeshOfficeRoom` | `world/src/world_durable_objects.py` | Office meeting rooms and chairs |
| `FORKMESH_CRON_RUNNER` | `ForkMeshCronRunner` | `app/src/entry.py` | The cron coordinator (`scheduled-runner-v1`), the snapshot republish doorbell (`snapshot-republish-v1`), the mirror-wake doorbell, and sixteen fetch-only ingress shards |
| `FORKMESH_CRON_WATCHDOG` | `ForkMeshCronWatchdog` | `app/src/entry.py` | Moves its alarm three minutes forward on each completed tick; sends one outage email when it fires and one recovery email after |
| `FORKMESH_DISCORD_GATE` | `ForkMeshDiscordGate` | `app/src/entry.py` | Singleton Discord API rate gate; stores only bucket metadata |

## The per-minute cron

`[triggers] crons = ["* * * * *"]`. `Default.scheduled`
(`app/src/app_runtime.py`) only rings `scheduled-runner-v1`. That object's
alarm runs every phase due that minute itself:

- The phase list for a minute is built by minute residues (for example
  `usage-budget` at `minute % 10 == 6`, `webhook-outbox` at `minute % 5 == 4`,
  `snapshot-status` every 3 minutes). The status chain
  `CRON_STATUS_CHAIN` (`status-sample`, `status-alert-pings`,
  `status-alert-email`, `snapshot-status`) leads and stays ordered.
- Workers Free allows 50 subrequests per invocation. Each phase's measured
  subrequest high-water (`phase_cost_v5`) is reserved before it runs; when the
  next one does not fit, the object saves progress in `tick_state_v5` and
  re-arms its alarm for now. A busy minute is a short chain of invocations on
  the same object, never a fan-out.
- `status-alert-pings` also rings calendar, package and persistent task
  reminders. The package and task wrappers log their own failures as a 500
  error group, except an exhausted phase budget (`_cron_budget_exhausted`):
  that is re-raised so the runner records it in `cron_tick` and the next
  minute delivers what is still due. Logging it raised a Worker-errors alert
  (`plugins/reminders`, 2026-10-07) for what was only a busy minute.
- A phase that was running when an invocation died is reported incomplete,
  not replayed, unless it is in `CRON_PHASE_REPLAY_SAFE`.
- Every tick writes one `cron_tick` D1 row (six-hour retention). Read it with
  `GET /api/accounts/admin-cron-report` or the desktop Log page's Cron tab.

## Event-driven snapshot republish

A mutation that changes an anonymous catalog projection rings
`snapshot-republish-v1` (`schedule_public_catalog_republish`). Its alarm
republishes the affected public snapshot paths, coalesced per path to at most
one trailing rebuild every `SNAPSHOT_REPUBLISH_MIN_INTERVAL_MS` (two minutes).
The cron cadence remains the freshness floor.

## Budgets that bite

- **DO duration** is an account-wide daily allowance on Workers Free. An
  object is billed while it has work in flight; one that never goes quiet for
  about ten seconds stays resident all day. When the allowance runs out every
  object fails until 00:00 UTC, and Cloudflare may then serve a zone-wide 429
  page after sustained exceptions. The `usage-budget` phase samples the three
  allowances and Pings administrators at 10, 50, 80 and 90 percent.
- **Ingress shards.** Only mirror-daemon sync, Git clone reads and a short
  list of bursty public reads cross into an ingress shard
  (`_app_ingress_fetch`). When the shard is unavailable those requests get a
  marked 503 with `retry-after: 30`; ordinary API requests are unaffected.

## Traps

- The retired `scheduled-batch-*`, `scheduled-phase-v2:*` and
  `scheduled-status-v1:*` object names answer 404; do not reintroduce
  per-phase objects.
- Do not start detached Python tasks (`ctx.waitUntil` on a coroutine) from a
  request; use a doorbell object with an alarm instead.

## Related

- `www/docs/operations/worker-split.md` (scheduled maintenance section)
- `www/docs/operations/cloudflare-free-tier-audit.md`
- [App Worker home](README.md)
