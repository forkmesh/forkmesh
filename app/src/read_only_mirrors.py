import re
from urllib.parse import urlsplit

MIRROR_OF_RE = re.compile(
    r"https://[A-Za-z0-9.-]+(?::[0-9]{1,5})?(?:/[A-Za-z0-9._~-]+){1,8}")
COMMIT_RE = re.compile(r"[0-9a-f]{40}|[0-9a-f]{64}")
MILLIS_RE = re.compile(r"[1-9][0-9]{0,15}")
WRITE_PATH_RE = re.compile(
    r"/api/repo/[^/]+/[^/]+/(?:file-edit|branches/(?:ops|delete)|pulls"
    r"|pulls/[1-9][0-9]{0,8}/(?:merge|update))/?")
SAFE_METHODS = frozenset({"GET", "HEAD", "OPTIONS"})
PLATFORM_REPOSITORY = "forkmesh"
MIRROR_CLOSED_FEATURES = ("issues", "pulls", "discussions")


def upstream_page(value):
    page = str(value or "").strip().rstrip("/")
    if page.endswith(".git"):
        page = page[:-4]
    return page if MIRROR_OF_RE.fullmatch(page) else ""


def upstream_label(page):
    path = urlsplit(page).path.strip("/") if page else ""
    return path


def _millis(value):
    text = str(value if value is not None else "").strip()
    return int(text) if MILLIS_RE.fullmatch(text) else 0


def _commit(value):
    text = str(value or "").strip().lower()
    return text if COMMIT_RE.fullmatch(text) else ""


def not_a_mirror():
    return {"mirrorOf": "", "readOnly": False, "lastSyncedAt": 0,
            "upstreamCommit": ""}


def mirror_fields(record, instance_upstream="", flagship_node="", owner="",
                  repo=""):
    record = record if isinstance(record, dict) else {}
    page = upstream_page(record.get("mirrorOf"))
    if not page and instance_upstream and flagship_node and (
            str(owner or "").strip().lower() == flagship_node
            and str(repo or "").strip().lower() == PLATFORM_REPOSITORY):
        page = upstream_page(instance_upstream)
    if not page:
        return not_a_mirror()
    return {
        "mirrorOf": page,
        "readOnly": True,
        "lastSyncedAt": (_millis(record.get("lastSyncedAt"))
                         or _millis(record.get("lastSync"))),
        "upstreamCommit": (_commit(record.get("upstreamCommit"))
                           or _commit(record.get("commit"))),
    }


def is_write_request(method, path):
    return (str(method or "").upper() not in SAFE_METHODS
            and bool(WRITE_PATH_RE.fullmatch(str(path or ""))))


def refusal_message(page):
    return ("This repository is a read-only mirror of %s. Send changes "
            "upstream there, or fork this repository and change your fork."
            % page)


def refusal_body(page):
    return {"ok": False, "error": "read_only_mirror",
            "message": refusal_message(page), "mirrorOf": page}


def about_fields(features, fields):
    features = dict(features) if isinstance(features, dict) else {}
    if fields.get("readOnly"):
        features.update({kind: False for kind in MIRROR_CLOSED_FEATURES})
    return dict(fields, features=features)
