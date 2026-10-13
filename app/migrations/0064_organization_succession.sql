-- Non-custodial organization succession. This schema can change only the
-- configured organization's owner/admin roles. It has no columns for funds,
-- addresses, keys, credentials, user-account contents/ownership, devices,
-- personal profile data, repository ownership, repository links, or
-- private-repository permissions. Public org-member names identify the policy.

CREATE TABLE IF NOT EXISTS org_succession_policies (
    org_bi TEXT PRIMARY KEY,
    owner_bi TEXT NOT NULL,
    owner_name TEXT NOT NULL,
    successor_bi TEXT NOT NULL,
    successor_name TEXT NOT NULL,
    inactivity_days INTEGER NOT NULL CHECK (
        inactivity_days BETWEEN 30 AND 730),
    grace_days INTEGER NOT NULL CHECK (grace_days BETWEEN 7 AND 90),
    approval_threshold INTEGER NOT NULL CHECK (
        approval_threshold BETWEEN 2 AND 20),
    owner_last_active_at INTEGER NOT NULL,
    enabled INTEGER NOT NULL DEFAULT 1 CHECK (enabled IN (0, 1)),
    configured_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_org_succession_warning
    ON org_succession_policies(enabled, owner_last_active_at);

CREATE TABLE IF NOT EXISTS org_succession_cases (
    case_id TEXT PRIMARY KEY,
    org_bi TEXT NOT NULL,
    owner_bi TEXT NOT NULL,
    owner_name TEXT NOT NULL,
    successor_bi TEXT NOT NULL,
    successor_name TEXT NOT NULL,
    inactivity_days INTEGER NOT NULL CHECK (
        inactivity_days BETWEEN 30 AND 730),
    grace_days INTEGER NOT NULL CHECK (grace_days BETWEEN 7 AND 90),
    approval_threshold INTEGER NOT NULL CHECK (
        approval_threshold BETWEEN 2 AND 20),
    opened_at INTEGER NOT NULL,
    grace_ends_at INTEGER NOT NULL,
    status TEXT NOT NULL CHECK (
        status IN ('grace', 'cancelled', 'completed', 'superseded')),
    resolved_at INTEGER NOT NULL DEFAULT 0
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_org_succession_one_active_case
    ON org_succession_cases(org_bi) WHERE status='grace';
CREATE INDEX IF NOT EXISTS idx_org_succession_cases_history
    ON org_succession_cases(org_bi, opened_at);

CREATE TABLE IF NOT EXISTS org_succession_approvals (
    case_id TEXT NOT NULL,
    approver_bi TEXT NOT NULL,
    approved_at INTEGER NOT NULL,
    PRIMARY KEY (case_id, approver_bi)
);
CREATE INDEX IF NOT EXISTS idx_org_succession_approvals_case
    ON org_succession_approvals(case_id, approved_at);

CREATE TABLE IF NOT EXISTS org_succession_events (
    event_id TEXT PRIMARY KEY,
    org_bi TEXT NOT NULL,
    case_id TEXT NOT NULL DEFAULT '',
    actor_bi TEXT NOT NULL DEFAULT '',
    event_type TEXT NOT NULL CHECK (event_type IN (
        'configured', 'configuration-disabled', 'warning-issued',
        'grace-opened', 'approval-recorded', 'cancelled', 'check-in',
        'completed', 'completion-denied'
    )),
    status TEXT NOT NULL CHECK (status IN (
        'active', 'disabled', 'warning', 'grace', 'cancelled',
        'superseded', 'completed'
    )),
    created_at INTEGER NOT NULL,
    inactivity_days INTEGER NOT NULL DEFAULT 0,
    grace_days INTEGER NOT NULL DEFAULT 0,
    approval_threshold INTEGER NOT NULL DEFAULT 0,
    approval_count INTEGER NOT NULL DEFAULT 0,
    dedupe_key TEXT NOT NULL UNIQUE
);
CREATE INDEX IF NOT EXISTS idx_org_succession_events_history
    ON org_succession_events(org_bi, created_at);

-- Approvals are immutable and only independent, currently authorized members
-- may add one while the grace window is open.
CREATE TRIGGER IF NOT EXISTS trg_org_succession_approval_authorized
BEFORE INSERT ON org_succession_approvals
BEGIN
    SELECT RAISE(ABORT, 'succession_approver_not_authorized')
    WHERE NOT EXISTS (
        SELECT 1 FROM org_succession_cases c
        JOIN org_members m
          ON m.org_bi=c.org_bi AND m.member_bi=NEW.approver_bi
        WHERE c.case_id=NEW.case_id
          AND c.status='grace'
          AND NEW.approved_at<=c.grace_ends_at
          AND m.role IN ('owner','admin','member')
          AND NEW.approver_bi<>c.owner_bi
          AND NEW.approver_bi<>c.successor_bi
    );
END;
CREATE TRIGGER IF NOT EXISTS trg_org_succession_approval_no_update
BEFORE UPDATE ON org_succession_approvals
BEGIN
    SELECT RAISE(ABORT, 'succession_approvals_append_only');
END;
CREATE TRIGGER IF NOT EXISTS trg_org_succession_approval_no_delete
BEFORE DELETE ON org_succession_approvals
BEGIN
    SELECT RAISE(ABORT, 'succession_approvals_append_only');
END;

-- Event history is append-only, including cancellations and denied completion.
CREATE TRIGGER IF NOT EXISTS trg_org_succession_event_no_update
BEFORE UPDATE ON org_succession_events
BEGIN
    SELECT RAISE(ABORT, 'succession_events_append_only');
END;
CREATE TRIGGER IF NOT EXISTS trg_org_succession_event_no_delete
BEFORE DELETE ON org_succession_events
BEGIN
    SELECT RAISE(ABORT, 'succession_events_append_only');
END;

-- A case may only leave grace once, and its authorization snapshot is immutable.
CREATE TRIGGER IF NOT EXISTS trg_org_succession_case_immutable
BEFORE UPDATE ON org_succession_cases
WHEN NEW.case_id<>OLD.case_id
  OR NEW.org_bi<>OLD.org_bi
  OR NEW.owner_bi<>OLD.owner_bi
  OR NEW.owner_name<>OLD.owner_name
  OR NEW.successor_bi<>OLD.successor_bi
  OR NEW.successor_name<>OLD.successor_name
  OR NEW.inactivity_days<>OLD.inactivity_days
  OR NEW.grace_days<>OLD.grace_days
  OR NEW.approval_threshold<>OLD.approval_threshold
  OR NEW.opened_at<>OLD.opened_at
  OR NEW.grace_ends_at<>OLD.grace_ends_at
BEGIN
    SELECT RAISE(ABORT, 'succession_case_snapshot_immutable');
END;
CREATE TRIGGER IF NOT EXISTS trg_org_succession_case_transition
BEFORE UPDATE OF status ON org_succession_cases
WHEN NEW.status<>OLD.status
 AND NOT (
    OLD.status='grace'
    AND NEW.status IN ('cancelled','completed','superseded')
 )
BEGIN
    SELECT RAISE(ABORT, 'invalid_succession_transition');
END;
CREATE TRIGGER IF NOT EXISTS trg_org_succession_case_no_delete
BEFORE DELETE ON org_succession_cases
BEGIN
    SELECT RAISE(ABORT, 'succession_cases_append_only');
END;

-- Completion rechecks the live roster and approval threshold inside D1. A
-- platform role, stale approval, removed member, changed successor, or early
-- completion cannot satisfy this trigger.
CREATE TRIGGER IF NOT EXISTS trg_org_succession_completion_guard
BEFORE UPDATE OF status ON org_succession_cases
WHEN NEW.status='completed' AND OLD.status='grace'
BEGIN
    SELECT RAISE(ABORT, 'succession_grace_active')
    WHERE NEW.resolved_at<OLD.grace_ends_at;
    SELECT RAISE(ABORT, 'succession_owner_changed')
    WHERE NOT EXISTS (
        SELECT 1 FROM org_members
        WHERE org_bi=OLD.org_bi AND member_bi=OLD.owner_bi AND role='owner'
    );
    SELECT RAISE(ABORT, 'succession_successor_changed')
    WHERE NOT EXISTS (
        SELECT 1 FROM org_members
        WHERE org_bi=OLD.org_bi AND member_bi=OLD.successor_bi
          AND role IN ('admin','member')
    );
    SELECT RAISE(ABORT, 'succession_approval_threshold_not_met')
    WHERE (
        SELECT COUNT(*) FROM org_succession_approvals a
        JOIN org_members m
          ON m.org_bi=OLD.org_bi AND m.member_bi=a.approver_bi
        WHERE a.case_id=OLD.case_id
          AND m.role IN ('owner','admin','member')
          AND a.approver_bi<>OLD.owner_bi
          AND a.approver_bi<>OLD.successor_bi
    )<OLD.approval_threshold;
END;

-- These are the only succession side effects: two role values in this org.
CREATE TRIGGER IF NOT EXISTS trg_org_succession_role_transfer
AFTER UPDATE OF status ON org_succession_cases
WHEN NEW.status='completed' AND OLD.status='grace'
BEGIN
    UPDATE org_members SET role='admin'
    WHERE org_bi=OLD.org_bi AND member_bi=OLD.owner_bi AND role='owner';
    UPDATE org_members SET role='owner'
    WHERE org_bi=OLD.org_bi AND member_bi=OLD.successor_bi
      AND role IN ('admin','member');
END;

-- Organization deletion leaves history intact but cannot leave a live policy
-- behind for a later organization that reuses the same public name.
CREATE TRIGGER IF NOT EXISTS trg_org_succession_org_delete
BEFORE DELETE ON orgs
WHEN EXISTS (
    SELECT 1 FROM org_succession_policies
    WHERE org_bi=OLD.org_bi AND enabled=1
)
BEGIN
    INSERT OR IGNORE INTO org_succession_events
        (event_id,org_bi,case_id,actor_bi,event_type,status,created_at,
         inactivity_days,grace_days,approval_threshold,approval_count,
         dedupe_key)
    SELECT
        'org-delete:' || p.org_bi || ':' || p.updated_at,
        p.org_bi,
        COALESCE((
            SELECT case_id FROM org_succession_cases
            WHERE org_bi=p.org_bi AND status='grace' LIMIT 1
        ), ''),
        '',
        'configuration-disabled',
        'disabled',
        p.updated_at,
        p.inactivity_days,
        p.grace_days,
        p.approval_threshold,
        0,
        'org-delete:' || p.org_bi || ':' || p.updated_at
    FROM org_succession_policies p
    WHERE p.org_bi=OLD.org_bi AND p.enabled=1;
    UPDATE org_succession_cases
    SET status='cancelled',resolved_at=MAX(resolved_at, opened_at)
    WHERE org_bi=OLD.org_bi AND status='grace';
    UPDATE org_succession_policies
    SET enabled=0
    WHERE org_bi=OLD.org_bi AND enabled=1;
END;
