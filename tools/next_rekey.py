#!/usr/bin/env python3
"""Re-key a ForkMesh Next database copy from one DATA_KEY to another.

    next_rekey.py run    --in BACKUP [--out FILE] [--old-key FILE] [--new-key FILE]
    next_rekey.py verify --in REKEYED [--old-key FILE] [--new-key FILE]
    next_rekey.py find-key --key FILE [PATH ...]   # files still holding a key

Runs **only on the operator desktop**, against a local SQLite file (a backup
pulled with ``tools/next_backup.py``; ``.gz`` or already gunzipped). It never
talks to a node, rqlite or production. docs/REKEY.md has the inventory this
tool implements and the rollout runbook.

What ``run`` does, in memory, row by row:

* every ``encrypt_row`` ciphertext (AES-256-GCM, key ``SHA-256(DATA_KEY)``,
  ``base64(iv || ct || tag)``) anywhere in the database -- a whole cell, or a
  string nested inside a JSON cell or inside another decrypted row -- is
  decrypted with the old key and sealed again under the new key with a fresh
  nonce;
* every blind index (``HMAC-SHA256(SHA-256(DATA_KEY + ":blind-index"),
  lower(strip(value)))``) in a ``*_bi`` column, in a registered token column,
  under a ``*_bi``/``*Bi`` JSON key, or embedded as a 64-hex token in any other
  cell, is recomputed under the new key from plaintext found in the decrypted
  rows. A value whose plaintext is not recoverable is replaced by a random
  pseudonym, the same one everywhere it occurs (joins keep working; lookups
  by recomputation start fresh), except in the ``STRICT_BI`` identity
  columns, where it aborts unless ``--allow-orphans``;
* ``chat_history`` envelopes, sealed by clients under room passphrases
  derived from ``DATA_KEY``, are decrypted and sealed again under the new
  room key;
* ``DROP_TABLES`` (sessions, pending verification/reset state, link codes,
  signup intents, rate counters, OAuth states, the vault) are emptied.

Then it verifies the output: every ciphertext opens under the new key and
none under the old one, every chat envelope opens under its new room key,
and no old blind index of any recovered identifier (nor any pseudonymised
value) is left anywhere. The summary has counts only, never a value.

Keys are read from files only (default ``<config>/DATA_KEY`` = old,
``<config>/DATA_KEY.next-original`` = new); never argv, env or logs.
Temporary files live in a fresh 0700 directory under
``~/projects/forkmesh/data/fleet/rekey/`` and hold only ciphertext; the output
file is created 0600. Plaintext is never written to disk.
"""

from __future__ import annotations

import argparse
import base64
import binascii
import datetime as dt
import gzip
import hashlib
import hmac
import json
import os
import re
import secrets
import shutil
import sqlite3
import stat
import sys
import tempfile
from collections import Counter, defaultdict
from pathlib import Path

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

sys.path.insert(0, str(Path(__file__).resolve().parent))
import forkmesh_safe_delete  # noqa: E402

TOOL_VERSION = 1

# --------------------------------------------------------------------------
# The inventory (docs/REKEY.md). app/tests/host/test_next_rekey.py fails when
# the schema grows a *_bi column or a table this registry does not name.

# Emptied: state that is invalid under a new key anyway, or that a user or
# the operator simply re-creates.
DROP_TABLES = {
    "app_auth_codes": "app OAuth codes: blind indexes of the code itself, short-lived",
    "app_tokens": "app access tokens keyed by a blind index of the token: apps authorize again",
    "app_response_urls": "one-time app response URLs keyed by a blind index of the token",
    "mail_sends": "core mail tracking rows; their open/click tokens are signed under DATA_KEY",
    "email_sends": "sent-email tracking rows (every kind); their view tokens are signed under DATA_KEY and ref carries an org blind index",
    "mail_suppressions": "suppressed recipients keyed by blind indexes of their address (no plaintext to recompute)",
    "account_sessions": "session token digests (HMAC under ADMIN_PASS|DATA_KEY): everyone logs in again",
    "account_signup_intents": "signup-intent cookies are HMAC-signed under DATA_KEY",
    "password_reset_rate": "reset throttle keyed by blind indexes of the account",
    "link_codes": "device link codes and one-time grants",
    "login_attempts": "failed-login counters keyed by IP-derived blind indexes",
    "signup_rate": "signup throttle keyed by IP blind index",
    "ai_ask_rate": "rate counter",
    "agent_guestbook_rate": "rate counter keyed by an IP-derived blind index",
    "catalog_rate": "rate counter",
    "contributor_invitation_rate": "daily send counter",
    "referral_click_seen": "click dedupe keyed by IP/user-agent blind indexes",
    "referral_click_claims": "pending referral click tokens (HMAC under DATA_KEY)",
    "patreon_oauth_states": "OAuth state in flight",
    "organization_discord_oauth_states": "OAuth state in flight",
    "host_secrets": "the vault: next_vault.py sync re-seals it under the new key",
    "org_office_visits": "Office attendance (14-day retention), keyed by member blind indexes",
    "organization_agent_presence": "Office agent presence (hours of retention), keyed by account blind indexes",
    "probe_visitors": "404 probe scores keyed by an IP blind index (an hour of window)",
    "probe_bans": "automatic probe bans keyed by an IP blind index; the next sweep lifts their request_block_rules rows",
    "notification_email_sends": "notification email send claims keyed by a blind index of the address and ping (30-day retention)",
}

