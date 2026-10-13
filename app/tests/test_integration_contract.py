#!/usr/bin/env python3
"""Integration contract v1: manifest, source parity, runner, events, webhooks, examples."""

import importlib.util
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import random
import re
import subprocess
import sys
import threading

import pytest


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "forkmesh_integration.py"
CONTRACT = ROOT / "conformance" / "integrations" / "v1" / "contract.json"
DOC = ROOT / "www" / "docs" / "integrations.md"
EXAMPLES = ROOT / "conformance" / "integrations" / "v1" / "examples"
SECRET = "test-webhook-secret"


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture(scope="module")
def fm():
    return _load("forkmesh_integration", TOOL)


@pytest.fixture(scope="module")
def contract(fm):
    manifest = fm.load_contract(CONTRACT)
    fm.validate_contract(manifest)
    return manifest


def _resources(contract, service):
    return [item for item in contract["interfaces"]["http"]
            if item["service"] == service]


def _no_sleep(recorder=None):
    return (lambda seconds: recorder.append(seconds)) if recorder is not None \
        else (lambda _seconds: None)


# ------------------------------------------------------------ manifest parity


def test_contract_covers_every_scope_area_of_the_issue(contract):
    assert contract["suite"]["version"] == "1.0.0"
    assert set(contract["interfaces"]) == {"http", "events", "cli", "webhooks"}
    for policy in ("pagination", "filtering", "idempotency", "ordering",
                   "retries", "signatures", "rateLimits", "errors",
                   "deprecation", "authentication", "discovery"):
        assert contract["policies"][policy]["summary"]
    categories = set()
    for example in contract["examples"]:
        categories.update(example["categories"])
        assert (ROOT / example["path"]).is_file()
    assert categories >= {"migration", "automation", "discovery",
                          "deployment", "analytics", "moderation"}


def test_validator_rejects_drift(fm, contract):
    broken = json.loads(json.dumps(contract))
    broken["policies"]["retries"]["retryStatuses"] = [503]
    broken["interfaces"]["http"][0]["auth"] = "nobody"
    broken["deprecations"] = [{"id": "version", "announced": "2026-09-01",
                               "sunset": "2026-10-01", "replacement": "x"}]
    with pytest.raises(ValueError) as caught:
        fm.validate_contract(broken)
    message = str(caught.value)
    assert "retryStatuses" in message
    assert "auth is not a declared scheme" in message
    assert "sunsets before noticeDays" in message


def test_hosted_resources_exist_in_the_worker_route_table(contract):
    urls = _load("forkmesh_urls_for_contract", ROOT / "app" / "src" / "urls.py")
    patterns = [getattr(urls, name) for name in dir(urls) if name.endswith("_RE")]
    source = "\n".join(
        path.read_text(encoding="utf-8")
        for path in sorted((ROOT / "app" / "src").glob("*.py")))
    for resource in _resources(contract, "hosted"):
        path = resource["path"]
        served = ('"%s"' % path) in source or any(
            pattern.match(path) for pattern in patterns)
        assert served, "hosted contract path %s is not routed" % path
        for field in list(resource["requiredFields"]) + list(
                resource.get("itemFields") or {}):
            assert ('"%s"' % field) in source, (
                "hosted field %s of %s is never produced" % (field, path))
    inbound = contract["interfaces"]["webhooks"]["inbound"][0]
    assert urls.CHAT_WEBHOOK_DELIVERY_RE.match(
        inbound["path"].replace("{token}", "a" * 64))
    chat = (ROOT / "app" / "src" / "chat_channels_api.py").read_text(encoding="utf-8")
    for code in inbound["errors"].values():
        assert '"%s"' % code in chat
    assert "status=202" in chat


