"""D1 orchestration for community-governed contextual placements."""

from __future__ import annotations

import hashlib
import json

import community_ads as policy


PREFIX = "/api/world/community-ads"
MAX_PUBLIC_PROPOSALS = 100


def _response(runtime, data, status=200, cache_control=None, allow=""):
    headers = {"x-content-type-options": "nosniff"}
    if allow:
        headers["allow"] = allow
    return runtime.response(
        data,
        status=status,
        cache_control=cache_control or
        "no-store, max-age=0, must-revalidate",
        extra_headers=headers,
    )


def _route(path):
    clean = str(path or "").rstrip("/")
    if clean == PREFIX:
        return ("index", "")
    if not clean.startswith(PREFIX + "/"):
        return None
    parts = clean[len(PREFIX) + 1 :].split("/")
    if parts == ["criteria"]:
        return ("criteria", "")
    if parts == ["placements"]:
        return ("placements", "")
    if parts == ["policy"]:
        return ("policy", "")
    if parts == ["proposals"]:
        return ("proposals", "")
    if parts == ["revenue"]:
        return ("revenue", "")
    if len(parts) == 2 and parts[0] == "proposals" and policy.valid_id(parts[1]):
        return ("proposal", parts[1])
    if (
        len(parts) == 3
        and parts[0] == "proposals"
        and policy.valid_id(parts[1])
        and parts[2] in {"vote", "moderation", "appeal"}
    ):
        return (parts[2], parts[1])
    return None


async def _body(runtime):
    data, error = await runtime.json_body(policy.BODY_MAX_BYTES)
    if error:
        return None, _response(
            runtime,
            {"error": error},
            status=413 if error == "payload_too_large" else 400,
        )
    return data, None


async def _actor(runtime, data=None):
    account_bi, account = await runtime.session(data or {})
    name = policy.clean_text((account or {}).get("name"), 80).lower()
    return str(account_bi or ""), account, name


def _decode_data(row):
    if not row:
        return {}
    value = row.get("data")
    if isinstance(value, dict):
        return dict(value)
    try:
        parsed = json.loads(str(value or "{}"))
    except (TypeError, ValueError, json.JSONDecodeError):
        return {}
    return parsed if isinstance(parsed, dict) else {}


async def _votes(runtime, proposal_id):
    return await runtime.d1_all(
        "SELECT choice,conflict,disclosure,created_at,updated_at "
        "FROM community_ad_votes WHERE proposal_id=? "
        "ORDER BY created_at ASC",
        proposal_id,
    )


async def _moderation(runtime, proposal_id):
    rows = await runtime.d1_all(
        "SELECT record_id,action,reason,evidence_url,created_at "
        "FROM community_ad_moderation WHERE proposal_id=? "
        "ORDER BY created_at ASC",
        proposal_id,
    )
    return [
        {
            "id": str(row.get("record_id") or ""),
            "action": str(row.get("action") or ""),
            "reason": str(row.get("reason") or ""),
            "evidenceUrl": str(row.get("evidence_url") or ""),
            "createdAt": int(row.get("created_at") or 0),
        }
        for row in rows or []
    ]


async def _refresh_status(runtime, row):
    if not row:
        return None
    now = runtime.now()
    status = str(row.get("status") or "")
    data = _decode_data(row)
    if status == "voting" and now >= int(row.get("closes_at") or 0):
        result = policy.tally(
            await _votes(runtime, row["proposal_id"]),
            int(data.get("quorum") or policy.DEFAULT_QUORUM),
        )
        status = "approved" if result["approved"] else "rejected"
        await runtime.d1_run(
            "UPDATE community_ad_proposals SET status=?,updated_at=? "
            "WHERE proposal_id=? AND status='voting'",
            status,
            now,
            row["proposal_id"],
        )
        await runtime.d1_run(
            "INSERT INTO community_ad_moderation "
            "(record_id,proposal_id,action,reason,evidence_url,actor_bi,"
            "created_at) VALUES (?,?,?,?,?,?,?)",
            runtime.new_id(),
            row["proposal_id"],
            "approve" if status == "approved" else "reject",
            (
                "Voting window closed; the configured quorum and "
                "two-thirds threshold were evaluated."
            ),
            "",
            "system:vote-tally",
            now,
        )
        row = {**row, "status": status, "updated_at": now}
    if (
        status == "approved"
        and now >= int(row.get("review_due_at") or 0)
    ):
        status = "expired"
        await runtime.d1_run(
            "UPDATE community_ad_proposals SET status='expired',updated_at=? "
            "WHERE proposal_id=? AND status='approved'",
            now,
            row["proposal_id"],
        )
        row = {**row, "status": status, "updated_at": now}
    return row


