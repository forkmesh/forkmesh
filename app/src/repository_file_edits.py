import difflib
import hashlib
import re
import time
from email.header import Header
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
LINE_RE = re.compile(r"[^\n]*\n|[^\n]+\Z")
QUOTE_RE = re.compile(r'[\x00-\x1f"\\\x7f]')
QUOTE_ESCAPES = {"\t": "\\t", "\n": "\\n", '"': '\\"', "\\": "\\\\"}
MODES = frozenset({"", "commit", "probe", "propose"})
# edit (and create) one file's text, delete a file, or rename/move one.
OPS = frozenset({"edit", "delete", "rename"})
CONTEXT_LINES = 3


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


def _error(code, message, status, **extra):
    body = {"ok": False, "error": code, "message": message}
    body.update(extra)
    return json_response(body, status=status, cache_control="no-store")


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


def _entry_rows(entries):
    rows = {}
    for entry in entries or []:
        mode = str(entry.get("mode") or "")
        oid = str(entry.get("oid") or "").lower()
        name = str(entry.get("name") or "")
        if not mode or not OID_RE.match(oid) or not name:
            raise FileEditError(
                "gateway_outdated",
                "The repository host does not report object ids yet.", 503)
        rows[name] = (mode, oid)
    return rows


def apply_tree_changes(listings, changes):
    """Rebuild every tree above ``changes`` ({path: (mode, oid) or None}).

    None removes the entry; a folder left empty is removed from its parent,
    as Git stores no empty trees. ``listings`` must hold each touched folder
    (a folder that does not exist yet is an empty list). Returns the new root
    tree id and the tree objects to send.
    """
    directories = {""}
    for path in changes:
        parts = path.split("/")
        for depth in range(1, len(parts)):
            directories.add("/".join(parts[:depth]))
    results = {}
    objects = []
    for directory in sorted(
            directories, key=lambda d: -(d.count("/") + 1 if d else 0)):
        rows = _entry_rows(listings.get(directory))
        for path, value in changes.items():
            parent, _, name = path.rpartition("/")
            if parent != directory:
                continue
            if value is None:
                rows.pop(name, None)
            else:
                rows[name] = value
        for child, oid in results.items():
            parent, _, name = child.rpartition("/")
            if parent != directory:
                continue
            if oid is None:
                rows.pop(name, None)
            else:
                rows[name] = ("40000", oid)
        if not rows and directory:
            results[directory] = None
            continue
        payload = tree_payload(sorted(
            ((mode, name, oid) for name, (mode, oid) in rows.items()),
            key=_tree_sort_key))
        results[directory] = object_id("tree", payload)
        objects.append(("tree", payload))
    return results[""], objects


def deleted_patch(path, old_text, old_oid, mode):
    lines = _lines(old_text)
    out = [
        "diff --git %s %s\n" % (_side("a/", path), _side("b/", path)),
        "deleted file mode %s\n" % mode,
        "index %s..%s\n" % (old_oid, ZERO_OID),
        "--- %s\n" % _side("a/", path),
        "+++ /dev/null\n",
    ]
    if lines:
        out.append("@@ -%s +0,0 @@\n" % _range(0, len(lines)))
        out.extend(_patch_line("-", line) for line in lines)
    return "".join(out)


def _lines(text):
    return LINE_RE.findall(text)


def _range(start, stop):
    length = stop - start
    beginning = start + 1
    if length == 1:
        return str(beginning)
    if length == 0:
        beginning -= 1
    return "%d,%d" % (beginning, length)


def _patch_line(prefix, line):
    if line.endswith("\n"):
        return prefix + line
    return prefix + line + "\n\\ No newline at end of file\n"


def _quote_char(char):
    if char in QUOTE_ESCAPES:
        return QUOTE_ESCAPES[char]
    if ord(char) < 32 or ord(char) == 127:
        return "\\%03o" % ord(char)
    return char


def _side(prefix, path):
    name = prefix + path
    if not QUOTE_RE.search(name):
        return name
    return '"%s"' % "".join(_quote_char(char) for char in name)


