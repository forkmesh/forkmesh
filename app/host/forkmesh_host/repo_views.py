"""The reads a pull request, issue or discussion page needs, in one answer.

The dashboard reads a repository's pull requests, issues and discussions
from ``.forkmesh`` files through the repository read API, one request after
another: the pull ledger's named ref, then its tree pinned to the commit that
names, then batched ``pull.md`` blobs, then one blob per pull whose own
``forkmesh/pull/<n>`` branch is ahead of the ledger (118 of them for
forkmesh/forkmesh, four at a time), which put the pull list at ~2 s. A pull's
detail is six sequential reads, an issue's three or four.

On a node that reads the repository locally (repo_fastpath) this module makes
the same reads against the node's gateway, in parallel and mostly from host
memory (commit-pinned answers are immutable), and returns them as one bundle
keyed exactly like the dashboard's own requests (``read_key``). The dashboard
takes a request from the bundle instead of the network on an exact match
(06-repo-content.js ``seededRepoRead``); anything not in it is read as
before. The same data renders the first view into the page (repo_ssr).

* ``GET /api/repo/<o>/<r>/page-reads?view=<view>[&number=<n>][&v=<hash>]``,
  views: pulls, pull, issues, issue, discussions, discussion. ``v`` names
  one bundle by content hash; such an answer is immutable. Without it (or
  when that bundle is gone) the current one is sent with ``no-cache``.
* Only public repositories the local gateway serves take part; everything
  else answers 404 and the page reads as before.
* Reads at a named ref (``main``, ``forkmesh/pulls``) are made on every
  bundle build; the dashboard uses them once and only within a minute.
  Everything pinned to a commit comes from the fast path's ImmutableCache.
"""

from __future__ import annotations

import asyncio
import hashlib
import json
import re
import time
import urllib.parse

from .repo_fastpath import (FULL_COMMIT_RE, PRIVATE_VIEW_FIELDS,
                            gateway_query)

VIEWS = ("pulls", "pull", "issues", "issue", "discussions", "discussion")
NUMBER_VIEWS = ("pull", "issue", "discussion")
PULLS_DIR = ".forkmesh/pulls"
PULL_LEDGER = "forkmesh/pulls"
PULL_BRANCH_PREFIX = "forkmesh/pull/"
ISSUES_DIR = ".forkmesh/issues"
COLLAB_SETTINGS_PATHS = (".forkmesh/issues.json", ".forkmesh/pulls.json",
                         "AI_POLICY.md")
DISCUSSIONS_DIR = ".forkmesh/discussions"
BLOB_BATCH = 50           # 06-repo-content.js batches /blobs by 50
GENERIC_LIST_LIMIT = 50   # loadRepoRecordsFromMirror: non-pull collections
PARALLEL_READS = 12
MAX_BUNDLES = 64
# A view's last bundle is served as is for BUNDLE_FRESH_SECONDS, then (up to
# BUNDLE_STALE_SECONDS old) served while one background build refreshes it,
# so neither the page nor /page-reads waits for a build once a view has been
# built. Reads pinned to a commit are keyed by that commit; the rest are
# re-read on every refresh.
BUNDLE_FRESH_SECONDS = 5.0
BUNDLE_STALE_SECONDS = 300.0
PUBLIC_CHECK_SECONDS = 5.0
MAX_BUNDLE_BYTES = 32 * 1024 * 1024
SEED_ERROR = "__seedError"
NUMBERED = re.compile(r"^\d+$")
CLAIMED_NAME = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")


def read_key(operation, pairs):
    """The dashboard's lookup key for one read (seededRepoRead in
    06-repo-content.js builds the same string): the operation and its query
    pairs, sorted, without the routing-only ``fmv``/``_`` fields."""
    items = sorted((str(k), str(v)) for k, v in pairs if k not in ("fmv", "_"))
    return operation + "?" + "&".join("%s=%s" % item for item in items)


def is_commit(value):
    return bool(FULL_COMMIT_RE.match(str(value or "").lower()))


def parse_front_matter(text):
    """06-repo-content.js parseFrontMatter."""
    text = str(text or "")
    if not (text.startswith("---\n") or text.startswith("---\r\n")):
        return {}, text.strip()
    normalized = text.replace("\r\n", "\n")
    end = normalized.find("\n---\n", 4)
    if end < 0:
        return {}, text.strip()
    values = {}
    for line in normalized[4:end].split("\n"):
        colon = line.find(":")
        if colon <= 0:
            continue
        values[line[:colon].strip()] = line[colon + 1:].strip()
    return values, normalized[end + 5:].strip()


def blob_text(blob):
    if not isinstance(blob, dict):
        return ""
    content = blob.get("content") or blob.get("text") or ""
    if blob.get("encoding") == "base64":
        return ""
    return str(content)


class ViewReader:
    """Gateway reads for one bundle, recorded under the dashboard's keys."""

    def __init__(self, fastpath, owner, repo):
        self.fastpath = fastpath
        self.owner = owner
        self.repo = repo
        self.reads = {}
        self.raw = {}  # key -> the gateway's JSON bytes, sent verbatim
        self._limit = asyncio.Semaphore(PARALLEL_READS)

    async def get(self, operation, pairs, negative=False):
        """The read's JSON (recorded in the bundle), or None.

        ``negative`` records a failed read too, for callers in the dashboard
        that treat any failure the same way (a probe of candidate paths)."""
        pairs = [(k, str(v)) for k, v in pairs]
        key = read_key(operation, pairs)
        if key in self.reads:
            data = self.reads[key]
            return None if SEED_ERROR in data else data
        query = gateway_query(operation, urllib.parse.urlencode(pairs))
        if query is None:
            return None
        from .repo_fastpath import IMMUTABLE, cache_control
        immutable = cache_control(operation, query) == IMMUTABLE
        url = self.fastpath.gateway_url(self.owner, self.repo, operation,
                                        query)
        async with self._limit:
            status, body, data = await self.fastpath.read(url, immutable)
        if status == 200 and isinstance(data, dict) \
                and data.get("ok") is not False:
            self.reads[key] = data
            self.raw[key] = body
            return data
        if negative and status in (404, 503):
            error = data.get("error") if isinstance(data, dict) else ""
            self.reads[key] = {SEED_ERROR: {
                "status": 404, "error": str(error or "not_found")}}
            self.raw[key] = json.dumps(self.reads[key]).encode()
        return None


# -- planners: the dashboard's read sequence for each view ------------------
# Each mirrors the client code it names; a read it does not know about is
# simply fetched by the dashboard as before.

async def _root(reader, branch):
    # renderRepoDetail reads the root tree of the selected branch first.
    await reader.get("tree", [("path", ""), ("ref", branch)])


async def _pull_ledger(reader):
    """resolveRepoPullMetadataCommit: (commit, tree) of forkmesh/pulls."""
    tree = await reader.get("tree", [("path", PULLS_DIR),
                                     ("ref", PULL_LEDGER)])
    commit = str((tree or {}).get("commit")
                 or ((tree or {}).get("analysis") or {}).get("commit") or "")
    return (commit if is_commit(commit) else ""), tree


async def _blobs(reader, paths, ref):
    """fetchRepoBlobs in BLOB_BATCH-sized requests; {path: blob}."""
    found = {}

    async def batch(chunk):
        pairs = [("path", path) for path in chunk]
        if ref:
            pairs.append(("ref", ref))
        data = await reader.get("blobs", pairs)
        for path, blob in ((data or {}).get("blobs") or {}).items():
            if blob:
                found[path] = blob
    await asyncio.gather(*(batch(paths[i:i + BLOB_BATCH])
                           for i in range(0, len(paths), BLOB_BATCH)))
    return found


