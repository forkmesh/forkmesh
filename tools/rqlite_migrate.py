#!/usr/bin/env python3
"""Apply app/migrations/*.sql to an rqlite cluster, the way wrangler does D1.

* Tracks applied files in ``d1_migrations`` with wrangler's exact shape
  (``id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT UNIQUE, applied_at
  TIMESTAMP DEFAULT CURRENT_TIMESTAMP NOT NULL``) and skips applied ones, so
  a database later moved between D1 and rqlite keeps one ledger.
* Files run in filename order (duplicate numbers such as 0069_* sort by the
  full name, as wrangler does). Each file plus its ledger row is one
  ``/db/request?transaction``: a failing file leaves nothing behind.
* Statements are split with ``sqlite3.complete_statement`` so ``CREATE
  TRIGGER ... BEGIN ...; END;`` stays whole.
* rqlite rewrites ``RANDOM()`` and ``'now'`` time functions into literals
  before replicating. That keeps DML deterministic but *freezes* a CREATE
  TRIGGER / CREATE VIEW body at creation time (see
  app/host/RQLITE_FINDINGS.md). A file whose trigger/view body calls one of
  those functions is therefore sent with ``norwtime&norwrandom``; every other
  file keeps the rewrite, so replicas compute the same values.
* Afterwards it stamps ``schema_meta`` with ``schema_fingerprint:<fp>`` =
  ``applied`` (the literal ``_SCHEMA_FINGERPRINT`` in app/src/entry.py),
  exactly the row the latest native migration writes in production, so the
  app's ``_apply_schema`` takes its one-SELECT fast path, and sets the
  ``d1_read_guard`` switch (default ``off``: there is no D1 read quota).
* Runs against any voter of a cluster that has a Raft leader (rqlite
  forwards every write to its leader); a node without a leader is refused
  (``--force-follower`` skips even that check, tests only).

Usage::

    tools/rqlite_migrate.py --url http://127.0.0.1:4001 [--user U --password P]
    tools/rqlite_migrate.py --list      # show pending migrations only
"""

from __future__ import annotations

import argparse
import ast
import base64
import datetime as _dt
import json
import os
import re
import sqlite3
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MIGRATIONS = ROOT / "app" / "migrations"
ENTRY = ROOT / "app" / "src" / "entry.py"

D1_MIGRATIONS_DDL = (
    "CREATE TABLE IF NOT EXISTS d1_migrations("
    "id INTEGER PRIMARY KEY AUTOINCREMENT, "
    "name TEXT UNIQUE, "
    "applied_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP NOT NULL)"
)

_NOISE = re.compile(r"^(?:\s+|--[^\n]*(?:\n|$)|/\*.*?\*/)+", re.S)
_DEFINES_BODY = re.compile(r"^\s*CREATE\s+(?:TEMP\s+|TEMPORARY\s+)?"
                           r"(TRIGGER|VIEW)\b", re.I)
_ADD_COLUMN = re.compile(
    r"^\s*ALTER\s+TABLE\s+[\"`\[]?(\w+)[\"`\]]?\s+ADD\s+(?:COLUMN\s+)?"
    r"[\"`\[]?(\w+)", re.I)
_NONDETERMINISTIC = re.compile(
    r"random\s*\(|randomblob\s*\(|'now'|\"now\"|current_(?:timestamp|date|"
    r"time)\b|unixepoch\s*\(\s*\)", re.I)


class MigrateError(RuntimeError):
    pass


def split_statements(sql: str) -> list[str]:
    """Cut at a ';' only where sqlite3 agrees the statement is complete."""
    out, start = [], 0
    for index, char in enumerate(sql):
        if char != ";":
            continue
        candidate = sql[start:index + 1]
        if sqlite3.complete_statement(candidate):
            if _NOISE.sub("", candidate).strip(" ;\n\t\r"):
                out.append(candidate.strip())
            start = index + 1
    tail = sql[start:]
    if _NOISE.sub("", tail).strip(" ;\n\t\r"):
        out.append(tail.strip())
    return out


def runtime_schema_fingerprint() -> str:
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(t, ast.Name) and t.id == "_SCHEMA_FINGERPRINT"
                for t in node.targets):
            value = ast.literal_eval(node.value)
            if isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value):
                return value
    raise MigrateError("could not read _SCHEMA_FINGERPRINT from entry.py")


