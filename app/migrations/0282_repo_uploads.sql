CREATE TABLE IF NOT EXISTS repo_uploads (
    media_id TEXT PRIMARY KEY,
    repo_bi TEXT NOT NULL,
    uploader_bi TEXT NOT NULL,
    state TEXT NOT NULL CHECK (state IN ('uploading', 'ready', 'removed')),
    mime TEXT NOT NULL,
    size INTEGER NOT NULL CHECK (size >= 0),
    parts INTEGER NOT NULL CHECK (parts >= 0),
    created_at INTEGER NOT NULL,
    ready_at INTEGER NOT NULL DEFAULT 0,
    data TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_repo_uploads_uploader ON repo_uploads(uploader_bi, created_at);
CREATE INDEX IF NOT EXISTS idx_repo_uploads_state ON repo_uploads(state, created_at);
CREATE TABLE IF NOT EXISTS repo_upload_parts (
    media_id TEXT NOT NULL,
    part_index INTEGER NOT NULL CHECK (part_index >= 0),
    size INTEGER NOT NULL CHECK (size >= 0),
    data TEXT NOT NULL,
    PRIMARY KEY (media_id, part_index)
);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:af3db7532a65ca75fe044f52c5b7bdf51c22bc662cfb4c1d196eb6f93dec24c9',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
