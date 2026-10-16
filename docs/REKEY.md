# Re-keying the database (`DATA_KEY` rotation)

`DATA_KEY` encrypts stored rows, keys every blind index, and seeds several
HMAC secrets. Rotating it means decrypting everything under the old key and
re-creating it under the new one, offline, with the apps stopped. The tool is
`tools/next_rekey.py`; its tests are `app/tests/host/test_next_rekey.py`. It
runs only on the operator box, against a local SQLite file, never on a node.
Users keep their passwords: password hashes are salted PBKDF2 inside the
encrypted account record and do not depend on `DATA_KEY`.

## What depends on `DATA_KEY`

| Kind | Examples | Handling |
|---|---|---|
| Stored ciphertext | `encrypt_row` rows (AES-256-GCM, key `SHA-256(DATA_KEY)`), `ad_network_ads.owner_sealed`, chat history envelopes | re-encrypted with fresh nonces |
| Vault rows | `host_secrets` (HKDF of `DATA_KEY`, [VAULT.md](VAULT.md)) | emptied; `next_vault.py sync` re-seals them |
| Stored blind indexes | `blind_index(v) = HMAC-SHA256(SHA-256(DATA_KEY + ":blind-index"), lower(strip(v)))` in `*_bi` columns, token columns, `_bi` JSON keys | recomputed from recovered plaintext, or replaced by one consistent random pseudonym per value |
| Derived on the fly | password-reset and email-verify links, session token digests, admin sessions, signed tickets, push passwords, room chat passphrases | nothing stored; outstanding tokens stop verifying, sessions end |
| Not affected | client-side end-to-end keys, password hashes, router key, mesh and edge secrets, node-local caches | untouched |

Identity indexes (`STRICT_BI`: `users.user_bi`, `nodes.node_bi`,
`orgs.org_bi`, `repositories.key_bi`) must be recomputed; if any value cannot
be recovered the tool aborts unless `--allow-orphans` is given. A strict row
whose data the old key cannot open is pseudonymised and reported.

## What the tool does

`tools/next_rekey.py run --in <backup> --out <file>`:

1. Reads both keys from 0400/0600 files only (or `--old-key-env FILE:NAME`),
   never argv or environment. Refuses to run on a node.
2. Works in a fresh 0700 directory; plaintext never touches disk.
3. Pass 1 decrypts every row in memory, harvests identifiers (with every
   blind-index family prefix in `BI_PREFIXES`) and builds the old-to-new map.
4. Pass 2 writes a fresh SQLite file with the same schema, re-encrypting,
   rewriting indexes, re-sealing chat and emptying the dropped tables.
5. Pass 3 verifies: integrity, every ciphertext opens under the new key and
   none under the old, no old index remains, dropped tables are empty, counts
   match. Only then is the output written (0600).
6. Prints per-table counts and both key ids, never a value.

`verify --in <file>` repeats pass 3. `find-key --key <file> [paths]` names
files containing a key or its derived keys (raw, hex or base64). An
unregistered `*_bi` or ciphertext column is still processed and listed as a
warning (`--strict-registry` aborts instead).

## Runbook

1. Stop the operator timers and the app on every core (rqlite keeps running).
2. `next_backup.py run --execute`, then `restore` the newest backup into the
   re-key directory.
3. `next_rekey.py run` from the old key to the new key; read the summary.
4. Load the output through the rqlite leader (`/db/load`) and compare row
   counts on every voter.
5. Switch the operator `DATA_KEY` file to the new key.
6. `next_vault.py sync --execute` to re-seal the vault.
7. Start the apps and unseal; re-enable timers.
8. Confirm no old-key material remains: `find-key` reports `0 file(s)`,
   `next_vault.py status` shows the new key id on every row, and `verify`
   passes on a fresh backup. Then destroy the old key copies by name.

## Residual risk

- Pseudonymised indexes start fresh on the next lookup (rate counters,
  notification dedupe, unstored bot and webhook tokens must be re-issued).
- An unrecoverable 64-hex value in an unregistered column is left as is; it
  reveals nothing about either key.
- Old ciphertext remains in earlier backups and in rqlite's Raft log until
  compaction; delete those copies by name when no longer needed.

## Table coverage

The test `test_inventory_doc_names_every_registered_table` requires every
table the tool registers to be named here.