def test_hosted_policy_numbers_match_the_worker(contract):
    entry = (ROOT / "app" / "src" / "entry.py").read_text(encoding="utf-8")
    ttl = int(re.search(r"^CATALOG_TTL = (\d+)", entry, re.M).group(1))
    maximum = int(re.search(r"^MAX_CATALOG_REPOS = (\d+)", entry, re.M).group(1))
    policies = contract["policies"]
    assert policies["rateLimits"]["minimumPollSeconds"]["hosted"] == ttl
    assert policies["pagination"]["maxItems"]["hosted:repositories"] == maximum
    assert '"retryAfterMs"' in entry and '"Retry-After"' in entry


def test_self_hosted_resources_match_daemon_routes_and_json_tags(contract):
    daemon = (ROOT / "server" / "daemon.go").read_text(encoding="utf-8")
    config = (ROOT / "server" / "config.go").read_text(encoding="utf-8")
    assert '"127.0.0.1:8791"' in config
    for resource in _resources(contract, "self-hosted"):
        assert 'mux.HandleFunc("%s"' % resource["path"] in daemon
        for field in resource["requiredFields"]:
            assert ('json:"%s' % field) in daemon or ('"%s":' % field) in daemon, (
                "daemon never produces %s for %s" % (field, resource["path"]))
    assert 'w.Header().Set("Allow", http.MethodPost)' in daemon


def test_mirror_node_cli_flags_and_output_match_the_binary(contract):
    main = (ROOT / "server" / "cmd" / "forkmesh-mirror-node" / "main.go").read_text(
        encoding="utf-8")
    cli = contract["interfaces"]["cli"]["mirrorNode"]
    for flag in cli["flags"]:
        assert re.search(r'flag\.\w+\("%s"' % re.escape(flag), main), flag
    for line in cli["stdout"].values():
        assert 'fmt.Println("%s")' % line in main


def test_client_constants_match_the_contract(fm, contract):
    policies = contract["policies"]
    assert set(policies["retries"]["retryStatuses"]) == set(fm.RETRY_STATUSES)
    assert policies["rateLimits"]["minimumPollSeconds"] == fm.MIN_POLL_SECONDS
    assert policies["ordering"]["gapTimeoutSeconds"] == fm.GAP_TIMEOUT_SECONDS
    assert policies["signatures"]["toleranceSeconds"] == fm.WEBHOOK_TOLERANCE_SECONDS
    assert {command["id"] for command in contract["interfaces"]["cli"]["commands"]} \
        == fm.REQUIRED_CLI_COMMANDS
    parser_commands = fm.build_parser()._subparsers._group_actions[0].choices
    assert set(parser_commands) == fm.REQUIRED_CLI_COMMANDS


# ------------------------------------------------------------ contract runner


@pytest.mark.parametrize("profile", ["previous", "current", "next"])
@pytest.mark.parametrize("target", ["hosted", "self-hosted"])
def test_contract_passes_across_supported_version_skew(fm, contract, target, profile):
    with fm.FixtureServer(profile) as server:
        report = fm.run_contract(contract, target, server.base_url,
                                 allow_mutating=True)
    assert report["schema"] == fm.REPORT_SCHEMA
    failures = [item for item in report["results"] if item["status"] != "pass"]
    assert failures == []
    assert report["service"]["verdict"] == "supported"
    assert report["service"]["serviceVersion"] == fm.FIXTURE_VERSIONS[profile]
    assert report["summary"]["passRate"] == 1.0
    assert report["telemetry"]["requests"] >= len(_resources(contract, target))
    if profile == "next" and target == "hosted":
        version = next(item for item in report["results"] if item["id"] == "version")
        assert version["additionalFields"] >= 2


