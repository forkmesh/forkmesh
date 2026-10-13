"""Organization-private universal tasks for the World and dashboard HUD.

The legacy Marketing wall is a filtered view of the same task catalog. The
Worker adapter owns account sessions, organization membership, row encryption,
and D1 access; no task record is returned outside the configured organization.

Task copy, assignee labels, and check-in notes are encrypted at rest.  The
plaintext columns contain only opaque blind indexes, bounded routing/state,
and server-authoritative timer metadata. Nothing is stored in or published
through the Office Durable Object.
"""

import base64
import binascii
import re
from urllib.parse import urlsplit


PREFIX = "/api/world/office/marketing-tasks"
UNIVERSAL_PREFIX = "/api/tasks"
LEGACY_MARKETING_PREFIX = PREFIX
BODY_MAX_BYTES = 64 * 1024
# A catalog refresh may read this many rows across pages, but each Worker
# request decrypts only one small page. Keep the server page deliberately
# below the UI's 100-row display page: projection decrypts each private row,
# and live catalogs near 100 rows have crossed Cloudflare's Python CPU ceiling
# before a response could be written. Clients already follow nextOffset, so a
# smaller transport page changes no visible catalog semantics.
TASK_LIST_RESPONSE_LIMIT = 2000
TASK_LIST_PAGE_SIZE = 25
MAX_CHECKINS_PER_TASK = 50
MAX_TASK_REPLIES = 50
# Keep the response-table name and the public terminology close to the
# endpoint: replies are encrypted response records, never task columns.
MAX_RESPONSES_PER_TASK = MAX_TASK_REPLIES
MAX_REPLY_BODY = 2000
# Every recorded thing that has happened to one task, oldest evicted first.
# The window is deliberately larger than the reply window: a timeline is only
# trustworthy if it keeps the lifecycle transitions the replies refer to.
MAX_TASK_EVENTS = 250
MAX_EVENT_SUMMARY = 200
# A comment can name a lot of people; only a bounded prefix is pinged, so one
# reply can never fan out into an unbounded notification write per request.
MAX_MENTIONS = 10
MAX_COMMENT_READERS = 100
MAX_COMPLETION_HISTORY = 20
MAX_TITLE = 160
MAX_PARENT_DEPTH = 32
MAX_DETAILS = 4000
MAX_COMPLETION_NOTE = 4000
MAX_CHECKIN_NOTE = 500
MAX_REMINDER_NOTE = 500
REMINDER_COOLDOWN_MS = 10 * 60 * 1000
PERSISTENT_REMINDER_DEFAULT_MINUTES = 60
PERSISTENT_REMINDER_MIN_MINUTES = 15
PERSISTENT_REMINDER_MAX_MINUTES = 7 * 24 * 60
PERSISTENT_REMINDER_BATCH = 25
MAX_AGENT_FIELD = 64
MAX_TASK_IMAGES = 4
MAX_TASK_IMAGE_BYTES = 256 * 1024
MAX_TASK_IMAGE_TOTAL_BYTES = 1024 * 1024
TASK_IMAGE_MIMES = frozenset({
    "image/gif", "image/jpeg", "image/png", "image/webp",
})
MIN_PRIORITY = 1
MAX_PRIORITY = 99
DEFAULT_PRIORITY = 50
MIN_PROGRESS = 0
MAX_PROGRESS = 100
MAX_ELAPSED_MS = 10 * 365 * 24 * 60 * 60 * 1000
MAX_BOUNTY_LAMPORTS = 1_000_000 * 1_000_000_000
MAX_PROOFS = 5000
MAX_PROOFS_PER_MEMBER = 100
MAX_INITIATIVES = 250
MAX_WORK_ITEMS = 20
MAX_WORK_ITEM_BRANCH = 200
MAX_WORK_ITEM_WORKTREE = 400
WORK_ITEM_UPDATE_ATTEMPTS = 5
TRASH_RETENTION_MS = 30 * 24 * 60 * 60 * 1000
MAX_DELETE_BATCH = 50
TRASH_PURGE_BATCH = 25
MAX_DELETE_REASON = 500
LIVE_AGENT_RUN_STATES = frozenset({"queued", "running", "waiting"})
TRASH_LIST_STATUSES = frozenset({"deleted", "trash"})
CHECKIN_MIN_MS = 4 * 60 * 1000
CHECKIN_MAX_MS = 9 * 60 * 1000

TASK_STATES = frozenset({"idle", "active", "done"})
AGENT_RUN_STATES = frozenset({
    "queued", "running", "waiting", "success", "failed", "stopped",
})
# How many runs one batched agent-status write may report. A desktop publishes
# one entry per local session it owns; the cap keeps a single signed request
# from turning into an unbounded row scan.
MAX_AGENT_STATUS_BATCH = 200
AGENT_LIVE_STATES = frozenset({"queued", "running", "waiting"})
MAX_AGENT_PROSE = 200
MAX_AGENT_TITLE = 120
PRESENCE_FINISHED_MS = 8 * 60 * 60 * 1000
PRESENCE_STALE_MS = 24 * 60 * 60 * 1000
PRESENCE_PER_ACCOUNT = 40
PRESENCE_LIST_LIMIT = 60
AGENT_STATUS_ERROR_CODES = {
    "invalid_agent_status": 400,
    "task_not_found": 404,
    "forbidden": 403,
    "agent_run_unavailable": 409,
}
TASK_KINDS = frozenset({"task", "bid"})
CHECKIN_STATES = frozenset({"going_well", "blocked", "needs_help"})
CHECKIN_STATE_COPY = {
    "going_well": "going well",
    "blocked": "blocked",
    "needs_help": "needs help",
}
# The timeline vocabulary. Every mutation path records exactly one of these, so
# a client can style an entry without parsing prose, and an unknown kind from a
# newer deployment is dropped rather than rendered as an unlabelled row.
TASK_EVENT_KINDS = frozenset({
    "created", "updated", "started", "stopped", "checkin", "replied",
    "completed", "reopened", "returned", "qa_requested", "qa_reviewed",
    "deleted", "reminded", "reminder_seen", "emailed", "email_opened",
    "email_clicked", "email_viewed",
})
REMINDER_SEEN_VIA = frozenset({"ping", "task"})
DESTINATIONS = frozenset({
    "department", "personal", "repository", "qa", "agent",
})
# One general bot replaces the old per-vendor Claude/Codex choice. The legacy
# kinds still decode from stored rows and are projected as "agent" so an
# existing task keeps working after the picker was removed.
AGENT_ASSIGNEE_KINDS = frozenset({"agent", "bot", "claude", "codex"})
ASSIGNEE_KINDS = (
    frozenset({"user", "unassigned"}) | AGENT_ASSIGNEE_KINDS
)
QA_STATES = frozenset({"unknown", "passed", "failed"})
DEPARTMENTS = (
    "general", "marketing", "engineering", "product-design", "security",
    "infrastructure", "community", "partnerships", "operations", "executive",
    "quality-assurance",
)
PERMISSION_RANK = {
    "read": 0,
    "write": 1,
    "maintain": 2,
    "admin": 3,
}

_ID_RE = re.compile(r"^[a-f0-9]{32}$")
# Matches the relay's own MENTION_RE so a name that pings here is a name that
# pings everywhere else. An address-like "@" inside an email never matches.
_MENTION_RE = re.compile(
    r"(?<![A-Za-z0-9._%+-])@([a-z](?:[a-z0-9-]{0,61}[a-z0-9])?)\b")
_AGENT_REF_RE = re.compile(r"^[A-Za-z0-9._@#/-]{1,64}$")
_REPO_SEGMENT_RE = re.compile(r"^[A-Za-z0-9._-]{1,100}$")
_SCOPE_RE = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?$")
_BRANCH_RE = re.compile(r"^[A-Za-z0-9._/+-]{1,200}$")
_NODE_NAME_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
_WINDOWS_ABSOLUTE_RE = re.compile(r"^[A-Za-z]:[\\/]")
_SOL_AMOUNT_RE = re.compile(r"^(0|[1-9][0-9]{0,6})(?:\.([0-9]{1,9}))?$")


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


def _text(value, maximum, fallback=""):
    """Bound user-facing copy without retaining controls or markup."""

    clean = "".join(
        character
        if character.isprintable() and character not in "<>"
        else " "
        for character in str(value or "")
    )
    clean = " ".join(clean.split()).strip()
    return (clean or fallback)[:maximum]


def _multiline(value, maximum, fallback=""):
    text = str(value or "").replace("\r\n", "\n").replace("\r", "\n")
    text = text.replace("\t", "    ")
    clean = "".join(
        character
        if character == "\n" or character.isprintable()
        else " "
        for character in text
    )
    lines = [line.rstrip() for line in clean.split("\n")]
    clean = re.sub(r"\n{4,}", "\n\n\n", "\n".join(lines)).strip("\n")
    if not clean.strip():
        clean = ""
    return (clean or fallback)[:maximum]


def _attachments(value):
    """Keep bounded metadata plus a tiny encrypted image preview with a task."""
    if not isinstance(value, list):
        return []
    result = []
    for item in value[:MAX_TASK_IMAGES]:
        if not isinstance(item, dict):
            continue
        attachment_id = str(item.get("id") or "").strip().lower()
        name = _text(item.get("name"), 180)
        mime = _text(item.get("mime"), 100, "application/octet-stream")
        try:
            size = max(
                0, min(MAX_TASK_IMAGE_BYTES, int(item.get("size") or 0)))
        except (TypeError, ValueError):
            size = 0
        if name and size:
            attachment = {"name": name, "mime": mime, "size": size}
            task_id = str(item.get("taskId") or "").strip().lower()
            if valid_id(attachment_id) and valid_id(task_id):
                attachment["id"] = attachment_id
                attachment["url"] = (
                    UNIVERSAL_PREFIX + "/" + task_id +
                    "/attachments/" + attachment_id
                )
            thumbnail = str(item.get("thumbnail") or "")
            if (
                mime in ("image/png", "image/jpeg", "image/webp")
                and len(thumbnail) <= 10_000
                and re.fullmatch(
                    r"data:image/(?:png|jpeg|webp);base64,[A-Za-z0-9+/=]+",
                    thumbnail,
                )
            ):
                attachment["thumbnail"] = thumbnail
            result.append(attachment)
    return result


def _image_magic_matches(mime, raw):
    if mime == "image/png":
        return raw.startswith(b"\x89PNG\r\n\x1a\n")
    if mime == "image/jpeg":
        return raw.startswith(b"\xff\xd8\xff")
    if mime == "image/gif":
        return raw.startswith((b"GIF87a", b"GIF89a"))
    if mime == "image/webp":
        return (
            len(raw) >= 12
            and raw.startswith(b"RIFF")
            and raw[8:12] == b"WEBP"
        )
    return False


