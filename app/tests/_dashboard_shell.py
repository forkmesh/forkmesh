"""Test helper: compose the per-page dashboard documents from HTML partials.

``public/dashboard/shell.html`` is the authored scaffold with ``<!--#include
partial="name"-->`` / ``<!--#page token-->`` placeholders and one view file per
page under ``public/dashboard/partials/views/``. ``tools/build_dashboard_assets.py``
stitches one document per ``dashboard_shell.PAGES`` entry into the static files
browsers receive. Frontend contract tests can read a single page via
``assembled_dashboard_page(page_id)`` or every page concatenated via
``assembled_dashboard()`` (for "marker exists somewhere in the dashboard"
assertions).
"""

import sys
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
_SRC = _ROOT / "src"
if str(_SRC) not in sys.path:
    sys.path.insert(0, str(_SRC))

import dashboard_shell  # noqa: E402

PUBLIC = _ROOT / "public"
WWW_PUBLIC = _ROOT.parent / "www" / "public"
PACKAGED_PAGES = {
    "tasks": ("Tasks", "The organization-private task catalog."),
    "calendar": ("Calendar", "Private personal and organization calendars."),
    "notes": ("Notes", "Collaborative Markdown notes."),
}


def _read(rel: str) -> str:
    source = PUBLIC / rel
    if not source.is_file():
        source = WWW_PUBLIC / rel
    return source.read_text(encoding="utf-8")


def assembled_dashboard_page(page_id: str) -> str:
    """One fully composed page document (shell + chrome partials + its view)."""
    if page_id in PACKAGED_PAGES:
        title, description = PACKAGED_PAGES[page_id]
        package_view = (
            _ROOT.parent / "marketplace" / "packages" / page_id
            / "content" / "web" / "view.html"
        ).read_text(encoding="utf-8")
        return dashboard_shell.compose_page_from_reader(
            _read,
            page_id,
            page={
                "view": "",
                "section": page_id,
                "nav": page_id,
                "route": "/dashboard/" + page_id,
                "asset": "dashboard-plugins/%s/default/index.html" % page_id,
                "title": title + " - ForkMesh",
                "description": description,
            },
            view=package_view,
        )
    return dashboard_shell.compose_page_from_reader(_read, page_id)


def assembled_dashboard() -> str:
    """Every page document concatenated, in PAGES order.

    Kept for the many contract tests that only assert a marker exists somewhere
    in the dashboard chrome or views.
    """
    return "".join(
        assembled_dashboard_page(page)
        for page in (*dashboard_shell.PAGES, *PACKAGED_PAGES)
    )
