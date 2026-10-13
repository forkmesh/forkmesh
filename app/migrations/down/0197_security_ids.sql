-- Reverse 0197: drop the IDS tables.
DROP TABLE IF EXISTS ids_heartbeats;
DROP INDEX IF EXISTS idx_security_incidents_node;
DROP INDEX IF EXISTS idx_security_incidents_pending;
DROP TABLE IF EXISTS security_incidents;
DROP INDEX IF EXISTS idx_cluster_quarantine_node;
DROP TABLE IF EXISTS cluster_quarantine;
