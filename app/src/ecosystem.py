import datetime
import hashlib
import html
import json
import re
from urllib.parse import parse_qs, urlparse


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


VIEW = "ecosystem"
CATEGORIES = (
    ("decentralized-git", "Decentralized Git"),
    ("forges", "Git hosting"),
    ("version-control", "Version control"),
    ("issue-tracking", "Issue tracking"),
    ("code-review", "Code review"),
    ("chat", "Chat & discussions"),
    ("software-factories", "Software factories"),
    ("agent-workspaces", "Agent workspaces"),
    ("agent-infrastructure", "Agent infrastructure"),
    ("ai-agents", "AI agents"),
    ("federation", "Federation"),
    ("identity", "Identity"),
    ("packages", "Package distribution"),
    ("dev-tooling", "Developer tooling"),
    ("cloud-hosting", "Cloud & hosting"),
    ("networking", "Networking"),
    ("ai-models", "AI models & APIs"),
    ("payments", "Payments & billing"),
    ("messaging", "Email & notifications"),
    ("storage", "Databases & storage"),
    ("security", "Security & cryptography"),
    ("analytics", "Analytics & monitoring"),
    ("runtimes", "Languages & runtimes"),
    ("libraries", "Libraries & frameworks"),
    ("platforms", "Platforms & app stores"),
    ("editors", "Editors & IDEs"),
    ("build-ci", "Build & CI"),
    ("design-assets", "Fonts & icons"),
    ("apps", "Apps & services"),
)
COMPATIBILITY = (
    ("unknown", "Not yet assessed"),
    ("none", "Not compatible"),
    ("partial", "Partial"),
    ("full", "Full"),
)
INTEGRATION = (
    ("watching", "Watching"),
    ("exploring", "Exploring"),
    ("planned", "Planned"),
    ("building", "In progress"),
    ("available", "Available"),
    ("uses", "Used by ForkMesh"),
)
LEVELS = ("low", "medium", "high")
ACTIONS = ("investigate", "adopt", "interoperate", "absorb", "partner",
           "ignore")
CATEGORY_LABELS = dict(CATEGORIES)
COMPATIBILITY_LABELS = dict(COMPATIBILITY)
INTEGRATION_LABELS = dict(INTEGRATION)
PUBLIC_FIELDS = (
    "name", "tagline", "summary", "watching", "url", "repo", "tags",
    "openSource", "license", "gitNative", "maker", "compatibility",
    "integration", "integrations", "reviewed")
FIELDS = ("slug",) + PUBLIC_FIELDS + (
    "category", "published", "featured", "position", "internal")
REQUIRED = ("name", "tagline", "url", "category")
TEXT_LIMITS = {"name": 80, "tagline": 120, "summary": 900, "watching": 400,
               "license": 60, "maker": 80}
