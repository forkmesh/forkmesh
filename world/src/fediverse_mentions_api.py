"""Manual, privacy-safe lifecycle for verified public fediverse feedback.

Inbound ActivityPub signature verification remains in ``entry.py``. Callers
record only notes that have passed that gate and belong to a public repository.
This module deliberately never auto-files an issue: an authorized owner must
preview and explicitly confirm a draft. The resulting inbox submission remains
``pending`` until the owner node reports the issue number it materialized.
"""

from __future__ import annotations

import ipaddress
import json
import re
from urllib.parse import urlparse


PREFIX = "/api/world/fediverse-mentions"
BODY_MAX_BYTES = 72 * 1024
CONTENT_MAX = 8 * 1024
TITLE_MAX = 240
PUBLIC_EXCERPT_MAX = 320
FEED_LIMIT = 50
RETENTION_MS = 180 * 24 * 60 * 60 * 1000
CREATE_LEASE_MS = 2 * 60 * 1000
_ID_RE = re.compile(r"^[a-f0-9]{32}$")
_SEGMENT_RE = re.compile(r"^[A-Za-z0-9_.-]{1,100}$")
_NONPUBLIC_SUFFIXES = (
    ".internal",
    ".invalid",
    ".local",
    ".localhost",
    ".test",
    ".example",
    ".onion",
)


def _clean_text(value, limit):
    clean = "".join(
        char if char == "\n" or (char.isprintable() and char not in "\x00")
        else " "
        for char in str(value or "").replace("\r\n", "\n").replace("\r", "\n")
    )
    clean = "\n".join(
        re.sub(r"[ \t]+", " ", line).strip()
        for line in clean.splitlines()
    )
    return clean.strip()[:limit]


def _public_https_url(value, limit=1600):
    raw = str(value or "").strip()
    if not raw or len(raw) > limit or "\\" in raw:
        return ""
    try:
        parsed = urlparse(raw)
        host = str(parsed.hostname or "").lower().rstrip(".")
        port = parsed.port
    except (TypeError, ValueError):
        return ""
    if (
        parsed.scheme.lower() != "https"
        or not host
        or parsed.username is not None
        or parsed.password is not None
        or port not in (None, 443)
        or "." not in host
        or any(host.endswith(suffix) for suffix in _NONPUBLIC_SUFFIXES)
    ):
        return ""
    try:
        ipaddress.ip_address(host)
        return ""
    except ValueError:
        pass
    return parsed._replace(
        scheme="https", netloc=host, fragment=""
    ).geturl()


def _valid_id(value):
    return bool(_ID_RE.fullmatch(str(value or "").strip().lower()))


def _valid_segment(value):
    return bool(_SEGMENT_RE.fullmatch(str(value or "").strip()))


def _response(runtime, data, status=200, allow="", public=False):
    headers = {"x-content-type-options": "nosniff"}
    if allow:
        headers["allow"] = allow
    return runtime.response(
        data,
        status=status,
        cache_control=(
            "public, max-age=30, stale-while-revalidate=60"
            if public else "no-store, max-age=0, must-revalidate"
        ),
        extra_headers=headers,
    )


def _route(path):
    clean = str(path or "").rstrip("/")
    if clean == PREFIX:
        return ("feed", "")
    if not clean.startswith(PREFIX + "/"):
        return None
    parts = clean[len(PREFIX) + 1 :].split("/")
    if len(parts) == 2 and _valid_id(parts[0]) and parts[1] in {
        "preview", "create", "moderate", "materialized",
    }:
        return (parts[1], parts[0])
    return None


async def _body(runtime):
    data, error = await runtime.json_body(BODY_MAX_BYTES)
    if error:
        return None, _response(
            runtime,
            {"error": error},
            status=413 if error == "payload_too_large" else 400,
        )
    return data, None


async def _decode(runtime, row):
    if not row:
        return {}
    value = row.get("data")
    if isinstance(value, dict):
        return dict(value)
    opened = await runtime.open(value)
    return dict(opened) if isinstance(opened, dict) else {}


