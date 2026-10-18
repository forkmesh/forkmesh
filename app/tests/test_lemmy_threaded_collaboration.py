"""End-to-end source contracts for Lemmy-compatible collaboration surfaces."""

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WORKER = ROOT / "app"
FIXTURES = WORKER / "tests" / "fixtures" / "activitypub" / "lemmy"


def text(path):
    return path.read_text(encoding="utf-8")


def test_fixtures_cover_issue_pull_comment_and_nested_discussion_lifecycles():
    issue = json.loads(text(FIXTURES / "issue-thread.json"))
    pull = json.loads(text(FIXTURES / "pull-comment-thread.json"))
    discussion = json.loads(text(FIXTURES / "discussion-thread.json"))

    assert issue["context"]["kind"] == "issue"
    assert [item["type"] for item in issue["activities"]] == [
        "Create",
        "Update",
    ]
    assert pull["context"]["kind"] == "pull"
    assert [item["type"] for item in pull["activities"]] == [
        "Create",
        "Delete",
    ]
    assert discussion["context"]["kind"] == "discussion"
    assert discussion["activities"][1]["object"]["inReplyTo"] == \
        discussion["activities"][0]["object"]["id"]
    assert discussion["activities"][2]["type"] == "Remove"


def test_worker_dispatches_lifecycle_handlers_and_keeps_separate_storage():
    entry_loader = text(WORKER / "src" / "entry.py")
    entry = entry_loader + "\n" + text(WORKER / "src" / "fediverse_routes.py")
    module = text(WORKER / "src" / "activitypub_threads.py")
    schema = text(WORKER / "src" / "schema.py")
    migration = text(
        WORKER / "migrations" / "0065_activitypub_thread_lifecycle.sql"
    )

    assert 'ap_threads = _LazyModule("activitypub_threads")' in entry_loader
    assert 'if activity_type == "Update":' in entry
    assert 'if activity_type == "Remove":' in entry
    assert "UPDATE ap_comments SET lifecycle=?, data=?" in entry
    assert "ap_threads.thread_projection" in entry
    assert "STORAGE_NAMESPACE = \"federatedReplies\"" in module
    assert '"nativeEvent": False' in module
    assert "parent_remote_id_bi" in schema
    assert "parent_remote_id_bi" in migration
    assert "lifecycle" in migration
    # Delete is a content-free lifecycle mutation, not a hard delete that could
    # orphan nested replies and not a native event-store write.
    delete_start = entry.index("async def _ap_handle_delete")
    delete_end = entry.index("async def ap_inbox_handler", delete_start)
    delete_handler = entry[delete_start:delete_end]
    assert "UPDATE ap_comments" in delete_handler
    assert "DELETE FROM ap_comments" not in delete_handler
    assert "IssueStore" not in delete_handler
    assert "PullStore" not in delete_handler
    assert "DiscussionStore" not in delete_handler


def test_world_web_native_and_qt_render_provenance_backlinks_as_non_native():
    world = text(ROOT / "world" / "public" / "world" / "world.js")
    dashboard = text(
        WORKER / "public" / "dashboard" / "js" / "06-repo-content.js"
    )
    android = ROOT / "native" / "android" / "core" / "src" / "main" / "kotlin"
    android = android / "com" / "forkmesh" / "nativeapp"
    android_model = text(android / "model" / "Notifications.kt")
    android_api = text(android / "api" / "ApiService.kt")
    android_ui = text(android / "collab" / "DiscussionThread.kt")
    ios = ROOT / "native" / "ios" / "ForkMeshCore" / "Sources"
    ios_api = text(ios / "FMApi" / "Services" / "ApiService+Repo.swift")
    ios_ui = text(ios / "FMCode" / "Collab" / "DiscussionThread.swift")
    qt_view = text(ROOT / "desktop" / "src" / "FederatedThreadView.cpp")
    qt_issue = text(ROOT / "desktop" / "src" / "MainWindowIssues.cpp")
    qt_pull = text(ROOT / "desktop" / "src" / "MainWindowPulls.cpp")
    qt_discussion = text(
        ROOT / "desktop" / "src" / "MainWindowDiscussions.cpp"
    )

    assert "data-world-fediverse-thread" in world
    assert "openFediverseThread(id)" in world
    assert "data-native-event=\"false\"" in world
    assert "not a signed ForkMesh native event" in world
    assert "data-repo-federated-replies" in dashboard
    assert "loadFederatedReplies(repo, kind, number" in dashboard
    assert "not a signed ${escapeHtml(platformName())} event" in dashboard
    assert "class FederatedReply" in android_model
    assert "nativeEvent = false" in android_model
    assert "fedi-comments" in android_api
    assert "not a signed native event" in android_ui
    assert "fedi-comments" in ios_api
    assert "not a signed native event" in ios_ui
    assert "fedi-comments" in qt_view
    assert "not a signed native event" in qt_view
    assert "new FederatedThreadView" in qt_issue
    assert "new FederatedThreadView" in qt_pull
    assert "new FederatedThreadView" in qt_discussion


def test_documentation_states_context_and_signature_boundaries():
    docs = text(ROOT / "www" / "docs" / "lemmy-threaded-collaboration.md")
    assert "A remote object cannot select a ForkMesh context" in docs
    assert "does not make the reply a ForkMesh native event" in docs
    assert "Undo(Remove)" in docs
    assert "Private repository" in docs
