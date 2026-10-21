
import hashlib
import json
import re

import repository_file_edits as edits
from docs_book import OID_RE

PROJECTS_DIR = ".forkmesh/projects"
EVENT_TYPES = frozenset({
    "open", "title", "edit", "status", "dates", "milestone", "issues",
    "delete"})
MAX_EVENTS = 8
MAX_TITLE = 240
MAX_BODY_BYTES = 64 * 1024
MAX_ISSUES = 200
MAX_PROJECT_BYTES = 2 * 1024 * 1024
ID_RE = re.compile(r"^[A-Za-z0-9._:-]{1,80}$")


def _bind_runtime(runtime):
    edits._bind_runtime(runtime)
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def project_path(number):
    return "%s/%d/project-%d.json" % (PROJECTS_DIR, number, number)


def sorted_issues(values):
    return sorted({int(v) for v in values})


def content_for_signing(ev):
    t = ev.get("type")
    if t == "open":
        return str(ev.get("title", "")) + "\0" + str(ev.get("body", ""))
    if t == "title":
        return str(ev.get("title", ""))
    if t == "edit":
        return str(ev.get("body", ""))
    if t == "status":
        return str(ev.get("status", ""))
    if t == "dates":
        return "%d\0%d" % (int(ev.get("startDate") or 0),
                           int(ev.get("endDate") or 0))
    if t == "milestone":
        return str(ev.get("milestone", ""))
    if t == "issues":
        return ",".join(str(n) for n in sorted_issues(ev.get("issues") or []))
    if t == "delete":
        return str(ev.get("target", ""))
    return ""


def canonical_string(number, ev):
    digest = hashlib.sha256(content_for_signing(ev).encode("utf-8")).hexdigest()
    return ("forkmesh-project-event-v1\n%s\n%d\n%s\n%d\n%s" % (
        ev.get("type"), number, ev.get("author"), int(ev.get("ts")),
        digest)).encode("utf-8")


def clean_event(raw):
    if not isinstance(raw, dict):
        return None
    t = str(raw.get("type") or "")
    if t not in EVENT_TYPES:
        return None
    try:
        ts = int(raw.get("ts"))
    except (TypeError, ValueError):
        return None
    ev = {
        "type": t, "id": str(raw.get("id") or ""),
        "author": str(raw.get("author") or ""),
        "authorName": str(raw.get("authorName") or "")[:63],
        "ts": ts, "sig": str(raw.get("sig") or ""),
    }
    if not ID_RE.match(ev["id"]) or not ev["author"] or not ev["sig"]:
        return None
    if t in ("open", "title"):
        ev["title"] = str(raw.get("title") or "")
        if not ev["title"].strip() or len(ev["title"]) > MAX_TITLE:
            return None
    if t in ("open", "edit"):
        ev["body"] = str(raw.get("body") or "")
        if len(ev["body"].encode("utf-8")) > MAX_BODY_BYTES:
            return None
    if t == "status":
        ev["status"] = str(raw.get("status") or "")
        if ev["status"] not in ("open", "closed"):
            return None
    if t == "dates":
        try:
            ev["startDate"] = int(raw.get("startDate") or 0)
            ev["endDate"] = int(raw.get("endDate") or 0)
        except (TypeError, ValueError):
            return None
        if min(ev["startDate"], ev["endDate"]) < 0:
            return None
    if t == "milestone":
        ev["milestone"] = str(raw.get("milestone") or "")[:120]
    if t == "issues":
        values = raw.get("issues")
        if not isinstance(values, list) or len(values) > MAX_ISSUES:
            return None
        try:
            ev["issues"] = sorted_issues(values)
        except (TypeError, ValueError):
            return None
        if any(n <= 0 for n in ev["issues"]):
            return None
    if t == "delete":
        ev["target"] = str(raw.get("target") or "")
        if ev["target"] != "self":
            return None
    return ev


def recompute(project):
    updated = int(project.get("createdAt") or 0)
    for ev in project.get("events") or []:
        t = ev.get("type")
        if t == "open":
            project["title"] = ev.get("title", "")
            project["body"] = ev.get("body", "")
        elif t == "title" and ev.get("title"):
            project["title"] = ev["title"]
        elif t == "edit":
            project["body"] = ev.get("body", "")
        elif t == "status":
            project["status"] = ev.get("status", "open")
        elif t == "dates":
            project["startDate"] = int(ev.get("startDate") or 0)
            project["endDate"] = int(ev.get("endDate") or 0)
        elif t == "milestone":
            project["milestone"] = ev.get("milestone", "")
        elif t == "issues":
            project["issues"] = sorted_issues(ev.get("issues") or [])
        updated = max(updated, int(ev.get("ts") or 0))
    project["updatedAt"] = updated
    return project


