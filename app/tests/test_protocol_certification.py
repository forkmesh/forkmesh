"""Protocol certification: profiles, adapter harness, fault detection, evidence, and expiring badges."""

import copy
import importlib.util
import json
import shutil
import subprocess
import sys
import time
from datetime import date, timedelta
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "protocol_certification.py"
CLEAN_ROOM = ["{python}", "conformance/certification/v1/adapters/clean_room_python.py"]
JAVASCRIPT = ["node", "conformance/certification/v1/adapters/webcrypto_javascript.js"]


def _module():
    spec = importlib.util.spec_from_file_location("forkmesh_protocol_certification", TOOL)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


pc = _module()


@pytest.fixture(scope="module")
def program():
    return pc.load_program()


@pytest.fixture(scope="module")
def registry():
    return pc.load_registry()


@pytest.fixture(scope="module")
def clean_room_report(program):
    return pc.run_certification(program, "core", CLEAN_ROOM, implementation_id="forkmesh-clean-room-python")


def _cli(*args):
    return subprocess.run(
        [sys.executable, str(TOOL), *args], cwd=ROOT, capture_output=True, text=True, check=False, timeout=300
    )


def test_program_registry_evidence_and_outputs_are_consistent():
    validated = _cli("validate")
    assert validated.returncode == 0, validated.stderr
    rendered = _cli("render", "--check")
    assert rendered.returncode == 0, rendered.stderr


def test_every_profile_can_detect_the_classes_it_requires(program):
    vectors = pc.load_vectors(ROOT, program)
    core = next(item for item in program["profiles"] if item["id"] == "core")
    assert set(core["detectionClasses"]) == {
        "malformed-signature", "divergence", "downgrade", "replay", "data-loss", "incompatible-version",
    }
    for profile in program["profiles"]:
        cases = pc.profile_cases(program, profile["id"], vectors)
        assert {item["kind"] for item in cases} == {"positive", "negative"}
        for class_id in profile["detectionClasses"]:
            assert any(class_id in item["classes"] for item in cases), (profile["id"], class_id)


def test_program_rejects_a_class_its_profile_cannot_detect(program, registry):
    broken = copy.deepcopy(program)
    fold = next(item for item in broken["profiles"] if item["id"] == "collaboration-fold")
    fold["detectionClasses"].append("downgrade")
    with pytest.raises(pc.CertificationError, match="cannot detect downgrade"):
        pc.validate_program(broken, registry)

    unknown = copy.deepcopy(program)
    unknown["detectionClasses"][0]["cases"].append("signatures:no-such-case")
    with pytest.raises(pc.CertificationError, match="unknown case signatures:no-such-case"):
        pc.validate_program(unknown, registry)


def test_registry_requires_a_reviewed_public_source_for_independence(program, registry):
    claimed = copy.deepcopy(registry)
    claimed["implementations"][0]["independent"] = True
    with pytest.raises(pc.CertificationError) as raised:
        pc.validate_program(program, claimed)
    assert "reviewer, date, and statement" in str(raised.value)
    assert "public https URL" in str(raised.value)


@pytest.mark.parametrize(
    ("expect", "response", "outcome"),
    [
        ({"valid": False, "error": "duplicate_key"}, {"valid": False, "error": "invalid_json"}, (True, "", "invalid_json")),
        ({"valid": True, "value": "a"}, {"valid": True, "value": "b"}, (False, "unexpected value 'b'", None)),
        ({"valid": False}, {"valid": True, "value": None}, (False, "expected valid=false, got None", None)),
        ({"value": "x"}, {"unsupported": True}, (False, "operation unsupported by the implementation", None)),
        ({"value": "x"}, {"value": "x"}, (False, "response has no boolean valid field", None)),
        (
            {"fold": {"stateDigest": "d", "telemetry": {"accepted": 1}, "rejectionReasons": {}}},
            {"valid": True, "value": {"stateDigest": "d", "telemetry": {"accepted": 2}, "rejectionReasons": {}}},
            (False, "telemetry differs: accepted", None),
        ),
    ],
    ids=["code-deviation-passes", "wrong-value", "accepts-invalid", "unsupported", "no-verdict", "fold-telemetry"],
)
def test_judge_separates_normative_failures_from_code_deviations(expect, response, outcome):
    assert pc.judge(expect, response) == outcome


def test_clean_room_python_passes_core(clean_room_report):
    assert clean_room_report["summary"]["result"] == "pass", [
        item for item in clean_room_report["cases"] if not item["ok"]
    ]
    assert clean_room_report["adapter"]["handshake"] == "compatible"
    assert all(item["failed"] == [] and item["cases"] > 0 for item in clean_room_report["detection"].values())


