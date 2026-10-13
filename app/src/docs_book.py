import hashlib
import re
import struct
import time
import zlib
from urllib.parse import quote

from hosted_repos import noreply_email

OWNER = "forkmesh"
REPO = "forkmesh"
ROOT = "docs"
BRANCH = "main"
READ_TIMEOUT_SECONDS = 25
CACHE_TTL_MS = 30 * 1000
MAX_DIRECTORIES = 200
MAX_DOCS = 400
BLOB_BATCH = 40
BLOB_QUERY_CHARS = 3500
MAX_CONTENT_BYTES = 2 * 1024 * 1024
MAX_REQUEST_BYTES = 5 * 1024 * 1024
MAX_MESSAGE = 200
OID_RE = re.compile(r"^[0-9a-f]{40}$")
PATH_RE = re.compile(r"^docs/(?:[A-Za-z0-9._-]+/)*[A-Za-z0-9._-]+\.md$")
FILE_MODES = frozenset({"100644", "100755"})
OBJECT_TYPES = {"commit": 1, "tree": 2, "blob": 3}

_CACHE = {"commit": "", "at": 0, "docs": None}


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


class DocsBookError(Exception):
    def __init__(self, code, message, status=400):
        super().__init__(message)
        self.code = code
        self.message = message
        self.status = status


def valid_path(path):
    path = str(path or "")
    return bool(PATH_RE.match(path)) and not any(
        part in ("", ".", "..") or part.startswith(".")
        for part in path.split("/"))


def object_id(kind, payload):
    return hashlib.sha1(
        kind.encode("ascii") + b" " + str(len(payload)).encode("ascii")
        + b"\0" + payload).hexdigest()


def tree_payload(rows):
    out = bytearray()
    for mode, name, oid in rows:
        out += mode.lstrip("0").encode("ascii") + b" "
        out += name.encode("utf-8") + b"\0" + bytes.fromhex(oid)
    return bytes(out)


def rebuilt_trees(listings, path, blob_oid):
    parts = path.split("/")
    objects = []
    child = blob_oid
    for depth in range(len(parts) - 1, -1, -1):
        directory = "/".join(parts[:depth])
        name = parts[depth]
        rows = []
        found = False
        for entry in listings.get(directory) or []:
            mode = str(entry.get("mode") or "")
            oid = str(entry.get("oid") or "").lower()
            entry_name = str(entry.get("name") or "")
            if not mode or not OID_RE.match(oid) or not entry_name:
                raise DocsBookError(
                    "gateway_outdated",
                    "The repository host does not report object ids yet.",
                    503)
            if entry_name == name:
                found = True
                oid = child
            rows.append((mode, entry_name, oid))
        if not found:
            raise DocsBookError(
                "not_found", "The file is no longer in the repository.", 404)
        payload = tree_payload(rows)
        child = object_id("tree", payload)
        objects.append(("tree", payload))
    return child, objects


def commit_payload(tree, parent, name, email, seconds, message):
    stamp = "%s <%s> %d +0000" % (name, email, int(seconds))
    return (
        "tree %s\nparent %s\nauthor %s\ncommitter %s\n\n%s\n"
        % (tree, parent, stamp, stamp, message.strip())
    ).encode("utf-8")


def _pack_header(kind, size):
    byte = (OBJECT_TYPES[kind] << 4) | (size & 0x0F)
    size >>= 4
    out = bytearray()
    while size:
        out.append(byte | 0x80)
        byte = size & 0x7F
        size >>= 7
    out.append(byte)
    return bytes(out)


def pack(objects):
    out = bytearray(b"PACK" + struct.pack(">II", 2, len(objects)))
    for kind, payload in objects:
        out += _pack_header(kind, len(payload))
        out += zlib.compress(payload)
    out += hashlib.sha1(bytes(out)).digest()
    return bytes(out)


def pkt_line(text):
    data = text.encode("utf-8")
    return ("%04x" % (len(data) + 4)).encode("ascii") + data


