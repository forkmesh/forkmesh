-- Phase 5 runner tier: machines that only run agent jobs.
--
-- runner_nodes      registered runners (key, registrant, pool/personal scope,
--                   owners whose jobs it may take). See app/src/runner_leases.py.
-- runner_job_leases one row per org_agent_jobs row a runner holds; the grant
--                   it was handed covers exactly `ref` of target_node/repo.
-- runner_request_nonces single-use request ids of signed runner requests.

CREATE TABLE IF NOT EXISTS runner_nodes (
        name TEXT PRIMARY KEY,
        public_key TEXT NOT NULL,
        registered_by TEXT NOT NULL,
        scope TEXT NOT NULL DEFAULT 'personal'
            CHECK (scope IN ('pool','personal')),
        allowed_owners TEXT NOT NULL DEFAULT '[]',
        status TEXT NOT NULL DEFAULT 'active',
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        last_seen_at INTEGER NOT NULL DEFAULT 0);

CREATE TABLE IF NOT EXISTS runner_job_leases (
        job_id INTEGER PRIMARY KEY,
        runner TEXT NOT NULL,
        session_id TEXT NOT NULL,
        lease_id TEXT NOT NULL,
        grant_id TEXT NOT NULL,
        target_node TEXT NOT NULL,
        repo TEXT NOT NULL,
        ref TEXT NOT NULL,
        expires_at INTEGER NOT NULL,
        created_at INTEGER NOT NULL);

CREATE INDEX IF NOT EXISTS idx_runner_job_leases_session
ON runner_job_leases(session_id, created_at);

CREATE INDEX IF NOT EXISTS idx_runner_job_leases_runner
ON runner_job_leases(runner, expires_at);

CREATE TABLE IF NOT EXISTS runner_request_nonces (
        runner TEXT NOT NULL,
        request_id TEXT NOT NULL,
        expires_at INTEGER NOT NULL,
        PRIMARY KEY (runner, request_id));

-- The runtime schema text changed with it, so stamp the new fingerprint:
INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:a886ca6ba8d55e0acdf6d52ddc4b9e1af25ae281abda08d6707eb4966dd54636',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
