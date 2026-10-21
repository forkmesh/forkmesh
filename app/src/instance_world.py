import json
import re
from urllib.parse import urlsplit

SETTING_KEY = "world"
STYLES = ("meadow", "security")
MAX_DOC_BYTES = 8 * 1024
MAX_OBJECTS = 12
MAX_BUGS = 60
REACH = 300
LABEL_CHARS = 40
URL_CHARS = 300
HEX_RE = re.compile(r"^#[0-9a-f]{6}$")
DOMAIN_RE = re.compile(
    r"^(?=.{1,253}$)(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\.)+"
    r"[a-z][a-z0-9-]{0,61}[a-z0-9]$")
REPO_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,99}/"
                     r"[A-Za-z0-9][A-Za-z0-9._-]{0,99}$")
REF_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]{0,99}$")
PATH_RE = re.compile(r"^[A-Za-z0-9._-][A-Za-z0-9._/-]{0,199}$")
CONTROL_RE = re.compile(r"[\x00-\x1f\x7f]")
STARTER = {
    "style": "meadow",
    "accent": "",
    "objects": [],
    "bugs": None,
}


class SettingError(ValueError):
    pass


def default():
    return json.loads(json.dumps(STARTER))


def _text(value, limit):
    return " ".join(CONTROL_RE.sub(" ", str(value or "")).split())[:limit]


def clean_link(value):
    url = str(value or "").strip()
    if not url or len(url) > URL_CHARS or CONTROL_RE.search(url) or \
            re.search(r"[\s<>\"'`\\]", url):
        raise SettingError("A building link must be a full https address.")
    try:
        parts = urlsplit(url)
    except ValueError as error:
        raise SettingError("A building link is not a valid address.") \
            from error
    host = str(parts.hostname or "").lower()
    if parts.scheme != "https" or parts.username or parts.password or \
            not DOMAIN_RE.fullmatch(host):
        raise SettingError("A building link must be a full https address.")
    return url


def _coordinate(value):
    try:
        number = float(value)
    except (TypeError, ValueError) as error:
        raise SettingError("A position is two numbers, [x, z].") from error
    if number != number or abs(number) > REACH:
        raise SettingError("A position stays within %d of the plaza." % REACH)
    return round(number, 1)


def _position(value, fallback):
    if value is None:
        return list(fallback)
    if not isinstance(value, (list, tuple)) or len(value) != 2:
        raise SettingError("A position is two numbers, [x, z].")
    return [_coordinate(value[0]), _coordinate(value[1])]


def _color(value):
    color = str(value or "").strip().lower()
    if color and not HEX_RE.fullmatch(color):
        raise SettingError("Colours are written as #rrggbb.")
    return color


def repo_path(value):
    parts = []
    for part in str(value or "").split("/"):
        if part in ("", "."):
            continue
        if part == "..":
            raise SettingError("The bug list path may not leave the "
                               "repository.")
        parts.append(part)
    path = "/".join(parts)
    if not PATH_RE.fullmatch(path):
        raise SettingError("The bug list path is a file in the repository, "
                           "such as .forkmesh/world/bugs.json.")
    return path


def _object(item):
    if not isinstance(item, dict):
        raise SettingError("Each building is an object.")
    kind = str(item.get("type") or "house")
    if kind != "house":
        raise SettingError("Buildings are of type house.")
    label = _text(item.get("label"), LABEL_CHARS)
    if not label:
        raise SettingError("Each building needs a label.")
    return {"type": "house", "label": label,
            "url": clean_link(item.get("url")),
            "position": _position(item.get("position"), (0, -60)),
            "color": _color(item.get("color"))}


def _bugs(value):
    if value in (None, False, {}):
        return None
    if not isinstance(value, dict):
        raise SettingError("The bug terrarium is an object.")
    repo = str(value.get("repo") or "").strip()
    ref = str(value.get("ref") or "main").strip()
    if not REPO_RE.fullmatch(repo):
        raise SettingError("The bug list repository is owner/name.")
    if not REF_RE.fullmatch(ref) or ".." in ref:
        raise SettingError("The bug list branch is not valid.")
    try:
        limit = int(value.get("limit") or MAX_BUGS)
    except (TypeError, ValueError) as error:
        raise SettingError("The bug limit is a number.") from error
    return {"repo": repo, "ref": ref, "path": repo_path(value.get("path")),
            "title": _text(value.get("title"), LABEL_CHARS) or "Bug terrarium",
            "limit": max(1, min(MAX_BUGS, limit)),
            "position": _position(value.get("position"), (70, 40))}


def clean(value):
    if not isinstance(value, dict):
        raise SettingError("The World settings are an object.")
    unknown = sorted(set(value) - {"style", "accent", "objects", "bugs"})
    if unknown:
        raise SettingError("Unknown World setting: %s" % str(unknown[0])[:40])
    style = str(value.get("style") or "meadow").strip().lower()
    if style not in STYLES:
        raise SettingError("The style is one of: %s." % ", ".join(STYLES))
    objects = value.get("objects") or []
    if not isinstance(objects, list) or len(objects) > MAX_OBJECTS:
        raise SettingError("Up to %d buildings." % MAX_OBJECTS)
    return {"style": style, "accent": _color(value.get("accent")),
            "objects": [_object(item) for item in objects],
            "bugs": _bugs(value.get("bugs"))}


def dump(setting):
    doc = json.dumps(clean(setting), separators=(",", ":"), sort_keys=True)
    if len(doc.encode("utf-8")) > MAX_DOC_BYTES:
        raise SettingError("The World settings are too large.")
    return doc


def parse(doc):
    try:
        return clean(json.loads(str(doc or "")))
    except (ValueError, TypeError):
        return default()


def from_text(text):
    if len(str(text or "").encode("utf-8")) > MAX_DOC_BYTES:
        raise SettingError("The World settings are too large.")
    try:
        value = json.loads(str(text or "").strip() or "{}")
    except ValueError as error:
        raise SettingError("The World settings are not valid JSON.") \
            from error
    return clean(value)


def pretty(setting):
    return json.dumps(setting, indent=2, sort_keys=True)


def public(setting, logo_path=""):
    logo = str(logo_path or "")
    payload = {"style": setting["style"], "accent": setting["accent"],
               "objects": setting["objects"], "bugs": setting["bugs"]}
    if re.fullmatch(r"/instance-logo\.(?:png|jpg|jpeg|webp|svg)", logo):
        payload["logo"] = logo
    return payload
