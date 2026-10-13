-- Self-serve plan-trial register (issue #488). One trial per account, started
-- from the dashboard without support intervention. The trial's entitlement is
-- a promotion-scope role grant with its own expiry — the lane neither Polar's
-- recompute nor Patreon's revoke touches — so a billing-provider failure can
-- never extend, revoke, or resurrect a trial. This table is the auditable
-- register the activation/revenue scorecard's trialsCompleted count
-- reconciles to.

CREATE TABLE IF NOT EXISTS plan_trials (
    account_bi TEXT PRIMARY KEY,
    tier TEXT NOT NULL CHECK (tier IN ('supporter')),
    started_at INTEGER NOT NULL,
    ends_at INTEGER NOT NULL
);

-- The runtime schema text grew (plan_trials), so stamp the new fingerprint:
-- ensure_schema's fast path then skips the full replay.
INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:bb64b9830dd4dc29d9f6e4cb28245d2b9e375511b38d686094b0df464db41910',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
