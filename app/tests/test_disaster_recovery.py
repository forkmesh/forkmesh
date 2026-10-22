"""Disaster-mode recovery runbook, bootstrap kit, drills, and exercises (issue #518).

Drives tools/forkmesh_disaster_recovery.py: runbook validation and its failure
modes, the printable runbook, roster checks, recovery-key rotation with its
threshold and refusals, the signed bootstrap kit verified with the network
blocked, every live drill and its fault injection, exercise records bound to
content-addressed drill evidence, closure status, telemetry, and the
documentation and gate wiring.
"""

import copy
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
TOOL_PATH = ROOT / "tools" / "forkmesh_disaster_recovery.py"
DOC = ROOT / "www" / "docs" / "disaster-recovery" / "README.md"


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


tool = _load("forkmesh_disaster_recovery", TOOL_PATH)
offline = tool.offline
RUNBOOK = tool.load_runbook()


def cli(*args, cwd=ROOT):
    return subprocess.run([sys.executable, str(TOOL_PATH), *map(str, args)], cwd=cwd,
                          capture_output=True, text=True, check=False, timeout=600)


def public(seed):
    return tool._b64(offline.ed25519_public_key(seed))


def write_seed(path, seed):
    path.write_text(tool._b64(seed) + "\n", encoding="utf-8")
    return path


@pytest.fixture
def keys():
    return {"export": bytes([1] * 32), "kit": bytes([2] * 32),
            "recovery": [bytes([10 + i] * 32) for i in range(3)], "successor": bytes([9] * 32)}


@pytest.fixture
def roster(keys):
    value = tool._roster(keys["export"], keys["recovery"], [{
        "name": "lan mirror", "urls": ["http://mirror.example.lan:8080/project.git"],
        "pinnedHosts": {"mirror.example.lan": "10.0.0.20"}}])
    value["updated"] = "2026-09-14"
    return value


# --- Runbook -------------------------------------------------------------------------

def test_runbook_validates_and_rendered_documents_are_current():
    assert tool.runbook_errors(RUNBOOK) == []
    assert tool.gate_problems() == []
    result = cli("gate")
    assert result.returncode == 0, result.stderr


def test_runbook_covers_every_scenario_topic_and_role():
    scenarios = {s["id"] for s in RUNBOOK["scenarios"]}
    assert set(tool.REQUIRED_SCENARIOS) <= scenarios
    assert {p["topic"] for p in RUNBOOK["procedures"]} == set(tool.TOPICS)
    assert set(tool.REQUIRED_ROLES) <= {r["id"] for r in RUNBOOK["roles"]}
    assert scenarios == set(tool.DRILLS) == set(tool.DRILL_FAULTS)
    for scenario in RUNBOOK["scenarios"]:
        assert scenario["drill"]["faults"] == list(tool.DRILL_FAULTS[scenario["id"]])


def _mutate(edit):
    runbook = copy.deepcopy(RUNBOOK)
    edit(runbook)
    return tool.runbook_errors(runbook)


def _scenario(runbook, scenario_id):
    return next(s for s in runbook["scenarios"] if s["id"] == scenario_id)


def _procedure(runbook, procedure_id):
    return next(p for p in runbook["procedures"] if p["id"] == procedure_id)


