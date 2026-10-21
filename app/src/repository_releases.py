"""Releases and tags from the browser: POST /api/repo/<owner>/<repo>/release-edit.

A web release is a Git tag plus release notes kept in the repository, in the
same place every client already reads releases from:
``.forkmesh/releases/<tag>/release.json`` on the default branch. The
operations, all for accounts with write access (the /file-edit permission):

* ``tag``            create a tag at a branch or commit (tags never move or
                     disappear here: mirrors refuse both without an owner's
                     tag-move grant, which keeps signed releases trustworthy);
* ``save``           create or edit a release: creates the tag when it does
                     not exist yet, then commits release.json (title, notes,
                     pre-release flag, the tag's commit);
* ``delete``         remove a release's notes (the tag stays).

Signed release channels (a folder holding ``release.json.sig``, and the
``latest`` / ``candidate-*`` channels the installers read) are never written
from here: editing them would break their signature.
"""

import base64
import binascii
import hashlib
import json
import re
import time

import repository_file_edits as edits
from docs_book import (OID_RE, commit_payload, object_id, pack,
                       receive_pack_body)
from hosted_repos import noreply_email
from releases import valid_asset_name, valid_release_tag

MAX_TITLE = 200
MAX_NOTES_BYTES = 128 * 1024
RELEASES_DIR = ".forkmesh/releases"
RESERVED_CHANNEL_RE = re.compile(r"^(latest|candidate(-.*)?)$", re.I)
OPS = frozenset({"tag", "save", "delete", "asset", "asset-delete"})
MAX_ASSET_BYTES = edits.MAX_CONTENT_BYTES
MAX_ASSETS = 20
CONTENT_TYPE_RE = re.compile(r"^[a-z0-9][a-z0-9.+-]*/[a-z0-9][a-z0-9.+-]*$")


def _bind_runtime(runtime):
    edits._bind_runtime(runtime)
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def _error(code, message, status, **extra):
    return edits._error(code, message, status, **extra)


def release_record(tag, title, notes, prerelease, tag_commit, author,
                   created_at, previous=None):
    """The release.json a web release writes (keys the release list reads)."""
    record = dict(previous or {})
    record.update({
        "tag": tag,
        "name": title or tag,
        "body": notes,
        "prerelease": bool(prerelease),
        "tag_commit": tag_commit,
        "assets": list(record.get("assets") or []),
        "created_at": record.get("created_at") or created_at,
        "author": record.get("author") or author,
        "source": "web",
    })
    if previous:
        record["updated_at"] = created_at
    return record


async def _resolve(env, owner, repo, ref):
    tree = await edits._tree(env, owner, repo, "", ref)
    commit = str((tree or {}).get("commit") or "").lower()
    return commit if OID_RE.match(commit) else ""


async def _create_tag(env, request, owner, repo, actor, tag, commit):
    body = receive_pack_body(edits.ZERO_OID, commit, "refs/tags/" + tag,
                             pack([]))
    try:
        await edits._push(env, request, owner, repo, actor,
                          "", body, "", ref="refs/tags/" + tag)
    except edits.FileEditError as exc:
        if exc.code == "conflict":
            raise edits.FileEditError(
                "tag_exists", "That tag already exists.", 409) from exc
        raise


async def _commit_release_file(env, request, owner, repo, actor, branch,
                               tag, record, message):
    """Write (record) or remove (None) .forkmesh/releases/<tag>/release.json."""
    path = "%s/%s/release.json" % (RELEASES_DIR, tag)
    parent, listings, _create = await edits._listings(
        env, owner, repo, branch, path, "", allow_missing=True)
    folder = edits._current(listings, "%s/%s" % (RELEASES_DIR, tag))
    if folder is not None:
        entries = listings.get("%s/%s" % (RELEASES_DIR, tag))
        if entries is None:
            listing = await edits._tree(
                env, owner, repo, "%s/%s" % (RELEASES_DIR, tag), parent)
            entries = (listing or {}).get("entries") or []
            listings["%s/%s" % (RELEASES_DIR, tag)] = entries
        if any(str(e.get("name") or "") == "release.json.sig" for e in entries):
            raise edits.FileEditError(
                "signed_release",
                "This release is signed by its publisher; it cannot be "
                "changed from the web.", 409)
    current = edits._current(listings, path)
    objects = []
    if record is None:
        if current is None:
            raise edits.FileEditError(
                "not_found", "That release has no notes to delete.", 404)
        changes = {path: None}
    else:
        blob = (json.dumps(record, indent=2, sort_keys=True) + "\n").encode(
            "utf-8")
        blob_oid = object_id("blob", blob)
        objects.append(("blob", blob))
        changes = {path: ("100644", blob_oid)}
    root, trees = edits.apply_tree_changes(listings, changes)
    email = noreply_email(actor, _public_base_url(env))
    commit = commit_payload(root, parent, actor, email, int(time.time()),
                            message)
    commit_oid = object_id("commit", commit)
    body = receive_pack_body(parent, commit_oid, "refs/heads/" + branch,
                             pack(objects + trees + [("commit", commit)]))
    await edits._push(env, request, owner, repo, actor, branch, body, "")
    return commit_oid


