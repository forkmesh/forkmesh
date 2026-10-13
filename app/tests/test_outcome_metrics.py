"""Outcome metric dictionary, monthly review template, and review report (issue #519).

``www/docs/outcome-metrics.json`` defines every community, product, and revenue
KPI once. ``tools/outcome_metrics.py`` validates it and turns an aggregate
observation file into values with freshness and confidence; missing data is
unknown, small segments are suppressed, and identifiers are refused.
"""

from copy import deepcopy
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "outcome_metrics.py"
REVIEW_PATH = ROOT / "www" / "docs" / "outcome-review.md"
SCORECARD_PATH = ROOT / "www" / "docs" / "activation-revenue-scorecard.md"
SPEC = importlib.util.spec_from_file_location("outcome_metrics", TOOL)
assert SPEC and SPEC.loader
OUTCOMES = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(OUTCOMES)

ISSUE_SCOPE = {
    "activation", "successful migration", "first contribution",
    "active maintainers", "repository availability", "paid-team conversion",
    "hosted-service adoption", "retention", "support", "infrastructure margin",
}
MONTH_START = "2026-08-01T00:00:00Z"
MONTH_END = "2026-09-01T00:00:00Z"


def _dictionary():
    return OUTCOMES.load_dictionary()


def _metric(dictionary, metric_id):
    return next(m for m in dictionary["metrics"] if m["id"] == metric_id)


def _observation(metric, **overrides):
    row = {
        "metric": metric,
        "numerator": 9_990,
        "denominator": 10_000,
        "sourceTotal": 31,
        "collectedTotal": 31,
        "continuationExhausted": True,
        "coverage": 1.0,
        "dataCutAt": "2026-09-01T01:00:00Z",
    }
    row.update(overrides)
    return row


def _observations(*rows, cadence="monthly", generated="2026-09-01T02:00:00Z"):
    dictionary = _dictionary()
    return {
        "schema": "forkmesh-outcome-observations-v1",
        "dictionarySchema": dictionary["schema"],
        "dictionaryVersion": dictionary["version"],
        "cadence": cadence,
        "periodStart": MONTH_START,
        "periodEnd": MONTH_END,
        "generatedAt": generated,
        "observations": list(rows),
    }


def _report(*rows, **kwargs):
    report = OUTCOMES.review_report(_observations(*rows, **kwargs), _dictionary())
    return report, {item["metric"]: item for item in report["metrics"]}


def test_dictionary_is_valid_versioned_and_supersedes_v1():
    dictionary = _dictionary()

    OUTCOMES.validate_dictionary(dictionary)
    assert dictionary["schema"] == "forkmesh-outcome-metric-dictionary-v2"
    assert dictionary["version"] == 2
    assert dictionary["supersedes"] == (
        "forkmesh-activation-revenue-metric-dictionary-v1")
    assert [entry["version"] for entry in dictionary["changelog"]] == [1, 2]
    assert not (ROOT / "www" / "docs" / "activation-revenue-metrics.json").exists()


def test_every_issue_scope_term_maps_to_defined_kpis():
    dictionary = _dictionary()
    ids = {metric["id"] for metric in dictionary["metrics"]}

    assert set(dictionary["scopeCoverage"]) == ISSUE_SCOPE
    covered = set()
    for metric_ids in dictionary["scopeCoverage"].values():
        assert set(metric_ids) <= ids
        covered.update(metric_ids)
    # Every KPI answers at least one scope question except the clone total,
    # which is an activation diagnostic kept outside the attributed funnel.
    assert ids - covered == {"anonymous_clone"}


def test_every_kpi_names_numerator_denominator_window_owner_privacy_and_source():
    dictionary = _dictionary()
    for metric in dictionary["metrics"]:
        for field in ("numerator", "window", "owner", "privacyClass"):
            assert metric[field].strip(), (metric["id"], field)
        assert "denominator" in metric, metric["id"]
        assert metric["privacyClass"] in dictionary["privacyClasses"]
        assert metric["source"] and all(
            (ROOT / source["path"]).is_file() for source in metric["source"])
        for field in ("consent", "optOut", "retention"):
            assert metric[field].strip(), (metric["id"], field)


@pytest.mark.parametrize(("mutate", "message"), [
    (lambda m: m.update(minimumCellSize=3), "below the policy default"),
    (lambda m: m["baseline"].update(value=120), "unmeasured baseline"),
    (lambda m: m["target"].update(value=500), "not adopted"),
    (lambda m: m["target"].update(status="adopted", value=500), "review record"),
    (lambda m: m.update(cadence="daily"), "unknown cadence"),
    (lambda m: m.update(privacyClass="raw-events"), "unknown privacyClass"),
    (lambda m: m["pagination"].update(page_size_is_cap=True), "never the population"),
    (lambda m: m["source"].append({"path": "app/src/nope.py", "locator": "x_y"}), "missing source"),
    (lambda m: m.update(denominator=None), "needs a denominator"),
])
def test_validator_rejects_weakened_or_invented_definitions(mutate, message):
    dictionary = deepcopy(_dictionary())
    mutate(_metric(dictionary, "successful_migration"))

    with pytest.raises(OUTCOMES.OutcomeError, match=message):
        OUTCOMES.validate_dictionary(dictionary)


