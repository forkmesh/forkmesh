"""Authenticated D1 API for organization-only succession.

The runtime adapter intentionally exposes organization membership but no
platform-admin authorization primitive.  A platform administrator who is not a
currently authorized member of the organization follows the same fail-closed
path as every other caller.
"""

from __future__ import annotations

import organization_succession as policy


PREFIX_PART = "succession"


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


def _path_action(action):
    action = str(action or "").strip().lower()
    return action if action in {
        "", "check-in", "activate", "approve", "finalize", "cancel",
    } else None


async def _body(runtime):
    data, error = await runtime.json_body(policy.MAX_BODY_BYTES)
    if error:
        return None, _response(
            runtime,
            {"error": error},
            status=413 if error == "payload_too_large" else 400,
        )
    return data, None


async def _context(runtime, org, data=None):
    actor_bi, account = await runtime.session(data or {})
    actor = str((account or {}).get("name") or "").strip().lower()
    if not actor or not actor_bi:
        return None, _response(
            runtime, {"error": "invalid_session"}, status=401)
    org_bi, org_row = await runtime.organization(org)
    if not org_row:
        return None, _response(runtime, {"error": "not_found"}, status=404)
    role = await runtime.org_role(org_bi, actor)
    if role not in policy.ORG_ROLES:
        return None, _response(runtime, {"error": "forbidden"}, status=403)
    return {
        "actorBi": str(actor_bi),
        "actor": actor,
        "role": role,
        "orgBi": str(org_bi),
        "org": str(org_row.get("name") or org).strip().lower(),
    }, None


async def _policy_row(runtime, org_bi):
    return await runtime.d1_first(
        "SELECT org_bi,owner_bi,owner_name,successor_bi,successor_name,"
        "inactivity_days,grace_days,approval_threshold,owner_last_active_at,"
        "enabled,configured_at,updated_at "
        "FROM org_succession_policies WHERE org_bi=?",
        org_bi,
    )


async def _active_case(runtime, org_bi):
    return await runtime.d1_first(
        "SELECT case_id,org_bi,owner_bi,owner_name,successor_bi,"
        "successor_name,inactivity_days,grace_days,approval_threshold,"
        "opened_at,grace_ends_at,status,resolved_at "
        "FROM org_succession_cases WHERE org_bi=? AND status='grace' "
        "ORDER BY opened_at DESC LIMIT 1",
        org_bi,
    )


async def _current_approval_count(runtime, case):
    if not case:
        return 0
    row = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM org_succession_approvals a "
        "JOIN org_members m ON m.org_bi=? AND m.member_bi=a.approver_bi "
        "WHERE a.case_id=? AND m.role IN ('owner','admin','maintainer','member') "
        "AND a.approver_bi<>? AND a.approver_bi<>?",
        case["org_bi"],
        case["case_id"],
        case["owner_bi"],
        case["successor_bi"],
    )
    return int((row or {}).get("n") or 0)


async def _event(runtime, context, event_type, status, case_id="",
                 inactivity_days=0, grace_days=0, approval_threshold=0,
                 approval_count=0, dedupe_key=""):
    event_id = runtime.new_id()
    await runtime.d1_run(
        "INSERT OR IGNORE INTO org_succession_events "
        "(event_id,org_bi,case_id,actor_bi,event_type,status,created_at,"
        "inactivity_days,grace_days,approval_threshold,approval_count,"
        "dedupe_key) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
        event_id,
        context["orgBi"],
        str(case_id or ""),
        str(context.get("actorBi") or ""),
        event_type,
        status,
        runtime.now(),
        int(inactivity_days or 0),
        int(grace_days or 0),
        int(approval_threshold or 0),
        int(approval_count or 0),
        str(dedupe_key or event_id),
    )
    return event_id


def _case_public(case, approvals, now):
    if not case:
        return None
    return {
        "id": str(case.get("case_id") or ""),
        "status": str(case.get("status") or ""),
        "successor": str(case.get("successor_name") or ""),
        "openedAt": int(case.get("opened_at") or 0),
        "graceEndsAt": int(case.get("grace_ends_at") or 0),
        "graceComplete": int(now) >= int(case.get("grace_ends_at") or 0),
        "approvalCount": int(approvals),
        "approvalThreshold": int(case.get("approval_threshold") or 0),
    }


