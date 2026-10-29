"""A local-development D1 binding on the stdlib ``sqlite3`` module.

Same shape as Cloudflare D1 (and ``d1_rqlite.py``)::

    await env.DB.prepare(sql).bind(*args).first()      # row dict or None
    await env.DB.prepare(sql).bind(*args).first("col") # one value
    (await env.DB.prepare(sql).all()).results          # list of dicts
    (await stmt.run()).meta.changes / .meta.last_row_id
    await env.DB.batch([stmt, ...])                    # atomic
    env.DB.withSession("first-primary")                # returns the DB

Statements run synchronously on the calling thread; that is fine for a
single developer and tests, and is why production uses rqlite instead.
``apply_migrations`` applies ``app/migrations/*.sql`` in order and records
them in ``d1_migrations`` like wrangler does.
"""

from __future__ import annotations

import sqlite3
import time
from pathlib import Path

from . import d1_caller
from . import read_only

__all__ = ["D1Sqlite", "D1Error", "ReadOnlyError", "apply_app_schema",
           "apply_migrations", "make_d1"]


class D1Error(Exception):
    pass


class ReadOnlyError(D1Error):
    pass


class D1Meta:
    __slots__ = ("changes", "last_row_id", "changed_db", "duration",
                 "rows_read", "rows_written", "served_by", "size_after")

    def __init__(self, changes=0, last_row_id=0, duration=0.0, rows_read=0):
        self.changes = int(changes or 0)
        self.last_row_id = int(last_row_id or 0)
        self.changed_db = bool(self.changes)
        self.duration = float(duration)
        self.rows_read = int(rows_read)
        self.rows_written = self.changes
        self.served_by = "sqlite3"
        self.size_after = 0

    def to_py(self):
        return {name: getattr(self, name) for name in self.__slots__}

    def get(self, key, default=None):
        return getattr(self, key, default) if key in self.__slots__ \
            else default

    def __getitem__(self, key):
        if key not in self.__slots__:
            raise KeyError(key)
        return getattr(self, key)


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


def _param(value):
    if value is None or type(value).__name__ in ("JsNull", "JsUndefined"):
        return None
    if isinstance(value, bool):
        return 1 if value else 0
    if isinstance(value, (int, float, str, bytes)):
        return value
    if isinstance(value, (bytearray, memoryview)):
        return bytes(value)
    convert = getattr(value, "to_bytes", None)
    if callable(convert):
        return bytes(convert())
    if isinstance(value, (list, tuple)):
        return bytes(value)
    raise D1Error("D1_TYPE_ERROR: Type '%s' not supported for value %r"
                  % (type(value).__name__, value))


def _row(cursor, values):
    out = {}
    for (name, *_), value in zip(cursor.description, values):
        out[name] = list(value) if isinstance(value, bytes) else value
    return out


def _error(error):
    code = "SQLITE_CONSTRAINT" if isinstance(
        error, sqlite3.IntegrityError) else "SQLITE_ERROR"
    return D1Error("D1_ERROR: %s: %s" % (error, code))


class D1PreparedStatement:
    __slots__ = ("_db", "_sql", "_args")

    def __init__(self, db, sql, args=()):
        self._db = db
        self._sql = str(sql)
        self._args = tuple(args)

    def bind(self, *args):
        return D1PreparedStatement(self._db, self._sql,
                                   tuple(_param(a) for a in args))

    def _execute(self):
        if self._db.read_only():
            return self._db._gated(self._sql, self._args)
        return self._db._execute(self._sql, self._args)

    async def all(self):
        return self._execute()

    async def run(self):
        return self._execute()

    async def first(self, column=None):
        result = self._execute()
        if not result.results:
            return None
        row = result.results[0]
        if column is None or type(column).__name__ in ("JsNull",):
            return row
        if column not in row:
            raise D1Error("D1_COLUMN_NOTFOUND: Column not found (%s)"
                          % column)
        return row[column]

    async def raw(self, options=None):
        result = self._execute()
        rows = [list(row.values()) for row in result.results]
        names = False
        if isinstance(options, dict):
            names = bool(options.get("columnNames"))
        elif options is not None:
            names = bool(getattr(options, "columnNames", False))
        if names:
            head = list(result.results[0].keys()) if result.results else []
            return [head] + rows
        return rows


