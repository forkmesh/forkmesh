"""A Cloudflare D1-shaped binding backed by a local rqlite node.

The app talks to ``env.DB`` exactly as it does on Workers::

    await env.DB.prepare(sql).bind(*args).first()      # row dict or None
    await env.DB.prepare(sql).bind(*args).first("col") # one value or None
    result = await env.DB.prepare(sql).bind(*args).all()
    result.results                                     # list of row dicts
    result.meta.changes / result.meta.last_row_id      # after run()
    await env.DB.batch([stmt, ...])                    # one atomic transaction
    env.DB.withSession("first-primary").prepare(...)   # linearizable read

Every statement goes to rqlite's unified ``/db/request`` endpoint, which
decides per statement whether it is a read or a write (so ``DELETE ...
RETURNING`` awaited through ``first()`` is still replicated through Raft).

Read consistency:

* plain reads use ``level=none`` (served by this node from its own copy), or,
  with ``read_mode="local"``, an in-process read-only ``sqlite3`` connection
  on rqlite's database file (see ``app/host/RQLITE_FINDINGS.md``);
* once the current request has written anything, later reads in that request
  use ``level=weak`` (served by the leader), so a request reads its own
  writes even on a follower;
* ``withSession("first-primary")`` reads use ``level=linearizable``.

Errors keep D1's text shape: SQLite errors become ``D1_ERROR: <sqlite
message>: SQLITE_...`` and an unreachable node, a missing leader or a timeout
becomes ``D1_ERROR: D1 DB is overloaded ...`` so the app's existing
``_D1_SUSTAINED_MARKERS`` path answers 503 instead of a 500.

The module is self-contained: its only third-party dependency is httpx.
"""

from __future__ import annotations

import asyncio
import contextvars
import json
import os
import re
import sqlite3
import time
from dataclasses import dataclass, field
from typing import Any, Iterable

import httpx

from . import d1_caller
from . import read_only

__all__ = [
    "D1Config",
    "D1Error",
    "D1Meta",
    "D1PreparedStatement",
    "D1Result",
    "D1Rqlite",
    "D1Session",
    "ReadOnlyError",
    "make_d1",
    "request_scope",
]


# --------------------------------------------------------------------------
# configuration


@dataclass
class D1Config:
    url: str = "http://127.0.0.1:4001"
    username: str = ""
    password: str = ""
    # "http": every read is an HTTP request with level=none.
    # "local": plain reads open rqlite's SQLite file read-only in-process.
    read_mode: str = "http"
    # rqlite's on-disk database file (<rqlite data dir>/db.sqlite); only
    # used by read_mode="local".
    db_path: str = ""
    timeout: float = 15.0
    # Optional rqlite "freshness" bound (e.g. "5s") for level=none HTTP
    # reads, so a partitioned follower refuses rather than serving
    # arbitrarily stale data.
    freshness: str = ""
    max_connections: int = 64
    max_keepalive: int = 32
    # read_mode="local": a local read running longer than this is aborted
    # and retried over HTTP (0 disables the guard).
    local_read_budget_ms: float = 100.0
    # A level=none read that finds no row is re-asked at level=weak (the
    # leader), so a row just written through another node is never
    # reported missing (round-robin DNS spreads one client's requests).
    confirm_misses: bool = True
    # Leader confirmations are budgeted per process (token bucket): every
    # empty SELECT, anonymous 404s included, would otherwise cost a leader
    # read (audit 2026-09-24, M8). Past the budget the local answer stands.
    confirm_rate: float = 20.0
    confirm_burst: float = 40.0
    extra: dict = field(default_factory=dict)

    @classmethod
    def from_any(cls, config) -> "D1Config":
        if isinstance(config, cls):
            return config
        if config is None:
            config = {}
        if not isinstance(config, dict):
            config = {k: getattr(config, k) for k in dir(config)
                      if not k.startswith("_")}
        known = {f for f in cls.__dataclass_fields__ if f != "extra"}
        aliases = {
            "RQLITE_URL": "url", "rqlite_url": "url",
            "RQLITE_USERNAME": "username", "RQLITE_PASSWORD": "password",
            "RQLITE_READ_MODE": "read_mode", "RQLITE_DB_PATH": "db_path",
            "RQLITE_TIMEOUT": "timeout", "RQLITE_FRESHNESS": "freshness",
            "RQLITE_CONFIRM_MISSES": "confirm_misses",
            "RQLITE_CONFIRM_RATE": "confirm_rate",
            "RQLITE_CONFIRM_BURST": "confirm_burst",
        }
        values, extra = {}, {}
        for key, value in config.items():
            name = aliases.get(key, key)
            if name in known:
                values[name] = value
            else:
                extra[key] = value
        out = cls(**values)
        out.extra = extra
        out.timeout = float(out.timeout)
        out.read_mode = str(out.read_mode or "http").lower()
        if out.read_mode not in ("http", "local"):
            raise ValueError("read_mode must be 'http' or 'local'")
        if out.read_mode == "local" and not out.db_path:
            raise ValueError("read_mode='local' needs db_path")
        out.url = str(out.url).rstrip("/")
        out.confirm_rate = max(0.0, float(out.confirm_rate))
        out.confirm_burst = max(1.0, float(out.confirm_burst))
        if isinstance(out.confirm_misses, str):
            out.confirm_misses = out.confirm_misses.strip().lower() not in (
                "0", "false", "no", "off", "")
        return out