async def _status(runtime, context):
    row = await _policy_row(runtime, context["orgBi"])
    case = await _active_case(runtime, context["orgBi"])
    approvals = await _current_approval_count(runtime, case)
    now = runtime.now()
    events = await runtime.d1_all(
        "SELECT event_id,case_id,event_type,status,created_at,"
        "inactivity_days,grace_days,approval_threshold,approval_count "
        "FROM org_succession_events WHERE org_bi=? "
        "ORDER BY created_at DESC,event_id DESC LIMIT ?",
        context["orgBi"],
        policy.MAX_HISTORY,
    )
    configured = bool(row and int(row.get("enabled") or 0))
    output = {
        "ok": True,
        "org": context["org"],
        "configured": configured,
        "case": _case_public(case, approvals, now),
        "history": [
            {
                "id": str(item.get("event_id") or ""),
                "caseId": str(item.get("case_id") or ""),
                "event": str(item.get("event_type") or ""),
                "status": str(item.get("status") or ""),
                "createdAt": int(item.get("created_at") or 0),
                "inactivityDays": int(item.get("inactivity_days") or 0),
                "graceDays": int(item.get("grace_days") or 0),
                "approvalThreshold": int(
                    item.get("approval_threshold") or 0),
                "approvalCount": int(item.get("approval_count") or 0),
            }
            for item in events or []
        ],
        "transferBoundary": policy.transfer_boundary(),
    }
    if configured:
        last_active = await runtime.owner_activity(
            str(row.get("owner_bi") or ""),
            int(row.get("owner_last_active_at") or 0),
        )
        timeline = policy.inactivity_timeline(
            last_active, int(row.get("inactivity_days") or 0), now)
        if case:
            timeline["state"] = "grace"
        output["policy"] = {
            "owner": str(row.get("owner_name") or ""),
            "successor": str(row.get("successor_name") or ""),
            "inactivityDays": int(row.get("inactivity_days") or 0),
            "graceDays": int(row.get("grace_days") or 0),
            "approvalThreshold": int(row.get("approval_threshold") or 0),
            "configuredAt": int(row.get("configured_at") or 0),
            "updatedAt": int(row.get("updated_at") or 0),
            "inactivity": timeline,
        }
    else:
        output["policy"] = None
    return output