# Every *_bi column of the schema (app/migrations + app/src/schema.py).
BI_COLUMNS = {
    "about_inbox": ("repo_bi",),
    "account_activity_days": ("account_bi",),
    "account_devices": ("device_bi", "account_bi"),
    "account_presence": ("name_bi",),
    "account_sessions": ("account_bi", "device_bi"),
    "account_signup_intents": ("account_bi",),
    "account_ssh_keys": ("account_bi", "key_bi"),
    "ad_network_ads": ("account_bi",),
    "ad_network_reports": ("account_bi",),
    "ad_network_reviews": ("reviewer_bi",),
    "admin_signup_referral_code": ("code_bi", "last_used_by_bi"),
    "affiliate_leads": ("affiliate_bi", "domain_bi"),
    "affiliates": ("account_bi",),
    "agent_guestbook_rate": ("visitor_bi",),
    "agent_prompts": ("repo_bi",),
    "ai_ask_rate": ("account_bi",),
    "ap_actors": ("actor_bi",),
    "ap_comments": ("context_bi", "remote_id_bi", "parent_remote_id_bi"),
    "ap_digest_queues": ("scope_bi",),
    "plugin_race_entrants": ("account_bi",),
    "plugin_race_plugins": ("account_bi",),
    "plugin_race_posts": ("post_bi", "account_bi"),
    "probe_bans": ("visitor_bi",),
    "probe_visitors": ("visitor_bi",),
    "ap_followers": ("actor_bi",),
    "ap_mentions": ("remote_id_bi",),
    "ap_objects": ("actor_bi", "context_bi"),
    "ap_org_digest_settings": ("org_bi",),
    "ap_outbox": ("dedupe_bi",),
    "ap_repo_settings": ("repo_bi",),
    "apns_device_tokens": ("token_bi", "account_bi", "device_bi"),
    "badge_awards": ("account_bi",),
    "badge_answer_facts": ("repo_bi",),
    "badge_coauthor_facts": ("repo_bi", "account_bi"),
    "badge_progress": ("account_bi",),
    "badge_pull_facts": ("repo_bi", "closer_bi"),
    "badge_repo_sweeps": ("repo_bi",),
    "blog_post_links": ("updated_by_bi",),
    "bounty_wallet": ("wallet_bi",),
    "branch_delete_jobs": ("repo_bi",),
    "branch_op_inbox": ("repo_bi",),
    "branch_op_results": ("repo_bi",),
    "bug_reports": ("reporter_bi",),
    "calendar_attendees": ("attendee_bi",),
    "calendar_events": ("owner_bi", "org_bi"),
    "calendar_reminder_deliveries": ("recipient_bi",),
    "catalog_rate": ("owner_bi",),
    "chat_channel_invites": ("token_bi", "created_by_bi"),
    "chat_channel_members": ("member_bi", "invited_by_bi"),
    "chat_channel_reads": ("account_bi",),
    "chat_channel_webhooks": ("token_bi", "created_by_bi"),
    "chat_channels": ("name_bi", "created_by_bi"),
    "chat_direct_conversations": ("pair_bi",),
    "chat_direct_media": ("sender_bi", "recipient_bi"),
    "chat_read_marks": ("account_bi",),
    "chat_direct_participants": ("participant_bi",),
    "clone_rr": ("repo_bi",),
    "code_challenge_members": ("account_bi",),
    "code_challenges": ("owner_bi",),
    "clone_sticky": ("repo_bi",),
    "commit_inbox": ("repo_bi",),
    "community_ad_instance_policy": ("updated_by_bi",),
    "community_ad_moderation": ("actor_bi",),
    "community_ad_proposals": ("proposer_bi",),
    "community_ad_votes": ("voter_bi",),
    "contributor_activity": ("author_bi",),
    "contributor_invitation_optouts": ("email_bi",),
    "contributor_invitation_provenance": ("contributor_bi", "email_bi", "created_by_bi"),
    "contributor_invitation_rate": ("inviter_bi",),
    "contributor_invitations": ("inviter_bi", "email_bi", "contributor_bi"),
    "contributor_reward_estimates": ("recipient_bi",),
    "discord_bug_reports": ("message_bi", "repo_bi"),
    "discord_bug_watch": ("channel_bi",),
    "discussion_inbox": ("repo_bi", "submitter_bi", "claimed_by_bi", "mirrored_by_bi"),
    "edge_route_cursor": ("repo_bi",),
    "federated_presence": ("relay_bi",),
    "federated_signup": ("relay_bi",),
    "feedback_email_sends": ("account_bi",),
    "marketing_email_sends": ("account_bi",),
    "hackathon_ambassadors": ("account_bi",),
    "hackathon_challenge_entries": ("account_bi",),
    "hackathon_challenges": ("challenger_bi",),
    "hackathon_judges": ("account_bi",),
    "hackathon_mission_progress": ("account_bi",),
    "hackathon_participants": ("account_bi", "referrer_bi"),
    "hackathon_points": ("account_bi",),
    "hackathon_reward_grants": ("account_bi",),
    "hackathon_scores": ("judge_bi",),
    "hackathon_share_events": ("account_bi",),
    "hackathon_squads": ("owner_bi",),
    "hackathon_team_members": ("account_bi",),
    "hackathon_teams": ("captain_bi",),
    "hackathon_votes": ("voter_bi",),
    "host_presence": ("repo_bi",),
    "hosted_repos": ("key_bi", "owner_bi", "creator_bi"),
    "inbox_drain_log": ("repo_bi",),
    "issue_bounty": ("bounty_bi",),
    "issue_inbox": ("repo_bi", "claimed_by_bi", "mirrored_by_bi"),
    "issue_seq": ("repo_bi",),
    "ledger_backfill_sources": ("subject_repo_bi",),
    "link_codes": ("code_bi",),
    "login_attempts": ("id_bi",),
    "org_office_channels": ("org_bi",),
    "org_office_visits": ("org_bi", "member_bi"),
    "organization_agent_presence": ("org_bi", "account_bi"),
    "mailtrap_email_sends": ("account_bi",),
    "mail_sends": ("account_bi", "recipient_bi"),
    "email_sends": ("recipient_bi",),
    "email_footers": ("owner_bi",),
    "apps": ("owner_bi", "org_bi", "client_secret_bi"),
    "app_install_requests": ("requester_bi", "org_bi", "decided_by_bi"),
    "app_installs": ("installed_by_bi", "target_bi"),
    "app_org_approvals": ("org_bi", "approved_by_bi"),
    "app_org_policies": ("org_bi", "updated_by_bi"),
    "app_reviews": ("reviewer_bi",),
    "app_auth_codes": ("account_bi", "code_bi"),
    "app_tokens": ("token_bi",),
    "app_response_urls": ("account_bi", "token_bi"),
    "desktop_commands": ("repo_bi",),
    "git_tag_moves": ("repo_bi", "actor_bi"),
    "mail_suppressions": ("recipient_bi",),
    "marketplace_member_approvals": ("reviewer_bi",),
    "marketplace_package_activity": ("account_bi",),
    "marketplace_package_uses": ("account_bi",),
    "marketplace_review_history": ("reviewer_bi",),
    "marketplace_review_reports": ("reporter_bi",),
    "marketplace_review_votes": ("voter_bi",),
    "marketplace_reviews": ("reviewer_bi",),
    "marketplace_submissions": ("submitted_by_bi", "owner_approved_by_bi"),
    "microjob_claims": ("account_bi", "subject_bi"),
    "mirror_https_endpoints": ("node_bi",),
    "mirror_repository_integrity": ("repo_bi", "node_bi"),
    "nodes": ("node_bi", "user_bi"),
    "note_events": ("actor_bi",),
    "note_shares": ("principal_bi", "added_by_bi"),
    "note_versions": ("author_bi",),
    "notes": ("owner_bi",),
    "notifications": ("dedupe_bi", "recipient_bi", "content_bi"),
    "notification_email_sends": ("send_bi",),
    "org_agent_attachments": ("org_bi",),
    "org_agent_jobs": ("org_bi",),
    "org_agent_sessions": ("org_bi", "created_by_bi"),
    "org_bot_token_usage": ("org_bi",),
    "org_bot_tokens": ("org_bi", "secret_bi", "created_by_bi"),
    "org_members": ("org_bi", "member_bi"),
    "org_plugin_installs": ("org_bi", "installed_by_bi"),
    "org_pull_rules": ("org_bi",),
    "org_repos": ("org_bi",),
    "org_succession_approvals": ("approver_bi",),
    "org_succession_cases": ("org_bi", "owner_bi", "successor_bi"),
    "org_succession_events": ("org_bi", "actor_bi"),
    "org_succession_policies": ("org_bi", "owner_bi", "successor_bi"),
    "org_team_collaborators": ("org_bi", "account_bi", "added_by_bi"),
    "org_team_members": ("org_bi", "member_bi"),
    "org_teams": ("org_bi",),
    "organization_discord_connectors": ("org_bi", "updated_by_bi"),
    "organization_discord_oauth_grants": ("org_bi", "verified_by_bi"),
    "organization_discord_oauth_states": ("org_bi",),
    "organization_discord_setup_tasks": ("org_bi",),
    "organization_task_attachments": ("org_bi", "created_by_bi"),
    "organization_task_checkins": ("org_bi", "account_bi"),
    "organization_task_events": ("org_bi", "actor_bi"),
    "organization_task_qa_reviews": ("org_bi", "reviewer_bi"),
    "organization_task_responses": ("org_bi", "author_bi"),
    "organization_tasks": ("org_bi", "assignee_bi", "active_assignee_bi", "created_by_bi", "qa_reviewer_bi", "deleted_by_bi"),
    "orgs": ("org_bi",),
    "owner_encryption_keys": ("account_bi",),
    "password_reset_rate": ("key_bi",),
    "patreon_account_links": ("account_bi", "patreon_user_bi"),
    "patreon_oauth_states": ("account_bi",),
    "patreon_reconciliation_generations": ("member_bi",),
    "patreon_webhook_events": ("account_bi",),
    "pending_rewards": ("recipient_bi",),
    "pending_verifications": ("name_bi",),
    "plan_trials": ("account_bi",),
    "private_mirror_routes": ("binding_bi", "repo_bi", "node_bi"),
    "profile_contribution_days": ("generation_bi", "subject_user_bi", "source_account_bi", "source_repo_bi", "project_bi"),
    "profile_contribution_languages": ("generation_bi", "owner_user_bi", "source_repo_bi", "project_bi"),
    "profile_contribution_projects": ("source_repo_bi", "source_account_bi", "owner_user_bi", "project_bi", "active_generation_bi"),
    "profile_contribution_receipts": ("generation_bi", "source_account_bi", "source_repo_bi"),
    "profile_follows": ("follower_bi", "target_bi"),
    "pull_inbox": ("repo_bi", "claimed_by_bi", "mirrored_by_bi", "submission_bi"),
    "pull_review_ledger": ("repo_bi",),
    "pull_batch_sent": ("repo_bi", "target_bi"),
    "pull_submission_receipts": ("repo_bi", "submission_bi"),
    "quarantined_nodes": ("node_bi", "user_bi", "quarantined_user_bi"),
    "quarantined_users": ("user_bi", "email_bi", "ip_bi", "referrer_bi", "discord_bi"),
    "referral_attributions": ("referred_bi", "referrer_bi", "signup_ip_bi", "user_agent_bi", "network_window_bi", "client_fingerprint_bi"),
    "referral_click_audit": ("visit_bi", "referrer_bi", "ip_bi", "user_agent_bi"),
    "referral_click_claims": ("referrer_bi", "referred_bi"),
    "referral_click_seen": ("visit_bi", "referrer_bi"),
    "referral_legacy_totals": ("referrer_bi",),
    "referral_referrer_controls": ("referrer_bi",),
    "referral_stats": ("referrer_bi",),
    "relays": ("relay_bi",),
    "release_downloads": ("repo_bi",),
    "repo_agents": ("repo_bi",),
    "repo_alert_settings": ("repo_bi",),
    "repo_deletions": ("repo_bi", "owner_bi"),
    "repo_device_mirrors": ("repo_bi",),
    "repo_feature_settings": ("repo_bi",),
    "repo_first_hosted": ("repo_bi",),
    "repo_forks": ("parent_bi", "fork_bi", "account_bi"),
    "repo_media": ("repo_bi",),
    "repo_merge_jobs": ("repo_bi", "actor_bi"),
    "repo_privacy_policy": ("repo_bi", "owner_bi"),
    "repo_provider_mirror_links": ("repo_bi",),
    "repo_provider_mirror_outbox": ("repo_bi",),
    "repo_provider_mirrors": ("repo_bi",),
    "repo_remote_forks": ("repo_bi", "url_bi"),
    "repo_security_scan_leases": ("repo_bi", "lease_token_bi"),
    "repo_security_scan_reviews": ("repo_bi", "reviewer_bi"),
    "repo_security_scans": ("repo_bi",),
    "repo_shares": ("repo_bi", "grantee_bi"),
    "repo_source_authorities": ("repo_bi",),
    "repo_stars": ("repo_bi", "account_bi"),
    "repo_state_history": ("key_bi",),
    "repo_terms_flags": ("repo_bi", "updated_by_bi"),
    "repo_view_totals": ("repo_bi",),
    "repo_views": ("repo_bi",),
    "repo_webhook_deliveries": ("repo_bi",),
    "repo_webhook_outbox": ("repo_bi",),
    "repo_webhook_seeds": ("repo_bi",),
    "repo_webhook_sequence": ("repo_bi",),
    "repo_webhooks": ("repo_bi",),
    "repositories": ("key_bi", "owner_bi"),
    "repository_fork_jobs": ("target_node_bi", "target_repo_bi", "source_repo_bi", "actor_bi"),
    "repository_imports": ("owner_bi",),
    "repository_logo_suggestions": ("proposer_bi", "reviewed_by_bi"),
    "repository_migration_jobs": ("owner_bi",),
    "repository_mirror_volunteers": ("operator_bi",),
    "reward_node_observations": ("node_bi",),
    "reward_wallet_reminders": ("recipient_bi",),
    "role_grants": ("account_bi", "scope_bi", "granted_by_bi"),
    "security_advisories": ("repo_bi",),
    "security_advisory_comments": ("repo_bi",),
    "security_advisory_credits": ("repo_bi", "user_bi"),
    "security_advisory_members": ("repo_bi", "member_bi"),
    "security_advisory_settings": ("repo_bi",),
    "sensitive_audit_log": ("actor_bi",),
    "signup_rate": ("ip_bi",),
    "thread_subscriptions": ("thread_bi", "subscriber_bi"),
    "tip_notice": ("from_bi", "to_bi"),
    "users": ("user_bi", "email_bi", "ip_bi", "discord_bi"),
    "workflow_credentials": ("owner_bi",),
    "workflow_runs": ("owner_bi",),
    "workflows": ("owner_bi",),
    "world_build_board_items": ("updated_by_bi", "completed_by_bi"),
    "world_element_purchases": ("account_bi",),
    "world_events": ("created_by_bi",),
    "world_fediverse_instances": ("created_by_bi",),
    "world_fediverse_mention_moderation": ("actor_bi",),
    "world_fediverse_mentions": ("remote_id_bi", "repo_bi"),
    "world_floor_note_replies": ("account_bi",),
    "world_floor_notes": ("account_bi",),
    "world_inactive_presence": ("account_bi",),
    "world_lobby_links": ("account_bi", "url_bi"),
    "world_manual_blocks": ("created_by_bi",),
    "world_media_items": ("added_by_bi",),
    "world_media_playback": ("changed_by_bi",),
    "world_media_roles": ("account_bi", "granted_by_bi"),
    "world_media_schedules": ("created_by_bi",),
    "world_media_spaces": ("owner_bi",),
    "world_office_attendance": ("account_bi",),
    "world_office_marketing_checkins": ("org_bi", "account_bi"),
    "world_office_marketing_initiatives": ("org_bi", "source_bi", "created_by_bi"),
    "world_office_marketing_proofs": ("org_bi", "account_bi", "url_bi"),
    "world_office_marketing_tasks": ("org_bi", "assignee_bi", "active_assignee_bi", "created_by_bi"),
    "world_qa_reviews": ("account_bi",),
    "world_user_activity": ("account_bi",),
}

