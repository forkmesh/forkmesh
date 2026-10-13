"""Protocol governance ledger: charter rules, disclosures, timelines, quorum, dissent, and the public register."""

import copy
import importlib.util
import json
import subprocess
import sys
from datetime import date
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "protocol_governance.py"


def _module():
    spec = importlib.util.spec_from_file_location("forkmesh_protocol_governance", TOOL)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


pg = _module()


@pytest.fixture()
def ledger():
    return copy.deepcopy(pg.load_ledger())


def _errors(ledger):
    try:
        pg.validate(ledger)
    except pg.GovernanceError as exc:
        return str(exc)
    return ""


def _record(ledger, record_id):
    return next(record for record in ledger["records"] if record["id"] == record_id)


def _person(pid, constituency, funder="none"):
    return {"id": pid, "name": pid, "kind": "person", "constituency": constituency,
            "employerOrFunder": funder, "source": f"https://example.org/{pid}"}


def _seat_independents(ledger):
    """Seat a full, disclosed protocol council and security team on a copy of the ledger."""
    document = ledger["participants"]
    people = [
        ("maint-a", "forkmesh-maintainer", "forkmesh.com operator", "maintainer-1"),
        ("maint-b", "forkmesh-maintainer", "forkmesh.com operator", "maintainer-2"),
        ("impl-a", "independent-implementer", "none", "implementer-1"),
        ("impl-b", "independent-implementer", "Example Co-op", "implementer-2"),
        ("oper-a", "community-operator", "none", "operator-1"),
        ("oper-b", "community-operator", "none", "operator-2"),
        ("sec-impl", "independent-implementer", "none", "security-independent"),
        ("sec-maint", "forkmesh-maintainer", "forkmesh.com operator", "security-maintainer"),
    ]
    for pid, constituency, funder, seat in people:
        document["participants"].append(_person(pid, constituency, funder))
        document["seatHolders"].append({"seat": seat, "participant": pid, "since": "2026-09-01", "termEnds": "2027-09-01"})
        document["disclosures"].append({
            "id": f"DISC-{pid}", "participant": pid, "filed": "2026-09-10", "conflicts": [],
            "funding": [{"kind": "none", "amountPublished": False}],
        })
    return [pid for pid, *_ in people]


def _decide_charter(ledger, votes, dissent=()):
    record = _record(ledger, "FMP-0001")
    record["timeline"].update({"reviewCloses": "2026-10-14", "lastCallCloses": "2026-10-21", "decided": "2026-10-25"})
    record["stage"] = "decided"
    record["outcome"] = "accepted"
    record["votes"] = [{"participant": pid, "seat": seat, "vote": vote, "rationale": "r"} for pid, seat, vote in votes]
    record["disclosures"] = sorted({"DISC-0001", *(f"DISC-{pid}" for pid, _, _ in votes)})
    record["dissent"] = list(dissent)
    return record


FULL_COUNCIL = [
    ("maint-a", "maintainer-1", "yes"),
    ("impl-a", "implementer-1", "yes"),
    ("impl-b", "implementer-2", "yes"),
    ("oper-a", "operator-1", "yes"),
    ("oper-b", "operator-2", "no"),
]


def test_committed_ledger_validates_and_register_is_current():
    for command in ("validate", "gate"):
        result = subprocess.run([sys.executable, str(TOOL), command], cwd=ROOT, capture_output=True, text=True, check=False)
        assert result.returncode == 0, result.stderr


def test_committed_ledger_reports_every_open_closure_criterion():
    status = pg.evaluate(pg.load_ledger(), date(2026, 9, 14))
    criteria = [item["criterion"] for item in status["closureBlockers"]]
    assert criteria == ["proposal-completed", "security-decision-completed", "independent-roles", "disclosures-current"]
    assert status["overdue"] == []
    closure = subprocess.run([sys.executable, str(TOOL), "closure", "--as-of", "2026-09-14"], cwd=ROOT, capture_output=True, text=True, check=False)
    assert closure.returncode == 1


def test_pre_charter_records_never_count_toward_closure(ledger):
    for record_id in ("FMP-0000", "FMS-0001"):
        record = _record(ledger, record_id)
        assert record["adoptedUnder"] is None and record["dissentSolicited"] is False
    # No independent seat holder may be a ForkMesh group or a forkmesh.com-paid person.
    assert all(item["constituency"] == "forkmesh-maintainer" for item in ledger["participants"]["participants"])


def test_review_window_minimum_and_breaking_minimum(ledger):
    record = _record(ledger, "FMP-0001")
    record["timeline"]["reviewCloses"] = "2026-09-20"
    assert "review window is shorter than 14 days" in _errors(ledger)
    record["timeline"]["reviewCloses"] = "2026-10-01"
    assert _errors(ledger) == ""
    record["breaking"] = True
    assert "review window is shorter than 30 days" in _errors(ledger)


