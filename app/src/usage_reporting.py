"""Cloudflare usage reporting and alerts, loaded on demand.

Split out of entry.py so Cloudflare startup validation does not pay for the
analytics/reporting domain on every isolate. Admin usage routes and the
periodic budget sampler load it only when needed.
"""


def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by this domain."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def _usage_budget_utc(now_ms):
    """(YYYY-MM-DD, ISO day start, ISO now) without importing datetime."""
    parts = time.gmtime(int(now_ms) // 1000)
    day = "%04d-%02d-%02d" % parts[:3]
    return (
        day,
        day + "T00:00:00Z",
        "%04d-%02d-%02dT%02d:%02d:%02dZ" % parts[:6],
    )
def _usage_budget_amount(value, unit):
    if unit == "GB-s":
        return "{:,.0f}".format(float(value))
    return "{:,}".format(int(value))
async def _cloudflare_usage_today(account_id, token, now):
    """Account-wide usage since 00:00 UTC, in free-allowance units."""
    day, start, end = _usage_budget_utc(now)
    query = (
        "query($acct:String!,$day:Date!,$start:Time!,$end:Time!){viewer{"
        "accounts(filter:{accountTag:$acct}){"
        "duration: durableObjectsPeriodicGroups(limit:1,"
        "filter:{datetime_geq:$start,datetime_leq:$end})"
        "{sum{activeTime}}"
        "doRequests: durableObjectsInvocationsAdaptiveGroups(limit:1,"
        "filter:{datetime_geq:$start,datetime_leq:$end})"
        "{sum{requests}}"
        "workerRequests: workersInvocationsAdaptive(limit:1,"
        "filter:{datetime_geq:$start,datetime_leq:$end})"
        "{sum{requests}}"
        "d1Rows: d1AnalyticsAdaptiveGroups(limit:1,"
        "filter:{date_geq:$day,date_leq:$day})"
        "{sum{rowsRead}}"
        "}}}"
    )
    try:
        response = await js_fetch_with_timeout(
            USAGE_BUDGET_GRAPHQL_URL,
            {
                "method": "POST",
                "headers": {
                    "authorization": "Bearer " + token,
                    "content-type": "application/json",
                },
                "body": json.dumps({
                    "query": query,
                    "variables": {
                        "acct": account_id, "day": day,
                        "start": start, "end": end},
                }, separators=(",", ":")),
            },
            CRON_OUTBOUND_FETCH_TIMEOUT_SECONDS,
        )
        if int(getattr(response, "status", 0) or 0) != 200:
            return None
        payload = json.loads(str(await response.text()))
        account = payload["data"]["viewer"]["accounts"][0]
    except BaseException:
        return None

    def first_sum(groups, field):
        try:
            return float(groups[0]["sum"][field])
        except (LookupError, TypeError, ValueError):
            return 0.0

    # activeTime arrives in microseconds; Cloudflare bills a Durable Object
    # at 128 MB, so GB-s = seconds * 0.128 (decimal GB, matching its meter).
    return {
        "do-duration": first_sum(
            account.get("duration") or (), "activeTime") / 1e6 * 0.128,
        "do-requests": first_sum(account.get("doRequests") or (), "requests"),
        "worker-requests": first_sum(
            account.get("workerRequests") or (), "requests"),
        # Feeds the D1 read guard's auto mode as well as the Ping ladder.
        "d1-rows-read": first_sum(account.get("d1Rows") or (), "rowsRead"),
    }
def _safe_d1_query_text(value):
    """Compact provider-captured SQL while removing comments and literals."""
    text = str(value or "")
    text = re.sub(r"\b[xX]'(?:''|[^'])*'", "?", text)
    text = re.sub(r"'(?:''|[^'])*'", "?", text)
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"--[^\r\n]*", " ", text)
    text = re.sub(r"\b\d+(?:\.\d+)?\b", "?", text)
    text = " ".join(text.split())
    return text if len(text) <= 360 else text[:359].rstrip() + "…"
