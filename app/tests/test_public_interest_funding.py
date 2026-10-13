"""Public-interest funding ledger: eligibility, conflicts, selection, rails, independence, disbursements, and reports."""

import copy
import importlib.util
import json
import subprocess
import sys
from datetime import date
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "public_interest_funding.py"


def _module():
    spec = importlib.util.spec_from_file_location("forkmesh_public_interest_funding", TOOL)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


pf = _module()


@pytest.fixture()
def ledger():
    return copy.deepcopy(pf.load_ledger())


def _errors(ledger):
    try:
        pf.validate(ledger)
    except pf.FundingError as exc:
        return str(exc)
    return ""


def _pilot(ledger):
    return next(item for item in ledger["rounds"] if item["id"] == "PIF-0001")


def _seat_reviewers(ledger):
    document = ledger["governanceParticipants"]
    reviewers = [
        ("rev-impl", "independent-implementer", "none"),
        ("rev-oper", "community-operator", "Example Co-op"),
        ("rev-maint", "forkmesh-maintainer", "forkmesh.com operator"),
        ("rev-member", "community-member", "none"),
    ]
    for pid, constituency, funder in reviewers:
        document["participants"].append({"id": pid, "name": pid, "kind": "person", "constituency": constituency,
                                         "employerOrFunder": funder, "source": f"https://example.org/{pid}"})
        document["disclosures"].append({"id": f"DISC-{pid}", "participant": pid, "filed": "2026-09-20", "conflicts": [],
                                        "funding": [{"kind": "none", "amountPublished": False}]})
    return [{"participant": pid, "disclosure": f"DISC-{pid}"} for pid, _, _ in reviewers]


APPLICANTS = [
    ("APP-1", "Mirror Collective Berlin", "none", "volunteer-mirrors", "targeted-grant"),
    ("APP-2", "Andes Relay", "none", "volunteer-mirrors", "matching"),
    ("APP-3", "Screen Reader Lab", "Example Co-op", "accessibility", "targeted-grant"),
    ("APP-4", "Docs in Swahili", "none", "documentation", "targeted-grant"),
]
RAILS = ["bank-transfer", "solana", "liberapay", "open-collective"]


def _scores(value):
    return {"public-benefit": value, "feasibility": value, "accessibility-inclusion": value, "sustainability": value, "decentralization": value}