NULLABLE_TEXT = ("license", "maker")
URL_LIMIT = 400
MAX_TAGS = 8
MAX_INTEGRATIONS = 8
INTEGRATION_LIMIT = 100
MAX_POSITION = 1_000_000
POSITION_STEP = 10
MAX_PROJECTS = 1000
MAX_IMPORT = 500
MAX_INTERNAL_KEYS = 40
MAX_INTERNAL_TEXT = 4000
MAX_INTERNAL_LIST = 20
MAX_INTERNAL_ITEM = 400
MAX_INTERNAL_BYTES = 16384
MAX_FORM_BYTES = 1024 * 1024
MAX_SHOWN_ERRORS = 40
API_CACHE_KEY = "https://forkmesh.internal/api/ecosystem"
API_CACHE_SECONDS = 60
SLUG_RE = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?$")
TAG_RE = re.compile(r"^[a-z0-9][a-z0-9+#.-]{0,31}$")
REVIEWED_RE = re.compile(r"^(\d{4})-(\d{2})(?:-(\d{2}))?$")
INTERNAL_KEY_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_-]{0,39}$")
INTERNAL_LEVEL_KEYS = ("overlap", "threat", "opportunity", "confidence")
MONTHS = ("Jan", "Feb", "Mar", "Apr", "May", "Jun",
          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")
TABS = (("all", "All"), ("published", "Published"), ("drafts", "Drafts"))
TOGGLES = {
    "publish": ("published", True),
    "unpublish": ("published", False),
    "feature": ("featured", True),
    "unfeature": ("featured", False),
}
NOTICES = {
    "saved": ("Saved %s.", "ok"),
    "unchanged": ("Nothing changed, so nothing was saved.", "ok"),
    "publish": ("%s is now public.", "ok"),
    "unpublish": ("%s is a draft again.", "ok"),
    "feature": ("%s is now featured on the landing page.", "ok"),
    "unfeature": ("%s is no longer featured on the landing page.", "ok"),
    "deleted": ("Deleted %s.", "ok"),
    "missing": ("That project no longer exists.", "bad"),
    "confirm_delete": ("Tick the confirmation box to delete this project.",
                       "warn"),
    "csrf": ("Action blocked: invalid or missing CSRF token. Reload the page "
             "and try again.", "bad"),
    "unknown_action": ("That action is not available here.", "bad"),
    "no_table": ("The ecosystem_projects table is not available on this "
                 "database yet. Apply migration 0212 and reload.", "bad"),
}
ACTION_TONES = {"investigate": "info", "adopt": "ok", "interoperate": "ok",
                "absorb": "done", "partner": "done", "ignore": "line"}
LEVEL_TONES = {"high": "bad", "medium": "warn", "low": ""}
ADMIN_SQL = (
    "SELECT slug, category, published, featured, position, data, internal, "
    "created_at, updated_at, updated_by FROM ecosystem_projects "
    "ORDER BY position, slug")
PUBLIC_SQL = (
    "SELECT slug, category, featured, position, data, updated_at "
    "FROM ecosystem_projects WHERE published = 1 "
    "ORDER BY position, slug LIMIT ?")
UPSERT_SQL = (
    "INSERT INTO ecosystem_projects (slug, category, published, featured, "
    "position, data, internal, created_at, updated_at, updated_by) "
    "VALUES (?,?,?,?,?,?,?,?,?,?) ON CONFLICT(slug) DO UPDATE SET "
    "category=excluded.category, published=excluded.published, "
    "featured=excluded.featured, position=excluded.position, "
    "data=excluded.data, internal=excluded.internal, "
    "updated_at=excluded.updated_at, updated_by=excluded.updated_by")
EXAMPLE = """[
  {
    "slug": "git-bug",
    "name": "git-bug",
    "tagline": "Distributed issue tracker stored in Git",
    "url": "https://github.com/git-bug/git-bug",
    "category": "issue-tracking",
    "openSource": true,
    "gitNative": true,
    "published": true,
    "internal": {"action": "interoperate", "notes": "Private note"}
  }
]"""


def _esc(value):
    return html.escape("" if value is None else str(value), quote=True)


def _int(value, default=0):
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _first(params, key):
    values = (params or {}).get(key) or [""]
    return str(values[0] or "")


def _now():
    return int(Date.now())


def _month(now_ms):
    moment = datetime.datetime.fromtimestamp(
        now_ms / 1000, datetime.timezone.utc)
    return "%04d-%02d" % (moment.year, moment.month)


def _day(now_ms):
    moment = datetime.datetime.fromtimestamp(
        now_ms / 1000, datetime.timezone.utc)
    return "%04d-%02d-%02d" % (moment.year, moment.month, moment.day)


def reviewed_label(value):
    match = REVIEWED_RE.match(str(value or ""))
    if not match:
        return ""
    month = int(match.group(2))
    if not 1 <= month <= 12:
        return ""
    return "%s %s" % (MONTHS[month - 1], match.group(1))


def slug_from_name(name):
    text = re.sub(r"[^a-z0-9]+", "-", str(name or "").lower()).strip("-")
    return text[:64].strip("-")


def clean_slug(value):
    value = str(value or "").strip()
    return value if SLUG_RE.match(value) else ""


def _reject_constant(name):
    raise ValueError("%s is not allowed in JSON" % name)


def _loads(text):
    return json.loads(text, parse_constant=_reject_constant)


def defaults(now_ms, position):
    return {
        "summary": "", "watching": "", "repo": None, "tags": [],
        "openSource": None, "license": None, "gitNative": False,
        "maker": None, "compatibility": "unknown", "integration": "watching",
        "integrations": [], "reviewed": _month(now_ms), "published": False,
        "featured": False, "position": position, "internal": {}}


def _clean_text(field, value):
    if value is None:
        value = ""
    if not isinstance(value, str):
        return None, "%s must be text" % field
    value = " ".join(value.split())
    if len(value) > TEXT_LIMITS[field]:
        return None, "%s is longer than %d characters" % (
            field, TEXT_LIMITS[field])
    if field in NULLABLE_TEXT and not value:
        return None, None
    return value, None


def _clean_url(field, value):
    if value is None or value == "":
        return None, None
    message = "%s must be an http(s) URL of at most %d characters" % (
        field, URL_LIMIT)
    if not isinstance(value, str):
        return None, message
    value = value.strip()
    if (not value or len(value) > URL_LIMIT
            or any(ch.isspace() or ord(ch) < 32 for ch in value)):
        return None, message
    try:
        parsed = urlparse(value)
        host = parsed.hostname
    except ValueError:
        return None, message
    if (parsed.scheme not in ("http", "https") or not host
            or parsed.username is not None or parsed.password is not None):
        return None, message
    return value, None


def _clean_tags(value):
    if value is None:
        return [], None
    message = "tags must be a list of short words"
    if not isinstance(value, list):
        return None, message
    tags = []
    for item in value:
        if not isinstance(item, str):
            return None, message
        tag = "-".join(item.strip().lower().split())
        if not TAG_RE.match(tag):
            return None, ('tag "%s" may use letters, digits and - + # . and '
                          'at most 32 characters' % item[:40])
        if tag not in tags:
            tags.append(tag)
    if len(tags) > MAX_TAGS:
        return None, "at most %d tags" % MAX_TAGS
    return tags, None


def _clean_integrations(value):
    if value is None:
        return [], None
    message = ("integrations must be a list of phrases of 1 to %d characters"
               % INTEGRATION_LIMIT)
    if not isinstance(value, list):
        return None, message
    items = []
    for item in value:
        if not isinstance(item, str):
            return None, message
        text = " ".join(item.split())
        if not text or len(text) > INTEGRATION_LIMIT:
            return None, message
        items.append(text)
    if len(items) > MAX_INTEGRATIONS:
        return None, "at most %d integrations" % MAX_INTEGRATIONS
    return items, None


def _clean_bool(field, value, nullable=False):
    if value is None:
        return (None if nullable else False), None
    if not isinstance(value, bool):
        return None, "%s must be true or false%s" % (
            field, " or null" if nullable else "")
    return value, None


def _clean_choice(field, value, choices, default):
    if value is None:
        return default, None
    if value not in choices:
        return None, "%s must be one of: %s" % (field, ", ".join(choices))
    return value, None


def _clean_position(value, fallback):
    if value is None:
        return fallback, None
    if (isinstance(value, bool) or not isinstance(value, int)
            or not 0 <= value <= MAX_POSITION):
        return None, "position must be a whole number from 0 to %d" % (
            MAX_POSITION)
    return value, None


def _clean_reviewed(value, fallback):
    if value is None or value == "":
        return fallback, None
    match = REVIEWED_RE.match(value) if isinstance(value, str) else None
    valid = False
    if match:
        year = int(match.group(1))
        try:
            datetime.date(year, int(match.group(2)), int(match.group(3) or 1))
            valid = 2000 <= year <= 2100
        except ValueError:
            valid = False
    if not valid:
        return None, 'reviewed must be a date like "2026-09" or "2026-09-25"'
    return value, None


def _clean_internal_value(key, value):
    field = "internal." + key
    if key in INTERNAL_LEVEL_KEYS:
        return _clean_choice(field, value, LEVELS, None)
    if key == "action":
        return _clean_choice(field, value, ACTIONS, None)
    if key == "sources":
        message = "%s must be a list of at most %d URLs" % (
            field, MAX_INTERNAL_LIST)
        if not isinstance(value, list) or len(value) > MAX_INTERNAL_LIST:
            return None, message
        urls = []
        for item in value:
            url, error = _clean_url(field, item)
            if error or not url:
                return None, message
            urls.append(url)
        return urls, None
    if isinstance(value, bool) or (
            isinstance(value, (int, float)) and abs(value) < 1e15):
        return value, None
    if isinstance(value, str):
        text = value.strip()
        if len(text) > MAX_INTERNAL_TEXT:
            return None, "%s is longer than %d characters" % (
                field, MAX_INTERNAL_TEXT)
        return text, None
    if (isinstance(value, list) and len(value) <= MAX_INTERNAL_LIST
            and all(isinstance(item, str) and len(item) <= MAX_INTERNAL_ITEM
                    for item in value)):
        return [item.strip() for item in value], None
    return None, ("%s must be text, a number, true or false, or a list of "
                  "short texts" % field)


def _merge_internal(current, value, replace):
    if value is None:
        return {}, []
    if not isinstance(value, dict):
        return None, ["internal must be an object"]
    merged = {} if replace else dict(current or {})
    errors = []
    for key, item in value.items():
        if not INTERNAL_KEY_RE.match(key):
            errors.append('internal key "%s" may use letters, digits, - and _'
                          % key[:40])
            continue
        if item is None:
            merged.pop(key, None)
            continue
        clean, error = _clean_internal_value(key, item)
        if error:
            errors.append(error)
        else:
            merged[key] = clean
    if len(merged) > MAX_INTERNAL_KEYS:
        errors.append("internal holds at most %d keys" % MAX_INTERNAL_KEYS)
    elif len(json.dumps(merged)) > MAX_INTERNAL_BYTES:
        errors.append("internal is larger than %d bytes" % MAX_INTERNAL_BYTES)
    return merged, errors


def _clean_field(field, value, base, now_ms):
    if field in TEXT_LIMITS:
        return _clean_text(field, value)
    if field in ("url", "repo"):
        return _clean_url(field, value)
    if field == "tags":
        return _clean_tags(value)
    if field == "integrations":
        return _clean_integrations(value)
    if field == "openSource":
        return _clean_bool(field, value, nullable=True)
    if field in ("gitNative", "published", "featured"):
        return _clean_bool(field, value)
    if field == "compatibility":
        return _clean_choice(field, value, tuple(COMPATIBILITY_LABELS),
                             "unknown")
    if field == "integration":
        return _clean_choice(field, value, tuple(INTEGRATION_LABELS),
                             "watching")
    if field == "category":
        return _clean_choice(field, value, tuple(CATEGORY_LABELS), None)
    if field == "position":
        return _clean_position(value, base["position"])
    return _clean_reviewed(value, _month(now_ms))


def normalize(raw, current=None, replace=False, now_ms=0, position=0):
    if not isinstance(raw, dict):
        return None, ["each project must be a JSON object"]
    errors = ['unknown field "%s"; private notes go under "internal"'
              % str(key)[:40] for key in raw if key not in FIELDS]
    if current is not None and not replace:
        entry = dict(current)
    else:
        entry = defaults(now_ms, current["position"] if current else position)
    for field in FIELDS:
        if field in ("slug", "internal") or field not in raw:
            continue
        clean, error = _clean_field(field, raw[field], entry, now_ms)
        if error:
            errors.append(error)
        else:
            entry[field] = clean
    if "internal" in raw:
        base = {} if replace or current is None else current.get("internal")
        internal, internal_errors = _merge_internal(
            base, raw["internal"], replace)
        errors.extend(internal_errors)
        if not internal_errors:
            entry["internal"] = internal
    for field in REQUIRED:
        if not entry.get(field):
            errors.append("%s is required" % field)
    return (None if errors else entry), errors


def _slug_of(raw):
    value = raw.get("slug")
    if value is None or value == "":
        name = raw.get("name")
        slug = slug_from_name(name if isinstance(name, str) else "")
        if not slug:
            return "", "slug is required, or a name to derive it from"
    elif not isinstance(value, str):
        return "", "slug must be text"
    else:
        slug = value.strip()
    if not SLUG_RE.match(slug):
        return "", ('slug "%s" must be lowercase letters, digits and inner '
                    'hyphens, at most 64 characters' % slug[:70])
    return slug, None


def parse_payload(text):
    if not str(text or "").strip():
        return None, ["Paste a project, or a list of projects, as JSON."]
    try:
        data = _loads(text)
    except ValueError as exc:
        return None, ["The JSON could not be parsed: %s" % exc]
    if isinstance(data, dict) and isinstance(data.get("projects"), list):
        data = data["projects"]
    elif isinstance(data, dict):
        data = [data]
    if not isinstance(data, list) or not data:
        return None, ["Paste a project object or a non-empty list of them."]
    if len(data) > MAX_IMPORT:
        return None, ["Import at most %d projects at a time." % MAX_IMPORT]
    return data, []


def plan_import(items, existing, now_ms):
    by_slug = {entry["slug"]: entry for entry in existing}
    next_position = max(
        [entry["position"] for entry in existing] + [0]) + POSITION_STEP
    seen = set()
    changes = []
    errors = []
    for index, raw in enumerate(items, 1):
        if not isinstance(raw, dict):
            errors.append("Project %d: each project must be a JSON object"
                          % index)
            continue
        slug, error = _slug_of(raw)
        label = "Project %d%s" % (index, (" (%s)" % slug) if slug else "")
        if error:
            errors.append("%s: %s" % (label, error))
            continue
        if slug in seen:
            errors.append("%s: appears more than once" % label)
            continue
        seen.add(slug)
        current = by_slug.get(slug)
        entry, entry_errors = normalize(
            raw, current, False, now_ms, next_position)
        if entry_errors:
            errors.extend("%s: %s" % (label, item) for item in entry_errors)
            continue
        entry["slug"] = slug
        if current is None:
            status = "added"
            if "position" not in raw:
                next_position += POSITION_STEP
        else:
            status = "unchanged" if entry == current else "updated"
        changes.append((slug, entry, status))
    added = sum(1 for change in changes if change[2] == "added")
    if not errors and len(existing) + added > MAX_PROJECTS:
        errors.append("The ecosystem holds at most %d projects." % MAX_PROJECTS)
    return changes, errors


def _json_object(text):
    try:
        value = json.loads(text or "{}")
    except (TypeError, ValueError):
        return None
    return value if isinstance(value, dict) else None


def row_entry(row, include_internal=True):
    data = _json_object(row.get("data"))
    if data is None:
        return None
    blank = defaults(0, 0)
    entry = {"slug": str(row.get("slug") or "")}
    for field in PUBLIC_FIELDS:
        entry[field] = data.get(field, blank.get(field))
    entry["category"] = str(row.get("category") or "")
    entry["published"] = bool(_int(row.get("published")))
    entry["featured"] = bool(_int(row.get("featured")))
    entry["position"] = _int(row.get("position"))
    if include_internal:
        entry["internal"] = _json_object(row.get("internal")) or {}
    return entry


def export_entry(entry):
    return {field: entry.get(field) for field in FIELDS}


def row_args(entry, now_ms, actor):
    data = {field: entry.get(field) for field in PUBLIC_FIELDS}
    return (
        entry["slug"], entry["category"], 1 if entry["published"] else 0,
        1 if entry["featured"] else 0, int(entry["position"]),
        json.dumps(data, separators=(",", ":")),
        json.dumps(entry.get("internal") or {}, separators=(",", ":")),
        now_ms, now_ms, str(actor or ""))


def public_project(entry):
    return {
        "slug": entry["slug"],
        "name": entry["name"],
        "tagline": entry["tagline"],
        "summary": entry["summary"],
        "watching": entry["watching"],
        "url": entry["url"],
        "repo": entry["repo"],
        "category": entry["category"],
        "categoryLabel": CATEGORY_LABELS.get(entry["category"], ""),
        "tags": list(entry["tags"] or []),
        "openSource": entry["openSource"],
        "license": entry["license"],
        "gitNative": bool(entry["gitNative"]),
        "maker": entry["maker"],
        "compatibility": entry["compatibility"],
        "compatibilityLabel": COMPATIBILITY_LABELS.get(
            entry["compatibility"], ""),
        "integration": entry["integration"],
        "integrationLabel": INTEGRATION_LABELS.get(entry["integration"], ""),
        "integrations": list(entry["integrations"] or []),
        "reviewed": entry["reviewed"],
        "reviewedLabel": reviewed_label(entry["reviewed"]),
        "featured": bool(entry["featured"]),
    }


def public_payload(rows):
    projects = []
    counts = {}
    updated = 0
    for row in rows or []:
        row = dict(row)
        entry = row_entry(row, include_internal=False)
        if entry is None or entry["category"] not in CATEGORY_LABELS:
            continue
        projects.append(public_project(entry))
        counts[entry["category"]] = counts.get(entry["category"], 0) + 1
        updated = max(updated, _int(row.get("updated_at")))
    return {
        "version": 1,
        "updatedAt": updated or None,
        "categories": [
            {"id": key, "label": label, "count": counts[key]}
            for key, label in CATEGORIES if counts.get(key)],
        "projects": projects,
    }


async def api_handler(env, request):
    if method_name(request) not in ("GET", "HEAD"):
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            cache_control="no-store", extra_headers={"allow": "GET, HEAD"})
    cached = await edge_cache_match(API_CACHE_KEY)
    if cached is not None:
        return cached
    try:
        await ensure_schema(env)
        rows = await d1_all(env, PUBLIC_SQL, MAX_PROJECTS)
    except Exception:
        return json_response({"error": "unavailable"}, status=503,
                             cache_control="no-store")
    response = json_response(public_payload(rows),
                             cache_seconds=API_CACHE_SECONDS)
    await edge_cache_put(API_CACHE_KEY, response)
    return response


