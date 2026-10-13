"""Test helper: compose the dashboard behaviour script from its JS fragments.

``public/dashboard.js`` is built from ordered fragment files under
``public/dashboard/js/`` before deploy. Frontend contract tests that assert on
the shipped dashboard script should read the assembled bundle via
``assembled_dashboard_js()``.
"""

import sys
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
_SRC = _ROOT / "src"
if str(_SRC) not in sys.path:
    sys.path.insert(0, str(_SRC))

import dashboard_bundle  # noqa: E402

PUBLIC = _ROOT / "public"


def assembled_dashboard_js() -> str:
    """The fully composed dashboard script (all public/dashboard/js fragments)."""
    return dashboard_bundle.compose_from_reader(
        lambda rel: (PUBLIC / rel).read_text(encoding="utf-8"))


def built_dashboard_js() -> str:
    """The served ``/dashboard.js``: the fragments minus the per-page chunks
    (``dashboard_bundle.LAZY_CHUNKS``), which ship as their own scripts."""
    import dashboard_shell
    return dashboard_bundle.compose_split_from_reader(
        lambda rel: (PUBLIC / rel).read_text(encoding="utf-8"),
        dashboard_shell.content_version)[0]


def shipped_dashboard_scripts() -> str:
    """Every dashboard script on disk: dashboard.js and its page chunks."""
    names = ["dashboard.js"] + sorted(
        p.name for p in PUBLIC.glob("dashboard-page-*.js"))
    return "\n".join((PUBLIC / name).read_text(encoding="utf-8")
                     for name in names)