def _record_author(values):
    name = str(values.get("authorName") or "").strip()
    if name:
        return name
    claimed = str(values.get("authorClaimedName") or "").strip().lower()
    if CLAIMED_NAME.match(claimed):
        return "%s (unverified)" % claimed
    return values.get("author") or "unknown"


def _record_row(number, values, body, kind):
    return {
        "number": number,
        "title": values.get("title") or "%s #%d" % (
            {"pulls": "pull request", "discussions": "discussion"}.get(
                kind, "record"), number),
        "state": values.get("status") or values.get("state")
        or values.get("category") or "open",
        "author": _record_author(values),
        "ts": values.get("updatedAt") or values.get("createdAt")
        or values.get("ts") or "",
        "base": values.get("base", ""),
        "head": values.get("head", ""),
        "category": values.get("category", ""),
        "body": body[:400],
    }


def _stamp(row):
    try:
        value = float(row.get("ts") or 0)
    except ValueError:
        return 0
    return value * 1000 if 0 < value < 10_000_000_000 else value


def review_policy(reviews, pull_author=""):
    """06-repo-content.js pullReviewPolicy over the listing's
    [[reviewer, state], ...]: each reviewer's latest review counts, the
    pull author's never does."""
    author = str(pull_author or "").strip()
    latest = {}
    for entry in reviews if isinstance(reviews, list) else []:
        if not isinstance(entry, (list, tuple)) or len(entry) < 2:
            continue
        key = str(entry[0] or "").strip()
        if not key or key == author:
            continue
        if entry[1] in ("approved", "changes_requested"):
            latest[key] = entry[1]
        else:
            latest.pop(key, None)
    approvals = sum(1 for value in latest.values() if value == "approved")
    blockers = sum(1 for value in latest.values()
                   if value == "changes_requested")
    return {"approvals": approvals, "blockers": blockers,
            "ready": approvals >= 1 and blockers == 0}


def review_state(reviews, pull_author, status, draft=False):
    """06-repo-content.js pullListReviewState."""
    if not isinstance(reviews, list):
        return None
    policy = review_policy(reviews, pull_author)
    is_open = str(status or "open").lower() == "open" and not draft
    if policy["blockers"]:
        return {"state": "changes_requested", "approvals": policy["approvals"],
                "blockers": policy["blockers"]}
    if policy["ready"]:
        return {"state": "approved", "approvals": policy["approvals"],
                "blockers": 0}
    return ({"state": "review_required", "approvals": 0, "blockers": 0}
            if is_open else None)


def _is_draft(values):
    return str(values.get("draft") or "").strip().lower() in (
        "true", "yes", "1")


async def plan_pulls(reader, branch):
    """loadRepoRecordsFromMirror for .forkmesh/pulls."""
    root = asyncio.ensure_future(_root(reader, branch))
    commit, _named = await _pull_ledger(reader)
    rows = []
    if commit:
        tree = await reader.get("tree", [("path", PULLS_DIR), ("ref", commit)])
        tree = tree or {}
        listed = sorted(
            (e for e in tree.get("entries") or []
             if e.get("type") == "tree" and NUMBERED.match(str(e.get("name")))),
            key=lambda e: -int(e["name"]))
        names = {str(e["name"]) for e in listed}
        refs = tree.get("pullRefs") if isinstance(
            tree.get("pullRefs"), dict) else {}
        branch_only = [(name, str(refs[name]).strip()) for name in refs
                       if NUMBERED.match(name) and name not in names
                       and is_commit(refs[name])]
        reviews = tree.get("pullReviews") if isinstance(
            tree.get("pullReviews"), dict) else None
        paths = ["%s/%s/pull.md" % (PULLS_DIR, e["name"]) for e in listed]
        blobs, extra = await asyncio.gather(
            _blobs(reader, paths, commit),
            asyncio.gather(*(reader.get("blob", [
                ("path", "%s/%s/pull.md" % (PULLS_DIR, name)),
                ("ref", ref)]) for name, ref in branch_only)))
        for entry in listed:
            number = int(entry["name"])
            blob = blobs.get("%s/%d/pull.md" % (PULLS_DIR, number))
            if blob:
                values, body = parse_front_matter(blob_text(blob))
                row = _record_row(number, values, body, "pulls")
                if reviews is not None:
                    row["review"] = review_state(
                        reviews.get(str(number)) or [],
                        values.get("author", ""), row["state"],
                        _is_draft(values))
                rows.append(row)
        for (name, _ref), blob in zip(branch_only, extra):
            if blob:
                rows.append(_record_row(int(name), *parse_front_matter(
                    blob_text(blob)), "pulls"))
    await root
    rows = [row for row in rows
            if str(row["state"]).strip().lower() != "deleted"]
    rows.sort(key=lambda r: (-_stamp(r), -r["number"]))
    return {"rows": rows}


async def plan_discussions(reader, branch):
    root = asyncio.ensure_future(_root(reader, branch))
    tree = await reader.get("tree", [("path", DISCUSSIONS_DIR),
                                     ("ref", branch)], negative=True)
    rows = []
    if tree:
        listed = sorted(
            (e for e in tree.get("entries") or []
             if e.get("type") == "tree" and NUMBERED.match(str(e.get("name")))),
            key=lambda e: -int(e["name"]))[:GENERIC_LIST_LIMIT]
        paths = ["%s/%s/discussion.md" % (DISCUSSIONS_DIR, e["name"])
                 for e in listed]
        blobs = await _blobs(reader, paths, branch)
        for entry in listed:
            blob = blobs.get("%s/%s/discussion.md" % (
                DISCUSSIONS_DIR, entry["name"]))
            if blob:
                rows.append(_record_row(int(entry["name"]), *parse_front_matter(
                    blob_text(blob)), "discussions"))
    await root
    rows.sort(key=lambda r: (-_stamp(r), -r["number"]))
    return {"rows": rows}


def issue_path(number, subdir=""):
    base = "%s/%s" % (ISSUES_DIR, subdir) if subdir else ISSUES_DIR
    return "%s/%d/issue-%d.json" % (base, number, number)


def issue_summary(text, number):
    """The fields of parseIssueJson the first view shows."""
    try:
        issue = json.loads(text or "{}")
    except ValueError:
        issue = {}
    if not isinstance(issue, dict):
        issue = {}
    events = [e for e in issue.get("events") or [] if isinstance(e, dict)]
    opened = next((e for e in events if e.get("type") == "open"), {})
    edits = {str(e.get("target")): e for e in events
             if e.get("type") == "edit" and e.get("target")}
    effective = edits.get(str(opened.get("id") or ""), opened)
    status = issue.get("status") or issue.get("state") or "open"
    for event in events:
        if event.get("type") == "status" and event.get("status"):
            status = event["status"]
    creator = opened.get("author") or ""
    deleted = any(e.get("type") == "delete" and e.get("target") == "self"
                  and e.get("author") and e.get("author") == creator
                  for e in events)
    labels = issue.get("labels")
    if not isinstance(labels, list):
        labels = []
    removed = {str(e.get("target")) for e in events
               if e.get("type") == "delete" and e.get("target")
               and e.get("target") != "self"}
    comments = [e for e in events if e.get("type") == "comment"
                and str(e.get("id") or "") not in removed]
    return {
        "number": int(issue.get("number") or number),
        "title": issue.get("title") or opened.get("title")
        or "issue #%d" % number,
        "state": status,
        "deleted": deleted,
        "author": issue.get("authorName") or opened.get("authorName")
        or issue.get("author") or opened.get("author") or "unknown",
        "ts": issue.get("createdAt") or opened.get("ts") or "",
        "labels": [str(label) for label in labels][:20],
        "body": str(effective.get("body") or issue.get("body") or ""),
        "comments": [{"author": c.get("authorName") or c.get("author")
                      or "unknown", "ts": c.get("ts") or "",
                      "body": str(c.get("body") or "")} for c in comments],
    }