def _console_path(env):
    path = str(getattr(env, "ADMIN_PATH", "") or "").strip("/")
    return "/" + path if path else ""


def _href(admin_query, **params):
    return _admin_href(admin_query, view=VIEW, **params)


def _location(env, admin_query, **params):
    return _console_path(env) + _href(admin_query, **params).replace(
        "&amp;", "&")


def _html(body, status=200, extra=None):
    headers = {
        "content-type": "text/html; charset=utf-8",
        "cache-control": "no-store",
        "x-robots-tag": "noindex, nofollow",
    }
    headers.update(extra or {})
    return Response(body, status=status, headers=headers)


def _redirect(location):
    return Response("", status=303, headers={
        "location": location, "cache-control": "no-store"})


async def load(env):
    try:
        rows = await d1_all(env, ADMIN_SQL)
    except Exception:
        return None
    entries = []
    meta = {}
    for row in rows or []:
        row = dict(row)
        entry = row_entry(row)
        if entry is None:
            continue
        entries.append(entry)
        meta[entry["slug"]] = {
            "created_at": _int(row.get("created_at")),
            "updated_at": _int(row.get("updated_at")),
            "updated_by": str(row.get("updated_by") or ""),
        }
    return entries, meta


async def admin_handler(env, request, params, csrf_field="", admin_query="",
                        account=""):
    if _first(params, "view") != VIEW:
        return None
    now = _now()
    if method_name(request) == "POST":
        return await _handle_post(
            env, request, params, csrf_field, admin_query, account, now)
    loaded = await load(env)
    if loaded is None:
        return await _page(env, request, account, "Ecosystem",
                           _notice_banner("no_table", "")
                           + _admin_shell.page_head("Ecosystem"))
    entries, meta = loaded
    if _first(params, "format") == "json":
        return _export(entries, now)
    slug = clean_slug(_first(params, "project"))
    if slug:
        entry = next((item for item in entries if item["slug"] == slug), None)
        if entry is not None:
            return await _edit_page(
                env, request, params, csrf_field, admin_query, account, now,
                entry, meta.get(slug) or {})
        params = dict(params, notice=["missing"])
    return await _list_page(env, request, params, csrf_field, admin_query,
                            account, now, entries, meta)