def test_participant_without_linked_disclosure_is_rejected(ledger):
    _record(ledger, "FMP-0001")["disclosures"] = []
    assert "forkmesh-maintainers takes part without a linked disclosure" in _errors(ledger)


def test_maintainers_alone_cannot_decide(ledger):
    _seat_independents(ledger)
    _decide_charter(ledger, [("maint-a", "maintainer-1", "yes"), ("maint-b", "maintainer-2", "yes"),
                             ("impl-a", "implementer-1", "yes"), ("impl-b", "implementer-2", "abstain")])
    errors = _errors(ledger)
    assert "no community-operator voted" in errors
    assert "forkmesh.com operator holds 2 of 4 votes" not in errors  # exactly half is allowed
    _decide_charter(ledger, [("maint-a", "maintainer-1", "yes"), ("maint-b", "maintainer-2", "yes"),
                             ("oper-a", "operator-1", "yes"), ("impl-a", "implementer-1", "yes"),
                             ("sec-maint", "maintainer-2", "yes")])
    assert "does not hold seat maintainer-2" in _errors(ledger)


def test_no_vote_requires_recorded_dissent(ledger):
    _seat_independents(ledger)
    _decide_charter(ledger, FULL_COUNCIL)
    assert "oper-b voted no without recorded dissent" in _errors(ledger)
    _decide_charter(ledger, FULL_COUNCIL, [{"participant": "oper-b", "recorded": "2026-10-22", "statement": "Seat terms are too long."}])
    assert _errors(ledger) == ""


def test_completed_charter_decisions_clear_closure_blockers(ledger):
    _seat_independents(ledger)
    _decide_charter(ledger, FULL_COUNCIL, [{"participant": "oper-b", "recorded": "2026-10-22", "statement": "Seat terms are too long."}])
    ledger["records"].append({
        "schema": pg.RECORD_SCHEMA, "_file": "FMS-0002", "id": "FMS-0002", "process": "security", "adoptedUnder": "1.0.0-rc.1",
        "title": "t", "summary": "s", "authors": ["sec-maint"], "disclosures": ["DISC-sec-maint", "DISC-sec-impl"],
        "threats": ["TM-014"], "links": [], "stage": "decided", "outcome": "mitigated", "decidingBody": "security-response-team",
        "timeline": {"opened": "2026-10-01", "disclosed": "2026-10-05", "embargoEnds": "2026-10-05", "decided": "2026-10-05", "ratified": "2026-10-20"},
        "votes": [{"participant": "sec-maint", "seat": "security-maintainer", "vote": "yes"},
                  {"participant": "sec-impl", "seat": "security-independent", "vote": "yes"}],
        "dissent": [], "dissentSolicited": True, "affects": ["security"],
    })
    for disclosure in ledger["participants"]["disclosures"]:
        for entry in disclosure["funding"]:
            if entry["kind"] != "none":
                entry.update(amountPublished=True, amount="USD 0")
    assert _errors(ledger) == ""
    status = pg.evaluate(ledger, date(2026, 10, 30))
    assert status["closureBlockers"] == []
    # Disclosures expire after 180 days and reopen the criterion.
    later = pg.evaluate(ledger, date(2027, 6, 1))
    assert "disclosures-current" in [item["criterion"] for item in later["closureBlockers"]]


def test_security_embargo_limit_and_ratification_deadline(ledger):
    ledger["records"].append({
        "schema": pg.RECORD_SCHEMA, "_file": "FMS-0003", "id": "FMS-0003", "process": "security", "adoptedUnder": "1.0.0-rc.1",
        "title": "t", "summary": "s", "authors": ["forkmesh-maintainers"], "disclosures": ["DISC-0001"], "links": [],
        "stage": "embargoed", "outcome": None, "decidingBody": "security-response-team",
        "timeline": {"opened": "2026-09-01", "disclosed": "2026-09-05", "embargoEnds": "2026-12-15", "decided": None, "ratified": None},
        "votes": [], "dissent": [], "dissentSolicited": True,
    })
    assert "embargo exceeds 90 days" in _errors(ledger)
    _record(ledger, "FMS-0003")["timeline"]["embargoEnds"] = "2026-09-05"
    assert _errors(ledger) == ""
    overdue = pg.evaluate(ledger, date(2026, 10, 6))["overdue"]
    assert {"record": "FMS-0003", "due": "2026-10-05", "awaiting": "ratification"} in overdue