# Identity columns: lookups by name (login, org and repository routing) go
# through these. An unrecoverable value here aborts the run (the account,
# org or repository would become unreachable) unless --allow-orphans.
STRICT_BI = {
    ("users", "user_bi"),
    ("nodes", "node_bi"),
    ("orgs", "org_bi"),
    ("repositories", "key_bi"),
}

# Columns that are not named *_bi but hold a blind index (full 64-hex HMAC
# under the blind-index key). Treated exactly like a *_bi column.
TOKEN_COLUMNS = {
    "feedback": ("ip_hash",),                   # blind_index(ip)
    "attribution_ip_hourly": ("ip_hash",),      # "attribution-ip-v1:<day>:<ip>"
    "note_views": ("viewer_key",),              # "note-view-v1\n<note>\n<ip>\n<ua>"
    "blog_post_view_seen": ("viewer_key",),     # "blog-post-unique-v1:..."
    "repo_views": ("viewer_key",),              # "repo-view-v1\n<owner/repo>\n<account|visitor>..."
    "sensitive_audit_log": ("target_token",),   # "audit:<type>:<target>"
    "world_manual_blocks": ("subject_token",),  # "world-manual-{ip,agent}-v1:<day>:..."
}

# Columns known to hold encrypt_row ciphertext (DATA_KEY AES-GCM). The tool
# does not rely on this list -- it trial-decrypts every text value -- but it
# reports ciphertext found anywhere else, and the coverage test keeps it in
# step with the schema.
CIPHER_COLUMNS = {
    t: ("data",) for t in (
        "about_inbox", "account_ssh_keys", "admin_signup_referral_code",
        "affiliate_leads", "affiliates",
        "agent_prompts", "ap_actors", "ap_comments", "ap_digest_queues",
        "ap_objects", "ap_org_digest_settings", "ap_outbox",
        "ap_repo_settings", "ap_service_keys", "apns_device_tokens",
        "branch_delete_jobs", "branch_op_inbox", "branch_op_results",
        "bug_reports", "calendar_events",
        "chat_channel_invites", "chat_channel_members",
        "chat_channel_webhooks", "chat_channels",
        "chat_direct_conversations", "chat_direct_media",
        "chat_direct_media_parts", "chat_direct_participants",
        "contributor_invitation_abuse", "contributor_invitation_provenance",
        "contributor_invitations", "discord_bug_reports", "discussion_inbox",
        "email_footers",
        "hosted_repos",
        "issue_bounty", "issue_inbox", "link_codes",
        "marketing_email_campaigns", "microjob_claims",
        "nodes", "note_versions", "notes", "notifications", "org_agent_jobs",
        "org_agent_sessions", "org_bot_tokens",
        "organization_discord_connectors",
        "organization_discord_oauth_grants",
        "organization_discord_oauth_states", "organization_task_attachments",
        "organization_task_checkins", "organization_task_events",
        "organization_task_qa_reviews", "organization_task_responses",
        "organization_tasks", "orgs", "patreon_account_links",
        "patreon_oauth_states", "pending_verifications",
        "plugin_race_entrants", "plugin_race_plugins",
        "profile_contribution_days", "profile_contribution_projects",
        "public_catalog_snapshot", "pull_inbox", "pull_submission_receipts",
        "quarantined_nodes",
        "quarantined_users", "referral_attributions", "referral_click_audit",
        "relay_self", "repo_action_jobs", "repo_agents", "repo_alert_settings",
        "repo_device_mirrors", "repo_media", "repo_provider_mirror_outbox",
        "repo_provider_mirrors", "repo_security_scan_leases",
        "repo_security_scan_reviews", "repo_security_scans", "repo_shares",
        "repo_source_authorities", "repo_terms_flags", "repo_webhook_outbox",
        "repo_webhooks", "repositories", "repository_fork_jobs",
        "repository_imports", "repository_logo_suggestions",
        "repository_migration_jobs", "repository_migration_objects",
        "repository_migration_runs", "security_advisories",
        "security_advisory_comments", "security_advisory_credits",
        "security_advisory_members", "security_reports",
        "thread_subscriptions", "users", "workflow_credentials",
        "workflow_runs", "workflows", "world_fediverse_mentions",
        "world_floor_note_replies", "world_floor_notes",
        "world_inactive_presence", "world_lobby_links",
        "world_office_marketing_initiatives", "world_office_marketing_proofs")
}
CIPHER_COLUMNS["ad_network_ads"] = ("owner_sealed",)
CIPHER_COLUMNS["world_element_purchases"] = ("deposit_secret",)
CIPHER_COLUMNS["not_found_paths"] = ("last_client",)
CIPHER_COLUMNS["probe_bans"] = ("client",)