@pytest.mark.parametrize("edit, expected", [
    (lambda r: r["scenarios"].remove(_scenario(r, "dns-loss")), "missing required scenario dns-loss"),
    (lambda r: _scenario(r, "origin-loss")["decisionTree"]["nodes"]["return"].update(next="discover"), "cycle through"),
    (lambda r: _scenario(r, "origin-loss")["decisionTree"]["nodes"].update(orphan={"outcome": "escalate", "text": "x"}),
     "node orphan is unreachable"),
    (lambda r: _scenario(r, "origin-loss")["decisionTree"]["nodes"]["restore"].update(procedure="no-such"),
     "unknown procedure no-such"),
    (lambda r: _scenario(r, "origin-loss")["decisionTree"]["nodes"]["copy"].update(no="missing"),
     "points to missing node 'missing'"),
    (lambda r: _procedure(r, "carry-removable-media")["steps"][0]["run"].__setitem__(2, "exprot"),
     "has no command exprot"),
    (lambda r: _procedure(r, "carry-removable-media")["steps"][2]["run"].append("--allow-everything"),
     "has no option --allow-everything"),
    (lambda r: _procedure(r, "restore-node-backup")["steps"][0]["run"].append("--restore-everything"),
     "forkmesh-mirror-node has no flag --restore-everything"),
    (lambda r: _procedure(r, "choose-good-backup").update(rollback=[]), "procedure choose-good-backup needs rollback"),
    (lambda r: _procedure(r, "clone-over-lan-without-dns").update(docs=["www/docs/transports.md#no-such-heading"]),
     "has no heading #no-such-heading"),
    (lambda r: r["kit"]["files"].remove("www/docs/transports.md"), "kit.files is missing www/docs/transports.md"),
    (lambda r: r["kit"]["files"].remove("tools/forkmesh_offline_sync.py"),
     "kit.files is missing tools/forkmesh_offline_sync.py"),
    (lambda r: _procedure(r, "clone-over-lan-without-dns").update(topic="removable-media"), "topic local-network has no procedure"),
    (lambda r: _scenario(r, "key-loss")["drill"]["faults"].append("meteor"), "does not implement fault meteor"),
    (lambda r: _scenario(r, "key-loss")["drill"]["procedures"].append("serve-static-emergency-mirror"),
     "drill procedure serve-static-emergency-mirror is not in its decision tree"),
    (lambda r: (r["triage"]["nodes"]["newcomer"].update(yes="no-incident"), r["triage"]["nodes"].pop("go-offline-bootstrap")),
     "triage never reaches scenario offline-bootstrap"),
    (lambda r: r["roles"].pop(), "missing required role scribe"),
    (lambda r: _procedure(r, "verify-bootstrap-kit")["steps"][0].update(role="bystander"), "unknown role 'bystander'"),
])
def test_validation_rejects_broken_runbooks(edit, expected):
    errors = _mutate(edit)
    assert any(expected in error for error in errors), errors


def test_printable_runbook_carries_every_procedure_warning_and_worksheet_field():
    text = (ROOT / tool.RUNBOOK_DOC_REL).read_text(encoding="utf-8")
    assert tool.runbook_digest(RUNBOOK) in text
    for proc in RUNBOOK["procedures"]:
        assert "### %s (`%s`)" % (proc["title"], proc["id"]) in text
        for step in proc["steps"]:
            if step.get("unsafe"):
                assert "**Unsafe:** %s" % step["unsafe"] in text
    for scenario in RUNBOOK["scenarios"]:
        assert "### %s (`%s`)" % (scenario["title"], scenario["id"]) in text
    for field in ("Recovered and verified at (recovery time)", "Data loss", "Missing prerequisites",
                  "Unsafe steps taken or refused", "Findings and their corrective owners",
                  "unfamiliar with ForkMesh", "Only the offline runbook and kit were used"):
        assert field in text
    anchors = {tool.heading_anchor(line.lstrip("#")) for line in text.splitlines() if line.startswith("#")}
    for target in re.findall(r"\]\(#([^)]+)\)", text):
        assert target in anchors, target


# --- Roster and key rotation ----------------------------------------------------------

def test_roster_check_warns_and_refuses(roster, tmp_path):
    problems, warnings, summary = tool.roster_problems(roster)
    assert problems == [] and warnings == [] and summary["dnsFree"] is True
    no_pins = copy.deepcopy(roster)
    no_pins["mirrors"][0]["pinnedHosts"] = {}
    assert any("without DNS" in w for w in tool.roster_problems(no_pins)[1])
    short = copy.deepcopy(roster)
    short["recoveryThreshold"] = 4
    assert any("not met" in p for p in tool.roster_problems(short)[0])
    same_holder = copy.deepcopy(roster)
    for key in same_holder["keys"]:
        key["holder"] = "one person"
    assert any("not met" in p for p in tool.roster_problems(same_holder)[0])
    path = tmp_path / "roster.json"
    path.write_text(json.dumps(roster))
    assert cli("roster", "check", path).returncode == 0
    path.write_text(json.dumps(short))
    assert cli("roster", "check", path).returncode == 3


