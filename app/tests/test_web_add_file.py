#!/usr/bin/env python3
"""Add file > Create new file works in the browser (no desktop needed)."""

import re

from _dashboard_bundle import assembled_dashboard_js


DASHBOARD_JS = assembled_dashboard_js()


def _function(name):
    match = re.search(r"\n  (?:async )?function %s\(" % re.escape(name), DASHBOARD_JS)
    assert match, name
    return DASHBOARD_JS[match.start():DASHBOARD_JS.index("\n  }\n", match.start()) + 4]


def test_the_add_file_buttons_are_live_controls():
    assert 'aria-disabled="true" class="inline-flex h-9 items-center justify-center gap-2 rounded-md border border-border bg-secondary px-3 text-xs font-semibold text-foreground hover:bg-background"><i data-lucide="plus"' not in DASHBOARD_JS
    assert DASHBOARD_JS.count("${addFileButton(") == 2
    button = DASHBOARD_JS[DASHBOARD_JS.index("const addFileButton = (height) =>"):]
    button = button[:button.index("\n")]
    assert "data-repo-file-new" in button
    # A read-only mirror cannot take a commit; the button says why.
    assert "readOnlyMirror ?" in button and "REPO_READ_ONLY_MIRROR_HINT" in button


def test_the_click_opens_the_new_file_form_for_signed_in_people():
    click = DASHBOARD_JS[DASHBOARD_JS.index(
        'const fileNewButton = event.target.closest("[data-repo-file-new]");'):]
    click = click[:click.index("return;\n      }\n\n")]
    assert 'location.href = "/login?next="' in click
    assert "openRepoNewFile(state.selectedRepo)" in DASHBOARD_JS


def test_the_new_file_form_commits_or_proposes_with_create():
    form = _function("openRepoNewFile")
    assert "data-repo-new-file-path" in form
    assert "currentRepoBrowseFolder()" in form
    assert 'mode: "commit", path, branch, baseRef: currentBranch, content, message, create: true' in form
    assert 'mode: "propose", path, branch, content, message, create: true' in form
    assert "await proposeRepoFileEdit(viewer, repo, proposal)" in form
    assert "probeRepoFileEditAccess(viewer, editor, repo, controller.signal)" in form
    assert 'navigateHistory(repoLinkUrl(repo, "blob", path))' in form
    assert "A file with that name already exists" in form


def test_new_file_paths_are_normalized():
    import json
    import subprocess
    script = _function("normalizedNewFilePath") + (
        "console.log(JSON.stringify(['/docs//a.md', ' docs\\\\b.md ', 'c.md']"
        ".map(normalizedNewFilePath)))")
    out = subprocess.run(["node", "-e", script], check=True,
                         capture_output=True, text=True).stdout
    assert json.loads(out) == ["docs/a.md", "docs/b.md", "c.md"]