def _run_pilot(ledger):
    """Walk the committed draft pilot through a complete, valid round on a copy of the ledger."""
    round_ = _pilot(ledger)
    round_.update({
        "stage": "reported",
        "timeline": {"announced": "2026-09-30", "applicationsOpen": "2026-10-01", "applicationsClose": "2026-10-29",
                     "selectionsPublished": "2026-11-15", "finalReportDue": "2027-03-31", "reported": "2027-03-15"},
        "budget": {"currency": "USD", "committed": 11000, "administrativeCost": 1000},
        "funders": [
            {"id": "F-1", "name": "Example Foundation", "kind": "grant", "amount": 6000, "conflicts": [], "restrictions": {}},
            {"id": "F-2", "name": "ForkMesh maintainers", "participant": "forkmesh-maintainers", "kind": "membership", "amount": 5000,
             "conflicts": ["Operates forkmesh.com"], "restrictions": {"areas": ["accessibility", "documentation"]}},
        ],
        "reviewers": _seat_reviewers(ledger),
        "note": "Test pilot.",
    })
    round_["applications"] = [
        {"id": app_id, "applicant": {"name": name, "source": f"https://example.org/{app_id}", "employerOrFunder": funder},
         "area": area, "offer": offer, "requested": 3000, "received": "2026-10-10", "eligible": True, "summary": "s"}
        for app_id, name, funder, area, offer in APPLICANTS
    ] + [{"id": "APP-5", "applicant": {"name": "Hosted Only", "source": "https://example.org/APP-5", "employerOrFunder": "none"},
          "area": "documentation", "offer": "targeted-grant", "requested": 2000, "received": "2026-10-12", "eligible": False,
          "eligibilityNote": "The guide requires a paid hosted plan to follow.", "summary": "s"}]
    round_["conflicts"] = [{"application": "APP-3", "reviewer": "rev-oper", "reason": "Shares Example Co-op as funder.", "action": "recused"}]
    round_["reviews"] = [
        {"application": app_id, "reviewer": reviewer, "scores": _scores(4), "recorded": "2026-11-05"}
        for app_id, *_ in APPLICANTS
        for reviewer in (["rev-impl", "rev-maint", "rev-member"] if app_id == "APP-3" else ["rev-impl", "rev-oper", "rev-maint"])
    ]
    round_["awards"] = []
    round_["disbursements"] = []
    for index, (app_id, *_) in enumerate(APPLICANTS):
        award_id = f"AW-{index + 1}"
        award = {"id": award_id, "application": app_id, "amount": 2500, "rail": RAILS[index], "grants": [],
                 "obligations": ["milestone-report", "outcome-report"], "milestones": [
                     {"id": "M1", "deliverable": "first", "due": "2026-12-15", "amount": 1250, "status": "accepted",
                      "report": f"https://example.org/{award_id}/m1", "reported": "2026-12-10"},
                     {"id": "M2", "deliverable": "second", "due": "2027-02-15", "amount": 1250, "status": "accepted",
                      "report": f"https://example.org/{award_id}/m2", "reported": "2027-02-10"}]}
        if app_id == "APP-2":
            award["matching"] = {"contributions": 1500, "ratio": 2, "receipt": "https://example.org/receipts/andes"}
        round_["awards"].append(award)
        for milestone, paid in (("M1", "2026-12-12"), ("M2", "2027-02-12")):
            round_["disbursements"].append({"id": f"D-{award_id}-{milestone}", "award": award_id, "milestone": milestone, "amount": 1250,
                                            "rail": RAILS[index], "date": paid, "status": "sent", "receipt": f"https://example.org/pay/{award_id}/{milestone}"})
    round_["communityReview"] = {"opened": "2026-11-15", "closes": "2026-12-01", "findings": [
        {"id": "CR-1", "risk": "concentration", "raisedBy": "rev-oper", "statement": "One funder supplies 55% of the pool.",
         "status": "accepted-risk", "correction": "The next round is announced only after a second funder brings the largest share under 50%.", "recorded": "2026-11-20"},
        {"id": "CR-2", "risk": "accessibility", "raisedBy": "Community reviewer by email", "statement": "The application form was a PDF only.",
         "status": "corrected", "correction": "Plain-text and HTML forms added.", "recorded": "2026-11-21"},
        {"id": "CR-3", "risk": "conflict", "raisedBy": "rev-impl", "statement": "rev-oper and APP-3 share a funder.",
         "status": "corrected", "correction": "rev-oper recused and a third reviewer scored APP-3.", "recorded": "2026-11-22"},
    ]}
    round_["outcomeReport"] = {
        "published": "2027-03-15", "href": "https://example.org/pif-0001-report",
        "fundsRaised": 11000, "fundsAwarded": 10000, "fundsDisbursed": 10000, "administrativeCost": 1000,
        "applications": 5, "recipients": 4, "milestonesAccepted": 8, "milestonesTotal": 8, "operatorsFunded": 2, "operatorsRetained": 2,
        "accessibilityImpact": "One screen-reader audit of the web repository view.",
        "lessons": ["Only one volunteer-mirror applicant came from outside Europe.", "A PDF-only form excluded applicants until corrected."],
    }
    return round_


def test_committed_ledger_validates_and_register_is_current():
    for command in ("validate", "gate"):
        result = subprocess.run([sys.executable, str(TOOL), command], cwd=ROOT, capture_output=True, text=True, check=False)
        assert result.returncode == 0, result.stderr


def test_committed_pilot_is_a_draft_and_closure_stays_open():
    ledger = pf.load_ledger()
    pilot = _pilot(ledger)
    assert pilot["pilot"] is True and pilot["stage"] == "draft"
    assert pilot["funders"] == [] and pilot["applications"] == [] and pilot["awards"] == []
    status = pf.evaluate(ledger, date(2026, 9, 14))
    criteria = [item["criterion"] for item in status["closureBlockers"]]
    assert criteria == ["pilot-round-reported", "recipients-paid-on-portable-rails", "community-review-corrected"]
    assert status["overdue"] == [] and status["concentrationWarnings"] == []
    assert {offer["status"] for offer in ledger["program"]["offers"]} == {"planned"}
    closure = subprocess.run([sys.executable, str(TOOL), "closure", "--as-of", "2026-09-14"], cwd=ROOT, capture_output=True, text=True, check=False)
    assert closure.returncode == 1


