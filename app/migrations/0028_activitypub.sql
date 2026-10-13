-- ActivityPub federation (fediverse interop). Local user/repo actors get RSA
-- keypairs and become followable from Mastodon-compatible servers; remote
-- replies land in ap_comments (never in the Ed25519-signed event log); the
-- ap_outbox table is the queue-free (free plan) delivery retry queue.
CREATE TABLE IF NOT EXISTS ap_actors (
    actor_bi TEXT PRIMARY KEY, kind TEXT NOT NULL,
    pubkey_pem TEXT NOT NULL, data TEXT NOT NULL,
    created_at INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS ap_followers (
    actor_bi TEXT NOT NULL, follower_id TEXT NOT NULL,
    inbox TEXT NOT NULL, shared_inbox TEXT, follower_handle TEXT,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (actor_bi, follower_id));
CREATE INDEX IF NOT EXISTS idx_ap_followers_actor ON ap_followers(actor_bi);
CREATE TABLE IF NOT EXISTS ap_remote_actors (
    actor_id TEXT PRIMARY KEY, inbox TEXT, shared_inbox TEXT,
    pubkey_pem TEXT, handle TEXT, display_name TEXT, url TEXT,
    updated_at INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS ap_objects (
    object_uuid TEXT PRIMARY KEY, actor_bi TEXT NOT NULL,
    context_bi TEXT, data TEXT NOT NULL, published INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS idx_ap_objects_actor ON ap_objects(actor_bi);
CREATE INDEX IF NOT EXISTS idx_ap_objects_context ON ap_objects(context_bi);
CREATE TABLE IF NOT EXISTS ap_comments (
    id INTEGER PRIMARY KEY AUTOINCREMENT, context_bi TEXT NOT NULL,
    remote_id_bi TEXT UNIQUE, data TEXT NOT NULL, ts INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS idx_ap_comments_context ON ap_comments(context_bi, ts);
CREATE TABLE IF NOT EXISTS ap_outbox (
    id INTEGER PRIMARY KEY AUTOINCREMENT, inbox TEXT NOT NULL,
    data TEXT NOT NULL, attempts INTEGER NOT NULL DEFAULT 0,
    next_ts INTEGER NOT NULL, created_at INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS idx_ap_outbox_next ON ap_outbox(next_ts);
