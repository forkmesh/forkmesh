import ast
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
# The World and Office route bodies moved into sibling modules so Cloudflare's
# Python startup validation never parses them. These contracts still span the
# whole route family, so their source is read alongside entry.py.
WORLD_ROUTE_SOURCE = "".join(
    (Path(__file__).resolve().parents[2] / "world" / "src" / name).read_text(
        encoding="utf-8")
    for name in ("world_routes.py", "world_office_routes.py")
)
ENTRY = (
    (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
    + WORLD_ROUTE_SOURCE
)
SCHEMA = (ROOT / "src" / "schema.py").read_text(encoding="utf-8")
DASHBOARD = (
    ROOT / "public" / "dashboard" / "js" / "04-account.js"
).read_text(encoding="utf-8")
MIGRATION = (
    ROOT / "migrations" / "0107_org_bot_token_usage.sql"
).read_text(encoding="utf-8")


def _definition(name):
    """One top-level definition's source, wherever the Worker keeps it."""
    for node in ast.parse(ENTRY).body:
        if getattr(node, "name", None) == name:
            return ast.get_source_segment(ENTRY, node)
    raise AssertionError("%s not found in the Worker sources" % name)


def test_bot_usage_log_is_metadata_only_and_retained_with_a_bound():
    for source in (MIGRATION, SCHEMA):
        assert "CREATE TABLE IF NOT EXISTS org_bot_token_usage" in source
        for column in ("token_id", "org_bi", "provider", "action", "method", "used_at"):
            assert column in source
    columns = MIGRATION[
        MIGRATION.index("CREATE TABLE IF NOT EXISTS"):
        MIGRATION.index(");", MIGRATION.index("CREATE TABLE IF NOT EXISTS"))
    ].lower()
    for forbidden in ("secret", "request_body", "query_string", "ip_address", "user_agent"):
        assert forbidden not in columns
    assert "DELETE FROM org_bot_token_usage WHERE used_at<?" in ENTRY


def test_successful_bot_authentication_appends_usage_without_request_content():
    helper = ENTRY[
        ENTRY.index("async def _org_bot_token_context("):
        ENTRY.index("\n\nasync def org_bot_tokens_handler", ENTRY.index(
            "async def _org_bot_token_context("))
    ]
    assert "INSERT INTO org_bot_token_usage" in helper
    assert "urlparse(request.url).path" in helper
    assert "searchParams" not in helper
    assert "request.text" not in helper
    assert "user-agent" not in helper.lower()


def test_org_admin_token_list_and_page_include_recent_usage_preview():
    assert "ORDER BY used_at DESC,id DESC LIMIT 5" in ENTRY
    assert '"usagePreview": usage_preview' in ENTRY
    assert "Recent token use" in DASHBOARD
    assert "secrets, bodies, network addresses, and user agents are never logged" in DASHBOARD


def test_scoped_bot_tokens_can_read_and_write_only_their_organization_tasks():
    handler = ENTRY[
        ENTRY.index("async def organization_tasks_handler("):
        ENTRY.index("\n\nworld_office_marketing_tasks_handler", ENTRY.index(
            "async def organization_tasks_handler("))
    ]
    runtime = _definition("_OfficeMarketingTasksRuntime")
    assert "invalid_bot_token" in handler
    assert 'authorization.lower().startswith("bearer fmbot_")' in handler
    assert '"organization.tasks.read"' in handler
    assert '"organization.tasks.write"' in handler
    assert '"requiredScope": required_scope' in handler
    assert "str(org_bi or \"\") != str(self.bot_context.get(\"orgBi\") or \"\")" in runtime
    assert 'return "admin", "maintain"' in runtime