async def _signed_or_missing(env, owner, repo, branch, tag):
    """Refuse a publisher-signed release folder; return its listings."""
    folder = "%s/%s" % (RELEASES_DIR, tag)
    parent, listings, _create = await edits._listings(
        env, owner, repo, branch, folder + "/release.json", "",
        allow_missing=True)
    entries = listings.get(folder) or []
    if any(str(e.get("name") or "") == "release.json.sig" for e in entries):
        raise edits.FileEditError(
            "signed_release",
            "This release is signed by its publisher; it cannot be changed "
            "from the web.", 409)
    return parent, listings


async def _save_asset(env, request, owner, repo, actor, branch, tag, data,
                      remove):
    """Attach (or remove) a file to a web release.

    The bytes are committed next to the notes, at
    .forkmesh/releases/<tag>/assets/<name> (up to 2 MB each), and listed in
    release.json with their size, type, SHA-256 and repository path, so any
    mirror serving the repository serves the download too.
    """
    name = str(data.get("name") or "").strip()
    if not valid_asset_name(name) or not edits.valid_path(name):
        raise edits.FileEditError("invalid_asset", "Choose a valid file name.",
                                  400)
    record = await _previous_record(env, owner, repo, branch, tag)
    if not record or record.get("source") != "web":
        raise edits.FileEditError(
            "not_found", "Publish the release on the web first.", 404)
    asset_path = "%s/%s/assets/%s" % (RELEASES_DIR, tag, name)
    assets = [a for a in record.get("assets") or []
              if isinstance(a, dict) and a.get("name") != name]
    files = {}
    if remove:
        if len(assets) == len(record.get("assets") or []):
            raise edits.FileEditError("not_found", "No such asset.", 404)
        files[asset_path] = None
    else:
        try:
            raw = base64.b64decode(str(data.get("content") or ""),
                                   validate=True)
        except (binascii.Error, ValueError):
            raise edits.FileEditError("invalid_content",
                                      "The file is not valid base64.", 400)
        if not raw or len(raw) > MAX_ASSET_BYTES:
            raise edits.FileEditError(
                "too_large", "Release files up to 2 MB can be added here.",
                413)
        if len(assets) >= MAX_ASSETS:
            raise edits.FileEditError(
                "too_many_assets", "A release can carry 20 files here.", 409)
        content_type = str(data.get("contentType") or "").strip().lower()
        if not CONTENT_TYPE_RE.match(content_type):
            content_type = "application/octet-stream"
        assets.append({
            "name": name, "size": len(raw), "content_type": content_type,
            "sha256": hashlib.sha256(raw).hexdigest(), "path": asset_path,
            "source": "web",
        })
        files[asset_path] = raw
    record["assets"] = sorted(assets, key=lambda a: a.get("name", ""))
    record["updated_at"] = int(time.time() * 1000)
    files["%s/%s/release.json" % (RELEASES_DIR, tag)] = (
        json.dumps(record, indent=2, sort_keys=True) + "\n").encode("utf-8")
    await _signed_or_missing(env, owner, repo, branch, tag)
    commit = await edits.commit_paths(
        env, request, owner, repo, actor, branch, files,
        ("Remove %s from the %s release" if remove
         else "Add %s to the %s release") % (name, tag))
    return commit, record


async def _previous_record(env, owner, repo, branch, tag):
    data = await edits._blob(
        env, owner, repo, "%s/%s/release.json" % (RELEASES_DIR, tag), branch)
    try:
        record = json.loads(str((data or {}).get("content") or ""))
    except ValueError:
        return None
    return record if isinstance(record, dict) else None