async def _cloudflare_d1_usage_today(account_id, token, now):
    """Account-wide and per-database D1 billing metrics for this UTC day."""
    day, start, end = _usage_budget_utc(now)
    # D1's analytics dataset uses Date filters (not the Time filters used by
    # Workers and Durable Objects). The ungrouped alias owns the allowance
    # totals; the second alias adds attribution without making totals depend on
    # the bounded database list.
    query = (
        "query($acct:String!,$day:Date!,$start:Time!,$end:Time!){viewer{"
        "accounts(filter:{accountTag:$acct}){"
        "totals: d1AnalyticsAdaptiveGroups(limit:1,"
        "filter:{date_geq:$day,date_leq:$day})"
        "{sum{readQueries writeQueries rowsRead rowsWritten}}"
        "databases: d1AnalyticsAdaptiveGroups(limit:%d,"
        "filter:{date_geq:$day,date_leq:$day})"
        "{dimensions{databaseId}"
        "sum{readQueries writeQueries rowsRead rowsWritten}}"
        "hours: d1AnalyticsAdaptiveGroups(limit:24,"
        "filter:{datetimeHour_geq:$start,datetimeHour_leq:$end},"
        "orderBy:[datetimeHour_ASC])"
        "{dimensions{datetimeHour}sum{rowsRead rowsWritten}}"
        "queries: d1QueriesAdaptiveGroups(limit:%d,"
        "filter:{datetimeHour_geq:$start,datetimeHour_leq:$end},"
        "orderBy:[sum_rowsRead_DESC])"
        "{dimensions{query}count "
        "sum{queryDurationMs rowsRead rowsWritten rowsReturned}"
        "avg{queryDurationMs rowsRead rowsWritten rowsReturned}}"
        "}}}" % (D1_USAGE_DETAIL_GROUP_LIMIT, D1_QUERY_INSIGHT_LIMIT)
    )
    try:
        response = await js_fetch_with_timeout(
            USAGE_BUDGET_GRAPHQL_URL,
            {
                "method": "POST",
                "headers": {
                    "authorization": "Bearer " + token,
                    "content-type": "application/json",
                },
                "body": json.dumps({
                    "query": query,
                    "variables": {
                        "acct": account_id, "day": day,
                        "start": start, "end": end,
                    },
                }, separators=(",", ":")),
            },
            CRON_OUTBOUND_FETCH_TIMEOUT_SECONDS,
        )
        if int(getattr(response, "status", 0) or 0) != 200:
            return None
        payload = json.loads(str(await response.text()))
        account = payload["data"]["viewer"]["accounts"][0]
    except BaseException:
        return None

    fields = (
        "readQueries", "writeQueries", "rowsRead", "rowsWritten",
    )

    def sums(group):
        raw = (group or {}).get("sum") or {}
        cleaned = {}
        for field in fields:
            try:
                cleaned[field] = max(0.0, float(raw.get(field) or 0))
            except (AttributeError, TypeError, ValueError):
                cleaned[field] = 0.0
        return cleaned

    def add_sums(groups):
        result = {field: 0.0 for field in fields}
        for group in groups or ():
            current = sums(group)
            for field in fields:
                result[field] += current[field]
        return result

    # With no dimensions the provider normally returns one account aggregate.
    # Sum defensively instead of trusting only the first row if that response
    # shape changes during an analytics rollout.
    totals = add_sums(account.get("totals") or ())
    databases = []
    database_groups = account.get("databases") or ()
    for group in database_groups:
        try:
            database_id = str(
                (group.get("dimensions") or {}).get("databaseId") or ""
            ).strip()
        except AttributeError:
            continue
        if not database_id:
            continue
        databases.append({"databaseId": database_id, **sums(group)})
    databases.sort(
        key=lambda row: (
            -row["rowsRead"], -row["rowsWritten"], row["databaseId"]
        )
    )
    database_totals = add_sums(database_groups)
    for field in fields:
        totals[field] = max(totals[field], database_totals[field])

    timeline = []
    for group in account.get("hours") or ():
        try:
            at = str(
                (group.get("dimensions") or {}).get("datetimeHour") or ""
            ).strip()
        except AttributeError:
            continue
        if at:
            timeline.append({"at": at, **sums(group)})
    timeline.sort(key=lambda row: row["at"])

    insights = []
    for group in account.get("queries") or ():
        try:
            query_text = _safe_d1_query_text(
                (group.get("dimensions") or {}).get("query"))
            summed = group.get("sum") or {}
            averaged = group.get("avg") or {}
            runs = max(0, int(group.get("count") or 0))
            total_read = max(0, int(float(summed.get("rowsRead") or 0)))
            avg_read = max(0.0, float(averaged.get("rowsRead") or 0))
            avg_returned = max(
                0.0, float(averaged.get("rowsReturned") or 0))
            if not query_text:
                continue
            insights.append({
                "query": query_text,
                "runs": runs,
                "totalRowsRead": total_read,
                "avgRowsRead": round(avg_read, 2),
                "totalRowsWritten": max(
                    0, int(float(summed.get("rowsWritten") or 0))),
                "avgRowsWritten": round(max(
                    0.0, float(averaged.get("rowsWritten") or 0)), 2),
                "avgDurationMs": round(max(
                    0.0, float(averaged.get("queryDurationMs") or 0)), 2),
                "totalDurationMs": round(max(
                    0.0, float(summed.get("queryDurationMs") or 0)), 2),
                "efficiency": round(
                    avg_returned / avg_read if avg_read else 0.0, 4),
            })
        except (AttributeError, TypeError, ValueError):
            continue
    insights.sort(key=lambda row: (-row["totalRowsRead"], -row["runs"],
                                   row["query"]))
    return {
        "totals": totals,
        "databases": databases,
        "timeline": timeline,
        "queryInsights": insights,
        "queryInsightLimit": D1_QUERY_INSIGHT_LIMIT,
        "accountWide": True,
        "analyticsDelayed": True,
        "detailLimit": D1_USAGE_DETAIL_GROUP_LIMIT,
        "detailTruncated": len(database_groups) >= D1_USAGE_DETAIL_GROUP_LIMIT,
    }