# Client-sealed chat envelopes under DATA_KEY-derived room passphrases.
CHAT_TABLE = ("chat_history", "body")

# Blind-index plaintext families (the argument of blind_index(env, ...)).
# Each prefix is tried in front of every harvested identifier.
BI_PREFIXES = (
    "org:", "link:", "apns:", "org-bot-token:", "security-scan-lease:",
    "chat-invite-token:",
    "referral-user-agent:", "referral-client-v1:", "profile-project:",
    "patreon-member-v1:", "hackathon:", "discord-bug-message:",
    "discord-bug-channel:", "desktop-error:", "client-error:",
    "bug-reporter:account:", "bug-reporter:address:", "ap-mention:",
    "ap-context:", "ap-comment:", "ap-digest:", "audit:database_request:",
    "reward-operator:", "reward-recipient:", "ssh-public-key:",
    "private-repo-device:", "admin-signup-referral-code-v1:",
    "repo-alert-settings:", "repo-feature-settings:", "ap-repo-settings:", "remote-fork-url:", "pull-batch-target:", "issue-inbox-claim:",
    "collaboration-inbox-claim:discussion_inbox:",
    "collaboration-inbox-claim:pull_inbox:",
    "password-reset-requester-v1\n", "agent-guestbook-rate-v1\n",
    "ap-actor:user:", "ap-actor:repo:", "ap-actor:instance:",
    "ap-actor:org:",
)
# "<owner>/<repo>" families.
REPO_PREFIXES = ("", "repo-alert-settings:", "repo-feature-settings:", "ap-repo-settings:", "ap-digest:", "remote-forks:", "pull-review-ledger:")
# Families whose argument embeds another blind index.
NESTED_PREFIXES = (
    "password-change-v1\n", "password-reset-account-v1\n", "login-source:",
)
BI_CONSTANTS = ("", "bug-reporter:anonymous")

OWNER_KEYS = ("owner", "owner_name", "ownerName", "node", "node_owner",
              "nodeOwner", "repo_owner", "repoOwner", "fork_owner",
              "source_owner", "sourceOwner", "org", "org_name", "orgName",
              "account", "host_owner", "hostOwner", "target_node")
REPO_KEYS = ("repo", "repo_name", "repoName", "name", "fork_name",
             "source_name", "sourceName", "repository", "source_repo")
NUMBER_KEYS = ("number", "issue_number", "issueNumber", "pull_number",
               "pullNumber", "subject_number", "ref")
KIND_KEYS = ("kind", "source", "type", "subjectKind", "subject_kind")
THREAD_SOURCES = ("issue", "issues", "pull", "pulls", "pr", "discussion",
                  "discussions")
AP_CONTEXT_KINDS = ("issue", "pull", "discussion", "commit", "release")

MAX_HARVEST_LEN = 512
HEX64 = re.compile(r"^[0-9a-f]{64}$")
# An account, node or org name (entry.py valid_node_name-like).
ACCOUNT_LIKE = re.compile(r"^[a-z0-9][a-z0-9_.-]{0,63}$")
HEX64_IN = re.compile(r"(?<![0-9a-f])[0-9a-f]{64}(?![0-9a-f])")
B64_CELL = re.compile(r"^[A-Za-z0-9+/]{40,}={0,2}$")
INSECURE_KEYS = frozenset({"", "forkmesh-dev-data-key",
                           "forkmesh-dev-data-key-change-me"})
PUBLIC_WORLD_GENERAL_ROOM = "world-general"
REPO_ROOM_KEY = re.compile(r"^repo:([^/]+)/([^:]+):room:(.+)$")
LEGACY_ROOM_KEY = re.compile(r"^legacy:room:(.+)$")
CHANNEL_ROOM_KEY = re.compile(r"^chat-channel:([^:]+):v(\d+)$")
DIRECT_ROOM_KEY = re.compile(r"^chat-direct:([^:]+):v(\d+)$")


class RekeyError(RuntimeError):
    pass


# --------------------------------------------------------------------------
# Crypto: byte-compatible with app/src/entry.py (proved in
# app/tests/host/test_next_rekey.py against the real functions).


class Keyset:
    """Every DATA_KEY derivation the database depends on. Never printable."""

    def __init__(self, secret, label):
        secret = str(secret or "").strip()
        if secret in INSECURE_KEYS or len(secret) < 16:
            raise RekeyError("%s key is empty, a placeholder or too short" % label)
        if "\n" in secret or "\r" in secret or "\0" in secret:
            raise RekeyError("%s key file must hold one line" % label)
        self.label = label
        self._secret = secret
        # _data_key: AES-GCM key = SHA-256(DATA_KEY).
        self._aes = AESGCM(hashlib.sha256(secret.encode()).digest())
        # _hmac_key: HMAC-SHA256 key = SHA-256(DATA_KEY + ":blind-index").
        self._bi_key = hashlib.sha256((secret + ":blind-index").encode()).digest()
        self._chat_keys = {}

    def __repr__(self):
        return "<Keyset %s>" % self.label

    __str__ = __repr__

    def same_as(self, other):
        return hmac.compare_digest(self._secret.encode(), other._secret.encode())

    def fingerprint(self):
        """The vault key_id (docs/VAULT.md), shown by next_vault.py status."""
        try:
            sys.path.insert(0, str(Path(__file__).resolve().parents[1]
                                   / "app" / "host"))
            from forkmesh_host.vault_crypto import VaultKey
            return VaultKey(self._secret).key_id
        except Exception:
            return "unavailable"

    # blind_index(env, value)
    def bi(self, value):
        norm = str(value if value is not None else "").strip().lower().encode()
        return hmac.digest(self._bi_key, norm, "sha256").hex()

    # encrypt_row / decrypt_row (on the exact JSON bytes)
    def seal(self, plaintext):
        iv = os.urandom(12)
        return base64.b64encode(iv + self._aes.encrypt(iv, plaintext, None)).decode()

    def open(self, stored):
        if not isinstance(stored, str) or len(stored) % 4 or not B64_CELL.match(stored):
            return None
        try:
            blob = base64.b64decode(stored, validate=True)
        except (binascii.Error, ValueError):
            return None
        if len(blob) < 12 + 16 + 1:
            return None
        try:
            return self._aes.decrypt(blob[:12], blob[12:], None)
        except InvalidTag:
            return None

    # _room_chat_passphrase / _chat_channel_passphrase / _chat_direct_passphrase
    def _passphrase(self, suffix):
        return hashlib.sha256((self._secret + suffix).encode()).hexdigest()

    def room_passphrase(self, owner, repo, room):
        scope = owner.lower() + "/" + repo.lower()
        if scope == "mainnode/forkmesh" and room.lower() == PUBLIC_WORLD_GENERAL_ROOM:
            suffix = ":room-chat-passphrase-public-world-general-v1"
        elif scope == "mainnode/forkmesh":
            suffix = ":room-chat-passphrase-v1"
        else:
            suffix = ":room-chat-passphrase-v2:" + scope
        return self._passphrase(suffix)

    def channel_passphrase(self, channel_id, version):
        return self._passphrase(":private-chat-channel-v1:%s:%d"
                                % (channel_id, int(version)))

    def direct_passphrase(self, conversation_id, version):
        return self._passphrase(":chat-direct-passphrase-v1:%s:v%d"
                                % (conversation_id, int(version)))

    def chat_candidates(self, room_key):
        """(passphrase, salt room) pairs a client may have used for a row."""
        match = CHANNEL_ROOM_KEY.match(room_key)
        if match:
            return [(self.channel_passphrase(match.group(1), match.group(2)), room_key)]
        match = DIRECT_ROOM_KEY.match(room_key)
        if match:
            return [(self.direct_passphrase(match.group(1), match.group(2)), room_key)]
        match = REPO_ROOM_KEY.match(room_key)
        if match:
            owner, repo, room = match.groups()
        else:
            match = LEGACY_ROOM_KEY.match(room_key)
            if not match:
                return []
            owner, repo, room = "mainnode", "forkmesh", match.group(1)
        phrase = self.room_passphrase(owner, repo, room)
        salts = []
        for salt in (room.lower(), room):
            if salt not in salts:
                salts.append(salt)
        return [(phrase, salt) for salt in salts]

    def chat_cipher(self, passphrase, salt_room):
        """_chat_encrypted_envelope / chat-crypto.js derivePassphraseKey."""
        cached = self._chat_keys.get((passphrase, salt_room))
        if cached is None:
            salt = hashlib.sha256(("ForkMesh room:" + salt_room).encode()).digest()[:16]
            key = hashlib.pbkdf2_hmac("sha256", passphrase.encode(), salt, 210000, 32)
            cached = self._chat_keys[(passphrase, salt_room)] = AESGCM(key)
        return cached


