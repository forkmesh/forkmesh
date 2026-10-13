import hashlib
import re
import time
from urllib.parse import quote

from docs_book import (OID_RE, clean_message, commit_payload, object_id, pack,
                       receive_pack_body, report_status, tree_payload)
from hosted_repos import noreply_email

READ_TIMEOUT_SECONDS = 25
MAX_CONTENT_BYTES = 2 * 1024 * 1024
MAX_REQUEST_BYTES = 5 * 1024 * 1024
FILE_MODES = frozenset({"100644", "100755"})
ZERO_OID = "0" * 40
BRANCH_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]{0,239}$")


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


class FileEditError(Exception):
    def __init__(self, code, message, status=400):
        super().__init__(message)
        self.code = code
        self.message = message
        self.status = status


def _error(code, message, status):
    return json_response(
        {"ok": False, "error": code, "message": message}, status=status,
        cache_control="no-store")


def valid_path(path):
    path = str(path or "")
    if not path or len(path.encode("utf-8")) > 1024:
        return False
    if path.startswith("/") or "\\" in path or "\0" in path:
        return False
    return not any(
        part in ("", ".", "..", ".git")
        for part in path.split("/"))


def valid_branch(branch):
    branch = str(branch or "")
    if not BRANCH_RE.match(branch):
        return False
    if branch.endswith((".", "/")) or branch.startswith("/") or "//" in branch:
        return False
    if ".." in branch or "@{" in branch or "\\" in branch:
        return False
    return not any(
        part in ("", ".", "..") or part.endswith(".lock")
        for part in branch.split("/"))


async def _read(env, owner, repo, action):
    origin = _public_base_url(env).rstrip("/")
    try:
        response = await js_fetch_with_timeout(
            "%s/api/repo/%s/%s/%s" % (
                origin, quote(owner), quote(repo), action),
            {"method": "GET"}, READ_TIMEOUT_SECONDS)
        if int(getattr(response, "status", 0) or 0) != 200:
            return None
        data = await response.json()
        if hasattr(data, "to_py"):
            data = data.to_py()
    except Exception:
        return None
    return data if isinstance(data, dict) else None


async def _tree(env, owner, repo, path, ref):
    data = await _read(
        env, owner, repo, "tree?path=%s&ref=%s" % (
            quote(path, safe=""), quote(ref, safe="")))
    if not data or not isinstance(data.get("entries"), list):
        return None
    return data


async def _blob(env, owner, repo, path, ref):
    data = await _read(
        env, owner, repo, "blob?path=%s&ref=%s" % (
            quote(path, safe=""), quote(ref, safe="")))
    if not isinstance(data, dict):
        return None
    return data


async def _listings(env, owner, repo, branch, path, base_ref,
                    allow_missing=False):
    root = await _tree(env, owner, repo, "", branch)
    create = False
    if root is None:
        if not base_ref or base_ref == branch:
            raise FileEditError(
                "branch_not_found", "That branch is not available.", 404)
        root = await _tree(env, owner, repo, "", base_ref)
        create = True
    if root is None:
        raise FileEditError(
            "base_not_found", "The base branch is not available.", 404)
    commit = str(root.get("commit") or "")
    if not OID_RE.match(commit):
        raise FileEditError(
            "unsupported", "Only SHA-1 repositories can be edited here.", 409)
    listings = {"": root["entries"]}
    parts = path.split("/")
    for depth in range(1, len(parts)):
        directory = "/".join(parts[:depth])
        parent_directory = "/".join(parts[:depth - 1])
        if allow_missing and not any(
                entry.get("name") == parts[depth - 1]
                for entry in listings.get(parent_directory) or []):
            listings[directory] = []
            continue
        listing = await _tree(env, owner, repo, directory, commit)
        if listing is None:
            raise FileEditError(
                "not_found", "The file is no longer in the repository.", 404)
        listings[directory] = listing["entries"]
    return commit, listings, create


def _current(listings, path):
    directory, _, name = path.rpartition("/")
    for entry in listings.get(directory) or []:
        if entry.get("name") == name:
            return entry
    return None


def _tree_sort_key(row):
    mode, name, _oid = row
    suffix = "/" if mode.lstrip("0") == "40000" else ""
    return (name + suffix).encode("utf-8")


def rebuilt_trees(listings, path, blob_oid, create=False):
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
                raise FileEditError(
                    "gateway_outdated",
                    "The repository host does not report object ids yet.",
                    503)
            if entry_name == name:
                found = True
                oid = child
            rows.append((mode, entry_name, oid))
        if not found and not create:
            raise FileEditError(
                "not_found", "The file is no longer in the repository.", 404)
        if not found:
            mode = "100644" if depth == len(parts) - 1 else "40000"
            rows.append((mode, name, child))
            rows.sort(key=_tree_sort_key)
        payload = tree_payload(rows)
        child = object_id("tree", payload)
        objects.append(("tree", payload))
    return child, objects


def build_commit(listings, parent, path, content, name, email, seconds,
                 message, create=False):
    blob = content.encode("utf-8")
    blob_oid = object_id("blob", blob)
    root, trees = rebuilt_trees(listings, path, blob_oid, create)
    commit = commit_payload(root, parent, name, email, seconds, message)
    commit_oid = object_id("commit", commit)
    objects = [("blob", blob)] + trees + [("commit", commit)]
    return commit_oid, blob_oid, pack(objects)


