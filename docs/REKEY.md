# Re-keying the Next database off production's DATA_KEY

Next's rqlite database holds an imported copy of production's D1. To read it,
Next ran on production's `DATA_KEY`. The 2026-09-24 audit rated that
Critical: a compromised Next node could decrypt production data and forge
production password-reset and email-verify links (`_password_reset_token`,
`_email_verify_token`, both keyed by the blind-index key derived from
`DATA_KEY`).

The fix is to **re-key the copy**: decrypt everything that was encrypted or
derived under production's key and re-create it under Next's own key
(`config/DATA_KEY.next-original`). Afterwards Next holds no production
secret and no key material derived from one, and users still log in with
their passwords (password hashes are salted PBKDF2 inside the encrypted
account record and do not depend on `DATA_KEY`).

The tool is `tools/next_rekey.py`; its tests are
`app/tests/host/test_next_rekey.py`. It runs only on the operator desktop,
against a local SQLite file, with the fleet's apps stopped.

## 1. Inventory: every use of DATA_KEY

`DATA_KEY` is read through `_require_data_secret(env)` (app/src/entry.py), or
directly as `env.DATA_KEY` in a few HMAC secrets. Each use is one of four
kinds:

- **stored ciphertext**: re-encrypted by the tool;
- **stored blind index**: recomputed by the tool;
- **derived on the fly**: nothing stored, so it changes with the key (an
  outstanding token or link simply stops verifying);
- **client-side E2E**: not keyed by `DATA_KEY`, so not affected.

### 1.1 Stored ciphertext (re-encrypted)

| Derivation | Format | Where stored |
|---|---|---|
| `_data_key` → `encrypt_row` / `decrypt_row` | AES-256-GCM, key `SHA-256(DATA_KEY)`, `base64(iv12 ‖ ct ‖ tag16)` of `json.dumps(obj)` | 95 tables (`CIPHER_COLUMNS`: the `data` column of 92 tables, `ad_network_ads.owner_sealed`, `world_element_purchases.deposit_secret`, `not_found_paths.last_client`). Also any such string nested in a plaintext JSON cell or inside another decrypted row. The tool trial-decrypts **every** text value and JSON leaf (GCM authentication makes a false positive impossible), so it does not depend on the list; the list is for reporting and the coverage test. |
| `_scoped_key` (plugin/package row keys) | AES-GCM under caller-supplied material | The material is stored inside `encrypt_row` rows, so it is re-encrypted with them. Rows sealed under the scoped key itself do not depend on `DATA_KEY` and are left untouched. |
| `_room_chat_passphrase` (repository rooms, `world-general`), `_chat_channel_passphrase`, `_chat_direct_passphrase` | `SHA-256(DATA_KEY + suffix)` hex, used by clients as a PBKDF2-SHA256 passphrase (210,000 rounds, salt `SHA-256("ForkMesh room:" + room)[:16]`) → AES-GCM envelope `{kind:"cipher",v:1,nonce,tag,body}` | `chat_history.body`. The tool derives the old and new room key from `room_key` (`repo:<o>/<r>:room:<room>`, `legacy:room:<room>`, `chat-channel:<id>:v<n>`, `chat-direct:<id>:v<n>`), re-seals each envelope with a fresh nonce, and drops the rare row that opens under no candidate. An inner `chat-seal` E2EE layer, when present, is carried unchanged. |
| Vault: `HKDF-SHA256(DATA_KEY, "fmnext-vault-v1", ...)` (docs/VAULT.md) | AES-256-GCM per row | `host_secrets`. The tool **empties** it; `next_vault.py sync` re-seals every variable under the new key. |

### 1.2 Stored blind indexes (recomputed)

`blind_index(env, value)` = `HMAC-SHA256(SHA-256(DATA_KEY + ":blind-index"),
lower(strip(value)))` hex. It is stored in:

- 382 `*_bi` columns in 240 tables (`BI_COLUMNS`, table in section 5);
- 7 token columns not named `*_bi` (`TOKEN_COLUMNS`): `feedback.ip_hash`,
  `attribution_ip_hourly.ip_hash`, `note_views.viewer_key`,
  `blog_post_view_seen.viewer_key`, `repo_views.viewer_key`,
  `sensitive_audit_log.target_token`, `world_manual_blocks.subject_token`;
- JSON keys ending in `_bi` / `Bi` inside decrypted rows;
- 64-hex substrings of any other cell or JSON value (for example an
  `account_bi` quoted in `error_log.message`).

**How plaintext is recovered.** In a first pass the tool decrypts every row
in memory and collects every string and integer it sees (plaintext columns,
decrypted JSON values and keys), owner/repository pairs (from records with
owner- and repository-like keys, from row columns such as
`org_repos.node_owner`/`repo`, and from `a/b` path segments), and a few
row-local facts. It then computes the **old** blind index of every candidate
plaintext, with every family prefix the app uses (`org:`, `apns:`,
`ssh-public-key:`, `ap-actor:<kind>:`, `repo-alert-settings:`, `ap-context:`,
thread keys `thread:<o>/<r>:<source>:<n>`, bounty keys `<o>/<r>#[pr-]<n>`,
`audit:<type>:`, ... see `BI_PREFIXES`), and keeps it when that value occurs
in the database. Families that embed another blind index are resolved in a
second round: `password-change-v1\n<account_bi>`,
`password-reset-account-v1\n<name_bi>`, `login-source:<ip_bi>`,
`account_devices.device_bi = blind_index(<account_bi> + ":" + pubkey)`,
`profile-generation:<source_repo_bi>:<snapshot_hash>`. Every recovered value
is replaced by `blind_index_new(plaintext)`, where the embedded blind index
is itself the new one.