def chat_envelope(text):
    if not isinstance(text, str) or not text.lstrip().startswith("{"):
        return None
    try:
        envelope = json.loads(text)
    except ValueError:
        return None
    if (isinstance(envelope, dict) and envelope.get("kind") == "cipher"
            and envelope.get("v") == 1 and all(
                isinstance(envelope.get(k), str) for k in ("nonce", "tag", "body"))):
        return envelope
    return None


def chat_open(keyset, room_key, envelope):
    """Returns (plaintext, candidate index) or (None, -1)."""
    try:
        nonce = base64.b64decode(envelope["nonce"])
        sealed = base64.b64decode(envelope["body"]) + base64.b64decode(envelope["tag"])
    except (binascii.Error, ValueError):
        return None, -1
    for index, (phrase, salt) in enumerate(keyset.chat_candidates(room_key)):
        try:
            return keyset.chat_cipher(phrase, salt).decrypt(nonce, sealed, None), index
        except (InvalidTag, ValueError):
            continue
    return None, -1


def chat_seal(keyset, room_key, index, plaintext, envelope):
    phrase, salt = keyset.chat_candidates(room_key)[index]
    nonce = os.urandom(12)
    combined = keyset.chat_cipher(phrase, salt).encrypt(nonce, plaintext, None)
    out = dict(envelope)
    out["nonce"] = base64.b64encode(nonce).decode()
    out["tag"] = base64.b64encode(combined[-16:]).decode()
    out["body"] = base64.b64encode(combined[:-16]).decode()
    return json.dumps(out, separators=(",", ":"), ensure_ascii=False)


# --------------------------------------------------------------------------
# Files and keys


def config_dir():
    import next_common
    return next_common.config_dir()


def default_work_root():
    import next_common
    return next_common.data_dir() / "rekey"


def read_key(path, label):
    path = Path(path)
    try:
        info = path.stat()
    except OSError:
        raise RekeyError("%s key file %s is missing" % (label, path)) from None
    if not stat.S_ISREG(info.st_mode):
        raise RekeyError("%s key file %s is not a regular file" % (label, path))
    if info.st_mode & 0o077:
        raise RekeyError("%s key file %s is readable by group/other; chmod 0400 it"
                         % (label, path))
    return Keyset(path.read_text("utf-8"), label)


