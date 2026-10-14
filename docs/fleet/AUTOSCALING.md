# Fleet autoscaling

The fleet grows and shrinks two pools without the operator box:

- **App capacity:** app replicas, label `fmnext-auto-N`, fleet name `aN`,
  WireGuard `10.77.0.2N`.
- **Burst runners:** throwaway agent-job machines, label `fmnext-burst-N`,
  fleet name `bN`.

Cores and the always-on runner are never scaled, listed, touched or destroyed
by the autoscaler; rqlite voters stay at three. The desktop (Network, Hosts)
is only the control plane: it signs settings and commands and reads status.
Scaling keeps working while it is off.

## Where the scaler runs

On the cores, inside the app host (`app/host/forkmesh_host/autoscaler.py`),
leaderless:

- Every 60 s each unsealed, hosting core checks whether it owns this minute of
  the cron rotation (`cluster.rotation_owner`).
- The owner takes the rqlite lease `autoscale_lease` (compare-and-set, 150 s,
  renewed while working), so only one actor runs per tick.
- Ticks are short; nodes move one step per tick through a state machine:
  `creating` → `booting` → `joining` → `serving`, and `draining` → `removing`
  → gone. A core that dies mid-action is replaced by the next tick's owner.
- Replicas never host Durable Objects or own cron, so they never run it.

## Settings

One JSON object: a global `killSwitch` and, per pool, `enabled`, `dryRun`,
`min`, `max`, scale-up and scale-down rules, `cooldownMinutes`, `plan`,
`region`, `monthlyCapCents` and the snapshot id.

- Signed by the operator Ed25519 key (`config/fleet/ids-operator.key`) and sent
  with `tools/next_autoscale.py settings` to `POST /api/fleet/autoscale/settings`.
- Cores verify against `FORKMESH_IDS_OPERATOR_PUBKEY` and require a `signedAt`
  newer than the stored one; every tick re-verifies, and an invalid row means
  "act on nothing".
- Status and manual commands are signed the same way with a timestamp and a
  single-use nonce (60 s window).

### Defaults