async def _public_proposal(runtime, row, detail=False):
    row = await _refresh_status(runtime, row)
    data = _decode_data(row)
    votes = await _votes(runtime, row["proposal_id"])
    result = policy.tally(
        votes, int(data.get("quorum") or policy.DEFAULT_QUORUM))
    public = {
        "id": str(row.get("proposal_id") or ""),
        "status": str(row.get("status") or ""),
        "title": str(data.get("title") or ""),
        "sponsor": str(data.get("sponsor") or ""),
        "sponsorDisclosure": str(data.get("sponsorDisclosure") or ""),
        "copy": str(data.get("copy") or ""),
        "destinationUrl": str(data.get("destinationUrl") or ""),
        "contextTags": list(data.get("contextTags") or []),
        "evidence": list(data.get("evidence") or []),
        "conflictDisclosure": str(data.get("conflictDisclosure") or ""),
        "vote": result,
        "opensAt": int(row.get("opens_at") or 0),
        "closesAt": int(row.get("closes_at") or 0),
        "reviewDueAt": int(row.get("review_due_at") or 0),
        "createdAt": int(row.get("created_at") or 0),
        "updatedAt": int(row.get("updated_at") or 0),
        "label": "Community-reviewed placement",
        "universalEthicsClaim": False,
    }
    if detail:
        public["moderation"] = await _moderation(
            runtime, row["proposal_id"])
    return public


async def _instance_policy(runtime):
    instance_id = runtime.instance_id()
    row = await runtime.d1_first(
        "SELECT instance_id,enabled,contexts,revenue_destination,"
        "revenue_destination_type,updated_at "
        "FROM community_ad_instance_policy WHERE instance_id=?",
        instance_id,
    )
    if not row:
        return {
            "instanceId": instance_id,
            "enabled": False,
            "contexts": [],
            "revenueDestination": "",
            "revenueDestinationType": "",
            "ledgerClass": "advertising-revenue",
            "defaultDisabled": True,
            "updatedAt": 0,
        }
    try:
        contexts = json.loads(str(row.get("contexts") or "[]"))
    except (TypeError, ValueError, json.JSONDecodeError):
        contexts = []
    return {
        "instanceId": str(row.get("instance_id") or instance_id),
        "enabled": bool(row.get("enabled")),
        "contexts": [
            item for item in contexts
            if isinstance(item, str) and item in policy.CONTEXTS
        ],
        "revenueDestination": str(
            row.get("revenue_destination") or ""),
        "revenueDestinationType": str(
            row.get("revenue_destination_type") or ""),
        "ledgerClass": "advertising-revenue",
        "defaultDisabled": True,
        "updatedAt": int(row.get("updated_at") or 0),
    }


async def _list_proposals(runtime, detail=False):
    rows = await runtime.d1_all(
        "SELECT proposal_id,status,data,opens_at,closes_at,review_due_at,"
        "created_at,updated_at FROM community_ad_proposals "
        "ORDER BY created_at DESC LIMIT ?",
        MAX_PUBLIC_PROPOSALS,
    )
    return [
        await _public_proposal(runtime, row, detail=detail)
        for row in rows or []
    ]


