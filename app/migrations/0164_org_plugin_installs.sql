-- Organization plugin install registry. Market plugins own their data
-- tables: installing a plugin for an organization runs that plugin's
-- DATA_SCHEMA_UP, and removing its last install runs DATA_SCHEMA_DOWN,
-- reversing the plugin's migration so an uninstalled plugin leaves no trace
-- in the database. Only this registry table is core schema.

CREATE TABLE IF NOT EXISTS org_plugin_installs (
  org_bi TEXT NOT NULL,
  plugin TEXT NOT NULL,
  org_name TEXT NOT NULL DEFAULT '',
  installed_by_bi TEXT NOT NULL DEFAULT '',
  installed_by_name TEXT NOT NULL DEFAULT '',
  installed_at INTEGER NOT NULL DEFAULT 0 CHECK (installed_at >= 0),
  PRIMARY KEY (org_bi, plugin)
);

CREATE INDEX IF NOT EXISTS idx_org_plugin_installs_plugin
  ON org_plugin_installs(plugin, org_bi);

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:6ad4e35a83b64adbd2de5411e6aed18fc5b8e1776a522bfff3db22f844294677',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
