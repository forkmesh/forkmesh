# ForkMesh D1 database audit — 2026-09-10

> **Status on 2026-09-24.** This audit is a dated snapshot and is not kept
> current. The live work list, with corrections and per-step status, is
> [`www/docs/operations/d1-database-cleanup-plan.md`](../../www/docs/operations/d1-database-cleanup-plan.md).
> Changes since the audit, checked against `app/migrations/` and `app/src/`:
>
> - Done: the public catalog is served from one snapshot row (migration
>   `0179_public_catalog_snapshot.sql`, section 5 item 4 in part); heavy
>   readers share one colo-wide copy through `app/src/d1_read_guard.py`
>   (migration `0186_d1_read_guard.sql`); the three `polar_*` tables are
>   dropped (`0189_drop_polar_tables.sql`, Polar removed); `api_metrics_minute`
>   is dropped and Web Requests stats come from the Worker log tail
>   (`0190_drop_api_metrics_minute.sql`), which retires finding 4.4 item 4 for
>   that table.
> - Still open: the dead-table drop (item 1: `host_presence`,
>   `system_status_minute`, `telemetry`, `commit_inbox` still have readers or
>   references in `app/src`), the migration back-fill (item 2), the plaintext
>   `deposit_secret` column (item 3), per-status audit rows (item 5), rate-table
>   prunes, legacy custody tables (item 6), and the key-version byte (item 7).
> - New tables since the audit include `ad_network_*` (0187), `repo_webhook*`
>   (0188), `org_agent_attachments` (0191), and the `analytics_*` tables
>   created by the `analytics` package.

Scope: the production Cloudflare D1 database `forkmesh` (binding `DB`, id `2d33ad9e-…`) behind
`app.forkmesh.com`. Durable Object storage (chat rooms, world, cron runner) and the desktop node's
local `forkmesh.db` are out of scope; they are noted only where they explain a D1 table.

Sources used:

- Live production D1 via read-only `wrangler d1 execute --remote` (object list, row counts,
  ciphertext spot checks, payload-size estimates) and `wrangler d1 info` / `wrangler d1 insights
  --time-period 7d` for traffic.
- The 2026-07-23 D1 snapshot in `deploy-backups/20260724T015026Z/…/forkmesh-d1-snapshot-post-scrub.sqlite`
  for 49-day growth comparison.
- A clean replay of `app/migrations/*.sql` into scratch SQLite, plus `app/src/schema.py`,
  `app/src/entry.py` (encryption primitives, retention constants) and every `app/src`,
  `world/src` and `marketplace/packages/*/content/worker` module for table references.

**Corrections (2026-09-13).** A code re-check while turning this audit into a tracked plan
reversed four claims in the first draft: `relay_self` is live (keep it); `telemetry` is an orphan
to export and drop, not a table needing a cap; `host_presence` has no writer, so its readers are
the dead part; and the `schema_meta` fast path is already memoized per isolate, so its query count
measures isolate churn. Sections 3–5 below reflect the corrected findings.

The audit's own queries are visible in the 7-day insights (the `GROUP BY action` on
`sensitive_audit_log` and the `sum(length(...))` size probes). Discount those rows when reading
section 4.

---

## 1. Headline numbers