**Blind-index tables (`BI_COLUMNS`):** `about_inbox`, `account_activity_days`, `account_devices`, `account_presence`, `account_sessions`, `account_signup_intents`, `account_ssh_keys`, `ad_network_ads`, `ad_network_reports`, `ad_network_reviews`, `admin_signup_referral_code`, `affiliate_leads`, `affiliates`, `agent_guestbook_rate`, `agent_prompts`, `ai_ask_rate`, `ap_actors`, `ap_comments`, `ap_digest_queues`, `ap_followers`, `ap_mentions`, `ap_objects`, `ap_org_digest_settings`, `ap_outbox`, `ap_repo_settings`, `apns_device_tokens`, `app_auth_codes`, `app_install_requests`, `app_installs`, `app_org_approvals`, `app_org_policies`, `app_response_urls`, `app_reviews`, `app_tokens`, `apps`, `badge_awards`, `badge_answer_facts`, `badge_coauthor_facts`, `badge_progress`, `badge_pull_facts`, `badge_repo_sweeps`, `blog_post_links`, `bounty_wallet`, `branch_op_inbox`, `branch_op_results`, `bug_reports`, `calendar_attendees`, `calendar_events`, `calendar_reminder_deliveries`, `catalog_rate`, `chat_channel_invites`, `chat_channel_members`, `chat_channel_reads`, `chat_channel_webhooks`, `chat_channels`, `chat_direct_conversations`, `chat_direct_media`, `chat_direct_participants`, `chat_read_marks`, `clone_rr`, `clone_sticky`, `code_challenge_members`, `code_challenges`, `commit_inbox`, `community_ad_instance_policy`, `community_ad_moderation`, `community_ad_proposals`, `community_ad_votes`, `contributor_activity`, `contributor_invitation_optouts`, `contributor_invitation_provenance`, `contributor_invitation_rate`, `contributor_invitations`, `contributor_reward_estimates`, `desktop_commands`, `discord_bug_reports`, `discord_bug_watch`, `discussion_inbox`, `edge_route_cursor`, `email_footers`, `email_sends`, `federated_presence`, `federated_signup`, `feedback_email_sends`, `git_tag_moves`, `hackathon_ambassadors`, `hackathon_challenge_entries`, `hackathon_challenges`, `hackathon_judges`, `hackathon_mission_progress`, `hackathon_participants`, `hackathon_points`, `hackathon_reward_grants`, `hackathon_scores`, `hackathon_share_events`, `hackathon_squads`, `hackathon_team_members`, `hackathon_teams`, `hackathon_votes`, `host_presence`, `hosted_repos`, `inbox_drain_log`, `issue_bounty`, `issue_inbox`, `issue_seq`, `ledger_backfill_sources`, `link_codes`, `login_attempts`, `mail_sends`, `mail_suppressions`, `mailtrap_email_sends`, `marketing_email_sends`, `marketplace_member_approvals`, `marketplace_package_activity`, `marketplace_package_uses`, `marketplace_review_history`, `marketplace_review_reports`, `marketplace_review_votes`, `marketplace_reviews`, `marketplace_submissions`, `microjob_claims`, `mirror_https_endpoints`, `mirror_repository_integrity`, `nodes`, `note_shares`, `note_versions`, `notes`, `notification_email_sends`, `notifications`, `org_agent_attachments`, `org_agent_jobs`, `org_agent_sessions`, `org_bot_token_usage`, `org_bot_tokens`, `org_members`, `org_office_channels`, `org_office_visits`, `org_plugin_installs`, `org_pull_rules`, `org_repos`, `org_succession_approvals`, `org_succession_cases`, `org_succession_events`, `org_succession_policies`, `org_team_collaborators`, `org_team_members`, `org_teams`, `organization_agent_presence`, `organization_discord_connectors`, `organization_discord_oauth_grants`, `organization_discord_oauth_states`, `organization_discord_setup_tasks`, `organization_task_attachments`, `organization_task_checkins`, `organization_task_events`, `organization_task_qa_reviews`, `organization_task_responses`, `organization_tasks`, `orgs`, `owner_encryption_keys`, `password_reset_rate`, `patreon_account_links`, `patreon_oauth_states`, `patreon_reconciliation_generations`, `patreon_webhook_events`, `pending_rewards`, `pending_verifications`, `plan_trials`, `plugin_race_entrants`, `plugin_race_plugins`, `plugin_race_posts`, `private_mirror_routes`, `probe_bans`, `probe_visitors`, `profile_contribution_days`, `profile_contribution_languages`, `profile_contribution_projects`, `profile_contribution_receipts`, `profile_follows`, `pull_inbox`, `pull_submission_receipts`, `quarantined_nodes`, `quarantined_users`, `referral_attributions`, `referral_click_audit`, `referral_click_claims`, `referral_click_seen`, `referral_legacy_totals`, `referral_referrer_controls`, `referral_stats`, `relays`, `release_downloads`, `repo_agents`, `repo_alert_settings`, `repo_deletions`, `repo_device_mirrors`, `repo_first_hosted`, `repo_forks`, `repo_media`, `repo_merge_jobs`, `repo_privacy_policy`, `repo_provider_mirror_links`, `repo_provider_mirror_outbox`, `repo_provider_mirrors`, `repo_remote_forks`, `repo_security_scan_leases`, `repo_security_scan_reviews`, `repo_security_scans`, `repo_shares`, `repo_source_authorities`, `repo_stars`, `repo_state_history`, `repo_terms_flags`, `repo_view_totals`, `repo_views`, `repo_webhook_deliveries`, `repo_webhook_outbox`, `repo_webhook_seeds`, `repo_webhook_sequence`, `repo_webhooks`, `repositories`, `repository_fork_jobs`, `repository_imports`, `repository_logo_suggestions`, `repository_migration_jobs`, `repository_mirror_volunteers`, `reward_node_observations`, `reward_wallet_reminders`, `role_grants`, `sensitive_audit_log`, `signup_rate`, `thread_subscriptions`, `tip_notice`, `users`, `workflow_credentials`, `workflow_runs`, `workflows`, `world_build_board_items`, `world_element_purchases`, `world_events`, `world_fediverse_instances`, `world_fediverse_mention_moderation`, `world_fediverse_mentions`, `world_floor_note_replies`, `world_floor_notes`, `world_inactive_presence`, `world_lobby_links`, `world_manual_blocks`, `world_media_items`, `world_media_playback`, `world_media_roles`, `world_media_schedules`, `world_media_spaces`, `world_office_attendance`, `world_office_marketing_checkins`, `world_office_marketing_initiatives`, `world_office_marketing_proofs`, `world_office_marketing_tasks`, `world_qa_reviews`, `world_user_activity`

