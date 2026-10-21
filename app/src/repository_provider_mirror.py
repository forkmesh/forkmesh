from __future__ import annotations

import json
import re
from urllib.parse import quote, urlparse

import instance_features


MARKER = "<!-- forkmesh-mirror -->"
USER_AGENT = "ForkMesh-provider-mirror/1.0 (+https://forkmesh.com/docs)"
GITHUB_API_VERSION = "2022-11-28"
MAX_TOKEN_LENGTH = 512
MAX_TITLE_LENGTH = 256
MAX_BODY_LENGTH = 262_144
MAX_LABELS = 100
MAX_HISTORY = 20
MAX_CANDIDATES = 50
MAX_IMPORT_PER_REQUEST = 25
AUTO_IMPORT_PER_RUN = 100
AUTO_COMMENTS_PER_RUN = 500
SYNC_INTERVAL_MS = 10 * 60 * 1000
CRON_MIRRORS_PER_TICK = 3
OUTBOX_DRAIN_IMMEDIATE = 2
OUTBOX_MAX_ATTEMPTS = 6
OUTBOX_BASE_DELAY_MS = 60 * 1000
OUTBOX_MAX_DELAY_MS = 60 * 60 * 1000
REQUEST_TIMEOUT_SECONDS = 15
MAX_RESPONSE_CHARS = 2 * 1024 * 1024
PUSH_REFS = ("all", "default")
RETRY_STATUSES = frozenset({0, 408, 425, 429, 500, 502, 503, 504})

PROVIDERS = {
    "github": {
        "label": "GitHub",
        "api": "https://api.github.com",
        "web": "https://github.com",
        "username": "x-access-token",
        "tokenUrl": "https://github.com/settings/personal-access-tokens/new",
        "tokenHelp": "A fine-grained token with Contents, Issues, Pull "
                     "requests, Discussions and Metadata access to the "
                     "repository (read and write), or a classic token with "
                     "the repo scope.",
    },
    "gitlab": {
        "label": "GitLab",
        "api": "https://gitlab.com/api/v4",
        "web": "https://gitlab.com",
        "username": "oauth2",
        "tokenUrl": "https://gitlab.com/-/user_settings/personal_access_tokens",
        "tokenHelp": "A personal access token with the api and "
                     "write_repository scopes.",
    },
}

DIRECTIONS = ("off", "push", "pull", "both")
MODES = ("auto", "manual")

RESOURCES = (
    {"key": "code", "label": "Code", "icon": "git-branch",
     "hint": "Branches and tags, pushed from ForkMesh after every change.",
     "directions": ("off", "push"), "providers": ("github", "gitlab"),
     "default": "push"},
    {"key": "issues", "label": "Issues", "icon": "circle-dot",
     "hint": "Issues, their labels, state and comments.",
     "directions": DIRECTIONS, "providers": ("github", "gitlab"),
     "default": "off"},
    {"key": "pulls", "label": "Pull requests", "icon": "git-pull-request",
     "labels": {"gitlab": "Merge requests"},
     "hint": "Proposals and their conversation. Imported ones arrive as "
             "issues labelled pull-request that link to the original.",
     "directions": DIRECTIONS, "providers": ("github", "gitlab"),
     "default": "off"},
    {"key": "discussions", "label": "Discussions", "icon": "messages-square",
     "hint": "Discussion threads with their replies.",
     "directions": DIRECTIONS, "providers": ("github",), "default": "off"},
    {"key": "releases", "label": "Releases", "icon": "tag",
     "hint": "Signed ForkMesh releases, published on the other side with "
             "their notes.",
     "directions": ("off", "push"), "providers": ("github", "gitlab"),
     "default": "off"},
)
RESOURCE_KEYS = tuple(item["key"] for item in RESOURCES)
PULL_KINDS = ("issues", "pulls", "discussions")

EVENT_KINDS = {
    "issue.opened": ("issues", "open"),
    "issue.comment": ("issues", "comment"),
    "pull.opened": ("pulls", "open"),
    "pull.comment": ("pulls", "comment"),
    "discussion.opened": ("discussions", "open"),
    "release.published": ("releases", "open"),
}

_SEGMENT_RE = re.compile(r"^[A-Za-z0-9_.-]{1,100}$")
_LABEL_RE = re.compile(r"[^A-Za-z0-9 _.:/+-]")


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def clean_provider(value):
    provider = str(value or "").strip().lower()
    return provider if provider in PROVIDERS else ""


def clean_remote(provider, value):
    raw = str(value or "").strip()
    if not raw or len(raw) > 300 or any(ch.isspace() for ch in raw):
        return None
    if "://" in raw:
        try:
            parsed = urlparse(raw)
        except ValueError:
            return None
        host = str(parsed.hostname or "").lower()
        if (parsed.scheme.lower() != "https" or parsed.username
                or parsed.password or parsed.port not in (None, 443)
                or host != urlparse(PROVIDERS[provider]["web"]).hostname):
            return None
        raw = parsed.path
    parts = [part for part in raw.strip("/").split("/") if part]
    if parts and parts[-1].lower().endswith(".git"):
        parts[-1] = parts[-1][:-4]
    if len(parts) != 2:
        return None
    for part in parts:
        if not _SEGMENT_RE.fullmatch(part) or part.startswith("."):
            return None
    return {"namespace": "/".join(parts[:-1]), "name": parts[-1],
            "path": "/".join(parts)}


def clean_token(value):
    token = str(value or "").strip()
    if not token or len(token) > MAX_TOKEN_LENGTH:
        return ""
    if any(ord(ch) < 33 or ord(ch) > 126 for ch in token):
        return ""
    return token


def clean_push_refs(value):
    value = str(value or "").strip().lower()
    return value if value in PUSH_REFS else ""


def token_hint(token):
    token = str(token or "")
    return ("…" + token[-4:]) if len(token) >= 8 else "…"


def resource_supported(key, provider):
    for item in RESOURCES:
        if item["key"] == key:
            return provider in item["providers"]
    return False


def normalize_resources(value, provider, previous=None):
    value = value if isinstance(value, dict) else {}
    previous = previous if isinstance(previous, dict) else {}
    out = {}
    for item in RESOURCES:
        key = item["key"]
        chosen = value.get(key)
        if not isinstance(chosen, dict):
            chosen = previous.get(key) if isinstance(
                previous.get(key), dict) else {}
        direction = str(chosen.get("direction") or item["default"]).lower()
        if direction not in item["directions"]:
            direction = "off"
        if provider not in item["providers"]:
            direction = "off"
        mode = str(chosen.get("mode") or "auto").lower()
        if mode not in MODES:
            mode = "auto"
        out[key] = {"direction": direction, "mode": mode,
                    "comments": chosen.get("comments", True) is not False}
    return out


def pushes(resources, key):
    return (resources or {}).get(key, {}).get("direction") in ("push", "both")


def pulls(resources, key):
    return (resources or {}).get(key, {}).get("direction") in ("pull", "both")


def auto_pulls(resources, key):
    return pulls(resources, key) and (
        resources.get(key, {}).get("mode") != "manual")