def new_project(number, opening):
    return recompute({
        "schema": "forkmesh-project-v1", "number": number, "title": "",
        "body": "", "status": "open", "startDate": 0, "endDate": 0,
        "milestone": "", "issues": [], "createdAt": opening["ts"],
        "author": opening["author"], "authorName": opening["authorName"],
        "events": [opening],
    })


async def _next_number(env, owner, repo, branch):
    tree = await edits._tree(env, owner, repo, PROJECTS_DIR, branch)
    numbers = [int(e["name"]) for e in (tree or {}).get("entries") or []
               if e.get("type") == "tree" and str(e.get("name", "")).isdigit()]
    return max(numbers, default=0) + 1


async def _save(env, request, owner, repo):
    try:
        data = await bounded_json_request(
            request, max_bytes=edits.MAX_REQUEST_BYTES)
    except Exception:
        return edits._error("invalid_json", "The request body is not JSON.",
                            400)
    if not isinstance(data, dict):
        data = {}
    actor = await _authed_account_name(env, request, data)
    if not actor:
        return edits._error("unauthorized", "Sign in to edit projects.", 401)
    permission = await edits._permission(env, request, owner, repo, actor)
    if permission == "hidden":
        return edits._error("not_found", "That repository is not available.",
                            404)
    if permission != "write":
        return edits._error("forbidden",
                            "You need write access to edit projects.", 403)
    branch = str(data.get("branch") or "").strip()
    if not edits.valid_branch(branch):
        return edits._error("invalid_branch", "Choose a valid branch.", 400)
    try:
        number = int(data.get("number") or 0)
    except (TypeError, ValueError):
        number = 0
    raw_events = data.get("events")
    if (number <= 0 or not isinstance(raw_events, list) or not raw_events
            or len(raw_events) > MAX_EVENTS):
        return edits._error("invalid_request", "Nothing to save.", 400)
    events = [clean_event(raw) for raw in raw_events]
    if any(ev is None for ev in events):
        return edits._error("invalid_event", "A project event is invalid.",
                            400)
    for ev in events:
        if not await ed25519_verify(ev["author"], ev["sig"],
                                    canonical_string(number, ev)):
            return edits._error("bad_signature",
                                "A project event's signature is invalid.",
                                401)
        refused = await _bind_session_author(env, request, data, ev,
                                             session=actor)
        if refused is not None:
            return refused
        ev.pop("authorVerified", None)
    opening = events[0]["type"] == "open"
    if any(ev["type"] == "open" for ev in events[1:]):
        return edits._error("invalid_event", "Only one opening event.", 400)
    path = project_path(number)
    try:
        if opening:
            expected = await _next_number(env, owner, repo, branch)
            if number != expected:
                raise edits.FileEditError(
                    "number_taken",
                    "Another project took that number; sign it again.", 409)
            project = new_project(number, events[0])
            rest = events[1:]
        else:
            blob = await edits._blob(env, owner, repo, path, branch)
            try:
                project = json.loads(str((blob or {}).get("content") or ""))
            except ValueError:
                project = None
            if not isinstance(project, dict):
                raise edits.FileEditError(
                    "not_found", "That project is not in the repository.",
                    404)
            rest = events
        known = {str(ev.get("id")) for ev in project.get("events") or []}
        for ev in rest:
            if ev["id"] not in known:
                project.setdefault("events", []).append(ev)
                known.add(ev["id"])
        recompute(project)
        blob = (json.dumps(project, indent=4, sort_keys=True) + "\n").encode(
            "utf-8")
        if len(blob) > MAX_PROJECT_BYTES:
            raise edits.FileEditError(
                "too_large", "This project's history is too large.", 413)
        summary = ("Create project #%d" if opening else
                   "Update project #%d") % number
        commit = await edits.commit_paths(
            env, request, owner, repo, actor, branch, {path: blob},
            summary + ": " + project.get("title", "")[:80])
    except edits.FileEditError as exc:
        return edits._error(exc.code, exc.message, exc.status,
                            **({"next": await _next_number(
                                env, owner, repo, branch)}
                               if exc.code == "number_taken" else {}))
    try:
        await _audit_sensitive_action(
            env, actor, "repository.project_edit", "repository",
            owner + "/" + repo, "success",
            {"number": number, "events": [ev["type"] for ev in events]})
    except Exception:
        pass
    return json_response({"ok": True, "number": number, "commit": commit,
                          "project": project}, cache_control="no-store")


async def route(env, request, owner, repo, action):
    if action != "project-edit":
        return edits._error("not_found", "Not found.", 404)
    if method_name(request) != "POST":
        return edits._error("method_not_allowed", "Use POST.", 405)
    return await _save(env, request, owner, repo)
