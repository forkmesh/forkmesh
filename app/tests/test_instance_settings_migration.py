import sqlite3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_migration_creates_and_drops_the_settings_table():
    up = (ROOT / "migrations" / "0253_instance_settings.sql").read_text()
    down = (ROOT / "migrations" / "down" / "0253_instance_settings.sql").read_text()
    db = sqlite3.connect(":memory:")
    db.execute("CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT)")
    db.executescript(up)
    db.execute("INSERT INTO instance_settings (k, doc, updated_at) VALUES ('theme', '{}', 1)")
    db.executescript(down)
    assert not db.execute(
        "SELECT name FROM sqlite_master WHERE name='instance_settings'").fetchall()
