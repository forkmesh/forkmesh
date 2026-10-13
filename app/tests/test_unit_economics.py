"""Commercial contract and unit-economics launch-gate tests."""

from copy import deepcopy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "unit_economics.py"
SPEC = importlib.util.spec_from_file_location("unit_economics", TOOL)
assert SPEC and SPEC.loader
ECONOMICS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ECONOMICS)


def _period(*, all_units=False, complete=True, evidence=True, revenue=20_000_000):
    contract = ECONOMICS.load_contract()
    units = [item["id"] for item in contract["usageUnits"]]
    if not all_units:
        units = ["worker-request", "durable-object-request"]
    measurements = [
        {
            "unit": unit,
            "quantity": str(index + 1),
            "costMicrousd": 500_000 if index < 6 else 0,
            "sourceEvidence": f"invoice-2026-07#{unit}",
        }
        for index, unit in enumerate(units)
    ]
    evidence_value = "evidence/2026-07/run-001" if evidence else ""
    return {
        "schema": "forkmesh-unit-economics-period-v1",
        "periodStart": "2026-07-01T00:00:00Z",
        "periodEnd": "2026-08-01T00:00:00Z",
        "usageWindowComplete": complete,
        "funnel": {
            "publicProjectsAtProof": 100,
            "qualifiedDemand": 20,
            "trialsStarted": 10,
            "trialsCompleted": 8,
            "newPaidCustomers": 4,
        },
        "recognizedRevenueMicrousd": revenue,
        "revenueSource": "billing/2026-07/reconciliation-001",
        "servicesInScope": units,
        "measurements": measurements,
        "grossMarginTargetBps": 6_000,
        "evidence": {
            "billingReconciliation": evidence_value,
            "providerInvoice": evidence_value,
            "portabilityDrill": evidence_value,
            "recoveryDrill": evidence_value,
            "securityPrivacyReview": evidence_value,
            "supportReview": evidence_value,
        },
    }


def _validated_research():
    research = deepcopy(ECONOMICS.load_research())
    research.update({
        "status": "validated",
        "qualifyingInterviews": 5,
        "qualifyingInterviewIds": [
            f"PI-2026-{index:03d}" for index in range(1, 6)],
        "organizationsRepresented": 3,
        "organizationSizeBandsRepresented": 2,
        "buyerValidated": True,
        "budgetValidated": True,
        "topPaidJobsValidated": True,
        "latestRevision": 2,
        "validatedRevision": 2,
    })
    return research


def test_checked_in_contract_is_valid_and_managed_checkout_is_off():
    contract = ECONOMICS.load_contract()
    research = ECONOMICS.load_research()

    ECONOMICS.validate_contract(contract, research)
    assert research["qualifyingInterviews"] == 0
    assert research["syntheticRecordsCount"] == 0
    assert contract["launchGates"] == {
        "unlimitedPaidPlanAllowed": False,
        "managedCheckoutEnabled": False,
        "requiredBeforeManagedCheckout": [
            "pricing-research-validated",
            "all-variable-cost-units-metered",
            "provider-invoices-reconciled",
            "gross-margin-window-reviewed",
            "portability-export-restore-passed",
            "failure-and-recovery-drill-passed",
            "security-privacy-support-docs-approved",
        ],
    }


def test_period_report_calculates_funnel_cost_and_margin_in_integer_money():
    contract = ECONOMICS.load_contract()
    report = ECONOMICS.period_report(
        _period(), contract, ECONOMICS.load_research())

    assert report["qualifiedDemandRateBps"] == 2_000
    assert report["trialStartRateBps"] == 5_000
    assert report["trialToPaidConversionBps"] == 5_000
    assert report["recognizedRevenueMicrousd"] == 20_000_000
    assert report["serviceCostMicrousd"] == 1_000_000
    assert report["grossProfitMicrousd"] == 19_000_000
    assert report["grossMarginBps"] == 9_500
    assert report["complete"] is True
    assert report["managedLaunchReady"] is False
    assert "pricing research is not validated" in report["launchBlockers"]
    assert any(
        blocker.startswith("published cost units are outside")
        for blocker in report["launchBlockers"])


