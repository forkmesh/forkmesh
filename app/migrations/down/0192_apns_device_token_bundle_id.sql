-- Reverse 0192_apns_device_token_bundle_id.sql. Every token is then sent under
-- the previous app's topic again, so native-app tokens are refused by Apple and
-- pruned on their next push.
ALTER TABLE apns_device_tokens DROP COLUMN bundle_id;