def test_webcrypto_javascript_passes_collaboration_fold_with_recorded_code_deviations(program):
    assert shutil.which("node"), "Node.js is required by the collaboration conformance gate"
    report = pc.run_certification(
        program, "collaboration-fold", JAVASCRIPT, implementation_id="forkmesh-webcrypto-javascript"
    )
    assert report["summary"]["result"] == "pass", [item for item in report["cases"] if not item["ok"]]
    manifest = json.loads((ROOT / "conformance/protocol/v1/manifest.json").read_text(encoding="utf-8"))
    recorded = next(item for item in manifest["implementations"] if item["id"] == "webcrypto-javascript")
    assert {
        item["case"].split(":", 1)[1]: item["actualError"] for item in report["cases"] if "actualError" in item
    } == recorded["codeDeviations"]


@pytest.mark.parametrize(
    ("fault", "detected"),
    [
        ("accept-malformed-signatures", "malformed-signature"),
        ("skip-normalization", "divergence"),
        ("downgrade-on-failure", "downgrade"),
        ("ignore-expiry", "replay"),
        ("lose-data", "data-loss"),
        ("accept-future-versions", "incompatible-version"),
    ],
)
def test_suite_detects_each_defect_class(program, fault, detected):
    report = pc.run_certification(
        program, "core", CLEAN_ROOM + ["--fault", fault], implementation_id="fault-probe", case_timeout=10
    )

    assert report["summary"]["result"] == "fail"
    assert report["detection"][detected]["failed"], report["detection"]
    # The fault breaks one property; the adapter itself keeps answering.
    assert not any("adapter exited" in item.get("detail", "") for item in report["cases"])
    assert report["summary"]["failed"] < report["summary"]["cases"] // 2


def test_version_skewed_adapter_is_incompatible_and_fails_every_case(program):
    report = pc.run_certification(
        program, "core", CLEAN_ROOM + ["--fault", "old-suite"], implementation_id="fault-probe"
    )

    assert report["adapter"]["handshake"] == "incompatible"
    assert "0.9.0" in report["adapter"]["detail"]
    assert report["summary"]["passed"] == 0 and report["summary"]["result"] == "fail"


def test_crashing_and_hanging_adapters_still_produce_failing_reports(program):
    crashed = pc.run_certification(
        program, "core", CLEAN_ROOM + ["--fault", "crash-midway"], implementation_id="fault-probe"
    )
    assert crashed["summary"]["passed"] == 10
    assert crashed["cases"][10]["detail"] == "adapter exited"
    assert crashed["cases"][11]["detail"] == "not run: adapter exited"

    began = time.monotonic()
    hung = pc.run_certification(
        program, "core", CLEAN_ROOM + ["--fault", "hang"], implementation_id="fault-probe", case_timeout=1.5
    )
    assert time.monotonic() - began < 30
    assert hung["summary"]["passed"] == 2
    assert hung["cases"][2]["detail"] == "timed out after 1.5s"
    assert hung["summary"]["result"] == "fail"


def test_performance_budget_fails_a_slow_implementation(program):
    tight = copy.deepcopy(program)
    tight["performance"]["maxCaseMilliseconds"] = 20
    tight["profiles"] = [
        {**item, "groups": ["canonical-json:encode"], "detectionClasses": ["divergence"]}
        for item in tight["profiles"] if item["id"] == "core"
    ]
    report = pc.run_certification(tight, "core", CLEAN_ROOM + ["--fault", "slow"], implementation_id="fault-probe")

    assert report["summary"]["result"] == "fail"
    assert report["performance"]["withinBudget"] is False
    assert all(item["detail"] == "exceeded the 20 ms case budget" for item in report["cases"])


def test_attest_rejects_reports_that_are_not_reproducible_evidence(program, registry, clean_room_report):
    faulted = copy.deepcopy(clean_room_report)
    faulted["adapter"]["command"] = CLEAN_ROOM + ["--fault", "lose-data"]
    with pytest.raises(pc.CertificationError, match="registered adapter command"):
        pc.attest(program, registry, {}, [faulted])

    unregistered = copy.deepcopy(clean_room_report)
    unregistered["implementation"]["id"] = "acme-relay"
    with pytest.raises(pc.CertificationError, match="acme-relay is not registered"):
        pc.attest(program, registry, {}, [unregistered])

    trimmed = copy.deepcopy(clean_room_report)
    trimmed["cases"] = [item for item in trimmed["cases"] if not item["case"].startswith("sessions:")]
    trimmed["summary"] = pc._summary(trimmed["cases"], "compatible", trimmed["performance"])
    trimmed["performance"]["totalMilliseconds"] = sum(item["elapsedMs"] for item in trimmed["cases"])
    with pytest.raises(pc.CertificationError, match="case list differs"):
        pc.attest(program, registry, {}, [trimmed])

    inflated = copy.deepcopy(clean_room_report)
    inflated["cases"][0]["ok"] = False
    with pytest.raises(pc.CertificationError, match="summary does not match"):
        pc.attest(program, registry, {}, [inflated])

    leaked = copy.deepcopy(clean_room_report)
    leaked["cases"][0]["detail"] = "loaded /home/alice/impl/vectors.json"
    with pytest.raises(pc.CertificationError, match="local path"):
        pc.attest(program, registry, {}, [leaked])


