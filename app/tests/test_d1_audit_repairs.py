"""D1 audit (#682) repairs: retired tables lose every reader, and a database
replayed from migrations alone has the tables schema.py used to create alone."""

import ast
import re
import sqlite3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
RETIRED = ("host_presence", "system_status_minute", "system_status_hourly",
           "system_status_daily")
CATCH_UP = ("agent_prompts", "ai_ask_rate", "catalog_rev", "clone_sticky",
            "issue_seq", "login_attempts", "online_hourly_nodes",
            "relay_self", "repo_agents", "repo_deletions",
            "repository_monitor_state", "signup_rate")
SQL_REF = re.compile(
    r"\b(?:FROM|INTO|UPDATE|JOIN|TABLE(?: IF NOT EXISTS)?)\s+(%s)\b"
    % "|".join(RETIRED), re.IGNORECASE)


def _sources():
    for base in (ROOT / "src", REPO / "world" / "src"):
        for path in sorted(base.rglob("*.py")):
            yield path


def test_no_app_or_world_sql_touches_a_retired_table():
    hits = []
    for path in _sources():
        for number, line in enumerate(
                path.read_text(encoding="utf-8").splitlines(), 1):
            if SQL_REF.search(line):
                hits.append("%s:%d" % (path.relative_to(REPO), number))
    assert hits == []


def _replayed():
    db = sqlite3.connect(":memory:")
    for path in sorted((ROOT / "migrations").glob("*.sql")):
        try:
            db.executescript(path.read_text(encoding="utf-8"))
        except sqlite3.OperationalError as error:
            if "duplicate column name" not in str(error):
                raise
    return {row[0] for row in db.execute(
        "SELECT name FROM sqlite_master WHERE type='table'")}


def test_migrations_alone_build_the_catch_up_tables_and_drop_dead_ones():
    tables = _replayed()
    assert set(CATCH_UP) <= tables
    assert "commit_inbox" not in tables
    assert "forkmesh_d1_migrations" not in tables


def _schema_statements():
    tree = ast.parse((ROOT / "src" / "schema.py").read_text(encoding="utf-8"))
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name)
                and target.id == "SCHEMA_STATEMENTS"
                for target in node.targets):
            return ast.literal_eval(node.value)
    raise AssertionError("missing SCHEMA_STATEMENTS")


def test_catch_up_migration_matches_the_runtime_ddl():
    schema = "\n".join(statement + ";" for statement in _schema_statements())
    migration = (ROOT / "migrations" / "0294_d1_audit_repairs.sql").read_text(
        encoding="utf-8")

    def columns(text, table):
        match = re.search(
            r"CREATE TABLE IF NOT EXISTS %s \((.*?)\);" % table, text,
            re.DOTALL)
        assert match, table
        return " ".join(match.group(1).split())

    for table in CATCH_UP:
        assert columns(migration, table) == columns(schema, table), table