def remote_web_url(provider, remote):
    return PROVIDERS[provider]["web"] + "/" + remote["path"]


def remote_push_url(provider, remote):
    return remote_web_url(provider, remote) + ".git"


def catalog():
    return {
        "providers": [
            {"key": key, "label": spec["label"], "web": spec["web"],
             "tokenUrl": spec["tokenUrl"], "tokenHelp": spec["tokenHelp"]}
            for key, spec in PROVIDERS.items()
        ],
        "resources": [
            {"key": item["key"], "label": item["label"],
             "labels": dict(item.get("labels") or {}),
             "icon": item["icon"], "hint": item["hint"],
             "directions": list(item["directions"]),
             "providers": list(item["providers"]),
             "default": item["default"]}
            for item in RESOURCES
        ],
        "directions": list(DIRECTIONS),
        "modes": list(MODES),
    }


def public_view(row, record):
    record = record if isinstance(record, dict) else {}
    provider = clean_provider(record.get("provider"))
    remote = record.get("remote") if isinstance(
        record.get("remote"), dict) else {}
    status = record.get("status") if isinstance(
        record.get("status"), dict) else {}
    return {
        "provider": provider,
        "providerLabel": PROVIDERS.get(provider, {}).get("label", ""),
        "remote": str(remote.get("path") or ""),
        "remoteUrl": remote_web_url(provider, remote)
        if provider and remote.get("path") else "",
        "tokenHint": str(record.get("tokenHint") or ""),
        "active": int((row or {}).get("active") or 0) == 1,
        "prune": bool(record.get("prune")),
        "refs": clean_push_refs(record.get("refs")) or "all",
        "resources": normalize_resources(
            record.get("resources"), provider) if provider else {},
        "createdAt": int((row or {}).get("created_at") or 0),
        "updatedAt": int((row or {}).get("updated_at") or 0),
        "lastSyncAt": int((row or {}).get("last_sync_at") or 0),
        "status": {
            "login": str(status.get("login") or ""),
            "verifiedAt": int(status.get("verifiedAt") or 0),
            "remoteExists": bool(status.get("remoteExists")),
            "remotePrivate": bool(status.get("remotePrivate")),
            "canPush": bool(status.get("canPush")),
            "error": str(status.get("error") or ""),
            "codePush": dict(status.get("codePush") or {}),
            "lastSync": dict(status.get("lastSync") or {}),
        },
        "history": list(record.get("history") or [])[:MAX_HISTORY],
    }


def public_link(row, record):
    record = record if isinstance(record, dict) else {}
    provider = clean_provider(record.get("provider"))
    remote = record.get("remote") if isinstance(
        record.get("remote"), dict) else {}
    status = record.get("status") if isinstance(
        record.get("status"), dict) else {}
    if (not provider or not remote.get("path")
            or int((row or {}).get("active") or 0) != 1
            or not pushes(normalize_resources(
                record.get("resources"), provider), "code")
            or not status.get("remoteExists")
            or status.get("remotePrivate")):
        return None
    return {"provider": provider, "label": PROVIDERS[provider]["label"],
            "url": remote_web_url(provider, remote), "role": "mirror"}


def clean_title(value):
    text = re.sub(r"\s+", " ", str(value or "")).strip()
    return text[:MAX_TITLE_LENGTH] or "Untitled"


def clean_labels(values):
    out = []
    for value in values or []:
        name = value.get("name") if isinstance(value, dict) else value
        name = _LABEL_RE.sub("", str(name or "")).strip().lower()[:40]
        if name and name not in out:
            out.append(name)
        if len(out) >= MAX_LABELS:
            break
    return out


def is_mirrored_text(body):
    return MARKER in str(body or "")