def test_attest_keeps_the_newest_run_per_configuration(program, registry, clean_room_report):
    older = copy.deepcopy(clean_room_report)
    older["startedAt"] = older["finishedAt"] = "2026-01-01T00:00:00Z"

    retained = pc.attest(program, registry, {}, [clean_room_report])
    assert pc.attest(program, registry, retained, [older]) == retained
    replaced = pc.attest(program, registry, {pc.report_id(older): older}, [clean_room_report])
    assert list(replaced) == [pc.report_id(clean_room_report)]


def _badge(published, implementation="forkmesh-clean-room-python", profile="core"):
    return next(
        item for item in published["badges"] if item["implementation"] == implementation and item["profile"] == profile
    )


def test_badges_expire_with_age_suite_changes_failures_and_revocation(program, registry, clean_room_report):
    tested = date.fromisoformat(clean_room_report["finishedAt"][:10])
    reports = {pc.report_id(clean_room_report): clean_room_report}

    fresh = _badge(pc.derive_badges(program, registry, reports, as_of=tested + timedelta(days=90)))
    assert fresh["status"] == "certified"
    assert fresh["expiresAt"] == (tested + timedelta(days=90)).isoformat()
    assert fresh["evidence"] == f"/docs/certification/evidence/{pc.report_id(clean_room_report)}.json"
    assert "--implementation forkmesh-clean-room-python --profile core" in fresh["reproduce"]

    aged = _badge(pc.derive_badges(program, registry, reports, as_of=tested + timedelta(days=91)))
    assert (aged["status"], aged["reason"]) == ("expired", "evidence is older than 90 days")

    changed = copy.deepcopy(clean_room_report)
    changed["suite"]["vectorsSha256"] = "0" * 64
    suite_changed = _badge(pc.derive_badges(program, registry, {"x": changed}, as_of=tested))
    assert suite_changed["status"] == "expired" and "changed after testing" in suite_changed["reason"]

    failing = copy.deepcopy(clean_room_report)
    failing["summary"]["result"] = "fail"
    assert _badge(pc.derive_badges(program, registry, {"x": failing}, as_of=tested))["status"] == "failing"

    multi_platform = copy.deepcopy(registry)
    multi_platform["implementations"][0]["platforms"] = ["linux", "windows"]
    unverified = _badge(pc.derive_badges(program, multi_platform, reports, as_of=tested))
    assert (unverified["status"], unverified["reason"]) == ("unverified", "no retained run on windows")

    revoked_registry = copy.deepcopy(registry)
    revoked_registry["implementations"][0]["revocations"] = [
        {"profile": "core", "date": "2026-09-14", "reason": "evidence not reproducible"}
    ]
    assert _badge(pc.derive_badges(program, revoked_registry, reports, as_of=tested))["status"] == "revoked"


def test_closure_gate_counts_only_independent_implementations(program, registry, clean_room_report):
    tested = date.fromisoformat(clean_room_report["finishedAt"][:10])
    reports = {pc.report_id(clean_room_report): clean_room_report}

    assert pc.derive_badges(program, registry, reports, as_of=tested)["closureGate"]["met"] is False

    independent = copy.deepcopy(registry)
    independent["implementations"][0]["independent"] = True
    gate = pc.derive_badges(program, independent, reports, as_of=tested)["closureGate"]
    assert gate["met"] is True and gate["certified"] == ["forkmesh-clean-room-python"]


def test_published_registry_does_not_claim_the_closure_gate():
    published = json.loads((ROOT / "www/public/docs/certification/badges.json").read_text(encoding="utf-8"))
    assert published["closureGate"]["met"] is False
    assert not any(item["independent"] for item in published["badges"])
    assert "Closure gate **not met**" in (ROOT / "www/docs/protocol/certification-registry.md").read_text(
        encoding="utf-8"
    )


def _copy_evidence_root(destination, program):
    manifest_path = program["protocolSuite"]
    manifest = json.loads((ROOT / manifest_path).read_text(encoding="utf-8"))
    paths = [manifest_path] + [entry["path"] for entry in manifest["vectors"]]
    for path in paths:
        (destination / path).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / path, destination / path)
    shutil.copytree(ROOT / "conformance/certification", destination / "conformance/certification")
    return destination