async def _configure(runtime, context, data):
    if context["role"] != "owner":
        return _response(runtime, {"error": "owner_required"}, status=403)
    members = await runtime.org_members(context["orgBi"])
    owners = [
        item for item in members
        if str(item.get("role") or "") == "owner"
    ]
    if len(owners) != 1 or str(owners[0].get("name") or "") != context["actor"]:
        return _response(
            runtime, {"error": "single_owner_required"}, status=409)
    config, error = policy.normalize_config(data)
    if error:
        return _response(runtime, {"error": error}, status=400)
    successor = next((
        item for item in members
        if str(item.get("name") or "").strip().lower()
        == config["successor"]
    ), None)
    if (
        not successor
        or str(successor.get("role") or "") not in {"admin", "member"}
        or config["successor"] == context["actor"]
    ):
        return _response(
            runtime, {"error": "successor_must_be_eligible_member"},
            status=400,
        )
    successor_bi, successor_account = await runtime.account(
        config["successor"])
    if not successor_bi or not successor_account:
        return _response(
            runtime, {"error": "successor_must_be_active_user"},
            status=400,
        )
    eligible = [
        member for member in members
        if policy.eligible_approver(
            member.get("role"),
            member.get("name"),
            context["actor"],
            config["successor"],
        )
    ]
    if len(eligible) < config["approvalThreshold"]:
        return _response(
            runtime,
            {
                "error": "insufficient_eligible_approvers",
                "eligible": len(eligible),
                "required": config["approvalThreshold"],
            },
            status=409,
        )
    now = runtime.now()
    active = await _active_case(runtime, context["orgBi"])
    statements = []
    if active:
        statements.extend([
            (
                "UPDATE org_succession_cases SET status='superseded',"
                "resolved_at=? WHERE case_id=? AND status='grace'",
                (now, active["case_id"]),
            ),
            (
                "INSERT INTO org_succession_events "
                "(event_id,org_bi,case_id,actor_bi,event_type,status,"
                "created_at,inactivity_days,grace_days,approval_threshold,"
                "approval_count,dedupe_key) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                (
                    runtime.new_id(), context["orgBi"], active["case_id"],
                    context["actorBi"], "cancelled", "superseded", now,
                    active["inactivity_days"], active["grace_days"],
                    active["approval_threshold"],
                    await _current_approval_count(runtime, active),
                    "superseded:" + active["case_id"],
                ),
            ),
        ])
    statements.extend([
        (
            "INSERT INTO org_succession_policies "
            "(org_bi,owner_bi,owner_name,successor_bi,successor_name,"
            "inactivity_days,grace_days,approval_threshold,"
            "owner_last_active_at,enabled,configured_at,updated_at) "
            "VALUES (?,?,?,?,?,?,?,?,?,1,?,?) "
            "ON CONFLICT(org_bi) DO UPDATE SET "
            "owner_bi=excluded.owner_bi,owner_name=excluded.owner_name,"
            "successor_bi=excluded.successor_bi,"
            "successor_name=excluded.successor_name,"
            "inactivity_days=excluded.inactivity_days,"
            "grace_days=excluded.grace_days,"
            "approval_threshold=excluded.approval_threshold,"
            "owner_last_active_at=excluded.owner_last_active_at,enabled=1,"
            "configured_at=excluded.configured_at,updated_at=excluded.updated_at",
            (
                context["orgBi"], context["actorBi"], context["actor"],
                successor_bi, config["successor"],
                config["inactivityDays"], config["graceDays"],
                config["approvalThreshold"], now, now, now,
            ),
        ),
        (
            "INSERT INTO org_succession_events "
            "(event_id,org_bi,case_id,actor_bi,event_type,status,created_at,"
            "inactivity_days,grace_days,approval_threshold,approval_count,"
            "dedupe_key) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
            (
                runtime.new_id(), context["orgBi"], "", context["actorBi"],
                "configured", "active", now, config["inactivityDays"],
                config["graceDays"], config["approvalThreshold"], 0,
                "configured:" + str(now),
            ),
        ),
    ])
    await runtime.batch(statements)
    return _response(runtime, await _status(runtime, context), status=201)


async def _disable(runtime, context):
    if context["role"] != "owner":
        return _response(runtime, {"error": "owner_required"}, status=403)
    row = await _policy_row(runtime, context["orgBi"])
    if not row or not int(row.get("enabled") or 0):
        return _response(runtime, {"error": "not_configured"}, status=404)
    now = runtime.now()
    case = await _active_case(runtime, context["orgBi"])
    statements = [(
        "UPDATE org_succession_policies SET enabled=0,updated_at=? "
        "WHERE org_bi=? AND owner_bi=? AND enabled=1",
        (now, context["orgBi"], context["actorBi"]),
    )]
    if case:
        statements.append((
            "UPDATE org_succession_cases SET status='cancelled',resolved_at=? "
            "WHERE case_id=? AND status='grace'",
            (now, case["case_id"]),
        ))
    statements.append((
        "INSERT INTO org_succession_events "
        "(event_id,org_bi,case_id,actor_bi,event_type,status,created_at,"
        "inactivity_days,grace_days,approval_threshold,approval_count,"
        "dedupe_key) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
        (
            runtime.new_id(), context["orgBi"],
            str((case or {}).get("case_id") or ""), context["actorBi"],
            "configuration-disabled", "disabled", now,
            row["inactivity_days"], row["grace_days"],
            row["approval_threshold"],
            await _current_approval_count(runtime, case),
            "disabled:" + str(now),
        ),
    ))
    await runtime.batch(statements)
    return _response(runtime, await _status(runtime, context))


