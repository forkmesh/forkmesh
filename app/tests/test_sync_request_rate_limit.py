import importlib.util
from pathlib import Path
import sys

import pytest


MODULE_PATH = (
    Path(__file__).resolve().parents[1] /
    "tools" / "sync_request_rate_limit.py")
sys.path.insert(0, str(MODULE_PATH.parent))
SPEC = importlib.util.spec_from_file_location(
    "sync_request_rate_limit", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def test_default_rule_protects_site_traffic_per_client():
    payload = MODULE.rule_payload({})
    assert payload["action"] == "block"
    assert payload["enabled"] is True
    assert payload["expression"] == (
        '(not cf.client.bot and not starts_with('
        'http.request.uri.path, "/cdn-cgi/"))')
    assert payload["ratelimit"] == {
        "characteristics": ["cf.colo.id", "ip.src"],
        "period": 10,
        "requests_per_period": 120,
        "mitigation_timeout": 10,
    }


def test_limits_can_be_tuned_without_source_changes():
    payload = MODULE.rule_payload({
        "FORKMESH_EDGE_RATE_LIMIT_PERIOD_SECONDS": "60",
        "FORKMESH_EDGE_RATE_LIMIT_REQUESTS": "300",
        "FORKMESH_EDGE_RATE_LIMIT_TIMEOUT_SECONDS": "600",
    })
    assert payload["ratelimit"]["period"] == 60
    assert payload["ratelimit"]["requests_per_period"] == 300
    assert payload["ratelimit"]["mitigation_timeout"] == 600


def test_invalid_values_fail_closed_before_updating_cloudflare():
    with pytest.raises(ValueError):
        MODULE.rule_payload({"FORKMESH_EDGE_RATE_LIMIT_REQUESTS": "zero"})
    with pytest.raises(ValueError):
        MODULE.rule_payload({
            "FORKMESH_EDGE_RATE_LIMIT_PERIOD_SECONDS": "11"})


def test_explicit_disable_values_are_supported():
    for value in ("0", "false", "NO", "off"):
        assert MODULE.enabled({
            "FORKMESH_EDGE_RATE_LIMIT_ENABLED": value}) is False
