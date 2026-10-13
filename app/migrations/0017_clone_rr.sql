-- ForkMesh D1 migration 0017 — clone-fallback round-robin cursor.
-- When a repo's named source-of-truth host is offline, a clone is redirected to
-- one of its online mirrors. This per-repo counter is bumped on each redirect so
-- the picks rotate across every mirror in turn, spreading the load instead of
-- piling every clone onto the single freshest mirror. The worker also creates
-- this lazily (ensure_schema in src/entry.py); this migration keeps the
-- file-based schema in sync. repo_bi is the same blind index host_presence uses;
-- no plaintext repo name — n is just a rotation cursor.

CREATE TABLE IF NOT EXISTS clone_rr (
    repo_bi TEXT PRIMARY KEY,    -- blind index of "<owner>/<repo>"
    n INTEGER NOT NULL DEFAULT 0 -- monotonically increasing round-robin cursor
);
