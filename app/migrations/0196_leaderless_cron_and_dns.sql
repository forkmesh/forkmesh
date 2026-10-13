-- ForkMesh Next: leaderless cron rotation and DNS health (docs/OPERATIONS.md).
--
-- host_cron_runs: which core host ran (part of) which cron minute of the
-- rotating cron coordinator (app/host/forkmesh_host/durable_cluster.py).
-- Audit only; nothing reads it to decide anything. Rows older than a day are
-- swept by the hosts themselves.
CREATE TABLE IF NOT EXISTS host_cron_runs (
    ns TEXT NOT NULL,
    name TEXT NOT NULL,
    slot INTEGER NOT NULL,
    node_id TEXT NOT NULL,
    first_ms INTEGER NOT NULL,
    last_ms INTEGER NOT NULL,
    invocations INTEGER NOT NULL DEFAULT 1,
    phases TEXT,
    PRIMARY KEY (ns, name, slot, node_id)
);

-- dns_health_votes: every unsealed core host's latest view of every core
-- node's public readiness (app/host/forkmesh_host/dns_reconciler.py). The
-- host chosen for the current 30 s interval changes the app/world A records
-- only when a majority of live voters agree.
CREATE TABLE IF NOT EXISTS dns_health_votes (
    voter TEXT NOT NULL,
    target TEXT NOT NULL,
    healthy INTEGER NOT NULL,
    observed_ms INTEGER NOT NULL,
    detail TEXT,
    PRIMARY KEY (voter, target)
);

-- dns_reconciler_state: cluster-wide reconciler bookkeeping. The row
-- 'last_change_ms' is advanced with a compare-and-set before any DNS change,
-- so at most one change is made per 5 minutes whichever host acts.
CREATE TABLE IF NOT EXISTS dns_reconciler_state (
    name TEXT PRIMARY KEY,
    value TEXT NOT NULL,
    updated_ms INTEGER NOT NULL
);