async def _open_issue_index(reader, ref_pairs, path=ISSUES_DIR + "/open"):
    tree = await reader.get("tree", [("path", path)] + ref_pairs)
    return tree


def index_list_row(summary):
    body = summary.get("body")
    created = summary.get("createdAtMs")
    return {
        "number": int(summary.get("number") or 0),
        "title": summary.get("title"),
        "state": summary.get("status"),
        "deleted": bool(summary.get("deleted")),
        "author": summary.get("author"),
        "ts": created or "",
        "labels": [str(label) for label in summary.get("labels") or []][:20],
        "body": ("" if body is None else str(body))[:300],
    }


def _indexed_issues(index):
    if not isinstance(index, dict) or index.get("truncated") \
            or not isinstance(index.get("issues"), list):
        return None
    rows = [index_list_row(item) for item in index["issues"]
            if isinstance(item, dict) and item.get("location") in ("open", "")]
    legacy = {int(item.get("number") or 0) for item in index["issues"]
              if isinstance(item, dict) and item.get("location") == ""}
    open_count = int(index.get("openFolders") or 0) + sum(
        1 for row in rows if row["number"] in legacy
        and row["state"] != "closed" and not row["deleted"])
    return {"rows": rows, "open": open_count,
            "closed": int(index.get("closedFolders") or 0)}


async def plan_issues(reader, branch):
    """loadRepoIssues: the folder listing, then the open issues."""
    root = asyncio.ensure_future(_root(reader, branch))
    tree = await reader.get("tree", [("path", ISSUES_DIR), ("ref", branch)],
                            negative=True)
    rows, open_count, closed_count = [], 0, 0
    if tree:
        commit = str(tree.get("commit") or "")
        ref = commit if is_commit(commit) else branch
        indexed = _indexed_issues(
            await reader.get("issue-index", [("ref", ref)]))
        if indexed is not None:
            await root
            return indexed
        entries = tree.get("entries") or []
        legacy = {int(e["name"]): issue_path(int(e["name"]))
                  for e in entries if e.get("type") == "tree"
                  and NUMBERED.match(str(e.get("name")))}
        dirs = [str(e.get("name")) for e in entries
                if e.get("type") == "tree"
                and str(e.get("name")) in ("open", "closed")]
        subtrees = await asyncio.gather(*(reader.get("tree", [
            ("path", "%s/%s" % (ISSUES_DIR, d)), ("ref", ref)])
            for d in dirs))
        by_status = {"open": {}, "closed": {}}
        for name, sub in zip(dirs, subtrees):
            for entry in (sub or {}).get("entries") or []:
                if entry.get("type") == "tree" and NUMBERED.match(
                        str(entry.get("name"))):
                    number = int(entry["name"])
                    by_status[name][number] = issue_path(number, name)
                    legacy.pop(number, None)
        wanted = dict(legacy)
        wanted.update(by_status["open"])
        numbers = sorted(wanted, reverse=True)
        blobs = await _blobs(reader, [wanted[n] for n in numbers], ref)
        for number in numbers:
            blob = blobs.get(wanted[number])
            if blob:
                row = issue_summary(blob_text(blob), number)
                row.pop("comments", None)
                row["body"] = row["body"][:300]
                rows.append(row)
        open_count = len(by_status["open"]) + sum(
            1 for r in rows if r["number"] in legacy
            and r["state"] != "closed" and not r["deleted"])
        closed_count = len(by_status["closed"])
    await root
    return {"rows": rows, "open": open_count, "closed": closed_count}


async def plan_issue(reader, branch, number):
    """loadRepoRecordDetail for an issue, plus the open-issue index it reads
    for mentions and sub-issues (loadRepoIssueRelationIndex)."""
    root = asyncio.ensure_future(_root(reader, branch))

    async def relation_index():
        for pairs in ([("ref", branch), ("related", number)],
                      [("ref", branch)]):
            indexed = await reader.get("issue-index", pairs)
            if _indexed_issues(indexed) is not None:
                return indexed
        tree = await reader.get("tree", [("path", ISSUES_DIR + "/open"),
                                         ("ref", branch)])
        if not tree:
            return
        commit = str(tree.get("commit") or "")
        numbers = sorted((int(e["name"]) for e in tree.get("entries") or []
                          if e.get("type") == "tree"
                          and NUMBERED.match(str(e.get("name")))),
                         reverse=True)
        return await _blobs(reader, [issue_path(n, "open") for n in numbers],
                            commit if is_commit(commit) else "")
    index = asyncio.ensure_future(relation_index())
    settings = asyncio.ensure_future(reader.get(
        "blobs", [("path", path) for path in COLLAB_SETTINGS_PATHS]
        + [("ref", branch)]))
    paths = [issue_path(number, subdir) for subdir in ("open", "closed", "")]
    blobs = await _blobs(reader, paths, branch)
    found = next((path for path in paths if blobs.get(path)), "")
    issue = issue_summary(blob_text(blobs[found]), number) if found else None
    _root_done, open_issues, settings_data = await asyncio.gather(
        root, index, settings)
    return {"issue": issue, "issueJson": blob_text(blobs[found]) if found else "",
            "index": open_issues or {},
            "settings": (settings_data or {}).get("blobs") or {}}


async def _pull_conversation(reader, number, commit):
    tree = await reader.get("tree", [("path", "%s/%d" % (PULLS_DIR, number)),
                                     ("ref", commit)])
    files = sorted(str(e.get("name")) for e in (tree or {}).get("entries") or []
                   if e.get("type") != "tree"
                   and re.match(r"^\d+-", str(e.get("name") or "")))
    if not files:
        return []
    paths = ["%s/%d/%s" % (PULLS_DIR, number, name) for name in files]
    data = await reader.get("blobs", [("path", p) for p in paths]
                            + [("ref", commit)])
    events = []
    for path in paths:
        blob = ((data or {}).get("blobs") or {}).get(path)
        if not blob:
            continue
        values, body = parse_front_matter(blob_text(blob))
        events.append({"type": values.get("type") or "comment",
                       "id": values.get("id", ""),
                       "author": _record_author(values),
                       "ts": values.get("ts") or "", "state":
                       values.get("state", ""), "path": values.get("path", ""),
                       "body": body, "authorKey": values.get("author", ""),
                       "authorName": values.get("authorName", ""),
                       "target": values.get("target", ""),
                       "line": values.get("line", ""),
                       "lineStart": values.get("lineStart", ""),
                       "lineEnd": values.get("lineEnd", "")})
    return events