class TokenBucket:
    """A per-process token bucket (``rate`` tokens/s, up to ``burst``)."""

    def __init__(self, rate, burst, clock=time.monotonic):
        self.rate = float(rate)
        self.burst = float(burst)
        self.clock = clock
        self.tokens = float(burst)
        self.stamp = clock()

    def take(self):
        now = self.clock()
        self.tokens = min(self.burst,
                          self.tokens + (now - self.stamp) * self.rate)
        self.stamp = now
        if self.tokens >= 1.0:
            self.tokens -= 1.0
            return True
        return False


def make_d1(config=None) -> "D1Rqlite":
    """Build the ``env.DB`` binding from a dict, object or D1Config."""
    return D1Rqlite(D1Config.from_any(config))


# --------------------------------------------------------------------------
# per-request read-your-writes state

_REQUEST_STATE: contextvars.ContextVar = contextvars.ContextVar(
    "forkmesh_d1_rqlite_request", default=None)


class request_scope:
    """Mark the start of one request (optional).

    Without a scope, the first write in a task still switches that task (and
    tasks it creates afterwards) to ``level=weak`` reads. A scope also lets
    child tasks created *before* the write see it, since they share the
    mutable holder.
    """

    def __enter__(self):
        self._token = _REQUEST_STATE.set({"wrote": False})
        return self

    def __exit__(self, *exc):
        _REQUEST_STATE.reset(self._token)

    async def __aenter__(self):
        return self.__enter__()

    async def __aexit__(self, *exc):
        self.__exit__(*exc)


def _mark_write():
    state = _REQUEST_STATE.get()
    if state is None:
        _REQUEST_STATE.set({"wrote": True})
    else:
        state["wrote"] = True


def _wrote_in_request() -> bool:
    state = _REQUEST_STATE.get()
    return bool(state and state.get("wrote"))


# --------------------------------------------------------------------------
# errors


class D1Error(Exception):
    """An error whose text matches what the app expects from D1."""


class ReadOnlyError(D1Error):
    pass


_OVERLOAD_HINTS = (
    "leader not found", "not leader", "no leader", "leadership lost",
    "context deadline exceeded",
    "timeout", "timed out", "connection refused", "stale read",
    "unavailable", "shutdown", "raft", "store not open",
)


_RAFT_ERROR_HINTS = (
    "leader not found", "not leader", "leadership lost",
    "context deadline exceeded", "timed out enqueuing",
    "raft is already shutdown", "timeout waiting for fsm",
    "store not open", "connection reset by peer", "connection refused",
    "broken pipe", "i/o timeout",
)


def _overload(detail: str) -> D1Error:
    # "overload" is one of the app's _D1_SUSTAINED_MARKERS and "D1_ERROR"
    # makes _is_d1_platform_error treat it as the database failing, so the
    # router answers 503 (backpressure) instead of a 500 with a stack trace.
    detail = " ".join(str(detail or "").split())[:300]
    return D1Error("D1_ERROR: D1 DB is overloaded (rqlite unavailable: %s)"
                   % detail)


