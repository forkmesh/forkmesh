"""ForkMesh-hosted repositories on the Next core nodes (docs/HOSTED_REPOS.md).

``hosted_git.py`` creates a repository from the website and authorizes pushes
to it. On a relay with ``HOSTED_GIT_CORE_NODES`` set (Next: the three core
nodes) every such repository also gets a ``hosted_repos`` row, and the core
daemons (server/hosted.go) make each node hold a replica:

* ``POST /api/hosted-repositories/replicas`` is signed by a core node's claimed
  key over the router's canonical request message (the runner claim shape, one
  single-use request id). The node reports the copies it holds; the answer is
  the complete desired set. Per repository: whether the caller is its primary
  (rendezvous-first live core: the node pushes go to and the one that writes
  the first commit or runs an import), and, when the caller's copy is behind
  the last recorded push or still empty, the peers to fetch from, each with a
  router-signed read grant for ``/v1/runner/<owner>/<repo>.git``.
* Each node writes only its own ``hosted_repo_copies`` rows (leaderless); the
  quota reads the largest copy.
* A repository whose catalog row disappeared (deleted on the website) is
  marked deleted and listed under ``removed`` for 30 days, so every node moves
  its copy out of the served root.

This module is stdlib-only. ``hosted_git.py`` supplies a runtime adapter with
D1, the router signer, endpoint lookups and the catalog pin writer; the tests
drive it with fakes.
"""

import hashlib
import json
import re
import time
from urllib.parse import urlparse

try:  # Worker: loaded lazily next to entry.py; tests import it directly.
    import edge_routing
except Exception:  # pragma: no cover
    edge_routing = None

try:
    import runner_leases
except Exception:  # pragma: no cover
    runner_leases = None


REPLICAS_PATH = "/api/hosted-repositories/replicas"
# hosted_git.HOSTED_SOURCE: the catalog source of a record the relay authored.
HOSTED_SOURCE = "hosted-web"
ID_RE = re.compile(r"^hr[0-9a-f]{24}$")
NAME_RE = re.compile(r"^[A-Za-z0-9._-]{1,64}$")
DIGEST_RE = re.compile(r"^[0-9a-f]{64}$")
OID_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
ACTIVITY_WEEKS = 52
MAX_COMMIT_COUNT = 10 ** 9
BRANCH = "main"
DEFAULT_MAX_PER_ACCOUNT = 10
DEFAULT_MAX_BYTES = 200 * 1024 * 1024
DEFAULT_IMPORT_HOSTS = ("github.com", "gitlab.com", "codeberg.org")
REMOVED_KEEP_MS = 30 * 24 * 60 * 60 * 1000
# Replica read grants: long enough for a slow fetch of a full-size repository.
GRANT_TTL_MS = 60 * 60 * 1000
MAX_REPORTED = 5000
# An unchanged copy report is rewritten at most this often (seen_at).
COPY_REFRESH_MS = 10 * 60 * 1000
MAX_BODY_BYTES = 512 * 1024

# Each core node writes only its own rows here; nothing else is shared state.
TABLES = (
    """CREATE TABLE IF NOT EXISTS hosted_repos (
        id TEXT PRIMARY KEY,
        key_bi TEXT NOT NULL,
        owner_bi TEXT NOT NULL,
        creator_bi TEXT NOT NULL,
        visibility TEXT NOT NULL CHECK (visibility IN ('public','private')),
        data TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        pushed_at INTEGER NOT NULL DEFAULT 0,
        pushed_by TEXT NOT NULL DEFAULT '',
        deleted_at INTEGER NOT NULL DEFAULT 0)""",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_hosted_repos_key "
    "ON hosted_repos(key_bi) WHERE deleted_at=0",
    "CREATE INDEX IF NOT EXISTS idx_hosted_repos_creator "
    "ON hosted_repos(creator_bi, deleted_at)",
    """CREATE TABLE IF NOT EXISTS hosted_repo_copies (
        repo_id TEXT NOT NULL,
        node TEXT NOT NULL,
        pushed_at INTEGER NOT NULL DEFAULT 0,
        empty INTEGER NOT NULL DEFAULT 1,
        routing_digest TEXT NOT NULL DEFAULT '',
        size_bytes INTEGER NOT NULL DEFAULT 0,
        seen_at INTEGER NOT NULL,
        PRIMARY KEY (repo_id, node))""",
)


