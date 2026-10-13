CREATE TABLE IF NOT EXISTS repo_remote_forks (
  repo_bi TEXT NOT NULL,
  url_bi TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  data TEXT NOT NULL,
  PRIMARY KEY (repo_bi, url_bi));

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:9e025def33f0a64e293bca381a178481bc48c7f28cb2ec3b701e33f4774b3e2c',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
