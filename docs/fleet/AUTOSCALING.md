# Fleet autoscaling

The fleet grows and shrinks two pools without the operator desktop:

- **App capacity:** extra web/API nodes. They are app replicas: label `fmnext-auto-N`, fleet name `aN`, WireGuard `10.77.0.2N`.
- **Burst runners:** throwaway agent-job machines. Label `fmnext-burst-N`, fleet name `bN`.

The three cores `n1`–`n3` and the always-on runner `r1` (`fmnext-run-1`) are never scaled, listed, touched or destroyed by the autoscaler. Voters stay at exactly three.

The desktop (Network → Hosts) is only the control plane. It signs settings and commands with the operator key and reads status from the cluster. Scaling keeps working while this computer is off.

## Where the scaler runs

On the cores, inside the app host (`app/host/forkmesh_host/autoscaler.py`), leaderless:

- **Owner per tick.** Every 60 s each unsealed, hosting core asks whether it owns this minute of the cron rotation (`cluster.rotation_owner` over the live cores, the same rotation the per-minute cron uses). Only that core goes on.
- **Lease.** The owner takes the rqlite lease `autoscale_lease`: a compare-and-set on one row, 150 s long and renewed while it works. If the rotation view ever disagrees, the lease still guarantees that two cores never act at once. A core that loses the lease stops at its next step.
- **Short ticks.** A tick never blocks for minutes. Nodes move through a state machine, one step per tick:
  - scale-up: `creating` → `booting` → `joining` → `serving`;
  - scale-down: `draining` → `removing` → gone.

  So a core that dies mid-action is simply replaced by the next tick's owner.
- **Replicas never run it.** App replicas never host Durable Objects and never own cron (replica mode below), so they never run the scaler.

## Settings (control plane)

Settings are one JSON object: a global `killSwitch`, and per pool `enabled`, `dryRun`, `min`, `max`, the scale-up and scale-down rules, `cooldownMinutes`, `plan`, `region`, `monthlyCapCents` and `snapshotId`.

- **Signing.** The desktop signs them with the operator Ed25519 key (`config/fleet/ids-operator.key`, desktop-only). It sends `POST /api/fleet/autoscale/settings` through `tools/next_autoscale.py settings set`.
- **Checks on write.** The cores verify the signature against `FORKMESH_IDS_OPERATOR_PUBKEY`, which is rendered into every core's `app.env`, and reject a `signedAt` that is not newer than the stored one (no replay). They store the signed document in `autoscale_settings`.
- **Checks on use.** Every tick re-verifies the stored document. An unverifiable or invalid row means "act on nothing".
- **Status and commands.** Status and manual commands (`scale app up`, and so on) are signed the same way: operator key, timestamp and nonce, 60 s window, single use.

### Defaults, and why