async def _row(runtime, mention_id):
    return await runtime.d1_first(
        "SELECT mention_id,remote_id_bi,repo_bi,kind,state,data,"
        "issue_number,create_lease_id,create_lease_until,followup_state,"
        "created_at,updated_at,expires_at "
        "FROM world_fediverse_mentions WHERE mention_id=?",
        mention_id,
    )


def _draft(record):
    text = _clean_text(record.get("content"), CONTENT_MAX)
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    first = lines[0] if lines else "Fediverse feedback"
    title = first
    if len(title) > 120:
        title = title[:117].rstrip() + "..."
    remote_url = _public_https_url(record.get("remoteUrl"))
    author = _clean_text(record.get("author"), 160) or "a fediverse user"
    body = text or "The public post did not include a text body."
    body = (
        body.rstrip()
        + "\n\n---\n"
        + "Proposed manually from verified public fediverse feedback by "
        + author
        + (": " + remote_url if remote_url else ".")
    )
    return {"title": title[:TITLE_MAX], "body": body[:CONTENT_MAX]}


def _progress(row):
    state = str(row.get("state") or "")
    followup = str(row.get("followup_state") or "")
    if state == "review":
        return "Awaiting authorized manual review."
    if state == "creating":
        return "The authorized request is being queued."
    if state == "pending":
        return "Pending owner-node materialization."
    if state == "created":
        if followup == "failed":
            return "Issue created; consented follow-up delivery is delayed."
        return "Owner node confirmed issue creation."
    if state == "linked":
        return "Verified reply linked to a tracked public issue."
    if state == "failed":
        return "The request was not queued; an authorized reviewer may retry."
    return "Hidden by repository moderation."


def _public_state(row):
    state = str(row.get("state") or "review")
    return "review" if state == "creating" else state


async def _public_item(runtime, row):
    record = await _decode(runtime, row)
    state = _public_state(row)
    return {
        "id": str(row.get("mention_id") or ""),
        "kind": str(row.get("kind") or "mention"),
        "state": state,
        "repository": _clean_text(record.get("publicRepository"), 220),
        "author": _clean_text(record.get("author"), 160),
        "authorName": _clean_text(record.get("authorName"), 160),
        "remoteUrl": _public_https_url(record.get("remoteUrl")),
        "excerpt": _clean_text(record.get("content"), PUBLIC_EXCERPT_MAX),
        "published": _clean_text(record.get("published"), 60),
        "issueNumber": (
            int(row.get("issue_number") or 0)
            if state in {"created", "linked"} else 0
        ),
        "issueUrl": (
            _public_https_url(record.get("issueUrl"))
            if state in {"created", "linked"} else ""
        ),
        "progress": _progress(row),
        "verifiedPublicActivity": True,
        "automaticIssueCreation": False,
    }