def test_contract_detects_breakage_and_unsupported_versions(fm, contract, capsys):
    with fm.FixtureServer("broken") as server:
        hosted = fm.run_contract(contract, "hosted", server.base_url)
        node = fm.run_contract(contract, "self-hosted", server.base_url)
    version = next(item for item in hosted["results"] if item["id"] == "version")
    assert "missing field rev" in version["problems"]
    status = next(item for item in node["results"] if item["id"] == "node-status")
    assert status["status"] == "fail" and status["httpStatus"] == 500
    sync = next(item for item in node["results"] if item["id"] == "request-sync")
    assert sync["status"] == "skip"

    assert fm.main(["contract", "--target", "hosted", "--fixture", "broken"]) == fm.EXIT_FAILED
    assert fm.main(["contract", "--target", "self-hosted", "--fixture",
                    "unsupported"]) == fm.EXIT_SKEW
    captured = capsys.readouterr()
    # CI sends the report to /dev/null, so a non-zero exit must say why on
    # stderr (Actions run #334 failed with nothing but "exit code 1").
    assert re.search(r"^hosted contract at http://127\.0\.0\.1:\d+: "
                     r"version failed: missing field rev", captured.err, re.M)
    assert re.search(r"^self-hosted contract at \S+: version-skew failed: "
                     r"service 0\.7\.4 is below the supported minimum",
                     captured.err, re.M)
    assert fm.main(["discover", "--target", "hosted", "--fixture", "current"]) == fm.EXIT_OK
    captured_discover = capsys.readouterr()
    assert captured_discover.err == ""
    lines = [json.loads(line) for line in
             (captured.out + captured_discover.out).splitlines()]
    assert all(line["schema"] == fm.CLI_SCHEMA for line in lines)
    assert lines[-1]["command"] == "discover"
    assert all(lines[-1]["capabilities"].values())


def test_skew_verdicts(fm, contract):
    assert fm.skew_verdict(contract, "0.8.0")["verdict"] == "supported"
    assert fm.skew_verdict(contract, "v1.2")["verdict"] == "supported"
    assert fm.skew_verdict(contract, "0.7.99")["verdict"] == "unsupported"
    assert fm.skew_verdict(contract, "dev")["verdict"] == "unknown"
    assert fm.redact_url("https://user:pw@app.example/x/?token=1") == "https://app.example/x"


def test_client_retries_honor_retry_after_and_protect_non_idempotent_posts(fm):
    with fm.FixtureServer("current") as server:
        sleeps = []
        client = fm.Client(server.base_url, sleep=_no_sleep(sleeps),
                           rng=random.Random(7))
        server.state.inject("/api/version", 503, (429, "2"))
        result = client.request("GET", "/api/version")
        assert result.status == 200 and result.attempts == 3
        assert sleeps[1] == 2.0 and 0.25 <= sleeps[0] <= 0.5
        assert client.telemetry["retries"] == 2
        assert client.telemetry["statusClasses"] == {"5xx": 1, "4xx": 1, "2xx": 1}

        server.state.inject("/v1/control/sync", 503)
        posted = client.request("POST", "/v1/control/sync", body={}, idempotent=False)
        assert posted.status == 503 and posted.attempts == 1

        server.state.inject("/v1/control/sync", (429, "0"))
        posted = client.request("POST", "/v1/control/sync", body={}, idempotent=False)
        assert posted.status == 202 and posted.attempts == 2
        again = client.request("POST", "/v1/control/sync", body={}, idempotent=False)
        assert again.json["queued"] is False

        server.state.inject("/api/status", *([500] * 10))
        exhausted = fm.Client(server.base_url, sleep=_no_sleep(),
                              policy=fm.RetryPolicy(max_attempts=3))
        assert exhausted.request("GET", "/api/status").status == 500

    unreachable = fm.Client("http://127.0.0.1:1", sleep=_no_sleep(),
                            policy=fm.RetryPolicy(max_attempts=2), timeout=2)
    with pytest.raises(fm.TransportError):
        unreachable.request("GET", "/healthz")
    assert unreachable.telemetry["transportErrors"] == 2