def _iso_ms(value):
    text = str(value or "").strip()
    match = re.match(
        r"^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})", text)
    if not match:
        return 0
    year, month, day, hour, minute, second = (int(x) for x in match.groups())
    if month <= 2:
        year -= 1
        month += 12
    days = (365 * year + year // 4 - year // 100 + year // 400
            + (153 * (month - 3) + 2) // 5 + day - 719469)
    return ((days * 24 + hour) * 60 + minute) * 60000 + second * 1000


def normalize_remote_item(provider, kind, raw):
    if not isinstance(raw, dict):
        return None
    if provider == "github":
        if kind == "issues" and raw.get("pull_request"):
            return None
        if kind == "discussions":
            number = int(raw.get("number") or 0)
            author = (raw.get("author") or {}).get("login") or "ghost"
            comments = (raw.get("comments") or {}).get("nodes") or []
            return {
                "kind": kind, "number": number, "key": str(number),
                "title": clean_title(raw.get("title")),
                "body": str(raw.get("body") or ""), "author": str(author),
                "state": "closed" if raw.get("closed") else "open",
                "url": str(raw.get("url") or ""),
                "createdAt": _iso_ms(raw.get("createdAt")),
                "updatedAt": _iso_ms(raw.get("updatedAt")),
                "labels": [], "category": str(
                    (raw.get("category") or {}).get("name") or ""),
                "comments": [
                    {"id": str(c.get("id") or ""),
                     "author": str((c.get("author") or {}).get("login")
                                   or "ghost"),
                     "body": str(c.get("body") or ""),
                     "url": str(c.get("url") or ""),
                     "createdAt": _iso_ms(c.get("createdAt"))}
                    for c in comments if isinstance(c, dict)
                ],
                "commentCount": int(
                    (raw.get("comments") or {}).get("totalCount")
                    or len(comments)),
            }
        number = int(raw.get("number") or 0)
        item = {
            "kind": kind, "number": number, "key": str(number),
            "title": clean_title(raw.get("title")),
            "body": str(raw.get("body") or ""),
            "author": str((raw.get("user") or {}).get("login") or "ghost"),
            "state": "closed" if raw.get("state") == "closed" else "open",
            "url": str(raw.get("html_url") or ""),
            "createdAt": _iso_ms(raw.get("created_at")),
            "updatedAt": _iso_ms(raw.get("updated_at")),
            "labels": clean_labels(raw.get("labels")),
            "commentCount": int(raw.get("comments") or 0),
        }
        if kind == "pulls":
            item["head"] = str((raw.get("head") or {}).get("label") or "")
            item["base"] = str((raw.get("base") or {}).get("ref") or "")
            item["merged"] = bool(raw.get("merged_at"))
        return item
    number = int(raw.get("iid") or 0)
    item = {
        "kind": kind, "number": number, "key": str(number),
        "title": clean_title(raw.get("title")),
        "body": str(raw.get("description") or ""),
        "author": str((raw.get("author") or {}).get("username") or "ghost"),
        "state": "open" if raw.get("state") in ("opened", "open")
        else "closed",
        "url": str(raw.get("web_url") or ""),
        "createdAt": _iso_ms(raw.get("created_at")),
        "updatedAt": _iso_ms(raw.get("updated_at")),
        "labels": clean_labels(raw.get("labels")),
        "commentCount": int(raw.get("user_notes_count") or 0),
    }
    if kind == "pulls":
        item["head"] = str(raw.get("source_branch") or "")
        item["base"] = str(raw.get("target_branch") or "")
        item["merged"] = raw.get("state") == "merged"
    return item


def candidate_view(item, imported):
    view = {key: item.get(key) for key in (
        "kind", "number", "key", "title", "author", "state", "url",
        "updatedAt", "labels", "commentCount")}
    view["excerpt"] = re.sub(r"\s+", " ", str(item.get("body") or ""))[:160]
    view["imported"] = bool(imported)
    view["mirrored"] = is_mirrored_text(item.get("body"))
    if item.get("kind") == "pulls":
        view["head"] = item.get("head", "")
        view["base"] = item.get("base", "")
        view["merged"] = bool(item.get("merged"))
    return view


def import_body(provider, item):
    label = PROVIDERS[provider]["label"]
    noun = {"issues": "issue", "pulls": "pull request" if provider ==
            "github" else "merge request", "discussions": "discussion"}.get(
        item.get("kind"), "item")
    lines = [str(item.get("body") or "").strip()]
    if item.get("kind") == "pulls" and (item.get("head") or item.get("base")):
        lines.append("")
        lines.append("`%s` → `%s`%s" % (
            item.get("head", ""), item.get("base", ""),
            " (merged)" if item.get("merged") else ""))
    lines.append("")
    lines.append("---")
    lines.append("_Mirrored from %s %s [#%s](%s) by @%s._" % (
        label, noun, item.get("number"), item.get("url"),
        item.get("author")))
    lines.append(MARKER)
    return "\n".join(lines).strip()[:MAX_BODY_LENGTH]


def import_comment_body(provider, comment):
    label = PROVIDERS[provider]["label"]
    return ("%s\n\n---\n_Comment by @%s on [%s](%s)._\n%s" % (
        str(comment.get("body") or "").strip(), comment.get("author"),
        label, comment.get("url") or "", MARKER))[:MAX_BODY_LENGTH]


def export_body(body, author, link, label):
    return ("%s\n\n---\n_Mirrored from ForkMesh: [%s](%s) by %s._\n%s" % (
        str(body or "").strip(), label, link, author or "someone",
        MARKER))[:MAX_BODY_LENGTH]


def backoff_ms(attempt):
    step = max(1, int(attempt or 1))
    return min(OUTBOX_MAX_DELAY_MS, OUTBOX_BASE_DELAY_MS * (2 ** (step - 1)))


def api_headers(provider, token, graphql=False):
    headers = {"accept": "application/json",
               "user-agent": instance_features.user_agent(USER_AGENT, "provider-mirror")}
    if provider == "github":
        headers["accept"] = "application/vnd.github+json"
        headers["x-github-api-version"] = GITHUB_API_VERSION
        headers["authorization"] = "Bearer " + token
    else:
        headers["private-token"] = token
    if graphql:
        headers["content-type"] = "application/json"
    return headers


def project_path(remote):
    return quote(remote["path"], safe="")


def error_text(status, data):
    message = ""
    if isinstance(data, dict):
        message = str(data.get("message") or data.get("error")
                      or data.get("error_description") or "")
        if not message and isinstance(data.get("errors"), list) and \
                data["errors"]:
            first = data["errors"][0]
            message = str(first.get("message") if isinstance(first, dict)
                          else first)
    if status == 0:
        return "The provider could not be reached."
    if status == 401:
        return "The token was refused. Check it has not expired."
    if status == 403:
        return message[:200] or "The token lacks permission for this."
    if status == 404:
        return "Not found, or the token cannot see it."
    return ("HTTP %d %s" % (status, message[:200])).strip()


DISCUSSIONS_QUERY = (
    "query($owner:String!,$name:String!,$first:Int!){repository("
    "owner:$owner,name:$name){id hasDiscussionsEnabled "
    "discussionCategories(first:10){nodes{id name}} "
    "discussions(first:$first,orderBy:{field:UPDATED_AT,direction:DESC})"
    "{nodes{id number title body url closed createdAt updatedAt "
    "author{login} category{name} comments(first:30){totalCount "
    "nodes{id body url createdAt author{login}}}}}}}")

CREATE_DISCUSSION_MUTATION = (
    "mutation($repo:ID!,$cat:ID!,$title:String!,$body:String!){"
    "createDiscussion(input:{repositoryId:$repo,categoryId:$cat,"
    "title:$title,body:$body}){discussion{id number url}}}")


def _now():
    return int(Date.now())


async def _repo_bi(env, owner, repo):
    return await blind_index(env, str(owner).lower() + "/" + str(repo))


async def _http(method, url, headers, body=None):
    init = {"method": method, "headers": headers, "redirect": "manual"}
    if body is not None:
        init["body"] = json.dumps(body)
        init["headers"] = dict(headers, **{"content-type": "application/json"})
    try:
        response = await js_fetch_with_timeout(
            url, init, REQUEST_TIMEOUT_SECONDS)
    except Exception:
        return 0, None
    if response is None:
        return 0, None
    status = int(getattr(response, "status", 0) or 0)
    try:
        text = str(await response.text() or "")
    except Exception:
        text = ""
    if len(text) > MAX_RESPONSE_CHARS:
        return 502, None
    try:
        data = json.loads(text) if text else None
    except ValueError:
        data = None
    return status, data


async def _api(record, method, path, body=None):
    provider = record["provider"]
    url = PROVIDERS[provider]["api"] + path
    return await _http(method, url, api_headers(provider, record["token"]),
                       body)


async def _graphql(record, query, variables):
    status, data = await _http(
        "POST", "https://api.github.com/graphql",
        api_headers("github", record["token"], graphql=True),
        {"query": query, "variables": variables})
    if status == 200 and isinstance(data, dict) and data.get("errors"):
        first = data["errors"][0] if data["errors"] else {}
        return 422, {"message": str((first or {}).get("message") or "")}
    return status, data


def _remote_api_path(record):
    remote = record["remote"]
    if record["provider"] == "github":
        return "/repos/%s/%s" % (remote["namespace"], remote["name"])
    return "/projects/" + project_path(remote)


async def _verify(record):
    provider = record["provider"]
    status, user = await _api(record, "GET", "/user")
    result = {"verifiedAt": _now(), "login": "", "remoteExists": False,
              "remotePrivate": False, "canPush": False, "error": ""}
    if status != 200 or not isinstance(user, dict):
        result["error"] = error_text(status, user)
        return result
    result["login"] = str(user.get("login") or user.get("username") or "")
    status, repo = await _api(record, "GET", _remote_api_path(record))
    if status == 404:
        return result
    if status != 200 or not isinstance(repo, dict):
        result["error"] = error_text(status, repo)
        return result
    result["remoteExists"] = True
    if provider == "github":
        result["remotePrivate"] = bool(repo.get("private"))
        result["canPush"] = bool((repo.get("permissions") or {}).get("push"))
        result["defaultBranch"] = str(repo.get("default_branch") or "")
    else:
        result["remotePrivate"] = repo.get("visibility") != "public"
        permissions = repo.get("permissions") or {}
        levels = [
            int(((permissions.get(k) or {}).get("access_level")) or 0)
            for k in ("project_access", "group_access")]
        result["canPush"] = max(levels or [0]) >= 30
        result["defaultBranch"] = str(repo.get("default_branch") or "")
    return result


async def _create_remote(record, private, description):
    remote = record["remote"]
    if record["provider"] == "github":
        status, user = await _api(record, "GET", "/user")
        login = str((user or {}).get("login") or "").lower()
        path = ("/user/repos" if remote["namespace"].lower() == login
                else "/orgs/%s/repos" % remote["namespace"])
        status, data = await _api(record, "POST", path, {
            "name": remote["name"], "private": bool(private),
            "description": str(description or "")[:300],
            "has_issues": True, "auto_init": False})
        return status in (200, 201), error_text(status, data)
    status, namespace = await _api(
        record, "GET", "/namespaces/" + quote(remote["namespace"], safe=""))
    if status != 200 or not isinstance(namespace, dict):
        return False, error_text(status, namespace)
    status, data = await _api(record, "POST", "/projects", {
        "name": remote["name"], "path": remote["name"],
        "namespace_id": namespace.get("id"),
        "visibility": "private" if private else "public",
        "description": str(description or "")[:300]})
    return status in (200, 201), error_text(status, data)


async def _list_remote(record, kind, per_page, since_ms=0, page=1):
    provider = record["provider"]
    base = _remote_api_path(record)
    if provider == "github":
        if kind == "discussions":
            status, data = await _graphql(record, DISCUSSIONS_QUERY, {
                "owner": record["remote"]["namespace"],
                "name": record["remote"]["name"], "first": per_page})
            if status != 200:
                return None, error_text(status, data)
            repo = ((data or {}).get("data") or {}).get("repository") or {}
            nodes = (repo.get("discussions") or {}).get("nodes") or []
            items = [normalize_remote_item(provider, kind, n) for n in nodes]
            return [i for i in items if i], ""
        path = base + ("/pulls" if kind == "pulls" else "/issues")
        path += "?state=all&sort=updated&direction=desc&per_page=%d&page=%d" % (
            per_page, max(1, int(page)))
    else:
        if kind == "discussions":
            return [], ""
        path = base + ("/merge_requests" if kind == "pulls" else "/issues")
        path += ("?scope=all&state=all&order_by=updated_at&sort=desc"
                 "&per_page=%d&page=%d" % (per_page, max(1, int(page))))
    status, data = await _api(record, "GET", path)
    if status != 200 or not isinstance(data, list):
        return None, error_text(status, data)
    items = [normalize_remote_item(provider, kind, raw) for raw in data]
    items = [item for item in items if item]
    if since_ms:
        items = [item for item in items if item["updatedAt"] >= since_ms]
    return items, ""


async def _remote_comments(record, kind, number, since_ms=0):
    provider = record["provider"]
    base = _remote_api_path(record)
    if provider == "github":
        path = base + "/issues/%d/comments?per_page=50" % int(number)
        status, data = await _api(record, "GET", path)
        if status != 200 or not isinstance(data, list):
            return []
        return [
            {"id": str(c.get("id") or ""),
             "author": str((c.get("user") or {}).get("login") or "ghost"),
             "body": str(c.get("body") or ""),
             "url": str(c.get("html_url") or ""),
             "createdAt": _iso_ms(c.get("created_at"))}
            for c in data if isinstance(c, dict)
            and _iso_ms(c.get("created_at")) >= since_ms
        ]
    noun = "merge_requests" if kind == "pulls" else "issues"
    path = base + "/%s/%d/notes?sort=asc&order_by=created_at&per_page=50" % (
        noun, int(number))
    status, data = await _api(record, "GET", path)
    if status != 200 or not isinstance(data, list):
        return []
    return [
        {"id": str(c.get("id") or ""),
         "author": str((c.get("author") or {}).get("username") or "ghost"),
         "body": str(c.get("body") or ""),
         "url": "", "createdAt": _iso_ms(c.get("created_at"))}
        for c in data if isinstance(c, dict) and not c.get("system")
        and _iso_ms(c.get("created_at")) >= since_ms
    ]


async def _load(env, repo_bi):
    row = await d1_first(
        env,
        "SELECT repo_bi, provider, active, created_at, updated_at,"
        " next_sync_at, last_sync_at, data FROM repo_provider_mirrors"
        " WHERE repo_bi=?", repo_bi)
    if not row:
        return None, None
    record = await decrypt_row(env, row.get("data"))
    if not isinstance(record, dict) or not clean_provider(
            record.get("provider")):
        return row, None
    record["resources"] = normalize_resources(
        record.get("resources"), record["provider"])
    return row, record


async def public_about_link(env, owner, repo):
    try:
        row, record = await _load(env, await _repo_bi(env, owner, repo))
    except Exception:
        return None
    return public_link(row, record)


async def _store(env, repo_bi, record, active=None, next_sync_at=None,
                 last_sync_at=None):
    now = _now()
    encrypted = await encrypt_row(env, record)
    existing = await d1_first(
        env, "SELECT active, created_at, next_sync_at, last_sync_at FROM"
        " repo_provider_mirrors WHERE repo_bi=?", repo_bi)
    if not existing:
        await d1_run(
            env,
            "INSERT INTO repo_provider_mirrors (repo_bi, provider, active,"
            " created_at, updated_at, next_sync_at, last_sync_at, data)"
            " VALUES (?,?,?,?,?,?,?,?)",
            repo_bi, record["provider"], 1 if active in (None, True, 1)
            else 0, now, now, int(next_sync_at or 0),
            int(last_sync_at or 0), encrypted)
        return
    await d1_run(
        env,
        "UPDATE repo_provider_mirrors SET provider=?, active=?, updated_at=?,"
        " next_sync_at=?, last_sync_at=?, data=? WHERE repo_bi=?",
        record["provider"],
        int(existing.get("active") or 0) if active is None
        else (1 if active else 0),
        now,
        int(existing.get("next_sync_at") or 0) if next_sync_at is None
        else int(next_sync_at),
        int(existing.get("last_sync_at") or 0) if last_sync_at is None
        else int(last_sync_at),
        encrypted, repo_bi)


def _note_history(record, kind, direction, ok, message):
    history = list(record.get("history") or [])
    history.insert(0, {"at": _now(), "kind": kind, "direction": direction,
                       "ok": bool(ok), "message": str(message or "")[:200]})
    record["history"] = history[:MAX_HISTORY]


async def _link(env, repo_bi, kind, remote_key):
    return await d1_first(
        env, "SELECT local_key, origin FROM repo_provider_mirror_links"
        " WHERE repo_bi=? AND kind=? AND remote_key=?",
        repo_bi, kind, str(remote_key))


async def _link_by_local(env, repo_bi, kind, local_key):
    return await d1_first(
        env, "SELECT remote_key, origin FROM repo_provider_mirror_links"
        " WHERE repo_bi=? AND kind=? AND local_key=?",
        repo_bi, kind, str(local_key))


async def _save_link(env, repo_bi, kind, local_key, remote_key, origin):
    await d1_run(
        env,
        "INSERT INTO repo_provider_mirror_links (repo_bi, kind, local_key,"
        " remote_key, origin, created_at) VALUES (?,?,?,?,?,?)"
        " ON CONFLICT(repo_bi, kind, remote_key) DO UPDATE SET"
        " local_key=excluded.local_key",
        repo_bi, kind, str(local_key), str(remote_key), origin, _now())


async def _imported_keys(env, repo_bi, kind):
    rows = await d1_all(
        env, "SELECT remote_key FROM repo_provider_mirror_links"
        " WHERE repo_bi=? AND kind=?", repo_bi, kind)
    return {str(row.get("remote_key") or "") for row in rows or []}


async def _inbox_target(env, owner, repo):
    host_owner = await _ap_org_alias_owner(env, owner, repo)
    inbox_bi = await blind_index(env, host_owner + "/" + repo)
    row = await d1_first(
        env, "SELECT key_bi FROM repositories WHERE key_bi=?", inbox_bi)
    if not row:
        return host_owner, ""
    return host_owner, inbox_bi


async def _inbox_room(env, table, inbox_bi, limit):
    count = await d1_first(
        env, "SELECT COUNT(*) AS c FROM %s WHERE repo_bi=?" % table, inbox_bi)
    return not (count and int(count.get("c", 0) or 0) >= limit)


async def _import_item(env, record, repo_bi, item):
    provider = record["provider"]
    owner, repo = record["owner"], record["repo"]
    kind = item["kind"]
    if await _link(env, repo_bi, kind, item["key"]):
        return "exists"
    if is_mirrored_text(item.get("body")):
        await _save_link(env, repo_bi, kind, "", item["key"], "forkmesh")
        return "exists"
    host_owner, inbox_bi = await _inbox_target(env, owner, repo)
    if not inbox_bi:
        return "repository_not_found"
    now = _now()
    author_name = "@%s on %s" % (item["author"], PROVIDERS[provider]["label"])
    submitter_bi = await blind_index(env, "provider-mirror:" + provider)
    if kind == "discussions":
        if not await _inbox_room(env, "discussion_inbox", inbox_bi,
                                 MAX_PENDING_DISCUSSIONS):
            return "inbox_full"
        body = import_body(provider, item)
        replies = [c for c in item.get("comments") or []
                   if not is_mirrored_text(c.get("body"))]
        for comment in replies:
            body += "\n\n> **@%s** replied:\n>\n%s" % (
                comment["author"], "\n".join(
                    "> " + line for line in str(
                        comment["body"]).splitlines()))
        event = {"type": "open",
                 "id": "mirror-%s-discussion-%s" % (provider, item["key"]),
                 "author": provider, "authorName": author_name,
                 "ts": item.get("createdAt") or now,
                 "title": item["title"], "category": item.get("category")
                 or "General", "body": body[:MAX_BODY_LENGTH], "sig": ""}
        await d1_run(
            env, "INSERT INTO discussion_inbox (repo_bi, data, submitter_bi)"
            " VALUES (?,?,?)", inbox_bi, await encrypt_row(env, {
                "number": 0, "titleIfNew": item["title"], "event": event,
                "submitter": provider, "submittedAt": now,
                "source": "provider-mirror"}), submitter_bi)
        await _save_link(env, repo_bi, kind, "", item["key"], "provider")
        await notify_repo_host(env, host_owner, repo, "discussions")
        return "imported"
    if not await _inbox_room(env, "issue_inbox", inbox_bi, MAX_PENDING_ISSUES):
        return "inbox_full"
    number = int(await _forkbot_next_issue_number(
        env, inbox_bi, owner, repo) or 0)
    labels = [provider] + (["pull-request"] if kind == "pulls" else [])
    labels += [label for label in item.get("labels") or []
               if label not in labels]
    event = {"type": "open",
             "id": "mirror-%s-%s-%s" % (provider, kind, item["key"]),
             "author": provider, "authorName": author_name,
             "ts": item.get("createdAt") or now, "title": item["title"],
             "body": import_body(provider, item), "attachments": [],
             "sig": ""}
    rows = [{
        "number": number, "titleIfNew": item["title"], "event": event,
        "meta": {"labels": labels[:MAX_LABELS], "milestone": "",
                 "priority": 0, "assignees": [], "reporter": "",
                 "wantsAgent": False, "model": "", "provider": ""},
        "submitter": provider, "submittedAt": now,
        "source": "provider-mirror", "issueNumber": number,
    }]
    if item.get("state") == "closed" and number > 0:
        rows.append({
            "number": number, "titleIfNew": "",
            "event": {"type": "status", "status": "closed",
                      "id": "mirror-%s-%s-%s-closed" % (
                          provider, kind, item["key"]),
                      "author": provider, "authorName": author_name,
                      "ts": max(now, int(item.get("updatedAt") or 0)),
                      "sig": ""},
            "submitter": provider, "submittedAt": now,
            "source": "provider-mirror"})
    for row in rows:
        await d1_run(
            env, "INSERT INTO issue_inbox (repo_bi, data, submitter_bi)"
            " VALUES (?,?,?)", inbox_bi, await encrypt_row(env, row),
            submitter_bi)
    await _save_link(env, repo_bi, kind, str(number) if number else "",
                     item["key"], "provider")
    if number > 0 and (record.get("resources") or {}).get(kind, {}).get(
            "comments", True):
        await _import_comments(env, record, repo_bi, kind, item, number,
                               inbox_bi, submitter_bi, 0)
    await notify_repo_host(env, host_owner, repo, "issues")
    return "imported"


async def _import_comments(env, record, repo_bi, kind, item, number,
                           inbox_bi, submitter_bi, since_ms):
    provider = record["provider"]
    comments = await _remote_comments(record, kind, item["number"], since_ms)
    added = 0
    for comment in comments[:AUTO_COMMENTS_PER_RUN]:
        if not comment.get("id") or is_mirrored_text(comment.get("body")):
            continue
        key = "%s#%s" % (item["key"], comment["id"])
        if await _link(env, repo_bi, "comments", key):
            continue
        event = {"type": "comment",
                 "id": "mirror-%s-comment-%s" % (provider, comment["id"]),
                 "author": provider,
                 "authorName": "@%s on %s" % (
                     comment["author"], PROVIDERS[provider]["label"]),
                 "ts": comment.get("createdAt") or _now(),
                 "body": import_comment_body(provider, comment),
                 "attachments": [], "sig": ""}
        await d1_run(
            env, "INSERT INTO issue_inbox (repo_bi, data, submitter_bi)"
            " VALUES (?,?,?)", inbox_bi, await encrypt_row(env, {
                "number": int(number), "titleIfNew": "", "event": event,
                "meta": {"labels": [], "milestone": "", "priority": 0,
                         "assignees": [], "wantsAgent": False, "model": "",
                         "provider": ""},
                "submitter": provider, "submittedAt": _now(),
                "source": "provider-mirror"}), submitter_bi)
        await _save_link(env, repo_bi, "comments", str(number), key,
                         "provider")
        added += 1
    return added


async def _sync_inbound(env, repo_bi, record):
    resources = record.get("resources") or {}
    summary = {"imported": 0, "comments": 0, "errors": []}
    cursors = dict(record.get("cursors") or {})
    for kind in PULL_KINDS:
        if not resource_supported(kind, record["provider"]):
            continue
        if not auto_pulls(resources, kind):
            continue
        since = int(cursors.get(kind) or 0)
        items, error = await _list_remote(record, kind, 30, since)
        if items is None:
            summary["errors"].append("%s: %s" % (kind, error))
            continue
        newest = since
        imported = 0
        host_owner, inbox_bi = await _inbox_target(
            env, record["owner"], record["repo"])
        submitter_bi = await blind_index(
            env, "provider-mirror:" + record["provider"])
        for item in sorted(items, key=lambda i: i["updatedAt"]):
            link = await _link(env, repo_bi, kind, item["key"])
            if not link:
                if imported >= AUTO_IMPORT_PER_RUN:
                    break
                outcome = await _import_item(env, record, repo_bi, item)
                if outcome == "imported":
                    imported += 1
                elif outcome not in ("exists",):
                    summary["errors"].append("%s: %s" % (kind, outcome))
                    break
            elif (kind != "discussions" and inbox_bi
                  and str(link.get("local_key") or "").isdigit()
                  and resources.get(kind, {}).get("comments", True)
                  and since):
                summary["comments"] += await _import_comments(
                    env, record, repo_bi, kind, item,
                    int(link["local_key"]), inbox_bi, submitter_bi, since)
            newest = max(newest, int(item["updatedAt"] or 0))
        summary["imported"] += imported
        cursors[kind] = newest
    record["cursors"] = cursors
    return summary


async def _queue(env, repo_bi, payload):
    now = _now()
    await d1_run(
        env,
        "INSERT INTO repo_provider_mirror_outbox (repo_bi, attempts, next_ts,"
        " created_at, data) VALUES (?,0,?,?,?)",
        repo_bi, now, now, await encrypt_row(env, payload))


async def note_event(env, owner, repo, event_type, data):
    try:
        if event_type not in EVENT_KINDS:
            return 0
        owner = str(owner or "").strip().lower()
        repo = str(repo or "").strip()
        if not owner or not repo:
            return 0
        repo_bi = await _repo_bi(env, owner, repo)
        row = await d1_first(
            env, "SELECT active FROM repo_provider_mirrors WHERE repo_bi=?",
            repo_bi)
        if not row or int(row.get("active") or 0) != 1:
            return 0
        _row, record = await _load(env, repo_bi)
        if not record:
            return 0
        kind, action = EVENT_KINDS[event_type]
        if not resource_supported(kind, record["provider"]):
            return 0
        if not pushes(record.get("resources"), kind):
            return 0
        if action == "comment" and not record["resources"][kind].get(
                "comments", True):
            return 0
        data = data if isinstance(data, dict) else {}
        if is_mirrored_text(data.get("body")) or is_mirrored_text(
                data.get("notes")):
            return 0
        await _queue(env, repo_bi, {
            "kind": kind, "action": action,
            "number": int(data.get("number") or 0),
            "title": clean_title(data.get("title") or data.get("name")
                                 or data.get("tag")),
            "body": str(data.get("body") or data.get("notes") or "")[
                :MAX_BODY_LENGTH],
            "author": str(data.get("author") or "")[:80],
            "tag": str(data.get("tag") or "")[:200],
            "owner": owner, "repo": repo,
        })
        await drain_outbox(env, OUTBOX_DRAIN_IMMEDIATE)
        return 1
    except Exception:
        return 0


def _site_origin(env):
    base = str(getattr(env, "PUBLIC_BASE_URL", "") or "").strip().rstrip("/")
    return base if base.startswith("https://") else "https://forkmesh.com"


def _forkmesh_link(env, owner, repo, kind, number):
    base = "%s/%s/%s/%s" % (_site_origin(env), owner, repo, kind)
    return base + ("/%d" % number if number else "")


async def _deliver(env, repo_bi, record, payload):
    provider = record["provider"]
    kind = payload.get("kind")
    action = payload.get("action")
    owner, repo = payload.get("owner", ""), payload.get("repo", "")
    number = int(payload.get("number") or 0)
    base = _remote_api_path(record)
    noun = {"issues": "issue", "pulls": "pull request",
            "discussions": "discussion", "releases": "release"}.get(kind, "")
    link = _forkmesh_link(env, owner, repo, kind, number)
    label = "%s #%d" % (noun, number) if number else noun
    author = payload.get("author") or "someone"
    if kind == "releases":
        tag = payload.get("tag") or ""
        if not tag:
            return 400, "release without a tag"
        body = export_body(payload.get("body"), author,
                           link + "/" + quote(tag, safe=""), "release " + tag)
        if provider == "github":
            status, data = await _api(record, "POST", base + "/releases", {
                "tag_name": tag, "name": payload.get("title") or tag,
                "body": body})
        else:
            status, data = await _api(record, "POST", base + "/releases", {
                "tag_name": tag, "name": payload.get("title") or tag,
                "description": body})
        return status, error_text(status, data)
    if action == "comment":
        target, target_kind = None, kind
        for candidate in (kind, "pulls" if kind == "issues" else "issues"):
            target = await _link_by_local(env, repo_bi, candidate, number)
            if target and str(target.get("remote_key") or "").isdigit():
                target_kind = candidate
                break
            target = None
        if not target:
            return 404, "no mirrored counterpart for #%d yet" % number
        remote_number = int(target["remote_key"])
        body = export_body(payload.get("body"), author, link,
                           "comment on " + label)
        if provider == "github":
            status, data = await _api(
                record, "POST", base + "/issues/%d/comments" % remote_number,
                {"body": body})
        else:
            imported_pull = (target_kind == "pulls"
                             and target.get("origin") == "provider")
            noun_path = "merge_requests" if imported_pull else "issues"
            status, data = await _api(
                record, "POST", base + "/%s/%d/notes" % (
                    noun_path, remote_number), {"body": body})
        if status in (200, 201) and isinstance(data, dict):
            await _save_link(env, repo_bi, "comments", str(number),
                             "%s#%s" % (remote_number, data.get("id")),
                             "forkmesh")
        return status, error_text(status, data)
    title = payload.get("title") or "Untitled"
    body = export_body(payload.get("body"), author, link, label)
    if kind == "discussions":
        status, data = await _graphql(record, DISCUSSIONS_QUERY, {
            "owner": record["remote"]["namespace"],
            "name": record["remote"]["name"], "first": 1})
        repo_node = ((data or {}).get("data") or {}).get("repository") or {}
        categories = (repo_node.get("discussionCategories") or {}).get(
            "nodes") or []
        if status != 200 or not repo_node.get("id") or not categories:
            return (status if status != 200 else 422,
                    "Discussions are not enabled on the other side.")
        category = next(
            (c for c in categories if str(c.get("name")).lower()
             in ("general", "ideas")), categories[0])
        status, data = await _graphql(record, CREATE_DISCUSSION_MUTATION, {
            "repo": repo_node["id"], "cat": category["id"],
            "title": title, "body": body})
        created = (((data or {}).get("data") or {}).get(
            "createDiscussion") or {}).get("discussion") or {}
        if status == 200 and created.get("number"):
            await _save_link(env, repo_bi, kind, "",
                             str(created["number"]), "forkmesh")
        return status, error_text(status, data)
    if kind == "pulls":
        title = "Pull request: " + title
    if provider == "github":
        status, data = await _api(record, "POST", base + "/issues", {
            "title": title[:MAX_TITLE_LENGTH], "body": body})
        remote_number = (data or {}).get("number") if isinstance(
            data, dict) else None
    else:
        status, data = await _api(record, "POST", base + "/issues", {
            "title": title[:MAX_TITLE_LENGTH], "description": body})
        remote_number = (data or {}).get("iid") if isinstance(
            data, dict) else None
    if status in (200, 201) and remote_number:
        await _save_link(env, repo_bi, kind, str(number) if number else "",
                         str(remote_number), "forkmesh")
    return status, error_text(status, data)


async def drain_outbox(env, limit):
    now = _now()
    rows = await d1_all(
        env,
        "SELECT id, repo_bi, attempts, data FROM repo_provider_mirror_outbox"
        " WHERE next_ts<=? ORDER BY next_ts ASC LIMIT ?", now, int(limit))
    delivered = 0
    for row in rows or []:
        try:
            repo_bi = str(row.get("repo_bi") or "")
            mirror_row, record = await _load(env, repo_bi)
            payload = await decrypt_row(env, row.get("data"))
            if (not record or not isinstance(payload, dict)
                    or int((mirror_row or {}).get("active") or 0) != 1):
                await d1_run(env, "DELETE FROM repo_provider_mirror_outbox"
                             " WHERE id=?", row.get("id"))
                continue
            attempts = int(row.get("attempts") or 0) + 1
            status, message = await _deliver(env, repo_bi, record, payload)
            ok = 200 <= int(status) < 300
            retry = not ok and (int(status) in RETRY_STATUSES or (
                int(status) == 404 and payload.get("action") == "comment")
                or (int(status) == 422 and payload.get("kind") == "releases"))
            if retry and attempts < OUTBOX_MAX_ATTEMPTS:
                await d1_run(
                    env, "UPDATE repo_provider_mirror_outbox SET attempts=?,"
                    " next_ts=? WHERE id=?", attempts,
                    _now() + backoff_ms(attempts), row.get("id"))
                continue
            await d1_run(env, "DELETE FROM repo_provider_mirror_outbox"
                         " WHERE id=?", row.get("id"))
            _note_history(
                record, payload.get("kind"), "push", ok,
                ("%s %s" % (payload.get("action"), payload.get("title")))
                if ok else message)
            await _store(env, repo_bi, record)
            if ok:
                delivered += 1
        except Exception:
            continue
    return delivered


async def run_cron(env, deliveries):
    delivered = await drain_outbox(env, deliveries)
    now = _now()
    rows = await d1_all(
        env, "SELECT repo_bi FROM repo_provider_mirrors WHERE active=1"
        " AND next_sync_at<=? ORDER BY next_sync_at ASC LIMIT ?",
        now, CRON_MIRRORS_PER_TICK)
    synced = 0
    for row in rows or []:
        repo_bi = str(row.get("repo_bi") or "")
        try:
            mirror_row, record = await _load(env, repo_bi)
            if not record:
                await d1_run(
                    env, "UPDATE repo_provider_mirrors SET next_sync_at=?"
                    " WHERE repo_bi=?", now + SYNC_INTERVAL_MS, repo_bi)
                continue
            await _run_sync(env, repo_bi, record)
            synced += 1
        except Exception:
            await d1_run(
                env, "UPDATE repo_provider_mirrors SET next_sync_at=?"
                " WHERE repo_bi=?", now + SYNC_INTERVAL_MS, repo_bi)
    return delivered + synced


async def _run_sync(env, repo_bi, record):
    now = _now()
    summary = await _sync_inbound(env, repo_bi, record)
    ok = not summary["errors"]
    status = dict(record.get("status") or {})
    status["lastSync"] = {"at": now, "ok": ok,
                          "imported": summary["imported"],
                          "comments": summary["comments"],
                          "error": "; ".join(summary["errors"])[:300]}
    record["status"] = status
    if summary["imported"] or summary["comments"] or not ok:
        _note_history(
            record, "sync", "pull", ok,
            "Imported %d item(s) and %d comment(s)" % (
                summary["imported"], summary["comments"])
            if ok else status["lastSync"]["error"])
    await _store(env, repo_bi, record, next_sync_at=now + SYNC_INTERVAL_MS,
                 last_sync_at=now)
    return summary


async def push_config(env, owner, name, aliases=()):
    try:
        for candidate in (owner,) + tuple(aliases or ()):
            repo_bi = await _repo_bi(env, candidate, name)
            row, record = await _load(env, repo_bi)
            if not record or int((row or {}).get("active") or 0) != 1:
                continue
            if not pushes(record.get("resources"), "code"):
                return None
            provider = record["provider"]
            return {
                "revision": str(record.get("revision") or provider),
                "provider": provider,
                "url": remote_push_url(provider, record["remote"]),
                "username": PROVIDERS[provider]["username"],
                "token": record["token"],
                "prune": bool(record.get("prune")),
                "refs": clean_push_refs(record.get("refs")) or "all",
            }
    except Exception:
        return None
    return None


async def record_push(env, owner, name, report, aliases=()):
    if not isinstance(report, dict):
        return False
    try:
        for candidate in (owner,) + tuple(aliases or ()):
            repo_bi = await _repo_bi(env, candidate, name)
            row, record = await _load(env, repo_bi)
            if not record:
                continue
            status = dict(record.get("status") or {})
            previous = dict(status.get("codePush") or {})
            current = {
                "at": int(report.get("at") or 0),
                "ok": bool(report.get("ok")),
                "error": str(report.get("error") or "")[:300],
                "refs": int(report.get("refs") or 0),
                "digest": str(report.get("digest") or "")[:64],
            }
            if previous == current:
                return False
            status["codePush"] = current
            record["status"] = status
            _note_history(
                record, "code", "push", current["ok"],
                "Pushed %d ref(s)" % current["refs"] if current["ok"]
                else current["error"])
            await _store(env, repo_bi, record)
            return True
    except Exception:
        return False
    return False


async def _code_hosted(env, owner, repo):
    try:
        row = await d1_first(
            env, "SELECT id FROM hosted_repos WHERE key_bi=? AND"
            " deleted_at=0", await _repo_bi(env, owner, repo))
        return bool(row)
    except Exception:
        return False


async def _response(env, owner, repo, repo_bi, extra=None):
    row, record = await _load(env, repo_bi)
    pending = await d1_first(
        env, "SELECT COUNT(*) AS c FROM repo_provider_mirror_outbox"
        " WHERE repo_bi=?", repo_bi)
    linked = await d1_all(
        env, "SELECT kind, origin, COUNT(*) AS c FROM"
        " repo_provider_mirror_links WHERE repo_bi=? GROUP BY kind, origin",
        repo_bi)
    counts = {}
    for item in linked or []:
        counts.setdefault(str(item.get("kind")), {})[
            str(item.get("origin"))] = int(item.get("c") or 0)
    body = {
        "ok": True,
        "mirror": public_view(row, record) if record else None,
        "catalog": catalog(),
        "codeHosted": await _code_hosted(env, owner, repo),
        "private": bool(await _repo_is_private(env, owner, repo)),
        "pending": int((pending or {}).get("c") or 0),
        "counts": counts,
    }
    if extra:
        body.update(extra)
    return json_response(body, cache_control="no-store")


async def repo_provider_mirror_handler(env, request, owner, repo):
    if method_name(request) != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    await ensure_schema(env)
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    if not isinstance(data, dict):
        return json_response({"error": "invalid_json"}, status=400)
    if not await _authorize_repo_owner_web(env, request, owner, repo, data):
        return json_response({"error": "not_authorized"}, status=403)
    action = clean_string(data.get("action", "get"), 16).lower()
    owner = str(owner or "").strip().lower()
    repo_bi = await _repo_bi(env, owner, repo)
    row, record = await _load(env, repo_bi)

    if action == "get":
        return await _response(env, owner, repo, repo_bi)

    if action == "save":
        provider = clean_provider(data.get("provider"))
        if not provider:
            return json_response({"error": "invalid_provider"}, status=400)
        remote = clean_remote(provider, data.get("remote"))
        if not remote:
            return json_response({"error": "invalid_remote"}, status=400)
        if "refs" in data and data.get("refs") not in (None, ""):
            refs = clean_push_refs(data.get("refs"))
            if not refs:
                return json_response({"error": "invalid_refs"}, status=400)
        else:
            refs = clean_push_refs((record or {}).get("refs")) or "all"
        token = clean_token(data.get("token"))
        if not token and record and record.get("provider") == provider:
            token = record.get("token", "")
        if not token:
            return json_response({"error": "token_required"}, status=400)
        previous = record or {}
        same_remote = (previous.get("provider") == provider and (
            previous.get("remote") or {}).get("path") == remote["path"])
        updated = {
            "provider": provider, "remote": remote, "token": token,
            "tokenHint": token_hint(token), "owner": owner, "repo": repo,
            "resources": normalize_resources(
                data.get("resources"), provider,
                previous.get("resources") if same_remote else None),
            "prune": bool(data.get("prune")),
            "refs": refs,
            "createdBy": previous.get("createdBy") or clean_string(
                data.get("ownerAccount", ""), MAX_NODE_NAME).lower(),
            "cursors": previous.get("cursors", {}) if same_remote else {},
            "history": list(previous.get("history") or []),
            "status": dict(previous.get("status") or {})
            if same_remote else {},
            "revision": "%s:%d" % (provider, _now()),
        }
        verification = await _verify(updated)
        if verification.get("error"):
            return json_response(
                {"error": "verification_failed",
                 "message": verification["error"]}, status=400)
        created = False
        if not verification.get("remoteExists"):
            if not data.get("createRemote"):
                return json_response(
                    {"error": "remote_missing",
                     "message": "%s has no repository %s. Tick “Create it "
                                "for me” or create it first." % (
                                    PROVIDERS[provider]["label"],
                                    remote["path"])}, status=400)
            private = bool(await _repo_is_private(env, owner, repo))
            created, message = await _create_remote(
                updated, private, data.get("description"))
            if not created:
                return json_response(
                    {"error": "create_failed", "message": message},
                    status=400)
            verification = await _verify(updated)
        status = dict(updated["status"])
        status.update({key: verification.get(key) for key in (
            "login", "verifiedAt", "remoteExists", "remotePrivate",
            "canPush", "error")})
        updated["status"] = status
        _note_history(
            updated, "settings", "", True,
            ("Created %s and connected" % remote["path"]) if created
            else ("Connected to %s" % remote["path"]) if not same_remote
            else "Settings saved")
        await _store(env, repo_bi, updated,
                     active=data.get("active", True) is not False,
                     next_sync_at=0)
        if not same_remote and record:
            await d1_run(env, "DELETE FROM repo_provider_mirror_links"
                         " WHERE repo_bi=?", repo_bi)
            await d1_run(env, "DELETE FROM repo_provider_mirror_outbox"
                         " WHERE repo_bi=?", repo_bi)
        return await _response(env, owner, repo, repo_bi,
                               {"saved": True, "created": created})

    if not record:
        return json_response({"error": "not_configured"}, status=404)

    if action == "toggle":
        await _store(env, repo_bi, record,
                     active=data.get("active", True) is not False,
                     next_sync_at=0)
        return await _response(env, owner, repo, repo_bi)

    if action == "verify":
        verification = await _verify(record)
        status = dict(record.get("status") or {})
        status.update({key: verification.get(key) for key in (
            "login", "verifiedAt", "remoteExists", "remotePrivate",
            "canPush", "error")})
        record["status"] = status
        await _store(env, repo_bi, record)
        return await _response(env, owner, repo, repo_bi)

    if action == "disconnect":
        for table in ("repo_provider_mirror_outbox",
                      "repo_provider_mirror_links", "repo_provider_mirrors"):
            await d1_run(env, "DELETE FROM %s WHERE repo_bi=?" % table,
                         repo_bi)
        return await _response(env, owner, repo, repo_bi,
                               {"disconnected": True})

    if action == "sync":
        await drain_outbox(env, 10)
        summary = await _run_sync(env, repo_bi, record)
        return await _response(env, owner, repo, repo_bi, {"sync": summary})

    kind = clean_string(data.get("kind", ""), 16).lower()
    if kind not in PULL_KINDS or not resource_supported(
            kind, record["provider"]):
        return json_response({"error": "invalid_kind"}, status=400)

    try:
        page = max(1, min(int(data.get("page") or 1), 50))
    except (TypeError, ValueError):
        page = 1

    if action == "candidates":
        items, error = await _list_remote(
            record, kind, MAX_CANDIDATES, 0, page)
        if items is None:
            return json_response(
                {"error": "provider_error", "message": error}, status=502)
        imported = await _imported_keys(env, repo_bi, kind)
        return json_response({
            "ok": True, "kind": kind, "page": page,
            "more": len(items) >= MAX_CANDIDATES and kind != "discussions",
            "items": [candidate_view(item, item["key"] in imported)
                      for item in items],
        }, cache_control="no-store")

    if action == "import":
        keys = data.get("keys")
        if not isinstance(keys, list) or not keys:
            return json_response({"error": "nothing_selected"}, status=400)
        wanted = {str(key) for key in keys[:MAX_IMPORT_PER_REQUEST]}
        items, error = await _list_remote(
            record, kind, MAX_CANDIDATES, 0, page)
        if items is None:
            return json_response(
                {"error": "provider_error", "message": error}, status=502)
        results = {}
        for item in sorted(items, key=lambda i: i["number"]):
            if item["key"] in wanted:
                results[item["key"]] = await _import_item(
                    env, record, repo_bi, item)
        imported = sum(1 for value in results.values() if value == "imported")
        _note_history(record, kind, "pull", True,
                      "Imported %d picked item(s)" % imported)
        await _store(env, repo_bi, record)
        return await _response(env, owner, repo, repo_bi,
                               {"results": results, "imported": imported})

    return json_response({"error": "bad_action"}, status=400)