def test_roster_profile_pins_addresses_for_a_dns_free_route(roster, tmp_path):
    import forkmesh_transports as transports
    out = tmp_path / "lan.json"
    assert cli("roster", "profile", _dump(tmp_path, roster), "--out", out).returncode == 0
    profile = transports.load_profile(out)
    planned = transports.plan(profile, "http://mirror.example.lan:8080/project.git")
    assert planned["ok"] and planned["routes"][0]["dns"] == "pinned"
    assert not transports.plan(profile, "http://mirror.elsewhere.example/project.git")["ok"]


def _dump(tmp_path, value, name="roster.json"):
    path = tmp_path / name
    path.write_text(json.dumps(value), encoding="utf-8")
    return path


def test_key_rotation_needs_the_threshold_and_refuses_the_lost_key(roster, keys):
    args = dict(revoke=[public(keys["export"])], successor=public(keys["successor"]),
                holder="operator", reason="lost", issued="2026-09-14")
    with pytest.raises(tool.VerificationError, match="signer_not_recovery_key"):
        tool.make_rotation(roster, keys["export"], **args)
    rotation = tool.make_rotation(roster, keys["recovery"][0], **args)
    with pytest.raises(tool.VerificationError, match="below_threshold: 1 of 2"):
        tool.apply_rotation(roster, rotation)
    with pytest.raises(tool.UsageError):
        tool.cosign_rotation(rotation, keys["recovery"][0], roster)
    rotation = tool.cosign_rotation(rotation, keys["recovery"][1], roster)
    updated = tool.apply_rotation(roster, rotation)
    status = {k["publicKey"]: k["status"] for k in updated["keys"]}
    assert status[public(keys["export"])] == "revoked" and status[public(keys["successor"])] == "active"
    assert tool.roster_keys(updated, "export") == {offline.ed25519_public_key(keys["successor"])}
    with pytest.raises(tool.VerificationError, match="already_applied"):
        tool.apply_rotation(updated, rotation)
    tampered = copy.deepcopy(rotation)
    tampered["statement"]["successor"]["holder"] = "someone else"
    with pytest.raises(tool.VerificationError, match="statement_digest"):
        tool.apply_rotation(roster, tampered)
    tampered["statementSha256"] = tool.sha256_json(tampered["statement"])
    with pytest.raises(tool.VerificationError, match="bad_signature"):
        tool.apply_rotation(roster, tampered)
    recovery_revoke = tool.make_rotation(roster, keys["recovery"][0], **dict(args, revoke=[public(keys["recovery"][2])]))
    recovery_revoke = tool.cosign_rotation(recovery_revoke, keys["recovery"][1], roster)
    with pytest.raises(tool.VerificationError, match="revoke_recovery_key"):
        tool.apply_rotation(roster, recovery_revoke)


def test_key_rotation_commands(roster, keys, tmp_path):
    roster_path = _dump(tmp_path, roster)
    rotation = tmp_path / "rotation.json"
    common = ["--revoke", public(keys["export"]), "--successor", public(keys["successor"]),
              "--holder", "operator", "--reason", "lost", "--issued", "2026-09-14", "--out", rotation]
    lost = write_seed(tmp_path / "lost.key", keys["export"])
    assert cli("key", "rotate", "--roster", roster_path, "--recovery-key", lost, *common).returncode == 3
    first = write_seed(tmp_path / "r1.key", keys["recovery"][0])
    second = write_seed(tmp_path / "r2.key", keys["recovery"][1])
    assert cli("key", "rotate", "--roster", roster_path, "--recovery-key", first, *common).returncode == 0
    assert cli("key", "apply", roster_path, rotation, "--out", tmp_path / "new.json").returncode == 3
    assert cli("key", "cosign", rotation, "--recovery-key", second, "--roster", roster_path).returncode == 0
    result = cli("key", "apply", roster_path, rotation, "--out", tmp_path / "new.json")
    assert result.returncode == 0, result.stderr
    keys_out = cli("roster", "keys", tmp_path / "new.json", "--role", "export").stdout.split()
    assert keys_out == [public(keys["successor"])]