class Rqlite:
    def __init__(self, url, user="", password="", timeout=120.0):
        self.url = url.rstrip("/")
        self.timeout = timeout
        self.headers = {"content-type": "application/json"}
        if user:
            token = base64.b64encode(
                ("%s:%s" % (user, password)).encode()).decode()
            self.headers["authorization"] = "Basic " + token

    def _call(self, path, body=None):
        data = json.dumps(body).encode() if body is not None else None
        request = urllib.request.Request(
            self.url + path, data=data, headers=self.headers,
            method="POST" if data is not None else "GET")
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as r:
                raw = r.read()
        except urllib.error.HTTPError as error:
            raise MigrateError("HTTP %d from %s: %s" % (
                error.code, path, error.read()[:300].decode(errors="replace")))
        except (urllib.error.URLError, OSError) as error:
            raise MigrateError("cannot reach rqlite at %s: %s" % (
                self.url, error))
        try:
            return json.loads(raw)
        except ValueError:
            raise MigrateError("non-JSON reply from %s: %r" % (path, raw[:200]))

    def status(self):
        return self._call("/status")

    def is_leader(self):
        state = (((self.status() or {}).get("store") or {}).get("raft")
                 or {}).get("state")
        return state == "Leader", state

    def has_leader(self):
        """(True, state) when this node belongs to a cluster with a leader."""
        store = (self.status() or {}).get("store") or {}
        state = (store.get("raft") or {}).get("state")
        leader = store.get("leader") or {}
        known = bool(leader.get("node_id") or leader.get("addr"))
        return state == "Leader" or (state == "Follower" and known), state

    def request(self, statements, transaction=False, rewrite=True,
                level="strong"):
        results = self.results(statements, transaction, rewrite, level)
        for index, item in enumerate(results):
            if item.get("error"):
                raise MigrateError("statement %d: %s" % (index + 1,
                                                         item["error"]))
        return results

    def results(self, statements, transaction=False, rewrite=True,
                level="strong"):
        params = ["associative"]
        if transaction:
            params.append("transaction")
        if not rewrite:
            params += ["norwtime", "norwrandom"]
        if level:
            params.append("level=" + level)
        body = self._call("/db/request?" + "&".join(params), statements)
        if isinstance(body, dict) and body.get("error"):
            raise MigrateError(body["error"])
        return body.get("results") or []


def migration_files(directory: Path) -> list[Path]:
    return sorted(p for p in directory.glob("*.sql") if p.is_file())


def plan_file(path: Path):
    """Split one file and decide whether rqlite may rewrite it.

    Rewriting is turned off only for a file whose CREATE TRIGGER / VIEW body
    calls a non-deterministic function: rewriting would freeze that body.
    Everything else keeps the rewrite so replicas compute identical values.
    """
    statements = split_statements(path.read_text(encoding="utf-8"))
    frozen_bodies = [s for s in statements
                     if _DEFINES_BODY.match(_NOISE.sub("", s))
                     and _NONDETERMINISTIC.search(s)]
    warnings = []
    if frozen_bodies:
        for s in statements:
            body = _NOISE.sub("", s)
            if not _DEFINES_BODY.match(body) and _NONDETERMINISTIC.search(s):
                warnings.append(
                    "%s: sent without rqlite's rewrite (a trigger/view body "
                    "uses a non-deterministic function) but it also has "
                    "non-deterministic DML; replicas may disagree: %s" % (
                        path.name, " ".join(body.split())[:70]))
    return statements, not frozen_bodies, warnings


def existing_columns(db: Rqlite, table: str) -> set[str]:
    rows = db.request([["SELECT name FROM pragma_table_info(?)", table]])
    return {row["name"].lower() for row in (rows[0].get("rows") or [])}


