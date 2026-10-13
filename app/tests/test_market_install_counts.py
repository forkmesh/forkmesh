#!/usr/bin/env python3
"""Anonymous marketplace install-count checks (stdlib only).

Loads the pure normalizer (_package_install_events) out of src/entry.py without
importing the Workers-only JS runtime, the same way test_install_diag.py does.
Verifies the unauthenticated /api/market/installs endpoint can't be used to
store identifying junk or markup, that a removal stops counting toward
"installs" without rewinding "downloads", and that the table stays bounded.
"""

import ast
import re
import sqlite3
from pathlib import Path


APP = Path(__file__).resolve().parents[1]
ENTRY = APP / "src" / "entry.py"
SCHEMA = APP / "src" / "schema.py"

ENTRY_SOURCE = ENTRY.read_text(encoding="utf-8")
APP_RUNTIME_SOURCE = ENTRY.with_name("app_runtime.py").read_text(encoding="utf-8")
SCHEMA_SOURCE = SCHEMA.read_text(encoding="utf-8")

_WANT_FUNCS = (
    "_sanitize_diag_field", "_marketplace_identifier", "_package_install_events",
    "_merge_member_installs",
)
_WANT_CONSTS = ("MAX_PACKAGE_INSTALL_EVENTS", "PACKAGE_MEMBER_INSTALL_SQL")

# The aggregation the GET handler runs, kept in one place so the behavioural
# tests below and the routing contract agree on it.
COUNTS_SQL = (
    "SELECT market, pkg, SUM(installed) AS installs, "
    "SUM(downloads) AS downloads FROM package_installs "
    "GROUP BY market, pkg"
)


def _load():
    tree = ast.parse(ENTRY_SOURCE, filename=str(ENTRY))
    body = []
    for node in tree.body:
        if isinstance(node, ast.FunctionDef) and node.name in _WANT_FUNCS:
            body.append(node)
        elif isinstance(node, ast.Assign) and any(
            isinstance(t, ast.Name) and t.id in _WANT_CONSTS for t in node.targets
        ):
            body.append(node)
    module = ast.fix_missing_locations(ast.Module(body=body, type_ignores=[]))
    namespace = {"re": re}
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


_NS = _load()
_events = _NS["_package_install_events"]
_merge = _NS["_merge_member_installs"]
MEMBER_SQL = dict(_NS["PACKAGE_MEMBER_INSTALL_SQL"])
MAX_EVENTS = _NS["MAX_PACKAGE_INSTALL_EVENTS"]


def _payload(packages=None, run="abc123"):
    if packages is None:
        packages = [{"id": "capture", "installed": True}]
    return {"run": run, "packages": packages}


def test_valid_report_is_normalized():
    assert _events(_payload()) == ("abc123", [("forkmesh", "capture", 1)])


def test_removal_is_recorded_as_a_cleared_state():
    run, events = _events(_payload([{"id": "winamp", "installed": False}]))
    assert run == "abc123"
    assert events == [("forkmesh", "winamp", 0)]


def test_installed_flag_accepts_the_json_and_string_spellings():
    for truthy in (1, True, "1", "true", "True"):
        _, events = _events(_payload([{"id": "capture", "installed": truthy}]))
        assert events == [("forkmesh", "capture", 1)]
    for falsy in (0, False, "0", "false", None, "yes"):
        _, events = _events(_payload([{"id": "capture", "installed": falsy}]))
        assert events == [("forkmesh", "capture", 0)]


def test_packages_are_deduplicated_and_ordered():
    run, events = _events(
        _payload(
            [
                {"id": "winamp", "installed": True},
                {"id": "capture", "installed": True},
                # A later report for the same package wins, so one run can
                # never be counted twice for the same package.
                {"id": "winamp", "installed": False},
            ]
        )
    )
    assert run == "abc123"
    assert events == [
        ("forkmesh", "capture", 1), ("forkmesh", "winamp", 0)
    ]


def test_marketplace_identity_is_stored_and_deduplicated_independently():
    _, events = _events(_payload([
        {"market": "forkmesh", "id": "capture", "installed": True},
        {"market": "community.example", "id": "capture", "installed": True},
    ]))
    assert events == [
        ("community.example", "capture", 1),
        ("forkmesh", "capture", 1),
    ]