def _d1_usage_report_payload(detail, storage, now):
    """Build the desktop D1 tab's account meters and database attribution."""
    day, start, end = _usage_budget_utc(now)
    totals = (detail or {}).get("totals") or {}
    metrics = (
        ("rows-read", "Rows read", D1_FREE_ROWS_READ_PER_DAY, "rows",
         "rowsRead"),
        ("rows-written", "Rows written", D1_FREE_ROWS_WRITTEN_PER_DAY,
         "rows", "rowsWritten"),
    )
    budgets = []
    for metric, label, allowance, unit, field in metrics:
        used = max(0.0, float(totals.get(field) or 0))
        budgets.append({
            "metric": metric,
            "label": label,
            "unit": unit,
            "allowance": allowance,
            "used": round(used, 2),
            "percent": round(used / allowance * 100.0, 2),
        })
    databases = []
    for raw in (detail or {}).get("databases") or ():
        database_id = str(raw.get("databaseId") or "").strip()
        if not database_id:
            continue
        databases.append({
            "databaseId": database_id,
            "rowsRead": int(raw.get("rowsRead") or 0),
            "rowsWritten": int(raw.get("rowsWritten") or 0),
            "readQueries": int(raw.get("readQueries") or 0),
            "writeQueries": int(raw.get("writeQueries") or 0),
        })
    timeline = []
    for raw in (detail or {}).get("timeline") or ():
        at = str(raw.get("at") or "").strip()
        if not at:
            continue
        timeline.append({
            "at": at,
            "rows-read": max(0, int(raw.get("rowsRead") or 0)),
            "rows-written": max(0, int(raw.get("rowsWritten") or 0)),
        })
    return {
        "ok": True,
        "day": day,
        "windowStart": start,
        "windowEnd": end,
        "budgets": budgets,
        "databases": databases,
        "timeline": timeline,
        "queryInsights": list(
            (detail or {}).get("queryInsights") or ()),
        "queryInsightLimit": int(
            (detail or {}).get("queryInsightLimit")
            or D1_QUERY_INSIGHT_LIMIT),
        "accountWide": bool((detail or {}).get("accountWide", True)),
        "analyticsDelayed": bool(
            (detail or {}).get("analyticsDelayed", True)),
        "detailTruncated": bool(
            (detail or {}).get("detailTruncated")),
        "storage": dict(storage or {}),
    }