def test_retry_after_parsing(fm):
    assert fm.retry_after_seconds({"Retry-After": "7"}) == 7.0
    assert fm.retry_after_seconds({}, {"retryAfterMs": 1500}) == 1.5
    assert fm.retry_after_seconds({"retry-after": "Wed, 21 Oct 2015 07:28:00 GMT"}) == 0.0
    assert fm.retry_after_seconds({}) is None
    assert fm.RetryPolicy(max_delay=30).delay(9, 999) == 30


# ------------------------------------------------------------ event stream


def test_change_feed_sequences_changes_and_ignores_volatile_telemetry(fm):
    with fm.FixtureServer("next") as server:
        client = fm.Client(server.base_url)
        feed = fm.ChangeFeed(server.base_url)
        first = feed.observe(fm.fetch_snapshot(client, "hosted"))
        assert [(e["subject"], e["sequence"]) for e in first] == [
            ("example/hello", 1), ("forkmesh/forkmesh", 1)]
        assert all("cpuPercent" not in e["data"] for e in first)
        assert feed.observe(fm.fetch_snapshot(client, "hosted")) == []

        server.state.set_repository("example", "hello", description="changed")
        changed = feed.observe(fm.fetch_snapshot(client, "hosted"))
        assert [(e["type"], e["sequence"]) for e in changed] == [
            ("repository.changed", 2)]
        server.state.remove_repository("example", "hello")
        removed = feed.observe(fm.fetch_snapshot(client, "hosted"))
        assert [(e["type"], e["sequence"], e["data"]) for e in removed] == [
            ("repository.removed", 3, None)]
        restored = fm.ChangeFeed(server.base_url, json.loads(json.dumps(feed.state)))
        server.state.set_repository("example", "hello")
        back = restored.observe(fm.fetch_snapshot(client, "hosted"))
        assert [(e["type"], e["sequence"]) for e in back] == [
            ("repository.changed", 4)]
        assert len({e["id"] for e in first + changed + removed + back}) == 5

        node_feed = fm.ChangeFeed(server.base_url)
        node_events = node_feed.observe(fm.fetch_snapshot(client, "self-hosted"))
        assert {e["type"] for e in node_events} == {
            "mirror.node.changed", "mirror.repository.changed"}
    declared = {item["type"] for item in json.loads(
        CONTRACT.read_text(encoding="utf-8"))["interfaces"]["events"]["types"]}
    assert {e["type"] for e in first + removed + node_events} <= declared


def test_watch_cli_resumes_from_its_state_file(fm, tmp_path, capsys):
    state = tmp_path / "watch.json"
    with fm.FixtureServer("current") as server:
        argv = ["watch", "--target", "hosted", "--base-url", server.base_url,
                "--state", str(state), "--once", "--allow-fast-poll"]
        assert fm.main(argv) == fm.EXIT_OK
        assert len(capsys.readouterr().out.splitlines()) == 2
        assert fm.main(argv) == fm.EXIT_OK
        assert capsys.readouterr().out == ""
        server.state.set_repository("forkmesh", "forkmesh", commit="1" * 40)
        assert fm.main(argv) == fm.EXIT_OK
        events = [json.loads(line) for line in capsys.readouterr().out.splitlines()]
    assert [(e["subject"], e["sequence"]) for e in events] == [("forkmesh/forkmesh", 2)]
    assert fm.main(["watch", "--target", "hosted", "--base-url",
                    "http://127.0.0.1:1", "--once"]) == fm.EXIT_FAILED


def _history(fm, rng, subjects=4, changes=6):
    events = []
    final = {}
    for index in range(subjects):
        subject = "owner/repo-%d" % index
        for sequence in range(1, rng.randint(2, changes) + 1):
            removed = rng.random() < 0.15
            data = None if removed else {"commit": "%040x" % rng.getrandbits(160)}
            events.append(fm.make_event(
                "https://fixture", "repository.removed" if removed
                else "repository.changed", subject, sequence,
                "" if removed else "%064x" % rng.getrandbits(256), data, 0))
            final[subject] = data
    return events, {k: v for k, v in sorted(final.items()) if v is not None}


