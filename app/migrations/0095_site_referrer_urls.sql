-- Retain the latest bounded, sanitized full referring URL for each existing
-- aggregate hostname row. Historical paths cannot be reconstructed; they fill
-- on the next real referral from that hostname.
ALTER TABLE site_referrers
ADD COLUMN last_url TEXT NOT NULL DEFAULT '';
