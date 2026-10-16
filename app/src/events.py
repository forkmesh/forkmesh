"""Signed-event crypto + canonicalization for the ForkMesh relay Worker.

The mesh's issues, discussions, pull requests, PR review comments, commit
comments and release manifests are all authenticated by a raw Ed25519 signature
over a versioned canonical string (``forkmesh-<kind>-v1\n...``). Both halves of
that spine — the WebCrypto verify/sha256 primitives and the per-event content
canonicalization that MUST byte-match the desktop client's ``*Store``
``contentForSigning`` / ``canonicalString`` — are pulled out of the ~11k-line
``entry.py`` into this one module.

Like ``solana.py``/``git_http.py``/``releases.py`` (adhoc #215/#279), the import
is strictly one-directional: this depends only on the stdlib, the Worker
runtime's ``js``/``pyodide`` bridge, ``catalog.clean_string`` and the two pure
release helpers, never on other ``entry`` state — so ``entry`` imports these back
and the AST test suite loads them straight from this file.
"""

import base64
import time

from js import Object
from js import Uint8Array
from js import crypto as js_crypto
from pyodide.ffi import to_js as _to_js

from catalog import clean_string
from releases import (
    release_manifest_content,
    release_signing_message,
    valid_git_commit,
)


ISSUE_EVENT_TYPES = frozenset({
    "open", "comment", "edit", "title", "status", "labels", "milestone",
    "dates", "priority", "progress", "bounty", "assignees", "agent",
    "reaction", "delete", "vote", "claim",
})

# A "claim" carries ONE account name and a direction, never a whole list. That
# shape is what makes self-assignment safe for a public contributor: the event
# a visitor can sign cannot drop anybody else's name the way an "assignees"
# event (which replaces the list wholesale) would, and two contributors
# claiming at once converge instead of overwriting each other.
CLAIM_STATES = frozenset({"assigned", "released"})

EVENT_SIGNATURE_V1_INTAKE_UNTIL_MS = 1798761600000


def event_signing_repo_key(owner, repo):
    owner = str(owner or "").strip().lower()
    repo = str(repo or "").strip().lower()
    if not owner or not repo:
        return ""
    for part in (owner, repo):
        if "/" in part or "\n" in part or "\x00" in part:
            return ""
    return owner + "/" + repo


def event_signing_repo_keys(*pairs):
    keys = []
    for owner, repo in pairs:
        key = event_signing_repo_key(owner, repo)
        if key and key not in keys:
            keys.append(key)
    return tuple(keys)


BRANCH_COMMENT_TYPES = ("thread-comment", "thread-reply", "thread-state")
MAX_BRANCH_NAME = 255


def valid_branch_name(branch):
    if not isinstance(branch, str) or not branch or len(branch) > MAX_BRANCH_NAME:
        return False
    if branch.startswith(("-", "/")) or branch.endswith(("/", ".lock")):
        return False
    if ".." in branch or "//" in branch or "@{" in branch:
        return False
    return not any(ch in branch for ch in "\x00\n\r\t ~^:?*[\\\x7f")


async def verify_branch_comment_event(branch, ev, repo_keys=()):
    if not valid_branch_name(branch):
        return False
    author = ev.get("author", "")
    signature = ev.get("sig", "")
    event_type = ev.get("type", "")
    if not author or not signature or event_type not in BRANCH_COMMENT_TYPES:
        return False
    try:
        ts = int(ev.get("ts", 0))
    except (TypeError, ValueError):
        return False
    content_hash = await sha256_hex(pull_comment_content(ev))
    body = (
        event_type + "\n" + branch + "\n" +
        author + "\n" + str(ts) + "\n" + content_hash
    )
    return await _verify_event_messages(
        author, signature, "branch-comment", body, repo_keys, False)


def v1_event_intake_open(now_ms=None):
    if now_ms is None:
        now_ms = int(time.time() * 1000)
    return int(now_ms) < EVENT_SIGNATURE_V1_INTAKE_UNTIL_MS


def event_signing_messages(kind, body, repo_keys=(), allow_v1=None):
    if isinstance(repo_keys, str):
        repo_keys = (repo_keys,)
    messages = []
    for key in repo_keys or ():
        if key:
            messages.append(
                ("forkmesh-" + kind + "-v2\n" + key + "\n" + body).encode())
    if allow_v1 is None:
        allow_v1 = v1_event_intake_open()
    if allow_v1:
        messages.append(("forkmesh-" + kind + "-v1\n" + body).encode())
    return messages


async def _verify_event_messages(author, signature, kind, body, repo_keys,
                                 allow_v1):
    for message in event_signing_messages(kind, body, repo_keys, allow_v1):
        if await ed25519_verify(author, signature, message):
            return True
    return False