def receive_pack_body(old, new, ref, pack_bytes):
    return (
        pkt_line("%s %s %s\0report-status agent=forkmesh-docs\n"
                 % (old, new, ref))
        + b"0000" + pack_bytes)


def report_status(raw, ref):
    if isinstance(raw, str):
        raw = raw.encode("utf-8", "replace")
    lines = []
    position = 0
    while position + 4 <= len(raw):
        try:
            length = int(raw[position:position + 4].decode("ascii"), 16)
        except (UnicodeDecodeError, ValueError):
            break
        if length == 0:
            position += 4
            continue
        if length < 4:
            break
        lines.append(raw[position + 4:position + length].decode(
            "utf-8", "replace").rstrip("\n"))
        position += length
    unpacked = "unpack ok" in lines
    for line in lines:
        if line == "ok " + ref:
            return unpacked, "" if unpacked else "unpack failed"
        if line.startswith("ng " + ref + " "):
            return False, line[len("ng " + ref + " "):]
    return False, next(
        (line for line in lines if line.startswith("unpack ")),
        "no report from the repository host")


def build_commit(listings, parent, path, content, name, email, seconds,
                 message):
    blob = content.encode("utf-8")
    blob_oid = object_id("blob", blob)
    root, trees = rebuilt_trees(listings, path, blob_oid)
    commit = commit_payload(root, parent, name, email, seconds, message)
    commit_oid = object_id("commit", commit)
    objects = [("blob", blob)] + trees + [("commit", commit)]
    return commit_oid, pack(objects)


def order_key(path):
    directory, _, name = path.rpartition("/")
    return (directory.lower(), name != "README.md", name.lower())


def clean_message(value, path, fallback):
    text = " ".join(str(value or "").split())[:MAX_MESSAGE]
    return text or fallback % path


def _error(code, message, status):
    return json_response(
        {"ok": False, "error": code, "message": message}, status=status,
        cache_control="no-store")


async def _read(env, action):
    origin = _public_base_url(env).rstrip("/")
    try:
        response = await js_fetch_with_timeout(
            "%s/api/repo/%s/%s/%s" % (origin, quote(OWNER), quote(REPO), action),
            {"method": "GET"}, READ_TIMEOUT_SECONDS)
        if int(getattr(response, "status", 0) or 0) != 200:
            return None
        data = await response.json()
        if hasattr(data, "to_py"):
            data = data.to_py()
    except Exception:
        return None
    return data if isinstance(data, dict) else None


async def _tree(env, path, ref):
    data = await _read(
        env, "tree?path=%s&ref=%s" % (quote(path, safe=""), quote(ref, safe="")))
    if not data or not isinstance(data.get("entries"), list):
        return None
    return data


async def _collect(env, commit, root_listing):
    files = []
    pending = [(ROOT, root_listing)]
    seen = 0
    while pending:
        directory, listing = pending.pop(0)
        seen += 1
        for entry in listing.get("entries") or []:
            name = str(entry.get("name") or "")
            path = directory + "/" + name
            if entry.get("type") == "tree":
                if seen + len(pending) < MAX_DIRECTORIES:
                    child = await _tree(env, path, commit)
                    if child is not None:
                        pending.append((path, child))
            elif entry.get("type") == "blob" and valid_path(path):
                files.append({
                    "path": path,
                    "oid": str(entry.get("oid") or ""),
                    "size": int(entry.get("size") or 0),
                    "updated": str(entry.get("date") or ""),
                    "author": str(entry.get("author") or ""),
                    "subject": str(entry.get("subject") or ""),
                    "change": str(entry.get("hash") or ""),
                })
    files.sort(key=lambda item: order_key(item["path"]))
    return files[:MAX_DOCS]


