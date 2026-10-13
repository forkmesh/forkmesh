"""The app's D1 helpers, reduced to their D1-facing behaviour.

A faithful copy of what app/src/entry.py does around ``env.DB``
(d1_bind_args, d1_row_to_dict, _d1_read with one transient replay, d1_all,
d1_first, d1_first_primary, d1_run, d1_run_batch and the error
classification), without importing the 48k-line Worker module. Keep it in
step with entry.py ~12544-12782.
"""

from __future__ import annotations

_D1_TRANSIENT_MARKERS = ("internal error", "network connection lost")
_D1_SUSTAINED_MARKERS = ("overload", "too many")


class JsNull:
    """Stand-in for pyodide's jsnull (the host shim's type has this name)."""

    def __repr__(self):
        return "jsnull"


jsnull = JsNull()


class Env:
    def __init__(self, db):
        self.DB = db


def js_nullish(value):
    return value is None or type(value).__name__ in ("JsNull", "JsUndefined")


def d1_bind_args(args):
    return tuple(jsnull if js_nullish(value) else value for value in args)


def d1_row_to_dict(row):
    if js_nullish(row):
        return None
    if hasattr(row, "to_py"):
        row = row.to_py()
        if js_nullish(row):
            return None
    return dict(row)


def is_transient_d1_error(error):
    text = str(error).lower()
    if any(marker in text for marker in _D1_SUSTAINED_MARKERS):
        return False
    return any(marker in text for marker in _D1_TRANSIENT_MARKERS)


def is_d1_platform_error(error):
    text = str(error).lower()
    if "d1_error" not in text:
        return False
    return bool(is_transient_d1_error(error)
                or any(marker in text for marker in _D1_SUSTAINED_MARKERS))


async def _d1_read(env, sql, args, first):
    last_error = None
    for attempt in range(2):
        stmt = env.DB.prepare(sql)
        if args:
            stmt = stmt.bind(*d1_bind_args(args))
        try:
            return await (stmt.first() if first else stmt.all())
        except Exception as error:
            if attempt or not is_transient_d1_error(error):
                raise
            last_error = error
    raise last_error


async def d1_all(env, sql, *args):
    result = await _d1_read(env, sql, args, False)
    out = []
    results = getattr(result, "results", None)
    if js_nullish(results):
        return out
    for row in results:
        converted = d1_row_to_dict(row)
        if converted is not None:
            out.append(converted)
    return out


async def d1_first(env, sql, *args):
    return d1_row_to_dict(await _d1_read(env, sql, args, True))


async def d1_first_primary(env, sql, *args):
    database = getattr(env, "DB", None)
    with_session = getattr(database, "withSession", None)
    if not callable(with_session):
        return await d1_first(env, sql, *args)
    session = with_session("first-primary")
    stmt = session.prepare(sql)
    if args:
        stmt = stmt.bind(*d1_bind_args(args))
    return d1_row_to_dict(await stmt.first())


async def d1_run(env, sql, *args):
    stmt = env.DB.prepare(sql)
    if args:
        stmt = stmt.bind(*d1_bind_args(args))
    return await stmt.run()


async def d1_run_batch(env, statements):
    prepared = []
    for sql, args in statements:
        stmt = env.DB.prepare(sql)
        if args:
            stmt = stmt.bind(*d1_bind_args(args))
        prepared.append(stmt)
    if not prepared:
        return None
    return await env.DB.batch(prepared)


def changes(result):
    """entry.py:10666 / ledger.py:436 - how the app reads meta.changes."""
    try:
        return int(result.meta.changes)
    except Exception:
        try:
            converted = result.to_py() if hasattr(result, "to_py") else result
            return int((converted.get("meta") or {}).get("changes") or 0)
        except Exception:
            return 0
