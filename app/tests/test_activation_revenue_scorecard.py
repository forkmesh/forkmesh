"""Activation-to-revenue metric dictionary and weekly scorecard contract.

``www/docs/outcome-metrics.json`` is the one definition of every
scorecard row and ``www/docs/activation-revenue-scorecard.md`` is the weekly
template. Every metric must reconcile to a source that exists, and no report
may treat a 100-row page as the whole population.
"""

import ast
import json
from pathlib import Path
import re
import sys


REPO = Path(__file__).resolve().parents[2]
DOCS = REPO / "www" / "docs"
DICTIONARY_PATH = DOCS / "outcome-metrics.json"
SCORECARD_PATH = DOCS / "activation-revenue-scorecard.md"
ATTRIBUTION_PATH = REPO / "app" / "src" / "attribution.py"
ATTRIBUTION_TESTS_PATH = REPO / "app" / "tests" / "test_attribution.py"

REQUIRED_METRICS = (
    "landing_activation",
    "anonymous_clone",
    "signup",
    "first_mirror",
    "contribution",
    "managed_service_qualification",
    "paid_conversion",
    "retention",
    "feature_adoption",
    "support_load",
    "infrastructure_cost",
    "gross_margin",
)
REQUIRED_FIELDS = (
    "id", "stage", "label", "kind", "unit", "definition", "numerator",
    "window", "grain", "source", "reconciliation", "pagination",
    "exclusions", "privacyBoundary", "owner", "availability",
)
PAGINATED_READERS = {"list_sessions", "list_events", "list_clone_snapshots"}
FUNCTIONS = (ast.FunctionDef, ast.AsyncFunctionDef)
# snake_case, kebab-case, SCREAMING_CASE, or camelCase names in a locator.
IDENTIFIER = re.compile(
    r"[A-Za-z][A-Za-z0-9]*(?:[_-][A-Za-z0-9]+)+|[a-z]+(?:[A-Z][a-z0-9]*)+")


def _dictionary():
    return json.loads(DICTIONARY_PATH.read_text(encoding="utf-8"))


def _normalized(path):
    return " ".join(path.read_text(encoding="utf-8").split())


def _calls(node, names):
    return [
        child for child in ast.walk(node)
        if isinstance(child, ast.Call)
        and isinstance(child.func, ast.Name)
        and child.func.id in names
    ]


def test_dictionary_defines_every_required_metric_once_in_funnel_order():
    dictionary = _dictionary()

    assert dictionary["schema"] == (
        "forkmesh-outcome-metric-dictionary-v2")
    assert dictionary["version"] == 2
    assert dictionary["reportingTimezone"] == "UTC"
    assert tuple(m["id"] for m in dictionary["metrics"]
                 if m["cadence"] == "weekly") == REQUIRED_METRICS
    assert any("never zero" in p for p in dictionary["principles"])
    for metric in dictionary["metrics"]:
        for field in REQUIRED_FIELDS:
            assert metric.get(field) not in (None, "", []), (
                metric["id"], field)
        assert "denominator" in metric, metric["id"]
        assert all(item.strip() for item in metric["exclusions"]), metric["id"]


def test_every_metric_reconciles_to_a_source_that_exists():
    for metric in _dictionary()["metrics"]:
        for source in metric["source"]:
            path = REPO / source["path"]
            assert path.is_file(), (metric["id"], source["path"])
            text = path.read_text(encoding="utf-8")
            locator = source["locator"]
            if path.suffix == ".md":
                headings = {
                    line.lstrip("#").strip()
                    for line in text.splitlines() if line.startswith("#")
                }
                for section in locator.split(" / "):
                    assert section in headings, (metric["id"], path, section)
                continue
            names = IDENTIFIER.findall(locator)
            assert names, (metric["id"], path, locator)
            for name in names:
                assert name in text, (metric["id"], path, name)


