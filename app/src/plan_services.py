"""Self-serve plan usage meters and the trial register (issue #488).

The Workers adapter in ``entry.py`` owns the session and D1 primitives.  This
module stays importable under ordinary CPython so trial and usage behavior can
be tested without a Cloudflare runtime.

Boundaries this module must keep:
- The trial entitlement is a promotion-scope ``role_grants`` row with its own
  expiry.  A billing provider's revoke never touches that scope, so no
  provider state or failure can revoke, extend, or resurrect a trial — and letting a trial lapse never touches portable identity or public
  protocol access, because a missing role grant only withholds member perks.
- Usage limits reported here are the plan-independent operational caps their
  owning routes already enforce.  No numeric *plan allowance* may appear here
  until it is published in business-model.json with metering tests in the
  same change (www/docs/business-model.md, "Free plan boundary").
"""

from __future__ import annotations


BODY_MAX_BYTES = 16 * 1024
TRIAL_TIER = "supporter"
TRIAL_DURATION_MS = 14 * 24 * 60 * 60 * 1000
# Blind-index subject for the trial's role_grants scope_bi. A fixed subject
# (unlike the hackathon's per-campaign scope) because an account gets exactly
# one trial ever, enforced by the plan_trials primary key.
TRIAL_SCOPE_SUBJECT = "plan-trial"

# meter key -> (limit, human label). Enforcement lives with each owning route
# (catalog_routes.py, route_handlers.py, ssh_keys.py, entry.py); this module
# only reports headroom. test_plan_services.py pins these values to the
# enforcing constants so the meter can never drift from the actual cap.
USAGE_METERS = {
    "repositories": (100, "Published repositories"),
    "organizationsOwned": (10, "Organizations you own"),
    "sshKeys": (20, "SSH keys"),
    "storedNotifications": (500, "Stored notifications"),
}


def _response(runtime, data, status=200, allow=""):
    headers = {"x-content-type-options": "nosniff"}
    if allow:
        headers["allow"] = allow
    return runtime.response(
        data,
        status=status,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers=headers,
    )


async def _role_window(runtime, account_bi, now, scope_type):
    """How long the account's member roles run under one grant scope.

    One aggregate row, because this module's runtime adapter reads single
    rows; a multi-row read would fail and silently report no membership.
    """

    try:
        row = await runtime.d1_first(
            "SELECT MAX(CASE WHEN role='pro_member' THEN expires_at END) "
            "AS pro,MAX(CASE WHEN role='supporting_member' THEN expires_at "
            "END) AS supporter FROM role_grants WHERE account_bi=? "
            "AND scope_type=? AND revoked_at=0 AND expires_at>?",
            account_bi, scope_type, now,
        )
    except Exception:
        return None
    pro = int((row or {}).get("pro") or 0)
    supporter = int((row or {}).get("supporter") or 0)
    if pro > now:
        return {"tier": "pro", "expiresAt": pro}
    if supporter > now:
        return {"tier": "supporter", "expiresAt": supporter}
    return None


async def _active_membership(runtime, account_bi, now):
    """The account's paid membership, or None.

    Payment providers (Patreon today) write the entitlement itself as a
    platform-scope role grant, so this reads the grant rather than any one
    provider's own tables: a membership bought anywhere counts the same, and
    a provider that goes away takes no plan logic with it.
    """

    return await _role_window(runtime, account_bi, now, "platform")


async def promotion_status(runtime, account_bi, now):
    """A membership somebody earned rather than bought.

    Hackathon prizes and this module's own trial are written to role_grants
    under scope_type='promotion', which no provider sync recomputes. They are
    reported in their own field, never folded into the paid membership: a
    pricing page that read a prize as a subscription would stop offering
    membership to somebody whose free months are about to run out.
    """

    return await _role_window(runtime, account_bi, now, "promotion")


def _trial_payload(row, membership, now):
    row = row if isinstance(row, dict) else {}
    started_at = int(row.get("started_at") or 0)
    ends_at = int(row.get("ends_at") or 0)
    used = bool(started_at)
    return {
        "used": used,
        "active": used and ends_at > now,
        "tier": str(row.get("tier") or "") if used else "",
        "startedAt": started_at,
        "endsAt": ends_at,
        "durationDays": TRIAL_DURATION_MS // (24 * 60 * 60 * 1000),
        # One trial per account, and a currently-paying member has nothing to
        # trial; the paid state is read locally, so a provider outage cannot
        # block (or double-grant) eligibility.
        "eligible": not used and membership is None,
    }


async def _trial_status(runtime):
    account_bi, _account = await runtime.session({})
    if not account_bi:
        return _response(runtime, {"error": "unauthorized"}, 401)
    now = runtime.now()
    row = await runtime.d1_first(
        "SELECT tier,started_at,ends_at FROM plan_trials WHERE account_bi=?",
        account_bi,
    )
    membership = await _active_membership(runtime, account_bi, now)
    return _response(runtime, _trial_payload(row, membership, now))