async def _handle_post(env, request, params, csrf_field, admin_query, account,
                       now):
    try:
        raw = await bounded_request_text(request, MAX_FORM_BYTES)
    except RequestBodyTooLarge:
        loaded = await load(env) or ([], {})
        return await _list_page(
            env, request, params, csrf_field, admin_query, account, now,
            loaded[0], loaded[1], errors=[
                "That form is larger than 1 MB. Import the projects in "
                "smaller batches."], status=413)
    except Exception:
        raw = ""
    form = parse_qs(str(raw or ""), keep_blank_values=True)
    if not _admin_csrf_ok(env, form):
        return _redirect(_location(env, admin_query, notice="csrf"))
    action = _first(form, "action")
    if action == "import":
        return await _import(env, request, params, form, csrf_field,
                             admin_query, account, now)
    if action == "save":
        return await _save(env, request, params, form, csrf_field,
                           admin_query, account, now)
    if action in TOGGLES:
        return await _toggle(env, form, admin_query, account, now, action)
    if action == "delete":
        return await _delete(env, form, admin_query, account)
    return _redirect(_location(env, admin_query, notice="unknown_action"))


async def _import(env, request, params, form, csrf_field, admin_query,
                  account, now):
    loaded = await load(env)
    if loaded is None:
        return _redirect(_location(env, admin_query, notice="no_table"))
    entries, meta = loaded
    text = _first(form, "json")
    items, errors = parse_payload(text)
    changes = []
    if not errors:
        changes, errors = plan_import(items, entries, now)
    if errors:
        return await _list_page(
            env, request, params, csrf_field, admin_query, account, now,
            entries, meta, import_text=text, errors=errors, status=422)
    writes = [(UPSERT_SQL, row_args(entry, now, account))
              for _slug, entry, status in changes if status != "unchanged"]
    if writes:
        await d1_run_batch(env, writes)
        await edge_cache_delete(API_CACHE_KEY)
    counts = {key: sum(1 for change in changes if change[2] == key)
              for key in ("added", "updated", "unchanged")}
    await _audit_sensitive_action(
        env, account, "admin.ecosystem_import", "ecosystem",
        str(len(changes)), "success", counts)
    return _redirect(_location(env, admin_query, notice="imported", **counts))


