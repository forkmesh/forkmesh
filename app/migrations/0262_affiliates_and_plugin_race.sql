CREATE TABLE IF NOT EXISTS affiliates (
        account_bi TEXT PRIMARY KEY,
        username TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'active'
            CHECK (status IN ('active','paused','removed')),
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL CHECK (created_at >= 0),
        updated_at INTEGER NOT NULL CHECK (updated_at >= 0));

CREATE INDEX IF NOT EXISTS idx_affiliates_username ON affiliates(username);

CREATE TABLE IF NOT EXISTS affiliate_leads (
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
        expires_at INTEGER NOT NULL DEFAULT 0 CHECK (expires_at >= 0));

CREATE UNIQUE INDEX IF NOT EXISTS idx_affiliate_leads_open_domain ON affiliate_leads(domain_bi) WHERE status IN ('new','contacted','trial','won','paid');

CREATE INDEX IF NOT EXISTS idx_affiliate_leads_affiliate ON affiliate_leads(affiliate_bi, created_at);

CREATE INDEX IF NOT EXISTS idx_affiliate_leads_status ON affiliate_leads(status, expires_at);

CREATE TABLE IF NOT EXISTS plugin_race_entrants (
        account_bi TEXT PRIMARY KEY,
        username TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'pending'
            CHECK (status IN ('pending','qualified','disqualified')),
        qualified_at INTEGER NOT NULL DEFAULT 0 CHECK (qualified_at >= 0),
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL CHECK (created_at >= 0),
        updated_at INTEGER NOT NULL CHECK (updated_at >= 0));

CREATE INDEX IF NOT EXISTS idx_plugin_race_entrants_status ON plugin_race_entrants(status, qualified_at);

CREATE TABLE IF NOT EXISTS plugin_race_posts (
        post_bi TEXT PRIMARY KEY,
        account_bi TEXT NOT NULL,
        network TEXT NOT NULL CHECK (network IN ('x','reddit','mastodon')),
        created_at INTEGER NOT NULL CHECK (created_at >= 0));

CREATE INDEX IF NOT EXISTS idx_plugin_race_posts_account ON plugin_race_posts(account_bi);

CREATE TABLE IF NOT EXISTS plugin_race_plugins (
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
        PRIMARY KEY (market, package_id));

CREATE INDEX IF NOT EXISTS idx_plugin_race_plugins_account ON plugin_race_plugins(account_bi, created_at);

CREATE UNIQUE INDEX IF NOT EXISTS idx_plugin_race_plugins_rank ON plugin_race_plugins(award_rank) WHERE award_rank>0;

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:ad25ed4761784ba8ed1739f3932706abf1fc5aebe29af5eef61cce77a5bf1572',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