def test_ledger_converges_under_duplicates_delay_and_reordering(fm):
    for seed in range(300):
        rng = random.Random(seed)
        events, expected = _history(fm, rng)
        deliveries = events + [e for e in events if rng.random() < 0.3]
        rng.shuffle(deliveries)
        clock = {"now": 0.0}
        ledger = fm.EventLedger(clock=lambda: clock["now"], gap_timeout=60)
        applied_order = {}
        for event in deliveries:
            clock["now"] += 1
            for action, payload in ledger.accept(event) + ledger.expire():
                if action == "apply":
                    applied_order.setdefault(payload["subject"], []).append(
                        payload["sequence"])
        clock["now"] += 3600
        for action, payload in ledger.expire():
            if action == "apply":
                applied_order.setdefault(payload["subject"], []).append(
                    payload["sequence"])
        assert ledger.snapshot() == expected, seed
        assert ledger.pending == {}
        for sequences in applied_order.values():
            assert sequences == sorted(set(sequences)), seed


def test_ledger_resyncs_a_missing_event_then_drops_it_when_late(fm):
    clock = {"now": 0.0}
    ledger = fm.EventLedger(clock=lambda: clock["now"], gap_timeout=120)
    events = [fm.make_event("s", "repository.changed", "a/b", n, str(n), {"n": n}, 0)
              for n in range(1, 6)]
    assert [a for a, _ in ledger.accept(events[0])] == ["apply"]
    assert [a for a, _ in ledger.accept(events[1])] == ["apply"]
    assert [a for a, _ in ledger.accept(events[3])] == ["buffered"]
    assert [a for a, _ in ledger.accept(events[4])] == ["buffered"]
    assert [a for a, _ in ledger.accept(events[4])] == ["duplicate"]
    clock["now"] = 119
    assert ledger.expire() == []
    clock["now"] = 121
    actions = ledger.expire()
    assert actions[0] == ("resync", {"subject": "a/b", "missing": [3, 3], "resumeAt": 4})
    assert [(a, p["sequence"]) for a, p in actions[1:]] == [("apply", 4), ("apply", 5)]
    assert [a for a, _ in ledger.accept(events[2])] == ["stale"]
    assert ledger.snapshot() == {"a/b": {"n": 5}}

    bounded = fm.EventLedger(max_pending=2)
    bounded.accept(events[0])
    assert [a for a, _ in bounded.accept(events[2])] == ["buffered"]
    assert [a for a, _ in bounded.accept(events[3])] == ["buffered"]
    overflow = bounded.accept(events[4])
    assert [a for a, _ in overflow] == ["buffered", "resync", "apply", "apply", "apply"]


def test_ledger_persists_across_restart_and_supports_stream_resync(fm):
    events = [fm.make_event("s", "repository.changed", "a/b", n, str(n), {"n": n}, 0)
              for n in range(1, 4)]
    ledger = fm.EventLedger()
    ledger.accept(events[0])
    ledger.accept(events[2])
    restored = fm.EventLedger.from_dict(json.loads(json.dumps(ledger.to_dict())))
    assert [a for a, _ in restored.accept(events[0])] == ["duplicate"]
    assert [(a, p["sequence"]) for a, p in restored.accept(events[1])] == [
        ("apply", 2), ("apply", 3)]
    restored.reset()
    rebased = fm.make_event("s", "repository.changed", "a/b", 1, "new", {"n": 0}, 0)
    assert [a for a, _ in restored.accept(rebased)] == ["apply"]
    with pytest.raises(ValueError):
        restored.accept({"schema": fm.EVENT_SCHEMA, "id": "x", "sequence": 0})


# ------------------------------------------------------------ webhooks


