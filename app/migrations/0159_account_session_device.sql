-- Link each account session to the node key that opened it.
--
-- A key-bound login (desktop node, and now a mobile node — see
-- www/docs/mobile/mobile-node-contract.md) records the account_devices row it
-- proved possession of on the session it mints. Revoking that device from the
-- web registry (DELETE /api/accounts/devices/{id}) then revokes every session
-- the device opened, and session lookups treat a session whose device row is
-- revoked as dead. Plain web sessions store ''.

ALTER TABLE account_sessions
  ADD COLUMN device_bi TEXT NOT NULL DEFAULT '';

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:526c35c3a0bed08099a01004cf6873dee8a1206a62e7c566df47a4076e10e294',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