**Ciphertext tables (`CIPHER_COLUMNS`):** `about_inbox`, `account_ssh_keys`, `ad_network_ads`, `admin_signup_referral_code`, `affiliate_leads`, `affiliates`, `agent_prompts`, `ap_actors`, `ap_comments`, `ap_digest_queues`, `ap_objects`, `ap_org_digest_settings`, `ap_outbox`, `ap_repo_settings`, `ap_service_keys`, `apns_device_tokens`, `branch_op_inbox`, `branch_op_results`, `bug_reports`, `calendar_events`, `chat_channel_invites`, `chat_channel_members`, `chat_channel_webhooks`, `chat_channels`, `chat_direct_conversations`, `chat_direct_media`, `chat_direct_media_parts`, `chat_direct_participants`, `contributor_invitation_abuse`, `contributor_invitation_provenance`, `contributor_invitations`, `discord_bug_reports`, `discussion_inbox`, `email_footers`, `hosted_repos`, `issue_bounty`, `issue_inbox`, `link_codes`, `marketing_email_campaigns`, `microjob_claims`, `nodes`, `not_found_paths`, `note_versions`, `notes`, `notifications`, `org_agent_jobs`, `org_agent_sessions`, `org_bot_tokens`, `organization_discord_connectors`, `organization_discord_oauth_grants`, `organization_discord_oauth_states`, `organization_task_attachments`, `organization_task_checkins`, `organization_task_events`, `organization_task_qa_reviews`, `organization_task_responses`, `organization_tasks`, `orgs`, `patreon_account_links`, `patreon_oauth_states`, `pending_verifications`, `plugin_race_entrants`, `plugin_race_plugins`, `probe_bans`, `profile_contribution_days`, `profile_contribution_projects`, `public_catalog_snapshot`, `pull_inbox`, `pull_submission_receipts`, `quarantined_nodes`, `quarantined_users`, `referral_attributions`, `referral_click_audit`, `relay_self`, `repo_action_jobs`, `repo_agents`, `repo_alert_settings`, `repo_device_mirrors`, `repo_media`, `repo_provider_mirror_outbox`, `repo_provider_mirrors`, `repo_security_scan_leases`, `repo_security_scan_reviews`, `repo_security_scans`, `repo_shares`, `repo_source_authorities`, `repo_terms_flags`, `repo_webhook_outbox`, `repo_webhooks`, `repositories`, `repository_fork_jobs`, `repository_imports`, `repository_logo_suggestions`, `repository_migration_jobs`, `repository_migration_objects`, `repository_migration_runs`, `security_reports`, `thread_subscriptions`, `users`, `workflow_credentials`, `workflow_runs`, `workflows`, `world_element_purchases`, `world_fediverse_mentions`, `world_floor_note_replies`, `world_floor_notes`, `world_inactive_presence`, `world_lobby_links`, `world_office_marketing_initiatives`, `world_office_marketing_proofs`

**Token columns (`TOKEN_COLUMNS`):** `attribution_ip_hourly`, `blog_post_view_seen`, `feedback`, `note_views`, `repo_views`, `sensitive_audit_log`, `world_manual_blocks`

**Emptied tables (`DROP_TABLES`):** `account_sessions`, `account_signup_intents`, `agent_guestbook_rate`, `ai_ask_rate`, `app_auth_codes`, `app_response_urls`, `app_tokens`, `catalog_rate`, `contributor_invitation_rate`, `email_sends`, `host_secrets`, `link_codes`, `login_attempts`, `mail_sends`, `mail_suppressions`, `notification_email_sends`, `org_office_visits`, `organization_agent_presence`, `organization_discord_oauth_states`, `password_reset_rate`, `patreon_oauth_states`, `probe_bans`, `probe_visitors`, `referral_click_claims`, `referral_click_seen`, `signup_rate`

**Chat envelopes (`CHAT_TABLE`):** `chat_history` (`body`).

Every other table is copied row for row and scanned the same way: any value that opens under the old key is re-encrypted and any embedded old blind index is replaced.