**Values whose plaintext is not recoverable** (the plaintext was never
stored) are replaced by a random 256-bit pseudonym, **the same pseudonym
everywhere that value occurs**, so every join between tables still holds; a
future lookup that recomputes the index from a fresh plaintext simply does
not find the old row and starts a new one. Nothing derived from the old key
remains. The decisions, family by family:

| Family (argument of `blind_index`) | Where | Decision |
|---|---|---|
| IP addresses: `ip`, `attribution-ip-v1:<day>:<ip>`, `world-manual-ip-v1:<day>:<ip>`, `note-view-v1\n<note>\n<ip>\n<ua>`, `blog-post-unique-v1:...`, `repo-view-v1\n<owner/repo>\n<account or visitor>`, `world-unique-visitor-*` | `users.ip_bi`, `quarantined_users.ip_bi`, `referral_attributions.signup_ip_bi`, `referral_click_audit.ip_bi`, `feedback.ip_hash`, `attribution_ip_hourly.ip_hash`, `note_views.viewer_key`, `blog_post_view_seen.viewer_key`, `repo_views.viewer_key`, `world_manual_blocks.subject_token` | Pseudonym (equal addresses stay equal, so duplicate-signup and abuse grouping keeps working on old rows). The hour/day counters restart; manual world blocks by IP stop matching (they are day-scoped and expire anyway). |
| Rate counters and throttles keyed by IP or identifier: `login-local:`, `login-pair:`, `login-source:`, `password-reset-*`, `agent-guestbook-rate-v1`, signup IP | `login_attempts`, `password_reset_rate`, `signup_rate`, `agent_guestbook_rate`, `ai_ask_rate`, `catalog_rate`, `contributor_invitation_rate` | Table emptied (ephemeral). |
| Referral click fingerprints: `referral-click:`, `referral-network-window:`, `referral-user-agent:` (recovered when the UA is stored), `referral-client-v1:` | `referral_click_seen` (emptied), `referral_click_claims` (emptied), `referral_click_audit.visit_bi`, `referral_attributions.network_window_bi` / `client_fingerprint_bi` / `user_agent_bi` | Pseudonym; in-flight click claims are emptied. |
| Notification dedupe `notification:<recipient>:<dedupe>` (the dedupe value is not stored) | `notifications.dedupe_bi` | Pseudonym. The notification stays readable and markable (its id is the pseudonym); an old event re-sent after the re-key could appear once more. |
| Secrets shown once and not stored: `org-bot-token:<token>`, chat webhook tokens, `security-scan-lease:<token>` | `org_bot_tokens.secret_bi`, `chat_channel_webhooks.token_bi`, `repo_security_scan_leases.lease_token_bi` | Recomputed if the token is inside the encrypted row, otherwise pseudonym: **such org bot tokens and channel webhook URLs stop working and must be re-issued**; scan leases expire. |
| Chat room invite links `chat-invite-token:<token>` | `chat_channel_invites.token_bi` | Recomputed: the token is kept inside the row's encrypted `data`, so shared invite links keep working. |
| Pull submission retry key `pull-submission:<author>:<sig>` | `pull_submission_receipts.submission_bi`, `pull_inbox.submission_bi` | Pseudonym (the same one in both tables, so an undrained inbox row still matches its receipt). A desktop retry of a pull request submitted before the re-key reserves a new number instead of being recognised as a replay. |
| ActivityPub delivery dedupe (`ap-digest-delivery:`, `ap-mention-followup:`, `ap-context:user-update:`) and anything else not listed | `ap_outbox.dedupe_bi`, ... | Recomputed when the parts are in the row, otherwise pseudonym. |
| Identity: account names, org names, repositories | `users.user_bi`, `nodes.node_bi`, `orgs.org_bi`, `repositories.key_bi` (`STRICT_BI`) | Must be recomputed. If any value is not recoverable the tool **aborts** and names the column; re-run with `--allow-orphans` only after deciding that row may become unreachable by name. |

### 1.3 Derived on the fly (nothing stored)

These change with the key. Nothing to migrate; outstanding tokens die, which
is the point.

| Use | Effect of the re-key |
|---|---|
| `_password_reset_token`, `_email_verify_token`, `_email_verify_link` (blind-index key) | Links sent before the re-key no longer verify; users request new ones. `password_reset_rate` is emptied. The admin queue `pending_verifications` (name + email of unverified accounts) is kept and re-encrypted: it holds no token. |
| `_account_session_secret` = `ADMIN_PASS|DATA_KEY`: account session token digests | Stored in `account_sessions.token_digest`: **emptied**, everyone logs in again (browser, mobile, and every device session). |
| same secret: account-event tickets, World tickets, status-update and new-member signatures, repository-invitation tokens (emailed contributor invitations), hosted-git push passwords (`hosted_push_mac`), World manual-block and mirror-push signatures, relay migration report HMAC fallback | Stateless: tickets are re-issued on the next request; outstanding invitation links and git push passwords must be re-issued. |
| `_admin_session_secret`, `_admin_csrf_token` (`ADMIN_PASS|DATA_KEY`) | Admins sign in again. |
| `_require_data_secret + ":..."` HMACs: signup-intent cookie (`account_signup_intents` emptied), referral click token (`referral_click_claims` emptied), chat channel / direct WebSocket tickets, Office entry and meeting tickets, agent-guestbook and chat-webhook delivery signatures, bootstrap readiness bearer | Stateless; re-derived under the new key. |
| Tracked mail footer image (`/api/mail/t/f/<footer id>.<sig>.png`, `sig` = blind index of `mail-footer:<footer id>`) | The `email_footers` row and its open count are kept, but footers already pasted into a mail signature stop counting opens; the user copies the footer again from Settings → Account → Tracked email footer (same footer id, new signature). |
| `product_analysis` session (`password|DATA_KEY`) | Sign in again. |
| `link_codes` (device link codes, one-time link grants) | Emptied; link again. |
| Office agent presence (`organization_agent_presence`) | Emptied; rows live at most 8 hours after a run ends and the desktop reports its live runs again on its next reload. |
| Office attendance (`org_office_visits`) | Emptied; attendance keeps 14 days. Office chat is the site chat since migration 0227 (its channels are ordinary `chat_channels` rows, re-keyed with them); `org_office_channels` only maps an organization's blind index to those channel ids and is recomputed like every other `org_bi`. |
| OAuth states (`patreon_oauth_states`, `organization_discord_oauth_states`) | Emptied; restart the OAuth flow. |