async def record_verified(
    runtime,
    *,
    note,
    remote,
    actor_owner,
    data_owner,
    repo,
    kind="mention",
    context=None,
    signature_verified=False,
    public_activity=False,
    public_repository=False,
):
    """Record a verified public note once; return its random public id.

    The three booleans are intentionally explicit so a future caller cannot
    accidentally expose unverified or private activity merely by calling this
    helper. ``kind=reply`` requires a public issue context and is never offered
    as a new issue candidate.
    """
    if (
        not signature_verified
        or not public_activity
        or not public_repository
        or kind not in {"mention", "reply"}
        or not all(
            _valid_segment(value)
            for value in (actor_owner, data_owner, repo)
        )
        or not isinstance(note, dict)
        or not isinstance(remote, dict)
    ):
        return ""
    remote_id = _public_https_url(note.get("id"))
    remote_url = _public_https_url(note.get("url") or note.get("id"))
    actor_url = _public_https_url(
        remote.get("url") or remote.get("actor_id"))
    if not remote_id or not remote_url or not actor_url:
        return ""
    await runtime.ensure_schema()
    remote_bi = await runtime.blind("world-fediverse-note:" + remote_id)
    existing = await runtime.d1_first(
        "SELECT mention_id FROM world_fediverse_mentions "
        "WHERE remote_id_bi=?",
        remote_bi,
    )
    if existing:
        return str(existing.get("mention_id") or "")
    now = runtime.now()
    mention_id = runtime.new_id()
    issue_number = 0
    issue_url = ""
    if kind == "reply" and isinstance(context, dict):
        try:
            issue_number = max(0, int(context.get("ref") or 0))
        except (TypeError, ValueError):
            issue_number = 0
        issue_url = _public_https_url(context.get("issueUrl"))
    state = "linked" if kind == "reply" and issue_number > 0 else "review"
    record = {
        "remoteId": remote_id,
        "remoteUrl": remote_url,
        "actorUrl": actor_url,
        "inbox": _public_https_url(remote.get("inbox")),
        "sharedInbox": _public_https_url(remote.get("shared_inbox")),
        "author": _clean_text(
            remote.get("handle") or remote.get("actor_id"), 160),
        "authorName": _clean_text(remote.get("display_name"), 160),
        "content": _clean_text(note.get("content"), CONTENT_MAX),
        "published": _clean_text(note.get("published"), 60),
        "actorOwner": str(actor_owner).lower(),
        "dataOwner": str(data_owner).lower(),
        "repo": str(repo),
        "publicRepository": f"{actor_owner}/{repo}",
        "issueUrl": issue_url,
        "followupConsent": False,
        "failure": "",
    }
    repo_bi = await runtime.blind(
        "world-fediverse-repo:" + str(data_owner).lower() + "/" + str(repo))
    await runtime.d1_run(
        "INSERT OR IGNORE INTO world_fediverse_mentions "
        "(mention_id,remote_id_bi,repo_bi,kind,state,data,issue_number,"
        "created_at,updated_at,expires_at) VALUES (?,?,?,?,?,?,?,?,?,?)",
        mention_id,
        remote_bi,
        repo_bi,
        kind,
        state,
        await runtime.seal(record),
        issue_number,
        now,
        now,
        now + RETENTION_MS,
    )
    row = await runtime.d1_first(
        "SELECT mention_id FROM world_fediverse_mentions "
        "WHERE remote_id_bi=?",
        remote_bi,
    )
    return str((row or {}).get("mention_id") or "")


async def _authorize_session(runtime, row, data):
    record = await _decode(runtime, row)
    actor_bi, actor = await runtime.authorize_repository_owner(
        _clean_text(record.get("dataOwner"), 100), data)
    return actor_bi, _clean_text(actor, 100).lower(), record


async def _feed(runtime):
    rows = await runtime.d1_all(
        "SELECT mention_id,kind,state,data,issue_number,followup_state,"
        "created_at,updated_at FROM world_fediverse_mentions "
        "WHERE state!='dismissed' AND expires_at>? "
        "ORDER BY created_at DESC LIMIT ?",
        runtime.now(),
        FEED_LIMIT,
    )
    return _response(
        runtime,
        {
            "ok": True,
            "items": [await _public_item(runtime, row) for row in rows or []],
            "verification": "ActivityPub HTTP signature verified",
            "privacy": (
                "Only sanitized public post fields and generalized progress "
                "are shown. Private repositories are never recorded."
            ),
            "automaticIssueCreation": False,
        },
        public=True,
    )


