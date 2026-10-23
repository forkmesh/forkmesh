"""D1 read guard: shared copies of heavy reads and an operator conserve switch.

The Free plan bills every row a statement scans (5M rows/day, account-wide).
Per-isolate memos do not bound that bill: a cold isolate arrives every few
seconds and each one re-runs its scans before any memo exists. Heavy readers
go through ``shared_read`` instead, which keeps one JSON copy per reader in the
colo Cache API so every isolate in a colo reuses a single scan for the reader's
TTL.

The guard's switch lives in the ``d1_read_guard`` row and is set from the
desktop D1 tab: ``auto`` conserves once the usage sampler reports today's rows
read at or above the threshold, ``on`` always conserves, ``off`` never does.
Conserving stretches every reader's TTL, and readers that only orient an
operator serve their fallback instead of scanning on a miss.
"""

from urllib.parse import quote


def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by this domain."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


READ_GUARD_MODES = ("auto", "on", "off")
READ_GUARD_DEFAULT_MODE = "auto"
READ_GUARD_DEFAULT_THRESHOLD = 60
# The usage_budget_state metric the sampler writes for D1 rows read.
READ_GUARD_METRIC = "d1-rows-read"
READ_GUARD_STATE_TTL_MS = 60 * 1000
# An isolate re-checks the colo copy this often, so a forget in another
# isolate reaches it quickly without a Cache API lookup per call.
READ_GUARD_LOCAL_TTL_MS = 15 * 1000
READ_GUARD_LOCAL_MAX = 64
READ_GUARD_CACHE_PREFIX = "https://forkmesh.internal/d1-read-guard/"

_MINUTE_MS = 60 * 1000
_HOUR_MS = 60 * _MINUTE_MS

# name -> (label, normal TTL ms, conserving TTL ms, provider query prefixes).
# The prefixes match the redacted statement text Cloudflare's query insights
# report, so the D1 tab can say which ranked statements the guard controls.
READ_GUARD_READERS = {
    "table-inventory": (
        "World infrastructure table row counts",
        6 * _HOUR_MS, 24 * _HOUR_MS,
        ('SELECT COUNT(*) AS row_count, \'',
         'SELECT COUNT(*) AS row_count FROM "')),
    "admin-table-counts": (
        "Admin console navigation row counts",
        30 * _MINUTE_MS, 24 * _HOUR_MS,
        ('SELECT ? AS t, COUNT(*) AS n FROM "',)),
    "admin-stats": (
        "Admin console header stats",
        5 * _MINUTE_MS, _HOUR_MS,
        ("SELECT COUNT(DISTINCT +run) AS n FROM install_diag",
         "SELECT COUNT(*) AS n FROM error_log WHERE ts >= ?",
         "SELECT COUNT(*) AS n FROM host_presence WHERE ts >= ?")),
    "users-directory": (
        "Users directory joined rows",
        5 * _MINUTE_MS, _HOUR_MS,
        ("SELECT u.user_bi,a.total_active_ms,a.total_steps,",)),
    "commit-totals": (
        "Directory lifetime commit totals",
        30 * _MINUTE_MS, 6 * _HOUR_MS,
        ("WITH ranked_sources AS ( SELECT projects.source_repo_bi, "
         "projects.active_generation_bi,",)),
    "contributor-board": (
        "Network contributor leaderboard",
        30 * _MINUTE_MS, 6 * _HOUR_MS,
        ("WITH resolved AS (SELECT c.issues,",)),
    "repositories-count": (
        "Repository count",
        10 * _MINUTE_MS, _HOUR_MS,
        ("SELECT COUNT(*) AS n FROM repositories",)),
}

_READ_GUARD_STATE = {"ts": 0, "state": None}
# cache key -> (checked_at, stored_ts, value)
_READ_GUARD_LOCAL = {}
_NO_FALLBACK = object()


