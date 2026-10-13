-- Reverse 0192_cluster_members_and_do_state.sql. Durable Object storage and
-- pending alarms are lost; hosts recreate their membership rows on the next
-- heartbeat once the table exists again.

DROP TABLE IF EXISTS do_alarms;
DROP TABLE IF EXISTS do_storage;
DROP TABLE IF EXISTS cluster_members;