async def _usage_budget_ping_admins(env, title, body, dedupe, now):
    """One deduplicated usage Ping per platform admin; count deliveries."""
    rows = await d1_all(
        env, "SELECT data FROM users WHERE is_admin=1 "
        "ORDER BY user_bi LIMIT ?", STATUS_ALERT_ADMIN_BATCH)
    delivered = 0
    for row in rows or []:
        try:
            record = await decrypt_row(env, row.get("data", "")) or {}
        except Exception:
            continue
        admin = clean_string(
            record.get("name", ""), MAX_NODE_NAME).strip().lower()
        if not valid_node_name(admin):
            continue
        try:
            if await enqueue_notification(
                    env, admin, "operational_alert", title, body=body,
                    href="/status", source="usage-budget", dedupe=dedupe,
                    ts=now):
                delivered += 1
        except BaseException:
            continue
    return delivered
async def record_usage_budget_sample(env):
    """Ping admins once per threshold per UTC day as free budgets fill."""
    account_id = clean_string(
        getattr(env, "WORKERS_OBSERVABILITY_ACCOUNT_ID", ""), 64).strip()
    token = clean_string(
        getattr(env, "USAGE_ANALYTICS_API_TOKEN", ""), 512).strip()
    if not account_id or not token:
        return
    now = int(Date.now())
    usage = await _cloudflare_usage_today(account_id, token, now)
    if usage is None:
        return
    await ensure_schema(env)
    day, _, _ = _usage_budget_utc(now)
    pings_sent = 0
    metrics = USAGE_BUDGET_METRICS + ((
        "d1-rows-read", "D1 rows read", D1_FREE_ROWS_READ_PER_DAY, "rows"),)
    for metric, label, allowance, unit in metrics:
        used = usage.get(metric)
        if used is None:
            continue
        percent = float(used) / allowance * 100.0
        crossed = 0
        for threshold in USAGE_BUDGET_THRESHOLDS:
            # A tenth of the D1 read allowance is an ordinary morning, not
            # news; its ladder starts at half.
            if metric == "d1-rows-read" and threshold < 50:
                continue
            if percent >= threshold:
                crossed = threshold
        row = await d1_first(
            env,
            "SELECT day, pinged_threshold FROM usage_budget_state "
            "WHERE metric=?",
            metric,
        )
        prior = (
            int(row.get("pinged_threshold") or 0)
            if row and str(row.get("day") or "") == day else 0)
        delivered_threshold = prior
        if crossed > prior and pings_sent < USAGE_BUDGET_PING_BATCH:
            # Only the highest crossed threshold pings: jumping straight from
            # 9% to 85% between samples is one "over 80%" alert, not three.
            title = "Cloudflare usage over %d%%" % crossed
            body = (
                "%s is at %d%% of today's free allowance (%s of %s %s "
                "used since 00:00 UTC). The daily budget resets at "
                "00:00 UTC; at 100%% these operations fail for the rest "
                "of the day." % (
                    label, int(percent),
                    _usage_budget_amount(used, unit),
                    _usage_budget_amount(allowance, unit), unit))
            delivered = await _usage_budget_ping_admins(
                env, title, body,
                "usage-budget:%s:%d:%s" % (metric, crossed, day), now)
            pings_sent += delivered
            if delivered:
                delivered_threshold = crossed
        await d1_run(
            env,
            """INSERT INTO usage_budget_state
                 (metric, day, percent, pinged_threshold, sampled_at)
               VALUES (?,?,?,?,?)
               ON CONFLICT(metric) DO UPDATE SET
                 day=excluded.day, percent=excluded.percent,
                 pinged_threshold=excluded.pinged_threshold,
                 sampled_at=excluded.sampled_at""",
            metric, day, round(percent, 1), delivered_threshold, now,
        )
