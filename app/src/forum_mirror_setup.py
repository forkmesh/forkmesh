from __future__ import annotations

import json
import math
import random
import re
import urllib.robotparser
from urllib.parse import parse_qs, urlparse

import market_packages
import repository_file_edits as edits
import repository_webhooks

IMPORTER_PACKAGE = "discourse-mirror"
IMPORTER_ENTRYPOINT = "mirror"
FORUM_PATH = ".forkmesh/forum"
WORKFLOW_PATH = ".forkmesh/discourse-mirror.yml"
TOOL_PATH = FORUM_PATH + "/tool/discourse_mirror.py"
SETTINGS_PATH = FORUM_PATH + "/source/settings.json"
ROBOTS_AGENT = "ForkMeshDiscourseMirror"
USER_AGENT = ROBOTS_AGENT + "/1.0 (+https://forkmesh.com/market/discourse-mirror)"
CHECK_TIMEOUT_SECONDS = 15
MAX_CHECK_CHARS = 4 * 1024 * 1024
MAX_ORIGIN_LENGTH = 200
TOPICS_PER_RUN = 400
RUN_MINUTES = 45
UPLOAD_BYTES_PER_RUN = 100_000_000
PATH_RE = re.compile(r"^(/[A-Za-z0-9._~-]+){0,4}$")
SEGMENT_RE = re.compile(r"^[A-Za-z0-9_.-]{1,100}$")


class SetupError(Exception):
    def __init__(self, code, message, status):
        super().__init__(message)
        self.code = code
        self.message = message
        self.status = status


def _bind_runtime(runtime):
    edits._bind_runtime(runtime)
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def clean_origin(value):
    raw = str(value or "").strip()
    if not raw or len(raw) > MAX_ORIGIN_LENGTH:
        return ""
    if "://" not in raw:
        raw = "https://" + raw
    cleaned = repository_webhooks.clean_url(raw)
    if not cleaned:
        return ""
    parsed = urlparse(cleaned)
    path = parsed.path.rstrip("/")
    if parsed.query or not PATH_RE.match(path):
        return ""
    return "https://%s%s" % (parsed.netloc.lower(), path)


def first_copy_estimate(topics, posts, delay_seconds=1.0):
    topics = max(0, int(topics or 0))
    posts = max(0, int(posts or 0))
    requests = math.ceil(topics / 30) + topics + max(0, posts - 20 * topics) // 20
    return {"requests": requests, "hours": round(requests * delay_seconds / 3600, 1),
            "runs": max(1, math.ceil(topics / TOPICS_PER_RUN))}


def workflow_text(minute=None):
    minute = random.randrange(30) if minute is None else int(minute) % 30
    return (
        "name: Discourse mirror\n"
        "on:\n"
        "  schedule:\n"
        "    - cron: \"%d,%d * * * *\"\n"
        "  workflow_dispatch:\n"
        "permissions:\n"
        "  contents: write\n"
        "jobs:\n"
        "  mirror:\n"
        "    runs-on: runner\n"
        "    timeout-minutes: 60\n"
        "    steps:\n"
        "      - name: Copy new and changed topics\n"
        "        run: python3 %s ci --max-minutes %d\n"
        "      - name: Publish the copy\n"
        "        run: git push origin \"HEAD:$GITHUB_REF_NAME\""
        " || (git pull --rebase origin \"$GITHUB_REF_NAME\""
        " && git push origin \"HEAD:$GITHUB_REF_NAME\")\n"
    ) % (minute, minute + 30, TOOL_PATH, RUN_MINUTES)


def settings_record(origin, uploads, actor, now_ms):
    return {
        "schemaVersion": 1,
        "origin": origin,
        "uploads": bool(uploads),
        "maxTopicsPerRun": TOPICS_PER_RUN,
        "maxUploadBytesPerRun": UPLOAD_BYTES_PER_RUN,
        "startedBy": actor,
        "startedAt": int(now_ms),
        "agreement": "The person who started this mirror confirmed the forum's operators agreed to it.",
    }