def test_voluntary_operator_reports_can_never_claim_full_coverage():
    dictionary = deepcopy(_dictionary())
    mirrors = _metric(dictionary, "independent_mirror_nodes")
    assert mirrors["collection"] == "operator-reported"
    assert mirrors["coverage"] == "lower-bound"
    mirrors["coverage"] = "full"

    with pytest.raises(OUTCOMES.OutcomeError, match="voluntary"):
        OUTCOMES.validate_dictionary(dictionary)


def test_validator_rejects_duplicate_ids_and_unknown_scope_metrics():
    dictionary = deepcopy(_dictionary())
    dictionary["metrics"].append(deepcopy(dictionary["metrics"][0]))
    with pytest.raises(OUTCOMES.OutcomeError, match="unique"):
        OUTCOMES.validate_dictionary(dictionary)

    dictionary = deepcopy(_dictionary())
    dictionary["scopeCoverage"]["retention"] = ["active_owner_retention"]
    with pytest.raises(OUTCOMES.OutcomeError, match="unknown metric"):
        OUTCOMES.validate_dictionary(dictionary)


def test_missing_observation_is_unknown_never_zero():
    report, by_id = _report(_observation("repository_availability"))

    assert len(report["metrics"]) == len(_dictionary()["metrics"])
    missing = by_id["active_maintainers"]
    assert missing["status"] == "UNKNOWN"
    assert missing["value"] is None
    assert missing["display"] == "UNKNOWN"
    assert "not zero" in missing["reasons"][0]
    assert report["qualityState"] == "PARTIAL"


def test_confidence_and_freshness_levels():
    _, by_id = _report(
        _observation("repository_availability"),
        _observation("landing_activation", numerator=420, denominator=None),
        _observation("successful_migration", numerator=40, denominator=50,
                     coverage=0.4),
    )

    high = by_id["repository_availability"]
    assert (high["value"], high["display"]) == (9_990, "9990 bps")
    assert (high["freshness"], high["confidence"], high["status"]) == (
        "fresh", "high", "COMPLETE")
    # A partial coverage class caps confidence at medium.
    medium = by_id["landing_activation"]
    assert (medium["value"], medium["confidence"]) == (420, "medium")
    low = by_id["successful_migration"]
    assert (low["value"], low["confidence"], low["status"]) == (
        8_000, "low", "PARTIAL")


def test_stale_data_cut_lowers_confidence_and_says_why():
    _, by_id = _report(
        _observation("repository_availability"),
        generated="2026-09-10T02:00:00Z",
    )

    stale = by_id["repository_availability"]
    assert stale["freshness"] == "stale"
    assert stale["confidence"] == "low"
    assert stale["status"] == "PARTIAL"
    assert any("limit 168h" in reason for reason in stale["reasons"])


@pytest.mark.parametrize(("overrides", "reason"), [
    ({"collectedTotal": 30}, "reconciliation failed"),
    ({"sourceTotal": None}, "both required"),
    ({"continuationExhausted": False}, "not exhausted"),
    ({"dataCutAt": "2026-08-31T23:00:00Z"}, "precedes the end"),
    ({"numerator": None}, "numerator missing"),
    ({"denominator": None}, "denominator missing"),
])
def test_unreconciled_incomplete_or_early_cut_is_unknown(overrides, reason):
    _, by_id = _report(_observation("successful_migration", **overrides))

    result = by_id["successful_migration"]
    assert result["status"] == "UNKNOWN"
    assert result["value"] is None
    assert any(reason in item for item in result["reasons"])


def test_zero_denominator_rate_is_not_applicable():
    _, by_id = _report(_observation("gross_margin", numerator=0, denominator=0))

    assert by_id["gross_margin"]["value"] is None
    assert by_id["gross_margin"]["display"] == "n/a"
    assert by_id["gross_margin"]["status"] == "COMPLETE"


def test_small_segments_are_suppressed_with_a_complement():
    _, by_id = _report(_observation(
        "successful_migration", numerator=60, denominator=70,
        segments=[
            {"label": "github", "value": 40},
            {"label": "gitlab", "value": 16},
            {"label": "codeberg", "value": 4},
        ]))

    segments = {s["label"]: s for s in by_id["successful_migration"]["segments"]}
    assert segments["codeberg"] == {
        "label": "codeberg", "value": None, "suppressed": True}
    # Otherwise 60 - 40 - 16 would reveal the hidden cell.
    assert segments["gitlab"]["suppressed"] is True
    assert segments["github"] == {
        "label": "github", "value": 40, "suppressed": False}


def test_synthetic_metric_segments_are_not_suppressed():
    _, by_id = _report(_observation(
        "repository_availability",
        segments=[{"label": "clone", "value": 2}, {"label": "read", "value": 1}]))

    assert all(not s["suppressed"]
               for s in by_id["repository_availability"]["segments"])