async def plan_pull(reader, branch, number):
    """loadRepoRecordDetail for a pull request: its record, patch, commits
    and conversation, at the ledger commit or its own branch."""
    root = asyncio.ensure_future(_root(reader, branch))
    record_path = "%s/%d/pull.md" % (PULLS_DIR, number)
    commit, _tree = await _pull_ledger(reader)
    blob = None
    if commit:
        blob = await reader.get("blob", [("path", record_path),
                                         ("ref", commit)], negative=True)
    if not blob:
        own = await reader.get("tree", [
            ("path", "%s/%d" % (PULLS_DIR, number)),
            ("ref", PULL_BRANCH_PREFIX + str(number))])
        own_commit = str((own or {}).get("commit") or "")
        if is_commit(own_commit) and own_commit != commit:
            commit = own_commit
            blob = await reader.get("blob", [("path", record_path),
                                             ("ref", commit)])
    if not blob:
        await root
        return {"pull": None}
    values, body = parse_front_matter(blob_text(blob))
    is_open = str(values.get("status") or "open").lower() == "open"
    settings = asyncio.ensure_future(reader.get(
        "blobs", [("path", path) for path in COLLAB_SETTINGS_PATHS]
        + [("ref", branch)])) if is_open else None
    patch_ref = []

    async def patch():
        if not is_open:
            return ""
        synced = str(values.get("syncedHeadOid") or "").strip().lower()
        onto = str(values.get("headBaseOid") or "").strip().lower()
        if (is_commit(synced) and is_commit(onto)
                and synced == str(values.get("headOid") or "").strip().lower()):
            data = await reader.get("compare", [("base", onto),
                                                ("head", synced)])
            if data and str(data.get("patch") or "").strip():
                return str(data.get("patch") or "")
        data = await reader.get("blob", [
            ("path", "%s/%d/changes.patch" % (PULLS_DIR, number)),
            ("ref", commit)], negative=True)
        text = blob_text(data)
        if text.strip():
            if is_commit(commit):
                patch_ref.append(commit)
            return text
        base, head = (str(values.get("creationBaseOid") or "").strip(),
                      str(values.get("creationHeadOid") or "").strip())
        if is_commit(base) and is_commit(head):
            data = await reader.get("compare", [("base", base),
                                                ("head", head)])
            return str((data or {}).get("patch") or "")
        return ""

    async def commits():
        await reader.get("blob", [
            ("path", "%s/%d/commits.mbox" % (PULLS_DIR, number)),
            ("ref", commit)], negative=True)

    # The branch list (repoPullBaseTip, merge base) is not in the bundle: on
    # a busy repository it is the gateway's most expensive listing (ahead and
    # behind for every branch) and only the merge controls need it.
    diff, conversation, _c = await asyncio.gather(
        patch(), _pull_conversation(reader, number, commit), commits())
    settings_data = await settings if settings is not None else None
    await root
    return {"settings": (settings_data or {}).get("blobs") or {}, "pull": {
        "number": number, "title": values.get("title") or
        "pull request #%d" % number, "state": values.get("status") or "open",
        "author": _record_author(values), "ts": values.get("ts") or values.get("createdAt") or "",
        "base": values.get("base", ""), "head": values.get("head", ""),
        "body": body, "conversation": conversation, "patch": diff,
        "patchRef": patch_ref[0] if patch_ref else ""}}


async def plan_discussion(reader, branch, number):
    root = asyncio.ensure_future(_root(reader, branch))
    folder = "%s/%d" % (DISCUSSIONS_DIR, number)
    blob, tree = await asyncio.gather(
        reader.get("blob", [("path", folder + "/discussion.md"),
                            ("ref", branch)]),
        reader.get("tree", [("path", folder), ("ref", branch)]))
    replies = []
    files = sorted(str(e.get("name")) for e in (tree or {}).get("entries") or []
                   if e.get("type") != "tree"
                   and re.match(r"^\d+-comment\.md$", str(e.get("name") or "")))
    if files:
        paths = ["%s/%s" % (folder, name) for name in files]
        data = await reader.get("blobs", [("path", p) for p in paths]
                                + [("ref", branch)])
        for path in paths:
            item = ((data or {}).get("blobs") or {}).get(path)
            if item:
                values, body = parse_front_matter(blob_text(item))
                replies.append({"id": values.get("id", ""),
                                "type": "comment",
                                "author": values.get("authorName")
                                or values.get("author") or "unknown",
                                "ts": values.get("ts") or "", "body": body})
    await root
    if not blob:
        return {"discussion": None}
    values, body = parse_front_matter(blob_text(blob))
    return {"discussion": {
        "number": number, "title": values.get("title")
        or "discussion #%d" % number, "category": values.get("category", ""),
        "status": values.get("status") if values.get("status") in (
            "closed", "archived") else "open",
        "author": values.get("authorName") or values.get("author")
        or "unknown", "ts": values.get("ts") or values.get("createdAt") or "",
        "body": body, "replies": replies}}


PLANNERS = {
    "pulls": lambda reader, branch, _n: plan_pulls(reader, branch),
    "issues": lambda reader, branch, _n: plan_issues(reader, branch),
    "discussions": lambda reader, branch, _n: plan_discussions(reader, branch),
    "pull": plan_pull,
    "issue": plan_issue,
    "discussion": plan_discussion,
}


# -- bundles ------------------------------------------------------------------

class Bundle:
    __slots__ = ("owner", "repo", "view", "number", "version", "body",
                 "model", "served_by", "ref", "built", "rendered")

    def __init__(self, owner, repo, view, number, raw, model, served_by,
                 ref):
        self.owner, self.repo, self.view, self.number = owner, repo, view, \
            number
        head = {"base": ("/api/repo/%s/%s" % (
            urllib.parse.quote(owner, safe=""),
            urllib.parse.quote(repo, safe=""))).lower(),
            "view": view, "number": number, "ref": ref,
            "servedBy": served_by}
        # The reads are the gateway's own JSON bytes, joined rather than
        # re-serialized: a pulls bundle is ~350 KB, and dumping it on every
        # build held the event loop on a 1 vCPU node.
        parts = [json.dumps(head, separators=(",", ":"))[:-1].encode(),
                 b',"reads":{']
        for index, key in enumerate(sorted(raw)):
            if index:
                parts.append(b",")
            parts += [json.dumps(key).encode(), b":", raw[key]]
        parts.append(b"}}")
        self.body = b"".join(parts)
        self.version = hashlib.sha256(self.body).hexdigest()[:20]
        self.rendered = {}
        self.model = model
        self.served_by = served_by
        self.ref = ref
        self.built = time.monotonic()

    def url(self):
        query = [("view", self.view)]
        if self.number:
            query.append(("number", str(self.number)))
        query.append(("v", self.version))
        return "/api/repo/%s/%s/page-reads?%s" % (
            urllib.parse.quote(self.owner, safe=""),
            urllib.parse.quote(self.repo, safe=""),
            urllib.parse.urlencode(query))


_bundles = {}      # version -> Bundle (most recent MAX_BUNDLES)
_latest = {}       # (owner, repo, view, number) -> Bundle
_building = {}     # (owner, repo, view, number) -> Future
_public = {}       # (owner, repo) -> (checked at, served publicly)


async def still_public(fastpath, owner, repo):
    """The gateway still serves the repository on its public route (a
    remembered bundle is never handed out after it went private). The answer
    is kept for PUBLIC_CHECK_SECONDS."""
    now = time.monotonic()
    known = _public.get((owner, repo))
    if known is not None and now - known[0] < PUBLIC_CHECK_SECONDS:
        return known[1]
    status, _body, _data = await fastpath.read(
        fastpath.gateway_url(owner, repo, "tree", []))
    if len(_public) > 1024:
        _public.clear()
    _public[(owner, repo)] = (now, status == 200)
    return status == 200


def _remember(bundle):
    if len(_bundles) >= MAX_BUNDLES:
        oldest = min(_bundles.values(), key=lambda b: b.built)
        _bundles.pop(oldest.version, None)
    _bundles[bundle.version] = bundle
    _latest[(bundle.owner, bundle.repo, bundle.view, bundle.number)] = bundle


def _start(fastpath, key):
    pending = _building.get(key)
    if pending is None:
        pending = asyncio.ensure_future(_build(fastpath, *key))
        _building[key] = pending
        pending.add_done_callback(lambda _f: _building.pop(key, None))
    return pending


def cached(fastpath, owner, repo, view, number=0):
    """The remembered bundle for a view without waiting (None if there is
    none or it is too old); a stale one starts a background refresh."""
    key = (owner, repo, view, number)
    latest = _latest.get(key)
    if latest is None or _public.get((owner, repo), (0, True))[1] is False:
        return None
    age = time.monotonic() - latest.built
    if age >= BUNDLE_STALE_SECONDS:
        return None
    if age >= BUNDLE_FRESH_SECONDS:
        _start(fastpath, key)
    return latest