def drop_satisfied_add_columns(db: Rqlite, path: Path, statements):
    """Skip an ``ALTER TABLE t ADD COLUMN c`` whose column already exists.

    A fresh database can already have the column when an earlier migration's
    CREATE TABLE was later edited to include it (0075 vs 0089 here). D1's
    long-lived production database never replays those files, so only a
    fresh database hits it; SQLite has no ``ADD COLUMN IF NOT EXISTS``.
    """
    kept, skipped = [], []
    cache: dict[str, set[str]] = {}
    for statement in statements:
        match = _ADD_COLUMN.match(_NOISE.sub("", statement))
        if match:
            table, column = match.group(1), match.group(2).lower()
            if table not in cache:
                cache[table] = existing_columns(db, table)
            if column in cache[table]:
                skipped.append("%s: skipped ALTER TABLE %s ADD COLUMN %s "
                               "(column already exists)" % (
                                   path.name, table, column))
                continue
        kept.append(statement)
    return kept, skipped


def applied_names(db: Rqlite) -> set[str]:
    db.request([[D1_MIGRATIONS_DDL]])
    rows = db.request([["SELECT name FROM d1_migrations"]], level="strong")
    return {row["name"] for row in (rows[0].get("rows") or [])}


def stamp(db: Rqlite, fingerprint: str, read_guard: str):
    statements = [[
        "INSERT INTO schema_meta (k, v) VALUES (?, 'applied') "
        "ON CONFLICT(k) DO UPDATE SET v = excluded.v",
        "schema_fingerprint:" + fingerprint,
    ]]
    if read_guard in ("auto", "on", "off"):
        statements.append([
            "INSERT INTO d1_read_guard (id, mode, updated_at, updated_by) "
            "VALUES (1, ?, ?, 'rqlite_migrate') "
            "ON CONFLICT(id) DO UPDATE SET mode = excluded.mode, "
            "updated_at = excluded.updated_at, "
            "updated_by = excluded.updated_by",
            read_guard, int(time.time() * 1000),
        ])
    db.request(statements, transaction=True)


def last_migration_stamp(files) -> str | None:
    pattern = re.compile(
        r"VALUES\s*\(\s*'schema_fingerprint:([0-9a-f]{64})'\s*,"
        r"\s*'applied'\s*\)", re.I | re.S)
    found = None
    for path in files:
        for match in pattern.findall(path.read_text(encoding="utf-8")):
            found = match.lower()
    return found


def _entry_list(name: str) -> list[str]:
    """A top-level list of SQL strings from entry.py, read without importing
    it (entry.py imports the Worker runtime)."""
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"))
    for node in tree.body:
        if (isinstance(node, ast.Assign) and len(node.targets) == 1
                and isinstance(node.targets[0], ast.Name)
                and node.targets[0].id == name):
            return list(ast.literal_eval(node.value))
    raise MigrateError("entry.py has no %s list" % name)


def app_schema_groups():
    """The app's own idempotent DDL, in the order entry._apply_schema runs it.

    The fingerprint stamp makes the app skip that DDL, but schema.py still
    creates tables no migration does (system_status_minute, ...). Production
    D1 has them from earlier full applies; a fresh cluster needs them once.
    """
    namespace: dict = {}
    exec(compile((ENTRY.parent / "schema.py").read_text(encoding="utf-8"),
                 "schema.py", "exec"), namespace)
    return (
        (_entry_list("SCHEMA_PRE_CREATE_ALTER_STATEMENTS"), True),
        (list(namespace["SCHEMA_STATEMENTS"]), False),
        (_entry_list("SCHEMA_ALTER_STATEMENTS"), True),
    )


SCHEMA_BATCH = 100


def apply_app_schema(db: Rqlite, log=print) -> dict:
    """Replay the app DDL. ALTERs that add an existing column (or touch a
    table that does not exist yet) are expected to fail and are counted."""
    applied = tolerated = 0
    for statements, tolerant in app_schema_groups():
        for sql in statements:
            if _DEFINES_BODY.match(sql) and _NONDETERMINISTIC.search(sql):
                raise MigrateError(
                    "app DDL defines a trigger/view with non-deterministic "
                    "SQL; replicas would diverge: %s" % sql[:120])
        for start in range(0, len(statements), SCHEMA_BATCH):
            chunk = statements[start:start + SCHEMA_BATCH]
            results = db.results([[sql] for sql in chunk])
            if len(results) != len(chunk):
                raise MigrateError("rqlite answered %d of %d app DDL "
                                   "statements" % (len(results), len(chunk)))
            for sql, item in zip(chunk, results):
                if not item.get("error"):
                    applied += 1
                elif tolerant:
                    tolerated += 1
                else:
                    raise MigrateError("statement failed: %s: %s" % (
                        item["error"], sql[:120]))
    log("app schema: %d statements applied, %d tolerated ALTER failures"
        % (applied, tolerated))
    return {"applied": applied, "tolerated": tolerated}


