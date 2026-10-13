#!/usr/bin/env python3
"""Validate ForkMesh's commercial contract and calculate measured unit economics.

Money is represented as integer micro-USD throughout. Quantities are parsed as
``Decimal`` values from JSON strings, so the report never gains binary floating
point rounding at invoice boundaries.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
from decimal import Decimal, InvalidOperation
import json
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
CONTRACT_PATH = ROOT / "www" / "public" / "business-model.json"
RESEARCH_PATH = (
    ROOT / "www" / "docs" / "research" / "pricing-interviews.json"
)
PERIOD_SCHEMA = "forkmesh-unit-economics-period-v1"
REQUIRED_PLAN_IDS = {
    "protocol-self-hosted",
    "hosted-free",
    "supporter-membership",
    "pro-membership",
    "managed-services",
}
REQUIRED_EVIDENCE = (
    "billingReconciliation",
    "providerInvoice",
    "portabilityDrill",
    "recoveryDrill",
    "securityPrivacyReview",
    "supportReview",
)
FUNNEL_FIELDS = (
    "publicProjectsAtProof",
    "qualifiedDemand",
    "trialsStarted",
    "trialsCompleted",
    "newPaidCustomers",
)
REQUIRED_MANAGED_GATES = {
    "pricing-research-validated",
    "all-variable-cost-units-metered",
    "provider-invoices-reconciled",
    "gross-margin-window-reviewed",
    "portability-export-restore-passed",
    "failure-and-recovery-drill-passed",
    "security-privacy-support-docs-approved",
}


class EconomicsError(ValueError):
    """The checked-in contract or supplied accounting period is invalid."""


def _object(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise EconomicsError(f"cannot read {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise EconomicsError(f"{path} must contain a JSON object")
    return value


def load_contract(path: Path = CONTRACT_PATH) -> dict:
    return _object(path)


def load_research(path: Path = RESEARCH_PATH) -> dict:
    return _object(path)


def validate_contract(contract: dict, research: dict) -> None:
    if contract.get("schema") != "forkmesh-business-model-v1":
        raise EconomicsError("unsupported business-model schema")
    if contract.get("schemaVersion") != 1:
        raise EconomicsError("unsupported business-model schemaVersion")

    commitments = contract.get("permanentCommitments")
    if not isinstance(commitments, dict):
        raise EconomicsError("permanentCommitments must be an object")
    for key in (
        "protocolUseRequiresPayment",
        "selfHostingRequiresPayment",
        "publicReadAndCloneRequirePaidPlan",
        "portableGitDataRequiresPaidPlan",
        "portableSignedCollaborationRequiresPaidPlan",
        "governanceRequiresPaidPlan",
    ):
        if commitments.get(key) is not False:
            raise EconomicsError(f"permanent free commitment changed: {key}")
    if commitments.get("protocolLicense") != "MIT":
        raise EconomicsError("protocol license must match LICENSE")
    if commitments.get("noDataSale") is not True:
        raise EconomicsError("the permanent no-data-sale commitment is required")
    data_policy = contract.get("dataPolicy")
    if not isinstance(data_policy, dict) or data_policy.get("noDataSale") is not True:
        raise EconomicsError("dataPolicy.noDataSale must be true")

    units = contract.get("usageUnits")
    if not isinstance(units, list) or not units:
        raise EconomicsError("usageUnits must be a non-empty list")
    unit_ids = []
    for unit in units:
        if not isinstance(unit, dict):
            raise EconomicsError("every usage unit must be an object")
        unit_id = unit.get("id")
        if not isinstance(unit_id, str) or not unit_id:
            raise EconomicsError("every usage unit needs an id")
        if not str(unit.get("definition") or "").strip():
            raise EconomicsError(f"{unit_id}: missing definition")
        if not str(unit.get("costSource") or "").strip():
            raise EconomicsError(f"{unit_id}: missing cost source")
        unit_ids.append(unit_id)
    if len(unit_ids) != len(set(unit_ids)):
        raise EconomicsError("usage unit ids must be unique")

    portability = contract.get("portability")
    if not isinstance(portability, dict):
        raise EconomicsError("portability must be an object")
    if portability.get("canonicalRepositoryFormat") != "Git objects and refs":
        raise EconomicsError("canonical repository portability changed")
    if portability.get("canonicalCollaborationFormat") != (
        "Signed .forkmesh repository records"
    ):
        raise EconomicsError("canonical collaboration portability changed")
    if portability.get("exportFeeMicrousd") != 0:
        raise EconomicsError("canonical export must remain zero-price")

    entitlements = contract.get("entitlements")
    if not isinstance(entitlements, list) or not entitlements:
        raise EconomicsError("the public entitlement matrix cannot be empty")
    entitlement_fields = {
        "capability", "protocolRight", "selfHosted", "hostedFree", "paidService"
    }
    capabilities = []
    for row in entitlements:
        if not isinstance(row, dict) or set(row) != entitlement_fields:
            raise EconomicsError("each entitlement needs the five public columns")
        if any(not isinstance(row[field], str) or not row[field].strip()
               for field in entitlement_fields):
            raise EconomicsError("entitlement values must be non-empty strings")
        capabilities.append(row["capability"])
    if len(capabilities) != len(set(capabilities)):
        raise EconomicsError("entitlement capabilities must be unique")

    plans = contract.get("plans")
    if not isinstance(plans, list):
        raise EconomicsError("plans must be a list")
    by_plan = {
        str(plan.get("id") or ""): plan
        for plan in plans
        if isinstance(plan, dict)
    }
    if set(by_plan) != REQUIRED_PLAN_IDS:
        raise EconomicsError("the five public plan boundaries must be explicit")
    for plan_id, plan in by_plan.items():
        limits = plan.get("limits")
        if not isinstance(limits, list) or not limits or any(
            not isinstance(item, str) or not item.strip() for item in limits
        ):
            raise EconomicsError(f"{plan_id} needs published limits")
        if not isinstance(plan.get("support"), str) or not plan["support"].strip():
            raise EconomicsError(f"{plan_id} needs a support boundary")
    for free_plan in ("protocol-self-hosted", "hosted-free"):
        price = by_plan[free_plan].get("price") or {}
        if price.get("amountMicrousd") != 0:
            raise EconomicsError(f"{free_plan} must remain zero-price")
    for membership, amount in (
        ("supporter-membership", 5_000_000),
        ("pro-membership", 10_000_000),
    ):
        price = by_plan[membership].get("price") or {}
        if price != {"amountMicrousd": amount, "interval": "month"}:
            raise EconomicsError(f"{membership} published price drifted")
    managed = by_plan["managed-services"]
    managed_availability = managed.get("availability")
    if managed_availability not in {
        "research-preview-not-for-sale", "metered-limited-availability"
    }:
        raise EconomicsError("managed services need a recognized bounded availability")
    managed_text = " ".join(str(item) for item in managed.get("limits") or [])
    if "No unlimited plan" not in managed_text:
        raise EconomicsError("managed services must explicitly prohibit unlimited plans")

    launch = contract.get("launchGates")
    if not isinstance(launch, dict):
        raise EconomicsError("launchGates must be an object")
    if launch.get("unlimitedPaidPlanAllowed") is not False:
        raise EconomicsError("unlimited paid plans are prohibited")
    checkout_enabled = launch.get("managedCheckoutEnabled")
    if not isinstance(checkout_enabled, bool):
        raise EconomicsError("managedCheckoutEnabled must be boolean")
    expected_availability = (
        "metered-limited-availability"
        if checkout_enabled else "research-preview-not-for-sale"
    )
    if managed_availability != expected_availability:
        raise EconomicsError(
            "managed availability must agree with the checkout state")
    required_gates = launch.get("requiredBeforeManagedCheckout")
    if not isinstance(required_gates, list) or set(required_gates) != (
        REQUIRED_MANAGED_GATES
    ):
        raise EconomicsError("managed-checkout evidence gates are incomplete")

    if research.get("schema") != "forkmesh-pricing-research-v1":
        raise EconomicsError("unsupported pricing-research schema")
    if research.get("status") not in {"not-validated", "validated"}:
        raise EconomicsError("pricing-research status is not recognized")
    interviews = _whole(research.get("qualifyingInterviews"), "qualifyingInterviews")
    minimum = _whole(
        research.get("minimumQualifyingInterviews"),
        "minimumQualifyingInterviews",
    )
    interview_ids = research.get("qualifyingInterviewIds")
    if not isinstance(interview_ids, list) or any(
        not isinstance(item, str)
        or re.fullmatch(r"PI-[0-9]{4}-[0-9]{3}", item) is None
        for item in interview_ids
    ):
        raise EconomicsError("qualifyingInterviewIds must contain only PI-YYYY-NNN ids")
    if len(interview_ids) != len(set(interview_ids)):
        raise EconomicsError("qualifyingInterviewIds must be unique")
    if len(interview_ids) != interviews:
        raise EconomicsError("qualifying interview count must match its evidence ids")
    latest_revision = _whole(research.get("latestRevision"), "latestRevision")
    validated_revision = research.get("validatedRevision")
    if research.get("status") == "validated":
        if interviews < minimum:
            raise EconomicsError("pricing research cannot validate below its cohort minimum")
        if (
            isinstance(validated_revision, bool)
            or not isinstance(validated_revision, int)
            or validated_revision <= 1
            or validated_revision != latest_revision
        ):
            raise EconomicsError("validated pricing needs a new documented revision")
    elif validated_revision is not None:
        raise EconomicsError("unvalidated pricing cannot name a validated revision")
    if _whole(research.get("syntheticRecordsCount"), "syntheticRecordsCount"):
        raise EconomicsError("synthetic pricing interviews are forbidden")


def _whole(value, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise EconomicsError(f"{label} must be a non-negative integer")
    return value


def _utc(value, label: str) -> datetime:
    if not isinstance(value, str) or not value.endswith("Z"):
        raise EconomicsError(f"{label} must be an ISO-8601 UTC timestamp ending in Z")
    try:
        parsed = datetime.fromisoformat(value[:-1] + "+00:00")
    except ValueError as exc:
        raise EconomicsError(f"{label} is not a valid timestamp") from exc
    if parsed.tzinfo != timezone.utc:
        raise EconomicsError(f"{label} must be UTC")
    return parsed


def _quantity(value, label: str) -> Decimal:
    if (
        not isinstance(value, str)
        or len(value) > 80
        or re.fullmatch(r"(?:0|[1-9][0-9]*)(?:\.[0-9]{1,9})?", value) is None
    ):
        raise EconomicsError(f"{label} must be a bounded decimal string")
    try:
        quantity = Decimal(value)
    except InvalidOperation as exc:
        raise EconomicsError(f"{label} is not a decimal") from exc
    if not quantity.is_finite() or quantity < 0:
        raise EconomicsError(f"{label} must be non-negative with at most 9 decimals")
    return quantity


def validate_period(period: dict, contract: dict) -> dict:
    allowed = {
        "schema",
        "periodStart",
        "periodEnd",
        "usageWindowComplete",
        "funnel",
        "recognizedRevenueMicrousd",
        "revenueSource",
        "servicesInScope",
        "measurements",
        "grossMarginTargetBps",
        "evidence",
    }
    extra = set(period) - allowed
    if extra:
        raise EconomicsError("unsupported period fields: " + ", ".join(sorted(extra)))
    if period.get("schema") != PERIOD_SCHEMA:
        raise EconomicsError("unsupported unit-economics period schema")
    start = _utc(period.get("periodStart"), "periodStart")
    end = _utc(period.get("periodEnd"), "periodEnd")
    if end <= start:
        raise EconomicsError("periodEnd must be after periodStart")
    if (end - start).total_seconds() > 366 * 86_400:
        raise EconomicsError("one report cannot span more than 366 days")
    if not isinstance(period.get("usageWindowComplete"), bool):
        raise EconomicsError("usageWindowComplete must be boolean")

    funnel = period.get("funnel")
    if not isinstance(funnel, dict) or set(funnel) != set(FUNNEL_FIELDS):
        raise EconomicsError("funnel must contain only the five defined stages")
    counts = {field: _whole(funnel.get(field), f"funnel.{field}") for field in FUNNEL_FIELDS}
    ordered = [counts[field] for field in FUNNEL_FIELDS]
    if any(later > earlier for earlier, later in zip(ordered, ordered[1:])):
        raise EconomicsError("funnel stages cannot increase downstream")

    revenue = _whole(
        period.get("recognizedRevenueMicrousd"),
        "recognizedRevenueMicrousd",
    )
    revenue_source = period.get("revenueSource")
    if (
        not isinstance(revenue_source, str)
        or not revenue_source.strip()
        or len(revenue_source) > 400
    ):
        raise EconomicsError("revenueSource must name bounded reconciliation evidence")

    known_units = {str(unit["id"]) for unit in contract["usageUnits"]}
    services = period.get("servicesInScope")
    if not isinstance(services, list) or not services:
        raise EconomicsError("servicesInScope must be a non-empty list")
    if any(not isinstance(service, str) or not service for service in services):
        raise EconomicsError("servicesInScope values must be non-empty strings")
    if len(services) != len(set(services)):
        raise EconomicsError("servicesInScope must not contain duplicates")
    unknown = set(services) - known_units
    if unknown:
        raise EconomicsError("unknown servicesInScope: " + ", ".join(sorted(unknown)))

    measurements = period.get("measurements")
    if not isinstance(measurements, list) or not measurements:
        raise EconomicsError("measurements must be a non-empty list")
    measured_units = set()
    service_cost = 0
    normalized_measurements = []
    for index, measurement in enumerate(measurements):
        if not isinstance(measurement, dict) or set(measurement) != {
            "unit", "quantity", "costMicrousd", "sourceEvidence"
        }:
            raise EconomicsError(f"measurements[{index}] has unsupported fields")
        unit = measurement.get("unit")
        if unit not in known_units:
            raise EconomicsError(f"measurements[{index}] uses unknown unit")
        if unit in measured_units:
            raise EconomicsError(f"measurements[{index}] duplicates {unit}")
        if unit not in services:
            raise EconomicsError(
                f"measurements[{index}] is outside servicesInScope")
        quantity = _quantity(measurement.get("quantity"), f"measurements[{index}].quantity")
        cost = _whole(measurement.get("costMicrousd"), f"measurements[{index}].costMicrousd")
        source = measurement.get("sourceEvidence")
        if not isinstance(source, str) or not source.strip() or len(source) > 400:
            raise EconomicsError(f"measurements[{index}] needs source evidence")
        measured_units.add(unit)
        service_cost += cost
        normalized_measurements.append({
            "unit": unit,
            "quantity": format(quantity, "f"),
            "costMicrousd": cost,
            "sourceEvidence": source.strip(),
        })
    missing = set(services) - measured_units
    if missing:
        raise EconomicsError(
            "missing measurements for in-scope units: "
            + ", ".join(sorted(missing)))

    target = period.get("grossMarginTargetBps")
    if isinstance(target, bool) or not isinstance(target, int) or not (0 <= target <= 10_000):
        raise EconomicsError("grossMarginTargetBps must be an integer from 0 to 10000")
    evidence = period.get("evidence")
    if not isinstance(evidence, dict) or set(evidence) != set(REQUIRED_EVIDENCE):
        raise EconomicsError("evidence must contain only the six defined gates")
    for key, value in evidence.items():
        if not isinstance(value, str) or len(value) > 400:
            raise EconomicsError(f"evidence.{key} must be a bounded string")

    return {
        "start": start,
        "end": end,
        "counts": counts,
        "revenueMicrousd": revenue,
        "serviceCostMicrousd": service_cost,
        "services": services,
        "measurements": normalized_measurements,
        "targetMarginBps": target,
        "evidence": evidence,
    }


def _rate(numerator: int, denominator: int) -> int | None:
    if denominator == 0:
        return None
    return (numerator * 10_000 + denominator // 2) // denominator


def period_report(period: dict, contract: dict, research: dict) -> dict:
    validate_contract(contract, research)
    normalized = validate_period(period, contract)
    revenue = normalized["revenueMicrousd"]
    cost = normalized["serviceCostMicrousd"]
    profit = revenue - cost
    margin = None if revenue == 0 else int(
        (Decimal(profit) * Decimal(10_000) / Decimal(revenue)).quantize(Decimal("1"))
    )
    counts = normalized["counts"]
    blockers = launch_blockers(period, contract, research, normalized, margin)
    return {
        "schema": "forkmesh-unit-economics-report-v1",
        "periodStart": period["periodStart"],
        "periodEnd": period["periodEnd"],
        "periodDays": (normalized["end"] - normalized["start"]).total_seconds() / 86400,
        "funnel": counts,
        "qualifiedDemandRateBps": _rate(
            counts["qualifiedDemand"], counts["publicProjectsAtProof"]
        ),
        "trialStartRateBps": _rate(
            counts["trialsStarted"], counts["qualifiedDemand"]
        ),
        "trialToPaidConversionBps": _rate(
            counts["newPaidCustomers"], counts["trialsCompleted"]
        ),
        "recognizedRevenueMicrousd": revenue,
        "serviceCostMicrousd": cost,
        "grossProfitMicrousd": profit,
        "grossMarginBps": margin,
        "grossMarginTargetBps": normalized["targetMarginBps"],
        "measurements": normalized["measurements"],
        "complete": bool(period["usageWindowComplete"]) and not (
            set(normalized["services"])
            - {item["unit"] for item in normalized["measurements"]}
        ),
        "managedLaunchReady": not blockers,
        "launchBlockers": blockers,
    }


def launch_blockers(
    period: dict,
    contract: dict,
    research: dict,
    normalized: dict | None = None,
    margin_bps: int | None = None,
) -> list[str]:
    normalized = normalized or validate_period(period, contract)
    blockers = []
    if research.get("status") != "validated":
        blockers.append("pricing research is not validated")
    for key in ("buyerValidated", "budgetValidated", "topPaidJobsValidated"):
        if research.get(key) is not True:
            blockers.append(key + " is false")
    minimums = (
        ("qualifyingInterviews", "minimumQualifyingInterviews"),
        ("organizationsRepresented", "minimumOrganizations"),
        ("organizationSizeBandsRepresented", "minimumOrganizationSizeBands"),
    )
    for actual_key, minimum_key in minimums:
        actual = _whole(research.get(actual_key), actual_key)
        minimum = _whole(research.get(minimum_key), minimum_key)
        if actual < minimum:
            blockers.append(f"{actual_key} is below {minimum_key}")
    if not period.get("usageWindowComplete"):
        blockers.append("usage window is incomplete")
    if (normalized["end"] - normalized["start"]).days < 30:
        blockers.append("unit-economics window is shorter than 30 days")
    missing_evidence = [
        key for key in REQUIRED_EVIDENCE
        if not str(normalized["evidence"].get(key) or "").strip()
    ]
    if missing_evidence:
        blockers.append("missing evidence: " + ", ".join(missing_evidence))
    known_units = {str(item["id"]) for item in contract["usageUnits"]}
    unscoped_units = known_units - set(normalized["services"])
    if unscoped_units:
        blockers.append(
            "published cost units are outside the measured window: "
            + ", ".join(sorted(unscoped_units)))
    if margin_bps is None:
        revenue = normalized["revenueMicrousd"]
        if revenue:
            margin_bps = int(
                (Decimal(revenue - normalized["serviceCostMicrousd"])
                 * Decimal(10_000) / Decimal(revenue)).quantize(Decimal("1"))
            )
    if margin_bps is None:
        blockers.append("gross margin is unknown because recognized revenue is zero")
    elif margin_bps < normalized["targetMarginBps"]:
        blockers.append("gross margin is below the reviewed target")
    return blockers


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("validate-contract")
    for command in ("report", "launch-gate"):
        child = subparsers.add_parser(command)
        child.add_argument("--input", required=True, type=Path)
    return parser


def main() -> int:
    args = _parser().parse_args()
    try:
        contract = load_contract()
        research = load_research()
        validate_contract(contract, research)
        if args.command == "validate-contract":
            print("business-model contract is valid")
            return 0
        period = _object(args.input)
        report = period_report(period, contract, research)
        print(json.dumps(report, indent=2, sort_keys=True))
        if args.command == "launch-gate" and not report["managedLaunchReady"]:
            return 2
        return 0
    except (EconomicsError, OSError) as exc:
        print(f"unit-economics error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
