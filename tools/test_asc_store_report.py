import gzip
import json
import sys
import urllib.parse
from datetime import date
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "native" / "ios" / "tools"))
sys.path.insert(0, str(ROOT / "marketplace" / "packages" / "app-store-status" / "content" / "worker"))

import app_store_status  # noqa: E402
import asc_store_report  # noqa: E402
from asc_bundle_ids import credentials, token  # noqa: E402

ANSWERS = {
    "/v1/apps/app/appStoreVersions": {"data": [
        {"id": "v14", "attributes": {"versionString": "1.4.0", "appVersionState": "REJECTED",
                                     "releaseType": "AFTER_APPROVAL",
                                     "createdDate": "2026-09-16T09:45:00Z"}},
        {"id": "v11", "attributes": {"versionString": "1.1.0",
                                     "appVersionState": "READY_FOR_DISTRIBUTION",
                                     "createdDate": "2026-09-14T09:23:00Z"}},
    ]},
    "/v1/appStoreVersions/v14/build": {"data": {"id": "b19", "attributes": {"version": "19"}}},
    "/v1/appStoreVersions/v11/build": {"data": None},
    "/v1/reviewSubmissions": {"data": [
        {"id": "s1", "attributes": {"state": "UNRESOLVED_ISSUES",
                                    "submittedDate": "2026-10-08T18:29:00Z"}},
    ]},
    "/v1/reviewSubmissions/s1/items": {"data": [
        {"id": "i1", "attributes": {"state": "REJECTED"},
         "relationships": {"appStoreVersion": {"data": {"type": "appStoreVersions", "id": "v14"}}}},
    ]},
    "/v1/builds": {
        "data": [{"id": "b19", "attributes": {"version": "19", "processingState": "VALID",
                                              "uploadedDate": "2026-10-05T12:51:00Z"},
                  "relationships": {
                      "preReleaseVersion": {"data": {"type": "preReleaseVersions", "id": "p"}},
                      "buildBetaDetail": {"data": {"type": "buildBetaDetails", "id": "d"}},
                      "betaAppReviewSubmission": {"data": None},
                      "betaGroups": {"data": [{"type": "betaGroups", "id": "g1"}]}}}],
        "included": [
            {"type": "preReleaseVersions", "id": "p", "attributes": {"version": "1.4.0"}},
            {"type": "buildBetaDetails", "id": "d",
             "attributes": {"internalBuildState": "IN_BETA_TESTING",
                            "externalBuildState": "WAITING_FOR_BETA_REVIEW"}},
            {"type": "betaGroups", "id": "g1", "attributes": {"name": "forkmesh-team"}},
        ]},
    "/v1/apps/app/betaGroups": {"data": [
        {"id": "g2", "attributes": {"name": "outside", "isInternalGroup": False,
                                    "publicLinkEnabled": True,
                                    "publicLink": "https://testflight.apple.com/join/x",
                                    "feedbackEnabled": True}},
        {"id": "g1", "attributes": {"name": "forkmesh-team", "isInternalGroup": True,
                                    "hasAccessToAllBuilds": True, "feedbackEnabled": True}},
    ]},
    "/v1/betaGroups/g1/betaTesters": {"data": [{}], "meta": {"paging": {"total": 3}}},
    "/v1/betaGroups/g2/betaTesters": {"data": [{}], "meta": {"paging": {"total": 1}}},
    "/v1/apps/app/betaFeedbackScreenshotSubmissions": {"data": [
        {"id": "fb1",
         "attributes": {"createdDate": "2026-09-01T03:06:00Z", "email": "t@example.com",
                        "deviceModel": "iPhone16_2", "osVersion": "27.0",
                        "comment": "Show time\nas 5:45pm",
                        "screenshots": [
                            {"url": "https://tf.example.apple.com/shot-1.png", "width": 1179,
                             "height": 2556, "expirationDate": "2026-09-02T03:06:00Z"},
                            {"url": "http://insecure.example/shot.png", "width": 1, "height": 1},
                            {"url": "https://tf.example.apple.com/shot-2.png", "width": 2556,
                             "height": 1179}]}}]},
    "/v1/apps/app/betaFeedbackCrashSubmissions": None,
    "/v1/apps/app/customerReviews": {"data": []},
}


