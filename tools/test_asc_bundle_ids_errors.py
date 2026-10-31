import io
import json
import sys
import urllib.error
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "native" / "ios" / "tools"))

import asc_bundle_ids  # noqa: E402

PLA = json.dumps({"errors": [{
    "status": "403", "code": "FORBIDDEN_ERROR.PLA_NOT_ACCEPTED",
    "title": "Unable to process request - PLA Update available",
    "detail": "Your team's Account Holder, A Person, must agree to the latest Program License Agreement.",
}]})


def refuse(code, body):
    def urlopen(request, timeout=None):
        raise urllib.error.HTTPError(request.full_url, code, "refused", {}, io.BytesIO(body.encode()))
    return urlopen


def test_pla_refusal_names_the_fix_on_one_line(monkeypatch):
    monkeypatch.setattr(asc_bundle_ids.urllib.request, "urlopen", refuse(403, PLA))
    with pytest.raises(SystemExit) as stop:
        asc_bundle_ids.call("bearer", "GET", "/v1/bundleIds")
    message = str(stop.value)
    assert "\n" not in message
    assert "Program License Agreement" in message
    assert "https://developer.apple.com/account" in message
    assert "A Person" in message


def test_download_pla_refusal_names_the_fix(monkeypatch):
    monkeypatch.setattr(asc_bundle_ids.urllib.request, "urlopen", refuse(403, PLA))
    with pytest.raises(SystemExit) as stop:
        asc_bundle_ids.download("bearer", "/v1/salesReports")
    assert "Account Holder accepts it" in str(stop.value)


@pytest.mark.parametrize("body", ['{"errors": [{"code": "FORBIDDEN_ERROR"}]}', "not json", "[]"])
def test_other_refusals_keep_apples_answer(monkeypatch, body):
    monkeypatch.setattr(asc_bundle_ids.urllib.request, "urlopen", refuse(403, body))
    with pytest.raises(SystemExit) as stop:
        asc_bundle_ids.call("bearer", "GET", "/v1/bundleIds")
    assert str(stop.value) == f"ERROR: App Store Connect GET /v1/bundleIds answered 403: {body}"
