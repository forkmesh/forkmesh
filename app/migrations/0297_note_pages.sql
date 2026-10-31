CREATE TABLE IF NOT EXISTS note_pages (
    page_id TEXT PRIMARY KEY,
    owner_bi TEXT NOT NULL,
    version INTEGER NOT NULL DEFAULT 1 CHECK (version >= 1),
    created_at INTEGER NOT NULL CHECK (created_at >= 0),
    updated_at INTEGER NOT NULL CHECK (updated_at >= 0),
    data TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_note_pages_owner
    ON note_pages(owner_bi, updated_at DESC);

CREATE TABLE IF NOT EXISTS note_page_shares (
    page_id TEXT NOT NULL REFERENCES note_pages(page_id) ON DELETE CASCADE,
    principal_type TEXT NOT NULL
        CHECK (principal_type IN ('user','organization')),
    principal_bi TEXT NOT NULL,
    principal_name TEXT NOT NULL,
    role TEXT NOT NULL CHECK (role IN ('viewer','editor')),
    added_by_bi TEXT NOT NULL,
    created_at INTEGER NOT NULL CHECK (created_at >= 0),
    PRIMARY KEY(page_id, principal_type, principal_bi)
);

CREATE INDEX IF NOT EXISTS idx_note_page_shares_principal
    ON note_page_shares(principal_type, principal_bi, page_id);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:43155bc95879db379571b3fb5eaed011df8a14d9adc05bf124258679dce3f8ba',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