def test_launch_gate_can_pass_only_with_real_research_all_units_and_evidence():
    contract = ECONOMICS.load_contract()
    report = ECONOMICS.period_report(
        _period(all_units=True), contract, _validated_research())

    assert report["grossMarginBps"] == 8_500
    assert report["launchBlockers"] == []
    assert report["managedLaunchReady"] is True


def test_a_future_checkout_state_must_remain_metered_and_bounded():
    contract = deepcopy(ECONOMICS.load_contract())
    managed = next(
        plan for plan in contract["plans"] if plan["id"] == "managed-services")
    contract["launchGates"]["managedCheckoutEnabled"] = True
    managed["availability"] = "metered-limited-availability"

    ECONOMICS.validate_contract(contract, _validated_research())
    managed["availability"] = "available"
    with pytest.raises(ECONOMICS.EconomicsError, match="bounded availability"):
        ECONOMICS.validate_contract(contract, _validated_research())


def test_pricing_validation_requires_interview_ids_and_a_new_revision():
    contract = ECONOMICS.load_contract()
    research = _validated_research()
    research["qualifyingInterviewIds"].pop()
    with pytest.raises(ECONOMICS.EconomicsError, match="count must match"):
        ECONOMICS.validate_contract(contract, research)

    research = _validated_research()
    research["validatedRevision"] = 1
    with pytest.raises(ECONOMICS.EconomicsError, match="new documented revision"):
        ECONOMICS.validate_contract(contract, research)


@pytest.mark.parametrize(
    ("mutate", "message"),
    [
        (
            lambda period: period["funnel"].update(
                {"trialsCompleted": 11}),
            "funnel stages cannot increase downstream",
        ),
        (
            lambda period: period["measurements"].append(
                deepcopy(period["measurements"][0])),
            "duplicates worker-request",
        ),
        (
            lambda period: period["measurements"].pop(),
            "missing measurements for in-scope units",
        ),
        (
            lambda period: period["measurements"][0].update(
                {"quantity": "1e6"}),
            "bounded decimal string",
        ),
        (
            lambda period: period["measurements"][0].update(
                {"quantity": "01"}),
            "bounded decimal string",
        ),
        (
            lambda period: period["measurements"][0].update(
                {"costMicrousd": -1}),
            "non-negative integer",
        ),
        (
            lambda period: period.update({"grossMarginTargetBps": -1}),
            "integer from 0 to 10000",
        ),
    ],
)
def test_malformed_or_selectively_accounted_periods_fail_closed(mutate, message):
    period = _period()
    mutate(period)
    with pytest.raises(ECONOMICS.EconomicsError, match=message):
        ECONOMICS.period_report(
            period, ECONOMICS.load_contract(), ECONOMICS.load_research())


def test_missing_window_evidence_and_zero_revenue_are_explicit_blockers():
    period = _period(
        all_units=True, complete=False, evidence=False, revenue=0)
    report = ECONOMICS.period_report(
        period, ECONOMICS.load_contract(), _validated_research())

    assert report["complete"] is False
    assert report["grossMarginBps"] is None
    assert "usage window is incomplete" in report["launchBlockers"]
    assert "gross margin is unknown because recognized revenue is zero" in (
        report["launchBlockers"])
    assert any(
        blocker.startswith("missing evidence:")
        for blocker in report["launchBlockers"])


def test_cli_validates_contract_and_fails_the_current_managed_launch(tmp_path):
    period_path = tmp_path / "period.json"
    period_path.write_text(json.dumps(_period(all_units=True)), encoding="utf-8")

    validation = subprocess.run(
        [sys.executable, str(TOOL), "validate-contract"],
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
    )
    gate = subprocess.run(
        [sys.executable, str(TOOL), "launch-gate", "--input", str(period_path)],
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
    )

    assert validation.returncode == 0
    assert "business-model contract is valid" in validation.stdout
    assert gate.returncode == 2
    gate_report = json.loads(gate.stdout)
    assert gate_report["managedLaunchReady"] is False
    assert "pricing research is not validated" in gate_report["launchBlockers"]
