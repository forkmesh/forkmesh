"""Infrastructure-capacity measurements exposed to the World.

This module owns the bounded, content-free inventory shown on the Office
Infrastructure floor.  The Worker entrypoint supplies its D1 helpers and live
Durable Object discovery functions, keeping database inspection out of the
request router and making the platform limits explicit in one named place.
"""

import re


MAX_TABLES = 256
MAX_DURABLE_OBJECTS = 32
MAX_SAFE_INTEGER = 9_007_199_254_740_991
TABLE_NAME_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]{0,127}")

# Cloudflare D1 limits documented at
# https://developers.cloudflare.com/d1/platform/limits/
D1_FREE_DATABASE_BYTES = 500_000_000
D1_PAID_DATABASE_BYTES = 10_000_000_000
D1_INCLUDED_ACCOUNT_BYTES = 5_000_000_000


async def d1_storage_usage(env):
    """Return D1 ``size_after`` and the documented Free/Paid ceilings.

    D1 rejects SQLite page-count PRAGMAs at its authorizer boundary. Every
    supported prepared-statement result instead includes ``meta.size_after``;
    a constant SELECT obtains that provider-owned measurement without scanning
    an application table.
    """
    try:
        # Logged like entry.py's D1 helpers so the desktop cloud log sees it.
        print("D1 query: SELECT 1 AS capacity_probe")
        result = await env.DB.prepare("SELECT 1 AS capacity_probe").run()
        meta = getattr(result, "meta", None)
        if meta is None and isinstance(result, dict):
            meta = result.get("meta")
        size_after = (
            meta.get("size_after")
            if isinstance(meta, dict)
            else getattr(meta, "size_after", 0)
        )
        stored_bytes = int(size_after or 0)
    except Exception:
        stored_bytes = 0
    stored_bytes = max(0, min(MAX_SAFE_INTEGER, stored_bytes))
    return {
        "bytes": stored_bytes,
        "freeDatabaseLimitBytes": D1_FREE_DATABASE_BYTES,
        "paidDatabaseLimitBytes": D1_PAID_DATABASE_BYTES,
        "includedAccountStorageBytes": D1_INCLUDED_ACCOUNT_BYTES,
    }


async def d1_table_inventory(env, d1_all, d1_first):
    """Return bounded table row counts without exposing record contents."""
    rows = await d1_all(
        env,
        "SELECT name FROM sqlite_master WHERE type='table' "
        "AND substr(lower(name),1,7)!='sqlite_' "
        "AND substr(lower(name),1,4)!='_cf_' "
        "ORDER BY name LIMIT ?",
        MAX_TABLES,
    )
    capacity = []
    for row in rows or []:
        raw_name = row.get("name") if isinstance(row, dict) else None
        if not isinstance(raw_name, str):
            continue
        name = raw_name.strip()
        lowered = name.lower()
        if (
            name != raw_name
            or lowered.startswith(("sqlite_", "_cf_"))
            or not TABLE_NAME_RE.fullmatch(name)
        ):
            continue
        try:
            count_row = await d1_first(
                env,
                'SELECT COUNT(*) AS row_count FROM "' + name + '"',
            )
            row_count = int((count_row or {}).get("row_count", 0) or 0)
        except Exception:
            # A table can disappear during a rolling migration.
            continue
        if 0 <= row_count <= MAX_SAFE_INTEGER:
            capacity.append({"name": name, "rowCount": row_count})
    return capacity


async def durable_object_inventory(
    env,
    d1_all,
    discover_bindings,
    display_label,
    traffic_max=MAX_SAFE_INTEGER,
):
    """Project discovered Durable Object bindings and content-free traffic."""
    bindings = discover_bindings(env)
    if not bindings:
        return []
    try:
        rows = await d1_all(
            env,
            "SELECT binding, bytes_in, bytes_out, messages, updated_at "
            "FROM durable_object_traffic ORDER BY binding LIMIT ?",
            MAX_DURABLE_OBJECTS,
        )
    except Exception:
        rows = []
    totals = {
        str(row.get("binding") or ""): row
        for row in rows or []
        if isinstance(row, dict)
    }

    def count(row, field):
        try:
            value = int(row.get(field) or 0)
        except (AttributeError, TypeError, ValueError):
            return 0
        return max(0, min(value, traffic_max))

    objects = []
    for binding in bindings[:MAX_DURABLE_OBJECTS]:
        row = totals.get(binding) or {}
        bytes_in = count(row, "bytes_in")
        bytes_out = count(row, "bytes_out")
        objects.append(
            {
                "id": binding,
                "binding": binding,
                "name": display_label(binding),
                "bytesIn": bytes_in,
                "bytesOut": bytes_out,
                "bytesTotal": min(bytes_in + bytes_out, traffic_max),
                "messages": count(row, "messages"),
                "updatedAt": count(row, "updated_at"),
            }
        )
    return objects
