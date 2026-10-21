import hashlib
import html
import json
import re
import secrets
import time
from datetime import datetime, timezone
from urllib.parse import parse_qs, quote, unquote, urlparse

import advisory_cvss

STATES = ("triage", "draft", "published", "closed", "withdrawn")
PRIVATE_STATES = ("triage", "draft", "closed")
PUBLIC_STATES = ("published", "withdrawn")
CREDIT_TYPES = (
    "finder", "reporter", "analyst", "coordinator", "remediation_developer",
    "remediation_reviewer", "remediation_verifier", "tool", "sponsor", "other",
)
CREDIT_STATES = ("pending", "accepted", "declined")
OSV_ECOSYSTEMS = (
    "AlmaLinux", "Alpine", "Android", "Bioconductor", "Bitnami", "Chainguard",
    "ConanCenter", "CRAN", "crates.io", "Debian", "GHC", "GitHub Actions",
    "Go", "Hackage", "Hex", "Linux", "Mageia", "Maven", "npm", "NuGet",
    "openSUSE", "OSS-Fuzz", "Packagist", "Photon OS", "Pub", "PyPI",
    "Red Hat", "Rocky Linux", "RubyGems", "SUSE", "SwiftURL", "Ubuntu",
    "Wolfi",
)
OSV_SCHEMA_VERSION = "1.6.0"
LEDGER_FORMAT = "forkmesh-advisory-v1"
LEDGER_DIR = ".forkmesh/advisories"
SIGNING_KIND = "advisory-event"
SIGNED_KINDS = ("publish", "update", "withdraw")
ID_ALPHABET = "23456789cfghjmpqrvwx"
DEFAULT_PREFIX = "FMSA"
PREFIX_RE = re.compile(r"^[A-Z][A-Z0-9]{1,15}$")
ID_RE = re.compile(
    r"^[A-Z][A-Z0-9]{1,15}-[%s]{4}-[%s]{4}-[%s]{4}$"
    % ((ID_ALPHABET,) * 3))
CVE_RE = re.compile(r"^CVE-\d{4}-\d{4,7}$")
CWE_RE = re.compile(r"^(?:CWE-)?(\d{1,5})$", re.I)
URL_RE = re.compile(r"^https?://[^\s<>\"']{3,500}$")
COMMIT_RE = re.compile(r"^[0-9a-f]{7,64}$")
BRANCH_RE = re.compile(r"^[A-Za-z0-9._/+-]{1,200}$")
NAME_RE = re.compile(r"^[a-z0-9][a-z0-9_.-]{0,62}$")
RANGE_PART_RE = re.compile(r"^(>=|>|<=|<|==|=)?\s*([^\s<>=,]{1,100})$")
TITLE_MAX = 200
DESCRIPTION_MAX = 64 * 1024
COMMENT_MAX = 16 * 1024
FIELD_MAX = 200
MAX_AFFECTED = 20
MAX_REFERENCES = 20
MAX_CWES = 10
MAX_COLLABORATORS = 20
MAX_CREDITS = 20
MAX_COMMENTS = 500
# Every report notifies the maintainers and mails the instance's security
# contact, so one account's reports across all repositories are capped too.
MAX_REPORTS_PER_REPORTER_PER_DAY = 20
REPORT_WINDOW_MS = 24 * 60 * 60 * 1000
MAX_HISTORY = 200
MAX_OPEN_REPORTS_PER_REPORTER = 10
MAX_TRIAGE_PER_REPO = 200
LIST_LIMIT = 100
FEED_LIMIT = 500
HALL_ROWS = 5000
REQUEST_MAX_BYTES = 160 * 1024
PREPARE_TTL_MS = 15 * 60 * 1000
SIGNATURE_SKEW_MS = 15 * 60 * 1000
HALL_CACHE_MS = 5 * 60 * 1000
SEVERITY_POINTS = {"critical": 10, "high": 6, "moderate": 3, "low": 1}
NOTIFICATION_KIND = "security_advisory"
NOT_FOUND = {"ok": False, "error": "not_found"}
PUBLIC_PATHS = ("/advisories", "/advisories/", "/advisories.json",
                "/security-researchers", "/security-researchers/")

_hall_cache = {"at": 0, "value": None}
_runtime = {}


def _bind_runtime(runtime):
    _runtime["namespace"] = runtime
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def _now():
    return int(time.time() * 1000)


def _rfc3339(ms):
    return datetime.fromtimestamp(
        int(ms) / 1000.0, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _text(value, limit):
    return str(value if isinstance(value, (str, int, float)) else "")[:limit]


def _line(value, limit=FIELD_MAX):
    return re.sub(r"[\x00-\x1f\x7f]+", " ", _text(value, limit * 2)).strip()[
        :limit]


def _canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False)


def content_hash(osv):
    return hashlib.sha256(_canonical(osv).encode("utf-8")).hexdigest()


def id_prefix(env):
    value = str(getattr(env, "SECURITY_ADVISORY_ID_PREFIX", "") or "")
    value = value.strip().upper()
    return value if PREFIX_RE.match(value) else DEFAULT_PREFIX


def new_id(env):
    groups = ["".join(secrets.choice(ID_ALPHABET) for _ in range(4))
              for _ in range(3)]
    return id_prefix(env) + "-" + "-".join(groups)


def valid_id(value):
    return bool(ID_RE.match(str(value or "")))


def signing_message(repo_key, kind, advisory_id, author, ts, digest):
    body = "\n".join((kind, advisory_id, author, str(int(ts)), digest))
    return ("forkmesh-" + SIGNING_KIND + "-v2\n" + repo_key + "\n"
            + body).encode("utf-8")


def _json(data, status=200, cache="private, no-store"):
    return json_response(data, status=status, cache_control=cache)


def _not_found():
    return _json(NOT_FOUND, 404)


def _error(code, status=400, **extra):
    body = {"ok": False, "error": code}
    body.update(extra)
    return _json(body, status)


def _severity(value):
    value = value if isinstance(value, dict) else {}
    vector = _line(value.get("vector"), 400)
    if vector:
        score, kind = advisory_cvss.score(vector)
        if score is None:
            return None, "invalid_cvss"
        level = advisory_cvss.level_for_score(score)
        return {"vector": vector, "type": kind, "score": score,
                "level": "low" if level == "none" else level}, ""
    level = _line(value.get("level"), 20).lower()
    if level and level not in advisory_cvss.LEVELS:
        return None, "invalid_severity"
    return {"vector": "", "type": "", "score": None, "level": level}, ""


def _cwes(value):
    items = value if isinstance(value, list) else str(value or "").split(",")
    out = []
    for item in items[:MAX_CWES * 2]:
        text = _line(item, 20)
        if not text:
            continue
        match = CWE_RE.match(text)
        if not match:
            return None
        cwe = "CWE-" + str(int(match.group(1)))
        if cwe not in out:
            out.append(cwe)
    return out[:MAX_CWES]


def _affected(value):
    if not isinstance(value, list):
        return []
    out = []
    for item in value[:MAX_AFFECTED]:
        if not isinstance(item, dict):
            continue
        entry = {
            "ecosystem": _line(item.get("ecosystem"), 40),
            "package": _line(item.get("package"), FIELD_MAX),
            "path": _line(item.get("path"), FIELD_MAX),
            "vulnerable": _line(item.get("vulnerable"), FIELD_MAX),
            "patched": _line(item.get("patched"), FIELD_MAX),
        }
        if entry["ecosystem"] and entry["ecosystem"] not in OSV_ECOSYSTEMS:
            return None
        if any(entry.values()):
            out.append(entry)
    return out


def _references(value):
    items = value if isinstance(value, list) else str(value or "").split()
    out = []
    for item in items[:MAX_REFERENCES * 2]:
        text = _line(item, 500)
        if not text:
            continue
        if not URL_RE.match(text):
            return None
        if text not in out:
            out.append(text)
    return out[:MAX_REFERENCES]