| Metric | Value |
|---|---|
| Tables in production | 260 application tables (+ Cloudflare's `_cf_KV`) |
| Indexes / triggers in production | 280 / 43 |
| Migrations on disk / applied in production | 183 / 183 (`0001` … `0175`, all applied) |
| Tables created only at runtime (no migration) | 20 (+ 8 marketplace-plugin tables + 2 Cloudflare) |
| Database size (D1 reported) | 32.1 MB |
| Total rows across all tables | 68,492 |
| Tables with zero rows | 119 of 260 |
| Reads / writes per 24h (D1 reported) | 200,455 queries / 19,891 queries |
| Rows read / written per 24h | 1,923,275 / 42,798 |
| Read replication | disabled |

The database is small. Almost half the tables are empty, and six tables hold two thirds of the
bytes. The traffic story is dominated by a handful of hot polled reads, not by data volume.

---

## 2. Encryption: what is encrypted and what is not

### 2.1 The mechanism

All application encryption is keyed off one Worker secret, `DATA_KEY` (set in
`app/.env.production`, deployed as a Worker secret). From it the Worker derives, and caches per
isolate (`app/src/entry.py` around line 11760):

| Purpose | Derivation | Algorithm |
|---|---|---|
| Row encryption key | `SHA-256(DATA_KEY)` imported as an AES key | AES-256-GCM, fresh 12-byte random IV per row, stored as base64(`iv || ciphertext`) in a column named `data` |
| Blind index key | `SHA-256(DATA_KEY + ":blind-index")` | HMAC-SHA256 over the lower-cased, trimmed value; hex output in columns named `*_bi` |
| Room-chat passphrases | `SHA-256(DATA_KEY + ":room-chat-passphrase-…")` | Per-repository symmetric chat key handed to authorised clients; separate public derivation for World `#general` |
| Password storage | PBKDF2-SHA256, 100,000 iterations, 16-byte random salt | Salt and hash live *inside* the encrypted `data` blob of `users`; nothing password-related is a column |
| Marketplace plugins | `runtime.seal()` / `runtime.blind()` | Same primitives exposed to plugin workers (CRM and Marketing use them) |

Worker code refuses to run with an empty or placeholder key (`_INSECURE_DATA_KEYS`). This is
encryption at rest against database exposure only. The Worker holds the key, so anyone with Worker
secrets (or a Worker code-execution path) can read everything. It is explicitly *not*
zero-knowledge, except for chat message bodies (see 2.4).

### 2.2 Per-table storage model

Every one of the 260 tables falls into one of three shapes.

| Shape | Tables | Meaning |
|---|---:|---|
| **Encrypted blob + blind indexes** | 111 | Human-readable content is in the AES-GCM `data` column. Other columns are `*_bi` HMACs plus bounded operational fields (timestamps, flags, counters). |
| **Blind indexes + plaintext columns** | 92 | No `data` blob. Identity is hidden behind `*_bi` HMACs but every other column is plaintext. |
| **Plaintext only** | 57 | Operational tables: counters, rate limits, status samples, metrics, migration bookkeeping. |

A live spot check of the 48 non-empty `data` tables confirmed the blob is real ciphertext
(first byte is never `{` or `[`) for 42 of them. Six tables store **plaintext JSON** in a column
that happens to be named `data`, which is documented and intentional (operational config, not
user content):

`cron_tick`, `repo_alert_settings`, `system_status_minute_batch`, `system_status_hourly_batch`,
`system_status_daily_batch`, `world_satellite_snapshot`.

Anyone grepping for `data` columns to reason about encryption will over-count by these six. Renaming
them (for example to `payload_json`) would remove the ambiguity.

### 2.3 What is encrypted (representative)

| Table | Encrypted in `data` | Plaintext beside it | Notes |
|---|---|---|---|
| `users` | email, password salt+hash, display profile, wallet address, settings, contribution snapshot | `username`, `is_admin`, `enable_outreach`, `signup_origin`; `email_bi`, `ip_bi`, `discord_bi` | Username is public by design. 8.4 KB/row average: the blob has grown fat. |
| `quarantined_users` | same as `users` | `username`, `reason_codes`, `status`, review timestamps | Mirror of `users` for held sign-ups. |
| `repositories` | name, owner, description, mirrors, refs metadata | `is_private` | The only plaintext bit lets anonymous listing skip decrypt. |
| `nodes` | node record, endpoints | `pubkey`, `name`, `last_seen` | Node public key and label are public identity. |
| `notifications` | notification payload | `ts`, `read_at`; `recipient_bi`, `dedupe_bi` | |
| `notes`, `note_versions` | note body and title | `note_id` (opaque id) | |
| `org_bot_tokens` | token metadata | `token_id`, provider, lifecycle timestamps; `secret_bi` | The secret itself is never stored; only its HMAC. |
| `account_ssh_keys` | the public key material | `key_type`, `fingerprint`; `key_bi` | |
| `apns_device_tokens` | the APNs token | environment, timestamps; `token_bi` | |
| `ap_actors`, `ap_service_keys` | RSA-2048 private key (PKCS8) | `pubkey_pem` | Public key is published in the actor document anyway. |
| `security_reports`, `repo_security_scans` | report / findings | bounded state only | |
| `organization_tasks`, `organization_task_responses` | task copy, assignee labels, notes | state, timers, `agent_session_id` | |
| `crm_contacts`, `marketing_content` (plugins) | contact / content envelopes | `org_name`, `*_by_name` labels | Plugin-owned; use the shared seal. |

### 2.4 What is deliberately not encrypted

These plaintext columns are documented in `app/src/schema.py` with a rationale. They are listed so
the reader can decide whether the rationale still holds.

| Table | Plaintext column(s) | Stated rationale |
|---|---|---|
| `users`, `outreach_team`, `referral_stats`, `org_team_members`, `profile_follows`, `badge_awards`, `contributor_activity` | public account names | Already public on profiles / leaderboards; keeps rosters listable without a decrypt-per-row pass. |
| `sensitive_audit_log` | `actor_label`, `action`, `target_type`, `target_token`, `details` JSON | Details are an allowlisted JSON object; the schema comment promises no credentials, bodies, addresses, private repo names or raw IPs. `actor_bi` is a blind index. |
| `chat_history.body` | the message body | It is the client-sealed envelope relayed as-is. This is the one table where the relay genuinely cannot read the content (second encryption layer, see the chat-room-seal design). |
| `release_downloads.ua` | raw User-Agent string | Shown to admins to spot bot traffic. No account or IP recorded. This is the only raw browser identifier stored anywhere. |
| `telemetry.summary` | desktop stall / crash summaries (`kind`, `node`, `version`, `os`) | Operational diagnostics from desktop nodes. Averages 6 KB per row. |
| `install_diag.detail`, `error_log.message` | installer step output; server error text | Operational diagnostics; `install_diag.run` is a random per-run id, not tied to an account. |
| `feedback.ip_hash`, `feedback.user_agent`, `feedback.message` | keyed hash of IP; broad client category; free text | `ip_hash` is a blind index not a raw IP; `user_agent` is a legacy name holding only a category. The free-text message is plaintext. |
| `account_sessions.token_digest` | SHA digest of the bearer token | Hash only; the token is not recoverable. The schema comment says a `client_ip` is kept for the owner to recognise stolen devices, but the live table has no such column and no `data` blob, so the sign-in IP is not persisted here and that comment is stale. |
| `contributor_reward_estimates`, `tip_notice`, `reward_contributions`, `ledger_backfill_*`, `pending_rewards`, `wallet_balances`, `world_element_purchases` | Solana addresses and transaction signatures, recipient names, subject titles | Public on-chain data and admin payment records. |
| `ap_remote_actors`, `ap_followers`, `mirror_https_endpoints`, `relays`, `federated_presence` | fediverse handles, URLs, avatar URLs, bios, mirror base URLs, public keys | Public federation / routing identity. |
| `operating_expenses` | `description`, `paid_by_name`, `payment_method_summary` | Ledger transparency records. |

### 2.5 Encryption findings

1. **`world_element_purchases.deposit_secret` is a plaintext column that is meant to hold a
   secret.** The table is empty and the comment says the sweep clears it, but a column whose name
   says "secret" with no encryption is a foot-gun and contradicts the "ForkMesh never stores a
   key" comment two lines above it. Recommend dropping the `deposit_*` / `sweep_*` columns or
   moving them into a sealed blob before the feature ships.
2. **`users.data` averages 8.4 KB per row (2.0 MB for 241 users).** Whatever is being folded into
   the account blob (contribution snapshots, settings, wallet history) is decrypted on the hottest
   read in the system (`SELECT data FROM users WHERE user_bi=?`, 72,749 runs in 7 days). Splitting
   rarely-read sections into their own table would cut both AES work and rows-read bytes.
3. **Six `data` columns are plaintext JSON.** Documented, but the naming makes the "every `data`
   column is ciphertext" invariant false. Rename or add a `CHECK (substr(data,1,1) <> '{')`
   comment marker so future audits do not have to spot-check.
4. **`release_downloads.ua` and `feedback.message` are the only free-form, user-supplied plaintext
   fields outside the diagnostics tables.** Both are low risk but worth a conscious decision to
   keep.
5. **Blind indexes are deterministic HMACs.** Equality lookups are the design goal, but it also
   means two accounts sharing an email or IP produce identical `email_bi` / `ip_bi`, which is
   observable to anyone with DB read access. This is inherent to the pattern and acceptable; it
   is noted so nobody assumes the `_bi` columns are anonymous.
6. **Key rotation has no migration path.** Everything derives from one `DATA_KEY`. Rotating it
   requires re-encrypting all 111 blob tables and recomputing every `*_bi` column; there is no
   key-version column anywhere. Add a `key_id` byte to the blob prefix now while the data is 30 MB.

---

## 3. Tables that are no longer needed, dormant, or unaccounted for

### 3.1 Safe to drop (no code references, no live use)

| Table | Rows | Why |
|---|---:|---|
| `commit_inbox` | 0 | Created by migration `0009`; zero references in `app/src`, `world/src`, tests or plugins. Dead. |
| `forkmesh_d1_migrations` | 1 | Legacy hand-rolled migration tracker from before `wrangler d1 migrations`. No code references. Cloudflare's `d1_migrations` (183 rows) is the live tracker. |
| `system_status_minute`, `system_status_hourly`, `system_status_daily` | 0 / 0 / 0 | Superseded by the `*_batch` tables in the status rewrite; the old rows have finished draining (1,691 + 216 + 129 rows in July, 0 now). `system_status_minute` is still *read* 26,475 times a week by the dual-read in `_status_minute_records` (`status_monitoring.py:297`, also `:437` and the prune at `:1997`), so remove the read first. `system_status_minute` has no migration at all. |
| `telemetry` | 313 | **Orphaned.** The opt-in crash/stall upload pipeline was removed in `b417261a1` (2026-07-25). No DDL, route, or SQL remains anywhere in the repo; the last row is dated 2026-07-25 18:23 UTC. 1.8 MB of plaintext desktop diagnostics with no reader. Export, then drop. |
| `host_presence` | 0 | **Writer retired, readers remain.** Repository sockets and their heartbeat were retired for signed HTTPS mirrors (`catalog_routes.py:275`), and no `INSERT INTO host_presence` exists, but `entry.py` still selects, counts, renames and deletes it (lines 3209, 3235, 3438, 11009, 11098, 15884, 16120, 27341, 32355, 40045–40050): 9,920 queries and 126k rows read a week against an always-empty table. Remove the readers, then drop. |

### 3.2 Legacy custody tables (keep until the custody record is formally closed, then drop)

`LEGACY_WALLET_CUSTODY_TABLES` in `entry.py` still names these, and the July scrub run kept them
for reconciliation.

| Table | Rows | Status |
|---|---:|---|
| `bounty_wallet` | 0 | Runtime-only (no migration); referenced only by schema/ledger/admin read paths. |
| `central_fund` | 0 | Same. |
| `issue_bounty` | 0 | Runtime-only; no reads or writes. |
| `federated_signup` | 0 | Custody-era sign-up path. |
| `legacy_custody_migration_audit` | 1 | Schema-only reference. Retained as the audit record of the 2026-07-23 scrub. |
| `legacy_custody_reconciliation` | 2 | Same. |
| `funds_received` | 2 | Unchanged since July. Historical deposit records for display only. |

Recommendation: export the five non-empty rows into the deploy-backup directory alongside the
existing custody audit JSON, then drop all seven tables in one migration. Also drop the
`LEGACY_WALLET_CUSTODY_TABLES` constant.

### 3.3 Dormant features (zero rows, but live code paths)

These are not "unneeded"; they are shipped features nobody has used in production yet. They cost
nothing at 0 rows but add to the 260-table sprawl and to `ensure_schema` startup work.

- **ActivityPub outbound**: `ap_outbox`, `ap_comments`, `ap_blocked_domains`, `ap_settings`,
  `ap_repo_settings`, `ap_org_digest_settings`.
- **Attribution pipeline** (migration `0174`, one week old): `attribution_sessions`,
  `attribution_events`, `attribution_filtered_hits`, `attribution_ip_hourly`,
  `attribution_clone_snapshots`, `product_clone_daily`.
- **Community ads**: `community_ad_proposals`, `community_ad_votes`, `community_ad_moderation`,
  `community_ad_revenue`, `community_ad_instance_policy`.
- **Contributor invitations**: `contributor_invitations` plus four `contributor_invitation_*`
  side tables and nine triggers.
- **Org succession**: `org_succession_cases`, `_approvals`, `_events`, `_policies` (ten triggers).
- **Patreon and Polar integrations**: `patreon_*` (4 tables), `polar_*` (3 tables).
- **Marketplace governance**: `marketplace_submissions`, `marketplace_member_approvals`,
  `package_ratings`.
- **Repository security scans**: `repo_security_scans`, `_leases`, `_reviews`.
- **Reward pool**: `reward_rounds`, `reward_node_observations`, `reward_contribution_intents`,
  `reward_wallet_reminders`, `pending_rewards`, `chain_intents`, `microjob_claims`.
- **World media / workshops / events / fediverse mentions**: 16 `world_*` tables including
  `world_media_*` (5), `world_workshop_*` (4), `world_office_marketing_*` (4), `world_events`,
  `world_lobby_links`, `world_fediverse_*` (3).
- **Misc**: `repo_shares`, `repo_media`, `repo_forks`, `repository_fork_jobs`,
  `repo_device_mirrors`, `repo_privacy_policy`, `repo_terms_flags`, `repository_logo_suggestions`,
  `private_mirror_routes`, `role_grants`, `note_links`, `note_shares`, `calendar_attendees`,
  `bug_reports`, `security_reports`, `pending_verifications`, `password_reset_rate`,
  `quarantined_nodes`, `agent_prompts`, `repo_agents`, `repo_deletions`, `outreach_team`,
  and the four `*_inbox` tables (`issue`, `pull`, `discussion`, `about`), which drain to zero by
  design.
- **Federated relay identity**: `relay_self` (0 rows). `_relay_identity` (`entry.py:28691`)
  lazily generates and seals this relay's Ed25519 signing key on first federated call; zero
  rows means that path has not run in production, not that the table is dead. Keep.

### 3.4 Tables in production with no migration (schema drift)

Twenty tables exist because `ensure_schema()` in `schema.py` issues `CREATE TABLE IF NOT EXISTS`
at runtime, not because a migration created them. A fresh D1 built from `migrations/` alone would
lack them:

`agent_prompts`, `ai_ask_rate`, `bounty_wallet`, `catalog_rev`, `clone_sticky`,
`forkmesh_d1_migrations`, `host_presence`, `issue_bounty`, `issue_seq`, `login_attempts`,
`online_hourly_nodes`, `outreach_log`, `outreach_team`, `relay_self`, `repo_agents`,
`repo_deletions`, `repository_monitor_state`, `signup_rate`, `system_status_minute`, `telemetry`.

Several are load-bearing (`login_attempts`, `signup_rate`, `online_hourly_nodes`,
`repository_monitor_state`, `catalog_rev`, `issue_seq`). `telemetry` and `forkmesh_d1_migrations`
are not even in `schema.py` any more; they survive only as production leftovers. Recommend one catch-up
migration that mirrors their current `schema.py` definitions so `migrate.sh` is the single source
of truth.

Related: **migration `0089` fails on a clean replay** (`duplicate column name: completed_at`)
because `0075_world_office_marketing_tasks.sql` was later edited to include the column that
`0089` adds. Production applied `0089` before that edit, so it is fine there, but anyone building a
dev database from scratch will hit it. Make `0089` a no-op or revert the edit to `0075`.

### 3.5 Plugin-owned tables

Eight tables belong to installed marketplace plugins and are created/dropped by the plugin
lifecycle (`app/src/plugin_data.py`), not by app migrations:

- CRM plugin: `crm_contacts` (52 rows), `crm_email_sends`, `crm_mail_messages`, `crm_settings`.
- Marketing plugin: `marketing_content` (83), `marketing_feed_items` (83), `marketing_metrics`,
  `marketing_settings` (1).

They correctly use the shared seal and blind-index runtime. They will disappear when the last
organisation uninstalls the plugin. Not a drop candidate.

---

## 4. Traffic and data volume

### 4.1 Where the bytes are

Payload estimate is `sum(length(column))` over live rows; D1's 32.1 MB total includes 280 indexes
and page overhead on top of the 16.2 MB of payload measured across the 60 largest tables.

| Table | Rows | Payload | Avg row | Retention in code |
|---|---:|---:|---:|---|
| `sensitive_audit_log` | 23,625 | 5.3 MB | 229 B | 365 days (`SECURITY_AUDIT_RETAIN_MS`) |
| `users` | 241 | 2.0 MB | 8.4 KB | n/a |
| `telemetry` | 313 | 1.8 MB | 6.0 KB | **orphaned** (writer removed 2026-07-25) |
| `org_bot_token_usage` | 10,182 | 1.3 MB | 127 B | 365 days |
| `notifications` | 1,250 | 1.2 MB | 991 B | 90 days and 500 per recipient |
| `system_status_minute_batch` | 1,844 | 1.15 MB | 640 B | 30 hours (`STATUS_MINUTE_RETAIN_MS`) |
| `system_status_hourly_batch` | 734 | 0.6 MB | 871 B | `STATUS_HISTORY_DAYS` |
| `api_metrics_minute` | 15,265 | 0.5 MB | 32 B | 7 days |
| `install_diag` | 2,631 | 0.25 MB | 96 B | last 5,000 rows |
| `organization_tasks` | 133 | 0.24 MB | 1.8 KB | n/a |
| `quarantined_users` | 170 | 0.24 MB | 1.4 KB | n/a |
| `release_downloads` | 1,568 | 0.22 MB | 145 B | **none** |
| `repositories` | 54 | 0.14 MB | 2.7 KB | n/a |

### 4.2 Row growth, 2026-07-23 to 2026-09-10 (49 days, 128 tables present in both)

| Table | Jul 23 | Sep 10 | Per day | Comment |
|---|---:|---:|---:|---|
| `sensitive_audit_log` | 0 | 23,625 | 482 | Front-loaded: only 214 rows in the last 7 days. 9,907 rows are `organization.task_agent_status`, 5,694 are `ssh.git_authorize`, 2,470 `repository.actions_status`. Agent status pings and SSH authorisations are being audited per event. |
| `install_diag` | 987 | 2,631 | 34 | Capped at 5,000. |
| `notifications` | 183 | 1,250 | 22 | Bounded per recipient. |
| `repo_state_history` | 35 | 768 | 15 | Trimmed to 4 rows per `key_bi`, so growth tracks the number of distinct keys (repo × mirror), not time. Worth confirming the trim runs on every write path (three call sites). |
| `profile_contribution_days` | 22 | 405 | 8 | Fine. |
| `signup_rate` | 5 | 291 | 6 | Rate-limit rows; no prune found. |
| `feedback_email_sends` | 7 | 258 | 5 | No prune found. |
| `users` | 9 | 241 | 4.7 | Real growth. |
| `login_attempts` | 8 | 92 | 1.7 | Rows deleted only on successful login; stale lockout rows accumulate. |
| `telemetry` | 291 | 313 | 0.4 | Frozen since 2026-07-25 when the upload pipeline was removed. |
| `system_status_hourly` | 1,691 | 0 | | Drained (superseded). |
| `ap_objects` | 73 | 11 | | Shrank; was the second-largest table (720 KB) in July. |

132 of today's 260 tables did not exist in the July snapshot: half the schema is under seven weeks
old.

### 4.3 Query traffic, last 7 days (D1 insights, top-60 lists merged, so per-table totals are lower bounds)

| Table | Query runs | Rows read | Rows written | What is driving it |
|---|---:|---:|---:|---|
| `nodes` | 159,719 | 957,415 | 20,433 | `SELECT data FROM nodes WHERE node_bi=?` 54k; `SELECT pubkey …` 37k; heartbeat `UPDATE nodes SET last_seen` 19k writes |
| `repositories` | 115,831 | **3,214,258** | 6,844 | Public catalog full scan `… WHERE is_private = 0` ran 37,029 times reading 54 rows each (2.0 M rows). `SELECT COUNT(*) FROM repositories` ran 5,732 times as a full scan (310k rows, efficiency 0.02). |
| `mirror_https_endpoints` | 101,363 | 469,844 | 31,445 | Health probe `UPDATE … SET checked_at,latency_ms,healthy…` 15,590 runs, 2 rows written each |
| `users` | 94,052 | 1,269,073 | 928 | `SELECT data FROM users WHERE user_bi=?` 72,749 runs (the single most-run statement) |
| `schema_meta` | 78,409 | 115,688 | 190 | Two `ensure_schema` fingerprint checks (41k + 37k runs). Pure overhead per cold isolate / request. |
| `repository_monitor_state` | 76,586 | 1,022,149 | 2,917 | The `INSERT … ON CONFLICT` with a `json_each` subquery reads 372k rows for 44 rows written (efficiency 0.00); the roster `SELECT` reads the whole 49-row table 12,200 times |
| `account_devices` | 55,728 | 448,104 | 0 | Device roster per authenticated request |
| `org_repos` | 51,955 | 53,062 | 0 | `SELECT node_owner FROM org_repos WHERE org_bi=? AND repo=?` on repo routing |
| `system_status_minute_batch` | 37,142 | 298,820 | 27,961 | Status page reads 16.8k; per-minute upsert 10.2k |
| `system_status_minute` | 26,475 | 26,475 | 0 | Legacy read of an empty table (see 3.1) |
| `api_metrics_minute` | 18,852 | 42,702 | **72,026** | Highest write volume. The hourly retention `DELETE` alone wrote 38.5k rows over 201 runs because it runs once per isolate-hour, not once per hour. |
| `public_api_snapshots` | 19,850 | 17,018 | 14,622 | Snapshot regenerated 14.6k times a week for a 5-row table |
| `cron_tick` | 10,632 | 20,782 | 30,738 | Every-minute cron heartbeat upsert (2 rows written per tick) + 30-minute prune |
| `online_hourly_nodes` | 5,685 | 850,825 | 6,037 | Leaderboard reads scan 167 rows × 48 hours per call (efficiency 0.33) |
| `notifications` | 5,144 | 347,826 | 1,429 | The per-recipient dedupe cap `DELETE … NOT IN (SELECT … LIMIT 500)` reads ~1k rows per run (206k rows / 202 runs) |
| `chat_history` | ~700 | 191,445 | | Replay window query uses `ROW_NUMBER()` over the whole room |

### 4.4 Traffic findings

1. **The public repository catalog is re-read from D1 37k times a week, 2 M rows.** The in-isolate
   memo (`PUBLIC_CATALOG_MEMO_TTL_MS = 30 s`) only helps within one warm isolate. A single row in
   `public_api_snapshots` or a KV entry keyed by `catalog_rev` (which already exists and is bumped
   on every catalog write) would remove roughly 15 % of all rows read in the database (2.0 M of about 13.5 M rows read per week).
2. **`SELECT COUNT(*) FROM repositories` is a full table scan run 5,732 times.** Read the count
   from the same cached catalog, or maintain it in `catalog_rev`.
3. **`schema_meta` fingerprint checks are 78k queries a week (roughly 6 % of all queries).** The
   answer is *already* memoized per isolate (`_schema_ready`, `entry.py:11508`), so the count is a
   measure of isolate churn and a further in-isolate cache buys nothing. The real options are to
   trust the deploy gate (`migrate.sh` writes the fingerprint before code goes live) and skip the
   runtime SELECT, or to fold it into the isolate's first real query.
4. **`api_metrics_minute` retention runs per isolate, not per hour.** Guard the prune with a
   `schema_meta` or `usage_budget_state` row so only one isolate does it, or fold it into the cron
   runner. Same pattern applies to the `system_status_*` prunes.
5. **`repository_monitor_state` upsert reads 8,500 rows per row written.** The `json_each`
   subquery joins against the whole table; restructure as one `INSERT … ON CONFLICT` per monitor
   or index the join key.
6. **`sensitive_audit_log` is audited per agent status ping.** 9.9k rows of
   `organization.task_agent_status` at 229 bytes each is 2.3 MB of audit log for one bot loop.
   The write is `_apply_agent_status` in `world/src/world_office_tasks.py:1829`, which audits every
   status update even when the status did not change. Audit only transitions. At the
   365-day retention this table is on track to be the largest by far; consider 90 days for
   non-security actions.
7. **`telemetry` is an orphan, not an unbounded table.** It is the third-largest table on 313
   rows, but nothing has written it since the pipeline was removed on 2026-07-25. Export and drop
   it (see 3.1); no cap is needed.
8. **`signup_rate`, `feedback_email_sends`, `login_attempts` accumulate without a prune.** All
   three are bounded-window rate tables whose old rows are never deleted.
9. **`public_api_snapshots` is rewritten ~2,000 times a day.** Check whether regeneration is
   triggered by reads rather than by catalog changes.
10. **`host_presence` is read 10k times a week and is always empty.** Its writer is gone; see 3.1.

---

## 5. Recommended actions, in order

1. One migration: drop `commit_inbox`, `forkmesh_d1_migrations`, `telemetry` (after export),
   `host_presence`, and the three legacy `system_status_{minute,hourly,daily}` tables — after
   removing the `system_status_minute` dual-read and the `host_presence` readers. Do **not** drop
   `relay_self`.
2. One migration: back-fill `CREATE TABLE IF NOT EXISTS` for the 20 runtime-only tables so the
   migration set builds a complete database; fix the `0089` replay failure.
3. Fix or remove the `deposit_secret` plaintext column in `world_element_purchases` before the
   store ships.
4. Cache the public catalog and its count outside D1 (KV keyed by `catalog_rev`, or one
   `public_api_snapshots` row). This is the single biggest read reduction available.
5. Move the `api_metrics_minute` / status prunes to the cron runner; stop auditing
   `organization.task_agent_status` per ping; prune the three rate tables.
6. Export and drop the seven legacy custody tables once the custody record is signed off.
7. Add a key-version byte to the AES blob so `DATA_KEY` rotation becomes possible.

---

## 6. Reproduction

```bash
# Node 22 is required by wrangler 4.x; the system node is 20.
export PATH=$HOME/.nvm/versions/node/v22.23.1/bin:$PATH
cd app && set -a && source .env.production && set +a

npx wrangler@4.120.0 d1 info forkmesh
npx wrangler@4.120.0 d1 insights forkmesh --time-period 7d --sort-by reads --limit 60 --json
npx wrangler@4.120.0 d1 execute forkmesh --remote --json \
  --command "SELECT name, type FROM sqlite_master WHERE type IN ('table','index','trigger')"

# Clean replay of the migrations (fails at 0089, see 3.4)
rm -f /tmp/schema.sqlite; for f in $(ls migrations/*.sql | sort -V); do sqlite3 /tmp/schema.sqlite < "$f"; done
```

Row counts were gathered with one `SELECT '<t>', count(*) FROM "<t>"` per table (D1 rejects
compound SELECTs beyond a few dozen terms and refuses to read `_cf_KV`). Ciphertext spot checks
used `substr(data,1,1) IN ('{','[')`.

---

## Appendix A. Every table

Columns: live rows today; rows in the 2026-07-23 snapshot (blank if the table did not exist);
estimated payload KB (top 60 tables only); storage model; plaintext columns whose names suggest
content or identity (heuristic, names only); 7-day query runs / rows read / rows written from D1
insights (blank if the table did not appear in any top-60 list); number of references in
`app/src`, `world/src` and plugin worker code; where the table's DDL lives.

| Table | Rows (live) | Rows (Jul 23) | Est. KB | Storage model | Plaintext columns worth knowing | 7d query runs | 7d rows read | 7d rows written | Code refs | Origin |
|---|---:|---:|---:|---|---|---:|---:|---:|---:|---|
| `about_inbox` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 9 | migration |
| `account_devices` | 18 | 1 |  | plaintext columns + blind indexes | pubkey, label | 55728 | 448104 | 0 | 63 | migration |
| `account_presence` | 1 | 3 |  | plaintext columns + blind indexes |  | 11257 | 128634 | 3308 | 23 | migration |
| `account_sessions` | 3 |  |  | plaintext columns + blind indexes | session_id, token_digest, device_label | 109 | 83 | 213 | 39 | migration |
| `account_signup_intents` | 1 |  |  | plaintext columns + blind indexes | email_field_name |  |  |  | 6 | migration |
| `account_ssh_keys` | 5 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 14 | migration |
| `admin_signup_referral_code` | 1 |  |  | encrypted `data` + blind indexes |  |  |  |  | 11 | migration |
| `agent_guestbook_rate` | 1 |  |  | plaintext columns + blind indexes |  |  |  |  | 8 | migration |
| `agent_prompts` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 21 | runtime only (no migration) |
| `ai_ask_rate` | 2 |  |  | plaintext columns + blind indexes |  |  |  |  | 12 | runtime only (no migration) |
| `ap_actors` | 18 | 3 |  | encrypted `data` + blind indexes | pubkey_pem |  |  |  | 8 | migration |
| `ap_blocked_domains` | 0 | 0 |  | plaintext only |  |  |  |  | 5 | migration |
| `ap_comments` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 13 | migration |
| `ap_digest_queues` | 5 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 12 | migration |
| `ap_followers` | 4 | 4 |  | plaintext columns + blind indexes | follower_handle | 7050 | 28200 | 0 | 24 | migration |
| `ap_mentions` | 5 | 5 |  | plaintext columns + blind indexes |  |  |  |  | 2 | migration |
| `ap_objects` | 11 | 73 |  | encrypted `data` + blind indexes |  |  |  |  | 18 | migration |
| `ap_org_digest_settings` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 5 | migration |
| `ap_outbox` | 0 | 2 |  | encrypted `data` + blind indexes |  |  |  |  | 21 | migration |
| `ap_remote_actors` | 6 | 6 |  | plaintext only | pubkey_pem, handle, display_name, url, avatar_url, summary | 7050 | 28200 | 0 | 11 | migration |
| `ap_repo_settings` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 4 | migration |
| `ap_service_keys` | 1 |  |  | encrypted `data` + blind indexes | key_name, pubkey_pem |  |  |  | 4 | migration |
| `ap_settings` | 0 | 0 |  | plaintext only |  |  |  |  | 17 | migration |
| `api_metrics_minute` | 15252 |  | 480 | plaintext only |  | 18852 | 42702 | 72026 | 6 | migration |
| `apns_device_tokens` | 1 |  |  | encrypted `data` + blind indexes |  |  |  |  | 9 | migration |
| `attribution_clone_snapshots` | 0 |  |  | plaintext only |  |  |  |  | 6 | migration |
| `attribution_events` | 0 |  |  | plaintext only | session_id, event_name |  |  |  | 12 | migration |
| `attribution_filtered_hits` | 0 |  |  | plaintext only | reason |  |  |  | 4 | migration |
| `attribution_ip_hourly` | 0 |  |  | plaintext only | ip_hash, sessions, session_ids |  |  |  | 5 | migration |
| `attribution_sessions` | 0 |  |  | plaintext only | first_utm_content, last_utm_content, first_referrer_host, bot_reason |  |  |  | 22 | migration |
| `badge_awards` | 96 |  | 10 | plaintext columns + blind indexes | name | 210 | 266376 | 0 | 9 | migration |
| `blog_post_links` | 0 |  |  | plaintext columns + blind indexes | url |  |  |  | 9 | migration |
| `blog_post_metrics` | 23 |  |  | plaintext only |  |  |  |  | 7 | migration |
| `blog_post_referrers` | 9 |  |  | plaintext only |  |  |  |  | 5 | migration |
| `blog_post_unique_hll` | 91 |  | 2 | plaintext only |  |  |  |  | 5 | migration |
| `blog_post_view_seen` | 68 |  | 8 | plaintext only | referrer_host |  |  |  | 9 | migration |
| `bounty_wallet` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 4 | runtime only (no migration) |
| `bug_reports` | 0 |  |  | encrypted `data` + blind indexes |  |  |  |  | 8 | migration |
| `calendar_attendees` | 0 |  |  | plaintext columns + blind indexes | attendee_name | 9357 | 0 | 0 | 11 | migration |
| `calendar_events` | 1 |  |  | encrypted `data` + blind indexes | owner_name, org_name | 10166 | 20332 | 0 | 12 | migration |
| `calendar_reminder_deliveries` | 2 |  |  | plaintext columns + blind indexes |  |  |  |  | 5 | migration |
| `catalog_rate` | 30 | 13 | 2 | plaintext columns + blind indexes |  | 3146 | 3146 | 3146 | 5 | migration |
| `catalog_rev` | 1 |  |  | plaintext only |  | 828 | 828 | 828 | 3 | runtime only (no migration) |
| `central_fund` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 5 | migration |
| `chain_intents` | 0 | 0 |  | encrypted `data` + blind indexes | source_address, tx_signature |  |  |  | 35 | migration |
| `chat_channel_members` | 4 |  |  | encrypted `data` + blind indexes |  |  |  |  | 25 | migration |
| `chat_channel_webhooks` | 0 |  |  | encrypted `data` + blind indexes | webhook_id |  |  |  | 12 | migration |
| `chat_channels` | 2 |  |  | encrypted `data` + blind indexes |  | 3760 | 3760 | 0 | 20 | migration |
| `chat_direct_conversations` | 11 |  |  | encrypted `data` + blind indexes | message_count |  |  |  | 14 | migration |
| `chat_direct_participants` | 22 |  |  | encrypted `data` + blind indexes |  |  |  |  | 19 | migration |
| `chat_history` | 83 | 5 | 31 | plaintext only | body | 901 | 191838 | 194 | 18 | migration |
| `clone_rr` | 9 | 12 |  | plaintext columns + blind indexes |  |  |  |  | 6 | migration |
| `clone_sticky` | 18 | 11 |  | plaintext columns + blind indexes |  | 889 | 889 | 889 | 4 | runtime only (no migration) |
| `commit_inbox` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 0 | migration |
| `community_ad_instance_policy` | 0 | 0 |  | plaintext columns + blind indexes | contexts |  |  |  | 3 | migration |
| `community_ad_moderation` | 0 | 0 |  | plaintext columns + blind indexes | reason, evidence_url |  |  |  | 6 | migration |
| `community_ad_proposals` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 17 | migration |
| `community_ad_revenue` | 0 | 0 |  | plaintext only |  |  |  |  | 6 | migration |
| `community_ad_votes` | 0 | 0 |  | plaintext columns + blind indexes |  |  |  |  | 4 | migration |
| `contributor_activity` | 74 | 22 | 7 | plaintext columns + blind indexes | name | 2351 | 640181 | 106 | 9 | migration |
| `contributor_invitation_abuse` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 3 | migration |
| `contributor_invitation_optouts` | 0 | 0 |  | plaintext columns + blind indexes |  |  |  |  | 4 | migration |
| `contributor_invitation_provenance` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 10 | migration |
| `contributor_invitation_rate` | 0 | 0 |  | plaintext columns + blind indexes |  |  |  |  | 10 | migration |
| `contributor_invitations` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 23 | migration |
| `contributor_reward_estimates` | 57 |  | 33 | plaintext columns + blind indexes | recipient_name, repo_name, subject_number, subject_title, subject_summary, source_address, destination_address, tx_signature |  |  |  | 22 | migration |
| `crm_contacts` | 52 |  | 46 | encrypted `data` + blind indexes | org_name, created_by_name, updated_by_name | 57 | 1416 | 228 | 0 | plugin (marketplace) |
| `crm_email_sends` | 0 |  |  | encrypted `data` + blind indexes | org_name, sender_name |  |  |  | 0 | plugin (marketplace) |
| `crm_mail_messages` | 0 |  |  | encrypted `data` + blind indexes | message_id, org_name, provider_message_id |  |  |  | 0 | plugin (marketplace) |
| `crm_settings` | 0 |  |  | encrypted `data` + blind indexes | updated_by_name |  |  |  | 0 | plugin (marketplace) |
| `cron_tick` | 371 |  | 130 | `data` is **plaintext JSON** (operational) |  | 10632 | 20782 | 30738 | 6 | migration |
| `d1_migrations` | 183 | 69 |  | plaintext only |  | 30 | 30 | 90 | 0 | Cloudflare |
| `discord_bug_reports` | 12 |  |  | encrypted `data` + blind indexes |  |  |  |  | 6 | migration |
| `discord_bug_watch` | 1 |  |  | plaintext columns + blind indexes | last_message_id |  |  |  | 3 | migration |
| `discussion_inbox` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 32 | migration |
| `durable_object_abort_minute` | 116 |  | 3 | plaintext only |  | 13614 | 42379 | 88 | 9 | migration |
| `durable_object_traffic` | 0 |  |  | plaintext only | messages |  |  |  | 6 | migration |
| `edge_route_cursor` | 116 | 0 | 9 | plaintext columns + blind indexes |  |  |  |  | 3 | migration |
| `error_log` | 61 | 9 | 26 | plaintext only | message | 13244 | 41986 | 189 | 59 | migration |
| `federated_presence` | 0 | 0 |  | plaintext columns + blind indexes | wallet, name, device_id, public_key, base_url, health_message |  |  |  | 10 | migration |
| `federated_signup` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 6 | migration |
| `feedback` | 11 | 0 |  | plaintext only | message, ip_hash, user_agent |  |  |  | 44 | migration |
| `feedback_email_sends` | 258 | 7 | 22 | plaintext columns + blind indexes | name | 208 | 95812 | 0 | 6 | migration |
| `forkmesh_d1_migrations` | 1 | 1 |  | plaintext only | name |  |  |  | 0 | runtime only (no migration) |
| `funds_received` | 2 | 2 |  | plaintext only | name |  |  |  | 8 | migration |
| `host_presence` | 0 | 2 |  | plaintext columns + blind indexes |  | 9920 | 126064 | 0 | 23 | runtime only (no migration) |
| `inbox_drain_log` | 78 | 3 | 7 | plaintext columns + blind indexes |  |  |  |  | 5 | migration |
| `install_diag` | 2631 | 987 | 246 | plaintext only | detail |  |  |  | 18 | migration |
| `issue_bounty` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 12 | runtime only (no migration) |
| `issue_inbox` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 44 | migration |
| `issue_seq` | 8 | 1 |  | plaintext columns + blind indexes |  |  |  |  | 4 | runtime only (no migration) |
| `ledger_backfill_sources` | 3 |  |  | plaintext columns + blind indexes | address, subject_kind, subject_number, before_signature |  |  |  | 13 | migration |
| `ledger_backfill_transactions` | 0 |  |  | plaintext only | signature, source_address, destination_address, observed_address |  |  |  | 7 | migration |
| `legacy_custody_migration_audit` | 1 | 1 |  | plaintext only | address_count |  |  |  | 1 | migration |
| `legacy_custody_reconciliation` | 2 | 2 |  | plaintext only | public_address |  |  |  | 2 | migration |
| `link_codes` | 77 | 63 | 11 | encrypted `data` + blind indexes |  |  |  |  | 10 | migration |
| `login_attempts` | 92 | 8 | 7 | plaintext columns + blind indexes |  |  |  |  | 13 | runtime only (no migration) |
| `mailtrap_email_sends` | 708 |  | 119 | plaintext columns + blind indexes | message_id | 10297 | 76566 | 84 | 14 | migration |
| `mailtrap_webhook_events` | 1273 |  | 61 | plaintext only |  |  |  |  | 5 | migration |
| `marketing_content` | 83 |  | 68 | encrypted `data` + blind indexes | content_id, org_name, created_by_name, updated_by_name |  |  |  | 0 | plugin (marketplace) |
| `marketing_feed_items` | 83 |  | 15 | plaintext columns + blind indexes | content_id |  |  |  | 0 | plugin (marketplace) |
| `marketing_metrics` | 0 |  |  | encrypted `data` + blind indexes | org_name, recorded_by_name |  |  |  | 0 | plugin (marketplace) |
| `marketing_settings` | 1 |  |  | encrypted `data` + blind indexes | updated_by_name |  |  |  | 0 | plugin (marketplace) |
| `marketplace_member_approvals` | 0 |  |  | plaintext columns + blind indexes | reviewer_name, wallet_address |  |  |  | 12 | migration |
| `marketplace_submissions` | 0 |  |  | plaintext columns + blind indexes | manifest_url, submitted_by_name, owner_approved_by_name, rejection_reason |  |  |  | 16 | migration |
| `microjob_claims` | 0 |  |  | encrypted `data` + blind indexes | tx_signature |  |  |  | 21 | migration |
| `mirror_https_endpoints` | 4 | 0 |  | plaintext columns + blind indexes | node_name, base_url, public_key, health_message, mirror_url | 101363 | 469844 | 31445 | 44 | migration |
| `mirror_repository_integrity` | 0 |  |  | plaintext columns + blind indexes | mirror_url, reason | 3243 | 32430 | 0 | 7 | migration |
| `mirror_serve_counters` | 68 |  | 3 | plaintext only | node_name | 4493 | 67784 | 770 | 15 | migration |
| `nodes` | 27 | 3 |  | encrypted `data` + blind indexes | pubkey, name | 159719 | 957415 | 20433 | 307 | migration |
| `note_links` | 0 |  |  | plaintext only | note_id |  |  |  | 5 | migration |
| `note_shares` | 0 |  |  | plaintext columns + blind indexes | note_id, principal_name |  |  |  | 9 | migration |
| `note_versions` | 59 |  | 99 | encrypted `data` + blind indexes | note_id |  |  |  | 7 | migration |
| `note_views` | 10 |  |  | plaintext only | note_id |  |  |  | 5 | migration |
| `notes` | 32 |  | 56 | encrypted `data` + blind indexes | note_id | 44 | 22 | 154 | 76 | migration |
| `notifications` | 1250 | 183 | 1209 | encrypted `data` + blind indexes |  | 5144 | 347826 | 1429 | 68 | migration |
| `online_hourly` | 49 | 35 | 1 | plaintext only |  | 3354 | 51487 | 1269 | 4 | migration |
| `online_hourly_nodes` | 167 | 75 | 14 | plaintext only | label | 5685 | 850825 | 6037 | 8 | runtime only (no migration) |
| `operating_expenses` | 15 |  |  | plaintext only | description, paid_by_name, payment_method_summary |  |  |  | 6 | migration |
| `org_agent_jobs` | 7 |  |  | encrypted `data` + blind indexes | session_id |  |  |  | 15 | migration |
| `org_agent_sessions` | 7 |  |  | encrypted `data` + blind indexes | session_id |  |  |  | 11 | migration |
| `org_bot_token_usage` | 10182 |  | 1262 | plaintext columns + blind indexes | token_id | 500 | 400031 | 1107 | 7 | migration |
| `org_bot_tokens` | 9 |  |  | encrypted `data` + blind indexes | token_id |  |  |  | 14 | migration |
| `org_members` | 2 | 1 |  | plaintext columns + blind indexes | name |  |  |  | 45 | migration |
| `org_plugin_installs` | 2 |  |  | plaintext columns + blind indexes | org_name, installed_by_name |  |  |  | 10 | migration |
| `org_repos` | 1 | 0 |  | plaintext columns + blind indexes |  | 51955 | 53062 | 0 | 33 | migration |
| `org_succession_approvals` | 0 | 0 |  | plaintext columns + blind indexes |  |  |  |  | 9 | migration |
| `org_succession_cases` | 0 | 0 |  | plaintext columns + blind indexes | owner_name, successor_name |  |  |  | 19 | migration |
| `org_succession_events` | 0 | 0 |  | plaintext columns + blind indexes |  |  |  |  | 16 | migration |
| `org_succession_policies` | 0 | 0 |  | plaintext columns + blind indexes | owner_name, successor_name |  |  |  | 13 | migration |
| `org_team_collaborators` | 0 |  |  | plaintext columns + blind indexes | name |  |  |  | 10 | migration |
| `org_team_members` | 18 | 0 |  | plaintext columns + blind indexes | name |  |  |  | 16 | migration |
| `org_teams` | 9 | 0 |  | plaintext columns + blind indexes |  |  |  |  | 14 | migration |
| `organization_discord_connectors` | 1 |  |  | encrypted `data` + blind indexes |  |  |  |  | 8 | migration |
| `organization_discord_oauth_grants` | 1 |  |  | encrypted `data` + blind indexes |  |  |  |  | 7 | migration |
| `organization_discord_oauth_states` | 0 |  |  | encrypted `data` + blind indexes |  |  |  |  | 12 | migration |
| `organization_discord_setup_tasks` | 1 |  |  | plaintext columns + blind indexes |  |  |  |  | 13 | migration |
| `organization_task_attachments` | 0 |  |  | encrypted `data` + blind indexes |  |  |  |  | 9 | migration |
| `organization_task_checkins` | 0 |  |  | encrypted `data` + blind indexes |  |  |  |  | 12 | migration |
| `organization_task_qa_reviews` | 0 |  |  | encrypted `data` + blind indexes |  |  |  |  | 7 | migration |
| `organization_task_responses` | 9 |  |  | encrypted `data` + blind indexes |  |  |  |  | 12 | migration |
| `organization_tasks` | 133 |  | 237 | encrypted `data` + blind indexes | agent_session_id | 747 | 149881 | 955 | 47 | migration |
| `orgs` | 1 | 1 |  | encrypted `data` + blind indexes | name | 8207 | 16705 | 0 | 43 | migration |
| `outreach_log` | 23 | 23 |  | encrypted `data` + blind indexes |  |  |  |  | 6 | runtime only (no migration) |
| `outreach_team` | 0 | 0 |  | plaintext columns + blind indexes | name |  |  |  | 9 | runtime only (no migration) |
| `owner_encryption_keys` | 13 | 0 |  | plaintext columns + blind indexes |  |  |  |  | 8 | migration |
| `package_installs` | 30 |  | 2 | plaintext only |  | 94 | 94 | 188 | 10 | migration |
| `package_ratings` | 0 |  |  | plaintext only |  |  |  |  | 7 | migration |
| `password_reset_rate` | 0 |  |  | plaintext columns + blind indexes |  |  |  |  | 10 | migration |
| `patreon_account_links` | 0 |  |  | encrypted `data` + blind indexes |  |  |  |  | 8 | migration |
| `patreon_oauth_states` | 0 |  |  | encrypted `data` + blind indexes |  |  |  |  | 11 | migration |
| `patreon_reconciliation_generations` | 0 |  |  | plaintext columns + blind indexes |  |  |  |  | 4 | migration |
| `patreon_webhook_events` | 0 |  |  | plaintext columns + blind indexes |  |  |  |  | 8 | migration |
| `pending_rewards` | 0 | 0 |  | plaintext columns + blind indexes | recipient_name, source_address, wallet_address, tx_signature |  |  |  | 15 | migration |
| `pending_verifications` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 14 | migration |
| `polar_customers` | 0 |  |  | encrypted `data` + blind indexes |  |  |  |  | 7 | migration |
| `polar_memberships` | 0 |  |  | encrypted `data` + blind indexes |  |  |  |  | 12 | migration |
| `polar_webhook_events` | 0 |  |  | plaintext only |  |  |  |  | 7 | migration |
| `private_mirror_routes` | 0 | 0 |  | plaintext columns + blind indexes |  |  |  |  | 12 | migration |
| `product_clone_daily` | 0 |  |  | plaintext only |  |  |  |  | 10 | migration |
| `profile_contribution_days` | 405 | 22 | 177 | encrypted `data` + blind indexes |  | 432 | 421804 | 0 | 17 | migration |
| `profile_contribution_languages` | 371 | 28 | 102 | plaintext columns + blind indexes |  |  |  |  | 9 | migration |
| `profile_contribution_projects` | 41 | 2 | 22 | encrypted `data` + blind indexes |  | 3468 | 424840 | 9108 | 31 | migration |
| `profile_contribution_receipts` | 35 | 2 | 11 | plaintext columns + blind indexes |  |  |  |  | 7 | migration |
| `profile_follows` | 14 | 2 |  | plaintext columns + blind indexes | follower_name, target_name |  |  |  | 18 | migration |
| `public_api_snapshots` | 5 |  |  | plaintext only | content_length, payload | 19850 | 17018 | 14622 | 11 | migration |
| `pull_inbox` | 0 | 1 |  | encrypted `data` + blind indexes |  | 2729 | 3026 | 216 | 36 | migration |
| `quarantined_nodes` | 0 |  |  | encrypted `data` + blind indexes | pubkey, name | 151 | 68403 | 0 | 11 | migration |
| `quarantined_users` | 170 |  | 237 | encrypted `data` + blind indexes | username, reason_codes | 151 | 68403 | 0 | 20 | migration |
| `referral_attributions` | 113 |  | 94 | encrypted `data` + blind indexes | referred_name, referrer_name | 563 | 203055 | 0 | 48 | migration |
| `referral_click_audit` | 170 |  | 102 | encrypted `data` + blind indexes |  | 22 | 32 | 78 | 7 | migration |
| `referral_click_claims` | 42 |  | 5 | plaintext columns + blind indexes |  | 27 | 0 | 81 | 6 | migration |
| `referral_click_seen` | 178 |  | 25 | plaintext columns + blind indexes |  |  |  |  | 5 | migration |
| `referral_legacy_totals` | 7 |  |  | plaintext columns + blind indexes | name | 119 | 101250 | 0 | 4 | migration |
| `referral_referrer_controls` | 1 |  |  | plaintext columns + blind indexes | reason_codes |  |  |  | 5 | migration |
| `referral_stats` | 9 |  |  | plaintext columns + blind indexes | name | 153 | 101284 | 68 | 14 | migration |
| `relay_self` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 4 | runtime only (no migration) |
| `relays` | 0 | 0 |  | plaintext columns + blind indexes | pubkey, label, base_url |  |  |  | 33 | migration |
| `release_downloads` | 1568 | 1568 | 223 | plaintext columns + blind indexes | ua |  |  |  | 5 | migration |
| `repo_agents` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 13 | runtime only (no migration) |
| `repo_alert_settings` | 3 |  |  | `data` is **plaintext JSON** (operational) |  | 11953 | 0 | 0 | 3 | migration |
| `repo_deletions` | 0 |  |  | plaintext columns + blind indexes |  |  |  |  | 7 | runtime only (no migration) |
| `repo_device_mirrors` | 0 |  |  | encrypted `data` + blind indexes |  |  |  |  | 7 | migration |
| `repo_first_hosted` | 5 | 8 |  | plaintext columns + blind indexes |  | 3571 | 35710 | 0 | 7 | migration |
| `repo_forks` | 0 |  |  | plaintext columns + blind indexes | fork_name |  |  |  | 13 | migration |
| `repo_media` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 11 | migration |
| `repo_merge_jobs` | 1 |  |  | plaintext columns + blind indexes |  |  |  |  | 22 | migration |
| `repo_privacy_policy` | 0 | 0 |  | plaintext columns + blind indexes |  |  |  |  | 4 | migration |
| `repo_security_scan_leases` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 6 | migration |
| `repo_security_scan_reviews` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 6 | migration |
| `repo_security_scans` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 8 | migration |
| `repo_shares` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 20 | migration |
| `repo_source_authorities` | 29 |  | 4 | plaintext columns + blind indexes | machine_name | 2267 | 65743 | 0 | 5 | migration |
| `repo_stars` | 8 | 3 |  | plaintext columns + blind indexes |  |  |  |  | 11 | migration |
| `repo_state_history` | 768 | 35 | 106 | plaintext columns + blind indexes |  | 21963 | 143054 | 0 | 15 | migration |
| `repo_terms_flags` | 0 |  |  | encrypted `data` + blind indexes |  |  |  |  | 5 | migration |
| `repositories` | 54 | 23 | 141 | encrypted `data` + blind indexes | is_private | 115831 | 3214258 | 6844 | 292 | migration |
| `repository_fork_jobs` | 0 |  |  | encrypted `data` + blind indexes |  | 6345 | 0 | 0 | 11 | migration |
| `repository_imports` | 2 | 0 |  | encrypted `data` + blind indexes | is_private | 22029 | 24581 | 0 | 21 | migration |
| `repository_logo_suggestions` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 27 | migration |
| `repository_migration_jobs` | 1 |  |  | encrypted `data` + blind indexes |  | 43 | 43 | 129 | 11 | migration |
| `repository_migration_objects` | 13 |  |  | encrypted `data` + blind indexes | payload_hash | 29 | 3 | 84 | 10 | migration |
| `repository_migration_runs` | 2 |  |  | encrypted `data` + blind indexes |  |  |  |  | 6 | migration |
| `repository_mirror_volunteers` | 1 | 0 |  | plaintext columns + blind indexes | node_label |  |  |  | 7 | migration |
| `repository_monitor_state` | 49 |  | 4 | plaintext only | reason | 76586 | 1022149 | 2917 | 29 | runtime only (no migration) |
| `request_block_rules` | 8 |  |  | plaintext only | note |  |  |  | 2 | migration |
| `reward_contribution_intents` | 0 | 0 |  | plaintext only |  |  |  |  | 10 | migration |
| `reward_contributions` | 2 | 0 |  | plaintext only | reference_address, tx_signature, source_address |  |  |  | 22 | migration |
| `reward_node_observations` | 0 | 0 |  | plaintext columns + blind indexes |  |  |  |  | 7 | migration |
| `reward_rounds` | 0 | 0 |  | plaintext only |  |  |  |  | 10 | migration |
| `reward_wallet_reminders` | 0 |  |  | plaintext columns + blind indexes | recipient_name |  |  |  | 4 | migration |
| `role_grants` | 0 | 0 |  | plaintext columns + blind indexes |  |  |  |  | 17 | migration |
| `schema_meta` | 49 | 2 | 4 | plaintext only |  | 78409 | 115688 | 190 | 15 | migration |
| `security_reports` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 6 | migration |
| `sensitive_audit_log` | 23625 | 0 | 5276 | plaintext columns + blind indexes | actor_label, target_token, details | 222 | 1422378 | 768 | 8 | migration |
| `signup_rate` | 291 | 5 | 22 | plaintext columns + blind indexes |  |  |  |  | 6 | runtime only (no migration) |
| `site_referrers` | 23 |  |  | plaintext only | last_url |  |  |  | 11 | migration |
| `system_status_daily` | 0 | 129 |  | plaintext only |  | 3325 | 63891 | 0 | 4 | migration |
| `system_status_daily_batch` | 30 |  | 21 | `data` is **plaintext JSON** (operational) |  | 4458 | 159036 | 0 | 6 | migration |
| `system_status_hourly` | 0 | 1691 |  | plaintext only | reason | 193 | 3649 | 3456 | 4 | migration |
| `system_status_hourly_batch` | 734 |  | 625 | `data` is **plaintext JSON** (operational) |  | 4096 | 77254 | 77 | 6 | migration |
| `system_status_minute` | 0 | 216 |  | plaintext only | reason | 26475 | 26475 | 0 | 6 | runtime only (no migration) |
| `system_status_minute_batch` | 1842 |  | 1152 | `data` is **plaintext JSON** (operational) |  | 37142 | 298820 | 27961 | 7 | migration |
| `system_status_sample_claim` | 1843 |  | 117 | plaintext only |  | 20165 | 28007 | 17853 | 4 | migration |
| `telemetry` | 313 | 291 | 1849 | plaintext only | summary |  |  |  | 0 (word hits are comments) | orphaned (pipeline removed 2026-07-25) |
| `thread_subscriptions` | 34 | 6 | 8 | encrypted `data` + blind indexes |  |  |  |  | 6 | migration |
| `tip_notice` | 39 |  | 10 | plaintext columns + blind indexes | signature, emailed, task_description, output_url |  |  |  | 15 | migration |
| `usage_budget_state` | 3 |  |  | plaintext only |  | 3096 | 3096 | 3096 | 4 | migration |
| `users` | 241 | 9 | 1983 | encrypted `data` + blind indexes | username | 94052 | 1269073 | 928 | 269 | migration |
| `wallet_balances` | 15 |  |  | plaintext only | wallet, name | 73 | 73 | 146 | 10 | migration |
| `world_build_board_items` | 24 |  |  | plaintext columns + blind indexes | title |  |  |  | 13 | migration |
| `world_deploy_status` | 1 |  |  | plaintext only |  | 10009 | 10009 | 0 | 3 | migration |
| `world_element_purchases` | 0 |  |  | plaintext columns + blind indexes | reference_address, tx_signature, source_address, deposit_address, deposit_secret, sweep_signature |  |  |  | 27 | migration |
| `world_events` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 17 | migration |
| `world_fediverse_instances` | 0 | 0 |  | encrypted `data` + blind indexes | url |  |  |  | 10 | migration |
| `world_fediverse_mention_moderation` | 0 | 0 |  | plaintext columns + blind indexes | reason |  |  |  | 4 | migration |
| `world_fediverse_mentions` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 19 | migration |
| `world_inactive_presence` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 9 | migration |
| `world_lobby_links` | 0 |  |  | encrypted `data` + blind indexes |  |  |  |  | 10 | migration |
| `world_manual_blocks` | 0 |  |  | plaintext columns + blind indexes | subject_token |  |  |  | 5 | migration |
| `world_media_items` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 18 | migration |
| `world_media_playback` | 0 | 0 |  | plaintext columns + blind indexes |  |  |  |  | 14 | migration |
| `world_media_roles` | 0 | 0 |  | plaintext columns + blind indexes | account_label |  |  |  | 13 | migration |
| `world_media_schedules` | 0 | 0 |  | encrypted `data` + blind indexes |  |  |  |  | 12 | migration |
| `world_media_spaces` | 0 | 0 |  | plaintext columns + blind indexes | name, description, session_type, owner_label |  |  |  | 19 | migration |
| `world_office_attendance` | 259 |  | 37 | plaintext columns + blind indexes | account_name | 210 | 266376 | 0 | 21 | migration |
| `world_office_marketing_checkins` | 0 |  |  | encrypted `data` + blind indexes |  |  |  |  | 7 | migration |
| `world_office_marketing_initiatives` | 0 |  |  | encrypted `data` + blind indexes |  |  |  |  | 4 | migration |
| `world_office_marketing_proofs` | 0 |  |  | encrypted `data` + blind indexes |  |  |  |  | 9 | migration |
| `world_office_marketing_tasks` | 0 |  |  | encrypted `data` + blind indexes |  |  |  |  | 11 | migration |
| `world_qa_items` | 13 |  |  | plaintext only | title |  |  |  | 5 | migration |
| `world_qa_reviews` | 58 |  | 6 | plaintext columns + blind indexes |  |  |  |  | 7 | migration |
| `world_satellite_snapshot` | 1 |  |  | `data` is **plaintext JSON** (operational) |  |  |  |  | 6 | migration |
| `world_user_activity` | 109 |  | 11 | plaintext columns + blind indexes |  | 546 | 303000 | 0 | 19 | migration |
| `world_visit_stats` | 17 |  |  | plaintext only |  |  |  |  | 1 | migration |
| `world_visit_unique_hll` | 563 |  | 3 | plaintext only |  | 58 | 64656 | 0 | 5 | migration |
| `world_workshop_events` | 0 | 0 |  | encrypted `data` + blind indexes | session_id |  |  |  | 6 | migration |
| `world_workshop_participants` | 0 | 0 |  | encrypted `data` + blind indexes | session_id |  |  |  | 13 | migration |
| `world_workshop_results` | 0 | 0 |  | encrypted `data` + blind indexes | session_id |  |  |  | 8 | migration |
| `world_workshop_sessions` | 0 | 0 |  | encrypted `data` + blind indexes | session_id |  |  |  | 12 | migration |