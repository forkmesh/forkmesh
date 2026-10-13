#!/usr/bin/env python3
"""Durable content-free clone and website counters for headless mirrors."""

import json
import os
from pathlib import Path
import sys
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import headless_mirror_refresh as refresh  # noqa: E402
import mirror_gateway as gateway  # noqa: E402


def test_gateway_initializes_truthful_zero_counters(tmp_path: Path):
    path = tmp_path / gateway.SERVICE_COUNTERS_FILE
    counters = gateway.GatewayServiceCounters(
        path,
        clock_ms=lambda: 1_765_000_000_000,
    )
    assert counters.ensure_repository("mirror6", "ForkMesh")
    assert counters.ensure_repository("mirror6", "forkmesh")
    assert not counters.ensure_repository("../mirror6", "forkmesh")
    counters.close()

    value = json.loads(path.read_text(encoding="utf-8"))
    assert value["repositories"] == {
        "mirror6/forkmesh": {
            "clonesServed": 0,
            "websiteServed": 0,
            "updatedAt": 1_765_000_000_000,
        }
    }


def test_gateway_counters_persist_only_completed_clone_and_browse_requests(
    tmp_path: Path,
):
    path = tmp_path / gateway.SERVICE_COUNTERS_FILE
    counters = gateway.GatewayServiceCounters(
        path,
        clock_ms=lambda: 1_765_000_000_000,
    )
    assert counters.record(
        "mirror6", "forkmesh", "git-upload-pack", "git/2.43.0"
    )
    assert counters.record("mirror6", "forkmesh", "tree")
    assert counters.record(
        "mirror6", "forkmesh", "raw", "Mozilla/5.0 (X11) Firefox/141.0"
    )
    assert not counters.record("mirror6", "forkmesh", "git-info-refs")
    assert not counters.record("mirror6", "forkmesh", "actions-status")
    counters.close()

    assert path.stat().st_mode & 0o777 == 0o600
    value = json.loads(path.read_text(encoding="utf-8"))
    assert value == {
        "schemaVersion": 1,
        "type": gateway.SERVICE_COUNTERS_TYPE,
        "repositories": {
            "mirror6/forkmesh": {
                "clonesServed": 1,
                "websiteServed": 2,
                "updatedAt": 1_765_000_000_000,
                # When each kind of request was last served, and the bounded
                # class of client served. The raw User-Agent never lands here.
                "cloneServedAt": 1_765_000_000_000,
                "cloneServedAgent": "git-client",
                "websiteServedAt": 1_765_000_000_000,
                "websiteServedAgent": "browser",
            },
        },
    }
    reloaded = gateway.GatewayServiceCounters(
        path,
        clock_ms=lambda: 1_765_000_001_000,
    )
    assert reloaded.record("mirror6", "forkmesh", "stats")
    reloaded.close()
    row = json.loads(path.read_text(encoding="utf-8"))["repositories"][
        "mirror6/forkmesh"
    ]
    assert row["websiteServed"] == 3
    # The website stamp advances with its own counter; the clone stamp and the
    # client that earned it are untouched by an unrelated browse.
    assert row["websiteServedAt"] == 1_765_000_001_000
    assert row["websiteServedAgent"] == "client"
    assert row["cloneServedAt"] == 1_765_000_000_000
    assert row["cloneServedAgent"] == "git-client"


