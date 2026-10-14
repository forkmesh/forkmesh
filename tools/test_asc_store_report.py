import gzip
import sys
import urllib.parse
from datetime import date
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "native" / "ios" / "tools"))

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
        {"attributes": {"createdDate": "2026-09-01T03:06:00Z", "email": "t@example.com",
                        "deviceModel": "iPhone16_2", "osVersion": "27.0",
                        "comment": "Show time\nas 5:45pm"}}]},
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
    asc_store_report.downloads(api, "app", versions, "85000000", date(2026, 10, 13))
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
    asc_store_report.downloads(api, "app", versions, "85000000", date(2026, 10, 5))
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
    assert "screenshot from t@example.com (iPhone16_2 27.0): Show time as 5:45pm" in out
    assert "(unavailable: ERROR: App Store Connect GET answered 404)" in out
    assert out.rstrip().endswith("== App Store customer reviews ==\n  none")


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