async def _contents(env, commit, files):
    batches = []
    batch = []
    chars = 0
    for item in files:
        piece = "path=" + quote(item["path"], safe="")
        if batch and (len(batch) >= BLOB_BATCH
                      or chars + len(piece) > BLOB_QUERY_CHARS):
            batches.append(batch)
            batch, chars = [], 0
        batch.append(item)
        chars += len(piece) + 1
    if batch:
        batches.append(batch)
    for batch in batches:
        query = "&".join(
            ["ref=" + quote(commit, safe="")]
            + ["path=" + quote(item["path"], safe="") for item in batch])
        data = await _read(env, "blobs?" + query)
        blobs = (data or {}).get("blobs") or {}
        for item in batch:
            blob = blobs.get(item["path"]) if isinstance(blobs, dict) else None
            if not isinstance(blob, dict):
                blob = await _read(env, "blob?path=%s&ref=%s" % (
                    quote(item["path"], safe=""), quote(commit, safe="")))
            if isinstance(blob, dict) and blob.get("encoding") == "utf8":
                item["content"] = str(blob.get("content") or "")
                item["truncated"] = bool(blob.get("truncated"))
            else:
                item["content"] = None
                item["truncated"] = False


async def _can_edit(env, request):
    try:
        actor = await _authed_account_name(env, request)
    except Exception:
        return ""
    if actor and await _is_admin(env, actor):
        return actor
    return ""


async def _list(env, request):
    head = await _tree(env, ROOT, BRANCH)
    if head is None:
        return _error(
            "unavailable", "The docs could not be read from the repository.",
            503)
    commit = str(head.get("commit") or "")
    now = int(time.time() * 1000)
    docs = _CACHE["docs"] if (
        _CACHE["commit"] == commit and now - _CACHE["at"] < CACHE_TTL_MS
    ) else None
    if docs is None:
        docs = await _collect(env, commit, head)
        await _contents(env, commit, docs)
        _CACHE.update({"commit": commit, "at": now, "docs": docs})
    latest = head.get("latestCommit") or {}
    return json_response({
        "ok": True,
        "repository": OWNER + "/" + REPO,
        "branch": BRANCH,
        "commit": commit,
        "latest": {
            "hash": str(latest.get("hash") or ""),
            "date": str(latest.get("date") or ""),
            "subject": str(latest.get("subject") or ""),
        },
        "canEdit": bool(await _can_edit(env, request)),
        "docs": docs,
    }, cache_control="no-store")


async def _listings(env, path):
    root = await _tree(env, "", BRANCH)
    if root is None:
        raise DocsBookError(
            "unavailable", "The repository could not be read.", 503)
    commit = str(root.get("commit") or "")
    if not OID_RE.match(commit):
        raise DocsBookError(
            "unsupported", "Only SHA-1 repositories can be edited here.", 409)
    listings = {"": root["entries"]}
    parts = path.split("/")
    for depth in range(1, len(parts)):
        directory = "/".join(parts[:depth])
        listing = await _tree(env, directory, commit)
        if listing is None:
            raise DocsBookError(
                "not_found", "The file is no longer in the repository.", 404)
        listings[directory] = listing["entries"]
    return commit, listings


def _current(listings, path):
    directory, _, name = path.rpartition("/")
    for entry in listings.get(directory) or []:
        if entry.get("name") == name:
            return entry
    return None


async def _push(env, request, actor, body):
    node = await _org_repo_node(env, OWNER, REPO)
    owner = node or OWNER
    alias = OWNER if node and node != OWNER else ""
    core_held = owner.lower() in _core_push_nodes(env)
    upstream, _endpoint = await _https_mirror_push_send(
        env, request, owner, REPO, actor, "POST", body, None,
        hashlib.sha256(body).hexdigest(), alias_owner=alias,
        core_held=core_held, credential="docs-page")
    if upstream is None:
        raise DocsBookError(
            "push_unavailable", "No repository host accepted the commit.", 503)
    status = int(getattr(upstream, "status", 0) or 0)
    try:
        text = str(await upstream.text())
    except Exception:
        text = ""
    if status != 200:
        raise DocsBookError(
            "push_refused", "The repository host refused the commit (HTTP %d)."
            % status, 502)
    ok, reason = report_status(text, "refs/heads/" + BRANCH)
    if not ok:
        stale = any(word in reason for word in (
            "fetch first", "non-fast-forward", "stale", "lock"))
        raise DocsBookError(
            "conflict" if stale else "push_refused",
            "main moved while saving; reload and try again." if stale
            else "The repository host refused the commit: " + reason,
            409 if stale else 502)