async def _save(env, request, owner, repo):
    try:
        data = await bounded_json_request(
            request, max_bytes=edits.MAX_REQUEST_BYTES)
    except Exception:
        return _error("invalid_json", "The request body is not JSON.", 400)
    if not isinstance(data, dict):
        data = {}
    op = str(data.get("op") or "").strip().lower()
    if op not in OPS:
        return _error("invalid_op", "Unknown release operation.", 400)
    actor = await _authed_account_name(env, request, data)
    if not actor:
        return _error("unauthorized", "Sign in to manage releases.", 401)
    permission = await edits._permission(env, request, owner, repo, actor)
    if permission == "hidden":
        return _error("not_found", "That repository is not available.", 404)
    if permission != "write":
        return _error("forbidden",
                      "You need write access to manage releases.", 403)
    tag = str(data.get("tag") or "").strip()
    if not valid_release_tag(tag) or not edits.valid_branch(tag):
        return _error("invalid_tag", "Choose a valid tag name.", 400)
    if RESERVED_CHANNEL_RE.match(tag):
        return _error("reserved_tag",
                      "That name is reserved for signed release channels.",
                      400)
    branch = str(data.get("branch") or "").strip()
    if not edits.valid_branch(branch):
        return _error("invalid_branch", "Choose a valid branch name.", 400)
    title = " ".join(str(data.get("title") or "").split())[:MAX_TITLE]
    notes = str(data.get("notes") or "").replace("\r\n", "\n")
    if len(notes.encode("utf-8")) > MAX_NOTES_BYTES:
        return _error("too_large", "Release notes are limited to 128 KB.",
                      413)
    target = str(data.get("target") or branch).strip()
    if not (edits.valid_branch(target) or OID_RE.match(target.lower())):
        return _error("invalid_target", "Choose a branch or commit.", 400)
    result = {"ok": True, "op": op, "tag": tag}
    try:
        if op in ("asset", "asset-delete"):
            result["commit"], result["release"] = await _save_asset(
                env, request, owner, repo, actor, branch, tag, data,
                op == "asset-delete")
        elif op == "delete":
            previous = await _previous_record(env, owner, repo, branch, tag)
            asset_paths = [
                a.get("path") for a in (previous or {}).get("assets") or []
                if isinstance(a, dict) and a.get("source") == "web"
                and str(a.get("path") or "").startswith(
                    "%s/%s/assets/" % (RELEASES_DIR, tag))]
            if asset_paths:
                await _signed_or_missing(env, owner, repo, branch, tag)
                files = {"%s/%s/release.json" % (RELEASES_DIR, tag): None}
                files.update({path: None for path in asset_paths})
                result["commit"] = await edits.commit_paths(
                    env, request, owner, repo, actor, branch, files,
                    "Delete the %s release" % tag)
            else:
                result["commit"] = await _commit_release_file(
                    env, request, owner, repo, actor, branch, tag, None,
                    "Delete the %s release notes" % tag)
        else:
            existing = await _resolve(env, owner, repo, "refs/tags/" + tag)
            if existing:
                tag_commit = existing
                if op == "tag":
                    raise edits.FileEditError(
                        "tag_exists", "That tag already exists.", 409)
            else:
                tag_commit = await _resolve(env, owner, repo, target)
                if not tag_commit:
                    raise edits.FileEditError(
                        "target_not_found",
                        "That branch or commit is not available.", 404)
                await _create_tag(env, request, owner, repo, actor, tag,
                                  tag_commit)
                result["createdTag"] = True
            result["tagCommit"] = tag_commit
            if op == "save":
                previous = await _previous_record(
                    env, owner, repo, branch, tag)
                now = int(time.time() * 1000)
                record = release_record(
                    tag, title, notes, data.get("prerelease") is True,
                    tag_commit, actor, now, previous)
                result["commit"] = await _commit_release_file(
                    env, request, owner, repo, actor, branch, tag, record,
                    ("Update the %s release" if previous
                     else "Release %s") % tag)
                result["release"] = record
    except edits.FileEditError as exc:
        return _error(exc.code, exc.message, exc.status)
    try:
        await _audit_sensitive_action(
            env, actor, "repository.release_" + op, "repository",
            owner + "/" + repo, "success", {"tag": tag, "branch": branch})
    except Exception:
        pass
    if op == "save" and result.get("createdTag"):
        notify = globals().get("notify_release_published")
        if callable(notify):
            try:
                await notify(env, owner, repo, dict(
                    result["release"], created_by=actor,
                    published_at=int(time.time() * 1000)))
            except Exception:
                pass
    return json_response(result, cache_control="no-store")


async def route(env, request, owner, repo, action):
    if action != "release-edit":
        return _error("not_found", "Not found.", 404)
    if method_name(request) != "POST":
        return _error("method_not_allowed", "Use POST.", 405)
    return await _save(env, request, owner, repo)