def _task_image_uploads(value):
    """Decode a bounded set of raster images without accepting active SVG."""
    if value in (None, []):
        return [], ""
    if not isinstance(value, list) or len(value) > MAX_TASK_IMAGES:
        return [], "too_many_task_images"
    uploads = []
    total = 0
    for item in value:
        if not isinstance(item, dict):
            return [], "invalid_task_image"
        name = _text(item.get("name"), 180)
        mime = _text(item.get("mime"), 100).lower()
        encoded = str(item.get("file") or "")
        if not encoded:
            # Older clients sent display-only metadata. Preserve that contract;
            # only records carrying validated bytes enter the image store.
            try:
                size = int(item.get("size") or 0)
            except (TypeError, ValueError, OverflowError):
                size = 0
            if not name or not mime or size <= 0:
                return [], "invalid_task_image"
            metadata = {
                "name": name,
                "mime": mime,
                "size": min(size, MAX_TASK_IMAGE_TOTAL_BYTES),
                "file": "",
            }
            thumbnail = _attachments([item])
            if thumbnail and thumbnail[0].get("thumbnail"):
                metadata["thumbnail"] = thumbnail[0]["thumbnail"]
            uploads.append(metadata)
            continue
        if not name or mime not in TASK_IMAGE_MIMES:
            return [], "invalid_task_image"
        if len(encoded) > ((MAX_TASK_IMAGE_BYTES + 2) // 3) * 4 + 4:
            return [], "task_image_too_large"
        try:
            raw = base64.b64decode(encoded, validate=True)
        except (binascii.Error, ValueError):
            return [], "invalid_task_image"
        if (
            not raw
            or len(raw) > MAX_TASK_IMAGE_BYTES
            or not _image_magic_matches(mime, raw)
        ):
            return [], (
                "task_image_too_large"
                if len(raw) > MAX_TASK_IMAGE_BYTES
                else "invalid_task_image"
            )
        total += len(raw)
        if total > MAX_TASK_IMAGE_TOTAL_BYTES:
            return [], "task_images_too_large"
        uploads.append({
            "name": name,
            "mime": mime,
            "size": len(raw),
            "file": base64.b64encode(raw).decode("ascii"),
        })
    return uploads, ""


def _valid_repository(value):
    if not value:
        return True
    parts = value.split("/", 1)
    return (
        len(parts) == 2
        and bool(_REPO_SEGMENT_RE.fullmatch(parts[0]))
        and bool(_REPO_SEGMENT_RE.fullmatch(parts[1]))
    )


def valid_branch(value):
    value = str(value or "")
    if not _BRANCH_RE.fullmatch(value):
        return False
    if (
        value.startswith(("-", "/", "."))
        or value.endswith(("/", ".", ".lock"))
        or ".." in value
        or "//" in value
        or value == "HEAD"
    ):
        return False
    return not any(
        part.startswith(".") or part.endswith(".lock")
        for part in value.split("/")
    )


def valid_worktree_path(value):
    value = str(value or "")
    if not value:
        return True
    if len(value) > MAX_WORK_ITEM_WORKTREE:
        return False
    if any(not character.isprintable() for character in value):
        return False
    if not (value.startswith("/") or _WINDOWS_ABSOLUTE_RE.match(value)):
        return False
    return ".." not in re.split(r"[\\/]", value)


def _work_item(item):
    if not isinstance(item, dict):
        return None
    repository = str(item.get("repository") or "").strip()
    branch = str(item.get("branch") or "").strip()
    worktree = str(item.get("worktree") or "")
    node = str(item.get("node") or "").strip().lower()
    if (
        len(repository) > 201
        or not _valid_repository(repository)
        or not valid_branch(branch)
        or not valid_worktree_path(worktree)
        or (node and not _NODE_NAME_RE.fullmatch(node))
    ):
        return None
    return {
        "repository": repository,
        "branch": branch,
        "worktree": worktree,
        "node": node,
    }


def _work_items(value, strict=False):
    if value in (None, ""):
        return [], ""
    if not isinstance(value, list):
        return [], "invalid_work_items"
    ordered = {}
    for raw in value:
        item = _work_item(raw)
        if item is None:
            if strict:
                return [], "invalid_work_item"
            continue
        ordered[_work_item_key(item)] = item
    if strict and len(ordered) > MAX_WORK_ITEMS:
        return [], "too_many_work_items"
    return list(ordered.values())[:MAX_WORK_ITEMS], ""


def valid_id(value):
    return bool(_ID_RE.fullmatch(str(value or "").strip().lower()))


def _priority(value, default=DEFAULT_PRIORITY):
    try:
        value = int(value)
    except (TypeError, ValueError, OverflowError):
        value = int(default)
    return max(MIN_PRIORITY, min(MAX_PRIORITY, value))


def _progress(value, default=MIN_PROGRESS):
    try:
        value = int(value)
    except (TypeError, ValueError, OverflowError):
        value = int(default)
    return max(MIN_PROGRESS, min(MAX_PROGRESS, value))


def _persistent_reminder_every_ms(value):
    if value is None:
        return None, ""
    if isinstance(value, bool):
        enabled, minutes = value, PERSISTENT_REMINDER_DEFAULT_MINUTES
    elif isinstance(value, dict):
        enabled = value.get("enabled", True)
        if not isinstance(enabled, bool):
            return None, "invalid_persistent_reminder"
        minutes = value.get("everyMinutes", PERSISTENT_REMINDER_DEFAULT_MINUTES)
    else:
        return None, "invalid_persistent_reminder"
    if not enabled:
        return 0, ""
    if isinstance(minutes, bool) or not isinstance(minutes, (int, float)):
        return None, "invalid_persistent_reminder"
    if minutes != minutes:
        return None, "invalid_persistent_reminder"
    minutes = int(round(minutes))
    if not (PERSISTENT_REMINDER_MIN_MINUTES <= minutes
            <= PERSISTENT_REMINDER_MAX_MINUTES):
        return None, "invalid_persistent_reminder_interval"
    return minutes * 60 * 1000, ""


def _project_persistent_reminder(row, state):
    every_ms = max(0, _integer(row.get("reminder_every_ms")))
    return {
        "enabled": every_ms > 0,
        "everyMinutes": every_ms // 60000,
        "nextAt": (
            max(0, _integer(row.get("reminder_next_at")))
            if every_ms and state != "done" else 0
        ),
    }


def _can_edit_progress(row, account_bi, can_manage):
    account_bi = str(account_bi or "")
    return bool(
        can_manage
        or (account_bi and str(row.get("created_by_bi") or "") == account_bi)
        or (account_bi and str(row.get("assignee_bi") or "") == account_bi)
    )


def _agent_ref(value):
    """Bound an opaque desktop-side agent reference (bot label, session id).

    Anything that is not already a compact token is dropped rather than
    squeezed into one: a mangled bot label is worse than no bot label.
    """

    clean = _text(value, MAX_AGENT_FIELD)
    return clean if _AGENT_REF_RE.fullmatch(clean) else ""


_SECRET_RES = (
    re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----.*?(?:-----END [A-Z ]*PRIVATE KEY-----|$)", re.S),
    re.compile(r"\b(?:sk-ant-|sk-proj-|sk-|ghp_|gho_|ghu_|ghs_|github_pat_|xox[abprs]-|glpat-|AKIA|ASIA)[A-Za-z0-9_\-]{8,}"),
    re.compile(r"\beyJ[A-Za-z0-9_\-]{10,}\.[A-Za-z0-9_\-]{10,}\.[A-Za-z0-9_\-]{5,}"),
    re.compile(r"(?i)\b(password|passwd|secret|token|api[_-]?key)\s*[:=]\s*\S+"),
    re.compile(r"\b[A-Fa-f0-9]{32,}\b"),
    re.compile(r"\b[A-Za-z0-9+/_\-]{40,}={0,2}"),
)


def _redact(value, limit):
    text = re.sub(r"\s+", " ", str(value or "")).strip()
    for pattern in _SECRET_RES:
        text = pattern.sub("[redacted]", text)
    return _text(text, limit)


def _run_by(value):
    return value.get("runBy") if isinstance(value, dict) else ""


def _agent_run(value, run_by=""):
    """Bound the run provenance a desktop records against an agent task.

    A prompt launched from the desktop opens the task, so the task itself has
    to explain the run without reaching back into a transcript nobody else can
    read: which bot opened it, which bot reported it finished, and the model,
    permission mode, and reasoning strength the run used. Every field is
    encrypted with the rest of the task copy; ``None`` means "no agent run".
    """

    record = value if isinstance(value, dict) else {}
    run = {
        "provider": _text(record.get("provider"), 40).lower(),
        "startedBy": _agent_ref(record.get("startedBy")).lower(),
        "finishedBy": _agent_ref(record.get("finishedBy")).lower(),
        "model": _text(record.get("model"), MAX_AGENT_FIELD),
        "mode": _text(record.get("mode"), 40),
        "strength": _text(record.get("strength"), 32).lower(),
        "sessionId": _agent_ref(record.get("sessionId")),
        "runBy": _text(run_by, 64).lower(),
        "prose": _redact(record.get("prose"), MAX_AGENT_PROSE),
        "status": (
            _text(record.get("status"), 16).lower()
            if _text(record.get("status"), 16).lower() in AGENT_RUN_STATES
            else ""
        ),
    }
    if not run["runBy"]:
        del run["runBy"]
    if not run["prose"]:
        del run["prose"]
    return run if any(run.values()) else None


def _sol_amount(value):
    """Return a canonical SOL string backed by an exact lamport integer."""

    match = _SOL_AMOUNT_RE.fullmatch(str(value or "").strip())
    if not match:
        return "", 0
    whole = int(match.group(1))
    fraction = (match.group(2) or "").ljust(9, "0")
    lamports = whole * 1_000_000_000 + int(fraction or "0")
    if lamports <= 0 or lamports > MAX_BOUNTY_LAMPORTS:
        return "", 0
    canonical_fraction = f"{lamports % 1_000_000_000:09d}".rstrip("0")
    canonical = str(lamports // 1_000_000_000)
    if canonical_fraction:
        canonical += "." + canonical_fraction
    return canonical, lamports


def _route(path):
    clean = str(path or "").rstrip("/")
    base = next(
        (
            candidate
            for candidate in (UNIVERSAL_PREFIX, LEGACY_MARKETING_PREFIX)
            if clean == candidate or clean.startswith(candidate + "/")
        ),
        "",
    )
    if not base:
        return None
    if clean == base:
        return ("collection", "", "")
    # Collection actions must be resolved before the opaque task-id parser.
    if clean == base + "/stop-active":
        return ("stop-active", "", "stop-active")
    if clean == base + "/proofs":
        return ("proofs", "", "proofs")
    if clean == base + "/initiatives":
        return ("initiatives", "", "initiatives")
    # The whole fleet's live agent state in one write, so a desktop restart
    # publishes a single request instead of one per session (adhoc #1618).
    if clean == base + "/agent-status":
        return ("agent-status-batch", "", "agent-status")
    if clean == base + "/delete":
        return ("delete-batch", "", "delete")
    if not clean.startswith(base + "/"):
        return None
    parts = clean[len(base) + 1:].split("/")
    if not parts or not valid_id(parts[0]):
        return None
    task_id = parts[0].lower()
    if len(parts) == 1:
        return ("task", task_id, "")
    if (
        len(parts) == 3
        and parts[1] == "attachments"
        and valid_id(parts[2])
    ):
        return ("attachment", task_id, parts[2].lower())
    if len(parts) == 2 and parts[1] in (
            "start", "stop", "checkin", "remind", "complete", "qa",
            "return", "agent-status", "undo", "restore"):
        return ("action", task_id, parts[1])
    if len(parts) == 2 and parts[1] in ("replies", "responses", "reply"):
        return ("replies", task_id, "replies")
    if len(parts) == 2 and parts[1] in ("history", "activity", "events"):
        return ("history", task_id, "history")
    return None


async def _body(runtime):
    value, error = await runtime.json_body(BODY_MAX_BYTES)
    if error:
        status = 413 if error == "payload_too_large" else 400
        return None, _response(runtime, {"error": error}, status=status)
    return value, None


async def _actor(runtime, data=None):
    account_bi, record = await runtime.session(data or {})
    actor = _text((record or {}).get("name"), 64).lower()
    return str(account_bi or ""), actor


def _can_manage(role, permission):
    return (
        str(role or "") in ("owner", "admin")
        or PERMISSION_RANK.get(str(permission or ""), -1)
        >= PERMISSION_RANK["maintain"]
    )


def _checkin_deadline(runtime, now):
    """Return a cryptographically seeded deadline between 4 and 9 minutes."""

    token = str(runtime.new_id() or "")
    try:
        seed = int(token[:16], 16)
    except (TypeError, ValueError):
        seed = sum(ord(character) for character in token)
    span = CHECKIN_MAX_MS - CHECKIN_MIN_MS
    return int(now) + CHECKIN_MIN_MS + (seed % (span + 1))


async def _latest_checkins(runtime, task_ids):
    if not task_ids:
        return {}
    # task_ids originate in the D1 result and are validated opaque hex ids.
    task_ids = [
        str(value).lower() for value in task_ids if valid_id(value)
    ][:TASK_LIST_RESPONSE_LIMIT]
    if not task_ids:
        return {}
    rows = []
    # Keep each D1 statement well below its bound-parameter ceiling.
    for offset in range(0, len(task_ids), 50):
        chunk = task_ids[offset:offset + 50]
        placeholders = ",".join("?" for _value in chunk)
        rows.extend(await runtime.d1_all(
            "SELECT c.* FROM organization_task_checkins c "
            "WHERE c.task_id IN (" + placeholders + ") "
            "AND c.checkin_id=("
            "SELECT newest.checkin_id "
            "FROM organization_task_checkins newest "
            "WHERE newest.task_id=c.task_id "
            "ORDER BY newest.created_at DESC,newest.checkin_id DESC LIMIT 1)",
            *chunk,
        ) or [])
    return {str(row.get("task_id") or ""): row for row in rows or []}


async def _project_checkin(runtime, row):
    if not row:
        return None
    try:
        data = await runtime.open(row.get("data"))
    except Exception:
        return None
    if not isinstance(data, dict):
        return None
    state = str(row.get("state") or "")
    if state not in CHECKIN_STATES:
        return None
    return {
        "state": state,
        "note": _text(data.get("note"), MAX_CHECKIN_NOTE),
        "at": int(row.get("created_at") or 0),
    }


def _last_reminder(value):
    if not isinstance(value, dict):
        return None
    at = max(0, _integer(value.get("at")))
    if not at:
        return None
    delivery = value.get("delivery")
    known = isinstance(delivery, dict)
    delivery = delivery if known else {}
    seen_at = max(0, _integer(value.get("seenAt")))
    seen_via = _text(value.get("seenVia"), 16).lower()
    return {
        "at": at,
        "by": _text(value.get("by"), 64).lower(),
        "to": _text(value.get("to"), 64).lower(),
        "note": _text(value.get("note"), MAX_REMINDER_NOTE),
        "delivered": bool(delivery.get("pinged")) if known else None,
        "phones": max(0, _integer(delivery.get("phones"))),
        "seenAt": seen_at,
        "seenVia": (
            seen_via if seen_at and seen_via in REMINDER_SEEN_VIA else ""),
    }


def _integer(value, default=0):
    try:
        return int(value)
    except (TypeError, ValueError, OverflowError):
        return default


def _completion_history(value):
    """Project a small, durable completion ledger from the sealed payload."""

    if not isinstance(value, list):
        return []
    history = []
    for item in value[-MAX_COMPLETION_HISTORY:]:
        if not isinstance(item, dict):
            continue
        completed_at = max(0, _integer(item.get("completedAt")))
        note = _multiline(item.get("note"), MAX_COMPLETION_NOTE)
        if not completed_at and not note:
            continue
        qa_status = str(item.get("qaStatus") or "unknown").lower()
        if qa_status not in QA_STATES:
            qa_status = "unknown"
        entry = {
            "completedAt": completed_at,
            "completedBy": _text(item.get("completedBy"), 64).lower(),
            "note": note,
            "qaStatus": qa_status,
            "qaReviewer": _text(item.get("qaReviewer"), 64).lower(),
            "qaReviewedAt": max(0, _integer(item.get("qaReviewedAt"))),
        }
        undone_at = max(0, _integer(item.get("undoneAt")))
        if undone_at:
            entry["undoneAt"] = undone_at
            entry["undoneBy"] = _text(item.get("undoneBy"), 64).lower()
        history.append(entry)
    return history[-MAX_COMPLETION_HISTORY:]


def _routing_snapshot_values(
        department="general", team="", destination="department",
        assignee_kind="user", assignee="", repository=""):
    kind = str(assignee_kind or "user").strip().lower()
    if kind in AGENT_ASSIGNEE_KINDS:
        kind = "agent"
    if kind not in ASSIGNEE_KINDS:
        kind = "user"
    route = str(destination or "department").strip().lower()
    if route not in DESTINATIONS:
        route = "department"
    return {
        "department": _text(department, 64, "general").lower(),
        "team": _text(team, 64).lower(),
        "destination": route,
        "assigneeKind": kind,
        "assignee": (
            "agent" if kind == "agent"
            else _text(assignee, 64).lower()
        ),
        "repository": _text(repository, 201),
    }


def _routing_snapshot(row, data):
    return _routing_snapshot_values(
        row.get("department"),
        row.get("team"),
        row.get("destination"),
        row.get("assignee_kind"),
        data.get("assignee"),
        data.get("repository"),
    )


def _compact_task_context(task):
    """Return enough ownership/routing/QA state to place a reply safely."""

    qa = task.get("qa") if isinstance(task.get("qa"), dict) else {}
    return {
        "taskId": task.get("id", ""),
        "title": _text(task.get("title"), MAX_TITLE, "Organization task"),
        "ownership": {
            "createdBy": _text(task.get("createdBy"), 64).lower(),
            "assignee": _text(task.get("assignee"), 64).lower(),
            "assigneeKind": _text(task.get("assigneeKind"), 16).lower(),
        },
        "routing": {
            "department": _text(task.get("department"), 64, "general").lower(),
            "team": _text(task.get("team"), 64).lower(),
            "destination": _text(task.get("destination"), 32, "department").lower(),
            "repository": _text(task.get("repository"), 201),
        },
        "status": _text(task.get("status"), 16, "idle").lower(),
        "completedAt": max(0, _integer(task.get("completedAt"))),
        "qa": {
            "status": _text(qa.get("status"), 16, "unknown").lower(),
            "requestedAt": max(0, _integer(qa.get("requestedAt"))),
            "reviewedAt": max(0, _integer(qa.get("reviewedAt"))),
            "reviewer": _text(qa.get("reviewer"), 64).lower(),
        },
    }


async def _latest_task_replies(runtime, org_bi, task_id):
    """Read only the bounded newest reply window for one authorized task."""

    try:
        rows = await runtime.d1_all(
            "SELECT response_id,author_bi,data,created_at "
            "FROM organization_task_responses "
            "WHERE org_bi=? AND task_id=? "
            "ORDER BY created_at DESC,response_id DESC LIMIT ?",
            org_bi,
            task_id,
            MAX_TASK_REPLIES,
        )
    except Exception:
        # A rolling deployment can briefly serve code before the additive
        # migration. A task read must remain available during that window.
        return []
    replies = []
    for row in reversed(rows or []):
        try:
            data = await runtime.open(row.get("data"))
        except Exception:
            continue
        if not isinstance(data, dict):
            continue
        response_id = str(row.get("response_id") or "").lower()
        body = _multiline(data.get("body"), MAX_REPLY_BODY)
        if not valid_id(response_id) or not body:
            continue
        context = data.get("context")
        replies.append({
            "id": response_id,
            "author": _text(
                data.get("author") or row.get("author_bi"), 64).lower(),
            "body": body,
            "context": context if isinstance(context, dict) else {},
            "createdAt": max(0, _integer(row.get("created_at"))),
        })
    return replies[-MAX_TASK_REPLIES:]


def _comment_reads(data):
    reads = data.get("commentReads") if isinstance(data, dict) else None
    if not isinstance(reads, dict):
        return {}
    out = {}
    for name, at in reads.items():
        reader = _text(name, 64).lower()
        when = max(0, _integer(at))
        if reader and when:
            out[reader] = when
    return out


def _with_read_receipts(replies, reads):
    receipts = []
    for reply in replies or []:
        author = str(reply.get("author") or "")
        at = _integer(reply.get("createdAt"))
        receipts.append(dict(reply, readBy=[
            {"name": name, "at": when}
            for name, when in sorted(reads.items(), key=lambda item: item[1])
            if name != author and when >= at
        ]))
    return receipts


async def _task_replies_with_receipts(runtime, org_bi, task_id, row):
    replies = await _latest_task_replies(runtime, org_bi, task_id)
    try:
        data = await runtime.open(row.get("data"))
    except Exception:
        data = {}
    return _with_read_receipts(replies, _comment_reads(data))


async def _task_reply_stats(runtime, org_bi, task_id):
    try:
        row = await runtime.d1_first(
            "SELECT COUNT(*) AS count,MAX(created_at) AS latest "
            "FROM organization_task_responses WHERE org_bi=? AND task_id=?",
            org_bi,
            task_id,
        )
    except Exception:
        return 0, 0
    return max(0, _integer((row or {}).get("count"))), max(
        0, _integer((row or {}).get("latest")))


async def _task_reply_stats_for_tasks(runtime, org_bi, task_ids):
    ids = [str(value).lower() for value in task_ids if valid_id(value)]
    result = {}
    for offset in range(0, len(ids), 50):
        chunk = ids[offset:offset + 50]
        if not chunk:
            continue
        placeholders = ",".join("?" for _value in chunk)
        try:
            rows = await runtime.d1_all(
                "SELECT task_id,COUNT(*) AS count,MAX(created_at) AS latest "
                "FROM organization_task_responses WHERE org_bi=? AND task_id IN ("
                + placeholders + ") GROUP BY task_id",
                org_bi,
                *chunk,
            )
        except Exception:
            return result
        for row in rows or []:
            task_id = str(row.get("task_id") or "").lower()
            result[task_id] = (
                max(0, _integer(row.get("count"))),
                max(0, _integer(row.get("latest"))),
            )
    return result


async def _project_task(runtime, row, now, checkin=None, replies=None):
    try:
        data = await runtime.open(row.get("data"))
    except Exception:
        return None
    if not isinstance(data, dict):
        return None
    state = (
        "done"
        if int(row.get("completed_at") or 0) > 0
        else str(row.get("status") or "")
    )
    if state not in TASK_STATES:
        return None
    started_at = int(row.get("started_at") or 0) if state == "active" else 0
    elapsed_ms = max(0, int(row.get("elapsed_ms") or 0))
    if started_at:
        elapsed_ms += max(0, int(now) - started_at)
    assignee_kind = str(row.get("assignee_kind") or "user")
    if assignee_kind in AGENT_ASSIGNEE_KINDS:
        assignee_kind = "agent"
    elif assignee_kind not in ASSIGNEE_KINDS:
        assignee_kind = "user"
    qa_status = str(row.get("qa_status") or "unknown")
    if qa_status not in QA_STATES:
        qa_status = "unknown"
    task_kind = str(data.get("kind") or "task").strip().lower()
    if task_kind not in TASK_KINDS:
        task_kind = "task"
    bounty_request = None
    if task_kind == "bid":
        amount_sol, lamports = _sol_amount(
            (data.get("bountyRequest") or {}).get("amountSol")
            if isinstance(data.get("bountyRequest"), dict)
            else ""
        )
        if amount_sol and lamports:
            bounty_request = {
                "currency": "SOL",
                "amountSol": amount_sol,
                "lamports": lamports,
                "status": "requested",
            }
    task = {
        "id": str(row.get("task_id") or ""),
        "kind": task_kind,
        "title": _text(data.get("title"), MAX_TITLE, "Organization task"),
        "details": _multiline(data.get("details"), MAX_DETAILS),
        "attachments": _attachments(data.get("attachments")),
        "completionNote": _multiline(
            data.get("completionNote"), MAX_COMPLETION_NOTE),
        "createdBy": _text(data.get("createdBy"), 64).lower(),
        "parentTaskId": (
            str(data.get("parentTaskId") or "").lower()
            if valid_id(data.get("parentTaskId")) else ""
        ),
        "bountyRequest": bounty_request,
        "assignee": (
            "agent"
            if assignee_kind == "agent"
            else _text(data.get("assignee"), 64).lower()
        ),
        "assigneeKind": assignee_kind,
        "department": _text(row.get("department"), 64, "general").lower(),
        "team": _text(row.get("team"), 64).lower(),
        "destination": str(row.get("destination") or "department"),
        "priority": _priority(row.get("priority")),
        "progress": (
            MAX_PROGRESS if state == "done"
            else _progress(row.get("progress"))
        ),
        "repository": _text(data.get("repository"), 201),
        "workItems": _work_items(data.get("workItems"))[0],
        "agentSessionId": str(row.get("agent_session_id") or ""),
        "agent": _agent_run(data.get("agent"), _run_by(data.get("agent"))),
        "qa": {
            "status": qa_status,
            "reviewer": _text(data.get("qaReviewer"), 64).lower(),
            "reviewedAt": int(row.get("qa_reviewed_at") or 0),
            "requestedAt": int(row.get("qa_requested_at") or 0),
            "howToTest": _multiline(data.get("howToTest"), 720),
        },
        "status": state,
        "elapsedMs": min(MAX_ELAPSED_MS, elapsed_ms),
        "startedAt": started_at,
        "nextCheckinAt": (
            int(row.get("next_checkin_at") or 0)
            if state == "active" else 0
        ),
        "createdAt": int(row.get("created_at") or 0),
        "updatedAt": int(row.get("updated_at") or 0),
        "completedAt": int(row.get("completed_at") or 0),
        "lastCheckin": await _project_checkin(runtime, checkin),
        "lastReminder": _last_reminder(data.get("reminder")),
        "persistentReminder": _project_persistent_reminder(row, state),
    }
    task["completionHistory"] = _completion_history(
        data.get("completionHistory"))
    task["routing"] = _routing_snapshot(row, data)
    task["ownership"] = {
        "createdBy": task["createdBy"],
        "assignee": task["assignee"],
        "assigneeKind": task["assigneeKind"],
    }
    task["context"] = _compact_task_context(task)
    task["replies"] = _with_read_receipts(replies, _comment_reads(data))
    deleted_at = max(0, _integer(row.get("deleted_at")))
    task["deletedAt"] = deleted_at
    if deleted_at:
        trash = data.get("trash") if isinstance(data.get("trash"), dict) else {}
        task["deletedBy"] = _text(trash.get("by"), 64).lower()
        task["deleteReason"] = _multiline(trash.get("reason"), MAX_DELETE_REASON)
        task["purgeAt"] = deleted_at + TRASH_RETENTION_MS
    return task


async def _notify_activity(
        runtime, org_bi, actor, task_id, row, now, action, changes=None,
        participants=None, skip=()):
    """Best-effort private HUD fan-out; mutations never depend on it."""
    notifier = getattr(runtime, "notify_organization_task_activity", None)
    if not callable(notifier) and action == "started":
        notifier = getattr(runtime, "notify_engineering_task_started", None)
        if callable(notifier):
            try:
                projected = await _project_task(runtime, row, now)
                if projected:
                    await notifier(org_bi, actor, task_id, projected)
            except Exception:
                pass
        return
    if not callable(notifier) or not row:
        return
    try:
        projected = (
            row if isinstance(row, dict) and "title" in row
            else await _project_task(runtime, row, now)
        )
        if projected:
            await notifier(
                org_bi, actor, task_id, projected, action,
                changes=changes, participants=participants,
                skip=list(skip or []))
    except Exception:
        pass


async def _participants(runtime, org_bi, task_id):
    reader = getattr(runtime, "task_participants", None)
    if not callable(reader):
        return None
    try:
        return list(await reader(org_bi, task_id) or [])
    except Exception:
        return None


def _mentions(*parts):
    """Return the bounded, de-duplicated @names named in free-form copy."""

    found = []
    for part in parts:
        for match in _MENTION_RE.finditer(str(part or "").lower()):
            name = match.group(1)
            if name and name not in found:
                found.append(name)
            if len(found) >= MAX_MENTIONS:
                return found
    return found


def _work_item_key(item):
    return (item["repository"].lower(), item["branch"], item["node"])


def _merge_work_items(current, attach, detach):
    ordered = {_work_item_key(item): item for item in current}
    for item in detach:
        ordered.pop(_work_item_key(item), None)
    for item in attach:
        ordered[_work_item_key(item)] = item
    if len(ordered) > MAX_WORK_ITEMS:
        return [], "too_many_work_items"
    return list(ordered.values()), ""


def _changed_rows(result):
    meta = getattr(result, "meta", None)
    if meta is None and isinstance(result, dict):
        meta = result.get("meta")
    if meta is None:
        return None
    changes = (
        meta.get("changes") if isinstance(meta, dict)
        else getattr(meta, "changes", None)
    )
    try:
        return int(changes)
    except (TypeError, ValueError):
        return None


def _work_items_summary(previous, current):
    before = {_work_item_key(item) for item in previous}
    after = {_work_item_key(item) for item in current}
    added = [
        item["branch"] for item in current
        if _work_item_key(item) not in before
    ]
    removed = [
        item["branch"] for item in previous
        if _work_item_key(item) not in after
    ]
    parts = []
    if added:
        parts.append("attached " + ", ".join(added[:3]))
    if removed:
        parts.append("detached " + ", ".join(removed[:3]))
    return "; ".join(parts) or "work items"


def _event_summary(kind, detail):
    """One short, already-bounded line describing a recorded event."""

    detail = detail if isinstance(detail, dict) else {}
    if kind == "checkin":
        state = CHECKIN_STATE_COPY.get(str(detail.get("state") or ""), "")
        note = _text(detail.get("note"), MAX_EVENT_SUMMARY)
        if state and note:
            return "%s — %s" % (state, note)
        return state or note
    if kind == "replied":
        return _text(detail.get("body"), MAX_EVENT_SUMMARY)
    if kind in ("completed", "reopened"):
        return _text(detail.get("note"), MAX_EVENT_SUMMARY)
    if kind == "qa_reviewed":
        return _text(detail.get("verdict"), 32)
    if kind == "updated":
        changed = [
            _text(field, 32)
            for field in (detail.get("fields") or [])
            if _text(field, 32)
        ]
        return _text(", ".join(changed[:8]), MAX_EVENT_SUMMARY)
    return _text(detail.get("summary"), MAX_EVENT_SUMMARY)


async def _record_event(
        runtime, org_bi, task_id, account_bi, actor, kind, now, detail=None):
    """Append one sealed timeline entry; never fails the mutation it records.

    The actor name and the human summary live inside the sealed payload for the
    same reason task copy does: the plaintext columns carry only the bounded
    kind, the blind index of who acted, and the timestamp.
    """

    if kind not in TASK_EVENT_KINDS:
        return ""
    event_id = runtime.new_id()
    if not valid_id(event_id):
        return ""
    detail = detail if isinstance(detail, dict) else {}
    mentions = [
        name for name in (detail.get("mentions") or [])
        if isinstance(name, str)
    ][:MAX_MENTIONS]
    sealed = await runtime.seal({
        "actor": _text(actor, 64).lower(),
        "summary": _event_summary(kind, detail),
        "mentions": mentions,
    })
    try:
        # Trim before insert rather than in a trigger: the same statement then
        # bounds databases created by the lazy schema installer, which has no
        # migration file to carry one.
        await runtime.d1_run(
            "DELETE FROM organization_task_events WHERE event_id IN ("
            "SELECT event_id FROM organization_task_events "
            "WHERE org_bi=? AND task_id=? "
            "ORDER BY created_at DESC,event_id DESC LIMIT -1 OFFSET ?)",
            org_bi,
            task_id,
            MAX_TASK_EVENTS - 1,
        )
        await runtime.d1_run(
            "INSERT INTO organization_task_events "
            "(event_id,task_id,org_bi,actor_bi,kind,data,created_at) "
            "VALUES (?,?,?,?,?,?,?)",
            event_id,
            task_id,
            org_bi,
            account_bi,
            kind,
            sealed,
            now,
        )
    except Exception:
        # A rolling deployment can serve this code before the additive
        # migration lands. Losing a timeline row must never lose the write.
        return ""
    return event_id


async def _task_history(runtime, org_bi, task_id):
    """The bounded oldest-first timeline for one already-authorized task."""

    try:
        rows = await runtime.d1_all(
            "SELECT event_id,kind,data,created_at "
            "FROM organization_task_events WHERE org_bi=? AND task_id=? "
            "ORDER BY created_at DESC,event_id DESC LIMIT ?",
            org_bi,
            task_id,
            MAX_TASK_EVENTS,
        )
    except Exception:
        return []
    history = []
    for row in reversed(rows or []):
        kind = str(row.get("kind") or "")
        if kind not in TASK_EVENT_KINDS:
            continue
        try:
            data = await runtime.open(row.get("data"))
        except Exception:
            continue
        if not isinstance(data, dict):
            continue
        history.append({
            "id": str(row.get("event_id") or "").lower(),
            "kind": kind,
            "actor": _text(data.get("actor"), 64).lower(),
            "summary": _text(data.get("summary"), MAX_EVENT_SUMMARY),
            "mentions": [
                _text(name, 64).lower()
                for name in (data.get("mentions") or [])
                if _text(name, 64)
            ][:MAX_MENTIONS],
            "createdAt": max(0, _integer(row.get("created_at"))),
        })
    return history


async def _notify_mentions(
        runtime, org_bi, actor, task_id, task, names, context, excerpt, now):
    """Ping the organization members a comment or check-in named by @handle.

    Separate from the activity fan-out on purpose: activity reaches the floor
    team, a mention reaches exactly the person addressed, even when they are
    on no team the task routes through.
    """

    notifier = getattr(runtime, "notify_organization_task_mentions", None)
    recipients = [
        name for name in (names or [])
        if name and name != _text(actor, 64).lower()
    ][:MAX_MENTIONS]
    if not callable(notifier) or not recipients or not task:
        return
    try:
        await notifier(
            org_bi,
            actor,
            task_id,
            task,
            recipients,
            _text(context, 32),
            _text(excerpt, MAX_EVENT_SUMMARY),
            max(0, _integer(now)),
        )
    except Exception:
        pass


async def _notify_reminder(
        runtime, org_bi, actor, task_id, task, recipient, note, now):
    notifier = getattr(runtime, "notify_organization_task_reminder", None)
    if not callable(notifier) or not task or not recipient:
        return {"pinged": False, "error": "notifier_unavailable"}
    try:
        result = await notifier(
            org_bi,
            actor,
            task_id,
            task,
            recipient,
            _text(note, MAX_REMINDER_NOTE),
            max(0, _integer(now)),
        )
    except Exception as error:
        await _log_reminder_failure(runtime, task_id, repr(error))
        return {"pinged": False, "error": "notify_failed"}
    if not result:
        await _log_reminder_failure(runtime, task_id, "ping_not_filed")
        return {"pinged": False, "error": "ping_not_filed"}
    receipt = result if isinstance(result, dict) else {}
    return {
        "pinged": True,
        "pingId": _text(receipt.get("pingId"), 160).lower(),
        "phones": max(0, _integer(receipt.get("phones"))),
    }


async def _log_reminder_failure(runtime, task_id, reason):
    logger = getattr(runtime, "log_error", None)
    if not callable(logger):
        return
    try:
        await logger(
            "organization/tasks/remind",
            "Task reminder ping was not delivered for %s: %s" % (
                _text(task_id, 64), _text(reason, 300)))
    except Exception:
        pass


async def _observe_reminder(runtime, org_bi, account_bi, actor, row, now,
                            check_ping=False):
    if not row or not str(row.get("data") or ""):
        return row
    try:
        current = await runtime.open(row.get("data"))
    except Exception:
        return row
    if not isinstance(current, dict):
        return row
    reminder = current.get("reminder")
    if (
        not isinstance(reminder, dict)
        or not _integer(reminder.get("at"))
        or _integer(reminder.get("seenAt"))
    ):
        return row
    via = ""
    seen_at = 0
    assignee = _text(current.get("assignee"), 64).lower()
    if (
        account_bi
        and str(row.get("assignee_bi") or "") == account_bi
        and str(row.get("assignee_kind") or "user") == "user"
    ):
        via, seen_at = "task", int(now)
    elif check_ping:
        delivery = reminder.get("delivery")
        ping_id = (
            _text(delivery.get("pingId"), 160).lower()
            if isinstance(delivery, dict) else "")
        reader = getattr(runtime, "reminder_ping_read_at", None)
        if ping_id and assignee and callable(reader):
            try:
                read_at = max(0, _integer(await reader(assignee, ping_id)))
            except Exception:
                read_at = 0
            if read_at:
                via, seen_at = "ping", read_at
    if not via:
        return row
    reminder = dict(reminder)
    reminder["seenAt"] = max(seen_at, _integer(reminder.get("at")))
    reminder["seenVia"] = via
    current["reminder"] = reminder
    sealed = await runtime.seal(current)
    await runtime.d1_run(
        "UPDATE organization_tasks SET data=? "
        "WHERE org_bi=? AND task_id=? AND data=?",
        sealed,
        org_bi,
        str(row.get("task_id") or ""),
        row.get("data"),
    )
    changed = await _task(runtime, org_bi, str(row.get("task_id") or ""))
    if not changed or changed.get("data") != sealed:
        return changed or row
    seer = assignee or _text(actor, 64).lower()
    await _record_event(
        runtime, org_bi, str(row.get("task_id") or ""),
        str(row.get("assignee_bi") or ""), seer, "reminder_seen",
        int(now), {
            "summary": (
                "@%s opened the reminder ping" % seer if via == "ping"
                else "@%s opened the task after the reminder" % seer),
        })
    return changed


async def _observe_comment_read(runtime, org_bi, account_bi, actor, row, now):
    reader = _text(actor, 64).lower()
    if not row or not reader or not account_bi or not str(row.get("data") or ""):
        return row
    task_id = str(row.get("task_id") or "")
    try:
        latest = await runtime.d1_first(
            "SELECT MAX(created_at) AS latest "
            "FROM organization_task_responses "
            "WHERE org_bi=? AND task_id=? AND author_bi<>?",
            org_bi,
            task_id,
            account_bi,
        )
        latest = max(0, _integer((latest or {}).get("latest")))
        if not latest:
            return row
        current = await runtime.open(row.get("data"))
    except Exception:
        return row
    if not isinstance(current, dict):
        return row
    reads = _comment_reads(current)
    if reads.get(reader, 0) >= latest:
        return row
    reads[reader] = max(int(now), latest)
    current["commentReads"] = dict(sorted(
        reads.items(), key=lambda item: item[1])[-MAX_COMMENT_READERS:])
    sealed = await runtime.seal(current)
    try:
        await runtime.d1_run(
            "UPDATE organization_tasks SET data=? "
            "WHERE org_bi=? AND task_id=? AND data=?",
            sealed,
            org_bi,
            task_id,
            row.get("data"),
        )
    except Exception:
        return row
    return await _task(runtime, org_bi, task_id) or row


async def _observe_email_view(runtime, org_bi, task_id, actor, now):
    query = getattr(runtime, "query", None)
    observer = getattr(runtime, "task_email_viewed", None)
    if not callable(query) or not callable(observer):
        return
    try:
        token = _text(query("mail"), 80)
        if token:
            await observer(org_bi, task_id, token, actor, now)
    except Exception:
        pass


async def _task(runtime, org_bi, task_id, trashed=False):
    return await runtime.d1_first(
        "SELECT * FROM organization_tasks "
        "WHERE org_bi=? AND task_id=? AND "
        + ("deleted_at>0" if trashed else "deleted_at=0"),
        org_bi,
        task_id,
    )


async def _task_response(runtime, row, now, status=200, extra=None):
    if not row:
        return _response(runtime, {"error": "task_not_found"}, status=404)
    org_bi = str(row.get("org_bi") or "")
    task_id = str(row.get("task_id") or "")
    checkins = await _latest_checkins(
        runtime, [task_id])
    replies = await _latest_task_replies(runtime, org_bi, task_id)
    reply_count, last_reply_at = await _task_reply_stats(
        runtime, org_bi, task_id)
    task = await _project_task(
        runtime,
        row,
        now,
        checkins.get(task_id),
        replies=replies,
    )
    if task is None:
        return _response(runtime, {"error": "task_unavailable"}, status=500)
    task["replyCount"] = reply_count
    task["lastReplyAt"] = last_reply_at
    task["history"] = await _task_history(runtime, org_bi, task_id)
    payload = {"ok": True, "task": task}
    if isinstance(extra, dict):
        payload.update(extra)
    return _response(runtime, payload, status=status)


async def _list(
        runtime, org_bi, account_bi, actor, can_manage, now,
        marketing_only=False, department="", team="", trash=False):
    where = ["org_bi=?", "deleted_at>0" if trash else "deleted_at=0"]
    arguments = [org_bi]
    if marketing_only:
        where.append("department='marketing'")
    elif department:
        where.append("department=?")
        arguments.append(department)
    if team:
        where.append("team=?")
        arguments.append(team)
    raw_offset = str(runtime.query("offset") or "").strip()
    if raw_offset and not raw_offset.isdigit():
        return _response(runtime, {"error": "invalid_task_offset"}, status=400)
    offset = int(raw_offset or 0)
    if offset < 0 or offset >= TASK_LIST_RESPONSE_LIMIT:
        return _response(runtime, {"error": "invalid_task_offset"}, status=400)
    if marketing_only and not can_manage:
        # Apply the physical Marketing wall's assignee boundary before LIMIT
        # and OFFSET; filtering a page afterward can skip authorized rows.
        where.append("assignee_bi=?")
        arguments.append(account_bi)
    remaining = TASK_LIST_RESPONSE_LIMIT - offset
    page_size = min(TASK_LIST_PAGE_SIZE, remaining)
    rows = await runtime.d1_all(
        "SELECT * FROM organization_tasks WHERE "
        + " AND ".join(where)
        + (
            " ORDER BY deleted_at DESC,task_id DESC "
            if trash else
            " ORDER BY completed_at>0,priority,updated_at DESC,task_id DESC "
        )
        + "LIMIT ? OFFSET ?",
        *arguments,
        page_size + 1,
        offset,
    )
    rows = rows or []
    has_more = len(rows) > page_size and offset + page_size < \
        TASK_LIST_RESPONSE_LIMIT
    rows = rows[:page_size]
    latest = await _latest_checkins(
        runtime, [str(row.get("task_id") or "") for row in rows or []])
    reply_stats = await _task_reply_stats_for_tasks(
        runtime, org_bi, [str(row.get("task_id") or "") for row in rows or []])
    tasks = []
    for row in rows or []:
        task = await _project_task(
            runtime,
            row,
            now,
            latest.get(str(row.get("task_id") or "")),
        )
        if task is not None:
            count, last_reply_at = reply_stats.get(
                str(row.get("task_id") or ""), (0, 0))
            task["replyCount"] = count
            task["lastReplyAt"] = last_reply_at
            if trash:
                task["canRestore"] = _can_trash(row, {
                    "accountBi": account_bi, "canManage": can_manage})
            tasks.append(task)
    # Roster and Marketing side data are invariant across catalog pages. Send
    # them only with page zero so continuation requests spend their budget on
    # task projection rather than repeatedly decrypting the same accounts.
    first_page = offset == 0
    marketing_members = (
        await runtime.marketing_members(org_bi)
        if marketing_only and first_page else []
    )
    marketing_actor = (
        await runtime.marketing_member(org_bi, actor)
        if marketing_only and first_page else None
    )
    marketing_names = [
        _text(member.get("name"), 64).lower()
        for member in marketing_members
        if isinstance(member, dict) and _text(member.get("name"), 64)
    ]
    result = {
        "ok": True,
        "authorized": True,
        "privacyBoundary": "organization-private-encrypted-at-rest",
        "actor": actor,
        "canManage": bool(can_manage),
        "serverNow": int(now),
        "tasks": tasks,
        "trash": bool(trash),
        "trashRetentionMs": TRASH_RETENTION_MS,
        "offset": offset,
        "hasMore": has_more,
        "nextOffset": offset + len(rows) if has_more else None,
        "taskLimit": TASK_LIST_RESPONSE_LIMIT,
        "marketingMembers": marketing_names if marketing_actor else [],
        "attendanceDays": (
            await runtime.marketing_attendance(marketing_members, now)
            if marketing_actor else []
        ),
        "proofs": (
            await _proof_records(runtime, org_bi)
            if marketing_actor else []
        ),
        "initiatives": (
            await _initiative_records(runtime, org_bi)
            if marketing_actor else []
        ),
    }
    if marketing_only and can_manage and first_page:
        # Assignment is deliberately narrower than organization management:
        # even an owner may only place Marketing work onto a Marketing member.
        result["members"] = marketing_names
    elif not marketing_only and first_page:
        if not trash:
            result["agentRuns"] = await _presence_records(runtime, org_bi, now)
        result["members"] = [
            member["name"]
            for member in await runtime.organization_members(org_bi)
        ]
        result["teams"] = await runtime.organization_teams(org_bi)
        result["departments"] = list(DEPARTMENTS)
    return _response(runtime, result)


async def _initiative_records(runtime, org_bi):
    rows = await runtime.d1_all(
        "SELECT initiative_id,data,created_at "
        "FROM world_office_marketing_initiatives "
        "WHERE org_bi=? "
        "ORDER BY created_at DESC,initiative_id DESC LIMIT ?",
        org_bi,
        MAX_INITIATIVES,
    )
    records = []
    for row in rows or []:
        try:
            data = await runtime.open(row.get("data"))
        except Exception:
            continue
        if not isinstance(data, dict) or not valid_id(row.get("initiative_id")):
            continue
        owner = _text(data.get("owner"), 100)
        repo = _text(data.get("repo"), 100)
        number = int(data.get("number") or 0)
        title = _text(data.get("title"), MAX_TITLE, "Repository issue")
        if (
            not _REPO_SEGMENT_RE.fullmatch(owner)
            or not _REPO_SEGMENT_RE.fullmatch(repo)
            or number <= 0
            or number > 2_147_483_647
        ):
            continue
        records.append({
            "id": str(row.get("initiative_id")),
            "title": title,
            "repo": owner + "/" + repo,
            "number": number,
            "href": f"/{owner}/{repo}/issues/{number}",
            "createdAt": int(row.get("created_at") or 0),
        })
    return records


async def _initiative_create(
        runtime, org_bi, account_bi, actor, data, can_manage, now):
    marketing_actor = await runtime.marketing_member(org_bi, actor)
    if not can_manage and not marketing_actor:
        return _response(runtime, {"error": "marketing_team_only"}, status=403)
    owner = _text(data.get("owner"), 100)
    repo = _text(data.get("repo"), 100)
    title = _text(data.get("title"), MAX_TITLE, "Repository issue")
    try:
        number = int(data.get("number") or 0)
    except (TypeError, ValueError):
        number = 0
    if (
        not _REPO_SEGMENT_RE.fullmatch(owner)
        or not _REPO_SEGMENT_RE.fullmatch(repo)
        or number <= 0
        or number > 2_147_483_647
    ):
        return _response(runtime, {"error": "invalid_issue_source"}, status=400)
    source_bi = await runtime.blind(
        "office-marketing-initiative:"
        + owner.lower() + "/" + repo.lower() + "#" + str(number)
    )
    existing = await runtime.d1_first(
        "SELECT initiative_id FROM world_office_marketing_initiatives "
        "WHERE org_bi=? AND source_bi=?",
        org_bi,
        source_bi,
    )
    if existing:
        records = await _initiative_records(runtime, org_bi)
        initiative_id = str(existing.get("initiative_id") or "")
        initiative = next(
            (record for record in records if record["id"] == initiative_id),
            None,
        )
        return _response(runtime, {
            "ok": True,
            "existing": True,
            "initiative": initiative,
        })
    count = await runtime.d1_first(
        "SELECT COUNT(*) AS count "
        "FROM world_office_marketing_initiatives WHERE org_bi=?",
        org_bi,
    )
    if int((count or {}).get("count") or 0) >= MAX_INITIATIVES:
        return _response(
            runtime, {"error": "initiative_capacity_reached"}, status=409)
    initiative_id = runtime.new_id()
    if not valid_id(initiative_id):
        return _response(runtime, {"error": "id_generation_failed"}, status=500)
    sealed = await runtime.seal({
        "owner": owner,
        "repo": repo,
        "number": number,
        "title": title,
    })
    try:
        await runtime.d1_run(
            "INSERT INTO world_office_marketing_initiatives "
            "(initiative_id,org_bi,source_bi,data,created_by_bi,created_at) "
            "VALUES (?,?,?,?,?,?)",
            initiative_id,
            org_bi,
            source_bi,
            sealed,
            account_bi,
            now,
        )
    except Exception as error:
        message = str(error)
        if "catalog_full" in message:
            return _response(
                runtime, {"error": "initiative_capacity_reached"}, status=409)
        if "UNIQUE" in message.upper():
            return _response(runtime, {"ok": True, "existing": True})
        raise
    await runtime.audit(
        actor,
        "office.marketing_initiative_created",
        "office_marketing_initiative",
        initiative_id,
        details={"source": "repository_issue"},
    )
    records = await _initiative_records(runtime, org_bi)
    initiative = next(
        (record for record in records if record["id"] == initiative_id),
        None,
    )
    return _response(
        runtime, {"ok": True, "initiative": initiative}, status=201)


def _proof_url(value):
    raw = str(value or "").strip()
    if not raw or len(raw) > 500:
        return ""
    try:
        parsed = urlsplit(raw)
        port = parsed.port
    except (TypeError, ValueError):
        return ""
    if (
        parsed.scheme != "https"
        or not parsed.hostname
        or parsed.username
        or parsed.password
        or parsed.fragment
        or port not in (None, 443)
    ):
        return ""
    host = parsed.hostname.lower().rstrip(".")
    if (
        host in ("localhost", "localhost.localdomain")
        or host.endswith(".local")
        or host.endswith(".internal")
        or re.fullmatch(r"\d+(?:\.\d+){3}", host)
        or ":" in host
    ):
        return ""
    return raw


async def _proof_records(runtime, org_bi):
    rows = await runtime.d1_all(
        "SELECT proof_id,account_bi,data,created_at "
        "FROM world_office_marketing_proofs "
        "WHERE org_bi=? "
        "ORDER BY created_at DESC,proof_id DESC LIMIT ?",
        org_bi,
        MAX_PROOFS,
    )
    records = []
    for row in rows or []:
        try:
            data = await runtime.open(row.get("data"))
        except Exception:
            continue
        if not isinstance(data, dict):
            continue
        url = _proof_url(data.get("url"))
        member = _text(data.get("member"), 64).lower()
        if not url or not member or not valid_id(row.get("proof_id")):
            continue
        records.append({
            "id": str(row.get("proof_id")),
            "member": member,
            "url": url,
            "host": _text(urlsplit(url).hostname, 120),
            "label": _text(data.get("label"), 120, urlsplit(url).hostname),
            "createdAt": int(row.get("created_at") or 0),
        })
    return records


async def _proof_create(
        runtime, org_bi, account_bi, actor, data, now):
    if not await runtime.marketing_member(org_bi, actor):
        return _response(runtime, {"error": "marketing_team_only"}, status=403)
    url = _proof_url(data.get("url"))
    if not url:
        return _response(runtime, {"error": "invalid_social_url"}, status=400)
    label = _text(
        data.get("label"), 120, urlsplit(url).hostname or "Social post")
    count = await runtime.d1_first(
        "SELECT COUNT(*) AS count FROM world_office_marketing_proofs "
        "WHERE org_bi=? AND account_bi=?",
        org_bi,
        account_bi,
    )
    if int((count or {}).get("count") or 0) >= MAX_PROOFS_PER_MEMBER:
        return _response(runtime, {"error": "proof_capacity_reached"}, status=409)
    proof_id = runtime.new_id()
    if not valid_id(proof_id):
        return _response(runtime, {"error": "id_generation_failed"}, status=500)
    sealed = await runtime.seal({
        "member": actor,
        "url": url,
        "label": label,
    })
    try:
        await runtime.d1_run(
            "INSERT INTO world_office_marketing_proofs "
            "(proof_id,org_bi,account_bi,url_bi,data,created_at) "
            "VALUES (?,?,?,?,?,?)",
            proof_id,
            org_bi,
            account_bi,
            await runtime.blind("office-marketing-proof:" + url.lower()),
            sealed,
            now,
        )
    except Exception as error:
        if "catalog_full" in str(error):
            return _response(
                runtime, {"error": "proof_capacity_reached"}, status=409)
        raise
    await runtime.audit(
        actor,
        "office.marketing_proof_created",
        "office_marketing_proof",
        proof_id,
        details={"source": "marketing_desk"},
    )
    records = await _proof_records(runtime, org_bi)
    proof = next(
        (record for record in records if record["id"] == proof_id),
        None,
    )
    return _response(
        runtime, {"ok": True, "proof": proof}, status=201)


async def _create(
        runtime, org_bi, account_bi, actor, data, can_manage, now,
        marketing_only=False):
    if marketing_only and not can_manage:
        return _response(runtime, {"error": "forbidden"}, status=403)
    title = _text(data.get("title"), MAX_TITLE)
    details = _multiline(data.get("details"), MAX_DETAILS)
    reminder_every_ms, reminder_error = _persistent_reminder_every_ms(
        data.get("persistentReminder"))
    if reminder_error:
        return _response(runtime, {"error": reminder_error}, status=400)
    reminder_every_ms = reminder_every_ms or 0
    department = (
        "marketing"
        if marketing_only
        else _text(data.get("department"), 64, "general").lower()
    )
    team = (
        "marketing"
        if marketing_only
        else _text(data.get("team"), 64).lower()
    )
    destination = (
        "department"
        if marketing_only
        else str(data.get("destination") or "department").strip().lower()
    )
    requested_assignee_kind = (
        "user"
        if marketing_only
        else str(data.get("assigneeKind") or "user").strip().lower()
    )
    assignee_kind = requested_assignee_kind
    parent_task_id = str(data.get("parentTaskId") or "").strip().lower()
    parent_data = None
    parent_row = None
    if parent_task_id:
        if marketing_only or not valid_id(parent_task_id):
            return _response(
                runtime, {"error": "invalid_parent_task"}, status=400)
        parent_row = await _task(runtime, org_bi, parent_task_id)
        if not parent_row:
            return _response(
                runtime, {"error": "parent_task_not_found"}, status=404)
        try:
            parent_data = await runtime.open(parent_row.get("data"))
        except Exception:
            parent_data = None
        if not isinstance(parent_data, dict):
            return _response(
                runtime, {"error": "parent_task_unavailable"}, status=500)
        if (
            not can_manage
            and str(parent_row.get("created_by_bi") or "") != account_bi
            and str(parent_row.get("assignee_bi") or "") != account_bi
        ):
            return _response(runtime, {"error": "forbidden"}, status=403)
        # A follow-up is a continuation of the routed work. The server, rather
        # than the client, inherits its assignee and routing from the parent.
        assignee_kind = str(
            parent_row.get("assignee_kind") or "unassigned").lower()
        requested_assignee_kind = assignee_kind
        department = str(parent_row.get("department") or "general")
        team = str(parent_row.get("team") or "")
        destination = str(parent_row.get("destination") or "department")
        data = dict(data)
        data["assignee"] = parent_data.get("assignee")
        data["repository"] = parent_data.get("repository")
    task_kind = (
        "task"
        if marketing_only
        else str(data.get("kind") or "task").strip().lower()
    )
    if (
        not title
        or task_kind not in TASK_KINDS
        or not _SCOPE_RE.fullmatch(department)
        or (team and not _SCOPE_RE.fullmatch(team))
        or destination not in DESTINATIONS
        or assignee_kind not in ASSIGNEE_KINDS
    ):
        return _response(runtime, {"error": "invalid_task_routing"}, status=400)
    if department not in DEPARTMENTS:
        return _response(runtime, {"error": "invalid_department"}, status=400)

    bounty_request = None
    if task_kind == "bid":
        amount_sol, lamports = _sol_amount(data.get("bountyAmountSol"))
        if not amount_sol or not lamports:
            return _response(
                runtime, {"error": "invalid_bounty_request"}, status=400)
        # A lobby bid is always made by and assigned to the authenticated
        # organization member. Client-supplied assignee fields cannot make a
        # compensation request appear to come from somebody else.
        assignee_kind = "user"
        data = dict(data)
        data["assignee"] = actor
        bounty_request = {
            "currency": "SOL",
            "amountSol": amount_sol,
            "lamports": lamports,
            "status": "requested",
        }

    assignee = _text(data.get("assignee"), 64).lower()
    member = None
    assignee_bi = ""
    if assignee_kind == "user":
        assignee = assignee or actor
        member = (
            await runtime.marketing_member(org_bi, assignee)
            if marketing_only
            else await runtime.organization_member(org_bi, assignee)
        )
        if not member:
            return _response(
                runtime,
                {
                    "error": (
                        "assignee_not_marketing_member"
                        if marketing_only else "assignee_not_org_member"
                    )
                },
                status=400,
            )
        if not can_manage and member["name"] != actor:
            return _response(
                runtime, {"error": "cannot_assign_other_member"}, status=403)
        if team and not marketing_only and not await runtime.team_member(
                org_bi, team, member["name"]):
            return _response(
                runtime, {"error": "assignee_not_team_member"}, status=400)
        assignee = member["name"]
        assignee_bi = member["bi"]
    elif assignee_kind in AGENT_ASSIGNEE_KINDS:
        # The board carries one general bot; the node picks the runtime.
        assignee_kind = "agent"
        assignee = "agent"
        destination = "agent"
    else:
        assignee = ""

    repository = _text(data.get("repository"), 201)
    if repository:
        parts = repository.split("/", 1)
        if (
            len(parts) != 2
            or not _REPO_SEGMENT_RE.fullmatch(parts[0])
            or not _REPO_SEGMENT_RE.fullmatch(parts[1])
        ):
            return _response(
                runtime, {"error": "invalid_repository"}, status=400)
    if destination in ("repository", "agent") and not repository:
        return _response(
            runtime, {"error": "repository_required"}, status=400)
    how_to_test = _multiline(data.get("howToTest"), 720)
    work_items, work_items_error = _work_items(
        data.get("workItems"), strict=True)
    if work_items_error:
        return _response(runtime, {"error": work_items_error}, status=400)
    image_uploads, image_error = _task_image_uploads(data.get("attachments"))
    if image_error:
        return _response(runtime, {"error": image_error}, status=400)
    requested_priority = _priority(data.get("priority"))
    if "priority" in data and not can_manage:
        return _response(runtime, {"error": "manager_required"}, status=403)
    # A desktop that launches a prompt opens the task in the same call, so the
    # run's provenance arrives with it rather than through a second round trip.
    agent_run = (
        _agent_run(data.get("agent"))
        if requested_assignee_kind in AGENT_ASSIGNEE_KINDS
        else None
    )
    agent_session_id = _agent_ref((agent_run or {}).get("sessionId"))
    qa_requested_at = (
        now
        if destination == "qa" or data.get("sendToQa") is True
        else 0
    )
    task_id = runtime.new_id()
    if not valid_id(task_id):
        return _response(runtime, {"error": "id_generation_failed"}, status=500)
    attachments = []
    for upload in image_uploads:
        if upload["file"]:
            attachment_id = runtime.new_id()
            if not valid_id(attachment_id):
                return _response(
                    runtime, {"error": "id_generation_failed"}, status=500)
            upload["id"] = attachment_id
            attachments.append({
                "id": attachment_id,
                "taskId": task_id,
                "name": upload["name"],
                "mime": upload["mime"],
                "size": upload["size"],
            })
        else:
            metadata = {
                "name": upload["name"],
                "mime": upload["mime"],
                "size": upload["size"],
            }
            if upload.get("thumbnail"):
                metadata["thumbnail"] = upload["thumbnail"]
            attachments.append(metadata)
    sealed = await runtime.seal({
        "kind": task_kind,
        "title": title,
        "details": details,
        "attachments": attachments,
        "completionNote": "",
        "assignee": assignee,
        "createdBy": actor,
        "parentTaskId": parent_task_id,
        "bountyRequest": bounty_request,
        "repository": repository,
        "howToTest": how_to_test,
        "workItems": work_items,
        "qaReviewer": "",
        # Routing and completion history travel with the encrypted task copy.
        # Mark-undone can therefore restore the pre-QA destination without
        # trusting mutable client state or exposing human labels in D1.
        "routing": _routing_snapshot_values(
            department, team, destination, assignee_kind, assignee, repository,
        ),
        "preQaRouting": None,
        "completionHistory": [],
        "agent": agent_run,
    })
    try:
        await runtime.d1_run(
            "INSERT INTO organization_tasks "
            "(task_id,org_bi,department,team,destination,assignee_kind,"
            "status,assignee_bi,data,created_by_bi,created_at,updated_at,"
            "elapsed_ms,started_at,next_checkin_at,qa_requested_at,"
            "agent_session_id,priority,reminder_every_ms,reminder_next_at) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            task_id,
            org_bi,
            department,
            team,
            destination,
            assignee_kind,
            "idle",
            assignee_bi,
            sealed,
            account_bi,
            now,
            now,
            0,
            0,
            0,
            qa_requested_at,
            agent_session_id,
            requested_priority,
            reminder_every_ms,
            now + reminder_every_ms if reminder_every_ms else 0,
        )
        for upload in image_uploads:
            if not upload["file"]:
                continue
            await runtime.d1_run(
                "INSERT INTO organization_task_attachments "
                "(attachment_id,task_id,org_bi,data,created_by_bi,created_at) "
                "VALUES (?,?,?,?,?,?)",
                upload["id"],
                task_id,
                org_bi,
                await runtime.seal({
                    "name": upload["name"],
                    "mime": upload["mime"],
                    "size": upload["size"],
                    "file": upload["file"],
                }),
                account_bi,
                now,
            )
    except Exception as error:
        try:
            await runtime.d1_run(
                "DELETE FROM organization_task_attachments "
                "WHERE org_bi=? AND task_id=?",
                org_bi,
                task_id,
            )
            await runtime.d1_run(
                "DELETE FROM organization_tasks "
                "WHERE org_bi=? AND task_id=?",
                org_bi,
                task_id,
            )
        except Exception:
            pass
        if "catalog_full" in str(error):
            return _response(
                runtime, {"error": "task_capacity_reached"}, status=409)
        raise
    await runtime.audit(
        actor,
        (
            "office.marketing_task_created"
            if marketing_only else "organization.task_created"
        ),
        "organization_task",
        task_id,
        details=(
            {"assigned": True}
            if marketing_only
            else {
                "kind": task_kind,
                "department": department,
                "destination": destination,
                "assigneeKind": assignee_kind,
                "bountyAmountSol": (
                    bounty_request["amountSol"] if bounty_request else ""
                ),
                "agentRun": bool(agent_run),
                "priority": requested_priority,
                "followUp": bool(parent_task_id),
                "workItems": len(work_items),
            }
        ),
    )
    row = await _task(runtime, org_bi, task_id)
    mentioned = _mentions(title, details)
    await _record_event(
        runtime, org_bi, task_id, account_bi, actor, "created", now,
        detail={"summary": title, "mentions": mentioned},
    )
    await _notify_activity(
        runtime, org_bi, actor, task_id, row, now, "created",
        skip=mentioned,
    )
    created_task = await _project_task(runtime, row, now) if row else None
    await _notify_mentions(
        runtime, org_bi, actor, task_id, created_task, mentioned,
        "task", details or title, now,
    )
    return await _task_response(runtime, row, now, status=201)


async def _update(
        runtime, org_bi, account_bi, actor, task_id, data, can_manage, now,
        marketing_only=False):
    row = await _task(runtime, org_bi, task_id)
    if not row:
        return _response(runtime, {"error": "task_not_found"}, status=404)
    try:
        current = await runtime.open(row.get("data"))
    except Exception:
        return _response(runtime, {"error": "task_unavailable"}, status=500)
    if not isinstance(current, dict):
        return _response(runtime, {"error": "task_unavailable"}, status=500)
    if set(data).issubset({"agentSessionId", "sessionToken"}):
        session_id = str(data.get("agentSessionId") or "").strip().lower()
        if (
            str(row.get("assignee_kind") or "") not in AGENT_ASSIGNEE_KINDS
            or not re.fullmatch(r"[a-f0-9-]{16,64}", session_id)
            or (
                not can_manage
                and str(row.get("created_by_bi") or "") != account_bi
            )
        ):
            return _response(
                runtime, {"error": "invalid_agent_session_link"}, status=400)
        await runtime.d1_run(
            "UPDATE organization_tasks SET agent_session_id=?,updated_at=? "
            "WHERE org_bi=? AND task_id=?",
            session_id, now, org_bi, task_id,
        )
        await runtime.audit(
            actor, "organization.task_agent_linked", "organization_task",
            task_id, details={"linked": True},
        )
        return await _task_response(
            runtime, await _task(runtime, org_bi, task_id), now)
    if (
        "persistentReminder" in data
        and set(data).issubset({"persistentReminder", "sessionToken"})
    ):
        return await _set_persistent_reminder(
            runtime, org_bi, account_bi, actor, task_id, row,
            data.get("persistentReminder"), can_manage, now)
    if "progress" in data and set(data).issubset({"progress", "sessionToken"}):
        if not _can_edit_progress(row, account_bi, can_manage):
            return _response(runtime, {"error": "forbidden"}, status=403)
        raw = data.get("progress")
        if isinstance(raw, bool):
            return _response(runtime, {"error": "invalid_progress"}, status=400)
        try:
            requested = float(raw)
        except (TypeError, ValueError, OverflowError):
            return _response(runtime, {"error": "invalid_progress"}, status=400)
        if requested != requested:
            return _response(runtime, {"error": "invalid_progress"}, status=400)
        progress = _progress(round(requested))
        previous = _progress(row.get("progress"))
        await runtime.d1_run(
            "UPDATE organization_tasks SET progress=?,updated_at=? "
            "WHERE org_bi=? AND task_id=?",
            progress, now, org_bi, task_id,
        )
        await runtime.audit(
            actor, "organization.task_progress_changed",
            "organization_task", task_id,
            details={"progress": progress, "previous": previous},
        )
        await _record_event(
            runtime, org_bi, task_id, account_bi, actor, "updated", now,
            detail={"fields": ["progress %d%%" % progress]},
        )
        changed_row = await _task(runtime, org_bi, task_id)
        if progress != previous:
            await _notify_activity(
                runtime, org_bi, actor, task_id, changed_row, now, "updated",
                changes=[{
                    "field": "progress", "from": previous, "to": progress,
                }],
            )
        return await _task_response(runtime, changed_row, now)
    delta_fields = {"attachWorkItems", "detachWorkItems"}
    work_items_patch = bool(
        delta_fields & set(data)
        and set(data).issubset(delta_fields | {"sessionToken"})
    )
    if work_items_patch:
        if not _can_edit_progress(row, account_bi, can_manage):
            return _response(runtime, {"error": "forbidden"}, status=403)
        attach, attach_error = _work_items(
            data.get("attachWorkItems"), strict=True)
        detach, detach_error = _work_items(
            data.get("detachWorkItems"), strict=True)
        if attach_error or detach_error:
            return _response(
                runtime, {"error": attach_error or detach_error}, status=400)
        for _attempt in range(WORK_ITEM_UPDATE_ATTEMPTS):
            previous_items = _work_items(current.get("workItems"))[0]
            work_items, work_items_error = _merge_work_items(
                previous_items, attach, detach)
            if work_items_error:
                return _response(
                    runtime, {"error": work_items_error}, status=400)
            if work_items == previous_items:
                return await _task_response(runtime, row, now)
            current["workItems"] = work_items
            result = await runtime.d1_run(
                "UPDATE organization_tasks SET data=?,updated_at=? "
                "WHERE org_bi=? AND task_id=? AND data=?",
                await runtime.seal(current), now, org_bi, task_id,
                row.get("data"),
            )
            if _changed_rows(result) != 0:
                break
            row = await _task(runtime, org_bi, task_id)
            if not row:
                return _response(
                    runtime, {"error": "task_not_found"}, status=404)
            try:
                current = await runtime.open(row.get("data"))
            except Exception:
                return _response(
                    runtime, {"error": "task_unavailable"}, status=500)
            if not isinstance(current, dict):
                return _response(
                    runtime, {"error": "task_unavailable"}, status=500)
        else:
            return _response(runtime, {"error": "task_busy"}, status=409)
    elif "workItems" in data and set(data).issubset(
            {"workItems", "sessionToken"}):
        work_items_patch = True
        if not _can_edit_progress(row, account_bi, can_manage):
            return _response(runtime, {"error": "forbidden"}, status=403)
        work_items, work_items_error = _work_items(
            data.get("workItems"), strict=True)
        if work_items_error:
            return _response(runtime, {"error": work_items_error}, status=400)
        previous_items = _work_items(current.get("workItems"))[0]
        if work_items == previous_items:
            return await _task_response(runtime, row, now)
        current["workItems"] = work_items
        await runtime.d1_run(
            "UPDATE organization_tasks SET data=?,updated_at=? "
            "WHERE org_bi=? AND task_id=?",
            await runtime.seal(current), now, org_bi, task_id,
        )
    if work_items_patch:
        await runtime.audit(
            actor, "organization.task_work_items_changed",
            "organization_task", task_id,
            details={
                "workItems": len(work_items),
                "previous": len(previous_items),
            },
        )
        await _record_event(
            runtime, org_bi, task_id, account_bi, actor, "updated", now,
            detail={
                "fields": [_work_items_summary(previous_items, work_items)],
            },
        )
        changed_row = await _task(runtime, org_bi, task_id)
        await _notify_activity(
            runtime, org_bi, actor, task_id, changed_row, now, "updated",
            changes=[{
                "field": "workItems",
                "summary": _work_items_summary(previous_items, work_items),
            }],
        )
        return await _task_response(runtime, changed_row, now)
    if not can_manage:
        return _response(runtime, {"error": "forbidden"}, status=403)
    if "priority" in data and set(data).issubset({"priority", "sessionToken"}):
        priority = _priority(data.get("priority"))
        previous_priority = _priority(row.get("priority"))
        await runtime.d1_run(
            "UPDATE organization_tasks SET priority=?,updated_at=? "
            "WHERE org_bi=? AND task_id=?",
            priority, now, org_bi, task_id,
        )
        await runtime.audit(
            actor, "organization.task_priority_changed",
            "organization_task", task_id,
            details={"priority": priority},
        )
        changed_row = await _task(runtime, org_bi, task_id)
        if priority != previous_priority:
            await _notify_activity(
                runtime, org_bi, actor, task_id, changed_row, now, "updated",
                changes=[{
                    "field": "priority", "from": previous_priority,
                    "to": priority,
                }],
            )
        return await _task_response(runtime, changed_row, now)
    allowed = {
        "title", "details", "assignee", "assigneeKind", "priority",
        "repository", "parentTaskId", "howToTest", "department", "team",
        "workItems", "sessionToken",
    }
    changed = [
        field for field in (
            "title", "details", "assignee", "assigneeKind", "priority",
            "repository", "parentTaskId", "howToTest", "department", "team",
            "workItems",
        )
        if field in data
    ]
    if not changed or any(field not in allowed for field in data):
        return _response(runtime, {"error": "invalid_update"}, status=400)
    title = (
        _text(data.get("title"), MAX_TITLE)
        if "title" in data
        else _text(current.get("title"), MAX_TITLE)
    )
    details = (
        _multiline(data.get("details"), MAX_DETAILS)
        if "details" in data
        else _multiline(current.get("details"), MAX_DETAILS)
    )
    assignee_kind = str(
        data.get("assigneeKind")
        if "assigneeKind" in data
        else row.get("assignee_kind") or "user"
    ).strip().lower()
    if assignee_kind in AGENT_ASSIGNEE_KINDS:
        assignee_kind = "agent"
    if assignee_kind not in ("user", "agent", "unassigned"):
        return _response(
            runtime, {"error": "invalid_assignee_kind"}, status=400)
    assignee = (
        _text(data.get("assignee"), 64).lower()
        if "assignee" in data
        else _text(current.get("assignee"), 64).lower()
    )
    repository = (
        _text(data.get("repository"), 201)
        if "repository" in data
        else _text(current.get("repository"), 201)
    )
    if repository:
        parts = repository.split("/", 1)
        if (
            len(parts) != 2
            or not _REPO_SEGMENT_RE.fullmatch(parts[0])
            or not _REPO_SEGMENT_RE.fullmatch(parts[1])
        ):
            return _response(
                runtime, {"error": "invalid_repository"}, status=400)
    department = (
        _text(data.get("department"), 64, "general").lower()
        if "department" in data
        else str(row.get("department") or "general").lower()
    )
    team = (
        _text(data.get("team"), 64).lower()
        if "team" in data
        else str(row.get("team") or "").lower()
    )
    if (
        ("department" in data or "team" in data)
        and (
            marketing_only
            or not _SCOPE_RE.fullmatch(department)
            or department not in DEPARTMENTS
            or (team and not _SCOPE_RE.fullmatch(team))
        )
    ):
        return _response(runtime, {"error": "invalid_task_routing"}, status=400)
    how_to_test = (
        _multiline(data.get("howToTest"), 720)
        if "howToTest" in data
        else _multiline(current.get("howToTest"), 720)
    )
    if "workItems" in data:
        work_items, work_items_error = _work_items(
            data.get("workItems"), strict=True)
        if work_items_error:
            return _response(runtime, {"error": work_items_error}, status=400)
    else:
        work_items = _work_items(current.get("workItems"))[0]
    parent_task_id = (
        str(current.get("parentTaskId") or "").lower()
        if valid_id(current.get("parentTaskId")) else ""
    )
    if "parentTaskId" in data:
        parent_task_id = str(data.get("parentTaskId") or "").strip().lower()
        if parent_task_id and (
            marketing_only
            or not valid_id(parent_task_id)
            or parent_task_id == task_id
        ):
            return _response(
                runtime, {"error": "invalid_parent_task"}, status=400)
        cursor = parent_task_id
        for _ in range(MAX_PARENT_DEPTH if parent_task_id else 0):
            ancestor = await _task(runtime, org_bi, cursor)
            if not ancestor:
                return _response(
                    runtime, {"error": "parent_task_not_found"}, status=404)
            try:
                ancestor_data = await runtime.open(ancestor.get("data"))
            except Exception:
                ancestor_data = {}
            cursor = str((ancestor_data or {}).get("parentTaskId") or "").lower()
            if not valid_id(cursor):
                break
            if cursor == task_id:
                return _response(
                    runtime, {"error": "invalid_parent_task"}, status=400)
    priority = _priority(
        data.get("priority")
        if "priority" in data
        else row.get("priority"),
    )
    if not title:
        return _response(
            runtime, {"error": "title_required"}, status=400)
    if int(row.get("completed_at") or 0) > 0:
        return _response(
            runtime, {"error": "completed_task_cannot_be_updated"}, status=409)
    if str(row.get("status") or "") == "active":
        return _response(
            runtime, {"error": "active_task_cannot_be_updated"}, status=409)
    member = None
    assignee_bi = ""
    destination = str(row.get("destination") or "department")
    if assignee_kind == "user":
        if not assignee:
            return _response(
                runtime, {"error": "assignee_required"}, status=400)
        member = (
            await runtime.marketing_member(org_bi, assignee)
            if marketing_only
            else await runtime.organization_member(org_bi, assignee)
        )
        if not member:
            return _response(
                runtime,
                {
                    "error": (
                        "assignee_not_marketing_member"
                        if marketing_only else "assignee_not_org_member"
                    )
                },
                status=400,
            )
        assignee = member["name"]
        assignee_bi = member["bi"]
        if destination == "agent":
            destination = "department"
    elif assignee_kind == "agent":
        if marketing_only:
            return _response(runtime, {"error": "invalid_update"}, status=400)
        if not repository:
            return _response(
                runtime, {"error": "repository_required"}, status=400)
        assignee = "agent"
        destination = "agent"
    else:
        assignee = ""
        destination = (
            "department" if destination == "agent" else destination
        )
    agent_session_id = (
        ""
        if "assigneeKind" in data
        else str(row.get("agent_session_id") or "")
    )
    routing = _routing_snapshot_values(
        department, team,
        destination, assignee_kind, assignee, repository,
    )
    prior_pre_qa = current.get("preQaRouting")
    if not isinstance(prior_pre_qa, dict):
        prior_pre_qa = None
    sealed = await runtime.seal({
        "kind": (
            str(current.get("kind") or "task")
            if str(current.get("kind") or "task") in TASK_KINDS
            else "task"
        ),
        "title": title,
        "details": details,
        "attachments": _attachments(current.get("attachments")),
        "completionNote": _multiline(
            current.get("completionNote"), MAX_COMPLETION_NOTE),
        "assignee": assignee,
        "createdBy": _text(current.get("createdBy"), 64).lower(),
        "parentTaskId": parent_task_id,
        "bountyRequest": (
            current.get("bountyRequest")
            if isinstance(current.get("bountyRequest"), dict)
            else None
        ),
        "repository": repository,
        "howToTest": how_to_test,
        "workItems": work_items,
        "qaReviewer": _text(current.get("qaReviewer"), 64).lower(),
        "routing": routing,
        "preQaRouting": prior_pre_qa,
        "completionHistory": _completion_history(
            current.get("completionHistory")),
        # Editing the copy never rewrites who ran the task or how.
        "agent": _agent_run(
            current.get("agent"), _run_by(current.get("agent"))),
    })
    await runtime.d1_run(
        "UPDATE organization_tasks "
        "SET assignee_kind=?,assignee_bi=?,destination=?,agent_session_id=?,"
        "department=?,team=?,priority=?,data=?,updated_at=? "
        "WHERE org_bi=? AND task_id=? AND status='idle'",
        assignee_kind,
        assignee_bi,
        destination,
        agent_session_id,
        department,
        team,
        priority,
        sealed,
        now,
        org_bi,
        task_id,
    )
    changed_row = await _task(runtime, org_bi, task_id)
    if (
        not changed_row
        or str(changed_row.get("status") or "") != "idle"
        or str(changed_row.get("assignee_kind") or "") != assignee_kind
        or str(changed_row.get("assignee_bi") or "") != assignee_bi
        or str(changed_row.get("data") or "") != str(sealed)
    ):
        return _response(runtime, {"error": "task_state_conflict"}, status=409)
    await runtime.audit(
        actor,
        (
            "office.marketing_task_updated"
            if marketing_only else "organization.task_updated"
        ),
        "organization_task",
        task_id,
        details={"fields": ",".join(changed)},
    )
    await _record_event(
        runtime, org_bi, task_id, account_bi, actor, "updated", now,
        detail={"fields": changed},
    )
    updated_task = await _project_task(runtime, changed_row, now)
    edit_changes = _edit_changes(
        current, row, title, details, assignee_kind, assignee, priority,
        repository, department, team, how_to_test, parent_task_id,
        work_items)
    if edit_changes and updated_task:
        await _notify_activity(
            runtime, org_bi, actor, task_id, updated_task, now, "updated",
            changes=edit_changes,
        )
    await _notify_mentions(
        runtime, org_bi, actor, task_id, updated_task,
        _mentions(
            updated_task.get("title") if updated_task else "",
            updated_task.get("details") if updated_task else "",
        ),
        "task",
        (updated_task or {}).get("details") or "",
        now,
    )
    return await _task_response(runtime, changed_row, now)


def _edit_changes(
        current, row, title, details, assignee_kind, assignee, priority,
        repository, department=None, team=None, how_to_test=None,
        parent_task_id=None, work_items=None):
    changes = []
    before_title = _text(current.get("title"), MAX_TITLE)
    if title != before_title:
        changes.append({"field": "title", "from": before_title, "to": title})
    if details != _multiline(current.get("details"), MAX_DETAILS):
        changes.append({"field": "details"})
    before_kind = str(row.get("assignee_kind") or "user")
    if before_kind in AGENT_ASSIGNEE_KINDS:
        before_kind = "agent"
    before_assignee = (
        _text(current.get("assignee"), 64).lower()
        if before_kind == "user" else ""
    )
    after_assignee = assignee if assignee_kind == "user" else ""
    if before_kind != assignee_kind or before_assignee != after_assignee:
        changes.append({
            "field": "assignee",
            "kind": assignee_kind,
            "to": after_assignee,
            "from": before_assignee,
        })
    before_priority = _priority(row.get("priority"))
    if priority != before_priority:
        changes.append({
            "field": "priority", "from": before_priority, "to": priority,
        })
    if repository != _text(current.get("repository"), 201):
        changes.append({"field": "repository", "to": repository})
    before_department = str(row.get("department") or "general").lower()
    before_team = str(row.get("team") or "").lower()
    if (
        department is not None
        and (department, team or "") != (before_department, before_team)
    ):
        changes.append({
            "field": "routing", "department": department, "team": team or "",
        })
    if (
        how_to_test is not None
        and how_to_test != _multiline(current.get("howToTest"), 720)
    ):
        changes.append({"field": "howToTest"})
    before_parent = (
        str(current.get("parentTaskId") or "").lower()
        if valid_id(current.get("parentTaskId")) else ""
    )
    if parent_task_id is not None and parent_task_id != before_parent:
        changes.append({"field": "parent", "to": parent_task_id})
    if work_items is not None:
        previous_items = _work_items(current.get("workItems"))[0]
        if work_items != previous_items:
            changes.append({
                "field": "workItems",
                "summary": _work_items_summary(previous_items, work_items),
            })
    return changes


async def _start(
        runtime, org_bi, account_bi, actor, task_id, row, now):
    if str(row.get("assignee_bi") or "") != account_bi:
        return _response(runtime, {"error": "assignee_only"}, status=403)
    if int(row.get("completed_at") or 0) > 0:
        return _response(runtime, {"error": "task_completed"}, status=409)
    if str(row.get("status") or "") == "active":
        return await _task_response(runtime, row, now)
    existing = await runtime.d1_first(
        "SELECT task_id FROM organization_tasks "
        "WHERE org_bi=? AND active_assignee_bi=? AND status='active' "
        "LIMIT 1",
        org_bi,
        account_bi,
    )
    if existing:
        return _response(
            runtime,
            {
                "error": "active_task_exists",
                "activeTaskId": str(existing.get("task_id") or ""),
            },
            status=409,
        )
    deadline = _checkin_deadline(runtime, now)
    try:
        await runtime.d1_run(
            "UPDATE organization_tasks SET "
            "status='active',active_assignee_bi=?,started_at=?,"
            "next_checkin_at=?,updated_at=? "
            "WHERE org_bi=? AND task_id=? AND assignee_bi=? "
            "AND status='idle' AND active_assignee_bi=''",
            account_bi,
            now,
            deadline,
            now,
            org_bi,
            task_id,
            account_bi,
        )
    except Exception as error:
        if "UNIQUE" in str(error).upper():
            return _response(
                runtime, {"error": "active_task_exists"}, status=409)
        raise
    changed = await _task(runtime, org_bi, task_id)
    if not changed or str(changed.get("status") or "") != "active":
        return _response(runtime, {"error": "task_state_conflict"}, status=409)
    await runtime.audit(
        actor,
        "office.marketing_task_started",
        "office_task",
        task_id,
        details={"state": "active"},
    )
    await _record_event(
        runtime, org_bi, task_id, account_bi, actor, "started", now)
    await _notify_activity(
        runtime, org_bi, actor, task_id, changed, now, "started")
    return await _task_response(runtime, changed, now)


async def _apply_agent_status(
        runtime, org_bi, account_bi, actor, task_id, row, status, can_manage,
        now, agent_run=None):
    """Write one task's live agent state, returning "" or an error code.

    A desktop may report the task it created without receiving broad task-edit
    authority.  The signature is checked by the entrypoint; this second gate
    still verifies ownership and that the row is agent work — and it applies to
    a batched write exactly as it does to a single-task one.

    ``agent_run`` carries the run's provenance (which bot, model, mode and local
    session) so that a desktop picking up a task somebody else opened by hand
    can say what is working it.  It layers onto whatever is already recorded,
    exactly the way the completion report does.
    """

    if status not in AGENT_RUN_STATES:
        return "invalid_agent_status"
    if not row:
        return "task_not_found"
    if (
        str(row.get("assignee_kind") or "") not in AGENT_ASSIGNEE_KINDS
        or (
            not can_manage
            and str(row.get("created_by_bi") or "") != account_bi
        )
    ):
        return "forbidden"
    try:
        current = await runtime.open(row.get("data"))
    except Exception:
        current = None
    if not isinstance(current, dict):
        return "agent_run_unavailable"
    # A task opened by hand on the board holds no run provenance until a desktop
    # starts an agent on it, so the first status write seeds the record instead
    # of being refused — otherwise a run bound to an existing task could never
    # report anything. The gates above are unchanged: this is still only
    # reachable for agent work, by its creator or a manager.
    agent = dict(current.get("agent") or {})
    if isinstance(agent_run, dict):
        agent.update({
            field: value
            for field, value in agent_run.items()
            if value and field != "status"
        })
    agent["status"] = status
    current["agent"] = _agent_run(
        agent, actor or _run_by(current.get("agent")))
    await runtime.d1_run(
        "UPDATE organization_tasks SET data=?,updated_at=? "
        "WHERE org_bi=? AND task_id=?",
        await runtime.seal(current), now, org_bi, task_id,
    )
    await runtime.audit(
        actor,
        "organization.task_agent_status",
        "organization_task",
        task_id,
        details={"status": status},
    )
    return ""


async def _update_agent_status(
        runtime, org_bi, account_bi, actor, task_id, row, data, can_manage,
        now):
    """Update only the live state of the desktop run attached to one task."""

    error = await _apply_agent_status(
        runtime, org_bi, account_bi, actor, task_id, row,
        _text(data.get("status"), 16).lower(), can_manage, now,
        agent_run=data.get("agent"),
    )
    if error:
        return _response(
            runtime,
            {"error": error},
            status=AGENT_STATUS_ERROR_CODES.get(error, 400),
        )
    return await _task_response(
        runtime, await _task(runtime, org_bi, task_id), now)


async def _update_agent_status_batch(
        runtime, org_bi, account_bi, actor, data, can_manage, now,
        marketing_only=False):
    """Report the live state of every desktop run in one signed write.

    A desktop mirrors each local session's state onto its task, and the first
    reload after launch has a state to publish for every session at once — one
    signed POST per session, all in the same millisecond, which is what the
    relay's rate limiter answered 429 to on a fleet-sized node (adhoc #1618).
    The batch collapses that startup burst into a single request; afterwards
    only genuinely changed rows are sent, and the board's own updates reach the
    desktop over its node socket rather than by polling.

    Per-task authorization is unchanged: each entry runs the same ownership and
    agent-row gates as the single-task endpoint, so a batch can never reach a
    task its signer could not have written to one at a time.  Failures are
    reported per entry instead of failing the whole write — one task whose row
    was deleted must not strand the states of the other sixteen.
    """

    entries = data.get("statuses")
    if not isinstance(entries, list) or not entries:
        return _response(
            runtime, {"error": "invalid_agent_status_batch"}, status=400)
    if len(entries) > MAX_AGENT_STATUS_BATCH:
        return _response(
            runtime, {"error": "agent_status_batch_too_large"}, status=400)
    results = []
    seen = set()
    presence = 0
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        if not _text(entry.get("task"), 64) and not marketing_only:
            error = await _record_presence(
                runtime, org_bi, account_bi, actor, entry, now)
            if error is None:
                continue
            presence += 1
            results.append({
                "task": "",
                "session": _agent_ref((entry.get("agent") or {}).get(
                    "sessionId") if isinstance(entry.get("agent"), dict)
                    else ""),
                "ok": not error,
                "error": error,
            })
            continue
        task_id = _text(entry.get("task"), 64).lower()
        status = _text(entry.get("status"), 16).lower()
        if not valid_id(task_id) or task_id in seen:
            continue
        seen.add(task_id)
        row = await _task(runtime, org_bi, task_id)
        if row and marketing_only and str(
                row.get("department") or "") != "marketing":
            row = None
        error = await _apply_agent_status(
            runtime, org_bi, account_bi, actor, task_id, row, status,
            can_manage, now, agent_run=entry.get("agent"),
        )
        results.append({
            "task": task_id,
            "ok": not error,
            "error": error,
        })
    if not results:
        return _response(
            runtime, {"error": "invalid_agent_status_batch"}, status=400)
    if presence:
        await _prune_presence(runtime, org_bi, account_bi, now)
    return _response(runtime, {
        "ok": True,
        "actor": actor,
        "results": results,
    })


async def _record_presence(runtime, org_bi, account_bi, actor, entry, now):
    agent = entry.get("agent") if isinstance(entry.get("agent"), dict) else {}
    session_id = _agent_ref(agent.get("sessionId"))
    status = _text(entry.get("status"), 16).lower()
    if not session_id:
        return None
    if status not in AGENT_RUN_STATES or not account_bi or not actor:
        return "invalid_agent_status"
    started = agent.get("startedAt")
    started_at = (
        int(started) if isinstance(started, int) and not isinstance(
            started, bool) and 0 < started <= now + 60000 else 0)
    run = _agent_run({**agent, "status": status}) or {}
    record = {
        "provider": run.get("provider", ""),
        "model": run.get("model", ""),
        "startedBy": run.get("startedBy", ""),
        "strength": run.get("strength", ""),
        "sessionId": session_id,
        "status": status,
        "owner": actor,
        "startedAt": started_at,
        "title": _redact(agent.get("title"), MAX_AGENT_TITLE),
        "prose": run.get("prose", ""),
    }
    await runtime.d1_run(
        "INSERT INTO organization_agent_presence "
        "(org_bi,account_bi,session_id,data,live,updated_at) "
        "VALUES (?,?,?,?,?,?) ON CONFLICT(org_bi,account_bi,session_id) "
        "DO UPDATE SET data=excluded.data,live=excluded.live,"
        "updated_at=excluded.updated_at",
        org_bi, account_bi, session_id, await runtime.seal(record),
        1 if status in AGENT_LIVE_STATES else 0, now,
    )
    return ""


async def _prune_presence(runtime, org_bi, account_bi, now):
    await runtime.d1_run(
        "DELETE FROM organization_agent_presence WHERE org_bi=? AND "
        "account_bi=? AND ((live=0 AND updated_at<?) OR updated_at<?)",
        org_bi, account_bi, now - PRESENCE_FINISHED_MS,
        now - PRESENCE_STALE_MS,
    )
    await runtime.d1_run(
        "DELETE FROM organization_agent_presence WHERE org_bi=? AND "
        "account_bi=? AND session_id NOT IN (SELECT session_id FROM "
        "organization_agent_presence WHERE org_bi=? AND account_bi=? "
        "ORDER BY live DESC,updated_at DESC LIMIT ?)",
        org_bi, account_bi, org_bi, account_bi, PRESENCE_PER_ACCOUNT,
    )


async def _presence_records(runtime, org_bi, now):
    try:
        return await _presence_rows(runtime, org_bi, now)
    except Exception:
        return []


async def _presence_rows(runtime, org_bi, now):
    rows = await runtime.d1_all(
        "SELECT p.data,p.live,p.updated_at FROM organization_agent_presence p "
        "INNER JOIN org_members m ON m.org_bi=p.org_bi "
        "AND m.member_bi=p.account_bi WHERE p.org_bi=? AND "
        "((p.live=1 AND p.updated_at>=?) OR p.updated_at>=?) "
        "ORDER BY p.live DESC,p.updated_at DESC LIMIT ?",
        org_bi, now - PRESENCE_STALE_MS, now - PRESENCE_FINISHED_MS,
        PRESENCE_LIST_LIMIT,
    )
    out = []
    for row in rows or []:
        try:
            record = await runtime.open(row.get("data"))
        except Exception:
            record = None
        if not isinstance(record, dict):
            continue
        session_id = _agent_ref(record.get("sessionId"))
        owner = _text(record.get("owner"), 64).lower()
        status = _text(record.get("status"), 16).lower()
        if not session_id or not owner or status not in AGENT_RUN_STATES:
            continue
        out.append({
            "sessionId": session_id,
            "owner": owner,
            "status": status,
            "provider": _text(record.get("provider"), 40).lower(),
            "model": _text(record.get("model"), MAX_AGENT_FIELD),
            "startedBy": _agent_ref(record.get("startedBy")).lower(),
            "strength": _text(record.get("strength"), 32).lower(),
            "startedAt": int(record.get("startedAt") or 0),
            "updatedAt": int(row.get("updated_at") or 0),
            "title": _redact(record.get("title"), MAX_AGENT_TITLE),
            "prose": _redact(record.get("prose"), MAX_AGENT_PROSE),
        })
    return out


async def _stop(
        runtime, org_bi, account_bi, actor, task_id, row, now):
    if str(row.get("assignee_bi") or "") != account_bi:
        return _response(runtime, {"error": "assignee_only"}, status=403)
    if (
        str(row.get("status") or "") != "active"
        or str(row.get("active_assignee_bi") or "") != account_bi
    ):
        return _response(runtime, {"error": "task_not_active"}, status=409)
    started_at = int(row.get("started_at") or 0)
    addition = max(0, now - started_at) if started_at else 0
    elapsed = min(
        MAX_ELAPSED_MS,
        max(0, int(row.get("elapsed_ms") or 0)) + addition,
    )
    await runtime.d1_run(
        "UPDATE organization_tasks SET "
        "status='idle',active_assignee_bi='',elapsed_ms=?,started_at=0,"
        "next_checkin_at=0,updated_at=? "
        "WHERE org_bi=? AND task_id=? AND assignee_bi=? "
        "AND status='active' AND active_assignee_bi=?",
        elapsed,
        now,
        org_bi,
        task_id,
        account_bi,
        account_bi,
    )
    await runtime.audit(
        actor,
        "office.marketing_task_stopped",
        "office_task",
        task_id,
        details={"state": "idle"},
    )
    changed = await _task(runtime, org_bi, task_id)
    await _record_event(
        runtime, org_bi, task_id, account_bi, actor, "stopped", now)
    await _notify_activity(
        runtime, org_bi, actor, task_id, changed, now, "stopped")
    return await _task_response(runtime, changed, now)


def _completion_entry(note, completed_at, completed_by, qa_status="unknown"):
    status = str(qa_status or "unknown").lower()
    if status not in QA_STATES:
        status = "unknown"
    return {
        "note": _multiline(note, MAX_COMPLETION_NOTE),
        "completedAt": max(0, _integer(completed_at)),
        "completedBy": _text(completed_by, 64).lower(),
        "qaStatus": status,
        "qaReviewer": "",
        "qaReviewedAt": 0,
    }


def _remember_completion(data, note, completed_at, completed_by):
    history = _completion_history(data.get("completionHistory"))
    history.append(_completion_entry(
        note, completed_at, completed_by, "unknown"))
    data["completionHistory"] = history[-MAX_COMPLETION_HISTORY:]


def _update_latest_completion_note(data, note, completed_at, completed_by=""):
    history = _completion_history(data.get("completionHistory"))
    if not history:
        history = [_completion_entry(note, completed_at, completed_by)]
    else:
        latest = dict(history[-1])
        latest["note"] = _multiline(note, MAX_COMPLETION_NOTE)
        if not latest.get("completedAt"):
            latest["completedAt"] = max(0, _integer(completed_at))
        if completed_by and not latest.get("completedBy"):
            latest["completedBy"] = _text(completed_by, 64).lower()
        history[-1] = latest
    data["completionHistory"] = history[-MAX_COMPLETION_HISTORY:]


async def _complete(
        runtime, org_bi, account_bi, actor, task_id, row, data,
        can_manage, now):
    """Finish assigned work and place it in the private QA review queue."""
    # A bot-assigned task has no member assignee_bi to match, so the desktop
    # that opened it — and only that desktop — reports its run as finished.
    launched_agent_run = (
        str(row.get("assignee_kind") or "") in AGENT_ASSIGNEE_KINDS
        and str(row.get("created_by_bi") or "") == account_bi
    )
    if (
        not can_manage
        and not launched_agent_run
        and str(row.get("assignee_bi") or "") != account_bi
    ):
        return _response(runtime, {"error": "assignee_or_manager_only"}, status=403)
    if (
        int(row.get("completed_at") or 0) > 0
        and int(row.get("qa_requested_at") or 0) > 0
    ):
        # Completion is idempotent, but a retried client may be adding the
        # human-readable work note after an older client already finished the
        # task. Preserve the completed/QA state and update only encrypted copy.
        if "completionNote" in data:
            try:
                completed_data = await runtime.open(row.get("data"))
            except Exception:
                completed_data = None
            if not isinstance(completed_data, dict):
                return _response(
                    runtime, {"error": "task_unavailable"}, status=500)
            completed_data["completionNote"] = _multiline(
                data.get("completionNote"), MAX_COMPLETION_NOTE)
            _update_latest_completion_note(
                completed_data,
                completed_data["completionNote"],
                row.get("completed_at"),
                completed_data.get("completedBy") or actor,
            )
            await runtime.d1_run(
                "UPDATE organization_tasks SET data=?,updated_at=? "
                "WHERE org_bi=? AND task_id=? AND completed_at>0",
                await runtime.seal(completed_data),
                now,
                org_bi,
                task_id,
            )
            await runtime.audit(
                actor,
                "organization.task_completion_note_updated",
                "organization_task",
                task_id,
                details={
                    "hasCompletionNote": bool(
                        completed_data["completionNote"]),
                },
            )
            row = await _task(runtime, org_bi, task_id)
        return await _task_response(runtime, row, now)
    try:
        current = await runtime.open(row.get("data"))
    except Exception:
        current = None
    if not isinstance(current, dict):
        return _response(runtime, {"error": "task_unavailable"}, status=500)
    current_routing = current.get("routing")
    if not isinstance(current_routing, dict):
        current_routing = _routing_snapshot(row, current)
    if str(row.get("destination") or "") != "qa":
        current["preQaRouting"] = current_routing
    current["routing"] = current_routing
    current["completionNote"] = _multiline(
        data.get("completionNote")
        if "completionNote" in data
        else current.get("completionNote"),
        MAX_COMPLETION_NOTE,
    )
    if not _multiline(current.get("howToTest"), 720):
        current["howToTest"] = (
            "Open the completed feature and follow its normal user flow. "
            "Confirm the requested behavior works, existing behavior did not "
            "regress, and no console or network error appears."
        )
    current["qaReviewer"] = ""
    current["qaFailureReason"] = ""
    current["qaFailureScreenshot"] = ""
    # The finishing bot (and any model/mode/strength it ended up running with)
    # layers onto whatever the launching bot recorded when it opened the task.
    if isinstance(data.get("agent"), dict):
        merged = dict(current.get("agent") or {})
        merged.update({
            field: value
            for field, value in data["agent"].items()
            if value
        })
        current["agent"] = _agent_run(
            merged, _run_by(current.get("agent")))
    _remember_completion(
        current,
        current["completionNote"],
        now,
        actor,
    )
    started_at = int(row.get("started_at") or 0)
    addition = (
        max(0, int(now) - started_at)
        if str(row.get("status") or "") == "active" and started_at
        else 0
    )
    elapsed = min(
        MAX_ELAPSED_MS,
        max(0, int(row.get("elapsed_ms") or 0)) + addition,
    )
    await runtime.d1_run(
        "UPDATE organization_tasks SET "
        "status='idle',active_assignee_bi='',elapsed_ms=?,started_at=0,"
        "next_checkin_at=0,completed_at=?,destination='qa',"
        "qa_status='unknown',qa_reviewer_bi='',qa_reviewed_at=0,"
        "qa_requested_at=?,data=?,updated_at=? "
        "WHERE org_bi=? AND task_id=?",
        elapsed,
        now,
        now,
        await runtime.seal(current),
        now,
        org_bi,
        task_id,
    )
    changed = await _task(runtime, org_bi, task_id)
    if not changed or int(changed.get("completed_at") or 0) <= 0:
        return _response(runtime, {"error": "task_state_conflict"}, status=409)
    await runtime.audit(
        actor,
        "office.marketing_task_completed",
        "office_task",
        task_id,
        details={
            "state": "done",
            "qa": "ready",
            "hasCompletionNote": bool(current["completionNote"]),
        },
    )
    await _record_event(
        runtime, org_bi, task_id, account_bi, actor, "completed", now,
        detail={"note": current["completionNote"]},
    )
    await _notify_activity(
        runtime, org_bi, actor, task_id, changed, now, "completed")
    return await _task_response(runtime, changed, now)


async def _request_qa(
        runtime, org_bi, account_bi, actor, task_id, row, data,
        can_manage, now):
    if (
        not can_manage
        and str(row.get("created_by_bi") or "") != account_bi
        and str(row.get("assignee_bi") or "") != account_bi
    ):
        return _response(
            runtime, {"error": "task_owner_or_manager_only"}, status=403)
    try:
        current = await runtime.open(row.get("data"))
    except Exception:
        current = None
    if not isinstance(current, dict):
        return _response(runtime, {"error": "task_unavailable"}, status=500)
    routing = current.get("routing")
    if not isinstance(routing, dict):
        routing = _routing_snapshot(row, current)
    if str(row.get("destination") or "") != "qa":
        current["preQaRouting"] = routing
    current["routing"] = routing
    how_to_test = _multiline(
        data.get("howToTest"),
        720,
        _multiline(
            current.get("howToTest"),
            720,
            "Follow the normal user flow and confirm the requested behavior.",
        ),
    )
    current["howToTest"] = how_to_test
    current["qaReviewer"] = ""
    await runtime.d1_run(
        "UPDATE organization_tasks SET destination='qa',qa_status='unknown',"
        "qa_reviewer_bi='',qa_reviewed_at=0,qa_requested_at=?,data=?,"
        "updated_at=? WHERE org_bi=? AND task_id=?",
        now, await runtime.seal(current), now, org_bi, task_id,
    )
    await runtime.audit(
        actor, "organization.task_sent_to_qa", "organization_task", task_id,
        details={"qa": "requested"},
    )
    changed = await _task(runtime, org_bi, task_id)
    await _record_event(
        runtime, org_bi, task_id, account_bi, actor, "qa_requested", now)
    await _notify_activity(
        runtime, org_bi, actor, task_id, changed, now, "qa requested")
    return await _task_response(runtime, changed, now)


async def _undo(
        runtime, org_bi, account_bi, actor, task_id, row, can_manage, now):
    """Reopen completed work and restore its encrypted pre-QA route."""

    reviewer = str(row.get("qa_reviewer_bi") or "")
    if (
        not can_manage
        and str(row.get("created_by_bi") or "") != account_bi
        and str(row.get("assignee_bi") or "") != account_bi
        and reviewer != account_bi
    ):
        return _response(runtime, {"error": "task_owner_or_reviewer_only"}, status=403)
    completed_at = max(0, _integer(row.get("completed_at")))
    qa_requested_at = max(0, _integer(row.get("qa_requested_at")))
    if not completed_at and not qa_requested_at:
        return _response(runtime, {"error": "task_not_completed"}, status=409)
    try:
        current = await runtime.open(row.get("data"))
    except Exception:
        current = None
    if not isinstance(current, dict):
        return _response(runtime, {"error": "task_unavailable"}, status=500)

    routing = current.get("preQaRouting")
    if not isinstance(routing, dict):
        routing = current.get("routing")
    if not isinstance(routing, dict):
        routing = _routing_snapshot(row, current)
    restored_destination = str(
        routing.get("destination") or "department").strip().lower()
    if restored_destination not in DESTINATIONS or restored_destination == "qa":
        restored_destination = (
            "agent"
            if str(row.get("assignee_kind") or "") in AGENT_ASSIGNEE_KINDS
            else "department"
        )
    restored_routing = _routing_snapshot_values(
        routing.get("department") or row.get("department"),
        routing.get("team") or row.get("team"),
        restored_destination,
        routing.get("assigneeKind") or row.get("assignee_kind"),
        routing.get("assignee") or current.get("assignee"),
        routing.get("repository") or current.get("repository"),
    )

    history = _completion_history(current.get("completionHistory"))
    if completed_at:
        if not history:
            history = [_completion_entry(
                current.get("completionNote"), completed_at, actor)]
        latest = dict(history[-1])
        latest["qaStatus"] = str(row.get("qa_status") or "unknown").lower()
        if latest["qaStatus"] not in QA_STATES:
            latest["qaStatus"] = "unknown"
        latest["qaReviewer"] = _text(
            current.get("qaReviewer"), 64).lower()
        latest["qaReviewedAt"] = max(
            0, _integer(row.get("qa_reviewed_at")))
        latest["undoneAt"] = now
        latest["undoneBy"] = actor
        history[-1] = latest
    current["completionHistory"] = history[-MAX_COMPLETION_HISTORY:]
    current["routing"] = restored_routing
    current["preQaRouting"] = None
    # Keep completionNote and the history ledger; only the live QA state is
    # cleared. Failure details remain inside the completed history boundary,
    # while the next run starts without a stale reviewer or screenshot.
    current["qaReviewer"] = ""
    current["qaFailureReason"] = ""
    current["qaFailureScreenshot"] = ""
    await runtime.d1_run(
        "UPDATE organization_tasks SET status='idle',active_assignee_bi='',"
        "started_at=0,next_checkin_at=0,completed_at=0,destination=?,"
        "qa_status='unknown',qa_reviewer_bi='',qa_reviewed_at=0,"
        "qa_requested_at=0,agent_session_id='',data=?,updated_at=? "
        "WHERE org_bi=? AND task_id=?",
        restored_destination,
        await runtime.seal(current),
        now,
        org_bi,
        task_id,
    )
    # A review belongs to the completed QA pass, not to the reopened run.
    # Removing it also keeps the QA board from surfacing stale verdicts.
    await runtime.d1_run(
        "DELETE FROM organization_task_qa_reviews "
        "WHERE org_bi=? AND task_id=?",
        org_bi,
        task_id,
    )
    changed = await _task(runtime, org_bi, task_id)
    if not changed or int(changed.get("completed_at") or 0) > 0:
        return _response(runtime, {"error": "task_state_conflict"}, status=409)
    await runtime.audit(
        actor,
        "organization.task_marked_undone",
        "organization_task",
        task_id,
        details={
            "restoredDestination": restored_destination,
            "hadCompletion": bool(completed_at),
            "hadQaReview": bool(reviewer or row.get("qa_reviewed_at")),
        },
    )
    await _record_event(
        runtime, org_bi, task_id, account_bi, actor, "reopened", now)
    await _notify_activity(
        runtime, org_bi, actor, task_id, changed, now, "undone")
    return await _task_response(runtime, changed, now)


async def _reply(
        runtime, org_bi, account_bi, actor, task_id, row, data, now):
    """Store one encrypted member reply and return the bounded task window."""

    body = _multiline(
        data.get("body")
        if "body" in data
        else data.get("reply")
        if "reply" in data
        else data.get("message")
        if "message" in data
        else data.get("text"),
        MAX_REPLY_BODY,
    )
    if not body:
        return _response(runtime, {"error": "reply_required"}, status=400)
    task = await _project_task(runtime, row, now)
    if task is None:
        return _response(runtime, {"error": "task_unavailable"}, status=500)
    response_id = runtime.new_id()
    if not valid_id(response_id):
        return _response(runtime, {"error": "id_generation_failed"}, status=500)
    # The sealed snapshot lets a compact context remain stable even after a
    # later routing/QA transition; it never becomes an authorization source.
    sealed = await runtime.seal({
        "author": actor,
        "body": body,
        "context": _compact_task_context(task),
    })
    try:
        await runtime.d1_run(
            "INSERT INTO organization_task_responses "
            "(response_id,task_id,org_bi,author_bi,data,created_at) "
            "VALUES (?,?,?,?,?,?)",
            response_id,
            task_id,
            org_bi,
            account_bi,
            sealed,
            now,
        )
    except Exception as error:
        if "response" in str(error).lower() and "full" in str(error).lower():
            return _response(
                runtime, {"error": "reply_capacity_reached"}, status=409)
        raise
    await runtime.audit(
        actor,
        "organization.task_replied",
        "organization_task",
        task_id,
        details={"bodyLength": len(body)},
    )
    mentioned = _mentions(body)
    await _record_event(
        runtime, org_bi, task_id, account_bi, actor, "replied", now,
        detail={"body": body, "mentions": mentioned},
    )
    # A comment is the one task event every watcher wants: it fans out to the
    # floor team like any other activity, and separately to whoever it named.
    await _notify_activity(
        runtime, org_bi, actor, task_id, task, now, "commented",
        changes=[{"field": "comment", "excerpt": body}],
        skip=mentioned,
    )
    await _notify_mentions(
        runtime, org_bi, actor, task_id, task, mentioned, "comment", body,
        now)
    changed = await _task(runtime, org_bi, task_id)
    return await _task_response(runtime, changed, now, status=201)


async def _return_to_list(
        runtime, org_bi, account_bi, actor, task_id, row, can_manage, now):
    """Pull a bot task back off the node queue and onto the task list."""

    if (
        not can_manage
        and str(row.get("created_by_bi") or "") != account_bi
    ):
        return _response(
            runtime, {"error": "task_owner_or_manager_only"}, status=403)
    if (
        str(row.get("assignee_kind") or "") not in AGENT_ASSIGNEE_KINDS
        and str(row.get("destination") or "") != "agent"
    ):
        return _response(runtime, {"error": "task_not_queued"}, status=409)
    if int(row.get("completed_at") or 0) > 0:
        return _response(runtime, {"error": "task_already_done"}, status=409)
    try:
        current = await runtime.open(row.get("data"))
    except Exception:
        current = None
    if not isinstance(current, dict):
        return _response(runtime, {"error": "task_unavailable"}, status=500)
    session_id = str(row.get("agent_session_id") or "")
    # Best effort: a node that already leased the job simply finds it retired.
    cancel = getattr(runtime, "cancel_agent_session", None)
    if session_id and callable(cancel):
        try:
            await cancel(org_bi, session_id)
        except Exception:
            pass
    current["assignee"] = ""
    await runtime.d1_run(
        "UPDATE organization_tasks SET assignee_kind='unassigned',"
        "assignee_bi='',destination='department',agent_session_id='',"
        "data=?,updated_at=? WHERE org_bi=? AND task_id=?",
        await runtime.seal(current), now, org_bi, task_id,
    )
    await runtime.audit(
        actor, "organization.task_returned", "organization_task", task_id,
        details={"returned": True, "hadSession": bool(session_id)},
    )
    changed = await _task(runtime, org_bi, task_id)
    await _record_event(
        runtime, org_bi, task_id, account_bi, actor, "returned", now)
    await _notify_activity(
        runtime, org_bi, actor, task_id, changed, now, "returned")
    return await _task_response(runtime, changed, now)


def _can_trash(row, principal):
    account_bi = str((principal or {}).get("accountBi") or "")
    return bool(
        (principal or {}).get("canManage")
        or (account_bi and str(row.get("created_by_bi") or "") == account_bi)
        or (account_bi and str(row.get("assignee_bi") or "") == account_bi)
    )


async def _principal(runtime, org_bi, account_bi, role, permission):
    delegated = getattr(runtime, "delegated_principal", None)
    if callable(delegated):
        found = await delegated(org_bi)
        if found is not None:
            found = found if isinstance(found, dict) else {}
            account_bi = str(found.get("accountBi") or "")
            role = str(found.get("role") or "")
            permission = str(found.get("permission") or "")
            if not account_bi:
                role = permission = ""
    return {
        "accountBi": str(account_bi or ""),
        "canManage": _can_manage(role, permission),
        "owner": str(role or "") == "owner",
    }


def _delete_options(data):
    data = data if isinstance(data, dict) else {}
    force = data.get("force", False)
    if not isinstance(force, bool):
        return False, "", "invalid_force"
    reason = data.get("reason", "")
    if reason is None:
        reason = ""
    if not isinstance(reason, str):
        return False, "", "invalid_reason"
    return force, _multiline(reason, MAX_DELETE_REASON), ""


async def _delete_blockers(runtime, org_bi, row, data):
    blockers = []
    agent = data.get("agent") if isinstance(data.get("agent"), dict) else {}
    run_status = str(agent.get("status") or "").strip().lower()
    running = (
        int(row.get("completed_at") or 0) <= 0
        and run_status in LIVE_AGENT_RUN_STATES
    )
    session_id = str(row.get("agent_session_id") or "")
    if not running and session_id:
        checker = getattr(runtime, "agent_session_running", None)
        if callable(checker):
            try:
                running = bool(await checker(org_bi, session_id))
            except Exception:
                running = True
    if running:
        blockers.append("agent_running")
    if (
        int(row.get("qa_requested_at") or 0) > 0
        and int(row.get("qa_reviewed_at") or 0) <= 0
    ):
        blockers.append("qa_in_progress")
    return blockers


async def _trash_check(runtime, org_bi, principal, row, force):
    task_id = str(row.get("task_id") or "")
    try:
        data = await runtime.open(row.get("data"))
    except Exception:
        data = None
    if not isinstance(data, dict):
        return {"id": task_id, "error": "task_unavailable"}, 500, None, []
    if not _can_trash(row, principal):
        return {"id": task_id, "error": "forbidden"}, 403, None, []
    blockers = await _delete_blockers(runtime, org_bi, row, data)
    if blockers and not force:
        return {
            "id": task_id,
            "error": "task_delete_blocked",
            "blockedBy": blockers,
            "forceRequires": "owner",
            "canForce": bool(principal.get("owner")),
        }, 409, None, blockers
    if blockers and not principal.get("owner"):
        return {
            "id": task_id,
            "error": "force_requires_owner",
            "blockedBy": blockers,
        }, 403, None, blockers
    return None, 200, data, blockers


def _request_context(runtime, actor, method, path, now):
    reader = getattr(runtime, "request_context", None)
    try:
        found = reader() if callable(reader) else {}
    except Exception:
        found = {}
    found = found if isinstance(found, dict) else {}
    context = {"method": method, "path": path, "user": actor, "at": int(now)}
    for key in ("via", "node", "bot", "agent", "ip", "userAgent", "client"):
        context[key] = _text(found.get(key), 200)
    return context


async def _refuse_delete(
        runtime, actor, refusal, status, method, path, now):
    context = _request_context(runtime, actor, method, path, now)
    failures = refusal.get("failures") or [refusal]
    for failure in failures:
        blocked = failure.get("blockedBy") or []
        await runtime.audit(
            actor,
            "organization.task_delete",
            "organization_task",
            str(failure.get("id") or ""),
            outcome="denied",
            details={
                "state": "refused",
                "error": str(failure.get("error") or ""),
                "blockedBy": ",".join(blocked),
                "force": bool(refusal.get("forced")),
                "batch": method == "POST",
                "via": context["via"],
                "node": context["node"],
                "agent": context["agent"],
                "client": context["client"],
            },
        )
        print(
            "task delete refused: task %s error %s blocked %s user %s via %s "
            "node %s bot %s agent %s from %s ua %s" % (
                str(failure.get("id") or "")[:8],
                failure.get("error") or "-",
                ",".join(blocked) or "-",
                actor or "-",
                context["via"] or "-",
                context["node"] or "-",
                context["bot"] or "-",
                context["agent"] or "-",
                context["ip"] or "-",
                context["userAgent"] or "-",
            ))
    body = {key: value for key, value in refusal.items() if key != "forced"}
    body["request"] = context
    return _response(runtime, body, status=status)


async def _apply_trash(
        runtime, org_bi, account_bi, actor, row, data, blockers, force,
        reason, now, batch=False):
    task_id = str(row.get("task_id") or "")
    participants = await _participants(runtime, org_bi, task_id)
    items = _work_items(data.get("workItems"))[0]
    sealed = dict(data)
    sealed["trash"] = {"by": _text(actor, 64).lower(), "reason": reason,
                       "at": int(now)}
    elapsed_ms = max(0, _integer(row.get("elapsed_ms")))
    started_at = max(0, _integer(row.get("started_at")))
    if str(row.get("status") or "") == "active" and started_at:
        elapsed_ms = min(
            MAX_ELAPSED_MS, elapsed_ms + max(0, int(now) - started_at))
    await runtime.d1_run(
        "UPDATE organization_tasks SET deleted_at=?,deleted_by_bi=?,"
        "status=CASE WHEN status='active' THEN 'idle' ELSE status END,"
        "active_assignee_bi='',started_at=0,next_checkin_at=0,"
        "elapsed_ms=?,data=?,updated_at=? "
        "WHERE org_bi=? AND task_id=? AND deleted_at=0",
        int(now),
        str(account_bi or ""),
        elapsed_ms,
        await runtime.seal(sealed),
        int(now),
        org_bi,
        task_id,
    )
    trashed = await _task(runtime, org_bi, task_id, trashed=True)
    if not trashed:
        return None
    overridden = list(blockers) if force else []
    session_id = str(row.get("agent_session_id") or "")
    cancel = getattr(runtime, "cancel_agent_session", None)
    if "agent_running" in overridden and session_id and callable(cancel):
        try:
            await cancel(org_bi, session_id)
        except Exception:
            pass
    await runtime.audit(
        actor,
        "organization.task_delete",
        "organization_task",
        task_id,
        details={
            "state": "trashed",
            "force": bool(overridden),
            "overrode": overridden,
            "batch": bool(batch),
            "hasReason": bool(reason),
            "workItems": len(items),
        },
    )
    await _record_event(
        runtime, org_bi, task_id, account_bi, actor, "deleted", now,
        {"summary": reason or "moved to trash"})
    await _notify_activity(
        runtime, org_bi, actor, task_id, row, now, "deleted",
        participants=participants,
    )
    deleted_at = _integer(trashed.get("deleted_at"), int(now))
    return {
        "id": task_id,
        "deletedAt": deleted_at,
        "purgeAt": deleted_at + TRASH_RETENTION_MS,
        "workItems": items,
    }


async def _delete(
        runtime, org_bi, account_bi, actor, row, data, principal, now):
    force, reason, error = _delete_options(data)
    if error:
        return _response(runtime, {"error": error}, status=400)
    refusal, status, current, blockers = await _trash_check(
        runtime, org_bi, principal, row, force)
    if refusal:
        task_id = str(row.get("task_id") or "")
        return await _refuse_delete(
            runtime, actor, {**refusal, "forced": force}, status,
            "DELETE", "/api/tasks/" + task_id, now)
    result = await _apply_trash(
        runtime, org_bi, account_bi, actor, row, current, blockers, force,
        reason, now)
    if result is None:
        return _response(runtime, {"error": "task_not_found"}, status=404)
    return _response(runtime, {
        "ok": True,
        "deleted": True,
        "trashed": True,
        "restorable": True,
        **result,
    })


async def _delete_batch(
        runtime, org_bi, account_bi, actor, data, principal, now):
    raw_ids = data.get("ids", data.get("taskIds"))
    if not isinstance(raw_ids, list) or not raw_ids:
        return _response(runtime, {"error": "invalid_task_ids"}, status=400)
    if len(raw_ids) > MAX_DELETE_BATCH:
        return _response(runtime, {
            "error": "too_many_task_ids", "max": MAX_DELETE_BATCH,
        }, status=400)
    ids = []
    for value in raw_ids:
        if not isinstance(value, str) or not valid_id(value):
            return _response(
                runtime, {"error": "invalid_task_ids"}, status=400)
        if value.lower() not in ids:
            ids.append(value.lower())
    force, reason, error = _delete_options(data)
    if error:
        return _response(runtime, {"error": error}, status=400)
    prepared = []
    failures = []
    statuses = set()
    for task_id in ids:
        row = await _task(runtime, org_bi, task_id)
        if not row:
            failures.append({"id": task_id, "error": "task_not_found"})
            statuses.add(404)
            continue
        refusal, status, current, blockers = await _trash_check(
            runtime, org_bi, principal, row, force)
        if refusal:
            failures.append(refusal)
            statuses.add(status)
            continue
        prepared.append((row, current, blockers))
    if failures:
        status = (
            403 if 403 in statuses
            else 404 if 404 in statuses
            else 409 if 409 in statuses
            else 500
        )
        return await _refuse_delete(runtime, actor, {
            "error": "task_delete_batch_refused",
            "failures": failures,
            "deleted": [],
            "forced": force,
        }, status, "POST", "/api/tasks/delete", now)
    deleted = []
    work_items = {}
    for row, current, blockers in prepared:
        result = await _apply_trash(
            runtime, org_bi, account_bi, actor, row, current, blockers,
            force, reason, now, batch=True)
        if result is not None:
            deleted.append(result["id"])
            if result["workItems"]:
                work_items[result["id"]] = result["workItems"]
    return _response(runtime, {
        "ok": True,
        "deleted": deleted,
        "count": len(deleted),
        "trashed": True,
        "restorable": True,
        "purgeAt": int(now) + TRASH_RETENTION_MS,
        "workItems": work_items,
    })


async def _restore(runtime, org_bi, account_bi, actor, task_id, principal,
                   now):
    row = await _task(runtime, org_bi, task_id, trashed=True)
    if not row:
        if await _task(runtime, org_bi, task_id):
            return _response(
                runtime, {"error": "task_not_deleted"}, status=409)
        return _response(runtime, {"error": "task_not_found"}, status=404)
    if not _can_trash(row, principal):
        return _response(runtime, {"error": "forbidden"}, status=403)
    if _integer(row.get("deleted_at")) + TRASH_RETENTION_MS <= int(now):
        return _response(runtime, {"error": "task_trash_expired"}, status=410)
    try:
        current = await runtime.open(row.get("data"))
    except Exception:
        current = None
    if not isinstance(current, dict):
        return _response(runtime, {"error": "task_unavailable"}, status=500)
    current.pop("trash", None)
    await runtime.d1_run(
        "UPDATE organization_tasks SET deleted_at=0,deleted_by_bi='',"
        "data=?,updated_at=? WHERE org_bi=? AND task_id=? AND deleted_at>0",
        await runtime.seal(current),
        int(now),
        org_bi,
        task_id,
    )
    restored = await _task(runtime, org_bi, task_id)
    if not restored:
        return _response(runtime, {"error": "task_not_found"}, status=404)
    await runtime.audit(
        actor,
        "organization.task_restore",
        "organization_task",
        task_id,
        details={
            "state": "restored",
            "trashedForMs": max(0, int(now) - _integer(row.get("deleted_at"))),
        },
    )
    await _record_event(
        runtime, org_bi, task_id, account_bi, actor, "updated", now,
        {"fields": ["restored from trash"]})
    return await _task_response(runtime, restored, now)


async def _purge_task_rows(runtime, org_bi, task_id, cutoff):
    row = await runtime.d1_first(
        "SELECT deleted_at FROM organization_tasks "
        "WHERE org_bi=? AND task_id=?",
        org_bi,
        task_id,
    )
    deleted_at = _integer((row or {}).get("deleted_at"))
    if not row or deleted_at <= 0 or deleted_at > int(cutoff):
        return False
    for table in (
            "organization_task_attachments",
            "organization_task_checkins",
            "organization_task_qa_reviews",
            "organization_task_responses",
    ):
        await runtime.d1_run(
            "DELETE FROM " + table + " WHERE org_bi=? AND task_id=?",
            org_bi,
            task_id,
        )
    for table in (
            "organization_task_events",
            "world_office_marketing_checkins",
            "world_office_marketing_tasks",
    ):
        try:
            await runtime.d1_run(
                "DELETE FROM " + table + " WHERE org_bi=? AND task_id=?",
                org_bi,
                task_id,
            )
        except Exception:
            pass
    await runtime.d1_run(
        "DELETE FROM organization_tasks "
        "WHERE org_bi=? AND task_id=? AND deleted_at>0 AND deleted_at<=?",
        org_bi,
        task_id,
        int(cutoff),
    )
    return True


async def purge_trash(runtime, now, limit=TRASH_PURGE_BATCH):
    cutoff = int(now) - TRASH_RETENTION_MS
    bound = max(1, min(TRASH_PURGE_BATCH, _integer(limit, TRASH_PURGE_BATCH)))
    rows = await runtime.d1_all(
        "SELECT org_bi,task_id FROM organization_tasks "
        "WHERE deleted_at>0 AND deleted_at<=? "
        "ORDER BY deleted_at,task_id LIMIT ?",
        cutoff,
        bound,
    )
    purged = 0
    for row in rows or []:
        task_id = str(row.get("task_id") or "").lower()
        if not valid_id(task_id):
            continue
        if await _purge_task_rows(
                runtime, str(row.get("org_bi") or ""), task_id, cutoff):
            purged += 1
    return {"purged": purged, "limit": bound, "cutoff": cutoff}


async def _attachment(runtime, org_bi, task_id, attachment_id):
    row = await runtime.d1_first(
        "SELECT data FROM organization_task_attachments "
        "WHERE org_bi=? AND task_id=? AND attachment_id=?",
        org_bi,
        task_id,
        attachment_id,
    )
    if not row:
        return _response(runtime, {"error": "task_image_not_found"}, status=404)
    record = await runtime.open(row.get("data"))
    if not isinstance(record, dict):
        return _response(
            runtime, {"error": "task_image_unavailable"}, status=500)
    uploads, error = _task_image_uploads([record])
    if error or len(uploads) != 1:
        return _response(
            runtime, {"error": "task_image_unavailable"}, status=500)
    upload = uploads[0]
    return runtime.binary_response(
        base64.b64decode(upload["file"]),
        upload["mime"],
        upload["name"],
    )


async def _stop_active(runtime, org_bi, account_bi, actor, now):
    """Idempotently stop this account's active timer using server time."""
    row = await runtime.d1_first(
        "SELECT task_id,elapsed_ms,started_at "
        "FROM organization_tasks "
        "WHERE org_bi=? AND active_assignee_bi=? AND status='active' "
        "LIMIT 1",
        org_bi,
        account_bi,
    )
    if not row:
        return _response(runtime, {
            "ok": True,
            "stopped": False,
            "serverNow": int(now),
        })
    started_at = int(row.get("started_at") or 0)
    addition = max(0, int(now) - started_at) if started_at else 0
    elapsed = min(
        MAX_ELAPSED_MS,
        max(0, int(row.get("elapsed_ms") or 0)) + addition,
    )
    task_id = str(row.get("task_id") or "")
    await runtime.d1_run(
        "UPDATE organization_tasks SET "
        "status='idle',active_assignee_bi='',elapsed_ms=?,started_at=0,"
        "next_checkin_at=0,updated_at=? "
        "WHERE org_bi=? AND task_id=? AND active_assignee_bi=? "
        "AND status='active'",
        elapsed,
        now,
        org_bi,
        task_id,
        account_bi,
    )
    changed = await runtime.d1_first(
        "SELECT status FROM organization_tasks "
        "WHERE org_bi=? AND task_id=?",
        org_bi,
        task_id,
    )
    stopped = bool(changed and changed.get("status") == "idle")
    if stopped:
        await runtime.audit(
            actor,
            "office.marketing_task_stopped",
            "office_task",
            task_id,
            details={"state": "idle", "source": "world-exit"},
        )
        await _notify_activity(
            runtime, org_bi, actor, task_id,
            await _task(runtime, org_bi, task_id), now, "stopped")
    return _response(runtime, {
        "ok": True,
        "stopped": stopped,
        "serverNow": int(now),
    })


async def _checkin(
        runtime, org_bi, account_bi, actor, task_id, row, data, now):
    if str(row.get("assignee_bi") or "") != account_bi:
        return _response(runtime, {"error": "assignee_only"}, status=403)
    if (
        str(row.get("status") or "") != "active"
        or str(row.get("active_assignee_bi") or "") != account_bi
    ):
        return _response(runtime, {"error": "task_not_active"}, status=409)
    state = str(data.get("state") or "").strip().lower()
    if state not in CHECKIN_STATES:
        return _response(runtime, {"error": "invalid_checkin_state"}, status=400)
    note = _text(data.get("note"), MAX_CHECKIN_NOTE)
    checkin_id = runtime.new_id()
    if not valid_id(checkin_id):
        return _response(runtime, {"error": "id_generation_failed"}, status=500)
    # Retain a small per-task history; the migration's global trigger is the
    # final race-safe guard against an unbounded table.
    await runtime.d1_run(
        "DELETE FROM organization_task_checkins WHERE checkin_id IN ("
        "SELECT checkin_id FROM organization_task_checkins "
        "WHERE task_id=? ORDER BY created_at DESC,checkin_id DESC "
        "LIMIT -1 OFFSET ?)",
        task_id,
        MAX_CHECKINS_PER_TASK - 1,
    )
    try:
        await runtime.d1_run(
            "INSERT INTO organization_task_checkins "
            "(checkin_id,task_id,org_bi,account_bi,state,data,created_at) "
            "SELECT ?,?,?,?,?,?,? "
            "FROM organization_tasks "
            "WHERE org_bi=? AND task_id=? AND status='active' "
            "AND assignee_bi=? AND active_assignee_bi=?",
            checkin_id,
            task_id,
            org_bi,
            account_bi,
            state,
            await runtime.seal({"note": note}),
            now,
            org_bi,
            task_id,
            account_bi,
            account_bi,
        )
    except Exception as error:
        if "catalog_full" in str(error):
            return _response(
                runtime, {"error": "checkin_capacity_reached"}, status=409)
        raise
    inserted = await runtime.d1_first(
        "SELECT checkin_id FROM organization_task_checkins "
        "WHERE checkin_id=?",
        checkin_id,
    )
    if not inserted:
        return _response(runtime, {"error": "task_state_conflict"}, status=409)
    deadline = _checkin_deadline(runtime, now)
    await runtime.d1_run(
        "UPDATE organization_tasks "
        "SET next_checkin_at=?,updated_at=? "
        "WHERE org_bi=? AND task_id=? AND status='active' "
        "AND active_assignee_bi=?",
        deadline,
        now,
        org_bi,
        task_id,
        account_bi,
    )
    await runtime.audit(
        actor,
        "office.marketing_task_checkin",
        "office_task",
        task_id,
        details={"state": state},
    )
    changed = await _task(runtime, org_bi, task_id)
    mentioned = _mentions(note)
    await _record_event(
        runtime, org_bi, task_id, account_bi, actor, "checkin", now,
        detail={"state": state, "note": note, "mentions": mentioned},
    )
    # The check-in button is how an assignee says "still on it" out loud, so it
    # pings the floor the same way starting or finishing the task does.
    await _notify_activity(
        runtime, org_bi, actor, task_id, changed, now,
        "checked in " + CHECKIN_STATE_COPY.get(state, state),
        changes=[{
            "field": "checkin",
            "state": CHECKIN_STATE_COPY.get(state, state),
            "excerpt": note,
        }],
        skip=mentioned,
    )
    if mentioned:
        projected = await _project_task(runtime, changed, now)
        await _notify_mentions(
            runtime, org_bi, actor, task_id, projected, mentioned,
            "check-in", note, now,
        )
    return await _task_response(runtime, changed, now)


async def _set_persistent_reminder(
        runtime, org_bi, account_bi, actor, task_id, row, value, can_manage,
        now):
    if not _can_edit_progress(row, account_bi, can_manage):
        return _response(runtime, {"error": "forbidden"}, status=403)
    every_ms, error = _persistent_reminder_every_ms(value)
    if error or every_ms is None:
        return _response(
            runtime, {"error": error or "invalid_persistent_reminder"},
            status=400)
    previous_ms = max(0, _integer(row.get("reminder_every_ms")))
    await runtime.d1_run(
        "UPDATE organization_tasks SET reminder_every_ms=?,"
        "reminder_next_at=?,updated_at=? WHERE org_bi=? AND task_id=?",
        every_ms,
        now + every_ms if every_ms else 0,
        now,
        org_bi,
        task_id,
    )
    await runtime.audit(
        actor, "organization.task_persistent_reminder_changed",
        "organization_task", task_id,
        details={"everyMinutes": every_ms // 60000,
                 "previous": previous_ms // 60000},
    )
    if every_ms != previous_ms:
        await _record_event(
            runtime, org_bi, task_id, account_bi, actor, "updated", now,
            detail={"fields": [
                "persistent reminder every %d min" % (every_ms // 60000)
                if every_ms else "persistent reminder off"]},
        )
    return await _task_response(
        runtime, await _task(runtime, org_bi, task_id), now)


def _persistent_reminder_recipient(row, data):
    if (
        str(row.get("assignee_kind") or "user") == "user"
        and str(row.get("assignee_bi") or "")
    ):
        recipient = _text(data.get("assignee"), 64).lower()
        if recipient:
            return recipient
    return _text(data.get("createdBy"), 64).lower()


async def deliver_persistent_reminders(
        runtime, now, limit=PERSISTENT_REMINDER_BATCH):
    bound = max(1, min(PERSISTENT_REMINDER_BATCH,
                       _integer(limit, PERSISTENT_REMINDER_BATCH)))
    rows = await runtime.d1_all(
        "SELECT * FROM organization_tasks "
        "WHERE reminder_next_at>0 AND reminder_next_at<=? "
        "AND reminder_every_ms>0 AND completed_at=0 AND deleted_at=0 "
        "AND status!='done' ORDER BY reminder_next_at LIMIT ?",
        int(now),
        bound,
    )
    notifier = getattr(
        runtime, "notify_organization_task_persistent_reminder", None)
    sent = 0
    for row in rows or []:
        org_bi = str(row.get("org_bi") or "")
        task_id = str(row.get("task_id") or "").lower()
        due_at = max(0, _integer(row.get("reminder_next_at")))
        every_ms = max(0, _integer(row.get("reminder_every_ms")))
        if not valid_id(task_id) or not every_ms:
            continue
        claimed = await runtime.d1_run(
            "UPDATE organization_tasks SET reminder_next_at=? "
            "WHERE org_bi=? AND task_id=? AND reminder_next_at=?",
            int(now) + every_ms,
            org_bi,
            task_id,
            due_at,
        )
        if _changed_rows(claimed) == 0:
            continue
        try:
            data = await runtime.open(row.get("data"))
        except Exception:
            data = None
        if not isinstance(data, dict) or not callable(notifier):
            continue
        recipient = _persistent_reminder_recipient(row, data)
        if not recipient:
            continue
        projected = await _project_task(runtime, row, now)
        if not projected:
            continue
        try:
            if await notifier(
                    org_bi, task_id, projected, recipient,
                    every_ms // 60000, int(now)):
                sent += 1
            else:
                await _log_reminder_failure(
                    runtime, task_id, "persistent_ping_not_filed")
        except Exception as failure:
            await _log_reminder_failure(runtime, task_id, repr(failure))
    return {"due": len(rows or []), "sent": sent}


async def _remind(
        runtime, org_bi, account_bi, actor, task_id, row, data, now):
    if (
        int(row.get("completed_at") or 0) > 0
        or str(row.get("status") or "") == "done"
    ):
        return _response(runtime, {"error": "task_done"}, status=409)
    assignee_bi = str(row.get("assignee_bi") or "")
    if (
        str(row.get("assignee_kind") or "user") != "user"
        or not assignee_bi
    ):
        return _response(
            runtime, {"error": "task_has_no_assignee"}, status=409)
    if assignee_bi == account_bi:
        return _response(
            runtime, {"error": "cannot_remind_yourself"}, status=409)
    try:
        current = await runtime.open(row.get("data"))
    except Exception:
        current = None
    if not isinstance(current, dict):
        return _response(runtime, {"error": "task_unavailable"}, status=500)
    assignee = _text(current.get("assignee"), 64).lower()
    if not assignee:
        return _response(
            runtime, {"error": "task_has_no_assignee"}, status=409)
    previous = _last_reminder(current.get("reminder"))
    if previous and int(now) - previous["at"] < REMINDER_COOLDOWN_MS:
        return _response(runtime, {
            "error": "reminder_too_soon",
            "retryAt": previous["at"] + REMINDER_COOLDOWN_MS,
            "lastReminder": previous,
        }, status=429)
    note = _text(data.get("note"), MAX_REMINDER_NOTE)
    current["reminder"] = {
        "at": int(now), "by": actor, "to": assignee, "note": note}
    sealed = await runtime.seal(current)
    await runtime.d1_run(
        "UPDATE organization_tasks SET data=? "
        "WHERE org_bi=? AND task_id=? AND data=? AND completed_at=0",
        sealed,
        org_bi,
        task_id,
        row.get("data"),
    )
    changed = await _task(runtime, org_bi, task_id)
    if not changed or changed.get("data") != sealed:
        return _response(runtime, {"error": "task_state_conflict"}, status=409)
    await runtime.audit(
        actor,
        "organization.task_reminder_sent",
        "organization_task",
        task_id,
        details={"noteLength": len(note)},
    )
    projected = await _project_task(runtime, changed, now)
    delivery = await _notify_reminder(
        runtime, org_bi, actor, task_id, projected, assignee, note, now)
    await _record_event(
        runtime, org_bi, task_id, account_bi, actor, "reminded", now, {
            "summary": _reminder_event_summary(assignee, note, delivery),
            "mentions": [assignee],
        })
    current["reminder"] = dict(current["reminder"], delivery=delivery)
    delivered = await runtime.seal(current)
    await runtime.d1_run(
        "UPDATE organization_tasks SET data=? "
        "WHERE org_bi=? AND task_id=? AND data=?",
        delivered,
        org_bi,
        task_id,
        sealed,
    )
    final = await _task(runtime, org_bi, task_id)
    return await _task_response(
        runtime, final or changed, now,
        extra=None if delivery.get("pinged") else {
            "reminderError": delivery.get("error") or "ping_not_filed"})


def _reminder_event_summary(assignee, note, delivery):
    if not delivery.get("pinged"):
        head = "Reminder to @%s could not be delivered" % assignee
    else:
        phones = max(0, _integer(delivery.get("phones")))
        head = "Reminded @%s — ping sent%s" % (
            assignee,
            ", pushed to %d phone%s" % (phones, "" if phones == 1 else "s")
            if phones else "")
    note = _text(note, MAX_EVENT_SUMMARY)
    return _text(
        "%s: “%s”" % (head, note) if note else head, MAX_EVENT_SUMMARY)


async def handle(runtime, path):
    route = _route(path)
    if route is None:
        return _response(runtime, {"error": "not_found"}, status=404)
    method = runtime.method()
    marketing_only = str(path or "").rstrip("/").startswith(
        LEGACY_MARKETING_PREFIX)
    allowed = (
        ("GET", "POST")
        if route[0] in ("collection", "proofs", "initiatives")
        else ("GET",)
        if route[0] == "attachment"
        else ("POST",)
        if route[0] == "stop-active"
        else ("GET", "PATCH", "DELETE")
        if route[0] == "task"
        else ("GET", "POST")
        if route[0] == "replies"
        else ("GET",)
        if route[0] == "history"
        else ("POST",)
    )
    if method not in allowed:
        return _response(
            runtime,
            {"error": "method_not_allowed"},
            status=405,
            allow=", ".join(allowed),
        )
    if method != "GET" and not runtime.same_origin():
        return _response(runtime, {"error": "origin_not_allowed"}, status=403)

    data = {}
    if method != "GET":
        data, error_response = await _body(runtime)
        if error_response is not None:
            return error_response

    await runtime.ensure_schema()
    account_bi, actor = await _actor(runtime, data)
    if not account_bi or not actor:
        return _response(runtime, {"error": "invalid_session"}, status=401)
    org_bi, organization = await runtime.organization()
    if not org_bi or not organization:
        return _response(
            runtime, {"error": "office_organization_unavailable"}, status=503)
    role, permission = await runtime.membership(org_bi, actor)
    can_manage = _can_manage(role, permission)
    now = int(runtime.now())

    kind, task_id, action = route
    if not role:
        # Task titles and routing are organization-private. A valid ForkMesh
        # account without current membership receives no existence signal.
        return _response(runtime, {"error": "org_member_required"}, status=403)
    if kind == "initiatives":
        if not marketing_only:
            return _response(runtime, {"error": "not_found"}, status=404)
        marketing_actor = await runtime.marketing_member(org_bi, actor)
        if method == "GET":
            if not marketing_actor:
                return _response(
                    runtime, {"error": "marketing_team_only"}, status=403)
            return _response(runtime, {
                "ok": True,
                "actor": actor,
                "initiatives": await _initiative_records(runtime, org_bi),
            })
        return await _initiative_create(
            runtime, org_bi, account_bi, actor, data, can_manage, now)
    if kind == "proofs":
        if not marketing_only:
            return _response(runtime, {"error": "not_found"}, status=404)
        if not await runtime.marketing_member(org_bi, actor):
            return _response(
                runtime, {"error": "marketing_team_only"}, status=403)
        if method == "POST":
            return await _proof_create(
                runtime, org_bi, account_bi, actor, data, now)
        return _response(runtime, {
            "ok": True,
            "actor": actor,
            "proofs": await _proof_records(runtime, org_bi),
        })
    if kind == "stop-active":
        return await _stop_active(
            runtime, org_bi, account_bi, actor, now)
    if kind == "agent-status-batch":
        return await _update_agent_status_batch(
            runtime, org_bi, account_bi, actor, data, can_manage, now,
            marketing_only=marketing_only,
        )
    if kind == "delete-batch":
        if marketing_only:
            return _response(runtime, {"error": "not_found"}, status=404)
        return await _delete_batch(
            runtime, org_bi, account_bi, actor, data,
            await _principal(runtime, org_bi, account_bi, role, permission),
            now)
    if kind == "action" and action == "restore":
        if marketing_only:
            return _response(runtime, {"error": "not_found"}, status=404)
        return await _restore(
            runtime, org_bi, account_bi, actor, task_id,
            await _principal(runtime, org_bi, account_bi, role, permission),
            now)
    if kind == "collection":
        if method == "GET":
            department = ""
            team = ""
            status_filter = _text(runtime.query("status"), 16).lower()
            trash = status_filter in TRASH_LIST_STATUSES
            if trash and marketing_only:
                return _response(
                    runtime, {"error": "invalid_task_filter"}, status=400)
            if not marketing_only:
                department = _text(
                    runtime.query("department"), 64).lower()
                team = _text(runtime.query("team"), 64).lower()
                if (
                    (department and (
                        department not in DEPARTMENTS
                        or not _SCOPE_RE.fullmatch(department)
                    ))
                    or (team and not _SCOPE_RE.fullmatch(team))
                ):
                    return _response(
                        runtime, {"error": "invalid_task_filter"}, status=400)
            return await _list(
                runtime, org_bi, account_bi, actor, can_manage, now,
                marketing_only=marketing_only,
                department=department,
                team=team,
                trash=trash,
            )
        return await _create(
            runtime, org_bi, account_bi, actor, data, can_manage, now,
            marketing_only=marketing_only,
        )

    row = await _task(runtime, org_bi, task_id)
    if (
        not row
        or (
            marketing_only
            and str(row.get("department") or "") != "marketing"
        )
    ):
        return _response(runtime, {"error": "task_not_found"}, status=404)
    if kind != "attachment":
        row = await _observe_reminder(
            runtime, org_bi, account_bi, actor, row, now,
            check_ping=method == "GET" and kind in ("task", "history"))
    if (
        method == "GET"
        and kind in ("task", "history", "replies")
        and not marketing_only
    ):
        row = await _observe_comment_read(
            runtime, org_bi, account_bi, actor, row, now)
    if kind == "replies":
        if (
            marketing_only
            and not can_manage
            and str(row.get("assignee_bi") or "") != account_bi
            and not await runtime.marketing_member(org_bi, actor)
        ):
            return _response(runtime, {"error": "task_not_found"}, status=404)
        if method == "GET":
            return await _task_response(runtime, row, now)
        return await _reply(
            runtime, org_bi, account_bi, actor, task_id, row, data, now)
    if kind == "history":
        if (
            marketing_only
            and not can_manage
            and str(row.get("assignee_bi") or "") != account_bi
            and not await runtime.marketing_member(org_bi, actor)
        ):
            return _response(runtime, {"error": "task_not_found"}, status=404)
        return _response(runtime, {
            "ok": True,
            "actor": actor,
            "id": task_id,
            "history": await _task_history(runtime, org_bi, task_id),
            "replies": await _task_replies_with_receipts(
                runtime, org_bi, task_id, row),
        })
    if kind == "attachment":
        return await _attachment(runtime, org_bi, task_id, action)
    if kind == "task":
        if method == "GET":
            if (
                marketing_only
                and not can_manage
                and str(row.get("assignee_bi") or "") != account_bi
            ):
                return _response(
                    runtime, {"error": "task_not_found"}, status=404)
            await _observe_email_view(runtime, org_bi, task_id, actor, now)
            return await _task_response(runtime, row, now)
        if method == "DELETE":
            return await _delete(
                runtime, org_bi, account_bi, actor, row, data,
                await _principal(
                    runtime, org_bi, account_bi, role, permission),
                now)
        return await _update(
            runtime, org_bi, account_bi, actor, task_id, data, can_manage, now,
            marketing_only=marketing_only,
        )
    if action == "agent-status":
        return await _update_agent_status(
            runtime, org_bi, account_bi, actor, task_id, row, data,
            can_manage, now,
        )
    if action == "start":
        return await _start(
            runtime, org_bi, account_bi, actor, task_id, row, now)
    if action == "stop":
        return await _stop(
            runtime, org_bi, account_bi, actor, task_id, row, now)
    if action == "complete":
        return await _complete(
            runtime, org_bi, account_bi, actor, task_id, row, data,
            can_manage, now)
    if action == "return":
        return await _return_to_list(
            runtime, org_bi, account_bi, actor, task_id, row, can_manage, now)
    if action == "qa":
        return await _request_qa(
            runtime, org_bi, account_bi, actor, task_id, row, data,
            can_manage, now)
    if action == "undo":
        return await _undo(
            runtime, org_bi, account_bi, actor, task_id, row, can_manage, now)
    if action == "remind":
        if (
            marketing_only
            and not can_manage
            and not await runtime.marketing_member(org_bi, actor)
        ):
            return _response(runtime, {"error": "task_not_found"}, status=404)
        return await _remind(
            runtime, org_bi, account_bi, actor, task_id, row, data, now)
    return await _checkin(
        runtime, org_bi, account_bi, actor, task_id, row, data, now)
