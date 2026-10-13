-- Reverse 0183_organization_rbac_roles.sql.
--
-- Restores the 0064 succession triggers, whose approver list is
-- owner/admin/member. Rows in org_members holding the newer roles
-- (maintainer, auditor, guest) are left untouched: the Worker build that
-- pairs with this rollback treats any role outside its vocabulary as no
-- membership at all (deny-by-default), so nothing is granted by leaving them.
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
          AND m.role IN ('owner','admin','member')
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
          AND m.role IN ('owner','admin','member')
          AND a.approver_bi<>OLD.owner_bi
          AND a.approver_bi<>OLD.successor_bi
    )<OLD.approval_threshold;
END;