def _fix(value):
    value = value if isinstance(value, dict) else {}
    branch = _line(value.get("branch"), 200)
    commit = _line(value.get("commit"), 64).lower()
    try:
        pull = int(value.get("pull") or 0)
    except (TypeError, ValueError):
        return None
    if branch and (not BRANCH_RE.match(branch) or ".." in branch):
        return None
    if commit and not COMMIT_RE.match(commit):
        return None
    if pull < 0 or pull > 10 ** 9:
        return None
    return {"branch": branch, "pull": pull, "commit": commit}


def clean_fields(data, partial=False):
    data = data if isinstance(data, dict) else {}
    out = {}
    if "title" in data or not partial:
        title = _line(data.get("title"), TITLE_MAX)
        if not title:
            return None, "title_required"
        out["title"] = title
    if "description" in data or not partial:
        description = str(data.get("description") or "")
        description = description.replace("\r\n", "\n").replace("\r", "\n")
        try:
            size = len(description.encode("utf-8"))
        except UnicodeEncodeError:
            return None, "invalid_text"
        if size > DESCRIPTION_MAX:
            return None, "description_too_large"
        out["description"] = description.strip("\n")
    if "severity" in data or not partial:
        severity, error = _severity(data.get("severity"))
        if error:
            return None, error
        out["severity"] = severity
    if "cwes" in data or not partial:
        cwes = _cwes(data.get("cwes"))
        if cwes is None:
            return None, "invalid_cwe"
        out["cwes"] = cwes
    if "affected" in data or not partial:
        affected = _affected(data.get("affected"))
        if affected is None:
            return None, "invalid_ecosystem"
        out["affected"] = affected
    if "references" in data or not partial:
        references = _references(data.get("references"))
        if references is None:
            return None, "invalid_reference"
        out["references"] = references
    if "cve" in data or not partial:
        cve = _line(data.get("cve"), 20).upper()
        if cve and not CVE_RE.match(cve):
            return None, "invalid_cve"
        out["cve"] = cve
    if "fix" in data or not partial:
        fix = _fix(data.get("fix"))
        if fix is None:
            return None, "invalid_fix"
        out["fix"] = fix
    return out, ""


def has_patched_version(record):
    return any(entry.get("patched") for entry in record.get("affected") or [])


def public_view(record, credits):
    return {
        "id": record["id"],
        "repository": record.get("repository", ""),
        "state": record.get("state", ""),
        "title": record.get("title", ""),
        "description": record.get("description", ""),
        "severity": record.get("severity") or {},
        "cwes": list(record.get("cwes") or []),
        "affected": list(record.get("affected") or []),
        "references": list(record.get("references") or []),
        "cve": record.get("cve", ""),
        "fix": record.get("fix") or {},
        "credits": [{"user": item["user"], "type": item["type"]}
                    for item in credits if item.get("state") == "accepted"],
        "publishedAt": int(record.get("publishedAt") or 0),
        "updatedAt": int(record.get("updatedAt") or 0),
        "withdrawnAt": int(record.get("withdrawnAt") or 0),
        "ledger": {key: value for key, value in (
            record.get("ledger") or {}).items()
            if key in ("state", "path", "commit")},
    }


def _range_events(vulnerable, patched):
    introduced = "0"
    fixed = ""
    last = ""
    exact = []
    for part in [p.strip() for p in str(vulnerable or "").split(",")]:
        if not part:
            continue
        match = RANGE_PART_RE.match(part)
        if not match:
            continue
        operator, version = match.group(1) or "=", match.group(2)
        if operator in (">=", ">"):
            introduced = version
        elif operator == "<":
            fixed = version
        elif operator == "<=":
            last = version
        else:
            exact.append(version)
    if not fixed and not last:
        patched_first = [p.strip() for p in str(patched or "").split(",")
                         if p.strip()]
        if patched_first:
            fixed = patched_first[0]
    events = [{"introduced": introduced}]
    if fixed:
        events.append({"fixed": fixed})
    elif last:
        events.append({"last_affected": last})
    return events, exact


def osv_record(view, origin):
    origin = str(origin or "").rstrip("/")
    repo_url = origin + "/" + view["repository"]
    advisory_url = repo_url + "/security/advisories/" + view["id"]
    fix = view.get("fix") or {}
    osv = {
        "schema_version": OSV_SCHEMA_VERSION,
        "id": view["id"],
        "modified": _rfc3339(view["updatedAt"]),
        "published": _rfc3339(view["publishedAt"]),
        "summary": view["title"],
        "details": view["description"],
    }
    if view.get("withdrawnAt"):
        osv["withdrawn"] = _rfc3339(view["withdrawnAt"])
    if view.get("cve"):
        osv["aliases"] = [view["cve"]]
    severity = view.get("severity") or {}
    if severity.get("vector"):
        osv["severity"] = [{"type": severity["type"],
                            "score": severity["vector"]}]
    affected = []
    for entry in view.get("affected") or []:
        events, exact = _range_events(entry.get("vulnerable"),
                                      entry.get("patched"))
        item = {}
        if entry.get("ecosystem") and entry.get("package"):
            item["package"] = {"ecosystem": entry["ecosystem"],
                               "name": entry["package"]}
            item["ranges"] = [{"type": "ECOSYSTEM", "events": events}]
            if exact:
                item["versions"] = exact
        else:
            git_events = [{"introduced": "0"}]
            if fix.get("commit") and len(fix["commit"]) >= 40:
                git_events.append({"fixed": fix["commit"]})
            item["ranges"] = [{"type": "GIT", "repo": repo_url,
                               "events": git_events}]
        item["database_specific"] = {
            key: entry[key] for key in ("path", "vulnerable", "patched")
            if entry.get(key)
        }
        affected.append(item)
    if affected:
        osv["affected"] = affected
    references = [{"type": "ADVISORY", "url": advisory_url}]
    references.extend({"type": "WEB", "url": url}
                      for url in view.get("references") or [])
    if fix.get("pull"):
        references.append({"type": "FIX", "url": "%s/pulls/%d" % (
            repo_url, int(fix["pull"]))})
    if fix.get("commit"):
        references.append({"type": "FIX",
                           "url": repo_url + "/commits/" + fix["commit"]})
    references.append({"type": "PACKAGE", "url": repo_url})
    osv["references"] = references
    if view.get("credits"):
        osv["credits"] = [{"name": item["user"],
                           "contact": [origin + "/" + item["user"]],
                           "type": item["type"].upper()}
                          for item in view["credits"]]
    specific = {"source": advisory_url, "repository": view["repository"]}
    if severity.get("level"):
        specific["severity"] = severity["level"].upper()
    if view.get("cwes"):
        specific["cwe_ids"] = list(view["cwes"])
    osv["database_specific"] = specific
    return osv


def ledger_document(record):
    events = []
    for event in (record.get("ledger") or {}).get("events") or []:
        events.append({key: event[key] for key in (
            "type", "ts", "author", "authorName", "repoKey", "contentHash",
            "sig", "osv") if key in event})
    return {
        "format": LEDGER_FORMAT,
        "id": record["id"],
        "repository": record.get("repository", ""),
        "events": events,
    }


def ledger_path(advisory_id):
    return LEDGER_DIR + "/" + advisory_id + ".json"


def _origin(env):
    try:
        return str(_public_base_url(env) or "").rstrip("/")
    except Exception:
        return ""


def _repo_href(repository, advisory_id=""):
    href = "/" + "/".join(quote(part) for part in repository.split("/"))
    href += "/security"
    if advisory_id:
        href += "/advisories/" + advisory_id
    return href