### 1.4 Not affected

- Client-side E2E data: `owner_encryption_keys`, the `chat-seal` inner layer,
  agent E2EE, private mirror replicas (`private_mirror_routes`) and
  password-vault package rows are keyed by user keys, not `DATA_KEY`.
- Password hashes (PBKDF2, per-user salt).
- `MIRROR_ROUTER_SIGNING_SEED`, `ADMIN_PASS`, `WWW_EDGE_SECRET`,
  `FORKMESH_MESH_SECRET`, `RQLITE_PASSWORD`: already Next's own.
  (At the production cutover the router key pair becomes production's, because
  every production mirror pinned its public key; docs/CUTOVER.md. It is not
  derived from `DATA_KEY`, so the re-key does not touch it either way.)
- The node-local KV cache (`<data>/kv.sqlite`, `REPOSITORY_METADATA`) is not in
  rqlite and holds no `DATA_KEY` ciphertext; it is a cache.

## 2. What the tool does

`tools/next_rekey.py run --in <backup> --out <file>`:

1. Reads the old and new key from files only (mode must be 0400/0600; never
   argv, env or logs). Refuses to run on a node (`/etc/forkmesh-next`).
2. Opens the input read-only (a SQLite file from `next_backup.py restore`;
   a plain `.gz` backup is decompressed into the work directory; either holds
   only ciphertext). Work files live in a fresh 0700
   `~/projects/forkmesh/data/fleet/rekey/run-*` directory, removed at the end;
   SQLite temp storage is in memory. Plaintext never touches disk.
3. Pass 1 (harvest): decrypts every row in memory, collects identifiers, and
   builds the old → new blind-index map and the pseudonyms.
4. Pass 2 (write): builds a fresh SQLite file (same schema, `sqlite_sequence`,
   indexes and triggers created after the data, so nothing fires and no page
   of the old file is copied), re-encrypting every ciphertext with a fresh
   nonce, rewriting every blind index, re-sealing chat envelopes, and
   emptying the `DROP_TABLES`.
5. Pass 3 (verify, on the new file): integrity check; every ciphertext opens
   under the new key and **none** under the old key (whole cells and nested
   JSON); every chat envelope opens under its new room key and none under the
   old; no old value the run replaced is left anywhere; the old blind index
   of every identifier found in the re-keyed, decrypted rows (with every
   family prefix) is absent; the emptied tables are empty; the per-table count
   of re-encrypted values equals the count that opens under the new key.
   Only then is the output written (0600).
6. Prints counts per table (rows in/out, re-encrypted, recomputed,
   pseudonymised, chat) and the two vault key ids. Never a value.

`verify --in <file>` repeats pass 3 on any file. `find-key --key <file>
[paths]` names files that contain a key or its AES / blind-index keys
(default: the Next config, data and logs directories and `deploy/`).