async def _save(env, request, params, form, csrf_field, admin_query, account,
                now):
    slug = clean_slug(_first(form, "project"))
    loaded = await load(env)
    if loaded is None:
        return _redirect(_location(env, admin_query, notice="no_table"))
    entries, meta = loaded
    current = next((item for item in entries if item["slug"] == slug), None)
    if current is None:
        return _redirect(_location(env, admin_query, notice="missing"))
    text = _first(form, "json")
    errors = []
    raw = None
    try:
        raw = _loads(text)
    except ValueError as exc:
        errors.append("The JSON could not be parsed: %s" % exc)
    if not errors and not isinstance(raw, dict):
        errors.append("Edit one project as a JSON object.")
    if not errors and raw.get("slug", slug) != slug:
        errors.append(
            "The slug cannot change here. Import the project under its new "
            "slug, then delete this one.")
    entry = None
    if not errors:
        entry, errors = normalize(raw, current, True, now, current["position"])
    if errors:
        return await _edit_page(
            env, request, params, csrf_field, admin_query, account, now,
            current, meta.get(slug) or {}, text=text, errors=errors,
            status=422)
    entry["slug"] = slug
    if entry == current:
        return _redirect(_location(env, admin_query, notice="unchanged",
                                   slug=slug))
    await d1_run_batch(env, [(UPSERT_SQL, row_args(entry, now, account))])
    await edge_cache_delete(API_CACHE_KEY)
    await _audit_sensitive_action(
        env, account, "admin.ecosystem_save", "ecosystem_project", slug,
        "success", {"published": entry["published"]})
    return _redirect(_location(env, admin_query, notice="saved", slug=slug))


async def _toggle(env, form, admin_query, account, now, action):
    slug = clean_slug(_first(form, "project"))
    back = _first(form, "back")
    back = back if back in ("published", "drafts") else None
    column, value = TOGGLES[action]
    row = None
    if slug:
        try:
            row = await d1_first(
                env, "SELECT published, featured FROM ecosystem_projects "
                "WHERE slug = ?", slug)
        except Exception:
            row = None
    if not row:
        return _redirect(_location(env, admin_query, status=back,
                                   notice="missing"))
    if bool(_int(dict(row).get(column))) == value:
        return _redirect(_location(env, admin_query, status=back,
                                   notice="unchanged", slug=slug))
    await d1_run(
        env, "UPDATE ecosystem_projects SET %s = ?, updated_at = ?, "
        "updated_by = ? WHERE slug = ?" % column,
        1 if value else 0, now, str(account or ""), slug)
    await edge_cache_delete(API_CACHE_KEY)
    await _audit_sensitive_action(
        env, account, "admin.ecosystem_" + action, "ecosystem_project", slug,
        "success", {column: value})
    return _redirect(_location(env, admin_query, status=back, notice=action,
                               slug=slug))


async def _delete(env, form, admin_query, account):
    slug = clean_slug(_first(form, "project"))
    if _first(form, "confirm") != "yes":
        return _redirect(_location(env, admin_query, project=slug or None,
                                   notice="confirm_delete"))
    row = None
    if slug:
        try:
            row = await d1_first(
                env, "SELECT slug FROM ecosystem_projects WHERE slug = ?",
                slug)
        except Exception:
            row = None
    if not row:
        return _redirect(_location(env, admin_query, notice="missing"))
    await d1_run(env, "DELETE FROM ecosystem_projects WHERE slug = ?", slug)
    await edge_cache_delete(API_CACHE_KEY)
    await _audit_sensitive_action(
        env, account, "admin.ecosystem_delete", "ecosystem_project", slug,
        "success", {})
    return _redirect(_location(env, admin_query, notice="deleted", slug=slug))