| | app replicas | burst runners |
|---|---|---|
| enabled / dry run | off / on | off / on |
| min / max extra nodes | 0 / 2 | 0 / 2 |
| scale up | p95 > 500 ms, loop lag > 250 ms or load/CPU > 0.8, for 5 min | queue ≥ 1 job waiting ≥ 2 min, and the always-on runner has no free slot |
| scale down | every signal under 50 % of its threshold for 30 min | idle 10 min |
| cooldown | 10 min | 10 min |
| plan / region | `vc2-1c-1gb` / `ewr` (the cores' plan) | same |
| monthly cap | $15 | $10 |

Both pools start off and in dry run. Two extras per pool fit a $25 combined
monthly cap. The p95 threshold is far above the normal target, so only real
overload scales. Burst runners never scale up automatically until per-job
provider credentials exist (`BURST_JOBS_READY = False` in
`autoscale_policy.py`; [../RUNNERS.md](../RUNNERS.md) mode B).

### Hard limits (in code)

At most 4 nodes and $100 a month per pool; cooldown at least 5 minutes; at
most one create or destroy per pool per cooldown and 12 changes per 24 h;
`min` is never undercut; reaching the monthly cap destroys the pool's nodes
one per tick; the kill switch stops every create and destroy. Signed manual
scale commands run even while a pool is disabled or in dry run.

## Metrics

Each node keeps a 5-minute window of request p50/p95, event-loop lag, RPS and
load per CPU (`host_metrics.py`) and upserts it into `host_metrics` every 30 s.
The tick owner aggregates rows fresher than 2 minutes (max of p95 and lag,
mean of load and RPS). No fresh metrics means hold.

## Cloud credential

The provider API key lives only in the vault and in unsealed core memory.
Every call goes through `vultr_guard.py`:

- no listing; only create, read one instance by id, and delete one by id;
- create only with labels `fmnext-auto-<1..9>` or `fmnext-burst-<1..9>`, the
  `forkmesh-next` tag, the configured plan, region and snapshot, recorded in
  `autoscale_nodes`;
- destroy only a recorded id after a fresh read confirms id, label and tag;
- core and always-on runner labels are always refused.

`app/host/tests_unit/test_vultr_guard.py` checks every rule. The guard binds
honest code only: a compromised core holding the key could call the provider
directly. Use a sub-user key limited to instances and snapshots, billing
alerts, and a provider account that holds nothing else.

## Provisioning without operator keys

**Snapshot.** `tools/next_autoscale.py snapshot build --execute` creates a
builder `fmnext-snap-N`, installs packages, rqlite, Caddy, the daemon and the
join agent, strips machine identity (host keys, `machine-id`, cloud-init
state, logs), installs only public keys, snapshots, destroys the builder and
publishes the id with a signed settings write. Rebuild it after changes to the
log reader, rqlite, Caddy, the daemon or the OS.

**Join.** A new instance boots with a one-time join token (name, provider id,
30-minute expiry, nonce, MACed under a key derived from `DATA_KEY`).
`fmnext-join`:

1. generates its WireGuard and IDS keys on the node;
2. calls `POST /api/fleet/join`; the core checks the MAC, expiry, single use,
   that the source IP equals the instance's address from a fresh provider
   read, and that label, tag and name match the record;
3. receives its rendered files, bootstrap credentials and release id;
4. installs and starts WireGuard; the cores' root agent `fmnext-fleet-peers`
   (every 15 s) adds the peer, firewall set element and certsync allowance;
   rqlite joins as a non-voter; certsync follows and never issues;
5. downloads and verifies the release, builds the venv offline and starts;
6. peer-unseals from a core (cores accept requesters in `autoscale_nodes` from
   their own WireGuard address).

Burst runners skip WireGuard, rqlite and the app and are registered as pool
runners with no owners.

**IDS keys.** The vault holds a sub-master derived from the IDS master; cores
can derive node keys only for `a1..a9` and `b1..b9`.

**DNS.** A serving replica enters DNS through the operator-box health loop, or
through the node reconciler when it is enabled with a DNS-scoped token.

**Scale-down.** Mark `draining` (health loops remove it from DNS), wait 3
minutes, remove the rqlite non-voter (or revoke the runner), destroy through
the guard, delete the row; peers drop the WireGuard peer within 15 s.

## Audit

Every non-trivial decision, create, destroy, join, failure, settings write and
command is recorded in `autoscale_audit`, and every real action also raises a
`security_incidents` row (detector `autoscaler`) delivered to admins.

## Threat model

- **Compromised core app:** can use the provider key directly; cannot obtain
  the IDS master or the core certsync secret.
- **Forged join:** tokens are short-lived, single-use, MACed, and bound to the
  provider id and the source IP read fresh from the provider.
- **Malicious `autoscale_nodes` rows:** the root peers agent accepts only
  expected names, mesh addresses and well-formed keys.
- **Replayed commands:** newer-than-stored settings, single-use nonces.
- **Runaway cost:** hard limits, caps, kill switch, dry run by default.
- **Split brain:** replicas host no objects or cron; voters fixed at three;
  lease plus rotation.

## Code

- App host: `autoscaler.py`, `autoscale_policy.py`, `vultr_guard.py`,
  `fleet_api.py`, `host_metrics.py`, replica mode in `cluster.py` and
  `durable_cluster.py`, replica peer unseal in `vault.py`.
- Deploy: `deploy/core/fleet_peers.py`, `deploy/core/join_agent.py`, replica
  render in `render.py`, certsync follower in `certsync.py`. Each core needs
  the `AUTOSCALE_CERTSYNC_SECRET` credential file.
- Tools: `tools/next_autoscale.py`, `tools/next_ids.py`, `tools/next_dns.py`.
- Migration: `app/migrations/0211_fleet_autoscale.sql`.
- Desktop: `desktop/src/FleetAutoscalePanel.*`.
