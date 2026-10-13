#!/usr/bin/env python3
"""Validate ForkMesh's outcome metric dictionary and build a review report.

``www/docs/outcome-metrics.json`` is the single definition of every KPI. The
``review`` command turns an aggregate observation file into per-metric values
with freshness, confidence, and small-cell suppression. A missing or
unreconciled observation is reported as unknown, never as zero, and an input
that carries an identifier field is rejected before anything is calculated.
"""

from __future__ import annotations

import argparse
from datetime import datetime
import json
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
DICTIONARY_PATH = ROOT / "www" / "docs" / "outcome-metrics.json"
DICTIONARY_SCHEMA = "forkmesh-outcome-metric-dictionary-v2"
OBSERVATIONS_SCHEMA = "forkmesh-outcome-observations-v1"
CADENCES = ("weekly", "monthly")
KINDS = ("count", "rate", "quantity", "currency")
AVAILABILITY = {
    "implemented",
    "implemented-with-partial-coverage",
    "implemented-with-invoice-reconciliation",
    "accounting-reconciliation",
    "requires-period-export",
    "requires-boundary-snapshot",
}
REQUIRED_FIELDS = (
    "id", "stage", "label", "kind", "unit", "definition", "numerator",
    "window", "grain", "source", "reconciliation", "pagination",
    "exclusions", "privacyBoundary", "owner", "availability", "cadence",
    "privacyClass", "collection", "consent", "optOut", "retention",
    "freshnessHours", "coverage", "baseline", "target",
)
HIGH_COVERAGE = 0.95
LOW_COVERAGE = 0.5
EMAIL = re.compile(r"[^@\s]+@[^@\s]+\.[^@\s]+")
IPV4 = re.compile(r"\b\d{1,3}(?:\.\d{1,3}){3}\b")


