-- ForkMesh Next, Phase 3: Durable Objects across the core cluster
-- (app/host/forkmesh_host/cluster.py, durable_cluster.py).
--
-- cluster_members: one row per core host. Each host upserts its own
-- heartbeat_ms every 5 s (status is left alone on conflict); only the host
-- next to the rqlite leader flips status between 'active' and 'suspended'
-- (heartbeat older than 15 s). Object placement is a rendezvous hash over
-- the 'active' rows.
CREATE TABLE IF NOT EXISTS cluster_members (
    node_id TEXT PRIMARY KEY,
    mesh_url TEXT,
    status TEXT,
    heartbeat_ms INTEGER,
    epoch INTEGER
);

-- ctx.storage of every placed Durable Object: ns is the class name, name the
-- idFromName() name (or '#id:<hex>'), value a tagged JSON encoding.
CREATE TABLE IF NOT EXISTS do_storage (
    ns TEXT NOT NULL,
    name TEXT NOT NULL,
    key TEXT NOT NULL,
    value TEXT,
    PRIMARY KEY (ns, name, key)
);

-- ctx.storage.setAlarm(): at most one pending alarm per object. The owner
-- arms an asyncio timer; a new owner re-arms from this table.
CREATE TABLE IF NOT EXISTS do_alarms (
    ns TEXT NOT NULL,
    name TEXT NOT NULL,
    at_ms INTEGER NOT NULL,
    PRIMARY KEY (ns, name)
);
