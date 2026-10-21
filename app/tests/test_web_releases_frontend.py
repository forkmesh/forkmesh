#!/usr/bin/env python3
"""Releases tab: draft, edit and delete releases in the browser."""

import re

from _dashboard_bundle import assembled_dashboard_js


DASHBOARD_JS = assembled_dashboard_js()


def _function(name):
    match = re.search(r"\n  (?:async )?function %s\(" % re.escape(name), DASHBOARD_JS)
    assert match, name
    return DASHBOARD_JS[match.start():DASHBOARD_JS.index("\n  }\n", match.start()) + 4]


def test_writers_get_the_draft_button_and_edit_controls_on_web_releases():
    assert '<button type="button" data-repo-release-new hidden' in DASHBOARD_JS
    manageable = _function("repoReleasesManageable")
    assert "sessionOwnsRepo(repo)" in manageable
    assert 'permission !== "read"' in manageable
    card = _function("renderRepoRelease")
    assert 'state.repoReleasesManageable === true && release.source === "web"' in card
    assert "data-repo-release-edit" in card and "data-repo-release-delete" in card
    # Notes are Markdown, like everywhere else on a repository page.
    assert "renderRecordMarkdown(notes, repo)" in card


def test_the_form_posts_to_release_edit():
    form = _function("renderReleaseForm")
    for marker in ("data-repo-release-tag", "data-repo-release-target",
                   "data-repo-release-title", "data-repo-release-notes",
                   'data-repo-release-tab="preview"', "data-repo-release-prerelease"):
        assert marker in form
    submit = _function("handleReleaseFormSubmit")
    assert 'op: "save"' in submit and "await loadRepoReleases(repo)" in submit
    assert "/release-edit" in _function("releaseEditRequest")
    assert 'op: "delete"' in _function("handleReleaseDelete")


def test_the_page_wires_release_controls():
    for selector in ('"[data-repo-release-form]"',
                     '"[data-repo-release-new], [data-repo-release-edit]"',
                     '"[data-repo-release-delete]"', '"[data-repo-release-cancel]"',
                     '"[data-repo-release-tab]"'):
        assert "event.target.closest(%s)" % selector in DASHBOARD_JS


def test_files_can_be_attached_to_and_removed_from_web_releases():
    card = _function("renderRepoRelease")
    assert 'repoLiveUrl(repo, "raw", { path: String(asset.path) })' in card
    assert "data-repo-release-asset-upload" in card and "data-repo-release-asset-remove" in card
    upload = _function("handleReleaseAssetUpload")
    assert 'op: "asset"' in upload and "file.size > 2 * 1024 * 1024" in upload
    assert 'op: "asset-delete"' in _function("handleReleaseAssetRemove")
    assert 'event.target.closest?.("[data-repo-release-asset-upload]")' in DASHBOARD_JS