def to_js(value):
    return _to_js(value, dict_converter=Object.fromEntries)


def _to_js_bytes(value):
    """Copy Python bytes before an asynchronous WebCrypto operation."""
    return Uint8Array.new(_to_js(bytes(value)))


def b64url_decode(value):
    value = (value or "").strip()
    value += "=" * (-len(value) % 4)
    return base64.urlsafe_b64decode(value.encode())


async def ed25519_verify(pubkey_b64url, sig_b64url, data_bytes):
    # Verify a raw Ed25519 signature using the runtime's WebCrypto, matching the
    # desktop client's identity (raw 32-byte key + 64-byte sig, base64url).
    try:
        raw_key = b64url_decode(pubkey_b64url)
        signature = b64url_decode(sig_b64url)
    except Exception:
        return False
    if len(raw_key) != 32 or len(signature) != 64:
        return False
    try:
        key = await js_crypto.subtle.importKey(
            "raw", _to_js_bytes(raw_key), to_js({"name": "Ed25519"}), False,
            _to_js(["verify"])
        )
        ok = await js_crypto.subtle.verify(
            to_js({"name": "Ed25519"}), key, _to_js_bytes(signature),
            _to_js_bytes(data_bytes)
        )
        return bool(ok)
    except Exception:
        return False


async def sha256_hex(text):
    digest = await js_crypto.subtle.digest(
        "SHA-256", _to_js_bytes(text.encode()))
    return bytes(Uint8Array.new(digest).to_py()).hex()


def issue_event_content(ev):
    # Type-specific canonical content; MUST match the client's
    # IssueStore::contentForSigning and issues/README.md. Fields joined by NUL.
    t = ev.get("type", "")
    attachments = ",".join(ev.get("attachments") or [])
    if t == "open":
        return "\x00".join([ev.get("title", ""), ev.get("body", ""), attachments])
    if t in ("comment", "edit"):
        return "\x00".join([ev.get("body", ""), attachments])
    if t == "title":
        return ev.get("title", "")
    if t == "status":
        return ev.get("status", "")
    if t == "labels":
        return ",".join(ev.get("labels") or [])
    if t == "milestone":
        return ev.get("milestone", "") or ""
    if t == "dates":
        # Gantt start/end (epoch ms, 0 = unset); must byte-match the client's
        # IssueStore/ProjectStore contentForSigning "dates" case.
        parts = []
        for key in ("startDate", "endDate"):
            try:
                parts.append(str(int(ev.get(key, 0) or 0)))
            except (TypeError, ValueError):
                parts.append("0")
        return "\x00".join(parts)
    if t == "priority":
        try:
            return str(int(ev.get("priority", 0)))
        except (TypeError, ValueError):
            return "0"
    if t == "progress":
        try:
            return str(int(ev.get("progress", 0)))
        except (TypeError, ValueError):
            return "0"
    if t == "bounty":
        try:
            amount = "%.2f" % float(ev.get("bountyUsd", 0))
        except (TypeError, ValueError):
            amount = "0.00"
        return "\x00".join([amount, ev.get("bountyAddress", "") or "",
                            ev.get("bountyStatus", "") or ""])
    if t == "assignees":
        return ",".join(ev.get("assignees") or [])
    if t == "claim":
        return "\x00".join([
            ev.get("assignee", "") or "", ev.get("claimState", "") or "",
        ])
    if t == "reporter":
        return ev.get("reporter", "") or ""
    if t == "agent":
        try:
            session_id = str(int(ev.get("agentSessionId", 0) or 0))
        except (TypeError, ValueError):
            session_id = "0"
        return "\x00".join([
            ev.get("agentProvider", "") or "",
            session_id,
            ev.get("agentStatus", "") or "",
            "pr" if ev.get("agentCreatePr") else "no-pr",
        ])
    if t == "reaction":
        # target = the event reacted to; reaction = a ReactionCatalog slug.
        return "\x00".join([ev.get("target", "") or "",
                            ev.get("reaction", "") or ""])
    if t == "delete":
        return ev.get("target", "")
    if t == "vote":
        return ""  # type+number+author+ts already bind the signed vote
    return ""