def test_webhook_signatures_reject_tampering_staleness_and_accept_rotation(fm):
    event = fm.make_event("s", "repository.changed", "a/b", 1, "d", {"x": 1}, 0)
    body = fm.canonical_json(event)
    headers = fm.webhook_headers("old", event, body, 1, 1_000_000)
    assert fm.verify_webhook("old", headers, body, now=1_000_100) == event
    assert fm.verify_webhook(["new", "old"], headers, body, now=1_000_000) == event
    rotated = dict(headers)
    rotated["ForkMesh-Webhook-Signature"] = "%s, %s" % (
        fm.webhook_signature("new", event["id"], 1_000_000, body),
        headers["ForkMesh-Webhook-Signature"])
    assert fm.verify_webhook("new", rotated, body, now=1_000_000) == event
    for secrets, hdrs, payload, now, reason in (
        ("wrong", headers, body, 1_000_000, "signature_mismatch"),
        ("old", headers, body.replace(b'"x":1', b'"x":2'), 1_000_000,
         "signature_mismatch"),
        ("old", headers, body, 1_000_301, "timestamp_outside_tolerance"),
        ("old", {}, body, 1_000_000, "missing_signature_headers"),
    ):
        with pytest.raises(fm.WebhookVerificationError, match=reason):
            fm.verify_webhook(secrets, hdrs, payload, now=now)
    # The documented stand-alone verifier computes the same signature.
    doc = DOC.read_text(encoding="utf-8")
    assert 'b"forkmesh-webhook-v1\\n%d\\n%s\\n"' in doc


class _ScriptedReceiver:
    """A webhook endpoint answering scripted statuses per event id."""

    def __init__(self, fm, script):
        self.fm = fm
        self.script = {key: list(value) for key, value in script.items()}
        self.received = []
        outer = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_args):
                return

            def do_POST(self):  # noqa: N802
                body = self.rfile.read(int(self.headers["content-length"]))
                event = fm.verify_webhook(SECRET, self.headers.items(), body)
                outer.received.append((event["id"], self.headers["ForkMesh-Webhook-Attempt"]))
                queue = outer.script.get(event["id"]) or [200]
                status = queue.pop(0) if len(queue) > 1 else queue[0]
                self.send_response(status)
                if status == 429:
                    self.send_header("Retry-After", "0")
                self.send_header("Content-Length", "0")
                self.end_headers()

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        self.url = "http://127.0.0.1:%d/hook" % self.httpd.server_address[1]

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def test_delivery_retries_dead_letters_and_replays(fm, tmp_path, monkeypatch, capsys):
    events = [fm.make_event("s", "repository.changed", "a/b", n, str(n), {"n": n}, 0)
              for n in range(1, 5)]
    ok, flaky, rejected, gone = events
    receiver = _ScriptedReceiver(fm, {
        flaky["id"]: [503, 429, 200], rejected["id"]: [400], gone["id"]: [410],
    })
    try:
        sleeps = []
        policy = fm.RetryPolicy(max_attempts=4, base_delay=0.01)
        assert fm.deliver_event(receiver.url, SECRET, ok, policy=policy,
                                sleep=_no_sleep(sleeps))["delivered"]
        result = fm.deliver_event(receiver.url, SECRET, flaky, policy=policy,
                                  sleep=_no_sleep(sleeps))
        assert result == {"id": flaky["id"], "attempts": 3, "status": 200,
                          "delivered": True}
        assert sleeps[-1] == 0.0
        assert fm.deliver_event(receiver.url, SECRET, rejected, policy=policy,
                                sleep=_no_sleep())["reason"] == "rejected_400"
        assert fm.deliver_event(receiver.url, SECRET, gone, policy=policy,
                                sleep=_no_sleep())["reason"] == "subscription_gone"
        attempts = [attempt for event_id, attempt in receiver.received
                    if event_id == flaky["id"]]
        assert attempts == ["1", "2", "3"]

        source = tmp_path / "events.ndjson"
        source.write_text("\n".join(json.dumps(e) for e in (ok, rejected)) + "\n")
        dead = tmp_path / "dead.ndjson"
        monkeypatch.setenv("FORKMESH_WEBHOOK_SECRET", SECRET)
        assert fm.main(["deliver", "--url", receiver.url, "--input", str(source),
                        "--dead-letter", str(dead)]) == fm.EXIT_FAILED
        dead_records = [json.loads(line) for line in dead.read_text().splitlines()]
        assert [r["event"]["id"] for r in dead_records] == [rejected["id"]]
        receiver.script[rejected["id"]] = [200]
        assert fm.main(["deliver", "--url", receiver.url, "--input", str(dead),
                        "--dead-letter", str(dead)]) == fm.EXIT_OK
        assert dead.read_text() == ""
        lines = [json.loads(line) for line in capsys.readouterr().out.splitlines()]
        assert all(line["schema"] == fm.CLI_SCHEMA for line in lines)
        monkeypatch.delenv("FORKMESH_WEBHOOK_SECRET")
        assert fm.main(["deliver", "--url", receiver.url, "--input", str(source)]) \
            == fm.EXIT_USAGE
    finally:
        receiver.close()
    unreachable = fm.deliver_event("http://127.0.0.1:1/", SECRET, ok,
                                   policy=fm.RetryPolicy(max_attempts=2),
                                   sleep=_no_sleep(), timeout=2)
    assert unreachable["reason"] == "retries_exhausted" and unreachable["error"]