async def _preview(runtime, row, data):
    actor_bi, actor, record = await _authorize_session(runtime, row, data)
    if not actor_bi:
        return _response(runtime, {"error": "forbidden"}, status=403)
    if row["kind"] != "mention" or row["state"] not in {
        "review", "failed", "pending", "created",
    }:
        return _response(runtime, {"error": "not_reviewable"}, status=409)
    draft = _draft(record)
    return _response(runtime, {
        "ok": True,
        "mentionId": row["mention_id"],
        "repository": record.get("publicRepository", ""),
        "remoteUrl": record.get("remoteUrl", ""),
        "author": record.get("author", ""),
        "draft": draft,
        "state": _public_state(row),
        "issueNumber": (
            int(row.get("issue_number") or 0)
            if row["state"] == "created" else 0
        ),
        "requiresExplicitConfirmation": True,
        "followupDefault": False,
        "followupPolicy": (
            "A public follow-up is sent only with explicit owner consent and "
            "only after the owner node confirms materialization."
        ),
        "reviewer": actor,
    })


async def _create(runtime, row, data):
    actor_bi, actor, record = await _authorize_session(runtime, row, data)
    if not actor_bi:
        return _response(runtime, {"error": "forbidden"}, status=403)
    if row["kind"] != "mention":
        return _response(runtime, {"error": "not_reviewable"}, status=409)
    if row["state"] in {"pending", "created"}:
        return _response(runtime, {
            "ok": True,
            "deduplicated": True,
            "state": row["state"],
            "issueNumber": (
                int(row.get("issue_number") or 0)
                if row["state"] == "created" else 0
            ),
        })
    if row["state"] == "dismissed":
        return _response(runtime, {"error": "dismissed"}, status=409)
    if data.get("confirm") is not True:
        return _response(
            runtime, {"error": "explicit_confirmation_required"}, status=400)
    title = _clean_text(data.get("title"), TITLE_MAX)
    body = _clean_text(data.get("body"), CONTENT_MAX)
    if not title or not body:
        return _response(runtime, {"error": "draft_required"}, status=400)
    now = runtime.now()
    lease_id = runtime.new_id()
    await runtime.d1_run(
        "UPDATE world_fediverse_mentions SET state='creating',"
        "create_lease_id=?,create_lease_until=?,updated_at=? "
        "WHERE mention_id=? AND state IN ('review','failed') "
        "AND create_lease_until<=?",
        lease_id,
        now + CREATE_LEASE_MS,
        now,
        row["mention_id"],
        now,
    )
    claimed = await _row(runtime, row["mention_id"])
    if not claimed or claimed.get("create_lease_id") != lease_id:
        return _response(runtime, {
            "ok": True,
            "deduplicated": True,
            "state": _public_state(claimed or row),
            "issueNumber": 0,
        })
    followup_consent = data.get("publishFollowup") is True
    ok, result = await runtime.enqueue_issue(
        record["dataOwner"],
        record["repo"],
        title,
        body,
        record.get("author") or "fediverse",
        row["mention_id"],
        record.get("remoteUrl") or "",
    )
    record["followupConsent"] = followup_consent
    if not ok:
        record["failure"] = _clean_text(result, 120)
        await runtime.d1_run(
            "UPDATE world_fediverse_mentions SET state='failed',data=?,"
            "create_lease_id='',create_lease_until=0,updated_at=? "
            "WHERE mention_id=? AND create_lease_id=?",
            await runtime.seal(record),
            runtime.now(),
            row["mention_id"],
            lease_id,
        )
        await runtime.audit(
            actor,
            "fediverse_mention.issue_queue",
            "fediverse_mention",
            row["mention_id"],
            outcome="failed",
            details={"reason": "inbox_unavailable"},
        )
        return _response(
            runtime, {"error": "issue_queue_unavailable"}, status=503)
    try:
        proposed = max(0, int((result or {}).get("issueNumber") or 0))
    except (TypeError, ValueError):
        proposed = 0
    record["failure"] = ""
    await runtime.d1_run(
        "UPDATE world_fediverse_mentions SET state='pending',data=?,"
        "issue_number=?,create_lease_id='',create_lease_until=0,"
        "followup_state=?,updated_at=? "
        "WHERE mention_id=? AND create_lease_id=?",
        await runtime.seal(record),
        proposed,
        "ready" if followup_consent else "not-requested",
        runtime.now(),
        row["mention_id"],
        lease_id,
    )
    await runtime.audit(
        actor,
        "fediverse_mention.issue_queue",
        "fediverse_mention",
        row["mention_id"],
        details={
            "state": "pending",
            "followupConsent": followup_consent,
        },
    )
    return _response(runtime, {
        "ok": True,
        "state": "pending",
        "issueNumber": 0,
        "proposedIssueNumber": proposed,
        "progress": "Pending owner-node materialization.",
        "created": False,
    }, status=202)


