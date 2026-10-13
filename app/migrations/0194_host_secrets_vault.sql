-- ForkMesh Next: host secrets vault (docs/VAULT.md).
--
-- One row per application variable, AES-256-GCM encrypted on the operator's
-- desktop under a key derived from DATA_KEY (app/host/forkmesh_host/
-- vault_crypto.py). DATA_KEY itself is never stored here or on any node's
-- disk; a node reads these rows only after the operator hands it the key
-- over its unseal socket. The AAD binds each ciphertext to its name.
-- key_id identifies the DATA_KEY a row was sealed with (no decrypt oracle);
-- the reserved row '__vault_check__' proves an offered key is the right one.
CREATE TABLE IF NOT EXISTS host_secrets (
    name TEXT PRIMARY KEY,
    nonce TEXT NOT NULL,
    ciphertext TEXT NOT NULL,
    key_id TEXT NOT NULL,
    updated_at INTEGER NOT NULL
);
