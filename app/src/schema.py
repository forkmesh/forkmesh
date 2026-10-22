"""D1 table/index DDL for the relay, split out of entry.py.

SCHEMA_STATEMENTS is the ordered list of idempotent CREATE TABLE / CREATE
INDEX statements ensure_schema() runs on first request. It is pure data with
no runtime dependency, so it lives in its own js-free sibling module the
Worker runtime bundles and entry.py re-imports.
"""

SCHEMA_STATEMENTS = [
    # The legacy accounts table (superseded by users/nodes, migration 0025) was
    # fully drained and dropped by migration 0042; make sure lazily-ensured DBs
    # lose it too.
    "DROP TABLE IF EXISTS accounts",
    # email_bi (blind index of the email) lets users log in by email, not just
    # node name (migration 0003). is_admin is an operator-settable flag and
    # username is the public name in plaintext, so an admin can be granted
    # directly in the DB: UPDATE users SET is_admin=1 WHERE username='alice'
    # (migration 0006).
    # ip_bi is the blind index (keyed HMAC) of the transient signup IP; the raw
    # address is not retained in this row or inside the encrypted `data` blob.
    # It lets anti-abuse count accounts sharing a source network identifier
    # without exposing the original address (migration 0015).
    # enable_outreach is an operator-settable flag, like is_admin, that used to
    # grant the retired founders /outreach console (migration 0040). Founders
    # outreach now ships as a marketplace package behind an organization
    # roster (migration 0177); the column stays for schema compatibility.
    # The UNIQUE email index is partial: one account per email (keyless nodes
    # have no email, so NULL/'' rows are excluded and unconstrained). Enforced
    # for existing DBs by migration 0041, which also removes any pre-existing
    # duplicate emails.
    # A pre-noncustodial deployment may still have donation_secret inside an
    # existing encrypted data blob. Live writers reject such a record until the
    # explicit offline migration scrubs it; new records contain public wallets
    # only.
    """CREATE TABLE IF NOT EXISTS users (
        user_bi TEXT PRIMARY KEY,
        data TEXT NOT NULL,
        email_bi TEXT,
        username TEXT,
        is_admin INTEGER NOT NULL DEFAULT 0,
        ip_bi TEXT,
        enable_outreach INTEGER NOT NULL DEFAULT 0,
        discord_bi TEXT,
        signup_origin TEXT NOT NULL DEFAULT 'unknown'
            CHECK (signup_origin IN ('job_flow','referral','organic','unknown')),
        early_ios_tester INTEGER NOT NULL DEFAULT 0
            CHECK (early_ios_tester IN (0, 1)))""",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_users_email ON users(email_bi) "
    "WHERE email_bi IS NOT NULL AND email_bi <> ''",
    "CREATE INDEX IF NOT EXISTS idx_users_ip ON users(ip_bi)",
    # Public/admin roster readers order by username or select the small admin
    # subset. Without these indexes, each bounded read still scans every user
    # and is billed by D1 for all of those rows (migration 0172).
    "CREATE INDEX IF NOT EXISTS idx_users_username ON users(username)",
    "CREATE INDEX IF NOT EXISTS idx_users_admin ON users(is_admin, user_bi)",
    "CREATE INDEX IF NOT EXISTS idx_users_early_ios_tester "
    "ON users(early_ios_tester, user_bi) WHERE early_ios_tester=1",
    # discord_bi is the blind index of the reporter's Discord *user id*, not
    # their handle: handles are renameable, the snowflake is not. It exists so
    # bug-report credit can be resolved from a Discord author without keeping a
    # separate mapping file. Unique where set, because one Discord account
    # belongs to exactly one ForkMesh user (migration 0148).
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_users_discord ON users(discord_bi) "
    "WHERE discord_bi IS NOT NULL AND discord_bi <> ''",
    """CREATE TABLE IF NOT EXISTS nodes (
        node_bi TEXT PRIMARY KEY,
        user_bi TEXT,
        pubkey TEXT,
        data TEXT NOT NULL,
        name TEXT,
        last_seen INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_nodes_user ON nodes(user_bi)",
    "CREATE INDEX IF NOT EXISTS idx_nodes_pubkey ON nodes(pubkey)",
    "CREATE INDEX IF NOT EXISTS idx_nodes_name ON nodes(name)",
    """CREATE TABLE IF NOT EXISTS account_devices (
        device_bi TEXT PRIMARY KEY,
        account_bi TEXT NOT NULL,
        pubkey TEXT NOT NULL,
        kind TEXT NOT NULL DEFAULT 'desktop_node',
        label TEXT,
        capabilities TEXT NOT NULL DEFAULT '',
        enabled INTEGER NOT NULL DEFAULT 1,
        created_at INTEGER NOT NULL,
        last_seen INTEGER NOT NULL DEFAULT 0,
        revoked_at INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_account_devices_account ON account_devices(account_bi)",
    "CREATE INDEX IF NOT EXISTS idx_account_devices_pubkey ON account_devices(pubkey)",
    # APNs device tokens (one row per phone install). The token is the thing
    # that lets anyone push to a device, so it is treated like every other
    # secret here: token_bi is a blind index of the token and doubles as the
    # upsert key, while the token itself lives encrypted in `data`. Neither the
    # account nor the device is legible from this table. `environment` records
    # whether the token was minted by a development build (APNs sandbox) or a
    # release one; sending to the wrong gateway is a hard BadDeviceToken.
    # `bundle_id` is the app that minted the token and so its apns-topic
    # ('' = the App Store app, com.forkmesh.forkmesh; migration 0192).
    """CREATE TABLE IF NOT EXISTS apns_device_tokens (
        token_bi TEXT PRIMARY KEY,
        account_bi TEXT NOT NULL,
        device_bi TEXT NOT NULL DEFAULT '',
        environment TEXT NOT NULL DEFAULT 'production',
        bundle_id TEXT NOT NULL DEFAULT '',
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        last_seen INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_apns_tokens_account "
    "ON apns_device_tokens(account_bi, last_seen DESC)",
    """CREATE TABLE IF NOT EXISTS repositories (
        key_bi TEXT PRIMARY KEY, owner_bi TEXT NOT NULL, data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_repos_owner ON repositories(owner_bi)",
    # The first concrete publishing device becomes the repository authority.
    # Additional devices owned by the same account remain useful mirrors but
    # cannot replace its signed state merely because they cloned the checkout.
    """CREATE TABLE IF NOT EXISTS repo_source_authorities (
        repo_bi TEXT PRIMARY KEY, node_id TEXT NOT NULL,
        machine_name TEXT NOT NULL DEFAULT '', created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        is_private INTEGER NOT NULL DEFAULT 0,
        data TEXT NOT NULL DEFAULT '')""",
    """CREATE TABLE IF NOT EXISTS repo_device_mirrors (
        repo_bi TEXT NOT NULL, node_id TEXT NOT NULL, data TEXT NOT NULL,
        updated_at INTEGER NOT NULL, PRIMARY KEY (repo_bi, node_id))""",
    "CREATE INDEX IF NOT EXISTS idx_repo_device_mirrors_repo "
    "ON repo_device_mirrors(repo_bi, updated_at DESC)",
    # Per-repo collaborator ACL (issue #9): which grantee accounts an owner has
    # shared a private repo with. repo_bi = blind_index("<owner>/<repo>") (the
    # same key as repositories.key_bi); grantee_bi = blind_index(grantee account
    # name). `data` is the encrypted {grantee, owner, repo, ts} so the owner's UI
    # can list grantee names back. A grantee with a row here additionally sees the
    # repo in their authenticated catalog and may clone it with THEIR OWN key.
    """CREATE TABLE IF NOT EXISTS repo_shares (
        repo_bi TEXT NOT NULL, grantee_bi TEXT NOT NULL,
        data TEXT NOT NULL, ts INTEGER NOT NULL,
        PRIMARY KEY (repo_bi, grantee_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_repo_shares_grantee ON repo_shares(grantee_bi)",
    # Catalog write throttle: last write time per owner (blind index). Plaintext
    # timestamp only — no user content.
    "CREATE TABLE IF NOT EXISTS catalog_rate (owner_bi TEXT PRIMARY KEY, ts INTEGER NOT NULL)",
    # Single-row monotonic counter bumped only when a catalog write changed a
    # record beyond volatile telemetry. Backs the weak ETag that lets a
    # revalidating client skip the full-table decrypt on GET /api/repositories.
    """CREATE TABLE IF NOT EXISTS catalog_rev (
        id INTEGER PRIMARY KEY CHECK (id = 1),
        rev INTEGER NOT NULL DEFAULT 0,
        ts INTEGER NOT NULL DEFAULT 0)""",
    # Cross-isolate snapshot of the decrypted public catalog, keyed by the
    # catalog_rev it was scanned under. One encrypted row replaces the full
    # repositories scan plus a per-row decrypt on every cold isolate (2026-09-13:
    # ~570 cold isolates in 2.5 hours, each rescanning the whole table). data is
    # the last column so count-only readers never touch its overflow pages.
    """CREATE TABLE IF NOT EXISTS public_catalog_snapshot (
        id INTEGER PRIMARY KEY CHECK (id = 1),
        rev INTEGER NOT NULL DEFAULT 0,
        repo_count INTEGER,
        ts INTEGER NOT NULL DEFAULT 0,
        data TEXT NOT NULL DEFAULT '')""",
    """CREATE TABLE IF NOT EXISTS issue_inbox (
        id INTEGER PRIMARY KEY AUTOINCREMENT, repo_bi TEXT NOT NULL,
        data TEXT NOT NULL, submitter_bi TEXT,
        claimed_by_bi TEXT NOT NULL DEFAULT '',
        claim_expires_at INTEGER NOT NULL DEFAULT 0,
        mirrored_by_bi TEXT NOT NULL DEFAULT '',
        mirrored_at INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_issue_inbox_repo ON issue_inbox(repo_bi)",
    """CREATE TABLE IF NOT EXISTS pull_inbox (
        id INTEGER PRIMARY KEY AUTOINCREMENT, repo_bi TEXT NOT NULL,
        data TEXT NOT NULL, submitter_bi TEXT,
        submission_bi TEXT NOT NULL DEFAULT '',
        claimed_by_bi TEXT NOT NULL DEFAULT '',
        claim_expires_at INTEGER NOT NULL DEFAULT 0,
        mirrored_by_bi TEXT NOT NULL DEFAULT '',
        mirrored_at INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_pull_inbox_repo ON pull_inbox(repo_bi)",
    """CREATE UNIQUE INDEX IF NOT EXISTS idx_pull_inbox_submission
       ON pull_inbox(repo_bi, submission_bi) WHERE submission_bi <> ''""",
    """CREATE TABLE IF NOT EXISTS pull_submission_receipts (
        repo_bi TEXT NOT NULL, submission_bi TEXT NOT NULL, data TEXT NOT NULL,
        PRIMARY KEY (repo_bi, submission_bi))""",
    """CREATE TABLE IF NOT EXISTS discussion_inbox (
        id INTEGER PRIMARY KEY AUTOINCREMENT, repo_bi TEXT NOT NULL,
        data TEXT NOT NULL, submitter_bi TEXT,
        claimed_by_bi TEXT NOT NULL DEFAULT '',
        claim_expires_at INTEGER NOT NULL DEFAULT 0,
        mirrored_by_bi TEXT NOT NULL DEFAULT '',
        mirrored_at INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_discussion_inbox_repo ON discussion_inbox(repo_bi)",
    # Agent-session sync (website "Agents" tab, adhoc #182): the desktop app
    # pushes a full-replace snapshot of its running/finished Claude Code agent
    # sessions for a repo (one row per session, keyed by the desktop's local
    # session id) so the owner can see them on the website.
    """CREATE TABLE IF NOT EXISTS repo_agents (
        repo_bi TEXT NOT NULL, agent_id TEXT NOT NULL,
        data TEXT NOT NULL, updated_at INTEGER NOT NULL,
        PRIMARY KEY (repo_bi, agent_id))""",
    "CREATE INDEX IF NOT EXISTS idx_repo_agents_repo ON repo_agents(repo_bi)",
    # Prompts the website owner queues for a running agent; the desktop drains
    # (selects + deletes) this table the same way it drains issue_inbox.
    """CREATE TABLE IF NOT EXISTS agent_prompts (
        id INTEGER PRIMARY KEY AUTOINCREMENT, repo_bi TEXT NOT NULL,
        agent_id TEXT NOT NULL, data TEXT NOT NULL, queued_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_agent_prompts_repo ON agent_prompts(repo_bi)",
    # Historical issue-bounty compatibility only. Some pre-noncustodial rows
    # contain an encrypted wallet seed until the explicit offline migration
    # exports and scrubs it. The Worker has no creation, signing, payout, or
    # broadcast path; only a public status projection reads these records.
    """CREATE TABLE IF NOT EXISTS issue_bounty (
        bounty_bi TEXT PRIMARY KEY, data TEXT NOT NULL)""",
    # Historical inbuilt bounty-wallet compatibility. Never written by the live
    # Worker; retained until its encrypted keys are exported/reconciled/scrubbed.
    """CREATE TABLE IF NOT EXISTS bounty_wallet (
        wallet_bi TEXT PRIMARY KEY, data TEXT NOT NULL)""",
    # Server-side 5xx / error log surfaced on the admin dashboard. `actor` is
    # the account name the failing request's own session proved (empty for
    # anonymous traffic), so the admin error view can name the affected user.
    """CREATE TABLE IF NOT EXISTS error_log (
        id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER NOT NULL,
        status INTEGER NOT NULL, method TEXT, path TEXT, message TEXT, ray TEXT,
        actor TEXT NOT NULL DEFAULT '')""",
    "CREATE INDEX IF NOT EXISTS idx_error_log_ts ON error_log(ts)",
    # Operator-managed request policy. The runtime reads enabled rules before
    # loading the application router. Patterns are configuration data and are
    # deliberately not embedded in source or migration files.
    """CREATE TABLE IF NOT EXISTS request_block_rules (
        rule_id TEXT PRIMARY KEY,
        match_type TEXT NOT NULL CHECK (match_type IN (
            'user_agent_keyword', 'ip', 'cidr', 'ip_range')),
        pattern TEXT NOT NULL CHECK (length(trim(pattern)) > 0),
        enabled INTEGER NOT NULL DEFAULT 1 CHECK (enabled IN (0, 1)),
        priority INTEGER NOT NULL DEFAULT 100,
        note TEXT NOT NULL DEFAULT '',
        created_at INTEGER NOT NULL CHECK (created_at > 0),
        updated_at INTEGER NOT NULL CHECK (updated_at > 0),
        UNIQUE (match_type, pattern))""",
    """CREATE INDEX IF NOT EXISTS idx_request_block_rules_active
        ON request_block_rules(
            enabled, priority DESC, created_at, rule_id)""",
    # Inbox drain audit log (adhoc #97): one row each time the owner's node acks
    # (drains) queued inbox submissions, so a "my web-filed issue disappeared but
    # never synced" report can be traced to when/what drained it. Content-free —
    # repo_bi is a blind index and the only payload is a row count.
    """CREATE TABLE IF NOT EXISTS inbox_drain_log (
        id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER NOT NULL,
        repo_bi TEXT NOT NULL, kind TEXT NOT NULL, count INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_inbox_drain_log_ts ON inbox_drain_log(ts)",
    "CREATE INDEX IF NOT EXISTS idx_inbox_drain_log_repo ON inbox_drain_log(repo_bi)",
    # Anonymous installer diagnostics surfaced on the admin dashboard. One row per
    # reported install step (start/mirror/deps/fetch/build/install/launch/done).
    # `run` is a random id the installer mints per run — it is NOT tied to any
    # account, email, or IP; nothing user-identifying is stored here (plaintext
    # operational diagnostics only, like error_log).
    """CREATE TABLE IF NOT EXISTS install_diag (
        id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER NOT NULL,
        run TEXT NOT NULL, step TEXT NOT NULL, ok INTEGER NOT NULL,
        os TEXT, arch TEXT, pm TEXT, distro TEXT, version TEXT, detail TEXT)""",
    "CREATE INDEX IF NOT EXISTS idx_install_diag_ts ON install_diag(ts)",
    "CREATE INDEX IF NOT EXISTS idx_install_diag_run ON install_diag(run)",
    # Anonymous marketplace package install counts, shown on /market and in the
    # desktop Market. One row per (client run, marketplace, package):
    # `installed` is the
    # current state so a removal stops counting toward "installs", while
    # `downloads` keeps the cumulative number of times that run installed the
    # package. `run` is the client's random, rotating id — like install_diag it
    # is NOT tied to an account, email, or IP, and no client IP is stored.
    """CREATE TABLE IF NOT EXISTS package_installs (
        run TEXT NOT NULL, market TEXT NOT NULL DEFAULT 'forkmesh',
        pkg TEXT NOT NULL, ts INTEGER NOT NULL,
        installed INTEGER NOT NULL DEFAULT 1,
        downloads INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (run, market, pkg))""",
    "CREATE INDEX IF NOT EXISTS idx_package_installs_ts ON package_installs(ts)",
    "CREATE INDEX IF NOT EXISTS idx_package_installs_pkg ON package_installs(market, pkg)",
    # Public Market ratings are one value per anonymous browser id, marketplace,
    # and package. Updating a rating replaces that browser's prior vote instead
    # of inflating a package's aggregate. The opaque browser id is not tied to
    # an account, IP address, or hostname.
    """CREATE TABLE IF NOT EXISTS package_ratings (
        run TEXT NOT NULL, market TEXT NOT NULL DEFAULT 'forkmesh',
        pkg TEXT NOT NULL, rating INTEGER NOT NULL CHECK (rating BETWEEN 1 AND 5),
        ts INTEGER NOT NULL,
        PRIMARY KEY (run, market, pkg))""",
    "CREATE INDEX IF NOT EXISTS idx_package_ratings_ts ON package_ratings(ts)",
    "CREATE INDEX IF NOT EXISTS idx_package_ratings_pkg ON package_ratings(market, pkg)",
    # Immutable, content-addressed marketplace submissions. Publication is a
    # two-stage state machine: five distinct active members approve
    # the exact manifest digest, then a distinct current owner of the linked
    # forkmesh/forkmesh repository makes the final decision.
    """CREATE TABLE IF NOT EXISTS marketplace_submissions (
        submission_id TEXT PRIMARY KEY,
        marketplace_id TEXT NOT NULL,
        package_id TEXT NOT NULL,
        version TEXT NOT NULL,
        manifest_url TEXT NOT NULL,
        manifest_sha256 TEXT NOT NULL,
        sources_json TEXT NOT NULL,
        artifact_sha256 TEXT NOT NULL,
        artifact_size INTEGER NOT NULL,
        artifact_locations_json TEXT NOT NULL,
        submitted_by_bi TEXT NOT NULL,
        submitted_by_name TEXT NOT NULL,
        state TEXT NOT NULL DEFAULT 'member_review',
        owner_approved_by_bi TEXT NOT NULL DEFAULT '',
        owner_approved_by_name TEXT NOT NULL DEFAULT '',
        rejection_reason TEXT NOT NULL DEFAULT '',
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        published_at INTEGER NOT NULL DEFAULT 0,
        UNIQUE (marketplace_id, package_id, version, manifest_sha256))""",
    "CREATE INDEX IF NOT EXISTS idx_marketplace_submissions_state "
    "ON marketplace_submissions(state, updated_at DESC)",
    "CREATE INDEX IF NOT EXISTS idx_marketplace_submissions_package "
    "ON marketplace_submissions(marketplace_id, package_id, published_at DESC)",
    # wallet_address/value_lamports/value_checked_at are retired (#516):
    # governance writes blanks and never reads them.
    """CREATE TABLE IF NOT EXISTS marketplace_member_approvals (
        submission_id TEXT NOT NULL,
        reviewer_bi TEXT NOT NULL,
        reviewer_name TEXT NOT NULL,
        wallet_address TEXT NOT NULL,
        value_lamports INTEGER NOT NULL DEFAULT 0,
        value_checked_at INTEGER NOT NULL DEFAULT 0,
        manifest_sha256 TEXT NOT NULL,
        approved_at INTEGER NOT NULL,
        PRIMARY KEY (submission_id, reviewer_bi),
        FOREIGN KEY (submission_id) REFERENCES marketplace_submissions(submission_id)
            ON DELETE CASCADE)""",
    "CREATE INDEX IF NOT EXISTS idx_marketplace_member_approvals_value "
    "ON marketplace_member_approvals(submission_id, value_lamports DESC, approved_at)",
    # Live host presence: lets /api/network/stats report "hosts online" without
    # probing every repo's tunnel Durable Object on every page view. repo_bi is a
    # blind index (no plaintext repo name), ts is refreshed while a host is active
    # and a staleness window self-heals rows left behind by a missed disconnect.
    "CREATE TABLE IF NOT EXISTS host_presence (repo_bi TEXT PRIMARY KEY, ts INTEGER NOT NULL)",
    "CREATE INDEX IF NOT EXISTS idx_host_presence_ts ON host_presence(ts)",
    # Direct HTTPS mirror endpoints. These rows contain only public routing
    # identity/health data and never repository bytes, wallet material, node
    # private keys, or private repository names. The node identity signature
    # binds node_name + base_url + public_key + issued_at before registration;
    # a fresh signed challenge updates checked_at/healthy.
    """CREATE TABLE IF NOT EXISTS mirror_https_endpoints (
        node_bi TEXT PRIMARY KEY,
        node_name TEXT NOT NULL,
        base_url TEXT NOT NULL,
        mirror_url TEXT NOT NULL DEFAULT '',
        public_key TEXT NOT NULL,
        registration_sig TEXT NOT NULL,
        issued_at INTEGER NOT NULL,
        checked_at INTEGER NOT NULL DEFAULT 0,
        latency_ms INTEGER NOT NULL DEFAULT 0,
        region TEXT,
        healthy INTEGER NOT NULL DEFAULT 0,
        integrity TEXT NOT NULL DEFAULT 'unknown',
        abuse_blocked INTEGER NOT NULL DEFAULT 0,
        health_sig TEXT,
        forkmesh_verified_at INTEGER NOT NULL DEFAULT 0,
        forkmesh_refs_sha256 TEXT NOT NULL DEFAULT '',
        forkmesh_operations_sha256 TEXT NOT NULL DEFAULT '',
        forkmesh_operations_json TEXT NOT NULL DEFAULT '[]',
        forkmesh_active INTEGER NOT NULL DEFAULT 0,
        health_message TEXT NOT NULL DEFAULT '',
        updated_at INTEGER NOT NULL)""",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_mirror_https_endpoint_name "
    "ON mirror_https_endpoints(node_name)",
    "CREATE INDEX IF NOT EXISTS idx_mirror_https_endpoint_health "
    "ON mirror_https_endpoints(healthy, checked_at)",
    # Website-triggered verification of one repository at one advertised
    # direct origin. The Worker records only public routing evidence: whether
    # the HTTPS Git route answered and whether the node-signed refs digest
    # matched a source-attested repository pin already retained in D1. A URL
    # replacement does not inherit the prior verdict because readers compare
    # this recorded origin with the endpoint's current signed registration.
    """CREATE TABLE IF NOT EXISTS mirror_repository_integrity (
        repo_bi TEXT NOT NULL,
        node_bi TEXT NOT NULL,
        mirror_url TEXT NOT NULL DEFAULT '',
        registration_issued_at INTEGER NOT NULL DEFAULT 0,
        reachable INTEGER NOT NULL DEFAULT 0
            CHECK (reachable IN (0, 1)),
        integrity TEXT NOT NULL DEFAULT 'unknown'
            CHECK (integrity IN ('unknown', 'ok', 'failed')),
        refs_sha256 TEXT NOT NULL DEFAULT '',
        checked_at INTEGER NOT NULL DEFAULT 0,
        latency_ms INTEGER NOT NULL DEFAULT 0,
        status_code INTEGER NOT NULL DEFAULT 0,
        reason TEXT NOT NULL DEFAULT '',
        PRIMARY KEY (repo_bi, node_bi),
        FOREIGN KEY (node_bi) REFERENCES mirror_https_endpoints(node_bi)
            ON DELETE CASCADE) WITHOUT ROWID""",
    "CREATE INDEX IF NOT EXISTS idx_mirror_repository_integrity_checked "
    "ON mirror_repository_integrity(checked_at)",
    """CREATE TABLE IF NOT EXISTS edge_route_cursor (
        repo_bi TEXT PRIMARY KEY,
        cursor INTEGER NOT NULL DEFAULT 0,
        updated_at INTEGER NOT NULL)""",
    # Account-authorized pull merges are pinned to one independently operated
    # node. D1 stores only blind indexes, the caller's opaque idempotency key,
    # exact public Git object ids, and a bounded result; repository bytes and
    # node credentials remain outside the Worker. Expiry/indexes support
    # bounded cleanup; triggers are the concurrency-safe final row ceilings.
    """CREATE TABLE IF NOT EXISTS repo_merge_jobs (
        request_id TEXT PRIMARY KEY,
        request_digest TEXT NOT NULL,
        repo_bi TEXT NOT NULL,
        actor_bi TEXT NOT NULL,
        pull_number INTEGER NOT NULL,
        selected_node TEXT NOT NULL,
        status TEXT NOT NULL CHECK (status IN (
            'requested','succeeded','failed')),
        result TEXT NOT NULL DEFAULT '',
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        expires_at INTEGER NOT NULL CHECK (expires_at > created_at))""",
    "CREATE INDEX IF NOT EXISTS idx_repo_merge_jobs_repo "
    "ON repo_merge_jobs(repo_bi, status, updated_at)",
    "CREATE INDEX IF NOT EXISTS idx_repo_merge_jobs_expiry "
    "ON repo_merge_jobs(expires_at)",
    """CREATE TRIGGER IF NOT EXISTS trg_repo_merge_jobs_repo_bound
       BEFORE INSERT ON repo_merge_jobs
       WHEN (SELECT COUNT(*) FROM repo_merge_jobs
              WHERE repo_bi=NEW.repo_bi) >= 256
       BEGIN
         SELECT RAISE(ABORT, 'repo_merge_jobs_repo_limit');
       END""",
    """CREATE TRIGGER IF NOT EXISTS trg_repo_merge_jobs_global_bound
       BEFORE INSERT ON repo_merge_jobs
       WHEN (SELECT COUNT(*) FROM repo_merge_jobs) >= 10000
       BEGIN
         SELECT RAISE(ABORT, 'repo_merge_jobs_global_limit');
       END""",
    # Private replicas use the same registered HTTPS origins, but only after
    # repository authorization. repo_bi/binding_bi are blind indexes and the
    # random opaque id addresses an owner-sealed ciphertext file; no private
    # repository name, plaintext, recipient secret, or decryption key is stored.
    """CREATE TABLE IF NOT EXISTS private_mirror_routes (
        binding_bi TEXT PRIMARY KEY,
        repo_bi TEXT NOT NULL,
        node_bi TEXT NOT NULL,
        opaque_replica_id TEXT NOT NULL,
        replica_sha256 TEXT NOT NULL,
        key_epoch INTEGER NOT NULL,
        owner_sig TEXT NOT NULL,
        issued_at INTEGER NOT NULL,
        active INTEGER NOT NULL DEFAULT 1,
        updated_at INTEGER NOT NULL,
        FOREIGN KEY (node_bi) REFERENCES mirror_https_endpoints(node_bi)
            ON DELETE CASCADE)""",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_private_mirror_route_replica "
    "ON private_mirror_routes(node_bi, opaque_replica_id)",
    "CREATE INDEX IF NOT EXISTS idx_private_mirror_route_access "
    "ON private_mirror_routes(opaque_replica_id, active, key_epoch, repo_bi)",
    "CREATE INDEX IF NOT EXISTS idx_private_mirror_route_repo "
    "ON private_mirror_routes(repo_bi, active, updated_at)",
    # Per-repo round-robin cursor for clone fallbacks. When a repo's named host is
    # offline, a clone is redirected to one of its online mirrors; this counter is
    # bumped on each redirect so the picks rotate across every mirror instead of
    # all piling onto the single freshest one. repo_bi is the same blind index
    # host_presence uses; n is a monotonically increasing rotation cursor.
    "CREATE TABLE IF NOT EXISTS clone_rr (repo_bi TEXT PRIMARY KEY, n INTEGER NOT NULL DEFAULT 0)",
    # Which mirror is currently serving a downed repo's clones IN PLACE (same
    # URL, no redirect). git clones are two requests (info/refs then the
    # upload-pack POST) that must reach the SAME node, so the pick is pinned
    # here for a short window instead of rotating per request; rotation happens
    # when the pin expires. The owner name is public catalog data.
    "CREATE TABLE IF NOT EXISTS clone_sticky ("
    "repo_bi TEXT PRIMARY KEY, owner TEXT NOT NULL, ts INTEGER NOT NULL)",
    # Per-node serve tallies, counted at the router. The Worker is the only
    # component that sees every public read it routes to a mirror endpoint —
    # nodes stopped seeing per-request traffic when the per-repository
    # WebSocket transport (RepoHost) was retired — so it owns the Clones /
    # Website counters the Mirror nodes view shows. Names are public catalog
    # identities, already listed on the repository's Mirrors tab.
    """CREATE TABLE IF NOT EXISTS mirror_serve_counters (
        node_name TEXT NOT NULL,
        owner TEXT NOT NULL,
        repo TEXT NOT NULL,
        clones INTEGER NOT NULL DEFAULT 0,
        website INTEGER NOT NULL DEFAULT 0,
        updated_at INTEGER NOT NULL,
        PRIMARY KEY (node_name, owner, repo))""",
    # Mirrors payloads resolve one repository plus a bounded set of public
    # owner aliases.  The table primary key begins with node_name, so that
    # lookup otherwise scans every node's counters.  Keep the hot clone/website
    # values out of this secondary index: they change on every routed serve and
    # are fetched only for the small matching set after this prefix lookup.
    "CREATE INDEX IF NOT EXISTS idx_mirror_serve_counters_repo_owner_node "
    "ON mirror_serve_counters(repo, owner, node_name)",
    # Per-day clone counts behind the internal product dashboard. The serve
    # counters above are cumulative, so they cannot answer "how many clones
    # in the last 30 days"; this table buckets every upload-pack the public
    # HTTPS mirror route serves by UTC day. Owner/repo are the same public
    # catalog identities the counters already store (migration 0175).
    """CREATE TABLE IF NOT EXISTS product_clone_daily (
        day TEXT NOT NULL,
        owner TEXT NOT NULL,
        repo TEXT NOT NULL,
        clones INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (day, owner, repo))""",
    "CREATE INDEX IF NOT EXISTS idx_product_clone_daily_day "
    "ON product_clone_daily(day)",
    # Recent owner-attested repo-state pins (sha256 of the canonical heads+tags
    # advertisement), appended on every catalog publish by a working-copy holder
    # ("local-node"). Mirrors are integrity-checked against the SOURCE's pins —
    # current plus this short history — instead of their own self-attested hash,
    # so a tampered mirror can't just publish a matching pin for its forged refs,
    # while an honest mirror that lags the source by a few publishes still clones
    # (see clone_state_pins). Pruned to the newest STATE_PIN_HISTORY per repo.
    "CREATE TABLE IF NOT EXISTS repo_state_history ("
    "key_bi TEXT NOT NULL, state_hash TEXT NOT NULL, ts INTEGER NOT NULL, "
    "PRIMARY KEY (key_bi, state_hash))",
    # Every public snapshot/deep probe reads a fixed newest-pin window for a
    # handful of authority keys. This order lets each per-key LIMIT stop at the
    # window boundary even if a prior best-effort prune did not complete. Keep
    # state_hash in the index so D1 does not bill a second table-row lookup for
    # every returned pin.
    "CREATE INDEX IF NOT EXISTS idx_repo_state_history_key_ts "
    "ON repo_state_history(key_bi, ts DESC, state_hash)",
    # Account-level presence: a node heartbeats here while it is online so it can
    # be included in the reward split. Keyed by the account blind index.
    "CREATE TABLE IF NOT EXISTS account_presence (name_bi TEXT PRIMARY KEY, ts INTEGER NOT NULL)",
    "CREATE INDEX IF NOT EXISTS idx_account_presence_ts ON account_presence(ts)",
    # Login throttle: failed-attempt counter + lockout per login identifier (blind
    # index). No plaintext credential is stored — only the HMAC of the identifier.
    "CREATE TABLE IF NOT EXISTS login_attempts (id_bi TEXT PRIMARY KEY, "
    "fails INTEGER NOT NULL DEFAULT 0, first_fail_ts INTEGER NOT NULL DEFAULT 0, "
    "locked_until INTEGER NOT NULL DEFAULT 0)",
    # Per-source-IP account-creation throttle: a rolling window count keyed by the
    # blind index of the signup IP, so a single IP can't mass-create accounts /
    # squat node names. ip_bi is the same one-way index stored on the account.
    "CREATE TABLE IF NOT EXISTS signup_rate (ip_bi TEXT PRIMARY KEY, "
    "count INTEGER NOT NULL DEFAULT 0, window_start_ts INTEGER NOT NULL DEFAULT 0)",
    # Password-reset abuse budget. Keys are domain-separated blind indexes of
    # either an account or an edge-observed requester address; no identifier or
    # raw network address is stored.
    "CREATE TABLE IF NOT EXISTS password_reset_rate (key_bi TEXT PRIMARY KEY, "
    "count INTEGER NOT NULL DEFAULT 0, window_start_ts INTEGER NOT NULL DEFAULT 0, "
    "last_sent_ts INTEGER NOT NULL DEFAULT 0)",
    "CREATE INDEX IF NOT EXISTS idx_password_reset_rate_window "
    "ON password_reset_rate(window_start_ts)",
    # A GET of the canonical /signup page creates one short-lived claim. The
    # HttpOnly cookie contains the signed claim id while this table provides
    # one-time atomic consumption. account_bi is a keyed blind index, and no
    # address, user agent, email, password, or other browser data is stored.
    """CREATE TABLE IF NOT EXISTS account_signup_intents (
        claim_id TEXT PRIMARY KEY,
        issued_at INTEGER NOT NULL,
        expires_at INTEGER NOT NULL,
        email_field_name TEXT NOT NULL,
        consumed_at INTEGER NOT NULL DEFAULT 0,
        account_bi TEXT NOT NULL DEFAULT '')""",
    "CREATE INDEX IF NOT EXISTS idx_account_signup_intents_expiry "
    "ON account_signup_intents(expires_at,consumed_at)",
    # Optional administrator-issued signup gate. There is exactly one current
    # code for the relay. Its searchable value is a keyed blind index and the
    # display value is encrypted in data; successful use atomically replaces
    # both values so two signups cannot consume the same generation.
    """CREATE TABLE IF NOT EXISTS admin_signup_referral_code (
        id INTEGER PRIMARY KEY CHECK (id = 1),
        code_bi TEXT NOT NULL,
        data TEXT NOT NULL,
        enabled INTEGER NOT NULL DEFAULT 0 CHECK (enabled IN (0, 1)),
        generation INTEGER NOT NULL DEFAULT 1 CHECK (generation >= 1),
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        last_used_at INTEGER NOT NULL DEFAULT 0,
        last_used_by_bi TEXT NOT NULL DEFAULT '')""",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_admin_signup_referral_code_bi "
    "ON admin_signup_referral_code(code_bi)",
    # Per-account Workers AI throttle for POST /api/ai/ask (the desktop
    # composer's "send this prompt to a Cloudflare AI model" path). Every call
    # bills the relay's Workers AI account, so a signed account gets a bounded
    # number per rolling window. Plaintext counters only — no prompt content.
    "CREATE TABLE IF NOT EXISTS ai_ask_rate (account_bi TEXT PRIMARY KEY, "
    "count INTEGER NOT NULL DEFAULT 0, window_start_ts INTEGER NOT NULL DEFAULT 0)",
    # Installer link-code rendezvous (adhoc #53): install.sh mints a short code
    # the fresh headless node registers with, and the installing user's desktop
    # app offers the same code signed by its key. Whichever side arrives first
    # parks its half here; the second side completes the link (node.owner =
    # user, user.nodes += node) and deletes the row. code_bi is the blind index
    # of "link:<code>"; data is the encrypted {node|user}; rows expire after
    # LINK_CODE_TTL_MS and are pruned as they are looked up.
    "CREATE TABLE IF NOT EXISTS link_codes (code_bi TEXT PRIMARY KEY, "
    "data TEXT NOT NULL, ts INTEGER NOT NULL)",
    # Email-verification fallback queue: accounts land here only when the
    # configured email provider cannot deliver a verification message, so an
    # administrator can review them. data = encrypted {name, email, joinedAt}.
    "CREATE TABLE IF NOT EXISTS pending_verifications (name_bi TEXT PRIMARY KEY, data TEXT NOT NULL)",
    # Hourly online-activity samples for the /network/ graph. A weighted
    # ten-minute sample adds the current online-node count into the current
    # hour's bucket, so
    # node_minutes is "node-minutes online" that hour (one node online all hour
    # = 60). hour_ts is the epoch-ms start of the hour.
    "CREATE TABLE IF NOT EXISTS online_hourly (hour_ts INTEGER PRIMARY KEY, node_minutes INTEGER NOT NULL DEFAULT 0)",
    """CREATE TABLE IF NOT EXISTS online_hourly_nodes (
        hour_ts INTEGER NOT NULL, node_key TEXT NOT NULL, label TEXT NOT NULL,
        node_minutes INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (hour_ts, node_key))""",
    "CREATE INDEX IF NOT EXISTS idx_online_hourly_nodes_key ON online_hourly_nodes(node_key)",
    # Retained chat history: the relay keeps the last few days of *encrypted*
    # durable messages per room so a node joining later sees some history even
    # when no peer is online to replay it. body is the opaque encrypted envelope
    # exactly as relayed; the server never sees plaintext. room_key is the same
    # key used to address the room Durable Object.
    """CREATE TABLE IF NOT EXISTS chat_history (
        room_key TEXT NOT NULL, msg_id TEXT NOT NULL, ts INTEGER NOT NULL,
        body TEXT NOT NULL, acked_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (room_key, msg_id))""",
    "CREATE INDEX IF NOT EXISTS idx_chat_history_room_ts ON chat_history(room_key, ts)",
    "CREATE INDEX IF NOT EXISTS idx_chat_history_ts ON chat_history(ts)",
    "CREATE INDEX IF NOT EXISTS idx_chat_history_acked ON chat_history(acked_at) WHERE acked_at>0",
    # Per-requester throttle for the unauthenticated AI guestbook. The key is a
    # DATA_KEY-blinded edge address; public notes themselves use the existing
    # encrypted #general history and are never duplicated into this table
    # (migration 0157).
    "CREATE TABLE IF NOT EXISTS agent_guestbook_rate ("
    "visitor_bi TEXT PRIMARY KEY, count INTEGER NOT NULL DEFAULT 0, "
    "window_start_ts INTEGER NOT NULL DEFAULT 0)",
    "CREATE INDEX IF NOT EXISTS idx_agent_guestbook_rate_window "
    "ON agent_guestbook_rate(window_start_ts)",
    "CREATE TABLE IF NOT EXISTS agent_note_queue ("
    "entry_id TEXT PRIMARY KEY, status TEXT NOT NULL DEFAULT 'pending', "
    "created_at INTEGER NOT NULL DEFAULT 0, "
    "reviewed_at INTEGER NOT NULL DEFAULT 0, "
    "note_id TEXT NOT NULL DEFAULT '', data TEXT NOT NULL)",
    "CREATE INDEX IF NOT EXISTS idx_agent_note_queue_status "
    "ON agent_note_queue(status, created_at)",
    # User-created public/private chat channels (migration 0069). Channel
    # names and creator labels live only in encrypted data; name_bi enforces
    # uniqueness without exposing the normalized name in plaintext. Every
    # creator and every platform administrator have implicit access, while
    # direct user grants are recorded in chat_channel_members.
    """CREATE TABLE IF NOT EXISTS chat_channels (
        channel_id TEXT PRIMARY KEY,
        name_bi TEXT NOT NULL UNIQUE,
        data TEXT NOT NULL,
        created_by_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        key_version INTEGER NOT NULL DEFAULT 1 CHECK (key_version >= 1))""",
    """CREATE TABLE IF NOT EXISTS chat_channel_members (
        channel_id TEXT NOT NULL,
        member_bi TEXT NOT NULL,
        data TEXT NOT NULL,
        invited_by_bi TEXT NOT NULL,
        joined_at INTEGER NOT NULL,
        PRIMARY KEY (channel_id, member_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_chat_channel_members_member "
    "ON chat_channel_members(member_bi, channel_id)",
    # Incoming channel webhooks (migration 0132). The bearer token never lands
    # in plaintext: only its keyed blind index is queryable, while the display
    # name and creator live in the encrypted data envelope.
    """CREATE TABLE IF NOT EXISTS chat_channel_webhooks (
        webhook_id TEXT PRIMARY KEY,
        channel_id TEXT NOT NULL,
        token_bi TEXT NOT NULL UNIQUE,
        data TEXT NOT NULL,
        created_by_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        last_used_at INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_chat_channel_webhooks_channel "
    "ON chat_channel_webhooks(channel_id, created_at)",
    """CREATE TABLE IF NOT EXISTS chat_channel_invites (
        channel_id TEXT PRIMARY KEY,
        token_bi TEXT NOT NULL UNIQUE,
        data TEXT NOT NULL,
        created_by_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS chat_channel_reads (
        channel_id TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        read_at INTEGER NOT NULL DEFAULT 0 CHECK (read_at >= 0),
        reminded_at INTEGER NOT NULL DEFAULT 0 CHECK (reminded_at >= 0),
        PRIMARY KEY (channel_id, account_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_chat_channel_reads_account "
    "ON chat_channel_reads(account_bi)",
    """CREATE TABLE IF NOT EXISTS chat_read_marks (
        account_bi TEXT NOT NULL,
        conversation TEXT NOT NULL,
        read_ts INTEGER NOT NULL DEFAULT 0 CHECK (read_ts >= 0),
        updated_at INTEGER NOT NULL DEFAULT 0 CHECK (updated_at >= 0),
        PRIMARY KEY (account_bi, conversation))""",
    "CREATE INDEX IF NOT EXISTS idx_chat_read_marks_updated "
    "ON chat_read_marks(account_bi, updated_at)",
    """CREATE TABLE IF NOT EXISTS chat_blocks (
        account_bi TEXT PRIMARY KEY,
        data TEXT NOT NULL,
        updated_at INTEGER NOT NULL DEFAULT 0 CHECK (updated_at >= 0))""",
    # Rotate the shared-key namespace atomically whenever a real membership is
    # removed. An idempotent DELETE that matches no row does not fire it.
    # The body is deterministic (migration 0191): rqlite replicates statement
    # text, so a 'now' here would either be frozen at CREATE time or differ
    # per replica. The app stamps updated_at itself after a real removal.
    """CREATE TRIGGER IF NOT EXISTS trg_chat_channel_member_remove_rotate
        AFTER DELETE ON chat_channel_members
        BEGIN
          UPDATE chat_channels
             SET key_version = key_version + 1
           WHERE channel_id = OLD.channel_id;
        END""",
    # One-to-one direct messages (migration 0079). The pair blind index keeps
    # one stable conversation per pair without exposing either username.
    # Participant rows, not administrator status, control access.
    """CREATE TABLE IF NOT EXISTS chat_direct_conversations (
        conversation_id TEXT PRIMARY KEY,
        pair_bi TEXT NOT NULL UNIQUE,
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        key_version INTEGER NOT NULL DEFAULT 1 CHECK (key_version >= 1),
        message_count INTEGER NOT NULL DEFAULT 0 CHECK (message_count >= 0))""",
    """CREATE TABLE IF NOT EXISTS chat_direct_participants (
        conversation_id TEXT NOT NULL,
        participant_bi TEXT NOT NULL,
        data TEXT NOT NULL,
        joined_at INTEGER NOT NULL,
        last_read_count INTEGER NOT NULL DEFAULT 0
            CHECK (last_read_count >= 0),
        initiated INTEGER NOT NULL DEFAULT 0 CHECK (initiated IN (0, 1)),
        unread_reminded_at INTEGER NOT NULL DEFAULT 0
            CHECK (unread_reminded_at >= 0),
        PRIMARY KEY (conversation_id, participant_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_chat_direct_participants_account "
    "ON chat_direct_participants(participant_bi, conversation_id)",
    "CREATE INDEX IF NOT EXISTS idx_chat_direct_participants_creation_rate "
    "ON chat_direct_participants(participant_bi, initiated, joined_at)",
    """CREATE TRIGGER IF NOT EXISTS trg_chat_direct_creation_rate
        BEFORE INSERT ON chat_direct_participants
        WHEN NEW.initiated = 1 AND (
          SELECT COUNT(*)
          FROM chat_direct_participants
          WHERE participant_bi = NEW.participant_bi
            AND initiated = 1
            AND joined_at > NEW.joined_at - 3600000
        ) >= 20
        BEGIN
          SELECT RAISE(ABORT, 'chat_direct_creation_rate_limited');
        END""",
    """CREATE TABLE IF NOT EXISTS chat_direct_media (
        media_id TEXT PRIMARY KEY,
        conversation_id TEXT NOT NULL,
        sender_bi TEXT NOT NULL,
        recipient_bi TEXT NOT NULL,
        state TEXT NOT NULL CHECK (state IN
            ('uploading', 'ready', 'opened', 'expired', 'withdrawn')),
        size INTEGER NOT NULL CHECK (size >= 0),
        parts INTEGER NOT NULL CHECK (parts >= 0),
        created_at INTEGER NOT NULL,
        expires_at INTEGER NOT NULL,
        opened_at INTEGER NOT NULL DEFAULT 0,
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_chat_direct_media_conversation "
    "ON chat_direct_media(conversation_id, created_at)",
    "CREATE INDEX IF NOT EXISTS idx_chat_direct_media_sender "
    "ON chat_direct_media(sender_bi, state)",
    "CREATE INDEX IF NOT EXISTS idx_chat_direct_media_expiry "
    "ON chat_direct_media(state, expires_at)",
    """CREATE TABLE IF NOT EXISTS chat_direct_media_parts (
        media_id TEXT NOT NULL,
        part_index INTEGER NOT NULL CHECK (part_index >= 0),
        size INTEGER NOT NULL CHECK (size >= 0),
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        PRIMARY KEY (media_id, part_index))""",
    # --- Relay federation (main relay only) ---------------------------------
    # Allowlist of relays that federate with this (main) relay. A relay is known
    # by its Ed25519 pubkey; only status='approved' relays may custody signups
    # here or submit node attestations for reward verification. Grant directly:
    #   UPDATE relays SET status='approved' WHERE label='...';
    """CREATE TABLE IF NOT EXISTS relays (
        relay_bi TEXT PRIMARY KEY, pubkey TEXT NOT NULL, label TEXT,
        base_url TEXT, status TEXT NOT NULL DEFAULT 'pending',
        registered_at INTEGER, approved_at INTEGER)""",
    # Reward candidates reported by approved federated relays. A wallet and
    # name alone are never sufficient: every live row carries the original
    # node-signed registration + repository-health proof, relay-observed
    # integrity facts, and main-relay observation history. Migration 0057
    # discards the historical wallet-only rows before adding these columns.
    """CREATE TABLE IF NOT EXISTS federated_presence (
        relay_bi TEXT NOT NULL, wallet TEXT NOT NULL, name TEXT,
        operator_id TEXT NOT NULL DEFAULT '',
        device_id TEXT NOT NULL DEFAULT '',
        public_key TEXT NOT NULL DEFAULT '',
        base_url TEXT NOT NULL DEFAULT '',
        registration_sig TEXT NOT NULL DEFAULT '',
        registration_issued_at INTEGER NOT NULL DEFAULT 0,
        health_message TEXT NOT NULL DEFAULT '',
        health_sig TEXT NOT NULL DEFAULT '',
        checked_at INTEGER NOT NULL DEFAULT 0,
        healthy INTEGER NOT NULL DEFAULT 0,
        integrity TEXT NOT NULL DEFAULT 'unknown',
        forkmesh_active INTEGER NOT NULL DEFAULT 0,
        forkmesh_verified_at INTEGER NOT NULL DEFAULT 0,
        forkmesh_refs_sha256 TEXT NOT NULL DEFAULT '',
        forkmesh_operations_sha256 TEXT NOT NULL DEFAULT '',
        abuse_blocked INTEGER NOT NULL DEFAULT 0,
        attestation_id TEXT NOT NULL DEFAULT '',
        first_verified_at INTEGER NOT NULL DEFAULT 0,
        last_verified_at INTEGER NOT NULL DEFAULT 0,
        consecutive_checks INTEGER NOT NULL DEFAULT 0,
        ts INTEGER NOT NULL,
        PRIMARY KEY (relay_bi, wallet))""",
    "CREATE INDEX IF NOT EXISTS idx_federated_presence_ts ON federated_presence(ts)",
    # Historical federated-signup deposit rows. Creation and sweep routes are
    # disabled; existing public status remains readable while any encrypted key
    # awaits explicit offline migration.
    """CREATE TABLE IF NOT EXISTS federated_signup (
        reference TEXT PRIMARY KEY, relay_bi TEXT NOT NULL, data TEXT NOT NULL,
        created_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_federated_signup_relay ON federated_signup(relay_bi)",
    # This relay's own federation identity (federated relays only): a single
    # encrypted row holding the auto-generated Ed25519 keypair used to sign
    # relay->main calls. id is always 1.
    "CREATE TABLE IF NOT EXISTS relay_self (id INTEGER PRIMARY KEY, data TEXT NOT NULL)",
    # Historical central-fund row retained for schema compatibility and offline
    # custody migration. The active pool address comes from public configuration;
    # the Worker never reads this row for signing or distribution.
    "CREATE TABLE IF NOT EXISTS central_fund (id INTEGER PRIMARY KEY, data TEXT NOT NULL)",
    # --- Leaderboard backing data (issue #11) -------------------------------
    # First time each repo's tunnel was ever seen live, so the "longest hosted"
    # board can rank by age. repo_bi is the same blind index host_presence uses
    # (== repositories.key_bi for published repos); written once and never
    # updated, so it survives the host going offline and coming back.
    "CREATE TABLE IF NOT EXISTS repo_first_hosted (repo_bi TEXT PRIMARY KEY, ts INTEGER NOT NULL)",
    # Cumulative contributor activity for the "contributor activity" board. Keyed
    # by a blind index of the (public) author name; the plaintext display name is
    # kept alongside since issue/PR authorship is already public. The inbox tables
    # get drained on merge, so the running tally lives here instead.
    """CREATE TABLE IF NOT EXISTS contributor_activity (
        author_bi TEXT PRIMARY KEY, name TEXT NOT NULL,
        issues INTEGER NOT NULL DEFAULT 0, pulls INTEGER NOT NULL DEFAULT 0,
        commits INTEGER NOT NULL DEFAULT 0, discussions INTEGER NOT NULL DEFAULT 0,
        total INTEGER NOT NULL DEFAULT 0,
        last_ts INTEGER)""",
    "CREATE INDEX IF NOT EXISTS idx_contributor_activity_total ON contributor_activity(total)",
    # Cumulative funds (lamports) actually disbursed to each recipient, for the
    # "funds received" boards. scope is 'mainnode' (donation-sweep node split),
    # 'contributor' (bounty payee), or 'project' (the owner/repo a paid bounty
    # belonged to). key is the recipient's Solana address (mainnode/contributor)
    # or "owner/repo" (project). Treasury transfers are never recorded here.
    """CREATE TABLE IF NOT EXISTS funds_received (
        scope TEXT NOT NULL, key TEXT NOT NULL, name TEXT,
        lamports INTEGER NOT NULL DEFAULT 0, last_ts INTEGER,
        PRIMARY KEY (scope, key))""",
    # Last public on-chain balance read for a member's published payout address,
    # for the "member SOL wallets" board. Both columns are public data (the
    # address is public profile data, the balance is a public getBalance), and
    # the row exists only so the board can rank every published address while
    # re-reading a rotating slice per rebuild instead of all of them at once.
    """CREATE TABLE IF NOT EXISTS wallet_balances (
        wallet TEXT PRIMARY KEY, name TEXT,
        lamports INTEGER NOT NULL DEFAULT 0,
        checked_at INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_wallet_balances_checked ON wallet_balances(checked_at)",
    """CREATE TABLE IF NOT EXISTS notifications (
        dedupe_bi TEXT PRIMARY KEY,
        recipient_bi TEXT NOT NULL,
        ts INTEGER NOT NULL,
        read_at INTEGER NOT NULL DEFAULT 0,
        data TEXT NOT NULL,
        content_bi TEXT NOT NULL DEFAULT '')""",
    "CREATE INDEX IF NOT EXISTS idx_notifications_recipient_ts ON notifications(recipient_bi, ts)",
    "CREATE INDEX IF NOT EXISTS idx_notifications_unread ON notifications(recipient_bi, read_at, ts)",
    # Thread subscriptions (issue #361): who follows a given issue/PR. thread_bi is
    # a blind index of "thread:<owner>/<repo>:<source>:<number>"; the encrypted
    # data holds the subscriber's (public) node name plus a `muted` flag so an
    # explicit unsubscribe survives the auto-subscribe that fires when someone
    # comments. Enqueue reads the name back to fan a reply out to every follower.
    """CREATE TABLE IF NOT EXISTS thread_subscriptions (
        thread_bi TEXT NOT NULL,
        subscriber_bi TEXT NOT NULL,
        ts INTEGER NOT NULL,
        data TEXT NOT NULL,
        PRIMARY KEY (thread_bi, subscriber_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_thread_subscriptions_thread ON thread_subscriptions(thread_bi)",
    # One row per completed release-asset download (served by _release_blob), so
    # the Releases tab can show a per-artifact download count and the admin
    # dashboard has a plain event log of them (same treatment as install_diag).
    # repo_bi is the blind index of "<owner>/<repo>"; sha256 is the content
    # address of the asset. ua is the raw User-Agent header, shown as-is in the
    # admin list so operators can spot scripted/bot download traffic. No
    # account/IP is recorded.
    """CREATE TABLE IF NOT EXISTS release_downloads (
        id INTEGER PRIMARY KEY AUTOINCREMENT, repo_bi TEXT NOT NULL,
        sha256 TEXT NOT NULL, ts INTEGER NOT NULL, ua TEXT)""",
    "CREATE INDEX IF NOT EXISTS idx_release_downloads_repo_sha "
    "ON release_downloads(repo_bi, sha256)",
    # Private vulnerability reports submitted via /api/security/report. The body
    # is AES-GCM encrypted at rest (DATA_KEY); only the operator can read the
    # plaintext. No IP or identifying information is stored beyond what the
    # reporter voluntarily provides (contact field). Bounded by MAX_SECURITY_REPORTS
    # so the unauthenticated endpoint can't grow D1 without limit.
    """CREATE TABLE IF NOT EXISTS security_reports (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        ts INTEGER NOT NULL,
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_security_reports_ts ON security_reports(ts)",
    """CREATE TABLE IF NOT EXISTS security_advisories (
        id TEXT PRIMARY KEY,
        repo_bi TEXT NOT NULL,
        state TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        published_at INTEGER NOT NULL DEFAULT 0,
        data TEXT NOT NULL,
        public_data TEXT NOT NULL DEFAULT '')""",
    "CREATE INDEX IF NOT EXISTS idx_security_advisories_repo "
    "ON security_advisories(repo_bi, state, updated_at)",
    "CREATE INDEX IF NOT EXISTS idx_security_advisories_published "
    "ON security_advisories(state, published_at)",
    """CREATE TABLE IF NOT EXISTS security_advisory_members (
        advisory_id TEXT NOT NULL,
        repo_bi TEXT NOT NULL,
        member_bi TEXT NOT NULL,
        role TEXT NOT NULL,
        data TEXT NOT NULL,
        PRIMARY KEY (advisory_id, member_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_security_advisory_members_repo "
    "ON security_advisory_members(repo_bi, member_bi)",
    """CREATE TABLE IF NOT EXISTS security_advisory_comments (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        advisory_id TEXT NOT NULL,
        repo_bi TEXT NOT NULL,
        ts INTEGER NOT NULL,
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_security_advisory_comments_advisory "
    "ON security_advisory_comments(advisory_id, id)",
    """CREATE TABLE IF NOT EXISTS security_advisory_credits (
        advisory_id TEXT NOT NULL,
        repo_bi TEXT NOT NULL,
        user_bi TEXT NOT NULL,
        state TEXT NOT NULL,
        updated_at INTEGER NOT NULL,
        data TEXT NOT NULL,
        PRIMARY KEY (advisory_id, user_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_security_advisory_credits_user "
    "ON security_advisory_credits(user_bi, state)",
    """CREATE TABLE IF NOT EXISTS security_advisory_settings (
        repo_bi TEXT PRIMARY KEY,
        reporting INTEGER NOT NULL,
        updated_at INTEGER NOT NULL)""",
    # Daily automated scan artifacts. repo_bi is the keyed blind index of
    # "<owner>/<repo>"; the strict, already-redacted rich + clipboard envelope
    # is still AES-GCM encrypted in data so private repo names, paths and
    # findings do not become plaintext D1 metadata. The ingest handler retains
    # only the newest fixed number of rows per repository.
    """CREATE TABLE IF NOT EXISTS repo_security_scans (
        repo_bi TEXT NOT NULL,
        scan_id TEXT NOT NULL,
        scanned_at INTEGER NOT NULL,
        received_at INTEGER NOT NULL,
        data TEXT NOT NULL,
        PRIMARY KEY (repo_bi, scan_id))""",
    "CREATE INDEX IF NOT EXISTS idx_repo_security_scans_latest "
    "ON repo_security_scans(repo_bi, scanned_at DESC, received_at DESC)",
    # Owner/security-reviewer false-positive triage is an encrypted overlay,
    # leaving the scanner's immutable content-addressed artifact untouched.
    """CREATE TABLE IF NOT EXISTS repo_security_scan_reviews (
        repo_bi TEXT NOT NULL,
        scan_id TEXT NOT NULL,
        finding_id TEXT NOT NULL,
        reviewer_bi TEXT NOT NULL,
        reviewed_at INTEGER NOT NULL,
        data TEXT NOT NULL,
        PRIMARY KEY (repo_bi, scan_id, finding_id))""",
    "CREATE INDEX IF NOT EXISTS idx_repo_security_scan_reviews_scan "
    "ON repo_security_scan_reviews(repo_bi, scan_id, reviewed_at DESC)",
    # Short-lived native mirror-node scan leases. Repository identity and lease
    # capability are keyed blind indexes; commit/runner metadata stays inside
    # the encrypted data blob. A completed row is retained as the per-repository
    # daily idempotency marker and never contains a reusable bearer token.
    """CREATE TABLE IF NOT EXISTS repo_security_scan_leases (
        repo_bi TEXT PRIMARY KEY,
        lease_token_bi TEXT NOT NULL DEFAULT '',
        status TEXT NOT NULL,
        acquired_at INTEGER NOT NULL,
        expires_at INTEGER NOT NULL,
        next_attempt_at INTEGER NOT NULL DEFAULT 0,
        attempt_count INTEGER NOT NULL DEFAULT 0,
        completed_scan_id TEXT NOT NULL DEFAULT '',
        updated_at INTEGER NOT NULL,
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_repo_security_scan_leases_due "
    "ON repo_security_scan_leases(status, expires_at, next_attempt_at)",
    # Anonymous website feedback. One row per like/dislike click from static
    # pages. `ip_hash` is a keyed blind index of the request IP when present,
    # never the raw address; `user_agent` is a legacy column name containing
    # only a broad client category. Bounded by MAX_FEEDBACK in feedback_handler.
    """CREATE TABLE IF NOT EXISTS feedback (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        ts INTEGER NOT NULL,
        source TEXT NOT NULL,
        vote TEXT NOT NULL,
        path TEXT NOT NULL,
        message TEXT,
        ip_hash TEXT,
        user_agent TEXT)""",
    "CREATE INDEX IF NOT EXISTS idx_feedback_ts ON feedback(ts)",
    "CREATE INDEX IF NOT EXISTS idx_feedback_source_vote ON feedback(source, vote, ts)",
    """CREATE TABLE IF NOT EXISTS profile_follows (
        follower_bi TEXT NOT NULL,
        target_bi TEXT NOT NULL,
        follower_name TEXT NOT NULL,
        target_name TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        PRIMARY KEY (follower_bi, target_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_profile_follows_target "
    "ON profile_follows(target_bi, created_at)",
    "CREATE INDEX IF NOT EXISTS idx_profile_follows_follower "
    "ON profile_follows(follower_bi, created_at)",
    # Legacy per-system /status aggregates.  New writes use the batch tables
    # below so a larger mirror roster does not multiply D1 row writes; these
    # remain readable during the bounded retention handoff from older workers.
    """CREATE TABLE IF NOT EXISTS system_status_daily (
        day_ts INTEGER NOT NULL, system TEXT NOT NULL,
        checks INTEGER NOT NULL DEFAULT 0, failures INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (day_ts, system))""",
    # Hourly breakdown backing the per-day sliver bars on /status. `reason` is a
    # short human-readable note on the most recent failing check that hour (e.g.
    # which path/host check failed), so hovering a degraded/down hour explains
    # why instead of just showing a color.
    """CREATE TABLE IF NOT EXISTS system_status_hourly (
        hour_ts INTEGER NOT NULL, system TEXT NOT NULL,
        checks INTEGER NOT NULL DEFAULT 0, failures INTEGER NOT NULL DEFAULT 0,
        reason TEXT,
        PRIMARY KEY (hour_ts, system))""",
    # Per-minute detail backing the /status page's 60-minute strip. Unlike the
    # daily/hourly rollups above (which only ever store a checks/failures
    # counter, never the individual samples), a minute IS the individual
    # sample, so this is one row per system per minute: ok=1/0 plus the same
    # short failure reason. Short-retention (pruned well under an hour of
    # slack past what the page displays) since nothing else needs history
    # this granular once it has aged out of the visible window.
    """CREATE TABLE IF NOT EXISTS system_status_minute (
        minute_ts INTEGER NOT NULL, system TEXT NOT NULL,
        ok INTEGER NOT NULL DEFAULT 1, reason TEXT,
        PRIMARY KEY (minute_ts, system))""",
    # Roster-independent status storage. Each UTC bucket is one bounded JSON
    # object keyed by public system id, so a 50-node mirror fleet costs the
    # same number of D1 row writes as the base eleven checks. The INTEGER
    # primary keys already provide the exact time-range index these reads use.
    """CREATE TABLE IF NOT EXISTS system_status_minute_batch (
        minute_ts INTEGER PRIMARY KEY, data TEXT NOT NULL,
        CHECK(length(data) <= 131072))""",
    """CREATE TABLE IF NOT EXISTS system_status_hourly_batch (
        hour_ts INTEGER PRIMARY KEY, data TEXT NOT NULL,
        CHECK(length(data) <= 131072))""",
    """CREATE TABLE IF NOT EXISTS system_status_daily_batch (
        day_ts INTEGER PRIMARY KEY, data TEXT NOT NULL,
        CHECK(length(data) <= 65536))""",
    # At-most-once ownership of each minute's status sample. Replayed alarm
    # delivery may call record_status_sample again for the same minute. The
    # daily/hourly rollups are checks-counter increments, so whichever attempt
    # INSERTs this minute's row first owns the sample; later attempts skip it
    # instead of double-counting the hour. `claim` is a random token the winner
    # reads back to recognize itself (D1's Python client exposes no reliable
    # changes() count). Pruned alongside system_status_minute.
    """CREATE TABLE IF NOT EXISTS system_status_sample_claim (
        minute_ts INTEGER PRIMARY KEY, claim TEXT NOT NULL,
        claimed_at INTEGER NOT NULL)""",
    # Edge repository-render monitor state. One row is enough to deduplicate
    # outage/recovery mail while the normal status tables retain the public
    # minute/hour/day history. Pings and email are independently switchable, so
    # each channel keeps its own delivered-transition marker: pinged_state must
    # not be inferred from notified_state or a deployment with mail off would
    # re-announce the same recovery on every cron tick.
    """CREATE TABLE IF NOT EXISTS repository_monitor_state (
        monitor_id TEXT PRIMARY KEY,
        is_up INTEGER NOT NULL DEFAULT 1,
        changed_at INTEGER NOT NULL,
        outage_started_at INTEGER NOT NULL DEFAULT 0,
        checked_at INTEGER NOT NULL,
        reason TEXT,
        notified_state TEXT NOT NULL DEFAULT '',
        pinged_state TEXT NOT NULL DEFAULT '')""",
    # Free-plan usage sampler state: one row per monitored allowance, holding
    # the current UTC day, the last sampled percentage, and the highest
    # threshold already Pinged today so each threshold alerts exactly once.
    """CREATE TABLE IF NOT EXISTS usage_budget_state (
        metric TEXT PRIMARY KEY,
        day TEXT NOT NULL,
        percent REAL NOT NULL DEFAULT 0,
        pinged_threshold INTEGER NOT NULL DEFAULT 0,
        sampled_at INTEGER NOT NULL DEFAULT 0)""",
    # Operator switch for the D1 read guard (d1_read_guard.py), one row set
    # from the desktop D1 tab: auto conserves once the sampler's rows-read
    # percentage for today reaches the threshold, on always, off never.
    """CREATE TABLE IF NOT EXISTS d1_read_guard (
        id INTEGER PRIMARY KEY CHECK (id = 1),
        mode TEXT NOT NULL DEFAULT 'auto' CHECK (mode IN ('auto', 'on', 'off')),
        threshold INTEGER NOT NULL DEFAULT 60
            CHECK (threshold BETWEEN 1 AND 100),
        updated_at INTEGER NOT NULL DEFAULT 0,
        updated_by TEXT NOT NULL DEFAULT '')""",
    # One row per maintenance tick: which batch objects it took, how many
    # phases ran, what they cost in subrequests, and a bounded JSON detail of
    # the per-phase measurements and failures. Durable Object storage cannot
    # answer "what has the cron been doing" — it is per-object and evicted —
    # so this is the history the admin cron report reads. The primary key is
    # the minute slot, so a replayed tick overwrites instead of accumulating,
    # and rows age out on a six-hour window swept twice an hour.
    """CREATE TABLE IF NOT EXISTS cron_tick (
        slot INTEGER PRIMARY KEY,
        ts INTEGER NOT NULL,
        batches INTEGER NOT NULL DEFAULT 0,
        phases INTEGER NOT NULL DEFAULT 0,
        failures INTEGER NOT NULL DEFAULT 0,
        subrequests INTEGER NOT NULL DEFAULT 0,
        ms INTEGER NOT NULL DEFAULT 0,
        data TEXT NOT NULL,
        CHECK(length(data) <= 8192))""",
    "CREATE INDEX IF NOT EXISTS idx_cron_tick_ts ON cron_tick(ts)",
    # Per-repo issue-number allocator for issues the relay files on a user's
    # behalf (ForkBot). Desktop nodes own the real numbering (nextNumber() =
    # highest issue dir + 1), so a relay-created issue can only be given a
    # *proposed* number the desktop honors when the slot is free. Persisting
    # the allocation here lets ForkBot answer with the number immediately and
    # keeps successive ForkBot issues from proposing the same one; it is
    # re-anchored to each catalog publish's issueMaxNumber so it tracks the
    # desktop's real count over time.
    """CREATE TABLE IF NOT EXISTS issue_seq (
        repo_bi TEXT PRIMARY KEY, next_number INTEGER NOT NULL)""",
    # --- ActivityPub federation (migration 0028) -----------------------------
    # Local fediverse actors: one row per user/repo/instance actor that has
    # actually been looked up from the fediverse (rows are minted lazily on the
    # first WebFinger/actor fetch, so repos nobody follows cost nothing).
    # actor_bi = blind_index("ap-actor:<kind>:<handle>"); handle is "alice" for
    # users, "owner.repo" for repos, "instance" for the service actor. The
    # encrypted data blob holds the RSA-2048 private key (PKCS8); pubkey_pem is
    # plaintext because the actor document publishes it anyway.
    """CREATE TABLE IF NOT EXISTS ap_actors (
        actor_bi TEXT PRIMARY KEY, kind TEXT NOT NULL,
        pubkey_pem TEXT NOT NULL, data TEXT NOT NULL,
        created_at INTEGER NOT NULL)""",
    # The relay's secure-fetch signing key is infrastructure state, not a
    # followable repository actor. Keeping it separate ensures ap_actors is an
    # inventory of official repository actors only.
    """CREATE TABLE IF NOT EXISTS ap_service_keys (
        key_name TEXT PRIMARY KEY, pubkey_pem TEXT NOT NULL,
        data TEXT NOT NULL, created_at INTEGER NOT NULL)""",
    # Remote accounts following a local actor. follower_id/inbox URLs stay
    # plaintext: they are public fediverse identifiers needed for cron
    # delivery fan-out without decrypting every row.
    """CREATE TABLE IF NOT EXISTS ap_followers (
        actor_bi TEXT NOT NULL, follower_id TEXT NOT NULL,
        inbox TEXT NOT NULL, shared_inbox TEXT, follower_handle TEXT,
        created_at INTEGER NOT NULL,
        PRIMARY KEY (actor_bi, follower_id))""",
    "CREATE INDEX IF NOT EXISTS idx_ap_followers_actor ON ap_followers(actor_bi)",
    # Cache of remote actor documents (public data, plaintext like `relays`).
    # avatar_url/summary are the follower's published avatar and bio (migration
    # 0078) — the same public presentation any fediverse client shows, cached so
    # "who follows this repo" can be rendered without re-fetching every actor.
    """CREATE TABLE IF NOT EXISTS ap_remote_actors (
        actor_id TEXT PRIMARY KEY, inbox TEXT, shared_inbox TEXT,
        pubkey_pem TEXT, handle TEXT, display_name TEXT, url TEXT,
        updated_at INTEGER NOT NULL, avatar_url TEXT, summary TEXT)""",
    # Local ActivityPub objects (the Notes we publish), served at /ap/o/<uuid>.
    # context_bi = blind_index("ap-context:<owner>/<repo>#<kind>#<ref>") maps a
    # remote reply's inReplyTo back to its forkmesh thread.
    """CREATE TABLE IF NOT EXISTS ap_objects (
        object_uuid TEXT PRIMARY KEY, actor_bi TEXT NOT NULL,
        context_bi TEXT, data TEXT NOT NULL, published INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_ap_objects_actor ON ap_objects(actor_bi)",
    "CREATE INDEX IF NOT EXISTS idx_ap_objects_context ON ap_objects(context_bi)",
    # Inbound remote replies, kept OUTSIDE the Ed25519-signed event log (they
    # cannot carry forkmesh author signatures) and surfaced to clients as
    # clearly-marked fediverse comments. remote_id_bi dedupes redeliveries;
    # parent_remote_id_bi supports Lemmy-style nesting without exposing the
    # remote URL, and lifecycle preserves edits/tombstones/moderation.
    """CREATE TABLE IF NOT EXISTS ap_comments (
        id INTEGER PRIMARY KEY AUTOINCREMENT, context_bi TEXT NOT NULL,
        remote_id_bi TEXT UNIQUE, parent_remote_id_bi TEXT,
        lifecycle TEXT NOT NULL DEFAULT 'active'
            CHECK (lifecycle IN (
                'active', 'edited', 'tombstoned', 'moderated',
                'awaiting-redelivery'
            )),
        data TEXT NOT NULL, ts INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_ap_comments_context ON ap_comments(context_bi, ts)",
    "CREATE INDEX IF NOT EXISTS idx_ap_comments_parent "
    "ON ap_comments(parent_remote_id_bi)",
    # Inbound repo-actor mentions already processed into an issue-inbox
    # submission (or confidently classified as not-an-issue), keyed by
    # blind_index("ap-mention:<note id>") — dedupes fediverse redeliveries so
    # one Mastodon post can never file the same issue twice (migration 0037).
    """CREATE TABLE IF NOT EXISTS ap_mentions (
        remote_id_bi TEXT PRIMARY KEY, ts INTEGER NOT NULL)""",
    # Verified public fediverse feedback shown in World. Remote actor/content,
    # repository names, consent, and errors stay in the encrypted data
    # envelope. A mention is never an issue until an authorized reviewer queues
    # it and the owner node subsequently confirms the materialized issue number
    # (migration 0066).
    """CREATE TABLE IF NOT EXISTS world_fediverse_mentions (
        mention_id TEXT PRIMARY KEY, remote_id_bi TEXT NOT NULL UNIQUE,
        repo_bi TEXT NOT NULL, kind TEXT NOT NULL,
        state TEXT NOT NULL, data TEXT NOT NULL,
        issue_number INTEGER NOT NULL DEFAULT 0,
        create_lease_id TEXT NOT NULL DEFAULT '',
        create_lease_until INTEGER NOT NULL DEFAULT 0,
        followup_state TEXT NOT NULL DEFAULT 'not-requested',
        created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL,
        expires_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_world_fediverse_mentions_feed "
    "ON world_fediverse_mentions(state, created_at DESC)",
    "CREATE INDEX IF NOT EXISTS idx_world_fediverse_mentions_repo "
    "ON world_fediverse_mentions(repo_bi, created_at DESC)",
    """CREATE TABLE IF NOT EXISTS world_fediverse_mention_moderation (
        record_id TEXT PRIMARY KEY, mention_id TEXT NOT NULL,
        action TEXT NOT NULL, reason TEXT NOT NULL,
        actor_bi TEXT NOT NULL, created_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_world_fediverse_mention_moderation "
    "ON world_fediverse_mention_moderation(mention_id, created_at)",
    # Outbound delivery queue (no Cloudflare Queues on the free plan): one row
    # per (activity, destination inbox). A publish inserts rows and best-effort
    # drains a few inline; the cron drains the rest with capped backoff.
    """CREATE TABLE IF NOT EXISTS ap_outbox (
        id INTEGER PRIMARY KEY AUTOINCREMENT, inbox TEXT NOT NULL,
        data TEXT NOT NULL, attempts INTEGER NOT NULL DEFAULT 0,
        next_ts INTEGER NOT NULL, created_at INTEGER NOT NULL,
        dedupe_bi TEXT)""",
    "CREATE INDEX IF NOT EXISTS idx_ap_outbox_next ON ap_outbox(next_ts)",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_ap_outbox_dedupe "
    "ON ap_outbox(dedupe_bi)",
    # Fediverse operator configuration (admin-managed, migration 0029): k/v
    # settings (currently just enabled=1/0, default on when absent) and the
    # remote-domain blocklist (defederation). Plaintext operational config.
    """CREATE TABLE IF NOT EXISTS ap_settings (
        k TEXT PRIMARY KEY, v TEXT NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS ap_blocked_domains (
        domain TEXT PRIMARY KEY, added_by TEXT, added_at INTEGER NOT NULL)""",
    # Per-repo branding (migration 0030): owner-uploaded logo/banner PNGs shown
    # on the repo's fediverse actor (and anywhere else that wants them).
    # repo_bi = blind_index("owner/repo") (== repositories.key_bi); kind is
    # 'logo' or 'banner'; data = encrypted {png: <base64>}. One row per image
    # so a large banner never pushes the combined row past D1's 2 MB cap, and
    # existence checks (SELECT kind, updated_at) never decrypt the blob.
    """CREATE TABLE IF NOT EXISTS repo_media (
        repo_bi TEXT NOT NULL, kind TEXT NOT NULL,
        data TEXT NOT NULL, updated_at INTEGER NOT NULL,
        PRIMARY KEY (repo_bi, kind))""",
    # Web edits of the repo About card queued for the owner's desktop node
    # (migration 0031): the node drains this via GET /api/sync (same
    # drain-on-read semantics as agent_prompts) and writes the change into the
    # repo's committed .forkmesh/info.json — the single source of truth the
    # desktop app shows. One row per repo; the latest web edit wins.
    """CREATE TABLE IF NOT EXISTS about_inbox (
        repo_bi TEXT PRIMARY KEY, data TEXT NOT NULL,
        queued_at INTEGER NOT NULL)""",
    # Branch management requested from the website's Branches page (adhoc
    # #2098): delete a branch, update one from the default branch, or create
    # one. The relay never runs Git, so each request is queued here for the
    # repository's own node, which drains it from GET /api/sync (drain-on-read,
    # like about_inbox), applies it to the checkout it owns, and propagates the
    # result to every mirror. Rows carry the tip the requester saw, so a branch
    # that moved in the meantime is refused by the node rather than applied to
    # commits nobody reviewed.
    """CREATE TABLE IF NOT EXISTS branch_op_inbox (
        id INTEGER PRIMARY KEY AUTOINCREMENT, repo_bi TEXT NOT NULL,
        data TEXT NOT NULL, queued_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_branch_op_inbox_repo "
    "ON branch_op_inbox(repo_bi, id)",
    """CREATE TRIGGER IF NOT EXISTS trg_branch_op_inbox_repo_bound
       BEFORE INSERT ON branch_op_inbox
       WHEN (SELECT COUNT(*) FROM branch_op_inbox
              WHERE repo_bi=NEW.repo_bi) >= 1024
       BEGIN
         SELECT RAISE(ABORT, 'branch_op_inbox_repo_limit');
       END""",
    """CREATE TABLE IF NOT EXISTS branch_op_results (
        id INTEGER PRIMARY KEY AUTOINCREMENT, repo_bi TEXT NOT NULL,
        data TEXT NOT NULL, reported_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_branch_op_results_repo "
    "ON branch_op_results(repo_bi, id)",
    """CREATE TABLE IF NOT EXISTS branch_delete_jobs (
        id INTEGER PRIMARY KEY AUTOINCREMENT, repo_bi TEXT NOT NULL,
        state TEXT NOT NULL, data TEXT NOT NULL,
        created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL,
        lease_until INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_branch_delete_jobs_repo "
    "ON branch_delete_jobs(repo_bi, id)",
    "CREATE INDEX IF NOT EXISTS idx_branch_delete_jobs_state "
    "ON branch_delete_jobs(state, lease_until)",
    # Repositories deleted from the website's repo Settings tab (adhoc #91).
    # The owner's node keeps its local mirror and republishes the catalog
    # record on every heartbeat, so a web delete used to reappear within a
    # minute and looked like it had silently failed. This tombstone makes the
    # deletion stick: automatic publishes for the repo are refused with HTTP
    # 410 until it expires, and an explicit user-initiated publish from the
    # desktop ("publishIntent":"user", owner-signed like any other publish)
    # clears the row so the repo can be shared again.
    """CREATE TABLE IF NOT EXISTS repo_deletions (
        repo_bi TEXT PRIMARY KEY, owner_bi TEXT NOT NULL,
        deleted_at INTEGER NOT NULL, expires_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_repo_deletions_expires "
    "ON repo_deletions(expires_at)",
    # Repo stars (migration 0032): which accounts starred which repo. Keyed by
    # blind indexes only (no signing needed - a star is a plain per-account
    # preference, same trust level as profile_follows), so the count and the
    # caller's own starred state are cheap membership checks.
    """CREATE TABLE IF NOT EXISTS repo_stars (
        repo_bi TEXT NOT NULL, account_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        PRIMARY KEY (repo_bi, account_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_repo_stars_repo ON repo_stars(repo_bi, created_at)",
    "CREATE INDEX IF NOT EXISTS idx_repo_stars_account ON repo_stars(account_bi, created_at)",
    # Fork edges (migration 0129): one row per fork, derived from the forking
    # node's own signed catalog record (its `forkedFrom`), so the parent's repo
    # page can list who forked it without scanning the whole catalog. The fork's
    # owner/name are the same public strings the catalog already publishes for a
    # public repository, kept in plaintext here (like profile_follows' names) so
    # the list is one indexed read instead of a decrypt-per-row pass.
    """CREATE TABLE IF NOT EXISTS repo_forks (
        parent_bi TEXT NOT NULL, fork_bi TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        fork_owner TEXT NOT NULL, fork_name TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        PRIMARY KEY (parent_bi, fork_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_repo_forks_parent "
    "ON repo_forks(parent_bi, created_at)",
    "CREATE INDEX IF NOT EXISTS idx_repo_forks_fork ON repo_forks(fork_bi)",
    "CREATE INDEX IF NOT EXISTS idx_repo_forks_account ON repo_forks(account_bi)",
    # Durable website-created fork jobs (migration 0146). A browser records
    # the request while the user's managed mirror may be offline; that mirror
    # reads its own encrypted jobs with its node signature and acknowledges
    # only after the bare fork and gateway configuration are durable. The
    # target/source/account blind indexes support bounded authorization and
    # idempotency checks without exposing repository choices in plaintext.
    """CREATE TABLE IF NOT EXISTS repository_fork_jobs (
        request_id TEXT PRIMARY KEY,
        target_node_bi TEXT NOT NULL,
        target_repo_bi TEXT NOT NULL UNIQUE,
        source_repo_bi TEXT NOT NULL,
        actor_bi TEXT NOT NULL,
        data TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'pending'
            CHECK (status IN ('pending','completed')),
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        completed_at INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_repository_fork_jobs_target "
    "ON repository_fork_jobs(target_node_bi, status, created_at)",
    "CREATE INDEX IF NOT EXISTS idx_repository_fork_jobs_actor "
    "ON repository_fork_jobs(actor_bi, status, created_at)",
    "CREATE INDEX IF NOT EXISTS idx_repository_fork_jobs_source "
    "ON repository_fork_jobs(source_repo_bi, status, created_at)",
    # "How are we doing?" founder feedback email, sent once per user account
    # ~24h after signup (migration 0033). One row per send is both the
    # once-only guard and the admin's send log (the admin table browser lists
    # every D1 table): `name` is the public node name, nothing else — the
    # recipient email stays only in the encrypted account record.
    """CREATE TABLE IF NOT EXISTS feedback_email_sends (
        account_bi TEXT PRIMARY KEY, name TEXT, sent_at INTEGER NOT NULL)""",
    # Per-repo fediverse switches (migration 0035), owner-managed from the web
    # and Qt repo settings: federate (actor exists at all), broadcastEvents
    # (issues/PRs/releases posted to followers), acceptComments (remote
    # replies ingested as federated comments). repo_bi =
    # blind_index("ap-repo-settings:<owner>/<repo>") with both halves
    # lowercased; data is plaintext JSON of booleans (same operational-config
    # trust level as ap_settings) so the hot unauthenticated AP gates never
    # decrypt. A missing row means "all on" — existing repos keep federating
    # exactly as before.
    """CREATE TABLE IF NOT EXISTS ap_repo_settings (
        repo_bi TEXT PRIMARY KEY, data TEXT NOT NULL,
        updated_at INTEGER NOT NULL)""",
    # Per-repo operational-alert switches (migration 0086), managed by the
    # org admin from the same repo About/settings form as the fediverse
    # switches: statusEmails turns the "[ForkMesh outage]" /
    # "[ForkMesh recovered]" status and cron-watchdog mail on. repo_bi =
    # blind_index("repo-alert-settings:<owner>/<repo>") with both halves
    # lowercased; data is plaintext JSON of booleans (same operational-config
    # trust level as ap_repo_settings). A missing row means "all off" — a
    # deployment mails nobody until an admin opts in.
    """CREATE TABLE IF NOT EXISTS repo_alert_settings (
        repo_bi TEXT PRIMARY KEY, data TEXT NOT NULL,
        updated_at INTEGER NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS repo_feature_settings (
        repo_bi TEXT PRIMARY KEY, data TEXT NOT NULL,
        updated_at INTEGER NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS pull_review_ledger (
        repo_bi TEXT NOT NULL, pull_number INTEGER NOT NULL,
        account TEXT NOT NULL, state TEXT NOT NULL,
        updated_at INTEGER NOT NULL,
        PRIMARY KEY (repo_bi, pull_number, account))""",
    # Encrypted, bounded automatic-update queues (migration 0046). Human
    # repository/org names and event metadata live only inside `data`;
    # scope_bi is a blind index. next_ts/last_published_at are operational
    # timestamps used by the bounded cron query. Private repositories never
    # receive a row.
    """CREATE TABLE IF NOT EXISTS ap_digest_queues (
        scope_bi TEXT PRIMARY KEY, next_ts INTEGER NOT NULL,
        last_published_at INTEGER NOT NULL DEFAULT 0,
        data TEXT NOT NULL, updated_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_ap_digest_queues_due "
    "ON ap_digest_queues(next_ts, updated_at)",
    # Org owners/admins may independently disable the org-alias digest for
    # linked repositories. The backing repo owner's setting remains an AND
    # gate, so organization administration never grants publication rights.
    """CREATE TABLE IF NOT EXISTS ap_org_digest_settings (
        org_bi TEXT PRIMARY KEY, data TEXT NOT NULL,
        updated_at INTEGER NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS org_pull_rules (
        org_bi TEXT PRIMARY KEY, data TEXT NOT NULL,
        updated_by TEXT NOT NULL DEFAULT '',
        updated_at INTEGER NOT NULL)""",
    # Bounded contribution projections for native profile activity. Every
    # identity and repository join key is a blind index; human-readable labels
    # remain in encrypted data, and raw Git author names/emails are never stored.
    """CREATE TABLE IF NOT EXISTS profile_contribution_receipts (
        generation_bi TEXT PRIMARY KEY,
        snapshot_hash TEXT NOT NULL,
        source_account_bi TEXT NOT NULL,
        source_repo_bi TEXT NOT NULL,
        captured_at INTEGER NOT NULL,
        head TEXT NOT NULL,
        day_rows INTEGER NOT NULL,
        language_rows INTEGER NOT NULL,
        created_at INTEGER NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS profile_contribution_projects (
        source_repo_bi TEXT PRIMARY KEY,
        source_account_bi TEXT NOT NULL,
        owner_user_bi TEXT NOT NULL,
        project_bi TEXT NOT NULL,
        first_public_day TEXT NOT NULL,
        is_public INTEGER NOT NULL DEFAULT 1,
        active_generation_bi TEXT,
        captured_at INTEGER NOT NULL DEFAULT 0,
        verified_from TEXT,
        data TEXT NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS profile_contribution_days (
        generation_bi TEXT NOT NULL,
        subject_user_bi TEXT NOT NULL,
        source_account_bi TEXT NOT NULL,
        source_repo_bi TEXT NOT NULL,
        project_bi TEXT NOT NULL,
        day TEXT NOT NULL,
        commits INTEGER NOT NULL DEFAULT 0,
        issues INTEGER NOT NULL DEFAULT 0,
        pulls INTEGER NOT NULL DEFAULT 0,
        reviews INTEGER NOT NULL DEFAULT 0,
        captured_at INTEGER NOT NULL,
        data TEXT NOT NULL,
        PRIMARY KEY (generation_bi, subject_user_bi, source_repo_bi, day))""",
    """CREATE TABLE IF NOT EXISTS profile_contribution_languages (
        generation_bi TEXT NOT NULL,
        owner_user_bi TEXT NOT NULL,
        source_repo_bi TEXT NOT NULL,
        project_bi TEXT NOT NULL,
        language TEXT NOT NULL,
        bytes INTEGER NOT NULL,
        files INTEGER NOT NULL,
        captured_at INTEGER NOT NULL,
        PRIMARY KEY (generation_bi, owner_user_bi, source_repo_bi, language))""",
    """CREATE INDEX IF NOT EXISTS idx_profile_contribution_projects_owner_public
        ON profile_contribution_projects(owner_user_bi, is_public)""",
    """CREATE INDEX IF NOT EXISTS idx_profile_contribution_projects_project_public
        ON profile_contribution_projects(project_bi, is_public)""",
    """CREATE INDEX IF NOT EXISTS idx_profile_contribution_days_subject_day
        ON profile_contribution_days(subject_user_bi, day)""",
    """CREATE INDEX IF NOT EXISTS idx_profile_contribution_days_repo_generation
        ON profile_contribution_days(source_repo_bi, generation_bi)""",
    """CREATE INDEX IF NOT EXISTS idx_profile_contribution_languages_owner_generation
        ON profile_contribution_languages(owner_user_bi, generation_bi)""",
    # --- Organizations + teams (issue #388, migration 0038) ------------------
    # User-created org namespaces that serve linked repos at /<org>/<repo>
    # instead of the hosting node's name. Org identity and membership rosters
    # are public data (same trust level as users.username / profile_follows),
    # so names and roles stay plaintext for cheap listing;
    # per-org extras (display name, description, creator) live in the encrypted
    # data blob. org_bi = blind_index("org:<name>").
    """CREATE TABLE IF NOT EXISTS orgs (
        org_bi TEXT PRIMARY KEY,
        name TEXT NOT NULL,
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL)""",
    # Who belongs to an org and at what role ('owner' | 'admin' | 'member').
    """CREATE TABLE IF NOT EXISTS org_members (
        org_bi TEXT NOT NULL, member_bi TEXT NOT NULL,
        role TEXT NOT NULL DEFAULT 'member',
        name TEXT NOT NULL, created_at INTEGER NOT NULL,
        PRIMARY KEY (org_bi, member_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_org_members_member ON org_members(member_bi)",
    # Teams: a named subset of org members holding one repository permission
    # ('read' | 'write' | 'maintain' | 'admin') over the org's linked repos.
    """CREATE TABLE IF NOT EXISTS org_teams (
        org_bi TEXT NOT NULL, team TEXT NOT NULL,
        permission TEXT NOT NULL DEFAULT 'read',
        created_at INTEGER NOT NULL,
        PRIMARY KEY (org_bi, team))""",
    """CREATE TABLE IF NOT EXISTS org_team_members (
        org_bi TEXT NOT NULL, team TEXT NOT NULL, member_bi TEXT NOT NULL,
        name TEXT NOT NULL, created_at INTEGER NOT NULL,
        PRIMARY KEY (org_bi, team, member_bi))""",
    # Alias map: which node-hosted repo each /<org>/<repo> URL serves. The
    # reverse index lets the push gate find the orgs a node repo is linked
    # under without scanning.
    """CREATE TABLE IF NOT EXISTS org_repos (
        org_bi TEXT NOT NULL, repo TEXT NOT NULL, node_owner TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        PRIMARY KEY (org_bi, repo))""",
    "CREATE INDEX IF NOT EXISTS idx_org_repos_node ON org_repos(node_owner, repo)",
    # Organization-scoped coding bots are intentionally separate from the
    # owner-only repo_agents/agent_prompts E2EE channel. Members see only
    # sessions for an org they currently belong to; every prompt is encrypted
    # at rest until the selected node claims it. (status still defaults to the
    # retired 'security_pending' value; the column default is not migrated.)
    """CREATE TABLE IF NOT EXISTS org_agent_sessions (
        session_id TEXT PRIMARY KEY,
        org_bi TEXT NOT NULL,
        repo TEXT NOT NULL,
        target_node TEXT NOT NULL,
        provider TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'security_pending',
        created_by_bi TEXT NOT NULL,
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        completed_at INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_org_agent_sessions_scope "
    "ON org_agent_sessions(org_bi, repo, updated_at DESC)",
    """CREATE TABLE IF NOT EXISTS org_agent_jobs (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        session_id TEXT NOT NULL,
        org_bi TEXT NOT NULL,
        target_node TEXT NOT NULL,
        repo TEXT NOT NULL,
        provider TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'queued',
        lease_id TEXT NOT NULL DEFAULT '',
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_org_agent_jobs_drain "
    "ON org_agent_jobs(target_node, repo, status, id)",
    "CREATE INDEX IF NOT EXISTS idx_org_agent_jobs_session "
    "ON org_agent_jobs(session_id, status)",
    # Images attached to a queued agent prompt, sealed like the prompt and
    # split into D1-row-sized chunks. Deleted with the session, once the node
    # holding the job's lease reports on it, or by the orphan sweep.
    """CREATE TABLE IF NOT EXISTS org_agent_attachments (
        session_id TEXT NOT NULL,
        attachment_id TEXT NOT NULL,
        chunk_index INTEGER NOT NULL,
        org_bi TEXT NOT NULL,
        repo TEXT NOT NULL,
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        PRIMARY KEY (session_id, attachment_id, chunk_index))""",
    "CREATE INDEX IF NOT EXISTS idx_org_agent_attachments_created "
    "ON org_agent_attachments(created_at)",
    # A user may explicitly opt into the Town Square's recently-inactive
    # seating area. The account key is a blind index and the generalized public
    # card is encrypted; exact paths, activity history, IP data, and location
    # are never stored. Rows expire automatically after a bounded window.
    """CREATE TABLE IF NOT EXISTS world_inactive_presence (
        account_bi TEXT PRIMARY KEY,
        data TEXT NOT NULL,
        updated_at INTEGER NOT NULL,
        expires_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_world_inactive_presence_expiry "
    "ON world_inactive_presence(expires_at)",
    # Server-authoritative aggregate time and steps in the ForkMesh World
    # (migrations 0076 and 0137). The account key is a blind index; no display
    # name, IP, route, movement history, or client-reported duration is stored.
    # The generation makes a signed ticket continuation single-use even when
    # refreshes race across tabs.
    """CREATE TABLE IF NOT EXISTS world_user_activity (
        account_bi TEXT PRIMARY KEY,
        total_active_ms INTEGER NOT NULL DEFAULT 0
            CHECK (total_active_ms >= 0),
        total_steps INTEGER NOT NULL DEFAULT 0
            CHECK (total_steps >= 0),
        last_touch_at INTEGER NOT NULL DEFAULT 0
            CHECK (last_touch_at >= 0),
        generation INTEGER NOT NULL DEFAULT 1
            CHECK (generation > 0),
        last_credit_ms INTEGER NOT NULL DEFAULT 0
            CHECK (last_credit_ms >= 0),
        updated_at INTEGER NOT NULL DEFAULT 0
            CHECK (updated_at >= 0))""",
    # Privacy-preserving approximate unique counts for the Arrival Grid
    # (migration 0077). The edge transiently HMACs its observed source address
    # plus a coarse browser/OS/device category, then stores only HyperLogLog
    # register/rank projections. Temporal HMAC context rotates at UTC midnight
    # and is domain-separated from the all-time context. No raw address, raw
    # User-Agent, account, digest, or movement history enters D1. A
    # 1,024-register all-time sketch plus at most 50 hours of 10-minute
    # sketches fixes the maximum footprint.
    """CREATE TABLE IF NOT EXISTS world_visit_unique_hll (
        bucket_start INTEGER NOT NULL,
        register_id INTEGER NOT NULL
            CHECK (register_id >= 0 AND register_id < 1024),
        rank INTEGER NOT NULL CHECK (rank >= 1 AND rank <= 247),
        PRIMARY KEY (bucket_start, register_id)
    ) WITHOUT ROWID""",
    # Content-free relay accounting behind the System Capacity platform
    # (migration 0083). One row per Durable Object binding: how many bytes and
    # frames that class has relayed. No room key, account, peer id, instance
    # id, or message content is stored, and each Durable Object folds its
    # in-memory counters in at most once a minute.
    """CREATE TABLE IF NOT EXISTS durable_object_traffic (
        binding TEXT PRIMARY KEY,
        bytes_in INTEGER NOT NULL DEFAULT 0 CHECK (bytes_in >= 0),
        bytes_out INTEGER NOT NULL DEFAULT 0 CHECK (bytes_out >= 0),
        messages INTEGER NOT NULL DEFAULT 0 CHECK (messages >= 0),
        updated_at INTEGER NOT NULL DEFAULT 0 CHECK (updated_at >= 0))""",
    # Minute aggregates for platform-aborted Durable Object requests
    # (migration 0116). A reconnect storm increments one content-free row
    # instead of writing one error_log row per affected user. /status consumes
    # these counters, preserving incident visibility without flooding the
    # operator's actionable Worker-error queue.
    """CREATE TABLE IF NOT EXISTS durable_object_abort_minute (
        minute_ts INTEGER PRIMARY KEY CHECK (minute_ts >= 0),
        aborts INTEGER NOT NULL DEFAULT 0 CHECK (aborts >= 0),
        duration_aborts INTEGER NOT NULL DEFAULT 0
            CHECK (duration_aborts >= 0),
        updated_at INTEGER NOT NULL DEFAULT 0 CHECK (updated_at >= 0))""",
    # Aggregate-only Town Square arrival odometer for the Arrival Grid plaque
    # (migration 0074). Each accepted world join adds one to a coarse
    # 10-minute UTC bucket; rows never carry a visitor id, country, IP, or
    # session field, so the plaque totals stay outside the world's no-history
    # privacy contract. Buckets older than two days fold into the -1 archive
    # row, keeping the table bounded.
    """CREATE TABLE IF NOT EXISTS world_visit_stats (
        bucket_start INTEGER PRIMARY KEY,
        visits INTEGER NOT NULL DEFAULT 0)""",
    # Public-only, consent-attested social directory (migrations 0052/0061).
    # `data` is not encrypted because the validator permits only fields that
    # are intentionally public; account/follower/private activity data has no
    # storage field here.
    """CREATE TABLE IF NOT EXISTS world_fediverse_instances (
        instance_id TEXT PRIMARY KEY,
        kind TEXT NOT NULL CHECK (
            kind IN ('mastodon', 'lemmy', 'x', 'reddit')),
        host TEXT NOT NULL UNIQUE,
        url TEXT NOT NULL UNIQUE,
        data TEXT NOT NULL,
        created_by_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_world_fediverse_kind_updated "
    "ON world_fediverse_instances(kind, updated_at)",
    # Shared-media rooms coordinate bounded external HTTPS provider links.
    # ForkMesh does not store, proxy, autoplay, record, or rebroadcast media.
    """CREATE TABLE IF NOT EXISTS world_media_spaces (
        space_id TEXT PRIMARY KEY,
        name TEXT NOT NULL,
        description TEXT NOT NULL DEFAULT '',
        session_type TEXT NOT NULL CHECK (session_type IN (
            'listening-room', 'dj-session', 'video-room', 'watch-party',
            'repository-launch', 'organization-presentation')),
        owner_bi TEXT NOT NULL,
        owner_label TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'active'
            CHECK (status IN ('active', 'archived')),
        playback_state TEXT NOT NULL DEFAULT 'idle'
            CHECK (playback_state IN ('idle', 'stopped')),
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        last_activity_at INTEGER NOT NULL,
        archived_at INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_world_media_spaces_owner "
    "ON world_media_spaces(owner_bi, status, updated_at)",
    "CREATE INDEX IF NOT EXISTS idx_world_media_spaces_active "
    "ON world_media_spaces(status, last_activity_at)",
    # One server UTC clock coordinates opt-in provider playback. It contains no
    # media bytes and cannot authorize autoplay or rebroadcasting.
    """CREATE TABLE IF NOT EXISTS world_media_playback (
        space_id TEXT PRIMARY KEY,
        item_id TEXT NOT NULL DEFAULT '',
        state TEXT NOT NULL DEFAULT 'idle'
            CHECK (state IN ('idle', 'playing', 'paused', 'stopped')),
        position_ms INTEGER NOT NULL DEFAULT 0
            CHECK (position_ms >= 0 AND position_ms <= 604800000),
        started_at INTEGER NOT NULL DEFAULT 0,
        changed_at INTEGER NOT NULL,
        changed_by_bi TEXT NOT NULL DEFAULT '',
        revision INTEGER NOT NULL DEFAULT 0 CHECK (revision >= 0),
        change_id TEXT NOT NULL DEFAULT '')""",
    "CREATE INDEX IF NOT EXISTS idx_world_media_playback_changed "
    "ON world_media_playback(changed_at)",
    # Ownership comes from spaces.owner_bi; only explicit moderator grants are
    # rows. Platform roles do not silently inherit room control.
    """CREATE TABLE IF NOT EXISTS world_media_roles (
        role_id TEXT PRIMARY KEY,
        space_id TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        account_label TEXT NOT NULL,
        role TEXT NOT NULL CHECK (role = 'moderator'),
        granted_by_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        UNIQUE (space_id, account_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_world_media_roles_account "
    "ON world_media_roles(account_bi, space_id)",
    """CREATE TABLE IF NOT EXISTS world_media_items (
        item_id TEXT PRIMARY KEY,
        space_id TEXT NOT NULL,
        position INTEGER NOT NULL,
        status TEXT NOT NULL DEFAULT 'queued'
            CHECK (status IN ('queued', 'stopped', 'removed')),
        data TEXT NOT NULL,
        added_by_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        removed_at INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_world_media_items_space "
    "ON world_media_items(space_id, status, position, created_at)",
    "CREATE INDEX IF NOT EXISTS idx_world_media_items_retention "
    "ON world_media_items(status, removed_at)",
    """CREATE TABLE IF NOT EXISTS world_media_schedules (
        schedule_id TEXT PRIMARY KEY,
        space_id TEXT NOT NULL,
        item_id TEXT NOT NULL DEFAULT '',
        status TEXT NOT NULL DEFAULT 'scheduled'
            CHECK (status IN ('scheduled', 'cancelled', 'completed')),
        starts_at INTEGER NOT NULL,
        ends_at INTEGER NOT NULL,
        data TEXT NOT NULL,
        created_by_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        cancelled_at INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_world_media_schedules_space "
    "ON world_media_schedules(space_id, status, starts_at)",
    "CREATE INDEX IF NOT EXISTS idx_world_media_schedules_retention "
    "ON world_media_schedules(status, ends_at, cancelled_at)",
    # --- Security authorization and audit plane (migration 0043) -------------
    # Explicit least-privilege grants. Guest/registered-user are derived states;
    # this table holds elevated or scoped roles only. scope_bi is a blind index
    # of the organization/repository identifier, never a private repo name.
    """CREATE TABLE IF NOT EXISTS role_grants (
        account_bi TEXT NOT NULL,
        role TEXT NOT NULL,
        scope_type TEXT NOT NULL DEFAULT 'platform',
        scope_bi TEXT NOT NULL DEFAULT '',
        granted_by_bi TEXT,
        granted_at INTEGER NOT NULL,
        expires_at INTEGER NOT NULL DEFAULT 0,
        revoked_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (account_bi, role, scope_type, scope_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_role_grants_role "
    "ON role_grants(role, scope_type, scope_bi, revoked_at, expires_at)",
    # Self-serve plan-trial register (issue #488): one trial per account,
    # started without support intervention. The entitlement itself is a
    # promotion-scope role grant that expires on its own, so this table is
    # purely the auditable register the trialsCompleted outcome metric
    # reconciles to; deleting it never revokes or extends anyone's access.
    """CREATE TABLE IF NOT EXISTS plan_trials (
        account_bi TEXT PRIMARY KEY,
        tier TEXT NOT NULL CHECK (tier IN ('supporter')),
        started_at INTEGER NOT NULL,
        ends_at INTEGER NOT NULL)""",
    # ForkMesh Ads (app/src/ad_network.py): member-written text ads that run
    # only after a human review. The creative is public by design, so it is
    # stored in the clear; the submitter's name is sealed and only a reviewer
    # opens it. approved_hash pins an approval to the exact words reviewed:
    # an edit changes content_hash and the ad stops serving until re-reviewed.
    """CREATE TABLE IF NOT EXISTS ad_network_ads (
        ad_id TEXT PRIMARY KEY,
        account_bi TEXT NOT NULL,
        owner_sealed TEXT NOT NULL DEFAULT '',
        status TEXT NOT NULL DEFAULT 'draft' CHECK (status IN
            ('draft','pending','approved','rejected','suspended',
             'withdrawn')),
        paused INTEGER NOT NULL DEFAULT 0,
        headline TEXT NOT NULL,
        body TEXT NOT NULL DEFAULT '',
        cta TEXT NOT NULL DEFAULT '',
        sponsor TEXT NOT NULL,
        url TEXT NOT NULL,
        category TEXT NOT NULL,
        placements TEXT NOT NULL DEFAULT '',
        content_hash TEXT NOT NULL,
        approved_hash TEXT NOT NULL DEFAULT '',
        screen_flags TEXT NOT NULL DEFAULT '',
        owner_note TEXT NOT NULL DEFAULT '',
        review_reason TEXT NOT NULL DEFAULT '',
        review_codes TEXT NOT NULL DEFAULT '',
        starts_at INTEGER NOT NULL DEFAULT 0,
        ends_at INTEGER NOT NULL DEFAULT 0,
        submitted_at INTEGER NOT NULL DEFAULT 0,
        reviewed_at INTEGER NOT NULL DEFAULT 0,
        approved_until INTEGER NOT NULL DEFAULT 0,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_ad_network_ads_account "
    "ON ad_network_ads(account_bi, updated_at)",
    "CREATE INDEX IF NOT EXISTS idx_ad_network_ads_status "
    "ON ad_network_ads(status, submitted_at)",
    # Append-only decision trail. It holds the content hash a reviewer ruled
    # on, never ad text, so it survives the ad's deletion as evidence.
    """CREATE TABLE IF NOT EXISTS ad_network_reviews (
        review_id TEXT PRIMARY KEY,
        ad_id TEXT NOT NULL,
        action TEXT NOT NULL CHECK (action IN
            ('approve','reject','suspend','reinstate','auto-suspend')),
        reviewer_bi TEXT NOT NULL DEFAULT '',
        self_review INTEGER NOT NULL DEFAULT 0,
        reason TEXT NOT NULL DEFAULT '',
        codes TEXT NOT NULL DEFAULT '',
        content_hash TEXT NOT NULL DEFAULT '',
        created_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_ad_network_reviews_ad "
    "ON ad_network_reviews(ad_id, created_at)",
    # One open report per member per ad; the primary key is the vote limit.
    """CREATE TABLE IF NOT EXISTS ad_network_reports (
        ad_id TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        reason TEXT NOT NULL,
        note TEXT NOT NULL DEFAULT '',
        created_at INTEGER NOT NULL,
        resolved_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (ad_id, account_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_ad_network_reports_account "
    "ON ad_network_reports(account_bi)",
    # Aggregate click counters only: no viewer, address or identifier. day is
    # a UTC yyyymmdd integer.
    """CREATE TABLE IF NOT EXISTS ad_network_daily_stats (
        ad_id TEXT NOT NULL,
        day INTEGER NOT NULL,
        placement TEXT NOT NULL,
        clicks INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (ad_id, day, placement))""",
    # Repository webhooks (app/src/repository_webhooks.py): an owner-registered
    # third-party endpoint that receives signed forkmesh-webhook-v1 deliveries
    # when the repository moves. The destination URL, the HMAC signing secret
    # and the subscribed event list all live inside the encrypted blob — an
    # operator reading this table learns only that a repository has a hook and
    # how its last delivery went. seed_key is non-empty for a hook ForkMesh
    # itself declares for a repository it operates.
    """CREATE TABLE IF NOT EXISTS repo_webhooks (
        webhook_id TEXT PRIMARY KEY,
        repo_bi TEXT NOT NULL,
        active INTEGER NOT NULL DEFAULT 1,
        seed_key TEXT NOT NULL DEFAULT '',
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        last_status INTEGER NOT NULL DEFAULT 0,
        last_delivery_at INTEGER NOT NULL DEFAULT 0,
        failures INTEGER NOT NULL DEFAULT 0,
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_repo_webhooks_repo "
    "ON repo_webhooks(repo_bi, created_at DESC)",
    # A declared hook is materialized at most once per repository even if the
    # bookkeeping write below it loses a race: the INSERT OR IGNORE that
    # creates it has a real constraint to be ignored by.
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_repo_webhooks_seed "
    "ON repo_webhooks(repo_bi, seed_key) WHERE seed_key <> ''",
    # One row per declared hook actually materialized, so deleting a seeded
    # webhook is final instead of being undone by the next event.
    """CREATE TABLE IF NOT EXISTS repo_webhook_seeds (
        repo_bi TEXT NOT NULL,
        seed_key TEXT NOT NULL,
        applied_at INTEGER NOT NULL,
        PRIMARY KEY (repo_bi, seed_key))""",
    # forkmesh-event-v1 orders events by a sequence that increases by exactly
    # one per observed change (conformance contract, policies.ordering), so a
    # receiver can detect a gap. One counter per ENDPOINT: an endpoint only
    # sees the event types it subscribed to, so a per-repository counter would
    # make somebody else's event look like a lost delivery to this one.
    """CREATE TABLE IF NOT EXISTS repo_webhook_sequence (
        webhook_id TEXT PRIMARY KEY,
        repo_bi TEXT NOT NULL,
        seq INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_repo_webhook_sequence_repo "
    "ON repo_webhook_sequence(repo_bi)",
    # Pending deliveries. A third-party endpoint must never add its timeout to
    # the push, merge or release request that produced the event, so every
    # delivery is queued here and drained by the webhook-outbox cron phase
    # (plus an immediate best-effort drain right after the enqueue).
    """CREATE TABLE IF NOT EXISTS repo_webhook_outbox (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        webhook_id TEXT NOT NULL,
        repo_bi TEXT NOT NULL,
        event_id TEXT NOT NULL,
        attempts INTEGER NOT NULL DEFAULT 0,
        next_ts INTEGER NOT NULL,
        created_at INTEGER NOT NULL,
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_repo_webhook_outbox_due "
    "ON repo_webhook_outbox(next_ts, id)",
    "CREATE INDEX IF NOT EXISTS idx_repo_webhook_outbox_hook "
    "ON repo_webhook_outbox(webhook_id)",
    # The owner's own delivery log: what was attempted, what answered, and how
    # it was settled. Trimmed to the newest rows per hook.
    """CREATE TABLE IF NOT EXISTS repo_webhook_deliveries (
        delivery_id TEXT PRIMARY KEY,
        webhook_id TEXT NOT NULL,
        repo_bi TEXT NOT NULL,
        event_type TEXT NOT NULL,
        status INTEGER NOT NULL DEFAULT 0,
        attempts INTEGER NOT NULL DEFAULT 0,
        outcome TEXT NOT NULL DEFAULT '',
        ts INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_repo_webhook_deliveries_hook "
    "ON repo_webhook_deliveries(webhook_id, ts DESC)",
    "CREATE INDEX IF NOT EXISTS idx_repo_webhook_deliveries_repo "
    "ON repo_webhook_deliveries(repo_bi, ts DESC)",
    # Patreon OAuth credentials are intentionally ephemeral. The durable link
    # stores only blind indexes and an encrypted, display-safe projection.
    """CREATE TABLE IF NOT EXISTS patreon_oauth_states (
        state_hash TEXT PRIMARY KEY,
        account_bi TEXT NOT NULL,
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        expires_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_patreon_oauth_states_account_expiry "
    "ON patreon_oauth_states(account_bi, expires_at)",
    """CREATE TABLE IF NOT EXISTS patreon_account_links (
        account_bi TEXT PRIMARY KEY,
        patreon_user_bi TEXT NOT NULL UNIQUE,
        data TEXT NOT NULL,
        linked_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS patreon_webhook_events (
        event_id TEXT PRIMARY KEY,
        received_at INTEGER NOT NULL,
        event_type TEXT NOT NULL,
        account_bi TEXT NOT NULL DEFAULT '')""",
    "CREATE INDEX IF NOT EXISTS idx_patreon_webhook_events_received "
    "ON patreon_webhook_events(received_at)",
    """CREATE TABLE IF NOT EXISTS patreon_reconciliation_generations (
        member_bi TEXT PRIMARY KEY,
        generation INTEGER NOT NULL CHECK (generation >= 1),
        updated_at INTEGER NOT NULL CHECK (updated_at >= 0))""",
    # Append-only, content-free audit trail for sensitive state changes. Targets
    # are blind indexes and details are an allowlisted JSON object with no
    # credentials, bodies, addresses, private repo names, or raw IPs.
    """CREATE TABLE IF NOT EXISTS sensitive_audit_log (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        ts INTEGER NOT NULL,
        actor_bi TEXT,
        actor_label TEXT NOT NULL DEFAULT '',
        action TEXT NOT NULL,
        target_type TEXT NOT NULL DEFAULT '',
        target_token TEXT NOT NULL DEFAULT '',
        outcome TEXT NOT NULL,
        details TEXT NOT NULL DEFAULT '{}')""",
    "CREATE INDEX IF NOT EXISTS idx_sensitive_audit_ts "
    "ON sensitive_audit_log(ts)",
    "CREATE INDEX IF NOT EXISTS idx_sensitive_audit_actor "
    "ON sensitive_audit_log(actor_bi, ts)",
    # Public encryption-key registrations contain public recipient bundles only.
    # Private halves remain on an owner's device. Revocation/rotation never asks
    # the Worker to possess a decryption key.
    """CREATE TABLE IF NOT EXISTS owner_encryption_keys (
        account_bi TEXT NOT NULL,
        key_id TEXT NOT NULL,
        public_bundle TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        revoked_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (account_bi, key_id))""",
    """CREATE TABLE IF NOT EXISTS repo_privacy_policy (
        repo_bi TEXT PRIMARY KEY,
        owner_bi TEXT NOT NULL,
        owner_key_id TEXT NOT NULL DEFAULT '',
        require_agent_e2ee INTEGER NOT NULL DEFAULT 1,
        require_mirror_encryption INTEGER NOT NULL DEFAULT 1,
        updated_at INTEGER NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS repo_terms_flags (
        repo_bi TEXT PRIMARY KEY,
        active INTEGER NOT NULL DEFAULT 1 CHECK (active IN (0, 1)),
        category TEXT NOT NULL CHECK (
            category IN ('spam','malware','harassment','illegal','other')),
        data TEXT NOT NULL,
        updated_by_bi TEXT NOT NULL,
        updated_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_repo_terms_flags_active "
    "ON repo_terms_flags(active, updated_at DESC)",
    # Non-custodial on-chain actions. The Worker stores an unsigned transfer
    # plan and later an on-chain signature; a designated local/external signer
    # owns the wallet and signs outside Cloudflare.
    """CREATE TABLE IF NOT EXISTS chain_intents (
        intent_id TEXT PRIMARY KEY,
        kind TEXT NOT NULL,
        source_address TEXT NOT NULL,
        signer_account TEXT NOT NULL DEFAULT '',
        status TEXT NOT NULL DEFAULT 'pending_signature',
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        expires_at INTEGER NOT NULL DEFAULT 0,
        tx_signature TEXT NOT NULL DEFAULT '',
        completed_at INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_chain_intents_status "
    "ON chain_intents(status, created_at)",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_chain_intents_tx_signature "
    "ON chain_intents(tx_signature) WHERE tx_signature<>''",
    # Bounded, read-only history reconciliation for public addresses ForkMesh
    # can prove belong to the system. The cursor walks Solana newest-to-oldest;
    # discovered System transfers are immutable public receipts. Member wallet
    # histories are deliberately never enrolled.
    """CREATE TABLE IF NOT EXISTS ledger_backfill_sources (
        source_id TEXT PRIMARY KEY,
        source_kind TEXT NOT NULL,
        address TEXT NOT NULL,
        network TEXT NOT NULL,
        subject_repo_bi TEXT NOT NULL DEFAULT '',
        subject_kind TEXT NOT NULL DEFAULT '',
        subject_number INTEGER NOT NULL DEFAULT 0,
        before_signature TEXT NOT NULL DEFAULT '',
        completed INTEGER NOT NULL DEFAULT 0 CHECK (completed IN (0, 1)),
        updated_at INTEGER NOT NULL DEFAULT 0,
        UNIQUE (network, address))""",
    "CREATE INDEX IF NOT EXISTS idx_ledger_backfill_sources_pending "
    "ON ledger_backfill_sources(completed, updated_at, source_id)",
    """CREATE TABLE IF NOT EXISTS ledger_backfill_transactions (
        entry_id TEXT PRIMARY KEY,
        network TEXT NOT NULL,
        signature TEXT NOT NULL,
        instruction_index INTEGER NOT NULL,
        block_time INTEGER NOT NULL DEFAULT 0,
        source_address TEXT NOT NULL,
        destination_address TEXT NOT NULL,
        amount_lamports INTEGER NOT NULL CHECK (amount_lamports > 0),
        observed_address TEXT NOT NULL,
        discovered_from TEXT NOT NULL,
        UNIQUE (network, signature, instruction_index))""",
    "CREATE INDEX IF NOT EXISTS idx_ledger_backfill_transactions_time "
    "ON ledger_backfill_transactions(block_time DESC, entry_id DESC)",
    "CREATE INDEX IF NOT EXISTS idx_ledger_backfill_transactions_signature "
    "ON ledger_backfill_transactions(signature)",
    # Pending rewards are ledger allocations only: funds remain at the external
    # source wallet until a self-custodial recipient address is supplied.
    """CREATE TABLE IF NOT EXISTS pending_rewards (
        reward_id TEXT PRIMARY KEY,
        recipient_bi TEXT NOT NULL,
        recipient_name TEXT NOT NULL DEFAULT '',
        source_address TEXT NOT NULL,
        amount_lamports INTEGER NOT NULL,
        wallet_address TEXT NOT NULL DEFAULT '',
        status TEXT NOT NULL DEFAULT 'pending_wallet',
        created_at INTEGER NOT NULL,
        expires_at INTEGER NOT NULL,
        intent_id TEXT NOT NULL DEFAULT '',
        tx_signature TEXT NOT NULL DEFAULT '')""",
    "CREATE INDEX IF NOT EXISTS idx_pending_rewards_recipient "
    "ON pending_rewards(recipient_bi, status, expires_at)",
    "CREATE INDEX IF NOT EXISTS idx_pending_rewards_tx_signature "
    "ON pending_rewards(tx_signature) WHERE tx_signature<>''",
    # Server-observed continuity for reward eligibility. A node cannot submit
    # its own uptime counter: the Worker advances this window only while its
    # signed HTTPS endpoint and forkmesh/forkmesh refs attestation are fresh.
    """CREATE TABLE IF NOT EXISTS reward_node_observations (
        node_bi TEXT PRIMARY KEY,
        first_verified_at INTEGER NOT NULL,
        last_verified_at INTEGER NOT NULL,
        consecutive_checks INTEGER NOT NULL DEFAULT 1,
        contribution_units INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_reward_node_observations_last "
    "ON reward_node_observations(last_verified_at)",
    # One auditable randomized schedule/selection record per configured time
    # bucket. All stored entropy and snapshots are public; no wallet secret is
    # involved in scheduling or selection.
    """CREATE TABLE IF NOT EXISTS reward_rounds (
        round_id TEXT PRIMARY KEY,
        status TEXT NOT NULL DEFAULT 'scheduled',
        interval_start INTEGER NOT NULL,
        scheduled_at INTEGER NOT NULL,
        execute_after INTEGER NOT NULL,
        schedule_entropy TEXT NOT NULL,
        snapshot_data TEXT NOT NULL,
        selection_data TEXT NOT NULL DEFAULT '',
        intent_id TEXT NOT NULL DEFAULT '',
        completed_at INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_reward_rounds_status "
    "ON reward_rounds(status, execute_after)",
    # Voluntary contributors transfer directly from their own wallet to the
    # public pool. These rows contain only public references and finalized
    # signatures, and choose either immediate all-node distribution or the
    # randomized trickle pool.
    """CREATE TABLE IF NOT EXISTS reward_contributions (
        contribution_id TEXT PRIMARY KEY,
        mode TEXT NOT NULL,
        amount_lamports INTEGER NOT NULL,
        reference_address TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'prepared',
        created_at INTEGER NOT NULL,
        expires_at INTEGER NOT NULL,
        tx_signature TEXT NOT NULL DEFAULT '',
        source_address TEXT NOT NULL DEFAULT '',
        confirmed_at INTEGER NOT NULL DEFAULT 0,
        distribution_intent_id TEXT NOT NULL DEFAULT '')""",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_reward_contribution_signature "
    "ON reward_contributions(tx_signature) WHERE tx_signature<>''",
    "CREATE INDEX IF NOT EXISTS idx_reward_contributions_status "
    "ON reward_contributions(status, created_at)",
    """CREATE TABLE IF NOT EXISTS reward_contribution_intents (
        contribution_id TEXT NOT NULL,
        intent_id TEXT NOT NULL,
        chunk_index INTEGER NOT NULL,
        chunk_count INTEGER NOT NULL,
        status TEXT NOT NULL DEFAULT 'pending_signature',
        PRIMARY KEY (contribution_id, intent_id))""",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_reward_contribution_intent_id "
    "ON reward_contribution_intents(intent_id)",
    # World store element purchases. Public fields include the Solana Pay
    # reference, finalized transfer/sweep receipts, and the mirror-node share.
    # An optional deposit checkout stores one encrypted per-purchase key until
    # its sweep; public projections never select it and the sweep clears it.
    """CREATE TABLE IF NOT EXISTS world_element_purchases (
        purchase_id TEXT PRIMARY KEY,
        account_bi TEXT NOT NULL,
        element_id TEXT NOT NULL,
        amount_lamports INTEGER NOT NULL,
        treasury_lamports INTEGER NOT NULL DEFAULT 0,
        mirror_lamports INTEGER NOT NULL DEFAULT 0,
        reference_address TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'prepared',
        created_at INTEGER NOT NULL,
        expires_at INTEGER NOT NULL,
        tx_signature TEXT NOT NULL DEFAULT '',
        source_address TEXT NOT NULL DEFAULT '',
        confirmed_at INTEGER NOT NULL DEFAULT 0,
        distribution_intent_id TEXT NOT NULL DEFAULT '',
        method TEXT NOT NULL DEFAULT 'direct',
        deposit_address TEXT NOT NULL DEFAULT '',
        deposit_secret TEXT NOT NULL DEFAULT '',
        sweep_lamports INTEGER NOT NULL DEFAULT 0
            CHECK (sweep_lamports >= 0),
        sweep_signature TEXT NOT NULL DEFAULT '',
        sweep_at INTEGER NOT NULL DEFAULT 0,
        sweep_error TEXT NOT NULL DEFAULT '')""",
    "CREATE INDEX IF NOT EXISTS idx_world_element_purchases_deposit "
    "ON world_element_purchases(status, method, expires_at)",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_world_element_purchase_signature "
    "ON world_element_purchases(tx_signature) WHERE tx_signature<>''",
    "CREATE INDEX IF NOT EXISTS idx_world_element_purchase_sweep_signature "
    "ON world_element_purchases(sweep_signature) WHERE sweep_signature<>''",
    "CREATE INDEX IF NOT EXISTS idx_world_element_purchases_account "
    "ON world_element_purchases(account_bi, created_at)",
    "CREATE INDEX IF NOT EXISTS idx_world_element_purchases_status "
    "ON world_element_purchases(status, created_at)",
    # External provider entries are intentionally separate from `repositories`:
    # an imported metadata record or stub can never be routed as a live mirror.
    # The encrypted data blob carries public/private provider metadata; the
    # plaintext privacy bit lets anonymous queries exclude private rows before
    # any decryption or response construction.
    """CREATE TABLE IF NOT EXISTS repository_imports (
        id TEXT PRIMARY KEY,
        provider TEXT NOT NULL,
        external_id TEXT NOT NULL,
        owner_bi TEXT NOT NULL,
        is_private INTEGER NOT NULL DEFAULT 0,
        status TEXT NOT NULL DEFAULT 'external_repository',
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL)""",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_repository_imports_provider_id "
    "ON repository_imports(provider, external_id)",
    "CREATE INDEX IF NOT EXISTS idx_repository_imports_public "
    "ON repository_imports(is_private, updated_at)",
    "CREATE INDEX IF NOT EXISTS idx_repository_imports_owner "
    "ON repository_imports(owner_bi, updated_at)",
    # Full-fidelity provider transfer is a durable job distinct from the
    # external listing above. Checkpoints, provider ids, and reports are kept
    # inside encrypted data; the coarse columns permit owner/status selection
    # and operational aggregation without decrypting unrelated private jobs.
    """CREATE TABLE IF NOT EXISTS repository_migration_jobs (
        id TEXT PRIMARY KEY,
        repo_id TEXT NOT NULL,
        owner_bi TEXT NOT NULL,
        provider TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'pending',
        phase TEXT NOT NULL DEFAULT 'git_refs',
        generation INTEGER NOT NULL DEFAULT 1,
        setup_ms INTEGER NOT NULL DEFAULT 0,
        transfer_started_at INTEGER NOT NULL DEFAULT 0,
        completed_at INTEGER NOT NULL DEFAULT 0,
        retry_count INTEGER NOT NULL DEFAULT 0,
        object_count INTEGER NOT NULL DEFAULT 0,
        state_hash TEXT NOT NULL DEFAULT '',
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        UNIQUE (repo_id))""",
    "CREATE INDEX IF NOT EXISTS idx_repository_migration_jobs_owner "
    "ON repository_migration_jobs(owner_bi, updated_at DESC)",
    "CREATE INDEX IF NOT EXISTS idx_repository_migration_jobs_status "
    "ON repository_migration_jobs(status, updated_at)",
    """CREATE TABLE IF NOT EXISTS repository_migration_objects (
        job_id TEXT NOT NULL,
        object_type TEXT NOT NULL,
        source_key TEXT NOT NULL,
        payload_hash TEXT NOT NULL,
        generation INTEGER NOT NULL,
        data TEXT NOT NULL,
        imported_at INTEGER NOT NULL,
        PRIMARY KEY (job_id, object_type, source_key))""",
    "CREATE INDEX IF NOT EXISTS idx_repository_migration_objects_generation "
    "ON repository_migration_objects(job_id, generation, object_type)",
    """CREATE TABLE IF NOT EXISTS repository_migration_runs (
        job_id TEXT NOT NULL,
        generation INTEGER NOT NULL,
        provider TEXT NOT NULL,
        outcome TEXT NOT NULL DEFAULT 'running',
        setup_ms INTEGER NOT NULL DEFAULT 0,
        transfer_ms INTEGER NOT NULL DEFAULT 0,
        retry_count INTEGER NOT NULL DEFAULT 0,
        object_count INTEGER NOT NULL DEFAULT 0,
        data TEXT NOT NULL,
        started_at INTEGER NOT NULL,
        finished_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (job_id, generation))""",
    "CREATE INDEX IF NOT EXISTS idx_repository_migration_runs_metrics "
    "ON repository_migration_runs(finished_at DESC, provider, outcome)",
    """CREATE TRIGGER IF NOT EXISTS trg_repository_import_delete_migration
    AFTER DELETE ON repository_imports
    BEGIN
        DELETE FROM repository_migration_objects
        WHERE job_id IN (
            SELECT id FROM repository_migration_jobs WHERE repo_id=OLD.id
        );
        DELETE FROM repository_migration_runs
        WHERE job_id IN (
            SELECT id FROM repository_migration_jobs WHERE repo_id=OLD.id
        );
        DELETE FROM repository_migration_jobs WHERE repo_id=OLD.id;
    END""",
    # Eligible operators may volunteer to mirror an external entry. Public
    # responses expose only a count; the owned node label is visible to the
    # operator and listing moderator, not used as surveillance data.
    """CREATE TABLE IF NOT EXISTS repository_mirror_volunteers (
        repo_id TEXT NOT NULL,
        operator_bi TEXT NOT NULL,
        node_label TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'requested',
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        PRIMARY KEY (repo_id, operator_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_repository_mirror_volunteers_status "
    "ON repository_mirror_volunteers(repo_id, status, updated_at)",
    # Contributor invitations store the address and policy basis only inside
    # the encrypted blob. email_bi is a keyed blind index used for cooldowns
    # and opt-outs without making the address readable to a DB operator.
    """CREATE TABLE IF NOT EXISTS contributor_invitations (
        id TEXT PRIMARY KEY,
        repo_id TEXT NOT NULL,
        inviter_bi TEXT NOT NULL,
        email_bi TEXT NOT NULL,
        contributor_bi TEXT NOT NULL,
        provenance_id TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'pending',
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        sent_at INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_contributor_invitations_repo "
    "ON contributor_invitations(repo_id, created_at)",
    "CREATE INDEX IF NOT EXISTS idx_contributor_invitations_recipient "
    "ON contributor_invitations(email_bi, created_at)",
    # Eligibility is proved before an invitation is reserved. Provider-public
    # evidence, recipient consent, and owner-supplied provenance retain only
    # blind indexes in plaintext columns; human-readable audit context remains
    # inside the encrypted data blob.
    """CREATE TABLE IF NOT EXISTS contributor_invitation_provenance (
        id TEXT PRIMARY KEY,
        repo_id TEXT NOT NULL,
        contributor_bi TEXT NOT NULL,
        email_bi TEXT NOT NULL,
        basis TEXT NOT NULL,
        created_by_bi TEXT NOT NULL,
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        revoked_at INTEGER NOT NULL DEFAULT 0,
        CHECK (basis IN (
            'public_for_invitations','prior_consent','owner_supplied')))""",
    "CREATE INDEX IF NOT EXISTS idx_contributor_invitation_provenance_match "
    "ON contributor_invitation_provenance("
    "repo_id, contributor_bi, email_bi, revoked_at)",
    """CREATE TABLE IF NOT EXISTS contributor_invitation_rate (
        inviter_bi TEXT NOT NULL,
        day_bucket INTEGER NOT NULL,
        repo_id TEXT NOT NULL,
        sent_count INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (inviter_bi, day_bucket, repo_id))""",
    """CREATE TABLE IF NOT EXISTS contributor_invitation_optouts (
        email_bi TEXT PRIMARY KEY,
        opted_out_at INTEGER NOT NULL,
        source_invitation_id TEXT NOT NULL DEFAULT '')""",
    """CREATE TABLE IF NOT EXISTS contributor_invitation_abuse (
        id TEXT PRIMARY KEY,
        invitation_id TEXT NOT NULL,
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        status TEXT NOT NULL DEFAULT 'open')""",
    "CREATE INDEX IF NOT EXISTS idx_contributor_invitation_abuse_invitation "
    "ON contributor_invitation_abuse(invitation_id, created_at)",
    # Uploaded logos are PNG/JPEG/WebP only (SVG uploads are rejected). Public
    # suggestions remain pending until a provider-verified owner or moderator
    # approves one. The partial unique index guarantees one official mark.
    """CREATE TABLE IF NOT EXISTS repository_logo_suggestions (
        id TEXT PRIMARY KEY,
        repo_id TEXT NOT NULL,
        proposer_bi TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'pending',
        official INTEGER NOT NULL DEFAULT 0,
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        reviewed_at INTEGER NOT NULL DEFAULT 0,
        reviewed_by_bi TEXT NOT NULL DEFAULT '')""",
    "CREATE INDEX IF NOT EXISTS idx_repository_logo_suggestions_repo "
    "ON repository_logo_suggestions(repo_id, status, created_at)",
    "CREATE INDEX IF NOT EXISTS idx_repository_logo_suggestions_proposer "
    "ON repository_logo_suggestions(repo_id, proposer_bi, status, created_at)",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_repository_logo_official "
    "ON repository_logo_suggestions(repo_id) WHERE official=1",
    # SQLite triggers make public-logo and invitation reservations atomic. The
    # matching 0056 migration upgrades databases that already ran migration
    # 0044; these IF NOT EXISTS statements also protect ensure_schema installs.
    """CREATE TRIGGER IF NOT EXISTS trg_logo_pending_proposer_limit
        BEFORE INSERT ON repository_logo_suggestions
        WHEN NEW.status='pending' AND NEW.official=0
         AND (SELECT COUNT(*) FROM repository_logo_suggestions
              WHERE repo_id=NEW.repo_id AND proposer_bi=NEW.proposer_bi
                AND status='pending') >= 5
        BEGIN
            SELECT RAISE(ABORT, 'logo_pending_proposer_limit');
        END""",
    """CREATE TRIGGER IF NOT EXISTS trg_logo_official_insert
        BEFORE INSERT ON repository_logo_suggestions
        WHEN NEW.official=1
        BEGIN
            DELETE FROM repository_logo_suggestions
            WHERE id IN (
                SELECT id FROM repository_logo_suggestions
                WHERE repo_id=NEW.repo_id AND official=0
                ORDER BY CASE WHEN status IN ('rejected','superseded')
                              THEN 0 ELSE 1 END,
                         created_at ASC
                LIMIT CASE
                    WHEN (
                        SELECT COUNT(*) FROM repository_logo_suggestions
                        WHERE repo_id=NEW.repo_id) >= 50
                    THEN (
                        SELECT COUNT(*) FROM repository_logo_suggestions
                        WHERE repo_id=NEW.repo_id) - 49
                    ELSE 0
                END);
            UPDATE repository_logo_suggestions
            SET official=0,
                status=CASE WHEN status='approved'
                            THEN 'superseded' ELSE status END
            WHERE repo_id=NEW.repo_id AND official=1;
        END""",
    """CREATE TRIGGER IF NOT EXISTS trg_logo_official_approval
        BEFORE UPDATE OF official ON repository_logo_suggestions
        WHEN NEW.official=1 AND OLD.official<>1
        BEGIN
            UPDATE repository_logo_suggestions
            SET official=0,
                status=CASE WHEN status='approved'
                            THEN 'superseded' ELSE status END
            WHERE repo_id=NEW.repo_id AND id<>OLD.id AND official=1;
        END""",
    """CREATE TRIGGER IF NOT EXISTS trg_logo_community_repository_limit
        BEFORE INSERT ON repository_logo_suggestions
        WHEN NEW.status='pending' AND NEW.official=0
         AND (SELECT COUNT(*) FROM repository_logo_suggestions
              WHERE repo_id=NEW.repo_id) >= 49
        BEGIN
            SELECT RAISE(ABORT, 'logo_community_repository_limit');
        END""",
    """CREATE TRIGGER IF NOT EXISTS trg_invitation_optout_reservation
        BEFORE INSERT ON contributor_invitations
        WHEN NEW.status='pending'
         AND EXISTS (SELECT 1 FROM contributor_invitation_optouts
                     WHERE email_bi=NEW.email_bi)
        BEGIN
            SELECT RAISE(ABORT, 'invitation_recipient_opted_out');
        END""",
    """CREATE TRIGGER IF NOT EXISTS trg_invitation_provenance_reservation
        BEFORE INSERT ON contributor_invitations
        WHEN NEW.status='pending'
         AND NOT EXISTS (
             SELECT 1 FROM contributor_invitation_provenance
             WHERE id=NEW.provenance_id
               AND repo_id=NEW.repo_id
               AND contributor_bi=NEW.contributor_bi
               AND email_bi=NEW.email_bi
               AND revoked_at=0
         )
        BEGIN
            SELECT RAISE(ABORT, 'invitation_provenance_invalid');
        END""",
    """CREATE TRIGGER IF NOT EXISTS trg_invitation_recipient_cooldown
        BEFORE INSERT ON contributor_invitations
        WHEN NEW.status='pending'
         AND EXISTS (
             SELECT 1 FROM contributor_invitations
             WHERE email_bi=NEW.email_bi
               AND status IN ('pending','sent')
               AND created_at > NEW.created_at - 2592000000)
        BEGIN
            SELECT RAISE(ABORT, 'invitation_recipient_cooldown');
        END""",
    """CREATE TRIGGER IF NOT EXISTS trg_invitation_daily_limit
        BEFORE INSERT ON contributor_invitations
        WHEN NEW.status='pending'
         AND COALESCE((
             SELECT sent_count FROM contributor_invitation_rate
             WHERE inviter_bi=NEW.inviter_bi
               AND day_bucket=CAST(NEW.created_at / 86400000 AS INTEGER)
               AND repo_id='*'), 0) >= 20
        BEGIN
            SELECT RAISE(ABORT, 'invitation_daily_limit');
        END""",
    """CREATE TRIGGER IF NOT EXISTS trg_invitation_repository_daily_limit
        BEFORE INSERT ON contributor_invitations
        WHEN NEW.status='pending'
         AND COALESCE((
             SELECT sent_count FROM contributor_invitation_rate
             WHERE inviter_bi=NEW.inviter_bi
               AND day_bucket=CAST(NEW.created_at / 86400000 AS INTEGER)
               AND repo_id=NEW.repo_id), 0) >= 10
        BEGIN
            SELECT RAISE(ABORT, 'invitation_repository_daily_limit');
        END""",
    """CREATE TRIGGER IF NOT EXISTS trg_invitation_repository_history_limit
        BEFORE INSERT ON contributor_invitations
        WHEN NEW.status='pending'
         AND (SELECT COUNT(*) FROM contributor_invitations
              WHERE repo_id=NEW.repo_id) >= 500
        BEGIN
            SELECT RAISE(ABORT, 'invitation_repository_history_limit');
        END""",
    """CREATE TRIGGER IF NOT EXISTS trg_invitation_reserve_rate
        AFTER INSERT ON contributor_invitations
        WHEN NEW.status='pending'
        BEGIN
            INSERT INTO contributor_invitation_rate
                (inviter_bi, day_bucket, repo_id, sent_count)
            VALUES
                (NEW.inviter_bi,
                 CAST(NEW.created_at / 86400000 AS INTEGER), '*', 1)
            ON CONFLICT(inviter_bi,day_bucket,repo_id)
            DO UPDATE SET sent_count=sent_count+1;
            INSERT INTO contributor_invitation_rate
                (inviter_bi, day_bucket, repo_id, sent_count)
            VALUES
                (NEW.inviter_bi,
                 CAST(NEW.created_at / 86400000 AS INTEGER), NEW.repo_id, 1)
            ON CONFLICT(inviter_bi,day_bucket,repo_id)
            DO UPDATE SET sent_count=sent_count+1;
        END""",
    """CREATE TRIGGER IF NOT EXISTS trg_invitation_release_rate
        AFTER DELETE ON contributor_invitations
        WHEN OLD.status='pending'
        BEGIN
            UPDATE contributor_invitation_rate
            SET sent_count=MAX(0, sent_count-1)
            WHERE inviter_bi=OLD.inviter_bi
              AND day_bucket=CAST(OLD.created_at / 86400000 AS INTEGER)
              AND repo_id IN ('*', OLD.repo_id);
            DELETE FROM contributor_invitation_rate
            WHERE inviter_bi=OLD.inviter_bi
              AND day_bucket=CAST(OLD.created_at / 86400000 AS INTEGER)
              AND repo_id IN ('*', OLD.repo_id)
              AND sent_count<=0;
        END""",
    # Content-free receipt written by the explicit offline legacy-custody scrub.
    # No address, key, artifact, account/repo name, or RPC credential is stored.
    """CREATE TABLE IF NOT EXISTS legacy_custody_migration_audit (
        migration_id TEXT PRIMARY KEY,
        artifact_sha256 TEXT NOT NULL,
        audit_sha256 TEXT NOT NULL,
        record_count INTEGER NOT NULL,
        address_count INTEGER NOT NULL,
        confirmed_at INTEGER NOT NULL,
        prepared_at INTEGER NOT NULL,
        applied_at INTEGER NOT NULL,
        status TEXT NOT NULL CHECK (status = 'scrubbed'),
        tool_version TEXT NOT NULL)""",
    # Public, key-free reconciliation evidence emitted by the offline scrub.
    # record_id is an opaque hash of the historical row locator; no account or
    # repository name, secret, RPC credential, or transaction bytes are stored.
    """CREATE TABLE IF NOT EXISTS legacy_custody_reconciliation (
        record_id TEXT PRIMARY KEY,
        source_table TEXT NOT NULL,
        public_address TEXT NOT NULL,
        network TEXT NOT NULL,
        balance_lamports INTEGER NOT NULL CHECK (balance_lamports = 0),
        rpc_slot INTEGER NOT NULL,
        reconciled_at INTEGER NOT NULL,
        artifact_sha256 TEXT NOT NULL,
        audit_sha256 TEXT NOT NULL)""",
    # Public UTC announcements are D1-backed rather than hardcoded into the
    # production client. Mutations are platform-admin gated and audited;
    # created_by_bi never appears in public reads.
    """CREATE TABLE IF NOT EXISTS world_events (
        event_id TEXT PRIMARY KEY,
        status TEXT NOT NULL DEFAULT 'scheduled'
            CHECK (status IN ('scheduled', 'cancelled')),
        starts_at INTEGER NOT NULL,
        ends_at INTEGER NOT NULL,
        data TEXT NOT NULL,
        created_by_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        cancelled_at INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_world_events_public "
    "ON world_events(status, ends_at, starts_at)",
    "CREATE INDEX IF NOT EXISTS idx_world_events_retention "
    "ON world_events(status, cancelled_at, ends_at)",
    """CREATE TRIGGER IF NOT EXISTS trg_world_events_record_limit
        BEFORE INSERT ON world_events
        WHEN (SELECT COUNT(*) FROM world_events) >= 500
        BEGIN
            SELECT RAISE(ABORT, 'world_event_catalog_full');
        END""",
    # Private organization marketing work for the in-world Office (migration
    # 0075). Human-readable task copy, assignee labels, and check-in notes are
    # encrypted; plaintext is limited to blind indexes, bounded state, and
    # server-authoritative timer metadata.
    """CREATE TABLE IF NOT EXISTS world_office_marketing_tasks (
        task_id TEXT PRIMARY KEY,
        org_bi TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'idle'
            CHECK (status IN ('idle', 'active')),
        assignee_bi TEXT NOT NULL,
        active_assignee_bi TEXT NOT NULL DEFAULT '',
        data TEXT NOT NULL,
        created_by_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        elapsed_ms INTEGER NOT NULL DEFAULT 0 CHECK (elapsed_ms >= 0),
        started_at INTEGER NOT NULL DEFAULT 0 CHECK (started_at >= 0),
        next_checkin_at INTEGER NOT NULL DEFAULT 0
            CHECK (next_checkin_at >= 0),
        completed_at INTEGER NOT NULL DEFAULT 0
            CHECK (completed_at >= 0))""",
    "CREATE INDEX IF NOT EXISTS idx_world_office_marketing_tasks_org "
    "ON world_office_marketing_tasks(org_bi, updated_at DESC)",
    "CREATE INDEX IF NOT EXISTS idx_world_office_marketing_tasks_assignee "
    "ON world_office_marketing_tasks(org_bi, assignee_bi, updated_at DESC)",
    """CREATE UNIQUE INDEX IF NOT EXISTS
        idx_world_office_one_active_per_assignee
        ON world_office_marketing_tasks(org_bi, active_assignee_bi)
        WHERE status = 'active' AND active_assignee_bi <> ''""",
    """CREATE TRIGGER IF NOT EXISTS trg_world_office_marketing_task_limit
        BEFORE INSERT ON world_office_marketing_tasks
        WHEN (
            SELECT COUNT(*) FROM world_office_marketing_tasks
            WHERE org_bi = NEW.org_bi
        ) >= 250
        BEGIN
            SELECT RAISE(
                ABORT, 'world_office_marketing_task_catalog_full');
        END""",
    """CREATE TABLE IF NOT EXISTS world_office_marketing_checkins (
        checkin_id TEXT PRIMARY KEY,
        task_id TEXT NOT NULL,
        org_bi TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        state TEXT NOT NULL CHECK (state IN (
            'going_well', 'blocked', 'needs_help')),
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_world_office_marketing_checkins_task "
    "ON world_office_marketing_checkins(task_id, created_at DESC)",
    """CREATE TRIGGER IF NOT EXISTS trg_world_office_marketing_checkin_limit
        BEFORE INSERT ON world_office_marketing_checkins
        WHEN (SELECT COUNT(*) FROM world_office_marketing_checkins) >= 12500
        BEGIN
            SELECT RAISE(
                ABORT, 'world_office_marketing_checkin_catalog_full');
        END""",
    # Organization-private universal tasks (migration 0105). Department/team
    # and bounded routing state remain queryable inside D1; all human-readable
    # task copy, labels, repository references, and reviewer names stay in
    # encrypted payloads.
    """CREATE TABLE IF NOT EXISTS organization_tasks (
        task_id TEXT PRIMARY KEY,
        org_bi TEXT NOT NULL,
        department TEXT NOT NULL DEFAULT 'general'
            CHECK (length(department) BETWEEN 1 AND 64),
        team TEXT NOT NULL DEFAULT '' CHECK (length(team) <= 64),
        destination TEXT NOT NULL DEFAULT 'department'
            CHECK (destination IN (
                'department','personal','repository','qa','agent')),
        assignee_kind TEXT NOT NULL DEFAULT 'user'
            CHECK (assignee_kind IN (
                'user','unassigned','agent')),
        status TEXT NOT NULL DEFAULT 'idle'
            CHECK (status IN ('idle','active')),
        assignee_bi TEXT NOT NULL DEFAULT '',
        active_assignee_bi TEXT NOT NULL DEFAULT '',
        data TEXT NOT NULL,
        created_by_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        elapsed_ms INTEGER NOT NULL DEFAULT 0 CHECK (elapsed_ms >= 0),
        started_at INTEGER NOT NULL DEFAULT 0 CHECK (started_at >= 0),
        next_checkin_at INTEGER NOT NULL DEFAULT 0
            CHECK (next_checkin_at >= 0),
        completed_at INTEGER NOT NULL DEFAULT 0
            CHECK (completed_at >= 0),
        qa_status TEXT NOT NULL DEFAULT 'unknown'
            CHECK (qa_status IN ('unknown','passed','failed')),
        qa_reviewer_bi TEXT NOT NULL DEFAULT '',
        qa_reviewed_at INTEGER NOT NULL DEFAULT 0
            CHECK (qa_reviewed_at >= 0),
        qa_requested_at INTEGER NOT NULL DEFAULT 0
            CHECK (qa_requested_at >= 0),
        priority INTEGER NOT NULL DEFAULT 50
            CHECK (priority BETWEEN 1 AND 99),
        agent_session_id TEXT NOT NULL DEFAULT ''
            CHECK (length(agent_session_id) <= 64),
        progress INTEGER NOT NULL DEFAULT 0
            CHECK (progress BETWEEN 0 AND 100),
        deleted_at INTEGER NOT NULL DEFAULT 0
            CHECK (deleted_at >= 0),
        deleted_by_bi TEXT NOT NULL DEFAULT '',
        reminder_every_ms INTEGER NOT NULL DEFAULT 0
            CHECK (reminder_every_ms >= 0),
        reminder_next_at INTEGER NOT NULL DEFAULT 0
            CHECK (reminder_next_at >= 0))""",
    "CREATE INDEX IF NOT EXISTS idx_organization_tasks_org_updated "
    "ON organization_tasks(org_bi, updated_at DESC)",
    "CREATE INDEX IF NOT EXISTS idx_organization_tasks_trash "
    "ON organization_tasks(org_bi, deleted_at) WHERE deleted_at>0",
    "CREATE INDEX IF NOT EXISTS idx_organization_tasks_reminder_due "
    "ON organization_tasks(reminder_next_at) WHERE reminder_next_at>0",
    "CREATE INDEX IF NOT EXISTS idx_organization_tasks_scope "
    "ON organization_tasks(org_bi, department, team, updated_at DESC)",
    "CREATE INDEX IF NOT EXISTS idx_organization_tasks_assignee "
    "ON organization_tasks(org_bi, assignee_bi, updated_at DESC)",
    "CREATE INDEX IF NOT EXISTS idx_organization_tasks_qa "
    "ON organization_tasks(org_bi, qa_requested_at, qa_reviewed_at DESC)",
    "CREATE INDEX IF NOT EXISTS idx_organization_tasks_global_priority "
    "ON organization_tasks(org_bi, priority, completed_at, updated_at DESC)",
    # The task list orders by (completed_at>0, priority, updated_at DESC,
    # task_id DESC) LIMIT/OFFSET; without an index in that exact order every
    # page sorted the whole organization's tasks (~170 rows read per 25
    # returned, ~300 pages an hour from the desktop). SQLite walks this
    # expression index in order and stops at the page boundary.
    "CREATE INDEX IF NOT EXISTS idx_organization_tasks_list_order "
    "ON organization_tasks(org_bi, (completed_at>0), priority, "
    "updated_at DESC, task_id DESC)",
    "UPDATE organization_tasks SET priority=99 WHERE priority>99",
    """CREATE UNIQUE INDEX IF NOT EXISTS
        idx_organization_tasks_one_active_assignee
        ON organization_tasks(org_bi, active_assignee_bi)
        WHERE status='active' AND active_assignee_bi<>''""",
    """CREATE TRIGGER IF NOT EXISTS trg_organization_task_limit
        BEFORE INSERT ON organization_tasks
        WHEN (
            SELECT COUNT(*) FROM organization_tasks
            WHERE org_bi=NEW.org_bi
        ) >= 2000
        BEGIN
            SELECT RAISE(ABORT, 'organization_task_catalog_full');
        END""",
    """CREATE TABLE IF NOT EXISTS organization_task_attachments (
        attachment_id TEXT PRIMARY KEY,
        task_id TEXT NOT NULL,
        org_bi TEXT NOT NULL,
        data TEXT NOT NULL,
        created_by_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_organization_task_attachments_task "
    "ON organization_task_attachments(org_bi, task_id, created_at)",
    """CREATE TRIGGER IF NOT EXISTS trg_organization_task_attachment_limit
        BEFORE INSERT ON organization_task_attachments
        WHEN (
            SELECT COUNT(*) FROM organization_task_attachments
            WHERE org_bi=NEW.org_bi AND task_id=NEW.task_id
        ) >= 4
        BEGIN
            SELECT RAISE(
                ABORT, 'organization_task_attachment_catalog_full');
        END""",
    """CREATE TABLE IF NOT EXISTS mailtrap_email_sends (
        send_id TEXT PRIMARY KEY,
        account_bi TEXT NOT NULL,
        kind TEXT NOT NULL DEFAULT '',
        sent_at INTEGER NOT NULL,
        accepted INTEGER NOT NULL DEFAULT 0
            CHECK (accepted IN (0, 1)),
        status TEXT NOT NULL DEFAULT '',
        status_at INTEGER NOT NULL DEFAULT 0,
        message_id TEXT NOT NULL DEFAULT '',
        opened_at INTEGER NOT NULL DEFAULT 0,
        clicked_at INTEGER NOT NULL DEFAULT 0,
        open_count INTEGER NOT NULL DEFAULT 0 CHECK (open_count >= 0),
        click_count INTEGER NOT NULL DEFAULT 0 CHECK (click_count >= 0))""",
    "CREATE INDEX IF NOT EXISTS idx_mailtrap_email_sends_account "
    "ON mailtrap_email_sends(account_bi, sent_at DESC)",
    "CREATE INDEX IF NOT EXISTS idx_mailtrap_email_sends_sent_at "
    "ON mailtrap_email_sends(sent_at DESC)",
    """CREATE TABLE IF NOT EXISTS mailtrap_webhook_events (
        event_id TEXT PRIMARY KEY,
        received_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_mailtrap_webhook_events_received "
    "ON mailtrap_webhook_events(received_at)",
    """CREATE TABLE IF NOT EXISTS organization_task_checkins (
        checkin_id TEXT PRIMARY KEY,
        task_id TEXT NOT NULL,
        org_bi TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        state TEXT NOT NULL CHECK (state IN (
            'going_well','blocked','needs_help')),
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_organization_task_checkins_task "
    "ON organization_task_checkins(org_bi, task_id, created_at DESC)",
    """CREATE TABLE IF NOT EXISTS organization_task_qa_reviews (
        task_id TEXT NOT NULL,
        org_bi TEXT NOT NULL,
        reviewer_bi TEXT NOT NULL,
        verdict TEXT NOT NULL CHECK (verdict IN (
            'passed','failed','unknown')),
        data TEXT NOT NULL,
        reviewed_at INTEGER NOT NULL CHECK (reviewed_at >= 0),
        PRIMARY KEY (task_id, reviewer_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_organization_task_qa_reviews_task "
    "ON organization_task_qa_reviews(org_bi, task_id, reviewed_at DESC)",
    # Replies are organization-private sealed records. The task row keeps
    # only bounded routing/state; this separate window preserves discussion
    # without making D1 task rows or plaintext indexes contain human copy.
    """CREATE TABLE IF NOT EXISTS organization_task_responses (
        response_id TEXT PRIMARY KEY,
        task_id TEXT NOT NULL,
        org_bi TEXT NOT NULL,
        author_bi TEXT NOT NULL,
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_organization_task_responses_task "
    "ON organization_task_responses(org_bi, task_id, created_at DESC)",
    """CREATE TRIGGER IF NOT EXISTS trg_organization_task_response_limit
        BEFORE INSERT ON organization_task_responses
        WHEN (
            SELECT COUNT(*) FROM organization_task_responses
            WHERE org_bi=NEW.org_bi AND task_id=NEW.task_id
        ) >= 50
        BEGIN
            DELETE FROM organization_task_responses
            WHERE response_id=(
                SELECT response_id FROM organization_task_responses
                WHERE org_bi=NEW.org_bi AND task_id=NEW.task_id
                ORDER BY created_at ASC,response_id ASC LIMIT 1);
        END""",
    # One append-only timeline row per recorded task event. Bounded kind,
    # actor blind index and timestamp in the clear; actor name, summary and
    # mentions sealed in `data`.
    """CREATE TABLE IF NOT EXISTS organization_task_events (
        event_id TEXT PRIMARY KEY,
        task_id TEXT NOT NULL,
        org_bi TEXT NOT NULL,
        actor_bi TEXT NOT NULL,
        kind TEXT NOT NULL CHECK (kind IN (
            'created','updated','started','stopped','checkin','replied',
            'completed','reopened','returned','qa_requested','qa_reviewed',
            'deleted','reminded','reminder_seen','emailed','email_opened',
            'email_clicked','email_viewed')),
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_organization_task_events_task "
    "ON organization_task_events(org_bi, task_id, created_at DESC)",
    """CREATE TRIGGER IF NOT EXISTS trg_organization_task_event_limit
        BEFORE INSERT ON organization_task_events
        WHEN (
            SELECT COUNT(*) FROM organization_task_events
            WHERE org_bi=NEW.org_bi AND task_id=NEW.task_id
        ) >= 250
        BEGIN
            DELETE FROM organization_task_events
            WHERE event_id=(
                SELECT event_id FROM organization_task_events
                WHERE org_bi=NEW.org_bi AND task_id=NEW.task_id
                ORDER BY created_at ASC,event_id ASC LIMIT 1);
        END""",
    # Outside collaborators can be placed on a bounded team without becoming
    # organization members. No repository or Office authorization query reads
    # this table; it is used only by explicitly collaborator-aware features
    # such as the private QA deck.
    """CREATE TABLE IF NOT EXISTS org_team_collaborators (
        org_bi TEXT NOT NULL,
        team TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        name TEXT NOT NULL CHECK (length(name) BETWEEN 1 AND 64),
        added_by_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        PRIMARY KEY (org_bi,team,account_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_org_team_collaborators_account "
    "ON org_team_collaborators(account_bi, org_bi, team)",
    # Lazy-schema installs also promote legacy Marketing rows. Keeping this
    # copy idempotent covers local databases that do not run migration files.
    """INSERT OR IGNORE INTO organization_tasks (
        task_id,org_bi,department,team,destination,assignee_kind,status,
        assignee_bi,active_assignee_bi,data,created_by_bi,created_at,updated_at,
        elapsed_ms,started_at,next_checkin_at,completed_at)
        SELECT task_id,org_bi,'marketing','marketing','department','user',
        status,assignee_bi,active_assignee_bi,data,created_by_bi,created_at,
        updated_at,elapsed_ms,started_at,next_checkin_at,completed_at
        FROM world_office_marketing_tasks""",
    """INSERT OR IGNORE INTO organization_task_checkins (
        checkin_id,task_id,org_bi,account_bi,state,data,created_at)
        SELECT checkin_id,task_id,org_bi,account_bi,state,data,created_at
        FROM world_office_marketing_checkins""",
    # Shared Office lobby attendance board (migration 0085). Each visit is one
    # row that is opened by an authenticated IN punch and closed in place by
    # the matching OUT punch. The public account name is the only display
    # value; no address, user-agent, session token, or device data is retained.
    """CREATE TABLE IF NOT EXISTS world_office_attendance (
        visit_id TEXT PRIMARY KEY CHECK (
            length(visit_id) = 32
            AND visit_id NOT GLOB '*[^0-9a-f]*'
        ),
        account_bi TEXT NOT NULL CHECK (
            length(account_bi) = 64
            AND account_bi NOT GLOB '*[^0-9a-f]*'
        ),
        account_name TEXT NOT NULL CHECK (
            length(account_name) BETWEEN 1 AND 32
        ),
        in_at INTEGER NOT NULL CHECK (in_at > 0),
        out_at INTEGER CHECK (out_at IS NULL OR out_at >= in_at),
        last_seen_at INTEGER NOT NULL DEFAULT 0 CHECK (last_seen_at >= 0),
        floor_id TEXT NOT NULL DEFAULT '' CHECK (length(floor_id) <= 32),
        visit_scope TEXT NOT NULL DEFAULT 'office'
            CHECK (visit_scope IN ('legacy', 'office')))""",
    """CREATE UNIQUE INDEX IF NOT EXISTS idx_world_office_attendance_open
        ON world_office_attendance(account_bi)
        WHERE out_at IS NULL""",
    """CREATE INDEX IF NOT EXISTS idx_world_office_attendance_recent
        ON world_office_attendance(in_at DESC, visit_id DESC)""",
    """CREATE INDEX IF NOT EXISTS idx_world_office_attendance_live
        ON world_office_attendance(out_at, last_seen_at DESC)""",
    """CREATE INDEX IF NOT EXISTS idx_world_office_attendance_scope
        ON world_office_attendance(visit_scope, account_bi, in_at)""",
    # Marketing proof-of-work links (migration 0096). Public social URLs and
    # labels remain inside encrypted payloads and are disclosed only to a
    # currently authorized Marketing-team member.
    """CREATE TABLE IF NOT EXISTS world_office_marketing_proofs (
        proof_id TEXT PRIMARY KEY CHECK (
            length(proof_id) = 32
            AND proof_id NOT GLOB '*[^0-9a-f]*'
        ),
        org_bi TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        url_bi TEXT NOT NULL,
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL CHECK (created_at > 0))""",
    """CREATE INDEX IF NOT EXISTS idx_world_office_marketing_proofs_org
        ON world_office_marketing_proofs(
            org_bi, created_at DESC, proof_id DESC)""",
    """CREATE INDEX IF NOT EXISTS idx_world_office_marketing_proofs_member
        ON world_office_marketing_proofs(
            org_bi, account_bi, created_at DESC, proof_id DESC)""",
    """CREATE TRIGGER IF NOT EXISTS
        trg_world_office_marketing_proofs_capacity
        BEFORE INSERT ON world_office_marketing_proofs
        WHEN (SELECT COUNT(*) FROM world_office_marketing_proofs) >= 10000
        BEGIN
            SELECT RAISE(
                ABORT, 'world_office_marketing_proof_catalog_full');
        END""",
    # Evidence-reviewed contextual placements. The accounting table is
    # deliberately isolated from every wallet/reward ledger.
    """CREATE TABLE IF NOT EXISTS community_ad_instance_policy (
        instance_id TEXT PRIMARY KEY,
        enabled INTEGER NOT NULL DEFAULT 0 CHECK (enabled IN (0, 1)),
        contexts TEXT NOT NULL DEFAULT '[]',
        revenue_destination TEXT NOT NULL DEFAULT '',
        revenue_destination_type TEXT NOT NULL DEFAULT ''
            CHECK (revenue_destination_type IN (
                '', 'project-operations', 'instance-operations',
                'community-grants', 'nonprofit'
            )),
        updated_by_bi TEXT NOT NULL,
        updated_at INTEGER NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS community_ad_proposals (
        proposal_id TEXT PRIMARY KEY,
        proposer_bi TEXT NOT NULL,
        status TEXT NOT NULL CHECK (status IN (
            'voting', 'approved', 'rejected', 'expired',
            'suspended', 'appealed'
        )),
        data TEXT NOT NULL,
        opens_at INTEGER NOT NULL,
        closes_at INTEGER NOT NULL,
        review_due_at INTEGER NOT NULL,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_community_ad_proposals_status_review "
    "ON community_ad_proposals(status, review_due_at)",
    """CREATE TABLE IF NOT EXISTS community_ad_votes (
        proposal_id TEXT NOT NULL,
        voter_bi TEXT NOT NULL,
        choice TEXT NOT NULL CHECK (choice IN (
            'approve', 'reject', 'abstain'
        )),
        conflict INTEGER NOT NULL DEFAULT 0 CHECK (conflict IN (0, 1)),
        disclosure TEXT NOT NULL DEFAULT '',
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        PRIMARY KEY (proposal_id, voter_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_community_ad_votes_proposal "
    "ON community_ad_votes(proposal_id, choice)",
    """CREATE TABLE IF NOT EXISTS community_ad_moderation (
        record_id TEXT PRIMARY KEY,
        proposal_id TEXT NOT NULL,
        action TEXT NOT NULL CHECK (action IN (
            'note', 'suspend', 'reinstate', 'reject', 'approve',
            'appeal-filed', 'appeal-upheld', 'appeal-denied'
        )),
        reason TEXT NOT NULL,
        evidence_url TEXT NOT NULL DEFAULT '',
        actor_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_community_ad_moderation_proposal "
    "ON community_ad_moderation(proposal_id, created_at)",
    """CREATE TABLE IF NOT EXISTS community_ad_revenue (
        entry_id TEXT PRIMARY KEY,
        instance_id TEXT NOT NULL,
        proposal_id TEXT NOT NULL,
        ledger_class TEXT NOT NULL DEFAULT 'advertising-revenue'
            CHECK (ledger_class = 'advertising-revenue'),
        amount_minor INTEGER NOT NULL CHECK (amount_minor >= 0),
        currency TEXT NOT NULL,
        destination_snapshot TEXT NOT NULL,
        external_reference TEXT NOT NULL,
        recorded_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_community_ad_revenue_instance "
    "ON community_ad_revenue(instance_id, recorded_at)",
    # Canonical external operating costs and their reimbursement state
    # (migration 0166). One expense can project as both the cost that was
    # incurred and the equal liability owed to the member who paid it. Paying
    # the liability updates this source row; clients never invent or duplicate
    # a second mutable debt record.
    """CREATE TABLE IF NOT EXISTS operating_expenses (
        expense_id TEXT PRIMARY KEY,
        vendor TEXT NOT NULL,
        description TEXT NOT NULL,
        paid_by_name TEXT NOT NULL,
        amount_minor INTEGER NOT NULL CHECK (amount_minor > 0),
        currency TEXT NOT NULL CHECK (
            length(currency) = 3 AND currency = upper(currency)),
        original_amount_minor INTEGER NOT NULL DEFAULT 0
            CHECK (original_amount_minor >= 0),
        original_currency TEXT NOT NULL DEFAULT '' CHECK (
            original_currency = '' OR (
                length(original_currency) = 3
                AND original_currency = upper(original_currency))),
        external_reference TEXT NOT NULL UNIQUE,
        payment_method_summary TEXT NOT NULL DEFAULT '',
        incurred_at INTEGER NOT NULL CHECK (incurred_at > 0),
        recorded_at INTEGER NOT NULL CHECK (recorded_at > 0),
        reimbursement_status TEXT NOT NULL DEFAULT 'outstanding'
            CHECK (reimbursement_status IN (
                'outstanding', 'reimbursed', 'waived')),
        reimbursed_at INTEGER NOT NULL DEFAULT 0 CHECK (reimbursed_at >= 0),
        reimbursement_reference TEXT NOT NULL DEFAULT '')""",
    "CREATE INDEX IF NOT EXISTS idx_operating_expenses_incurred "
    "ON operating_expenses(incurred_at DESC, expense_id DESC)",
    "CREATE INDEX IF NOT EXISTS idx_operating_expenses_reimbursement "
    "ON operating_expenses(reimbursement_status, incurred_at DESC)",
    """CREATE TRIGGER IF NOT EXISTS trg_community_ad_proposal_limit
        BEFORE INSERT ON community_ad_proposals
        WHEN (SELECT COUNT(*) FROM community_ad_proposals) >= 2000
        BEGIN
            SELECT RAISE(ABORT, 'community ad proposal limit reached');
        END""",
    # Non-custodial organization succession (migration 0064). Every identity is
    # already a public organization/account identifier; there is no arbitrary
    # encrypted blob and no field capable of carrying credentials, account
    # contents, devices, repository ACLs, private data, or financial material.
    """CREATE TABLE IF NOT EXISTS org_succession_policies (
        org_bi TEXT PRIMARY KEY,
        owner_bi TEXT NOT NULL,
        owner_name TEXT NOT NULL,
        successor_bi TEXT NOT NULL,
        successor_name TEXT NOT NULL,
        inactivity_days INTEGER NOT NULL CHECK (
            inactivity_days BETWEEN 30 AND 730),
        grace_days INTEGER NOT NULL CHECK (grace_days BETWEEN 7 AND 90),
        approval_threshold INTEGER NOT NULL CHECK (
            approval_threshold BETWEEN 2 AND 20),
        owner_last_active_at INTEGER NOT NULL,
        enabled INTEGER NOT NULL DEFAULT 1 CHECK (enabled IN (0, 1)),
        configured_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_org_succession_warning "
    "ON org_succession_policies(enabled, owner_last_active_at)",
    """CREATE TABLE IF NOT EXISTS org_succession_cases (
        case_id TEXT PRIMARY KEY,
        org_bi TEXT NOT NULL,
        owner_bi TEXT NOT NULL,
        owner_name TEXT NOT NULL,
        successor_bi TEXT NOT NULL,
        successor_name TEXT NOT NULL,
        inactivity_days INTEGER NOT NULL CHECK (
            inactivity_days BETWEEN 30 AND 730),
        grace_days INTEGER NOT NULL CHECK (grace_days BETWEEN 7 AND 90),
        approval_threshold INTEGER NOT NULL CHECK (
            approval_threshold BETWEEN 2 AND 20),
        opened_at INTEGER NOT NULL,
        grace_ends_at INTEGER NOT NULL,
        status TEXT NOT NULL CHECK (
            status IN ('grace', 'cancelled', 'completed', 'superseded')),
        resolved_at INTEGER NOT NULL DEFAULT 0)""",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_org_succession_one_active_case "
    "ON org_succession_cases(org_bi) WHERE status='grace'",
    "CREATE INDEX IF NOT EXISTS idx_org_succession_cases_history "
    "ON org_succession_cases(org_bi, opened_at)",
    """CREATE TABLE IF NOT EXISTS org_succession_approvals (
        case_id TEXT NOT NULL,
        approver_bi TEXT NOT NULL,
        approved_at INTEGER NOT NULL,
        PRIMARY KEY (case_id, approver_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_org_succession_approvals_case "
    "ON org_succession_approvals(case_id, approved_at)",
    """CREATE TABLE IF NOT EXISTS org_succession_events (
        event_id TEXT PRIMARY KEY,
        org_bi TEXT NOT NULL,
        case_id TEXT NOT NULL DEFAULT '',
        actor_bi TEXT NOT NULL DEFAULT '',
        event_type TEXT NOT NULL CHECK (event_type IN (
            'configured', 'configuration-disabled', 'warning-issued',
            'grace-opened', 'approval-recorded', 'cancelled', 'check-in',
            'completed', 'completion-denied')),
        status TEXT NOT NULL CHECK (status IN (
            'active', 'disabled', 'warning', 'grace', 'cancelled',
            'superseded', 'completed')),
        created_at INTEGER NOT NULL,
        inactivity_days INTEGER NOT NULL DEFAULT 0,
        grace_days INTEGER NOT NULL DEFAULT 0,
        approval_threshold INTEGER NOT NULL DEFAULT 0,
        approval_count INTEGER NOT NULL DEFAULT 0,
        dedupe_key TEXT NOT NULL UNIQUE)""",
    "CREATE INDEX IF NOT EXISTS idx_org_succession_events_history "
    "ON org_succession_events(org_bi, created_at)",
    """CREATE TRIGGER IF NOT EXISTS trg_org_succession_approval_authorized
        BEFORE INSERT ON org_succession_approvals
        BEGIN
            SELECT RAISE(
                ABORT, 'succession_approver_not_authorized')
            WHERE NOT EXISTS (
                SELECT 1 FROM org_succession_cases c
                JOIN org_members m
                  ON m.org_bi=c.org_bi AND m.member_bi=NEW.approver_bi
                WHERE c.case_id=NEW.case_id
                  AND c.status='grace'
                  AND NEW.approved_at<=c.grace_ends_at
                  AND m.role IN ('owner','admin','maintainer','member')
                  AND NEW.approver_bi<>c.owner_bi
                  AND NEW.approver_bi<>c.successor_bi
            );
        END""",
    """CREATE TRIGGER IF NOT EXISTS trg_org_succession_approval_no_update
        BEFORE UPDATE ON org_succession_approvals
        BEGIN
            SELECT RAISE(ABORT, 'succession_approvals_append_only');
        END""",
    """CREATE TRIGGER IF NOT EXISTS trg_org_succession_approval_no_delete
        BEFORE DELETE ON org_succession_approvals
        BEGIN
            SELECT RAISE(ABORT, 'succession_approvals_append_only');
        END""",
    """CREATE TRIGGER IF NOT EXISTS trg_org_succession_event_no_update
        BEFORE UPDATE ON org_succession_events
        BEGIN
            SELECT RAISE(ABORT, 'succession_events_append_only');
        END""",
    """CREATE TRIGGER IF NOT EXISTS trg_org_succession_event_no_delete
        BEFORE DELETE ON org_succession_events
        BEGIN
            SELECT RAISE(ABORT, 'succession_events_append_only');
        END""",
    """CREATE TRIGGER IF NOT EXISTS trg_org_succession_case_immutable
        BEFORE UPDATE ON org_succession_cases
        WHEN NEW.case_id<>OLD.case_id
          OR NEW.org_bi<>OLD.org_bi
          OR NEW.owner_bi<>OLD.owner_bi
          OR NEW.owner_name<>OLD.owner_name
          OR NEW.successor_bi<>OLD.successor_bi
          OR NEW.successor_name<>OLD.successor_name
          OR NEW.inactivity_days<>OLD.inactivity_days
          OR NEW.grace_days<>OLD.grace_days
          OR NEW.approval_threshold<>OLD.approval_threshold
          OR NEW.opened_at<>OLD.opened_at
          OR NEW.grace_ends_at<>OLD.grace_ends_at
        BEGIN
            SELECT RAISE(ABORT, 'succession_case_snapshot_immutable');
        END""",
    """CREATE TRIGGER IF NOT EXISTS trg_org_succession_case_transition
        BEFORE UPDATE OF status ON org_succession_cases
        WHEN NEW.status<>OLD.status
         AND NOT (
            OLD.status='grace'
            AND NEW.status IN ('cancelled','completed','superseded')
         )
        BEGIN
            SELECT RAISE(ABORT, 'invalid_succession_transition');
        END""",
    """CREATE TRIGGER IF NOT EXISTS trg_org_succession_case_no_delete
        BEFORE DELETE ON org_succession_cases
        BEGIN
            SELECT RAISE(ABORT, 'succession_cases_append_only');
        END""",
    """CREATE TRIGGER IF NOT EXISTS trg_org_succession_completion_guard
        BEFORE UPDATE OF status ON org_succession_cases
        WHEN NEW.status='completed' AND OLD.status='grace'
        BEGIN
            SELECT RAISE(ABORT, 'succession_grace_active')
            WHERE NEW.resolved_at<OLD.grace_ends_at;
            SELECT RAISE(ABORT, 'succession_owner_changed')
            WHERE NOT EXISTS (
                SELECT 1 FROM org_members
                WHERE org_bi=OLD.org_bi
                  AND member_bi=OLD.owner_bi AND role='owner'
            );
            SELECT RAISE(ABORT, 'succession_successor_changed')
            WHERE NOT EXISTS (
                SELECT 1 FROM org_members
                WHERE org_bi=OLD.org_bi
                  AND member_bi=OLD.successor_bi
                  AND role IN ('admin','member')
            );
            SELECT RAISE(
                ABORT, 'succession_approval_threshold_not_met')
            WHERE (
                SELECT COUNT(*) FROM org_succession_approvals a
                JOIN org_members m
                  ON m.org_bi=OLD.org_bi
                 AND m.member_bi=a.approver_bi
                WHERE a.case_id=OLD.case_id
                  AND m.role IN ('owner','admin','maintainer','member')
                  AND a.approver_bi<>OLD.owner_bi
                  AND a.approver_bi<>OLD.successor_bi
            )<OLD.approval_threshold;
        END""",
    """CREATE TRIGGER IF NOT EXISTS trg_org_succession_role_transfer
        AFTER UPDATE OF status ON org_succession_cases
        WHEN NEW.status='completed' AND OLD.status='grace'
        BEGIN
            UPDATE org_members SET role='admin'
            WHERE org_bi=OLD.org_bi
              AND member_bi=OLD.owner_bi AND role='owner';
            UPDATE org_members SET role='owner'
            WHERE org_bi=OLD.org_bi
              AND member_bi=OLD.successor_bi
              AND role IN ('admin','member');
        END""",
    """CREATE TRIGGER IF NOT EXISTS trg_org_succession_org_delete
        BEFORE DELETE ON orgs
        WHEN EXISTS (
            SELECT 1 FROM org_succession_policies
            WHERE org_bi=OLD.org_bi AND enabled=1
        )
        BEGIN
            INSERT OR IGNORE INTO org_succession_events
                (event_id,org_bi,case_id,actor_bi,event_type,status,
                 created_at,inactivity_days,grace_days,approval_threshold,
                 approval_count,dedupe_key)
            SELECT
                'org-delete:' || p.org_bi || ':' || p.updated_at,
                p.org_bi,
                COALESCE((
                    SELECT case_id FROM org_succession_cases
                    WHERE org_bi=p.org_bi AND status='grace' LIMIT 1
                ), ''),
                '',
                'configuration-disabled',
                'disabled',
                p.updated_at,
                p.inactivity_days,
                p.grace_days,
                p.approval_threshold,
                0,
                'org-delete:' || p.org_bi || ':' || p.updated_at
            FROM org_succession_policies p
            WHERE p.org_bi=OLD.org_bi AND p.enabled=1;
            UPDATE org_succession_cases
            SET status='cancelled',resolved_at=MAX(resolved_at, opened_at)
            WHERE org_bi=OLD.org_bi AND status='grace';
            UPDATE org_succession_policies
            SET enabled=0
            WHERE org_bi=OLD.org_bi AND enabled=1;
        END""",
    # User-managed Git SSH public keys. The reversible public-key line and its
    # private display label live only in the encrypted data envelope; D1 keeps
    # keyed blind indexes for account/key lookup plus the standard public
    # fingerprint and algorithm needed by the owner's settings UI. Revocation
    # is retained instead of deleting the row so a previously revoked key can
    # never silently become trusted again.
    """CREATE TABLE IF NOT EXISTS account_ssh_keys (
        key_id TEXT PRIMARY KEY,
        account_bi TEXT NOT NULL,
        key_bi TEXT NOT NULL UNIQUE,
        key_type TEXT NOT NULL,
        fingerprint TEXT NOT NULL,
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        last_used_at INTEGER NOT NULL DEFAULT 0,
        revoked_at INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_account_ssh_keys_account "
    "ON account_ssh_keys(account_bi, revoked_at, created_at)",
    # Revocable account sessions. The browser/desktop receives a random bearer
    # value; D1 keeps only its keyed digest and opaque id. Logout, password
    # reset, account disable/delete and identity-key rotation can therefore
    # revoke immediately without storing a replayable credential. client_ip is
    # the edge-observed sign-in address, returned only to the owner of the
    # account it belongs to so a stolen device can be recognized and revoked.
    # device_bi links a session minted by a key-bound login (desktop or mobile
    # node) to its account_devices row so revoking the device revokes every
    # session it opened; '' for plain web sessions.
    """CREATE TABLE IF NOT EXISTS account_sessions (
        session_id TEXT PRIMARY KEY,
        account_bi TEXT NOT NULL,
        token_digest TEXT NOT NULL UNIQUE,
        created_at INTEGER NOT NULL,
        last_seen_at INTEGER NOT NULL,
        expires_at INTEGER NOT NULL,
        revoked_at INTEGER NOT NULL DEFAULT 0,
        device_label TEXT NOT NULL DEFAULT '',
        client_ip TEXT NOT NULL DEFAULT '',
        device_bi TEXT NOT NULL DEFAULT '')""",
    "CREATE INDEX IF NOT EXISTS idx_account_sessions_account "
    "ON account_sessions(account_bi, revoked_at, expires_at)",
    "CREATE INDEX IF NOT EXISTS idx_account_sessions_expiry "
    "ON account_sessions(expires_at, revoked_at)",
    # Explicit, administrator-created World moderation blocks. Subjects are
    # rotating keyed tokens derived transiently at the edge; raw addresses and
    # user-agent strings never enter D1. This table is intentionally separate
    # from the retired automated abuse/quarantine tables and has no signal,
    # scoring, detection, or permanent-ban state.
    """CREATE TABLE IF NOT EXISTS world_manual_blocks (
        block_id TEXT PRIMARY KEY,
        target_type TEXT NOT NULL CHECK (target_type IN ('ip','agent')),
        subject_token TEXT NOT NULL,
        created_by_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        expires_at INTEGER NOT NULL,
        revoked_at INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_world_manual_blocks_active "
    "ON world_manual_blocks(target_type, subject_token, expires_at) "
    "WHERE revoked_at=0",
    # The administrator-curated Town Square placement overrides (migrations
    # 0080/0082) were retired with the layout editor by migration 0115; make
    # sure lazily-ensured DBs lose the table too.
    "DROP TABLE IF EXISTS world_object_layout",
    # One public, last-known-good CelesTrak VISUAL OMM snapshot. A scheduled
    # refresh owns all upstream traffic; visitor reads never fetch CelesTrak.
    # This row contains no visitor location, account, session, or wallet data.
    """CREATE TABLE IF NOT EXISTS world_satellite_snapshot (
        snapshot_id INTEGER PRIMARY KEY CHECK (snapshot_id = 1),
        data TEXT NOT NULL CHECK (
            length(data) > 0 AND length(data) <= 524288
        ),
        digest TEXT NOT NULL CHECK (
            length(digest) = 64
            AND digest NOT GLOB '*[^0-9a-f]*'
        ),
        fetched_at INTEGER NOT NULL CHECK (fetched_at > 0),
        source_epoch TEXT NOT NULL,
        last_attempt_at INTEGER NOT NULL CHECK (last_attempt_at > 0),
        last_status INTEGER NOT NULL DEFAULT 200 CHECK (
            last_status >= 0 AND last_status <= 599
        ),
        last_error TEXT NOT NULL DEFAULT ''
            CHECK (length(last_error) <= 240))""",
    # Referral-program counters, one row per referring account. referrer_bi is
    # the account blind index and name is the plaintext public username (the
    # same identity already shown on the profile page and leaderboards). Only
    # aggregate counters are kept here. Per-signup eligibility and attribution
    # live separately in referral_attributions so public leaderboard reads
    # never touch anti-gaming signals.
    """CREATE TABLE IF NOT EXISTS referral_stats (
        referrer_bi TEXT PRIMARY KEY,
        name TEXT NOT NULL,
        clicks INTEGER NOT NULL DEFAULT 0,
        signups INTEGER NOT NULL DEFAULT 0,
        last_ts INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_referral_stats_rank "
    "ON referral_stats(signups DESC, clicks DESC)",
    # Accounts removed from the active users roster by an administrator or by
    # referral-integrity controls retain their encrypted record here for
    # explicit verification or restoration. Quarantining never discards the
    # evidence needed to reverse a false positive, and the reserved blind
    # indexes prevent re-registration.
    """CREATE TABLE IF NOT EXISTS quarantined_users (
        user_bi TEXT PRIMARY KEY,
        data TEXT NOT NULL,
        email_bi TEXT,
        username TEXT NOT NULL,
        is_admin INTEGER NOT NULL DEFAULT 0,
        ip_bi TEXT,
        discord_bi TEXT,
        enable_outreach INTEGER NOT NULL DEFAULT 0,
        referrer_bi TEXT NOT NULL DEFAULT '',
        reason_codes TEXT NOT NULL DEFAULT '[]',
        status TEXT NOT NULL DEFAULT 'pending_verification'
            CHECK (status IN ('pending_verification','rejected','restored')),
        quarantined_at INTEGER NOT NULL,
        review_after INTEGER NOT NULL DEFAULT 0,
        reviewed_at INTEGER NOT NULL DEFAULT 0,
        reviewed_by TEXT NOT NULL DEFAULT '')""",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_quarantined_users_email "
    "ON quarantined_users(email_bi) WHERE email_bi IS NOT NULL AND email_bi<>''",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_quarantined_users_discord "
    "ON quarantined_users(discord_bi) "
    "WHERE discord_bi IS NOT NULL AND discord_bi<>''",
    "CREATE INDEX IF NOT EXISTS idx_quarantined_users_referrer "
    "ON quarantined_users(referrer_bi,status,quarantined_at DESC)",
    # Node identities owned by a quarantined user must leave the active nodes
    # table too; otherwise account lookup can fall back to the mirrored node
    # row. Keeping the ciphertext here makes an approved restoration complete.
    """CREATE TABLE IF NOT EXISTS quarantined_nodes (
        node_bi TEXT PRIMARY KEY,
        user_bi TEXT,
        pubkey TEXT,
        data TEXT NOT NULL,
        name TEXT,
        last_seen INTEGER NOT NULL DEFAULT 0,
        quarantined_user_bi TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'pending_verification'
            CHECK (status IN ('pending_verification','rejected','restored')),
        quarantined_at INTEGER NOT NULL,
        reviewed_at INTEGER NOT NULL DEFAULT 0,
        reviewed_by TEXT NOT NULL DEFAULT '')""",
    "CREATE INDEX IF NOT EXISTS idx_quarantined_nodes_user "
    "ON quarantined_nodes(quarantined_user_bi,status)",
    # A burst freezes its referrer's automatic credit until an administrator
    # reviews the cohort. New signups may still join, but cannot move rewards.
    """CREATE TABLE IF NOT EXISTS referral_referrer_controls (
        referrer_bi TEXT PRIMARY KEY,
        state TEXT NOT NULL CHECK (state IN ('active','frozen','blocked')),
        reason_codes TEXT NOT NULL DEFAULT '[]',
        updated_at INTEGER NOT NULL,
        review_after INTEGER NOT NULL DEFAULT 0,
        reviewed_at INTEGER NOT NULL DEFAULT 0,
        reviewed_by TEXT NOT NULL DEFAULT '')""",
    "CREATE INDEX IF NOT EXISTS idx_referral_referrer_controls_state "
    "ON referral_referrer_controls(state,updated_at DESC)",
    # A one-time audit remainder from the retired counter-only implementation.
    # These values never grant credit or affect rank; they preserve how many
    # historical identities could not be proven from retained signup evidence.
    """CREATE TABLE IF NOT EXISTS referral_legacy_totals (
        referrer_bi TEXT PRIMARY KEY,
        name TEXT NOT NULL,
        unattributed_signups INTEGER NOT NULL DEFAULT 0,
        migrated_at INTEGER NOT NULL DEFAULT 0)""",
    # One row per account that arrived with a valid referral link. Attribution
    # stays pending until email verification; automatic decisions then either
    # credit it or retain the rejection reason for counter integrity. IP and
    # normalized User-Agent are searchable only as keyed blind indexes. `data`
    # encrypts the bounded raw signup IP/User-Agent plus coarse country/client
    # category for the is_admin investigation view; none are public fields.
    """CREATE TABLE IF NOT EXISTS referral_attributions (
        referred_bi TEXT PRIMARY KEY,
        referred_name TEXT NOT NULL,
        referrer_bi TEXT NOT NULL,
        referrer_name TEXT NOT NULL,
        signup_ip_bi TEXT NOT NULL DEFAULT '',
        user_agent_bi TEXT NOT NULL DEFAULT '',
        client_fingerprint_bi TEXT NOT NULL DEFAULT '',
        referral_click_id TEXT NOT NULL DEFAULT '',
        network_window_bi TEXT NOT NULL DEFAULT '',
        risk_score INTEGER NOT NULL DEFAULT 0 CHECK (risk_score>=0),
        decision TEXT NOT NULL DEFAULT 'pending'
            CHECK (decision IN ('pending','credited','rejected')),
        risk_flags TEXT NOT NULL DEFAULT '[]',
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        verified_at INTEGER NOT NULL DEFAULT 0,
        credited_at INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_referral_attributions_referrer "
    "ON referral_attributions(referrer_bi, decision, credited_at DESC)",
    "CREATE INDEX IF NOT EXISTS idx_referral_attributions_network "
    "ON referral_attributions(signup_ip_bi, created_at DESC)",
    "CREATE INDEX IF NOT EXISTS idx_referral_attributions_agent "
    "ON referral_attributions(user_agent_bi, created_at DESC)",
    "CREATE INDEX IF NOT EXISTS idx_referral_attributions_client "
    "ON referral_attributions(client_fingerprint_bi, created_at DESC)",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_referral_attributions_click "
    "ON referral_attributions(referral_click_id) "
    "WHERE referral_click_id<>''",
    """CREATE TRIGGER IF NOT EXISTS referral_frozen_credit_update
        BEFORE UPDATE OF decision ON referral_attributions
        FOR EACH ROW
        WHEN NEW.decision='credited' AND EXISTS (
            SELECT 1 FROM referral_referrer_controls
            WHERE referrer_bi=NEW.referrer_bi
              AND state IN ('frozen','blocked'))
        BEGIN
          SELECT RAISE(ABORT,'referrer rewards frozen');
        END""",
    # Click totals count at most one source/referrer/window. visit_bi is a keyed
    # digest of referrer + IP blind index + User-Agent blind index + time
    # bucket; no raw request value enters this short-lived dedupe table.
    """CREATE TABLE IF NOT EXISTS referral_click_seen (
        visit_bi TEXT PRIMARY KEY,
        referrer_bi TEXT NOT NULL,
        seen_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_referral_click_seen_expiry "
    "ON referral_click_seen(seen_at)",
    # Server-issued referral clicks are signed in the redirect and consumed at
    # most once by signup. A browser-supplied referrer name without this row
    # cannot create attribution.
    """CREATE TABLE IF NOT EXISTS referral_click_claims (
        click_id TEXT PRIMARY KEY,
        referrer_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        expires_at INTEGER NOT NULL,
        consumed_at INTEGER NOT NULL DEFAULT 0,
        referred_bi TEXT NOT NULL DEFAULT '')""",
    "CREATE INDEX IF NOT EXISTS idx_referral_click_claims_expiry "
    "ON referral_click_claims(expires_at,consumed_at)",
    # Admin-only click-integrity ledger. One row represents one bounded
    # source/referrer/day key and repeat_count records refresh/replay attempts.
    # Raw address and User-Agent evidence is encrypted in data; equality and
    # uniqueness checks use only keyed blind indexes.
    """CREATE TABLE IF NOT EXISTS referral_click_audit (
        visit_bi TEXT PRIMARY KEY,
        referrer_bi TEXT NOT NULL,
        ip_bi TEXT NOT NULL DEFAULT '',
        user_agent_bi TEXT NOT NULL DEFAULT '',
        decision TEXT NOT NULL
            CHECK (decision IN ('accepted','duplicate','rejected')),
        risk_flags TEXT NOT NULL DEFAULT '[]',
        data TEXT NOT NULL,
        first_seen_at INTEGER NOT NULL,
        last_seen_at INTEGER NOT NULL,
        repeat_count INTEGER NOT NULL DEFAULT 1
            CHECK (repeat_count > 0))""",
    "CREATE INDEX IF NOT EXISTS idx_referral_click_audit_referrer "
    "ON referral_click_audit(referrer_bi, last_seen_at DESC)",
    "CREATE INDEX IF NOT EXISTS idx_referral_click_audit_expiry "
    "ON referral_click_audit(last_seen_at)",
    # Achievement badges (migration 0084): public recognition marks awarded
    # once per (badge, account), either automatically at a real platform event
    # (first 100 signups, an hour in the World, the first successful referral,
    # becoming a mirror operator) or by a platform administrator. account_bi is
    # a blind index; name is the plaintext public username (same trust level as
    # profile_follows / referral_stats.name). granted_by is 'system' for an
    # automatic award or the granting admin's username. Awards are permanent:
    # later losing eligibility does not revoke an already-earned badge.
    """CREATE TABLE IF NOT EXISTS badge_awards (
        badge_slug TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        name TEXT NOT NULL,
        granted_by TEXT NOT NULL DEFAULT 'system',
        created_at INTEGER NOT NULL,
        tier INTEGER NOT NULL DEFAULT 0 CHECK (tier BETWEEN 0 AND 3),
        tier_at INTEGER NOT NULL DEFAULT 0 CHECK (tier_at >= 0),
        PRIMARY KEY (badge_slug, account_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_badge_awards_account "
    "ON badge_awards(account_bi)",
    """CREATE TABLE IF NOT EXISTS badge_progress (
        badge_slug TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        value INTEGER NOT NULL DEFAULT 0 CHECK (value >= 0),
        updated_at INTEGER NOT NULL CHECK (updated_at >= 0),
        PRIMARY KEY (badge_slug, account_bi))""",
    """CREATE TABLE IF NOT EXISTS node_uptime_totals (
        node_key TEXT PRIMARY KEY,
        minutes INTEGER NOT NULL DEFAULT 0 CHECK (minutes >= 0),
        updated_at INTEGER NOT NULL CHECK (updated_at >= 0))""",
    """CREATE TABLE IF NOT EXISTS badge_pull_facts (
        repo_bi TEXT NOT NULL,
        kind TEXT NOT NULL CHECK (kind IN ('pull', 'issue')),
        number INTEGER NOT NULL CHECK (number > 0),
        author_key TEXT NOT NULL DEFAULT '',
        closer_key TEXT NOT NULL DEFAULT '',
        closer_bi TEXT NOT NULL DEFAULT '',
        opened_at INTEGER NOT NULL DEFAULT 0 CHECK (opened_at >= 0),
        closed_at INTEGER NOT NULL DEFAULT 0 CHECK (closed_at >= 0),
        merged INTEGER NOT NULL DEFAULT 0 CHECK (merged IN (0, 1)),
        reviewed INTEGER NOT NULL DEFAULT 0 CHECK (reviewed IN (0, 1)),
        review_waived INTEGER NOT NULL DEFAULT 0
            CHECK (review_waived IN (0, 1)),
        updated_at INTEGER NOT NULL DEFAULT 0 CHECK (updated_at >= 0),
        PRIMARY KEY (repo_bi, kind, number))""",
    "CREATE INDEX IF NOT EXISTS idx_badge_pull_facts_author "
    "ON badge_pull_facts(author_key)",
    """CREATE TABLE IF NOT EXISTS badge_coauthor_facts (
        repo_bi TEXT NOT NULL,
        number INTEGER NOT NULL CHECK (number > 0),
        account_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL CHECK (created_at >= 0),
        PRIMARY KEY (repo_bi, number, account_bi))""",
    """CREATE TABLE IF NOT EXISTS badge_repo_sweeps (
        repo_bi TEXT PRIMARY KEY,
        state_ts INTEGER NOT NULL DEFAULT 0 CHECK (state_ts >= 0),
        swept_at INTEGER NOT NULL DEFAULT 0 CHECK (swept_at >= 0))""",
    """CREATE TABLE IF NOT EXISTS badge_answer_facts (
        repo_bi TEXT NOT NULL,
        number INTEGER NOT NULL CHECK (number > 0),
        dir_oid TEXT NOT NULL DEFAULT '',
        opener_key TEXT NOT NULL DEFAULT '',
        answerer_key TEXT NOT NULL DEFAULT '',
        marked_at INTEGER NOT NULL DEFAULT 0 CHECK (marked_at >= 0),
        updated_at INTEGER NOT NULL DEFAULT 0 CHECK (updated_at >= 0),
        PRIMARY KEY (repo_bi, number))""",
    # Inbound website referrals: one aggregate row per referring hostname.
    # last_url is the latest bounded public referring URL, with credentials,
    # fragments, and sensitive query values removed (migration 0095).
    """CREATE TABLE IF NOT EXISTS site_referrers (
        host TEXT PRIMARY KEY,
        visits INTEGER NOT NULL DEFAULT 0,
        first_ts INTEGER NOT NULL DEFAULT 0,
        last_ts INTEGER NOT NULL DEFAULT 0,
        last_url TEXT NOT NULL DEFAULT '')""",
    "CREATE INDEX IF NOT EXISTS idx_site_referrers_rank "
    "ON site_referrers(visits DESC, last_ts DESC)",
    # Consented World lobby link submissions. URLs/account names live only in
    # encrypted data; public score/range values are bounded (migration 0103).
    """CREATE TABLE IF NOT EXISTS world_lobby_links (
        link_id TEXT PRIMARY KEY,
        account_bi TEXT NOT NULL,
        url_bi TEXT NOT NULL,
        data TEXT NOT NULL,
        score INTEGER NOT NULL CHECK (score BETWEEN 0 AND 100),
        potential_low INTEGER NOT NULL CHECK (potential_low >= 0),
        potential_high INTEGER NOT NULL CHECK (
            potential_high >= potential_low),
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        UNIQUE(account_bi, url_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_world_lobby_links_recent "
    "ON world_lobby_links(created_at DESC, link_id DESC)",
    "CREATE INDEX IF NOT EXISTS idx_world_lobby_links_account "
    "ON world_lobby_links(account_bi, created_at DESC)",
    # Aggregate-only reach for public blog posts (migration 0094). Unique
    # counts use a fixed 64-register HLL per slug; no visitor identifier or
    # digest is stored. Referrers retain only the normalized external host.
    """CREATE TABLE IF NOT EXISTS blog_post_metrics (
        slug TEXT PRIMARY KEY,
        views INTEGER NOT NULL DEFAULT 0 CHECK (views >= 0),
        updated_at INTEGER NOT NULL DEFAULT 0 CHECK (updated_at >= 0))""",
    """CREATE TABLE IF NOT EXISTS blog_post_unique_hll (
        slug TEXT NOT NULL,
        register_id INTEGER NOT NULL
            CHECK (register_id >= 0 AND register_id < 64),
        rank INTEGER NOT NULL CHECK (rank >= 1 AND rank <= 251),
        PRIMARY KEY (slug, register_id)
    ) WITHOUT ROWID""",
    """CREATE TABLE IF NOT EXISTS blog_post_referrers (
        slug TEXT NOT NULL,
        host TEXT NOT NULL,
        visits INTEGER NOT NULL DEFAULT 0 CHECK (visits >= 0),
        last_ts INTEGER NOT NULL DEFAULT 0 CHECK (last_ts >= 0),
        PRIMARY KEY (slug, host)
    ) WITHOUT ROWID""",
    "CREATE INDEX IF NOT EXISTS idx_blog_post_referrers_rank "
    "ON blog_post_referrers(slug, visits DESC, last_ts DESC)",
    # Read dedupe (migration 0127). One row per post per blinded reader so a
    # refresh is not another view; `viewer_key` is the same per-slug one-way
    # digest the HLL folds, and rows age out on the hourly referrer prune.
    """CREATE TABLE IF NOT EXISTS blog_post_view_seen (
        slug TEXT NOT NULL,
        viewer_key TEXT NOT NULL,
        first_at INTEGER NOT NULL CHECK (first_at >= 0),
        last_at INTEGER NOT NULL CHECK (last_at >= 0),
        counted_at INTEGER NOT NULL DEFAULT 0 CHECK (counted_at >= 0),
        hits INTEGER NOT NULL DEFAULT 1 CHECK (hits >= 1),
        referrer_host TEXT NOT NULL DEFAULT ''
            CHECK (length(referrer_host) <= 100),
        PRIMARY KEY (slug, viewer_key)
    ) WITHOUT ROWID""",
    "CREATE INDEX IF NOT EXISTS idx_blog_post_view_seen_age "
    "ON blog_post_view_seen(last_at)",
    # Where a post was announced (migration 0126). Public permalinks only,
    # one row per post and network, written by an is_admin session.
    """CREATE TABLE IF NOT EXISTS blog_post_links (
        slug TEXT NOT NULL,
        network TEXT NOT NULL
            CHECK (network IN ('reddit', 'mastodon', 'twitter')),
        url TEXT NOT NULL DEFAULT '' CHECK (length(url) <= 512),
        updated_at INTEGER NOT NULL DEFAULT 0 CHECK (updated_at >= 0),
        updated_by_bi TEXT NOT NULL DEFAULT '',
        PRIMARY KEY (slug, network)
    ) WITHOUT ROWID""",
    """CREATE TABLE IF NOT EXISTS world_build_board_items (
        item_key TEXT PRIMARY KEY,
        kind TEXT NOT NULL CHECK (kind IN ('task','issue')),
        owner TEXT NOT NULL DEFAULT '',
        repo TEXT NOT NULL DEFAULT '',
        issue_number INTEGER NOT NULL DEFAULT 0
            CHECK (issue_number >= 0),
        title TEXT NOT NULL DEFAULT '' CHECK (length(title) <= 160),
        priority INTEGER NOT NULL CHECK (priority >= 1 AND priority <= 64),
        updated_by_bi TEXT NOT NULL,
        updated_at INTEGER NOT NULL,
        completed_at INTEGER NOT NULL DEFAULT 0
            CHECK (completed_at >= 0),
        completed_by_bi TEXT NOT NULL DEFAULT '')""",
    "CREATE INDEX IF NOT EXISTS idx_world_build_board_priority "
    "ON world_build_board_items(priority, item_key)",
    """CREATE TABLE IF NOT EXISTS world_deploy_status (
        singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
        state TEXT NOT NULL
            CHECK (state IN ('idle', 'deploying', 'ready', 'failed')),
        revision TEXT NOT NULL DEFAULT '' CHECK (length(revision) <= 96),
        started_at INTEGER NOT NULL DEFAULT 0 CHECK (started_at >= 0),
        finished_at INTEGER NOT NULL DEFAULT 0 CHECK (finished_at >= 0))""",
    """CREATE TABLE IF NOT EXISTS world_qa_reviews (
        account_bi TEXT NOT NULL,
        item_key TEXT NOT NULL CHECK (length(item_key) BETWEEN 1 AND 80),
        verdict TEXT NOT NULL CHECK (verdict IN ('pass','fail','unsure')),
        reviewed_at INTEGER NOT NULL CHECK (reviewed_at >= 0),
        PRIMARY KEY (account_bi, item_key))""",
    "CREATE INDEX IF NOT EXISTS idx_world_qa_reviews_account_time "
    "ON world_qa_reviews(account_bi, reviewed_at DESC)",
    """CREATE TABLE IF NOT EXISTS world_qa_items (
        item_key TEXT PRIMARY KEY
            CHECK (length(item_key) BETWEEN 1 AND 80),
        title TEXT NOT NULL CHECK (length(title) BETWEEN 1 AND 160),
        how_to_test TEXT NOT NULL
            CHECK (length(how_to_test) BETWEEN 1 AND 720),
        source_key TEXT NOT NULL DEFAULT ''
            CHECK (length(source_key) <= 80),
        added_at INTEGER NOT NULL CHECK (added_at >= 0),
        active INTEGER NOT NULL DEFAULT 1 CHECK (active IN (0,1)))""",
    "CREATE INDEX IF NOT EXISTS idx_world_qa_items_active_time "
    "ON world_qa_items(active, added_at DESC)",
    """CREATE TABLE IF NOT EXISTS world_floor_notes (
        note_id TEXT PRIMARY KEY CHECK (length(note_id) = 32),
        account_bi TEXT NOT NULL,
        x REAL NOT NULL CHECK (x BETWEEN -1500 AND 1500),
        y REAL NOT NULL CHECK (y BETWEEN -20 AND 200),
        z REAL NOT NULL CHECK (z BETWEEN -1500 AND 1500),
        created_at INTEGER NOT NULL CHECK (created_at >= 0),
        updated_at INTEGER NOT NULL CHECK (updated_at >= 0),
        reply_count INTEGER NOT NULL DEFAULT 0 CHECK (reply_count >= 0),
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_world_floor_notes_updated "
    "ON world_floor_notes(updated_at DESC, note_id DESC)",
    "CREATE INDEX IF NOT EXISTS idx_world_floor_notes_account "
    "ON world_floor_notes(account_bi, created_at DESC)",
    """CREATE TABLE IF NOT EXISTS world_floor_note_replies (
        reply_id TEXT PRIMARY KEY CHECK (length(reply_id) = 32),
        note_id TEXT NOT NULL CHECK (length(note_id) = 32),
        account_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL CHECK (created_at >= 0),
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_world_floor_note_replies_note "
    "ON world_floor_note_replies(note_id, created_at DESC)",
    "CREATE INDEX IF NOT EXISTS idx_world_floor_note_replies_account "
    "ON world_floor_note_replies(account_bi, created_at DESC)",
    # Organization bot secrets are shown exactly once. Only their keyed blind
    # index and encrypted metadata remain server-side, making listing and
    # revocation possible without creating a credential-recovery endpoint.
    """CREATE TABLE IF NOT EXISTS org_bot_tokens (
        token_id TEXT PRIMARY KEY,
        org_bi TEXT NOT NULL,
        secret_bi TEXT NOT NULL UNIQUE,
        provider TEXT NOT NULL
            CHECK (provider IN ('codex','claude-code')),
        data TEXT NOT NULL,
        created_by_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL CHECK (created_at >= 0),
        last_used_at INTEGER NOT NULL DEFAULT 0 CHECK (last_used_at >= 0),
        expires_at INTEGER NOT NULL DEFAULT 0 CHECK (expires_at >= 0),
        revoked_at INTEGER NOT NULL DEFAULT 0 CHECK (revoked_at >= 0))""",
    "CREATE INDEX IF NOT EXISTS idx_org_bot_tokens_org "
    "ON org_bot_tokens(org_bi, revoked_at, created_at DESC)",
    # Metadata-only use history: no token secret, body, query string, network
    # address, or user agent is retained. The generic platform admin browser
    # can inspect the full append-only table while org admins receive only a
    # bounded preview through the purpose-built token endpoint.
    """CREATE TABLE IF NOT EXISTS org_bot_token_usage (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        token_id TEXT NOT NULL,
        org_bi TEXT NOT NULL,
        provider TEXT NOT NULL
            CHECK (provider IN ('codex','claude-code')),
        action TEXT NOT NULL CHECK (length(action) BETWEEN 1 AND 120),
        method TEXT NOT NULL
            CHECK (method IN ('GET','POST','PUT','PATCH','DELETE')),
        used_at INTEGER NOT NULL CHECK (used_at >= 0))""",
    "CREATE INDEX IF NOT EXISTS idx_org_bot_token_usage_org_time "
    "ON org_bot_token_usage(org_bi, used_at DESC, id DESC)",
    # Retention is account-wide, so neither org/token-leading index can serve
    # its timestamp-only DELETE. Keep that hourly sweep from scanning the full
    # append-only table (migration 0172).
    "CREATE INDEX IF NOT EXISTS idx_org_bot_token_usage_used_at "
    "ON org_bot_token_usage(used_at)",
    # Organization Discord connector policy.  The Worker secret used to call
    # Discord is never persisted here: this encrypted record contains only a
    # selected guild and bounded allowlist of public text-channel ids.
    """CREATE TABLE IF NOT EXISTS organization_discord_connectors (
        org_bi TEXT PRIMARY KEY,
        data TEXT NOT NULL,
        updated_by_bi TEXT NOT NULL,
        updated_at INTEGER NOT NULL CHECK (updated_at >= 0))""",
    "CREATE INDEX IF NOT EXISTS idx_organization_discord_connectors_updated "
    "ON organization_discord_connectors(updated_at DESC)",
    # A single durable, unassigned human setup task per organization.  It
    # deduplicates changing Discord setup states without storing the task text
    # or any credential outside organization_tasks' encrypted payload.
    """CREATE TABLE IF NOT EXISTS organization_discord_setup_tasks (
        org_bi TEXT PRIMARY KEY,
        task_id TEXT NOT NULL UNIQUE,
        created_at INTEGER NOT NULL CHECK (created_at >= 0),
        updated_at INTEGER NOT NULL CHECK (updated_at >= 0))""",
    # A bot's global guild membership is not organization consent. This
    # encrypted OAuth proof binds a single guild to an org before
    # any bot channel/message call is allowed. OAuth access/refresh tokens,
    # authorization codes, and raw callback state are never stored.
    """CREATE TABLE IF NOT EXISTS organization_discord_oauth_grants (
        org_bi TEXT PRIMARY KEY,
        data TEXT NOT NULL,
        verified_by_bi TEXT NOT NULL,
        verified_at INTEGER NOT NULL CHECK (verified_at >= 0),
        updated_at INTEGER NOT NULL CHECK (updated_at >= 0))""",
    "CREATE INDEX IF NOT EXISTS idx_organization_discord_oauth_grants_verified "
    "ON organization_discord_oauth_grants(verified_at DESC)",
    # The raw state and PKCE verifier live only inside this encrypted, ten
    # minute record. Its SHA-256 lookup digest makes the callback one-time
    # without persisting a reusable raw state value. org_bi is metadata only,
    # used to revoke pending attempts when an organization is deleted.
    """CREATE TABLE IF NOT EXISTS organization_discord_oauth_states (
        state_hash TEXT PRIMARY KEY,
        org_bi TEXT NOT NULL,
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL CHECK (created_at >= 0),
        expires_at INTEGER NOT NULL CHECK (expires_at >= 0))""",
    "CREATE INDEX IF NOT EXISTS idx_organization_discord_oauth_states_org "
    "ON organization_discord_oauth_states(org_bi, expires_at)",
    # Discord bug-channel watcher cursor. The channel id is stored only as a
    # blind index and the row carries one snowflake, so a database copy reveals
    # neither which channel is watched nor anything that was said in it.
    """CREATE TABLE IF NOT EXISTS discord_bug_watch (
        channel_bi TEXT PRIMARY KEY,
        last_message_id TEXT NOT NULL,
        updated_at INTEGER NOT NULL CHECK (updated_at >= 0))""",
    # One row per bug-channel message the watcher has already answered. It is
    # both the idempotency record (a phase that files an issue and then dies
    # before its cursor write must not re-file) and the offline half of the
    # duplicate-review corpus, so the encrypted payload keeps the issue number
    # and title it produced.
    """CREATE TABLE IF NOT EXISTS discord_bug_reports (
        message_bi TEXT PRIMARY KEY,
        repo_bi TEXT NOT NULL,
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL CHECK (created_at >= 0))""",
    "CREATE INDEX IF NOT EXISTS idx_discord_bug_reports_repo "
    "ON discord_bug_reports(repo_bi, created_at DESC)",
    # In-app bug reports (app/src/bug_reports.py). The reporter's prose and any
    # contact string they typed are user copy and live inside the encrypted
    # `data` blob; only columns the operator console lists, filters or counts
    # on stay in plaintext, so a report list never has to decrypt to paginate.
    # reporter_bi is the blind index of the account, or of the transient client
    # address when the report was filed signed out — it exists so an hourly
    # quota can count one reporter's recent submissions without the row
    # retaining the address itself (migration 0173).
    """CREATE TABLE IF NOT EXISTS bug_reports (
        report_id TEXT PRIMARY KEY CHECK (length(report_id) = 32),
        reporter_bi TEXT NOT NULL DEFAULT '',
        account TEXT NOT NULL DEFAULT '',
        source TEXT NOT NULL DEFAULT 'unknown',
        platform TEXT NOT NULL DEFAULT '',
        app_version TEXT NOT NULL DEFAULT '',
        severity TEXT NOT NULL DEFAULT 'normal'
            CHECK (severity IN ('low','normal','high','crash')),
        status TEXT NOT NULL DEFAULT 'new'
            CHECK (status IN ('new','triaged','closed')),
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL CHECK (created_at > 0))""",
    "CREATE INDEX IF NOT EXISTS idx_bug_reports_created "
    "ON bug_reports(created_at DESC)",
    # The submission path's quota read: one reporter, one recent window.
    "CREATE INDEX IF NOT EXISTS idx_bug_reports_reporter "
    "ON bug_reports(reporter_bi, created_at DESC)",
    # The console's default view is the untriaged queue, newest first.
    "CREATE INDEX IF NOT EXISTS idx_bug_reports_status "
    "ON bug_reports(status, created_at DESC)",
    """CREATE TABLE IF NOT EXISTS bug_report_attachments (
        report_id TEXT NOT NULL CHECK (length(report_id) = 32),
        attachment_id TEXT NOT NULL CHECK (length(attachment_id) = 32),
        chunk_index INTEGER NOT NULL CHECK (chunk_index >= 0),
        name TEXT NOT NULL,
        content_type TEXT NOT NULL DEFAULT '',
        size INTEGER NOT NULL CHECK (size > 0),
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL CHECK (created_at > 0),
        PRIMARY KEY (report_id, attachment_id, chunk_index))""",
    "CREATE INDEX IF NOT EXISTS idx_bug_report_attachments_report "
    "ON bug_report_attachments(report_id, attachment_id, chunk_index)",
    # Collaborative Markdown notes. User copy and every retained snapshot are
    # encrypted; the remaining columns are bounded routing and ACL metadata.
    """CREATE TABLE IF NOT EXISTS notes (
        note_id TEXT PRIMARY KEY CHECK (length(note_id) = 32),
        owner_bi TEXT NOT NULL,
        visibility TEXT NOT NULL DEFAULT 'private'
            CHECK (visibility IN ('private','public')),
        version INTEGER NOT NULL DEFAULT 1 CHECK (version >= 1),
        created_at INTEGER NOT NULL CHECK (created_at >= 0),
        updated_at INTEGER NOT NULL CHECK (updated_at >= 0),
        published_at INTEGER NOT NULL DEFAULT 0 CHECK (published_at >= 0),
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_notes_owner_updated "
    "ON notes(owner_bi, updated_at DESC)",
    "CREATE INDEX IF NOT EXISTS idx_notes_public_updated "
    "ON notes(visibility, updated_at DESC)",
    """CREATE TABLE IF NOT EXISTS note_shares (
        note_id TEXT NOT NULL REFERENCES notes(note_id) ON DELETE CASCADE,
        principal_type TEXT NOT NULL
            CHECK (principal_type IN ('user','organization')),
        principal_bi TEXT NOT NULL,
        principal_name TEXT NOT NULL,
        role TEXT NOT NULL CHECK (role IN ('viewer','editor')),
        added_by_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL CHECK (created_at >= 0),
        PRIMARY KEY(note_id, principal_type, principal_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_note_shares_principal "
    "ON note_shares(principal_type, principal_bi, note_id)",
    """CREATE TABLE IF NOT EXISTS note_versions (
        note_id TEXT NOT NULL REFERENCES notes(note_id) ON DELETE CASCADE,
        version INTEGER NOT NULL CHECK (version >= 1),
        author_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL CHECK (created_at >= 0),
        data TEXT NOT NULL,
        PRIMARY KEY(note_id, version))""",
    """CREATE TABLE IF NOT EXISTS note_links (
        note_id TEXT NOT NULL REFERENCES notes(note_id) ON DELETE CASCADE,
        owner TEXT NOT NULL,
        repo TEXT NOT NULL,
        kind TEXT NOT NULL CHECK (kind IN ('issue','pull','discussion')),
        number INTEGER NOT NULL CHECK (number BETWEEN 1 AND 999999999),
        created_at INTEGER NOT NULL CHECK (created_at >= 0),
        PRIMARY KEY(note_id, owner, repo, kind, number))""",
    "CREATE INDEX IF NOT EXISTS idx_note_links_target "
    "ON note_links(owner, repo, kind, number, note_id)",
    # Read counts for published notes. One row per note per blinded reader:
    # the owner sees distinct readers and total reads, and no address is kept.
    """CREATE TABLE IF NOT EXISTS note_views (
        note_id TEXT NOT NULL REFERENCES notes(note_id) ON DELETE CASCADE,
        viewer_key TEXT NOT NULL,
        first_at INTEGER NOT NULL CHECK (first_at >= 0),
        last_at INTEGER NOT NULL CHECK (last_at >= 0),
        hits INTEGER NOT NULL DEFAULT 1 CHECK (hits >= 1),
        PRIMARY KEY(note_id, viewer_key))""",
    "CREATE INDEX IF NOT EXISTS idx_note_views_note "
    "ON note_views(note_id, last_at DESC)",
    # Who opened, shared, published, linked or restored a note. Views by one
    # account within half an hour fold into one row; at most 500 per note.
    """CREATE TABLE IF NOT EXISTS note_events (
        id INTEGER PRIMARY KEY,
        note_id TEXT NOT NULL REFERENCES notes(note_id) ON DELETE CASCADE,
        kind TEXT NOT NULL,
        actor_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL CHECK (created_at >= 0),
        last_at INTEGER NOT NULL CHECK (last_at >= 0),
        hits INTEGER NOT NULL DEFAULT 1 CHECK (hits >= 1),
        detail TEXT NOT NULL DEFAULT '{}')""",
    "CREATE INDEX IF NOT EXISTS idx_note_events_note "
    "ON note_events(note_id, last_at DESC)",
    # Failed forkmesh/forkmesh content reads per minute (src/api_metrics.py),
    # the /status sampler's visitor-side outage evidence. Written only when a
    # read fails, pruned to a day on that same path. Per-route traffic
    # statistics are not stored anywhere: the desktop counts them from the
    # Worker log tail (migration 0190 dropped api_metrics_minute).
    """CREATE TABLE IF NOT EXISTS flagship_read_failure_minute (
        minute_ts INTEGER PRIMARY KEY CHECK (minute_ts >= 0),
        failures INTEGER NOT NULL DEFAULT 0 CHECK (failures >= 0))""",
    # Last-good public JSON projections served directly by the App Worker.
    # This table contains public response bodies only; private catalog fields
    # are rejected by public_snapshots.py before an UPSERT is prepared.
    """CREATE TABLE IF NOT EXISTS public_api_snapshots (
        path TEXT PRIMARY KEY NOT NULL,
        schema_version INTEGER NOT NULL,
        kind TEXT NOT NULL,
        generated_at INTEGER NOT NULL,
        fresh_until INTEGER NOT NULL,
        stale_until INTEGER NOT NULL,
        deploy_fingerprint TEXT NOT NULL DEFAULT '',
        content_length INTEGER NOT NULL,
        payload TEXT NOT NULL,
        CHECK (content_length > 1 AND content_length <= 1572864),
        CHECK (length(payload) = content_length),
        CHECK (fresh_until >= generated_at),
        CHECK (stale_until = fresh_until)
    ) WITHOUT ROWID""",
    # Member-to-member tip notices. ForkMesh is not in this transfer: the
    # sender's desktop holds the wallet key in its own encrypted vault, signs
    # locally, and broadcasts straight to a Solana RPC. This table records only
    # the public receipt so the recipient can be told (and so a replayed
    # notice cannot mail them twice) — the chain signature is the primary key.
    # Both account names are blind-indexed; nothing here identifies a wallet
    # key, and lamports/ts stay plaintext for the per-sender rate limit.
    """CREATE TABLE IF NOT EXISTS tip_notice (
        signature TEXT PRIMARY KEY CHECK (length(signature) BETWEEN 64 AND 88),
        from_bi TEXT NOT NULL,
        to_bi TEXT NOT NULL,
        lamports INTEGER NOT NULL CHECK (lamports > 0),
        network TEXT NOT NULL DEFAULT 'mainnet'
            CHECK (network IN ('mainnet','devnet','testnet')),
        ts INTEGER NOT NULL CHECK (ts >= 0),
        emailed INTEGER NOT NULL DEFAULT 0 CHECK (emailed IN (0,1)),
        task_description TEXT NOT NULL DEFAULT ''
            CHECK (length(task_description) <= 360),
        output_url TEXT NOT NULL DEFAULT ''
            CHECK (length(output_url) <= 1000),
        entered_currency TEXT NOT NULL DEFAULT 'SOL'
            CHECK (entered_currency IN ('SOL','USD')),
        entered_usd_micros INTEGER NOT NULL DEFAULT 0
            CHECK (entered_usd_micros >= 0),
        sol_usd_price_micros INTEGER NOT NULL DEFAULT 0
            CHECK (sol_usd_price_micros >= 0),
        price_observed_at INTEGER NOT NULL DEFAULT 0
            CHECK (price_observed_at >= 0))""",
    "CREATE INDEX IF NOT EXISTS idx_tip_notice_from_ts ON tip_notice(from_bi, ts)",
    "CREATE INDEX IF NOT EXISTS idx_tip_notice_to_ts ON tip_notice(to_bi, ts)",
    # Paid microjobs (migration 0131): one row per claimed piece of small,
    # independently verified work. The reviewer's verdict and the readable
    # subject stay inside the encrypted envelope; the plaintext columns are
    # only what the payout batcher and the per-account rate limits need in
    # SQL. subject_bi is a blind index over the exact work claimed, and its
    # UNIQUE index is the de-duplication — one issue, one merged pull, or one
    # posted page can be claimed once. Payment is still non-custodial: an
    # approved claim becomes a public unsigned chain_intents plan that only
    # the owner's local desktop vault can sign.
    """CREATE TABLE IF NOT EXISTS microjob_claims (
        claim_id TEXT PRIMARY KEY,
        account_bi TEXT NOT NULL,
        job TEXT NOT NULL
            CHECK (job IN ('issue_report','merged_pull','referral_post')),
        subject_bi TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'pending_review'
            CHECK (status IN
                ('pending_review','approved','queued','paid','rejected')),
        amount_lamports INTEGER NOT NULL DEFAULT 0
            CHECK (amount_lamports >= 0),
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL CHECK (created_at >= 0),
        updated_at INTEGER NOT NULL CHECK (updated_at >= 0),
        intent_id TEXT NOT NULL DEFAULT '',
        tx_signature TEXT NOT NULL DEFAULT '')""",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_microjob_claims_subject "
    "ON microjob_claims(subject_bi)",
    "CREATE INDEX IF NOT EXISTS idx_microjob_claims_account "
    "ON microjob_claims(account_bi, created_at)",
    "CREATE INDEX IF NOT EXISTS idx_microjob_claims_status "
    "ON microjob_claims(status, created_at)",
    "CREATE INDEX IF NOT EXISTS idx_microjob_claims_intent "
    "ON microjob_claims(intent_id)",
    "CREATE INDEX IF NOT EXISTS idx_microjob_claims_tx_signature "
    "ON microjob_claims(tx_signature) WHERE tx_signature<>''",
    """CREATE TABLE IF NOT EXISTS affiliates (
        account_bi TEXT PRIMARY KEY,
        username TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'active'
            CHECK (status IN ('active','paused','removed')),
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL CHECK (created_at >= 0),
        updated_at INTEGER NOT NULL CHECK (updated_at >= 0))""",
    "CREATE INDEX IF NOT EXISTS idx_affiliates_username "
    "ON affiliates(username)",
    """CREATE TABLE IF NOT EXISTS affiliate_leads (
        lead_id TEXT PRIMARY KEY,
        affiliate_bi TEXT NOT NULL,
        domain_bi TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'new'
            CHECK (status IN ('new','known','contacted','trial','won','paid',
                              'lost','rejected','expired')),
        pipeline_status TEXT NOT NULL DEFAULT '',
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL CHECK (created_at >= 0),
        updated_at INTEGER NOT NULL CHECK (updated_at >= 0),
        expires_at INTEGER NOT NULL DEFAULT 0 CHECK (expires_at >= 0))""",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_affiliate_leads_open_domain "
    "ON affiliate_leads(domain_bi) "
    "WHERE status IN ('new','contacted','trial','won','paid')",
    "CREATE INDEX IF NOT EXISTS idx_affiliate_leads_affiliate "
    "ON affiliate_leads(affiliate_bi, created_at)",
    "CREATE INDEX IF NOT EXISTS idx_affiliate_leads_status "
    "ON affiliate_leads(status, expires_at)",
    """CREATE TABLE IF NOT EXISTS plugin_race_entrants (
        account_bi TEXT PRIMARY KEY,
        username TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'pending'
            CHECK (status IN ('pending','qualified','disqualified')),
        qualified_at INTEGER NOT NULL DEFAULT 0 CHECK (qualified_at >= 0),
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL CHECK (created_at >= 0),
        updated_at INTEGER NOT NULL CHECK (updated_at >= 0))""",
    "CREATE INDEX IF NOT EXISTS idx_plugin_race_entrants_status "
    "ON plugin_race_entrants(status, qualified_at)",
    """CREATE TABLE IF NOT EXISTS plugin_race_posts (
        post_bi TEXT PRIMARY KEY,
        account_bi TEXT NOT NULL,
        network TEXT NOT NULL CHECK (network IN ('x','reddit','mastodon')),
        created_at INTEGER NOT NULL CHECK (created_at >= 0))""",
    "CREATE INDEX IF NOT EXISTS idx_plugin_race_posts_account "
    "ON plugin_race_posts(account_bi)",
    """CREATE TABLE IF NOT EXISTS plugin_race_plugins (
        market TEXT NOT NULL,
        package_id TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        username TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'review'
            CHECK (status IN ('review','accepted','rejected')),
        data TEXT NOT NULL,
        award_rank INTEGER NOT NULL DEFAULT 0
            CHECK (award_rank >= 0 AND award_rank <= 10),
        paid_tx TEXT NOT NULL DEFAULT '',
        created_at INTEGER NOT NULL CHECK (created_at >= 0),
        updated_at INTEGER NOT NULL CHECK (updated_at >= 0),
        PRIMARY KEY (market, package_id))""",
    "CREATE INDEX IF NOT EXISTS idx_plugin_race_plugins_account "
    "ON plugin_race_plugins(account_bi, created_at)",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_plugin_race_plugins_rank "
    "ON plugin_race_plugins(award_rank) WHERE award_rank>0",
    # Organization-owner contributor reward estimates (migration 0145).
    # These are administrative records mirrored from issue/PR metadata, not
    # claimant-submitted jobs and not balances held by ForkMesh. Payment is a
    # direct transfer signed by the organization owner's local Qt wallet; the
    # exact finalized Solana signature is attached here afterwards.
    """CREATE TABLE IF NOT EXISTS contributor_reward_estimates (
        reward_id TEXT PRIMARY KEY,
        recipient_bi TEXT NOT NULL,
        recipient_name TEXT NOT NULL,
        repo_owner TEXT NOT NULL,
        repo_name TEXT NOT NULL,
        source_kind TEXT NOT NULL
            CHECK (source_kind IN ('issue','pull')),
        subject_number INTEGER NOT NULL CHECK (subject_number > 0),
        subject_title TEXT NOT NULL DEFAULT '',
        subject_summary TEXT NOT NULL DEFAULT ''
            CHECK (length(subject_summary) <= 360),
        amount_lamports INTEGER NOT NULL
            CHECK ((source_kind='issue' AND
                    amount_lamports BETWEEN 1000000 AND 5000000) OR
                   (source_kind='pull' AND
                    amount_lamports BETWEEN 1000000 AND 25000000)),
        condition TEXT NOT NULL
            CHECK (condition IN ('accepted_report','merged')),
        status TEXT NOT NULL
            CHECK (status IN ('pending_merge','pending_wallet','ready',
                              'submitted','paid','cancelled')),
        created_at INTEGER NOT NULL CHECK (created_at >= 0),
        updated_at INTEGER NOT NULL CHECK (updated_at >= 0),
        submitted_at INTEGER NOT NULL DEFAULT 0,
        paid_at INTEGER NOT NULL DEFAULT 0,
        payment_usd_micros INTEGER NOT NULL DEFAULT 0
            CHECK (payment_usd_micros >= 0),
        payment_sol_usd_micros INTEGER NOT NULL DEFAULT 0
            CHECK (payment_sol_usd_micros >= 0),
        payment_valued_at INTEGER NOT NULL DEFAULT 0
            CHECK (payment_valued_at >= 0),
        source_address TEXT NOT NULL DEFAULT '',
        destination_address TEXT NOT NULL DEFAULT '',
        tx_signature TEXT NOT NULL DEFAULT '')""",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_contributor_reward_subject "
    "ON contributor_reward_estimates(repo_owner,repo_name,source_kind,subject_number)",
    "CREATE INDEX IF NOT EXISTS idx_contributor_reward_status "
    "ON contributor_reward_estimates(status,created_at)",
    "CREATE INDEX IF NOT EXISTS idx_contributor_reward_recipient "
    "ON contributor_reward_estimates(recipient_bi,status)",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_contributor_reward_tx "
    "ON contributor_reward_estimates(tx_signature) WHERE tx_signature<>''",
    # One "publish a payout address" nudge per contributor, whatever they are
    # owed (migration 0149). Keyed on the person rather than the reward: a
    # contributor can be owed a dozen unpayable rewards at once, and a
    # per-reward window would let one Ledger page mail them a dozen times about
    # the same missing address.
    """CREATE TABLE IF NOT EXISTS reward_wallet_reminders (
        recipient_bi TEXT PRIMARY KEY,
        recipient_name TEXT NOT NULL,
        reward_id TEXT NOT NULL,
        requested_by TEXT NOT NULL,
        sent_at INTEGER NOT NULL CHECK (sent_at >= 0),
        send_count INTEGER NOT NULL DEFAULT 1)""",
    # Organization plugin installs (migration 0157). Market plugins own their
    # data tables: installing a plugin for an organization creates that
    # plugin's schema (see each plugin module's DATA_SCHEMA_UP), and removing
    # its last install drops it again, so only this registry row is core
    # schema and an uninstalled plugin leaves no trace in the database.
    """CREATE TABLE IF NOT EXISTS org_plugin_installs (
        org_bi TEXT NOT NULL,
        plugin TEXT NOT NULL,
        org_name TEXT NOT NULL DEFAULT '',
        installed_by_bi TEXT NOT NULL DEFAULT '',
        installed_by_name TEXT NOT NULL DEFAULT '',
        installed_at INTEGER NOT NULL DEFAULT 0 CHECK (installed_at >= 0),
        PRIMARY KEY (org_bi, plugin))""",
    "CREATE INDEX IF NOT EXISTS idx_org_plugin_installs_plugin "
    "ON org_plugin_installs(plugin, org_bi)",
    # Personal and organization calendars (migration 0130). Descriptive event
    # copy and reminder choices are encrypted in data; date indexes and ACL
    # blind indexes stay queryable for authorized range reads and cron.
    """CREATE TABLE IF NOT EXISTS calendar_events (
        event_id TEXT PRIMARY KEY, owner_bi TEXT NOT NULL,
        owner_name TEXT NOT NULL, org_bi TEXT NOT NULL DEFAULT '',
        org_name TEXT NOT NULL DEFAULT '', start_at INTEGER NOT NULL,
        end_at INTEGER NOT NULL, all_day INTEGER NOT NULL DEFAULT 0
            CHECK (all_day IN (0,1)),
        timezone TEXT NOT NULL DEFAULT 'UTC',
        recurring INTEGER NOT NULL DEFAULT 0 CHECK (recurring IN (0,1)),
        created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL,
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_calendar_events_owner_range "
    "ON calendar_events(owner_bi, start_at, end_at)",
    "CREATE INDEX IF NOT EXISTS idx_calendar_events_org_range "
    "ON calendar_events(org_bi, start_at, end_at)",
    "CREATE INDEX IF NOT EXISTS idx_calendar_events_reminders "
    "ON calendar_events(recurring, start_at)",
    """CREATE TABLE IF NOT EXISTS calendar_attendees (
        event_id TEXT NOT NULL REFERENCES calendar_events(event_id)
            ON DELETE CASCADE,
        attendee_bi TEXT NOT NULL, attendee_name TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'pending'
            CHECK (status IN ('pending','accepted','declined','tentative')),
        created_at INTEGER NOT NULL, responded_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY(event_id, attendee_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_calendar_attendees_account "
    "ON calendar_attendees(attendee_bi, event_id)",
    """CREATE TABLE IF NOT EXISTS calendar_reminder_deliveries (
        event_id TEXT NOT NULL REFERENCES calendar_events(event_id)
            ON DELETE CASCADE,
        occurrence_at INTEGER NOT NULL, recipient_bi TEXT NOT NULL,
        offset_minutes INTEGER NOT NULL,
        delivered_at INTEGER NOT NULL,
         PRIMARY KEY(event_id, occurrence_at, recipient_bi, offset_minutes))""",
    # Acquisition attribution (migration 0174). One row per anonymous browser
    # session keyed by the opaque random id in the fm_attr cookie. UTM values
    # are sanitised and allowlisted server-side (see attribution.py); the
    # browser never writes them. No IP address is stored here: the rate-based
    # bot rule uses attribution_ip_hourly, whose daily-salted digests live 24h.
    # user_id is the account's users.user_bi, set once at signup so reporting
    # joins through the session instead of copying UTMs onto the account.
    """CREATE TABLE IF NOT EXISTS attribution_sessions (
        id TEXT PRIMARY KEY,
        first_seen_at INTEGER NOT NULL CHECK (first_seen_at > 0),
        last_seen_at INTEGER NOT NULL CHECK (last_seen_at > 0),
        first_utm_source TEXT, first_utm_medium TEXT, first_utm_campaign TEXT,
        first_utm_term TEXT, first_utm_content TEXT,
        last_utm_source TEXT, last_utm_medium TEXT, last_utm_campaign TEXT,
        last_utm_term TEXT, last_utm_content TEXT,
        first_landing_path TEXT NOT NULL DEFAULT '',
        first_referrer_host TEXT NOT NULL DEFAULT '',
        is_bot INTEGER NOT NULL DEFAULT 0 CHECK (is_bot IN (0, 1)),
        bot_reason TEXT NOT NULL DEFAULT '',
        user_id TEXT REFERENCES users(user_bi) ON DELETE SET NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_attribution_sessions_first_seen "
    "ON attribution_sessions(first_seen_at, id)",
    "CREATE INDEX IF NOT EXISTS idx_attribution_sessions_user "
    "ON attribution_sessions(user_id)",
    """CREATE TABLE IF NOT EXISTS attribution_events (
        id TEXT PRIMARY KEY,
        session_id TEXT NOT NULL REFERENCES attribution_sessions(id)
            ON DELETE CASCADE,
        event_name TEXT NOT NULL,
        occurred_at INTEGER NOT NULL CHECK (occurred_at > 0),
        metadata TEXT NOT NULL DEFAULT '{}')""",
    "CREATE INDEX IF NOT EXISTS idx_attribution_events_name_time "
    "ON attribution_events(event_name, occurred_at)",
    "CREATE INDEX IF NOT EXISTS idx_attribution_events_session "
    "ON attribution_events(session_id)",
    "CREATE INDEX IF NOT EXISTS idx_attribution_events_time "
    "ON attribution_events(occurred_at, id)",
    # Sessions per source address per hour, keyed by a digest whose HMAC input
    # includes the UTC day (a rotating daily salt). Rows older than 24 hours
    # are purged by the hourly attribution-maintenance cron phase.
    """CREATE TABLE IF NOT EXISTS attribution_ip_hourly (
        ip_hash TEXT NOT NULL,
        hour_bucket INTEGER NOT NULL,
        sessions INTEGER NOT NULL DEFAULT 0,
        session_ids TEXT NOT NULL DEFAULT '[]',
        PRIMARY KEY (ip_hash, hour_bucket))""",
    # Requests refused before a session existed (known crawlers), counted per
    # UTC day and reason so the transparency page can state what was filtered.
    """CREATE TABLE IF NOT EXISTS attribution_filtered_hits (
        day TEXT NOT NULL,
        reason TEXT NOT NULL,
        hits INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (day, reason))""",
    # Daily snapshot of the cumulative mirror_serve_counters clone totals per
    # repository, taken by cron. Per-day clones are the delta between
    # consecutive snapshots; this deliberately never touches the clone path.
    """CREATE TABLE IF NOT EXISTS attribution_clone_snapshots (
        day TEXT NOT NULL,
        owner TEXT NOT NULL,
        repo TEXT NOT NULL,
        clones_total INTEGER NOT NULL DEFAULT 0,
        captured_at INTEGER NOT NULL,
        PRIMARY KEY (day, owner, repo))""",
    # --- ForkMesh Hackathon (app/src/hackathon.py) ---------------------------
    # A campaign is a judged build contest wrapped in a referral, squad and
    # mission layer. Public routes 404 while status='draft', so the build
    # weeks can deploy freely -- every push to main deploys, and a
    # half-finished campaign must not appear on the marketing origin.
    #
    # The counter columns are denormalised on purpose: the landing page and
    # the milestone meter are the hottest reads in this feature, and D1 is
    # billed by rows read, so neither may run COUNT(*) over participants.
    """CREATE TABLE IF NOT EXISTS hackathons (
        slug TEXT PRIMARY KEY,
        title TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'draft'
            CHECK (status IN ('draft','registration','live','judging','closed')),
        config TEXT NOT NULL DEFAULT '{}',
        starts_at INTEGER NOT NULL DEFAULT 0,
        ends_at INTEGER NOT NULL DEFAULT 0,
        judging_ends_at INTEGER NOT NULL DEFAULT 0,
        participant_count INTEGER NOT NULL DEFAULT 0,
        verified_count INTEGER NOT NULL DEFAULT 0,
        squad_count INTEGER NOT NULL DEFAULT 0,
        submission_count INTEGER NOT NULL DEFAULT 0,
        created_at INTEGER NOT NULL DEFAULT 0,
        updated_at INTEGER NOT NULL DEFAULT 0)""",
    # account_bi is the account blind index; name is the plaintext public
    # username, the same trust level as referral_stats.name and
    # badge_awards.name (it is already shown on the profile and the public
    # boards). referrer_bi is the account whose invite link brought this
    # participant in -- recorded here for squad placement and the growth
    # funnel, while referral *credit* stays entirely in referral_attributions.
    """CREATE TABLE IF NOT EXISTS hackathon_participants (
        hackathon_slug TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        name TEXT NOT NULL,
        referrer_bi TEXT NOT NULL DEFAULT '',
        referrer_name TEXT NOT NULL DEFAULT '',
        squad_id TEXT NOT NULL DEFAULT '',
        track TEXT NOT NULL DEFAULT '',
        points INTEGER NOT NULL DEFAULT 0,
        onboarding_step INTEGER NOT NULL DEFAULT 1,
        share_count INTEGER NOT NULL DEFAULT 0,
        referrals_joined INTEGER NOT NULL DEFAULT 0,
        referrals_qualified INTEGER NOT NULL DEFAULT 0,
        joined_at INTEGER NOT NULL DEFAULT 0,
        verified_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (hackathon_slug, account_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_hackathon_participants_points "
    "ON hackathon_participants(hackathon_slug, points DESC, joined_at)",
    "CREATE INDEX IF NOT EXISTS idx_hackathon_participants_recruiters "
    "ON hackathon_participants(hackathon_slug, referrals_qualified DESC, "
    "points DESC)",
    "CREATE INDEX IF NOT EXISTS idx_hackathon_participants_referrer "
    "ON hackathon_participants(hackathon_slug, referrer_bi)",
    "CREATE INDEX IF NOT EXISTS idx_hackathon_participants_squad "
    "ON hackathon_participants(hackathon_slug, squad_id, points DESC)",
    "CREATE INDEX IF NOT EXISTS idx_hackathon_participants_name "
    "ON hackathon_participants(hackathon_slug, name)",
    # Append-only points ledger. The primary key is the whole anti-double-pay
    # rule: a unique point kind collapses its reference to '' so a second
    # award for the same participant is rejected by the key itself, and a
    # repeatable kind carries the event's own reference (a repo, a pull
    # number, a referred account) so one event pays exactly once however many
    # times its hook fires. participants.points caches the sum.
    """CREATE TABLE IF NOT EXISTS hackathon_points (
        hackathon_slug TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        kind TEXT NOT NULL,
        ref TEXT NOT NULL DEFAULT '',
        points INTEGER NOT NULL DEFAULT 0,
        created_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (hackathon_slug, account_bi, kind, ref))""",
    "CREATE INDEX IF NOT EXISTS idx_hackathon_points_account "
    "ON hackathon_points(hackathon_slug, account_bi, created_at DESC)",
    # A squad is an invite tree, not a chosen team: created the first time a
    # participant's link brings somebody in, and joined by everyone who
    # arrives through that participant afterwards. Distinct from a Team,
    # which is the submission unit; the two words are never interchanged.
    """CREATE TABLE IF NOT EXISTS hackathon_squads (
        squad_id TEXT PRIMARY KEY,
        hackathon_slug TEXT NOT NULL,
        code TEXT NOT NULL,
        owner_bi TEXT NOT NULL,
        owner_name TEXT NOT NULL,
        member_count INTEGER NOT NULL DEFAULT 1,
        points INTEGER NOT NULL DEFAULT 0,
        created_at INTEGER NOT NULL DEFAULT 0)""",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_hackathon_squads_code "
    "ON hackathon_squads(hackathon_slug, code)",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_hackathon_squads_owner "
    "ON hackathon_squads(hackathon_slug, owner_bi)",
    "CREATE INDEX IF NOT EXISTS idx_hackathon_squads_rank "
    "ON hackathon_squads(hackathon_slug, points DESC, member_count DESC)",
    # Share presses: the K-factor numerator and nothing else. Channel and
    # surface only -- no recipient, no message, no URL, no address. The
    # minute bucket is part of the key, so a jammed button records one press
    # per channel per minute instead of inflating the denominator.
    """CREATE TABLE IF NOT EXISTS hackathon_share_events (
        hackathon_slug TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        channel TEXT NOT NULL,
        surface TEXT NOT NULL DEFAULT '',
        minute_bucket INTEGER NOT NULL DEFAULT 0,
        created_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (hackathon_slug, account_bi, channel, minute_bucket))""",
    "CREATE INDEX IF NOT EXISTS idx_hackathon_share_events_time "
    "ON hackathon_share_events(hackathon_slug, created_at DESC)",
    # Community milestones. The ladder itself is configuration; only the
    # unlock timestamp is data, so a threshold can be retuned before it is
    # reached without rewriting history.
    """CREATE TABLE IF NOT EXISTS hackathon_milestones (
        hackathon_slug TEXT NOT NULL,
        key TEXT NOT NULL,
        metric TEXT NOT NULL,
        threshold INTEGER NOT NULL DEFAULT 0,
        label TEXT NOT NULL DEFAULT '',
        description TEXT NOT NULL DEFAULT '',
        reward TEXT NOT NULL DEFAULT '{}',
        unlocked_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (hackathon_slug, key))""",
    # Mission progress. Definitions live in the campaign config; a row here
    # only records how far one participant has come and whether the reward
    # was already handed over, so a reward cannot be granted twice.
    """CREATE TABLE IF NOT EXISTS hackathon_mission_progress (
        hackathon_slug TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        mission_key TEXT NOT NULL,
        progress INTEGER NOT NULL DEFAULT 0,
        completed_at INTEGER NOT NULL DEFAULT 0,
        rewarded_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (hackathon_slug, account_bi, mission_key))""",
    "CREATE INDEX IF NOT EXISTS idx_hackathon_mission_progress_account "
    "ON hackathon_mission_progress(hackathon_slug, account_bi)",
    # Challenge links: "Alex has 1,240 points, can you beat that?". The target
    # is the challenger's points at the moment they sent it -- the number the
    # page showed -- so a challenge is decided once and never flips back when
    # the challenger keeps building. One link may be accepted by many people,
    # because a challenge posted to a group chat is the ordinary case.
    """CREATE TABLE IF NOT EXISTS hackathon_challenges (
        token TEXT PRIMARY KEY,
        hackathon_slug TEXT NOT NULL,
        challenger_bi TEXT NOT NULL,
        challenger_name TEXT NOT NULL,
        target_points INTEGER NOT NULL DEFAULT 0,
        accepted_count INTEGER NOT NULL DEFAULT 0,
        beaten_count INTEGER NOT NULL DEFAULT 0,
        created_at INTEGER NOT NULL DEFAULT 0,
        expires_at INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_hackathon_challenges_challenger "
    "ON hackathon_challenges(hackathon_slug, challenger_bi, created_at DESC)",
    """CREATE TABLE IF NOT EXISTS hackathon_challenge_entries (
        token TEXT NOT NULL,
        hackathon_slug TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        name TEXT NOT NULL,
        accepted_at INTEGER NOT NULL DEFAULT 0,
        beaten_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (token, account_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_hackathon_challenge_entries_account "
    "ON hackathon_challenge_entries(hackathon_slug, account_bi, beaten_at)",
    # The reward ledger. Every membership month, badge and bonus a campaign
    # hands out is one row here first, keyed so a reward can be granted once
    # however many times the event behind it is observed. The membership
    # effect is then written to role_grants under a promotion scope that no
    # billing provider's sync touches: a provider revokes the platform-scope
    # rows on every webhook, and a prize must not vanish the next time
    # somebody's subscription renews.
    """CREATE TABLE IF NOT EXISTS hackathon_reward_grants (
        hackathon_slug TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        reward_key TEXT NOT NULL,
        kind TEXT NOT NULL
            CHECK (kind IN ('pro_months','supporter_months','badge')),
        months INTEGER NOT NULL DEFAULT 0,
        badge TEXT NOT NULL DEFAULT '',
        granted_at INTEGER NOT NULL DEFAULT 0,
        expires_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (hackathon_slug, account_bi, reward_key, kind))""",
    "CREATE INDEX IF NOT EXISTS idx_hackathon_reward_grants_account "
    "ON hackathon_reward_grants(hackathon_slug, account_bi, granted_at DESC)",
    # A Team is the submission unit -- at most five people, one project per
    # track -- and is a different thing from a Squad, which is everybody one
    # participant brought in. A participant belongs to at most one team per
    # campaign, which the member table's key enforces.
    """CREATE TABLE IF NOT EXISTS hackathon_teams (
        team_id TEXT PRIMARY KEY,
        hackathon_slug TEXT NOT NULL,
        name TEXT NOT NULL,
        captain_bi TEXT NOT NULL,
        captain_name TEXT NOT NULL,
        invite_code TEXT NOT NULL,
        member_count INTEGER NOT NULL DEFAULT 1,
        created_at INTEGER NOT NULL DEFAULT 0)""",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_hackathon_teams_code "
    "ON hackathon_teams(hackathon_slug, invite_code)",
    """CREATE TABLE IF NOT EXISTS hackathon_team_members (
        hackathon_slug TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        team_id TEXT NOT NULL,
        name TEXT NOT NULL,
        joined_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (hackathon_slug, account_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_hackathon_team_members_team "
    "ON hackathon_team_members(team_id)",
    # Submissions. Validity is checked against the catalog when a project is
    # submitted, and anything the check cannot confirm is marked for a human
    # rather than rejected: a false rejection costs a builder their entry,
    # while a false flag costs a judge one look. The score columns cache the
    # sum and count of judge totals so ranking a track is one ordered read.
    """CREATE TABLE IF NOT EXISTS hackathon_submissions (
        submission_id TEXT PRIMARY KEY,
        hackathon_slug TEXT NOT NULL,
        team_id TEXT NOT NULL,
        track TEXT NOT NULL,
        repo_owner TEXT NOT NULL,
        repo_name TEXT NOT NULL,
        title TEXT NOT NULL,
        pitch TEXT NOT NULL DEFAULT '',
        demo_url TEXT NOT NULL DEFAULT '',
        video_url TEXT NOT NULL DEFAULT '',
        status TEXT NOT NULL DEFAULT 'needs_review'
            CHECK (status IN ('valid','needs_review','invalid','withdrawn')),
        review_flags TEXT NOT NULL DEFAULT '[]',
        placement TEXT NOT NULL DEFAULT ''
            CHECK (placement IN ('','first','second')),
        score_total INTEGER NOT NULL DEFAULT 0,
        score_count INTEGER NOT NULL DEFAULT 0,
        vote_count INTEGER NOT NULL DEFAULT 0,
        submitted_at INTEGER NOT NULL DEFAULT 0,
        updated_at INTEGER NOT NULL DEFAULT 0)""",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_hackathon_submissions_team_track "
    "ON hackathon_submissions(hackathon_slug, team_id, track)",
    "CREATE INDEX IF NOT EXISTS idx_hackathon_submissions_gallery "
    "ON hackathon_submissions(hackathon_slug, status, track, submitted_at)",
    # Judges are named per campaign by an administrator. Each judge scores a
    # submission once (and may revise it); a judge who is also on the
    # submitting team is refused at write time.
    """CREATE TABLE IF NOT EXISTS hackathon_judges (
        hackathon_slug TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        name TEXT NOT NULL,
        added_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (hackathon_slug, account_bi))""",
    """CREATE TABLE IF NOT EXISTS hackathon_scores (
        submission_id TEXT NOT NULL,
        judge_bi TEXT NOT NULL,
        hackathon_slug TEXT NOT NULL,
        fit INTEGER NOT NULL CHECK (fit BETWEEN 1 AND 5),
        working INTEGER NOT NULL CHECK (working BETWEEN 1 AND 5),
        portability INTEGER NOT NULL CHECK (portability BETWEEN 1 AND 5),
        craft INTEGER NOT NULL CHECK (craft BETWEEN 1 AND 5),
        total INTEGER NOT NULL DEFAULT 0,
        scored_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (submission_id, judge_bi))""",
    # Community Choice: one vote per verified participant per track, which
    # they may move. Opens only once the community milestone that announces
    # it has been reached.
    """CREATE TABLE IF NOT EXISTS hackathon_votes (
        hackathon_slug TEXT NOT NULL,
        voter_bi TEXT NOT NULL,
        track TEXT NOT NULL,
        submission_id TEXT NOT NULL,
        voted_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (hackathon_slug, voter_bi, track))""",
    "CREATE INDEX IF NOT EXISTS idx_hackathon_votes_submission "
    "ON hackathon_votes(submission_id)",
    # The street team. Recruiting ambassadors is human work; the product
    # records who applied and who an administrator approved, and nothing
    # here contacts anybody.
    """CREATE TABLE IF NOT EXISTS hackathon_ambassadors (
        hackathon_slug TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        name TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'pending'
            CHECK (status IN ('pending','approved','rejected')),
        pitch TEXT NOT NULL DEFAULT '',
        applied_at INTEGER NOT NULL DEFAULT 0,
        reviewed_at INTEGER NOT NULL DEFAULT 0,
        reviewed_by TEXT NOT NULL DEFAULT '',
        PRIMARY KEY (hackathon_slug, account_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_hackathon_ambassadors_status "
    "ON hackathon_ambassadors(hackathon_slug, status, applied_at)",
    # Phase 5 runner tier (runner_leases.py, migration 0193). A runner is a
    # machine that only executes agent jobs: it holds no repositories and is
    # authorized per job by router-signed grants for one repository and one
    # refs/heads/agent/<session> branch.
    """CREATE TABLE IF NOT EXISTS runner_nodes (
        name TEXT PRIMARY KEY,
        public_key TEXT NOT NULL,
        registered_by TEXT NOT NULL,
        scope TEXT NOT NULL DEFAULT 'personal'
            CHECK (scope IN ('pool','personal')),
        allowed_owners TEXT NOT NULL DEFAULT '[]',
        status TEXT NOT NULL DEFAULT 'active',
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        last_seen_at INTEGER NOT NULL DEFAULT 0,
        actions_trusted INTEGER NOT NULL DEFAULT 0)""",
    """CREATE TABLE IF NOT EXISTS runner_job_leases (
        job_id INTEGER PRIMARY KEY,
        runner TEXT NOT NULL,
        session_id TEXT NOT NULL,
        lease_id TEXT NOT NULL,
        grant_id TEXT NOT NULL,
        target_node TEXT NOT NULL,
        repo TEXT NOT NULL,
        ref TEXT NOT NULL,
        expires_at INTEGER NOT NULL,
        created_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_runner_job_leases_session "
    "ON runner_job_leases(session_id, created_at)",
    "CREATE INDEX IF NOT EXISTS idx_runner_job_leases_runner "
    "ON runner_job_leases(runner, expires_at)",
    """CREATE TABLE IF NOT EXISTS runner_request_nonces (
        runner TEXT NOT NULL,
        request_id TEXT NOT NULL,
        expires_at INTEGER NOT NULL,
        PRIMARY KEY (runner, request_id))""",
    """CREATE TABLE IF NOT EXISTS repo_action_jobs (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        owner TEXT NOT NULL,
        repo TEXT NOT NULL,
        branch TEXT NOT NULL,
        commit_oid TEXT NOT NULL DEFAULT '',
        target_node TEXT NOT NULL DEFAULT '',
        pusher TEXT NOT NULL DEFAULT '',
        status TEXT NOT NULL DEFAULT 'queued',
        lease_id TEXT NOT NULL DEFAULT '',
        runner TEXT NOT NULL DEFAULT '',
        attempts INTEGER NOT NULL DEFAULT 0,
        expires_at INTEGER NOT NULL DEFAULT 0,
        note TEXT NOT NULL DEFAULT '',
        data TEXT NOT NULL DEFAULT '',
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        event TEXT NOT NULL DEFAULT 'push',
        workflow TEXT NOT NULL DEFAULT '',
        ref TEXT NOT NULL DEFAULT '',
        actor TEXT NOT NULL DEFAULT '',
        before_oid TEXT NOT NULL DEFAULT '',
        payload TEXT NOT NULL DEFAULT '',
        token_hash TEXT NOT NULL DEFAULT '',
        trusted INTEGER NOT NULL DEFAULT 0,
        pushed_commit TEXT NOT NULL DEFAULT '')""",
    "CREATE INDEX IF NOT EXISTS idx_repo_action_jobs_repo "
    "ON repo_action_jobs(owner, repo, id)",
    "CREATE INDEX IF NOT EXISTS idx_repo_action_jobs_status "
    "ON repo_action_jobs(status, id)",
    "CREATE INDEX IF NOT EXISTS idx_repo_action_jobs_token "
    "ON repo_action_jobs(token_hash) WHERE token_hash != ''",
    """CREATE TABLE IF NOT EXISTS repo_action_workflows (
        owner TEXT NOT NULL,
        repo TEXT NOT NULL,
        path TEXT NOT NULL,
        name TEXT NOT NULL DEFAULT '',
        events TEXT NOT NULL DEFAULT '[]',
        schedules TEXT NOT NULL DEFAULT '[]',
        inputs TEXT NOT NULL DEFAULT '{}',
        commit_oid TEXT NOT NULL DEFAULT '',
        last_slot INTEGER NOT NULL DEFAULT 0,
        updated_at INTEGER NOT NULL,
        PRIMARY KEY (owner, repo, path))""",
    """CREATE TABLE IF NOT EXISTS repo_action_secrets (
        owner TEXT NOT NULL,
        repo TEXT NOT NULL DEFAULT '',
        name TEXT NOT NULL,
        value TEXT NOT NULL,
        updated_by TEXT NOT NULL DEFAULT '',
        updated_at INTEGER NOT NULL,
        PRIMARY KEY (owner, repo, name))""",
    """CREATE TABLE IF NOT EXISTS repo_action_logs (
        job_id INTEGER NOT NULL,
        run_index INTEGER NOT NULL,
        data TEXT NOT NULL,
        updated_at INTEGER NOT NULL,
        PRIMARY KEY (job_id, run_index))""",
    """CREATE TABLE IF NOT EXISTS deploy_approvals (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        request_id TEXT NOT NULL UNIQUE,
        kind TEXT NOT NULL,
        owner TEXT NOT NULL,
        repo TEXT NOT NULL,
        target TEXT NOT NULL,
        expected_base TEXT NOT NULL DEFAULT '',
        expected_head TEXT NOT NULL DEFAULT '',
        base_before TEXT NOT NULL,
        head TEXT NOT NULL,
        base_after TEXT NOT NULL,
        public_key TEXT NOT NULL,
        signature TEXT NOT NULL,
        signed_at INTEGER NOT NULL,
        actor TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'pending',
        message TEXT NOT NULL DEFAULT '',
        announced INTEGER NOT NULL DEFAULT 0,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_deploy_approvals_status "
    "ON deploy_approvals(status, id)",
    """CREATE TABLE IF NOT EXISTS deploy_approval_nodes (
        approval_id INTEGER NOT NULL,
        node TEXT NOT NULL,
        status TEXT NOT NULL,
        message TEXT NOT NULL DEFAULT '',
        updated_at INTEGER NOT NULL,
        PRIMARY KEY (approval_id, node))""",
    # Web sign-in of a runner's provider CLI (runner_logins.py, migration
    # 0195): the relayed login terminal's session and its bytes in flight.
    """CREATE TABLE IF NOT EXISTS runner_login_sessions (
        id TEXT PRIMARY KEY,
        runner TEXT NOT NULL,
        provider TEXT NOT NULL,
        state TEXT NOT NULL DEFAULT 'pending'
            CHECK (state IN ('pending','running','succeeded','failed',
                             'expired','cancelled')),
        created_by TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        expires_at INTEGER NOT NULL,
        ended_at INTEGER NOT NULL DEFAULT 0,
        output_seq INTEGER NOT NULL DEFAULT 0,
        input_seq INTEGER NOT NULL DEFAULT 0,
        exit_code INTEGER,
        message TEXT NOT NULL DEFAULT '',
        kind TEXT NOT NULL DEFAULT 'login'
            CHECK (kind IN ('login','terminal')),
        term_cols INTEGER NOT NULL DEFAULT 0,
        term_rows INTEGER NOT NULL DEFAULT 0)""",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_runner_login_sessions_open "
    "ON runner_login_sessions(runner, provider, kind) "
    "WHERE state IN ('pending','running')",
    "CREATE INDEX IF NOT EXISTS idx_runner_login_sessions_runner "
    "ON runner_login_sessions(runner, state, expires_at)",
    """CREATE TABLE IF NOT EXISTS runner_login_chunks (
        session_id TEXT NOT NULL,
        direction TEXT NOT NULL CHECK (direction IN ('out','in')),
        seq INTEGER NOT NULL,
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        PRIMARY KEY (session_id, direction, seq))""",
    """CREATE TABLE IF NOT EXISTS runner_availability (
        runner TEXT PRIMARY KEY,
        data TEXT NOT NULL DEFAULT '{}',
        updated_at INTEGER NOT NULL,
        features TEXT NOT NULL DEFAULT '')""",
    """CREATE TABLE IF NOT EXISTS runner_account_requests (
        id TEXT PRIMARY KEY,
        runner TEXT NOT NULL,
        provider TEXT NOT NULL,
        action TEXT NOT NULL CHECK (action IN ('activate','remove')),
        account TEXT NOT NULL,
        state TEXT NOT NULL DEFAULT 'pending'
            CHECK (state IN ('pending','done','failed')),
        created_by TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        message TEXT NOT NULL DEFAULT '')""",
    "CREATE INDEX IF NOT EXISTS idx_runner_account_requests_runner "
    "ON runner_account_requests(runner, state, created_at)",
    """CREATE TABLE IF NOT EXISTS runner_setup_tokens (
        id TEXT PRIMARY KEY,
        token_hash TEXT NOT NULL UNIQUE,
        owner TEXT NOT NULL,
        runner TEXT NOT NULL,
        via TEXT NOT NULL DEFAULT 'command',
        state TEXT NOT NULL DEFAULT 'pending'
            CHECK (state IN ('pending','used','revoked')),
        created_at INTEGER NOT NULL,
        expires_at INTEGER NOT NULL,
        used_at INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_runner_setup_tokens_owner "
    "ON runner_setup_tokens(owner, created_at)",
    """CREATE TABLE IF NOT EXISTS runner_ssh_setups (
        id TEXT PRIMARY KEY,
        owner TEXT NOT NULL,
        host TEXT NOT NULL,
        address TEXT NOT NULL,
        port INTEGER NOT NULL,
        host_keys TEXT NOT NULL DEFAULT '[]',
        fingerprint TEXT NOT NULL DEFAULT '',
        runner TEXT NOT NULL DEFAULT '',
        token_id TEXT NOT NULL DEFAULT '',
        state TEXT NOT NULL DEFAULT 'probed'
            CHECK (state IN ('probed','running','succeeded','failed')),
        message TEXT NOT NULL DEFAULT '',
        log TEXT NOT NULL DEFAULT '',
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        expires_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_runner_ssh_setups_owner "
    "ON runner_ssh_setups(owner, created_at)",
    """CREATE TABLE IF NOT EXISTS instance_launches (
        id TEXT PRIMARY KEY,
        client_key TEXT NOT NULL,
        account TEXT NOT NULL DEFAULT '',
        project TEXT NOT NULL,
        domain TEXT NOT NULL,
        owner TEXT NOT NULL,
        owner_type TEXT NOT NULL DEFAULT 'org'
            CHECK (owner_type IN ('org','user')),
        admin TEXT NOT NULL,
        address TEXT NOT NULL,
        port INTEGER NOT NULL,
        host_key TEXT NOT NULL DEFAULT '{}',
        state TEXT NOT NULL DEFAULT 'running'
            CHECK (state IN ('running','succeeded','failed')),
        steps TEXT NOT NULL DEFAULT '[]',
        message TEXT NOT NULL DEFAULT '',
        log TEXT NOT NULL DEFAULT '',
        url TEXT NOT NULL DEFAULT '',
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_instance_launches_client "
    "ON instance_launches(client_key, created_at)",
    "CREATE INDEX IF NOT EXISTS idx_instance_launches_state "
    "ON instance_launches(state, updated_at)",
    """CREATE TABLE IF NOT EXISTS ecosystem_projects (
        slug TEXT PRIMARY KEY,
        category TEXT NOT NULL,
        published INTEGER NOT NULL DEFAULT 0 CHECK (published IN (0, 1)),
        featured INTEGER NOT NULL DEFAULT 0 CHECK (featured IN (0, 1)),
        position INTEGER NOT NULL DEFAULT 0,
        data TEXT NOT NULL DEFAULT '{}',
        internal TEXT NOT NULL DEFAULT '{}',
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        updated_by TEXT NOT NULL DEFAULT '')""",
    "CREATE INDEX IF NOT EXISTS idx_ecosystem_projects_published "
    "ON ecosystem_projects(published, position)",
    """CREATE TABLE IF NOT EXISTS mail_sends (
        send_id TEXT PRIMARY KEY,
        message_id TEXT NOT NULL,
        kind TEXT NOT NULL DEFAULT '',
        account_bi TEXT NOT NULL DEFAULT '',
        recipient_bi TEXT NOT NULL DEFAULT '',
        sent_at INTEGER NOT NULL,
        status TEXT NOT NULL DEFAULT '',
        status_at INTEGER NOT NULL DEFAULT 0,
        dsn TEXT NOT NULL DEFAULT '',
        cause TEXT NOT NULL DEFAULT '')""",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_mail_sends_message "
    "ON mail_sends(message_id)",
    "CREATE INDEX IF NOT EXISTS idx_mail_sends_sent ON mail_sends(sent_at)",
    """CREATE TABLE IF NOT EXISTS mail_suppressions (
        recipient_bi TEXT PRIMARY KEY,
        reason TEXT NOT NULL,
        created_at INTEGER NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS mail_log_node_notes (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        created_at INTEGER NOT NULL,
        node TEXT NOT NULL DEFAULT '' CHECK (length(node) <= 64),
        data TEXT NOT NULL CHECK (length(data) <= 4096))""",
    """CREATE TABLE IF NOT EXISTS code_challenges (
        challenge_id TEXT PRIMARY KEY,
        owner_bi TEXT NOT NULL,
        owner_name TEXT NOT NULL,
        starts_on TEXT NOT NULL,
        ends_on TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        data TEXT NOT NULL DEFAULT '{}')""",
    "CREATE INDEX IF NOT EXISTS idx_code_challenges_owner "
    "ON code_challenges(owner_bi, ends_on)",
    """CREATE TABLE IF NOT EXISTS code_challenge_members (
        challenge_id TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        name TEXT NOT NULL,
        joined_at INTEGER NOT NULL,
        PRIMARY KEY (challenge_id, account_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_code_challenge_members_account "
    "ON code_challenge_members(account_bi)",
    """CREATE TABLE IF NOT EXISTS agent_stream_state (
        session_id TEXT PRIMARY KEY,
        job_id INTEGER NOT NULL DEFAULT 0,
        job_seq INTEGER NOT NULL DEFAULT 0,
        seq INTEGER NOT NULL DEFAULT 0,
        watched_until INTEGER NOT NULL DEFAULT 0,
        ended_at INTEGER NOT NULL DEFAULT 0,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_agent_stream_state_sweep "
    "ON agent_stream_state(ended_at, updated_at)",
    """CREATE TABLE IF NOT EXISTS agent_stream_chunks (
        session_id TEXT NOT NULL,
        seq INTEGER NOT NULL,
        job_id INTEGER NOT NULL,
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        PRIMARY KEY (session_id, seq))""",
    """CREATE TABLE IF NOT EXISTS agent_stream_watchers (
        session_id TEXT NOT NULL,
        account TEXT NOT NULL,
        until INTEGER NOT NULL,
        PRIMARY KEY (session_id, account))""",
    "CREATE INDEX IF NOT EXISTS idx_agent_stream_watchers_until "
    "ON agent_stream_watchers(until)",
    # Single-row bookkeeping for ensure_schema's fast path: the fingerprint of
    # the DDL that has already been applied to this database. A cold isolate
    # reads this one row instead of replaying all ~90 statements above — the
    # full replay only happens when the fingerprint (i.e. this file) changed.
    """CREATE TABLE IF NOT EXISTS schema_meta (
        k TEXT PRIMARY KEY, v TEXT NOT NULL)""",
    "CREATE TABLE IF NOT EXISTS org_office_visits ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT, org_bi TEXT NOT NULL, "
    "member_bi TEXT NOT NULL, data TEXT NOT NULL, "
    "started_at INTEGER NOT NULL, ended_at INTEGER, "
    "seen_at INTEGER NOT NULL)",
    "CREATE INDEX IF NOT EXISTS idx_org_office_visits_started "
    "ON org_office_visits(org_bi, started_at)",
    "CREATE TABLE IF NOT EXISTS org_office_channels ("
    "org_bi TEXT NOT NULL, room TEXT NOT NULL, "
    "channel_id TEXT NOT NULL UNIQUE, created_at INTEGER NOT NULL, "
    "PRIMARY KEY (org_bi, room))",
    "CREATE TABLE IF NOT EXISTS organization_agent_presence ("
    "org_bi TEXT NOT NULL, account_bi TEXT NOT NULL, "
    "session_id TEXT NOT NULL, data TEXT NOT NULL, "
    "live INTEGER NOT NULL DEFAULT 0 CHECK (live IN (0, 1)), "
    "updated_at INTEGER NOT NULL, "
    "PRIMARY KEY (org_bi, account_bi, session_id))",
    "CREATE INDEX IF NOT EXISTS idx_organization_agent_presence_org "
    "ON organization_agent_presence(org_bi, updated_at)",
    "CREATE TABLE IF NOT EXISTS repo_views ("
    "repo_bi TEXT NOT NULL, viewer_key TEXT NOT NULL, "
    "first_at INTEGER NOT NULL CHECK (first_at >= 0), "
    "last_at INTEGER NOT NULL CHECK (last_at >= 0), "
    "counted_at INTEGER NOT NULL DEFAULT 0 CHECK (counted_at >= 0), "
    "hits INTEGER NOT NULL DEFAULT 1 CHECK (hits >= 1), "
    "PRIMARY KEY (repo_bi, viewer_key)) WITHOUT ROWID",
    "CREATE TABLE IF NOT EXISTS repo_view_totals ("
    "repo_bi TEXT PRIMARY KEY, "
    "views INTEGER NOT NULL DEFAULT 0 CHECK (views >= 0), "
    "viewers INTEGER NOT NULL DEFAULT 0 CHECK (viewers >= 0), "
    "last_viewed_at INTEGER NOT NULL DEFAULT 0 CHECK (last_viewed_at >= 0))",
    "CREATE TABLE IF NOT EXISTS account_activity_days ("
    "account_bi TEXT NOT NULL, day TEXT NOT NULL, device_key TEXT NOT NULL, "
    "kind TEXT NOT NULL DEFAULT '', counts TEXT NOT NULL, "
    "updated_at INTEGER NOT NULL CHECK (updated_at >= 0), "
    "PRIMARY KEY (account_bi, day, device_key)) WITHOUT ROWID",
    "CREATE TABLE IF NOT EXISTS not_found_paths ("
    "site TEXT NOT NULL, method TEXT NOT NULL, path TEXT NOT NULL, "
    "hits INTEGER NOT NULL DEFAULT 1 CHECK (hits >= 1), "
    "first_seen INTEGER NOT NULL CHECK (first_seen >= 0), "
    "last_seen INTEGER NOT NULL CHECK (last_seen >= 0), "
    "referrer TEXT NOT NULL DEFAULT '', "
    "alerted_at INTEGER NOT NULL DEFAULT 0 CHECK (alerted_at >= 0), "
    "last_client TEXT NOT NULL DEFAULT '', "
    "PRIMARY KEY (site, method, path))",
    "CREATE INDEX IF NOT EXISTS idx_not_found_paths_last_seen "
    "ON not_found_paths(last_seen)",
    "CREATE INDEX IF NOT EXISTS idx_not_found_paths_hits "
    "ON not_found_paths(hits, last_seen)",
    "CREATE INDEX IF NOT EXISTS idx_not_found_paths_alerted "
    "ON not_found_paths(alerted_at) WHERE alerted_at>0",
    "CREATE TABLE IF NOT EXISTS not_found_hours ("
    "site TEXT NOT NULL, method TEXT NOT NULL, path TEXT NOT NULL, "
    "hour INTEGER NOT NULL CHECK (hour >= 0), "
    "hits INTEGER NOT NULL DEFAULT 1 CHECK (hits >= 1), "
    "PRIMARY KEY (site, method, path, hour)) WITHOUT ROWID",
    "CREATE INDEX IF NOT EXISTS idx_not_found_hours_hour "
    "ON not_found_hours(hour)",
    "CREATE TABLE IF NOT EXISTS not_found_ignores ("
    "site TEXT NOT NULL, path TEXT NOT NULL, "
    "prefix INTEGER NOT NULL DEFAULT 0 CHECK (prefix IN (0, 1)), "
    "created_at INTEGER NOT NULL CHECK (created_at >= 0), "
    "created_by TEXT NOT NULL DEFAULT '', "
    "hits INTEGER NOT NULL DEFAULT 0 CHECK (hits >= 0), "
    "last_seen INTEGER NOT NULL DEFAULT 0 CHECK (last_seen >= 0), "
    "PRIMARY KEY (site, path))",
    "CREATE TABLE IF NOT EXISTS probe_visitors (visitor_bi TEXT "
    "PRIMARY KEY, window_start INTEGER NOT NULL CHECK (window_start >= "
    "0), score INTEGER NOT NULL DEFAULT 0 CHECK (score >= 0), paths "
    "TEXT NOT NULL DEFAULT '[]', last_seen INTEGER NOT NULL CHECK "
    "(last_seen >= 0), bans INTEGER NOT NULL DEFAULT 0 CHECK (bans >= "
    "0), last_ban_at INTEGER NOT NULL DEFAULT 0 CHECK (last_ban_at >= "
    "0), banned_until INTEGER NOT NULL DEFAULT 0 CHECK (banned_until "
    ">= 0))",
    "CREATE INDEX IF NOT EXISTS idx_probe_visitors_last_seen ON "
    "probe_visitors(last_seen)",
    "CREATE TABLE IF NOT EXISTS probe_bans (rule_id TEXT PRIMARY KEY, "
    "visitor_bi TEXT NOT NULL, client TEXT NOT NULL DEFAULT '', kinds "
    "TEXT NOT NULL DEFAULT '', paths TEXT NOT NULL DEFAULT '[]', score "
    "INTEGER NOT NULL DEFAULT 0 CHECK (score >= 0), enforced INTEGER "
    "NOT NULL DEFAULT 1 CHECK (enforced IN (0, 1)), created_at INTEGER "
    "NOT NULL CHECK (created_at >= 0), expires_at INTEGER NOT NULL "
    "CHECK (expires_at >= 0), lifted_at INTEGER NOT NULL DEFAULT 0 "
    "CHECK (lifted_at >= 0), lifted_by TEXT NOT NULL DEFAULT '')",
    "CREATE INDEX IF NOT EXISTS idx_probe_bans_expires ON "
    "probe_bans(expires_at)",
    """CREATE TABLE IF NOT EXISTS marketing_email_campaigns (
        campaign_id TEXT PRIMARY KEY,
        status TEXT NOT NULL DEFAULT 'draft'
            CHECK (status IN ('draft','sending','sent','cancelled')),
        data TEXT NOT NULL,
        created_by TEXT NOT NULL DEFAULT '',
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        started_at INTEGER NOT NULL DEFAULT 0,
        finished_at INTEGER NOT NULL DEFAULT 0,
        cursor_rowid INTEGER NOT NULL DEFAULT 0,
        sent INTEGER NOT NULL DEFAULT 0,
        failed INTEGER NOT NULL DEFAULT 0,
        skipped INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_marketing_email_campaigns_status "
    "ON marketing_email_campaigns(status, started_at)",
    """CREATE TABLE IF NOT EXISTS marketing_email_sends (
        campaign_id TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        sent_at INTEGER NOT NULL,
        PRIMARY KEY (campaign_id, account_bi))""",
    "CREATE TABLE IF NOT EXISTS notification_email_sends (send_bi TEXT "
    "PRIMARY KEY, created_at INTEGER NOT NULL CHECK (created_at >= 0))",
    "CREATE INDEX IF NOT EXISTS idx_notification_email_sends_created ON "
    "notification_email_sends(created_at)",
    """CREATE TABLE IF NOT EXISTS email_sends (
        send_id TEXT PRIMARY KEY,
        kind TEXT NOT NULL,
        recipient_bi TEXT NOT NULL DEFAULT '',
        ref TEXT NOT NULL DEFAULT '',
        data TEXT NOT NULL,
        sent_at INTEGER NOT NULL,
        opened_at INTEGER NOT NULL DEFAULT 0,
        clicked_at INTEGER NOT NULL DEFAULT 0,
        viewed_at INTEGER NOT NULL DEFAULT 0,
        open_count INTEGER NOT NULL DEFAULT 0,
        click_count INTEGER NOT NULL DEFAULT 0,
        view_count INTEGER NOT NULL DEFAULT 0)""",
    "CREATE INDEX IF NOT EXISTS idx_email_sends_recipient "
    "ON email_sends(recipient_bi, sent_at)",
    "CREATE INDEX IF NOT EXISTS idx_email_sends_sent "
    "ON email_sends(sent_at)",
    """CREATE TABLE IF NOT EXISTS email_footers (
        footer_id TEXT PRIMARY KEY,
        owner_bi TEXT NOT NULL UNIQUE,
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        opened_at INTEGER NOT NULL DEFAULT 0,
        last_open_at INTEGER NOT NULL DEFAULT 0,
        open_count INTEGER NOT NULL DEFAULT 0)""",
    """CREATE TABLE IF NOT EXISTS workflows (
        workflow_id TEXT PRIMARY KEY,
        owner_bi TEXT NOT NULL,
        active INTEGER NOT NULL DEFAULT 0,
        schedule_minutes INTEGER NOT NULL DEFAULT 0,
        next_run_at INTEGER NOT NULL DEFAULT 0,
        last_run_at INTEGER NOT NULL DEFAULT 0,
        last_status TEXT NOT NULL DEFAULT '',
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_workflows_owner "
    "ON workflows(owner_bi, updated_at)",
    "CREATE INDEX IF NOT EXISTS idx_workflows_schedule "
    "ON workflows(active, next_run_at)",
    """CREATE TABLE IF NOT EXISTS workflow_runs (
        run_id TEXT PRIMARY KEY,
        workflow_id TEXT NOT NULL,
        owner_bi TEXT NOT NULL,
        trigger_kind TEXT NOT NULL,
        status TEXT NOT NULL,
        started_at INTEGER NOT NULL,
        finished_at INTEGER NOT NULL,
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_workflow_runs_workflow "
    "ON workflow_runs(workflow_id, started_at)",
    "CREATE INDEX IF NOT EXISTS idx_workflow_runs_owner "
    "ON workflow_runs(owner_bi, started_at)",
    """CREATE TABLE IF NOT EXISTS workflow_credentials (
        credential_id TEXT PRIMARY KEY,
        owner_bi TEXT NOT NULL,
        integration TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_workflow_credentials_owner "
    "ON workflow_credentials(owner_bi, created_at)",
    """CREATE TABLE IF NOT EXISTS repo_remote_forks (
        repo_bi TEXT NOT NULL,
        url_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        data TEXT NOT NULL,
        PRIMARY KEY (repo_bi, url_bi))""",
    """CREATE TABLE IF NOT EXISTS pull_batch_sent (
        repo_bi TEXT NOT NULL,
        target_bi TEXT NOT NULL,
        branch TEXT NOT NULL,
        head TEXT NOT NULL,
        number INTEGER NOT NULL,
        created_at INTEGER NOT NULL,
        PRIMARY KEY (repo_bi, target_bi, branch, head))""",
    """CREATE TABLE IF NOT EXISTS recent_pushes (
        account_bi TEXT NOT NULL,
        repo_bi TEXT NOT NULL,
        branch_bi TEXT NOT NULL,
        pull_bi TEXT NOT NULL,
        pushed_at INTEGER NOT NULL,
        pulled INTEGER NOT NULL DEFAULT 0,
        data TEXT NOT NULL,
        PRIMARY KEY (account_bi, repo_bi, branch_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_recent_pushes_account "
    "ON recent_pushes(account_bi, pushed_at)",
    "CREATE INDEX IF NOT EXISTS idx_recent_pushes_branch "
    "ON recent_pushes(repo_bi, branch_bi)",
    "CREATE INDEX IF NOT EXISTS idx_recent_pushes_pull "
    "ON recent_pushes(pull_bi)",
    "CREATE INDEX IF NOT EXISTS idx_recent_pushes_age "
    "ON recent_pushes(pushed_at)",
    """CREATE TABLE IF NOT EXISTS comment_reactions (
        repo_bi TEXT NOT NULL,
        subject TEXT NOT NULL,
        comment TEXT NOT NULL,
        emoji TEXT NOT NULL,
        account_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        data TEXT NOT NULL,
        PRIMARY KEY (repo_bi, subject, comment, emoji, account_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_comment_reactions_account "
    "ON comment_reactions(account_bi, created_at)",
    """CREATE TABLE IF NOT EXISTS comment_reaction_rate (
        account_bi TEXT PRIMARY KEY,
        window_start INTEGER NOT NULL,
        count INTEGER NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS member_repo_activity (
        repo_bi TEXT NOT NULL,
        user_bi TEXT NOT NULL,
        commits INTEGER NOT NULL DEFAULT 0,
        pulls INTEGER NOT NULL DEFAULT 0,
        issues INTEGER NOT NULL DEFAULT 0,
        reviews INTEGER NOT NULL DEFAULT 0,
        day_start TEXT NOT NULL DEFAULT '',
        day_mask TEXT NOT NULL DEFAULT '',
        last_day TEXT NOT NULL DEFAULT '',
        updated_at INTEGER NOT NULL,
        PRIMARY KEY (repo_bi, user_bi))""",
    "CREATE INDEX IF NOT EXISTS idx_member_repo_activity_user "
    "ON member_repo_activity(user_bi)",
    """CREATE TABLE IF NOT EXISTS member_activity_repos (
        repo_bi TEXT PRIMARY KEY,
        refreshed_at INTEGER NOT NULL DEFAULT 0,
        people INTEGER NOT NULL DEFAULT 0,
        ok INTEGER NOT NULL DEFAULT 0)""",
    """CREATE TABLE IF NOT EXISTS custom_reactions (
        scope TEXT NOT NULL,
        slug TEXT NOT NULL,
        label TEXT NOT NULL,
        kind TEXT NOT NULL,
        asset TEXT NOT NULL,
        digest TEXT NOT NULL,
        anim_kind TEXT NOT NULL DEFAULT '',
        anim_asset TEXT NOT NULL DEFAULT '',
        anim_digest TEXT NOT NULL DEFAULT '',
        position INTEGER NOT NULL DEFAULT 0,
        enabled INTEGER NOT NULL DEFAULT 1,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        data TEXT NOT NULL,
        PRIMARY KEY (scope, slug))""",
    "CREATE INDEX IF NOT EXISTS idx_custom_reactions_digest "
    "ON custom_reactions(digest)",
    "CREATE INDEX IF NOT EXISTS idx_custom_reactions_anim_digest "
    "ON custom_reactions(anim_digest)",
    """CREATE TABLE IF NOT EXISTS comment_tips (
        reference TEXT PRIMARY KEY,
        repo_bi TEXT NOT NULL,
        subject TEXT NOT NULL,
        comment TEXT NOT NULL,
        tipper_bi TEXT NOT NULL,
        recipient_bi TEXT NOT NULL,
        token TEXT NOT NULL,
        mint TEXT NOT NULL DEFAULT '',
        amount TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'pending',
        signature TEXT NOT NULL DEFAULT '',
        created_at INTEGER NOT NULL,
        checked_at INTEGER NOT NULL DEFAULT 0,
        confirmed_at INTEGER NOT NULL DEFAULT 0,
        checks INTEGER NOT NULL DEFAULT 0,
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_comment_tips_record "
    "ON comment_tips(repo_bi, subject, status)",
    "CREATE INDEX IF NOT EXISTS idx_comment_tips_tipper "
    "ON comment_tips(tipper_bi, created_at)",
    "CREATE INDEX IF NOT EXISTS idx_comment_tips_recipient "
    "ON comment_tips(recipient_bi)",
    "CREATE INDEX IF NOT EXISTS idx_comment_tips_pending "
    "ON comment_tips(status, checked_at)",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_comment_tips_signature "
    "ON comment_tips(signature) WHERE signature<>''",
    """CREATE TABLE IF NOT EXISTS comment_extras_rate (
        k TEXT PRIMARY KEY,
        window_start INTEGER NOT NULL,
        count INTEGER NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS instance_settings (
        k TEXT PRIMARY KEY,
        doc TEXT NOT NULL,
        updated_at INTEGER NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS repo_provider_mirrors (
        repo_bi TEXT PRIMARY KEY,
        provider TEXT NOT NULL,
        active INTEGER NOT NULL DEFAULT 1,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        next_sync_at INTEGER NOT NULL DEFAULT 0,
        last_sync_at INTEGER NOT NULL DEFAULT 0,
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_repo_provider_mirrors_due "
    "ON repo_provider_mirrors(active, next_sync_at)",
    """CREATE TABLE IF NOT EXISTS repo_provider_mirror_links (
        repo_bi TEXT NOT NULL,
        kind TEXT NOT NULL,
        local_key TEXT NOT NULL,
        remote_key TEXT NOT NULL,
        origin TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        PRIMARY KEY (repo_bi, kind, remote_key))""",
    "CREATE INDEX IF NOT EXISTS idx_repo_provider_mirror_links_local "
    "ON repo_provider_mirror_links(repo_bi, kind, local_key)",
    """CREATE TABLE IF NOT EXISTS repo_provider_mirror_outbox (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        repo_bi TEXT NOT NULL,
        attempts INTEGER NOT NULL DEFAULT 0,
        next_ts INTEGER NOT NULL,
        created_at INTEGER NOT NULL,
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_repo_provider_mirror_outbox_due "
    "ON repo_provider_mirror_outbox(next_ts)",
    "CREATE INDEX IF NOT EXISTS idx_repo_provider_mirror_outbox_repo "
    "ON repo_provider_mirror_outbox(repo_bi)",
    """CREATE TABLE IF NOT EXISTS repo_uploads (
        media_id TEXT PRIMARY KEY,
        repo_bi TEXT NOT NULL,
        uploader_bi TEXT NOT NULL,
        state TEXT NOT NULL CHECK (state IN ('uploading', 'ready', 'removed')),
        mime TEXT NOT NULL,
        size INTEGER NOT NULL CHECK (size >= 0),
        parts INTEGER NOT NULL CHECK (parts >= 0),
        created_at INTEGER NOT NULL,
        ready_at INTEGER NOT NULL DEFAULT 0,
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_repo_uploads_uploader "
    "ON repo_uploads(uploader_bi, created_at)",
    "CREATE INDEX IF NOT EXISTS idx_repo_uploads_state "
    "ON repo_uploads(state, created_at)",
    """CREATE TABLE IF NOT EXISTS repo_upload_parts (
        media_id TEXT NOT NULL,
        part_index INTEGER NOT NULL CHECK (part_index >= 0),
        size INTEGER NOT NULL CHECK (size >= 0),
        data TEXT NOT NULL,
        PRIMARY KEY (media_id, part_index))""",
]