def file_patch(path, old_text, new_text, old_oid, new_oid, mode,
               created=False):
    old_lines = _lines(old_text)
    new_lines = _lines(new_text)
    out = ["diff --git %s %s\n" % (_side("a/", path), _side("b/", path))]
    if created:
        out.append("new file mode %s\n" % mode)
        out.append("index %s..%s\n" % (ZERO_OID, new_oid))
        out.append("--- /dev/null\n")
    else:
        out.append("index %s..%s %s\n" % (old_oid, new_oid, mode))
        out.append("--- %s\n" % _side("a/", path))
    out.append("+++ %s\n" % _side("b/", path))
    matcher = difflib.SequenceMatcher(
        None, old_lines, new_lines, autojunk=False)
    for group in matcher.get_grouped_opcodes(CONTEXT_LINES):
        first, last = group[0], group[-1]
        out.append("@@ -%s +%s @@\n" % (
            _range(first[1], last[2]), _range(first[3], last[4])))
        for tag, i1, i2, j1, j2 in group:
            if tag == "equal":
                out.extend(_patch_line(" ", line) for line in old_lines[i1:i2])
                continue
            if tag in ("replace", "delete"):
                out.extend(_patch_line("-", line) for line in old_lines[i1:i2])
            if tag in ("replace", "insert"):
                out.extend(_patch_line("+", line) for line in new_lines[j1:j2])
    return "".join(out)


def patch_mbox(commit_oid, name, email, seconds, subject, patch):
    header = "[PATCH] " + subject
    if not header.isascii():
        header = Header(header, "utf-8").encode()
    date = time.strftime(
        "%a, %d %b %Y %H:%M:%S +0000", time.gmtime(int(seconds)))
    return (
        "From %s Mon Sep 17 00:00:00 2001\n"
        "From: %s <%s>\n"
        "Date: %s\n"
        "Subject: %s\n"
        "MIME-Version: 1.0\n"
        "Content-Type: text/plain; charset=UTF-8\n"
        "Content-Transfer-Encoding: 8bit\n"
        "\n"
        "---\n"
        "\n"
        "%s"
        "-- \n"
        "ForkMesh\n"
        "\n"
    ) % (commit_oid, name, email, date, header, patch)


def proposal_branch(actor, path, commit_oid):
    name = path.rpartition("/")[2].lower()
    slug = re.sub(r"[^a-z0-9]+", "-", name).strip("-")[:40].strip("-")
    branch = "%s/edit-%s-%s" % (actor, slug or "file", commit_oid[:7])
    return branch if valid_branch(branch) else "web-edit/" + commit_oid[:12]


async def _push(env, request, owner, repo, actor, branch, body, alias_owner,
                ref=None):
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
    ref = ref or "refs/heads/" + branch
    ok, reason = report_status(text, ref)
    if not ok:
        stale = any(word in reason for word in (
            "fetch first", "non-fast-forward", "stale", "lock"))
        raise FileEditError(
            "conflict" if stale else "push_refused",
            "The branch moved while saving; reload and try again." if stale
            else "The repository host refused the commit: " + reason,
            409 if stale else 502)


async def _permission(env, request, owner, repo, actor):
    access = await _repository_access_context(
        env, request, owner, repo, viewer=actor)
    if not access:
        if await _org_write_allowed(env, owner, repo, actor):
            return "write"
        return "hidden"
    if await _account_owns_node(env, actor, owner):
        return "write"
    if await _org_write_allowed(env, owner, repo, actor):
        return "write"
    return "read"


async def _old_text(env, owner, repo, path, parent, current, new_file):
    if new_file:
        return ""
    data = await _blob(env, owner, repo, path, parent)
    if (not data or data.get("truncated")
            or str(data.get("encoding") or "utf8") != "utf8"
            or not isinstance(data.get("content"), str)):
        raise FileEditError(
            "base_unavailable",
            "The current file could not be read to build the proposal.", 503)
    text = data["content"]
    if object_id("blob", text.encode("utf-8")) != current:
        raise FileEditError(
            "base_unavailable",
            "The current file could not be read to build the proposal.", 503)
    return text