# ------------------------------------------------------------ examples


def _run_example(name, *args, env=None, timeout=60):
    return subprocess.run(
        [sys.executable, str(EXAMPLES / name), *args],
        capture_output=True, text=True, timeout=timeout,
        env=dict(os.environ, **(env or {})))


def test_discovery_and_migration_examples_against_the_fixture(fm, tmp_path):
    with fm.FixtureServer("current") as server:
        found = _run_example("discovery_catalog.py", "--base-url", server.base_url,
                             "--query", "decentralized")
        assert found.returncode == 0, found.stderr
        summary = json.loads(found.stdout)
        assert summary["repositories"] == 2 and summary["online"] == 2
        assert [m["repository"] for m in summary["matches"]] == ["forkmesh/forkmesh"]

        first = _run_example("migration_export.py", "--base-url", server.base_url,
                             "--output", str(tmp_path))
        assert first.returncode == 0, first.stderr
        assert json.loads(first.stdout)["added"] == 2
        server.state.set_repository("example", "hello", commit="2" * 40)
        second = json.loads(_run_example(
            "migration_export.py", "--base-url", server.base_url,
            "--output", str(tmp_path)).stdout)
        assert (second["added"], second["updated"], second["unchanged"]) == (0, 1, 1)
        manifest = json.loads((tmp_path / "forkmesh-export.json").read_text())
        assert manifest["schema"] == "forkmesh-export-v1"
        assert manifest["repositories"][0]["command"].startswith("git clone --mirror ")
    with fm.FixtureServer("unsupported") as server:
        old = _run_example("discovery_catalog.py", "--base-url", server.base_url)
        assert old.returncode == fm.EXIT_SKEW


def test_deployment_gate_example_waits_for_readiness_and_version(fm):
    with fm.FixtureServer("current") as server:
        server.state.ready = False
        timer = threading.Timer(1.0, lambda: setattr(server.state, "ready", True))
        timer.start()
        ready = _run_example("deployment_gate.py", "--base-url", server.base_url,
                             "--expect-version", "0.8.8", "--timeout", "20",
                             "--interval", "0.2")
        timer.cancel()
        assert ready.returncode == 0, ready.stderr
        assert json.loads(ready.stdout)["ready"] is True
        wrong = _run_example("deployment_gate.py", "--base-url", server.base_url,
                             "--expect-version", "9.9.9", "--timeout", "0.5",
                             "--interval", "0.2")
        assert wrong.returncode == fm.EXIT_FAILED
    with fm.FixtureServer("unsupported") as server:
        assert _run_example("deployment_gate.py", "--base-url", server.base_url,
                            "--timeout", "1").returncode == fm.EXIT_SKEW