def test_gateway_counters_load_rows_written_before_the_served_stamps(
    tmp_path: Path,
):
    path = tmp_path / gateway.SERVICE_COUNTERS_FILE
    path.write_text(
        json.dumps({
            "schemaVersion": 1,
            "type": gateway.SERVICE_COUNTERS_TYPE,
            "repositories": {
                "mirror6/forkmesh": {
                    "clonesServed": 7,
                    "websiteServed": 12,
                    "updatedAt": 1_765_000_000_000,
                },
                # A tampered row: neither an unknown agent string nor a
                # non-positive stamp may reach the published record.
                "mirror7/forkmesh": {
                    "clonesServed": 1,
                    "websiteServed": 0,
                    "updatedAt": 1_765_000_000_000,
                    "cloneServedAt": 0,
                    "cloneServedAgent": "git/2.43.0 (10.0.0.4)",
                },
            },
        }),
        encoding="utf-8",
    )
    os.chmod(path, 0o600)
    counters = gateway.GatewayServiceCounters(
        path,
        clock_ms=lambda: 1_765_000_002_000,
    )
    assert counters.record("mirror6", "forkmesh", "git-upload-pack", "curl/8")
    counters.close()

    repositories = json.loads(path.read_text(encoding="utf-8"))["repositories"]
    assert repositories["mirror6/forkmesh"] == {
        "clonesServed": 8,
        "websiteServed": 12,
        "updatedAt": 1_765_000_002_000,
        "cloneServedAt": 1_765_000_002_000,
        "cloneServedAgent": "bot-tool",
    }
    assert repositories["mirror7/forkmesh"] == {
        "clonesServed": 1,
        "websiteServed": 0,
        "updatedAt": 1_765_000_000_000,
    }


def test_gateway_route_counter_requires_successful_delivery_and_exact_route():
    calls = []
    application = SimpleNamespace(
        service_counters=SimpleNamespace(
            record=lambda owner, repository, operation, agent: calls.append(
                (owner, repository, operation, agent)
            ) or True
        )
    )
    record = gateway.GatewayApplication.record_service_counter
    target = "/v1/repositories/mirror6/forkmesh/tree?path=src"
    assert not record(
        application, target, "tree", 503, delivered=True
    )
    assert not record(
        application, target, "tree", 200, delivered=False
    )
    assert not record(
        application, target, "raw", 200, delivered=True
    )
    assert record(
        application, target, "tree", 200, delivered=True,
        user_agent="Mozilla/5.0",
    )
    assert calls == [("mirror6", "forkmesh", "tree", "Mozilla/5.0")]


def test_gateway_generalizes_request_agents_into_bounded_classes():
    assert gateway.service_counter_agent_class(
        "git/2.43.0 (Apple Git-1)") == "git-client"
    assert gateway.service_counter_agent_class(
        "forkmesh-node/0.7.4") == "forkmesh-node"
    assert gateway.service_counter_agent_class("curl/8.5.0") == "bot-tool"
    assert gateway.service_counter_agent_class(
        "Mozilla/5.0 (X11; Linux) Chrome/141") == "browser"
    assert gateway.service_counter_agent_class("") == "client"
    assert (
        gateway.service_counter_agent_class("anything else")
        in gateway.SERVICE_COUNTER_AGENT_CLASSES
    )


def test_refresh_publishes_exact_counters_and_rejects_unsafe_counter_files(
    tmp_path: Path,
):
    path = tmp_path / refresh.SERVICE_COUNTERS_FILE
    path.write_text(
        json.dumps({
            "schemaVersion": 1,
            "type": refresh.SERVICE_COUNTERS_TYPE,
            "repositories": {
                "mirror6/forkmesh": {
                    "clonesServed": 7,
                    "websiteServed": 12,
                    "updatedAt": 1_765_000_000_000,
                    "cloneServedAt": 1_764_999_000_000,
                    "cloneServedAgent": "git-client",
                    # An agent class the gateway never writes stays unknown
                    # rather than being published as-is.
                    "websiteServedAgent": "Mozilla/5.0",
                },
            },
        }),
        encoding="utf-8",
    )
    os.chmod(path, 0o600)
    config = SimpleNamespace(
        gateway_config_path=tmp_path / "gateway.json",
        node_owner="mirror6",
        repository_name="ForkMesh",
    )
    assert refresh._sample_gateway_service_counters(config) == {
        "clonesServed": "7",
        "websiteServed": "12",
        "cloneServedAt": "1764999000000",
        "cloneServedAgent": "git-client",
    }

    os.chmod(path, 0o644)
    assert refresh._sample_gateway_service_counters(config) == {}
    path.unlink()
    target = tmp_path / "outside.json"
    target.write_text("{}", encoding="utf-8")
    path.symlink_to(target)
    assert refresh._sample_gateway_service_counters(config) == {}