| | app replicas | burst runners |
|---|---|---|
| enabled / dry run | off / on | off / on |
| min / max extra nodes | 0 / 2 | 0 / 2 |
| scale up | p95 > 500 ms, loop lag > 250 ms or load/CPU > 0.8, for 5 min | queue ≥ 1 job waiting ≥ 2 min, and r1 has no free slot |
| scale down | every signal under 50 % of its threshold for 30 min | idle 10 min |
| cooldown | 10 min | 10 min |
| plan / region | `vc2-1c-1gb` / `ewr` (the cores' plan) | same |
| monthly cap | $15 | $10 |

The reasons:

- **Both pools start off and in dry run.** Turning scaling on is a deliberate act.
- **Two extras per pool.** Each 1 GB node is about $5/month. Two extras running all month cost about $10, which fits the $25 total cap with room for the burst tier.
- **The app thresholds.** The p95 threshold is well above the 1 GB target (reads p95 ≤ 80 ms, docs/PLAN.md), so only real overload scales. Five minutes of sustained breach filters spikes. Thirty minutes at half the threshold avoids flapping.
- **Burst runners never scale up automatically** until per-job provider credentials exist (RUNNERS.md "Mode B"). `BURST_JOBS_READY = False` in the policy means queued work only produces a "held" decision. Manual scale-up works, for tests. A burst runner is registered with `allowed_owners = []`, so it can never claim a job.

### Hard limits (in code, not settable)

- At most 4 nodes per pool.
- A monthly cap of at most $100 per pool.
- A cooldown of at least 5 minutes.
- At most one create or destroy per pool per cooldown.
- At most 12 changes per pool per 24 h.
- `min` is never undercut.
- Reaching the monthly cap destroys the pool's nodes one per tick and creates nothing more.
- The kill switch stops every create and destroy. It can be set only from a signed settings write.
- **Manual scale commands** ("Scale up now" on the Hosts page, `next_autoscale.py scale … --execute`) run even while a pool is disabled or in dry run: they are a deliberate operator act, signed with the operator key. They still obey the hard node limit, the monthly cap, "one node changing at a time" and the kill switch.

## Metrics

Every node's host keeps a 5-minute window of request latency (p50/p95), event-loop lag (a 0.5 s drift probe), RPS and load average per CPU (`host_metrics.py`).

- **Publishing.** Each node upserts its snapshot into `host_metrics` every 30 s.
- **Signed readout.** `/readyz?metrics=1` with a mesh-HMAC header also returns it, for tools.
- **Aggregation.** The tick owner reads rows fresher than 2 minutes from cores and serving replicas. It takes the max of p95 and loop lag, and the mean of load/CPU and RPS per node.
- **No metrics.** A tick with no fresh metrics holds; it never scales on missing data.

## Vultr credential

The user decided to use the current account API key. It lives only in the vault (`VULTR_API_KEY` in `host_secrets`, sealed under `DATA_KEY`). Unsealed cores hold it in memory; it is never in a file, a log or git.

The key can reach unrelated instances (tradeskie-production, Blt), so every call goes through `vultr_guard.py`, which enforces an allowlist:

- **No listing.** It never lists instances. It only creates, reads one instance by id, creates a snapshot-backed instance, and deletes one instance by id.
- **Create.** Only with the label `fmnext-auto-<1..9>` or `fmnext-burst-<1..9>`, the tag `forkmesh-next`, the configured plan and region, and a snapshot id. A creation record (`autoscale_nodes`: name, label, Vultr id) is written **before** the create call returns control, and the id is stored as soon as it is known.
- **Destroy.** Only an id recorded in `autoscale_nodes` by the autoscaler, immediately after a fresh `GET /instances/<id>` that shows the same id, the recorded label, the `forkmesh-next` tag and a matching creation record. Any mismatch refuses and raises an alert.
- **Refused always.** Labels `fmnext-core-*` and `fmnext-run-*` (r1), and any id not in the table.
- **Tests.** `app/host/tests_unit/test_vultr_guard.py` checks every rule against a fake Vultr.

A compromised core app could still spend money up to the Vultr account's own limits by calling the API directly with the key it holds. Nothing in software prevents that. See the threat model.

## Provisioning without desktop keys

### The snapshot

The desktop builds it with `tools/next_autoscale.py snapshot build --execute`:

1. It creates a builder instance `fmnext-snap-1` with the fleet SSH key.
2. It runs the base install for both roles: packages, users, directories, rqlite, Caddy and the daemon binaries, and the join agent `/opt/forkmesh-next/bin/fmnext-join`.
3. It removes everything that identifies a machine: SSH host keys, `machine-id`, cloud-init state, logs and root's shell history. No WireGuard, IDS or node key is ever created on a builder.
4. It installs root's `authorized_keys` with only the fleet public key and the log-reader line, snapshots the builder, and destroys it.
5. It publishes the snapshot id through a signed settings write.

The snapshot holds binaries and public keys only.

Refresh the snapshot after a change to `deploy/core/logread.py`, the `[logs]` key, rqlite, Caddy or the Go daemon, or after an OS point release. While the desktop is offline, scaling keeps using the last published snapshot. The release itself is never baked in: nodes fetch the cores' current release at join time.

### Join

A new instance boots with cloud-init `user_data` carrying a **one-time join token**. The token is `aN|bN`, the Vultr id, an expiry 30 minutes out and a nonce, MACed with `HMAC(DATA_KEY, "fmnext-autoscale-join-v1")`. Any unsealed core can mint and check it, and only the SHA-256 of the token is stored.

`fmnext-join` then does the following:

1. **Generate keys on the node.** It generates its WireGuard key and IDS signing key; a burst runner also initialises its node identity. Private keys never leave the node.
2. **Call join.** It calls `POST https://app.forkmesh.com/api/fleet/join` with the token and the public keys. The core checks, in order:
   - the MAC and the expiry;
   - that the token hash is unused;
   - that the source IP equals the instance's `main_ip` from a fresh `GET /instances/<recorded id>`;
   - that the label, tag and name match the record.

   It then marks the token used.
3. **Receive the bundle.** The answer holds:
   - the node's files, rendered by `deploy/core/render.py` from the release's `deploy/` tree, the same render as the desktop's;
   - its bootstrap credentials: the rqlite auth, the mesh secret, the replica certsync secret, and `FMNEXT_IDS_KEY` derived from the autoscale IDS sub-master;
   - the release id.
4. **Install.** The node installs the files, encrypts the credentials with `systemd-creds` and starts WireGuard.
   - The cores' root agent `fmnext-fleet-peers` (every 15 s) sees the `autoscale_nodes` row and adds the WireGuard peer, the nftables set element and the certsync allowance.
   - rqlite then joins as a **non-voter**.
   - certsync pulls the TLS pair: it is a follower and never issues.
5. **Fetch the release.** It downloads the release over HTTPS (`GET /api/fleet/release/<rev>`, authorised by the token until the node is serving), checks its SHA-256, builds the venv offline and starts the app in replica mode, **sealed**.
6. **Unseal and serve.** The app peer-unseals from a core. For replicas, the cores' vault accepts requesters in `autoscale_nodes` (state `joining` or `serving`, from their own WireGuard address, and not quarantined). `/readyz` turns green and the tick owner marks the node `serving`.
7. **Burst runners** skip WireGuard, rqlite and the app. The core registers `fmnext-bN` in `runner_nodes` as a pool runner with no owners, and the daemon starts polling.

No SSH key is needed for any of this. The desktop's fleet key is in `authorized_keys` so the operator can inspect nodes, and the log reader works through the `logread` contract (docs/NODE_LOGS.md).

### IDS on autoscaled nodes

The vault holds `AUTOSCALE_IDS_SUBMASTER = HMAC(FMNEXT_IDS_MASTER, "fmnext-ids-autoscale-submaster-v1")`. A node key for `aN` or `bN` is `HMAC(submaster, "fmnext-ids-node-key-v1:<name>")`. `tools/next_ids.py` derives the same key, so the desktop can still push expected-change windows to those nodes.

The cores can derive keys only for `a1..a9` and `b1..b9`, never for `n*` or `r*`. The desktop-only master stays on this box.

### DNS

A serving replica joins `app`/`world` through the DNS health loop:

- **This box, when online.** `fmnext-dns-health` reads the serving replicas from the cluster with a signed status call.
- **The node reconciler, when this box is off.** `dns_reconciler.py`, running on the cores, includes serving replicas. It changes records only while `[dns] reconciler = "on"` and a DNS-scoped token is in the vault.

Until the user adds a token scoped to the forkmesh.com DNS records, a replica created while this box is off serves nothing until the box returns. It still costs money, so it stays within the cap. This is a required user decision.

### Scale-down

1. The tick owner marks the node `draining`. The health loops remove it from DNS.
2. After 3 minutes (TTL 60 s plus slack), the node moves to `removing`. The owner does an rqlite `DELETE /remove` for the non-voter (or revokes the runner row), and the destroy goes through the guard.
3. The row is deleted.
4. The cores' `fmnext-fleet-peers` drops the WireGuard peer and the nftables element within 15 s.

## Audit and Pings

Every decision that is not "none", every create, destroy, join, failure, settings write and command lands in `autoscale_audit`.

Every real action also inserts a `security_incidents` row with detector `autoscaler`, which the cores deliver as a Ping and email to every admin. `ids_alerts` titles these rows "Fleet autoscaler".

## Threat model

**Assets:** the Vultr account (other businesses' instances on it), the cluster's data and keys, and money.

- **Compromised core app** (the realistic attacker). It already holds `DATA_KEY`, the vault and rqlite write access, and it now also holds the Vultr key. It can:
  - read the Vultr key and call Vultr directly. That includes deleting unrelated instances or creating costly ones: the guard binds honest code, not an attacker who holds the key.

  It cannot:
  - obtain the IDS master (only the sub-master, which covers `aN`/`bN` names);
  - obtain the core certsync secret. The replica certsync secret is accepted only from WireGuard addresses in the autoscaled range, and a core's app is not on one.

  Mitigations, which the user should adopt:
  - a Vultr sub-user key limited to the instance and snapshot permissions (Vultr offers no per-instance scoping);
  - Vultr account billing alerts;
  - moving tradeskie-production and Blt to a separate Vultr account. That removes the cross-business blast radius entirely.
- **Forged join.** Tokens are short-lived, single-use and MACed under `DATA_KEY`, and they are bound to the Vultr id and to the source IP read fresh from Vultr. A stolen `user_data` token (Vultr metadata is readable by root on that instance only) is useless once used or after 30 minutes.
- **Malicious rows in `autoscale_nodes`.** A row is written by a compromised app. The root peers agent accepts only names `a1..a9`, WireGuard IPs `10.77.0.21..29`, well-formed keys and public IPv4 endpoints, and it never touches core peers. The worst case is a WireGuard peer inside the replica range. That peer still needs the rqlite password (already held by such an attacker) to join rqlite.
- **Replay of desktop commands.** Settings must be newer than the stored ones. Commands carry a single-use nonce and a 60 s window.
- **Runaway cost from a bug.** The hard limits above, the per-pool cap, the kill switch and dry run by default.
- **Split brain.** Replicas never host Durable Objects or cron (replica mode). Voters are fixed at 3. The lease plus the rotation ensure one actor per tick.
- **Desktop offline.** Nothing on the desktop is needed to scale up or down. What degrades is DNS entry (above) and snapshot refresh.

## Code

- **App host:**
  - `autoscaler.py`: the tick owner, the state machine, and observation from `host_metrics`;
  - `autoscale_policy.py`: validation, signatures and the pure decisions;
  - `vultr_guard.py`;
  - `fleet_api.py`: the join, release, settings, status and command endpoints;
  - `host_metrics.py`;
  - replica mode in `cluster.py`/`durable_cluster.py`;
  - replica peer unseal in `vault.py`.
- **Deploy:**
  - `deploy/core/fleet_peers.py`: the root agent on the cores;
  - `deploy/core/join_agent.py`: `fmnext-join` on new nodes;
  - replica/burst render in `render.py`;
  - the certsync follower and replica secret in `certsync.py`.
- **Tools:**
  - `tools/next_autoscale.py`: the signed CLI for the desktop, and the snapshot builder;
  - `tools/next_ids.py`: sub-master derivation;
  - `tools/next_dns.py`: serving replicas from the cluster.
- **Migration:** `app/migrations/0211_fleet_autoscale.sql`.
- **Desktop:** `desktop/src/FleetAutoscalePanel.*` on Network → Hosts, which replaced the old mirror fleet sizing.

## The certsync replica secret stays on the cores

`AUTOSCALE_CERTSYNC_SECRET` is in the vault list (`[vault]` in
`deploy/fleet.toml`), but `fmnext-certsync.service` on every core loads it
with `LoadCredentialEncrypted`, so each core also needs its `.cred` file.
The `secrets` step used to write that file and then delete it in its
"now in the vault" cleanup. The unit then failed at CREDENTIALS every 5 s
(about 11,000 restarts on each core by 2026-09-26), and certificate sync
between cores stopped. `next_fleet.node_local_credential_names` now keeps
it out of the stale list on cores.

## Operating it

The one-time rollout, from the desktop:

1. `tools/next_vault.py sync --execute`, after adding `VULTR_API_KEY`, `AUTOSCALE_IDS_SUBMASTER` and `AUTOSCALE_CERTSYNC_SECRET` to `secrets.env`. Use `next_autoscale.py init-secrets --execute`, which derives or generates them and never prints them.
2. Roll the cores (`next_fleet.py install nN --steps files,secrets,start`, one at a time and drained). This installs `fmnext-fleet-peers`, the nftables set and the certsync replica secret.
3. Deploy the app release (`next_deploy.py all --execute`).
4. `next_autoscale.py snapshot build --execute`.
5. Enable the pools from the Hosts page, first in dry run.

Status: `next_autoscale.py status`, or the Hosts page. Emergency stop: set the kill switch on the Hosts page, or run `next_autoscale.py settings kill --execute`.