def test_rails_and_channels_never_require_forkmesh(ledger):
    assert _errors(ledger) == ""
    ledger["program"]["paymentRails"].append({"id": "membership-credit", "name": "ForkMesh membership credit", "kind": "conventional",
                                              "recurringSupported": True, "forkmeshAccountRequired": True, "forkmeshPlanRequired": True,
                                              "recipientControlled": False})
    errors = _errors(ledger)
    assert "rail membership-credit: a payment rail must not require a ForkMesh account or plan" in errors
    assert "rail membership-credit: the recipient must control the receiving account" in errors
    fresh = copy.deepcopy(pf.load_ledger())
    _pilot(fresh)["paymentRails"] = ["solana"]
    assert "must offer a conventional payment rail" in _errors(fresh)
    fresh = copy.deepcopy(pf.load_ledger())
    for channel in _pilot(fresh)["applicationChannels"]:
        channel["forkmeshAccountRequired"] = True
    assert "needs an application channel that requires no ForkMesh account" in _errors(fresh)
    fresh = copy.deepcopy(pf.load_ledger())
    _run_pilot(fresh)["awards"][0]["rail"] = "membership-credit"
    assert "rail must be one the round offers, chosen by the recipient" in _errors(fresh)


def test_awards_never_grant_access_votes_or_network_behavior(ledger):
    round_ = _run_pilot(ledger)
    assert _errors(ledger) == ""
    round_["awards"][0]["grants"] = ["protocol-votes"]
    round_["awards"][1]["obligations"].append("use-hosted-mirror")
    errors = _errors(ledger)
    assert "award AW-1: grants must be empty; awards never grant protocol-votes (protocol-votes is explicitly prohibited)" in errors
    assert "award AW-2: obligations use-hosted-mirror are not allowed" in errors
    fresh = copy.deepcopy(pf.load_ledger())
    fresh["businessModel"]["permanentCommitments"]["tokenOrBlockchainRequired"] = True
    assert "business-model commitment tokenOrBlockchainRequired no longer holds" in _errors(fresh)
    fresh = copy.deepcopy(pf.load_ledger())
    fresh["program"]["independence"]["awardsNeverGrant"].remove("required-network-behavior")
    assert "awardsNeverGrant must include required-network-behavior" in _errors(fresh)
    fresh = copy.deepcopy(pf.load_ledger())
    fresh["governanceCharter"]["protectedRights"] = []
    assert "governance protected right token-free-governance does not exist" in _errors(fresh)


def test_opening_a_round_requires_published_funding_and_independent_reviewers(ledger):
    round_ = _pilot(ledger)
    round_["stage"] = "open"
    round_["timeline"].update({"announced": "2026-09-30", "applicationsOpen": "2026-10-01", "applicationsClose": "2026-10-15"})
    errors = _errors(ledger)
    for expected in ("application window is shorter than 21 days", "an open round publishes its committed pool",
                     "an open round publishes its administrative cost", "an open round names its funders",
                     "an open round seats at least 3 reviewers", "an open round seats an independent reviewer"):
        assert expected in errors
    round_["budget"].update({"committed": 10000, "administrativeCost": 2000})
    round_["funders"] = [{"id": "F-1", "name": "Example Foundation", "kind": "grant", "amount": 4000, "conflicts": [], "restrictions": {"recipients": ["x"]}}]
    errors = _errors(ledger)
    assert "administrative cost exceeds 10% of the pool" in errors
    assert "funders cover less than the committed pool" in errors
    assert "a funder may restrict a gift only to areas" in errors


def test_conflicted_reviewer_must_recuse(ledger):
    round_ = _run_pilot(ledger)
    round_["conflicts"] = []
    assert "rev-oper must record a recusal from APP-3 (reviewer shares an employer or funder with the applicant)" in _errors(ledger)
    fresh = copy.deepcopy(pf.load_ledger())
    round_ = _run_pilot(fresh)
    round_["reviews"].append({"application": "APP-3", "reviewer": "rev-oper", "scores": _scores(5), "recorded": "2026-11-05"})
    assert "review APP-3/rev-oper: reviewer scored an application they recused from" in _errors(fresh)
    fresh = copy.deepcopy(pf.load_ledger())
    round_ = _run_pilot(fresh)
    round_["funders"][0]["participant"] = "rev-impl"
    assert "reviewer rev-impl: a funder never reviews" in _errors(fresh)