class D1Sqlite:
    def __init__(self, path=":memory:"):
        self.path = str(path)
        if self.path != ":memory:":
            Path(self.path).parent.mkdir(parents=True, exist_ok=True)
        self._conn = sqlite3.connect(self.path, isolation_level=None,
                                     check_same_thread=False)
        self._conn.execute("PRAGMA foreign_keys=ON")
        if self.path != ":memory:":
            self._conn.execute("PRAGMA journal_mode=WAL")
        self._conn.execute("PRAGMA busy_timeout=5000")
        self.statements = 0
        self.read_only_probe = None

    # -- D1 API --------------------------------------------------------
    def prepare(self, sql):
        return D1PreparedStatement(self, sql)

    def withSession(self, constraint_or_bookmark=None):
        del constraint_or_bookmark
        return self

    def getBookmark(self):
        return None

    async def batch(self, statements):
        statements = list(statements or [])
        if not statements:
            return []
        if self.read_only():
            results = [self._query_only(s._sql, s._args) for s in statements]
            if any(r is None for r in results):
                read_only.refuse(ReadOnlyError)
                return [D1Result() for _ in statements]
            return results
        conn = self._conn
        conn.execute("BEGIN")
        try:
            results = [self._execute(s._sql, s._args, in_tx=True)
                       for s in statements]
        except BaseException:
            conn.execute("ROLLBACK")
            raise
        conn.execute("COMMIT")
        return results

    async def exec(self, sql):
        if self.read_only():
            read_only.refuse(ReadOnlyError)
            return {"count": 0, "duration": 0}
        started = time.perf_counter()
        try:
            self._conn.executescript(str(sql))
        except sqlite3.Error as error:
            raise _error(error) from None
        return {"count": sum(1 for s in str(sql).split(";") if s.strip()),
                "duration": (time.perf_counter() - started) * 1000}

    async def dump(self):
        return b"".join(line.encode() + b"\n"
                        for line in self._conn.iterdump())

    def close(self):
        self._conn.close()

    def read_only(self):
        probe = self.read_only_probe
        return probe is not None and bool(probe())

    # -- internals -----------------------------------------------------
    def _gated(self, sql, args):
        result = self._query_only(sql, args)
        if result is None:
            read_only.refuse(ReadOnlyError)
            return D1Result()
        return result

    def _query_only(self, sql, args):
        conn = self._conn
        conn.execute("PRAGMA query_only=1")
        try:
            return self._execute(sql, args)
        except D1Error as error:
            if "readonly" in str(error).lower():
                return None
            raise
        finally:
            conn.execute("PRAGMA query_only=0")

    def _execute(self, sql, args, in_tx=False):
        del in_tx
        started = time.perf_counter()
        self.statements += 1
        conn = self._conn
        try:
            cursor = conn.execute(sql, args)
            rows = cursor.fetchall() if cursor.description else []
        except sqlite3.Error as error:
            raise _error(error) from None
        results = [_row(cursor, values) for values in rows]
        changes = conn.total_changes
        duration = (time.perf_counter() - started) * 1000
        d1_caller.note_query(duration)
        meta = D1Meta(
            changes=cursor.rowcount if cursor.rowcount > 0 else 0,
            last_row_id=cursor.lastrowid or 0,
            duration=duration,
            rows_read=len(results))
        del changes
        return D1Result(results, meta)


def make_d1(config=None):
    """Factory with the same calling convention as ``d1_rqlite.make_d1``."""
    path = ":memory:"
    if isinstance(config, (str, Path)):
        path = str(config)
    elif isinstance(config, dict):
        path = str(config.get("path") or config.get("db_path") or path)
    return D1Sqlite(path)


def split_statements(script):
    """Split SQL into complete statements (trigger bodies stay whole)."""
    out, current = [], ""
    for line in str(script).splitlines(keepends=True):
        current += line
        if sqlite3.complete_statement(current):
            statement = current.strip()
            if statement.strip(";").strip():
                out.append(statement)
            current = ""
    if current.strip():
        out.append(current.strip())
    return out


# A column added by a later migration that an edited earlier migration
# already creates: skipping the ALTER keeps a fresh database identical to an
# incrementally migrated one (0075 + 0089 on world_office_marketing_tasks).
_TOLERATED = ("duplicate column name",)


def apply_migrations(db, migrations_dir, quiet=True):
    """Apply every ``NNNN_*.sql`` not yet in ``d1_migrations``; returns names.

    Mirrors ``wrangler d1 migrations apply``: one migration per transaction,
    recorded by file name. ``ALTER TABLE ... ADD COLUMN`` of a column that
    already exists is skipped.
    """
    conn = db._conn if isinstance(db, D1Sqlite) else db
    conn.execute(
        "CREATE TABLE IF NOT EXISTS d1_migrations ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT UNIQUE, "
        "applied_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP NOT NULL)")
    done = {row[0] for row in conn.execute("SELECT name FROM d1_migrations")}
    applied = []
    for path in sorted(Path(migrations_dir).glob("*.sql")):
        if path.name in done:
            continue
        conn.execute("BEGIN")
        try:
            for statement in split_statements(path.read_text("utf-8")):
                upper = "\n".join(
                    line for line in statement.splitlines()
                    if not line.lstrip().startswith("--")).strip().upper()
                if upper.startswith(("BEGIN TRANSACTION", "COMMIT",
                                     "END TRANSACTION")) or \
                        upper.rstrip(";").strip() == "BEGIN":
                    continue
                try:
                    conn.execute(statement)
                except sqlite3.OperationalError as error:
                    if not ("ALTER TABLE" in upper and any(
                            text in str(error) for text in _TOLERATED)):
                        raise
            conn.execute("INSERT INTO d1_migrations (name) VALUES (?)",
                         (path.name,))
        except sqlite3.Error as error:
            conn.execute("ROLLBACK")
            raise RuntimeError("migration %s failed: %s"
                               % (path.name, error)) from None
        conn.execute("COMMIT")
        applied.append(path.name)
        if not quiet:
            print("applied", path.name)
    return applied


def apply_app_schema(db, entry_module):
    """Run the app's own idempotent DDL (entry._apply_schema's slow path).

    The migrations stamp the schema fingerprint, so ``ensure_schema`` takes
    its fast path on a database built only from app/migrations; but schema.py
    still creates tables no migration does (``issue_bounty`` ...).
    Production D1 has them from earlier full applies; a fresh database needs
    this once. Returns the number of statements that failed (ALTERs of
    columns that already exist are expected to).
    """
    conn = db._conn if isinstance(db, D1Sqlite) else db
    failed = 0
    groups = (
        (getattr(entry_module, "SCHEMA_PRE_CREATE_ALTER_STATEMENTS", ()),
         True),
        (getattr(entry_module, "SCHEMA_STATEMENTS", None)
         or entry_module.schema.SCHEMA_STATEMENTS, False),
        (getattr(entry_module, "SCHEMA_ALTER_STATEMENTS", ()), True),
    )
    for statements, tolerant in groups:
        for sql in statements:
            try:
                conn.execute(sql)
            except sqlite3.Error:
                if not tolerant:
                    raise
                failed += 1
    return failed
