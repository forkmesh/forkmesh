"""Pure policy for non-custodial organization-role succession.

Succession is deliberately narrower than account or repository ownership.  It
can only demote one organization owner to organization admin and promote one
explicitly named, existing organization member to owner.  No account, device,
repository ACL, private data, credential, key, address, or fund is part of this
policy model.
"""

from __future__ import annotations

import organization_rbac

import re


DAY_MS = 24 * 60 * 60 * 1000
MIN_INACTIVITY_DAYS = 30
DEFAULT_INACTIVITY_DAYS = 180
MAX_INACTIVITY_DAYS = 730
MIN_GRACE_DAYS = 7
DEFAULT_GRACE_DAYS = 30
MAX_GRACE_DAYS = 90
MIN_APPROVAL_THRESHOLD = 2
DEFAULT_APPROVAL_THRESHOLD = 2
MAX_APPROVAL_THRESHOLD = 20
MAX_BODY_BYTES = 8 * 1024
MAX_HISTORY = 100

# Succession is a governance vote: the roles that may approve one are the
# organization.succession.approve row of the authorization matrix, and the
# D1 triggers enumerate the same list (ORG_SUCCESSION_APPROVER_ROLE_SQL).
ORG_ROLES = frozenset(
    organization_rbac.ORG_AUTHORIZATION_MATRIX[
        "organization.succession.approve"][1])
CONFIG_FIELDS = frozenset({
    "successor",
    "inactivityDays",
    "graceDays",
    "approvalThreshold",
})
TRANSPORT_FIELDS = frozenset({"sessionToken"})
PROHIBITED_TRANSFER_FIELDS = frozenset({
    "wallet",
    "walletAddress",
    "funds",
    "privateKey",
    "privateKeys",
    "userAccount",
    "account",
    "credentials",
    "agentCredentials",
    "device",
    "devices",
    "deviceOwner",
    "deviceOwnership",
    "personalData",
    "repositoryOwner",
    "repositoryPermissions",
    "privateRepositoryPermissions",
})
EVENT_TYPES = frozenset({
    "configured",
    "configuration-disabled",
    "warning-issued",
    "grace-opened",
    "approval-recorded",
    "cancelled",
    "check-in",
    "completed",
    "completion-denied",
})
CASE_STATUSES = frozenset({"grace", "cancelled", "completed", "superseded"})

_NAME_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")


def valid_account_name(value):
    return bool(_NAME_RE.fullmatch(str(value or "").strip().lower()))


def _bounded_integer(value, default, minimum, maximum):
    if value is None or value == "":
        return default
    if isinstance(value, bool):
        return None
    try:
        parsed = int(value)
    except (TypeError, ValueError):
        return None
    if str(value).strip() not in (str(parsed),):
        return None
    return parsed if minimum <= parsed <= maximum else None


def normalize_config(data):
    """Return a bounded configuration or a stable validation error.

    Authentication transport is accepted but never copied into the returned
    policy.  Every other unknown field fails closed, with credential/account/
    wallet-shaped fields receiving an explicit scope error.
    """
    if not isinstance(data, dict):
        return None, "invalid_json"
    supplied = set(data)
    prohibited = supplied & PROHIBITED_TRANSFER_FIELDS
    if prohibited:
        return None, "non_transferable_field"
    if supplied - CONFIG_FIELDS - TRANSPORT_FIELDS:
        return None, "unsupported_field"

    successor = str(data.get("successor") or "").strip().lower()
    if not valid_account_name(successor):
        return None, "invalid_successor"
    inactivity = _bounded_integer(
        data.get("inactivityDays"),
        DEFAULT_INACTIVITY_DAYS,
        MIN_INACTIVITY_DAYS,
        MAX_INACTIVITY_DAYS,
    )
    grace = _bounded_integer(
        data.get("graceDays"),
        DEFAULT_GRACE_DAYS,
        MIN_GRACE_DAYS,
        MAX_GRACE_DAYS,
    )
    approvals = _bounded_integer(
        data.get("approvalThreshold"),
        DEFAULT_APPROVAL_THRESHOLD,
        MIN_APPROVAL_THRESHOLD,
        MAX_APPROVAL_THRESHOLD,
    )
    if inactivity is None:
        return None, "invalid_inactivity_threshold"
    if grace is None:
        return None, "invalid_grace_period"
    if approvals is None:
        return None, "invalid_approval_threshold"
    return {
        "successor": successor,
        "inactivityDays": inactivity,
        "graceDays": grace,
        "approvalThreshold": approvals,
    }, ""


def warning_lead_ms(inactivity_days):
    """Warn 10% ahead, bounded to 7..30 days."""
    inactivity_ms = int(inactivity_days) * DAY_MS
    return min(30 * DAY_MS, max(7 * DAY_MS, inactivity_ms // 10))


def inactivity_timeline(last_active_at, inactivity_days, now):
    last_active_at = max(0, int(last_active_at or 0))
    now = max(0, int(now or 0))
    inactivity_at = last_active_at + int(inactivity_days) * DAY_MS
    warning_at = inactivity_at - warning_lead_ms(inactivity_days)
    if now >= inactivity_at:
        state = "eligible"
    elif now >= warning_at:
        state = "warning"
    else:
        state = "active"
    return {
        "state": state,
        "warningAt": warning_at,
        "inactivityAt": inactivity_at,
        "remainingMs": max(0, inactivity_at - now),
    }


def eligible_approver(role, approver, owner, successor):
    """Require an independent, currently authorized organization member."""
    approver = str(approver or "").strip().lower()
    return (
        str(role or "").strip().lower() in ORG_ROLES
        and valid_account_name(approver)
        and approver not in {
            str(owner or "").strip().lower(),
            str(successor or "").strip().lower(),
        }
    )


def transfer_boundary():
    """Stable technical disclosure included with every status response."""
    return {
        "nonCustodial": True,
        "roleChangesOnly": [
            "configured organization owner becomes organization admin",
            "configured successor becomes organization owner",
        ],
        "preserved": [
            "repository ownership",
            "repository and private-repository permissions",
            "organization repository links",
            "organization teams and memberships",
        ],
        "neverTransferred": [
            "wallets or wallet addresses",
            "funds or rewards",
            "private keys",
            "user accounts or account credentials",
            "agent credentials or private agent data",
            "devices or device ownership",
            "personal data",
        ],
        "platformAdministratorOverride": False,
    }