def test_selection_requires_quorum_score_and_eligibility(ledger):
    round_ = _run_pilot(ledger)
    round_["reviews"] = [item for item in round_["reviews"] if not (item["application"] == "APP-4" and item["reviewer"] == "rev-impl")]
    assert "APP-4 was scored by 2 reviewers (1 independent); selection needs 3 (1 independent)" in _errors(ledger)
    fresh = copy.deepcopy(pf.load_ledger())
    round_ = _run_pilot(fresh)
    for review in round_["reviews"]:
        if review["application"] == "APP-1":
            review["scores"] = _scores(2)
    assert pf.application_score(round_, fresh["program"], "APP-1") == 40.0
    assert "award AW-1: score 40.0 is below the minimum 60" in _errors(fresh)
    fresh = copy.deepcopy(pf.load_ledger())
    round_ = _run_pilot(fresh)
    round_["applications"][0].update(eligible=False, eligibilityNote="Mirror is private.")
    round_["applications"][1]["applicant"]["employerOrFunder"] = "forkmesh.com operator"
    errors = _errors(fresh)
    assert "award AW-1: awarded an ineligible application" in errors
    assert "application APP-2: applicants paid by the forkmesh.com operator are not eligible" in errors


def test_recipient_concentration_cap_and_funder_warning(ledger):
    round_ = _run_pilot(ledger)
    assert pf.concentration_warnings(round_, ledger["program"]) == [
        {"round": "PIF-0001", "subject": "funder", "share": 0.5455, "limit": 0.5}]
    round_["applications"][3]["applicant"]["name"] = "Mirror Collective Berlin"
    errors = _errors(ledger)
    assert "one recipient receives 50% of awards; the limit is 25%" in errors
    assert "outcome report recipients is 4 but the ledger says 3" in errors


def test_returned_payments_do_not_count_as_disbursed(ledger):
    round_ = _run_pilot(ledger)
    returned = round_["disbursements"][0]
    returned.update(status="returned", note="The bank rejected the account number.")
    assert "outcome report fundsDisbursed is 10000 but the ledger says 8750" in _errors(ledger)
    round_["disbursements"].append(dict(returned, id="D-AW-1-M1-retry", status="sent", date="2026-12-20", note=None))
    assert _errors(ledger) == ""
    round_["disbursements"].append(dict(returned, id="D-AW-1-M1-double", status="sent"))
    assert "pays more than milestone M1 is worth" in _errors(ledger)
    fresh = copy.deepcopy(pf.load_ledger())
    round_ = _run_pilot(fresh)
    round_["awards"][0]["milestones"][1].update(status="pending", report=None, reported=None)
    assert "pays milestone M2 before it is accepted" in _errors(fresh)


def test_cancelled_round_publishes_lessons(ledger):
    round_ = _pilot(ledger)
    round_.update(stage="cancelled", cancelledFrom="draft")
    assert "a cancelled round publishes an outcome report" in _errors(ledger)
    round_["outcomeReport"] = {
        "published": "2026-12-01", "href": "https://example.org/pif-0001-cancelled", "fundsRaised": 0, "fundsAwarded": 0,
        "fundsDisbursed": 0, "administrativeCost": None, "applications": 0, "recipients": 0, "milestonesAccepted": 0,
        "milestonesTotal": 0, "operatorsFunded": 0, "operatorsRetained": 0, "accessibilityImpact": "None: the round did not open.", "lessons": [],
    }
    assert "outcome report lists lessons, including what did not work" in _errors(ledger)
    round_["outcomeReport"]["lessons"] = ["No funder committed a pool before the announcement deadline."]
    assert _errors(ledger) == ""
    assert "pilot-round-reported" in [item["criterion"] for item in pf.evaluate(ledger, date(2026, 12, 2))["closureBlockers"]]


def test_completed_pilot_clears_closure_and_reports_measures(ledger):
    round_ = _run_pilot(ledger)
    assert _errors(ledger) == ""
    status = pf.evaluate(ledger, date(2027, 3, 20))
    assert status["closureBlockers"] == []
    measures = status["rounds"][0]["measures"]
    assert measures == {
        "fundsCommitted": 11000, "fundsRaised": 11000, "fundsAwarded": 10000, "fundsDisbursed": 10000, "administrativeCost": 1000,
        "applications": 5, "recipients": 4, "milestonesAccepted": 8, "milestonesTotal": 8, "milestoneCompletion": 1.0,
        "operatorsFunded": 2, "operatorsRetained": 2, "accessibilityAwards": 1, "largestRecipientShare": 0.25,
        "largestAreaShare": 0.5, "largestFunderShare": 0.5455,
    }
    # A finding raised only by a ForkMesh maintainer is not independent community review.
    round_["communityReview"]["findings"][0]["raisedBy"] = "rev-maint"
    assert [item["criterion"] for item in pf.evaluate(ledger, date(2027, 3, 20))["closureBlockers"]] == ["community-review-corrected"]
    round_["communityReview"]["findings"][0]["raisedBy"] = "rev-oper"
    round_["communityReview"]["findings"][1]["status"] = "open"
    assert [item["criterion"] for item in pf.evaluate(ledger, date(2027, 3, 20))["closureBlockers"]] == ["community-review-corrected"]


