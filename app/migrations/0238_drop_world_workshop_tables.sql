DROP INDEX IF EXISTS idx_world_workshop_events_session;
DROP TABLE IF EXISTS world_workshop_events;
DROP INDEX IF EXISTS idx_world_workshop_results_session;
DROP TABLE IF EXISTS world_workshop_results;
DROP INDEX IF EXISTS idx_world_workshop_participants_account;
DROP TABLE IF EXISTS world_workshop_participants;
DROP INDEX IF EXISTS idx_world_workshop_sessions_owner;
DROP INDEX IF EXISTS idx_world_workshop_sessions_scope;
DROP TABLE IF EXISTS world_workshop_sessions;

INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:8926453f8fd31a02f4b85fcbd395fc57cbc6fe667033f57e316a35586e0c42ba',
  'applied'
) ON CONFLICT(k) DO UPDATE SET v='applied';