async def _moderate(runtime, row, data):
    actor_bi, actor, _record = await _authorize_session(runtime, row, data)
    if not actor_bi:
        return _response(runtime, {"error": "forbidden"}, status=403)
    action = _clean_text(data.get("action"), 20).lower()
    reason = _clean_text(data.get("reason"), 600)
    if action not in {"dismiss", "restore"} or not reason:
        return _response(runtime, {"error": "invalid_moderation"}, status=400)
    if action == "dismiss" and row["state"] not in {
        "review", "failed", "linked",
    }:
        return _response(
            runtime, {"error": "pending_or_created_not_dismissible"}, status=409)
    if action == "restore" and row["state"] != "dismissed":
        return _response(runtime, {"error": "not_dismissed"}, status=409)
    next_state = (
        "dismissed"
        if action == "dismiss"
        else ("linked" if row["kind"] == "reply" else "review")
    )
    now = runtime.now()
    await runtime.d1_run(
        "INSERT INTO world_fediverse_mention_moderation "
        "(record_id,mention_id,action,reason,actor_bi,created_at) "
        "VALUES (?,?,?,?,?,?)",
        runtime.new_id(),
        row["mention_id"],
        action,
        reason,
        actor_bi,
        now,
    )
    await runtime.d1_run(
        "UPDATE world_fediverse_mentions SET state=?,updated_at=? "
        "WHERE mention_id=?",
        next_state,
        now,
        row["mention_id"],
    )
    await runtime.audit(
        actor,
        "fediverse_mention.moderation." + action,
        "fediverse_mention",
        row["mention_id"],
        details={"reason": reason},
    )
    return _response(runtime, {"ok": True, "state": next_state})


async def _materialized(runtime, row, data):
    record = await _decode(runtime, row)
    owner = _clean_text(record.get("dataOwner"), 100).lower()
    if not owner or not await runtime.authorize_owner_node(owner):
        return _response(runtime, {"error": "forbidden"}, status=403)
    if row["kind"] != "mention" or row["state"] not in {
        "pending", "created",
    }:
        return _response(runtime, {"error": "not_pending"}, status=409)
    try:
        number = int(data.get("issueNumber") or 0)
    except (TypeError, ValueError):
        number = 0
    if number <= 0:
        return _response(
            runtime, {"error": "materialized_issue_number_required"}, status=400)
    already_created = row["state"] == "created"
    if already_created and int(row.get("issue_number") or 0) != number:
        return _response(
            runtime, {"error": "materialization_conflict"}, status=409)
    origin = runtime.public_origin().rstrip("/")
    public_owner = _clean_text(record.get("actorOwner"), 100)
    repo = _clean_text(record.get("repo"), 100)
    issue_url = (
        f"{origin}/{public_owner}/{repo}/issues/{number}"
        if origin and public_owner and repo else ""
    )
    now = runtime.now()
    if not already_created:
        record["issueUrl"] = issue_url
        await runtime.d1_run(
            "UPDATE world_fediverse_mentions SET state='created',data=?,"
            "issue_number=?,updated_at=? WHERE mention_id=? AND state='pending'",
            await runtime.seal(record),
            number,
            now,
            row["mention_id"],
        )
    else:
        issue_url = _public_https_url(record.get("issueUrl"))
    followup_state = str(row.get("followup_state") or "not-requested")
    if record.get("followupConsent") and followup_state in {"ready", "failed"}:
        delivery_lease = runtime.new_id()
        await runtime.d1_run(
            "UPDATE world_fediverse_mentions SET followup_state='sending',"
            "create_lease_id=?,create_lease_until=?,updated_at=? "
            "WHERE mention_id=? AND followup_state IN "
            "('ready','failed')",
            delivery_lease,
            now + CREATE_LEASE_MS,
            now,
            row["mention_id"],
        )
        claimed = await _row(runtime, row["mention_id"])
        if claimed and claimed.get("create_lease_id") == delivery_lease:
            message = (
                f"Issue #{number} was created after the repository owner node "
                f"materialized the reviewed feedback: {issue_url}"
            )
            sent = await runtime.publish_followup(
                row["mention_id"], record, message, number)
            await runtime.d1_run(
                "UPDATE world_fediverse_mentions SET followup_state=?,"
                "create_lease_id='',create_lease_until=0,updated_at=? "
                "WHERE mention_id=? AND followup_state='sending' "
                "AND create_lease_id=?",
                "sent" if sent else "failed",
                runtime.now(),
                row["mention_id"],
                delivery_lease,
            )
    await runtime.audit(
        owner,
        "fediverse_mention.materialized",
        "fediverse_mention",
        row["mention_id"],
        details={"issueNumber": number},
    )
    return _response(runtime, {
        "ok": True,
        "state": "created",
        "issueNumber": number,
        "issueUrl": issue_url,
        "created": True,
        "deduplicated": already_created,
    })