async def verify_issue_event(number, ev, repo_keys=(), allow_v1=None):
    author = ev.get("author", "")
    signature = ev.get("sig", "")
    event_type = ev.get("type", "")
    if not author or not signature or event_type not in ISSUE_EVENT_TYPES:
        return False
    try:
        event_number = int(number)
    except (TypeError, ValueError):
        return False
    if event_type == "open" and event_number != 0:
        return False
    if event_type != "open" and event_number <= 0:
        return False
    if event_type == "status" and ev.get("status") not in ("open", "closed"):
        return False
    if event_type in ("labels", "assignees"):
        values = ev.get(event_type)
        if not isinstance(values, list) or any(
            not isinstance(value, str) for value in values
        ):
            return False
    if event_type == "claim":
        assignee = ev.get("assignee")
        if not isinstance(assignee, str) or not assignee.strip():
            return False
        if ev.get("claimState") not in CLAIM_STATES:
            return False
    if event_type in ("edit", "delete", "reaction"):
        target = ev.get("target")
        if not isinstance(target, str) or not target:
            return False
    if event_type == "reaction":
        reaction = ev.get("reaction")
        if not isinstance(reaction, str) or not reaction.strip():
            return False
    if event_type == "priority":
        try:
            priority = int(ev.get("priority", 0))
        except (TypeError, ValueError):
            return False
        if priority < 0 or priority > 99:
            return False
    if event_type == "progress":
        try:
            progress = int(ev.get("progress", 0))
        except (TypeError, ValueError):
            return False
        if progress < 0 or progress > 100:
            return False
    try:
        ts = int(ev.get("ts", 0))
    except (TypeError, ValueError):
        return False
    content_hash = await sha256_hex(issue_event_content(ev))
    body = (
        event_type + "\n" + str(event_number) + "\n" +
        author + "\n" + str(ts) + "\n" + content_hash
    )
    return await _verify_event_messages(
        author, signature, "issue-event", body, repo_keys, allow_v1)


DISCUSSION_CATEGORIES = {
    "Announcements", "Ideas", "Q&A", "Show and tell", "Maintainer notes",
}
DISCUSSION_CATEGORY_MAP = {
    category.lower(): category for category in DISCUSSION_CATEGORIES
}


def normalized_discussion_category(value):
    return DISCUSSION_CATEGORY_MAP.get(clean_string(value, 80).strip().lower(), "")


def discussion_event_content(ev):
    # Mirrors DiscussionStore::contentForSigning. Fields joined by NUL.
    t = ev.get("type", "")
    if t == "open":
        return "\x00".join([
            ev.get("title", ""), ev.get("body", ""), ev.get("category", "")
        ])
    if t == "comment":
        return ev.get("body", "")
    return ""


async def verify_discussion_event(number, ev, repo_keys=(), allow_v1=None):
    # Mirrors DiscussionStore::canonicalString(number, ev): the signature binds
    # the canonical discussion number plus type-specific content. New discussions
    # submitted through the inbox are signed with number 0 because the owner
    # assigns the durable repo number when draining the inbox.
    author = ev.get("author", "")
    signature = ev.get("sig", "")
    event_type = ev.get("type", "")
    if not author or not signature or event_type not in ("open", "comment"):
        return False
    if event_type == "open" and not normalized_discussion_category(
            ev.get("category", "")):
        return False
    try:
        ts = int(ev.get("ts", 0))
    except (TypeError, ValueError):
        return False
    content_hash = await sha256_hex(discussion_event_content(ev))
    body = (
        event_type + "\n" + str(int(number)) +
        "\n" + author + "\n" + str(ts) + "\n" + content_hash
    )
    return await _verify_event_messages(
        author, signature, "discussion-event", body, repo_keys, allow_v1)


async def verify_pull_event(pr, repo_keys=(), allow_v1=None):
    # Mirrors PullStore::canonicalString: the signature commits to
    # title/base/head/patch (not the number, which the owner assigns on merge).
    # Newer clients append a 5th field — the format-patch mbox (commits) — so the
    # owner can replay authored commits on merge. Accept either form so a client
    # rollout doesn't reject not-yet-updated peers; an old client simply omits the
    # 5th field and an old peer that signed the 4-field form still verifies.
    author = pr.get("author", "")
    signature = pr.get("sig", "")
    if not author or not signature:
        return False
    try:
        ts = int(pr.get("ts", 0))
    except (TypeError, ValueError):
        return False
    fields = [pr.get("title", ""), pr.get("base", ""), pr.get("head", ""),
              pr.get("patch", "")]
    commits = pr.get("commits", "")
    # A legacy signature never authenticated a commit mbox. Only try that
    # four-field form when the submission carries no commits; otherwise an
    # attacker could append unsigned commits that Qt would replay in preference
    # to the signed flat patch.
    contents = ["\x00".join(fields + [commits])]
    if not commits:
        contents.append("\x00".join(fields))
    for content in contents:
        content_hash = await sha256_hex(content)
        body = author + "\n" + str(ts) + "\n" + content_hash
        if await _verify_event_messages(
                author, signature, "pull-event", body, repo_keys, allow_v1):
            return True
    return False


