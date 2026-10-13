-- Owner-authorized routes to opaque, end-to-end encrypted private replicas.
--
-- Repository identities are represented only by blind indexes. The random
-- replica id is not a repository name and addresses ciphertext at a registered
-- direct-HTTPS mirror. D1 never stores repository bytes, plaintext private
-- names, recipient private keys, wallet material, or decryption keys.

CREATE TABLE IF NOT EXISTS private_mirror_routes (
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
        ON DELETE CASCADE
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_private_mirror_route_replica
    ON private_mirror_routes(node_bi, opaque_replica_id);
CREATE INDEX IF NOT EXISTS idx_private_mirror_route_access
    ON private_mirror_routes(opaque_replica_id, active, key_epoch, repo_bi);
CREATE INDEX IF NOT EXISTS idx_private_mirror_route_repo
    ON private_mirror_routes(repo_bi, active, updated_at);
