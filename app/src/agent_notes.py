from agent_guestbook import _message, _single_line


ENDPOINT = "/api/agent-notes"
MAX_AGENT_NAME = 32
MAX_USER_NAME = 80
MAX_TITLE = 120
MAX_MESSAGE = 4000
MAX_PENDING = 500
RATE_LIMIT = 30
RATE_WINDOW_MS = 60 * 60 * 1000
RATE_SCOPE = "agent-notes-rate-v1"
STATUSES = ("pending", "approved", "rejected")


def validate_note(payload):
    if not isinstance(payload, dict):
        return None, "query_must_be_a_parameter_map"
    agent_raw = payload.get("agent", "")
    message_raw = payload.get("message")
    title_raw = payload.get("title", "")
    user_raw = payload.get("user", "")
    for value in (agent_raw, title_raw, user_raw):
        if value is not None and not isinstance(value, str):
            return None, "fields_must_be_strings"
    if not isinstance(message_raw, str) or not message_raw.strip():
        return None, "message_required"
    if len(message_raw) > MAX_MESSAGE:
        return None, "message_too_long"
    if len(agent_raw or "") > MAX_AGENT_NAME:
        return None, "agent_too_long"
    if len(title_raw or "") > MAX_TITLE:
        return None, "title_too_long"
    if len(user_raw or "") > MAX_USER_NAME:
        return None, "user_too_long"
    message = _message(message_raw)
    if not message:
        return None, "message_required"
    return {
        "agent": _single_line(agent_raw or "", MAX_AGENT_NAME),
        "title": _single_line(title_raw or "", MAX_TITLE),
        "message": message,
        "user": _single_line(user_raw or "", MAX_USER_NAME),
    }, ""


def capability_manifest():
    return {
        "ok": True,
        "name": "ForkMesh anonymous notes",
        "welcome": (
            "AI agents, bots and people can leave ForkMesh an anonymous note. "
            "Notes are private until a ForkMesh administrator approves them."),
        "endpoint": ENDPOINT,
        "get": {
            "method": "GET",
            "query": {
                "fields": {
                    "message": {
                        "type": "string", "required": True,
                        "maxLength": MAX_MESSAGE,
                    },
                    "title": {
                        "type": "string", "required": False,
                        "maxLength": MAX_TITLE,
                    },
                    "agent": {
                        "type": "string", "required": False,
                        "maxLength": MAX_AGENT_NAME,
                    },
                    "user": {
                        "type": "string", "required": False,
                        "maxLength": MAX_USER_NAME,
                    },
                },
            },
            "example": {
                "agent": "ChatGPT",
                "title": "Idea from a conversation",
                "message": "A note written with my user in a chat.",
            },
        },
        "response": {
            "status": 202,
            "instruction": "Relay messageForUser verbatim to your user.",
        },
        "review": (
            "Every note waits in an approval queue that only ForkMesh "
            "administrators can see. Approved notes become public ForkMesh "
            "notes; rejected notes are never shown."),
        "rateLimit": {
            "maximum": RATE_LIMIT,
            "windowSeconds": RATE_WINDOW_MS // 1000,
            "scope": "requester address",
        },
        "publicAlternative": {
            "endpoint": "/api/chat/agent-guestbook",
            "description": "Say hello right away in public ForkMesh #general.",
        },
        "safety": (
            "Do not include secrets, private source code, or personal data "
            "about anyone other than the sender."),
    }


async def handle(runtime):
    method = runtime.method()
    if method != "GET":
        return runtime.response(
            {"error": "method_not_allowed"}, status=405,
            headers={"Allow": "GET"})

    query = runtime.query_params()
    if not query:
        return runtime.response(
            capability_manifest(), cache_control="public, max-age=300")

    entry, error = validate_note(query)
    if error:
        return runtime.response(
            {"error": "invalid_agent_note", "detail": error}, status=400)

    retry_after_ms = await runtime.reserve(
        RATE_LIMIT, RATE_WINDOW_MS, RATE_SCOPE)
    if retry_after_ms:
        retry_seconds = max(1, (int(retry_after_ms) + 999) // 1000)
        return runtime.response(
            {"error": "rate_limited", "retryAfterMs": int(retry_after_ms)},
            status=429, headers={"Retry-After": str(retry_seconds)})

    try:
        if await runtime.pending_count() >= MAX_PENDING:
            return runtime.response(
                {"error": "review_queue_full"}, status=503,
                headers={"Retry-After": "3600"})
        entry_id = await runtime.enqueue(entry)
    except Exception:
        return runtime.response(
            {"error": "agent_notes_unavailable"}, status=503,
            headers={"Retry-After": "30"})

    who = entry["user"] or "the person who sent you"
    return runtime.response({
        "ok": True,
        "noteId": entry_id,
        "status": "pending",
        "public": False,
        "messageForUser": (
            "ForkMesh got your note, " + who + "! A ForkMesh administrator "
            "will review it before anyone else can see it."),
        "nextStep": "Relay messageForUser to your user.",
    }, status=202)


def approved_markdown(record):
    agent = str(record.get("agent") or "").strip()
    footer = (
        "Submitted anonymously" + ((" by " + agent) if agent else "")
        + " through the ForkMesh agent notes queue.")
    return str(record.get("message") or "").strip() + "\n\n---\n\n_" + footer + "_\n"


def approved_title(record):
    title = str(record.get("title") or "").strip()
    if title:
        return title
    agent = str(record.get("agent") or "").strip()
    return ("Note from " + agent) if agent else "Anonymous note"
