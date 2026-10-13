-- Content-free relay accounting behind the System Capacity platform. One row
-- per Durable Object binding, holding the bytes and frames that class has
-- relayed. No room key, account, peer id, instance id, or message content is
-- stored; each Durable Object folds its in-memory counters in at most once a
-- minute (or once a batch of bytes has gone by).
CREATE TABLE IF NOT EXISTS durable_object_traffic (
    binding TEXT PRIMARY KEY,
    bytes_in INTEGER NOT NULL DEFAULT 0 CHECK (bytes_in >= 0),
    bytes_out INTEGER NOT NULL DEFAULT 0 CHECK (bytes_out >= 0),
    messages INTEGER NOT NULL DEFAULT 0 CHECK (messages >= 0),
    updated_at INTEGER NOT NULL DEFAULT 0 CHECK (updated_at >= 0));
