-- Normalize the shared organization task catalog to the public 1..99 scale.
-- Application writes clamp to this range; this one-time migration brings
-- legacy 100/500/999 values onto the explicit lowest-priority endpoint.
UPDATE organization_tasks
SET priority = 99
WHERE priority > 99;