def _export(entries, now):
    body = json.dumps([export_entry(entry) for entry in entries], indent=2,
                      ensure_ascii=False)
    return Response(body + "\n", status=200, headers={
        "content-type": "application/json; charset=utf-8",
        "content-disposition": 'attachment; filename="forkmesh-ecosystem-%s'
                               '.json"' % _day(now),
        "cache-control": "no-store",
        "x-robots-tag": "noindex, nofollow",
    })


async def _page(env, request, account, title, content, status=200):
    shell = _admin_shell
    url = urlparse(request.url)
    body = shell.render_page(
        title, content, active=VIEW, console=shell.console_base(env),
        theme=shell.theme_from_request(request),
        badges=await shell.nav_badges(env), account=account,
        next_path=url.path + (("?" + url.query) if url.query else ""))
    return _html(body, status)


def _notice_banner(notice, slug):
    shell = _admin_shell
    if notice not in NOTICES:
        return ""
    text, tone = NOTICES[notice]
    if "%s" in text:
        text = text % (slug or "The project")
    return shell.banner(text, tone)


def _imported_banner(params):
    counts = [_int(_first(params, key)) for key in
              ("added", "updated", "unchanged")]
    return _admin_shell.banner(
        "Imported %d project%s: %d added, %d updated, %d unchanged." % (
            sum(counts), "" if sum(counts) == 1 else "s", *counts), "ok")


def _errors_banner(errors):
    if not errors:
        return ""
    shown = list(errors[:MAX_SHOWN_ERRORS])
    if len(errors) > MAX_SHOWN_ERRORS:
        shown.append("…and %d more." % (len(errors) - MAX_SHOWN_ERRORS))
    return _admin_shell.banner(
        "Nothing was saved. Fix these and submit again:\n"
        + "\n".join("• " + item for item in shown), "bad")


def _post_form(admin_query, csrf_field, action, slug, label, cls="btn btn-sm",
               back=None):
    return (
        '<form class="inline" method="post" action="%s">%s'
        '<input type="hidden" name="action" value="%s">'
        '<input type="hidden" name="project" value="%s">%s'
        '<button class="%s" type="submit">%s</button></form>' % (
            _href(admin_query), csrf_field, _esc(action), _esc(slug),
            ('<input type="hidden" name="back" value="%s">' % _esc(back))
            if back else "", cls, _esc(label)))


def _assessment_html(internal):
    shell = _admin_shell
    parts = []
    action = internal.get("action")
    if action in ACTIONS:
        parts.append(shell.pill(action, ACTION_TONES.get(action, "")))
    threat = internal.get("threat")
    if threat in LEVELS:
        parts.append(shell.pill("threat " + threat, LEVEL_TONES[threat]))
    return " ".join(parts) or '<span class="muted">—</span>'


def _row_html(entry, csrf_field, admin_query, tab):
    shell = _admin_shell
    slug = entry["slug"]
    back = tab if tab != "all" else None
    status = (shell.pill("Published", "ok") if entry["published"]
              else shell.pill("Draft", "line"))
    if entry["featured"]:
        status += " " + shell.pill("Featured", "info")
    relationship = "%s · %s" % (
        COMPATIBILITY_LABELS.get(entry["compatibility"], "—"),
        INTEGRATION_LABELS.get(entry["integration"], "—"))
    actions = (
        _post_form(admin_query, csrf_field,
                     "unpublish" if entry["published"] else "publish", slug,
                     "Unpublish" if entry["published"] else "Publish",
                     back=back)
        + _post_form(admin_query, csrf_field,
                     "unfeature" if entry["featured"] else "feature", slug,
                     "Unfeature" if entry["featured"] else "Feature",
                     back=back))
    return (
        '<tr><td><a href="%s" title="Edit"><b>%s</b></a><br><span class="muted">'
        '<span class="mono">%s</span> · %s</span></td>'
        '<td class="nowrap">%s</td><td>%s</td>'
        '<td class="nowrap">%s</td><td class="nowrap">%s</td>'
        '<td class="r"><div class="actions" style="justify-content:flex-end;'
        'flex-wrap:nowrap">%s</div></td></tr>' % (
            _href(admin_query, project=slug), _esc(entry["name"]),
            _esc(slug), _esc(CATEGORY_LABELS.get(entry["category"], "—")),
            status, _esc(relationship),
            _assessment_html(entry.get("internal") or {}),
            _esc(reviewed_label(entry["reviewed"]) or "—"), actions))


def _reference_html():
    rows = (
        ("slug", "Lowercase letters, digits and hyphens. Derived from the "
                 "name when left out, and the key an import merges on."),
        ("name *", "Up to 80 characters."),
        ("tagline *", "One line, up to 120 characters."),
        ("summary", "What it does, up to 900 characters."),
        ("watching", "Why we're watching, or how ForkMesh uses it, up to "
                     "400 characters."),
        ("url *", "The project's http(s) homepage."),
        ("repo", "Its source repository, if public."),
        ("category *", ", ".join(key for key, _label in CATEGORIES) + "."),
        ("tags", "Up to %d short words." % MAX_TAGS),
        ("openSource", "true, false, or null when unknown."),
        ("license", "An SPDX id such as MIT."),
        ("gitNative", "true when it hosts, replicates or stores its "
                      "collaboration data in Git."),
        ("maker", "The company or maintainer."),
        ("compatibility", ", ".join(key for key, _label in COMPATIBILITY)
         + " (default unknown)."),
        ("integration", ", ".join(key for key, _label in INTEGRATION)
         + " (default watching)."),
        ("integrations", "Up to %d ForkMesh integrations that exist today, "
                         "such as \"Import issues\", or, for a project "
                         "ForkMesh uses, where it uses it, such as "
                         "\"Desktop app: SSH to hosts\"." % MAX_INTEGRATIONS),
        ("reviewed", "\"2026-09\" or \"2026-09-25\". Defaults to this month."),
        ("published", "true to show it publicly. New projects are drafts."),
        ("featured", "true to show it in the landing page section."),
        ("position", "Sort order, lowest first. New projects go last."),
        ("internal", "Private, never public: overlap, threat, opportunity "
                     "and confidence (low, medium, high), action ("
                     + ", ".join(ACTIONS) + "), notes, sources (URLs), and "
                     "any other key you need."),
    )
    return (
        '<details class="more-text" style="margin-top:14px"><summary>JSON '
        'format</summary><p class="hint">Paste one object, a list of them, '
        'or an export. An import merges by slug: fields you include replace '
        'the stored ones, fields you leave out are kept, and null clears a '
        'field. * is required for a new project.</p><dl class="kv">%s</dl>'
        '</details>' % "".join(
            '<dt class="mono">%s</dt><dd>%s</dd>' % (_esc(field), _esc(text))
            for field, text in rows))