def _sqlite_error(message: str) -> D1Error:
    text = str(message or "").strip()
    lower = text.lower()
    if any(hint in lower for hint in _RAFT_ERROR_HINTS):
        return _overload(text)
    if "constraint failed" in lower:
        code = "SQLITE_CONSTRAINT"
    elif "database is locked" in lower or "busy" in lower:
        code = "SQLITE_BUSY"
    elif "readonly" in lower or "read-only" in lower:
        code = "SQLITE_READONLY"
    elif "too big" in lower:
        code = "SQLITE_TOOBIG"
    else:
        code = "SQLITE_ERROR"
    return D1Error("D1_ERROR: %s: %s" % (text, code))


# --------------------------------------------------------------------------
# results


class D1Meta:
    """D1's ``meta`` object: attribute access plus ``dict``-style ``get``."""

    __slots__ = ("changes", "last_row_id", "changed_db", "duration",
                 "rows_read", "rows_written", "served_by", "size_after")

    def __init__(self, changes=0, last_row_id=0, duration=0.0, rows_read=0,
                 served_by="rqlite"):
        self.changes = int(changes or 0)
        self.last_row_id = int(last_row_id or 0)
        self.changed_db = bool(self.changes)
        self.duration = float(duration or 0.0)
        self.rows_read = int(rows_read or 0)
        self.rows_written = self.changes
        self.served_by = served_by
        self.size_after = 0

    def to_py(self):
        return {name: getattr(self, name) for name in self.__slots__}

    def get(self, key, default=None):
        return getattr(self, key, default) if key in self.__slots__ else default

    def __getitem__(self, key):
        if key not in self.__slots__:
            raise KeyError(key)
        return getattr(self, key)

    def __repr__(self):
        return "D1Meta(%r)" % self.to_py()


class D1Result:
    __slots__ = ("results", "meta", "success")

    def __init__(self, results=None, meta=None, success=True):
        self.results = results if results is not None else []
        self.meta = meta if meta is not None else D1Meta()
        self.success = success

    def to_py(self):
        return {"results": self.results, "meta": self.meta.to_py(),
                "success": self.success}

    def get(self, key, default=None):
        return self.to_py().get(key, default)

    def __getitem__(self, key):
        return self.to_py()[key]

    def __repr__(self):
        return "D1Result(results=%d rows, meta=%r)" % (
            len(self.results), self.meta)


# --------------------------------------------------------------------------
# argument conversion


def _is_js_nullish(value) -> bool:
    return value is None or type(value).__name__ in ("JsNull", "JsUndefined")


def _to_param(value):
    if _is_js_nullish(value):
        return None
    if isinstance(value, bool):
        return 1 if value else 0
    if isinstance(value, (int, str)):
        return value
    if isinstance(value, float):
        if value != value or value in (float("inf"), float("-inf")):
            raise D1Error("D1_TYPE_ERROR: Type 'number' (%r) not supported "
                          "for value" % value)
        return value
    if isinstance(value, (bytes, bytearray, memoryview)):
        # rqlite binds a JSON array of byte values as a BLOB parameter.
        return list(bytes(value))
    # A host-shim Uint8Array / ArrayBuffer: accept anything that exposes
    # its bytes or supports the buffer protocol.
    for method in ("to_bytes", "tobytes"):
        convert = getattr(value, method, None)
        if callable(convert):
            return list(bytes(convert()))
    if type(value).__name__ in ("Uint8Array", "ArrayBuffer", "JsBuffer"):
        to_py = getattr(value, "to_py", None)
        if callable(to_py):
            return _to_param(to_py())
    try:
        return list(bytes(memoryview(value)))
    except TypeError:
        pass
    raise D1Error("D1_TYPE_ERROR: Type '%s' not supported for value '%r'"
                  % (type(value).__name__, value))


def _convert_value(value, sqltype):
    if isinstance(value, list):
        # blob_array: BLOBs come back as a JSON array of byte values.
        return bytes(value)
    if sqltype == "real" and isinstance(value, int) and not isinstance(
            value, bool):
        return float(value)
    return value


def _convert_rows(result) -> list:
    rows = result.get("rows") or []
    types = result.get("types") or {}
    if not types:
        return [dict(row) for row in rows]
    return [{key: _convert_value(value, types.get(key))
             for key, value in row.items()} for row in rows]


# --------------------------------------------------------------------------
# statement classification (only used to pick the local read path)