def fake_api(method, path, body=None):
    assert method == "GET"
    answer = ANSWERS[path.split("?", 1)[0]]
    if answer is None:
        raise SystemExit("ERROR: App Store Connect GET answered 404")
    return answer


fake_api.download = lambda path: None


def sales(*rows):
    header = "Provider\tSKU\tProduct Type Identifier\tUnits\tApple Identifier\n"
    return gzip.compress((header + "".join("APPLE\tfm\t" + "\t".join(row) + "\n"
                                           for row in rows)).encode())


class SalesApi:
    def __init__(self, reports):
        self.reports = reports
        self.asked = []

    def download(self, path):
        query = dict(urllib.parse.parse_qsl(path.split("?", 1)[1]))
        assert query["filter[vendorNumber]"] == "85000000"
        key = (query["filter[frequency]"], query["filter[reportDate]"])
        self.asked.append(key)
        return self.reports.get(key)


def test_downloads_add_closed_months_and_daily_reports_for_the_app(capsys):
    api = SalesApi({
        ("MONTHLY", "2026-08"): sales(("1F", "4", "app"), ("1F", "9", "other")),
        ("MONTHLY", "2026-09"): sales(("1F", "10", "app"), ("3F", "2", "app"), ("7F", "5", "app")),
        ("DAILY", "2026-09-20"): sales(("1F", "100", "app")),
        ("DAILY", "2026-10-01"): sales(("1", "3", "app"), ("IA1", "8", "app")),
        ("DAILY", "2026-10-12"): sales(("1T", "1", "app"), ("7", "2", "app")),
    })
    versions = [{"attributes": {"createdDate": "2026-09-14T09:23:00Z"}},
                {"attributes": {"createdDate": "2026-08-28T10:00:00Z"}}]
    asc_store_report.render_downloads(
        asc_store_report.downloads(api, "app", versions, "85000000", date(2026, 10, 13)))
    out = capsys.readouterr().out
    assert ("MONTHLY", "2026-07") not in api.asked
    assert ("MONTHLY", "2026-10") not in api.asked
    assert ("DAILY", "2026-10-13") not in api.asked
    assert "since 2026-08: 18 downloads, 2 redownloads, 7 updates" in out
    assert "last 30 days: 104 downloads, 0 redownloads, 2 updates" in out
    assert "last 7 days: 1 downloads, 0 redownloads, 2 updates" in out


def test_a_month_too_new_for_its_monthly_report_counts_its_days(capsys):
    api = SalesApi({
        ("MONTHLY", "2026-09"): sales(("1F", "50", "app")),
        ("DAILY", "2026-09-30"): sales(("1F", "6", "app")),
        ("DAILY", "2026-10-02"): sales(("1F", "1", "app")),
    })
    versions = [{"attributes": {"createdDate": "2026-09-14T09:23:00Z"}}]
    asc_store_report.render_downloads(
        asc_store_report.downloads(api, "app", versions, "85000000", date(2026, 10, 5)))
    out = capsys.readouterr().out
    assert ("MONTHLY", "2026-09") not in api.asked
    assert "since 2026-09: 7 downloads" in out


def test_the_report_flags_review_trouble_and_lists_groups_and_feedback(monkeypatch, capsys):
    monkeypatch.setattr(asc_store_report, "Session", lambda: fake_api)
    monkeypatch.setattr(asc_store_report, "app_id", lambda api: "app")
    monkeypatch.setattr(asc_store_report, "vendor_number", lambda: "")
    asc_store_report.main([])
    out = capsys.readouterr().out
    assert "1.4.0 (build 19): REJECTED  <-- needs action" in out
    assert "1.1.0 (build no build): READY_FOR_DISTRIBUTION" in out
    assert "s1: UNRESOLVED_ISSUES  <-- needs action; submitted 2026-10-08 18:29" in out
    assert "    1.4.0: REJECTED  <-- needs action" in out
    assert "Resolution Center" in out
    assert "== Downloads ==\n  set ASC_VENDOR_NUMBER" in out
    assert "external WAITING_FOR_BETA_REVIEW; beta review not submitted" in out
    assert "in groups: forkmesh-team" in out
    groups = out.split("== TestFlight groups ==")[1]
    assert groups.index("forkmesh-team (internal): 3 testers; every build") < groups.index(
        "outside (external): 1 tester; chosen builds; public link ON "
        "https://testflight.apple.com/join/x; feedback on")
    assert ("screenshot from t@example.com (iPhone16_2 27.0): Show time as 5:45pm "
            "[2 screenshots]") in out
    assert "(unavailable: ERROR: App Store Connect GET answered 404)" in out
    assert out.rstrip().endswith("== App Store customer reviews ==\n  none")