async def _list_page(env, request, params, csrf_field, admin_query, account,
                     now, entries, meta, import_text="", errors=None,
                     status=200):
    shell = _admin_shell
    tab = _first(params, "status")
    tab = tab if tab in ("published", "drafts") else "all"
    notice = _first(params, "notice")
    slug = clean_slug(_first(params, "slug"))
    banner = (_imported_banner(params) if notice == "imported"
              else _notice_banner(notice, slug)) + _errors_banner(errors)
    published = [entry for entry in entries if entry["published"]]
    drafts = [entry for entry in entries if not entry["published"]]
    featured = [entry for entry in published if entry["featured"]]
    shown = {"all": entries, "published": published, "drafts": drafts}[tab]
    categories = len({entry["category"] for entry in published})
    kpis = (
        '<div class="grid cols-4">'
        + shell.kpi("Published", shell.format_int(len(published)),
                    "Listed on /ecosystem")
        + shell.kpi("Drafts", shell.format_int(len(drafts)),
                    "Only visible here")
        + shell.kpi("Featured", shell.format_int(len(featured)),
                    "Shown first on the landing page")
        + shell.kpi("Categories", shell.format_int(categories),
                    "With at least one published project")
        + '</div>')
    importer = shell.card(
        '<form method="post" action="%s">%s'
        '<input type="hidden" name="action" value="import">'
        '<label class="sr" for="eco-json">Projects as JSON</label>'
        '<textarea id="eco-json" class="mono" name="json" rows="12" '
        'spellcheck="false" placeholder="%s">%s</textarea>'
        '<div class="form-row" style="justify-content:space-between;'
        'margin-top:12px"><span class="hint">New projects are saved as '
        'drafts unless they set "published": true. Private fields go under '
        '"internal" and never leave the admin.</span>'
        '<button class="btn btn-primary" type="submit">Import projects'
        '</button></div></form>%s' % (
            _href(admin_query), csrf_field, _esc(EXAMPLE), _esc(import_text),
            _reference_html()),
        title="Import JSON",
        description="Add or update several projects at once.")
    tabs = shell.tabs([
        (key, label, _href(admin_query, status=None if key == "all" else key),
         shell.format_int(len({"all": entries, "published": published,
                               "drafts": drafts}[key])))
        for key, label in TABS], tab)
    table = shell.table(
        "<th>Project</th><th>Status</th>"
        "<th>ForkMesh relationship</th><th>Assessment</th><th>Reviewed</th>"
        '<th class="r">Actions</th>',
        [_row_html(entry, csrf_field, admin_query, tab) for entry in shown],
        {"all": "No projects yet. Import some JSON above.",
         "published": "Nothing is published yet.",
         "drafts": "No drafts."}[tab])
    listing = ('<section class="card">%s<div class="card-t">%s</div>'
               '</section>' % (tabs, table))
    actions = (
        '<a class="btn" href="/ecosystem" target="_blank" rel="noopener">'
        'View public page</a><a class="btn" href="%s">Export JSON</a>' % (
            _href(admin_query, format="json")))
    content = (
        shell.page_head(
            "Ecosystem",
            "Projects listed on forkmesh.com/ecosystem and in the landing "
            "page section. Only published projects, and only their public "
            "fields, ever leave this page.",
            crumb="Growth", actions=actions)
        + banner + kpis + importer + listing)
    return await _page(env, request, account, "Ecosystem", content, status)


def _kv(pairs):
    return '<dl class="kv">%s</dl>' % "".join(
        "<dt>%s</dt><dd>%s</dd>" % (_esc(label), value)
        for label, value in pairs)


def _text_or_dash(value):
    if value in (None, "", []):
        return '<span class="muted">—</span>'
    if isinstance(value, list):
        return _esc(", ".join(str(item) for item in value))
    if isinstance(value, bool):
        return "true" if value else "false"
    return _esc(value)