# --- Bootstrap kit ----------------------------------------------------------------------

@pytest.fixture
def kit(tmp_path, keys, roster):
    directory = tmp_path / "kit"
    tool.build_kit(directory, keys["kit"], roster=roster, built="2026-09-14")
    return directory


def test_kit_round_trip_with_pure_python_signatures(monkeypatch, tmp_path, keys, roster):
    monkeypatch.setitem(sys.modules, "cryptography.hazmat.primitives.asymmetric.ed25519", None)
    directory = tmp_path / "kit"
    envelope = tool.build_kit(directory, keys["kit"], roster=roster, built="2026-09-14")
    manifest = envelope["manifest"]
    assert set(manifest["files"]) == set(RUNBOOK["kit"]["files"]) | {tool.KIT_ROSTER}
    assert manifest["reviewBy"] == "2026-12-13"
    trusted = {offline.ed25519_public_key(keys["kit"])}
    report = tool.verify_kit(directory, trusted, today=tool.parse_date("2026-09-20"))
    assert report["state"] == "verified", report["problems"]
    assert report["runbookSha256"] == tool.runbook_digest(RUNBOOK)
    assert tool.verify_kit(directory, trusted, today=tool.parse_date("2026-12-14"))["state"] == "stale"


def test_kit_refuses_wrong_keys_and_any_change(kit, keys):
    trusted = {offline.ed25519_public_key(keys["kit"])}
    assert tool.verify_kit(kit, set())["state"] == "failed"
    assert tool.verify_kit(kit, {offline.ed25519_public_key(keys["export"])})["state"] == "failed"

    def broken(change):
        target = kit.parent / ("copy-%d" % len(list(kit.parent.iterdir())))
        shutil.copytree(kit, target)
        change(target)
        report = tool.verify_kit(target, trusted)
        assert report["state"] == "failed"
        return report["problems"]

    assert "digest mismatch: www/docs/offline-sync.md" in broken(
        lambda d: (d / "www/docs/offline-sync.md").write_text("changed"))
    assert "unlisted file in the kit: tools/extra.py" in broken(lambda d: (d / "tools/extra.py").write_text("x"))
    assert "missing file: tools/forkmesh_transports.py" in broken(lambda d: (d / "tools/forkmesh_transports.py").unlink())
    assert any("does not match the manifest" in p or "signature" in p for p in broken(
        lambda d: (d / tool.KIT_MANIFEST).write_text(
            (d / tool.KIT_MANIFEST).read_text().replace('"reviewBy": "2026-12-13"', '"reviewBy": "2099-12-13"'))))
    if os.name != "nt":
        assert "the kit contains a symbolic link: link" in broken(lambda d: os.symlink("/etc/passwd", d / "link"))


def test_kit_built_from_stale_documents_is_refused(tmp_path, keys, monkeypatch):
    fake = tmp_path / "repo"
    for rel in RUNBOOK["kit"]["files"] + ["server/cmd/forkmesh-mirror-node/main.go"]:
        (fake / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / rel, fake / rel)
    (fake / tool.RUNBOOK_DOC_REL).write_text("# edited by hand\n")
    with pytest.raises(tool.UsageError, match="out of date"):
        tool.build_kit(tmp_path / "kit", keys["kit"], root=fake)