def migrate(url, user="", password="", directory=MIGRATIONS,
            force_follower=False, read_guard="off", list_only=False,
            log=print, app_schema=False):
    db = Rqlite(url, user, password)
    healthy, state = db.has_leader()
    if not healthy and not force_follower:
        raise MigrateError(
            "%s has no Raft leader (state %s); run against a node of a "
            "healthy cluster or pass --force-follower" % (url, state))
    files = migration_files(Path(directory))
    if not files:
        raise MigrateError("no migrations in %s" % directory)
    done = applied_names(db)
    pending = [p for p in files if p.name not in done]
    report = {"total": len(files), "already": len(files) - len(pending),
              "applied": [], "warnings": [], "seconds": 0.0}
    if list_only:
        report["pending"] = [p.name for p in pending]
        return report
    started = time.perf_counter()
    for path in pending:
        statements, rewrite, warnings = plan_file(path)
        statements, skipped = drop_satisfied_add_columns(db, path, statements)
        report["warnings"] += warnings + skipped
        applied_at = _dt.datetime.now(_dt.timezone.utc).strftime(
            "%Y-%m-%d %H:%M:%S")
        body = [[s] for s in statements]
        body.append([
            "INSERT INTO d1_migrations (name, applied_at) VALUES (?, ?)",
            path.name, applied_at])
        t0 = time.perf_counter()
        try:
            db.request(body, transaction=True, rewrite=rewrite)
        except MigrateError as error:
            raise MigrateError("%s failed: %s" % (path.name, error)) from None
        report["applied"].append(
            (path.name, len(statements), time.perf_counter() - t0))
        log("applied %s (%d statements%s)" % (
            path.name, len(statements), "" if rewrite else ", no rewrite"))
    if report["applied"] or app_schema:
        report["app_schema"] = apply_app_schema(db, log)
    fingerprint = runtime_schema_fingerprint()
    stamped = last_migration_stamp(files)
    if stamped and stamped != fingerprint:
        report["warnings"].append(
            "latest migration stamps %s but entry.py expects %s; the app "
            "will replay its schema once" % (stamped[:12], fingerprint[:12]))
    stamp(db, fingerprint, read_guard)
    report["fingerprint"] = fingerprint
    report["seconds"] = time.perf_counter() - started
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--url", default=os.environ.get(
        "RQLITE_URL", "http://127.0.0.1:4001"))
    parser.add_argument("--user", default=os.environ.get("RQLITE_USERNAME", ""))
    parser.add_argument("--password",
                        default=os.environ.get("RQLITE_PASSWORD", ""))
    parser.add_argument("--migrations", default=str(MIGRATIONS))
    parser.add_argument("--force-follower", action="store_true",
                        help="skip the cluster-has-a-leader check (tests only)")
    parser.add_argument("--d1-read-guard", default="off",
                        choices=("off", "on", "auto", "keep"))
    parser.add_argument("--list", action="store_true",
                        help="print pending migrations and exit")
    parser.add_argument("--app-schema", action="store_true",
                        help="replay the app DDL even if no migration is "
                        "pending (it always runs after new migrations)")
    args = parser.parse_args(argv)
    try:
        report = migrate(args.url, args.user, args.password, args.migrations,
                         args.force_follower, args.d1_read_guard, args.list,
                         app_schema=args.app_schema)
    except MigrateError as error:
        print("rqlite_migrate: %s" % error, file=sys.stderr)
        return 1
    if args.list:
        print("\n".join(report["pending"]) or "(nothing pending)")
        return 0
    for warning in report["warnings"]:
        print("warning: " + warning, file=sys.stderr)
    print("rqlite_migrate: %d applied, %d already applied, %.1fs; stamped "
          "schema_fingerprint:%s" % (
              len(report["applied"]), report["already"], report["seconds"],
              report["fingerprint"][:12]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
