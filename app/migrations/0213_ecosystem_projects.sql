CREATE TABLE IF NOT EXISTS ecosystem_projects (
  slug TEXT PRIMARY KEY,
  category TEXT NOT NULL,
  published INTEGER NOT NULL DEFAULT 0 CHECK (published IN (0, 1)),
  featured INTEGER NOT NULL DEFAULT 0 CHECK (featured IN (0, 1)),
  position INTEGER NOT NULL DEFAULT 0,
  data TEXT NOT NULL DEFAULT '{}',
  internal TEXT NOT NULL DEFAULT '{}',
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  updated_by TEXT NOT NULL DEFAULT ''
);

CREATE INDEX IF NOT EXISTS idx_ecosystem_projects_published
  ON ecosystem_projects(published, position);

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:64c5461c2ec36ef0c0d86949d5591fec47362629721d4c624b1660ef51a92cca',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
