#!/usr/bin/env python3
"""Web dashboard Badge tab for pull requests (adhoc #44).

The PR detail page gets a fourth section tab, "Badge", rendering the same
visual fingerprint the worker attaches to federated PR-opened notes: one
file-type glyph tile per changed file with a green/red additions:deletions
bar, tiles clustered per directory under a labeled connector line.
"""

from _dashboard_bundle import assembled_dashboard_js


DASHBOARD_JS = assembled_dashboard_js()


def test_badge_tab_button_rendered_for_pulls():
    assert 'data-repo-record-tab="badge"' in DASHBOARD_JS
    # Uses the fingerprint glyph — the badge is the PR's visual fingerprint.
    tab = DASHBOARD_JS.index('data-repo-record-tab="badge"')
    assert 'data-lucide="fingerprint"' in DASHBOARD_JS[tab:tab + 300]


def test_badge_panel_is_an_isolated_tab_section():
    assert 'data-repo-record-panel="badge"' in DASHBOARD_JS
    assert "panel.dataset.repoRecordPanel !== tab" in DASHBOARD_JS


def test_badge_renderer_shape():
    assert "function renderRepoPullBadge(" in DASHBOARD_JS
    assert "renderRepoPullBadge(title, number, author, pullPatch.files)" in DASHBOARD_JS
    # Tiles carry the per-file ratio bar in badge green/red.
    assert "function renderPullBadgeTile(" in DASHBOARD_JS
    assert "background:#3fb950" in DASHBOARD_JS
    assert "background:#f85149" in DASHBOARD_JS
    # Files cluster by directory with a compact "…N" count under the run
    # (no parens, no "files" word).
    start = DASHBOARD_JS.index("function renderRepoPullBadge(")
    body = DASHBOARD_JS[start:start + 4000]
    assert "lastIndexOf(\"/\")" in body
    assert "Files changed" in body
    assert "…${formatCount(group.files.length)}" in body
    # The label shows just the folder name (not the whole path), capped to
    # 8 chars, e.g. "one/two/three" -> "three".
    assert "function dirBadgeLabel(" in DASHBOARD_JS
    assert "dirBadgeLabel(group.dir)" in body


def test_badge_glyph_mapping_present_with_fallback():
    assert "function fileBadgeGlyph(" in DASHBOARD_JS
    start = DASHBOARD_JS.index("const fileBadgeStyles")
    body = DASHBOARD_JS[start:DASHBOARD_JS.index(
        "function renderPullBadgeTile", start)]
    # A few representative mappings shared with pull_badge.py.
    for token in ('ts: ["TS", "#3178c6"]', 'py: ["PY", "#4b8bbe"]',
                  'md: ["MD", "#519aba"]', 'dockerfile: ["DOCK", "#2496ed"]'):
        assert token in body
    # Unknown extensions still render a readable tile.
    assert '"FILE"' in DASHBOARD_JS[start:start + 6000]