_LEADING_NOISE = re.compile(r"^(?:\s+|--[^\n]*(?:\n|$)|/\*.*?\*/)+", re.S)
_READ_HEAD = ("SELECT", "WITH", "VALUES", "EXPLAIN", "PRAGMA")
_LOCAL_WRITE = object()
_WRITE_WORD = re.compile(
    r"\b(INSERT|UPDATE|DELETE|REPLACE|UPSERT|CREATE|DROP|ALTER|RETURNING|"
    r"ATTACH|DETACH|VACUUM|REINDEX|ANALYZE|PRAGMA)\b", re.I)


def _is_plain_read(sql: str) -> bool:
    text = _LEADING_NOISE.sub("", sql or "")
    head = text[:10].upper()
    if not (head.startswith("SELECT") or head.startswith("WITH")
            or head.startswith("VALUES")):
        return False
    # Conservative: any write keyword anywhere (even inside a string literal)
    # sends the statement through rqlite instead.
    return _WRITE_WORD.search(text) is None


def _looks_like_read(sql: str) -> bool:
    text = _LEADING_NOISE.sub("", sql or "")
    return text[:10].upper().startswith(_READ_HEAD)


# --------------------------------------------------------------------------
# the binding


class D1PreparedStatement:
    __slots__ = ("_db", "_sql", "_args", "_session")

    def __init__(self, db, sql, args=(), session=None):
        self._db = db
        self._sql = str(sql)
        self._args = tuple(args)
        self._session = session

    @property
    def sql(self):
        return self._sql

    @property
    def args(self):
        return self._args

    def bind(self, *args) -> "D1PreparedStatement":
        return D1PreparedStatement(self._db, self._sql, args, self._session)

    def _payload(self):
        if not self._args:
            return [self._sql]
        return [self._sql] + [_to_param(value) for value in self._args]

    async def _execute(self) -> D1Result:
        started = time.perf_counter()
        result = None
        try:
            result = await self._db._execute_one(self, self._session)
            return result
        finally:
            elapsed = (time.perf_counter() - started) * 1000
            d1_caller.note_query(elapsed)
            if result is None:
                d1_caller.note_done(self._sql, elapsed, failed=True)
            else:
                meta = result.meta
                d1_caller.note_done(
                    self._sql, elapsed, len(result.results), meta.changes,
                    "local" if meta.served_by == "rqlite-local" else "http")

    async def all(self) -> D1Result:
        return await self._execute()

    async def run(self) -> D1Result:
        return await self._execute()

    async def first(self, column=None):
        result = await self._execute()
        if not result.results:
            return None
        row = result.results[0]
        if column is None or _is_js_nullish(column):
            return row
        if column not in row:
            raise D1Error("D1_COLUMN_NOTFOUND: Column not found (%s)" % column)
        return row[column]

    async def raw(self, options=None):
        result = await self._execute()
        rows = [list(row.values()) for row in result.results]
        column_names = False
        if isinstance(options, dict):
            column_names = bool(options.get("columnNames"))
        elif options is not None:
            column_names = bool(getattr(options, "columnNames", False))
        if column_names:
            names = list(result.results[0].keys()) if result.results else []
            return [names] + rows
        return rows

    def __repr__(self):
        return "D1PreparedStatement(%r, %d args)" % (self._sql[:60],
                                                     len(self._args))


class D1Session:
    """``withSession(...)``: the first read is linearizable when asked for
    ``first-primary``; later reads in the session use at least ``weak``."""

    def __init__(self, db, constraint):
        self._db = db
        self._constraint = str(constraint or "first-unconstrained")
        self._first_done = False

    def _read_level(self):
        if self._constraint == "first-primary" and not self._first_done:
            self._first_done = True
            return "linearizable"
        if self._constraint == "first-primary":
            return "weak"
        return None

    def prepare(self, sql) -> D1PreparedStatement:
        return D1PreparedStatement(self._db, sql, (), self)

    async def batch(self, statements):
        return await self._db.batch(statements)

    def getBookmark(self):
        return None