def test_editing_retained_evidence_or_the_results_index_fails_validation(tmp_path, program):
    root = _copy_evidence_root(tmp_path, program)
    reports, _attested = pc.load_evidence(program, root)
    assert reports

    results = root / "conformance/certification/v1/results.json"
    original = results.read_text(encoding="utf-8")
    results.write_text(original.replace('"result": "pass"', '"result": "fail"', 1), encoding="utf-8")
    with pytest.raises(pc.CertificationError, match="does not match retained evidence"):
        pc.load_evidence(program, root)
    results.write_text(original, encoding="utf-8")

    evidence = next((root / "conformance/certification/v1/evidence").glob("*.json"))
    report = json.loads(evidence.read_text(encoding="utf-8"))
    report["implementation"]["version"] = "9.9.9"
    evidence.write_text(json.dumps(report), encoding="utf-8")
    with pytest.raises(pc.CertificationError, match="content address"):
        pc.load_evidence(program, root)


def test_gate_is_wired_into_quality_profiles_and_ci():
    config = json.loads((ROOT / "www/docs/engineering/quality-gates.json").read_text(encoding="utf-8"))
    gate = next(item for item in config["gates"] if item["id"] == "protocol-certification")
    assert gate["command"] == ["{python}", "tools/protocol_certification.py", "gate"]
    assert "protocol-certification" in config["profiles"]["core"]
    assert "protocol-certification" in config["profiles"]["release"]

    workflow = (ROOT / ".github/workflows/collaboration-conformance.yml").read_text(encoding="utf-8")
    assert "python tools/protocol_certification.py render --check" in workflow
    assert "--implementation forkmesh-clean-room-python" in workflow
    assert "--implementation forkmesh-webcrypto-javascript" in workflow

    wrangler = (ROOT / "www/wrangler.toml").read_text(encoding="utf-8")
    assert '"/docs/certification/badges/*",' in wrangler


def test_hosted_badge_rechecks_expiry_on_every_request():
    harness = r'''
import fs from "node:fs";
import assert from "node:assert";
const source = fs.readFileSync(process.argv[1], "utf8");
const worker = (await import(
  "data:text/javascript;base64," + Buffer.from(source).toString("base64")
)).default;
const day = (offset) => new Date(Date.now() + offset * 86400000).toISOString().slice(0, 10);
const badges = {
  badges: [
    { implementation: "acme", profile: "core", status: "certified", expiresAt: day(0),
      evidence: "/docs/certification/evidence/aaaaaaaaaaaaaaaa.json" },
    { implementation: "stale", profile: "core", status: "certified", expiresAt: day(-1),
      evidence: "/docs/certification/evidence/bbbbbbbbbbbbbbbb.json" },
    { implementation: "broken", profile: "core", status: "failing",
      evidence: "https://attacker.example/evidence.json" },
    { implementation: "<script>", profile: "core", status: "certified", expiresAt: day(5) },
  ],
};
const env = {
  APP_ORIGIN: "https://app.forkmesh.com",
  ASSETS: {
    async fetch(request) {
      assert.equal(new URL(request.url).pathname, "/docs/certification/badges.json");
      return new Response(JSON.stringify(badges), { headers: { "content-type": "application/json" } });
    },
  },
};
const get = (path, init) => worker.fetch(new Request("https://forkmesh.com" + path, init), env, {});

const current = await get("/docs/certification/badges/acme/core.svg");
assert.equal(current.status, 200);
assert.equal(current.headers.get("content-type"), "image/svg+xml; charset=utf-8");
assert.equal(current.headers.get("x-forkmesh-certification-status"), "certified");
assert.match(await current.text(), new RegExp("certified to " + day(0)));

const expired = await get("/docs/certification/badges/stale/core.svg");
assert.equal(expired.headers.get("x-forkmesh-certification-status"), "expired");
assert.doesNotMatch(await expired.text(), /certified/);

assert.equal((await get("/docs/certification/badges/broken/core.svg")).headers.get("x-forkmesh-certification-status"), "failing");
const missing = await get("/docs/certification/badges/nobody/core.svg");
assert.equal(missing.headers.get("x-forkmesh-certification-status"), "unverified");

const link = await get("/docs/certification/badges/acme/core");
assert.equal(link.status, 302);
assert.equal(link.headers.get("location"), "https://forkmesh.com/docs/certification/evidence/aaaaaaaaaaaaaaaa.json");
assert.equal((await get("/docs/certification/badges/broken/core")).status, 404);
assert.equal((await get("/docs/certification/badges/nobody/core")).status, 404);
assert.equal((await get("/docs/certification/badges/acme/core.svg", { method: "POST" })).status, 405);
const head = await get("/docs/certification/badges/acme/core.svg", { method: "HEAD" });
assert.equal(head.status, 200);
assert.equal(await head.text(), "");
'''
    completed = subprocess.run(
        ["node", "--input-type=module", "-e", harness, str(ROOT / "www/worker.js")],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
        timeout=60,
    )
    assert completed.returncode == 0, completed.stderr