def test_kit_verifies_from_its_own_tools_with_the_network_blocked(kit, keys):
    kit_tool = kit / "tools" / "forkmesh_disaster_recovery.py"
    result = tool._run_offline(kit_tool, "kit", "verify", kit, "--trusted-key", public(keys["kit"]), "--today", "2026-09-20")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "network blocked" not in result.stderr
    assert tool.runbook_digest(RUNBOOK) in result.stdout
    assert tool._run_offline(kit_tool, "kit", "verify", kit, "--trusted-key", public(keys["export"])).returncode == 3
    assert tool._run_offline(kit_tool, "kit", "verify", kit, "--trusted-key", public(keys["kit"]),
                             "--today", "2027-01-01").returncode == 4
    assert not list(kit.rglob("__pycache__"))
    assert tool.verify_kit(kit, {offline.ed25519_public_key(keys["kit"])})["problems"] == []


# --- Live drills --------------------------------------------------------------------------

def _tree_procedures(scenario_id):
    return tool.tree_procedures(_scenario(RUNBOOK, scenario_id)["decisionTree"])


@pytest.mark.parametrize("scenario_id", [s["id"] for s in RUNBOOK["scenarios"]])
def test_each_drill_recovers_within_its_objective(scenario_id):
    report = tool.run_drill(scenario_id)
    assert report["verdict"] == "pass", tool._describe_report(report)
    assert report["withinObjective"] and report["dataLoss"]["withinPolicy"]
    assert report["runbookSha256"] == tool.runbook_digest(RUNBOOK)
    assert report["verification"] and all(check["ok"] for check in report["verification"])
    assert {step["procedure"] for step in report["steps"]} <= _tree_procedures(scenario_id)
    assert set(_scenario(RUNBOOK, scenario_id)["drill"]["procedures"]) <= {s["procedure"] for s in report["steps"]}
    assert not [m for m in report["missingPrerequisites"] if m["blocking"]]
    if scenario_id != "corrupted-backup":
        assert report["dataLoss"]["commitsLost"] == 0


@pytest.mark.parametrize("scenario_id, fault", [
    (scenario_id, fault) for scenario_id, faults in tool.DRILL_FAULTS.items() for fault in faults])
def test_each_fault_fails_and_names_a_missing_prerequisite(scenario_id, fault):
    report = tool.run_drill(scenario_id, fault=fault)
    assert report["verdict"] == "fail"
    prerequisites = {p["id"] for p in RUNBOOK["prerequisites"]}
    assert report["missingPrerequisites"]
    assert {m["prerequisite"] for m in report["missingPrerequisites"]} <= prerequisites


def test_unsafe_steps_are_refused_not_taken():
    for scenario_id in ("origin-loss", "key-loss", "corrupted-backup", "offline-bootstrap"):
        report = tool.run_drill(scenario_id)
        assert report["unsafeSteps"]["refused"] and not report["unsafeSteps"]["taken"], scenario_id


def test_corrupted_backup_loses_only_commits_newer_than_the_verified_copy():
    report = tool.run_drill("corrupted-backup")
    assert report["dataLoss"]["commitsLost"] >= 1
    assert report["dataLoss"]["policy"] == "since-last-good-copy" and report["dataLoss"]["withinPolicy"]


def test_offline_bootstrap_passes_keys_that_start_with_a_dash(monkeypatch):
    seeds = iter([bytes([n] * 32) for n in (41, 62, 83, 130, 172)] * 4)
    real = os.urandom
    monkeypatch.setattr(tool.os, "urandom", lambda n: next(seeds) if n == 32 else real(n))
    assert public(bytes([41] * 32)).startswith("-")
    report = tool.run_drill("offline-bootstrap")
    assert report["verdict"] == "pass", tool._describe_report(report)


@pytest.mark.skipif(not (sys.platform.startswith("linux") and shutil.which("strace")), reason="needs strace")
def test_dns_loss_drill_traces_git_for_dns_queries():
    report = tool.run_drill("dns-loss")
    assert report["verdict"] == "pass"
    assert not report["missingPrerequisites"]