def read_key_from_env_file(spec, label):
    path_text, sep, name = str(spec).rpartition(":")
    if not sep or not path_text or not re.fullmatch(r"[A-Z][A-Z0-9_]*", name):
        raise RekeyError("%s key must be FILE:NAME (e.g. .env.production:DATA_KEY)"
                         % label)
    path = Path(path_text).expanduser()
    try:
        info = path.stat()
    except OSError:
        raise RekeyError("%s key file %s is missing" % (label, path)) from None
    if not stat.S_ISREG(info.st_mode):
        raise RekeyError("%s key file %s is not a regular file" % (label, path))
    if info.st_mode & 0o077:
        raise RekeyError("%s key file %s is readable by group/other; chmod 0600 it"
                         % (label, path))
    found = []
    for line in path.read_text("utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        if key.strip() == name:
            value = value.strip()
            if len(value) >= 2 and value[0] == value[-1] and value[0] in "'\"":
                value = value[1:-1]
            found.append(value)
    if len(found) != 1:
        raise RekeyError("%s key file %s must have exactly one %s= line (found %d)"
                         % (label, path, name, len(found)))
    return Keyset(found[0], label)


def load_old_key(args):
    if getattr(args, "old_key_env", None):
        if args.old_key:
            raise RekeyError("give --old-key or --old-key-env, not both")
        return read_key_from_env_file(args.old_key_env, "old")
    return read_key(args.old_key or config_dir() / "DATA_KEY", "old")


def load_new_key(args):
    if getattr(args, "new_key_env", None):
        if args.new_key:
            raise RekeyError("give --new-key or --new-key-env, not both")
        return read_key_from_env_file(args.new_key_env, "new")
    return read_key(args.new_key or config_dir() / "DATA_KEY.next-original", "new")


def refuse_on_node():
    if Path("/etc/forkmesh-next").exists() or os.environ.get("FORKMESH_VAULT"):
        raise RekeyError("this looks like a Next node; next_rekey.py runs only "
                         "on the operator desktop")


def make_work_dir(root):
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(root, 0o700)
    return Path(tempfile.mkdtemp(prefix="run-", dir=str(root)))


def remove_work_dir(root, work):
    # Only the mkdtemp directory this run created, and only inside the root.
    root, work = Path(root).resolve(), Path(work).resolve()
    if work.parent == root and work.name.startswith("run-"):
        forkmesh_safe_delete.remove_tree(work, root, ignore_errors=True)


def create_private_file(path):
    fd = os.open(str(path), os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    os.close(fd)


def open_input(path, work):
    """A read-only sqlite3 connection on the backup (gunzipped into work)."""
    path = Path(path)
    if not path.is_file():
        raise RekeyError("input %s does not exist" % path)
    if path.suffix == ".gz":
        target = work / "input.sqlite3"
        create_private_file(target)
        with gzip.open(path, "rb") as src, open(target, "wb") as dst:
            shutil.copyfileobj(src, dst, 1 << 20)
        path = target
    conn = sqlite3.connect("file:%s?mode=ro" % path, uri=True)
    conn.execute("PRAGMA temp_store=MEMORY")
    return conn


def qi(name):
    return '"' + str(name).replace('"', '""') + '"'


def user_tables(conn):
    return [r[0] for r in conn.execute(
        "SELECT name FROM sqlite_master WHERE type='table' "
        "AND name NOT LIKE 'sqlite_%' ORDER BY name")]


def columns_of(conn, table):
    return [r[1] for r in conn.execute("PRAGMA table_info(%s)" % qi(table))]


# --------------------------------------------------------------------------
# The walk: one traversal for harvest, write and verify, so they agree.


def is_bi_key(key):
    return isinstance(key, str) and (key.endswith("_bi") or key.endswith("Bi"))


def dump_like(original, obj):
    compact = re.search(r"[,:] ", original) is None
    ascii_only = all(ord(ch) < 128 for ch in original)
    if compact:
        return json.dumps(obj, separators=(",", ":"), ensure_ascii=ascii_only)
    return json.dumps(obj, ensure_ascii=ascii_only)


class Rekeyer:
    def __init__(self, old, new):
        self.old, self.new = old, new
        self.mode = "harvest"
        # harvest
        self.strings = set()
        self.extra = set()          # composite plaintexts from dict hooks
        self.pairs = set()          # (owner, repo)
        self.observed = set()       # every 64-hex token anywhere
        self.bi_positions = set()   # 64-hex values in *_bi / token positions
        self.strict_seen = defaultdict(set)   # value -> {(table, col)}
        self.strict_live = set()
        self.dead_strict_orphans = 0
        self._row_dead = False
        self.device_rows = set()    # (account_bi, pubkey)
        self.receipt_rows = set()   # (source_repo_bi, snapshot_hash)
        self.audit_types = set()
        # results
        self.mapped = {}            # old bi -> new bi
        self.pseudo = {}            # old bi -> random stand-in
        self.stats = defaultdict(Counter)
        self.unregistered_cipher = set()
        self.unregistered_bi = set()
        self.table = ""
        self.column = ""

    # -- harvest helpers -------------------------------------------------
    def _note_string(self, value):
        if len(value) <= MAX_HARVEST_LEN:
            self.strings.add(value.strip().lower())
            if "/" in value:
                segs = [s for s in value.split("/") if s]
                if len(segs) <= 24:
                    for a, b in zip(segs, segs[1:]):
                        self.pairs.add((a.lower(), b.lower()))

    def _dict_hook(self, record):
        owners, repos, numbers, kinds = set(), set(), set(), set()
        for key, value in record.items():
            if isinstance(value, bool) or value is None:
                continue
            text = str(value).strip().lower()
            if not text or len(text) > 200:
                continue
            if key in OWNER_KEYS:
                owners.add(text)
            if key in REPO_KEYS:
                if "/" in text:
                    a, _, b = text.partition("/")
                    owners.add(a)
                    repos.add(b)
                else:
                    repos.add(text)
            if key in NUMBER_KEYS:
                numbers.add(text)
            if key in KIND_KEYS and isinstance(value, str):
                kinds.add(text)
        if not owners or not repos:
            return
        for owner in owners:
            for repo in repos:
                self.pairs.add((owner, repo))
                base = owner + "/" + repo
                for number in numbers:
                    self.extra.add(base + "#" + number)
                    for kind in kinds | {"pr"}:
                        self.extra.add(base + "#" + kind + "-" + number)
                    for source in kinds | set(THREAD_SOURCES):
                        self.extra.add("thread:%s:%s:%s" % (base, source, number))
                    for kind in kinds | set(AP_CONTEXT_KINDS):
                        self.extra.add("ap-context:%s#%s#%s" % (base, kind, number))

    # -- blind-index positions ---------------------------------------------
    def bi_value(self, value, strict=False):
        if not isinstance(value, str) or not HEX64.match(value):
            if value not in (None, "") and self.mode == "write":
                self.stats[self.table]["bi_non_hex_kept"] += 1
            return value
        if self.mode == "harvest":
            self.observed.add(value)
            self.bi_positions.add(value)
            if strict:
                self.strict_seen[value].add((self.table, self.column))
                if not self._row_dead:
                    self.strict_live.add(value)
            return value
        if self.mode == "verify":
            self.observed.add(value)
            return value
        if value in self.mapped:
            self.stats[self.table]["bi_recomputed"] += 1
            return self.mapped[value]
        self.stats[self.table]["bi_pseudonymised"] += 1
        return self.pseudo.setdefault(value, secrets.token_hex(32))

    def sub_hex(self, text):
        if self.mode != "write":
            for match in HEX64_IN.findall(text):
                self.observed.add(match)
            return text

        def swap(match):
            value = match.group(0)
            if value in self.mapped:
                self.stats[self.table]["bi_embedded_recomputed"] += 1
                return self.mapped[value]
            if value in self.pseudo:
                self.stats[self.table]["bi_embedded_pseudonymised"] += 1
                return self.pseudo[value]
            return value
        return HEX64_IN.sub(swap, text)

    # -- values ------------------------------------------------------------
    def text(self, value):
        """A string anywhere: a whole cell, a JSON leaf, a decrypted leaf."""
        plain = self.old.open(value)
        if plain is not None:
            return self._cipher(plain)
        if self.mode == "verify":
            plain = self.new.open(value)
            if plain is not None:
                return self._cipher(plain)
        if self.mode == "harvest":
            self._note_string(value)
        stripped = value.lstrip()
        if stripped[:1] in ("{", "["):
            try:
                obj = json.loads(value)
            except ValueError:
                obj = None
            if isinstance(obj, (dict, list)):
                out, changed = self.walk(obj)
                return dump_like(value, out) if changed else value
        return self.sub_hex(value)

    def _cipher(self, plain):
        stats = self.stats[self.table]
        if self.mode == "verify":
            stats["cipher_new"] += 1
        elif self.mode == "write":
            stats["cipher_reencrypted"] += 1
            if self.column not in CIPHER_COLUMNS.get(self.table, ()):
                self.unregistered_cipher.add((self.table, self.column))
        try:
            obj = json.loads(plain)
        except ValueError:
            obj = None
        if isinstance(obj, (dict, list)):
            out, changed = self.walk(obj)
            if changed:
                plain = json.dumps(out).encode()
        elif isinstance(obj, str):
            out = self.text(obj)
            if out != obj:
                plain = json.dumps(out).encode()
        if self.mode == "write":
            return self.new.seal(plain)
        return None  # harvest / verify: the cell value is not used

    def walk(self, obj, key=None):
        if isinstance(obj, dict):
            if self.mode == "harvest":
                self._dict_hook(obj)
            changed, out = False, {}
            for k, v in obj.items():
                nk = k
                if isinstance(k, str):
                    if self.mode == "harvest":
                        self._note_string(k)
                    nk = self.sub_hex(k)
                nv, c = self.walk(v, k)
                out[nk] = nv
                changed = changed or c or nk != k
            return out, changed
        if isinstance(obj, list):
            changed, out = False, []
            for item in obj:
                nv, c = self.walk(item, key)
                out.append(nv)
                changed = changed or c
            return out, changed
        if isinstance(obj, str):
            if is_bi_key(key) and HEX64.match(obj):
                nv = self.bi_value(obj)
            else:
                nv = self.text(obj)
                if nv is None:  # a nested ciphertext in harvest/verify mode
                    nv = obj
            return nv, nv != obj
        if self.mode == "harvest" and isinstance(obj, int) and not isinstance(obj, bool):
            self.strings.add(str(obj))
        return obj, False

    # -- rows --------------------------------------------------------------
    def row(self, table, cols, values):
        """Returns the new row, or None to drop it."""
        self.table = table
        bi_cols = set(BI_COLUMNS.get(table, ())) | set(TOKEN_COLUMNS.get(table, ()))
        for col in cols:
            if col.endswith("_bi") and col not in bi_cols:
                self.unregistered_bi.add((table, col))
                bi_cols.add(col)
        record = dict(zip(cols, values))
        if self.mode == "harvest":
            self._row_dead = self._dead_record(table, record)
            self._dict_hook(record)
            self._row_hooks(table, record)
        if table == CHAT_TABLE[0] and self.mode != "harvest":
            return self._chat_row(cols, values)
        out = []
        for col, value in zip(cols, values):
            self.column = col
            if col in bi_cols:
                value = self.bi_value(value, strict=(table, col) in STRICT_BI)
            elif isinstance(value, str):
                new = self.text(value)
                value = value if new is None else new
            elif self.mode == "harvest" and isinstance(value, int):
                self.strings.add(str(value))
            out.append(value)
        return out

    def _dead_record(self, table, record):
        if not any(t == table for t, _ in STRICT_BI):
            return False
        data = record.get("data")
        if not isinstance(data, str) or not data or data[:1] in "{[":
            return False
        return self.old.open(data) is None

    def _row_hooks(self, table, record):
        if table == "account_devices" and record.get("account_bi") and record.get("pubkey"):
            self.device_rows.add((str(record["account_bi"]), str(record["pubkey"])))
        if table == "profile_contribution_receipts" and record.get("source_repo_bi"):
            self.receipt_rows.add((str(record["source_repo_bi"]),
                                   str(record.get("snapshot_hash") or "")))
        if table == "sensitive_audit_log" and record.get("target_type"):
            self.audit_types.add(str(record["target_type"]))

    def _chat_row(self, cols, values):
        idx_body, idx_room = cols.index(CHAT_TABLE[1]), cols.index("room_key")
        body, room_key = values[idx_body], str(values[idx_room] or "")
        stats = self.stats[self.table]
        envelope = chat_envelope(body)
        if envelope is None:
            stats["chat_not_cipher_kept"] += 1
            return list(values)
        if self.mode == "verify":
            old_plain, _ = chat_open(self.old, room_key, envelope)
            new_plain, _ = chat_open(self.new, room_key, envelope)
            stats["chat_old_opens"] += old_plain is not None
            stats["chat_new_opens"] += new_plain is not None
            stats["chat_unopenable"] += new_plain is None
            return list(values)
        plain, index = chat_open(self.old, room_key, envelope)
        if plain is None:
            stats["chat_undecryptable_dropped"] += 1
            return None
        out = list(values)
        out[idx_body] = chat_seal(self.new, room_key, index, plain, envelope)
        stats["chat_reencrypted"] += 1
        return out

    # -- mapping -----------------------------------------------------------
    def build_map(self):
        mapped, observed = self.mapped, self.observed
        old_bi, new_bi = self.old.bi, self.new.bi

        def offer(old_plain, new_plain=None):
            value = old_bi(old_plain)
            if value in observed and value not in mapped:
                mapped[value] = new_bi(old_plain if new_plain is None else new_plain)

        prefixes = BI_PREFIXES + tuple(
            "audit:" + re.sub(r"[^a-z0-9_.-]", "", t.strip().lower())[:40] + ":"
            for t in sorted(self.audit_types))
        for constant in BI_CONSTANTS:
            offer(constant)
        for plain in self.strings | self.extra:
            offer(plain)
            for prefix in prefixes:
                offer(prefix + plain)
        for owner, repo in self.pairs:
            for prefix in REPO_PREFIXES:
                offer(prefix + owner + "/" + repo)
            offer("ap-actor:repo:" + owner + "." + repo)
            offer("repo:" + owner + "/" + repo)
        # A repository key can be "<host owner>/<repo>" (entry.py: the node
        # hosting it, an org alias) rather than the owner its record names,
        # so also try every harvested account-like name as the owner of every
        # known repository name. Only the bare "owner/repo" form, and only
        # while unmatched strict positions remain.
        if any(v not in mapped for v in self.strict_seen):
            # Only names that are real identities here: their own blind
            # index occurs in the database (an account, node or org).
            names = {s for s in self.strings
                     if ACCOUNT_LIKE.match(s) and old_bi(s) in observed}
            names |= {o for o, _ in self.pairs}
            repos = {r for _, r in self.pairs}
            for owner in names:
                for repo in repos:
                    offer(owner + "/" + repo)
        # Families that embed an (already recovered) blind index.
        for old_inner, new_inner in list(mapped.items()):
            for prefix in NESTED_PREFIXES:
                offer(prefix + old_inner, prefix + new_inner)
        for account_bi, pubkey in self.device_rows:
            if account_bi in mapped:
                offer(account_bi + ":" + pubkey, mapped[account_bi] + ":" + pubkey)
        for source_bi, snapshot in self.receipt_rows:
            if source_bi in mapped:
                offer("profile-generation:%s:%s" % (source_bi, snapshot),
                      "profile-generation:%s:%s" % (mapped[source_bi], snapshot))
        for value in self.bi_positions:
            if value not in mapped:
                self.pseudo[value] = secrets.token_hex(32)
        dead = {v for v in self.strict_seen
                if v not in mapped and v not in self.strict_live}
        self.dead_strict_orphans = len(dead)
        return sorted({pos for value, positions in self.strict_seen.items()
                       if value not in mapped and value not in dead
                       for pos in positions})

    def clear_plaintext(self):
        """Drop the harvested identifiers once the map is built."""
        self.strings.clear()
        self.extra.clear()
        self.pairs.clear()
        self.device_rows.clear()
        self.receipt_rows.clear()


# --------------------------------------------------------------------------
# Passes


def scan(conn, rekeyer, tables):
    for table in tables:
        cols = columns_of(conn, table)
        for values in conn.execute("SELECT * FROM %s" % qi(table)):
            rekeyer.row(table, cols, values)


def write_output(src, path, rekeyer, tables):
    dst = sqlite3.connect(str(path), isolation_level=None)
    try:
        for pragma in ("journal_mode=OFF", "synchronous=OFF",
                       "temp_store=MEMORY", "foreign_keys=OFF",
                       "secure_delete=ON"):
            dst.execute("PRAGMA " + pragma)
        dst.execute("BEGIN")
        for name, sql in src.execute(
                "SELECT name, sql FROM sqlite_master WHERE type='table' "
                "AND name NOT LIKE 'sqlite_%' ORDER BY rowid"):
            dst.execute(sql)
        for table in tables:
            cols = columns_of(src, table)
            stats = rekeyer.stats[table]
            insert = "INSERT INTO %s (%s) VALUES (%s)" % (
                qi(table), ", ".join(qi(c) for c in cols), ", ".join("?" * len(cols)))
            for values in src.execute("SELECT * FROM %s" % qi(table)):
                stats["rows_in"] += 1
                if table in DROP_TABLES:
                    stats["rows_dropped_invalidated"] += 1
                    continue
                out = rekeyer.row(table, cols, values)
                if out is None:
                    continue
                dst.execute(insert, out)
                stats["rows_out"] += 1
        has_seq = src.execute("SELECT 1 FROM sqlite_master WHERE name='sqlite_sequence'").fetchone()
        if has_seq and dst.execute(
                "SELECT 1 FROM sqlite_master WHERE name='sqlite_sequence'").fetchone():
            dst.execute("DELETE FROM sqlite_sequence")
            dst.executemany("INSERT INTO sqlite_sequence(name, seq) VALUES (?, ?)",
                            src.execute("SELECT name, seq FROM sqlite_sequence"))
        for sql, in src.execute(
                "SELECT sql FROM sqlite_master WHERE type IN ('index','trigger','view') "
                "AND sql IS NOT NULL ORDER BY CASE type WHEN 'index' THEN 0 "
                "WHEN 'view' THEN 1 ELSE 2 END, rowid"):
            dst.execute(sql)
        dst.execute("COMMIT")
    finally:
        dst.close()


def verify_file(path, old, new, forbidden=(), expected=None):
    """Checks a re-keyed file. Returns (problems, stats)."""
    conn = sqlite3.connect("file:%s?mode=ro" % path, uri=True)
    conn.execute("PRAGMA temp_store=MEMORY")
    problems = []
    try:
        if conn.execute("PRAGMA integrity_check").fetchone()[0] != "ok":
            problems.append("integrity_check failed")
        tables = user_tables(conn)
        checker = Rekeyer(old, new)
        checker.mode = "verify"
        checker.stats = defaultdict(Counter)
        old_opens = Counter()
        real_open = old.open

        def spy(value, _open=real_open):
            plain = _open(value)
            if plain is not None:
                old_opens[checker.table] += 1
            return None  # never follow an old-key ciphertext
        checker.old = _Spy(old, spy)
        for table in tables:
            cols = columns_of(conn, table)
            for values in conn.execute("SELECT * FROM %s" % qi(table)):
                checker.row(table, cols, values)
            if table in DROP_TABLES and conn.execute(
                    "SELECT 1 FROM %s LIMIT 1" % qi(table)).fetchone():
                problems.append("%s is not empty" % table)
        for table, count in sorted(old_opens.items()):
            problems.append("%s: %d value(s) still open under the old key" % (table, count))
        chat = checker.stats.get(CHAT_TABLE[0], Counter())
        if chat["chat_old_opens"]:
            problems.append("chat_history: %d envelope(s) open under the old room key"
                            % chat["chat_old_opens"])
        if chat["chat_unopenable"]:
            problems.append("chat_history: %d envelope(s) do not open under the new room key"
                            % chat["chat_unopenable"])
        if expected is not None:
            for table, stats in expected.items():
                want = stats.get("cipher_reencrypted", 0)
                got = checker.stats[table]["cipher_new"]
                if want != got:
                    problems.append("%s: %d ciphertext(s) re-encrypted but %d open "
                                    "under the new key" % (table, want, got))
        # No old blind index may remain. (1) Everything this run replaced.
        leftover = checker.observed.intersection(forbidden)
        if leftover:
            problems.append("%d old blind-index value(s) remain" % len(leftover))
        # (2) Recompute known identifiers (harvested from the re-keyed,
        # decrypted rows) under the OLD key: none may be present.
        sample = Rekeyer(new, new)
        sample.mode = "harvest"
        for table in tables:
            if table == CHAT_TABLE[0]:
                continue
            cols = columns_of(conn, table)
            for values in conn.execute("SELECT * FROM %s" % qi(table)):
                sample.row(table, cols, values)
        probes = 0
        hits = 0
        for plain in sample.strings:
            for prefix in ("",) + BI_PREFIXES:
                probes += 1
                if old.bi(prefix + plain) in checker.observed:
                    hits += 1
        for owner, repo in sample.pairs:
            for prefix in REPO_PREFIXES:
                probes += 1
                if old.bi(prefix + owner + "/" + repo) in checker.observed:
                    hits += 1
        if hits:
            problems.append("%d identifier(s) still indexed under the old key" % hits)
        sample.clear_plaintext()
        checker.stats["_verify"]["old_key_probes"] = probes
        fk = conn.execute("PRAGMA foreign_key_check").fetchall()
        checker.stats["_verify"]["foreign_key_violations"] = len(fk)
        return problems, checker.stats
    finally:
        conn.close()


class _Spy:
    """A Keyset stand-in whose open() only reports (verify pass)."""

    def __init__(self, keyset, opener):
        self._keyset, self.open = keyset, opener

    def __getattr__(self, name):
        return getattr(self._keyset, name)


# --------------------------------------------------------------------------
# Commands


def rekey(input_path, out_path, old, new, work_root, allow_orphans=False,
          strict_registry=False, out=sys.stdout):
    refuse_on_node()
    if old.same_as(new):
        raise RekeyError("old and new keys are the same")
    out_path = Path(out_path)
    if out_path.exists():
        raise RekeyError("output %s already exists" % out_path)
    os.umask(0o077)
    work = make_work_dir(work_root)
    os.environ["SQLITE_TMPDIR"] = str(work)
    try:
        src = open_input(input_path, work)
        try:
            tables = user_tables(src)
            rekeyer = Rekeyer(old, new)
            scan(src, rekeyer, tables)
            if strict_registry and rekeyer.unregistered_bi:
                raise RekeyError("unregistered *_bi columns: " + ", ".join(
                    "%s.%s" % pos for pos in sorted(rekeyer.unregistered_bi)))
            orphans = rekeyer.build_map()
            rekeyer.clear_plaintext()
            if orphans and not allow_orphans:
                raise RekeyError(
                    "blind indexes in identity columns whose plaintext was not "
                    "recovered (%s); review, then re-run with --allow-orphans "
                    "to replace them with pseudonyms" % ", ".join(
                        "%s.%s" % pos for pos in orphans))
            rekeyer.mode = "write"
            rekeyer.observed = set()
            staged = work / "rekeyed.sqlite3"
            create_private_file(staged)
            write_output(src, staged, rekeyer, tables)
        finally:
            src.close()
        forbidden = set(rekeyer.mapped) | set(rekeyer.pseudo)
        problems, vstats = verify_file(staged, old, new, forbidden, rekeyer.stats)
        summary = build_summary(rekeyer, vstats, problems, old, new)
        print_summary(summary, out)
        if problems:
            raise RekeyError("verification failed; no output written")
        try:
            os.link(staged, out_path)
        except OSError:
            create_private_file(out_path)
            shutil.copyfile(staged, out_path)
        os.chmod(out_path, 0o600)
        print("wrote %s (0600)" % out_path, file=out)
        return summary
    finally:
        remove_work_dir(work_root, work)


def build_summary(rekeyer, vstats, problems, old, new):
    tables = {}
    for table, stats in sorted(rekeyer.stats.items()):
        if table.startswith("_"):
            continue
        tables[table] = dict(stats)
    totals = Counter()
    for stats in tables.values():
        totals.update(stats)
    return {
        "tool_version": TOOL_VERSION,
        "old_key_id": old.fingerprint(),
        "new_key_id": new.fingerprint(),
        "tables": tables,
        "totals": dict(totals),
        "blind_indexes": {"recomputed_distinct": len(rekeyer.mapped),
                          "pseudonymised_distinct": len(rekeyer.pseudo),
                          "dead_row_strict_pseudonymised":
                              rekeyer.dead_strict_orphans},
        "dropped_tables": sorted(t for t in DROP_TABLES if t in tables),
        "unregistered_bi_columns": ["%s.%s" % p for p in sorted(rekeyer.unregistered_bi)],
        "unregistered_cipher_columns": ["%s.%s" % p for p in sorted(rekeyer.unregistered_cipher)],
        "verify": dict(vstats.get("_verify", {})),
        "problems": list(problems),
    }


def print_summary(summary, out):
    print("next_rekey: old key_id %s -> new key_id %s"
          % (summary["old_key_id"], summary["new_key_id"]), file=out)
    cols = ("rows_in", "rows_out", "rows_dropped_invalidated", "cipher_reencrypted",
            "bi_recomputed", "bi_pseudonymised", "bi_embedded_recomputed",
            "chat_reencrypted", "chat_undecryptable_dropped")
    print("%-40s %s" % ("table", " ".join("%9s" % c[:9] for c in cols)), file=out)
    for table, stats in summary["tables"].items():
        if not any(stats.get(c) for c in cols[2:]) and stats.get("rows_in") == stats.get("rows_out"):
            continue
        print("%-40s %s" % (table[:40], " ".join("%9d" % stats.get(c, 0) for c in cols)),
              file=out)
    print("%-40s %s" % ("TOTAL", " ".join("%9d" % summary["totals"].get(c, 0) for c in cols)),
          file=out)
    print("blind indexes: %(recomputed_distinct)d distinct recomputed, "
          "%(pseudonymised_distinct)d distinct pseudonymised" % summary["blind_indexes"],
          file=out)
    if summary["blind_indexes"].get("dead_row_strict_pseudonymised"):
        print("NOTE %d identity blind index(es) seen only on dead rows (their "
              "record opens under no key given) were pseudonymised"
              % summary["blind_indexes"]["dead_row_strict_pseudonymised"], file=out)
    for key in ("unregistered_bi_columns", "unregistered_cipher_columns"):
        if summary[key]:
            print("WARNING %s: %s" % (key, ", ".join(summary[key])), file=out)
    verify = summary["verify"]
    print("verify: %d old-key identifier probes, %d foreign-key violations"
          % (verify.get("old_key_probes", 0), verify.get("foreign_key_violations", 0)),
          file=out)
    for problem in summary["problems"]:
        print("PROBLEM: %s" % problem, file=out)
    if not summary["problems"]:
        print("verify: OK (all ciphertext opens only under the new key; no old "
              "blind index remains)", file=out)


def find_key(key_path, roots, out=sys.stdout):
    """Files under roots that contain the key, or its AES / blind-index keys.

    Prints file names only. The key file itself is skipped (by inode).
    """
    keyset = read_key(key_path, "search")
    secret = keyset._secret.encode()
    aes = hashlib.sha256(secret).digest()
    needles = [secret, aes, aes.hex().encode(), base64.b64encode(aes),
               keyset._bi_key, keyset._bi_key.hex().encode()]
    longest = max(len(n) for n in needles)
    own = os.stat(key_path)
    hits = []
    for root in roots:
        root = Path(root)
        if not root.exists():
            continue
        files = [root] if root.is_file() else (
            Path(d) / f for d, _dirs, names in os.walk(root) for f in names)
        for path in files:
            try:
                info = os.lstat(path)
                if not stat.S_ISREG(info.st_mode) or (
                        info.st_ino == own.st_ino and info.st_dev == own.st_dev):
                    continue
                with open(path, "rb") as handle:
                    tail = b""
                    while True:
                        chunk = handle.read(1 << 20)
                        if not chunk:
                            break
                        window = tail + chunk
                        if any(n in window for n in needles):
                            hits.append(path)
                            break
                        tail = window[-(longest - 1):]
            except OSError:
                continue
    for path in hits:
        print("contains key material: %s" % path, file=out)
    print("find-key: %d file(s) contain it" % len(hits), file=out)
    return hits


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("find-key")
    p.add_argument("--key", required=True, help="key FILE to look for")
    p.add_argument("paths", nargs="*",
                   help="default: the Next config, data and logs dirs and deploy/")
    for name in ("run", "verify"):
        p = sub.add_parser(name)
        p.add_argument("--in", dest="input", required=True,
                       help="SQLite file (.sqlite3 or .sqlite3.gz)")
        p.add_argument("--old-key", default=None,
                       help="old key FILE (default <config>/DATA_KEY)")
        p.add_argument("--old-key-env", default=None, metavar="FILE:NAME",
                       help="old key from the NAME= line of an env FILE, read "
                            "into memory only (production's .env.production)")
        p.add_argument("--new-key", default=None,
                       help="new key FILE (default <config>/DATA_KEY.next-original)")
        p.add_argument("--new-key-env", default=None, metavar="FILE:NAME",
                       help="new key from the NAME= line of an env FILE (rollback "
                            "to production's key), read into memory only")
        p.add_argument("--work-dir", default=None,
                       help="0700 work root (default ~/projects/forkmesh/data/fleet/rekey)")
        if name == "run":
            p.add_argument("--out", default=None,
                           help="output FILE (default <work-dir>/rekeyed-<UTC>.sqlite3)")
            p.add_argument("--allow-orphans", action="store_true")
            p.add_argument("--strict-registry", action="store_true")
            p.add_argument("--summary-json", default=None)
    args = parser.parse_args(argv)
    try:
        refuse_on_node()
        if args.command == "find-key":
            import next_common
            roots = args.paths or [config_dir(), next_common.data_dir(),
                                   next_common.logs_dir(),
                                   Path(__file__).resolve().parents[1] / "deploy"]
            return 1 if find_key(args.key, roots) else 0
        old = load_old_key(args)
        new = load_new_key(args)
        work_root = Path(args.work_dir) if args.work_dir else default_work_root()
        if args.command == "verify":
            problems, stats = verify_file(Path(args.input), old, new)
            print("verify: %d old-key identifier probes" % stats["_verify"]["old_key_probes"])
            for problem in problems:
                print("PROBLEM: %s" % problem)
            print("verify: %s" % ("FAILED" if problems else "OK"))
            return 1 if problems else 0
        out_path = Path(args.out) if args.out else work_root / (
            "rekeyed-%s.sqlite3" % dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
        summary = rekey(args.input, out_path, old, new, work_root,
                        allow_orphans=args.allow_orphans,
                        strict_registry=args.strict_registry)
        if args.summary_json:
            path = Path(args.summary_json)
            create_private_file(path)
            path.write_text(json.dumps(summary, indent=1, sort_keys=True) + "\n")
        return 0
    except RekeyError as error:
        print("next_rekey: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
