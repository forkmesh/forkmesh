#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import re


REPO = Path(__file__).resolve().parents[2]
ENTRY = REPO / "app" / "src" / "entry.py"
PACKAGE_TESTS = REPO / "marketplace" / "packages" / "calories" / "tests" / "test_calories.py"


def _package_harness():
    spec = importlib.util.spec_from_file_location("calories_package_harness", PACKAGE_TESTS)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


HARNESS = _package_harness()
calories = HARNESS.calories
call = HARNESS.call
Runtime = HARNESS.Runtime
DEGRADED = "x-forkmesh-expected-degraded"
IMAGE = "data:image/jpeg;base64," + "QUJD" * 50


def _analyze(runtime, body):
    return call(runtime.use("POST", body=body), "/api/calories/analyze")


def _receipt(runtime):
    return call(runtime.use("POST", body={"image": IMAGE}), "/api/calories/receipt")


def test_host_without_an_ai_provider_answers_503_not_502_and_is_not_logged():
    runtime = Runtime()
    runtime.ai_result = {"ok": False, "error": "missing_binding"}
    for answer in (_analyze(runtime, {"description": "toast"}),
                   _analyze(runtime, {"image": IMAGE}), _receipt(runtime)):
        assert answer["status"] == 503, answer
        assert answer["data"] == {"error": "ai_unavailable", "reason": "missing_binding",
                                  "retryable": False}
        assert answer["headers"][DEGRADED] == "1"
        assert "retry-after" not in answer["headers"]


def test_runtime_without_ai_json_matches_the_missing_binding_answer():
    runtime = Runtime(ai=False)
    answer = _analyze(runtime, {"description": "toast"})
    assert answer["status"] == 503
    assert answer["data"]["reason"] == "missing_binding"
    assert answer["headers"][DEGRADED] == "1"


def test_provider_failures_answer_503_with_a_retry_hint():
    runtime = Runtime()
    for reason in ("provider_error", "unusable_response", "", "something_new"):
        runtime.ai_result = {"ok": False, "error": reason} if reason else None
        for answer in (_analyze(runtime, {"description": "toast"}), _receipt(runtime)):
            assert answer["status"] == 503, answer
            assert answer["data"] == {
                "error": "ai_unavailable", "reason": reason or "provider_error",
                "retryable": True, "retryAfterMs": calories.AI_RETRY_AFTER_MS}
            assert answer["headers"]["retry-after"] == "30"
            assert answer["headers"][DEGRADED] == "1"


def test_rate_limits_stay_429_with_retry_after_and_are_not_marked_degraded():
    runtime = Runtime()
    runtime.ai_result = {"ok": False, "error": "rate_limited", "retryAfterMs": 4200}
    answer = _analyze(runtime, {"description": "toast"})
    assert answer["status"] == 429
    assert answer["data"] == {"error": "rate_limited", "retryable": True, "retryAfterMs": 4200}
    assert answer["headers"]["retry-after"] == "5"
    assert DEGRADED not in answer["headers"]
    runtime.ai_result = {"ok": False, "error": "rate_limited"}
    bare = _analyze(runtime, {"description": "toast"})
    assert bare["status"] == 429 and "retryAfterMs" not in bare["data"]


def test_host_side_invalid_image_is_a_400():
    runtime = Runtime()
    runtime.ai_result = {"ok": False, "error": "invalid_image"}
    answer = _analyze(runtime, {"image": IMAGE})
    assert answer["status"] == 400 and answer["data"] == {"error": "invalid_image"}


def test_no_ai_failure_path_in_the_worker_answers_502():
    source = Path(calories.__file__).read_text(encoding="utf-8")
    start = source.index("async def _analyze(")
    end = source.index("\ndef _clean_quick_foods(")
    assert "502" not in source[start:end]
    receipt = source[source.index("async def _read_receipt("):]
    assert "502" not in receipt[:receipt.index("\nasync def ", 1)]


def test_degraded_header_matches_the_app_logger_marker():
    entry = ENTRY.read_text(encoding="utf-8")
    match = re.search(r'^EXPECTED_DEGRADED_HEADER = "([^"]+)"', entry, re.M)
    assert match and match.group(1) == calories.EXPECTED_DEGRADED_HEADER == DEGRADED