async def _cloudflare_usage_detail_today(account_id, token, now):
    """Per-namespace and per-Worker usage since 00:00 UTC, raw group rows.

    Dimension support differs per dataset (durableObjectsPeriodicGroups has
    no scriptName — verified against the live API), so duration/storage
    groups by namespaceId alone while the invocations dataset supplies the
    namespaceId→scriptName association.
    """
    _, start, end = _usage_budget_utc(now)
    query = (
        "query($acct:String!,$start:Time!,$end:Time!){viewer{"
        "accounts(filter:{accountTag:$acct}){"
        "periodic: durableObjectsPeriodicGroups(limit:%d,"
        "filter:{datetime_geq:$start,datetime_leq:$end})"
        "{dimensions{namespaceId}"
        "sum{activeTime cpuTime storageReadUnits storageWriteUnits "
        "storageDeletes subrequests inboundWebsocketMsgCount "
        "outboundWebsocketMsgCount}}"
        "invocations: durableObjectsInvocationsAdaptiveGroups(limit:%d,"
        "filter:{datetime_geq:$start,datetime_leq:$end})"
        "{dimensions{scriptName namespaceId}"
        "sum{requests errors wallTime responseBodySize}}"
        "workers: workersInvocationsAdaptive(limit:%d,"
        "filter:{datetime_geq:$start,datetime_leq:$end})"
        "{dimensions{scriptName}"
        "sum{requests errors subrequests duration}}"
        "durationHours: durableObjectsPeriodicGroups(limit:24,"
        "filter:{datetime_geq:$start,datetime_leq:$end},"
        "orderBy:[datetimeHour_ASC])"
        "{dimensions{datetimeHour}sum{activeTime}}"
        "doRequestHours: durableObjectsInvocationsAdaptiveGroups(limit:24,"
        "filter:{datetime_geq:$start,datetime_leq:$end},"
        "orderBy:[datetimeHour_ASC])"
        "{dimensions{datetimeHour}sum{requests}}"
        "workerRequestHours: workersInvocationsAdaptive(limit:24,"
        "filter:{datetime_geq:$start,datetime_leq:$end},"
        "orderBy:[datetimeHour_ASC])"
        "{dimensions{datetimeHour}sum{requests}}"
        "}}}" % ((USAGE_DETAIL_GROUP_LIMIT,) * 3)
    )
    try:
        response = await js_fetch_with_timeout(
            USAGE_BUDGET_GRAPHQL_URL,
            {
                "method": "POST",
                "headers": {
                    "authorization": "Bearer " + token,
                    "content-type": "application/json",
                },
                "body": json.dumps({
                    "query": query,
                    "variables": {
                        "acct": account_id, "start": start, "end": end},
                }, separators=(",", ":")),
            },
            CRON_OUTBOUND_FETCH_TIMEOUT_SECONDS,
        )
        if int(getattr(response, "status", 0) or 0) != 200:
            return None
        payload = json.loads(str(await response.text()))
        account = payload["data"]["viewer"]["accounts"][0]
    except BaseException:
        return None

    def rows(alias):
        cleaned = []
        for group in account.get(alias) or ():
            try:
                cleaned.append({
                    "dimensions": {
                        str(key): str(value or "")
                        for key, value in dict(
                            group.get("dimensions") or {}).items()},
                    "sum": {
                        str(key): float(value or 0)
                        for key, value in dict(
                            group.get("sum") or {}).items()},
                })
            except (AttributeError, TypeError, ValueError):
                continue
        return cleaned

    return {
        "periodic": rows("periodic"),
        "invocations": rows("invocations"),
        "workers": rows("workers"),
        "durationHours": rows("durationHours"),
        "doRequestHours": rows("doRequestHours"),
        "workerRequestHours": rows("workerRequestHours"),
    }
