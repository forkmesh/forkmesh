-- Efficient identity-free access to owner-sealed private replicas.
--
-- The client path contains only a random 256-bit opaque replica id.  Repository
-- names remain inside encrypted catalog rows, and authorization remains in the
-- non-forwarded HTTP Authorization header.  This index also upgrades databases
-- that already applied migration 0050 before the access route was introduced.

CREATE INDEX IF NOT EXISTS idx_private_mirror_route_access
    ON private_mirror_routes(opaque_replica_id, active, key_epoch, repo_bi);