# --- settings ---------------------------------------------------------------

def _int_setting(env, name, default):
    try:
        value = int(str(getattr(env, name, "") or "").strip() or default)
    except (TypeError, ValueError):
        return default
    return value if value > 0 else default


def core_nodes(env):
    """The ordered, de-duplicated core node names; empty = core hosting off."""
    raw = str(getattr(env, "HOSTED_GIT_CORE_NODES", "") or "")
    out = []
    for item in raw.split(","):
        name = item.strip().lower()
        if name and edge_routing.NODE_RE.fullmatch(name) and name not in out:
            out.append(name)
    return out


def core_owners(env):
    out = core_nodes(env)
    if not out:
        return out
    raw = str(getattr(env, "HOSTED_GIT_CORE_OWNERS", "") or "")
    for item in raw.split(","):
        name = item.strip().lower()
        if name and edge_routing.NODE_RE.fullmatch(name) and name not in out:
            out.append(name)
    return out


def enabled(env):
    return bool(core_nodes(env))


def max_per_account(env):
    return _int_setting(env, "HOSTED_REPO_MAX_PER_ACCOUNT",
                        DEFAULT_MAX_PER_ACCOUNT)


def max_bytes(env):
    return _int_setting(env, "HOSTED_REPO_MAX_BYTES", DEFAULT_MAX_BYTES)


def import_hosts(env):
    raw = str(getattr(env, "HOSTED_IMPORT_HOSTS", "") or "")
    hosts = [h.strip().lower() for h in raw.split(",") if h.strip()]
    return tuple(hosts) or DEFAULT_IMPORT_HOSTS


# --- pure helpers -----------------------------------------------------------

def rendezvous(nodes, repo_id):
    """Nodes in rendezvous (highest-random-weight) order for one repository."""
    def weight(node):
        return hashlib.sha256(
            ("forkmesh-hosted-v1\n%s\n%s" % (node, repo_id)).encode()
        ).hexdigest()
    return sorted(nodes, key=weight, reverse=True)


def valid_name(name):
    name = str(name or "")
    return bool(
        NAME_RE.fullmatch(name) and not name.startswith(".")
        and not name.lower().endswith(".git") and name not in (".", ".."))


def import_url(value, hosts):
    """A clean public https clone URL on an allowed forge, or ""."""
    raw = str(value or "").strip()
    if not raw or len(raw) > 500:
        return ""
    parsed = urlparse(raw)
    host = (parsed.hostname or "").lower()
    path = parsed.path or ""
    if (parsed.scheme != "https" or parsed.username or parsed.password
            or parsed.port or parsed.query or parsed.fragment
            or host not in hosts or not re.fullmatch(
                r"(/[A-Za-z0-9._-]+){2,6}(\.git)?/?", path)
            or any(part in (".", "..") for part in path.split("/"))):
        return ""
    return "https://" + host + path.rstrip("/")


GITIGNORE_TEMPLATES = {
    "python": (
        "__pycache__/\n*.py[cod]\n*.egg-info/\n.venv/\nvenv/\nbuild/\n"
        "dist/\n.pytest_cache/\n.mypy_cache/\n.env\n"),
    "node": (
        "node_modules/\nnpm-debug.log*\nyarn-error.log*\ndist/\nbuild/\n"
        ".env\ncoverage/\n"),
    "go": "/bin/\n*.exe\n*.test\n*.out\nvendor/\n",
    "rust": "/target/\n**/*.rs.bk\n",
    "java": "*.class\n*.jar\ntarget/\nbuild/\n.gradle/\n.idea/\n",
    "c": "*.o\n*.a\n*.so\n*.out\nbuild/\n",
}

_MIT = """MIT License

Copyright (c) {year} {holder}

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
"""

_ISC = """ISC License

Copyright (c) {year} {holder}

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
"""

_BSD3 = """BSD 3-Clause License

Copyright (c) {year}, {holder}

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
"""

