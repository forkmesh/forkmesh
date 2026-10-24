import json
import re

SETTING_KEY = "home"
REPOSITORY_RE = re.compile(r"^([a-z0-9][a-z0-9_-]{0,63})/([A-Za-z0-9._-]{1,100})$")


class SettingError(ValueError):
    pass


def clean(value):
    text = str(value or "").strip().strip("/")
    if not text:
        return ""
    owner, _sep, repo = text.partition("/")
    text = "%s/%s" % (owner.lower(), repo)
    if not REPOSITORY_RE.match(text) or repo in (".", "..") or repo.endswith(".git"):
        raise SettingError("Name a repository as owner/name, for example example/forum.")
    return text


def parse(doc):
    try:
        value = json.loads(doc or "{}")
    except ValueError:
        return ""
    if not isinstance(value, dict):
        return ""
    try:
        return clean(value.get("repository"))
    except SettingError:
        return ""


def dump(repository):
    return json.dumps({"repository": clean(repository)}, separators=(",", ":"))


def public(repository):
    return {"repository": repository} if repository else None