def importer_source(root=None):
    path = market_packages.entrypoint_file(
        IMPORTER_PACKAGE, IMPORTER_ENTRYPOINT, **({"root": root} if root else {}))
    if path is None:
        return None
    try:
        return path.read_bytes()
    except OSError:
        return None


def robots_allows(text, paths):
    parser = urllib.robotparser.RobotFileParser()
    parser.parse(str(text or "").splitlines())
    return all(parser.can_fetch(ROBOTS_AGENT, path) for path in paths)


async def _get(url, accept):
    init = {"method": "GET", "redirect": "manual",
            "headers": {"user-agent": USER_AGENT, "accept": accept}}
    try:
        response = await js_fetch_with_timeout(url, init, CHECK_TIMEOUT_SECONDS)
    except Exception:
        return 0, ""
    if response is None:
        return 0, ""
    status = int(getattr(response, "status", 0) or 0)
    try:
        text = str(await response.text() or "")
    except Exception:
        text = ""
    if len(text) > MAX_CHECK_CHARS:
        return 413, ""
    return status, text


def _json(text):
    try:
        value = json.loads(text)
    except ValueError:
        return None
    return value if isinstance(value, dict) else None


async def check(origin):
    parsed = urlparse(origin)
    report = {"origin": origin, "reachable": False, "discourse": False, "public": False,
              "robotsAllow": False, "title": "", "description": "", "categories": 0,
              "topics": 0, "posts": 0}
    robots_status, robots = await _get(
        "https://%s/robots.txt" % parsed.netloc, "text/plain")
    if robots_status in (401, 403):
        report["robotsAllow"] = False
    else:
        report["robotsAllow"] = robots_allows(
            robots if robots_status == 200 else "",
            [origin + "/site.json", origin + "/latest.json", origin + "/t/1.json"])
    status, text = await _get(origin + "/site.json", "application/json")
    report["reachable"] = status != 0
    site = _json(text) if status == 200 else None
    report["public"] = status == 200
    report["discourse"] = bool(site) and isinstance(site.get("categories"), list)
    if report["discourse"]:
        report["categories"] = len(site["categories"])
        status, text = await _get(origin + "/about.json", "application/json")
        about = ((_json(text) or {}).get("about") or {}) if status == 200 else {}
        stats = about.get("stats") if isinstance(about.get("stats"), dict) else about
        report["title"] = str(about.get("title") or "")[:200]
        report["description"] = str(about.get("description") or "")[:500]
        for key, names in (("topics", ("topics_count", "topic_count")),
                           ("posts", ("posts_count", "post_count"))):
            for name in names:
                try:
                    report[key] = max(0, int(stats.get(name) or 0))
                except (TypeError, ValueError):
                    continue
                if report[key]:
                    break
    report["estimate"] = first_copy_estimate(report["topics"], report["posts"])
    report["ready"] = bool(report["discourse"] and report["public"] and report["robotsAllow"])
    if not report["reachable"]:
        report["problem"] = "The forum did not answer."
    elif not report["discourse"]:
        report["problem"] = "This address does not answer like a Discourse forum."
    elif not report["robotsAllow"]:
        report["problem"] = "The forum's robots.txt does not allow copying."
    return report


async def _actor(env, request, data):
    _, record = await _account_session_record(env, request, data or {})
    actor = clean_string((record or {}).get("name", ""), MAX_NODE_NAME).strip().lower()
    if not actor:
        raise SetupError("invalid_session", "Sign in first.", 401)
    if not await _is_admin(env, actor):
        raise SetupError(
            "admin_required",
            "Only an admin of this ForkMesh instance can start a forum mirror.", 403)
    return actor


async def _enable_forum(env, owner, repo):
    features = await _repo_features_get(env, owner, repo)
    if features.get("forum") is True:
        return
    features["forum"] = True
    await d1_run(
        env,
        "INSERT INTO repo_feature_settings (repo_bi, data, updated_at)"
        " VALUES (?,?,?) ON CONFLICT(repo_bi) DO UPDATE SET"
        " data=excluded.data, updated_at=excluded.updated_at",
        await _repo_features_bi(env, owner, repo),
        json.dumps(features, sort_keys=True), int(Date.now()))