async def _placements(runtime):
    params = runtime.query_params()
    context = policy.clean_text(params.get("context"), 40).lower()
    if context not in policy.CONTEXTS:
        return _response(
            runtime, {"error": "valid_context_required"}, status=400)
    instance = await _instance_policy(runtime)
    base = {
        "ok": True,
        "label": "Community-reviewed placement",
        "context": context,
        "tracking": "none",
        "behavioralTargeting": False,
        "sensitiveTargeting": False,
        "personalDataUsed": False,
        "universalEthicsClaim": False,
        "revenue": {
            "ledgerClass": "advertising-revenue",
            "separateFromUserFunds": True,
            "separateFromRewardPool": True,
            "destination": instance["revenueDestination"],
            "destinationType": instance["revenueDestinationType"],
        },
        "placements": [],
    }
    if not instance["enabled"] or context not in instance["contexts"]:
        return _response(
            runtime, base,
            cache_control="public, max-age=60, stale-while-revalidate=300")
    rows = await runtime.d1_all(
        "SELECT proposal_id,status,data,opens_at,closes_at,review_due_at,"
        "created_at,updated_at FROM community_ad_proposals "
        "WHERE status='approved' AND review_due_at>? "
        "ORDER BY proposal_id ASC LIMIT ?",
        runtime.now(),
        MAX_PUBLIC_PROPOSALS,
    )
    candidates = []
    for row in rows or []:
        data = _decode_data(row)
        tags = list(data.get("contextTags") or [])
        # Context and declared content tags are the only selection inputs.
        candidates.append({
            "proposalId": str(row.get("proposal_id") or ""),
            "label": "Community-reviewed placement",
            "copy": str(data.get("copy") or ""),
            "sponsor": str(data.get("sponsor") or ""),
            "destinationUrl": str(data.get("destinationUrl") or ""),
            "contextTags": tags,
            "reviewDueAt": int(row.get("review_due_at") or 0),
            "whyShown": f"Enabled for the {context} page context.",
            "tracking": "none",
        })
    if candidates:
        day = runtime.now() // (24 * 60 * 60 * 1000)
        seed = (
            instance["instanceId"] + "\0" + context + "\0" + str(day)
        ).encode("utf-8")
        index = int.from_bytes(hashlib.sha256(seed).digest()[:8], "big")
        base["placements"] = [candidates[index % len(candidates)]]
    return _response(
        runtime, base,
        cache_control="public, max-age=60, stale-while-revalidate=300")