def _utc_day(now_ms):
    parts = time.gmtime(int(now_ms) // 1000)
    return "%04d-%02d-%02d" % parts[:3]


def _clean_mode(value):
    mode = str(value or "").strip().lower()
    return mode if mode in READ_GUARD_MODES else ""


def _clean_threshold(value):
    try:
        threshold = int(value)
    except (TypeError, ValueError):
        return 0
    return threshold if 1 <= threshold <= 100 else 0


def _guard_state_from_row(row, now):
    row = row if isinstance(row, dict) else {}
    mode = _clean_mode(row.get("mode")) or READ_GUARD_DEFAULT_MODE
    threshold = (_clean_threshold(row.get("threshold"))
                 or READ_GUARD_DEFAULT_THRESHOLD)
    percent = None
    if str(row.get("day") or "") == _utc_day(now):
        try:
            percent = float(row.get("percent"))
        except (TypeError, ValueError):
            percent = None
    if mode == "on":
        conserving, reason = True, "forced on by an administrator"
    elif mode == "off":
        conserving, reason = False, "switched off by an administrator"
    elif percent is None:
        conserving, reason = False, "no rows-read sample for today yet"
    elif percent >= threshold:
        conserving = True
        reason = "today's rows read reached %.1f%% (threshold %d%%)" % (
            percent, threshold)
    else:
        conserving = False
        reason = "today's rows read are at %.1f%% (threshold %d%%)" % (
            percent, threshold)
    return {
        "mode": mode,
        "threshold": threshold,
        "conserving": conserving,
        "reason": reason,
        "rowsReadPercent": percent,
        "sampledAt": int(row.get("sampled_at") or 0),
        "updatedAt": int(row.get("updated_at") or 0),
        "updatedBy": str(row.get("updated_by") or ""),
    }


async def read_guard_state(env, now=None):
    """The switch plus today's sample, memoized per isolate for a minute."""
    now = int(Date.now()) if now is None else int(now)
    memo = _READ_GUARD_STATE
    if (memo["state"] is not None
            and 0 <= now - int(memo["ts"]) < READ_GUARD_STATE_TTL_MS):
        return memo["state"]
    try:
        row = await d1_first(
            env,
            "SELECT g.mode, g.threshold, g.updated_at, g.updated_by, "
            "s.day, s.percent, s.sampled_at FROM (SELECT 1) AS one "
            "LEFT JOIN d1_read_guard AS g ON g.id=1 "
            "LEFT JOIN usage_budget_state AS s ON s.metric=?",
            READ_GUARD_METRIC)
    except Exception:
        # A database behind the migration conserves nothing, as before.
        row = None
    state = _guard_state_from_row(row, now)
    memo.update({"ts": now, "state": state})
    return state


def _cache_key(name, variant):
    key = READ_GUARD_CACHE_PREFIX + name
    return key + "/" + quote(str(variant), safe="") if variant else key


async def _cached_payload(key):
    cached = await edge_cache_match(key)
    if cached is None:
        return None
    try:
        payload = await _response_json(cached)
    except Exception:
        return None
    if not isinstance(payload, dict) or "value" not in payload:
        return None
    return payload


async def shared_read(env, name, loader, variant="", fallback=_NO_FALLBACK,
                      now=None):
    """Serve ``loader()``'s JSON result from one colo-wide copy while fresh.

    ``variant`` separates copies of one reader (for example a catalog rev).
    While the guard conserves, a reader given ``fallback`` returns it on a miss
    instead of scanning.
    """
    _, normal_ms, conserve_ms, _ = READ_GUARD_READERS[name]
    now = int(Date.now()) if now is None else int(now)
    state = await read_guard_state(env, now)
    ttl = conserve_ms if state["conserving"] else normal_ms
    key = _cache_key(name, variant)
    local = _READ_GUARD_LOCAL.get(key)
    process_cache = getattr(env, "HOST_BACKGROUND", None) is not None
    if (local is not None
            and (process_cache
                 or 0 <= now - local[0] < READ_GUARD_LOCAL_TTL_MS)
            and 0 <= now - local[1] < ttl):
        return local[2]
    payload = await _cached_payload(key)
    if payload is not None:
        try:
            stored_ts = int(payload.get("ts") or 0)
        except (TypeError, ValueError):
            stored_ts = -1
        if 0 <= now - stored_ts < ttl:
            _remember_local(key, now, stored_ts, payload["value"])
            return payload["value"]
    if state["conserving"] and fallback is not _NO_FALLBACK:
        return fallback

    async def load():
        return await _shared_load(key, loader, normal_ms, conserve_ms)

    flight = "shared-read:" + key
    if fallback is not _NO_FALLBACK and _host_background(env, flight, load):
        return payload["value"] if payload is not None else fallback
    join = _host_single_flight(env)
    return await (join(flight, load) if join is not None else load())


async def _shared_load(key, loader, normal_ms, conserve_ms):
    value = await loader()
    now = int(Date.now())
    await edge_cache_put(key, json_response(
        {"ts": now, "value": value},
        cache_control="public, max-age=%d" % (
            max(normal_ms, conserve_ms) // 1000)))
    _remember_local(key, now, now, value)
    return value


def _remember_local(key, checked_at, stored_ts, value):
    if len(_READ_GUARD_LOCAL) >= READ_GUARD_LOCAL_MAX:
        _READ_GUARD_LOCAL.clear()
    _READ_GUARD_LOCAL[key] = (checked_at, stored_ts, value)


async def shared_forget(name, variant=""):
    """A write changed what ``name`` reads: drop this colo's shared copy."""
    key = _cache_key(name, variant)
    _READ_GUARD_LOCAL.pop(key, None)
    await edge_cache_delete(key)


def reader_for_query(query_text):
    """Name the guarded reader behind a provider-reported statement, or ''."""
    text = str(query_text or "")
    for name, (_, _, _, prefixes) in READ_GUARD_READERS.items():
        if any(text.startswith(prefix) for prefix in prefixes):
            return name
    return ""


async def read_guard_report(env, now=None):
    """The D1 tab's view of the switch and of every guarded reader."""
    state = dict(await read_guard_state(env, now))
    state["modes"] = list(READ_GUARD_MODES)
    state["readers"] = [
        {
            "name": name,
            "label": label,
            "normalTtlMs": normal_ms,
            "conserveTtlMs": conserve_ms,
            "activeTtlMs": conserve_ms if state["conserving"] else normal_ms,
        }
        for name, (label, normal_ms, conserve_ms, _)
        in READ_GUARD_READERS.items()
    ]
    return state


async def admin_d1_read_guard(env, request):
    """Admin-signed POST that sets the guard's mode and auto threshold."""
    if method_name(request) != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    node = clean_string(data.get("node", ""), MAX_NODE_NAME).lower()
    ts = clean_string(data.get("ts", ""), 20)
    sig = clean_string(data.get("sig", ""), 200)
    mode = _clean_mode(data.get("mode"))
    threshold = _clean_threshold(data.get("threshold"))
    canonical = (
        "forkmesh-admin-d1-read-guard-v1\n" + node + "\n" + ts + "\n"
        + str(data.get("mode", "")) + "\n" + str(data.get("threshold", ""))
    ).encode()
    if not await _admin_authorized(env, node, ts, sig, canonical):
        return json_response({"error": "unauthorized"}, status=401)
    if not mode or not threshold:
        return json_response({"error": "invalid_guard"}, status=400)
    await ensure_schema(env)
    now = int(Date.now())
    await d1_run(
        env,
        "INSERT INTO d1_read_guard (id, mode, threshold, updated_at, "
        "updated_by) VALUES (1, ?, ?, ?, ?) ON CONFLICT(id) DO UPDATE SET "
        "mode=excluded.mode, threshold=excluded.threshold, "
        "updated_at=excluded.updated_at, updated_by=excluded.updated_by",
        mode, threshold, now, node)
    _READ_GUARD_STATE.update({"ts": 0, "state": None})
    try:
        await _audit_sensitive_action(
            env, node, "admin.d1_read_guard", "d1_read_guard", mode,
            "success", {"threshold": threshold})
    except Exception:
        pass
    return json_response(
        {"ok": True, "readGuard": await read_guard_report(env, now)},
        cache_control="no-store, max-age=0, must-revalidate")