def collected(monkeypatch):
    monkeypatch.setattr(asc_store_report, "vendor_number", lambda: "")
    return asc_store_report.collect(fake_api, "app", 5, "", date(2026, 10, 13))


def test_the_snapshot_carries_labels_tones_and_attention(monkeypatch):
    snapshot = collected(monkeypatch)
    assert snapshot["format"] == "forkmesh-app-store-status-v1"
    assert snapshot["app"]["id"] == "app"
    assert snapshot["versions"][0] == {
        "version": "1.4.0", "build": "19", "releaseType": "AFTER_APPROVAL",
        "createdAt": "2026-09-16T09:45:00Z",
        "status": {"state": "REJECTED", "label": "Rejected", "tone": "action"}}
    assert snapshot["versions"][1]["status"]["tone"] == "good"
    assert snapshot["versions"][1]["build"] == ""
    assert snapshot["submissions"][0]["items"][0]["name"] == "1.4.0"
    assert len(snapshot["attention"]) == 2
    build = snapshot["builds"][0]
    assert build["external"] == {"state": "WAITING_FOR_BETA_REVIEW",
                                 "label": "Waiting for beta review", "tone": "pending"}
    assert build["betaReview"] == {"state": "", "label": "Not submitted", "tone": "neutral"}
    assert [group["name"] for group in snapshot["groups"]] == ["forkmesh-team", "outside"]
    assert snapshot["groups"][1]["publicLink"] == "https://testflight.apple.com/join/x"
    assert snapshot["feedback"][0]["comment"] == "Show time as 5:45pm"
    assert snapshot["unavailable"] == {
        "feedback": "ERROR: App Store Connect GET answered 404"}
    assert snapshot["downloads"]["available"] is False
    shots = snapshot["feedback"][0]["screenshots"]
    assert [(shot["width"], shot["height"]) for shot in shots] == [(1179, 2556), (2556, 1179)]
    assert shots[0]["url"] == "https://tf.example.apple.com/shot-1.png"
    assert shots[0]["id"] == asc_store_report.screenshot_id({"id": "fb1"}, 0)
    assert len({shot["id"] for shot in shots}) == 2
    assert json.loads(json.dumps(snapshot)) == snapshot
    published = asc_store_report.public(snapshot)
    assert "url" not in json.dumps(published["feedback"])
    assert "url" in snapshot["feedback"][0]["screenshots"][0]
    assert app_store_status.clean_snapshot(published) == published


def test_publish_puts_the_snapshot_and_never_fails_the_report(monkeypatch, capsys):
    snapshot = collected(monkeypatch)
    sent = []
    assert asc_store_report.publish(snapshot, lambda *args: sent.append(args))
    assert sent == [("App Store Status", "/api/app-store-status",
                     "forkmesh-app-store-status-request-v1", "PUT",
                     "/api/app-store-status/snapshot", asc_store_report.public(snapshot))]

    def refuse(*args):
        raise ValueError("App Store Status API 401: invalid_session")

    assert not asc_store_report.publish(snapshot, refuse)
    assert "not published to the App Store Status page: App Store Status API 401" in (
        capsys.readouterr().out)


def detailed_sales(*rows):
    header = ("Provider\tSKU\tProduct Type Identifier\tUnits\tApple Identifier\t"
              "Country Code\tDevice\n")
    return gzip.compress((header + "".join("APPLE\tfm\t" + "\t".join(row) + "\n"
                                           for row in rows)).encode())