def _usage_report_payload(detail, now):
    """Merge raw analytics groups into the admin usage-report body.

    One row per Durable Object namespace — duration/storage/websockets from
    the periodic dataset joined with requests/errors/wall time from the
    invocations dataset — plus one row per Worker script, each sorted by
    what the free plan meters (GB-s and requests). Budget totals are summed
    from the same groups so the meters and the breakdown always agree.
    """
    day, start, end = _usage_budget_utc(now)
    by_namespace = {}

    def namespace_row(namespace_id):
        return by_namespace.setdefault(namespace_id, {
            "namespaceId": namespace_id,
            "label": USAGE_DO_NAMESPACE_LABELS.get(namespace_id, ""),
            "script": "",
            "gbSeconds": 0.0, "cpuSeconds": 0.0,
            "requests": 0, "errors": 0,
            "wallTimeMs": 0.0, "responseBytes": 0,
            "storageReads": 0, "storageWrites": 0, "storageDeletes": 0,
            "subrequests": 0, "websocketIn": 0, "websocketOut": 0,
        })

    for group in detail.get("periodic") or ():
        namespace_id = (group.get("dimensions") or {}).get("namespaceId", "")
        if not namespace_id:
            continue
        sums = group.get("sum") or {}
        row = namespace_row(namespace_id)
        # activeTime/cpuTime arrive in microseconds; Cloudflare bills a
        # Durable Object at 128 MB, so GB-s = seconds * 0.128 (decimal GB,
        # matching its meter and record_usage_budget_sample).
        row["gbSeconds"] += sums.get("activeTime", 0.0) / 1e6 * 0.128
        row["cpuSeconds"] += sums.get("cpuTime", 0.0) / 1e6
        row["storageReads"] += int(sums.get("storageReadUnits", 0))
        row["storageWrites"] += int(sums.get("storageWriteUnits", 0))
        row["storageDeletes"] += int(sums.get("storageDeletes", 0))
        row["subrequests"] += int(sums.get("subrequests", 0))
        row["websocketIn"] += int(sums.get("inboundWebsocketMsgCount", 0))
        row["websocketOut"] += int(sums.get("outboundWebsocketMsgCount", 0))
    for group in detail.get("invocations") or ():
        dimensions = group.get("dimensions") or {}
        namespace_id = dimensions.get("namespaceId", "")
        if not namespace_id:
            continue
        sums = group.get("sum") or {}
        row = namespace_row(namespace_id)
        row["script"] = row["script"] or dimensions.get("scriptName", "")
        row["requests"] += int(sums.get("requests", 0))
        row["errors"] += int(sums.get("errors", 0))
        row["wallTimeMs"] += sums.get("wallTime", 0.0) / 1e3
        row["responseBytes"] += int(sums.get("responseBodySize", 0))
    durable = sorted(
        by_namespace.values(),
        key=lambda row: (-row["gbSeconds"], -row["requests"],
                         row["namespaceId"]))
    for row in durable:
        row["gbSeconds"] = round(row["gbSeconds"], 2)
        row["cpuSeconds"] = round(row["cpuSeconds"], 2)
        row["wallTimeMs"] = round(row["wallTimeMs"], 1)

    workers = []
    for group in detail.get("workers") or ():
        script = (group.get("dimensions") or {}).get("scriptName", "")
        if not script:
            continue
        sums = group.get("sum") or {}
        workers.append({
            "script": script,
            "requests": int(sums.get("requests", 0)),
            "errors": int(sums.get("errors", 0)),
            "subrequests": int(sums.get("subrequests", 0)),
            "gbSeconds": round(sums.get("duration", 0.0), 2),
        })
    workers.sort(key=lambda row: (-row["requests"], row["script"]))

    totals = {
        "do-duration": sum(row["gbSeconds"] for row in durable),
        "do-requests": float(sum(row["requests"] for row in durable)),
        "worker-requests": float(sum(row["requests"] for row in workers)),
    }
    budgets = []
    for metric, label, allowance, unit in USAGE_BUDGET_METRICS:
        used = totals.get(metric, 0.0)
        budgets.append({
            "metric": metric, "label": label, "unit": unit,
            "allowance": allowance, "used": round(used, 2),
            "percent": round(used / allowance * 100.0, 2),
        })
    timeline_by_hour = {}

    def add_timeline(groups, metric, field, scale=1.0):
        for group in groups or ():
            at = (group.get("dimensions") or {}).get("datetimeHour", "")
            if not at:
                continue
            point = timeline_by_hour.setdefault(at, {"at": at})
            point[metric] = round(
                point.get(metric, 0.0)
                + float((group.get("sum") or {}).get(field, 0.0)) * scale,
                2,
            )

    add_timeline(
        detail.get("durationHours"), "do-duration", "activeTime",
        0.128 / 1e6)
    add_timeline(
        detail.get("doRequestHours"), "do-requests", "requests")
    add_timeline(
        detail.get("workerRequestHours"), "worker-requests", "requests")
    timeline = sorted(timeline_by_hour.values(), key=lambda row: row["at"])
    # Stable, network-aggregate quantities that can be reconciled against a
    # provider invoice by tools/unit_economics.py. Costs deliberately do not
    # appear here: analytics unavailability or an absent invoice must never be
    # represented as a zero-cost period.
    unit_economics = {
        "schema": "forkmesh-hosted-usage-v1",
        "scope": "network-aggregate",
        "source": "cloudflare-account-analytics",
        "costStatus": "provider-invoice-reconciliation-required",
        "measurements": [
            {
                "unit": "worker-request",
                "quantity": str(int(totals["worker-requests"])),
            },
            {
                "unit": "durable-object-request",
                "quantity": str(int(totals["do-requests"])),
            },
            {
                "unit": "durable-object-gb-second",
                "quantity": str(round(totals["do-duration"], 2)),
            },
        ],
        "providerDiagnostics": {
            "durableObjectStorageReadOperations": sum(
                row["storageReads"] for row in durable),
            "durableObjectStorageWriteOperations": sum(
                row["storageWrites"] for row in durable),
            "durableObjectStorageDeleteOperations": sum(
                row["storageDeletes"] for row in durable),
            "durableObjectResponseBytes": sum(
                row["responseBytes"] for row in durable),
        },
    }
    return {
        "ok": True, "day": day, "windowStart": start, "windowEnd": end,
        "budgets": budgets, "timeline": timeline,
        "durableObjects": durable, "workers": workers,
        "unitEconomics": unit_economics,
    }