def test_non_dict_missing_run_and_missing_packages_are_rejected():
    assert _events(None) is None
    assert _events("nope") is None
    assert _events({"packages": [{"id": "capture"}]}) is None
    assert _events(_payload(run="")) is None
    assert _events({"run": "abc123"}) is None
    assert _events({"run": "abc123", "packages": "capture"}) is None


def test_empty_and_unusable_events_are_rejected():
    assert _events(_payload([])) is None
    assert _events(_payload([{"installed": True}])) is None
    assert _events(_payload(["capture"])) is None
    assert _events(_payload([{"id": "", "installed": True}])) is None


def test_markup_and_control_characters_are_rejected_in_package_ids():
    assert _events(
        _payload([{"id": "<script>alert(1)</script>", "installed": True}])
    ) is None
    assert _events(
        _payload([{"id": "cap\nture\x00", "installed": True}])
    ) is None


def test_a_long_package_id_is_rejected():
    assert _events(
        _payload([{"id": "c" * 500, "installed": True}])
    ) is None


def test_a_flooded_report_is_capped():
    packages = [{"id": f"pkg{n}", "installed": True} for n in range(MAX_EVENTS * 3)]
    _, events = _events(_payload(packages))
    assert len(events) <= MAX_EVENTS


def test_the_run_id_is_clamped_like_the_installer_diagnostics_id():
    run, _ = _events(_payload(run="r" * 500))
    assert len(run) == 64


# --- static contracts -------------------------------------------------------


def test_the_table_is_keyed_per_run_marketplace_and_package():
    # One row per (run, marketplace, package) keeps this endpoint's row count
    # bounded by clients x catalogue size instead of by event volume.
    assert "CREATE TABLE IF NOT EXISTS package_installs" in SCHEMA_SOURCE
    assert "PRIMARY KEY (run, market, pkg)" in SCHEMA_SOURCE


def test_the_endpoint_is_routed_for_reads_and_reports():
    assert '"/api/market/installs", "/api/market/installs/"' in APP_RUNTIME_SOURCE
    assert "return await package_installs_handler(self.env, request)" in APP_RUNTIME_SOURCE


def _counting_database():
    """A real SQLite database running the shipped CREATE/UPSERT statements.

    D1 is SQLite, so the counter's semantics are checked by executing the exact
    statements the Worker sends rather than by matching their text.
    """
    create = re.search(
        r'"""(CREATE TABLE IF NOT EXISTS package_installs.*?)"""',
        SCHEMA_SOURCE,
        re.S,
    ).group(1)
    upsert = re.search(
        r'"""(INSERT INTO package_installs.*?)"""', ENTRY_SOURCE, re.S
    ).group(1)
    database = sqlite3.connect(":memory:")
    database.execute(create)

    def report(run, pkg, installed, ts=0, market="forkmesh"):
        database.execute(
            upsert, (run, market, pkg, ts, installed, installed)
        )

    def counts():
        return {
            (row[0], row[1]): {"installs": row[2], "downloads": row[3]}
            for row in database.execute(COUNTS_SQL).fetchall()
        }

    return report, counts


def test_a_repeated_report_of_an_unchanged_state_counts_once():
    # Clients re-send their whole package set, so an unchanged state must not
    # inflate the cumulative download counter shown on the Market.
    report, counts = _counting_database()
    for ts in range(5):
        report("runA", "capture", 1, ts)
    assert counts() == {("forkmesh", "capture"): {"installs": 1, "downloads": 1}}


def test_a_removal_clears_the_install_without_rewinding_downloads():
    report, counts = _counting_database()
    report("runA", "capture", 1, 10)
    report("runA", "capture", 0, 20)
    assert counts() == {("forkmesh", "capture"): {"installs": 0, "downloads": 1}}


def test_reinstalling_counts_a_second_download():
    report, counts = _counting_database()
    report("runA", "capture", 1, 10)
    report("runA", "capture", 0, 20)
    report("runA", "capture", 1, 30)
    assert counts() == {("forkmesh", "capture"): {"installs": 1, "downloads": 2}}


def test_each_client_is_counted_once_per_package():
    report, counts = _counting_database()
    report("runA", "capture", 1, 10)
    report("runB", "capture", 1, 20)
    report("runB", "capture", 1, 30)  # a repeat from the same client
    report("runB", "winamp", 0, 30)
    assert counts() == {
        ("forkmesh", "capture"): {"installs": 2, "downloads": 2},
        # Reported but never installed: the Market shows no line for this.
        ("forkmesh", "winamp"): {"installs": 0, "downloads": 0},
    }