def test_marketing_gets_each_reported_day_with_countries_and_devices(monkeypatch, capsys):
    snapshot = collected(monkeypatch)
    snapshot["downloads"]["since"] = "2026-08"
    api = SalesApi({
        ("DAILY", "2026-10-08"): detailed_sales(("1F", "3", "app", "US", "iPhone"),
                                                ("1F", "2", "app", "DE", "iPad"),
                                                ("7F", "4", "app", "US", "iPhone"),
                                                ("1F", "9", "other", "US", "iPhone")),
        ("DAILY", "2026-10-10"): detailed_sales(("3F", "1", "app", "FR", "iPhone")),
    })
    calls = []

    def request(method, path, body=None):
        calls.append((method, path, body))
        if method == "GET":
            return {"appStore": {"app": {"id": "app"}, "lastDay": "2026-10-10"}}
        return {"appStore": {"lastDay": body["days"][-1]["day"],
                             "windows": [{"days": 30, "downloads": 5}]}}

    assert asc_store_report.publish_marketing(snapshot, api, "app", "85000000",
                                              date(2026, 10, 13), "acme", request)
    assert [call[:2] for call in calls] == [("GET", "/api/marketing/acme/app-store"),
                                            ("PUT", "/api/marketing/acme/app-store")]
    body = calls[1][2]
    assert body["app"]["id"] == "app"
    assert body["versions"] == snapshot["versions"] and body["reviews"] == snapshot["reviews"]
    assert [day["day"] for day in body["days"]] == ["2026-10-07", "2026-10-08", "2026-10-09",
                                                    "2026-10-10", "2026-10-11"]
    assert body["days"][1] == {"day": "2026-10-08", "downloads": 5, "redownloads": 0,
                               "updates": 4, "territories": {"US": 3, "DE": 2},
                               "devices": {"iPhone": 3, "iPad": 2}}
    assert body["days"][3]["territories"] == {}
    assert ("DAILY", "2026-10-12") in api.asked
    assert "sent 5 days of App Store downloads to Marketing (acme)" in capsys.readouterr().out


def test_marketing_backfills_a_year_at_most_and_never_fails_the_report(monkeypatch, capsys):
    today = date(2026, 10, 13)
    assert asc_store_report.marketing_start("", "2026-08", today) == date(2026, 8, 1)
    assert asc_store_report.marketing_start("", "2024-01", today) == date(2025, 10, 13)
    assert asc_store_report.marketing_start("2026-10-01", "2026-08", today) == date(2026, 9, 28)
    snapshot = collected(monkeypatch)
    assert not asc_store_report.publish_marketing(snapshot, SalesApi({}), "app", "", today,
                                                  "acme", lambda *args: {})
    assert "set ASC_VENDOR_NUMBER" in capsys.readouterr().out

    def refuse(*args):
        raise ValueError("Marketing API 409: plugin_not_installed")

    assert not asc_store_report.publish_marketing(snapshot, SalesApi({}), "app", "85000000",
                                                  today, "acme", refuse)
    assert "not sent to Marketing: Marketing API 409" in capsys.readouterr().out


def test_json_mode_prints_the_snapshot(monkeypatch, capsys):
    monkeypatch.setattr(asc_store_report, "Session", lambda: fake_api)
    monkeypatch.setattr(asc_store_report, "app_id", lambda api: "app")
    monkeypatch.setattr(asc_store_report, "vendor_number", lambda: "")
    asc_store_report.main(["--json", "--feedback", "2"])
    assert json.loads(capsys.readouterr().out)["versions"][0]["version"] == "1.4.0"
    with pytest.raises(SystemExit, match="usage"):
        asc_store_report.main(["--bogus"])


def test_store_report_credentials_have_an_actionable_error(tmp_path, monkeypatch):
    monkeypatch.delenv("ASC_KEY_ID", raising=False)
    monkeypatch.delenv("ASC_ISSUER_ID", raising=False)
    monkeypatch.setenv("HOME", str(tmp_path))
    monkeypatch.setenv("FORKMESH_CONFIG_DIR", "/no-such-forkmesh-config")
    monkeypatch.setenv("XDG_CONFIG_HOME", str(tmp_path / "config"))
    with pytest.raises(SystemExit, match="missing App Store Connect credential ASC_KEY_ID and ASC_ISSUER_ID"):
        token()