async def _dispatch(env, owner, repo, branch, actor):
    if not str(getattr(env, "ACTIONS_INSTANCE_RUNNER", "") or "").strip():
        return {"dispatched": False, "reason": "actions_unavailable"}
    try:
        import repository_actions
        import runner_actions
        runtime = repository_actions._runner_runtime(env)
        await runtime.ensure_schema()
        job, reason, _message = await runner_actions.dispatch(
            runtime, owner, repo, WORKFLOW_PATH, "refs/heads/" + branch, None, actor)
    except Exception:
        return {"dispatched": False, "reason": "dispatch_failed"}
    if reason:
        return {"dispatched": False, "reason": reason}
    return {"dispatched": True, "runId": job * runner_actions.RUN_ID_STRIDE}


async def start(env, request, data, actor):
    owner = str(data.get("owner") or "").strip()
    repo = str(data.get("repo") or "").strip()
    branch = str(data.get("branch") or "main").strip()
    if not SEGMENT_RE.match(owner) or not SEGMENT_RE.match(repo) or not edits.valid_branch(branch):
        raise SetupError("invalid_repository", "Choose a repository.", 400)
    if data.get("agree") is not True:
        raise SetupError(
            "agreement_required",
            "Confirm that the forum's operators agreed to the mirror.", 400)
    origin = clean_origin(data.get("origin"))
    if not origin:
        raise SetupError("invalid_origin", "Enter the forum's https address.", 400)
    report = await check(origin)
    if not report["ready"]:
        raise SetupError("forum_not_ready", report.get("problem") or "The forum cannot be copied.", 409)
    if await edits._permission(env, request, owner, repo, actor) != "write":
        raise SetupError("not_authorized", "You cannot write to that repository.", 403)
    tool = importer_source()
    if tool is None:
        raise SetupError(
            "importer_unavailable",
            "This instance does not have the Discourse Mirror importer installed.", 503)
    now = int(Date.now())
    settings = settings_record(origin, data.get("uploads", True) is not False, actor, now)
    files = {
        WORKFLOW_PATH: workflow_text().encode("utf-8"),
        TOOL_PATH: tool,
        SETTINGS_PATH: (json.dumps(settings, indent=1, sort_keys=True) + "\n").encode("utf-8"),
    }
    try:
        commit = await edits.commit_paths(
            env, request, owner, repo, actor, branch, files,
            "Mirror %s into the Forum tab" % urlparse(origin).netloc)
    except edits.FileEditError as exc:
        raise SetupError(exc.code, exc.message, exc.status)
    await _enable_forum(env, owner, repo)
    run = await _dispatch(env, owner, repo, branch, actor)
    await _audit_sensitive_action(
        env, actor, "repository.forum_mirror", "repository", owner.lower() + "/" + repo,
        "success", {"origin": origin, "commit": commit, "dispatched": run.get("dispatched")})
    return {"ok": True, "commit": commit, "origin": origin, "title": report["title"],
            "estimate": report["estimate"], "workflow": WORKFLOW_PATH, **run}


async def forum_mirrors_handler(env, request):
    method = method_name(request)
    try:
        if method == "GET":
            actor = await _actor(env, request, {})
            query = parse_qs(urlparse(str(request.url)).query)
            if not (query.get("origin") or [""])[0].strip():
                return json_response({"ok": True, "actor": actor, "admin": True}, cache_control="no-store")
            origin = clean_origin((query.get("origin") or [""])[0])
            if not origin:
                raise SetupError("invalid_origin", "Enter the forum's https address.", 400)
            report = await check(origin)
            return json_response({"ok": True, "actor": actor, **report}, cache_control="no-store")
        if method != "POST":
            return json_response({"error": "method_not_allowed"}, status=405)
        try:
            data = await bounded_json_request(request)
        except Exception:
            data = None
        if not isinstance(data, dict):
            raise SetupError("invalid_json", "The request was not understood.", 400)
        actor = await _actor(env, request, data)
        await ensure_schema(env)
        return json_response(await start(env, request, data, actor), cache_control="no-store")
    except SetupError as exc:
        return json_response({"error": exc.code, "message": exc.message},
                             status=exc.status, cache_control="no-store")
