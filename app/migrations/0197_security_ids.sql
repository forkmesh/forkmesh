-- ForkMesh Next host intrusion detection (docs/IDS.md).
--
-- cluster_quarantine: signed quarantine records (app/host/forkmesh_host/
-- ids_records.py). kind 'self' (a node's IDS locked the node down, signed by
-- that node's IDS key), 'vote' (a peer's IDS saw the node's IDS heartbeat
-- stop, signed by the voter, and a strict majority of the core nodes counts) and
-- 'release' (signed by the operator key on the desktop). A row is trusted only
-- through its signature. reunsealed_ms (release rows only) is stamped by the
-- released node once the operator has unsealed it from the desktop. Until
-- then peers refuse it peer unseal.
CREATE TABLE IF NOT EXISTS cluster_quarantine (
    id TEXT PRIMARY KEY,
    node_id TEXT NOT NULL,
    kind TEXT NOT NULL,
    signer TEXT NOT NULL,
    created_ms INTEGER NOT NULL,
    body TEXT NOT NULL,
    sig TEXT NOT NULL,
    reunsealed_ms INTEGER
);
CREATE INDEX IF NOT EXISTS idx_cluster_quarantine_node
    ON cluster_quarantine(node_id, created_ms);

-- security_incidents: every IDS incident (info, alert, lockdown, test), the
-- durable record. Unsealed core hosts deliver the alert/lockdown/test rows to
-- the administrators (app/host/forkmesh_host/ids_alerts.py): claimed with a
-- compare-and-set on claimed_ms, then delivered_ms + delivery (JSON summary).
CREATE TABLE IF NOT EXISTS security_incidents (
    id TEXT PRIMARY KEY,
    node_id TEXT NOT NULL,
    created_ms INTEGER NOT NULL,
    severity TEXT NOT NULL,
    detector TEXT NOT NULL,
    summary TEXT NOT NULL,
    detail TEXT,
    claimed_by TEXT,
    claimed_ms INTEGER,
    delivered_ms INTEGER,
    delivery TEXT
);
CREATE INDEX IF NOT EXISTS idx_security_incidents_pending
    ON security_incidents(delivered_ms, created_ms);
CREATE INDEX IF NOT EXISTS idx_security_incidents_node
    ON security_incidents(node_id, created_ms);

-- ids_heartbeats: each node's IDS writes its own row every 30 s. A peer whose
-- app heart-beats in cluster_members while its IDS row goes stale is voted
-- into quarantine by the other nodes' IDS.
CREATE TABLE IF NOT EXISTS ids_heartbeats (
    node_id TEXT PRIMARY KEY,
    beat_ms INTEGER NOT NULL,
    state TEXT NOT NULL,
    detail TEXT
);
