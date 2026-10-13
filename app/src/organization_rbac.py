"""Organization role-based authorization matrix (issue #487).

One table answers "may this organization role do this?" for every
organization surface in the Worker. Every decision is deny-by-default:

* a role that is not in ``ORG_ROLES`` (including "", None and a stale value
  left in ``org_members.role``) is allowed nothing;
* a capability that is not in ``ORG_AUTHORIZATION_MATRIX`` is allowed to no
  role, owners included — a typo in a call site denies instead of granting;
* the matrix names the roles that hold a capability; nothing is inferred
  from rank, so adding a role grants it nothing until a row says so.

Roles, strongest first:

    owner       runs the organization; the only role that can dissolve it or
                hand out owner/admin.
    admin       administers members, teams, repositories, integrations.
    maintainer  ships code: holds ``maintain`` on every linked repository and
                may queue agents, but administers nothing.
    member      collaborates: ``read`` on linked repositories, raised only by
                an explicit team grant; may queue agents on the Engineering
                floor like today.
    auditor     read-only observer: sees the roster, agent work and the
                organization's internal surfaces, can never write or push.
    guest       outside collaborator: sees the public organization profile
                and the repositories a team explicitly grants, read-only; is
                not an insider for world floors, chat channels or task boards.

The module has no Worker dependencies so tests import it directly and the
documentation table in www/docs/organization-roles.md is checked against it.
"""

ORG_ROLES = ("owner", "admin", "maintainer", "member", "auditor", "guest")

# Roles that count as "inside" the organization: world floors, office chat
# channels, task boards and succession votes are member surfaces, and a guest
# is deliberately not a member of any of them.
ORG_INSIDER_ROLES = ("owner", "admin", "maintainer", "member", "auditor")

# Roles that administer the organization itself. Only an owner may grant or
# revoke them (see org_role_can_assign).
ORG_ADMINISTRATIVE_ROLES = ("owner", "admin")

# SQL fragments for roster queries that must agree with the matrix. Keep the
# role order identical to ORG_ROLES so the strings are greppable.
ORG_INSIDER_ROLE_SQL = "('owner','admin','maintainer','member','auditor')"
ORG_SUCCESSION_APPROVER_ROLE_SQL = "('owner','admin','maintainer','member')"

_READ = "read"
_WRITE = "write"

# capability -> (kind, roles). ``kind`` is documentation and a test contract:
# no ``write`` capability may ever list auditor or guest.
ORG_AUTHORIZATION_MATRIX = {
    # Organization identity and settings.
    "organization.read": (
        _READ, ("owner", "admin", "maintainer", "member", "auditor", "guest")),
    "organization.roster.read": (
        _READ, ("owner", "admin", "maintainer", "member", "auditor")),
    "organization.settings.write": (_WRITE, ("owner", "admin")),
    "organization.dissolve": (_WRITE, ("owner",)),
    # Membership administration. Which roles a caller may hand out is the
    # separate org_role_can_assign rule below.
    "organization.members.manage": (_WRITE, ("owner", "admin")),
    "organization.roles.administer": (_WRITE, ("owner",)),
    "organization.teams.manage": (_WRITE, ("owner", "admin")),
    # Repositories.
    "organization.repositories.link": (_WRITE, ("owner", "admin")),
    "organization.repositories.import": (_WRITE, ("owner", "admin")),
    "organization.issues.manage": (_WRITE, ("owner", "admin")),
    "organization.pull_rules.manage": (_WRITE, ("owner", "admin")),
    # Integrations.
    "organization.fediverse.manage": (_WRITE, ("owner", "admin")),
    "organization.bots.link": (
        _WRITE, ("owner", "admin", "maintainer", "member")),
    "organization.bots.manage": (_WRITE, ("owner", "admin")),
    # Agents (the Engineering-floor team gate still applies on top).
    "organization.agents.read": (
        _READ, ("owner", "admin", "maintainer", "member", "auditor")),
    "organization.agents.queue": (
        _WRITE, ("owner", "admin", "maintainer", "member")),
    "organization.tasks.administer": (_WRITE, ("owner", "admin")),
    # World building: "restricted" floors admit insiders, "private" ones only
    # administrators; "public" needs no role and is not a capability.
    "organization.world.restricted": (
        _READ, ("owner", "admin", "maintainer", "member", "auditor")),
    "organization.world.private": (_READ, ("owner", "admin")),
    "organization.chat.channels": (
        _WRITE, ("owner", "admin", "maintainer", "member")),
    # Governance.
    "organization.succession.approve": (
        _WRITE, ("owner", "admin", "maintainer", "member")),
}