def test_no_metric_treats_a_page_size_as_the_population():
    for metric in _dictionary()["metrics"]:
        pagination = metric["pagination"]
        assert pagination["page_size_is_cap"] is False, metric["id"]
        assert pagination["reader"].strip(), metric["id"]
        assert pagination["continuation"].strip(), metric["id"]
        sources = {source["path"] for source in metric["source"]}
        if "app/src/attribution.py" in sources:
            # Anything read from the acquisition tables pages through a
            # cursor reader and must be collected to exhaustion.
            assert pagination["required"] is True, metric["id"]
            assert re.search(r"list_(sessions|events|clone_snapshots)",
                             pagination["reader"]), metric["id"]


def test_attribution_reports_never_stop_at_the_first_page():
    text = ATTRIBUTION_PATH.read_text(encoding="utf-8")
    tree = ast.parse(text)
    reports = set()
    for function in tree.body:
        if (not isinstance(function, FUNCTIONS)
                or function.name in PAGINATED_READERS | {"collect_all"}):
            continue
        direct = _calls(function, PAGINATED_READERS)
        if not direct:
            continue
        if function.name == "admin_list_handler":
            # A single cursor page returned verbatim, next_cursor included,
            # to a caller that keeps paging. It is not a report.
            source = ast.get_source_segment(text, function)
            assert "one cursor page" in source
            assert "json_response(page" in source
            continue
        reports.add(function.name)
        collected = {
            arg.id for call in _calls(function, {"collect_all"})
            for arg in call.args if isinstance(arg, ast.Name)
        }
        nested = [
            child for child in ast.walk(function)
            if isinstance(child, FUNCTIONS) and child is not function
        ]
        inside_page_readers = 0
        for reader in nested:
            reads = _calls(reader, PAGINATED_READERS)
            if reads:
                assert reader.name in collected, (function.name, reader.name)
                inside_page_readers += len(reads)
        # No reader call sits outside a page function handed to collect_all.
        assert inside_page_readers == len(direct), function.name
    assert {"channel_report", "clone_count_report"} <= reports

    snapshot = next(
        f for f in tree.body
        if isinstance(f, FUNCTIONS) and f.name == "snapshot_clone_counts")
    assert _calls(snapshot, {"collect_all"})
    assert "PAGE_SIZE = 100" in text


def test_weekly_template_has_one_row_per_metric_and_names_the_dictionary():
    text = SCORECARD_PATH.read_text(encoding="utf-8")
    normalized = _normalized(SCORECARD_PATH)
    rows = re.findall(r"^\| [A-Za-z]+ \| `([a-z_]+)` \|", text, re.MULTILINE)

    assert tuple(rows) == REQUIRED_METRICS
    dictionary = _dictionary()
    assert "%s / version %d" % (
        dictionary["schema"], dictionary["version"]) in text
    for phrase in (
        "Monday–Sunday week in UTC",
        "Pagination audit: pages collected=<n>",
        "all continuations exhausted=true|false",
        "A page size of 100 is a transport limit, never a population limit.",
        "never show the partial first page",
        "Never add `clone_url_copied` to that total.",
        "A missing input is a blocker, not a zero.",
        "Do not include raw email, account, repository, path, IP,",
    ):
        assert " ".join(phrase.split()) in normalized, phrase
    assert "(activation-revenue-scorecard.md)" in (
        DOCS / "README.md").read_text(encoding="utf-8")


def test_reconciliation_checklist_commands_exist():
    text = SCORECARD_PATH.read_text(encoding="utf-8")
    normalized = _normalized(SCORECARD_PATH)

    selection = re.search(r"-k '([^']+)'", normalized)
    assert selection, "pagination regression command is missing"
    defined = re.findall(
        r"^def (test_\w+)\(", ATTRIBUTION_TESTS_PATH.read_text(encoding="utf-8"),
        re.MULTILINE)
    expressions = selection.group(1).split(" or ")
    assert len(expressions) >= 3
    for expression in expressions:
        assert any(expression in name for name in defined), expression

    sys.path.insert(0, str(REPO / "tools"))
    import unit_economics

    parser = unit_economics._parser()
    commands = re.findall(r"python3 tools/unit_economics\.py ([a-z-]+)", text)
    assert set(commands) == {"validate-contract", "report"}
    parser.parse_args(["validate-contract"])
    parser.parse_args(["report", "--input", "period.json"])
