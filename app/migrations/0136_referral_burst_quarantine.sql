-- Quarantine referral-credit bursts that predate the independently-active
-- member gate. Fifteen automatically credited accounts inside one hour
-- from the same referrer are removed from rank/rewards as one review cohort.
--
-- This is deliberately reversible evidence preservation, not deletion:
-- attribution, encrypted signup evidence, verification time, and risk flags
-- remain available to administrators. A reviewed cohort can be restored by
-- changing its decisions back to credited; automatic finalization will not
-- restore a cohort while the burst condition remains.

CREATE TABLE IF NOT EXISTS quarantined_users (
  user_bi          TEXT PRIMARY KEY,
  data             TEXT NOT NULL,
  email_bi         TEXT,
  username         TEXT NOT NULL,
  is_admin         INTEGER NOT NULL DEFAULT 0,
  ip_bi            TEXT,
  enable_outreach  INTEGER NOT NULL DEFAULT 0,
  referrer_bi      TEXT NOT NULL DEFAULT '',
  reason_codes     TEXT NOT NULL DEFAULT '[]',
  status           TEXT NOT NULL DEFAULT 'pending_verification'
    CHECK (status IN ('pending_verification','rejected','restored')),
  quarantined_at   INTEGER NOT NULL,
  review_after     INTEGER NOT NULL DEFAULT 0,
  reviewed_at      INTEGER NOT NULL DEFAULT 0,
  reviewed_by      TEXT NOT NULL DEFAULT ''
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_quarantined_users_email
  ON quarantined_users(email_bi)
  WHERE email_bi IS NOT NULL AND email_bi <> '';
CREATE INDEX IF NOT EXISTS idx_quarantined_users_referrer
  ON quarantined_users(referrer_bi, status, quarantined_at DESC);

CREATE TABLE IF NOT EXISTS quarantined_nodes (
  node_bi             TEXT PRIMARY KEY,
  user_bi             TEXT,
  pubkey              TEXT,
  data                TEXT NOT NULL,
  name                TEXT,
  last_seen           INTEGER NOT NULL DEFAULT 0,
  quarantined_user_bi TEXT NOT NULL,
  status              TEXT NOT NULL DEFAULT 'pending_verification'
    CHECK (status IN ('pending_verification','rejected','restored')),
  quarantined_at      INTEGER NOT NULL,
  reviewed_at         INTEGER NOT NULL DEFAULT 0,
  reviewed_by         TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_quarantined_nodes_user
  ON quarantined_nodes(quarantined_user_bi, status);

CREATE TABLE IF NOT EXISTS referral_referrer_controls (
  referrer_bi  TEXT PRIMARY KEY,
  state        TEXT NOT NULL CHECK (state IN ('active','frozen','blocked')),
  reason_codes TEXT NOT NULL DEFAULT '[]',
  updated_at   INTEGER NOT NULL,
  review_after INTEGER NOT NULL DEFAULT 0,
  reviewed_at  INTEGER NOT NULL DEFAULT 0,
  reviewed_by  TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_referral_referrer_controls_state
  ON referral_referrer_controls(state, updated_at DESC);

-- Enforce a frozen referrer at the ledger boundary as well as in Worker code.
-- This also protects the cutover window while an older Worker is still live.
CREATE TRIGGER IF NOT EXISTS referral_frozen_credit_update
BEFORE UPDATE OF decision ON referral_attributions
FOR EACH ROW
WHEN NEW.decision = 'credited' AND EXISTS (
  SELECT 1 FROM referral_referrer_controls
  WHERE referrer_bi = NEW.referrer_bi
    AND state IN ('frozen','blocked')
)
BEGIN
  SELECT RAISE(ABORT, 'referrer rewards frozen');
END;

CREATE TABLE IF NOT EXISTS referral_click_claims (
  click_id     TEXT PRIMARY KEY,
  referrer_bi  TEXT NOT NULL,
  created_at   INTEGER NOT NULL,
  expires_at   INTEGER NOT NULL,
  consumed_at  INTEGER NOT NULL DEFAULT 0,
  referred_bi  TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_referral_click_claims_expiry
  ON referral_click_claims(expires_at, consumed_at);

ALTER TABLE referral_attributions
  ADD COLUMN client_fingerprint_bi TEXT NOT NULL DEFAULT '';
ALTER TABLE referral_attributions
  ADD COLUMN referral_click_id TEXT NOT NULL DEFAULT '';
ALTER TABLE referral_attributions
  ADD COLUMN risk_score INTEGER NOT NULL DEFAULT 0 CHECK (risk_score >= 0);
CREATE INDEX IF NOT EXISTS idx_referral_attributions_client
  ON referral_attributions(client_fingerprint_bi, created_at DESC);
CREATE UNIQUE INDEX IF NOT EXISTS idx_referral_attributions_click
  ON referral_attributions(referral_click_id)
  WHERE referral_click_id <> '';
-- Network reuse remains a risk signal, not a uniqueness rule: households,
-- schools, offices, and carrier NATs can legitimately share one address.
DROP INDEX IF EXISTS idx_referral_attributions_credit_slot;

-- Freeze the referring account before changing any decision rows. Count every
-- attributed signup: excluding rejected rows would let an automated cohort
-- evade the velocity rule by deliberately tripping a per-account check.
INSERT INTO referral_referrer_controls
  (referrer_bi,state,reason_codes,updated_at,review_after)
SELECT DISTINCT referral_attributions.referrer_bi,
       'frozen', '["referrer_signup_burst"]',
       CAST(strftime('%s','now') AS INTEGER) * 1000,
       CAST(strftime('%s','now') AS INTEGER) * 1000 + 259200000
FROM referral_attributions
WHERE EXISTS (
    SELECT 1
    FROM referral_attributions AS window_start
    WHERE window_start.referrer_bi = referral_attributions.referrer_bi
      AND window_start.created_at
          BETWEEN referral_attributions.created_at - 3600000
              AND referral_attributions.created_at
      AND (
        SELECT COUNT(*)
        FROM referral_attributions AS candidate
        WHERE candidate.referrer_bi = window_start.referrer_bi
          AND candidate.created_at >= window_start.created_at
          AND candidate.created_at < window_start.created_at + 3600000
      ) >= 15
  )
ON CONFLICT(referrer_bi) DO UPDATE SET
  state = 'frozen',
  reason_codes = excluded.reason_codes,
  updated_at = excluded.updated_at,
  review_after = excluded.review_after,
  reviewed_at = 0,
  reviewed_by = '';

UPDATE referral_attributions
SET decision = 'pending',
    credited_at = 0,
    risk_score = MAX(risk_score, 100),
    risk_flags = CASE
      WHEN instr(risk_flags, '"referrer_signup_burst"') > 0 THEN risk_flags
      WHEN json_valid(risk_flags) THEN
        json_insert(risk_flags, '$[#]', 'referrer_signup_burst')
      ELSE '["referrer_signup_burst"]'
    END
WHERE decision = 'credited'
  AND referrer_bi IN (
    SELECT referrer_bi FROM referral_referrer_controls
    WHERE state IN ('frozen','blocked')
  );

UPDATE referral_attributions
SET risk_flags = CASE
      WHEN NOT json_valid(risk_flags) THEN
        '["referrer_signup_burst","referrer_rewards_frozen"]'
      WHEN instr(risk_flags, '"referrer_signup_burst"') > 0
       AND instr(risk_flags, '"referrer_rewards_frozen"') > 0 THEN risk_flags
      WHEN instr(risk_flags, '"referrer_signup_burst"') > 0 THEN
        json_insert(risk_flags, '$[#]', 'referrer_rewards_frozen')
      WHEN instr(risk_flags, '"referrer_rewards_frozen"') > 0 THEN
        json_insert(risk_flags, '$[#]', 'referrer_signup_burst')
      ELSE json_insert(risk_flags,
        '$[#]', 'referrer_signup_burst',
        '$[#]', 'referrer_rewards_frozen')
    END
WHERE referrer_bi IN (
  SELECT referrer_bi FROM referral_referrer_controls
  WHERE state IN ('frozen','blocked')
);

-- Preserve the entire encrypted user row outside the active roster before
-- revoking access and deleting it from users. Only accounts in the detected
-- burst referrer move. This deliberately removes the whole referred cohort
-- from the active roster: the evidence is strong enough to freeze the source,
-- but each account remains individually restorable after verification.
INSERT INTO quarantined_users
  (user_bi,data,email_bi,username,is_admin,ip_bi,enable_outreach,
   referrer_bi,reason_codes,status,quarantined_at,review_after)
SELECT u.user_bi,u.data,u.email_bi,u.username,u.is_admin,u.ip_bi,
       u.enable_outreach,a.referrer_bi,a.risk_flags,'pending_verification',
       CAST(strftime('%s','now') AS INTEGER) * 1000,
       CAST(strftime('%s','now') AS INTEGER) * 1000 + 259200000
FROM users AS u
JOIN referral_attributions AS a ON a.referred_bi = u.user_bi
WHERE a.referrer_bi IN (
  SELECT referrer_bi FROM referral_referrer_controls
  WHERE state IN ('frozen','blocked')
)
ON CONFLICT(user_bi) DO UPDATE SET
  reason_codes = excluded.reason_codes,
  status = 'pending_verification',
  quarantined_at = excluded.quarantined_at,
  review_after = excluded.review_after,
  reviewed_at = 0,
  reviewed_by = '';

UPDATE account_devices
SET enabled = 0,
    revoked_at = CAST(strftime('%s','now') AS INTEGER) * 1000
WHERE account_bi IN (
  SELECT user_bi FROM quarantined_users
  WHERE status = 'pending_verification'
);
UPDATE account_sessions
SET revoked_at = CAST(strftime('%s','now') AS INTEGER) * 1000
WHERE account_bi IN (
  SELECT user_bi FROM quarantined_users
  WHERE status = 'pending_verification'
);
INSERT INTO quarantined_nodes
  (node_bi,user_bi,pubkey,data,name,last_seen,quarantined_user_bi,status,
   quarantined_at)
SELECT n.node_bi,n.user_bi,n.pubkey,n.data,n.name,n.last_seen,
       CASE WHEN n.user_bi IN (
         SELECT user_bi FROM quarantined_users
         WHERE status = 'pending_verification'
       ) THEN n.user_bi ELSE n.node_bi END,
       'pending_verification',
       CAST(strftime('%s','now') AS INTEGER) * 1000
FROM nodes AS n
WHERE n.user_bi IN (
    SELECT user_bi FROM quarantined_users
    WHERE status = 'pending_verification'
  )
   OR n.node_bi IN (
    SELECT user_bi FROM quarantined_users
    WHERE status = 'pending_verification'
  )
ON CONFLICT(node_bi) DO UPDATE SET
  status = 'pending_verification',
  quarantined_at = excluded.quarantined_at,
  reviewed_at = 0,
  reviewed_by = '';
DELETE FROM nodes
WHERE user_bi IN (
    SELECT user_bi FROM quarantined_users
    WHERE status = 'pending_verification'
  )
   OR node_bi IN (
    SELECT user_bi FROM quarantined_users
    WHERE status = 'pending_verification'
  );
DELETE FROM account_presence
WHERE name_bi IN (
  SELECT user_bi FROM quarantined_users
  WHERE status = 'pending_verification'
);
DELETE FROM users
WHERE user_bi IN (
  SELECT user_bi FROM quarantined_users
  WHERE status = 'pending_verification'
);

-- The attribution ledger is authoritative; cached aggregate columns must not
-- retain credit removed above.
UPDATE referral_stats
SET signups = (
  SELECT COUNT(*)
  FROM referral_attributions AS attribution
  WHERE attribution.referrer_bi = referral_stats.referrer_bi
    AND attribution.decision = 'credited'
);

-- Connector is an automatic first-qualified-referral badge. Revoke only the
-- system grant when its account now has no qualifying referral; an explicit
-- administrator grant is never touched.
DELETE FROM badge_awards
WHERE badge_slug = 'first_referral'
  AND granted_by = 'system'
  AND NOT EXISTS (
    SELECT 1
    FROM referral_attributions AS attribution
    WHERE attribution.referrer_bi = badge_awards.account_bi
      AND attribution.decision = 'credited'
  );

INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:cbb84f825be4b5a58cde821a93c2415e11484ece084d9cd0bc657b28c3429155',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
