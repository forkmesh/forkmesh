# Relay data retention and deletion

Checked against main on **2026-09-16** and **2026-09-23** (Cloudflare stack),
and again on **2026-09-27** after the move to self-hosted core nodes (issue
#709).

This page covers the hosted relay behind `forkmesh.com`, `app.forkmesh.com`, and
`world.forkmesh.com`. For each store, backup, and outside processor it says what
the data is, how long it lasts, and what deletes it. Git objects and refs in
repositories published from desktops and mirror nodes are not relay data. The
relay cannot delete them (see [Copies the relay cannot
reach](#copies-the-relay-cannot-reach)).

Every row is **repository evidence** from the named source unless it is marked
**platform default** (a vendor or software default this repository does not
configure) or **missing proof**. "Until deleted" means no automatic limit
exists.

## Where the relay runs

On 2026-09-24 the relay moved off Cloudflare Workers. The same application code
(`app/src/`) now runs under `app/host/` on three core nodes, `fmnext-n1` to
`fmnext-n3`, rented from Vultr in the `ewr` (New Jersey, United States) region
(`deploy/fleet.toml`). The Cloudflare D1 database was deleted on 2026-09-27.
The `forkmesh-app` and `forkmesh-world` Workers were deleted the same day,
together with their Durable Object namespaces, the KV namespace, and the queue
([cutover record](../../docs/CUTOVER.md)).

- `app.forkmesh.com` and `world.forkmesh.com` resolve straight to the core
  nodes. Their DNS records at Cloudflare are not proxied.
- `forkmesh.com` and `www.forkmesh.com` are still served by the Cloudflare
  Worker `forkmesh-www`. It fetches pages from the core nodes and forwards the
  visitor's IP address and country in a signed header.

## Live stores

The application still declares its bindings in `app/wrangler.toml`. On the core
nodes `app/host/forkmesh_host/env.py` maps each binding to a local
implementation.

| Store | Replaces | What it holds | Retention | Deletion path |
| --- | --- | --- | --- | --- |
| rqlite database (`DB`) | D1 `forkmesh` | Accounts, repository catalog, collaboration inboxes, chat backlog, health, status, analytics, memberships, package data, and Durable Object storage. See [Database tables](#database-tables-rqlite) | Per table below | Per table below. Every core node holds a full replicated copy in `/var/lib/forkmesh-next/rqlite` |
| KV `REPOSITORY_METADATA` | Cloudflare KV | Cached repository-page JSON, in a separate SQLite file on each core node (`kv-repository_metadata.sqlite`). Not replicated and not backed up | Until overwritten. Writes set no expiry (`app/src/entry.py`) | No delete call exists. **Missing proof:** deleting an account or repository does not remove its cached page JSON |
| KV `admission-override:*` keys | Cloudflare KV | Blind-indexed repository id plus a time window, on the core node that handled the admin request | 30 minutes by default, 6 hours at most | Unreadable once expired; the row itself is removed only when it is read again |
| Durable Objects `ForkMeshRoom`, `ForkMeshNoteRoom`, `ForkMeshWorld`, `ForkMeshOfficeRoom` | Durable Objects | Sockets and live presence, in memory on the core node that owns each object. Kept chat messages go to `chat_history` | Socket lifetime | Socket close |
| Durable Objects `ForkMeshNodes`, `ForkMeshCronWatchdog`, `ForkMeshCronRunner`, `ForkMeshDiscordGate` | Durable Objects | Node push sockets, the latest commit target per repository, cron progress, and Discord rate buckets. Their storage is the `do_storage` and `do_alarms` tables, so it is replicated and backed up | Overwritten on each push or tick; rate buckets expire | Overwritten or expired |
| Queue `forkmesh-notification-email` | Cloudflare Queues | A wake-up naming the recipient, held in the memory of one app process | Until delivered; at most 3 retries; lost on restart, when the per-minute cron picks the work up | Consumed |
| Response cache | Cloudflare edge cache | Public pages and API responses, at most 8 MiB in memory per core node | Their `Cache-Control` lifetimes | Account delete purges catalog entries on the node that ran it only; the others expire |
| Hosted Git repositories | New | Repositories created on the website, a full copy on every core node under `/var/lib/forkmesh/hosted/` ([hosted repositories](../../docs/HOSTED_REPOS.md)) | Until removed | Removal moves each copy to `.trash`. **Missing proof:** nothing ever deletes `.trash`; an operator empties it by hand |
| AI binding (`AI`) | Workers AI | Nothing. No AI provider is configured on the core nodes, so ForkBot and username moderation run without one | None | None needed |

No object store, analytics dataset, or vector index is configured.

## Database tables (rqlite)

Migrations are in `app/migrations/`; `app/src/schema.py` adds a few runtime
tables. Rows marked "account delete" are removed by the account deletion
described [below](#what-account-deletion-removes). Every deletion below also
stays in the [backups](#backups-and-copies) until they age out.

| Data family | Representative tables | Retention | Deletion path |
| --- | --- | --- | --- |
| Accounts | `users`, `nodes`, `role_grants`, `owner_encryption_keys` | Until deleted | Account delete, by the user or an admin |
| Sessions | `account_sessions` | 5 years and 1 day; expired sessions, and revoked ones after 24 hours, are removed on the next login | Account delete; sign out; Settings revokes every device |
| Devices and keys | `account_devices`, `account_ssh_keys` | Stale devices 60 days | Account delete; removing the device or key |
| Push tokens | `apns_device_tokens` | 180 days without use | Token unregister. **Missing proof:** account delete leaves them |
| Sign-in throttles | `login_attempts`, `signup_rate`, `password_reset_rate`, `ai_ask_rate` | One rolling window each (15 minutes to 24 hours) | Overwritten when the window rolls; account delete removes `login_attempts` |
| Repository catalog and health | `repositories`, `repo_shares`, `repo_forks`, `repo_stars`, `repo_device_mirrors`, `mirror_repository_integrity`, `repo_security_scans` | Until deleted | Repository delete or account delete |
| Repository deletion tombstones | `repo_deletions` | 90 days | Expires on its own |
| Hosted repository records | `hosted_repos`, `hosted_repo_copies` | Marked deleted on removal, then dropped from replica exchange after 30 days | **Missing proof:** rows are never deleted, and account delete leaves them |
| Collaboration inboxes | `issue_inbox`, `pull_inbox`, `discussion_inbox` | Until drained or deleted | Repository delete or account delete. Records already committed to Git travel with the repository |
| Chat backlog | `chat_history` (encrypted envelopes) | 7 days, at most 500 messages and 16 MiB per room. A direct message is deleted as soon as the other person's device acknowledges it | Receipt from the recipient (direct messages); hourly sweep; message delete; room delete; account delete removes DMs and repository rooms |
| Notes and calendar | `notes`, `note_versions`, `calendar_events` | Until deleted | Per-item delete. **Missing proof:** account delete leaves them |
| World floor notes | `world_floor_notes`, `world_floor_note_replies` | Until deleted | Per-item delete; account delete |
| Organizations | `org_*`, `organization_task_*` | Until deleted | Organization delete or member removal |
| Notifications | `notifications` | 90 days, at most 500 per recipient | Account delete |
| Email delivery records | `mail_sends`, `mailtrap_email_sends`, `mailtrap_webhook_events` | 90 days | Expires on its own. Account delete removes `feedback_email_sends` only |
| Email suppressions | `mail_suppressions` (blind-indexed recipient and reason) | Until deleted | **Missing proof:** no delete path exists |
| Federation | `ap_outbox`, `ap_objects`, `ap_followers`, `ap_remote_actors` | Outbox row until delivered, refused, or 8 attempts; objects until deleted | Per-object delete sends a federated delete that peers may ignore |
| Security audit | `sensitive_audit_log` | 365 days | Expires on its own |
| Intrusion detection | `security_incidents`, `cluster_quarantine` ([IDS](../../docs/IDS.md)) | Until deleted | **Missing proof:** no delete path exists |
| Error and diagnostic logs | `error_log`, `install_diag`, `feedback` | Newest 5,000 rows each | Row cap; admin console delete |
| Bug reports | `bug_reports`, `discord_bug_reports` | Until deleted | **Missing proof:** no automatic or account delete found |
| Site analytics | `attribution_sessions`, `attribution_events` | 400 days (`ATTRIBUTION_RETENTION_DAYS`); IP digest 24 hours; `fm_attr` cookie 90 days | Delete button on the [privacy page](../public/privacy.html); daily sweep |
| Flagship read failures | `flagship_read_failure_minute` (a count per minute, no request data) | 24 hours | Expires on its own. `api_metrics_minute` was dropped by migration 0190 |
| Referrals | `referral_attributions`, `referral_click_seen`, `referral_stats` | Click records 35 days, click audit 180 days | Account delete |
| Presence and online counts | `account_presence`, `host_presence`, `online_hourly*`, `world_*` presence | 10 minutes to 30 days | Expires on its own; account delete |
| Status history | `system_status_minute`, `system_status_hourly`, `system_status_daily` | Minute rows 30 hours; hourly and daily 30 days | Expires on its own |
| Cron ticks | `cron_tick`, `host_cron_runs` | 6 hours and 24 hours | Expires on its own |
| Memberships | `patreon_*`, `plan_trials`, `role_grants` | Membership rows until deleted; webhook events 90 days | Account delete removes the Patreon link, trials, and grants. Migration 0189 dropped every `polar_*` table |
| Ledger and rewards | `funds_received`, `wallet_balances`, `reward_*`, `legacy_custody_*` | Until deleted | **Missing proof:** no documented deletion; financial records may need to be kept |
| Repository webhooks | `repo_webhooks` (encrypted URL and signing secret), `repo_webhook_outbox`, `repo_webhook_deliveries` | Outbox row until delivered or 6 attempts; newest 20 deliveries per webhook | Webhook delete; repository delete; account delete |
| GitHub and GitLab mirrors | `repo_provider_mirrors` (encrypted access token, remote name and settings), `repo_provider_mirror_links` (which issue numbers correspond), `repo_provider_mirror_outbox` (encrypted queued items) | Until the owner disconnects the mirror; outbox row until delivered or 6 attempts | Disconnect in repository settings; repository delete; account delete |
| Desktop commands | `desktop_commands` | 24 hours | Expires on its own |
| Runner tier | `runner_nodes`, `runner_job_leases`, `runner_login_sessions`, `runner_login_chunks` | Runners until removed; sign-in terminals deleted shortly after they end | Sweeps; runner removal |
| Third-party apps | `apps`, `app_installs`, `app_tokens`, `app_reviews`, `app_deliveries` | Until deleted | App delete. **Missing proof:** account delete leaves an account's installs, tokens, and reviews |
| Market ratings | `marketplace_reviews`, `marketplace_review_history`, `marketplace_review_reports` | Until deleted | **Missing proof:** account delete leaves them |
| Code challenges | `code_challenges`, `code_challenge_members` | Until deleted | Challenge delete; leaving. **Missing proof:** account delete leaves them |
| Ad network | `ad_network_ads`, `ad_network_reports`, `ad_network_reviews`, `ad_network_daily_stats` | Ads until deleted; click counts 400 days (`STATS_RETENTION_DAYS`) | Ad delete; account delete removes ads, reports, and counts. **Missing proof:** review records stay |
| Account-scoped packages | Calorie Counter `calories_*`, Password Vault `vault_*` (end-to-end encrypted items) | Until deleted | Per-account package uninstall; account delete runs each package's `purge_account_data` |
| Organization-scoped packages | CRM `crm_*`, Marketing `marketing_*`, Competitor `competitor_*` | Until deleted | Organization uninstall offers an export, purges that organization's rows, and drops the tables after the last uninstall; organization delete does the same (`app/src/plugin_data.py`) |
| Public snapshots | `public_catalog_snapshot`, `public_api_snapshots` | Replaced on republish | Account delete schedules a republish |
| Host secrets | `host_secrets` (application secrets encrypted under `DATA_KEY`, [vault](../../docs/VAULT.md)) | Until replaced | Operator vault sync. Not user data |

## What account deletion removes

Deleting an account from profile settings, or through the audited admin route
`POST /api/accounts/admin-delete`, runs `_delete_account_namespace` in
`app/src/entry.py`. The core nodes run the same code as the Cloudflare Worker
did. It removes:

- the account's repositories and every repository-scoped row, including inboxes,
  scans, mirrors, shares, forks, and repository chat rooms;
- `users` and `nodes` rows, sessions, devices, SSH keys, login lockout rows, and
  pending verifications;
- notifications, follows, referrals, contribution profile rows, stars, role
  grants, owner encryption keys, plan trials, the Patreon link, World and
  office participation rows, World floor notes and their replies, and
  organization task check-ins;
- the account's ads with their reports and click counts, and every
  account-scoped package's rows (Calorie Counter, Password Vault);
- channel memberships and direct conversations;
- cached catalog pages on the core node that ran the deletion, followed by a
  public catalog republish.

**Missing proof:** it leaves these rows keyed to the account or its content:
`notes`, `calendar_events`, `apns_device_tokens`, `org_members`,
`hosted_repos`, `app_installs`, `app_tokens`, `app_reviews`,
`marketplace_reviews`, `code_challenges`, `mail_suppressions`,
`mail_sends` and `mailtrap_email_sends` (removed after 90 days anyway),
`sensitive_audit_log` (365 days), `attribution_sessions` (400 days),
`error_log` actor fields (row cap), `ad_network_reviews`, federated `ap_*`
objects, bug reports, KV `REPOSITORY_METADATA` on every core node, and the
response caches of the other core nodes until they expire.

## Backups and copies

| Copy | What it holds | Retention | Deletion path |
| --- | --- | --- | --- |
| Nightly rqlite backups | The whole database, taken from the Raft leader at 03:30 UTC, checked, and encrypted with age on the operator's machine (`tools/next_backup.py`) | The newest backup of each of the last 7 days and of each of the last 4 weeks | Ages out. Deleted rows stay in older backups for up to about 4 weeks, and a restore brings them back, so deletions must be replayed |
| rqlite files on each core node | Raft log, snapshots, and the SQLite write-ahead log | **Platform default:** rqlite's snapshot and log truncation; no secure-delete or vacuum step is configured | Overwritten over time |
| Final D1 export (2026-09-27) | Every row of the deleted Cloudflare database, as SQL and as an age-encrypted copy, on the operator's machine ([cutover record](../../docs/CUTOVER.md)) | **Missing proof:** no retention period. Backup pruning skips it | **Missing proof:** no deletion step. Accounts deleted later remain in it |
| Old-stack archive (2026-09-24) and retired Worker export (2026-09-27) | The D1 export taken at the cutover, Durable Object state, and every `REPOSITORY_METADATA` value, on the operator's machine | Kept as the rollback record | **Missing proof:** no deletion step |
| Manual D1 exports before the move | SQL files and SQLite snapshots used before migrations and audits ([cleanup plan](operations/d1-database-cleanup-plan.md), [legacy custody migration](operations/legacy-solana-custody-migration.md)) | **Missing proof:** no retention period and no storage location are documented | **Missing proof:** no deletion step |
| D1 Time Travel | Point-in-time history of the deleted `forkmesh` database | **Missing proof:** nothing records whether Cloudflare kept any history after the deletion | None from the relay |
| Hosted repository `.trash` | Removed hosted repositories on every core node | Until an operator empties it | By hand only |
| GitHub Actions artifacts | Disaster-recovery drill evidence, no user data | 30 days | Expires on its own |

## Logs

| Log | What it holds | Retention | Deletion path |
| --- | --- | --- | --- |
| Core node journal | App errors with tracebacks, per-route request counts, the text of each database statement without its values, and Postfix delivery lines with recipient addresses ([node logs](../../docs/NODE_LOGS.md)) | At most 64 MiB per node, split into files of up to one week (`deploy/core/templates/journald.conf`); no time limit | Rotated out by size |
| Caddy access log | Path, status, and timing, with client addresses masked to /24 (IPv4) or /48 (IPv6), headers dropped, and credential query parameters replaced | In the core node journal, as above | Rotated out by size |
| Mail event spool | One file per delivery result, with the recipient address | 3 days | Pruned by the mail event agent |
| `forkmesh-www` Worker logs and traces | 5% sample of logs; every request traced and kept | **Platform default:** the Cloudflare plan's log and trace retention | Ages out; no relay delete |

## Outside processors

Details and sources for services outside the core nodes are in the [external
dependency map](external-dependency-map.md), which still describes the relay as
it ran on Cloudflare (see [Known gaps](#known-gaps)).

| Processor | Relay data it receives | Retention | Deletion path |
| --- | --- | --- | --- |
| Vultr | Hosts the core nodes and their disks: every store above, the journal, and hosted repositories. Automatic Vultr backups are off | Until the instance is destroyed | Destroying the instance |
| Cloudflare | DNS for every name; Turnstile checks on signup and password reset; for `forkmesh.com` only, every request (IP, headers, path, timing) through the `forkmesh-www` Worker | Its plan's retention | Logs age out |
| Recipients' mail servers | Account email, sent directly by Postfix on the core nodes. Mailtrap is no longer used for sending | The receiver's | The receiver's operator |
| Sentry (optional) | Redacted route, status, and stack locations; no IP or query strings | Operator's Sentry organization | Sentry project settings |
| Patreon | Email, pledge or membership state, and membership webhooks | Vendor retention | Vendor account; unlinking |
| Polar and Mailtrap (retired) | Nothing new. Their own records of past subscriptions and sent mail remain | Vendor retention | Vendor account |
| AI coding providers used by the runner tier | Repository code and prompts of agent jobs that the account starts on the pool runner | Each provider's; runner session homes are removed 14 days after last use ([runners](../../docs/RUNNERS.md)) | Provider account |
| Repository webhook receivers | Event payloads for the repository, sent to URLs its owner configures | The receiver's | The receiver's operator |
| Open Food Facts, USDA FoodData Central | Barcodes and food search text from Calorie Counter, with no account identifier | Each service's | None needed |
| Apple Push Notification service | Device token and a fixed "New activity" alert with no sender or content | Apple's | Token unregister; pruned on permanent failure |
| Discord | Discord user id, the #bugs channel messages the bot reads and replies to, and bridged organization channel messages | Discord's | Discord |
| ActivityPub peers | Public federated posts | Each peer's | Federated delete, which peers may ignore |
| PostHog (opt-in only) | A coarse page category and fixed action names, never under Do Not Track or Global Privacy Control | Operator's PostHog project | PostHog project settings |

## Copies the relay cannot reach

- **Mirror nodes** hold Git bytes, including `.forkmesh` issue, pull, and
  discussion records. Removing a repository from the catalog does not delete a
  mirror's copy, and a mirror operator's own backups follow
  [node backup and recovery](operations/node-backup-and-recovery.md), whose
  example timer removes archives older than 14 days.
- **Desktops** keep sessions, identities, transcripts, logs, and mirrors
  locally. `www/public/uninstall.sh --purge` deletes them on that machine only
  and touches no relay data.
- **Git history** cannot be recalled once cloned.

## Known gaps

Each gap is **missing proof** that a launch reader can ask about:

1. There is no user data export API.
2. Account deletion leaves the rows listed [above](#what-account-deletion-removes).
3. KV `REPOSITORY_METADATA` entries never expire and are never deleted, on any
   core node.
4. Deleted rows stay in the nightly backups for about 4 weeks, and no step
   replays deletions after a restore.
5. The final D1 export, the old-stack archive, and the retired Worker export
   have no documented retention or deletion.
6. Removed hosted repositories stay in `.trash` on every core node until an
   operator deletes them by hand.
7. The [external dependency map](external-dependency-map.md) still lists D1,
   KV, Durable Objects, Queues, Workers AI, and Mailtrap as live, and has no
   entry for Vultr as the relay's host or for direct mail delivery.

Update this page with any change to `app/wrangler.toml` bindings,
`app/host/forkmesh_host/env.py`, the backup retention in `tools/next_backup.py`,
a new retention sweep, or a change to `_delete_account_namespace`.