def test_store_report_reads_credentials_from_desktop_actions_variables(tmp_path, monkeypatch):
    config = tmp_path / "ForkMesh"
    config.mkdir()
    (config / "ForkMesh.conf").write_text(
        '[actions]\nvariables="@ByteArray({\\\"ASC_KEY_ID\\\":\\\"K\\\",\\\"ASC_ISSUER_ID\\\":\\\"I\\\"})"\n'
    )
    monkeypatch.delenv("ASC_KEY_ID", raising=False)
    monkeypatch.delenv("ASC_ISSUER_ID", raising=False)
    monkeypatch.setenv("HOME", str(tmp_path))
    monkeypatch.setenv("FORKMESH_CONFIG_DIR", str(tmp_path))
    monkeypatch.setenv("XDG_CONFIG_HOME", str(tmp_path / "config"))
    assert credentials() == ("K", "I")


PNG = b"\x89PNG\r\n\x1a\n" + b"\x00" * 32


def test_publish_uploads_only_the_screenshots_the_server_is_missing(monkeypatch, capsys):
    monkeypatch.setattr(asc_store_report, "shrink",
                        lambda raw: (raw, asc_store_report.image_mime(raw)))
    snapshot = collected(monkeypatch)
    first, second = (shot["id"] for shot in snapshot["feedback"][0]["screenshots"])
    sent, fetched = [], []

    def request(*args):
        sent.append(args)
        return {"ok": True, "missingScreenshots": [second]} if args[4].endswith("/snapshot") else {}

    def fetch(url):
        fetched.append(url)
        return PNG

    assert asc_store_report.publish(snapshot, request, fetch)
    assert fetched == ["https://tf.example.apple.com/shot-2.png"]
    assert [args[4] for args in sent] == ["/api/app-store-status/snapshot",
                                          f"/api/app-store-status/screenshots/{second}"]
    body = sent[1][5]
    assert body["mime"] == "image/png" and (body["width"], body["height"]) == (2556, 1179)
    assert asc_store_report.base64.b64decode(body["data"]) == PNG
    assert first not in json.dumps(sent[1])
    assert "uploaded 1 of 1 new tester feedback screenshot" in capsys.readouterr().out


def test_screenshot_problems_are_printed_and_never_fail_the_report(monkeypatch, capsys):
    snapshot = collected(monkeypatch)
    ids = [shot["id"] for shot in snapshot["feedback"][0]["screenshots"]]

    def request(*args):
        if args[4].endswith("/snapshot"):
            return {"ok": True, "missingScreenshots": ids}
        raise ValueError("App Store Status API 413: image_too_large")

    def fetch(url):
        if url.endswith("shot-1.png"):
            raise OSError("expired")
        return PNG

    monkeypatch.setattr(asc_store_report, "shrink",
                        lambda raw: (raw, asc_store_report.image_mime(raw)))
    assert asc_store_report.publish(snapshot, request, fetch)
    out = capsys.readouterr().out
    assert "uploaded 0 of 2 new tester feedback screenshots" in out
    assert f"screenshot {ids[0][:8]} could not be downloaded: expired" in out
    assert f"screenshot {ids[1][:8]} was not uploaded: App Store Status API 413" in out
    monkeypatch.setattr(asc_store_report, "shrink", lambda raw: (b"", ""))
    assert asc_store_report.upload_screenshots(snapshot, ids[1:], request, fetch)[1] == [
        f"screenshot {ids[1][:8]} is not a picture that fits in 3 MB"]


def test_shrink_keeps_a_small_picture_without_pillow(monkeypatch):
    import builtins
    real = builtins.__import__

    def no_pillow(name, *args, **kwargs):
        if name == "PIL":
            raise ImportError(name)
        return real(name, *args, **kwargs)

    monkeypatch.setattr(builtins, "__import__", no_pillow)
    assert asc_store_report.shrink(PNG) == (PNG, "image/png")
    assert asc_store_report.shrink(b"GIF89a") == (b"", "")


def test_shrink_turns_a_large_screenshot_into_a_jpeg_under_the_cap():
    image_module = pytest.importorskip("PIL.Image")
    import io
    out = io.BytesIO()
    image_module.new("RGB", (2400, 1200), (40, 90, 200)).save(out, "PNG")
    small, mime = asc_store_report.shrink(out.getvalue())
    assert mime == "image/jpeg" and small.startswith(b"\xff\xd8\xff")
    assert len(small) <= asc_store_report.SCREENSHOT_MAX_BYTES
    assert max(image_module.open(io.BytesIO(small)).size) == 2048
    assert asc_store_report.shrink(b"not a picture") == (b"", "")
