import importlib.util
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests" / "fixtures" / "activitypub" / "lemmy"
SPEC = importlib.util.spec_from_file_location(
    "activitypub_threads", ROOT / "src" / "activitypub_threads.py"
)
threads = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(threads)


def fixture(name):
    return json.loads((FIXTURES / name).read_text(encoding="utf-8"))


def apply_fixture(name):
    data = fixture(name)
    state = {}
    results = []
    for index, activity in enumerate(data["activities"]):
        result = threads.apply_activity(
            state,
            activity,
            remote_actor=data["remoteActor"],
            context=data["context"],
            received_ms=1000 + index,
        )
        results.append(result)
        state = result["records"]
    return data, state, results


def test_lemmy_issue_create_and_update_preserve_non_native_provenance():
    _, state, results = apply_fixture("issue-thread.json")
    assert [result["reason"] for result in results] == ["create", "update"]
    record = state["https://lemmy.example/comment/901"]
    assert record["body"] == "This reproduces on Linux and FreeBSD."
    assert record["lifecycle"] == "edited"
    assert record["context"] == {
        "owner": "forkmesh",
        "repo": "forkmesh",
        "kind": "issue",
        "ref": "42",
    }
    assert record["sourceSoftware"] == "lemmy"
    assert record["provenance"]["nativeEvent"] is False
    assert record["provenance"]["nativeEventId"] == ""
    assert record["provenance"]["signatureVerified"] is False
    assert record["storageNamespace"] == "federatedReplies"


def test_lemmy_pull_comment_delete_is_content_free_tombstone_with_backlink():
    _, state, _ = apply_fixture("pull-comment-thread.json")
    record = state["https://lemmy.dev/comment/300"]
    assert record["lifecycle"] == "tombstoned"
    assert record["tombstone"] is True
    assert record["body"] == ""
    assert record["backlink"] == "https://lemmy.dev/comment/300"
    projected = threads.thread_projection(state)
    assert projected[0]["tombstone"] is True
    assert projected[0]["nativeEvent"] is False


def test_lemmy_discussion_nested_reply_and_moderation_keep_thread_shape():
    _, state, _ = apply_fixture("discussion-thread.json")
    projected = threads.thread_projection(state)
    by_id = {item["remoteId"]: item for item in projected}
    root = by_id["https://community.example/comment/501"]
    nested = by_id["https://community.example/comment/502"]
    assert root["depth"] == 0
    assert nested["depth"] == 1
    assert nested["moderated"] is True
    assert nested["body"] == ""
    assert nested["url"] == "https://community.example/comment/502"


def test_duplicate_delivery_is_idempotent_and_uses_stable_dedupe_key():
    data = fixture("issue-thread.json")
    activity = data["activities"][0]
    first = threads.apply_activity(
        {}, activity, data["remoteActor"], data["context"], 1000
    )
    second = threads.apply_activity(
        first["records"], activity, data["remoteActor"], data["context"], 2000
    )
    assert first["changed"] is True
    assert second["changed"] is False
    assert second["reason"] == "duplicate"
    assert len(second["records"]) == 1
    record = second["record"]
    assert record["dedupeKey"] == threads.dedupe_key(record["remoteId"])
    assert record["receivedAt"] == 1000


def test_instance_blocks_cover_subdomains_and_do_not_store_delivery():
    data = fixture("issue-thread.json")
    result = threads.apply_activity(
        {},
        data["activities"][0],
        data["remoteActor"],
        data["context"],
        1000,
        blocked_instances=("example", "lemmy.example"),
    )
    assert result["ok"] is False
    assert result["reason"] == "instance_blocked"
    assert result["records"] == {}
    assert threads.instance_is_blocked("sub.lemmy.example", ["lemmy.example"])
    assert not threads.instance_is_blocked("notlemmy.example", ["lemmy.example"])


def test_remote_context_cannot_be_self_asserted_and_author_must_match():
    data = fixture("issue-thread.json")
    activity = data["activities"][0]
    assert threads.normalize_activity(
        activity, data["remoteActor"], context=None
    )["reason"] == "unresolved_context"
    forged = json.loads(json.dumps(activity))
    forged["object"]["attributedTo"] = "https://lemmy.example/u/mallory"
    assert threads.normalize_activity(
        forged, data["remoteActor"], data["context"]
    )["reason"] == "author_mismatch"


def test_untrusted_html_is_plain_text_and_non_https_ids_are_rejected():
    data = fixture("issue-thread.json")
    activity = json.loads(json.dumps(data["activities"][0]))
    activity["object"]["content"] = (
        "<script>steal()</script><p>Hello &amp; welcome</p><br><b>friend</b>"
    )
    normalized = threads.normalize_activity(
        activity, data["remoteActor"], data["context"]
    )
    assert normalized["record"]["body"] == "Hello & welcome\nfriend"
    activity["object"]["id"] = "http://lemmy.example/comment/901"
    assert threads.normalize_activity(
        activity, data["remoteActor"], data["context"]
    )["reason"] == "invalid_identity"


def test_lemmy_page_and_article_objects_use_the_same_thread_boundary():
    data = fixture("issue-thread.json")
    for object_type in ("Page", "Article"):
        activity = json.loads(json.dumps(data["activities"][0]))
        activity["object"]["type"] = object_type
        activity["object"]["id"] = (
            "https://lemmy.example/post/" + object_type.lower()
        )
        normalized = threads.normalize_activity(
            activity, data["remoteActor"], data["context"]
        )
        assert normalized["ok"] is True
        assert normalized["record"]["sourceSoftware"] == "lemmy"
        assert normalized["record"]["provenance"]["nativeEvent"] is False


def test_projection_omits_blocked_instances_but_keeps_other_replies():
    data, state, _ = apply_fixture("discussion-thread.json")
    other = fixture("issue-thread.json")
    extra = threads.apply_activity(
        state,
        other["activities"][0],
        other["remoteActor"],
        other["context"],
        2000,
    )
    projected = threads.thread_projection(
        extra["records"], blocked_instances=("community.example",)
    )
    assert [item["sourceInstance"] for item in projected] == ["lemmy.example"]
    assert all(item["nativeEvent"] is False for item in projected)
