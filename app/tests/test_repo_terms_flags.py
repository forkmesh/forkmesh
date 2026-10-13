"""Repository Terms flags are explicit, audited, and visible without notes."""

from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[1]
# entry.py plus its on-demand admin console (loaded outside Worker startup).
ENTRY = (
    (ROOT / "src" / "entry.py").read_text(encoding="utf-8") + "\n"
    + (ROOT / "src" / "admin_console.py").read_text(encoding="utf-8") + "\n"
    + (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
)
CATALOG_ROUTES = (ROOT / "src" / "catalog_routes.py").read_text(
    encoding="utf-8"
)
SCHEMA = (ROOT / "src" / "schema.py").read_text(encoding="utf-8")
DASHBOARD = (ROOT / "public" / "dashboard.js").read_text(encoding="utf-8")
WORLD = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()


def test_terms_flag_schema_and_admin_control_are_purpose_built():
    migration = (
        ROOT / "migrations" / "0102_repo_terms_flags.sql"
    ).read_text(encoding="utf-8")
    for source in (migration, SCHEMA):
        assert "CREATE TABLE IF NOT EXISTS repo_terms_flags" in source
        assert "spam" in source
        assert "malware" in source
    assert '"repo_terms_flags",' in ENTRY
    assert 'action="set_repo_terms_flag"' in ENTRY
    assert "await encrypt_row(self.env" in ENTRY
    assert "await purge_catalog_related_caches()" in ENTRY
    assert '"admin.console_" + action' in ENTRY


def test_public_catalog_projects_only_flag_and_category():
    assert (
        "SELECT repo_bi,category FROM repo_terms_flags WHERE active=1"
        in CATALOG_ROUTES
    )
    assert 'rec["termsFlagged"] = bool(terms_category)' in CATALOG_ROUTES
    assert 'rec["termsCategory"] = terms_category' in CATALOG_ROUTES
    catalog_projection = CATALOG_ROUTES[
        CATALOG_ROUTES.index("async def catalog_handler"):
    ]
    assert 'rec["termsNote"]' not in catalog_projection


def test_dashboard_and_world_show_visible_terms_flags():
    for contract in (
        "function repositoryTermsBadge(repo",
        "Terms of Service moderation flag",
        "Terms flag",
    ):
        assert contract in DASHBOARD
    assert "termsFlagged: repo.termsFlagged === true" in WORLD
    assert "record.termsFlagged" in SCENE
    assert "repository-terms-flag:" in SCENE
