import hashlib
import json
import random
import re
from urllib.parse import parse_qs, quote, urlparse

KINDS = ("forkmesh-instance", "git", "github", "gitlab", "codeberg", "other")
STATUSES = ("green", "yellow", "red", "unknown")
MAX_TARGET = 100
MAX_MIRRORS = 100
MAX_URL_LENGTH = 300
MAX_LABEL_LENGTH = 60
MAX_ADDS_PER_DAY = 10
MAX_ADDS_PER_MINUTE = 3
MINUTE_MS = 60 * 1000
HOUR_MS = 60 * MINUTE_MS
DAY_MS = 24 * HOUR_MS
GRACE_MS = DAY_MS
CHECK_INTERVAL_MS = 15 * MINUTE_MS
CHECK_JITTER_MS = 3 * MINUTE_MS
UNCHECKABLE_RETRY_MS = DAY_MS
HEADS_INTERVAL_MS = 5 * MINUTE_MS
SOURCE_READ_INTERVAL_MS = 6 * HOUR_MS
SOURCE_RETRY_MS = 30 * MINUTE_MS
MAX_HEADS = 200
QUOTA_ROWS_PER_RUN = 4
CHECKS_PER_RUN = 6
RUN_BUDGET_MS = 15 * 1000
PROBE_TIMEOUT_SECONDS = 10
SELF_READ_TIMEOUT_SECONDS = 5
MAX_REFS_BYTES = 4 * 1024 * 1024
MAX_SOURCE_BYTES = 64 * 1024
FAILURES_RED = 2
USER_AGENT = "git/2.43.0 (forkmesh-mirror-check)"
FORGE_SOURCE_PATH = ".forkmesh/forge-source.json"
FORGE_SOURCE_SCHEMA = "forkmesh-forge-source-v1"
PUBLIC_CACHE = "public, max-age=30, stale-while-revalidate=60"
PRIVATE_CACHE = "private, no-store"
VARY = {"vary": "authorization, cookie"}
HOST_RE = re.compile(
    r"^(?=.{1,253}$)(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\.)+"
    r"[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
PATH_RE = re.compile(r"^(?:/[A-Za-z0-9._~-]+)+/?$")
SCP_RE = re.compile(r"^([A-Za-z0-9._-]{1,64})@([A-Za-z0-9.-]+):([A-Za-z0-9._~/-]+)$")
USER_RE = re.compile(r"^[A-Za-z0-9._-]{1,64}$")
COMMIT_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
REPOSITORY_RE = re.compile(r"^[A-Za-z0-9_.-]{1,100}/[A-Za-z0-9_.-]{1,100}$")
ID_RE = re.compile(r"^[0-9a-f]{16}$")
BLOCKED_SUFFIXES = ("localhost", "local", "internal", "lan", "home.arpa",
                    "intranet", "corp", "test", "invalid", "example")
DISPLAY_ONLY_SUFFIXES = ("onion", "i2p")
SOURCE_LABELS = {"github": "GitHub (original)"}
STATUS_TEXT = {"green": "Healthy", "yellow": "Behind", "red": "Unreachable",
               "unknown": "Not checked"}


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def _now():
    return int(Date.now())


def _host_allowed(host):
    if not HOST_RE.fullmatch(host):
        return False
    return not any(host == suffix or host.endswith("." + suffix)
                   for suffix in BLOCKED_SUFFIXES)


def _display_only_host(host):
    return any(host.endswith("." + suffix) for suffix in DISPLAY_ONLY_SUFFIXES)


def mirror_key(host, path):
    path = path.rstrip("/")
    if path.lower().endswith(".git"):
        path = path[:-4]
    return hashlib.sha256(("%s%s" % (host, path.lower())).encode(
        "utf-8")).hexdigest()[:16]


def parse_mirror_url(text):
    raw = str(text or "").strip() if isinstance(text, str) else ""
    if not raw or len(raw) > MAX_URL_LENGTH or any(
            c.isspace() or ord(c) < 32 or ord(c) > 126 for c in raw):
        return None
    scp = SCP_RE.fullmatch(raw) if "://" not in raw else None
    if scp:
        host = scp.group(2).lower().rstrip(".")
        path = "/" + scp.group(3).lstrip("/")
        if not _host_allowed(host) or not PATH_RE.fullmatch(path):
            return None
        return {"url": "%s@%s:%s" % (scp.group(1), host, path.lstrip("/")),
                "scheme": "ssh", "host": host, "checkable": False,
                "id": mirror_key(host, path)}
    try:
        parsed = urlparse(raw)
        port = parsed.port
    except ValueError:
        return None
    scheme = parsed.scheme.lower()
    if scheme not in ("https", "git", "ssh") or parsed.query or parsed.fragment \
            or ";" in parsed.path:
        return None
    if parsed.password or (parsed.username and scheme != "ssh"):
        return None
    if parsed.username and not USER_RE.fullmatch(parsed.username):
        return None
    host = str(parsed.hostname or "").lower().rstrip(".")
    if not _host_allowed(host) or not PATH_RE.fullmatch(parsed.path or ""):
        return None
    default = {"https": 443, "git": 9418, "ssh": 22}[scheme]
    netloc = host if port in (None, default) else "%s:%d" % (host, port)
    path = parsed.path.rstrip("/")
    user = parsed.username + "@" if parsed.username else ""
    return {"url": "%s://%s%s%s" % (scheme, user, netloc, path),
            "scheme": scheme, "host": netloc,
            "checkable": scheme == "https" and not _display_only_host(host),
            "id": mirror_key(netloc, path)}


def infer_kind(host, scheme=""):
    name = str(host or "").split(":", 1)[0].lower()
    if name in ("github.com", "www.github.com"):
        return "github"
    if name == "gitlab.com" or name.startswith("gitlab."):
        return "gitlab"
    if name in ("codeberg.org", "www.codeberg.org"):
        return "codeberg"
    if name == "forkmesh.com" or name.endswith(".forkmesh.com"):
        return "forkmesh-instance"
    if scheme in ("git", "ssh"):
        return "git"
    return "other"


def choose_kind(host, scheme, requested):
    inferred = infer_kind(host, scheme)
    requested = str(requested or "").strip().lower()
    if requested in KINDS and inferred in ("other", "git"):
        return requested
    return inferred


def clean_label(value):
    text = " ".join(str(value or "").split()) if isinstance(value, str) else ""
    return "".join(c for c in text if c.isprintable())[:MAX_LABEL_LENGTH]


def clean_target(value):
    if isinstance(value, bool):
        return None
    try:
        number = int(str(value).strip())
    except (TypeError, ValueError):
        return None
    return number if 0 <= number <= MAX_TARGET else None


def _pkt_lines(body):
    cursor = 0
    while cursor + 4 <= len(body):
        try:
            size = int(body[cursor:cursor + 4], 16)
        except ValueError:
            return
        if size == 0:
            cursor += 4
            yield None
            continue
        if size < 4 or cursor + size > len(body):
            return
        yield body[cursor + 4:cursor + size]
        cursor += size


def parse_refs(body):
    if isinstance(body, str):
        body = body.encode("utf-8", "replace")
    refs = {}
    symref = ""
    body = bytes(body)
    if re.match(rb"[0-9a-f]{4}", body) and not re.match(rb"[0-9a-f]{40,64}\t", body):
        for line in _pkt_lines(body):
            if line is None:
                continue
            text = line.decode("utf-8", "replace").rstrip("\n")
            if text.startswith("# service="):
                continue
            text, _, caps = text.partition("\0")
            for cap in caps.split():
                if cap.startswith("symref=HEAD:"):
                    symref = cap[len("symref=HEAD:"):]
            commit, _, name = text.partition(" ")
            if COMMIT_RE.fullmatch(commit) and name:
                refs[name] = commit
    else:
        for raw in body.decode("utf-8", "replace").splitlines():
            commit, _, name = raw.strip().partition("\t")
            if COMMIT_RE.fullmatch(commit) and name:
                refs[name] = commit
    if symref and "HEAD" not in refs and symref in refs:
        refs["HEAD"] = refs[symref]
    return refs, symref


def head_of(refs, branch):
    if branch and refs.get("refs/heads/" + branch):
        return refs["refs/heads/" + branch]
    return refs.get("HEAD", "")


def _field(result, name, default=None):
    if isinstance(result, dict):
        return result.get(name, default)
    return getattr(result, name, default)


def _chunk(value):
    if value is None:
        return b""
    if isinstance(value, (bytes, bytearray)):
        return bytes(value)
    if isinstance(value, str):
        return value.encode("utf-8")
    converter = getattr(value, "to_py", None)
    if callable(converter):
        value = converter()
    return bytes(value)


async def bounded_body(response, limit):
    body = getattr(response, "body", None)
    reader_of = getattr(body, "getReader", None)
    if callable(reader_of):
        reader = reader_of()
        chunks, total = [], 0
        while True:
            result = await reader.read()
            if bool(_field(result, "done", False)):
                return b"".join(chunks)
            chunk = _chunk(_field(result, "value"))
            total += len(chunk)
            if total > limit:
                try:
                    cancelled = reader.cancel()
                    if hasattr(cancelled, "__await__"):
                        await cancelled
                except Exception:
                    pass
                return None
            chunks.append(chunk)
    raw = await response.text()
    raw = raw.encode("utf-8") if isinstance(raw, str) else _chunk(raw)
    return raw if len(raw) <= limit else None


async def probe(url, timeout_seconds):
    target = url.rstrip("/") + "/info/refs?service=git-upload-pack"
    response = await js_fetch_with_timeout(target, {
        "method": "GET", "redirect": "follow",
        "headers": {"user-agent": USER_AGENT,
                    "accept": "application/x-git-upload-pack-advertisement, */*"}},
        timeout_seconds)
    status = int(getattr(response, "status", 0) or 0)
    if status != 200:
        return status, b""
    body = await bounded_body(response, MAX_REFS_BYTES)
    if body is None:
        raise ValueError("refs_too_large")
    return status, body


def _probe_error(error):
    text = str(error or "").lower()
    if "refs_too_large" in text:
        return "advertised refs are too large"
    if "blocked" in text:
        return "refused by this instance's network policy"
    if "timeout" in text or "abort" in text or "timed out" in text:
        return "timed out"
    return "unreachable"


async def check_url(url, branch, timeout_seconds=PROBE_TIMEOUT_SECONDS):
    started = _now()
    try:
        status, body = await probe(url, timeout_seconds)
    except Exception as error:
        return {"at": _now(), "ok": False, "headCommit": "",
                "error": _probe_error(error),
                "latencyMs": max(0, _now() - started)}
    latency = max(0, _now() - started)
    if status != 200:
        return {"at": _now(), "ok": False, "headCommit": "",
                "error": "HTTP %d" % status, "latencyMs": latency}
    refs, _symref = parse_refs(body)
    if not refs:
        return {"at": _now(), "ok": False, "headCommit": "",
                "error": "no git refs advertised", "latencyMs": latency}
    head = head_of(refs, branch)
    return {"at": _now(), "ok": True, "headCommit": head,
            "error": "" if head else "branch %s not found" % (branch or "HEAD"),
            "latencyMs": latency}


def apply_check(state, result, reference=""):
    state = dict(state or {})
    state["lastCheck"] = {key: result.get(key) for key in
                          ("at", "ok", "headCommit", "error", "latencyMs")}
    state["checks"] = int(state.get("checks") or 0) + 1
    if result.get("ok"):
        state["failures"] = 0
        state["lastOkAt"] = int(result.get("at") or 0)
        state["lastOkHead"] = str(result.get("headCommit") or "")
        if reference:
            if state["lastOkHead"] and state["lastOkHead"] == reference:
                state.pop("differentSince", None)
            elif not state.get("differentSince"):
                state["differentSince"] = int(result.get("at") or 0)
    else:
        state["failures"] = int(state.get("failures") or 0) + 1
    return state


def remember_head(heads, commit, now):
    kept = [dict(row) for row in heads or []
            if isinstance(row, dict) and COMMIT_RE.fullmatch(str(row.get("c") or ""))
            and int(row.get("at") or 0) >= now - GRACE_MS]
    if commit and COMMIT_RE.fullmatch(commit):
        for row in kept:
            if row["c"] == commit:
                row["at"] = now
                break
        else:
            kept.append({"c": commit, "from": now, "at": now})
    kept.sort(key=lambda row: -int(row.get("at") or 0))
    return kept[:MAX_HEADS]


def recent_heads(heads, current, now):
    found = {str(row.get("c")) for row in heads or [] if isinstance(row, dict)
             and int(row.get("at") or 0) >= now - GRACE_MS}
    if current:
        found.add(current)
    return found


def classify(state, heads, now, checkable=True):
    state = state or {}
    if not checkable:
        return "unknown", "Not checked (no https address)"
    if not state.get("lastCheck"):
        return "unknown", "Not checked yet"
    if not state.get("lastOkAt"):
        return "red", "Never reached"
    if int(state.get("failures") or 0) >= FAILURES_RED:
        return "red", "Unreachable"
    if int(state.get("lastOkAt") or 0) < now - GRACE_MS:
        return "yellow", "Not reached for a day"
    head = str(state.get("lastOkHead") or "")
    if not head:
        return "yellow", "Default branch missing"
    if head in heads:
        return "green", "In sync"
    return "yellow", "Behind or different"


def classify_source(state, upstream, now):
    state = state or {}
    if not state.get("lastCheck"):
        return "unknown", "Not checked yet"
    if not state.get("lastOkAt"):
        return "red", "Never reached"
    if int(state.get("failures") or 0) >= FAILURES_RED:
        return "red", "Unreachable"
    if int(state.get("lastOkAt") or 0) < now - GRACE_MS:
        return "yellow", "Not reached for a day"
    since = int(state.get("differentSince") or 0)
    if upstream and since and since < now - GRACE_MS:
        return "yellow", "Our copy is behind"
    return "green", "Reachable"


def source_from_document(document):
    if not isinstance(document, dict) or document.get("schema") != FORGE_SOURCE_SCHEMA:
        return None
    provider = str(document.get("provider") or "").strip().lower()
    repository = str(document.get("repository") or "").strip()
    if not REPOSITORY_RE.fullmatch(repository) or ".." in repository:
        return None
    if provider == "github":
        url = "https://github.com/" + repository
    else:
        base = parse_mirror_url(str(document.get("baseUrl") or "").rstrip("/") + "/x")
        if not base or base["scheme"] != "https":
            return None
        url = "https://%s/%s" % (base["host"], repository)
    parsed = parse_mirror_url(url)
    if not parsed or not parsed["checkable"]:
        return None
    return {"provider": provider, "repository": repository, "url": parsed["url"],
            "host": parsed["host"], "id": parsed["id"],
            "kind": infer_kind(parsed["host"], "https"),
            "label": SOURCE_LABELS.get(provider, "%s (original)" % parsed["host"])}


def _public_row(row_id, label, kind, url, host, status, status_label, check,
                builtin=False, **extra):
    check = check or {}
    row = {"id": row_id, "label": label, "kind": kind, "url": url, "host": host,
           "builtin": builtin, "status": status, "statusLabel": status_label,
           "lastCheck": {"at": int(check.get("at") or 0), "ok": bool(check.get("ok")),
                         "headCommit": str(check.get("headCommit") or ""),
                         "error": str(check.get("error") or ""),
                         "latencyMs": int(check.get("latencyMs") or 0)}
           if check else None}
    row.update(extra)
    return row


def overall_status(healthy, target):
    if target <= 0:
        return "green" if healthy > 0 else "unknown"
    if healthy >= target:
        return "green"
    return "yellow" if healthy * 2 >= target else "red"


def build_payload(target, self_url, self_host, our_head, quota_data, mirrors, now,
                  upstream="", viewer=None, clone_url=""):
    quota_data = quota_data if isinstance(quota_data, dict) else {}
    heads = recent_heads(quota_data.get("heads"), our_head, now)
    rows = [_public_row(
        "self", "This instance", "forkmesh-instance", self_url, self_host, "green",
        "Serving this page", {"at": now, "ok": True, "headCommit": our_head},
        builtin=True)]
    source = quota_data.get("source")
    if isinstance(source, dict) and source.get("url"):
        state = quota_data.get("sourceCheck") or {}
        status, label = classify_source(state, upstream, now)
        rows.append(_public_row(
            "source", source.get("label") or "Original", source.get("kind") or "other",
            source["url"], source.get("host") or "", status, label,
            state.get("lastCheck"), builtin=True))
    for mirror in mirrors:
        state = mirror.get("check") or {}
        status, label = classify(state, heads, now, mirror.get("checkable", True))
        row = _public_row(
            mirror["id"], mirror.get("label") or mirror.get("host") or "",
            mirror.get("kind") or "other", mirror.get("url") or "",
            mirror.get("host") or "", status, label, state.get("lastCheck"),
            addedBy=str(mirror.get("addedBy") or ""),
            addedAt=int(mirror.get("addedAt") or 0),
            checkable=bool(mirror.get("checkable", True)))
        if viewer is not None:
            row["canRemove"] = bool(viewer.get("canSetTarget")) or (
                bool(viewer.get("name"))
                and str(mirror.get("addedBy") or "").lower() == viewer["name"])
        rows.append(row)
    healthy = sum(1 for row in rows if row["status"] == "green")
    payload = {"ok": True, "target": int(target or 0), "healthy": healthy,
               "status": overall_status(healthy, int(target or 0)),
               "total": len(rows), "registered": len(mirrors), "max": MAX_MIRRORS,
               "maxTarget": MAX_TARGET, "cloneUrl": clone_url, "mirrors": rows}
    if viewer is not None:
        payload["viewer"] = {"signedIn": bool(viewer.get("name")),
                             "name": str(viewer.get("name") or ""),
                             "canSetTarget": bool(viewer.get("canSetTarget")),
                             "canAdd": bool(viewer.get("name"))}
    return payload


def _error(error, status, message=""):
    body = {"ok": False, "error": error}
    if message:
        body["message"] = message
    return json_response(body, status=status, cache_control=PRIVATE_CACHE)


async def _decrypted(env, stored):
    try:
        data = await decrypt_row(env, stored)
    except Exception:
        return None
    return data if isinstance(data, dict) else None


async def _quota_row(env, repo_bi):
    row = await d1_first(
        env, "SELECT target, data FROM repo_mirror_quota WHERE repo_bi=?", repo_bi)
    if not row:
        return None
    return {"target": int(row.get("target") or 0),
            "data": await _decrypted(env, row.get("data")) or {}}


async def _mirror_rows(env, repo_bi):
    rows = await d1_all(
        env, "SELECT mirror_id, added_by_bi, created_at, data FROM repo_mirror_registry "
        "WHERE repo_bi=? ORDER BY created_at ASC, mirror_id ASC LIMIT ?",
        repo_bi, MAX_MIRRORS)
    out = []
    for row in rows or []:
        data = await _decrypted(env, row.get("data"))
        if not data or not parse_mirror_url(data.get("url")):
            continue
        data["id"] = str(row.get("mirror_id") or "")
        data["addedByBi"] = str(row.get("added_by_bi") or "")
        data["addedAt"] = int(row.get("created_at") or 0)
        out.append(data)
    return out


def _origin(env, request):
    base = str(_public_base_url(env, request) or "").rstrip("/")
    try:
        parsed = urlparse(base)
    except ValueError:
        return "", ""
    if parsed.scheme not in ("https", "http") or not parsed.netloc:
        return "", ""
    return base, parsed.netloc.lower()


def _public_owner(request, owner):
    try:
        parts = [part for part in urlparse(str(request.url)).path.split("/") if part]
    except Exception:
        return owner
    if len(parts) >= 4 and parts[0] == "api" and parts[1] == "repo":
        candidate = safe_segment(parts[2])
        if candidate:
            return candidate
    return owner


async def _can_set_target(env, account, owner, repo):
    if not account:
        return False
    try:
        if await _account_owns_node(env, account, owner):
            return True
        permission = await _org_linked_repo_permission(env, owner, repo, account)
        if team_permission_rank(permission) >= team_permission_rank("maintain"):
            return True
        return bool(await _is_admin(env, account))
    except Exception:
        return False


async def _context(env, request, owner, repo, account=""):
    access = await _repository_access_context(env, request, owner, repo,
                                              viewer=account)
    if access is None or access.get("visibility") != "public":
        return None
    record = access.get("record") or {}
    base, host = _origin(env, request)
    public_owner = _public_owner(request, owner)
    path = "/%s/%s" % (quote(public_owner, safe=""), quote(repo, safe=""))
    return {"repoBi": access["repoBi"], "record": record, "owner": owner,
            "repo": repo, "publicOwner": public_owner,
            "selfUrl": base + path if base else path, "selfHost": host,
            "cloneUrl": (base + path + ".git") if base else "",
            "head": str(record.get("commit") or "").lower(),
            "branch": str(record.get("branch") or "").strip(),
            "upstream": str(record.get("upstreamCommit") or "").lower()}


async def _payload(env, ctx, viewer=None):
    quota = await _quota_row(env, ctx["repoBi"]) or {"target": 0, "data": {}}
    mirrors = await _mirror_rows(env, ctx["repoBi"])
    return build_payload(
        quota["target"], ctx["selfUrl"], ctx["selfHost"], ctx["head"], quota["data"],
        mirrors, _now(), upstream=ctx["upstream"], viewer=viewer,
        clone_url=ctx["cloneUrl"])


def _quota_identity(data, ctx):
    data = dict(data or {})
    data["owner"] = ctx["owner"]
    data["repo"] = ctx["repo"]
    data["publicOwner"] = ctx["publicOwner"]
    return data


async def _store_quota(env, repo_bi, target, data, next_check_at, now):
    await d1_run(
        env,
        "INSERT INTO repo_mirror_quota (repo_bi, target, next_check_at, updated_at, data) "
        "VALUES (?,?,?,?,?) ON CONFLICT(repo_bi) DO UPDATE SET target=excluded.target, "
        "next_check_at=excluded.next_check_at, updated_at=excluded.updated_at, "
        "data=excluded.data",
        repo_bi, int(target), int(next_check_at), now, await encrypt_row(env, data))


async def _ensure_quota(env, ctx, now, target=None):
    quota = await _quota_row(env, ctx["repoBi"])
    data = _quota_identity((quota or {}).get("data"), ctx)
    data["heads"] = remember_head(data.get("heads"), ctx["head"], now)
    stored_target = (quota or {}).get("target", 0) if target is None else target
    due = 0 if not data.get("source") else now + HEADS_INTERVAL_MS
    await _store_quota(env, ctx["repoBi"], stored_target, data, due, now)
    return stored_target


async def _read(env, request, owner, repo):
    account = await _authed_account_name(env, request)
    ctx = await _context(env, request, owner, repo, account)
    if ctx is None:
        return _error("not_found", 404)
    viewer = None
    if account:
        viewer = {"name": account,
                  "canSetTarget": await _can_set_target(env, account, owner, repo)}
    return json_response(
        await _payload(env, ctx, viewer),
        cache_control=PRIVATE_CACHE if account else PUBLIC_CACHE, extra_headers=VARY)


async def _body(request):
    try:
        data = await bounded_json_request(request)
    except Exception:
        return None
    return data if isinstance(data, dict) else None


async def _signed_in(env, request, data):
    account_bi, rec = await _account_session_record(env, request, data)
    account = clean_string((rec or {}).get("name"), MAX_NODE_NAME).strip().lower()
    if not rec or not account_bi or not account:
        return "", ""
    return account, account_bi


async def _set_target(env, request, owner, repo):
    data = await _body(request)
    if data is None:
        return _error("invalid_json", 400)
    account, _account_bi = await _signed_in(env, request, data)
    if not account:
        return _error("unauthorized", 401)
    target = clean_target(data.get("target"))
    if target is None:
        return _error("invalid_target", 400,
                      "The goal is a whole number from 0 to %d." % MAX_TARGET)
    ctx = await _context(env, request, owner, repo, account)
    if ctx is None:
        return _error("not_found", 404)
    if not await _can_set_target(env, account, owner, repo):
        return _error("not_authorized", 403)
    await _ensure_quota(env, ctx, _now(), target=target)
    viewer = {"name": account, "canSetTarget": True}
    return json_response(await _payload(env, ctx, viewer), cache_control=PRIVATE_CACHE)


async def _recent_adds(env, account_bi, since):
    row = await d1_first(
        env, "SELECT COUNT(*) AS n FROM repo_mirror_registry "
        "WHERE added_by_bi=? AND created_at>=?", account_bi, since)
    return int((row or {}).get("n") or 0)


async def _add(env, request, owner, repo):
    data = await _body(request)
    if data is None:
        return _error("invalid_json", 400)
    account, account_bi = await _signed_in(env, request, data)
    if not account:
        return _error("unauthorized", 401)
    parsed = parse_mirror_url(data.get("url"))
    if not parsed:
        return _error("invalid_url", 400,
                      "Enter the address people clone the mirror from: https://, "
                      "git:// or ssh, with no password in it.")
    ctx = await _context(env, request, owner, repo, account)
    if ctx is None:
        return _error("not_found", 404)
    if ctx["selfHost"] and parsed["host"].split(":", 1)[0] == ctx["selfHost"].split(":", 1)[0]:
        return _error("same_instance", 400,
                      "That address is this instance. Add a copy hosted somewhere else.")
    now = _now()
    quota = await _quota_row(env, ctx["repoBi"])
    source = ((quota or {}).get("data") or {}).get("source") or {}
    if source.get("id") == parsed["id"]:
        return _error("duplicate", 409, "That is the original repository.")
    existing = await d1_first(
        env, "SELECT 1 AS yes FROM repo_mirror_registry WHERE repo_bi=? AND mirror_id=?",
        ctx["repoBi"], parsed["id"])
    if existing:
        return _error("duplicate", 409, "That mirror is already listed.")
    count = await d1_first(
        env, "SELECT COUNT(*) AS n FROM repo_mirror_registry WHERE repo_bi=?",
        ctx["repoBi"])
    if int((count or {}).get("n") or 0) >= MAX_MIRRORS:
        return _error("too_many_mirrors", 409,
                      "A repository can list up to %d mirrors." % MAX_MIRRORS)
    if await _recent_adds(env, account_bi, now - MINUTE_MS) >= MAX_ADDS_PER_MINUTE \
            or await _recent_adds(env, account_bi, now - DAY_MS) >= MAX_ADDS_PER_DAY:
        return json_response(
            {"ok": False, "error": "rate_limited",
             "message": "You have added several mirrors recently. Try again later."},
            status=429, cache_control=PRIVATE_CACHE, extra_headers={"Retry-After": "60"})
    record = {"url": parsed["url"], "host": parsed["host"], "scheme": parsed["scheme"],
              "checkable": parsed["checkable"],
              "label": clean_label(data.get("label")) or parsed["host"],
              "kind": choose_kind(parsed["host"], parsed["scheme"], data.get("kind")),
              "addedBy": account}
    await d1_run(
        env,
        "INSERT INTO repo_mirror_registry (repo_bi, mirror_id, added_by_bi, created_at, "
        "next_check_at, data) VALUES (?,?,?,?,?,?) ON CONFLICT(repo_bi, mirror_id) "
        "DO NOTHING",
        ctx["repoBi"], parsed["id"], account_bi, now,
        0 if parsed["checkable"] else now + UNCHECKABLE_RETRY_MS,
        await encrypt_row(env, record))
    if quota is None:
        await _ensure_quota(env, ctx, now, target=0)
    viewer = {"name": account,
              "canSetTarget": await _can_set_target(env, account, owner, repo)}
    payload = await _payload(env, ctx, viewer)
    payload["added"] = parsed["id"]
    return json_response(payload, status=201, cache_control=PRIVATE_CACHE)


async def _remove(env, request, owner, repo):
    data = await _body(request) or {}
    mirror_id = str(data.get("id") or "")
    if not mirror_id:
        query = parse_qs(urlparse(str(request.url)).query)
        mirror_id = str((query.get("id") or [""])[0])
    account, account_bi = await _signed_in(env, request, data)
    if not account:
        return _error("unauthorized", 401)
    if not ID_RE.fullmatch(mirror_id):
        return _error("invalid_id", 400)
    ctx = await _context(env, request, owner, repo, account)
    if ctx is None:
        return _error("not_found", 404)
    row = await d1_first(
        env, "SELECT added_by_bi FROM repo_mirror_registry WHERE repo_bi=? AND mirror_id=?",
        ctx["repoBi"], mirror_id)
    if not row:
        return _error("not_found", 404)
    maintainer = await _can_set_target(env, account, owner, repo)
    if not maintainer and str(row.get("added_by_bi") or "") != account_bi:
        return _error("not_authorized", 403)
    await d1_run(
        env, "DELETE FROM repo_mirror_registry WHERE repo_bi=? AND mirror_id=?",
        ctx["repoBi"], mirror_id)
    viewer = {"name": account, "canSetTarget": maintainer}
    return json_response(await _payload(env, ctx, viewer), cache_control=PRIVATE_CACHE)


async def handler(env, request, owner, repo):
    await ensure_schema(env)
    method = method_name(request)
    if method in ("GET", "HEAD"):
        return await _read(env, request, owner, repo)
    if method == "PUT":
        return await _set_target(env, request, owner, repo)
    if method == "POST":
        return await _add(env, request, owner, repo)
    if method == "DELETE":
        return await _remove(env, request, owner, repo)
    return json_response({"ok": False, "error": "method_not_allowed"}, status=405,
                         cache_control=PRIVATE_CACHE,
                         extra_headers={"allow": "GET, PUT, POST, DELETE"})


async def _repo_state(env, repo_bi):
    row = await d1_first(
        env, "SELECT is_private, data FROM repositories WHERE key_bi=?", repo_bi)
    if not row:
        return None
    try:
        if int(row.get("is_private")) != 0:
            return None
    except (TypeError, ValueError):
        return None
    record = await _decrypted(env, row.get("data"))
    if not record or record.get("visibility") != "public":
        return None
    return {"owner": str(record.get("owner") or ""), "repo": str(record.get("name") or ""),
            "head": str(record.get("commit") or "").lower(),
            "branch": str(record.get("branch") or "").strip(),
            "upstream": str(record.get("upstreamCommit") or "").lower()}


async def read_forge_source(env, owner, repo, branch):
    base = str(_public_base_url(env) or "").rstrip("/")
    if not base or not owner or not repo:
        return None
    url = "%s/api/repo/%s/%s/raw?path=%s&ref=%s" % (
        base, quote(owner, safe=""), quote(repo, safe=""),
        quote(FORGE_SOURCE_PATH, safe=""), quote(branch or "HEAD", safe=""))
    try:
        response = await js_fetch_with_timeout(
            url, {"method": "GET", "headers": {"accept": "application/json"}},
            SELF_READ_TIMEOUT_SECONDS)
        status = int(getattr(response, "status", 0) or 0)
        if status in (400, 404):
            return False
        if status != 200:
            return None
        body = await bounded_body(response, MAX_SOURCE_BYTES)
        return (source_from_document(json.loads(body)) or False) if body else False
    except ValueError:
        return False
    except Exception:
        return None


def _jittered(now):
    return now + CHECK_INTERVAL_MS + random.randint(0, CHECK_JITTER_MS)


async def _refresh_quota(env, row, now, deadline, report):
    repo_bi = str(row.get("repo_bi") or "")
    data = await _decrypted(env, row.get("data")) or {}
    repo = await _repo_state(env, repo_bi)
    if repo is None:
        await d1_run(env, "UPDATE repo_mirror_quota SET next_check_at=? WHERE repo_bi=?",
                     now + HOUR_MS, repo_bi)
        return None
    data["heads"] = remember_head(data.get("heads"), repo["head"], now)
    if int(data.get("sourceReadAt") or 0) <= now - SOURCE_READ_INTERVAL_MS:
        source = await read_forge_source(env, repo["owner"] or data.get("owner"),
                                         repo["repo"] or data.get("repo"), repo["branch"])
        if source is None:
            data["sourceReadAt"] = now - SOURCE_READ_INTERVAL_MS + SOURCE_RETRY_MS
        else:
            data["sourceReadAt"] = now
            if source:
                if (data.get("source") or {}).get("id") != source["id"]:
                    data.pop("sourceCheck", None)
                    data.pop("sourceNextAt", None)
                data["source"] = source
            else:
                data.pop("source", None)
                data.pop("sourceCheck", None)
    source = data.get("source")
    if isinstance(source, dict) and source.get("url") \
            and int(data.get("sourceNextAt") or 0) <= now and _now() < deadline:
        result = await check_url(source["url"], "")
        data["sourceCheck"] = apply_check(data.get("sourceCheck"), result, repo["upstream"])
        data["sourceNextAt"] = _jittered(now)
        report["checked"] += 1
    await d1_run(
        env, "UPDATE repo_mirror_quota SET data=?, next_check_at=?, updated_at=? "
        "WHERE repo_bi=?", await encrypt_row(env, data), now + HEADS_INTERVAL_MS, now,
        repo_bi)
    return repo


async def run_checks(env, now=None):
    await ensure_schema(env)
    now = _now() if now is None else int(now)
    deadline = _now() + RUN_BUDGET_MS
    report = {"repos": 0, "checked": 0, "skipped": 0}
    repos = {}
    for row in await d1_all(
            env, "SELECT repo_bi, data FROM repo_mirror_quota WHERE next_check_at<=? "
            "ORDER BY next_check_at ASC LIMIT ?", now, QUOTA_ROWS_PER_RUN) or []:
        repos[str(row.get("repo_bi") or "")] = await _refresh_quota(
            env, row, now, deadline, report)
        report["repos"] += 1
    for row in await d1_all(
            env, "SELECT repo_bi, mirror_id, data FROM repo_mirror_registry "
            "WHERE next_check_at<=? ORDER BY next_check_at ASC LIMIT ?",
            now, CHECKS_PER_RUN) or []:
        if _now() >= deadline:
            report["skipped"] += 1
            continue
        repo_bi = str(row.get("repo_bi") or "")
        mirror_id = str(row.get("mirror_id") or "")
        if repo_bi not in repos:
            repos[repo_bi] = await _repo_state(env, repo_bi)
        repo = repos[repo_bi]
        data = await _decrypted(env, row.get("data"))
        parsed = parse_mirror_url((data or {}).get("url"))
        if repo is None or not parsed or not parsed["checkable"]:
            await d1_run(
                env, "UPDATE repo_mirror_registry SET next_check_at=? "
                "WHERE repo_bi=? AND mirror_id=?", now + UNCHECKABLE_RETRY_MS,
                repo_bi, mirror_id)
            continue
        result = await check_url(parsed["url"], repo["branch"])
        data["check"] = apply_check(data.get("check"), result)
        report["checked"] += 1
        await d1_run(
            env, "UPDATE repo_mirror_registry SET data=?, next_check_at=? "
            "WHERE repo_bi=? AND mirror_id=?", await encrypt_row(env, data),
            _jittered(now), repo_bi, mirror_id)
    return report