async def _maintainer_level(env, actor, owner, repo):
    if not actor:
        return ""
    if await _account_owns_node(env, actor, owner):
        return "admin"
    permission = await _org_linked_repo_permission(env, owner, repo, actor)
    rank = team_permission_rank(permission)
    if rank >= team_permission_rank("admin"):
        return "admin"
    if rank >= team_permission_rank("maintain"):
        return "maintain"
    return ""


async def _session(env, request, data=None):
    try:
        _, rec = await _account_session_record(env, request, data)
    except RuntimeError:
        raise
    except Exception:
        rec = None
    rec = rec or {}
    name = str(rec.get("name") or "").strip().lower()
    return name, rec


async def context(env, request, owner, repo, data=None):
    actor, session = await _session(env, request, data)
    access = await _repository_access_context(
        env, request, owner, repo, viewer=actor)
    if not access:
        return None
    public_owner = await _public_repo_owner(
        env, request, owner, repo, REPO_API_PREFIX_RE)
    level = await _maintainer_level(env, actor, owner, repo)
    return {
        "owner": owner,
        "repo": repo,
        "repository": (public_owner or owner) + "/" + repo,
        "repoBi": access["repoBi"],
        "visibility": access.get("visibility", "private"),
        "actor": actor,
        "verified": session.get("email_verified") is True,
        "maintainer": bool(level),
        "admin": level == "admin",
    }


async def _reporting_enabled(env, ctx):
    row = await d1_first(
        env, "SELECT reporting FROM security_advisory_settings WHERE repo_bi=?",
        ctx["repoBi"])
    if row:
        return bool(int(row.get("reporting") or 0))
    return ctx["visibility"] == "public"


async def _member_role(env, advisory_id, actor):
    if not actor:
        return ""
    row = await d1_first(
        env, "SELECT role FROM security_advisory_members "
        "WHERE advisory_id=? AND member_bi=?",
        advisory_id, await blind_index(env, actor))
    return str((row or {}).get("role") or "")


async def load(env, ctx, advisory_id):
    if not valid_id(advisory_id):
        return None
    row = await d1_first(
        env, "SELECT id, state, data, public_data FROM security_advisories "
        "WHERE id=? AND repo_bi=?", advisory_id, ctx["repoBi"])
    if not row:
        return None
    role = "maintainer" if ctx["maintainer"] else await _member_role(
        env, advisory_id, ctx["actor"])
    state = str(row.get("state") or "")
    if not role and state not in PUBLIC_STATES:
        return None
    record = await decrypt_row(env, row.get("data")) if role else None
    if role and not isinstance(record, dict):
        return None
    public = None
    if state in PUBLIC_STATES:
        try:
            public = json.loads(row.get("public_data") or "null")
        except ValueError:
            public = None
        if not isinstance(public, dict):
            return None
    return {"row": row, "record": record, "role": role, "state": state,
            "public": public}


async def _credits(env, advisory_id):
    rows = await d1_all(
        env, "SELECT state, data FROM security_advisory_credits "
        "WHERE advisory_id=? ORDER BY updated_at ASC", advisory_id)
    out = []
    for row in rows or []:
        data = await decrypt_row(env, row.get("data"))
        if isinstance(data, dict) and data.get("user"):
            out.append({"user": data["user"], "type": data.get("type", ""),
                        "state": str(row.get("state") or "pending")})
    return out


async def _store(env, record, credits=None):
    record["revision"] = int(record.get("revision") or 0) + 1
    public = ""
    if record["state"] in PUBLIC_STATES:
        if credits is None:
            credits = await _credits(env, record["id"])
        public = json.dumps(public_view(record, credits),
                            separators=(",", ":"), ensure_ascii=False)
    await d1_run(
        env,
        "UPDATE security_advisories SET state=?, updated_at=?, "
        "published_at=?, data=?, public_data=? WHERE id=?",
        record["state"], int(record["updatedAt"]),
        int(record.get("publishedAt") or 0), await encrypt_row(env, record),
        public, record["id"])
    _hall_cache["value"] = None


def _history(record, actor, action, detail=""):
    entries = list(record.get("history") or [])
    entries.append({"ts": _now(), "actor": actor, "action": action,
                    "detail": detail})
    record["history"] = entries[-MAX_HISTORY:]


def _summary(record=None, public=None, state=""):
    source = public if public is not None else record
    severity = source.get("severity") or {}
    return {
        "id": source["id"],
        "title": source.get("title", ""),
        "state": state or source.get("state", ""),
        "severity": {"level": severity.get("level", ""),
                     "score": severity.get("score")},
        "cve": source.get("cve", ""),
        "updatedAt": int(source.get("updatedAt") or 0),
        "publishedAt": int(source.get("publishedAt") or 0),
        "reporter": (record or {}).get("reporter", "") if record else "",
    }


def permissions(role, state, ctx):
    maintainer = role == "maintainer"
    member = bool(role)
    edit = maintainer or (
        role == "collaborator" and state in ("triage", "draft")) or (
        role == "reporter" and state == "triage")
    return {
        "edit": bool(edit and state != "withdrawn"),
        "comment": member,
        "manage": maintainer,
        "publish": maintainer and state == "draft",
        "withdraw": maintainer and state == "published",
        "admin": bool(ctx.get("admin")),
    }


async def _recipients(env, ctx, record, exclude=""):
    names = []
    try:
        names.extend(await _org_fanout_recipients(
            env, ctx["owner"], ctx["repo"]))
    except Exception:
        names.append(ctx["owner"])
    if record.get("reporter"):
        names.append(record["reporter"])
    names.extend(record.get("collaborators") or [])
    out = []
    for name in names:
        name = str(name or "").strip().lower()
        if name and name != exclude and name not in out:
            out.append(name)
    return out


