-- Organization role matrix (issue #487): org_members.role grows from
-- owner/admin/member to owner/admin/maintainer/member/auditor/guest. The
-- vocabulary is plain text with no CHECK constraint, so no table changes.
--
-- The succession triggers enumerate the roles whose approval counts. That
-- list is now the organization.succession.approve row of the authorization
-- matrix in src/organization_rbac.py: owner, admin, maintainer, member.
-- Auditors are read-only and guests are not insiders, so neither votes.
-- CREATE TRIGGER IF NOT EXISTS never rewrites an existing trigger, hence the
-- explicit drop + recreate. Bodies otherwise match 0064_organization_succession.
DROP TRIGGER IF EXISTS trg_org_succession_approval_authorized;
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
          AND m.role IN ('owner','admin','maintainer','member')
          AND NEW.approver_bi<>c.owner_bi
          AND NEW.approver_bi<>c.successor_bi
    );
END;

DROP TRIGGER IF EXISTS trg_org_succession_completion_guard;
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
          AND m.role IN ('owner','admin','maintainer','member')
          AND a.approver_bi<>OLD.owner_bi
          AND a.approver_bi<>OLD.successor_bi
    )<OLD.approval_threshold;
END;

-- The runtime schema text changed (the two trigger bodies above), so stamp the
-- new fingerprint: ensure_schema's fast path then skips the full replay.
INSERT INTO schema_meta (k, v)
VALUES (
  'schema_fingerprint:9a913be6ed7a1eed1e4776614a7ee5ecb0adb5ff6a5f1f7b016f77f8ff663324',
  'applied'
)
ON CONFLICT(k) DO UPDATE SET v = excluded.v;
