-- APNs device tokens, one row per phone install.
--
-- A phone cannot hold the ForkMeshNodes event socket the desktop and browser
-- clients use: iOS suspends the process, so there is nothing left to receive a
-- {"type":"event","topic"} frame. APNs is the only path to a device whose app
-- is not running, so notify_account_event fans out to this table alongside the
-- socket (see app/src/apns.py).
--
-- The device token is what authorizes a push to a handset, so it is stored the
-- way every other secret is here: token_bi is a blind index of the token and
-- doubles as the upsert key (a re-registered token updates in place rather
-- than accumulating rows), while the token itself lives encrypted in `data`.
-- `environment` records whether a development build (APNs sandbox) or a
-- release build minted the token; sending to the wrong gateway is a hard
-- BadDeviceToken rather than a silent drop.

CREATE TABLE IF NOT EXISTS apns_device_tokens (
  token_bi TEXT PRIMARY KEY,
  account_bi TEXT NOT NULL,
  device_bi TEXT NOT NULL DEFAULT '',
  environment TEXT NOT NULL DEFAULT 'production',
  data TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  last_seen INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_apns_tokens_account
  ON apns_device_tokens(account_bi, last_seen DESC);

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:6e5c0525ce5493c17f5952da0dd32f33bc22c2aa2706467d2137a3c38b4c5ae7',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
