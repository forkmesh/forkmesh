#!/usr/bin/env python3
"""Build static dashboard assets from source partials/fragments.

The dashboard remains authored as an HTML shell plus partials and ordered JS
fragments, but production must serve the fully composed assets directly from
Cloudflare Static Assets. Building them here keeps hot dashboard requests out
of the Python Worker runtime.
"""

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
PUBLIC = ROOT.parent / "app" / "public"

if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import dashboard_bundle  # noqa: E402
import dashboard_shell  # noqa: E402


def _read(rel):
    return (PUBLIC / rel).read_text(encoding="utf-8")


def _write_if_changed(root, rel, text):
    path = root / rel
    old = path.read_text(encoding="utf-8") if path.exists() else None
    if old == text:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return True


def main():
    dashboard_js, chunks = dashboard_bundle.compose_split_from_reader(
        _read, dashboard_shell.content_version)
    app_outputs = {
        meta["asset"]: dashboard_shell.compose_page_from_reader(_read, page_id)
        for page_id, meta in dashboard_shell.PAGES.items()
    }
    app_outputs.update(
        (meta["asset"], dashboard_shell.compose_page_from_reader(
            _read, page_id, page=meta))
        for page_id, meta in dashboard_shell.PACKAGED_PAGE_COPIES.items()
    )
    app_outputs["dashboard.js"] = dashboard_js
    app_outputs.update(chunks)
    changed = [
        "app/public/" + rel
        for rel, text in sorted(app_outputs.items())
        if _write_if_changed(PUBLIC, rel, text)
    ]
    for stale in PUBLIC.glob("dashboard-page-*.js"):
        if stale.name not in chunks:
            stale.unlink()
            changed.append("app/public/%s (removed)" % stale.name)
    # The pre-split SPA duplicate: /dashboard.html routes are gone, the per-page
    # documents replace it. Drop a stale copy left by older builds.
    legacy = PUBLIC / "dashboard.html"
    if legacy.exists():
        legacy.unlink()
        changed.append("app/public/dashboard.html (removed)")
    if changed:
        print("Built dashboard assets: " + ", ".join(changed))
    else:
        print("Dashboard assets are up to date.")


if __name__ == "__main__":
    main()
