import base64
import io
import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

PIL = pytest.importorskip("PIL.Image")

import testflight_feedback_tasks as tool  # noqa: E402


def screenshot_bytes() -> bytes:
    image = PIL.new("RGB", (1290, 2796))
    image.putdata([(x % 256, (x // 7) % 256, (x * 3) % 256) for x in range(1290 * 2796)])
    out = io.BytesIO()
    image.save(out, "JPEG", quality=95)
    return out.getvalue()


SHOT = screenshot_bytes()


def submission(ident, comment, created, shots=1):
    return {
        "id": ident,
        "attributes": {
            "createdDate": created, "comment": comment, "email": "t@example.com",
            "deviceModel": "iPhone16_2", "osVersion": "27.0", "locale": "en-US",
            "connectionType": "WIFI", "buildBundleId": "com.forkmesh.forkmesh",
            "screenshots": [{"url": f"https://tf-feedback.example/{ident}/{n}", "width": 1290,
                             "height": 2796, "expirationDate": "2026-10-16T00:00:00Z"}
                            for n in range(shots)],
        },
        "relationships": {"build": {"data": {"type": "builds", "id": "b3"}},
                          "tester": {"data": {"type": "betaTesters", "id": "t1"}}},
    }


INCLUDED = [{"type": "builds", "id": "b3", "attributes": {"version": "3"}},
            {"type": "betaTesters", "id": "t1", "attributes": {"firstName": "Jett", "lastName": "Vale"}}]


class FakeAsc:
    def __init__(self, screenshots, crashes=()):
        self.screenshots = list(screenshots)
        self.crashes = list(crashes)
        self.deleted = []
        self.missing = set()

    def __call__(self, method, path, body=None):
        if method == "DELETE":
            if path in self.missing:
                raise SystemExit(f"ERROR: App Store Connect DELETE {path} answered 404: gone")
            self.deleted.append(path)
            return {}
        assert method == "GET"
        if "betaFeedbackScreenshotSubmissions" in path:
            if "cursor=2" in path:
                return {"data": self.screenshots[1:], "included": INCLUDED}
            return {"data": self.screenshots[:1], "included": INCLUDED,
                    "links": {"next": tool.API + path + "&cursor=2"}}
        return {"data": self.crashes, "included": INCLUDED}


def board(*tasks, trashed=()):
    def tasks_for(status):
        return {"tasks": list(trashed if status == "deleted" else tasks)}
    return tasks_for


def task_for(kind, ident, done=False, task_id="t" * 32):
    return {"id": task_id, "title": "x", "completedAt": 5 if done else 0,
            "details": f"words\n\n{tool.MARKER} {kind} {ident}"}


def run(asc, tasks, dry_run=False):
    made = []

    def make(fields):
        made.append(fields)
        return {"id": f"{len(made):032x}"}

    status = tool.run(asc, "app", tasks, make, "forkmesh/forkmesh", dry_run, fetch=lambda url: SHOT)
    return status, made


def test_new_feedback_becomes_a_task_with_its_screenshots_under_the_body_limit(capsys):
    asc = FakeAsc([submission("A1", "Show time as 5:45pm format ", "2026-09-01T03:06:33Z"),
                   submission("A2", "", "2026-08-30T05:24:00Z", shots=3)])
    status, made = run(asc, board())
    assert status == 0
    assert [fields["title"] for fields in made] == [
        "TestFlight: feedback with no comment", "TestFlight: Show time as 5:45pm format"]
    first = made[1]
    assert first["repository"] == "forkmesh/forkmesh"
    assert first["details"].startswith("Show time as 5:45pm format")
    assert "- From: Jett Vale, t@example.com" in first["details"]
    assert "- Build: 3 (com.forkmesh.forkmesh)" in first["details"]
    assert "- Screenshots: 1 attached, shrunk from 1290x2796" in first["details"]
    assert first["details"].endswith(f"{tool.MARKER} screenshot A1")
    assert len(made[0]["attachments"]) == 3
    for fields in made:
        body = {**fields, "destination": "department", "assigneeKind": "unassigned"}
        assert len(json.dumps(body, separators=(",", ":"))) < 64 * 1024
        for image in fields["attachments"]:
            raw = base64.b64decode(image["file"])
            assert raw[:3] == b"\xff\xd8\xff" and image["size"] == len(raw)
            assert image["mime"] == "image/jpeg"
    assert asc.deleted == []
    assert "2 tasks added, 0 feedback items marked done" in capsys.readouterr().out


def test_known_feedback_is_skipped_and_done_tasks_delete_their_feedback(capsys):
    asc = FakeAsc([submission("A1", "one", "2026-09-01T00:00:00Z"),
                   submission("A2", "two", "2026-09-02T00:00:00Z"),
                   submission("A3", "three", "2026-09-03T00:00:00Z")],
                  crashes=[submission("C1", "crashed", "2026-09-04T00:00:00Z", shots=0)])
    asc.missing.add("/v1/betaFeedbackCrashSubmissions/C1")
    tasks = board(task_for("screenshot", "A1"), task_for("screenshot", "A2", done=True),
                  task_for("crash", "C1", done=True),
                  trashed=[task_for("screenshot", "A3", done=True)])
    status, made = run(asc, tasks)
    assert status == 0
    assert made == []
    assert asc.deleted == ["/v1/betaFeedbackScreenshotSubmissions/A2"]
    assert "0 tasks added, 2 feedback items marked done" in capsys.readouterr().out


def test_dry_run_changes_nothing(capsys):
    asc = FakeAsc([submission("A1", "one", "2026-09-01T00:00:00Z"),
                   submission("A2", "two", "2026-09-02T00:00:00Z")])
    status, made = run(asc, board(task_for("screenshot", "A2", done=True)), dry_run=True)
    out = capsys.readouterr().out
    assert status == 0 and made == [] and asc.deleted == []
    assert "would add task for 2026-09-01 00:00 screenshot A1: TestFlight: one (1 image(s))" in out
    assert "would delete 2026-09-02 00:00 screenshot A2 from App Store Connect" in out


def test_a_failed_download_still_files_the_task_and_says_so():
    asc = FakeAsc([submission("A1", "one", "2026-09-01T00:00:00Z")])

    def broken(url):
        raise OSError("expired")

    made = []
    tool.run(asc, "app", board(), lambda fields: made.append(fields) or {"id": "x"},
             "forkmesh/forkmesh", False, fetch=broken)
    assert "attachments" not in made[0]
    assert "- Screenshot 1 could not be downloaded: expired" in made[0]["details"]


def test_a_failed_delete_is_reported_and_fails_the_run(capsys):
    asc = FakeAsc([submission("A1", "one", "2026-09-01T00:00:00Z")])

    def refuse(method, path, body=None):
        if method == "DELETE":
            raise SystemExit("ERROR: App Store Connect DELETE answered 403: forbidden")
        return asc(method, path, body)

    status = tool.run(refuse, "app", board(task_for("screenshot", "A1", done=True)),
                      lambda fields: {}, "forkmesh/forkmesh", False, fetch=lambda url: SHOT)
    assert status == 1
    assert "could not delete it: ERROR: App Store Connect DELETE answered 403" in capsys.readouterr().out


def test_long_comments_make_a_bounded_title():
    entry = {"kind": "crash", "id": "C", "attributes": {"comment": "word " * 100}}
    text = tool.title(entry)
    assert len(text) == tool.MAX_TITLE and text.startswith("TestFlight crash: word") and text.endswith("…")