async def confirm_materialized(runtime, mention_id, issue_number):
    """Confirm one materialization from an already owner-signed inbox ack."""
    if not _valid_id(mention_id):
        return False
    row = await _row(runtime, mention_id)
    if not row:
        return False
    response = await _materialized(
        runtime, row, {"issueNumber": issue_number})
    status = (
        response.get("status", 500)
        if isinstance(response, dict)
        else getattr(response, "status", 500)
    )
    return int(status) < 300


async def handle(runtime, path):
    await runtime.ensure_schema()
    route = _route(path)
    if route is None:
        return _response(runtime, {"error": "not_found"}, status=404)
    kind, mention_id = route
    method = runtime.method()
    if kind == "feed":
        if method != "GET":
            return _response(
                runtime, {"error": "method_not_allowed"}, status=405,
                allow="GET")
        return await _feed(runtime)
    if method != "POST":
        return _response(
            runtime, {"error": "method_not_allowed"}, status=405,
            allow="POST")
    row = await _row(runtime, mention_id)
    if not row:
        return _response(runtime, {"error": "not_found"}, status=404)
    data, error_response = await _body(runtime)
    if error_response:
        return error_response
    if kind == "preview":
        return await _preview(runtime, row, data)
    if kind == "create":
        return await _create(runtime, row, data)
    if kind == "moderate":
        return await _moderate(runtime, row, data)
    if kind == "materialized":
        return await _materialized(runtime, row, data)
    return _response(runtime, {"error": "not_found"}, status=404)


async def cleanup(runtime):
    await runtime.ensure_schema()
    now = runtime.now()
    await runtime.d1_run(
        "DELETE FROM world_fediverse_mention_moderation WHERE mention_id IN "
        "(SELECT mention_id FROM world_fediverse_mentions "
        "WHERE expires_at<=?)",
        now,
    )
    await runtime.d1_run(
        "DELETE FROM world_fediverse_mentions WHERE expires_at<=?",
        now,
    )
    # A worker interruption while an authorized create was claiming its lease
    # makes the item reviewable again; no issue is announced or inferred.
    await runtime.d1_run(
        "UPDATE world_fediverse_mentions SET state='failed',"
        "create_lease_id='',create_lease_until=0,updated_at=? "
        "WHERE state='creating' AND create_lease_until<=?",
        now,
        now,
    )
    await runtime.d1_run(
        "UPDATE world_fediverse_mentions SET followup_state='failed',"
        "create_lease_id='',create_lease_until=0,updated_at=? "
        "WHERE state='created' AND followup_state='sending' "
        "AND create_lease_until<=?",
        now,
        now,
    )