async def handle(runtime, path):
    await runtime.ensure_schema()
    route = _route(path)
    if route is None:
        return _response(runtime, {"error": "not_found"}, status=404)
    kind, target = route
    method = runtime.method()

    if kind == "criteria":
        if method != "GET":
            return _response(
                runtime, {"error": "method_not_allowed"}, status=405,
                allow="GET")
        return _response(
            runtime,
            {"ok": True, "criteria": policy.ELIGIBILITY_CRITERIA},
            cache_control="public, max-age=3600",
        )
    if kind == "placements":
        if method != "GET":
            return _response(
                runtime, {"error": "method_not_allowed"}, status=405,
                allow="GET")
        return await _placements(runtime)
    if kind == "index":
        if method != "GET":
            return _response(
                runtime, {"error": "method_not_allowed"}, status=405,
                allow="GET")
        return _response(runtime, {
            "ok": True,
            "criteria": policy.ELIGIBILITY_CRITERIA,
            "instancePolicy": await _instance_policy(runtime),
            "proposals": await _list_proposals(runtime),
        })
    if kind == "policy":
        if method == "GET":
            return _response(runtime, {
                "ok": True, "policy": await _instance_policy(runtime)})
        if method != "PUT":
            return _response(
                runtime, {"error": "method_not_allowed"}, status=405,
                allow="GET, PUT")
        data, error_response = await _body(runtime)
        if error_response:
            return error_response
        actor_bi, _account, actor = await _actor(runtime, data)
        if not actor or not await runtime.is_admin(actor):
            await runtime.audit(
                actor or "anonymous",
                "community_ads.policy.update",
                "instance",
                runtime.instance_id(),
                outcome="denied",
                details={"reason": "platform_administrator_required"},
            )
            return _response(runtime, {"error": "forbidden"}, status=403)
        normalized, error = policy.normalize_instance_policy(data)
        if error:
            return _response(runtime, {"error": error}, status=400)
        now = runtime.now()
        await runtime.d1_run(
            "INSERT INTO community_ad_instance_policy "
            "(instance_id,enabled,contexts,revenue_destination,"
            "revenue_destination_type,updated_by_bi,updated_at) "
            "VALUES (?,?,?,?,?,?,?) ON CONFLICT(instance_id) DO UPDATE SET "
            "enabled=excluded.enabled,contexts=excluded.contexts,"
            "revenue_destination=excluded.revenue_destination,"
            "revenue_destination_type=excluded.revenue_destination_type,"
            "updated_by_bi=excluded.updated_by_bi,"
            "updated_at=excluded.updated_at",
            runtime.instance_id(),
            1 if normalized["enabled"] else 0,
            json.dumps(normalized["contexts"], separators=(",", ":")),
            normalized["revenueDestination"],
            normalized["revenueDestinationType"],
            actor_bi,
            now,
        )
        await runtime.audit(
            actor,
            "community_ads.policy.update",
            "instance",
            runtime.instance_id(),
            details={
                "enabled": normalized["enabled"],
                "revenueDestinationType":
                    normalized["revenueDestinationType"],
            },
        )
        return _response(runtime, {
            "ok": True, "policy": await _instance_policy(runtime)})
    if kind == "proposals":
        if method == "GET":
            return _response(
                runtime, {"ok": True, "proposals": await _list_proposals(runtime)})
        if method != "POST":
            return _response(
                runtime, {"error": "method_not_allowed"}, status=405,
                allow="GET, POST")
        data, error_response = await _body(runtime)
        if error_response:
            return error_response
        actor_bi, account, actor = await _actor(runtime, data)
        eligible, error = policy.voter_eligible(account, runtime.now())
        if not actor or not eligible:
            return _response(
                runtime,
                {"error": error or "registered_account_required"},
                status=403,
            )
        normalized, error = policy.normalize_proposal(data, runtime.now())
        if error:
            return _response(runtime, {"error": error}, status=400)
        now = runtime.now()
        voting_days = int(data.get("votingDays") or 14)
        closes = now + voting_days * 24 * 60 * 60 * 1000
        proposal_id = runtime.new_id()
        await runtime.d1_run(
            "INSERT INTO community_ad_proposals "
            "(proposal_id,proposer_bi,status,data,opens_at,closes_at,"
            "review_due_at,created_at,updated_at) "
            "VALUES (?,?,'voting',?,?,?,?,?,?)",
            proposal_id,
            actor_bi,
            json.dumps(normalized, sort_keys=True, separators=(",", ":")),
            now,
            closes,
            closes + policy.MAX_REVIEW_MS,
            now,
            now,
        )
        await runtime.audit(
            actor,
            "community_ads.proposal.create",
            "community_ad_proposal",
            proposal_id,
            details={"evidenceCount": len(normalized["evidence"])},
        )
        row = await runtime.d1_first(
            "SELECT proposal_id,status,data,opens_at,closes_at,review_due_at,"
            "created_at,updated_at FROM community_ad_proposals "
            "WHERE proposal_id=?",
            proposal_id,
        )
        return _response(
            runtime,
            {"ok": True, "proposal": await _public_proposal(runtime, row, True)},
            status=201,
        )
    if kind == "proposal":
        if method != "GET":
            return _response(
                runtime, {"error": "method_not_allowed"}, status=405,
                allow="GET")
        row = await runtime.d1_first(
            "SELECT proposal_id,status,data,opens_at,closes_at,review_due_at,"
            "created_at,updated_at FROM community_ad_proposals "
            "WHERE proposal_id=?",
            target,
        )
        if not row:
            return _response(runtime, {"error": "not_found"}, status=404)
        return _response(runtime, {
            "ok": True,
            "proposal": await _public_proposal(runtime, row, detail=True),
        })

    data, error_response = await _body(runtime)
    if error_response:
        return error_response
    actor_bi, account, actor = await _actor(runtime, data)
    if not actor:
        return _response(runtime, {"error": "invalid_session"}, status=401)
    if kind == "revenue":
        if method != "POST":
            return _response(
                runtime, {"error": "method_not_allowed"}, status=405,
                allow="POST")
        if not await runtime.is_admin(actor):
            return _response(runtime, {"error": "forbidden"}, status=403)
        proposal_id = policy.clean_text(data.get("proposalId"), 32).lower()
        if not policy.valid_id(proposal_id):
            return _response(
                runtime, {"error": "valid_proposal_required"}, status=400)
        proposal = await runtime.d1_first(
            "SELECT proposal_id FROM community_ad_proposals "
            "WHERE proposal_id=?",
            proposal_id,
        )
        if not proposal:
            return _response(runtime, {"error": "not_found"}, status=404)
        try:
            amount = int(data.get("amountMinor"))
        except (TypeError, ValueError):
            amount = -1
        currency = policy.currency(data.get("currency"))
        external_reference = policy.clean_text(
            data.get("externalReference"), 160)
        instance = await _instance_policy(runtime)
        if (
            amount < 0
            or not currency
            or not external_reference
            or not instance["revenueDestination"]
        ):
            return _response(
                runtime, {"error": "invalid_revenue_record"}, status=400)
        entry_id = runtime.new_id()
        await runtime.d1_run(
            "INSERT INTO community_ad_revenue "
            "(entry_id,instance_id,proposal_id,ledger_class,amount_minor,"
            "currency,destination_snapshot,external_reference,recorded_at) "
            "VALUES (?,?,?,'advertising-revenue',?,?,?,?,?)",
            entry_id,
            runtime.instance_id(),
            proposal_id,
            amount,
            currency,
            instance["revenueDestination"],
            external_reference,
            runtime.now(),
        )
        await runtime.audit(
            actor,
            "community_ads.revenue.record",
            "community_ad_revenue",
            entry_id,
            details={
                "proposalId": proposal_id,
                "currency": currency,
                "ledgerClass": "advertising-revenue",
            },
        )
        return _response(runtime, {
            "ok": True,
            "entry": {
                "id": entry_id,
                "ledgerClass": "advertising-revenue",
                "amountMinor": amount,
                "currency": currency,
                "externalReference": external_reference,
                "custody": "external-accounting-record-only",
            },
        }, status=201)
    row = await runtime.d1_first(
        "SELECT proposal_id,proposer_bi,status,data,opens_at,closes_at,"
        "review_due_at,created_at,updated_at FROM community_ad_proposals "
        "WHERE proposal_id=?",
        target,
    )
    if not row:
        return _response(runtime, {"error": "not_found"}, status=404)
    row = await _refresh_status(runtime, row)
    now = runtime.now()

    if kind == "vote":
        if method != "POST":
            return _response(
                runtime, {"error": "method_not_allowed"}, status=405,
                allow="POST")
        eligible, error = policy.voter_eligible(account, now)
        if not eligible:
            return _response(runtime, {"error": error}, status=403)
        if row["status"] != "voting" or now >= int(row["closes_at"]):
            return _response(runtime, {"error": "voting_closed"}, status=409)
        vote, error = policy.normalize_vote(data)
        if error:
            return _response(runtime, {"error": error}, status=400)
        await runtime.d1_run(
            "INSERT INTO community_ad_votes "
            "(proposal_id,voter_bi,choice,conflict,disclosure,created_at,"
            "updated_at) VALUES (?,?,?,?,?,?,?) "
            "ON CONFLICT(proposal_id,voter_bi) DO UPDATE SET "
            "choice=excluded.choice,conflict=excluded.conflict,"
            "disclosure=excluded.disclosure,updated_at=excluded.updated_at",
            target,
            actor_bi,
            vote["choice"],
            1 if vote["conflictOfInterest"] else 0,
            vote["conflictDisclosure"],
            now,
            now,
        )
        await runtime.audit(
            actor,
            "community_ads.vote",
            "community_ad_proposal",
            target,
            details={
                "choice": vote["choice"],
                "conflict": vote["conflictOfInterest"],
                "weight": 1,
            },
        )
        public = await _public_proposal(runtime, row)
        return _response(runtime, {"ok": True, "vote": public["vote"]})

    is_admin = await runtime.is_admin(actor)
    if kind == "moderation":
        if method != "POST":
            return _response(
                runtime, {"error": "method_not_allowed"}, status=405,
                allow="POST")
        if not is_admin:
            return _response(runtime, {"error": "forbidden"}, status=403)
        record, error = policy.normalize_moderation(data)
        if error or record["action"] in {"appeal-filed"}:
            return _response(
                runtime, {"error": error or "invalid_moderation"}, status=400)
        next_status = {
            "suspend": "suspended",
            "reject": "rejected",
            "reinstate": "approved",
            "appeal-upheld": "approved",
            "appeal-denied": "rejected",
        }.get(record["action"], row["status"])
        if record["action"] == "reinstate" and row["status"] != "suspended":
            return _response(
                runtime, {"error": "proposal_not_suspended"}, status=409)
        await runtime.d1_run(
            "INSERT INTO community_ad_moderation "
            "(record_id,proposal_id,action,reason,evidence_url,actor_bi,"
            "created_at) VALUES (?,?,?,?,?,?,?)",
            runtime.new_id(),
            target,
            record["action"],
            record["reason"],
            record["evidenceUrl"],
            actor_bi,
            now,
        )
        await runtime.d1_run(
            "UPDATE community_ad_proposals SET status=?,updated_at=? "
            "WHERE proposal_id=?",
            next_status,
            now,
            target,
        )
        await runtime.audit(
            actor,
            "community_ads.moderation." + record["action"],
            "community_ad_proposal",
            target,
            details={"reason": record["reason"]},
        )
        return _response(runtime, {"ok": True, "status": next_status})

    if kind == "appeal":
        if method != "POST":
            return _response(
                runtime, {"error": "method_not_allowed"}, status=405,
                allow="POST")
        if actor_bi != str(row.get("proposer_bi") or "") and not is_admin:
            return _response(runtime, {"error": "forbidden"}, status=403)
        if row["status"] not in {"rejected", "suspended"}:
            return _response(
                runtime, {"error": "proposal_not_appealable"}, status=409)
        reason = policy.clean_text(data.get("reason"), 1200)
        evidence_url = (
            policy.public_https_url(data.get("evidenceUrl"))
            if data.get("evidenceUrl") else ""
        )
        if not reason or (data.get("evidenceUrl") and not evidence_url):
            return _response(runtime, {"error": "invalid_appeal"}, status=400)
        await runtime.d1_run(
            "INSERT INTO community_ad_moderation "
            "(record_id,proposal_id,action,reason,evidence_url,actor_bi,"
            "created_at) VALUES (?,?,'appeal-filed',?,?,?,?)",
            runtime.new_id(),
            target,
            reason,
            evidence_url,
            actor_bi,
            now,
        )
        await runtime.d1_run(
            "UPDATE community_ad_proposals SET status='appealed',updated_at=? "
            "WHERE proposal_id=?",
            now,
            target,
        )
        await runtime.audit(
            actor,
            "community_ads.appeal",
            "community_ad_proposal",
            target,
            details={"reason": reason},
        )
        return _response(runtime, {"ok": True, "status": "appealed"})

    return _response(runtime, {"error": "not_found"}, status=404)


async def cleanup(runtime):
    """Expire approved records and retain public audit data."""
    await runtime.ensure_schema()
    now = runtime.now()
    await runtime.d1_run(
        "UPDATE community_ad_proposals SET status='expired',updated_at=? "
        "WHERE status='approved' AND review_due_at<=?",
        now,
        now,
    )