async def _check_in(runtime, context):
    if context["role"] != "owner":
        return _response(runtime, {"error": "owner_required"}, status=403)
    row = await _policy_row(runtime, context["orgBi"])
    if (
        not row
        or not int(row.get("enabled") or 0)
        or str(row.get("owner_bi") or "") != context["actorBi"]
    ):
        return _response(runtime, {"error": "not_configured"}, status=404)
    now = runtime.now()
    case = await _active_case(runtime, context["orgBi"])
    statements = [(
        "UPDATE org_succession_policies SET owner_last_active_at=?,updated_at=? "
        "WHERE org_bi=? AND owner_bi=? AND enabled=1",
        (now, now, context["orgBi"], context["actorBi"]),
    )]
    if case:
        statements.append((
            "UPDATE org_succession_cases SET status='cancelled',resolved_at=? "
            "WHERE case_id=? AND status='grace'",
            (now, case["case_id"]),
        ))
    statements.append((
        "INSERT INTO org_succession_events "
        "(event_id,org_bi,case_id,actor_bi,event_type,status,created_at,"
        "inactivity_days,grace_days,approval_threshold,approval_count,"
        "dedupe_key) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
        (
            runtime.new_id(), context["orgBi"],
            str((case or {}).get("case_id") or ""), context["actorBi"],
            "check-in", "cancelled" if case else "active", now,
            row["inactivity_days"], row["grace_days"],
            row["approval_threshold"],
            await _current_approval_count(runtime, case),
            "check-in:" + str(now),
        ),
    ))
    await runtime.batch(statements)
    return _response(runtime, await _status(runtime, context))


async def _activate(runtime, context):
    row = await _policy_row(runtime, context["orgBi"])
    if not row or not int(row.get("enabled") or 0):
        return _response(runtime, {"error": "not_configured"}, status=404)
    if await _active_case(runtime, context["orgBi"]):
        return _response(runtime, {"error": "grace_already_active"}, status=409)
    members = await runtime.org_members(context["orgBi"])
    successor = next((
        member for member in members
        if str(member.get("member_bi") or "") == str(row["successor_bi"])
        and str(member.get("role") or "") in {"admin", "member"}
    ), None)
    owner = next((
        member for member in members
        if str(member.get("member_bi") or "") == str(row["owner_bi"])
        and str(member.get("role") or "") == "owner"
    ), None)
    if not owner or not successor:
        return _response(
            runtime, {"error": "policy_membership_changed"}, status=409)
    last_active = await runtime.owner_activity(
        row["owner_bi"], row["owner_last_active_at"])
    now = runtime.now()
    timeline = policy.inactivity_timeline(
        last_active, row["inactivity_days"], now)
    if timeline["state"] != "eligible":
        return _response(
            runtime,
            {
                "error": "owner_not_inactive",
                "inactivity": timeline,
            },
            status=409,
        )
    case_id = runtime.new_id()
    grace_ends = now + int(row["grace_days"]) * policy.DAY_MS
    await runtime.batch([
        (
            "INSERT INTO org_succession_cases "
            "(case_id,org_bi,owner_bi,owner_name,successor_bi,"
            "successor_name,inactivity_days,grace_days,approval_threshold,"
            "opened_at,grace_ends_at,status,resolved_at) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,'grace',0)",
            (
                case_id, context["orgBi"], row["owner_bi"],
                row["owner_name"], row["successor_bi"],
                row["successor_name"], row["inactivity_days"],
                row["grace_days"], row["approval_threshold"], now,
                grace_ends,
            ),
        ),
        (
            "INSERT INTO org_succession_events "
            "(event_id,org_bi,case_id,actor_bi,event_type,status,"
            "created_at,inactivity_days,grace_days,approval_threshold,"
            "approval_count,dedupe_key) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
            (
                runtime.new_id(), context["orgBi"], case_id,
                context["actorBi"], "grace-opened", "grace", now,
                row["inactivity_days"], row["grace_days"],
                row["approval_threshold"], 0, "grace:" + case_id,
            ),
        ),
    ])
    await runtime.notify(
        row["owner_name"],
        "Organization succession grace period opened",
        "An inactivity-based organization succession grace period was opened "
        "for " + context["org"] + ". Sign in and cancel or check in before "
        "the grace period ends if this is not intended.",
        "succession:grace-owner:" + case_id,
    )
    await runtime.notify(
        row["successor_name"],
        "Organization succession grace period opened",
        "You are the configured successor for " + context["org"] +
        ". No account, wallet, credential, device, or private key transfers.",
        "succession:grace-successor:" + case_id,
    )
    return _response(runtime, await _status(runtime, context), status=201)