async def _save(env, request, owner, repo):
    try:
        data = await bounded_json_request(request, max_bytes=MAX_REQUEST_BYTES)
    except Exception:
        return _error("invalid_json", "The request body is not JSON.", 400)
    if not isinstance(data, dict):
        data = {}
    mode = str(data.get("mode") or "").strip().lower()
    if mode not in MODES:
        return _error("invalid_mode", "Unknown file edit mode.", 400)
    actor = await _authed_account_name(env, request, data)
    if not actor:
        return _error("unauthorized", "Sign in to edit repository files.", 401)
    permission = await _permission(env, request, owner, repo, actor)
    if permission == "hidden":
        return _error("not_found", "That repository is not available.", 404)
    writer = permission == "write"
    if mode == "probe":
        return json_response({
            "ok": True, "access": "write" if writer else "propose",
            "account": actor,
        }, cache_control="no-store")
    propose = mode == "propose"
    if not writer and not propose:
        return _error(
            "forbidden",
            "You need repository write permission to edit files.", 403,
            canPropose=True)
    op = str(data.get("op") or "edit").strip().lower()
    if op not in OPS:
        return _error("invalid_op", "Unknown file operation.", 400)
    if op != "edit":
        return await _save_move(
            env, request, owner, repo, data, actor, propose, op)
    path = str(data.get("path") or "")
    content = data.get("content")
    new_file = data.get("create") is True
    base_oid = str(data.get("oid") or "").lower()
    branch = str(data.get("branch") or "").strip()
    base_ref = "" if propose else str(data.get("baseRef") or "").strip()
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
        file_mode = str(entry.get("mode") or "")
        if file_mode not in FILE_MODES:
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
        email = noreply_email(actor, _public_base_url(env))
        seconds = int(time.time())
        commit_oid, blob_oid, pack_bytes = build_commit(
            listings, parent, path, content, actor, email, seconds, message,
            create=new_file)
        if propose:
            old_text = await _old_text(
                env, owner, repo, path, parent, current, new_file)
            patch = file_patch(
                path, old_text, content, current, blob_oid, file_mode,
                created=new_file)
            commits = patch_mbox(
                commit_oid, actor, email, seconds, message, patch)
            head = proposal_branch(actor, path, commit_oid)
        else:
            old = ZERO_OID if create else parent
            body = receive_pack_body(
                old, commit_oid, "refs/heads/" + branch, pack_bytes)
            await _push(env, request, owner, repo, actor, branch, body, "")
    except FileEditError as exc:
        return _error(exc.code, exc.message, exc.status)
    if propose:
        try:
            await _audit_sensitive_action(
                env, actor, "repository.file_edit_proposal", "repository",
                owner + "/" + repo, "success", {
                    "path": path, "base": branch, "head": head,
                    "commit": commit_oid, "parent": parent,
                })
        except Exception:
            pass
        return json_response({
            "ok": True,
            "proposal": True,
            "path": path,
            "base": branch,
            "baseOid": parent,
            "head": head,
            "headOid": commit_oid,
            "oid": blob_oid,
            "author": actor,
            "subject": message,
            "patch": patch,
            "commits": commits,
        }, cache_control="no-store")
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


async def commit_paths(env, request, owner, repo, actor, branch, files,
                       message, listings=None, parent=None):
    """Commit whole-file writes ({path: bytes}) and removals ({path: None})
    to an existing branch and push it; returns the new commit id.

    Used by the routes that keep structured records in the repository
    (releases, projects). ``listings``/``parent`` may come from a caller that
    already read the trees; otherwise every folder on the way is read."""
    if listings is None:
        listings, parent = {}, None
        for path in files:
            commit, found, _create = await _listings(
                env, owner, repo, branch, path, "", allow_missing=True)
            if parent is not None and commit != parent:
                raise FileEditError(
                    "conflict", "The branch moved; reload and try again.", 409)
            parent = commit
            for directory, entries in found.items():
                listings.setdefault(directory, entries)
    objects = []
    changes = {}
    for path, blob in files.items():
        if blob is None:
            if _current(listings, path) is None:
                raise FileEditError(
                    "not_found", "The file is no longer in the repository.",
                    404)
            changes[path] = None
            continue
        oid = object_id("blob", blob)
        objects.append(("blob", blob))
        changes[path] = ("100644", oid)
    root, trees = apply_tree_changes(listings, changes)
    email = noreply_email(actor, _public_base_url(env))
    commit = commit_payload(root, parent, actor, email, int(time.time()),
                            message)
    commit_oid = object_id("commit", commit)
    body = receive_pack_body(parent, commit_oid, "refs/heads/" + branch,
                             pack(objects + trees + [("commit", commit)]))
    await _push(env, request, owner, repo, actor, branch, body, "")
    return commit_oid