ORG_CAPABILITIES = tuple(sorted(ORG_AUTHORIZATION_MATRIX))

# Repository permission a role holds on every linked repository before any
# team grant ("" = none). ``ceiling`` caps what a team grant can raise it to:
# an auditor or guest is read-only no matter which team lists them.
_ROLE_REPO_PERMISSION = {
    "owner": ("admin", "admin"),
    "admin": ("admin", "admin"),
    "maintainer": ("maintain", "admin"),
    "member": ("read", "admin"),
    "auditor": ("read", "read"),
    "guest": ("", "read"),
}


def clean_org_role(role):
    """The canonical role string, or "" for anything outside ORG_ROLES."""
    text = str(role or "").strip().lower() if role is not None else ""
    return text if text in ORG_ROLES else ""


def org_role_allows(role, capability):
    """True only when the matrix explicitly lists ``role`` for ``capability``."""
    role = clean_org_role(role)
    if not role:
        return False
    entry = ORG_AUTHORIZATION_MATRIX.get(str(capability or ""))
    if entry is None:
        return False
    return role in entry[1]


def org_role_is_insider(role):
    return clean_org_role(role) in ORG_INSIDER_ROLES


def org_role_is_administrative(role):
    return clean_org_role(role) in ORG_ADMINISTRATIVE_ROLES


def org_role_repo_permission(role):
    """Baseline repository permission for ``role`` ("" for none/unknown)."""
    return _ROLE_REPO_PERMISSION.get(clean_org_role(role), ("", ""))[0]


def org_role_repo_permission_ceiling(role):
    """Strongest repository permission a team grant may raise ``role`` to."""
    return _ROLE_REPO_PERMISSION.get(clean_org_role(role), ("", ""))[1]


def org_role_can_assign(caller_role, target_role, existing_role=""):
    """May ``caller_role`` set a member to ``target_role``?

    ``existing_role`` is the member's current role ("" when adding). Owners
    may assign anything; admins may only hand out and take away the
    non-administrative roles, so an admin can neither promote to owner/admin
    nor demote an owner/admin. Everyone else may assign nothing.
    """
    caller = clean_org_role(caller_role)
    target = clean_org_role(target_role)
    if not caller or not target:
        return False
    existing = clean_org_role(existing_role)
    if not org_role_allows(caller, "organization.members.manage"):
        return False
    if org_role_allows(caller, "organization.roles.administer"):
        return True
    if target in ORG_ADMINISTRATIVE_ROLES or existing in ORG_ADMINISTRATIVE_ROLES:
        return False
    return True


def org_role_can_remove(caller_role, existing_role):
    """May ``caller_role`` remove a member currently holding ``existing_role``?"""
    caller = clean_org_role(caller_role)
    existing = clean_org_role(existing_role)
    if not caller or not existing:
        return False
    if not org_role_allows(caller, "organization.members.manage"):
        return False
    if existing in ORG_ADMINISTRATIVE_ROLES:
        return org_role_allows(caller, "organization.roles.administer")
    return True


def org_authorization_matrix_rows():
    """[(capability, kind, {role: bool})] in a stable order, for docs/tests."""
    rows = []
    for capability in ORG_CAPABILITIES:
        kind, roles = ORG_AUTHORIZATION_MATRIX[capability]
        rows.append((capability, kind,
                     {role: role in roles for role in ORG_ROLES}))
    return rows


def org_authorization_matrix_markdown():
    """The matrix as the Markdown table published in the documentation."""
    header = "| Capability | Kind | " + " | ".join(ORG_ROLES) + " |"
    divider = "| --- | --- | " + " | ".join("---" for _ in ORG_ROLES) + " |"
    lines = [header, divider]
    for capability, kind, cells in org_authorization_matrix_rows():
        lines.append(
            "| `" + capability + "` | " + kind + " | "
            + " | ".join("yes" if cells[role] else "no" for role in ORG_ROLES)
            + " |")
    return "\n".join(lines)
