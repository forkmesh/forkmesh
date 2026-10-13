"""Once-daily, meaningful Fediverse digest policy."""

import sys
from pathlib import Path


SRC = Path(__file__).resolve().parents[1] / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import fediverse_digest as digest  # noqa: E402


def _event(title="Release 1.0", timestamp=1_000, **extra):
    return {
        "type": "release",
        "title": title,
        "url": "https://forkmesh.example/alice/repo/releases?secret=no#v1",
        "timestamp": timestamp,
        "scope": "alice/repo",
        **extra,
    }


def test_queue_requires_meaningful_public_update_and_drops_query_values():
    assert digest.add_event([], {}) == []
    assert digest.add_event([], _event(title="")) == []
    assert digest.add_event([], _event(url="file:///private/repo")) == []
    queued = digest.add_event([], _event())
    assert len(queued) == 1
    assert queued[0]["url"] == "https://forkmesh.example/alice/repo/releases#v1"
    assert set(queued[0]) == {
        "type", "title", "url", "timestamp", "scope", "id",
    }


def test_queue_deduplicates_and_is_bounded():
    queued = digest.add_event([], _event())
    queued = digest.add_event(queued, _event(timestamp=2_000))
    assert len(queued) == 1
    assert queued[0]["timestamp"] == 2_000
    for index in range(digest.MAX_PENDING_EVENTS + 20):
        queued = digest.add_event(
            queued, _event(title="Release %d" % index, timestamp=3_000 + index))
    assert len(queued) == digest.MAX_PENDING_EVENTS


def test_publish_gate_is_nonempty_and_at_most_daily():
    day = digest.DAY_MS
    assert not digest.digest_due([], 0, day)
    assert digest.digest_due([_event()], 0, day)
    assert not digest.digest_due([_event()], day, day + day - 1)
    assert digest.digest_due([_event()], day, day + day)


def test_digest_combines_updates_labels_automation_and_avoids_duplicates():
    first = digest.normalize_event(_event())
    second = digest.normalize_event(
        _event(title="Docs updated", type="documentation", timestamp=2_000))
    result = digest.build_digest([first, first, second], "alice/repo")
    assert result["eventCount"] == 2
    assert result["automated"] is True
    assert result["text"].count("Release 1.0") == 1
    assert "Docs updated" in result["text"]
    assert "Automated post" in result["text"]
    assert digest.build_digest([], "alice/repo") is None


def test_controls_are_per_scope_previewable_and_never_bidirectional_magic():
    assert digest.normalize_controls({}) == {
        "enabled": True,
        "preview": False,
        "cadence": "daily",
    }
    assert digest.normalize_controls({"enabled": False, "preview": True}) == {
        "enabled": False,
        "preview": True,
        "cadence": "daily",
    }


def test_internal_repository_event_reduces_body_to_public_bounded_metadata():
    event = digest.repository_event(
        "https://forkmesh.example/?ignored=1",
        "Alice",
        "Widget",
        "pull",
        "opened",
        "42",
        "",
        "  Add   routing support  \nPRIVATE BODY MUST NOT SURVIVE",
        1234,
    )
    assert event["type"] == "pull_request"
    assert event["scope"] == "alice/widget"
    assert event["url"] == (
        "https://forkmesh.example/alice/Widget/pulls/42")
    assert event["title"] == "Pull Request opened 42"
    assert "PRIVATE BODY" not in event["title"]
    assert set(event) == {
        "type", "title", "url", "timestamp", "scope", "id",
    }
    assert digest.repository_event(
        "file:///tmp/repo", "alice", "widget", "issue", "open", 1,
        "title", "", 1234,
    ) is None


def test_successful_digest_removes_only_published_ids():
    first = digest.normalize_event(_event(title="First"))
    second = digest.normalize_event(_event(title="Second", timestamp=2_000))
    assert digest.remove_published([first, second], [first["id"]]) == [second]
