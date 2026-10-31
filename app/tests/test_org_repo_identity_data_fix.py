import sqlite3
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))
from forkmesh_host import org_repo_identity  # noqa: E402


def _db():
    conn = sqlite3.connect(":memory:")
    conn.executescript("""
        CREATE TABLE repo_stars (repo_bi TEXT, account_bi TEXT,
            created_at INTEGER, PRIMARY KEY (repo_bi, account_bi));
        CREATE TABLE repo_views (repo_bi TEXT NOT NULL, viewer_key TEXT
            NOT NULL, first_at INTEGER NOT NULL, last_at INTEGER NOT NULL,
            counted_at INTEGER NOT NULL DEFAULT 0, hits INTEGER NOT NULL
            DEFAULT 1, PRIMARY KEY (repo_bi, viewer_key)) WITHOUT ROWID;
        CREATE TABLE repo_view_totals (repo_bi TEXT PRIMARY KEY,
            views INTEGER NOT NULL DEFAULT 0 CHECK (views >= 0),
            viewers INTEGER NOT NULL DEFAULT 0 CHECK (viewers >= 0),
            last_viewed_at INTEGER NOT NULL DEFAULT 0);
        CREATE TABLE issue_views (repo_bi TEXT NOT NULL, number INTEGER NOT
            NULL, viewer_key TEXT NOT NULL, first_at INTEGER NOT NULL,
            last_at INTEGER NOT NULL, counted_at INTEGER NOT NULL DEFAULT 0,
            hits INTEGER NOT NULL DEFAULT 1,
            PRIMARY KEY (repo_bi, number, viewer_key)) WITHOUT ROWID;
        CREATE TABLE issue_view_totals (repo_bi TEXT NOT NULL, number INTEGER
            NOT NULL, views INTEGER NOT NULL DEFAULT 0, viewers INTEGER NOT
            NULL DEFAULT 0 CHECK (viewers >= 0), last_viewed_at INTEGER NOT
            NULL DEFAULT 0, PRIMARY KEY (repo_bi, number)) WITHOUT ROWID;
    """)
    return conn


def _apply(conn, statements):
    with conn:
        for sql, args in statements:
            conn.execute(sql, args)


def test_star_rows_move_once_and_duplicates_collapse():
    conn = _db()
    conn.executemany("INSERT INTO repo_stars VALUES (?,?,?)", [
        ("old", "a", 1), ("old", "b", 2), ("new", "b", 3)])
    statements = org_repo_identity._set_statements(
        "repo_stars", ("account_bi",), "old", "new")
    _apply(conn, statements)
    _apply(conn, statements)
    assert sorted(conn.execute("SELECT * FROM repo_stars")) == [
        ("new", "a", 1), ("new", "b", 3)]


def test_views_merge_per_viewer_and_totals_add_up():
    conn = _db()
    conn.executemany("INSERT INTO repo_views VALUES (?,?,?,?,?,?)", [
        ("old", "v1", 10, 20, 0, 2), ("old", "v2", 5, 6, 0, 1),
        ("new", "v1", 3, 30, 0, 4)])
    conn.executemany("INSERT INTO repo_view_totals VALUES (?,?,?,?)", [
        ("old", 3, 2, 20), ("new", 4, 1, 30)])
    conn.executemany("INSERT INTO issue_views VALUES (?,?,?,?,?,?,?)", [
        ("old", 7, "v1", 1, 2, 0, 1), ("new", 7, "v1", 1, 3, 0, 1),
        ("old", 8, "v2", 1, 2, 0, 1)])
    conn.executemany("INSERT INTO issue_view_totals VALUES (?,?,?,?,?)", [
        ("old", 7, 1, 1, 2), ("new", 7, 1, 1, 3), ("old", 8, 1, 1, 2)])
    for totals, views, keys in org_repo_identity.VIEW_TABLES:
        statements = org_repo_identity._view_statements(
            totals, views, keys, "old", "new")
        _apply(conn, statements)
        _apply(conn, statements)
    assert sorted(conn.execute("SELECT * FROM repo_views")) == [
        ("new", "v1", 3, 30, 0, 6), ("new", "v2", 5, 6, 0, 1)]
    assert list(conn.execute("SELECT * FROM repo_view_totals")) == [
        ("new", 7, 2, 30)]
    assert sorted(conn.execute("SELECT * FROM issue_view_totals")) == [
        ("new", 7, 2, 1, 3), ("new", 8, 1, 1, 2)]
    assert sorted(conn.execute(
        "SELECT repo_bi, number, viewer_key, hits FROM issue_views")) == [
        ("new", 7, "v1", 2), ("new", 8, "v2", 1)]