def test_overdue_milestone_reports(ledger):
    round_ = _run_pilot(ledger)
    round_.update(stage="disbursing", outcomeReport=None)
    round_["timeline"]["reported"] = None
    round_["awards"][3]["milestones"][1].update(status="pending", report=None, reported=None)
    round_["disbursements"] = [item for item in round_["disbursements"] if item["id"] != "D-AW-4-M2"]
    assert _errors(ledger) == ""
    assert pf.evaluate(ledger, date(2027, 3, 17))["overdue"] == []
    status = pf.evaluate(ledger, date(2027, 3, 18))
    assert status["overdue"] == [{"round": "PIF-0001", "award": "AW-4", "milestone": "M2", "due": "2027-02-15", "awaiting": "milestone report"}]
    assert "reports-on-time" in [item["criterion"] for item in status["closureBlockers"]]
    later = pf.evaluate(ledger, date(2027, 4, 1))
    assert {"round": "PIF-0001", "due": "2027-03-31", "awaiting": "outcome report"} in later["overdue"]


def test_register_detects_hand_edits(tmp_path):
    document = json.loads((ROOT / pf.REGISTER_JSON_PATH).read_text(encoding="utf-8"))
    assert document["schema"] == pf.REGISTER_SCHEMA
    assert [item["id"] for item in document["rounds"]] == ["PIF-0001"]
    markdown = (ROOT / pf.REGISTER_MARKDOWN_PATH).read_text(encoding="utf-8")
    assert "| Liberapay | conventional | yes | no |" in markdown
    assert "None committed." in markdown and "No applications." in markdown
    ledger = pf.load_ledger()
    for relative in pf.generated_outputs(ledger):
        (tmp_path / relative).parent.mkdir(parents=True, exist_ok=True)
        (tmp_path / relative).write_text("edited\n", encoding="utf-8")
    assert sorted(pf.render(ledger, check=True, root=tmp_path)) == sorted(str(path) for path in pf.generated_outputs(ledger))
    assert (tmp_path / pf.REGISTER_JSON_PATH).read_text(encoding="utf-8") == "edited\n"
    # A completed round renders every public section.
    completed = copy.deepcopy(ledger)
    _run_pilot(completed)
    text = pf.render_markdown(pf.register_document(completed))
    for expected in ("rev-oper recused from APP-3", "| [D-AW-2-M1](https://example.org/pay/AW-2/M1) | AW-2 | M1 | USD 1,250 | solana |", "Concentration warning: one funder holds 55% (limit 50%).",
                     "4 recipients, 8 of 8 milestones accepted", "no: The guide requires a paid hosted plan to follow."):
        assert expected in text


def test_gate_is_wired_into_quality_profiles_and_ci():
    config = json.loads((ROOT / "www/docs/engineering/quality-gates.json").read_text(encoding="utf-8"))
    gate = next(item for item in config["gates"] if item["id"] == "public-interest-funding")
    assert gate["command"] == ["{python}", "tools/public_interest_funding.py", "gate"]
    assert "public-interest-funding" in config["profiles"]["core"]
    assert "public-interest-funding" in config["profiles"]["release"]
    incentives = next(item for item in config["gates"] if item["id"] == "incentive-independence")
    assert "app/tests/test_public_interest_funding.py" in incentives["command"]
    workflow = (ROOT / ".github/workflows/collaboration-conformance.yml").read_text(encoding="utf-8")
    assert "python tools/public_interest_funding.py gate" in workflow
    assert "app/tests/test_public_interest_funding.py" in workflow


def test_program_guide_documents_operations():
    guide = (ROOT / "www/docs/funding/README.md").read_text(encoding="utf-8")
    for marker in (
        "## Eligibility", "## Selection", "## Conflicts", "## Payment rails", "## Independence",
        "## Disclosure and reporting", "## Community review", "## Failure, rollback, and recovery", "## Measures",
        "## Closure criteria for issue #515", "## Linked automated evidence",
        "python3 tools/public_interest_funding.py gate", "app/tests/test_public_interest_funding.py",
        "(funding-register.md)",
    ):
        assert marker in guide, marker
    index = (ROOT / "www/docs/README.md").read_text(encoding="utf-8")
    assert "(funding/README.md)" in index
    incentives = (ROOT / "www/docs/incentive-independence.md").read_text(encoding="utf-8")
    assert "[public-interest funding](funding/README.md)" in incentives