async def _notify(env, ctx, record, recipients, title, body, actor, tag):
    href = _repo_href(ctx["repository"], record["id"])
    for name in recipients:
        try:
            await enqueue_notification(
                env, name, NOTIFICATION_KIND, title, body=body,
                repo=ctx["repository"], href=href, actor=actor,
                source="security_advisory",
                dedupe="security-advisory:%s:%s:%s:%d" % (
                    record["id"], tag, name, _now() // 60000))
        except Exception:
            pass


async def _email_security_contact(env, ctx, record):
    dest = str(getattr(env, "SECURITY_EMAIL", "") or "").strip()
    if not dest or "@" not in dest:
        return
    severity = (record.get("severity") or {}).get("level") or "not set"
    text = (
        "A private vulnerability report was filed against %s.\n\n"
        "Advisory: %s\nSeverity: %s\n%s%s\n\n"
        "The report itself is not sent by email: the repository's "
        "maintainers read it at the link.\n"
        % (ctx["repository"], record["id"], severity, _origin(env),
           _repo_href(ctx["repository"], record["id"])))
    try:
        await _send_email(
            env, dest, "[Security] Vulnerability report for "
            + ctx["repository"], text, email_kind="security_report")
    except Exception:
        pass


async def overview(env, request, ctx):
    published_rows = await d1_all(
        env, "SELECT state, public_data FROM security_advisories "
        "WHERE repo_bi=? AND state IN ('published','withdrawn') "
        "ORDER BY published_at DESC LIMIT ?", ctx["repoBi"], LIST_LIMIT)
    published = []
    for row in published_rows or []:
        try:
            view = json.loads(row.get("public_data") or "null")
        except ValueError:
            continue
        if isinstance(view, dict) and view.get("id"):
            published.append(_summary(public=view, state=row.get("state")))
    private = []
    if ctx["maintainer"]:
        rows = await d1_all(
            env, "SELECT data FROM security_advisories WHERE repo_bi=? "
            "AND state IN ('triage','draft','closed') "
            "ORDER BY updated_at DESC LIMIT ?", ctx["repoBi"], LIST_LIMIT)
    elif ctx["actor"]:
        rows = await d1_all(
            env, "SELECT a.data AS data FROM security_advisory_members m "
            "JOIN security_advisories a ON a.id=m.advisory_id "
            "WHERE m.repo_bi=? AND m.member_bi=? "
            "AND a.state IN ('triage','draft','closed') "
            "ORDER BY a.updated_at DESC LIMIT ?",
            ctx["repoBi"], await blind_index(env, ctx["actor"]), LIST_LIMIT)
    else:
        rows = []
    for row in rows or []:
        record = await decrypt_row(env, row.get("data"))
        if isinstance(record, dict) and record.get("id"):
            private.append(_summary(record=record))
    enabled = await _reporting_enabled(env, ctx)
    return _json({
        "ok": True,
        "repository": ctx["repository"],
        "viewer": ctx["actor"],
        "maintainer": ctx["maintainer"],
        "admin": ctx["admin"],
        "reportingEnabled": enabled,
        "canReport": bool(ctx["actor"] and ctx["verified"] and (
            enabled or ctx["maintainer"])),
        "visibility": ctx["visibility"],
        "published": published,
        "private": private,
        "idPrefix": id_prefix(env),
    })


async def update_settings(env, request, ctx, data):
    if not ctx["admin"]:
        return _not_found() if not ctx["actor"] else _error("forbidden", 403)
    enabled = data.get("reporting") is True
    await d1_run(
        env, "INSERT INTO security_advisory_settings (repo_bi, reporting, "
        "updated_at) VALUES (?,?,?) ON CONFLICT(repo_bi) DO UPDATE SET "
        "reporting=excluded.reporting, updated_at=excluded.updated_at",
        ctx["repoBi"], 1 if enabled else 0, _now())
    return _json({"ok": True, "reportingEnabled": enabled})


async def create(env, request, ctx, data):
    if not ctx["actor"]:
        return _error("sign_in_required", 401)
    if not ctx["verified"]:
        return _error("email_unverified", 403)
    if not ctx["maintainer"] and not await _reporting_enabled(env, ctx):
        return _error("reporting_disabled", 403)
    fields, error = clean_fields(data)
    if error:
        return _error(error)
    if not ctx["maintainer"]:
        # The fix and the CVE alias are the maintainers' to state.
        fields.pop("fix", None)
        fields.pop("cve", None)
    actor = ctx["actor"]
    actor_bi = await blind_index(env, actor)
    if not ctx["maintainer"]:
        recent = await d1_first(
            env, "SELECT COUNT(*) AS n FROM security_advisory_members m "
            "JOIN security_advisories a ON a.id=m.advisory_id "
            "WHERE m.member_bi=? AND m.role='reporter' AND a.created_at>?",
            actor_bi, _now() - REPORT_WINDOW_MS)
        if int((recent or {}).get("n") or 0) >= \
                MAX_REPORTS_PER_REPORTER_PER_DAY:
            return _error("too_many_reports", 429)
        open_rows = await d1_first(
            env, "SELECT COUNT(*) AS n FROM security_advisory_members m "
            "JOIN security_advisories a ON a.id=m.advisory_id "
            "WHERE m.repo_bi=? AND m.member_bi=? AND m.role='reporter' "
            "AND a.state='triage'", ctx["repoBi"], actor_bi)
        if int((open_rows or {}).get("n") or 0) >= \
                MAX_OPEN_REPORTS_PER_REPORTER:
            return _error("too_many_open_reports", 429)
        triage = await d1_first(
            env, "SELECT COUNT(*) AS n FROM security_advisories "
            "WHERE repo_bi=? AND state='triage'", ctx["repoBi"])
        if int((triage or {}).get("n") or 0) >= MAX_TRIAGE_PER_REPO:
            return _error("inbox_full", 429)
    now = _now()
    record = dict(fields)
    record.update({
        "id": new_id(env),
        "repository": ctx["repository"],
        "host": ctx["owner"] + "/" + ctx["repo"],
        "state": "draft" if ctx["maintainer"] else "triage",
        "reporter": "" if ctx["maintainer"] else actor,
        "createdBy": actor,
        "collaborators": [],
        "createdAt": now,
        "updatedAt": now,
        "publishedAt": 0,
        "withdrawnAt": 0,
        "ledger": {},
        "history": [],
    })
    _history(record, actor, "created" if ctx["maintainer"] else "reported")
    await d1_run(
        env, "INSERT INTO security_advisories (id, repo_bi, state, "
        "created_at, updated_at, published_at, data, public_data) "
        "VALUES (?,?,?,?,?,0,?,'')", record["id"], ctx["repoBi"],
        record["state"], now, now, await encrypt_row(env, record))
    if not ctx["maintainer"]:
        await d1_run(
            env, "INSERT INTO security_advisory_members (advisory_id, "
            "repo_bi, member_bi, role, data) VALUES (?,?,?,?,?)",
            record["id"], ctx["repoBi"], actor_bi, "reporter",
            await encrypt_row(env, {"name": actor}))
        await d1_run(
            env, "INSERT INTO security_advisory_credits (advisory_id, "
            "repo_bi, user_bi, state, updated_at, data) VALUES (?,?,?,?,?,?)",
            record["id"], ctx["repoBi"], actor_bi, "accepted", now,
            await encrypt_row(env, {"user": actor, "type": "reporter"}))
        await _notify(
            env, ctx, record, await _recipients(env, ctx, record, actor),
            "Private vulnerability report in " + ctx["repository"],
            record["title"], actor, "reported")
        await _email_security_contact(env, ctx, record)
    return _json({"ok": True, "id": record["id"], "state": record["state"],
                  "href": _repo_href(ctx["repository"], record["id"])},
                 201)


async def _comments(env, advisory_id):
    rows = await d1_all(
        env, "SELECT id, ts, data FROM security_advisory_comments "
        "WHERE advisory_id=? ORDER BY id ASC LIMIT ?", advisory_id,
        MAX_COMMENTS)
    out = []
    for row in rows or []:
        data = await decrypt_row(env, row.get("data"))
        if isinstance(data, dict):
            out.append({"id": int(row.get("id") or 0),
                        "ts": int(row.get("ts") or 0),
                        "author": data.get("author", ""),
                        "body": data.get("body", "")})
    return out


async def show(env, request, ctx, advisory_id):
    found = await load(env, ctx, advisory_id)
    if not found:
        return _not_found()
    role, state = found["role"], found["state"]
    credits = await _credits(env, advisory_id)
    viewer_credit = next(
        (item for item in credits if item["user"] == ctx["actor"]), None)
    body = {
        "ok": True,
        "viewer": ctx["actor"],
        "role": role,
        "repository": ctx["repository"],
        "permissions": permissions(role, state, ctx),
        "viewerCredit": viewer_credit,
        "osvUrl": "/advisories/%s.json" % advisory_id
        if state in PUBLIC_STATES else "",
    }
    if role:
        record = found["record"]
        body["advisory"] = dict(
            {key: value for key, value in record.items() if key != "ledger"},
            repository=ctx["repository"])
        body["credits"] = credits
        body["comments"] = await _comments(env, advisory_id)
        body["warnings"] = [] if has_patched_version(record) else [
            "no_patched_version"]
        body["ledger"] = {key: value for key, value in (
            record.get("ledger") or {}).items() if key != "events"}
    else:
        body["advisory"] = found["public"]
        body["credits"] = [dict(item, state="accepted")
                           for item in found["public"].get("credits") or []]
        body["comments"] = []
        body["ledger"] = found["public"].get("ledger") or {}
    return _json(body, cache="private, no-store")


async def _edit(env, ctx, found, data):
    record = found["record"]
    if not permissions(found["role"], found["state"], ctx)["edit"]:
        return _error("forbidden", 403)
    if found["state"] in PUBLIC_STATES:
        return _error("signature_required", 409)
    fields, error = clean_fields(data.get("fields"), partial=True)
    if error:
        return _error(error)
    if not ctx["maintainer"]:
        fields.pop("fix", None)
        fields.pop("cve", None)
    record.update(fields)
    record["updatedAt"] = _now()
    _history(record, ctx["actor"], "edited")
    await _store(env, record)
    return _json({"ok": True})


async def _comment(env, ctx, found, data):
    body = str(data.get("body") or "").replace("\r\n", "\n").strip()
    if not body:
        return _error("comment_required")
    if len(body.encode("utf-8")) > COMMENT_MAX:
        return _error("comment_too_large", 413)
    count = await d1_first(
        env, "SELECT COUNT(*) AS n FROM security_advisory_comments "
        "WHERE advisory_id=?", found["record"]["id"])
    if int((count or {}).get("n") or 0) >= MAX_COMMENTS:
        return _error("too_many_comments", 429)
    now = _now()
    await d1_run(
        env, "INSERT INTO security_advisory_comments (advisory_id, repo_bi, "
        "ts, data) VALUES (?,?,?,?)", found["record"]["id"], ctx["repoBi"],
        now, await encrypt_row(env, {"author": ctx["actor"], "body": body}))
    record = found["record"]
    await _notify(
        env, ctx, record, await _recipients(env, ctx, record, ctx["actor"]),
        "New comment on security advisory " + record["id"],
        record.get("title", ""), ctx["actor"], "comment")
    return _json({"ok": True, "ts": now}, 201)


STATE_TRANSITIONS = {
    ("triage", "draft"): "accepted",
    ("triage", "closed"): "closed",
    ("draft", "closed"): "closed",
    ("closed", "draft"): "reopened",
}


async def _transition(env, ctx, found, data):
    record = found["record"]
    target = _line(data.get("state"), 20)
    action = STATE_TRANSITIONS.get((found["state"], target))
    if not action:
        return _error("invalid_transition", 409)
    record["state"] = target
    record["updatedAt"] = _now()
    _history(record, ctx["actor"], action)
    await _store(env, record)
    await _notify(
        env, ctx, record, await _recipients(env, ctx, record, ctx["actor"]),
        "Security advisory %s was %s" % (record["id"], action),
        record.get("title", ""), ctx["actor"], action)
    return _json({"ok": True, "state": target})


async def _account_exists(env, name):
    try:
        _, rec = await _account_row(env, name)
    except Exception:
        return False
    return bool(rec)


async def _collaborators(env, ctx, found, data):
    record = found["record"]
    name = _line(data.get("name"), 63).lower().lstrip("@")
    if not NAME_RE.match(name):
        return _error("invalid_name")
    current = list(record.get("collaborators") or [])
    member_bi = await blind_index(env, name)
    if data.get("remove") is True:
        if name not in current:
            return _error("not_a_collaborator", 404)
        current.remove(name)
        await d1_run(
            env, "DELETE FROM security_advisory_members WHERE advisory_id=? "
            "AND member_bi=? AND role='collaborator'", record["id"],
            member_bi)
        action = "removed collaborator"
    else:
        if name in current or name == record.get("reporter"):
            return _error("already_a_member", 409)
        if len(current) >= MAX_COLLABORATORS:
            return _error("too_many_collaborators", 429)
        if not await _account_exists(env, name):
            return _error("unknown_account", 404)
        current.append(name)
        await d1_run(
            env, "INSERT INTO security_advisory_members (advisory_id, "
            "repo_bi, member_bi, role, data) VALUES (?,?,?,?,?) "
            "ON CONFLICT(advisory_id, member_bi) DO NOTHING",
            record["id"], ctx["repoBi"], member_bi, "collaborator",
            await encrypt_row(env, {"name": name}))
        action = "added collaborator"
        await _notify(
            env, ctx, record, [name],
            "You were added to security advisory " + record["id"],
            record.get("title", ""), ctx["actor"], "collaborator")
    record["collaborators"] = current
    record["updatedAt"] = _now()
    _history(record, ctx["actor"], action, name)
    await _store(env, record)
    return _json({"ok": True, "collaborators": current})


async def _credit(env, ctx, found, data):
    record = found["record"]
    name = _line(data.get("user"), 63).lower().lstrip("@")
    if not NAME_RE.match(name):
        return _error("invalid_name")
    user_bi = await blind_index(env, name)
    if data.get("remove") is True:
        await d1_run(
            env, "DELETE FROM security_advisory_credits WHERE advisory_id=? "
            "AND user_bi=?", record["id"], user_bi)
        _history(record, ctx["actor"], "removed credit", name)
    else:
        kind = _line(data.get("type"), 40).lower()
        if kind not in CREDIT_TYPES:
            return _error("invalid_credit_type")
        existing = await d1_first(
            env, "SELECT state FROM security_advisory_credits "
            "WHERE advisory_id=? AND user_bi=?", record["id"], user_bi)
        if not existing:
            total = await d1_first(
                env, "SELECT COUNT(*) AS n FROM security_advisory_credits "
                "WHERE advisory_id=?", record["id"])
            if int((total or {}).get("n") or 0) >= MAX_CREDITS:
                return _error("too_many_credits", 429)
            if not await _account_exists(env, name):
                return _error("unknown_account", 404)
        state = str((existing or {}).get("state") or "")
        if not state:
            state = "accepted" if name == ctx["actor"] else "pending"
        await d1_run(
            env, "INSERT INTO security_advisory_credits (advisory_id, "
            "repo_bi, user_bi, state, updated_at, data) VALUES (?,?,?,?,?,?) "
            "ON CONFLICT(advisory_id, user_bi) DO UPDATE SET "
            "data=excluded.data, updated_at=excluded.updated_at",
            record["id"], ctx["repoBi"], user_bi, state, _now(),
            await encrypt_row(env, {"user": name, "type": kind}))
        _history(record, ctx["actor"], "credited",
                 name + " (" + kind.replace("_", " ") + ")")
        if state == "pending" and (
                found["state"] in PUBLIC_STATES
                or await _member_role(env, record["id"], name)):
            await _notify_credit(env, ctx, record, name, ctx["actor"])
    record["updatedAt"] = _now()
    await _store(env, record)
    return _json({"ok": True, "credits": await _credits(env, record["id"])})


async def _notify_credit(env, ctx, record, name, actor):
    await _notify(
        env, ctx, record, [name],
        "You were credited on security advisory " + record["id"],
        "Accept or decline the credit: " + record.get("title", ""), actor,
        "credit")


def _candidate(record, kind, fields, prepared_at):
    candidate = json.loads(json.dumps(record))
    if kind in ("publish", "update") and fields:
        candidate.update(fields)
    candidate["updatedAt"] = prepared_at
    if kind == "publish":
        candidate["state"] = "published"
        candidate["publishedAt"] = prepared_at
    elif kind == "withdraw":
        candidate["state"] = "withdrawn"
        candidate["withdrawnAt"] = prepared_at
    return candidate


async def _signed_candidate(env, ctx, found, data, signing=False):
    kind = _line(data.get("kind"), 20)
    if kind not in SIGNED_KINDS:
        return None, _error("invalid_kind")
    state = found["state"]
    if kind == "publish" and state != "draft":
        return None, _error("publish_requires_draft", 409)
    if kind in ("update", "withdraw") and state != "published":
        return None, _error("not_published", 409)
    if ctx["visibility"] != "public":
        return None, _error("repository_private", 409)
    fields = {}
    if kind == "update" or (kind == "publish" and data.get("fields")):
        fields, error = clean_fields(data.get("fields"), partial=True)
        if error:
            return None, _error(error)
    try:
        prepared_at = int(data.get("preparedAt") or 0)
    except (TypeError, ValueError):
        prepared_at = 0
    now = _now()
    if not prepared_at:
        prepared_at = max(now, int(found["record"].get("updatedAt") or 0) + 1)
    if prepared_at <= int(found["record"].get("updatedAt") or 0) or \
            abs(now - prepared_at) > PREPARE_TTL_MS:
        return None, _error("stale_prepare", 409)
    revision = int(found["record"].get("revision") or 0)
    try:
        echoed = int(data.get("revision"))
    except (TypeError, ValueError):
        echoed = -1
    if signing and echoed != revision:
        return None, _error("stale_prepare", 409)
    candidate = _candidate(found["record"], kind, fields, prepared_at)
    credits = await _credits(env, candidate["id"])
    osv = osv_record(public_view(candidate, credits), _origin(env))
    return {"kind": kind, "preparedAt": prepared_at, "candidate": candidate,
            "revision": revision,
            "credits": credits, "osv": osv, "contentHash": content_hash(osv),
            "fields": fields}, None


async def _prepare(env, request, ctx, found, data):
    prepared, refusal = await _signed_candidate(env, ctx, found, data)
    if refusal:
        return refusal
    keys = _event_signing_repo_keys(request, ctx["owner"], ctx["repo"])
    warnings = [] if has_patched_version(prepared["candidate"]) else [
        "no_patched_version"]
    return _json({
        "ok": True,
        "kind": prepared["kind"],
        "preparedAt": prepared["preparedAt"],
        "revision": prepared["revision"],
        "contentHash": prepared["contentHash"],
        "repoKey": keys[0] if keys else "",
        "signingPrefix": "forkmesh-" + SIGNING_KIND + "-v2",
        "osv": prepared["osv"],
        "warnings": warnings,
    })


async def _verify_event(env, request, ctx, prepared, event, advisory_id):
    author = str(event.get("author") or "").strip()
    sig = str(event.get("sig") or "").strip()
    try:
        ts = int(event.get("ts") or 0)
    except (TypeError, ValueError):
        return False, ""
    if not author or not sig or abs(_now() - ts) > SIGNATURE_SKEW_MS:
        return False, ""
    for key in _event_signing_repo_keys(request, ctx["owner"], ctx["repo"]):
        message = signing_message(key, prepared["kind"], advisory_id, author,
                                  ts, prepared["contentHash"])
        if await ed25519_verify(author, sig, message):
            return True, key
    return False, ""


async def _signed(env, request, ctx, found, data):
    prepared, refusal = await _signed_candidate(
        env, ctx, found, data, signing=True)
    if refusal:
        return refusal
    event = data.get("event") if isinstance(data.get("event"), dict) else {}
    record = found["record"]
    if str(event.get("contentHash") or "") != prepared["contentHash"]:
        return _error("content_changed", 409)
    ok, repo_key = await _verify_event(
        env, request, ctx, prepared, event, record["id"])
    if not ok:
        return _error("bad_signature", 401)
    bound = {"author": str(event.get("author") or "").strip()}
    refused = await _bind_session_author(
        env, request, data, bound, session=ctx["actor"])
    if refused is not None:
        return refused
    candidate = prepared["candidate"]
    ledger = dict(candidate.get("ledger") or {})
    events = list(ledger.get("events") or [])
    events.append({
        "type": prepared["kind"],
        "ts": int(event.get("ts")),
        "author": bound["author"],
        "authorName": ctx["actor"],
        "repoKey": repo_key,
        "contentHash": prepared["contentHash"],
        "sig": str(event.get("sig")),
        "osv": prepared["osv"],
    })
    ledger.update({"events": events, "state": "pending", "error": ""})
    candidate["ledger"] = ledger
    _history(candidate, ctx["actor"], {
        "publish": "published", "update": "updated published advisory",
        "withdraw": "withdrew"}[prepared["kind"]])
    await _store(env, candidate, prepared["credits"])
    await _record_ledger(env, request, ctx, candidate)
    if prepared["kind"] == "publish":
        for item in prepared["credits"]:
            if item["state"] == "pending":
                await _notify_credit(env, ctx, candidate, item["user"],
                                     ctx["actor"])
        await _notify(
            env, ctx, candidate,
            await _recipients(env, ctx, candidate, ctx["actor"]),
            "Security advisory %s was published" % candidate["id"],
            candidate.get("title", ""), ctx["actor"], "published")
    return _json({
        "ok": True,
        "state": candidate["state"],
        "ledger": {key: value for key, value in candidate["ledger"].items()
                   if key != "events"},
        "warnings": [] if has_patched_version(candidate) else [
            "no_patched_version"],
    })


async def ledger_commit(env, request, ctx, path, content, message):
    import repository_file_edits as edits
    edits._bind_runtime(_runtime.get("namespace") or globals())
    owner, repo, actor = ctx["owner"], ctx["repo"], ctx["actor"]
    branches = await edits._read(env, owner, repo, "branches")
    branch = str((branches or {}).get("defaultBranch") or "").strip()
    if not branch or not edits.valid_branch(branch):
        raise edits.FileEditError(
            "branch_not_found", "The default branch is not available.", 503)
    parent, listings, _ = await edits._listings(
        env, owner, repo, branch, path, "", allow_missing=True)
    entry = edits._current(listings, path)
    if entry is not None and entry.get("type") != "blob":
        raise edits.FileEditError(
            "invalid_path", "The ledger path is not a file.", 409)
    email = edits.noreply_email(actor, edits._public_base_url(env))
    commit_oid, _, pack = edits.build_commit(
        listings, parent, path, content, actor, email, int(time.time()),
        message, create=entry is None)
    body = edits.receive_pack_body(
        parent, commit_oid, "refs/heads/" + branch, pack)
    await edits._push(env, request, owner, repo, actor, branch, body, "")
    return commit_oid


async def _record_ledger(env, request, ctx, record):
    ledger = dict(record.get("ledger") or {})
    if not ledger.get("events"):
        return record
    path = ledger_path(record["id"])
    content = json.dumps(ledger_document(record), indent=2, sort_keys=True,
                         ensure_ascii=False) + "\n"
    last = ledger["events"][-1]
    message = "Security advisory %s: %s" % (record["id"], last["type"])
    try:
        commit = await ledger_commit(env, request, ctx, path, content, message)
        ledger.update({"state": "recorded", "commit": commit, "path": path,
                       "recordedAt": _now(), "error": "",
                       "recordedEvents": len(ledger["events"])})
    except Exception as exc:
        ledger.update({"state": "pending", "path": path,
                       "error": str(getattr(exc, "code", "") or "unavailable")})
    record["ledger"] = ledger
    await d1_run(
        env, "UPDATE security_advisories SET data=? WHERE id=?",
        await encrypt_row(env, record), record["id"])
    if record.get("state") in PUBLIC_STATES:
        await _refresh_public(env, record["id"])
    return record


async def _ledger_retry(env, request, ctx, found):
    record = found["record"]
    ledger = record.get("ledger") or {}
    if not ledger.get("events"):
        return _error("nothing_to_record", 409)
    if ledger.get("state") != "pending":
        # Each attempt writes a commit to the default branch.
        return _error("already_recorded", 409)
    record = await _record_ledger(env, request, ctx, record)
    return _json({"ok": True, "ledger": {
        key: value for key, value in record["ledger"].items()
        if key != "events"}})


async def act(env, request, ctx, advisory_id, data):
    found = await load(env, ctx, advisory_id)
    if not found:
        return _not_found()
    if not found["role"]:
        return _error("forbidden", 403)
    action = _line(data.get("action"), 30)
    if action == "edit":
        return await _edit(env, ctx, found, data)
    if action == "comment":
        return await _comment(env, ctx, found, data)
    if not ctx["maintainer"]:
        return _error("forbidden", 403)
    if action == "state":
        return await _transition(env, ctx, found, data)
    if action == "collaborator":
        return await _collaborators(env, ctx, found, data)
    if action == "credit":
        return await _credit(env, ctx, found, data)
    if action == "prepare":
        return await _prepare(env, request, ctx, found, data)
    if action == "sign":
        return await _signed(env, request, ctx, found, data)
    if action == "ledger-retry":
        return await _ledger_retry(env, request, ctx, found)
    return _error("invalid_action")


async def osv_export(env, request, ctx, advisory_id):
    found = await load(env, ctx, advisory_id)
    if not found or not found["public"]:
        return _not_found()
    return _osv_response(env, found["public"])


def _osv_response(env, view):
    return _json(osv_record(view, _origin(env)), cache="public, max-age=300")


async def repo_route(env, request, owner, repo):
    method = method_name(request)
    path = urlparse(str(request.url)).path
    pieces = [unquote(piece) for piece in path.split("/") if piece][5:]
    await ensure_schema(env)
    data = {}
    if method == "POST":
        try:
            data = await bounded_json_request(
                request, max_bytes=REQUEST_MAX_BYTES)
        except Exception:
            return _error("invalid_json")
        if not isinstance(data, dict):
            return _error("invalid_json")
        try:
            json.dumps(data, ensure_ascii=False).encode("utf-8")
        except (UnicodeEncodeError, ValueError, TypeError):
            return _error("invalid_json")
    elif method not in ("GET", "HEAD"):
        return _error("method_not_allowed", 405)
    ctx = await context(env, request, owner, repo, data or None)
    if ctx is None:
        return _not_found()
    if not pieces:
        return await overview(env, request, ctx) if method != "POST" else \
            _error("method_not_allowed", 405)
    if pieces == ["settings"] and method == "POST":
        return await update_settings(env, request, ctx, data)
    if pieces == ["advisories"] and method == "POST":
        return await create(env, request, ctx, data)
    if len(pieces) == 2 and pieces[0] == "advisories":
        if method == "POST":
            return await act(env, request, ctx, pieces[1], data)
        return await show(env, request, ctx, pieces[1])
    if len(pieces) == 3 and pieces[0] == "advisories" and \
            pieces[2] == "osv.json" and method != "POST":
        return await osv_export(env, request, ctx, pieces[1])
    return _not_found()


async def _published_rows(env, limit, since=0):
    return await d1_all(
        env, "SELECT id, state, updated_at, public_data FROM "
        "security_advisories WHERE state IN ('published','withdrawn') "
        "AND updated_at>=? ORDER BY published_at DESC LIMIT ?",
        int(since), int(limit))


def _views(rows):
    out = []
    for row in rows or []:
        try:
            view = json.loads(row.get("public_data") or "null")
        except ValueError:
            continue
        if isinstance(view, dict) and view.get("id"):
            out.append(view)
    return out


async def _still_public(env, views):
    is_private = globals().get("_repo_is_private")
    if is_private is None:
        return views
    org_node = globals().get("_org_repo_node")
    hidden = {}
    out = []
    for view in views:
        owner, _, repo = str(view.get("repository") or "").partition("/")
        key = (owner.lower(), repo.lower())
        if key not in hidden:
            closed = True
            if owner and repo:
                try:
                    backing = (await org_node(env, owner, repo)
                               if org_node else "") or owner
                    closed = bool(await is_private(env, backing, repo))
                except Exception:
                    closed = True
            hidden[key] = closed
        if not hidden[key]:
            out.append(view)
    return out


def _since(value):
    text = str(value or "").strip()
    if not text:
        return 0
    if text.isdigit():
        return int(text)
    try:
        moment = datetime.strptime(text, "%Y-%m-%dT%H:%M:%SZ")
    except ValueError:
        return 0
    return int(moment.replace(tzinfo=timezone.utc).timestamp() * 1000)


async def feed(env, request):
    params = parse_qs(urlparse(str(request.url)).query)
    try:
        limit = max(1, min(FEED_LIMIT, int(params.get("limit", ["100"])[0])))
    except ValueError:
        limit = 100
    since = _since(params.get("since", [""])[0])
    origin = _origin(env)
    views = await _still_public(
        env, _views(await _published_rows(env, limit, since)))
    return _json({
        "ok": True,
        "schema_version": OSV_SCHEMA_VERSION,
        "advisories": [osv_record(view, origin) for view in views],
    }, cache="public, max-age=300")


async def _public_view_by_id(env, advisory_id):
    if not valid_id(advisory_id):
        return None
    row = await d1_first(
        env, "SELECT public_data FROM security_advisories WHERE id=? "
        "AND state IN ('published','withdrawn')", advisory_id)
    views = await _still_public(env, _views([row] if row else []))
    return views[0] if views else None


def _e(value):
    return html.escape(str(value if value is not None else ""), quote=True)


def _site_name(env):
    try:
        return instance_features.display_name(env)
    except Exception:
        return "ForkMesh"


def _html_response(body, status=200):
    return Response(body, status=status, headers={
        "content-type": "text/html; charset=utf-8",
        "cache-control": "public, max-age=120"})


PAGE_STYLE = (
    "<style>table{width:100%;border-collapse:collapse;font-size:14px}"
    "th,td{text-align:left;padding:8px 6px;border-bottom:1px solid "
    "var(--line);vertical-align:top}th{color:var(--muted);font-weight:600}"
    ".sev{display:inline-block;border:1px solid var(--line);"
    "border-radius:999px;padding:0 8px;font-size:12px;"
    "text-transform:capitalize}.mono{font-family:ui-monospace,monospace;"
    "font-size:12px}.muted{color:var(--muted)}</style>")


def _severity_label(severity):
    severity = severity or {}
    level = severity.get("level") or "unknown"
    score = severity.get("score")
    return level + (" %.1f" % score if isinstance(score, (int, float)) else "")


async def advisories_page(env, request):
    import instance_pages
    views = await _still_public(
        env, _views(await _published_rows(env, LIST_LIMIT)))
    rows = "".join(
        "<tr><td class=\"mono\"><a href=\"%s\">%s</a></td><td>%s%s</td>"
        "<td><a href=\"/%s\">%s</a></td><td><span class=\"sev\">%s</span>"
        "</td><td class=\"muted\">%s</td></tr>" % (
            _e(_repo_href(view["repository"], view["id"])), _e(view["id"]),
            _e(view["title"]),
            " <span class=\"muted\">(withdrawn)</span>"
            if view.get("withdrawnAt") else "",
            _e(view["repository"]), _e(view["repository"]),
            _e(_severity_label(view.get("severity"))),
            _e(_rfc3339(view["publishedAt"])[:10]))
        for view in views)
    body = (
        "<h1>Security advisories</h1><p class=\"lead\">Published "
        "vulnerability advisories for repositories on this instance. "
        "Scanners can read the same data as OSV JSON at "
        "<a href=\"/advisories.json\">/advisories.json</a>.</p>%s%s"
        "<p><a href=\"/security-researchers\">Security researchers</a></p>"
    ) % (PAGE_STYLE, (
        "<table><thead><tr><th>ID</th><th>Summary</th><th>Repository</th>"
        "<th>Severity</th><th>Published</th></tr></thead><tbody>%s"
        "</tbody></table>" % rows) if rows else
        "<p class=\"muted\">No advisories have been published yet.</p>")
    return _html_response(instance_pages.page(
        _site_name(env), "Security advisories", "/advisories", body))


async def hall_of_fame(env):
    now = _now()
    cached = _hall_cache.get("value")
    if cached is not None and now - _hall_cache["at"] < HALL_CACHE_MS:
        return cached
    rows = await d1_all(
        env, "SELECT c.data AS credit, a.public_data AS public FROM "
        "security_advisory_credits c JOIN security_advisories a "
        "ON a.id=c.advisory_id WHERE c.state='accepted' "
        "AND a.state='published' LIMIT ?", HALL_ROWS)
    totals = {}
    for row in rows or []:
        credit = await decrypt_row(env, row.get("credit"))
        try:
            view = json.loads(row.get("public") or "null")
        except ValueError:
            continue
        if not isinstance(credit, dict) or not isinstance(view, dict):
            continue
        user = str(credit.get("user") or "")
        if not user:
            continue
        level = (view.get("severity") or {}).get("level") or ""
        entry = totals.setdefault(user, {
            "user": user, "points": 0, "credits": 0,
            "critical": 0, "high": 0, "moderate": 0, "low": 0})
        entry["points"] += SEVERITY_POINTS.get(level, 1)
        entry["credits"] += 1
        if level in SEVERITY_POINTS:
            entry[level] += 1
    ranked = sorted(totals.values(), key=lambda item: (
        -item["points"], -item["credits"], item["user"]))
    for index, item in enumerate(ranked):
        item["rank"] = index + 1
    _hall_cache.update({"at": now, "value": ranked})
    return ranked


async def researchers_page(env, request):
    import instance_pages
    ranked = await hall_of_fame(env)
    rows = "".join(
        "<tr><td>%d</td><td><a href=\"/%s\">%s</a></td><td>%d</td><td>%d</td>"
        "<td class=\"muted\">%d critical, %d high, %d moderate, %d low</td>"
        "</tr>" % (item["rank"], _e(quote(item["user"])), _e(item["user"]),
                   item["points"], item["credits"], item["critical"],
                   item["high"], item["moderate"], item["low"])
        for item in ranked[:200])
    body = (
        "<h1>Security researchers</h1><p class=\"lead\">People credited on "
        "published security advisories, ranked by points: critical %d, "
        "high %d, moderate %d, low %d. Only credits the researcher accepted "
        "count.</p>%s%s<p><a href=\"/advisories\">All advisories</a></p>"
    ) % (SEVERITY_POINTS["critical"], SEVERITY_POINTS["high"],
         SEVERITY_POINTS["moderate"], SEVERITY_POINTS["low"], PAGE_STYLE, (
             "<table><thead><tr><th>#</th><th>Researcher</th><th>Points</th>"
             "<th>Credits</th><th>Severity</th></tr></thead><tbody>%s"
             "</tbody></table>" % rows) if rows else
         "<p class=\"muted\">No accepted credits yet.</p>")
    return _html_response(instance_pages.page(
        _site_name(env), "Security researchers", "/security-researchers",
        body))


async def _credit_entries(env, user, include_private):
    rows = await d1_all(
        env, "SELECT c.advisory_id AS id, c.state AS state, c.data AS credit, "
        "a.state AS advisory_state, a.public_data AS public FROM "
        "security_advisory_credits c JOIN security_advisories a "
        "ON a.id=c.advisory_id WHERE c.user_bi=? ORDER BY c.updated_at DESC "
        "LIMIT 500", await blind_index(env, user))
    out = []
    for row in rows or []:
        state = str(row.get("state") or "")
        advisory_state = str(row.get("advisory_state") or "")
        if advisory_state not in PUBLIC_STATES:
            continue
        if not include_private and (
                state != "accepted" or advisory_state != "published"):
            continue
        credit = await decrypt_row(env, row.get("credit"))
        if not isinstance(credit, dict):
            continue
        entry = {"id": row.get("id"), "type": credit.get("type", ""),
                 "state": state, "advisoryState": advisory_state}
        try:
            view = json.loads(row.get("public") or "null")
        except ValueError:
            view = None
        if isinstance(view, dict):
            entry.update({
                "title": view.get("title", ""),
                "repository": view.get("repository", ""),
                "severity": (view.get("severity") or {}).get("level", ""),
                "publishedAt": view.get("publishedAt", 0),
                "href": _repo_href(view.get("repository", ""), view["id"]),
            })
        out.append(entry)
    return out


async def credits_route(env, request, user):
    user = str(user or "").strip().lower()
    if not NAME_RE.match(user):
        return _not_found()
    method = method_name(request)
    data = {}
    if method == "POST":
        try:
            data = await bounded_json_request(request, max_bytes=4096)
        except Exception:
            return _error("invalid_json")
        if not isinstance(data, dict):
            return _error("invalid_json")
    actor, _ = await _session(env, request, data or None)
    if method == "POST":
        if actor != user:
            return _error("forbidden", 403)
        return await _decide_credit(env, request, actor, data)
    entries = await _credit_entries(env, user, include_private=actor == user)
    accepted = [entry for entry in entries if entry["state"] == "accepted"
                and entry["advisoryState"] == "published"]
    return _json({"ok": True, "user": user, "count": len(accepted),
                  "credits": entries if actor == user else accepted},
                 cache="private, no-store")


async def _decide_credit(env, request, actor, data):
    advisory_id = _line(data.get("advisoryId"), 40)
    decision = _line(data.get("decision"), 20)
    if decision not in ("accept", "decline") or not valid_id(advisory_id):
        return _error("invalid_decision")
    user_bi = await blind_index(env, actor)
    row = await d1_first(
        env, "SELECT a.state AS state FROM security_advisory_credits c "
        "JOIN security_advisories a ON a.id=c.advisory_id "
        "WHERE c.advisory_id=? AND c.user_bi=?", advisory_id, user_bi)
    if not row:
        return _not_found()
    advisory_state = str(row.get("state") or "")
    if advisory_state not in PUBLIC_STATES and not await _member_role(
            env, advisory_id, actor):
        return _not_found()
    state = "accepted" if decision == "accept" else "declined"
    await d1_run(
        env, "UPDATE security_advisory_credits SET state=?, updated_at=? "
        "WHERE advisory_id=? AND user_bi=?", state, _now(), advisory_id,
        user_bi)
    if advisory_state in PUBLIC_STATES:
        await _refresh_public(env, advisory_id)
    _hall_cache["value"] = None
    return _json({"ok": True, "state": state})


async def _refresh_public(env, advisory_id):
    row = await d1_first(
        env, "SELECT data FROM security_advisories WHERE id=?", advisory_id)
    record = await decrypt_row(env, (row or {}).get("data"))
    if not isinstance(record, dict):
        return
    credits = await _credits(env, advisory_id)
    now = _now()
    view = public_view(record, credits)
    view["updatedAt"] = max(int(view.get("updatedAt") or 0), now)
    await d1_run(
        env, "UPDATE security_advisories SET public_data=?, updated_at=? "
        "WHERE id=?",
        json.dumps(view, separators=(",", ":"), ensure_ascii=False),
        view["updatedAt"], advisory_id)


def is_public_path(path):
    path = str(path or "")
    return (path in PUBLIC_PATHS or path.startswith("/advisories/")
            or path.startswith("/api/security-advisories/"))


async def public_route(env, request, path):
    method = method_name(request)
    await ensure_schema(env)
    if path.startswith("/api/security-advisories/"):
        rest = path[len("/api/security-advisories/"):].strip("/").split("/")
        if rest == ["hall-of-fame"] and method in ("GET", "HEAD"):
            return _json({"ok": True, "points": SEVERITY_POINTS,
                          "researchers": (await hall_of_fame(env))[:200]},
                         cache="public, max-age=120")
        if len(rest) == 2 and rest[0] == "credits" and method in (
                "GET", "HEAD", "POST"):
            return await credits_route(env, request, unquote(rest[1]))
        return _not_found()
    if method not in ("GET", "HEAD"):
        return _error("method_not_allowed", 405)
    if path in ("/advisories", "/advisories/"):
        return await advisories_page(env, request)
    if path == "/advisories.json":
        return await feed(env, request)
    if path in ("/security-researchers", "/security-researchers/"):
        return await researchers_page(env, request)
    name = path[len("/advisories/"):].strip("/")
    as_json = name.endswith(".json")
    if as_json:
        name = name[:-5]
    view = await _public_view_by_id(env, name)
    if view is None:
        return _not_found()
    if as_json:
        return _osv_response(env, view)
    return Response("", status=302, headers={
        "location": _repo_href(view["repository"], view["id"]),
        "cache-control": "public, max-age=300"})
