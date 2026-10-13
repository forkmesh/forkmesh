-- Enforce invitation and logo quotas inside the same SQLite write that creates
-- the row. Application-side preflight checks remain useful for friendly errors,
-- but cannot protect against two Worker requests racing between COUNT/INSERT.

CREATE INDEX IF NOT EXISTS idx_repository_logo_suggestions_proposer
    ON repository_logo_suggestions(
        repo_id, proposer_bi, status, created_at);

CREATE TRIGGER IF NOT EXISTS trg_logo_pending_proposer_limit
BEFORE INSERT ON repository_logo_suggestions
WHEN NEW.status='pending' AND NEW.official=0
 AND (SELECT COUNT(*) FROM repository_logo_suggestions
      WHERE repo_id=NEW.repo_id AND proposer_bi=NEW.proposer_bi
        AND status='pending') >= 5
BEGIN
    SELECT RAISE(ABORT, 'logo_pending_proposer_limit');
END;

-- Official replacement is one statement: prune only enough non-official
-- history to stay at 50, supersede the old official, then let the new insert
-- complete under the partial unique index. If the insert fails, trigger effects
-- roll back with it.
CREATE TRIGGER IF NOT EXISTS trg_logo_official_insert
BEFORE INSERT ON repository_logo_suggestions
WHEN NEW.official=1
BEGIN
    DELETE FROM repository_logo_suggestions
    WHERE id IN (
        SELECT id FROM repository_logo_suggestions
        WHERE repo_id=NEW.repo_id AND official=0
        ORDER BY CASE WHEN status IN ('rejected','superseded')
                      THEN 0 ELSE 1 END,
                 created_at ASC
        LIMIT CASE
            WHEN (SELECT COUNT(*) FROM repository_logo_suggestions
                  WHERE repo_id=NEW.repo_id) >= 50
            THEN (SELECT COUNT(*) FROM repository_logo_suggestions
                  WHERE repo_id=NEW.repo_id) - 49
            ELSE 0
        END
    );
    UPDATE repository_logo_suggestions
    SET official=0,
        status=CASE WHEN status='approved' THEN 'superseded' ELSE status END
    WHERE repo_id=NEW.repo_id AND official=1;
END;

CREATE TRIGGER IF NOT EXISTS trg_logo_official_approval
BEFORE UPDATE OF official ON repository_logo_suggestions
WHEN NEW.official=1 AND OLD.official<>1
BEGIN
    UPDATE repository_logo_suggestions
    SET official=0,
        status=CASE WHEN status='approved' THEN 'superseded' ELSE status END
    WHERE repo_id=NEW.repo_id AND id<>OLD.id AND official=1;
END;

-- Community submissions may occupy at most 49 total history rows. The fiftieth
-- slot remains available to an owner/moderator replacement, whose official=1
-- insert bypasses this trigger and may prune one non-official history row.
CREATE TRIGGER IF NOT EXISTS trg_logo_community_repository_limit
BEFORE INSERT ON repository_logo_suggestions
WHEN NEW.status='pending' AND NEW.official=0
 AND (SELECT COUNT(*) FROM repository_logo_suggestions
      WHERE repo_id=NEW.repo_id) >= 49
BEGIN
    SELECT RAISE(ABORT, 'logo_community_repository_limit');
END;

CREATE TRIGGER IF NOT EXISTS trg_invitation_optout_reservation
BEFORE INSERT ON contributor_invitations
WHEN NEW.status='pending'
 AND EXISTS (SELECT 1 FROM contributor_invitation_optouts
             WHERE email_bi=NEW.email_bi)
BEGIN
    SELECT RAISE(ABORT, 'invitation_recipient_opted_out');
END;

CREATE TRIGGER IF NOT EXISTS trg_invitation_recipient_cooldown
BEFORE INSERT ON contributor_invitations
WHEN NEW.status='pending'
 AND EXISTS (
     SELECT 1 FROM contributor_invitations
     WHERE email_bi=NEW.email_bi
       AND status IN ('pending','sent')
       AND created_at > NEW.created_at - 2592000000
 )
BEGIN
    SELECT RAISE(ABORT, 'invitation_recipient_cooldown');
END;

CREATE TRIGGER IF NOT EXISTS trg_invitation_daily_limit
BEFORE INSERT ON contributor_invitations
WHEN NEW.status='pending'
 AND COALESCE((
     SELECT sent_count FROM contributor_invitation_rate
     WHERE inviter_bi=NEW.inviter_bi
       AND day_bucket=CAST(NEW.created_at / 86400000 AS INTEGER)
       AND repo_id='*'
 ), 0) >= 20
BEGIN
    SELECT RAISE(ABORT, 'invitation_daily_limit');
END;

CREATE TRIGGER IF NOT EXISTS trg_invitation_repository_daily_limit
BEFORE INSERT ON contributor_invitations
WHEN NEW.status='pending'
 AND COALESCE((
     SELECT sent_count FROM contributor_invitation_rate
     WHERE inviter_bi=NEW.inviter_bi
       AND day_bucket=CAST(NEW.created_at / 86400000 AS INTEGER)
       AND repo_id=NEW.repo_id
 ), 0) >= 10
BEGIN
    SELECT RAISE(ABORT, 'invitation_repository_daily_limit');
END;

CREATE TRIGGER IF NOT EXISTS trg_invitation_repository_history_limit
BEFORE INSERT ON contributor_invitations
WHEN NEW.status='pending'
 AND (SELECT COUNT(*) FROM contributor_invitations
      WHERE repo_id=NEW.repo_id) >= 500
BEGIN
    SELECT RAISE(ABORT, 'invitation_repository_history_limit');
END;

-- A pending row is the durable reservation. Its two counters are incremented
-- atomically with creation, before any email side effect occurs.
CREATE TRIGGER IF NOT EXISTS trg_invitation_reserve_rate
AFTER INSERT ON contributor_invitations
WHEN NEW.status='pending'
BEGIN
    INSERT INTO contributor_invitation_rate
        (inviter_bi, day_bucket, repo_id, sent_count)
    VALUES
        (NEW.inviter_bi, CAST(NEW.created_at / 86400000 AS INTEGER), '*', 1)
    ON CONFLICT(inviter_bi,day_bucket,repo_id)
    DO UPDATE SET sent_count=sent_count+1;

    INSERT INTO contributor_invitation_rate
        (inviter_bi, day_bucket, repo_id, sent_count)
    VALUES
        (NEW.inviter_bi, CAST(NEW.created_at / 86400000 AS INTEGER),
         NEW.repo_id, 1)
    ON CONFLICT(inviter_bi,day_bucket,repo_id)
    DO UPDATE SET sent_count=sent_count+1;
END;

-- Delivery failure deletes the still-pending reservation. Counter rollback is
-- part of that same delete, so a mail outage cannot consume invitation quota.
CREATE TRIGGER IF NOT EXISTS trg_invitation_release_rate
AFTER DELETE ON contributor_invitations
WHEN OLD.status='pending'
BEGIN
    UPDATE contributor_invitation_rate
    SET sent_count=MAX(0, sent_count-1)
    WHERE inviter_bi=OLD.inviter_bi
      AND day_bucket=CAST(OLD.created_at / 86400000 AS INTEGER)
      AND repo_id IN ('*', OLD.repo_id);

    DELETE FROM contributor_invitation_rate
    WHERE inviter_bi=OLD.inviter_bi
      AND day_bucket=CAST(OLD.created_at / 86400000 AS INTEGER)
      AND repo_id IN ('*', OLD.repo_id)
      AND sent_count<=0;
END;
