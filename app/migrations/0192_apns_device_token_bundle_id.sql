-- The app that minted each APNs device token.
--
-- Apple accepts a token only under the apns-topic of the app that minted it
-- and answers any other topic with 400 DeviceTokenNotForTopic, which the
-- fan-out treats as a permanently dead token and prunes. Every push used to go
-- out under the Flutter app's id (com.forkmesh.forkmesh), so the native iOS
-- app (com.forkmesh.nativeapp) had every token rejected and deleted on its
-- first push. The native app now sends its bundle id with the registration
-- (POST /api/accounts/register-device, "bundleId"), and the fan-out sends
-- each token under its own (app/src/apns.py apns_topic_for).
--
-- Existing rows were all registered by the Flutter app, which never sends the
-- field: '' keeps them on com.forkmesh.forkmesh, exactly as before.

ALTER TABLE apns_device_tokens ADD COLUMN bundle_id TEXT NOT NULL DEFAULT '';

-- The runtime schema text changed with it, so stamp the new fingerprint:
INSERT INTO schema_meta (k, v) VALUES (
  'schema_fingerprint:5af1f467ef3860fcb54a1fa94ee2cd7f4e0fefa4f8262697dd9aa9b8f0bddd9e',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