async def _approve(runtime, context):
    case = await _active_case(runtime, context["orgBi"])
    if not case:
        return _response(runtime, {"error": "no_active_grace"}, status=404)
    if runtime.now() > int(case.get("grace_ends_at") or 0):
        return _response(
            runtime, {"error": "approval_window_closed"}, status=409)
    if not policy.eligible_approver(
        context["role"], context["actor"],
        case["owner_name"], case["successor_name"]):
        return _response(
            runtime, {"error": "independent_member_approval_required"},
            status=403,
        )
    now = runtime.now()
    await runtime.d1_run(
        "INSERT OR IGNORE INTO org_succession_approvals "
        "(case_id,approver_bi,approved_at) VALUES (?,?,?)",
        case["case_id"], context["actorBi"], now)
    count = await _current_approval_count(runtime, case)
    await _event(
        runtime, context, "approval-recorded", "grace",
        case_id=case["case_id"],
        inactivity_days=case["inactivity_days"],
        grace_days=case["grace_days"],
        approval_threshold=case["approval_threshold"],
        approval_count=count,
        dedupe_key=(
            "approval:" + case["case_id"] + ":" + context["actorBi"]),
    )
    return _response(runtime, await _status(runtime, context))


async def _cancel(runtime, context):
    if context["role"] != "owner":
        return _response(runtime, {"error": "owner_required"}, status=403)
    case = await _active_case(runtime, context["orgBi"])
    if not case:
        return _response(runtime, {"error": "no_active_grace"}, status=404)
    now = runtime.now()
    if now > int(case.get("grace_ends_at") or 0):
        return _response(
            runtime, {"error": "grace_period_ended"}, status=409)
    count = await _current_approval_count(runtime, case)
    await runtime.batch([
        (
            "UPDATE org_succession_cases SET status='cancelled',resolved_at=? "
            "WHERE case_id=? AND status='grace'",
            (now, case["case_id"]),
        ),
        (
            "UPDATE org_succession_policies SET owner_last_active_at=?,"
            "updated_at=? WHERE org_bi=? AND owner_bi=? AND enabled=1",
            (now, now, context["orgBi"], context["actorBi"]),
        ),
        (
            "INSERT INTO org_succession_events "
            "(event_id,org_bi,case_id,actor_bi,event_type,status,"
            "created_at,inactivity_days,grace_days,approval_threshold,"
            "approval_count,dedupe_key) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
            (
                runtime.new_id(), context["orgBi"], case["case_id"],
                context["actorBi"], "cancelled", "cancelled", now,
                case["inactivity_days"], case["grace_days"],
                case["approval_threshold"], count,
                "cancelled:" + case["case_id"],
            ),
        ),
    ])
    return _response(runtime, await _status(runtime, context))


async def _finalize(runtime, context):
    case = await _active_case(runtime, context["orgBi"])
    if not case:
        return _response(runtime, {"error": "no_active_grace"}, status=404)
    now = runtime.now()
    count = await _current_approval_count(runtime, case)
    if now < int(case.get("grace_ends_at") or 0):
        return _response(
            runtime,
            {"error": "grace_period_active",
             "graceEndsAt": int(case.get("grace_ends_at") or 0)},
            status=409,
        )
    if count < int(case.get("approval_threshold") or 0):
        await _event(
            runtime, context, "completion-denied", "grace",
            case_id=case["case_id"],
            inactivity_days=case["inactivity_days"],
            grace_days=case["grace_days"],
            approval_threshold=case["approval_threshold"],
            approval_count=count,
            dedupe_key=(
                "completion-denied:" + case["case_id"] + ":" + str(count)),
        )
        return _response(
            runtime,
            {
                "error": "approval_threshold_not_met",
                "approvalCount": count,
                "approvalThreshold": int(case["approval_threshold"]),
            },
            status=409,
        )
    # Migration 0064's BEFORE/AFTER triggers recheck the live org roster and
    # perform only the two org_members.role updates.  The event is in the same
    # D1 batch, so completion and append-only history commit together.
    try:
        await runtime.batch([
            (
                "UPDATE org_succession_cases SET status='completed',"
                "resolved_at=? WHERE case_id=? AND status='grace'",
                (now, case["case_id"]),
            ),
            (
                "UPDATE org_succession_policies SET enabled=0,updated_at=? "
                "WHERE org_bi=? AND owner_bi=? AND successor_bi=?",
                (
                    now, context["orgBi"], case["owner_bi"],
                    case["successor_bi"],
                ),
            ),
            (
                "INSERT INTO org_succession_events "
                "(event_id,org_bi,case_id,actor_bi,event_type,status,"
                "created_at,inactivity_days,grace_days,approval_threshold,"
                "approval_count,dedupe_key) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                (
                    runtime.new_id(), context["orgBi"], case["case_id"],
                    context["actorBi"], "completed", "completed", now,
                    case["inactivity_days"], case["grace_days"],
                    case["approval_threshold"], count,
                    "completed:" + case["case_id"],
                ),
            ),
        ])
    except Exception:
        return _response(
            runtime, {"error": "succession_conditions_changed"}, status=409)
    return _response(runtime, await _status(runtime, context))


