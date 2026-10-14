# Relay data retention and deletion

What the hosted relay behind `forkmesh.com`, `app.forkmesh.com` and
`world.forkmesh.com` stores, how long, and what deletes it. Git objects in
repositories published from desktops and mirror nodes are not relay data and
the relay cannot delete them. "Until deleted" means no automatic limit;
**missing proof** marks a gap.

## Where the relay runs

The application code (`app/src/`) runs under `app/host/` on self-hosted core
nodes (`deploy/fleet.toml`). `app/wrangler.toml` still declares the bindings;
`app/host/forkmesh_host/env.py` maps each to a local implementation. The apex
site is fronted by an edge proxy that forwards visitor IP and country in a
signed header.

## Live stores

| Store | Holds | Retention / deletion |
| --- | --- | --- |
| rqlite database (`DB`) | Accounts, catalog, inboxes, chat backlog, status, analytics, memberships, package data, durable-object storage; full copy on every core node | Per table below |
| KV `REPOSITORY_METADATA` | Cached repository-page JSON per node, not replicated or backed up | No expiry, no delete (**missing proof**) |
| `ForkMeshRoom`, `ForkMeshNoteRoom`, `ForkMeshWorld`, `ForkMeshOfficeRoom` | Sockets and live presence in memory | Socket lifetime |
| `ForkMeshNodes`, `ForkMeshCronWatchdog`, `ForkMeshCronRunner`, `ForkMeshDiscordGate` | Push sockets, commit targets, cron progress, rate buckets (`do_storage`, `do_alarms`) | Overwritten or expired |
| Queue `forkmesh-notification-email` | Wake-up naming a recipient, in memory | Until delivered (3 retries) |
| Response cache | Public responses, ≤ 8 MiB per node | `Cache-Control` lifetime |
| Hosted Git repositories | Full copy per node | Removal moves to `.trash`; emptied by hand only |

## Database tables (rqlite)

| Data family | Retention | Deletion |
| --- | --- | --- |
| Accounts, devices, SSH keys, encryption keys | Until deleted; stale devices 60 days | Account delete |
| Sessions | Expired and revoked sessions pruned on login | Sign out; account delete |
| Repository catalog, health, inboxes, webhooks, provider mirrors | Until deleted; outboxes until delivered or retries exhausted | Repository or account delete |
| Chat backlog (encrypted) | 7 days, ≤ 500 messages and 16 MiB per room; DMs removed on recipient ack | Sweep; message, room or account delete |
| Notifications | 90 days, ≤ 500 per recipient | Account delete |
| Email delivery records | 90 days | Expiry |
| Security audit log | 365 days | Expiry |
| Error and diagnostic logs | Newest 5,000 rows each | Row cap |
| Site analytics | 400 days; IP digest 24 hours | Privacy-page delete; daily sweep |
| Status history, cron ticks, presence, throttles | Hours to 30 days | Expiry |
| Account-scoped packages | Until deleted | Account delete runs each package's `purge_account_data` |
| Organization-scoped packages | Until deleted | Uninstall or organization delete (`app/src/plugin_data.py`) |
| Notes, calendar, app installs, reviews, push tokens, suppressions, IDS records, bug reports, ledger rows | Until deleted | **Missing proof:** account delete leaves them |

Account deletion (`_delete_account_namespace` in `app/src/entry.py`) removes the
account's repositories and repository-scoped rows, identity and session rows,
notifications, follows, referrals, stars, grants, ads, package rows and direct
conversations, then republishes the public catalog.

## Backups and copies

| Copy | Retention |
| --- | --- |
| Nightly encrypted rqlite backups (`tools/next_backup.py`) | Last 7 daily and 4 weekly; a restore resurrects deleted rows unless deletions are replayed |
| rqlite Raft log and snapshots | rqlite truncation defaults |
| Archives from the previous hosting stack | **Missing proof:** no retention or deletion step |
| Hosted repository `.trash` | Until emptied by hand |

## Logs

| Log | Retention |
| --- | --- |
| Core node journal (errors, request counts, statement text without values, mail delivery lines) | ≤ 64 MiB per node (`journald.conf`), rotated by size |
| Reverse-proxy access log (masked client addresses, no headers) | In the journal |
| Mail event spool | 3 days |
| Edge proxy logs | Provider default |

## Outside processors

The hosting provider (all stores), DNS and edge provider, recipients' mail
servers, optional error reporting (redacted), membership provider, AI providers
used by runner jobs, webhook receivers, food-data lookups (no account id), push
notification service (token and fixed alert only), chat bridge, ActivityPub
peers, and opt-in product analytics. Each keeps data under its own policy.
Details: [external dependency map](external-dependency-map.md).

## Known gaps

1. No user data export API.
2. Account deletion leaves the rows marked **missing proof** above.
3. `REPOSITORY_METADATA` entries never expire.
4. No step replays deletions after a backup restore.
5. Archives from the previous stack have no retention policy.
6. `.trash` is emptied only by hand.

Update this page with any change to bindings, `env.py`, backup retention, a
retention sweep, or `_delete_account_namespace`.