def test_webhook_moderation_example_survives_hostile_delivery(fm, tmp_path):
    with fm.FixtureServer("current") as server:
        server.state.set_repository("spam", "bad", termsFlagged=False)
        client = fm.Client(server.base_url)
        feed = fm.ChangeFeed(server.base_url)
        events = feed.observe(fm.fetch_snapshot(client, "hosted"))
        server.state.set_repository("spam", "bad", termsFlagged=True)
        events += feed.observe(fm.fetch_snapshot(client, "hosted"))
        server.state.set_repository("example", "hello", description="v2")
        events += feed.observe(fm.fetch_snapshot(client, "hosted"))
        server.state.remove_repository("example", "hello")
        events += feed.observe(fm.fetch_snapshot(client, "hosted"))
    expected = {subject: data for subject, data in (
        (e["subject"], e["data"]) for e in sorted(events, key=lambda e: e["sequence"]))}
    expected = {k: v for k, v in sorted(expected.items()) if v is not None}

    rng = random.Random(510)
    deliveries = events + [events[0], events[-1], events[2]]
    rng.shuffle(deliveries)
    process = subprocess.Popen(
        [sys.executable, str(EXAMPLES / "webhook_moderation.py"),
         "--state", str(tmp_path / "ledger.json"), "--gap-timeout", "0",
         "--stop-after", str(len(deliveries))],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        env=dict(os.environ, FORKMESH_WEBHOOK_SECRET="rotated," + SECRET))
    try:
        url = json.loads(process.stdout.readline())["url"]
        forged = fm.deliver_event(url, "attacker", events[0],
                                  policy=fm.RetryPolicy(max_attempts=1))
        assert forged["reason"] == "rejected_401"
        for event in deliveries:
            assert fm.deliver_event(url, SECRET, event)["delivered"]
        out, err = process.communicate(timeout=30)
    finally:
        if process.poll() is None:
            process.kill()
    assert process.returncode == 0, err
    lines = [json.loads(line) for line in out.splitlines()]
    actions = [line["action"] for line in lines]
    assert actions.count("rejected") == 1
    assert actions.count("duplicate") == 3
    assert {"subject": "spam/bad"} == {
        k: v for k, v in next(l for l in lines if l["action"] == "moderation.flagged").items()
        if k == "subject"}
    assert lines[-1]["action"] == "final"
    assert lines[-1]["state"] == expected
    persisted = json.loads((tmp_path / "ledger.json").read_text())
    assert len(persisted["seen"]) == len(events)


# ------------------------------------------------------------ documentation


def test_documentation_publishes_the_whole_contract(fm, contract):
    doc = DOC.read_text(encoding="utf-8")
    for heading in ("## Pagination", "## Filtering", "## Idempotency",
                    "## Ordering and the event stream", "## Retries",
                    "### Signatures", "## Rate limits", "## Errors",
                    "## Deprecation", "## Version skew",
                    "## Services and authentication", "## Command-line output",
                    "## Examples", "## Contract tests and telemetry"):
        assert heading in doc
    for resource in contract["interfaces"]["http"]:
        if resource["kind"] == "resource":
            assert "`%s %s" % (resource["method"], resource["path"]) in doc
    for header in contract["interfaces"]["webhooks"]["outbound"]["headers"]:
        assert "`%s`" % header in doc
    for event_type in contract["interfaces"]["events"]["types"]:
        assert event_type["type"] in doc
    for command in fm.REQUIRED_CLI_COMMANDS:
        assert "`%s`" % command in doc
    for example in contract["examples"]:
        assert example["path"] in doc
    for code, meaning in contract["interfaces"]["cli"]["exitCodes"].items():
        assert "| %s | %s |" % (code, meaning) in doc
    readme = (ROOT / "www" / "docs" / "README.md").read_text(encoding="utf-8")
    assert "(integrations.md)" in readme