async def handle(runtime, org, action=""):
    await runtime.ensure_schema()
    action = _path_action(action)
    if action is None:
        return _response(runtime, {"error": "not_found"}, status=404)
    method = runtime.method()
    data = {}
    if method in {"PUT", "POST", "DELETE"}:
        data, error_response = await _body(runtime)
        if error_response is not None:
            return error_response
    context, error_response = await _context(runtime, org, data)
    if error_response is not None:
        return error_response

    if not action and method == "GET":
        return _response(runtime, await _status(runtime, context))
    if not action and method == "PUT":
        return await _configure(runtime, context, data)
    if not action and method == "DELETE":
        return await _disable(runtime, context)
    if action == "check-in" and method == "POST":
        return await _check_in(runtime, context)
    if action == "activate" and method == "POST":
        return await _activate(runtime, context)
    if action == "approve" and method == "POST":
        return await _approve(runtime, context)
    if action == "cancel" and method == "POST":
        return await _cancel(runtime, context)
    if action == "finalize" and method == "POST":
        return await _finalize(runtime, context)
    allow = {
        "": "GET, PUT, DELETE",
        "check-in": "POST",
        "activate": "POST",
        "approve": "POST",
        "cancel": "POST",
        "finalize": "POST",
    }[action]
    return _response(
        runtime, {"error": "method_not_allowed"}, status=405, allow=allow)


async def process_warnings(runtime, limit=50):
    """Cron: emit one bounded warning per inactivity deadline."""
    await runtime.ensure_schema()
    rows = await runtime.d1_all(
        "SELECT org_bi,owner_bi,owner_name,successor_bi,successor_name,"
        "inactivity_days,grace_days,approval_threshold,owner_last_active_at,"
        "updated_at FROM org_succession_policies WHERE enabled=1 "
        "ORDER BY updated_at ASC LIMIT ?",
        max(1, min(int(limit or 1), 100)),
    )
    warned = 0
    for row in rows or []:
        last_active = await runtime.owner_activity(
            row["owner_bi"], row["owner_last_active_at"])
        timeline = policy.inactivity_timeline(
            last_active, row["inactivity_days"], runtime.now())
        if timeline["state"] != "warning":
            continue
        dedupe = (
            "warning:" + str(row["org_bi"]) + ":" +
            str(timeline["inactivityAt"]))
        existing = await runtime.d1_first(
            "SELECT 1 AS one FROM org_succession_events "
            "WHERE org_bi=? AND dedupe_key=?",
            row["org_bi"], dedupe)
        if existing:
            continue
        context = {
            "orgBi": str(row["org_bi"]),
            "actorBi": "",
            "org": "",
        }
        await _event(
            runtime, context, "warning-issued", "warning",
            inactivity_days=row["inactivity_days"],
            grace_days=row["grace_days"],
            approval_threshold=row["approval_threshold"],
            dedupe_key=dedupe,
        )
        await runtime.notify(
            row["owner_name"],
            "Organization succession inactivity warning",
            "Your configured inactivity threshold is approaching. Check in "
            "to keep the organization succession policy active without "
            "opening its grace period.",
            dedupe,
        )
        warned += 1
    return warned