async def _admin_usage_report(env, request):
    # Admin-signed, read-only view of the same Cloudflare analytics the
    # usage-budget sampler watches, broken down per Durable Object namespace
    # and per Worker so the desktop log area can show what is spending the
    # free-plan allowances rather than only how full they are.
    params = parse_qs(urlparse(request.url).query)
    node = clean_string(params.get("node", [""])[0], MAX_NODE_NAME).lower()
    ts = clean_string(params.get("ts", [""])[0], 20)
    sig = clean_string(params.get("sig", [""])[0], 200)
    canonical = (
        "forkmesh-admin-usage-report-v1\n" + node + "\n" + ts).encode()
    if not await _admin_authorized(env, node, ts, sig, canonical):
        return json_response({"error": "unauthorized"}, status=401)
    account_id = clean_string(
        getattr(env, "WORKERS_OBSERVABILITY_ACCOUNT_ID", ""), 64).strip()
    token = clean_string(
        getattr(env, "USAGE_ANALYTICS_API_TOKEN", ""), 512).strip()
    if not account_id or not token:
        return json_response(
            {"ok": False, "configured": False,
             "error": "analytics_unconfigured"},
            cache_control="no-store, max-age=0, must-revalidate")
    now = int(Date.now())
    detail = await _cloudflare_usage_detail_today(account_id, token, now)
    if detail is None:
        return json_response(
            {"ok": False, "error": "analytics_unavailable"}, status=502)
    payload = _usage_report_payload(detail, now)
    payload["sampledAt"] = now

    # The sampler's alert ladder, so the meters can say "alerted at 80%".
    alerted = {}
    try:
        for row in await d1_all(
                env, "SELECT metric, day, pinged_threshold, sampled_at "
                "FROM usage_budget_state") or []:
            if str(row.get("day") or "") == payload["day"]:
                alerted[str(row.get("metric") or "")] = {
                    "pingedThreshold": int(row.get("pinged_threshold") or 0),
                    "sampledAt": int(row.get("sampled_at") or 0),
                }
    except Exception:
        alerted = {}
    for budget in payload["budgets"]:
        state = alerted.get(budget["metric"]) or {}
        budget["pingedThreshold"] = state.get("pingedThreshold", 0)
        budget["cronSampledAt"] = state.get("sampledAt", 0)

    # Mid-request platform aborts (durable_object_abort_minute buckets are
    # STATUS_SAMPLE_WINDOW_MS-aligned epoch ms): the free-tier kill signal
    # that pure duration analytics cannot show.
    aborts = {"aborts24h": 0, "durationAborts24h": 0,
              "aborts60m": 0, "durationAborts60m": 0}
    try:
        hour_ago = now - 60 * 60 * 1000
        row = await d1_first(
            env,
            "SELECT SUM(aborts) AS aborts24h, "
            "SUM(duration_aborts) AS duration24h, "
            "SUM(CASE WHEN minute_ts>=? THEN aborts ELSE 0 END) AS aborts60m, "
            "SUM(CASE WHEN minute_ts>=? THEN duration_aborts ELSE 0 END) "
            "AS duration60m "
            "FROM durable_object_abort_minute WHERE minute_ts>=?",
            hour_ago, hour_ago, now - 24 * 60 * 60 * 1000)
        if row:
            aborts = {
                "aborts24h": int(row.get("aborts24h") or 0),
                "durationAborts24h": int(row.get("duration24h") or 0),
                "aborts60m": int(row.get("aborts60m") or 0),
                "durationAborts60m": int(row.get("duration60m") or 0),
            }
    except Exception:
        pass
    payload["aborts"] = aborts
    return json_response(
        payload, cache_control="no-store, max-age=0, must-revalidate")