def test_drill_command_exit_codes_reports_and_count_only_telemetry(tmp_path):
    telemetry, report_path = tmp_path / "drills.jsonl", tmp_path / "report.json"
    result = cli("drill", "run", "origin-loss", "--report", report_path, "--telemetry", telemetry)
    assert result.returncode == 0, result.stdout + result.stderr
    assert json.loads(report_path.read_text())["scenario"] == "origin-loss"
    assert cli("drill", "run", "origin-loss", "--fault", "no-local-clone", "--telemetry", telemetry).returncode == 1
    assert cli("drill", "run", "meteor-strike").returncode == 2
    assert cli("drill", "run", "origin-loss", "--fault", "wrong-kit-key").returncode == 2
    records = [json.loads(line) for line in telemetry.read_text().splitlines()]
    assert [r["verdict"] for r in records] == ["pass", "fail"]
    text = telemetry.read_text()
    for private in (str(tmp_path), "drill community", "127.0.0.1", "refs/", "custodian"):
        assert private not in text


# --- Exercises, evidence, and closure -----------------------------------------------------------

def test_committed_exercises_validate_against_their_evidence():
    exercises = tool.load_exercises()
    assert exercises, "record at least the automated drill exercise"
    for name, exercise in exercises:
        assert tool.exercise_errors(name, exercise, RUNBOOK) == []


def test_exercise_validation_requires_owners_and_matching_evidence():
    name, exercise = next((n, e) for n, e in tool.load_exercises()
                          if any(r.get("drillReport") for r in e["scenarios"]))

    def errors(edit):
        record = copy.deepcopy(exercise)
        edit(record)
        return tool.exercise_errors(name, record, RUNBOOK)

    row = next(i for i, r in enumerate(exercise["scenarios"]) if r.get("drillReport"))
    assert any("recoveryMinutes" in e for e in errors(
        lambda r: r["scenarios"][row].update(recoveryMinutes=r["scenarios"][row]["recoveryMinutes"] + 1)))
    assert any("result" in e for e in errors(lambda r: r["scenarios"][row].update(result="partial")))
    assert any("does not exist" in e for e in errors(
        lambda r: r["scenarios"][row].update(drillReport="evidence/" + "0" * 64 + ".json")))
    assert any("names a finding" in e for e in errors(
        lambda r: r["scenarios"][row]["missingPrerequisites"].append({"prerequisite": "local-network"})))
    assert any("was taken names a finding" in e for e in errors(
        lambda r: r["scenarios"][row]["unsafeSteps"].append({"description": "forced a push", "taken": True})))
    if exercise.get("findings"):
        assert any("corrective owner" in e for e in errors(lambda r: r["findings"][0].pop("correctiveOwner")))
    other_version = copy.deepcopy(RUNBOOK)
    other_version["updated"] = "2099-01-01"
    assert any("changed without a version bump" in e for e in tool.exercise_errors(name, exercise, other_version))


def test_attested_evidence_is_content_addressed(tmp_path):
    report = tool.run_drill("dns-loss")
    source = _dump(tmp_path, [report], "reports.json")
    (reference,) = tool.attest([source], root=tmp_path)
    loaded, problem = tool.load_evidence(tmp_path, reference)
    assert problem is None and loaded == report
    stored = tmp_path / tool.EVIDENCE_REL / reference.split("/", 1)[1]
    stored.write_text(stored.read_text().replace('"pass"', '"fail"'))
    assert "does not match its digest" in tool.load_evidence(tmp_path, reference)[1]