async def _trial_start(runtime):
    data, error = await runtime.json_body(BODY_MAX_BYTES)
    if error:
        return _response(
            runtime, {"error": error},
            413 if error == "payload_too_large" else 400)
    if set(data) - {"tier", "sessionToken"}:
        return _response(runtime, {"error": "unsupported_trial_field"}, 400)
    tier = str(data.get("tier") or TRIAL_TIER).strip().lower()
    if tier != TRIAL_TIER:
        return _response(runtime, {"error": "invalid_tier"}, 400)
    account_bi, _account = await runtime.session(data)
    if not account_bi:
        return _response(runtime, {"error": "unauthorized"}, 401)
    now = runtime.now()
    existing = await runtime.d1_first(
        "SELECT ends_at FROM plan_trials WHERE account_bi=?", account_bi)
    if existing:
        return _response(runtime, {
            "error": "trial_already_used",
            "endsAt": int(existing.get("ends_at") or 0),
        }, 409)
    if await _active_membership(runtime, account_bi, now) is not None:
        return _response(runtime, {"error": "already_subscribed"}, 409)
    ends_at = now + TRIAL_DURATION_MS
    scope_bi = await runtime.blind(TRIAL_SCOPE_SUBJECT)
    # One batch: the register row and the entitlement land together. The plain
    # INSERT (no upsert) makes the primary key the race arbiter — a concurrent
    # double-submit aborts the whole batch instead of granting twice. The
    # grant upsert only ever lengthens an unrevoked expiry, so replaying the
    # batch can never shorten an entitlement another lane extended.
    await runtime.batch([
        (
            "INSERT INTO plan_trials(account_bi,tier,started_at,ends_at) "
            "VALUES(?,?,?,?)",
            (account_bi, TRIAL_TIER, now, ends_at),
        ),
        (
            "INSERT INTO role_grants"
            "(account_bi,role,scope_type,scope_bi,granted_by_bi,granted_at,"
            "expires_at,revoked_at) "
            "VALUES(?,'supporting_member','promotion',?,'',?,?,0) "
            "ON CONFLICT(account_bi,role,scope_type,scope_bi) DO UPDATE SET "
            "granted_by_bi='',granted_at=excluded.granted_at,"
            "expires_at=MAX(CASE WHEN role_grants.revoked_at=0 "
            "THEN role_grants.expires_at ELSE 0 END,excluded.expires_at),"
            "revoked_at=0",
            (account_bi, scope_bi, now, ends_at),
        ),
    ])
    return _response(runtime, {
        "ok": True,
        "tier": TRIAL_TIER,
        "startedAt": now,
        "endsAt": ends_at,
    }, 201)


async def _usage(runtime):
    account_bi, account = await runtime.session({})
    if not account_bi or not account:
        return _response(runtime, {"error": "unauthorized"}, 401)
    # Catalog rows are keyed by the blind index of the lowercase owner name
    # (catalog_routes publishes them that way); the other tables key directly
    # on the session's account_bi.
    owner = str(account.get("name") or "").strip().lower()
    owner_bi = await runtime.blind(owner) if owner else account_bi
    row = await runtime.d1_first(
        "SELECT "
        "(SELECT COUNT(*) FROM repositories WHERE owner_bi=?) AS repositories,"
        "(SELECT COUNT(*) FROM org_members WHERE member_bi=? "
        "AND role='owner') AS orgs,"
        "(SELECT COUNT(*) FROM account_ssh_keys WHERE account_bi=?) AS keys,"
        "(SELECT COUNT(*) FROM notifications WHERE recipient_bi=?) AS notes",
        owner_bi, account_bi, account_bi, account_bi,
    )
    row = row if isinstance(row, dict) else {}
    counts = {
        "repositories": int(row.get("repositories") or 0),
        "organizationsOwned": int(row.get("orgs") or 0),
        "sshKeys": int(row.get("keys") or 0),
        "storedNotifications": int(row.get("notes") or 0),
    }
    now = runtime.now()
    membership = await _active_membership(runtime, account_bi, now)
    promotion = await promotion_status(runtime, account_bi, now)
    return _response(runtime, {
        # One provider-neutral answer for "what is this account's plan": the
        # role grant a membership leaves behind, whoever sold it.
        "membership": membership,
        "promotion": promotion,
        "meters": {
            key: {
                "used": counts[key],
                "limit": limit,
                "label": label,
            }
            for key, (limit, label) in USAGE_METERS.items()
        },
        # These are operational caps every account shares, not plan
        # allowances; no membership or trial raises them today.
        "limitsPlanIndependent": True,
    })


async def handle(runtime, action):
    action = str(action or "").strip().lower()
    method = runtime.method()
    if action == "usage":
        if method != "GET":
            return _response(
                runtime, {"error": "method_not_allowed"}, 405, "GET")
        await runtime.ensure_schema()
        return await _usage(runtime)
    if action == "trial":
        if method not in ("GET", "POST"):
            return _response(
                runtime, {"error": "method_not_allowed"}, 405, "GET, POST")
        await runtime.ensure_schema()
        if method == "GET":
            return await _trial_status(runtime)
        return await _trial_start(runtime)
    return _response(runtime, {"error": "not_found"}, 404)
