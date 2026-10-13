import sys
from pathlib import Path

APP = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(APP / "src"))

import static_routes  # noqa: E402


def test_bounced_repo_deep_link_still_moves_to_clean_path():
    assert static_routes.dashboard_repo_redirect(
        "/dashboard/alice/tool") == "/alice/tool"
    assert static_routes.dashboard_repo_redirect(
        "/dashboard/alice/tool/tree/main") == "/alice/tool/tree/main"


def test_bounced_repo_redirect_never_leaves_the_origin():
    for path in (
        "/dashboard//evil.example/tool",
        "/dashboard//evil.example/tool/tree/main",
        "/dashboard///evil.example/tool",
        "/dashboard/\\evil.example/tool",
        "/dashboard/\\\\evil.example/tool",
        "/dashboard/\tevil.example/tool",
    ):
        assert static_routes.dashboard_repo_redirect(path) is None, path


def test_router_uses_the_guarded_redirect():
    source = (APP / "src/app_runtime.py").read_text(encoding="utf-8")
    assert "rest = dashboard_repo_redirect(url.path)" in source
    assert "if looks_like_repo_route(rest):" not in source
    entry = (APP / "src/entry.py").read_text(encoding="utf-8")
    assert 'dashboard_repo_redirect = _static_routes.export(' in entry
