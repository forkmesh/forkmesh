"""ActivityPub schema upgrades must never erase the durable social graph."""

from pathlib import Path
import ast
import re
import sqlite3
from types import SimpleNamespace
from urllib.parse import urlparse


ROOT = Path(__file__).resolve().parents[1]
MIGRATIONS = ROOT / "migrations"
FEDIVERSE_ROUTES_PATH = ROOT / "src" / "fediverse_routes.py"
ENTRY = FEDIVERSE_ROUTES_PATH.read_text(encoding="utf-8")


def test_official_fediverse_origins_converge_on_the_public_profile_domain():
    tree = ast.parse(ENTRY, filename=str(FEDIVERSE_ROUTES_PATH))
    node = next(
        item for item in tree.body
        if isinstance(item, ast.FunctionDef) and item.name == "_ap_origin"
    )
    namespace = {"urlparse": urlparse}
    exec(compile(ast.fix_missing_locations(ast.Module(
        body=[node], type_ignores=[])), str(FEDIVERSE_ROUTES_PATH), "exec"),
         namespace)
    origin = namespace["_ap_origin"]
    empty = SimpleNamespace(PUBLIC_BASE_URL="")
    for host in (
            "app.forkmesh.com", "forkmesh.com", "world.forkmesh.com"):
        request = SimpleNamespace(url="https://%s/@alice" % host)
        assert origin(empty, request) == "https://forkmesh.com"
    custom = SimpleNamespace(PUBLIC_BASE_URL="https://mesh.example")
    assert origin(custom, SimpleNamespace(
        url="https://app.forkmesh.com/@alice")) == "https://mesh.example"
    assert origin(empty, SimpleNamespace(
        url="https://relay.example/@alice")) == "https://relay.example"


def test_repository_actor_migration_preserves_every_follower_and_actor():
    db = sqlite3.connect(":memory:")
    db.executescript(
        (MIGRATIONS / "0028_activitypub.sql").read_text(encoding="utf-8")
    )
    db.executemany(
        "INSERT INTO ap_actors "
        "(actor_bi,kind,pubkey_pem,data,created_at) VALUES (?,?,?,?,?)",
        [
            ("repo-bi", "repo", "repo-pub", "repo-data", 1),
            ("user-bi", "user", "user-pub", "user-data", 2),
            ("instance-bi", "instance", "instance-pub", "instance-data", 3),
        ],
    )
    db.executemany(
        "INSERT INTO ap_followers "
        "(actor_bi,follower_id,inbox,shared_inbox,follower_handle,created_at) "
        "VALUES (?,?,?,?,?,?)",
        [
            ("repo-bi", "https://social.test/users/repo-fan",
             "https://social.test/users/repo-fan/inbox", None,
             "@repo-fan@social.test", 4),
            ("user-bi", "https://social.test/users/user-fan",
             "https://social.test/users/user-fan/inbox", None,
             "@user-fan@social.test", 5),
        ],
    )

    db.executescript(
        (MIGRATIONS / "0079_activitypub_repository_actors_only.sql")
        .read_text(encoding="utf-8")
    )

    assert db.execute(
        "SELECT actor_bi,follower_id FROM ap_followers ORDER BY actor_bi"
    ).fetchall() == [
        ("repo-bi", "https://social.test/users/repo-fan"),
        ("user-bi", "https://social.test/users/user-fan"),
    ]
    assert db.execute(
        "SELECT actor_bi,kind FROM ap_actors ORDER BY actor_bi"
    ).fetchall() == [
        ("instance-bi", "instance"),
        ("repo-bi", "repo"),
        ("user-bi", "user"),
    ]
    assert db.execute(
        "SELECT key_name,pubkey_pem,data FROM ap_service_keys"
    ).fetchall() == [("instance", "instance-pub", "instance-data")]


def test_startup_actor_reconciliation_is_non_destructive():
    tree = ast.parse(ENTRY, filename=str(FEDIVERSE_ROUTES_PATH))
    node = next(
        item for item in tree.body
        if isinstance(item, ast.AsyncFunctionDef)
        and item.name == "_ap_prune_actor_inventory"
    )
    body = ast.get_source_segment(ENTRY, node)
    assert "DELETE FROM ap_followers" not in body
    assert "DELETE FROM ap_objects" not in body
    assert "DELETE FROM ap_actors" not in body


def test_no_migration_can_delete_or_drop_the_follower_graph():
    destructive = re.compile(
        r"\b(?:DELETE\s+FROM|DROP\s+TABLE(?:\s+IF\s+EXISTS)?)"
        r"\s+ap_followers\b",
        re.IGNORECASE,
    )
    offenders = [
        path.name
        for path in MIGRATIONS.glob("*.sql")
        if destructive.search(path.read_text(encoding="utf-8"))
    ]
    assert offenders == []