_UNLICENSE = """This is free and unencumbered software released into the public domain.

Anyone is free to copy, modify, publish, use, compile, sell, or
distribute this software, either in source code form or as a compiled
binary, for any purpose, commercial or non-commercial, and by any
means.

In jurisdictions that recognize copyright laws, the author or authors
of this software dedicate any and all copyright interest in the
software to the public domain. We make this dedication for the benefit
of the public at large and to the detriment of our heirs and
successors. We intend this dedication to be an overt act of
relinquishment in perpetuity of all present and future rights to this
software under copyright law.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

For more information, please refer to <https://unlicense.org/>
"""

LICENSE_TEMPLATES = {
    "mit": _MIT, "isc": _ISC, "bsd-3-clause": _BSD3, "unlicense": _UNLICENSE,
}


def build_init(owner, name, description, *, readme, gitignore, license_id,
               author, author_email, created_at_ms, source_url=""):
    """The first-commit description the primary core writes (server/hosted.go).

    Returns (init, error). Every core that writes it produces the same commit
    id: the files, author, message and timestamp are all fixed here.
    """
    if source_url:
        return {"defaultBranch": BRANCH, "importUrl": source_url,
                "files": [], "message": "", "authorName": "",
                "authorEmail": "", "time": 0}, ""
    gitignore = str(gitignore or "").strip().lower()
    license_id = str(license_id or "").strip().lower()
    if gitignore and gitignore not in GITIGNORE_TEMPLATES:
        return None, "invalid_gitignore"
    if license_id and license_id not in LICENSE_TEMPLATES:
        return None, "invalid_license"
    year = time.gmtime(int(created_at_ms) // 1000).tm_year
    files = []
    if readme:
        body = "# %s\n" % name
        if description:
            body += "\n%s\n" % description
        files.append({"path": "README.md", "content": body})
    if gitignore:
        files.append({"path": ".gitignore",
                      "content": GITIGNORE_TEMPLATES[gitignore]})
    if license_id:
        files.append({"path": "LICENSE", "content": LICENSE_TEMPLATES[
            license_id].format(year=int(year), holder=owner)})
    if not files:
        return None, ""
    return {
        "defaultBranch": BRANCH,
        "files": files,
        "message": "Initial commit",
        "authorName": author,
        "authorEmail": author_email,
        "time": int(created_at_ms) // 1000,
        "importUrl": "",
    }, ""


def noreply_email(account, origin):
    host = (urlparse(str(origin or "")).hostname or "forkmesh.invalid")
    host = host[4:] if host.startswith("app.") else host
    return "%s@users.noreply.%s" % (account, host)


def grant_token(message, signature):
    import base64
    return (base64.urlsafe_b64encode(message.encode("utf-8"))
            .decode("ascii").rstrip("=") + "." + signature)


# --- database ---------------------------------------------------------------

_TABLES_READY = set()


async def ensure_tables(runtime):
    """Migration 0199 creates these; this keeps sqlite dev/test envs whole.

    Once per process and database binding (idempotent statements).
    """
    key = id(getattr(runtime, "env", None))
    if key in _TABLES_READY:
        return
    for statement in TABLES:
        await runtime.d1_run(statement)
    _TABLES_READY.add(key)


async def count_for_creator(runtime, creator_bi, forks=False):
    """Live hosted repositories a creator holds: forks or the others.

    Forks have their own, larger allowance (they cost a few KB each), so
    they never use up the repository quota.
    """
    count = 0
    for row in await runtime.d1_all(
            "SELECT data FROM hosted_repos "
            "WHERE creator_bi=? AND deleted_at=0", creator_bi) or []:
        try:
            record = await runtime.open(row.get("data"))
        except Exception:
            record = None
        if bool(fork_of(record)) == bool(forks):
            count += 1
    return count


async def find_by_key(runtime, key_bi):
    return await runtime.d1_first(
        "SELECT id,visibility,created_at,pushed_at,pushed_by,data "
        "FROM hosted_repos WHERE key_bi=? AND deleted_at=0", key_bi)


async def insert(runtime, repo_id, key_bi, owner_bi, creator_bi, visibility,
                 encrypted, created_at):
    await runtime.d1_run(
        "INSERT INTO hosted_repos (id,key_bi,owner_bi,creator_bi,visibility,"
        "data,created_at) VALUES (?,?,?,?,?,?,?)",
        repo_id, key_bi, owner_bi, creator_bi, visibility, encrypted,
        created_at)


async def link_organization(runtime, org_bi, repo, node_owner, now):
    """Link a hosted repository into an organization (org_repos alias)."""
    repo = str(repo or "").lower()
    await runtime.d1_run(
        "UPDATE OR IGNORE org_repos SET repo=? WHERE org_bi=? AND repo<>? "
        "AND lower(repo)=?", repo, org_bi, repo, repo)
    await runtime.d1_run(
        "INSERT INTO org_repos (org_bi, repo, node_owner, created_at) "
        "VALUES (?,?,?,?) ON CONFLICT(org_bi, repo) DO NOTHING",
        org_bi, repo, str(node_owner or "").lower(), now)


async def note_push(runtime, repo_id, node, now):
    await runtime.d1_run(
        "UPDATE hosted_repos SET pushed_at=?,pushed_by=? WHERE id=?",
        now, node, repo_id)


async def size_of(runtime, repo_id):
    row = await runtime.d1_first(
        "SELECT MAX(size_bytes) AS n FROM hosted_repo_copies WHERE repo_id=?",
        repo_id)
    return int((row or {}).get("n") or 0)


# --- forks ------------------------------------------------------------------

DEFAULT_MAX_FORKS_PER_ACCOUNT = 20


def max_forks_per_account(env):
    return _int_setting(env, "HOSTED_MAX_FORKS_PER_ACCOUNT",
                        DEFAULT_MAX_FORKS_PER_ACCOUNT)


def fork_of(record):
    """(parent id, network id) a hosted record names, or None."""
    fork = record.get("fork") if isinstance(record, dict) else None
    if not isinstance(fork, dict):
        return None
    parent = str(fork.get("of") or "")
    network = str(fork.get("network") or parent)
    if (not ID_RE.fullmatch(parent) or not ID_RE.fullmatch(network)
            or parent == str(record.get("id") or "")):
        return None
    return parent, network


async def rows_for_creator(runtime, creator_bi):
    return await runtime.d1_all(
        "SELECT id,key_bi,data FROM hosted_repos "
        "WHERE creator_bi=? AND deleted_at=0", creator_bi) or []


# --- the replica exchange ---------------------------------------------------

def _error(runtime, code, status):
    return runtime.response({"error": code}, status)


def _clean_count(value):
    try:
        number = int(value)
    except (TypeError, ValueError):
        return None
    return number if 0 <= number <= MAX_COMMIT_COUNT else None


def _clean_weeks(value):
    if not isinstance(value, list) or len(value) > ACTIVITY_WEEKS:
        return None
    weeks = []
    for item in value:
        number = _clean_count(item)
        if number is None:
            return None
        weeks.append(number)
    return [0] * (ACTIVITY_WEEKS - len(weeks)) + weeks


def clean_stats(state):
    if not isinstance(state, dict):
        return None
    commit = str(state.get("commit") or "").lower()
    count = _clean_count(state.get("commitCount"))
    weeks = _clean_weeks(state.get("activityWeeks"))
    if not OID_RE.fullmatch(commit) or not count or weeks is None:
        return None
    stats = {"commit": commit, "commitCount": str(count),
             "activityWeeks": weeks}
    merges = _clean_count(state.get("mergeCount"))
    merge_weeks = _clean_weeks(state.get("mergeActivityWeeks"))
    if merges is not None and merges <= count and merge_weeks is not None:
        stats["mergeCount"] = str(merges)
        stats["mergeActivityWeeks"] = merge_weeks
    return stats


def _clean_reports(value):
    reports = {}
    if not isinstance(value, dict):
        return reports
    for repo_id, state in list(value.items())[:MAX_REPORTED]:
        if not ID_RE.fullmatch(str(repo_id)) or not isinstance(state, dict):
            continue
        try:
            pushed_at = max(0, int(state.get("pushedAt") or 0))
            size = max(0, min(int(state.get("sizeBytes") or 0), 1 << 50))
        except (TypeError, ValueError):
            continue
        digest = str(state.get("routingDigest") or "").lower()
        reports[str(repo_id)] = {
            "pushedAt": pushed_at,
            "empty": bool(state.get("empty")),
            "digest": digest if DIGEST_RE.fullmatch(digest) else "",
            "sizeBytes": size,
        }
        stats = clean_stats(state)
        if stats and not state.get("empty"):
            reports[str(repo_id)]["stats"] = stats
    return reports


async def replicas(runtime, raw):
    """POST /api/hosted-repositories/replicas (core-node signed)."""
    cores = runtime.core_nodes()
    if not cores:
        return _error(runtime, "not_found", 404)

    async def core_keys(name):
        return await runtime.node_keys(name) if name in cores else []

    caller = await runner_leases._verify_signed(
        runtime, REPLICAS_PATH, raw, core_keys)
    if not caller:
        return _error(runtime, "unauthorized", 401)
    try:
        data = json.loads(raw.decode("utf-8")) if raw else {}
    except (UnicodeDecodeError, ValueError):
        return _error(runtime, "invalid_json", 400)
    if not isinstance(data, dict):
        return _error(runtime, "invalid_json", 400)
    await ensure_tables(runtime)
    now = runtime.now()
    reports = _clean_reports(data.get("repositories"))
    push_reports = data.get("pushMirrors")
    if not isinstance(push_reports, dict):
        push_reports = {}
    mirror_keys = (await runtime.push_mirror_keys()
                   if hasattr(runtime, "push_mirror_keys") else set())

    # A catalog row deleted on the website ends the hosted repository, and so
    # does one the owner's own node took over with a signed publication (the
    # "move to my own node" path): the cores then drop their copies.
    for row in await runtime.d1_all(
            "SELECT h.id AS id, r.key_bi AS catalog_key, r.data AS catalog "
            "FROM hosted_repos h LEFT JOIN repositories r "
            "ON r.key_bi=h.key_bi WHERE h.deleted_at=0") or []:
        ended = not row.get("catalog_key")
        if not ended:
            try:
                catalog = await runtime.open(row.get("catalog"))
            except Exception:
                catalog = None
            ended = (isinstance(catalog, dict)
                     and catalog.get("source") != HOSTED_SOURCE)
        if ended:
            await runtime.d1_run(
                "UPDATE hosted_repos SET deleted_at=? "
                "WHERE id=? AND deleted_at=0", now, row["id"])

    rows = await runtime.d1_all(
        "SELECT id,visibility,created_at,pushed_at,pushed_by,deleted_at,data "
        "FROM hosted_repos WHERE deleted_at=0 OR deleted_at>? ORDER BY id",
        now - REMOVED_KEEP_MS)
    copies = {}
    for row in await runtime.d1_all(
            "SELECT repo_id,node,empty,pushed_at,routing_digest,size_bytes,"
            "seen_at FROM hosted_repo_copies"):
        copies.setdefault(row["repo_id"], {})[row["node"]] = row
    live = await runtime.live_core_nodes(cores)
    endpoints = await runtime.core_endpoints(cores)
    repositories = []
    removed = []
    for row in rows or []:
        repo_id = str(row.get("id") or "")
        record = await runtime.open(row.get("data"))
        if not ID_RE.fullmatch(repo_id) or not isinstance(record, dict):
            continue
        owner = str(record.get("owner") or "")
        name = str(record.get("name") or "")
        if int(row.get("deleted_at") or 0):
            removed.append({"id": repo_id, "owner": owner, "name": name})
            continue
        report = reports.get(repo_id)
        stored = copies.get(repo_id, {}).get(caller) or {}
        unchanged = bool(stored) and (
            int(stored.get("pushed_at") or 0) == report["pushedAt"]
            and bool(int(stored.get("empty") or 0)) == report["empty"]
            and str(stored.get("routing_digest") or "") == report["digest"]
            and int(stored.get("size_bytes") or 0) == report["sizeBytes"]
            and now - int(stored.get("seen_at") or 0) < COPY_REFRESH_MS
        ) if report is not None else True
        if report is not None and not unchanged:
            await runtime.d1_run(
                "INSERT INTO hosted_repo_copies (repo_id,node,pushed_at,empty,"
                "routing_digest,size_bytes,seen_at) VALUES (?,?,?,?,?,?,?) "
                "ON CONFLICT(repo_id,node) DO UPDATE SET "
                "pushed_at=excluded.pushed_at,empty=excluded.empty,"
                "routing_digest=excluded.routing_digest,"
                "size_bytes=excluded.size_bytes,seen_at=excluded.seen_at",
                repo_id, caller, report["pushedAt"],
                1 if report["empty"] else 0, report["digest"],
                report["sizeBytes"], now)
            copies.setdefault(repo_id, {})[caller] = {
                "node": caller, "empty": 1 if report["empty"] else 0,
                "pushed_at": report["pushedAt"]}
            if (report["digest"] and not report["empty"]
                    and not int(row.get("pushed_at") or 0)
                    and stored.get("routing_digest") != report["digest"]):
                # The first content a core reports (an initial commit or an
                # import) becomes the routing pin; later pins come from pushes.
                await runtime.pin_initial(owner, name, report["digest"])
        if (report is not None and not unchanged and report.get("stats")
                and report["digest"] and hasattr(runtime, "record_stats")):
            await runtime.record_stats(owner, name, report["digest"],
                                       report["stats"])
        order = rendezvous(live or cores, repo_id)
        primary = order[0] if order else ""
        pushed_at = int(row.get("pushed_at") or 0)
        pushed_by = str(row.get("pushed_by") or "")
        item = {
            "id": repo_id, "owner": owner, "name": name,
            "visibility": "private" if row.get("visibility") == "private"
            else "public",
            "pushedAt": pushed_at,
            "primary": caller == primary,
            "aliases": [a for a in record.get("aliases", [])
                        if edge_routing.NODE_RE.fullmatch(str(a))][:4],
        }
        if caller == primary and isinstance(record.get("init"), dict):
            item["init"] = record["init"]
        fork = fork_of(record)
        if fork and item["visibility"] == "public":
            # Every core builds a fork from its own copy of the parent and the
            # network's shared object pool (server/hosted_forks.go).
            item["forkOf"], item["network"] = fork
        push_report = push_reports.get(repo_id)
        if isinstance(push_report, dict) and hasattr(
                runtime, "record_push_mirror"):
            await runtime.record_push_mirror(
                owner, name, item["aliases"], push_report)
        if caller == primary and mirror_keys:
            mirror = await runtime.push_mirror(
                owner, name, item["aliases"], mirror_keys)
            if mirror:
                item["pushMirror"] = mirror
        empty_here = report is None or report["empty"]
        others = copies.get(repo_id, {})
        content_elsewhere = any(
            not int(copy.get("empty") or 0)
            for node, copy in others.items() if node != caller)
        behind = (report is not None and report["pushedAt"] < pushed_at
                  and caller != pushed_by)
        wants = behind or (empty_here and (
            content_elsewhere or caller != primary))
        if wants:
            sources = [pushed_by] if pushed_by in endpoints else []
            sources += [n for n in order if n not in sources]
            item["sources"] = []
            for node in sources:
                if node == caller or node not in endpoints:
                    continue
                grant = await _read_grant(runtime, node, owner, name, caller,
                                          now)
                if grant:
                    item["sources"].append({
                        "node": node, "grant": grant,
                        "url": endpoints[node] + "/v1/runner/%s/%s.git" % (
                            owner, name),
                    })
        repositories.append(item)
    return runtime.response({"ok": True, "node": caller,
                             "repositories": repositories,
                             "removed": removed})


async def _read_grant(runtime, node, owner, name, caller, now):
    grant_id = "hosted_" + runtime.new_id()[:24]
    message = edge_routing.runner_grant_message(
        node, owner, name, "read", "", caller, grant_id, now,
        now + GRANT_TTL_MS)
    if not message:
        return ""
    signature = await runtime.sign_router(message)
    return grant_token(message, signature) if signature else ""