def test_a_first_report_that_is_already_a_removal_counts_nothing():
    report, counts = _counting_database()
    report("runA", "winamp", 0, 10)
    assert counts() == {("forkmesh", "winamp"): {"installs": 0, "downloads": 0}}


def test_same_package_in_two_marketplaces_has_independent_counts():
    report, counts = _counting_database()
    report("runA", "capture", 1, 10, "forkmesh")
    report("runA", "capture", 1, 20, "community.example")
    assert counts() == {
        ("community.example", "capture"): {"installs": 1, "downloads": 1},
        ("forkmesh", "capture"): {"installs": 1, "downloads": 1},
    }


def test_the_handler_runs_the_aggregation_these_tests_exercise():
    # Pull the statement the read handler actually sends, so the behavioural
    # tests above cannot pass against an aggregation the Worker no longer runs.
    tree = ast.parse(ENTRY_SOURCE, filename=str(ENTRY))
    statements = [
        node.value
        for parent in ast.walk(tree)
        if isinstance(parent, ast.AsyncFunctionDef)
        and parent.name == "package_install_counts"
        for node in ast.walk(parent)
        if isinstance(node, ast.Constant) and isinstance(node.value, str)
    ]
    assert COUNTS_SQL in statements


def test_the_open_endpoint_prunes_itself():
    assert "DELETE FROM package_installs WHERE rowid NOT IN" in ENTRY_SOURCE
    assert "MAX_PACKAGE_INSTALL_ROWS" in ENTRY_SOURCE


def test_the_handler_stores_no_identifying_client_data():
    start = ENTRY_SOURCE.index("async def package_installs_handler")
    end = ENTRY_SOURCE.index("async def feedback_handler", start)
    handler = ENTRY_SOURCE[start:end]
    # The stored columns are exactly run/market/pkg/ts/installed/downloads.
    assert "(run, market, pkg, ts, installed, downloads)" in handler
    for identifying in ("cf-connecting-ip", "CF-Ray", "headers.get", "user-agent"):
        assert identifying not in handler
    assert "No accounts, IP addresses, or hostnames" in handler


def test_account_and_organization_installs_join_the_counts():
    counts = {"forkmesh": {"capture": {"installs": 2, "downloads": 5}}}
    merged = _merge(counts, {
        "accounts": [{"pkg": "tasks", "n": 7}, {"pkg": "<bad>", "n": 9},
                     {"pkg": "capture", "n": -3}],
        "organizations": [{"pkg": "crm", "n": 2}],
    })
    assert merged["forkmesh"] == {
        "capture": {"installs": 2, "downloads": 5, "accounts": 0,
                    "organizations": 0},
        "tasks": {"installs": 0, "downloads": 0, "accounts": 7,
                  "organizations": 0},
        "crm": {"installs": 0, "downloads": 0, "accounts": 0,
                "organizations": 2},
    }
    assert _merge({}, {"accounts": [], "organizations": []}) == {"forkmesh": {}}


def test_member_install_queries_count_installed_rows_per_package():
    database = sqlite3.connect(":memory:")
    database.execute(
        "CREATE TABLE account_package_installs (account_bi TEXT, package TEXT, "
        "installed INTEGER, PRIMARY KEY (account_bi, package))")
    database.execute(
        "CREATE TABLE org_plugin_installs (org_bi TEXT, plugin TEXT, "
        "PRIMARY KEY (org_bi, plugin))")
    database.executemany(
        "INSERT INTO account_package_installs VALUES (?,?,?)",
        [("a", "tasks", 1), ("b", "tasks", 1), ("c", "tasks", 0), ("a", "notes", 1)])
    database.executemany(
        "INSERT INTO org_plugin_installs VALUES (?,?)",
        [("o1", "crm"), ("o2", "crm"), ("o1", "marketing")])
    accounts = dict(database.execute(MEMBER_SQL["accounts"]).fetchall())
    organizations = dict(database.execute(MEMBER_SQL["organizations"]).fetchall())
    assert accounts == {"tasks": 2, "notes": 1}
    assert organizations == {"crm": 2, "marketing": 1}


def test_the_read_handler_merges_member_installs():
    start = ENTRY_SOURCE.index("async def package_installs_handler")
    handler = ENTRY_SOURCE[start:ENTRY_SOURCE.index("if method != \"POST\"", start)]
    assert "_merge_member_installs(" in handler
    assert "await package_member_installs(env)" in handler