Coverage is enforced by `app/tests/host/test_next_rekey.py`: it builds the
schema from `app/migrations` plus `app/src/schema.py` and fails when a
`*_bi` column is not in `BI_COLUMNS`, when a registered table or column no
longer exists, and when this document does not name a registered table. At
run time, a `*_bi` column or a ciphertext column the registry does not know
(for example a marketplace plugin's table) is still processed and is listed
under `WARNING unregistered_*` in the summary (`--strict-registry` aborts
instead).

## 3. Rollout runbook

Run from `~/projects/forkmesh/src`. `K=~/projects/forkmesh/ssh/fleet/next_fleet_ed25519`,
node addresses from `config/fleet-instances.json`. Downtime is the whole
procedure (minutes).

```sh
cd ~/projects/forkmesh/src
K=~/projects/forkmesh/ssh/fleet/next_fleet_ed25519
CORES="<n1-ip> <n2-ip> <n3-ip>"
umask 077

# 0. Keep the desktop timers from acting mid-procedure.
systemctl --user stop fmnext-unseal.timer fmnext-backup.timer

# 1. Stop the app on every core node (rqlite keeps running).
for ip in $CORES; do ssh -i $K root@$ip systemctl stop forkmesh-app; done

# 2. Back up (pulls a verified, consistent rqlite snapshot; kept age-encrypted
#    as rqlite-<UTC>.sqlite3.gz.age since commit 0ba6371).
.venv/bin/python tools/next_backup.py run --execute
B=$(ls -1 ~/projects/forkmesh/data/fleet/backups/rqlite-*.sqlite3.gz* | tail -1)

# 3. Decrypt + gunzip into the 0700 re-key directory. `restore` verifies it
#    and writes a new 0600 file; --identity is the offline age key (default
#    config/backup-age.key). For a pre-encryption rqlite-*.sqlite3.gz backup,
#    `gunzip -c "$B" > "$IN"` does the same (or pass the .gz to next_rekey).
mkdir -p -m 0700 ~/projects/forkmesh/data/fleet/rekey
STAMP=$(basename "$B" | sed -E 's/\.sqlite3\.gz(\.age)?$//')
IN=~/projects/forkmesh/data/fleet/rekey/input-$STAMP.sqlite3
.venv/bin/python tools/next_backup.py restore "$B" --out "$IN" --identity <age identity file>

# 4. Re-key: production key -> Next's own key. Verifies before writing.
OUT=~/projects/forkmesh/data/fleet/rekey/rekeyed-$STAMP.sqlite3
.venv/bin/python tools/next_rekey.py run --in "$IN" --out "$OUT" \
  --old-key ~/projects/forkmesh/config/fleet/DATA_KEY \
  --new-key ~/projects/forkmesh-backup/config/DATA_KEY.next-original \
  --summary-json "$OUT.summary.json"
#    Read the summary: "verify: OK", the old/new key ids, and any WARNING or
#    pseudonymised counts. If it stops on STRICT_BI orphans, look first, then
#    re-run with --allow-orphans.

# 5. Load it through the Raft leader (/db/load replaces the whole database).
#    Find the leader: the node listed with "leader": true.
ssh -i $K root@<n1-ip> "curl -fsS -K /etc/forkmesh-next/rqlite.curlrc 'http://127.0.0.1:4001/nodes?pretty'"
LEADER=<leader-ip>
scp -i $K "$OUT" root@$LEADER:/root/rekeyed.sqlite3
ssh -i $K root@$LEADER "curl -fsS -K /etc/forkmesh-next/rqlite.curlrc -XPOST \
  -H 'Content-Type: application/octet-stream' --data-binary @/root/rekeyed.sqlite3 \
  http://127.0.0.1:4001/db/load && shred -u /root/rekeyed.sqlite3"
#    Check every voter has it (same count on each node):
for ip in $CORES; do ssh -i $K root@$ip "curl -fsS -K /etc/forkmesh-next/rqlite.curlrc \
  -G 'http://127.0.0.1:4001/db/query?level=none' --data-urlencode 'q=SELECT COUNT(*) FROM users'"; done

# 6. Switch the desktop key to Next's own.
cd ~/projects/forkmesh/config/fleet
mv DATA_KEY DATA_KEY.production           # temporary, for step 10 only
install -m 0400 DATA_KEY.next-original DATA_KEY
grep -c '^DATA_KEY=' secrets.env          # if 1: delete that line (config/DATA_KEY wins, but do not keep a second copy)
cd ~/projects/forkmesh/src

# 7. Re-seal the vault under the new key (host_secrets is empty after the load;
#    the apps are stopped, so no reload).
.venv/bin/python tools/next_vault.py sync --prune --no-reload --execute

# 8. Start the apps; they come up sealed. Unseal from the desktop.
for ip in $CORES; do ssh -i $K root@$ip systemctl start forkmesh-app; done
.venv/bin/python tools/next_vault.py unseal --execute
.venv/bin/python tools/next_vault.py status      # every node unsealed; every row key_id "(matches)"
systemctl --user start fmnext-unseal.timer fmnext-backup.timer

# 9. Mail back on: uncomment "MAILTRAP_API_TOKEN" in deploy/fleet.toml
#    [app] optional_secrets, commit, then re-sync (nodes reload, no restart).
.venv/bin/python tools/next_vault.py sync --prune --execute

# 10. Prove no production-key material is left (next section), then shred
#     the production key from the Next config.
.venv/bin/python tools/next_rekey.py find-key --key ~/projects/forkmesh/config/fleet/DATA_KEY.production
shred -u ~/projects/forkmesh/config/fleet/DATA_KEY.production
```

Then sign in on `app.next.forkmesh.com` with an existing account's password
(every session was invalidated), open an org and a note, and check a chat
channel's history.

**Keep the production key?** Only a future re-import of production data
needs it, and such an import must be re-keyed again with this same tool.
Recommendation: do not keep it in the Next config; fetch it from
production's own secret store for the next import, re-key, and shred it
again. Pre-re-key backups in `data/backups/` (and the `input-*` /
`imports/` files) are production-key ciphertext; they are useless without
that key, but delete them by name once the new nightly backups are good.

## 4. Verifying that no production-key material remains

1. `next_rekey.py find-key --key config/DATA_KEY.production` must report
   `0 file(s)` over the Next config, data and logs directories and `deploy/`.
   It looks for the key itself and for its AES (`SHA-256(key)`) and
   blind-index keys, in raw, hex and base64 form. It prints file names only.
   Add paths (e.g. `~/.config/systemd/user`) as arguments.
2. `next_vault.py status`: `local DATA_KEY: key_id` must equal `new_key_id`
   from the re-key summary and differ from `old_key_id`, and every row must
   say `(matches)`. A node that unseals proves the rows open under that key.
3. `next_rekey.py verify --in <a fresh backup, gunzipped> --old-key
   config/DATA_KEY.production --new-key config/DATA_KEY` (before step 10's
   shred, or against a later backup while you still hold the key): `OK`.
4. Nodes: `/etc/forkmesh-next/creds` holds no `DATA_KEY` since the vault
   (docs/VAULT.md), and the key only ever lived in the unsealed app's memory,
   which the restart in step 1/8 cleared.

## 5. Table coverage

**Bold** columns are `STRICT_BI`. "recompute" means recovered from plaintext
or replaced by a consistent pseudonym (section 1.2).

| Table | Blind-index columns | Ciphertext | Handling |
|---|---|---|---|
| `about_inbox` | repo_bi | data | re-encrypt; recompute |
| `account_activity_days` | account_bi | - | recompute |
| `account_devices` | device_bi, account_bi | - | recompute |
| `account_presence` | name_bi | - | recompute |
| `account_sessions` | account_bi, device_bi | - | emptied: session token digests (HMAC under ADMIN_PASS|DATA_KEY): everyone logs in again |
| `account_signup_intents` | account_bi | - | emptied: signup-intent cookies are HMAC-signed under DATA_KEY |
| `account_ssh_keys` | account_bi, key_bi | data | re-encrypt; recompute |
| `ad_network_ads` | account_bi | owner_sealed | re-encrypt; recompute |
| `ad_network_reports` | account_bi | - | recompute |
| `ad_network_reviews` | reviewer_bi | - | recompute |
| `admin_signup_referral_code` | code_bi, last_used_by_bi | data | re-encrypt; recompute |
| `agent_guestbook_rate` | visitor_bi | - | emptied: rate counter keyed by an IP-derived blind index |
| `agent_prompts` | repo_bi | data | re-encrypt; recompute |
| `ai_ask_rate` | account_bi | - | emptied: rate counter |
| `ap_actors` | actor_bi | data | re-encrypt; recompute |
| `ap_comments` | context_bi, remote_id_bi, parent_remote_id_bi | data | re-encrypt; recompute |
| `ap_digest_queues` | scope_bi | data | re-encrypt; recompute |
| `ap_followers` | actor_bi | - | recompute |
| `ap_mentions` | remote_id_bi | - | recompute |
| `ap_objects` | actor_bi, context_bi | data | re-encrypt; recompute |
| `ap_org_digest_settings` | org_bi | data | re-encrypt; recompute |
| `ap_outbox` | dedupe_bi | data | re-encrypt; recompute |
| `ap_repo_settings` | repo_bi | data | re-encrypt; recompute |
| `ap_service_keys` | - | data | re-encrypt |
| `apns_device_tokens` | token_bi, account_bi, device_bi | data | re-encrypt; recompute |
| `attribution_ip_hourly` | ip_hash (token) | - | recompute |
| `badge_awards` | account_bi | - | recompute |
| `blog_post_links` | updated_by_bi | - | recompute |
| `blog_post_view_seen` | viewer_key (token) | - | recompute |
| `bounty_wallet` | wallet_bi | - | recompute |
| `branch_op_inbox` | repo_bi | data | re-encrypt; recompute |
| `branch_op_results` | repo_bi | data | re-encrypt; recompute |
| `bug_reports` | reporter_bi | data | re-encrypt; recompute |
| `calendar_attendees` | attendee_bi | - | recompute |
| `calendar_events` | owner_bi, org_bi | data | re-encrypt; recompute |
| `calendar_reminder_deliveries` | recipient_bi | - | recompute |
| `catalog_rate` | owner_bi | - | emptied: rate counter |
| `chat_channel_invites` | token_bi, created_by_bi | data | re-encrypt; recompute |
| `chat_channel_members` | member_bi, invited_by_bi | data | re-encrypt; recompute |
| `chat_channel_reads` | account_bi | - | recompute |
| `chat_channel_webhooks` | token_bi, created_by_bi | data | re-encrypt; recompute |
| `chat_channels` | name_bi, created_by_bi | data | re-encrypt; recompute |
| `chat_direct_conversations` | pair_bi | data | re-encrypt; recompute |
| `chat_direct_media` | sender_bi, recipient_bi | data | re-encrypt; recompute |
| `chat_direct_media_parts` | - | data (client-encrypted media, sealed again) | re-encrypt |
| `chat_read_marks` | account_bi | - | recompute; keep the conversation keys and read timestamps |
| `chat_direct_participants` | participant_bi | data | re-encrypt; recompute |
| `chat_history` | - | body (room envelope) | envelopes re-sealed under the new room key; undecryptable rows dropped |
| `clone_rr` | repo_bi | - | recompute |
| `clone_sticky` | repo_bi | - | recompute |
| `code_challenge_members` | account_bi | - | recompute |
| `code_challenges` | owner_bi | - (data is plain JSON) | recompute |
| `commit_inbox` | repo_bi | - | recompute |
| `community_ad_instance_policy` | updated_by_bi | - | recompute |
| `community_ad_moderation` | actor_bi | - | recompute |
| `community_ad_proposals` | proposer_bi | - | recompute |
| `community_ad_votes` | voter_bi | - | recompute |
| `contributor_activity` | author_bi | - | recompute |
| `contributor_invitation_abuse` | - | data | re-encrypt |
| `contributor_invitation_optouts` | email_bi | - | recompute |
| `contributor_invitation_provenance` | contributor_bi, email_bi, created_by_bi | data | re-encrypt; recompute |
| `contributor_invitation_rate` | inviter_bi | - | emptied: daily send counter |
| `contributor_invitations` | inviter_bi, email_bi, contributor_bi | data | re-encrypt; recompute |
| `contributor_reward_estimates` | recipient_bi | - | recompute |
| `discord_bug_reports` | message_bi, repo_bi | data | re-encrypt; recompute |
| `discord_bug_watch` | channel_bi | - | recompute |
| `discussion_inbox` | repo_bi, submitter_bi, claimed_by_bi, mirrored_by_bi | data | re-encrypt; recompute |
| `edge_route_cursor` | repo_bi | - | recompute |
| `email_sends` | recipient_bi (ref holds an org_bi for task mail) | data | drop (view tokens are signed under DATA_KEY) |
| `email_footers` | owner_bi | data | re-encrypt; recompute (the footer image signature changes: see 1.3) |
| `federated_presence` | relay_bi | - | recompute |
| `federated_signup` | relay_bi | - | recompute |
| `feedback` | ip_hash (token) | - | recompute |
| `feedback_email_sends` | account_bi | - | recompute |
| `hackathon_ambassadors` | account_bi | - | recompute |
| `hackathon_challenge_entries` | account_bi | - | recompute |
| `hackathon_challenges` | challenger_bi | - | recompute |
| `hackathon_judges` | account_bi | - | recompute |
| `hackathon_mission_progress` | account_bi | - | recompute |
| `hackathon_participants` | account_bi, referrer_bi | - | recompute |
| `hackathon_points` | account_bi | - | recompute |
| `hackathon_reward_grants` | account_bi | - | recompute |
| `hackathon_scores` | judge_bi | - | recompute |
| `hackathon_share_events` | account_bi | - | recompute |
| `hackathon_squads` | owner_bi | - | recompute |
| `hackathon_team_members` | account_bi | - | recompute |
| `hackathon_teams` | captain_bi | - | recompute |
| `hackathon_votes` | voter_bi | - | recompute |
| `host_presence` | repo_bi | - | recompute |
| `hosted_repos` | key_bi, owner_bi, creator_bi | data | re-encrypt; recompute |
| `host_secrets` | - | - | emptied: the vault: next_vault.py sync re-seals it under the new key |
| `inbox_drain_log` | repo_bi | - | recompute |
| `issue_bounty` | bounty_bi | data | re-encrypt; recompute |
| `issue_inbox` | repo_bi, claimed_by_bi, mirrored_by_bi | data | re-encrypt; recompute |
| `issue_seq` | repo_bi | - | recompute |
| `ledger_backfill_sources` | subject_repo_bi | - | recompute |
| `link_codes` | code_bi | data | emptied: device link codes and one-time grants |
| `login_attempts` | id_bi | - | emptied: failed-login counters keyed by IP-derived blind indexes |
| `mailtrap_email_sends` | account_bi | - | recompute |
| `apps` | owner_bi, org_bi, client_secret_bi | - | recompute owner/org; client_secret_bi has no plaintext, so every app rotates its client secret after a rekey |
| `app_install_requests` | requester_bi, org_bi, decided_by_bi | - | recompute |
| `app_installs` | installed_by_bi, target_bi | - | recompute |
| `app_org_approvals` | org_bi, approved_by_bi | - | recompute |
| `app_org_policies` | org_bi, updated_by_bi | - | recompute |
| `app_reviews` | reviewer_bi | - | recompute |
| `app_auth_codes` | account_bi, code_bi | - | drop (short-lived OAuth codes) |
| `app_tokens` | token_bi | - | drop (apps authorize again) |
| `app_response_urls` | account_bi, token_bi | - | drop (one-time URLs) |
| `desktop_commands` | repo_bi | - | recompute |
| `git_tag_moves` | repo_bi, actor_bi | - | recompute |
| `mail_sends` | account_bi, recipient_bi | - | drop (tracking tokens are signed under DATA_KEY) |
| `mail_suppressions` | recipient_bi | - | drop (no plaintext address to recompute; bounced or unsubscribed recipients can be mailed again) |
| `marketing_email_campaigns` | - | data | re-encrypt |
| `marketing_email_sends` | account_bi | - | recompute |
| `marketplace_member_approvals` | reviewer_bi | - | recompute |
| `marketplace_package_activity` | account_bi | - | recompute |
| `marketplace_package_uses` | account_bi | - | recompute |
| `marketplace_review_history` | reviewer_bi | - | recompute |
| `marketplace_review_reports` | reporter_bi | - | recompute |
| `marketplace_review_votes` | voter_bi | - | recompute |
| `marketplace_reviews` | reviewer_bi | - | recompute |
| `marketplace_submissions` | submitted_by_bi, owner_approved_by_bi | - | recompute |
| `microjob_claims` | account_bi, subject_bi | data | re-encrypt; recompute |
| `mirror_https_endpoints` | node_bi | - | recompute |
| `mirror_repository_integrity` | repo_bi, node_bi | - | recompute |
| `not_found_paths` | - | last_client | re-encrypt |
| `nodes` | **node_bi**, user_bi | data | re-encrypt; recompute |
| `note_shares` | principal_bi, added_by_bi | - | recompute |
| `note_versions` | author_bi | data | re-encrypt; recompute |
| `note_views` | viewer_key (token) | - | recompute |
| `notes` | owner_bi | data | re-encrypt; recompute |
| `notifications` | dedupe_bi, recipient_bi | data | re-encrypt; recompute |
| `org_agent_attachments` | org_bi | - (sealed with the job row key, not `DATA_KEY`) | recompute |
| `org_agent_jobs` | org_bi | data | re-encrypt; recompute |
| `org_agent_sessions` | org_bi, created_by_bi | data | re-encrypt; recompute |
| `org_bot_token_usage` | org_bi | - | recompute |
| `org_bot_tokens` | org_bi, secret_bi, created_by_bi | data | re-encrypt; recompute |
| `org_members` | org_bi, member_bi | - | recompute |
| `org_plugin_installs` | org_bi, installed_by_bi | - | recompute |
| `org_pull_rules` | org_bi | - | recompute |
| `org_repos` | org_bi | - | recompute |
| `org_succession_approvals` | approver_bi | - | recompute |
| `org_succession_cases` | org_bi, owner_bi, successor_bi | - | recompute |
| `org_succession_events` | org_bi, actor_bi | - | recompute |
| `org_succession_policies` | org_bi, owner_bi, successor_bi | - | recompute |
| `org_team_collaborators` | org_bi, account_bi, added_by_bi | - | recompute |
| `org_team_members` | org_bi, member_bi | - | recompute |
| `org_teams` | org_bi | - | recompute |
| `organization_discord_connectors` | org_bi, updated_by_bi | data | re-encrypt; recompute |
| `organization_discord_oauth_grants` | org_bi, verified_by_bi | data | re-encrypt; recompute |
| `organization_discord_oauth_states` | org_bi | data | emptied: OAuth state in flight |
| `organization_discord_setup_tasks` | org_bi | - | recompute |
| `organization_task_attachments` | org_bi, created_by_bi | data | re-encrypt; recompute |
| `organization_task_checkins` | org_bi, account_bi | data | re-encrypt; recompute |
| `organization_task_events` | org_bi, actor_bi | data | re-encrypt; recompute |
| `organization_task_qa_reviews` | org_bi, reviewer_bi | data | re-encrypt; recompute |
| `organization_task_responses` | org_bi, author_bi | data | re-encrypt; recompute |
| `organization_tasks` | org_bi, assignee_bi, active_assignee_bi, created_by_bi, qa_reviewer_bi | data | re-encrypt; recompute |
| `orgs` | **org_bi** | data | re-encrypt; recompute |
| `owner_encryption_keys` | account_bi | - | recompute |
| `password_reset_rate` | key_bi | - | emptied: reset throttle keyed by blind indexes of the account |
| `patreon_account_links` | account_bi, patreon_user_bi | data | re-encrypt; recompute |
| `patreon_oauth_states` | account_bi | data | emptied: OAuth state in flight |
| `patreon_reconciliation_generations` | member_bi | - | recompute |
| `patreon_webhook_events` | account_bi | - | recompute |
| `pending_rewards` | recipient_bi | - | recompute |
| `pending_verifications` | name_bi | data | re-encrypt; recompute |
| `plan_trials` | account_bi | - | recompute |
| `private_mirror_routes` | binding_bi, repo_bi, node_bi | - | recompute |
| `profile_contribution_days` | generation_bi, subject_user_bi, source_account_bi, source_repo_bi, project_bi | data | re-encrypt; recompute |
| `profile_contribution_languages` | generation_bi, owner_user_bi, source_repo_bi, project_bi | - | recompute |
| `profile_contribution_projects` | source_repo_bi, source_account_bi, owner_user_bi, project_bi, active_generation_bi | data | re-encrypt; recompute |
| `profile_contribution_receipts` | generation_bi, source_account_bi, source_repo_bi | - | recompute |
| `profile_follows` | follower_bi, target_bi | - | recompute |
| `public_catalog_snapshot` | - | data | re-encrypt |
| `pull_inbox` | repo_bi, claimed_by_bi, mirrored_by_bi, submission_bi | data | re-encrypt; recompute |
| `pull_submission_receipts` | repo_bi, submission_bi | data | re-encrypt; recompute |
| `quarantined_nodes` | node_bi, user_bi, quarantined_user_bi | data | re-encrypt; recompute |
| `quarantined_users` | user_bi, email_bi, ip_bi, referrer_bi, discord_bi | data | re-encrypt; recompute |
| `referral_attributions` | referred_bi, referrer_bi, signup_ip_bi, user_agent_bi, network_window_bi, client_fingerprint_bi | data | re-encrypt; recompute |
| `referral_click_audit` | visit_bi, referrer_bi, ip_bi, user_agent_bi | data | re-encrypt; recompute |
| `referral_click_claims` | referrer_bi, referred_bi | - | emptied: pending referral click tokens (HMAC under DATA_KEY) |
| `referral_click_seen` | visit_bi, referrer_bi | - | emptied: click dedupe keyed by IP/user-agent blind indexes |
| `referral_legacy_totals` | referrer_bi | - | recompute |
| `referral_referrer_controls` | referrer_bi | - | recompute |
| `referral_stats` | referrer_bi | - | recompute |
| `relay_self` | - | data | re-encrypt |
| `relays` | relay_bi | - | recompute |
| `release_downloads` | repo_bi | - | recompute |
| `repo_action_jobs` | - | data | re-encrypt |
| `repo_agents` | repo_bi | data | re-encrypt; recompute |
| `repo_alert_settings` | repo_bi | data | re-encrypt; recompute |
| `repo_deletions` | repo_bi, owner_bi | - | recompute |
| `repo_device_mirrors` | repo_bi | data | re-encrypt; recompute |
| `repo_first_hosted` | repo_bi | - | recompute |
| `repo_forks` | parent_bi, fork_bi, account_bi | - | recompute |
| `repo_media` | repo_bi | data | re-encrypt; recompute |
| `repo_merge_jobs` | repo_bi, actor_bi | - | recompute |
| `repo_privacy_policy` | repo_bi, owner_bi | - | recompute |
| `repo_provider_mirror_links` | repo_bi | - | recompute |
| `repo_provider_mirror_outbox` | repo_bi | data | re-encrypt; recompute |
| `repo_provider_mirrors` | repo_bi | data | re-encrypt; recompute |
| `repo_remote_forks` | repo_bi, url_bi | - | recompute `remote-forks:<owner/repo>` and `remote-fork-url:<url>`; keep public fork metadata JSON |
| `repo_security_scan_leases` | repo_bi, lease_token_bi | data | re-encrypt; recompute |
| `repo_security_scan_reviews` | repo_bi, reviewer_bi | data | re-encrypt; recompute |
| `repo_security_scans` | repo_bi | data | re-encrypt; recompute |
| `repo_shares` | repo_bi, grantee_bi | data | re-encrypt; recompute |
| `repo_source_authorities` | repo_bi | data | re-encrypt; recompute |
| `repo_stars` | repo_bi, account_bi | - | recompute |
| `repo_state_history` | key_bi | - | recompute |
| `repo_terms_flags` | repo_bi, updated_by_bi | data | re-encrypt; recompute |
| `repo_view_totals` | repo_bi | - | recompute |
| `repo_views` | repo_bi, viewer_key (token) | - | recompute |
| `repo_webhook_deliveries` | repo_bi | - | recompute |
| `repo_webhook_outbox` | repo_bi | data | re-encrypt; recompute |
| `repo_webhook_seeds` | repo_bi | - | recompute |
| `repo_webhook_sequence` | repo_bi | - | recompute |
| `repo_webhooks` | repo_bi | data | re-encrypt; recompute |
| `repositories` | **key_bi**, owner_bi | data | re-encrypt; recompute |
| `repository_fork_jobs` | target_node_bi, target_repo_bi, source_repo_bi, actor_bi | data | re-encrypt; recompute |
| `repository_imports` | owner_bi | data | re-encrypt; recompute |
| `repository_logo_suggestions` | proposer_bi, reviewed_by_bi | data | re-encrypt; recompute |
| `repository_migration_jobs` | owner_bi | data | re-encrypt; recompute |
| `repository_migration_objects` | - | data | re-encrypt |
| `repository_migration_runs` | - | data | re-encrypt |
| `repository_mirror_volunteers` | operator_bi | - | recompute |
| `reward_node_observations` | node_bi | - | recompute |
| `reward_wallet_reminders` | recipient_bi | - | recompute |
| `role_grants` | account_bi, scope_bi, granted_by_bi | - | recompute |
| `security_reports` | - | data | re-encrypt |
| `sensitive_audit_log` | actor_bi, target_token (token) | - | recompute |
| `signup_rate` | ip_bi | - | emptied: signup throttle keyed by IP blind index |
| `thread_subscriptions` | thread_bi, subscriber_bi | data | re-encrypt; recompute |
| `tip_notice` | from_bi, to_bi | - | recompute |
| `users` | **user_bi**, email_bi, ip_bi, discord_bi | data | re-encrypt; recompute |
| `workflow_credentials` | owner_bi | data | re-encrypt; recompute |
| `workflow_runs` | owner_bi | data | re-encrypt; recompute |
| `workflows` | owner_bi | data | re-encrypt; recompute |
| `world_build_board_items` | updated_by_bi, completed_by_bi | - | recompute |
| `world_element_purchases` | account_bi | deposit_secret | re-encrypt; recompute |
| `world_events` | created_by_bi | - | recompute |
| `world_fediverse_instances` | created_by_bi | - | recompute |
| `world_fediverse_mention_moderation` | actor_bi | - | recompute |
| `world_fediverse_mentions` | remote_id_bi, repo_bi | data | re-encrypt; recompute |
| `world_floor_note_replies` | account_bi | data | re-encrypt; recompute |
| `world_floor_notes` | account_bi | data | re-encrypt; recompute |
| `world_inactive_presence` | account_bi | data | re-encrypt; recompute |
| `world_lobby_links` | account_bi, url_bi | data | re-encrypt; recompute |
| `world_manual_blocks` | created_by_bi, subject_token (token) | - | recompute |
| `world_media_items` | added_by_bi | - | recompute |
| `world_media_playback` | changed_by_bi | - | recompute |
| `world_media_roles` | account_bi, granted_by_bi | - | recompute |
| `world_media_schedules` | created_by_bi | - | recompute |
| `world_media_spaces` | owner_bi | - | recompute |
| `world_office_attendance` | account_bi | - | recompute |
| `world_office_marketing_checkins` | org_bi, account_bi | - | recompute |
| `world_office_marketing_initiatives` | org_bi, source_bi, created_by_bi | data | re-encrypt; recompute |
| `world_office_marketing_proofs` | org_bi, account_bi, url_bi | data | re-encrypt; recompute |
| `world_office_marketing_tasks` | org_bi, assignee_bi, active_assignee_bi, created_by_bi | - | recompute |
| `world_qa_reviews` | account_bi | - | recompute |
| `world_user_activity` | account_bi | - | recompute |

Every table not listed above is copied row for row and still scanned the
same way: any value that opens under the old key is re-encrypted, and any
embedded old blind index is replaced.

## 6. Residual risk

- **Pseudonymised indexes** (section 1.2): lookups that recompute them from a
  new plaintext start fresh (rate counters, IP grouping for rows created
  later, notification dedupe, unstored bot and webhook tokens, which must be
  re-issued).
- **Unknown 64-hex values in unregistered columns.** A blind index stored in
  a column that is neither `*_bi`, a registered token column nor a `*_bi`
  JSON key, and whose plaintext is not recoverable, cannot be told apart from
  a content hash and is left as is. It is an HMAC output: it reveals nothing
  about either key, but it still matches the old key's index of its
  plaintext. Truncated indexes (`error_log`-style `app:<32 hex>` source keys)
  are in the same class.
- **Old ciphertext outside the loaded database**: pre-re-key backups on the
  desktop, the production import files, and rqlite's Raft log and snapshots on
  the nodes until rqlite compacts them. All of it is production-key
  ciphertext, unreadable once the production key is gone from Next; delete the
  desktop copies by name when they are no longer needed.
- **The desktop** still has to be trusted with Next's own key (docs/VAULT.md).
- Tokens issued under the old key (reset/verify links, invitation links, git
  push passwords, sessions) stop working, by design.