async def _save(env, request):
    try:
        data = await bounded_json_request(request, max_bytes=MAX_REQUEST_BYTES)
    except Exception:
        return _error("invalid_json", "The request body is not JSON.", 400)
    if not isinstance(data, dict):
        data = {}
    actor = await _authed_account_name(env, request, data)
    if not actor or not await _is_admin(env, actor):
        return _error("not_admin", "Only admins can edit the docs.", 403)
    path = str(data.get("path") or "")
    content = data.get("content")
    base = str(data.get("oid") or "").lower()
    if not valid_path(path):
        return _error("invalid_path", "That is not a docs Markdown file.", 400)
    if not isinstance(content, str):
        return _error("invalid_content", "The new text is missing.", 400)
    content = content.replace("\r\n", "\n").replace("\r", "\n")
    if len(content.encode("utf-8")) > MAX_CONTENT_BYTES:
        return _error("too_large", "The file is larger than 2 MB.", 413)
    try:
        parent, listings = await _listings(env, path)
        entry = _current(listings, path)
        if entry is None or entry.get("type") != "blob":
            raise DocsBookError(
                "not_found", "The file is no longer in the repository.", 404)
        if not entry.get("mode") or not OID_RE.match(
                str(entry.get("oid") or "").lower()):
            raise DocsBookError(
                "gateway_outdated",
                "The repository host does not report object ids yet.", 503)
        if str(entry.get("mode") or "") not in FILE_MODES:
            raise DocsBookError(
                "invalid_path", "That path is not a regular file.", 400)
        current = str(entry.get("oid") or "").lower()
        if base and current and base != current:
            raise DocsBookError(
                "conflict",
                "Someone changed this file since you opened it; reload to "
                "see their version.", 409)
        if current == object_id("blob", content.encode("utf-8")):
            return json_response(
                {"ok": True, "unchanged": True, "path": path, "oid": current,
                 "commit": parent}, cache_control="no-store")
        message = clean_message(
            data.get("message"), path, "Docs: edit %s on the web")
        commit_oid, pack_bytes = build_commit(
            listings, parent, path, content, actor,
            noreply_email(actor, _public_base_url(env)),
            int(time.time()), message)
        body = receive_pack_body(
            parent, commit_oid, "refs/heads/" + BRANCH, pack_bytes)
        await _push(env, request, actor, body)
    except DocsBookError as exc:
        return _error(exc.code, exc.message, exc.status)
    _CACHE.update({"commit": "", "at": 0, "docs": None})
    try:
        await _audit_sensitive_action(
            env, actor, "docs.edit", "repository", OWNER + "/" + REPO,
            "success", {"path": path, "commit": commit_oid,
                        "parent": parent})
    except Exception:
        pass
    return json_response({
        "ok": True,
        "path": path,
        "commit": commit_oid,
        "oid": object_id("blob", content.encode("utf-8")),
        "updated": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "author": actor,
        "subject": message,
    }, cache_control="no-store")


async def route(env, request, path):
    method = method_name(request)
    if path in ("/api/docs-book", "/api/docs-book/"):
        if method not in ("GET", "HEAD"):
            return _error("method_not_allowed", "Use GET.", 405)
        return await _list(env, request)
    if path == "/api/docs-book/save":
        if method != "POST":
            return _error("method_not_allowed", "Use POST.", 405)
        return await _save(env, request)
    return _error("not_found", "Not found.", 404)