class OutcomeError(ValueError):
    """The dictionary or an observation file is invalid."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise OutcomeError(message)


def _object(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise OutcomeError(f"{path}: invalid JSON: {exc}") from exc
    _require(isinstance(value, dict), f"{path}: expected a JSON object")
    return value


def load_dictionary(path: Path = DICTIONARY_PATH) -> dict:
    return _object(path)


def validate_dictionary(dictionary: dict, root: Path = ROOT) -> None:
    _require(dictionary.get("schema") == DICTIONARY_SCHEMA,
             "unexpected dictionary schema")
    _require(isinstance(dictionary.get("version"), int)
             and dictionary["version"] >= 2, "dictionary version must be >= 2")
    versions = [entry.get("version") for entry in dictionary.get("changelog", [])]
    _require(versions and versions[-1] == dictionary["version"],
             "changelog must end with the current version")
    privacy = dictionary.get("privacyClasses") or {}
    collection = dictionary.get("collectionMethods") or {}
    coverage = dictionary.get("coverageClasses") or {}
    aggregation = dictionary.get("aggregation") or {}
    minimum = aggregation.get("defaultMinimumCellSize")
    _require(isinstance(minimum, int) and minimum > 1,
             "defaultMinimumCellSize must be an integer above 1")
    guarded = set(aggregation.get("minimumCellSizeAppliesTo") or ())
    _require(guarded <= set(privacy), "unknown privacy class in aggregation")
    _require(aggregation.get("forbiddenFields"), "forbiddenFields is empty")
    for level in ("high", "medium", "low", "unknown"):
        _require(dictionary.get("confidencePolicy", {}).get(level),
                 f"confidencePolicy.{level} is missing")
    for cadence in CADENCES + ("quarterly",):
        _require(dictionary.get("decisionCadence", {}).get(cadence),
                 f"decisionCadence.{cadence} is missing")

    ids = []
    for metric in dictionary.get("metrics") or []:
        metric_id = metric.get("id")
        for field in REQUIRED_FIELDS:
            _require(metric.get(field) not in (None, "", []),
                     f"{metric_id}: {field} is required")
        _require("denominator" in metric, f"{metric_id}: denominator is required")
        _require(metric["kind"] in KINDS, f"{metric_id}: unknown kind")
        if metric["kind"] == "rate":
            _require(bool(metric["denominator"]),
                     f"{metric_id}: a rate needs a denominator")
        _require(metric["cadence"] in CADENCES, f"{metric_id}: unknown cadence")
        _require(metric["availability"] in AVAILABILITY,
                 f"{metric_id}: unknown availability")
        _require(metric["privacyClass"] in privacy,
                 f"{metric_id}: unknown privacyClass")
        _require(metric["collection"] in collection,
                 f"{metric_id}: unknown collection method")
        _require(metric["coverage"] in coverage, f"{metric_id}: unknown coverage")
        cell = metric.get("minimumCellSize")
        _require(isinstance(cell, int) and cell >= 0,
                 f"{metric_id}: minimumCellSize must be a non-negative integer")
        if metric["privacyClass"] in guarded:
            _require(cell >= minimum,
                     f"{metric_id}: minimumCellSize is below the policy default")
        _require(isinstance(metric["freshnessHours"], int)
                 and metric["freshnessHours"] > 0,
                 f"{metric_id}: freshnessHours must be a positive integer")
        if metric["collection"] == "operator-reported":
            _require(metric["coverage"] != "full",
                     f"{metric_id}: operator reports are voluntary, never full")
        _require(metric["pagination"].get("page_size_is_cap") is False,
                 f"{metric_id}: a page size is never the population")
        for source in metric["source"]:
            _require((root / source["path"]).is_file(),
                     f"{metric_id}: missing source {source['path']}")
        _validate_baseline_and_target(metric)
        ids.append(metric_id)
    _require(ids, "the dictionary defines no metrics")
    _require(len(ids) == len(set(ids)), "metric ids must be unique")
    scope = dictionary.get("scopeCoverage") or {}
    _require(scope, "scopeCoverage is empty")
    for term, metric_ids in scope.items():
        _require(metric_ids and set(metric_ids) <= set(ids),
                 f"scope '{term}' names an unknown metric")


def _validate_baseline_and_target(metric: dict) -> None:
    metric_id = metric["id"]
    baseline = metric["baseline"]
    if baseline.get("status") == "measured":
        _require(baseline.get("value") is not None and baseline.get("asOf"),
                 f"{metric_id}: a measured baseline needs value and asOf")
    else:
        _require(baseline.get("status") == "unmeasured"
                 and baseline.get("value") is None,
                 f"{metric_id}: an unmeasured baseline cannot carry a value")
    target = metric["target"]
    if target.get("status") == "adopted":
        _require(target.get("value") is not None and target.get("source"),
                 f"{metric_id}: an adopted target needs a value and a review record")
    else:
        _require(target.get("status") == "to-be-set"
                 and target.get("value") is None,
                 f"{metric_id}: a target that is not adopted cannot carry a value")


def _timestamp(value: object, field: str) -> datetime:
    _require(isinstance(value, str) and value.endswith("Z"),
             f"{field} must be a UTC timestamp ending in Z")
    try:
        return datetime.fromisoformat(value[:-1] + "+00:00")
    except ValueError as exc:
        raise OutcomeError(f"{field} is not an ISO-8601 timestamp") from exc


def reject_identifiers(value: object, forbidden: set[str], where: str = "input") -> None:
    """Refuse any observation that carries an identifier-shaped field or value."""
    lowered = {name.lower() for name in forbidden}
    if isinstance(value, dict):
        for key, child in value.items():
            _require(str(key).lower() not in lowered,
                     f"{where}.{key}: identifier fields are not accepted")
            reject_identifiers(child, forbidden, f"{where}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            reject_identifiers(child, forbidden, f"{where}[{index}]")
    elif isinstance(value, str):
        _require(not EMAIL.search(value) and not IPV4.search(value),
                 f"{where}: email addresses and IP addresses are not accepted")


def _count(value: object, field: str) -> int | None:
    if value is None:
        return None
    _require(isinstance(value, int) and not isinstance(value, bool)
             and value >= 0, f"{field} must be a non-negative integer")
    return value


def _suppress(segments: list, minimum: int, metric_id: str) -> list:
    rows = []
    for index, segment in enumerate(segments or []):
        _require(isinstance(segment, dict) and isinstance(segment.get("label"), str),
                 f"{metric_id}.segments[{index}] needs a label")
        rows.append({
            "label": segment["label"],
            "value": _count(segment.get("value"),
                            f"{metric_id}.segments[{index}].value"),
        })
    hidden = {index for index, row in enumerate(rows)
              if minimum > 0 and row["value"] is not None
              and row["value"] < minimum}
    visible = [index for index in range(len(rows))
               if index not in hidden and rows[index]["value"] is not None]
    if len(hidden) == 1 and visible:
        hidden.add(min(visible, key=lambda index: rows[index]["value"]))
    return [
        {"label": row["label"], "value": None, "suppressed": True}
        if index in hidden else dict(row, suppressed=False)
        for index, row in enumerate(rows)
    ]


def evaluate(metric: dict, observation: dict | None, *, period_end: datetime,
             generated_at: datetime) -> dict:
    """Return one metric's value, freshness, confidence, and publishability."""
    metric_id = metric["id"]
    result = {
        "metric": metric_id,
        "kind": metric["kind"],
        "unit": metric["unit"],
        "owner": metric["owner"],
        "privacyClass": metric["privacyClass"],
        "coverageClass": metric["coverage"],
        "value": None,
        "display": "UNKNOWN",
        "freshness": "unknown",
        "confidence": "unknown",
        "status": "UNKNOWN",
        "reasons": [],
        "segments": [],
        "publishable": False,
    }
    if observation is None:
        result["reasons"].append("no observation: missing is unknown, not zero")
        return result

    numerator = _count(observation.get("numerator"), f"{metric_id}.numerator")
    denominator = _count(observation.get("denominator"), f"{metric_id}.denominator")
    source_total = _count(observation.get("sourceTotal"), f"{metric_id}.sourceTotal")
    collected = _count(observation.get("collectedTotal"),
                       f"{metric_id}.collectedTotal")
    reasons = result["reasons"]
    if numerator is None:
        reasons.append("numerator missing")
    if metric["kind"] == "rate" and denominator is None:
        reasons.append("denominator missing")
    if source_total is None or collected is None:
        reasons.append("source and collected totals are both required")
    elif source_total != collected:
        reasons.append(
            f"reconciliation failed: source {source_total} != collected {collected}")
    if (metric["pagination"].get("required")
            and observation.get("continuationExhausted") is not True):
        reasons.append("continuation pages were not exhausted")

    data_cut = _timestamp(observation.get("dataCutAt"), f"{metric_id}.dataCutAt")
    if data_cut < period_end:
        reasons.append("data cut precedes the end of the window")
    age_hours = (generated_at - data_cut).total_seconds() / 3600
    _require(age_hours >= 0, f"{metric_id}.dataCutAt is after generatedAt")
    result["dataAgeHours"] = round(age_hours, 2)

    coverage = observation.get("coverage")
    if coverage is not None:
        _require(isinstance(coverage, (int, float)) and not isinstance(coverage, bool)
                 and 0 <= coverage <= 1, f"{metric_id}.coverage must be 0..1")
    result["measuredCoverage"] = coverage
    result["segments"] = _suppress(
        observation.get("segments"), metric["minimumCellSize"], metric_id)
    population = _count(observation.get("population"), f"{metric_id}.population")
    small = (metric["minimumCellSize"] > 0 and population is not None
             and population < metric["minimumCellSize"])

    if reasons:
        return result

    if metric["kind"] == "rate":
        if denominator == 0:
            result["display"] = "n/a"
        else:
            result["value"] = numerator * 10_000 // denominator
            result["display"] = f"{result['value']} bps"
    else:
        result["value"] = numerator
        result["display"] = f"{numerator} {metric['unit']}"

    stale = age_hours > metric["freshnessHours"]
    result["freshness"] = "stale" if stale else "fresh"
    if stale or (coverage is not None and coverage < LOW_COVERAGE):
        confidence = "low"
    elif (metric["coverage"] != "full" or coverage is None
          or coverage < HIGH_COVERAGE):
        confidence = "medium"
    else:
        confidence = "high"
    if stale:
        reasons.append(
            f"data cut is {age_hours:.0f}h old; limit {metric['freshnessHours']}h")
    if small:
        reasons.append("population below the minimum cell size: internal only")
    result["confidence"] = confidence
    result["status"] = "PARTIAL" if confidence == "low" else "COMPLETE"
    result["publishable"] = not small
    return result