async def _edit_page(env, request, params, csrf_field, admin_query, account,
                     now, entry, meta, text=None, errors=None, status=200):
    shell = _admin_shell
    slug = entry["slug"]
    if text is None:
        text = json.dumps(export_entry(entry), indent=2, ensure_ascii=False)
    banner = _notice_banner(_first(params, "notice"), slug) + _errors_banner(
        errors)
    crumb = '<a href="%s">Ecosystem</a><span>/</span><span class="mono">%s' \
            '</span>' % (_href(admin_query), _esc(slug))
    actions = ""
    if entry["published"]:
        actions = ('<a class="btn" href="/ecosystem/%s" target="_blank" '
                   'rel="noopener">View public page</a>' % _esc(slug))
    editor = shell.card(
        '<form method="post" action="%s">%s'
        '<input type="hidden" name="action" value="save">'
        '<input type="hidden" name="project" value="%s">'
        '<label class="sr" for="eco-entry">Project JSON</label>'
        '<textarea id="eco-entry" class="mono" name="json" rows="30" '
        'spellcheck="false">%s</textarea>'
        '<div class="form-row" style="justify-content:space-between;'
        'margin-top:12px"><span class="hint">Saving replaces the whole '
        'project: a field you remove goes back to its default.</span>'
        '<div class="actions"><a class="btn" href="%s">Cancel</a>'
        '<button class="btn btn-primary" type="submit">Save</button></div>'
        '</div></form>' % (
            _href(admin_query), csrf_field, _esc(slug), _esc(text),
            _href(admin_query)),
        title="Edit as JSON")
    public = shell.card(
        _kv([
            ("Status", "Published" if entry["published"] else "Draft"),
            ("Category", _esc(CATEGORY_LABELS.get(entry["category"], "—"))),
            ("Tagline", _text_or_dash(entry["tagline"])),
            ("Compatible", _esc(COMPATIBILITY_LABELS.get(
                entry["compatibility"], "—"))),
            ("Integration", _esc(INTEGRATION_LABELS.get(
                entry["integration"], "—"))),
            ("Integrations", _text_or_dash(entry["integrations"])),
            ("Last reviewed", _esc(reviewed_label(entry["reviewed"]) or "—")),
        ]), title="Public card",
        description="What visitors see on /ecosystem/%s." % slug)
    internal = entry.get("internal") or {}
    private = shell.card(
        _kv([(key, _text_or_dash(value)) for key, value in internal.items()])
        if internal else shell.empty("No private notes yet."),
        title="Private assessment",
        description="Stored under \"internal\". Never public.")
    history = shell.card(_kv([
        ("Added", _esc(shell.utc_label(meta.get("created_at")))),
        ("Updated", _esc(shell.utc_label(meta.get("updated_at")))),
        ("Updated by", _text_or_dash(meta.get("updated_by"))),
    ]), title="History")
    danger = shell.card(
        '<form class="form-row" method="post" action="%s">%s'
        '<input type="hidden" name="action" value="delete">'
        '<input type="hidden" name="project" value="%s">'
        '<label class="form-row"><input type="checkbox" name="confirm" '
        'value="yes" required> Delete %s for good</label>'
        '<button class="btn btn-danger" type="submit">Delete project</button>'
        '</form>' % (_href(admin_query), csrf_field, _esc(slug), _esc(slug)),
        title="Delete",
        description="Removes the project and its private notes. Export "
                    "first if you may want it back.")
    content = (
        shell.page_head(entry["name"], entry["tagline"], crumb=crumb,
                        actions=actions)
        + banner
        + '<div class="grid split"><div class="stack">' + editor + danger
        + '</div><div class="stack">' + public + private + history
        + '</div></div>')
    return await _page(env, request, account, entry["name"], content, status)


IMPORT_CONTEXT = "forkmesh-ecosystem-import-v1"
IMPORT_ACTION = "admin.ecosystem_api_import"
IMPORT_TARGET = "ecosystem_request"
IMPORT_MAX_BYTES = 2 * 1024 * 1024
REQUEST_ID_RE = re.compile(r"^[A-Za-z0-9_-]{16,64}$")


def import_canonical(node, ts, request_id, payload_text):
    digest = hashlib.sha256(payload_text.encode("utf-8")).hexdigest()
    return "\n".join((IMPORT_CONTEXT, node, ts, request_id, digest)).encode(
        "utf-8")


def _no_store(payload, status=200):
    return json_response(payload, status=status, cache_control="no-store")


async def _request_spent(env, node, request_id, now):
    row = await d1_first(
        env,
        "SELECT 1 AS hit FROM sensitive_audit_log WHERE actor_bi=? AND ts>=? "
        "AND target_token=? LIMIT 1",
        await blind_index(env, node), now - 2 * LOGIN_MAX_SKEW_MS,
        await blind_index(env, "audit:%s:%s" % (IMPORT_TARGET, request_id)))
    return bool(row)


async def import_api_handler(env, request):
    if method_name(request) != "POST":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            cache_control="no-store", extra_headers={"allow": "POST"})
    try:
        raw = await bounded_request_text(request, IMPORT_MAX_BYTES)
    except RequestBodyTooLarge:
        return _no_store({"error": "payload_too_large"}, 413)
    try:
        body = json.loads(raw or "{}")
    except ValueError:
        return _no_store({"error": "invalid_json"}, 400)
    if not isinstance(body, dict):
        return _no_store({"error": "invalid_json"}, 400)
    node = str(body.get("node") or "").strip().lower()[:64]
    ts = str(body.get("ts") or "")[:20]
    sig = str(body.get("sig") or "")[:200]
    request_id = str(body.get("requestId") or "")
    payload_text = body.get("payload")
    if (not node or not isinstance(payload_text, str)
            or not REQUEST_ID_RE.match(request_id)):
        return _no_store({"error": "invalid_request"}, 400)
    await ensure_schema(env)
    canonical = import_canonical(node, ts, request_id, payload_text)
    if not await _admin_authorized(env, node, ts, sig, canonical):
        return _no_store({"error": "unauthorized"}, 401)
    now = _now()
    if await _request_spent(env, node, request_id, now):
        return _no_store({"error": "replayed_request"}, 409)
    await _audit_sensitive_action(
        env, node, IMPORT_ACTION, IMPORT_TARGET, request_id, "requested", {})
    loaded = await load(env)
    if loaded is None:
        return _no_store({"error": "unavailable"}, 503)
    entries, _meta = loaded
    items, errors = parse_payload(payload_text)
    changes = []
    if not errors:
        changes, errors = plan_import(items, entries, now)
    if errors:
        await _audit_sensitive_action(
            env, node, IMPORT_ACTION, IMPORT_TARGET, request_id, "failed",
            {"errors": len(errors)})
        return _no_store({"ok": False, "error": "invalid_projects",
                          "errors": errors[:MAX_SHOWN_ERRORS],
                          "errorCount": len(errors)}, 400)
    writes = [(UPSERT_SQL, row_args(entry, now, node))
              for _slug, entry, status in changes if status != "unchanged"]
    if writes:
        await d1_run_batch(env, writes)
        await edge_cache_delete(API_CACHE_KEY)
    counts = {key: sum(1 for change in changes if change[2] == key)
              for key in ("added", "updated", "unchanged")}
    await _audit_sensitive_action(
        env, node, IMPORT_ACTION, IMPORT_TARGET, request_id, "success", counts)
    return _no_store(dict(ok=True, **counts))