class D1Rqlite:
    def __init__(self, config: D1Config):
        self.config = config
        self._client = None
        self._client_loop = None
        self._auth = ((config.username, config.password)
                      if config.username else None)
        self._local = None
        self._local_ino = None
        self._local_started = None
        self.stats = {"http": 0, "local": 0, "local_fallback": 0,
                      "confirm_skipped": 0}
        self._confirm_bucket = TokenBucket(config.confirm_rate,
                                           config.confirm_burst)
        self._confirm_logged_at = None
        self._confirm_skipped_since_log = 0
        self.read_only_probe = None

    # -- D1 API --------------------------------------------------------

    def prepare(self, sql) -> D1PreparedStatement:
        return D1PreparedStatement(self, sql)

    def withSession(self, constraint_or_bookmark=None) -> D1Session:
        return D1Session(self, constraint_or_bookmark)

    async def batch(self, statements) -> list:
        statements = list(statements or [])
        if not statements:
            return []
        if self.read_only():
            if all(_is_plain_read(stmt.sql) for stmt in statements):
                return [await self._read_only_execute(stmt)
                        for stmt in statements]
            return self._refuse_write(len(statements))
        payload = [stmt._payload() for stmt in statements]
        started = time.perf_counter()
        try:
            body = await self._post(payload, transaction=True,
                                    level=self._write_read_level())
        finally:
            d1_caller.note_query((time.perf_counter() - started) * 1000,
                                 len(statements))
        results = body.get("results") or []
        self._raise_first_error(results)
        _mark_write()
        return [self._to_result(item) for item in results]

    async def exec(self, sql):
        statements = split_statements(sql)
        if not statements:
            return {"count": 0, "duration": 0}
        if self.read_only():
            self._refuse_write()
            return {"count": 0, "duration": 0}
        started = time.perf_counter()
        try:
            body = await self._post([[s] for s in statements],
                                    transaction=True)
        finally:
            d1_caller.note_query((time.perf_counter() - started) * 1000,
                                 len(statements))
        self._raise_first_error(body.get("results") or [])
        _mark_write()
        return {"count": len(statements),
                "duration": (time.perf_counter() - started) * 1000}

    async def dump(self):
        client = self._http()
        try:
            response = await client.get(self.config.url + "/db/backup")
        except httpx.HTTPError as error:
            raise _overload(repr(error)) from None
        if response.status_code != 200:
            raise _overload("backup HTTP %d" % response.status_code)
        return response.content

    # -- host helpers --------------------------------------------------

    async def aclose(self):
        client, self._client = self._client, None
        if client is not None:
            try:
                await client.aclose()
            except Exception:
                pass
        if self._local is not None:
            self._local.close()
            self._local = None

    # -- internals -----------------------------------------------------

    def _http(self) -> httpx.AsyncClient:
        loop = asyncio.get_running_loop()
        if self._client is None or self._client_loop is not loop:
            # A client is bound to the loop it was created on; tests (and
            # asyncio.run per call) get a fresh one per loop.
            self._client = httpx.AsyncClient(
                timeout=httpx.Timeout(self.config.timeout, connect=3.0),
                limits=httpx.Limits(
                    max_connections=self.config.max_connections,
                    max_keepalive_connections=self.config.max_keepalive,
                    keepalive_expiry=30.0),
                auth=self._auth,
                headers={"content-type": "application/json"},
            )
            self._client_loop = loop
        return self._client

    def _write_read_level(self):
        return "weak" if _wrote_in_request() else None

    def read_only(self):
        probe = self.read_only_probe
        return probe is not None and bool(probe())

    def _refuse_write(self, count=1):
        read_only.refuse(ReadOnlyError)
        return [D1Result(meta=D1Meta(served_by="read-only"))
                for _ in range(count)]

    async def _read_only_execute(self, stmt):
        if not _looks_like_read(stmt.sql):
            return self._refuse_write()[0]
        plain = _is_plain_read(stmt.sql)
        if self.config.read_mode == "local":
            local = self._local_read(stmt, writes_fail=not plain)
            if local is _LOCAL_WRITE:
                return self._refuse_write()[0]
            if local is not None:
                return local
        try:
            body = await self._post([stmt._payload()], level="none",
                                    freshness=False)
            results = body.get("results") or []
            self._raise_first_error(results)
        except D1Error as error:
            if plain or "overloaded" not in str(error):
                raise
            return self._refuse_write()[0]
        return self._to_result(results[0]) if results else D1Result()

    async def _post(self, payload, transaction=False, level=None,
                    freshness=True):
        params = ["associative", "blob_array"]
        if transaction:
            params.append("transaction")
        if level:
            params.append("level=" + level)
            if level == "none" and freshness and self.config.freshness:
                params.append("freshness=" + self.config.freshness)
        url = self.config.url + "/db/request?" + "&".join(params)
        content = json.dumps(payload, separators=(",", ":"),
                             allow_nan=False)
        self.stats["http"] += 1
        try:
            response = await self._http().post(url, content=content)
        except httpx.TimeoutException as error:
            raise _overload("timeout: %r" % error) from None
        except httpx.TransportError as error:
            raise _overload(repr(error)) from None
        if response.status_code in (502, 503, 504):
            raise _overload("HTTP %d %s" % (response.status_code,
                                            response.text[:200]))
        if response.status_code in (401, 403):
            raise D1Error("D1_ERROR: rqlite rejected credentials (HTTP %d)"
                          % response.status_code)
        if response.status_code != 200:
            text = response.text.strip()
            lower = text.lower()
            if any(hint in lower for hint in _OVERLOAD_HINTS):
                raise _overload("HTTP %d %s" % (response.status_code,
                                                text[:200]))
            raise _sqlite_error(text or "HTTP %d" % response.status_code)
        try:
            body = response.json()
        except ValueError:
            raise _sqlite_error("invalid rqlite response: %s"
                                % response.text[:200]) from None
        if isinstance(body, dict) and body.get("error"):
            raise _sqlite_error(body["error"])
        return body

    @staticmethod
    def _raise_first_error(results):
        for item in results:
            if isinstance(item, dict) and item.get("error"):
                raise _sqlite_error(item["error"])

    @staticmethod
    def _to_result(item) -> D1Result:
        rows = _convert_rows(item)
        changes = item.get("rows_affected")
        if changes is None and "rows" in item and "types" in item:
            changes = 0
        meta = D1Meta(
            changes=changes or 0,
            last_row_id=item.get("last_insert_id") or 0,
            duration=(item.get("time") or 0.0) * 1000,
            rows_read=len(rows),
        )
        return D1Result(rows, meta, True)

    async def _execute_one(self, stmt: D1PreparedStatement,
                           session: D1Session | None) -> D1Result:
        if self.read_only():
            return await self._read_only_execute(stmt)
        level = None
        if session is not None:
            level = session._read_level()
        if level is None:
            level = "weak" if _wrote_in_request() else "none"
        plain_read = _is_plain_read(stmt.sql)
        if not plain_read:
            # rqlite routes writes through Raft whatever the level; a level
            # only matters for reads (and a RETURNING write is a write).
            level = None
        if (plain_read and level == "none"
                and self.config.read_mode == "local"):
            local = self._local_read(stmt)
            if local is not None:
                return await self._confirm_miss(stmt, local) \
                    if not local.results else local
        body = await self._post([stmt._payload()], level=level)
        results = body.get("results") or []
        self._raise_first_error(results)
        if not results:
            return D1Result()
        if not plain_read:
            # Anything that is not a plain SELECT may have written (DML,
            # DDL, RETURNING): later reads in this request must see it.
            _mark_write()
        result = self._to_result(results[0])
        if plain_read and level == "none" and not result.results:
            return await self._confirm_miss(stmt, result)
        return result

    async def _confirm_miss(self, stmt, result):
        """Re-ask the leader when a local read finds nothing.

        Round-robin DNS sends consecutive requests of one client to different
        nodes, so a row written a few milliseconds ago on node A (a node
        reserving its account, then finalizing it) can still be missing from
        node B's replica. A miss is the only answer that shape of staleness
        produces, so only misses pay for a leader read; hits stay local."""
        if not self.config.confirm_misses:
            return result
        if not self._confirm_bucket.take():
            self._confirm_skipped()
            return result
        self.stats["confirmed_misses"] = self.stats.get("confirmed_misses", 0) + 1
        body = await self._post([stmt._payload()], level="weak")
        results = body.get("results") or []
        self._raise_first_error(results)
        return self._to_result(results[0]) if results else result

    def _confirm_skipped(self):
        """Count a miss answered locally because the budget was spent;
        log at most once a minute with the count since the last line."""
        self.stats["confirm_skipped"] += 1
        self._confirm_skipped_since_log += 1
        now = time.monotonic()
        if (self._confirm_logged_at is None
                or now - self._confirm_logged_at >= 60.0):
            print("d1_rqlite: confirm-miss budget spent (%.0f/s, burst %.0f):"
                  " %d misses answered locally since the last report "
                  "(total %d)" % (
                      self.config.confirm_rate, self.config.confirm_burst,
                      self._confirm_skipped_since_log,
                      self.stats["confirm_skipped"]), flush=True)
            self._confirm_logged_at = now
            self._confirm_skipped_since_log = 0

    # -- local read-only sqlite3 ----------------------------------------

    def _local_identity(self):
        """(db, -wal, -shm) inode triple, or None while any is missing.

        rqlite keeps db.sqlite's inode across a restart or a snapshot restore
        but creates *new* -wal and -shm files. SQLite shares one shm mapping
        per database inode per process, so a connection that outlives that
        swap - and every connection opened next to it - silently reads the
        old WAL forever (RQLITE_FINDINGS.md). Checking the triple before each
        read and reopening on any change is what makes local reads safe.
        """
        path = self.config.db_path
        try:
            return (os.stat(path).st_ino, os.stat(path + "-wal").st_ino,
                    os.stat(path + "-shm").st_ino)
        except OSError:
            return None

    def _close_local(self):
        conn, self._local = self._local, None
        self._local_ino = None
        if conn is not None:
            try:
                conn.close()
            except Exception:
                pass

    def _local_connection(self):
        identity = self._local_identity()
        if identity is None:
            self._close_local()
            return None
        if self._local is not None and identity != self._local_ino:
            self._close_local()
        if self._local is None:
            conn = sqlite3.connect(
                "file:%s?mode=ro" % self.config.db_path, uri=True,
                timeout=0.05, isolation_level=None, check_same_thread=False,
                cached_statements=256)
            conn.execute("PRAGMA query_only=1")
            conn.row_factory = sqlite3.Row
            budget = float(self.config.local_read_budget_ms) / 1000.0
            if budget > 0:
                # A read that runs long holds the WAL and makes rqlite's
                # snapshot checkpoint fail (it waits only 250 ms): abort it
                # and let rqlite serve that one over HTTP instead.
                def _over_budget():
                    started = self._local_started
                    return int(started is not None
                               and time.perf_counter() - started > budget)
                conn.set_progress_handler(_over_budget, 2000)
            self._local = conn
            self._local_ino = identity
        return self._local

    def _local_read(self, stmt: D1PreparedStatement, writes_fail=False):
        try:
            conn = self._local_connection()
            if conn is None:
                self.stats["local_fallback"] += 1
                return None
            args = []
            for value in stmt.args:
                param = _to_param(value)
                args.append(bytes(param) if isinstance(param, list) else param)
            started = time.perf_counter()
            self._local_started = started
            try:
                # fetch everything now: an open cursor is an open read
                # transaction, which pins the WAL.
                rows = [dict(row) for row in conn.execute(stmt.sql, args)]
            finally:
                self._local_started = None
        except sqlite3.OperationalError as error:
            text = str(error).lower()
            if writes_fail and ("readonly" in text
                                or "attempt to write" in text):
                return _LOCAL_WRITE
            if any(hint in text for hint in (
                    "interrupted", "locked", "busy", "disk i/o",
                    "unable to open", "malformed", "not a database",
                    "readonly", "schema has changed")):
                self.stats["local_fallback"] += 1
                self._close_local()
                return None
            raise _sqlite_error(str(error)) from None
        except sqlite3.DatabaseError as error:
            self.stats["local_fallback"] += 1
            self._close_local()
            return None
        self.stats["local"] += 1
        return D1Result(rows, D1Meta(
            duration=(time.perf_counter() - started) * 1000,
            rows_read=len(rows), served_by="rqlite-local"), True)


def split_statements(sql: str) -> list:
    """Split SQL text into complete statements.

    Cuts only at a ``;`` where ``sqlite3.complete_statement`` agrees the text
    so far is complete, so ``CREATE TRIGGER ... BEGIN ...; END;`` and a ``;``
    inside a string or comment stay whole. Comment-only chunks are dropped.
    """
    text = sql or ""
    out, start = [], 0
    for index, char in enumerate(text):
        if char != ";":
            continue
        candidate = text[start:index + 1]
        if sqlite3.complete_statement(candidate):
            if _LEADING_NOISE.sub("", candidate).strip(" ;\n\t\r"):
                out.append(candidate.strip())
            start = index + 1
    tail = text[start:]
    if _LEADING_NOISE.sub("", tail).strip(" ;\n\t\r"):
        out.append(tail.strip())
    return out


def iter_param_payload(sql: str, args: Iterable[Any]):
    """Public helper for tools: one rqlite statement payload."""
    return [sql] + [_to_param(value) for value in args]
