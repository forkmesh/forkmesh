# D1 database cleanup plan

Issue #682. This plan turns the 2026-09-10 audit of the production D1 database
`forkmesh` into ordered, prioritised steps. Every step has a ready prompt to
hand to an agent. The full audit, with a row for every table, is in the
repository at `docs/operations/database-audit-2026-09-10.md` and attached to
the issue.

The plan was checked against the code on 2026-09-14 (branch point
`4954dc197`). Several audit items had already shipped by then, and a few
audit claims turned out to be wrong. The [corrections](#corrections-to-the-audit)
section lists them, and the step prompts already reflect them.

## How to use this plan

1. Pick the first step in the [order of work](#order-of-work) whose status is
   **open** and whose dependencies are **done**. Skip steps marked
   **owner decision** until the decision is recorded in the
   [decision register](#owner-decisions).
2. Start a fresh agent in its own worktree. Paste the
   [shared preamble](#shared-agent-preamble), then the step's prompt.
3. Steps marked **operator** need production credentials (`app/.env.production`
   on the deploy box). Run them yourself, or give the prompt to an agent on
   that machine.
4. In the same commit as the work, the agent updates that step's row in the
   order of work: status, branch, and a one-line result.
5. Deploy order matters. A step that removes readers must be **live in
   production** before the step that drops the table is merged.

Priorities:

| Priority | Meaning |
|---|---|
| **P0** | Safety net. Do first; no behaviour change. |
| **P1** | Cheap and high value: dead reads, the largest remaining writes and reads, audit log growth. |
| **P2** | Worthwhile but larger: remaining hot paths, rate-table retention, key rotation groundwork. |
| **P3** | Gated on an owner decision, or cosmetic. |

## Order of work

| # | Step | Priority | Kind | Depends on | Status |
|---|---|---|---|---|---|
| 1 | [S0.1 Export production D1](#s01-export-production-d1) | P0 | operator | — | open |
| 2 | [S0.2 Migrations replay into a complete schema](#s02-migrations-replay-into-a-complete-schema) | P0 | agent | — | open |
| 3 | [S0.3 Re-measure the read baseline](#s03-re-measure-the-read-baseline) | P0 | operator | — | open |
| 4 | [S1.1 Remove readers of the legacy status tables](#s11-remove-readers-of-the-legacy-status-tables) | P1 | agent | S0.2 | open |
| 5 | [S1.2 Remove readers of `host_presence`](#s12-remove-readers-of-host_presence) | P1 | agent | S0.2 | open |
| 6 | [S1.3 Export `telemetry`](#s13-export-telemetry) | P1 | operator | S0.1 | open |
| 7 | [S3.1 Audit only agent status transitions](#s31-audit-only-agent-status-transitions) | P1 | agent | — | open |
| 8 | [S2.1 Retention prunes run once an hour from cron](#s21-retention-prunes-run-once-an-hour-from-cron) | P1 | agent | — | open |
| 9 | [S2.2 Skip unchanged public snapshot rewrites](#s22-skip-unchanged-public-snapshot-rewrites) | P1 | agent | S0.3 | open |
| 10 | [S1.4 Drop the dead tables](#s14-drop-the-dead-tables) | P1 | agent | S1.1, S1.2 **deployed**; S1.3; decision D1 | open |
| 11 | [S2.3 Remaining direct catalog scans](#s23-remaining-direct-catalog-scans) | P2 | agent | S0.3 | open |
| 12 | [S2.4 `online_hourly_nodes` reads](#s24-online_hourly_nodes-reads) | P2 | agent | S0.3 | open |
| 13 | [S3.2 Prune the rate-limit tables](#s32-prune-the-rate-limit-tables) | P2 | agent | — | open |
| 14 | [S4.1 Versioned ciphertext format](#s41-versioned-ciphertext-format) | P2 | agent | — | open |
| 15 | [S4.2 Pin derived keys, then rotate `DATA_KEY`](#s42-pin-derived-keys-then-rotate-data_key) | P2 | agent + operator | S4.1; decision D7 | open |
| 16 | [S2.5 Measure and split `users.data`](#s25-measure-and-split-usersdata) | P2 | agent | S0.1 | open |
| 17 | [S4.3 Label the plaintext `data` columns](#s43-label-the-plaintext-data-columns) | P3 | agent | decision D6 | open |
| 18 | [S3.3 Tiered audit retention](#s33-tiered-audit-retention) | P3 | agent | decision D2 | owner decision |
| 19 | [S3.4 `release_downloads` roll-up](#s34-release_downloads-roll-up) | P3 | agent | decision D3 | owner decision |
| 20 | [S5.1 Retire legacy custody code](#s51-retire-legacy-custody-code) | P3 | agent | S0.1; decision D5 | owner decision |
| 21 | [S5.2 Drop the legacy custody tables](#s52-drop-the-legacy-custody-tables) | P3 | agent | S5.1 **deployed** | owner decision |
| 22 | [S6.1 Dormant feature inventory](#s61-dormant-feature-inventory) | P3 | agent | — | open |
| 23 | [S7.1 Re-run the audit and close the issue](#s71-re-run-the-audit-and-close-the-issue) | — | operator + agent | everything above | open |

Already done before this plan was written (no step needed):

| Audit item | Where it landed |
|---|---|
| Public catalog read from one shared row keyed on `catalog_rev` | `public_catalog_snapshot` (migration `0179`); `_decrypted_public_catalog` and `_public_catalog_snapshot_read` / `_store` in `app/src/entry.py` |
| `SELECT COUNT(*) FROM repositories` full scans | `_repositories_count` (rev memo, then snapshot count, then the shared read guard) |
| `repository_monitor_state` roster upsert and whole-table reads | `_status_monitor_prior_rows` (exact `IN` list plus memo) and the changed-rows-only upsert in `app/src/status_monitoring.py` |
| Notifications cap trim on every insert | `_notification_trim_due` in `enqueue_notification` |
| Admin table counts and heavy admin reads | `app/src/d1_read_guard.py` (migration `0186`) |
| `world_element_purchases.deposit_secret` sealing | Already stored as `encrypt_row` output (see corrections) |

## Corrections to the audit

These were found while checking the audit against the code. The steps below
already follow the corrected facts.

- **`deposit_secret` is not plaintext.** `world_element_store_handler`
  (`world/src/world_routes.py`) stores `encrypt_row(env, {"seed": seed})`, and
  `_sweep_element_deposit` decrypts it and clears it after a successful sweep.
  Migration `0119` documents the column as an encrypted blob. The Phase 4
  "encrypt `deposit_secret`" item is dropped; S4.1 only adds a guard test.
- **The `account_sessions.client_ip` comment is not stale.** `schema.py`
  declares the column, `SCHEMA_ALTER_STATEMENTS` adds it, and
  `_account_session_token` writes it. The audit's live check likely predates
  the ALTER. S0.3 confirms the live column; no code change is planned.
- **`system_status_hourly` and `system_status_daily` still have readers**
  (`_status_hour_records`, `_status_day_records`) and prunes, and
  `world_admin_delete_node_handler` deletes from all three legacy status
  tables. They cannot be dropped until S1.1 is live.
- **`outreach_log` and `outreach_team` no longer exist.** They moved to the
  CRM plugin (`crm_outreach_*`) and an app migration dropped them. They are not
  part of the catch-up migration.
- **Four more tables have no creating migration:** `bounty_wallet`,
  `host_presence`, `issue_bounty`, and `system_status_minute`. They are all
  leaving, so they stay out of the catch-up migration too.
- **`issue_bounty` and `funds_received` still have live code**: bounty
  routes in `entry.py`, `ledger.py` reads, `_record_funds_received`,
  `_funds_received_boards`, and a `DELETE` in `catalog_routes.py`.
  `central_fund` has the `_account_central_fund` route in `app_runtime.py`.
  S5.1 has to remove those paths before any custody table can be dropped.
- **`feedback_email_sends` must not be pruned by age.** `_send_feedback_emails`
  picks recipients with a `LEFT JOIN` on that table: a missing row means "not
  sent yet". Deleting old rows would send the feedback email again to every
  older account. It grows by one row per account, which is fine.
- **0075 duplicates only the `completed_at` column**, not its index. Clean
  replay fails at `0089` with `duplicate column name: completed_at`, and that
  failure also skips `0089`'s index.
- **The `ensure_schema` fast path calls `_assert_legacy_wallet_custody_ready`
  from two places** in `_apply_schema`, and it runs a second `schema_meta`
  SELECT. It goes away with S5.1.
- **Traffic numbers are stale.** The audit's 7-day baseline predates
  migrations `0179` and `0186` and the read-diet passes of 2026-09-11 to
  2026-09-14. Later daily bills were both higher and lower than the audit's
  1.9 M rows a day. S0.3 re-measures before any Phase 2 target is judged.

The desktop's Database tab shows a label per table from
`ADMIN_DATABASE_AUDIT_FINDINGS` in `app/src/admin_database.py`. Those labels
were corrected in the same change as this plan.

## Shared agent preamble

Paste this before every step prompt.

```text
You are working on ForkMesh issue #682, the D1 database cleanup. The plan is
www/docs/operations/d1-database-cleanup-plan.md and the audit it came from is
docs/operations/database-audit-2026-09-10.md. Read your step in the plan and
the "Corrections to the audit" section before touching code. Line numbers in
the plan drift; find code by function name.

Ground rules:
- Work in your own git worktree and branch. Never use `git stash`. Commit when
  done; do not push and do not open a pull request unless asked.
- Before adding a migration, run `git log main --oneline -- app/migrations`
  and take the next free number. If main takes your number while you work,
  main keeps it and you renumber.
- Any change to the runtime schema text (SCHEMA_STATEMENTS in app/src/schema.py,
  or SCHEMA_PRE_CREATE_ALTER_STATEMENTS / SCHEMA_ALTER_STATEMENTS in
  app/src/entry.py) must move three things together: the LATEST migration ends
  with the `schema_fingerprint:<sha256>` stamp INSERT, `_SCHEMA_FINGERPRINT` in
  entry.py holds the same literal, and
  test_local_migrations::test_latest_native_migration_stamps_exact_runtime_schema_fingerprint
  pins that migration's file name. Compute the hash the way that test's
  `_runtime_schema_fingerprint()` does. A schema-only change with no DDL still
  needs a stamp-only migration.
- Dropping a table takes two deploys. Deploy N removes every reader, writer
  and the table's DDL from schema.py (ensure_schema replays schema.py whenever
  the fingerprint changes, so leftover DDL re-creates the table). Deploy N+1
  ships the DROP migration. Never put both in one change.
- `CREATE TRIGGER IF NOT EXISTS` never rewrites an existing trigger; a changed
  body needs DROP TRIGGER IF EXISTS plus the new CREATE.
- Many app/tests files assert on source text: SQL strings, function names, or
  slices of entry.py between two function names. Before renaming, deleting or
  moving anything, `grep -rn '<name or literal>' app/tests/`. Deleting a
  function can break a slice in a test about its neighbour. New helpers
  called from functions that AST-based harnesses extract should be reached
  via `globals().get("name")`, or added to that harness's loader list.
- A new lazy Worker module must be registered the same way
  app/src/d1_read_guard.py is (grep that name in app/src/entry.py and tools/),
  or the deploy fails at startup.
- A new cron phase must be added to CRON_PHASE_CATALOG in
  app/src/cron_activity.py and to the copies pinned by test_cron_runner and
  test_status_page.
- Never key liveness on `checked_at`.
- Tests: run the test files that reference what you changed, plus
  `python3 tools/quality_gate.py run --profile core`. For any failure, rerun
  the same test ids on a detached main worktree
  (`git worktree add --detach /tmp/<name> main`); only failures that main does
  not have are yours. Several source-text tests already fail on main.
- In the same commit, update your step's row in the plan's "Order of work"
  table: status (done / blocked), your branch, and a one-line result. If you
  learn something that changes a later step, edit that step too.

Finish with a short report: what changed, which tests you ran with results,
what must be deployed before the next step, and anything you could not verify.
```

---

## Phase 0: safety net

### S0.1 Export production D1

**Priority P0 · operator · no code.** Take a full export before any
destructive migration, and again right before S1.4 and S5.2 run.

```text
On the deploy box, from app/, take a full export of the production D1
database before the #682 cleanup. Load credentials from app/.env.production
(parse it with grep '^KEY=' lines; `source` fails on some values), put
Node 22 on PATH (export PATH=$HOME/.nvm/versions/node/v22.23.1/bin:$PATH),
then run:

  ts=$(date -u +%Y%m%dT%H%M%SZ)
  mkdir -p <deploy-backups>/$ts
  npx wrangler@4.136.2 d1 export forkmesh --remote \
    --output <deploy-backups>/$ts/forkmesh-d1-pre-audit-cleanup.sql

Load the export into scratch SQLite (sqlite3 /tmp/check.sqlite < file) and
report its table count and the row counts of users, repositories and
sensitive_audit_log next to the live counts. Do not modify production.
Record the backup path in step S0.1's row of
www/docs/operations/d1-database-cleanup-plan.md.
```

**Done when:** the export loads into SQLite and its row counts match
production.

### S0.2 Migrations replay into a complete schema

**Priority P0 · agent.** A fresh database built from `app/migrations` fails
at `0089`, and 12 tables that stay exist only because `schema.py` creates them
at runtime.

```text
Step S0.2 of the D1 cleanup plan: make a clean migration replay build every
table the Worker keeps.

1. Clean replay failure. app/migrations/0075_world_office_marketing_tasks.sql
   was edited after the fact to include the `completed_at` column that
   0089_world_office_task_completion.sql adds with ALTER TABLE. Production
   applied both, and D1 tracks migrations by name, so remove only the
   `completed_at` column definition from 0075's CREATE TABLE (keep 0075's
   other columns and indexes). Check whether any test in
   app/tests/test_local_migrations.py runs 0075 alone and expects the column;
   if one does, update it to run 0075 then 0089.
2. Catch-up migration. Add the next-numbered migration with verbatim
   `CREATE TABLE IF NOT EXISTS` (plus their indexes) copied from
   app/src/schema.py for: agent_prompts, ai_ask_rate, catalog_rev,
   clone_sticky, issue_seq, login_attempts, online_hourly_nodes, relay_self,
   repo_agents, repo_deletions, repository_monitor_state, signup_rate.
   Do NOT add bounty_wallet, host_presence, issue_bounty or
   system_status_minute; they are being removed. outreach_log/outreach_team
   no longer exist. Add the matching app/migrations/down file (DROP TABLE IF
   EXISTS for each). Because this migration becomes the latest one, it must
   end with the current schema fingerprint stamp, and the test's file-name pin
   must move to it.
3. Replay test. Add a test (in test_local_migrations.py or a new file) that
   replays every app/migrations/*.sql in version order into an in-memory
   sqlite3 database (one executescript per file), fails on the first error
   with the file name, then asserts that every table created in
   schema.SCHEMA_STATEMENTS exists, except an explicit, commented allowlist
   of tables scheduled for removal (bounty_wallet, host_presence,
   issue_bounty, system_status_minute). Later steps shrink that allowlist.
4. Verify from the repo root:
   rm -f /tmp/s.sqlite; for f in $(ls app/migrations/*.sql | sort -V); do
     sqlite3 /tmp/s.sqlite < "$f" || echo "FAILED $f"; done
   It must print no FAILED line.
```

**Done when:** the loop above completes with no errors and the new test
passes.

### S0.3 Re-measure the read baseline

**Priority P0 · operator.** The audit's 7-day numbers predate the catalog
snapshot and the read guard. Phase 2 targets need a current baseline.

```text
Step S0.3 of the D1 cleanup plan: take a fresh D1 traffic baseline. Do not
modify production.

From app/ on the deploy box, with credentials from app/.env.production and
Node 22 on PATH:

  npx wrangler@4.136.2 d1 info forkmesh
  npx wrangler@4.136.2 d1 insights forkmesh --time-period 7d \
    --sort-by reads --limit 60 --json > /tmp/d1-insights-7d.json

For an hourly bill, query the Cloudflare GraphQL analytics API
d1QueriesAdaptiveGroups ordered by sum_rowsRead_DESC with
USAGE_ANALYTICS_API_TOKEN from app/.env.production. `SELECT COUNT(*) AS
row_count FROM "<table>"` rows are the D1 dashboard's table browser, not our
code; list them separately.

Also run, read-only:
  npx wrangler@4.136.2 d1 execute forkmesh --remote --json \
    --command "PRAGMA table_info(account_sessions)"
and report whether client_ip exists (expected: yes, so no change needed).

Add a "Baseline 2026-MM-DD" subsection under "Measurement" in
www/docs/operations/d1-database-cleanup-plan.md with: total 7-day rows read
and written, the top 15 statements by rows read (runs, rows read, rows
written, the table), and which plan step each belongs to. Write a new
rows-read target for S7.1 if the old 13.5 M to 9 M one no longer fits.
```

**Done when:** the baseline subsection is committed and each top reader is
mapped to a step, or marked "not in plan".

---

## Phase 1: remove dead reads, then drop dead tables

### S1.1 Remove readers of the legacy status tables

**Priority P1 · agent · deploy N.** `system_status_minute`, `_hourly` and
`_daily` have no writer and 0 rows. The `*_batch` tables replaced them, but
the reads and prunes remain.

```text
Step S1.1 of the D1 cleanup plan: stop reading the legacy status tables
system_status_minute, system_status_hourly and system_status_daily (no
writer, 0 rows; the *_batch tables replaced them).

In app/src/status_monitoring.py:
- _status_minute_records: delete the legacy half of the dual read; keep the
  *_batch path and its memos (_STATUS_MINUTE_BATCH_MEMO,
  _STATUS_MINUTE_RECORDS_CACHE) exactly as they behave now.
- _write_status_minute_batch: remove its SELECT on system_status_minute.
- _status_hour_records and _status_day_records: remove the legacy-table
  reads and keep the batch reads.
- record_status_compaction (the hourly prune block): remove the DELETEs on the
  three legacy tables; keep the *_batch prunes.
In world/src/world_routes.py, world_admin_delete_node_handler: remove the
DELETEs on the three legacy tables.
In app/src/schema.py: remove the three CREATE TABLE statements and their
indexes (stamp rules apply: add a stamp-only migration). In
app/src/admin_database.py, change the three findings to "drop" with a note
that readers were removed.
Remove the three tables from the S0.2 replay-test allowlist if S0.2 added
system_status_minute there (hourly and daily have migrations; leave the
migrations alone).

Tests: grep app/tests for system_status_minute, system_status_hourly,
system_status_daily, _status_minute_records, _status_hour_records,
_status_day_records and record_status_compaction, and update assertions that
pin the legacy SQL. test_status_page has an assertion that no INSERT into
system_status_minute exists; keep its intent. Show that /api/status
systems[] is unchanged: build the handler's payload in a test from batch-only
fixtures before and after, and compare.
```

**Done when:** no code references the three tables, `/api/status`
`systems[]` output is unchanged, and the change is deployed. Record the
deploy date in the row; S1.4 waits for it.

### S1.2 Remove readers of `host_presence`

**Priority P1 · agent · deploy N.** Repository sockets were retired for signed
HTTPS mirrors. No code writes `host_presence`, yet about 9.9k queries a week
still read the empty table.

```text
Step S1.2 of the D1 cleanup plan: remove every use of host_presence. It has
no writer since repository sockets were retired (see the comment in
catalog_handler in app/src/catalog_routes.py), so every read returns nothing.

In app/src/entry.py, remove the host_presence reads, deletes and the rename
from: hydrate_repo_group_live_hosts, network_stats (fallback COUNT and
DELETE), _live_online_nodes, active_registered_node_bis,
_repository_import_mirror_status, notify_stale_hosts_offline, admin_stats
(COUNT and DELETE), purge_stale_registered_nodes, _delete_repo_scoped_state
and _move_repo_namespace. Also grep world/src, marketplace/packages and
app/src for any other reference (d1_read_guard.py's admin-stats reader SQL
names it).

Where a function still shows a count or live-host map, keep the output
shape and source it from signed HTTPS mirrors, as catalog_handler already
does: mirror_https_endpoints rows that are not blocked and are fresh within
HTTPS_MIRROR_STATUS_FRESH_MS. If a value only ever came from host_presence and
was always empty in production, return the empty value directly instead of
adding a new query, and say so in the report.

Remove the host_presence DDL and indexes from app/src/schema.py (stamp rules
apply), drop it from the S0.2 replay-test allowlist, and change its
admin_database.py finding to "drop".

Tests: grep app/tests for host_presence and every function above. Keep test
intent. Where a test fed host_presence rows to prove a feature, rewrite it to
feed mirror_https_endpoints, or delete it if the feature itself was the
retired socket heartbeat.
```

**Done when:** `grep -rn host_presence app/src world/src marketplace` returns
nothing, and the change is deployed.

### S1.3 Export `telemetry`

**Priority P1 · operator.** `telemetry` holds 313 plaintext desktop crash
summaries (1.8 MB). Nothing has read or written it since `b417261a1`
(2026-07-25). Its DDL is already gone from the code.

```text
Step S1.3 of the D1 cleanup plan: export the orphaned telemetry table before
it is dropped. Read-only against production.

From app/ on the deploy box (credentials from app/.env.production, Node 22):
  npx wrangler@4.136.2 d1 execute forkmesh --remote --json \
    --command "SELECT * FROM telemetry ORDER BY rowid" \
    > <deploy-backups>/<ts>/telemetry-export.json
Confirm the row count is 313 (or report the real count) and the newest row
date. The rows are plaintext desktop diagnostics: keep the file mode 600 and
do not commit it. Record the path in the plan's decision D1 row and ask the
owner whether the export itself is kept or deleted after the drop.
```

### S1.4 Drop the dead tables

**Priority P1 · agent · deploy N+1.** Only merge after S1.1 and S1.2 are
**live in production** and S0.1 has a fresh export.

```text
Step S1.4 of the D1 cleanup plan: drop the dead tables. Confirm first, and
stop if any check fails:
- The plan's order-of-work rows for S1.1 and S1.2 are done and their changes
  are deployed (check `git log main` and the deployed build rev on /health).
- `grep -rn` over app/src, world/src and marketplace/packages finds no
  reference to: commit_inbox, forkmesh_d1_migrations, telemetry,
  host_presence, system_status_minute, system_status_hourly,
  system_status_daily (the English word "telemetry" in unrelated code is fine;
  look for SQL).
- Decision D1 (telemetry) is recorded.

Add the next-numbered migration with one `DROP TABLE IF EXISTS <t>;` per
table (do NOT touch relay_self), ending with the current fingerprint stamp.
Add a down migration that recreates the empty shapes: copy the last known
DDL from git history (schema.py before S1.1/S1.2, migration 0009 for
commit_inbox; forkmesh_d1_migrations and telemetry have no DDL in the repo, so
recreate them from the columns in a production export:
`sqlite3 <export>.sqlite ".schema telemetry"`).
Remove those tables' entries from ADMIN_DATABASE_AUDIT_FINDINGS in
app/src/admin_database.py.

Replay test from S0.2 must still pass. After deploy, the operator checks
that a 7-day `d1 insights` run shows no statement touching these tables.
```

**Done when:** the migration is applied in production, and a 7-day insights
window starting after the deploy shows none of these tables.

---

## Phase 2: hot-path reads and writes

Re-measure with the S0.3 commands after each step and add the before and after
numbers to the step's row.

### S2.1 Retention prunes run once an hour from cron

**Priority P1 · agent.** The `api_metrics_minute` prune runs from the request
flush path about once per *isolate*-hour: 201 runs and 38.5k rows written in
the audit week. The status batch prunes run inside the status sampler.

**Update 2026-09-22:** `api_metrics_minute` is gone (migration 0190). The
desktop's Web Requests tab counts per-route traffic from the Worker log tail
instead, and the /status flagship evidence moved to the failure-only
`flagship_read_failure_minute`, which prunes on its own rare write path. Only
the status-sampler prunes below still apply.

```text
Step S2.1 of the D1 cleanup plan: make D1 retention prunes run at most once
an hour, from cron, not per isolate.

Today:
- app/src/api_metrics.py `_flush` prunes api_metrics_minute when minute == 7
  using the in-memory `_last_prune_minute`, so every isolate that flushes in
  that minute deletes again.
- app/src/status_monitoring.py deletes old rows from the *_batch tables,
  durable_object_abort_minute and the other tables in that block "on the
  first sample of each hour" inside the status sampler.

Change:
1. Add one hourly cron phase (for example `d1-retention`) beside the other
   hourly retention phases (sensitive-cleanup, chat-prune, ...). Register it in
   CRON_PHASE_CATALOG and the test copies (see the preamble).
2. Move the api_metrics_minute prune and the status-sampler prunes into that
   phase with the same cutoffs and constants. Remove the flush-path prune and
   `_last_prune_minute`.
3. Guard the phase with one `schema_meta` row (k = 'retention:last_prune_ms'):
   skip if it is younger than 55 minutes, else upsert it first and then run
   the deletes. That keeps a retried or duplicated tick from pruning twice.
4. Keep the sampler's first-sample-of-the-hour path cheap; the status sample
   must still run first on every tick.

Tests: grep app/tests for _last_prune_minute, api_metrics_minute, and the
prune SQL strings you move; test_cron_runner and test_status_page pin phase
lists. Add a test that two phase runs inside one hour issue the DELETEs once.
Target from the plan: at most 168 prune runs a week per table.
```

### S2.2 Skip unchanged public snapshot rewrites

**Priority P1 · agent.** `public_api_snapshots` (5 rows) was rewritten about
14.6k times a week. `_publish_public_snapshot_group` rebuilds and upserts on
every cron slot (status and mirrors every 3 minutes, catalog and network
every 5) and on every republish doorbell, without comparing to the stored
row.

```text
Step S2.2 of the D1 cleanup plan: stop rewriting public_api_snapshots rows
whose content did not change.

Read _publish_public_snapshot_group, PUBLIC_SNAPSHOT_UPSERT_SQL, the cron
phases snapshot-status / snapshot-mirrors / snapshot-catalog /
snapshot-network in _run_cron_phase, refresh_public_api_snapshots and
_republish_publish_path in app/src/entry.py, plus app/src/public_snapshots.py.

Change the publish path to skip the upsert when all of these hold:
- the new payload's digest equals the stored row's digest (store one if the
  row lacks it; reuse an existing fingerprint column if there is one),
- the stored row carries the current deploy fingerprint (deploy.sh
  verify_public_snapshot_routes waits for rows under the new deploy, so the
  first publish after a deploy must always write), and
- the stored `fresh_until` is further away than one publish period, so the
  readers never see a row expire between slots.
Keep reads served exactly as today. Do not add a D1 read per slot beyond one
indexed read of the rows being published (a batch read of the 5 rows is fine).

Tests: grep app/tests for PUBLIC_SNAPSHOT_UPSERT_SQL and
_publish_public_snapshot_group. Add tests: unchanged payload and fresh row
means no upsert; changed payload, a new deploy fingerprint, or a row near
expiry each means an upsert. Report the expected weekly write count.
```

### S2.3 Remaining direct catalog scans

**Priority P2 · agent.** The shared snapshot covers most readers. Two paths
still scan `repositories WHERE is_private = 0` directly.

```text
Step S2.3 of the D1 cleanup plan: check the remaining direct scans of the
public catalog against the S0.3 baseline, and fix the ones that matter.

1. _mirroring_owners in app/src/entry.py runs `SELECT data FROM repositories
   WHERE is_private = 0` with no memo (callers: hydrate_repo_group_live_hosts
   and one more). If it only needs owners, read them from
   _decrypted_public_catalog (its rows carry owner fields), which already
   answers from the per-isolate memo, the catalog_rev check, and the shared
   public_catalog_snapshot row.
2. catalog_handler in app/src/catalog_routes.py scans for the anonymous and
   the signed-in listing. A previous pass left the anonymous edge-miss scan
   alone on purpose: volatile catalog fields (CATALOG_VOLATILE_FIELDS, for
   example liveHost/cloneOnline) do not bump catalog_rev, so a rev-keyed copy
   would serve them up to 10 minutes stale. Only change it if the S0.3
   baseline shows it among the top readers, and then keep the volatile fields
   fresh (for example by overlaying them from the rows that change them).
   Otherwise, record "measured, left as is" in the plan with the numbers.
Do not put the weak ETag on the public cached response.

Tests: grep app/tests for _mirroring_owners, catalog_handler and the scan
SQL. Report rows read per call before and after.
```

### S2.4 `online_hourly_nodes` reads

**Priority P2 · agent.** `network_leaderboards` and `online_history` now
share a 5-minute per-isolate memo. There is still no covering index, and
`install_source` runs its own unmemoized 48-hour `SUM … GROUP BY node_key`.

```text
Step S2.4 of the D1 cleanup plan: cut online_hourly_nodes rows read.

Read _online_hourly_node_rows, network_leaderboards, online_history and
install_source in app/src/entry.py, and the table's DDL in app/src/schema.py
(PRIMARY KEY (hour_ts, node_key) plus idx_online_hourly_nodes_key).

1. For each query, run EXPLAIN QUERY PLAN on a scratch SQLite built by the
   S0.2 replay plus 48 hours x 170 nodes of synthetic rows, and record the
   plan.
2. install_source: serve it from _online_hourly_node_rows (the shared memo)
   when the window fits, instead of its own query.
3. If a query still scans more than the rows it returns, add a covering
   index on the columns it filters and reads (a migration plus the same
   CREATE INDEX IF NOT EXISTS in schema.py, with the stamp rules). Show the
   improved plan.
Do not precompute hourly unless the numbers after 2 and 3 still put it in the
S0.3 top 15.

Tests: grep app/tests for the four function names. Report the plans before and
after.
```

### S2.5 Measure and split `users.data`

**Priority P2 · agent.** `users.data` averages 8.4 KB per row and is decrypted
by `SELECT data FROM users WHERE user_bi=?`, the most-run statement in the
database. The first half of this step is measurement only.

```text
Step S2.5 of the D1 cleanup plan, part 1 (measure only, no production
changes): find out what makes users.data 8.4 KB.

Work on a local copy of the S0.1 export. Decrypting needs DATA_KEY; do it
only on the deploy box, in a scratch script outside the repo, and never print
or save decrypted values. Report sizes only. For each users row, decrypt the
blob with the same scheme as encrypt_row/decrypt_row in app/src/entry.py
(AES-256-GCM, key = SHA-256(DATA_KEY), base64(iv12 || ciphertext)), then sum
len(json.dumps(value)) per top-level key across all rows. Report the top
keys by total bytes and the share of the blob.

Then, in code (app/src/entry.py, account_users.py, patreon_handler.py,
microjobs_runtime.py and any other `SELECT data FROM users` site), list which
top-level keys each read path actually uses. Name the hot paths (session
auth, profile page) and whether they touch the large keys.

Write a proposal into step S2.5 in the plan: which keys move to a new sealed
table keyed by user_bi (for example contribution snapshot, wallet history),
the table DDL, the read paths that need the moved keys, and the backfill plan
(dual read: new table first, then the blob; cron backfill; removal from the
blob last). Do not implement part 2 until the owner approves the proposal.
```

---

## Phase 3: retention and growth

### S3.1 Audit only agent status transitions

**Priority P1 · agent.** 9,907 of 23,625 `sensitive_audit_log` rows are
`organization.task_agent_status`, written on every status write, including
ones that change nothing.

```text
Step S3.1 of the D1 cleanup plan: audit agent status only when it changes.

In world/src/world_office_tasks.py, _apply_agent_status updates
organization_tasks and then calls runtime.audit(actor,
"organization.task_agent_status", ...) on every accepted write. It is called
from _update_agent_status and the batch path.

Compare the incoming status with the task's current agent status (the
`current` record the function already has, current["agent"]["status"] or
its equivalent). If the status is unchanged, still apply the other fields the
write carries, but skip runtime.audit. If nothing at all changed, also skip
the UPDATE, as long as no caller relies on its side effects (check updated_at
consumers and the task events feed first; if any do, keep the UPDATE).
A transition, including the first status on a task, is always audited.

Tests: grep app/tests for task_agent_status and _apply_agent_status. Add
tests: repeated identical status writes produce one audit row; a changed
status produces a second one; the batch path follows the same rule.
```

### S3.2 Prune the rate-limit tables

**Priority P2 · agent.** `signup_rate` and `login_attempts` grow without a
time-based prune. `feedback_email_sends` is **excluded**: its rows are the
"already sent" record (see corrections).

```text
Step S3.2 of the D1 cleanup plan: prune expired rows from signup_rate and
login_attempts in cron. Do NOT touch feedback_email_sends: _send_feedback_emails
treats a missing row as "not sent yet", so pruning it re-sends mail to
every older account.

- signup_rate: written by signup_rate_check in app/src/entry.py, window
  SIGNUP_RATE_WINDOW_MS. Delete rows whose window started more than one
  full window ago, using the table's own window-start column.
- login_attempts: failures are upserted in the sign-in path; rows are deleted
  today only per identifier on success, reset or account deletion. Delete
  rows whose lockout has expired (locked_until < now) and whose first
  failure is older than the failure-counting window (find the constant the
  lockout logic uses, next to LOGIN_LOCKOUT_MS). A row that is still locked,
  or still counting failures inside the window, must survive.

Run both deletes from the hourly retention phase added in S2.1 if it exists,
otherwise from the existing sensitive-cleanup phase. Check that each DELETE's
WHERE columns are indexed; if not, add an index (migration plus schema.py,
with the stamp rules).

Tests: grep app/tests for signup_rate and login_attempts. Add tests showing a
locked row and an in-window row survive while expired rows go, and that the
signup limiter still counts correctly across a prune.
```

### S3.3 Tiered audit retention

**Priority P3 · agent · needs decision D2.** Deleting audit rows cannot be
undone. Do not start until the owner has approved the action list.

```text
Step S3.3 of the D1 cleanup plan: tiered retention for sensitive_audit_log.
First check that decision D2 in the plan is recorded with an approved list of
operational actions and their retention; if not, stop.

cleanup_sensitive_audit_records in app/src/entry.py deletes
sensitive_audit_log rows older than SECURITY_AUDIT_RETAIN_MS (365 days) for
every action. Keep 365 days for everything not on the approved list. For the
approved operational actions (proposed: organization.task_*,
repository.actions_status, ssh.git_authorize), delete after the approved
shorter window (proposed: 90 days). Use one DELETE per tier with an explicit
action list or prefix match that an index can serve; check EXPLAIN QUERY PLAN
and add an index on (action, ts) if needed. Put the list in one named
constant next to SECURITY_AUDIT_RETAIN_MS, with a comment pointing to D2.

Take a fresh export (S0.1) before the first run. Tests: security actions at
200 days survive; approved operational actions at 100 days go; an action not
on the list at 100 days survives.
```

### S3.4 `release_downloads` roll-up

**Priority P3 · agent · needs decision D3.**

```text
Step S3.4 of the D1 cleanup plan: roll release_downloads up into daily
counts. First check that decision D3 (roll-up age N and whether raw `ua` is
kept for recent rows) is recorded; if not, stop.

record_release_download in app/src/entry.py inserts one row per download;
release_download_counts and release_downloads_handler read them. Add a daily
counts table keyed by (repo_bi, sha256, day), a cron phase that folds rows
older than N days into it and deletes them, and make both readers add the
rolled-up counts to the raw rows so totals never change. Test that the total
per release is identical before and after a roll-up run.
```

---

## Phase 4: encryption hardening

### S4.1 Versioned ciphertext format

**Priority P2 · agent.** No ciphertext names the key that sealed it, so
`DATA_KEY` cannot be rotated. This step only adds the format. Nothing is
rotated yet.

```text
Step S4.1 of the D1 cleanup plan: give sealed rows a key identifier, without
changing any key.

Today, in app/src/entry.py: _data_key = SHA-256(DATA_KEY) as an AES-GCM key;
encrypt_row returns base64(iv12 || ciphertext+tag) of json.dumps(obj);
decrypt_row(env, stored, key=None) returns None on any failure.

1. New format for encrypt_row output: `k1.<8 hex key id>:<base64>`, where the
   key id is the first 8 hex characters of HMAC-SHA256(raw key material,
   "forkmesh-data-key-id-v1"). ':' and '.' are outside the base64 alphabet,
   so unprefixed legacy values stay unambiguous.
2. decrypt_row: when the value has the prefix, pick the key whose id matches
   (only DATA_KEY for now; S4.2 adds DATA_KEY_PREVIOUS) and decrypt; when it
   has no prefix, decrypt with the current key as today. Keep returning None
   on failure. Keep the explicit `key=` argument working for both forms.
3. Find every other place that seals or opens row blobs outside
   encrypt_row/decrypt_row (grep for AES-GCM, _data_key, and the plugin
   runtime's seal/open helpers used by marketplace/packages/*/content/worker)
   and route them through the same format, or list why one cannot be.
4. Check column constraints and readers that assume `data` is pure base64
   (length checks, `substr(data,1,1)` probes, public_catalog_snapshot size
   limits) and adjust.

Also add the deposit_secret guard test: world_element_store_handler must
write only encrypt_row output to world_element_purchases.deposit_secret (the
column is already sealed; this keeps it that way), and
_sweep_element_deposit must still clear it on success.

Tests: grep app/tests for encrypt_row and decrypt_row. Add tests: new rows
carry the prefix; legacy rows still decrypt; a prefixed row with an unknown
key id returns None and never raises; the blind index output is unchanged.
```

### S4.2 Pin derived keys, then rotate `DATA_KEY`

**Priority P2 · agent + operator · needs S4.1 deployed and decision D7.**
`DATA_KEY` also derives keys that cannot change without breaking things
already stored or handed out:
- the blind-index key: several `*_bi` columns index values that are never
  stored, such as `users.ip_bi` and `org_bot_tokens.secret_bi`;
- room-chat passphrases, and the private-chat and direct-message passphrase
  keys, which clients hold and chat history is sealed under;
- the admin CSRF HMAC.

```text
Step S4.2 of the D1 cleanup plan, part 1 (code): separate the derivation
root from the row-encryption key, so rotating DATA_KEY only changes the AES
row key. Check that decision D7 (two new Worker secrets) is recorded, and
count current Worker variables first: the Workers Free limit is 64
secrets + text vars, and wrangler.toml [vars], deploy.sh's --var stamps and
every secret all count.

1. Add `_key_derivation_root(env)`: returns env.DATA_KEY_DERIVATION_ROOT if
   set, else DATA_KEY. Every derivation other than the AES row key uses it:
   _hmac_key (blind index), the room-chat passphrase derivation, the private
   and direct chat passphrase keys, the admin CSRF HMAC, and any other
   `DATA_KEY +` or `hmac(DATA_KEY` use (grep for DATA_KEY across app/src,
   world/src and marketplace). Unset, output is byte-identical to today:
   test that.
2. decrypt_row: also try env.DATA_KEY_PREVIOUS when the key id matches it,
   or, for unprefixed legacy rows, when the current key fails.
3. Add both names to the secret allowlist in push_secrets in app/deploy.sh
   (otherwise deploy skips them), and to _require_data_secret-style
   placeholder checks.
4. Add a cron phase `data-key-reseal` that, only while DATA_KEY_PREVIOUS is
   set, re-seals rows table by table under the current key: bounded batch
   per run, progress in schema_meta (k = 'reseal:<table>:<last rowid>'),
   never writes a row it could not decrypt (count those and report them),
   and marks a table done when a pass finds no row sealed under the old key
   id. The list of sealed tables comes from schema.py (tables with a `data`
   column), minus the six plaintext-JSON tables (see S4.3).
5. Document the operator runbook in this step: set
   DATA_KEY_DERIVATION_ROOT = current DATA_KEY and deploy; confirm blind
   indexes and chat still work; then set DATA_KEY_PREVIOUS = old key and
   DATA_KEY = new key and deploy; wait for the reseal phase to report every
   table done; keep the old key with the D1 backups made before rotation
   (those exports stay sealed under it); only then remove DATA_KEY_PREVIOUS.

Tests: identical blind indexes and chat keys with and without the root
secret; rows sealed under the previous key decrypt and get re-sealed;
undecryptable rows are left untouched.
```

### S4.3 Label the plaintext `data` columns

**Priority P3 · agent · needs decision D6.** Six tables keep plaintext JSON
in a column named `data`: `cron_tick`, `repo_alert_settings`,
`system_status_minute_batch`, `system_status_hourly_batch`,
`system_status_daily_batch`, and `world_satellite_snapshot`.

```text
Step S4.3 of the D1 cleanup plan: make "a column named data is ciphertext"
true or explicitly documented. Check decision D6 (comment only, or rename to
payload_json).

Comment only: in app/src/schema.py add a one-line comment on each of the six
tables saying data is plaintext JSON on purpose and why (operational,
public or config). Add a test that lists every schema.py table with a `data`
column and requires it to be either in that six-table plaintext allowlist or
written only through encrypt_row (a source check of INSERT/UPDATE sites is
enough).

Rename: ALTER TABLE ... RENAME COLUMN data TO payload_json in a migration, the
same change in schema.py (stamp rules apply), and every SQL string that reads
or writes those columns. This is a single deploy only if the migration runs
before the new code serves traffic; confirm migrate.sh runs before the Worker
upload in deploy.sh, otherwise split it into two deploys that read both
names.
```

---

## Phase 5: legacy custody (gated on owner sign-off)

### S5.1 Retire legacy custody code

**Priority P3 · agent · needs decision D5.** Also removes the second
`schema_meta` read on every cold isolate.

```text
Step S5.1 of the D1 cleanup plan: remove the legacy custody guard and the
code that still uses the custody tables. First check that decision D5 is
recorded (custody record signed off, and whether bounty and funds-received
features are retired or kept), and that S0.1 has exported
legacy_custody_migration_audit, legacy_custody_reconciliation and
funds_received rows next to the existing custody audit JSON in
deploy-backups; if not, stop.

1. In app/src/entry.py remove LEGACY_WALLET_CUSTODY_MARKER,
   LEGACY_WALLET_CUSTODY_TABLES, _assert_legacy_wallet_custody_ready and both
   of its calls in _apply_schema. Update test_legacy_solana_custody and the
   test_status_page assertion that pins the call.
   Update www/docs/operations/legacy-solana-custody-migration.md: startup no
   longer fails closed; the offline tool stays documented for old exports.
2. If D5 retires them, remove the live code paths:
   - issue_bounty: the bounty read, write and delete code in entry.py and the
     reads and labels in app/src/ledger.py;
   - funds_received: _record_funds_received, _funds_received_boards, the
     DELETE in app/src/catalog_routes.py;
   - central_fund: the _account_central_fund route in app/src/app_runtime.py;
   - bounty_wallet and federated_signup: any remaining references.
   If D5 keeps a feature, leave that table out of S5.2 and say so in the plan.
3. Remove the DDL of every table being dropped from app/src/schema.py (stamp
   rules), their entries in admin_console.py's blocked list and in
   admin_database.py, and bounty_wallet / issue_bounty from the S0.2
   replay-test allowlist.
Report the ensure_schema fast path before and after: it should be one
schema_meta SELECT per cold isolate.
```

### S5.2 Drop the legacy custody tables

**Priority P3 · agent · after S5.1 is deployed.**

```text
Step S5.2 of the D1 cleanup plan: drop the legacy custody tables. Confirm
that S5.1 is deployed, a fresh S0.1 export exists, and grep finds no
remaining SQL reference to each table. Then add the next-numbered migration
with DROP TABLE IF EXISTS for those that D5 retired, from: bounty_wallet,
central_fund, issue_bounty, federated_signup, legacy_custody_migration_audit,
legacy_custody_reconciliation, funds_received. End with the fingerprint stamp;
the down migration recreates the empty shapes from the last DDL in git
history.
```

---

## Phase 6: dormant features

### S6.1 Dormant feature inventory

**Priority P3 · agent · produces the input for decision D4.** About 70
zero-row tables belong to shipped features that nobody uses in production
yet. Plugin tables (`crm_*`, `marketing_*`) belong to the plugin lifecycle and
are out of scope.

```text
Step S6.1 of the D1 cleanup plan: build the per-feature inventory the owner
needs to decide keep or remove. Read-only; do not remove anything.

For each feature group in section 3.3 of
docs/operations/database-audit-2026-09-10.md (ActivityPub outbound,
attribution, community ads, contributor invitations, org succession, Patreon,
marketplace governance, repository security scans, reward pool, World
media/workshops/events/fediverse mentions, and the misc list), report:
the tables and their live row count (from the S0.3 baseline or a read-only
count), the routes and cron phases that touch them, the user-facing entry
points (web pages, desktop sections, mobile screens), the docs page, any
Worker secret it needs, and a rough removal size (files and lines). Add it as
a table under decision D4 in the plan, one row per feature, with an empty
decision column.

Also fix app/src/admin_database.py's plugin-table list: add the CRM tables
that marketplace/packages/crm/content/worker/crm.py creates but the list
omits (crm_views, crm_view_shares, crm_outreach_team, crm_outreach_log,
crm_deck_links, crm_deck_sessions, crm_deck_page_views) and check the
marketing package the same way.
```

For each feature the owner marks "remove", create a new step that follows the
Phase 1 pattern: one step removes the code and the DDL, and a later step,
merged only after that one is deployed, drops the tables.

---

## Closing

### S7.1 Re-run the audit and close the issue

```text
Step S7.1 of the D1 cleanup plan: re-run the D1 audit and compare.

Repeat the audit's reproduction commands (section 6 of
docs/operations/database-audit-2026-09-10.md) and the S0.3 measurement.
Produce docs/operations/database-audit-<date>.md with the same sections,
plus a before/after table for: table count, rows, size, weekly rows read and
written, the top 15 statements, prune runs per week, and each acceptance item
below with pass/fail and evidence. Attach it to issue #682 as a follow-up
comment. Do not close the issue while any acceptance item fails without a
recorded decision.
```

Acceptance for closing #682:

- A clean migration replay builds every table the Worker uses (S0.2 test).
- No table without a reader or writer remains, except those with a recorded
  keep decision.
- Weekly D1 rows read are below the target set in S0.3, and retention prunes
  run at most hourly.
- No plaintext secret column remains, and the ciphertext format carries a key
  identifier (S4.1).
- The re-audit is attached to the issue with before and after numbers.

## Owner decisions

Record each decision here, with date and who decided, before starting a step
that depends on it.

| # | Question | Blocks | Proposal | Decision |
|---|---|---|---|---|
| D1 | Drop `telemetry` after export? Keep the export file or delete it after the drop? | S1.4 | Drop the table; delete the export after 30 days | — |
| D2 | Which audit actions get a shorter retention, and how long? | S3.3 | `organization.task_*`, `repository.actions_status`, `ssh.git_authorize` at 90 days; everything else 365 | — |
| D3 | Roll `release_downloads` up to daily counts after how many days? Keep raw `ua` for recent rows? | S3.4 | 30 days; keep `ua` only on unrolled rows | — |
| D4 | Keep or remove each dormant feature | S6.1 follow-ups | Decide per row of the S6.1 inventory | — |
| D5 | Is the legacy custody record signed off? Retire the issue-bounty, funds-received and central-fund paths? | S5.1, S5.2 | Sign off; retire all three | — |
| D6 | Plaintext `data` columns: comment only, or rename to `payload_json`? | S4.3 | Comment plus test | — |
| D7 | Spend two Worker secret slots on `DATA_KEY_DERIVATION_ROOT` and `DATA_KEY_PREVIOUS`? | S4.2 | Yes; `DATA_KEY_PREVIOUS` is removed after each rotation | — |
| D8 | Keep plaintext `release_downloads.ua` and `feedback.message`? (`feedback.user_agent` is only a broad client category and `ip_hash` is a blind index.) | — | Keep; both are shown to admins only | — |

## Measurement

Baselines and after-numbers go here. The audit's figures (7 days ending
2026-09-10, before migrations `0179` and `0186`): about 13.5 M rows read a
week; the public catalog scan 2.0 M; `api_metrics_minute` retention 201 runs
and 38.5k rows written; `public_api_snapshots` 14.6k writes; `host_presence`
9.9k queries; `system_status_minute` 26.5k queries.
