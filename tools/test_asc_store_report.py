import sys
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


def test_the_report_flags_review_trouble_and_lists_groups_and_feedback(monkeypatch, capsys):
    monkeypatch.setattr(asc_store_report, "Session", lambda: fake_api)
    monkeypatch.setattr(asc_store_report, "app_id", lambda api: "app")
    asc_store_report.main([])
    out = capsys.readouterr().out
    assert "1.4.0 (build 19): REJECTED  <-- needs action" in out
    assert "1.1.0 (build no build): READY_FOR_DISTRIBUTION" in out
    assert "s1: UNRESOLVED_ISSUES  <-- needs action; submitted 2026-10-08 18:29" in out
    assert "    1.4.0: REJECTED  <-- needs action" in out
    assert "Resolution Center" in out
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
