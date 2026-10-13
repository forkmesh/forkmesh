CREATE TABLE IF NOT EXISTS apps (
    app_id TEXT PRIMARY KEY,
    client_id TEXT NOT NULL UNIQUE,
    slug TEXT NOT NULL UNIQUE,
    owner_bi TEXT NOT NULL,
    org_bi TEXT NOT NULL DEFAULT '',
    client_secret_bi TEXT NOT NULL,
    listing TEXT NOT NULL DEFAULT 'private',
    listed_hash TEXT NOT NULL DEFAULT '',
    event_url_verified_at INTEGER NOT NULL DEFAULT 0,
    event_failures INTEGER NOT NULL DEFAULT 0,
    events_disabled_at INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    data TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS idx_apps_owner ON apps(owner_bi, created_at);
CREATE INDEX IF NOT EXISTS idx_apps_org ON apps(org_bi);
CREATE INDEX IF NOT EXISTS idx_apps_listing ON apps(listing, updated_at);

CREATE TABLE IF NOT EXISTS app_reviews (
    review_id TEXT PRIMARY KEY,
    app_id TEXT NOT NULL,
    action TEXT NOT NULL,
    reviewer_bi TEXT NOT NULL,
    self_review INTEGER NOT NULL DEFAULT 0,
    reason TEXT NOT NULL DEFAULT '',
    content_hash TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS idx_app_reviews_app ON app_reviews(app_id, created_at);

CREATE TABLE IF NOT EXISTS app_auth_codes (
    code_bi TEXT PRIMARY KEY,
    app_id TEXT NOT NULL,
    install_id TEXT NOT NULL,
    account_bi TEXT NOT NULL,
    family_id TEXT NOT NULL,
    expires_at INTEGER NOT NULL,
    used_at INTEGER NOT NULL DEFAULT 0,
    data TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS idx_app_auth_codes_install ON app_auth_codes(install_id);
CREATE INDEX IF NOT EXISTS idx_app_auth_codes_expiry ON app_auth_codes(expires_at);

CREATE TABLE IF NOT EXISTS app_installs (
    install_id TEXT PRIMARY KEY,
    app_id TEXT NOT NULL,
    target_kind TEXT NOT NULL,
    target_bi TEXT NOT NULL,
    installed_by_bi TEXT NOT NULL,
    scopes TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'active',
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    last_used_at INTEGER NOT NULL DEFAULT 0,
    revoked_at INTEGER NOT NULL DEFAULT 0,
    data TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS idx_app_installs_app_target ON app_installs(app_id, target_kind, target_bi, status);
CREATE INDEX IF NOT EXISTS idx_app_installs_target ON app_installs(target_bi, status);

CREATE TABLE IF NOT EXISTS app_tokens (
    token_bi TEXT PRIMARY KEY,
    kind TEXT NOT NULL,
    app_id TEXT NOT NULL,
    install_id TEXT NOT NULL,
    family_id TEXT NOT NULL,
    scopes TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL,
    revoked_at INTEGER NOT NULL DEFAULT 0,
    rotated_at INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS idx_app_tokens_install ON app_tokens(install_id, revoked_at);
CREATE INDEX IF NOT EXISTS idx_app_tokens_family ON app_tokens(family_id);
CREATE INDEX IF NOT EXISTS idx_app_tokens_expiry ON app_tokens(expires_at);

CREATE TABLE IF NOT EXISTS app_org_policies (
    org_bi TEXT PRIMARY KEY,
    request_policy TEXT NOT NULL DEFAULT 'any',
    allowlist TEXT NOT NULL DEFAULT '[]',
    updated_by_bi TEXT NOT NULL DEFAULT '',
    updated_at INTEGER NOT NULL DEFAULT 0);

CREATE TABLE IF NOT EXISTS app_org_approvals (
    org_bi TEXT NOT NULL,
    app_id TEXT NOT NULL,
    scopes TEXT NOT NULL,
    approved_by_bi TEXT NOT NULL,
    approved_at INTEGER NOT NULL,
    PRIMARY KEY (org_bi, app_id));

CREATE TABLE IF NOT EXISTS app_install_requests (
    request_id TEXT PRIMARY KEY,
    app_id TEXT NOT NULL,
    org_bi TEXT NOT NULL,
    requester_bi TEXT NOT NULL,
    scopes TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'pending',
    created_at INTEGER NOT NULL,
    decided_at INTEGER NOT NULL DEFAULT 0,
    decided_by_bi TEXT NOT NULL DEFAULT '',
    data TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS idx_app_install_requests_org ON app_install_requests(org_bi, status, created_at);

CREATE TABLE IF NOT EXISTS app_event_outbox (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    app_id TEXT NOT NULL,
    install_id TEXT NOT NULL,
    event_id TEXT NOT NULL,
    event_type TEXT NOT NULL,
    attempts INTEGER NOT NULL DEFAULT 0,
    next_ts INTEGER NOT NULL,
    created_at INTEGER NOT NULL,
    data TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS idx_app_event_outbox_due ON app_event_outbox(next_ts, id);
CREATE INDEX IF NOT EXISTS idx_app_event_outbox_app ON app_event_outbox(app_id);
CREATE INDEX IF NOT EXISTS idx_app_event_outbox_install ON app_event_outbox(install_id);

CREATE TABLE IF NOT EXISTS app_deliveries (
    delivery_id TEXT PRIMARY KEY,
    app_id TEXT NOT NULL,
    kind TEXT NOT NULL,
    event_type TEXT NOT NULL DEFAULT '',
    status INTEGER NOT NULL DEFAULT 0,
    attempts INTEGER NOT NULL DEFAULT 0,
    outcome TEXT NOT NULL DEFAULT '',
    detail TEXT NOT NULL DEFAULT '',
    ts INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS idx_app_deliveries_app ON app_deliveries(app_id, ts DESC);

CREATE TABLE IF NOT EXISTS app_rate (
    rate_key TEXT PRIMARY KEY,
    count INTEGER NOT NULL DEFAULT 0,
    window_start INTEGER NOT NULL);

CREATE TABLE IF NOT EXISTS app_messages (
    message_id TEXT PRIMARY KEY,
    app_id TEXT NOT NULL,
    install_id TEXT NOT NULL,
    channel_id TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL,
    data TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS idx_app_messages_expiry ON app_messages(expires_at);

CREATE TABLE IF NOT EXISTS app_response_urls (
    token_bi TEXT PRIMARY KEY,
    app_id TEXT NOT NULL,
    install_id TEXT NOT NULL,
    channel_id TEXT NOT NULL DEFAULT '',
    account_bi TEXT NOT NULL,
    uses INTEGER NOT NULL DEFAULT 0,
    expires_at INTEGER NOT NULL,
    data TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS idx_app_response_urls_install ON app_response_urls(install_id);
CREATE INDEX IF NOT EXISTS idx_app_response_urls_expiry ON app_response_urls(expires_at);