async def _push(env, request, owner, repo, actor, branch, body, alias_owner):
    core_held = owner.lower() in _core_push_nodes(env)
    upstream, _endpoint = await _https_mirror_push_send(
        env, request, owner, repo, actor, "POST", body, None,
        hashlib.sha256(body).hexdigest(), alias_owner=alias_owner,
        core_held=core_held, credential="web-file-edit")
    if upstream is None:
        raise FileEditError(
            "push_unavailable", "No repository host accepted the commit.", 503)
    status = int(getattr(upstream, "status", 0) or 0)
    try:
        text = str(await upstream.text())
    except Exception:
        text = ""
    if status != 200:
        raise FileEditError(
            "push_refused", "The repository host refused the commit (HTTP %d)."
            % status, 502)
    ref = "refs/heads/" + branch
    ok, reason = report_status(text, ref)
    if not ok:
        stale = any(word in reason for word in (
            "fetch first", "non-fast-forward", "stale", "lock"))
        raise FileEditError(
            "conflict" if stale else "push_refused",
            "The branch moved while saving; reload and try again." if stale
            else "The repository host refused the commit: " + reason,
            409 if stale else 502)


async def _save(env, request, owner, repo):
    try:
        data = await bounded_json_request(request, max_bytes=MAX_REQUEST_BYTES)
    except Exception:
        return _error("invalid_json", "The request body is not JSON.", 400)
    if not isinstance(data, dict):
        data = {}
    actor = await _authed_account_name(env, request, data)
    if not actor:
        return _error("unauthorized", "Sign in to edit repository files.", 401)
    access = await _repository_access_context(
        env, request, owner, repo, viewer=actor)
    owner_writer = False
    org_writer = False
    if not access:
        org_writer = await _org_write_allowed(env, owner, repo, actor)
        if not org_writer:
            return _error("not_found", "That repository is not available.", 404)
    if not org_writer:
        owner_writer = await _account_owns_node(env, actor, owner)
        if not owner_writer:
            org_writer = await _org_write_allowed(env, owner, repo, actor)
    if not owner_writer and not org_writer:
        return _error(
            "forbidden",
            "You need repository write permission to edit files.", 403)
    path = str(data.get("path") or "")
    content = data.get("content")
    new_file = data.get("create") is True
    base_oid = str(data.get("oid") or "").lower()
    branch = str(data.get("branch") or "").strip()
    base_ref = str(data.get("baseRef") or "").strip()
    if not valid_path(path):
        return _error("invalid_path", "That file path cannot be edited.", 400)
    if not valid_branch(branch):
        return _error("invalid_branch", "Choose a valid branch name.", 400)
    if base_ref and not valid_branch(base_ref) and not OID_RE.match(base_ref):
        return _error("invalid_base", "Choose a valid base branch.", 400)
    if not isinstance(content, str):
        return _error("invalid_content", "The new text is missing.", 400)
    content = content.replace("\r\n", "\n").replace("\r", "\n")
    if len(content.encode("utf-8")) > MAX_CONTENT_BYTES:
        return _error("too_large", "The file is larger than 2 MB.", 413)
    try:
        parent, listings, create = await _listings(
            env, owner, repo, branch, path, base_ref, allow_missing=new_file)
        entry = _current(listings, path)
        if new_file and entry is not None:
            raise FileEditError(
                "conflict", "That file already exists; reload to edit it.",
                409)
        if entry is None and new_file:
            entry = {"type": "blob", "mode": "100644", "oid": ZERO_OID}
        if entry is None or entry.get("type") != "blob":
            raise FileEditError(
                "not_found", "The file is no longer in the repository.", 404)
        if str(entry.get("mode") or "") not in FILE_MODES:
            raise FileEditError(
                "invalid_path", "That path is not a regular file.", 400)
        current = str(entry.get("oid") or "").lower()
        if not OID_RE.match(current):
            raise FileEditError(
                "gateway_outdated",
                "The repository host does not report object ids yet.", 503)
        if base_oid and current and base_oid != current:
            raise FileEditError(
                "conflict",
                "Someone changed this file since you opened it; reload to "
                "see their version.", 409)
        new_blob = object_id("blob", content.encode("utf-8"))
        if current == new_blob:
            return json_response({
                "ok": True, "unchanged": True, "path": path, "oid": current,
                "commit": parent, "branch": branch,
            }, cache_control="no-store")
        message = clean_message(
            data.get("message"), path, "Edit %s on the web")
        commit_oid, blob_oid, pack_bytes = build_commit(
            listings, parent, path, content, actor,
            noreply_email(actor, _public_base_url(env)),
            int(time.time()), message, create=new_file)
        old = ZERO_OID if create else parent
        body = receive_pack_body(
            old, commit_oid, "refs/heads/" + branch, pack_bytes)
        await _push(env, request, owner, repo, actor, branch, body, "")
    except FileEditError as exc:
        return _error(exc.code, exc.message, exc.status)
    try:
        await _audit_sensitive_action(
            env, actor, "repository.file_edit", "repository",
            owner + "/" + repo, "success", {
                "path": path, "branch": branch, "commit": commit_oid,
                "parent": parent, "createdBranch": create,
            })
    except Exception:
        pass
    return json_response({
        "ok": True,
        "path": path,
        "branch": branch,
        "createdBranch": create,
        "commit": commit_oid,
        "oid": blob_oid,
        "author": actor,
        "subject": message,
    }, cache_control="no-store")


async def route(env, request, owner, repo, action):
    if action != "file-edit":
        return _error("not_found", "Not found.", 404)
    if method_name(request) != "POST":
        return _error("method_not_allowed", "Use POST.", 405)
    return await _save(env, request, owner, repo)