async def build(fastpath, owner, repo, view, number=0):
    """The bundle for a view (None when the gateway does not serve this
    repository publicly): the remembered one when there is one (see
    cached()), otherwise a build that concurrent requests share."""
    key = (owner, repo, view, number)
    if key in _latest and not await still_public(fastpath, owner, repo):
        _latest.pop(key, None)
        return None
    latest = cached(fastpath, owner, repo, view, number)
    if latest is not None:
        return latest
    return await asyncio.shield(_start(fastpath, key))


async def _build(fastpath, owner, repo, view, number):
    started = time.perf_counter()
    reader = ViewReader(fastpath, owner, repo)
    # The dashboard reads at its selected branch: the gateway's HEAD branch
    # on a fresh page (repoSelectedBranch).
    root = await reader.get("tree", [("path", "")])
    commit = str((root or {}).get("commit") or "")
    if not is_commit(commit):
        _public[(owner, repo)] = (time.monotonic(), False)
        _latest.pop((owner, repo, view, number), None)
        return None
    _public[(owner, repo)] = (time.monotonic(), True)
    branch = await fastpath.default_branch(owner, repo, commit)
    if not branch:
        return None
    key = read_key("tree", [("path", "")])
    reader.reads.pop(key, None)
    reader.raw.pop(key, None)
    try:
        model = await PLANNERS[view](reader, branch, number)
    except Exception as error:  # a partial bundle is still correct
        print("page-reads %s/%s %s: %r" % (owner, repo, view, error))
        model = {}
    served_by = await fastpath.node_name() or ""
    bundle = Bundle(owner, repo, view, number, reader.raw, model,
                    served_by, branch)
    if len(bundle.body) > MAX_BUNDLE_BYTES:
        return None
    previous = _latest.get((owner, repo, view, number))
    if previous is not None and previous.version == bundle.version:
        previous.built = bundle.built  # unchanged: keep its renders
        bundle = previous
    _remember(bundle)
    elapsed = (time.perf_counter() - started) * 1000
    if elapsed > 250:
        print("page-reads %s/%s %s %s built in %.0f ms" % (
            owner, repo, view, number or "", elapsed))
    return bundle


def parse_request(query_string):
    params = urllib.parse.parse_qs(query_string or "", keep_blank_values=True,
                                   max_num_fields=20)
    if any(value for name in PRIVATE_VIEW_FIELDS
           for value in params.get(name, [])):
        return None
    view = (params.get("view") or [""])[0]
    if view not in VIEWS:
        return None
    number = 0
    if view in NUMBER_VIEWS:
        text = (params.get("number") or [""])[0]
        if not text.isdigit() or not 0 < int(text) < 10 ** 9:
            return None
        number = int(text)
    return view, number, (params.get("v") or [""])[0]


async def handle(fastpath, scope, send, owner, repo):
    """ASGI answer for GET /api/repo/<o>/<r>/page-reads."""
    method = scope.get("method", "GET").upper()
    parsed = parse_request(
        (scope.get("query_string") or b"").decode("latin-1"))
    if parsed is None:
        return False  # the Worker answers (404)
    view, number, version = parsed
    bundle = _bundles.get(version) if version else None
    immutable = bundle is not None and (bundle.owner, bundle.repo,
                                        bundle.view, bundle.number) == (
        owner, repo, view, number) and await still_public(fastpath, owner,
                                                           repo)
    if not immutable:
        bundle = await build(fastpath, owner, repo, view, number)
    if bundle is None:
        return False
    from .repo_fastpath import IMMUTABLE, _if_none_match
    etag = '"fm-view-%s"' % bundle.version
    headers = [(b"content-type", b"application/json; charset=utf-8"),
               (b"x-content-type-options", b"nosniff"),
               (b"x-forkmesh-local-reads", b"1"),
               (b"cache-control", (IMMUTABLE if immutable
                                   else "no-cache").encode()),
               (b"etag", etag.encode())]
    if _if_none_match(scope.get("headers") or [], etag):
        await send({"type": "http.response.start", "status": 304,
                    "headers": headers[1:]})
        await send({"type": "http.response.body", "body": b""})
        return True
    headers.append((b"content-length", str(len(bundle.body)).encode()))
    await send({"type": "http.response.start", "status": 200,
                "headers": headers})
    await send({"type": "http.response.body",
                "body": b"" if method == "HEAD" else bundle.body})
    return True


# -- server-rendered first view (repo_ssr.finish) -----------------------------

LIST_PAGE_SIZE = 25  # REPO_COLLECTION_PAGE_SIZE
MAX_DIFF_LINES = 1500
MAX_DIFF_FILES = 60
MAX_BODY_CHARS = 262_144
VIEW_PATH_RE = re.compile(
    r"^/([A-Za-z0-9][A-Za-z0-9_-]{0,63})/([A-Za-z0-9._-]{1,100})/"
    r"(pulls|pull|issues|discussions)(?:/(\d{1,9})"
    r"(?:/(files|commits|checks))?)?/?$")

DIFF_PATH_RE = re.compile(
    r"^/([A-Za-z0-9][A-Za-z0-9_-]{0,63})/([A-Za-z0-9._-]{1,100})/"
    r"pulls?/([1-9]\d{0,8})\.(diff|patch|txt)$")
SHORT_DIFF_PATH_RE = re.compile(
    r"^/([A-Za-z0-9_-][A-Za-z0-9._-]{0,99})/pulls?/([1-9]\d{0,8})"
    r"\.(diff|patch|txt)$")

VIEW_STYLE = """<style>
.fm-ssr-tabs{display:flex;gap:4px;border-bottom:1px solid rgb(var(--dashboard-border-rgb));margin:0 0 16px;overflow:hidden}
.fm-ssr-tabs a{padding:8px 12px;color:rgb(var(--dashboard-muted-foreground-rgb));border-bottom:2px solid transparent;white-space:nowrap}
.fm-ssr-tabs a.on{color:rgb(var(--dashboard-foreground-rgb));border-bottom-color:#f78166;font-weight:600}
.fm-ssr-item{padding:8px 16px;border-top:1px solid rgb(var(--dashboard-border-rgb))}.fm-ssr-item:first-child{border-top:0}
.fm-ssr-item>a{font-weight:600;color:rgb(var(--dashboard-foreground-rgb));font-size:15px}.fm-ssr-item .m{color:rgb(var(--dashboard-muted-foreground-rgb));font-size:12px;margin-top:2px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.fm-ssr-item .m.fm-ssr-m-chip{display:flex;flex-wrap:wrap;align-items:center;column-gap:4px;white-space:normal}
.fm-ssr-pill{display:inline-block;border-radius:2em;padding:2px 10px;font-size:12px;font-weight:600;color:#fff;background:#238636;margin-right:8px;vertical-align:middle}.fm-ssr-pill.closed{background:#8957e5}.fm-ssr-pill.merged{background:#8957e5}
.fm-ssr-label{display:inline-block;border:1px solid rgb(var(--dashboard-border-rgb));border-radius:2em;padding:0 7px;font-size:11px;margin-left:6px;color:rgb(var(--dashboard-muted-foreground-rgb))}
.fm-ssr-h1{font-size:26px;font-weight:400;margin:0 0 8px;overflow-wrap:anywhere}.fm-ssr-h1 span{color:rgb(var(--dashboard-muted-foreground-rgb))}
.fm-ssr-sub{color:rgb(var(--dashboard-muted-foreground-rgb));margin:0 0 16px;overflow-wrap:anywhere}.fm-ssr-sub code{font-family:ui-monospace,monospace;font-size:12px}
.fm-ssr-event{border:1px solid rgb(var(--dashboard-border-rgb));border-radius:6px;margin:0 0 16px;overflow:hidden;background:rgb(var(--dashboard-card-rgb))}
.fm-ssr-event>div:first-child{padding:8px 16px;background:rgb(var(--dashboard-subtle-rgb,22 27 34));border-bottom:1px solid rgb(var(--dashboard-border-rgb));color:rgb(var(--dashboard-muted-foreground-rgb))}
.fm-ssr-event>.fm-ssr-reactions{padding:0 24px 12px}
.fm-ssr-diff{font-family:ui-monospace,monospace;font-size:12px;line-height:20px;overflow-x:auto}
.fm-ssr-diff div{white-space:pre;padding:0 16px;min-width:max-content}.fm-ssr-diff .a{background:rgba(46,160,67,.15)}.fm-ssr-diff .d{background:rgba(248,81,73,.15)}.fm-ssr-diff .h{color:rgb(var(--dashboard-muted-foreground-rgb));background:rgba(56,139,253,.1)}
.fm-ssr [data-repo-pull-review-prompt]{margin-bottom:16px}
.fm-ssr-file{padding:8px 16px;font-family:ui-monospace,monospace;font-size:12px;border-bottom:1px solid rgb(var(--dashboard-border-rgb));background:rgb(var(--dashboard-subtle-rgb,22 27 34))}
</style>"""


