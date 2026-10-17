import json
import re
from urllib.parse import quote, unquote, urlencode

from static_routes import REPO_TAB_ROUTES

SETTING_KEY = "repo_projects"
CACHE_MS = 30 * 1000
MAX_DOC_BYTES = 64 * 1024
MAX_ROWS = 500
MAX_ALIASES = 20
NAME_RE = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9._-]{0,99}$")
FOLDER_RE = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9._-]{0,99}"
                       r"(?:/[A-Za-z0-9_][A-Za-z0-9._-]{0,99}){0,7}$")
OWNER_RE = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,62})$")
REF_RE = re.compile(r"^[A-Za-z0-9._/-]{1,200}$")
SHA_RE = re.compile(r"^[0-9a-fA-F]{4,64}$")
RETIRED = "-"
DEFAULT = {"repo": "", "table": "", "legacy": []}

_cache = {"at": -CACHE_MS, "value": None}


class SettingError(ValueError):
    pass


def parse_table(text):
    folders = {}
    names = {}
    for line in str(text or "").splitlines()[:MAX_ROWS + 50]:
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        cells = line.split("\t")
        folder = cells[0].strip()
        display = cells[1].strip() if len(cells) > 1 else ""
        aliases = [a.strip() for a in (cells[2] if len(cells) > 2 else "")
                   .split(",") if a.strip()][:MAX_ALIASES]
        if folder != RETIRED and not FOLDER_RE.fullmatch(folder):
            continue
        if folder != RETIRED:
            if not NAME_RE.fullmatch(display):
                display = folder.rsplit("/", 1)[-1]
            folders[folder] = display
            candidates = [folder, display] + aliases
        else:
            candidates = ([display] if display and display != RETIRED
                          else []) + aliases
        for name in candidates:
            if NAME_RE.fullmatch(name):
                names.setdefault(name.lower(), folder)
        if len(folders) >= MAX_ROWS:
            break
    return {"folders": folders, "names": names}


def parse(doc):
    try:
        value = json.loads(str(doc or "")[:MAX_DOC_BYTES] or "{}")
    except ValueError:
        value = {}
    if not isinstance(value, dict):
        value = {}
    repo = str(value.get("repo") or "").strip()
    if not NAME_RE.fullmatch(repo):
        repo = ""
    legacy = [str(o).strip().lower() for o in value.get("legacy") or []
              if isinstance(o, str)]
    legacy = [o for o in legacy if OWNER_RE.fullmatch(o)][:20]
    table = str(value.get("table") or "")[:MAX_DOC_BYTES]
    parsed = parse_table(table) if repo else {"folders": {}, "names": {}}
    return {"repo": repo, "table": table, "legacy": legacy,
            "folders": parsed["folders"], "names": parsed["names"]}


def clean(repo, table, legacy):
    repo = str(repo or "").strip()
    if repo and not NAME_RE.fullmatch(repo):
        raise SettingError("The repository must be a repository name.")
    table = str(table or "").replace("\r\n", "\n")
    if len(table.encode("utf-8")) > MAX_DOC_BYTES - 1024:
        raise SettingError("The project table is too long.")
    owners = []
    for owner in re.split(r"[\s,]+", str(legacy or "")):
        owner = owner.strip().lower().lstrip("@")
        if not owner:
            continue
        if not OWNER_RE.fullmatch(owner):
            raise SettingError("Old owners must be account or organization "
                               "names.")
        if owner not in owners:
            owners.append(owner)
    if repo and not parse_table(table)["folders"]:
        raise SettingError("The project table has no valid rows.")
    return {"repo": repo, "table": table if repo else "",
            "legacy": owners[:20] if repo else []}


def dump(setting):
    return json.dumps({"repo": setting.get("repo") or "",
                       "table": setting.get("table") or "",
                       "legacy": list(setting.get("legacy") or [])},
                      sort_keys=True, separators=(",", ":"))


def cached(now_ms):
    if _cache["value"] is not None and now_ms - _cache["at"] < CACHE_MS:
        return _cache["value"]
    return None


def remember(value, now_ms):
    _cache["value"] = value
    _cache["at"] = now_ms
    return value


def forget():
    _cache["value"] = None
    _cache["at"] = -CACHE_MS


def _segments(path):
    return [unquote(p) for p in str(path or "").split("/") if p]


def _join(*parts):
    return "/" + "/".join(quote(str(p), safe="") for p in parts if p != "")


def _with_query(path, params):
    params = [(k, v) for k, v in params if v]
    return path + ("?" + urlencode(params) if params else "")


def _project_target(repo, folder, tail, query):
    if folder == RETIRED:
        return _join(repo)
    if not tail:
        return _join(repo, "tree", *folder.split("/"))
    kind = tail[0].lower()
    rest = tail[1:]
    if kind in ("tree", "blob", "raw"):
        ref = rest[0] if rest and REF_RE.fullmatch(rest[0]) else ""
        inner = rest[1:] if ref else rest
        path = "/".join([folder] + list(inner))
        if kind == "raw":
            return None, _with_query(
                "/api/repo/{owner}/%s/raw" % quote(repo, safe=""),
                [("path", path), ("ref", ref)])
        return _with_query(_join(repo, kind, *path.split("/")),
                           [("ref", ref)])
    if kind == "commits":
        ref = "/".join(rest)
        return _with_query(_join(repo, "commits"),
                           [("path", folder),
                            ("ref", ref if REF_RE.fullmatch(ref) else "")])
    if kind == "issues" and not rest:
        return _with_query(_join(repo, "issues"), [("label", folder)])
    if kind in REPO_TAB_ROUTES:
        return _join(repo, kind, *rest)
    return _join(repo, "tree", *(folder.split("/") + list(tail)))


def redirect(path, query, setting, owner=""):
    repo = setting.get("repo") or ""
    names = setting.get("names") or {}
    folders = setting.get("folders") or {}
    if not repo or not names:
        return None
    parts = _segments(path)
    if len(parts) >= 2 and parts[0].lower() in setting.get("legacy", ()):
        name = parts[1]
        if name.lower().endswith(".git") or (
                parts[0].lower() == str(owner or "").lower()
                and name.lower() == repo.lower()):
            return None
        folder = names.get(name.lower())
        if folder is None:
            return None
        if folder == RETIRED:
            return _join(repo)
        target = _join(repo, folders.get(folder, folder), *parts[2:])
        return target + ("?" + query if query else "")
    if len(parts) < 2 or parts[0].lower() != repo.lower():
        return None
    head = parts[1]
    if head.lower() == "commit" and len(parts) == 3 and \
            SHA_RE.fullmatch(parts[2]):
        return _with_query(_join(repo, "commits"), [("ref", parts[2])])
    folder = names.get(head.lower())
    if folder is None:
        return None
    target = _project_target(repo, folder, parts[2:], query)
    if isinstance(target, tuple):
        return target[1].replace("{owner}", quote(owner, safe="")) \
            if owner else None
    return target