def pull_comment_anchor_fields(ev):
    """Signed patchset anchor of a line-anchored pull event, or nothing.

    Mirrors PullEvent::hasAnchor() in the desktop: a line-comment or
    thread-comment that names the patchset (head commit) it was written
    against and/or the commented text appends both to its signed content. An
    event without either signs the pre-anchor bytes exactly, so older
    signatures and older verifiers keep agreeing.
    """
    patchset = ev.get("patchset", "") or ""
    anchor_lines = ev.get("anchorLines", "") or ""
    if not patchset and not anchor_lines:
        return []
    return [str(patchset), str(anchor_lines)]


def pull_comment_content(ev):
    # Mirrors PullStore::contentForSigning. Fields joined by NUL.
    t = ev.get("type", "")
    def int_field(name):
        try:
            return str(int(ev.get(name, 0)))
        except (TypeError, ValueError):
            return "0"
    if t == "comment":
        return ev.get("body", "")
    if t == "review":
        return "\x00".join([ev.get("state", ""), ev.get("body", "")])
    if t == "line-comment":
        return "\x00".join([
            ev.get("path", ""), ev.get("side", ""), int_field("line"),
            ev.get("body", ""),
        ] + pull_comment_anchor_fields(ev))
    if t == "thread-comment":
        return "\x00".join([
            ev.get("threadId", ""), ev.get("path", ""), ev.get("side", ""),
            int_field("lineStart"), int_field("lineEnd"), ev.get("body", ""),
            ev.get("suggestionPatch", ""),
        ] + pull_comment_anchor_fields(ev))
    if t == "thread-reply":
        return "\x00".join([
            ev.get("threadId", ""), ev.get("parentId", ""),
            ev.get("body", ""),
        ])
    if t == "thread-state":
        return "\x00".join([
            ev.get("threadId", ""), ev.get("state", ""), ev.get("body", ""),
        ])
    if t == "suggestion-state":
        return "\x00".join([
            ev.get("threadId", ""), ev.get("state", ""),
            ev.get("appliedCommit", ""), ev.get("body", ""),
        ])
    if t == "status":
        return "\x00".join([ev.get("state", ""), ev.get("body", "")])
    if t == "delete":
        return ev.get("body", "")
    return ""


async def verify_pull_comment_event(number, ev, repo_keys=(), allow_v1=None):
    # Mirrors PullStore::canonicalString(number, ev): the signature binds the PR
    # number (reviewers act on the owner's mirror, which has canonical numbers).
    author = ev.get("author", "")
    signature = ev.get("sig", "")
    event_type = ev.get("type", "")
    allowed_types = (
        "comment", "review", "line-comment", "thread-comment", "thread-reply",
        "thread-state", "suggestion-state", "status", "delete",
    )
    if not author or not signature or event_type not in allowed_types:
        return False
    if event_type == "status" and ev.get("state") != "closed":
        return False
    try:
        ts = int(ev.get("ts", 0))
    except (TypeError, ValueError):
        return False
    content_hash = await sha256_hex(pull_comment_content(ev))
    body = (
        event_type + "\n" + str(int(number)) + "\n" +
        author + "\n" + str(ts) + "\n" + content_hash
    )
    return await _verify_event_messages(
        author, signature, "pull-comment", body, repo_keys, allow_v1)


# ---------------------------------------------------------------------------
# Release publishing (issue #304). See docs/design/release-binary-publishing.md.
#
# Release METADATA is small, signed, and git-committed (synced across the mesh
# like issues/PRs); release PAYLOADS (the binary bytes) live in a per-node
# content-addressed blob store that is gitignored and NEVER committed. The pure
# integrity-spine helpers shared by both sides — the canonical signable manifest,
# the CAS path layout, sha256sum-compatible checksum generation, semver `latest`
# resolution, and the same-name re-upload decision — live in releases.py. Only
# the async signature check lives here, since it reaches into this module's
# Ed25519/sha256 crypto.
# ---------------------------------------------------------------------------
async def verify_release_manifest(manifest):
    # Mirrors the client publisher: sign sha256(release_manifest_content) with the
    # creator's Ed25519 identity. One signature thus authenticates every asset's
    # bytes (each blob_sha256 is inside the signed content).
    author = manifest.get("created_by", "") or ""
    signature = manifest.get("sig", "") or ""
    repo = manifest.get("repo", "") or ""
    tag = manifest.get("tag", "") or ""
    if not author or not signature or not repo or not tag:
        return False
    build_commit = manifest.get("build_commit", "") or ""
    if build_commit and not valid_git_commit(build_commit):
        return False
    try:
        ts = int(manifest.get("published_at", 0))
    except (TypeError, ValueError):
        return False
    content_hash = await sha256_hex(release_manifest_content(manifest))
    canonical = release_signing_message(repo, tag, author, ts, content_hash)
    return await ed25519_verify(author, signature, canonical)