def test_appeal_excludes_original_deciders_and_late_filing(ledger):
    _seat_independents(ledger)
    ledger["participants"]["participants"].append(_person("member-a", "community-member"))
    ledger["participants"]["seatHolders"] += [
        {"seat": "appeal-implementer", "participant": "impl-a", "since": "2026-09-01", "termEnds": "2027-09-01"},
    ]
    ledger["records"].append({
        "schema": pg.RECORD_SCHEMA, "_file": "FMA-0001", "id": "FMA-0001", "process": "appeal", "adoptedUnder": "1.0.0-rc.1",
        "appeals": "FMP-0000", "title": "t", "summary": "s", "authors": ["oper-a"], "disclosures": ["DISC-oper-a", "DISC-impl-a"],
        "links": [], "stage": "filed", "outcome": None, "decidingBody": "appeals-panel",
        "timeline": {"filed": "2026-11-01", "decided": None},
        "votes": [{"participant": "impl-a", "seat": "appeal-implementer", "vote": "abstain"}], "dissent": [], "dissentSolicited": True,
    })
    assert "filed more than 30 days after the decision" in _errors(ledger)
    _record(ledger, "FMP-0000")["votes"] = [{"participant": "impl-a", "seat": "implementer-1", "vote": "yes"}]
    assert "impl-a took part in the appealed decision" in _errors(ledger)


def test_independence_cannot_be_claimed_by_forkmesh_paid_participants(ledger):
    ledger["participants"]["participants"].append(_person("fake", "independent-implementer", "ForkMesh.com operator"))
    assert "paid by the ForkMesh operator, so not independent" in _errors(ledger)


def test_protected_rights_track_business_model_commitments(ledger):
    ledger["businessModel"]["permanentCommitments"]["tokenOrBlockchainRequired"] = True
    assert "commitment tokenOrBlockchainRequired no longer holds" in _errors(ledger)
    fresh = copy.deepcopy(pg.load_ledger())
    fresh["charter"]["protectedRights"] = [r for r in fresh["charter"]["protectedRights"] if r["id"] != "token-free-governance"]
    assert "token-free-governance must stay a protected right" in _errors(fresh)


def test_narrowing_a_protected_right_needs_breaking_supermajority_and_independent_yes(ledger):
    _seat_independents(ledger)
    record = _decide_charter(ledger, FULL_COUNCIL, [{"participant": "oper-b", "recorded": "2026-10-22", "statement": "No."}])
    record["narrowsProtectedRights"] = True
    errors = _errors(ledger)
    assert "narrowing a protected right is a breaking change" in errors
    assert "needs a yes from independent seat operator-2" in errors


def test_register_is_searchable_and_links_disclosures_to_records(tmp_path):
    document = json.loads((ROOT / pg.REGISTER_JSON_PATH).read_text(encoding="utf-8"))
    assert document["schema"] == pg.REGISTER_SCHEMA
    disclosure = next(item for item in document["disclosures"] if item["id"] == "DISC-0001")
    assert disclosure["linkedRecords"] == ["FMP-0000", "FMP-0001", "FMS-0001"]
    assert {record["id"]: record["countsForCharter"] for record in document["records"]} == {
        "FMP-0000": False, "FMP-0001": True, "FMS-0001": False}
    markdown = (ROOT / pg.REGISTER_MARKDOWN_PATH).read_text(encoding="utf-8")
    assert "| Protocol council | implementer-1 | independent-implementer | — | vacant |" in markdown
    assert "not published" in markdown
    # A stale or hand-edited output is detected without being rewritten.
    ledger = pg.load_ledger()
    for relative in pg.generated_outputs(ledger):
        (tmp_path / relative).parent.mkdir(parents=True, exist_ok=True)
        (tmp_path / relative).write_text("edited\n", encoding="utf-8")
    assert sorted(pg.render(ledger, check=True, root=tmp_path)) == sorted(str(path) for path in pg.generated_outputs(ledger))
    assert (tmp_path / pg.REGISTER_JSON_PATH).read_text(encoding="utf-8") == "edited\n"


def test_gate_is_wired_into_quality_profiles_and_ci():
    config = json.loads((ROOT / "www/docs/engineering/quality-gates.json").read_text(encoding="utf-8"))
    gate = next(item for item in config["gates"] if item["id"] == "protocol-governance")
    assert gate["command"] == ["{python}", "tools/protocol_governance.py", "gate"]
    assert "protocol-governance" in config["profiles"]["core"]
    assert "protocol-governance" in config["profiles"]["release"]
    workflow = (ROOT / ".github/workflows/collaboration-conformance.yml").read_text(encoding="utf-8")
    assert "python tools/protocol_governance.py gate" in workflow
    assert "app/tests/test_protocol_governance.py" in workflow