def review_report(observations: dict, dictionary: dict) -> dict:
    _require(observations.get("schema") == OBSERVATIONS_SCHEMA,
             "unexpected observations schema")
    _require(observations.get("dictionarySchema") == dictionary["schema"]
             and observations.get("dictionaryVersion") == dictionary["version"],
             "observations were produced for a different dictionary version")
    reject_identifiers(observations,
                       set(dictionary["aggregation"]["forbiddenFields"]))
    cadence = observations.get("cadence")
    _require(cadence in CADENCES, "cadence must be weekly or monthly")
    period_start = _timestamp(observations.get("periodStart"), "periodStart")
    period_end = _timestamp(observations.get("periodEnd"), "periodEnd")
    generated_at = _timestamp(observations.get("generatedAt"), "generatedAt")
    _require(period_start < period_end <= generated_at,
             "periodStart < periodEnd <= generatedAt is required")

    by_id = {metric["id"]: metric for metric in dictionary["metrics"]}
    supplied = {}
    for index, row in enumerate(observations.get("observations") or []):
        _require(isinstance(row, dict), f"observations[{index}] must be an object")
        metric_id = row.get("metric")
        _require(metric_id in by_id, f"observations[{index}]: unknown metric")
        _require(metric_id not in supplied, f"{metric_id}: duplicate observation")
        supplied[metric_id] = row

    in_scope = [metric for metric in dictionary["metrics"]
                if cadence == "monthly" or metric["cadence"] == "weekly"]
    results = [
        evaluate(metric, supplied.get(metric["id"]), period_end=period_end,
                 generated_at=generated_at)
        for metric in in_scope
    ]
    statuses = {result["status"] for result in results}
    quality = ("COMPLETE" if statuses == {"COMPLETE"}
               else "UNKNOWN" if statuses == {"UNKNOWN"} else "PARTIAL")
    return {
        "dictionary": f"{dictionary['schema']} / version {dictionary['version']}",
        "cadence": cadence,
        "periodStart": observations["periodStart"],
        "periodEnd": observations["periodEnd"],
        "generatedAt": observations["generatedAt"],
        "qualityState": quality,
        "metrics": results,
    }


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("validate-dictionary")
    review = subparsers.add_parser("review")
    review.add_argument("--input", required=True, type=Path)
    return parser


def main() -> int:
    args = _parser().parse_args()
    try:
        dictionary = load_dictionary()
        validate_dictionary(dictionary)
        if args.command == "validate-dictionary":
            print("outcome metric dictionary is valid")
            return 0
        report = review_report(_object(args.input), dictionary)
        print(json.dumps(report, indent=2, sort_keys=True))
        return 0
    except (OutcomeError, OSError) as exc:
        print(f"outcome-metrics error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