def test_small_population_is_internal_only():
    _, by_id = _report(_observation(
        "paid_conversion", numerator=2, denominator=3, population=3))

    result = by_id["paid_conversion"]
    assert result["value"] == 6_666
    assert result["publishable"] is False
    assert any("internal only" in reason for reason in result["reasons"])


@pytest.mark.parametrize("row", [
    _observation("signup", email="person@example.com"),
    _observation("signup", accountId="acct_1"),
    _observation("signup", segments=[{"label": "a", "value": 20, "repository": "o/r"}]),
    _observation("signup", segments=[{"label": "person@example.com", "value": 20}]),
    _observation("signup", segments=[{"label": "203.0.113.9", "value": 20}]),
])
def test_identifier_fields_and_values_are_refused(row):
    with pytest.raises(OUTCOMES.OutcomeError, match="not accepted"):
        OUTCOMES.review_report(_observations(row), _dictionary())


def test_observations_must_cite_the_current_dictionary_version():
    observations = _observations(_observation("signup"))
    observations["dictionaryVersion"] = 1

    with pytest.raises(OUTCOMES.OutcomeError, match="different dictionary"):
        OUTCOMES.review_report(observations, _dictionary())


def test_unknown_or_duplicate_observations_are_refused():
    with pytest.raises(OUTCOMES.OutcomeError, match="unknown metric"):
        OUTCOMES.review_report(
            _observations(_observation("daily_active_users")), _dictionary())
    with pytest.raises(OUTCOMES.OutcomeError, match="duplicate"):
        OUTCOMES.review_report(
            _observations(_observation("signup"), _observation("signup")),
            _dictionary())


def test_weekly_review_covers_only_weekly_metrics():
    report, by_id = _report(cadence="weekly")
    dictionary = _dictionary()

    assert set(by_id) == {
        m["id"] for m in dictionary["metrics"] if m["cadence"] == "weekly"}
    assert "active_maintainers" not in by_id
    assert report["qualityState"] == "UNKNOWN"


def test_monthly_template_lists_every_metric_and_the_privacy_rules():
    text = REVIEW_PATH.read_text(encoding="utf-8")
    normalized = " ".join(text.split())
    dictionary = _dictionary()
    rows = re.findall(r"^\| [A-Za-z]+ \| `([a-z_]+)` \|", text, re.MULTILINE)

    assert tuple(rows) == tuple(m["id"] for m in dictionary["metrics"])
    assert "Dictionary: %s / version %d" % (
        dictionary["schema"], dictionary["version"]) in text
    for phrase in (
        "Measurement must never become required surveillance.",
        "A self-hosted node never has to report anything.",
        "the next smallest is suppressed too",
        "A metric without an observation is `UNKNOWN`, never zero.",
        "Decisions:", "Experiments:", "Roadmap changes:",
        "never quotes a person, repository, pull, ticket, or customer",
        "python3 tools/outcome_metrics.py validate-dictionary",
        "python3 tools/outcome_metrics.py review --input",
    ):
        assert " ".join(phrase.split()) in normalized, phrase
    for policy in dictionary["confidencePolicy"]:
        assert f"| `{policy}` |" in text
    assert "(outcome-review.md)" in (
        ROOT / "www" / "docs" / "README.md").read_text(encoding="utf-8")
    assert "(outcome-review.md)" in SCORECARD_PATH.read_text(encoding="utf-8")


def test_template_example_observation_is_a_valid_review_input():
    text = REVIEW_PATH.read_text(encoding="utf-8")
    example = json.loads(re.search(r"```json\n(.*?)```", text, re.S).group(1))

    report = OUTCOMES.review_report(example, _dictionary())
    availability = next(m for m in report["metrics"]
                        if m["metric"] == "repository_availability")
    assert availability["confidence"] == "high"
    assert availability["value"] == 9_977


def test_linked_evidence_files_exist():
    text = REVIEW_PATH.read_text(encoding="utf-8")
    linked = re.findall(r"`(app/tests/test_[a-z_]+\.py)`", text)

    assert "app/tests/test_outcome_metrics.py" in linked
    for path in linked:
        assert (ROOT / path).is_file(), path


def test_cli_validates_reports_and_fails_closed(tmp_path):
    valid = subprocess.run(
        [sys.executable, str(TOOL), "validate-dictionary"],
        capture_output=True, text=True, check=False)
    assert valid.returncode == 0, valid.stderr

    good = tmp_path / "month.json"
    good.write_text(json.dumps(_observations(_observation("signup", numerator=12))))
    report = subprocess.run(
        [sys.executable, str(TOOL), "review", "--input", str(good)],
        capture_output=True, text=True, check=False)
    assert report.returncode == 0, report.stderr
    assert json.loads(report.stdout)["dictionary"].endswith("/ version 2")

    bad = tmp_path / "bad.json"
    bad.write_text(json.dumps(_observations(_observation("signup", ip="198.51.100.4"))))
    refused = subprocess.run(
        [sys.executable, str(TOOL), "review", "--input", str(bad)],
        capture_output=True, text=True, check=False)
    assert refused.returncode == 2
    assert "not accepted" in refused.stderr