def view_target(scope):
    """(owner, repo, view, number, section) for a pull/issue/discussion
    page this node may render, else None."""
    if scope.get("method", "GET").upper() != "GET" or \
            (scope.get("query_string") or b""):
        return None
    raw = scope.get("raw_path")
    path = raw.decode("latin-1") if raw else (scope.get("path") or "")
    match = VIEW_PATH_RE.match(path)
    if not match:
        return None
    from .repo_ssr import RESERVED_OWNERS
    owner, repo, kind, number, section = match.groups()
    if owner.lower() in RESERVED_OWNERS or repo in (".", "..") \
            or repo.endswith(".git"):
        return None
    if kind in ("pull", "pulls") and (kind == "pull") != bool(number):
        return None
    view = {"pull": "pull", "issues": "issue",
            "discussions": "discussion"}[kind] if number else kind
    return owner.lower(), repo, view, int(number or 0), section or ""


def diff_target(scope):
    if scope.get("method", "GET").upper() not in ("GET", "HEAD") or \
            (scope.get("query_string") or b""):
        return None
    raw = scope.get("raw_path")
    path = raw.decode("latin-1") if raw else (scope.get("path") or "")
    from .repo_ssr import RESERVED_OWNERS
    match = DIFF_PATH_RE.match(path)
    if match:
        owner, repo, number, ext = match.groups()
        if owner.lower() in RESERVED_OWNERS or repo in (".", "..") \
                or repo.endswith(".git"):
            return None
        return owner.lower(), repo, int(number), ext == "txt"
    match = SHORT_DIFF_PATH_RE.match(path)
    if not match or match.group(1).lower() in RESERVED_OWNERS \
            or match.group(1).endswith(".git"):
        return None
    return None, match.group(1), int(match.group(2)), match.group(3) == "txt"


def pull_patch(bundle):
    pull = (bundle.model or {}).get("pull") if bundle is not None else None
    if not isinstance(pull, dict):
        return ""
    return str(pull.get("patch") or "")


EDITABLE_PULL_EVENTS = frozenset({"comment", "review", "line-comment",
                                  "thread-comment", "thread-reply"})


def _timestamp(value):
    try:
        number = float(value)
    except (TypeError, ValueError):
        return 0.0
    return number / 1000 if number > 10_000_000_000 else number


def _same_author(change, target):
    key = str(change.get("authorKey") or "")
    if key and key == str(target.get("authorKey") or ""):
        return True
    name = str(change.get("authorName") or "").strip().lower()
    return bool(name) and name == str(target.get("authorName") or "").strip().lower()


def folded_conversation(events):
    rows = [e for e in events or [] if isinstance(e, dict)]
    by_id = {str(e["id"]): e for e in rows if e.get("id")}
    edits, deleted = {}, set()
    for event in rows:
        if event.get("type") not in ("comment-edit", "comment-delete"):
            continue
        target_id = str(event.get("target") or "")
        target = by_id.get(target_id)
        if not target or target.get("type") not in EDITABLE_PULL_EVENTS \
                or not _same_author(event, target):
            continue
        if event["type"] == "comment-delete":
            if target.get("type") != "review":
                deleted.add(target_id)
        elif target_id not in edits or _timestamp(edits[target_id].get("ts")) \
                <= _timestamp(event.get("ts")):
            edits[target_id] = event
    folded = []
    for event in rows:
        event_id = str(event.get("id") or "")
        if event.get("type") in ("comment-edit", "comment-delete") \
                or event_id in deleted:
            continue
        edit = edits.get(event_id)
        folded.append(dict(event, body=edit.get("body", "")) if edit else event)
    return folded


