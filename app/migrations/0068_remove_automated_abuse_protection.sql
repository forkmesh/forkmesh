-- Remove the retired automated abuse/quarantine subsystem.
--
-- Appeals depend on restriction incidents, and restrictions depend on
-- accumulated signals, so remove them in that order. IF EXISTS keeps this
-- migration safe for databases where the subsystem was never installed or was
-- already removed. Least-privilege role grants, sensitive audit history, and
-- owner encryption-key registrations intentionally remain.

DROP TABLE IF EXISTS security_appeals;
DROP TABLE IF EXISTS security_restrictions;
DROP TABLE IF EXISTS security_signals;