async def _d1_read_guard_fields(env, payload, now):
    """Attach the read guard's switch and tag the statements it controls."""
    report = globals().get("d1_read_guard_report")
    reader = globals().get("d1_read_guard_reader")
    if report is None or reader is None:
        return
    try:
        payload["readGuard"] = await report(env, now)
        for row in payload.get("queryInsights") or ():
            row["guard"] = reader(row.get("query"))
    except Exception:
        pass
async def _admin_d1_usage_report(env, request):
    """Admin-only D1 spend against today's Free allowances.

    Daily reads/writes come from Cloudflare's account analytics. Storage comes
    from the bound database itself, so it remains visible even when the
    separately scoped analytics token has not been configured.
    """
    params = parse_qs(urlparse(request.url).query)
    node = clean_string(params.get("node", [""])[0], MAX_NODE_NAME).lower()
    ts = clean_string(params.get("ts", [""])[0], 20)
    sig = clean_string(params.get("sig", [""])[0], 200)
    canonical = (
        "forkmesh-admin-d1-usage-report-v1\n" + node + "\n" + ts
    ).encode()
    if not await _admin_authorized(env, node, ts, sig, canonical):
        return json_response({"error": "unauthorized"}, status=401)

    now = int(Date.now())
    try:
        storage = await _world_infrastructure_module().d1_storage_usage(env)
    except Exception:
        storage = {}
    day, start, end = _usage_budget_utc(now)
    partial = {
        "ok": False,
        "day": day,
        "windowStart": start,
        "windowEnd": end,
        "sampledAt": now,
        "storage": storage,
    }
    # The switch stays reachable when analytics are not.
    await _d1_read_guard_fields(env, partial, now)

    account_id = clean_string(
        getattr(env, "WORKERS_OBSERVABILITY_ACCOUNT_ID", ""), 64).strip()
    token = clean_string(
        getattr(env, "USAGE_ANALYTICS_API_TOKEN", ""), 512).strip()
    if not account_id or not token:
        partial["error"] = "analytics_unconfigured"
        partial["configured"] = False
        return json_response(
            partial,
            cache_control="no-store, max-age=0, must-revalidate")

    detail = await _cloudflare_d1_usage_today(account_id, token, now)
    if detail is None:
        partial["error"] = "analytics_unavailable"
        return json_response(
            partial, status=502,
            cache_control="no-store, max-age=0, must-revalidate")
    payload = _d1_usage_report_payload(detail, storage, now)
    payload["sampledAt"] = now
    await _d1_read_guard_fields(env, payload, now)
    return json_response(
        payload, cache_control="no-store, max-age=0, must-revalidate")