def test_closure_stays_open_until_people_run_the_exercises():
    checks = {c["id"]: c for c in tool.closure_checks(RUNBOOK, tool.load_exercises())}
    for check_id in ("tabletop-drills-recorded", "live-drills-recorded", "unfamiliar-team-restored", "platform-evidence"):
        assert not checks[check_id]["ok"], check_id
    assert cli("closure").returncode == 1
    scenarios = [s["id"] for s in RUNBOOK["scenarios"]]
    people = [{"name": "Sam", "roles": ["coordinator"], "familiarity": "unfamiliar"},
              {"name": "Ria", "roles": ["verifier", "scribe"], "familiarity": "unfamiliar"}]
    rows = [{"scenario": s, "result": "recovered", "verifiedRepositoryNetwork": True} for s in scenarios]
    synthetic = [("DRX-9001.json", {"id": "DRX-9001", "status": "completed", "kind": "tabletop", "participants": people,
                                    "scenarios": rows, "findings": []}),
                 ("DRX-9002.json", {"id": "DRX-9002", "status": "completed", "kind": "live", "participants": people,
                                    "offlineRunbookOnly": True, "scenarios": rows,
                                    "findings": [{"id": "F1", "status": "open",
                                                  "correctiveOwner": {"kind": "issue", "ref": "#1"}}]})]
    checks = {c["id"]: c for c in tool.closure_checks(RUNBOOK, synthetic)}
    assert checks["tabletop-drills-recorded"]["ok"] and checks["live-drills-recorded"]["ok"]
    assert checks["unfamiliar-team-restored"]["ok"]
    assert not checks["findings-owned"]["ok"]
    automation_only = [(n, dict(e, participants=[dict(p, familiarity="automation") for p in people])) for n, e in synthetic]
    checks = {c["id"]: c for c in tool.closure_checks(RUNBOOK, automation_only)}
    assert not checks["live-drills-recorded"]["ok"] and not checks["unfamiliar-team-restored"]["ok"]


# --- Documentation and wiring ----------------------------------------------------------------------

def test_documentation_covers_the_contract():
    doc = DOC.read_text(encoding="utf-8")
    for command in ("validate", "render", "gate", "closure", "roster check", "roster keys", "roster profile",
                    "key rotate", "key cosign", "key apply", "kit build", "kit verify", "drill run", "attest"):
        assert "forkmesh_disaster_recovery.py %s" % command in doc, command
    for code in ("0", "1", "2", "3", "4"):
        assert re.search(r"^\| `%s` \|" % code, doc, re.M), code
    for vocabulary in (list(tool.DRILL_FAULTS) + [f for faults in tool.DRILL_FAULTS.values() for f in faults]
                       + [c["id"] for c in tool.closure_checks(RUNBOOK, [])]
                       + ["signer_not_recovery_key", "below_threshold", "bad_signature", "revoke_unknown",
                          "revoke_recovery_key", "successor_known", "successor_role", "statement_digest",
                          "community_mismatch", "already_applied", "statement_invalid",
                          "verified", "stale", "failed"] + list(tool.KEY_ROLES)):
        assert "`%s`" % vocabulary in doc, vocabulary
    for schema in (tool.RUNBOOK_SCHEMA, tool.KIT_SCHEMA, tool.KIT_SIGNED_SCHEMA, tool.ROSTER_SCHEMA,
                   tool.ROTATION_SCHEMA, tool.ROTATION_SIGNED_SCHEMA, tool.REPORT_SCHEMA,
                   tool.EXERCISE_SCHEMA, tool.TELEMETRY_SCHEMA):
        assert schema in doc, schema
    index = (ROOT / "www" / "docs" / "README.md").read_text(encoding="utf-8")
    assert "(disaster-recovery/README.md)" in index and "(disaster-recovery/runbook.md)" in index


def test_quality_gate_and_platform_matrix_run_this_suite():
    config = json.loads((ROOT / "www" / "docs" / "engineering" / "quality-gates.json").read_text())
    gate = next(g for g in config["gates"] if g["id"] == "disaster-recovery-conformance")
    assert "app/tests/test_disaster_recovery.py" in gate["command"]
    assert "disaster-recovery-conformance" in config["profiles"]["core"]
    assert "disaster-recovery-conformance" in config["profiles"]["release"]
    workflow = (ROOT / ".github" / "workflows" / "disaster-recovery.yml").read_text()
    assert "app/tests/test_disaster_recovery.py" in workflow
    for platform_name in ("ubuntu-latest", "macos-latest", "windows-latest"):
        assert platform_name in workflow
    assert "forkmesh_disaster_recovery.py gate" in workflow and "drill run all" in workflow