async def _save_move(env, request, owner, repo, data, actor, propose, op):
    """Delete or rename one file: a commit (writers) or a proposal (anyone).

    A rename may also change the text (``content``); without it the blob is
    kept as is. A proposal is a deletion patch, plus a creation patch for a
    rename, so ``git am`` replays it without rename detection.
    """
    path = str(data.get("path") or "")
    new_path = str(data.get("newPath") or "") if op == "rename" else ""
    content = data.get("content")
    base_oid = str(data.get("oid") or "").lower()
    branch = str(data.get("branch") or "").strip()
    base_ref = "" if propose else str(data.get("baseRef") or "").strip()
    if not valid_path(path) or (op == "rename" and not valid_path(new_path)):
        return _error("invalid_path", "That file path cannot be used.", 400)
    if op == "rename" and new_path == path:
        return _error("invalid_path", "Choose a different name.", 400)
    if not valid_branch(branch):
        return _error("invalid_branch", "Choose a valid branch name.", 400)
    if base_ref and not valid_branch(base_ref) and not OID_RE.match(base_ref):
        return _error("invalid_base", "Choose a valid base branch.", 400)
    if content is not None and (op != "rename" or not isinstance(content, str)):
        return _error("invalid_content", "Only a rename can carry new text.", 400)
    if isinstance(content, str):
        content = content.replace("\r\n", "\n").replace("\r", "\n")
        if len(content.encode("utf-8")) > MAX_CONTENT_BYTES:
            return _error("too_large", "The file is larger than 2 MB.", 413)
    try:
        parent, listings, create = await _listings(
            env, owner, repo, branch, path, base_ref)
        if op == "rename":
            moved_parent, moved, _ = await _listings(
                env, owner, repo, branch, new_path, base_ref,
                allow_missing=True)
            if moved_parent != parent:
                raise FileEditError(
                    "conflict", "The branch moved; reload and try again.", 409)
            for directory, entries in moved.items():
                listings.setdefault(directory, entries)
        entry = _current(listings, path)
        if entry is None or entry.get("type") != "blob":
            raise FileEditError(
                "not_found", "The file is no longer in the repository.", 404)
        file_mode = str(entry.get("mode") or "")
        if file_mode not in FILE_MODES:
            raise FileEditError(
                "invalid_path", "That path is not a regular file.", 400)
        current = str(entry.get("oid") or "").lower()
        if not OID_RE.match(current):
            raise FileEditError(
                "gateway_outdated",
                "The repository host does not report object ids yet.", 503)
        if base_oid and base_oid != current:
            raise FileEditError(
                "conflict",
                "Someone changed this file since you opened it; reload to "
                "see their version.", 409)
        if op == "rename" and _current(listings, new_path) is not None:
            raise FileEditError(
                "conflict", "A file or folder already has that name.", 409)
        objects = []
        changes = {path: None}
        new_blob = current
        if op == "rename":
            if isinstance(content, str):
                blob = content.encode("utf-8")
                new_blob = object_id("blob", blob)
                objects.append(("blob", blob))
            changes[new_path] = (file_mode, new_blob)
        root, trees = apply_tree_changes(listings, changes)
        message = clean_message(
            data.get("message"), path,
            "Delete %s on the web" if op == "delete"
            else "Rename %s on the web")
        email = noreply_email(actor, _public_base_url(env))
        seconds = int(time.time())
        commit = commit_payload(root, parent, actor, email, seconds, message)
        commit_oid = object_id("commit", commit)
        objects = objects + trees + [("commit", commit)]
        if propose:
            old_text = await _old_text(
                env, owner, repo, path, parent, current, False)
            patch = deleted_patch(path, old_text, current, file_mode)
            if op == "rename":
                patch += file_patch(
                    new_path, "",
                    content if isinstance(content, str) else old_text,
                    ZERO_OID, new_blob, file_mode, created=True)
            commits = patch_mbox(
                commit_oid, actor, email, seconds, message, patch)
            head = proposal_branch(actor, path, commit_oid)
        else:
            old = ZERO_OID if create else parent
            body = receive_pack_body(
                old, commit_oid, "refs/heads/" + branch, pack(objects))
            await _push(env, request, owner, repo, actor, branch, body, "")
    except FileEditError as exc:
        return _error(exc.code, exc.message, exc.status)
    action = "repository.file_" + op + ("_proposal" if propose else "")
    try:
        await _audit_sensitive_action(
            env, actor, action, "repository", owner + "/" + repo, "success", {
                "path": path, "newPath": new_path, "branch": branch,
                "commit": commit_oid, "parent": parent,
            })
    except Exception:
        pass
    if propose:
        return json_response({
            "ok": True, "proposal": True, "op": op, "path": path,
            "newPath": new_path, "base": branch, "baseOid": parent,
            "head": head, "headOid": commit_oid, "author": actor,
            "subject": message, "patch": patch, "commits": commits,
        }, cache_control="no-store")
    return json_response({
        "ok": True, "op": op, "path": path, "newPath": new_path,
        "branch": branch, "createdBranch": create, "commit": commit_oid,
        "oid": new_blob if op == "rename" else "", "author": actor,
        "subject": message,
    }, cache_control="no-store")


async def route(env, request, owner, repo, action):
    if action != "file-edit":
        return _error("not_found", "Not found.", 404)
    if method_name(request) != "POST":
        return _error("method_not_allowed", "Use POST.", 405)
    return await _save(env, request, owner, repo)
