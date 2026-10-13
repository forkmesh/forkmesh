-- ForkMesh D1 migration 0042 — drop the legacy accounts table.
--
-- Account records were split into the physical users/nodes tables (migration
-- 0025) and every remaining accounts row has been drained into them (write
-- paths mirrored on save, reads repaired on access, and the admin console's
-- one-click verified-user migration handled the rest). Nothing reads or
-- writes the accounts table any more, so drop it (its indexes go with it).
DROP TABLE IF EXISTS accounts;