def _text_date(value):
    import datetime
    number = _timestamp(value)
    if number <= 0:
        return str(value or "").strip()
    try:
        return datetime.datetime.fromtimestamp(
            number, datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    except (OverflowError, OSError, ValueError):
        return ""


def _text_anchor(event):
    path = str(event.get("path") or "").strip()
    if not path:
        return ""
    start = str(event.get("lineStart") or event.get("line") or "").strip()
    end = str(event.get("lineEnd") or "").strip()
    if start and end and end != start:
        return "%s:%s-%s" % (path, start, end)
    return "%s:%s" % (path, start) if start else path


def _text_action(event):
    kind = str(event.get("type") or "comment")
    state = str(event.get("state") or "").strip().lower()
    if kind == "review":
        return {"approved": "approved", "changes_requested": "requested changes",
                "commented": "reviewed"}.get(state, "reviewed")
    if kind == "thread-reply":
        return "replied"
    if kind in ("thread-state", "suggestion-state"):
        return "marked the thread %s" % (state.replace("_", " ") or "updated")
    if kind in ("comment", "line-comment", "thread-comment"):
        return "commented"
    return kind.replace("-", " ")


def pull_text(bundle):
    pull = (bundle.model or {}).get("pull") if bundle is not None else None
    if not isinstance(pull, dict):
        return ""
    number = int(pull.get("number") or 0)
    lines = [
        "Pull request #%d: %s" % (number, pull.get("title") or ""),
        "Author: %s" % (pull.get("author") or "unknown"),
        "Merges %s into %s" % (pull.get("head") or "?", pull.get("base") or "main"),
        "State: %s" % (pull.get("state") or "open"),
        "Opened: %s" % _text_date(pull.get("ts")),
        "",
        "## Description",
        "",
        str(pull.get("body") or "").strip() or "(no description)",
        "",
    ]
    events = []
    for event in folded_conversation(pull.get("conversation")):
        body = str(event.get("body") or "").strip()
        kind = str(event.get("type") or "comment")
        if kind in ("commit", "agent") or (not body and kind != "review"):
            continue
        anchor = _text_anchor(event)
        when = _text_date(event.get("ts"))
        heading = "### %s %s%s%s" % (
            event.get("author") or "unknown", _text_action(event),
            " on %s" % anchor if anchor else "", ", %s" % when if when else "")
        events.append(heading + ("\n\n" + body if body else ""))
    lines.append("## Comments and reviews (%d)" % len(events))
    lines.append("")
    lines.append("\n\n".join(events) if events else "No comments yet.")
    lines.append("")
    patch = str(pull.get("patch") or "")
    lines += ["## Diff", ""]
    lines.append(patch.rstrip("\n") if patch.strip()
                 else "No diff is available for this pull request.")
    return "\n".join(lines) + "\n"


def _esc(value):
    import html
    return html.escape(str(value or ""))


def _person(name):
    from .repo_ssr_issue import actor_is_bot, profile_href
    text = str(name or "").strip()
    href = "" if actor_is_bot(text) else profile_href(re.sub(r"^@", "", text).lower())
    return '<a href="%s">%s</a>' % (_esc(href), _esc(text)) if href else _esc(text)


def _date(value):
    import datetime
    try:
        number = float(value)
    except (TypeError, ValueError):
        return _esc(str(value or "")[:10])
    if number <= 0:
        return ""
    if number > 10_000_000_000:
        number /= 1000
    try:
        return datetime.datetime.fromtimestamp(
            number, datetime.timezone.utc).strftime("%b %-d, %Y")
    except (OverflowError, OSError, ValueError):
        return ""


def _pill(state):
    state = str(state or "open").lower()
    cls = "" if state == "open" else " closed"
    return '<span class="fm-ssr-pill%s">%s</span>' % (cls, _esc(
        state.capitalize()))


def _md(text, base):
    from .repo_ssr import markdown_html
    return markdown_html(str(text or "")[:MAX_BODY_CHARS], base)


def _frame(bundle, active, inner, link=None):
    owner, repo = bundle.owner, bundle.repo
    base = link or "/%s/%s" % (urllib.parse.quote(owner), urllib.parse.quote(repo))
    tabs = "".join('<a href="%s%s"%s>%s</a>' % (
        base, suffix, ' class="on"' if name == active else "", label)
        for name, suffix, label in (
            ("code", "", "Code"), ("issues", "/issues", "Issues"),
            ("pulls", "/pulls", "Pull requests"),
            ("discussions", "/discussions", "Discussions")))
    title = '<a href="%s">%s</a>' % (_esc(base), _esc(repo))
    if not link:
        title = '<a href="/@%s">%s</a> / %s' % (
            _esc(urllib.parse.quote(owner)), _esc(owner), title)
    return (
        '<div class="fm-ssr" data-repo-ssr>'
        '<h1 class="fm-ssr-title">%s</h1><nav class="fm-ssr-tabs">%s</nav>%s'
        '</div>' % (title, tabs, inner))


REVIEW_CHIPS = {
    "approved": ("check", "Approved", "border-success/40 text-success"),
    "changes_requested": ("file-diff", "Changes requested",
                          "border-destructive/40 text-destructive"),
    "review_required": ("eye", "Review required",
                        "border-border text-muted-foreground"),
}
REVIEW_ICONS = {
    "check": '<path d="M20 6 9 17l-5-5"></path>',
    "file-diff": (
        '<path d="M15 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V7Z">'
        '</path><path d="M9 10h6"></path><path d="M12 13V7"></path>'
        '<path d="M9 17h6"></path>'),
    "eye": (
        '<path d="M2.062 12.348a1 1 0 0 1 0-.696 10.75 10.75 0 0 1 19.876 0 1 '
        '1 0 0 1 0 .696 10.75 10.75 0 0 1-19.876 0"></path>'
        '<circle cx="12" cy="12" r="3"></circle>'),
}


def _review_chip(review):
    """06-repo-content.js pullListReviewChipHtml, with the icon drawn."""
    chip = REVIEW_CHIPS.get((review or {}).get("state"))
    if not chip:
        return ""
    name, label, tone = chip

    def count(n):
        return "%s reviewer%s" % ("{:,}".format(n), "" if n == 1 else "s")
    if review["state"] == "approved":
        title = "Approved by " + count(review.get("approvals") or 0)
    elif review["state"] == "changes_requested":
        title = "Changes requested by " + count(review.get("blockers") or 0)
    else:
        title = "Needs an approval from someone other than the author"
    return ('<span data-repo-pull-review="%s" title="%s" class="inline-flex '
            'shrink-0 items-center gap-1 rounded-full border px-1.5 '
            'text-[11px] font-medium leading-4 %s"><svg xmlns="http://'
            'www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" '
            'fill="none" stroke="currentColor" stroke-width="2" '
            'stroke-linecap="round" stroke-linejoin="round" class="lucide '
            'lucide-%s h-3 w-3">%s</svg>%s</span>' % (
                review["state"], _esc(title), tone, name, REVIEW_ICONS[name],
                label))


def _list(bundle, kind, rows, open_count=None, closed_count=None, link=None):
    base = link or "/%s/%s" % (urllib.parse.quote(bundle.owner),
                               urllib.parse.quote(bundle.repo))
    if kind in ("pulls", "issues"):
        shown = [r for r in rows if str(r.get("state") or "open").lower()
                 == "open" and not r.get("deleted")]
        if kind == "issues":
            shown.sort(key=lambda r: -r["number"])
        if open_count is None:
            open_count = len(shown)
        if closed_count is None:
            closed_count = len(rows) - len(shown)
        head = ('<div class="fm-ssr-commit"><b>%d Open</b><span>%d Closed'
                '</span></div>' % (open_count, closed_count))
    else:
        shown = rows
        head = '<div class="fm-ssr-commit"><b>%d discussions</b></div>' % (
            len(rows))
    items = []
    for index, row in enumerate(shown[:LIST_PAGE_SIZE]):
        number = row["number"]
        meta = "#%d opened by %s" % (number, _person(row.get("author")))
        when = _date(row.get("ts"))
        if when:
            meta += " &middot; " + when
        if row.get("category"):
            meta += " &middot; " + _esc(row["category"])
        chip = _review_chip(row.get("review"))
        if chip:
            # The chip wraps under the line on a phone, as the dashboard's does.
            meta = '<span>%s</span>%s' % (meta, chip)
        labels = "".join('<span class="fm-ssr-label">%s</span>' % _esc(x)
                         for x in row.get("labels") or [])
        items.append(
            '<div class="fm-ssr-item" data-ssr-record="%s"%s><a href="%s/%s/%d">'
            '%s</a>%s<div class="m%s">%s</div></div>' % (
                kind, " data-ssr-usable" if index == 0 else "", _esc(base),
                "pull" if kind == "pulls" else kind, number,
                _esc(row.get("title")), labels,
                " fm-ssr-m-chip" if chip else "", meta))
    if not items:
        items.append('<div class="fm-ssr-item" data-ssr-usable>No open %s.'
                     '</div>' % {"pulls": "pull requests"}.get(kind, kind))
    return _frame(bundle, kind, '<div class="fm-ssr-box">%s%s</div>' % (
        head, "".join(items)), link)


def _diff(patch, href="", usable=True):
    files, lines, current = [], 0, None
    for line in patch.split("\n"):
        if line.startswith("diff --git "):
            if len(files) >= MAX_DIFF_FILES or lines >= MAX_DIFF_LINES:
                break
            name = line.split(" b/", 1)[-1] if " b/" in line else line[11:]
            current = [name, []]
            files.append(current)
            continue
        if current is None or line.startswith(("index ", "--- ", "+++ ",
                                               "new file", "deleted file",
                                               "similarity", "rename ",
                                               "old mode", "new mode",
                                               "GIT binary", "literal ",
                                               "delta ")):
            continue
        if lines >= MAX_DIFF_LINES:
            continue
        cls = ("h" if line.startswith("@@") else "a" if line.startswith("+")
               else "d" if line.startswith("-") else "")
        current[1].append('<div%s>%s</div>' % (
            ' class="%s"' % cls if cls else "", _esc(line[:400]) or " "))
        lines += 1
    out = []
    for index, (name, rows) in enumerate(files):
        out.append('<div class="fm-ssr-box"><div class="fm-ssr-file">%s</div>'
                   '<div class="fm-ssr-diff"%s>%s</div></div>' % (
                       _esc(name), " data-ssr-usable" if usable and index == 0
                       else "",
                       "".join(rows)))
    total = patch.count("\ndiff --git ") + (1 if patch.startswith(
        "diff --git ") else 0)
    if total > len(files) or lines >= MAX_DIFF_LINES:
        out.append('<p class="fm-ssr-sub">Showing %d of %d files; the full '
                   'diff %s.</p>' % (len(files), total, (
                       'is at <a href="%s">%s</a>' % (_esc(href), _esc(href)))
                       if href else "loads with the page"))
    return "".join(out) or '<p class="fm-ssr-sub"%s>No changes to show.</p>' % (
        " data-ssr-usable" if usable else "")


REACTION_EVENT_TYPES = ("comment", "review", "line-comment", "thread-comment",
                        "thread-reply")


def _reaction_bar(subject, comment, reactions, extra="fm-ssr-reactions"):
    from . import repo_ssr_issue
    return repo_ssr_issue.reaction_bar(subject, comment,
                                       (reactions or {}).get(comment), None, extra)


def _events(events, base, subject="", reactions=None):
    return "".join(
        '<div class="fm-ssr-event"><div><b>%s</b> %s &middot; %s</div>'
        '<div class="fm-ssr-md">%s</div>%s</div>' % (
            _person(e.get("author")), _esc(e.get("type") or "commented")
            if e.get("type") not in (None, "", "comment") else "commented",
            _date(e.get("ts")), _md(e.get("body"), base),
            _reaction_bar(subject, str(e.get("id") or ""), reactions)
            if subject and e.get("id") and (e.get("type") or "comment")
            in REACTION_EVENT_TYPES else "")
        for e in events if str(e.get("body") or "").strip())


REACTION_VIEWS = {"pull": "pull", "issue": "issue", "discussion": "discussion"}
RENDERED_PER_BUNDLE = 8


def reaction_subject(bundle):
    name = REACTION_VIEWS.get(bundle.view, "")
    return "%s:%d" % (name, bundle.number) if name and bundle.number else ""


def render(bundle, section="", reactions=None, link=None, reader=False, origin=""):
    """HTML of the first view for a bundle, or None (kept per bundle and
    reaction summary)."""
    from .repo_ssr_issue import public_site_origin
    origin = public_site_origin(origin)
    key = (section, reactions_digest(reactions), link, reader, origin)
    if key not in bundle.rendered:
        if len(bundle.rendered) >= RENDERED_PER_BUNDLE:
            bundle.rendered.clear()
        bundle.rendered[key] = _render(bundle, section, reactions, link,
                                       reader, origin)
    return bundle.rendered[key]


def reactions_digest(reactions):
    if not reactions:
        return ""
    return hashlib.sha256(json.dumps(reactions, sort_keys=True, separators=(
        ",", ":")).encode()).hexdigest()[:16]


def _review_prompt(bundle, pull, origin, link):
    from . import pull_review_prompt
    if not pull_review_prompt.is_open(pull):
        return ""
    link = link or "/%s/%s" % (urllib.parse.quote(bundle.owner),
                                urllib.parse.quote(bundle.repo))
    values = pull_review_prompt.fields(
        origin, link, pull["number"], pull,
        bool(str(pull.get("patch") or "").strip()))
    return pull_review_prompt.render(
        values, pull["number"], pull_review_prompt.template_from_blobs(
            (bundle.model or {}).get("settings")))


def _render(bundle, section="", reactions=None, link=None, reader=False, origin=""):
    subject = reaction_subject(bundle)
    model = bundle.model or {}
    base = link or "/%s/%s" % (urllib.parse.quote(bundle.owner),
                               urllib.parse.quote(bundle.repo))
    if bundle.view in ("pulls", "discussions"):
        return _list(bundle, bundle.view, model.get("rows") or [], link=link)
    if bundle.view == "issues":
        return _list(bundle, "issues", model.get("rows") or [],
                     model.get("open"), model.get("closed"), link)
    if bundle.view == "pull" and model.get("pull"):
        pull = model["pull"]
        tabs = "".join('<a href="%s/pull/%d%s"%s>%s</a>' % (
            _esc(base), pull["number"], suffix,
            ' class="on"' if suffix.strip("/") == section else "", label)
            for suffix, label in (("", "Conversation"), ("/commits", "Commits"),
                                  ("/files", "Files changed")))
        patch = pull.get("patch") or ""
        raw = "%s/pull/%d.diff" % (base, pull["number"]) if patch.strip() else ""
        head = ('<h2 class="fm-ssr-h1">%s <span>#%d</span></h2>'
                '<p class="fm-ssr-sub">%s<b>%s</b> wants to merge into '
                '<code>%s</code> from <code>%s</code> &middot; %s%s</p>'
                '<nav class="fm-ssr-tabs">%s</nav>' % (
                    _esc(pull["title"]), pull["number"], _pill(pull["state"]),
                    _person(pull["author"]), _esc(pull["base"]),
                    _esc(pull["head"]), _date(pull["ts"]),
                    ' &middot; <a href="%s" data-pull-raw-diff>Raw diff</a>'
                    % _esc(raw) if raw else "",
                    tabs))
        if section == "files":
            body = _diff(patch, raw)
        else:
            body = ('<div class="fm-ssr-event"><div><b>%s</b> opened this '
                    'pull request</div><div class="fm-ssr-md" '
                    'data-ssr-usable>%s</div>%s</div>%s' % (
                        _person(pull["author"]), _md(pull["body"], base),
                        _reaction_bar(subject, "body", reactions)
                        if str(pull.get("body") or "").strip() else "",
                        _events(pull.get("conversation") or [], base,
                                subject, reactions))) + _review_prompt(
                                    bundle, pull, origin, link)
            if reader and patch.strip():
                body += '<h3 class="fm-ssr-title">Files changed</h3>' + _diff(
                    patch, raw, usable=False)
        return _frame(bundle, "pulls", head + body, link)
    if bundle.view == "issue" and model.get("issue"):
        issue = model["issue"]
        labels = "".join('<span class="fm-ssr-label">%s</span>' % _esc(x)
                         for x in issue.get("labels") or [])
        head = ('<h2 class="fm-ssr-h1">%s <span>#%d</span></h2>'
                '<p class="fm-ssr-sub">%s<b>%s</b> opened this issue '
                '&middot; %s%s</p>' % (
                    _esc(issue["title"]), issue["number"],
                    _pill(issue["state"]), _person(issue["author"]),
                    _date(issue["ts"]), labels))
        body = ('<div class="fm-ssr-event"><div><b>%s</b></div>'
                '<div class="fm-ssr-md" data-ssr-usable>%s</div></div>%s' % (
                    _person(issue["author"]), _md(issue["body"], base),
                    _events(issue.get("comments") or [], base)))
        return _frame(bundle, "issues", head + body, link)
    if bundle.view == "discussion" and model.get("discussion"):
        item = model["discussion"]
        status = item.get("status") or "open"
        head = ('<h2 class="fm-ssr-h1">%s <span>#%d</span></h2>'
                '<p class="fm-ssr-sub">%s<b>%s</b> started this discussion '
                '&middot; %s%s</p>' % (
                    _esc(item["title"]), item["number"],
                    _pill(status) if status != "open" else "",
                    _person(item["author"]),
                    _date(item["ts"]), (" &middot; " + _esc(item["category"]))
                    if item.get("category") else ""))
        body = ('<div class="fm-ssr-event"><div><b>%s</b></div>'
                '<div class="fm-ssr-md" data-ssr-usable>%s</div>%s</div>%s' % (
                    _person(item["author"]), _md(item["body"], base),
                    _reaction_bar(subject, "body", reactions)
                    if str(item.get("body") or "").strip() else "",
                    _events(item.get("replies") or [], base, subject,
                            reactions)))
        return _frame(bundle, "discussions", head + body, link)
    return None
