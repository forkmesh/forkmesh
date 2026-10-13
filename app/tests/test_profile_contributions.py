"""Pure contract tests for signed profile contribution snapshots."""

import ast
import asyncio
import base64
import hashlib
import json
import re
import sqlite3
import sys
from datetime import datetime, timezone
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, quote, urlparse

import pytest

from worker_test_helpers import json_from_request_double

# Allow ``import src.*`` regardless of the working directory pytest is run from
# (the collector may load this module from the repo root, where the worker
# package dir is not otherwise on sys.path).
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import src.contributions as contributions
from src.contributions import (
    MAX_CONTRIBUTION_COUNT,
    MAX_CONTRIBUTION_DAYS,
    MAX_CONTRIBUTION_EXTENSIONS,
    MAX_CONTRIBUTION_PAYLOAD_ENCODED,
    MAX_CONTRIBUTION_YEARS,
    contribution_date_range,
    decode_snapshot_payload,
    language_for_extension,
    snapshot_signature_canonical,
)
from src.schema import SCHEMA_STATEMENTS
from _maintenance_stubs import with_maintenance_off


ACTOR_KEY = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
ACTOR_KEY_B = base64.urlsafe_b64encode(b"\x01" * 32).decode("ascii").rstrip("=")
NOW_MS = int(datetime(2026, 7, 13, 12, tzinfo=timezone.utc).timestamp() * 1000)
ROOT = Path(__file__).resolve().parents[1]
CONTRIBUTION_MIGRATION = ROOT / "migrations" / "0036_profile_contributions.sql"
ENTRY = ROOT / "src" / "entry.py"
CATALOG_ROUTES = ROOT / "src" / "catalog_routes.py"
ROUTE_HANDLERS = ROOT / "src" / "route_handlers.py"
URLS = ROOT / "src" / "urls.py"

# RFC 8032 test key 1, used here with a fixed signature over the exact baseline
# contribution canonical bytes. Keeping one real vector in the ingestion tests
# prevents the test harness from quietly weakening the transport contract.
PUBLISHER_KEY = "11qYAYKxCrfVS_7TyWQHOg7hcvPapiMlrwIaaPcHURo"
PUBLISHER_SIGNATURE = (
    "x6n31efOer_g6dFF_llDy9AlbdSjPGgFRTr4d6knK2Obtz2F9RotLDSYUX22nYIV"
    "la04sY4sFObId2OFiso4BQ"
)
DEVICE_KEY = base64.urlsafe_b64encode(b"\x02" * 32).decode().rstrip("=")
UNKNOWN_KEY = base64.urlsafe_b64encode(b"\x03" * 32).decode().rstrip("=")

CONTRIBUTION_SCHEMA_COLUMNS = {
    "profile_contribution_receipts": [
        ("generation_bi", "TEXT", 0, None, 1),
        ("snapshot_hash", "TEXT", 1, None, 0),
        ("source_account_bi", "TEXT", 1, None, 0),
        ("source_repo_bi", "TEXT", 1, None, 0),
        ("captured_at", "INTEGER", 1, None, 0),
        ("head", "TEXT", 1, None, 0),
        ("day_rows", "INTEGER", 1, None, 0),
        ("language_rows", "INTEGER", 1, None, 0),
        ("created_at", "INTEGER", 1, None, 0),
    ],
    "profile_contribution_projects": [
        ("source_repo_bi", "TEXT", 0, None, 1),
        ("source_account_bi", "TEXT", 1, None, 0),
        ("owner_user_bi", "TEXT", 1, None, 0),
        ("project_bi", "TEXT", 1, None, 0),
        ("first_public_day", "TEXT", 1, None, 0),
        ("is_public", "INTEGER", 1, "1", 0),
        ("active_generation_bi", "TEXT", 0, None, 0),
        ("captured_at", "INTEGER", 1, "0", 0),
        ("verified_from", "TEXT", 0, None, 0),
        ("data", "TEXT", 1, None, 0),
    ],
    "profile_contribution_days": [
        ("generation_bi", "TEXT", 1, None, 1),
        ("subject_user_bi", "TEXT", 1, None, 2),
        ("source_account_bi", "TEXT", 1, None, 0),
        ("source_repo_bi", "TEXT", 1, None, 3),
        ("project_bi", "TEXT", 1, None, 0),
        ("day", "TEXT", 1, None, 4),
        ("commits", "INTEGER", 1, "0", 0),
        ("issues", "INTEGER", 1, "0", 0),
        ("pulls", "INTEGER", 1, "0", 0),
        ("reviews", "INTEGER", 1, "0", 0),
        ("captured_at", "INTEGER", 1, None, 0),
        ("data", "TEXT", 1, None, 0),
    ],
    "profile_contribution_languages": [
        ("generation_bi", "TEXT", 1, None, 1),
        ("owner_user_bi", "TEXT", 1, None, 2),
        ("source_repo_bi", "TEXT", 1, None, 3),
        ("project_bi", "TEXT", 1, None, 0),
        ("language", "TEXT", 1, None, 4),
        ("bytes", "INTEGER", 1, None, 0),
        ("files", "INTEGER", 1, None, 0),
        ("captured_at", "INTEGER", 1, None, 0),
    ],
}

CONTRIBUTION_SCHEMA_INDEX_COLUMNS = {
    ("profile_contribution_projects", ("owner_user_bi", "is_public")),
    ("profile_contribution_projects", ("project_bi", "is_public")),
    ("profile_contribution_days", ("subject_user_bi", "day")),
    ("profile_contribution_days", ("source_repo_bi", "generation_bi")),
    ("profile_contribution_languages", ("owner_user_bi", "generation_bi")),
}


def _contribution_schema_snapshot(script):
    connection = sqlite3.connect(":memory:")
    try:
        connection.executescript(script)
        connection.executescript(script)
        table_names = tuple(
            row[0]
            for row in connection.execute(
                "SELECT name FROM sqlite_master "
                "WHERE type = 'table' AND name LIKE 'profile_contribution_%' "
                "ORDER BY name"
            ).fetchall()
        )
        columns = {}
        indexes = {}
        for table in CONTRIBUTION_SCHEMA_COLUMNS:
            columns[table] = [
                (row[1], row[2], row[3], row[4], row[5])
                for row in connection.execute(
                    'PRAGMA table_info("%s")' % table
                ).fetchall()
            ]
            for row in connection.execute(
                'PRAGMA index_list("%s")' % table
            ).fetchall():
                if row[3] != "c":
                    continue
                name = row[1]
                index_columns = tuple(
                    item[2]
                    for item in connection.execute(
                        'PRAGMA index_info("%s")' % name
                    ).fetchall()
                )
                indexes[name] = (table, index_columns, row[2])
        return {
            "table_names": table_names,
            "columns": columns,
            "indexes": indexes,
        }
    finally:
        connection.close()


def _migration_contribution_schema_snapshot():
    assert CONTRIBUTION_MIGRATION.exists(), (
        "missing profile contribution migration 0036"
    )
    return _contribution_schema_snapshot(
        CONTRIBUTION_MIGRATION.read_text(encoding="utf-8")
    )


def _lazy_contribution_schema_snapshot():
    return _contribution_schema_snapshot(";\n".join(SCHEMA_STATEMENTS) + ";")


def test_contribution_schema_migration_and_lazy_definitions_match_contract():
    migration = _migration_contribution_schema_snapshot()
    lazy = _lazy_contribution_schema_snapshot()

    assert migration == lazy
    assert migration["table_names"] == tuple(sorted(CONTRIBUTION_SCHEMA_COLUMNS))
    assert migration["columns"] == CONTRIBUTION_SCHEMA_COLUMNS


def test_contribution_schema_has_exact_required_indexes():
    migration = _migration_contribution_schema_snapshot()
    lazy = _lazy_contribution_schema_snapshot()

    assert migration["indexes"] == lazy["indexes"]
    assert {
        (table, columns)
        for table, columns, unique in migration["indexes"].values()
        if unique == 0
    } == CONTRIBUTION_SCHEMA_INDEX_COLUMNS


def test_contribution_schema_excludes_raw_git_identity_columns():
    migration = _migration_contribution_schema_snapshot()
    lazy = _lazy_contribution_schema_snapshot()
    prohibited = {
        "author",
        "author_name",
        "author_email",
        "git_author_name",
        "git_author_email",
        "raw_author_name",
        "raw_author_email",
    }

    for snapshot in (migration, lazy):
        for columns in snapshot["columns"].values():
            names = {column[0] for column in columns}
            assert not names & prohibited
            assert all("email" not in name for name in names)


def _snapshot(**overrides):
    snapshot = {
        "version": 1,
        "capturedAt": NOW_MS,
        "head": "abc123",
        "branch": "main",
        "from": "2021-07-13",
        "through": "2026-07-13",
        "coverage": {
            "commits": "complete",
            "collaboration": "complete",
            "languages": "complete",
        },
        "days": [],
        "extensions": [],
        "fileCount": 0,
    }
    snapshot.update(overrides)
    return snapshot


def _encode(snapshot):
    return _encode_raw(json.dumps(
        snapshot, ensure_ascii=False, separators=(",", ":")
    ).encode("utf-8"))


def _encode_raw(raw):
    return base64.urlsafe_b64encode(raw).decode("ascii").rstrip("=")


def _decode(snapshot, now_ms=NOW_MS):
    return decode_snapshot_payload(_encode(snapshot), now_ms=now_ms)


def test_snapshot_contract_preserves_exact_bytes_and_digest():
    raw = (
        b'{"version":1,"capturedAt":1783942200000,'
        b'"head":"abc123","branch":"main","from":"2026-01-01",'
        b'"through":"2026-07-13","coverage":{"commits":"complete",'
        b'"collaboration":"complete","languages":"complete"},'
        b'"days":[["2026-07-13",'
        b'"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",3,1,0,1]],'
        b'"extensions":[["cpp",123456,18]],"fileCount":42}'
    )
    encoded = base64.urlsafe_b64encode(raw).decode("ascii").rstrip("=")

    snapshot, decoded, digest = decode_snapshot_payload(
        encoded, now_ms=1783942200000
    )

    assert decoded == raw
    assert digest == hashlib.sha256(raw).hexdigest()
    assert snapshot["days"] == [[
        "2026-07-13",
        ACTOR_KEY,
        3,
        1,
        0,
        1,
    ]]


def test_signature_canonical_bytes_are_exact():
    digest = "a" * 64
    assert snapshot_signature_canonical(
        "kaif", "forkmesh", "1783942200000", digest
    ) == (
        "forkmesh-profile-contribution-v1\n"
        "kaif\nforkmesh\n1783942200000\n" + digest
    ).encode("utf-8")


def test_valid_minimal_snapshot():
    snapshot, _, _ = _decode(_snapshot())
    assert snapshot == _snapshot()


def test_valid_representative_snapshot_with_sorted_rows():
    days = [
        ["2026-07-12", ACTOR_KEY_B, 1, 0, 0, 0],
        ["2026-07-13", ACTOR_KEY, 0, 2, 1, 3],
        ["2026-07-13", ACTOR_KEY_B, 4, 0, 0, 0],
    ]
    extensions = [["cpp", 50, 2], ["py", 100, 3]]
    snapshot, _, _ = _decode(_snapshot(
        days=days,
        extensions=extensions,
        fileCount=5,
        coverage={
            "commits": "partial",
            "collaboration": "complete",
            "languages": "complete",
        },
    ))
    assert snapshot["days"] == days
    assert snapshot["extensions"] == extensions


def test_public_constants_are_stable():
    assert MAX_CONTRIBUTION_PAYLOAD_ENCODED == 64 * 1024
    assert MAX_CONTRIBUTION_DAYS == 2048
    assert MAX_CONTRIBUTION_EXTENSIONS == 256
    assert MAX_CONTRIBUTION_COUNT == 1_000_000
    assert MAX_CONTRIBUTION_YEARS == 5


def test_contribution_date_range_defaults_to_365_inclusive_days():
    assert contribution_date_range({}, now_ms=NOW_MS) == (
        "2025-07-14",
        "2026-07-13",
    )


def test_contribution_date_range_clamps_derived_from_to_date_min():
    assert contribution_date_range(
        {"to": "0001-01-01"}, now_ms=NOW_MS
    ) == ("0001-01-01", "0001-01-01")


def test_strict_unpadded_base64url_and_encoded_size_cap():
    encoded = _encode(_snapshot())
    assert len(encoded) < MAX_CONTRIBUTION_PAYLOAD_ENCODED

    with pytest.raises(ValueError):
        decode_snapshot_payload(encoded + "=", now_ms=NOW_MS)
    with pytest.raises(ValueError):
        decode_snapshot_payload(" " + encoded, now_ms=NOW_MS)
    with pytest.raises(ValueError):
        decode_snapshot_payload("+/8", now_ms=NOW_MS)
    with pytest.raises(ValueError):
        decode_snapshot_payload(
            "A" * (MAX_CONTRIBUTION_PAYLOAD_ENCODED + 1),
            now_ms=NOW_MS,
        )


def test_encoded_payload_size_cap_is_inclusive():
    snapshot = _snapshot(
        head="h",
        branch="b",
        **{"from": "2026-07-13", "through": "2026-07-13"},
    )
    actor_keys = sorted(
        base64.urlsafe_b64encode(index.to_bytes(32, "big"))
        .decode("ascii")
        .rstrip("=")
        for index in range(1000)
    )
    target_raw_size = MAX_CONTRIBUTION_PAYLOAD_ENCODED * 3 // 4

    for actor_key in actor_keys:
        snapshot["days"].append(
            ["2026-07-13", actor_key, 0, 0, 0, 0]
        )
        if len(json.dumps(snapshot, separators=(",", ":")).encode()) > target_raw_size:
            snapshot["days"].pop()
            break

    raw = json.dumps(snapshot, separators=(",", ":")).encode()
    remaining = target_raw_size - len(raw)
    assert remaining <= 119
    snapshot["branch"] += "b" * remaining
    raw = json.dumps(snapshot, separators=(",", ":")).encode()
    encoded = _encode_raw(raw)
    assert len(encoded) == MAX_CONTRIBUTION_PAYLOAD_ENCODED

    parsed, decoded, _ = decode_snapshot_payload(encoded, now_ms=NOW_MS)
    assert decoded == raw
    assert len(parsed["days"]) < MAX_CONTRIBUTION_DAYS


@pytest.mark.parametrize(
    "raw",
    [
        b"\xff",
        b"not json",
        b"[]",
        b"null",
        b'{"version":1,"version":1}',
        b' {"version":1}',
    ],
)
def test_decoder_rejects_invalid_utf8_json_and_noncompact_objects(raw):
    with pytest.raises(ValueError):
        decode_snapshot_payload(_encode_raw(raw), now_ms=NOW_MS)


def test_decoder_rejects_excessively_nested_json_cleanly():
    raw = b"[" * 10_000 + b"0" + b"]" * 10_000
    assert len(_encode_raw(raw)) < MAX_CONTRIBUTION_PAYLOAD_ENCODED
    with pytest.raises(ValueError):
        decode_snapshot_payload(_encode_raw(raw), now_ms=NOW_MS)


@pytest.mark.parametrize(
    "change",
    [
        {"version": 2},
        {"version": True},
        {"v": 1},
        {"unexpected": "value"},
        {"capturedAt": True},
        {"capturedAt": -1},
        {"capturedAt": NOW_MS + 1},
        {"head": ""},
        {"head": "a" * 65},
        {"head": 123},
        {"branch": "\nmain"},
        {"branch": "b" * 121},
        {"from": "2026-7-01"},
        {"from": "2026-07-14"},
        {"through": "2026-07-32"},
        {"through": "2026-07-14"},
        {"coverage": []},
        {"coverage": {"commits": "complete"}},
        {"coverage": {
            "commits": "complete",
            "collaboration": "complete",
            "languages": "unknown",
        }},
        {"coverage": {
            "commits": "complete",
            "collaboration": "complete",
            "languages": [],
        }},
        {"coverage": {
            "commits": "complete",
            "collaboration": "complete",
            "languages": "complete",
            "from": "2021-07-13",
        }},
        {"days": {}},
        {"extensions": {}},
        {"fileCount": True},
        {"fileCount": -1},
        {"fileCount": MAX_CONTRIBUTION_COUNT + 1},
    ],
)
def test_snapshot_rejection_matrix(change):
    snapshot = _snapshot()
    snapshot.update(change)
    with pytest.raises(ValueError):
        _decode(snapshot)


def test_top_level_keys_are_exactly_the_contract_keys():
    snapshot = _snapshot()
    snapshot.pop("fileCount")
    with pytest.raises(ValueError):
        _decode(snapshot)


@pytest.mark.parametrize("field", ["head", "branch"])
@pytest.mark.parametrize("unsafe", ["\u0085", "\u200b", "\u2028", "\u2029"])
def test_head_and_branch_reject_unicode_controls_and_separators(field, unsafe):
    with pytest.raises(ValueError):
        _decode(_snapshot(**{field: "safe" + unsafe + "text"}))


def test_head_and_branch_allow_normal_unicode_letters():
    snapshot, _, _ = _decode(_snapshot(head="révision", branch="功能/分支"))
    assert snapshot["head"] == "révision"
    assert snapshot["branch"] == "功能/分支"


@pytest.mark.parametrize(
    "days",
    [
        [
            ["2026-07-13", ACTOR_KEY_B, 0, 0, 0, 0],
            ["2026-07-13", ACTOR_KEY, 0, 0, 0, 0],
        ],
        [
            ["2026-07-13", ACTOR_KEY, 0, 0, 0, 0],
            ["2026-07-13", ACTOR_KEY, 0, 0, 0, 0],
        ],
        [
            ["2026-07-13", ACTOR_KEY, 0, 0, 0, 0],
            ["2026-07-12", ACTOR_KEY, 0, 0, 0, 0],
        ],
        [["2021-07-12", ACTOR_KEY, 0, 0, 0, 0]],
        [["2026-07-14", ACTOR_KEY, 0, 0, 0, 0]],
        [["2026-07-13", ACTOR_KEY + "=", 0, 0, 0, 0]],
        [["2026-07-13", ACTOR_KEY[:-1], 0, 0, 0, 0]],
        [["2026-07-13", "!" + ACTOR_KEY[1:], 0, 0, 0, 0]],
        [["2026-07-13", ACTOR_KEY, 0, 0, 0]],
        [["2026-07-13", ACTOR_KEY, 0, 0, 0, 0, 0]],
        [{"date": "2026-07-13"}],
    ],
)
def test_day_rows_must_be_sorted_unique_bounded_and_exact(days):
    with pytest.raises(ValueError):
        _decode(_snapshot(days=days))


@pytest.mark.parametrize("value", [True, -1, MAX_CONTRIBUTION_COUNT + 1, 1.0])
def test_day_counts_are_bounded_integers_not_booleans(value):
    row = ["2026-07-13", ACTOR_KEY, value, 0, 0, 0]
    with pytest.raises(ValueError):
        _decode(_snapshot(days=[row]))


def test_day_count_cap_is_inclusive():
    row = ["2026-07-13", ACTOR_KEY, MAX_CONTRIBUTION_COUNT, 0, 0, 0]
    snapshot, _, _ = _decode(_snapshot(days=[row]))
    assert snapshot["days"] == [row]


@pytest.mark.parametrize(
    "extensions",
    [
        [["py", 1, 1], ["CPP", 1, 1]],
        [["py", 1, 1], [".PY", 1, 1]],
        [["", 1, 1]],
        [[".", 1, 1]],
        [["py", 1]],
        [["py", 1, 1, 0]],
        [{"extension": "py"}],
    ],
)
def test_extension_rows_must_be_sorted_unique_and_exact(extensions):
    with pytest.raises(ValueError):
        _decode(_snapshot(extensions=extensions))


@pytest.mark.parametrize("value", [True, -1, MAX_CONTRIBUTION_COUNT + 1, 1.0])
def test_extension_counts_are_bounded_integers_not_booleans(value):
    with pytest.raises(ValueError):
        _decode(_snapshot(extensions=[["py", value, 0]]))


def test_extension_and_file_count_caps_are_inclusive():
    extensions = [
        ["e%03d" % index, MAX_CONTRIBUTION_COUNT, MAX_CONTRIBUTION_COUNT]
        for index in range(MAX_CONTRIBUTION_EXTENSIONS)
    ]
    snapshot, _, _ = _decode(_snapshot(
        extensions=extensions,
        fileCount=MAX_CONTRIBUTION_COUNT,
    ))
    assert len(snapshot["extensions"]) == MAX_CONTRIBUTION_EXTENSIONS

    too_many = extensions + [["e256", 0, 0]]
    with pytest.raises(ValueError):
        _decode(_snapshot(extensions=too_many))


def test_day_row_limit_guard_is_exercised_directly(monkeypatch):
    monkeypatch.setattr(contributions, "MAX_CONTRIBUTION_DAYS", 1)
    rows = [
        ["2026-07-13", ACTOR_KEY, 0, 0, 0, 0],
        ["2026-07-13", ACTOR_KEY_B, 0, 0, 0, 0],
    ]
    with pytest.raises(ValueError, match="days must be a bounded array"):
        _decode(_snapshot(days=rows))


def test_five_year_snapshot_boundary_including_leap_day():
    _decode(_snapshot(**{"from": "2020-02-29", "through": "2025-02-28"}))
    with pytest.raises(ValueError):
        _decode(_snapshot(**{"from": "2020-02-29", "through": "2025-03-01"}))


def test_five_year_limit_clamps_safely_at_date_max():
    captured_date = datetime(9999, 1, 2, tzinfo=timezone.utc)
    epoch = datetime(1970, 1, 1, tzinfo=timezone.utc)
    captured_ms = (captured_date - epoch).days * 86_400_000
    snapshot, _, _ = _decode(
        _snapshot(
            capturedAt=captured_ms,
            **{"from": "9999-01-01", "through": "9999-01-02"},
        ),
        now_ms=captured_ms,
    )
    assert snapshot["through"] == "9999-01-02"


def test_day_dates_accept_both_snapshot_boundaries():
    rows = [
        ["2021-07-13", ACTOR_KEY, 1, 0, 0, 0],
        ["2026-07-13", ACTOR_KEY, 1, 0, 0, 0],
    ]
    snapshot, _, _ = _decode(_snapshot(days=rows))
    assert snapshot["days"] == rows


@pytest.mark.parametrize(
    ("query", "expected"),
    [
        ({"from": "2026-01-01", "to": "2026-01-01"},
         ("2026-01-01", "2026-01-01")),
        ({"from": "2025-01-01", "to": "2026-01-01"},
         ("2025-01-01", "2026-01-01")),
        ({"to": "2026-01-01"}, ("2025-01-02", "2026-01-01")),
        ({"from": "2026-07-01"}, ("2026-07-01", "2026-07-13")),
    ],
)
def test_contribution_date_range_valid_boundaries(query, expected):
    assert contribution_date_range(query, now_ms=NOW_MS) == expected


@pytest.mark.parametrize(
    "query",
    [
        {"from": "2024-12-31", "to": "2026-01-01"},
        {"from": "2026-01-02", "to": "2026-01-01"},
        {"from": "2026-1-01", "to": "2026-01-02"},
        {"from": True, "to": "2026-01-02"},
        [],
    ],
)
def test_contribution_date_range_rejects_invalid_ranges(query):
    with pytest.raises(ValueError):
        contribution_date_range(query, now_ms=NOW_MS)


def test_signature_inputs_reject_ambiguous_canonical_values():
    digest = "a" * 64
    bad_inputs = [
        ("", "repo", "1", digest),
        ("owner\nother", "repo", "1", digest),
        ("owner", "repo/name", "1", digest),
        ("owner", "repo", "01", digest),
        ("owner", "repo", 1, digest),
        ("owner", "repo", "1", "A" * 64),
        ("owner", "repo", "1", "a" * 63),
    ]
    for args in bad_inputs:
        with pytest.raises(ValueError):
            snapshot_signature_canonical(*args)


@pytest.mark.parametrize(
    ("extension", "expected"),
    [
        ("c", ("C", "#555555")),
        (".CPP", ("C++", "#f34b7d")),
        ("cs", ("C#", "#178600")),
        ("go", ("Go", "#00ADD8")),
        ("html", ("HTML", "#e34c26")),
        ("css", ("CSS", "#563d7c")),
        ("java", ("Java", "#b07219")),
        ("jsx", ("JavaScript", "#f1e05a")),
        ("json", ("JSON", "#959595")),
        ("kt", ("Kotlin", "#A97BFF")),
        ("markdown", ("Markdown", "#083fa1")),
        ("php", ("PHP", "#4F5D95")),
        ("py", ("Python", "#3572A5")),
        ("rb", ("Ruby", "#701516")),
        ("rs", ("Rust", "#dea584")),
        ("bash", ("Shell", "#89e051")),
        ("sql", ("SQL", "#e38c00")),
        ("swift", ("Swift", "#F05138")),
        ("tsx", ("TypeScript", "#3178c6")),
        ("yml", ("YAML", "#cb171e")),
        ("unknown", ("Other", "#8b949e")),
    ],
)
def test_language_buckets_are_stable(extension, expected):
    assert language_for_extension(extension) == expected


def _load_entry_contribution_functions(extra_globals):
    source = (
        ROUTE_HANDLERS.read_text(encoding="utf-8")
        + "\n\n\n"
        + (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "node_events_runtime.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)
    )
    tree = ast.parse(source, filename=str(ENTRY))
    selected = [
        node
        for node in tree.body
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and (node.name.startswith("_contribution_")
                 or node.name in ("_delete_repo_scoped_state",
                                  "_repo_scoped_state_statements")))
        # The table-driven tail of the repo purge, so the cleanup test runs
        # the real list rather than a stub of it.
        or (isinstance(node, ast.Assign)
            and any(isinstance(target, ast.Name)
                    and target.id == "_REPO_SCOPED_TABLES"
                    for target in node.targets))
    ]
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra_globals)
    namespace.setdefault("bounded_json_request", json_from_request_double)
    exec(compile(module, str(ENTRY), "exec"), with_maintenance_off(namespace))
    return namespace


def _test_signature(owner, repo, updated_at, encoded):
    raw = base64.urlsafe_b64decode(encoded + "=" * ((4 - len(encoded) % 4) % 4))
    digest = hashlib.sha256(raw).hexdigest()
    canonical = snapshot_signature_canonical(owner, repo, updated_at, digest)
    return "test-" + base64.urlsafe_b64encode(
        hashlib.sha256(canonical).digest()
    ).decode().rstrip("=")


def _ingest_data(snapshot, signature=None):
    encoded = _encode(snapshot)
    return {
        "contributionPayload": encoded,
        "contributionSig": signature or _test_signature(
            "alice-node", "forkmesh", str(snapshot["capturedAt"]), encoded
        ),
    }


def _catalog_record(snapshot, **overrides):
    record = {
        "owner": "alice-node",
        "name": "forkmesh",
        "visibility": "public",
        "rootCommit": "stable-root",
        "commit": snapshot["head"],
        "branch": snapshot["branch"],
        "updatedAt": str(snapshot["capturedAt"]),
    }
    record.update(overrides)
    return record


def _blind(value):
    return hashlib.sha256(("blind:" + value.strip().lower()).encode()).hexdigest()


class _IngestHarness:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(CONTRIBUTION_MIGRATION.read_text(encoding="utf-8"))
        self.db.executescript(
            """
            CREATE TABLE nodes (
                node_bi TEXT PRIMARY KEY,
                user_bi TEXT,
                pubkey TEXT NOT NULL
            );
            CREATE TABLE account_devices (
                device_bi TEXT PRIMARY KEY,
                account_bi TEXT NOT NULL,
                pubkey TEXT NOT NULL,
                capabilities TEXT NOT NULL DEFAULT '',
                enabled INTEGER NOT NULL,
                revoked_at INTEGER NOT NULL
            );
            CREATE TABLE repositories (
                key_bi TEXT PRIMARY KEY,
                owner_bi TEXT NOT NULL,
                data TEXT NOT NULL,
                is_private INTEGER NOT NULL DEFAULT 0
            );
            """
        )
        self.statements = []
        self.select_statements = []
        self.project_read_hook = None

        async def d1_first(_env, sql, *args):
            self.select_statements.append((" ".join(sql.split()), args))
            row = self.db.execute(sql, args).fetchone()
            if (self.project_read_hook is not None
                    and "FROM profile_contribution_projects" in sql):
                await self.project_read_hook(sql, args)
            return dict(row) if row is not None else None

        async def d1_all(_env, sql, *args):
            self.select_statements.append((" ".join(sql.split()), args))
            return [dict(row) for row in self.db.execute(sql, args).fetchall()]

        async def d1_run(_env, sql, *args):
            self.statements.append((" ".join(sql.split()), args))
            try:
                self.db.execute(sql, args)
            except sqlite3.OperationalError as error:
                if "no such table" not in str(error):
                    raise
            self.db.commit()

        async def d1_run_batch(_env, statements):
            for sql, args in statements:
                await d1_run(_env, sql, *args)

        async def blind_index(_env, value):
            return _blind(value)

        async def encrypt_row(_env, value):
            return "enc:" + json.dumps(value, sort_keys=True, separators=(",", ":"))

        async def decrypt_row(_env, value):
            if not value or not value.startswith("enc:"):
                return None
            return json.loads(value[4:])

        async def ed25519_verify(pubkey, signature, canonical):
            if pubkey != PUBLISHER_KEY:
                return False
            baseline = _snapshot()
            baseline_encoded = _encode(baseline)
            baseline_canonical = snapshot_signature_canonical(
                "alice-node",
                "forkmesh",
                str(baseline["capturedAt"]),
                hashlib.sha256(base64.urlsafe_b64decode(
                    baseline_encoded
                    + "=" * ((4 - len(baseline_encoded) % 4) % 4)
                )).hexdigest(),
            )
            if canonical == baseline_canonical:
                return signature == PUBLISHER_SIGNATURE
            expected = "test-" + base64.urlsafe_b64encode(
                hashlib.sha256(canonical).digest()
            ).decode().rstrip("=")
            return signature == expected

        async def edge_cache_delete(_key):
            return None

        class _Date:
            @staticmethod
            def now():
                return NOW_MS + 86_400_000

        import src.catalog as catalog

        self.namespace = _load_entry_contribution_functions({
            "Date": _Date,
            "contributions": contributions,
            "safe_contribution_transport": getattr(
                catalog, "safe_contribution_transport", None
            ),
            "clean_string": lambda value, maximum=240: (
                value.strip()[:maximum] if isinstance(value, str) else ""
            ),
            "blind_index": blind_index,
            "d1_first": d1_first,
            "d1_all": d1_all,
            "d1_run": d1_run,
            "d1_run_batch": d1_run_batch,
            "encrypt_row": encrypt_row,
            "decrypt_row": decrypt_row,
            "ed25519_verify": ed25519_verify,
            "hashlib": hashlib,
            "json": json,
            "time": __import__("time"),
            "asyncio": asyncio,
            "quote": quote,
            "edge_cache_delete": edge_cache_delete,
            "PROFILE_CONTRIBUTION_CACHE_PREFIX": (
                "https://forkmesh.internal/api/profile-contributions/"
            ),
            "PROFILE_CONTRIBUTION_CACHE_TTL": 30,
            "_AGENT_PUSH_DIGESTS": {},
        })
        self.env = SimpleNamespace()

    def close(self):
        self.db.close()

    def add_node(self, node_bi, user_bi, pubkey):
        self.db.execute(
            "INSERT INTO nodes (node_bi, user_bi, pubkey) VALUES (?,?,?)",
            (node_bi, user_bi, pubkey),
        )
        self.db.commit()

    def add_device(
            self, account_bi, pubkey, *, proven=True, enabled=1, revoked_at=0):
        self.db.execute(
            "INSERT INTO account_devices "
            "(device_bi, account_bi, pubkey, capabilities, enabled, revoked_at) "
            "VALUES (?,?,?,?,?,?)",
            (_blind(account_bi + ":" + pubkey), account_bi, pubkey,
             "contribution_key_proof" if proven else "browse",
             enabled, revoked_at),
        )
        self.db.commit()

    def publish(self, record, account_rec=None):
        return asyncio.run(self.namespace["_contribution_write_catalog_state"](
            self.env,
            record,
            _blind("alice-node"),
            account_rec or {
                "name": "alice-node",
                "kind": "node",
                "pubkey": PUBLISHER_KEY,
            },
            _blind(record["owner"] + "/" + record["name"]),
            "encrypted-catalog-record",
        ))

    def run_ingest(self, data, record, account_rec=None, *, publish=True):
        if publish and "_contribution_write_catalog_state" in self.namespace:
            self.publish(record, account_rec)
        return asyncio.run(self.ingest_async(data, record, account_rec))

    async def ingest_async(self, data, record, account_rec=None):
        return await self.namespace["_contribution_ingest_snapshot"](
            self.env,
            data,
            record,
            _blind("alice-node"),
            account_rec or {
                "name": "alice-node",
                "kind": "node",
                "pubkey": PUBLISHER_KEY,
            },
            _blind("alice-node/forkmesh"),
        )


@pytest.fixture
def ingest_harness():
    harness = _IngestHarness()
    try:
        yield harness
    finally:
        harness.close()


def test_ingest_accepts_known_ed25519_vector_and_primary_generation(
        ingest_harness):
    ingest_harness.add_node(
        _blind("alice-node"), _blind("alice-user"), PUBLISHER_KEY
    )
    snapshot = _snapshot()

    result = ingest_harness.run_ingest(
        _ingest_data(snapshot, signature=PUBLISHER_SIGNATURE),
        _catalog_record(snapshot),
    )

    assert result == {"accepted": True, "warning": ""}
    project = dict(ingest_harness.db.execute(
        "SELECT * FROM profile_contribution_projects"
    ).fetchone())
    assert project["owner_user_bi"] == _blind("alice-user")
    assert project["active_generation_bi"]
    assert project["first_public_day"] == "2026-07-14"
    receipt = dict(ingest_harness.db.execute(
        "SELECT * FROM profile_contribution_receipts"
    ).fetchone())
    assert receipt["snapshot_hash"] == hashlib.sha256(
        base64.urlsafe_b64decode(
            _encode(snapshot) + "=" * ((4 - len(_encode(snapshot)) % 4) % 4)
        )
    ).hexdigest()


def test_ingest_groups_linked_node_and_enabled_device_actors_and_omits_unknown(
        ingest_harness):
    alice_user = _blind("alice-user")
    bob_user = _blind("bob-user")
    ingest_harness.add_node(_blind("alice-node"), alice_user, PUBLISHER_KEY)
    ingest_harness.add_device(bob_user, DEVICE_KEY)
    rows = sorted([
        ["2026-07-13", PUBLISHER_KEY, 3, 0, 0, 1],
        ["2026-07-13", DEVICE_KEY, 0, 2, 1, 0],
        ["2026-07-13", UNKNOWN_KEY, 9, 9, 9, 9],
    ], key=lambda row: (row[0], row[1]))
    snapshot = _snapshot(
        days=rows,
        extensions=[["cpp", 120, 2], ["hpp", 30, 1], ["py", 50, 1]],
        fileCount=4,
    )

    result = ingest_harness.run_ingest(
        _ingest_data(snapshot), _catalog_record(snapshot)
    )

    assert result["accepted"] is True
    days = [dict(row) for row in ingest_harness.db.execute(
        "SELECT subject_user_bi, commits, issues, pulls, reviews "
        "FROM profile_contribution_days ORDER BY subject_user_bi"
    ).fetchall()]
    assert days == sorted([
        {
            "subject_user_bi": alice_user,
            "commits": 3,
            "issues": 0,
            "pulls": 0,
            "reviews": 1,
        },
        {
            "subject_user_bi": bob_user,
            "commits": 0,
            "issues": 2,
            "pulls": 1,
            "reviews": 0,
        },
    ], key=lambda row: row["subject_user_bi"])
    languages = [dict(row) for row in ingest_harness.db.execute(
        "SELECT language, bytes, files FROM profile_contribution_languages "
        "ORDER BY language"
    ).fetchall()]
    assert languages == [
        {"language": "C++", "bytes": 150, "files": 3},
        {"language": "Python", "bytes": 50, "files": 1},
    ]
    update_index = next(
        index for index, (sql, _args) in enumerate(ingest_harness.statements)
        if (sql.startswith("UPDATE profile_contribution_projects")
            and "active_generation_bi=?" in sql)
    )
    assert all(
        index < update_index
        for index, (sql, _args) in enumerate(ingest_harness.statements)
        if sql.startswith("INSERT OR IGNORE INTO profile_contribution_days")
        or sql.startswith("INSERT OR IGNORE INTO profile_contribution_languages")
        or sql.startswith("INSERT OR IGNORE INTO profile_contribution_receipts")
    )


def test_ingest_rejects_disabled_and_revoked_device_actor_keys(ingest_harness):
    ingest_harness.add_node(
        _blind("alice-node"), _blind("alice-user"), PUBLISHER_KEY
    )
    ingest_harness.add_device(_blind("disabled"), DEVICE_KEY, enabled=0)
    ingest_harness.add_device(_blind("revoked"), UNKNOWN_KEY, revoked_at=1)

    mapping = asyncio.run(
        ingest_harness.namespace["_contribution_actor_user_bis"](
            ingest_harness.env, [DEVICE_KEY, UNKNOWN_KEY]
        )
    )

    assert mapping == {}


def test_ingest_is_idempotent_ignores_older_rejects_equal_conflict_and_activates_newer(
        ingest_harness):
    ingest_harness.add_node(
        _blind("alice-node"), _blind("alice-user"), PUBLISHER_KEY
    )
    first = _snapshot(days=[["2026-07-13", PUBLISHER_KEY, 1, 0, 0, 0]])
    assert ingest_harness.run_ingest(
        _ingest_data(first), _catalog_record(first)
    )["accepted"] is True
    first_generation = ingest_harness.db.execute(
        "SELECT active_generation_bi FROM profile_contribution_projects"
    ).fetchone()[0]
    statement_count = len(ingest_harness.statements)
    select_count = len(ingest_harness.select_statements)

    same = ingest_harness.run_ingest(_ingest_data(first), _catalog_record(first))
    assert same == {"accepted": True, "warning": ""}
    # Re-attesting an unchanged snapshot (the drifted-pin republish hot path)
    # takes the fast path: it recognizes the already-active generation from a
    # single lookup and skips the signature verify, the staging batch, and the
    # prune entirely — the Worker-CPU (Cloudflare 1102) optimization. So the
    # duplicate ingest writes none of the expensive generation-staging rows
    # (json_each fan-outs, receipts, prune deletes); only the unrelated
    # catalog-state upserts run, and it stays accepted without duplicating the
    # receipt.
    duplicate_writes = ingest_harness.statements[statement_count:]
    assert not any(
        "json_each" in sql
        or "profile_contribution_receipts" in sql
        or "DELETE FROM profile_contribution" in sql
        for sql, _args in duplicate_writes
    )
    assert len(ingest_harness.select_statements) > select_count
    assert ingest_harness.db.execute(
        "SELECT COUNT(*) FROM profile_contribution_receipts"
    ).fetchone()[0] == 1

    older = _snapshot(
        capturedAt=NOW_MS - 1000,
        days=[["2026-07-13", PUBLISHER_KEY, 2, 0, 0, 0]],
    )
    older_result = ingest_harness.run_ingest(
        _ingest_data(older), _catalog_record(older)
    )
    assert older_result == {
        "accepted": False,
        "warning": "contribution_snapshot_older",
    }

    conflict = _snapshot(
        head="def456",
        days=[["2026-07-13", PUBLISHER_KEY, 2, 0, 0, 0]],
    )
    conflict_result = ingest_harness.run_ingest(
        _ingest_data(conflict), _catalog_record(conflict)
    )
    assert conflict_result == {
        "accepted": False,
        "warning": "contribution_snapshot_conflict",
    }

    newer = _snapshot(
        capturedAt=NOW_MS + 1000,
        head="fedcba",
        days=[["2026-07-13", PUBLISHER_KEY, 7, 0, 0, 0]],
    )
    newer_result = ingest_harness.run_ingest(
        _ingest_data(newer), _catalog_record(newer)
    )
    assert newer_result == {"accepted": True, "warning": ""}
    project = ingest_harness.db.execute(
        "SELECT active_generation_bi, captured_at FROM profile_contribution_projects"
    ).fetchone()
    assert project[0] != first_generation
    assert project[1] == NOW_MS + 1000
    assert [row[0] for row in ingest_harness.db.execute(
        "SELECT commits FROM profile_contribution_days"
    ).fetchall()] == [7]
    assert ingest_harness.db.execute(
        "SELECT COUNT(*) FROM profile_contribution_receipts"
    ).fetchone()[0] == 1


@pytest.mark.parametrize(
    ("data_change", "record_change", "warning"),
    [
        ({"contributionPayload": "A" * (64 * 1024 + 1)}, {},
         "contribution_payload_too_large"),
        ({"contributionSig": "bad-signature"}, {},
         "invalid_contribution_signature"),
        ({}, {"commit": "different-head"}, "contribution_head_mismatch"),
        ({}, {"branch": "other"}, "contribution_branch_mismatch"),
        ({}, {"updatedAt": str(NOW_MS + 1)}, "contribution_time_mismatch"),
    ],
)
def test_ingest_returns_bounded_warning_without_writing_invalid_generation(
        ingest_harness, data_change, record_change, warning):
    ingest_harness.add_node(
        _blind("alice-node"), _blind("alice-user"), PUBLISHER_KEY
    )
    snapshot = _snapshot()
    data = _ingest_data(snapshot, signature=PUBLISHER_SIGNATURE)
    data.update(data_change)
    record = _catalog_record(snapshot)
    record.update(record_change)

    result = ingest_harness.run_ingest(data, record)

    assert result == {"accepted": False, "warning": warning}
    assert ingest_harness.db.execute(
        "SELECT COUNT(*) FROM profile_contribution_receipts"
    ).fetchone()[0] == 0


def test_ingest_mirror_records_share_root_commit_project_key(ingest_harness):
    helper = ingest_harness.namespace["_contribution_project_bi"]
    first = _catalog_record(_snapshot(), rootCommit="ABCDEF")
    mirror = _catalog_record(
        _snapshot(), owner="mirror-node", name="copy", rootCommit="abcdef"
    )

    first_bi = asyncio.run(helper(ingest_harness.env, first))
    mirror_bi = asyncio.run(helper(ingest_harness.env, mirror))

    assert first_bi == mirror_bi == _blind("profile-project:abcdef")


def test_ingest_repo_cleanup_removes_all_contribution_tables(ingest_harness):
    ingest_harness.add_node(
        _blind("alice-node"), _blind("alice-user"), PUBLISHER_KEY
    )
    snapshot = _snapshot(days=[["2026-07-13", PUBLISHER_KEY, 1, 0, 0, 0]])
    ingest_harness.run_ingest(_ingest_data(snapshot), _catalog_record(snapshot))
    repo_bi = _blind("alice-node/forkmesh")

    asyncio.run(ingest_harness.namespace["_delete_repo_scoped_state"](
        ingest_harness.env, repo_bi
    ))

    for table in (
        "profile_contribution_receipts",
        "profile_contribution_projects",
        "profile_contribution_days",
        "profile_contribution_languages",
    ):
        assert ingest_harness.db.execute(
            "SELECT COUNT(*) FROM " + table
        ).fetchone()[0] == 0


def test_ingest_catalog_handler_keeps_repository_write_on_optional_failure():
    source = (
        CATALOG_ROUTES.read_text(encoding="utf-8")
        + "\n\n\n"
        + ENTRY.read_text(encoding="utf-8")
    )
    tree = ast.parse(source, filename=str(ENTRY))
    nodes = [
        item for item in tree.body
        if isinstance(item, (ast.FunctionDef, ast.AsyncFunctionDef))
        and item.name in {
            "catalog_handler",
            "catalog_mirror_update_needed",
            "catalog_push_observed",
            "_repo_publication_authority",
        }
    ]
    assert {item.name for item in nodes} == {
        "catalog_handler",
        "catalog_mirror_update_needed",
        "catalog_push_observed",
        "_repo_publication_authority",
    }
    module = ast.fix_missing_locations(ast.Module(body=nodes, type_ignores=[]))
    writes = []

    async def noop(*_args, **_kwargs):
        return None

    async def d1_first(_env, sql, *_args):
        if "COUNT(*)" in sql:
            return {"c": 0}
        return None

    async def d1_all(*_args):
        return []

    async def d1_run(_env, sql, *_args):
        writes.append(" ".join(sql.split()))

    async def account_row(_env, _owner):
        return _blind("alice-node"), {"pubkey": PUBLISHER_KEY, "kind": "node"}

    async def always_verify(*_args):
        return True

    async def contribution_failure(*_args):
        raise RuntimeError("optional projection unavailable")

    async def catalog_state(_env, _record, _account_bi, _account_rec,
                            _repo_bi, _encrypted):
        writes.append("INSERT INTO repositories")

    def response(data, status=200, **_kwargs):
        return {"status": status, "data": data}

    record = {
        **_catalog_record(_snapshot()),
        "maintainer": PUBLISHER_KEY,
        "source": "local-node",
        "stateHash": "",
        "stateSig": "",
    }
    namespace = {
        "bounded_json_request": json_from_request_double,
        "ensure_schema": noop,
        "method_name": lambda request: request.method,
        "safe_catalog_record": lambda _data: dict(record),
        "_is_blocked_catalog_identity": lambda *_args: False,
        "_account_row": account_row,
        "_catalog_publication_key": (
            lambda _env, _owner, maintainer: asyncio.sleep(
                0, result=maintainer)),
        # Authority accepts the primary key or an enabled owner_sign device
        # key; this harness account publishes with its primary key.
        "_owner_signing_pubkeys": (
            lambda _env, _owner: asyncio.sleep(0, result=[PUBLISHER_KEY])),
        "clean_string": lambda value, maximum=240: str(value or "")[:maximum],
        "ed25519_verify": always_verify,
        "blind_index": lambda _env, value: asyncio.sleep(0, result=_blind(value)),
        "touch_registered_node": noop,
        "catalog_rate_check": noop,
        # No website deletion is pending for these repos, so every publish is
        # accepted (see test_repo_delete_tombstone.py for the refusal path).
        "_repo_delete_tombstone_active": (
            lambda _env, _repo_bi: asyncio.sleep(0, result=False)),
        "_clear_repo_delete_tombstone": noop,
        "d1_first": d1_first,
        "d1_all": d1_all,
        "d1_run": d1_run,
        "decrypt_row": lambda _env, value: asyncio.sleep(0, result=value),
        "encrypt_row": lambda _env, value: asyncio.sleep(0, result=value),
        "purge_catalog_related_caches": noop,
        "schedule_public_catalog_republish": noop,
        # Every publish in this harness counts as a change so the write path
        # (the behavior under test) always runs.
        "catalog_meaningful_change": lambda prior, record: True,
        "_catalog_ms": lambda value: 0,
        "CATALOG_UNCHANGED_LEASE_MS": 8 * 60 * 1000,
        "_bump_catalog_rev": noop,
        "notify_repo_host": noop,
        "notify_repo_mirrors": noop,
        "_https_mirror_refresh_catalog_publisher_health": noop,
        "_contribution_ingest_snapshot": contribution_failure,
        "_contribution_write_catalog_state": catalog_state,
        "_index_repo_fork_edge": noop,
        "json_response": response,
        "CATALOG_MAX_RECORDS_PER_OWNER": 50,
        "MAX_CATALOG_REPOS": 200,
        "MAX_NODE_NAME": 64,
        "STATE_PIN_HISTORY": 4,
        "Date": type("Date", (), {"now": staticmethod(lambda: NOW_MS)}),
    }
    exec(compile(module, str(ENTRY), "exec"), with_maintenance_off(namespace))

    async def read_json():
        return {
            "catalogSig": "catalog-signature",
            "contributionPayload": "invalid",
            "contributionSig": "invalid",
        }

    request = SimpleNamespace(method="POST", json=read_json)
    result = asyncio.run(namespace["catalog_handler"](SimpleNamespace(), request))

    assert result["status"] == 201
    assert result["data"]["ok"] is True
    assert result["data"]["contributionsAccepted"] is False
    assert result["data"]["contributionWarning"] == "contribution_ingestion_failed"
    assert any(sql.startswith("INSERT INTO repositories") for sql in writes)


def test_ingest_rename_and_account_cleanup_paths_cover_all_projection_keys():
    source = ENTRY.read_text(encoding="utf-8")
    for table in (
        "profile_contribution_receipts",
        "profile_contribution_projects",
        "profile_contribution_days",
        "profile_contribution_languages",
    ):
        assert "UPDATE " + table in source
        assert "DELETE FROM " + table in source
    assert "UPDATE account_devices SET account_bi=? WHERE account_bi=?" in source
    assert "DELETE FROM account_devices WHERE account_bi=?" in source


def test_ingest_privacy_and_catalog_eviction_cleanup_follow_verified_state():
    source = ENTRY.read_text(encoding="utf-8")
    catalog_source = CATALOG_ROUTES.read_text(encoding="utf-8")
    catalog = catalog_source[
        catalog_source.index("async def catalog_handler"):
    ]
    post = catalog[catalog.index('if method == "POST"'):catalog.index(
        'if method == "DELETE"'
    )]

    assert post.index('return json_response({"error": "stale_update"}') < (
        post.index("await _contribution_write_catalog_state(")
    )
    helper = source[source.index("async def _contribution_write_catalog_state"):]
    helper = helper[:helper.index("async def _contribution_move_repo_namespace")]
    assert "INSERT INTO repositories" in helper
    assert "SET is_public=0" in helper
    assert "is_public=1" in helper
    stale_delete = 'DELETE FROM repositories WHERE key_bi=?", stale_key'
    assert post.index("await _delete_repo_scoped_state(env, stale_key)") < (
        post.index(stale_delete)
    )
    blocked = source[source.index("async def purge_blocked_catalog"):]
    blocked = blocked[:blocked.index("async def active_registered_node_bis")]
    assert "await _delete_repo_scoped_state(env, repo_bi)" in blocked


def test_ingest_namespace_rename_refreshes_encrypted_day_labels():
    source = ENTRY.read_text(encoding="utf-8")
    move = source[source.index("async def _contribution_move_repo_namespace"):]
    move = move[:move.index("async def _contribution_ingest_snapshot")]

    assert (
        '"source_repo_bi=?, project_bi=?, data=? WHERE source_repo_bi=?"'
        in move
    )


@pytest.mark.parametrize(
    "attempt",
    ["absent", "replay", "older", "conflict", "invalid"],
)
def test_ingest_nonactivating_attempt_preserves_active_project_state(
        ingest_harness, attempt):
    old_user_bi = _blind("alice-user")
    ingest_harness.add_node(_blind("alice-node"), old_user_bi, PUBLISHER_KEY)
    initial = _snapshot(
        coverage={
            "commits": "partial",
            "collaboration": "complete",
            "languages": "partial",
        },
        days=[["2026-07-13", PUBLISHER_KEY, 1, 0, 0, 0]],
    )
    ingest_harness.run_ingest(_ingest_data(initial), _catalog_record(initial))
    before = dict(ingest_harness.db.execute(
        "SELECT owner_user_bi, project_bi, active_generation_bi, captured_at, "
        "verified_from, data FROM profile_contribution_projects"
    ).fetchone())
    ingest_harness.db.execute(
        "UPDATE nodes SET user_bi=? WHERE node_bi=?",
        (_blind("new-user"), _blind("alice-node")),
    )
    ingest_harness.db.commit()

    if attempt == "absent":
        data = {}
        candidate = initial
    elif attempt == "replay":
        data = _ingest_data(initial)
        candidate = initial
    elif attempt == "older":
        candidate = _snapshot(capturedAt=NOW_MS - 1, head="older")
        data = _ingest_data(candidate)
    elif attempt == "conflict":
        candidate = _snapshot(head="conflict")
        data = _ingest_data(candidate)
    else:
        candidate = _snapshot(capturedAt=NOW_MS + 1, head="invalid")
        data = _ingest_data(candidate, signature="bad-signature")

    record = _catalog_record(candidate, rootCommit="new-root")
    ingest_harness.run_ingest(data, record)
    after = dict(ingest_harness.db.execute(
        "SELECT owner_user_bi, project_bi, active_generation_bi, captured_at, "
        "verified_from, data FROM profile_contribution_projects"
    ).fetchone())

    assert after == before


def test_ingest_newer_activation_switches_project_and_rows_to_matching_keys(
        ingest_harness):
    ingest_harness.add_node(
        _blind("alice-node"), _blind("old-user"), PUBLISHER_KEY
    )
    initial = _snapshot(days=[["2026-07-13", PUBLISHER_KEY, 1, 0, 0, 0]])
    ingest_harness.run_ingest(
        _ingest_data(initial), _catalog_record(initial, rootCommit="old-root")
    )
    ingest_harness.db.execute(
        "UPDATE nodes SET user_bi=? WHERE node_bi=?",
        (_blind("new-user"), _blind("alice-node")),
    )
    ingest_harness.db.commit()
    newer = _snapshot(
        capturedAt=NOW_MS + 1,
        head="new-head",
        coverage={
            "commits": "complete",
            "collaboration": "partial",
            "languages": "complete",
        },
        days=[["2026-07-13", PUBLISHER_KEY, 4, 0, 0, 0]],
    )

    result = ingest_harness.run_ingest(
        _ingest_data(newer), _catalog_record(newer, rootCommit="new-root")
    )

    project = dict(ingest_harness.db.execute(
        "SELECT owner_user_bi, project_bi, data FROM "
        "profile_contribution_projects"
    ).fetchone())
    day = dict(ingest_harness.db.execute(
        "SELECT subject_user_bi, project_bi, commits FROM "
        "profile_contribution_days"
    ).fetchone())
    assert result == {"accepted": True, "warning": ""}
    assert project["owner_user_bi"] == day["subject_user_bi"] == _blind("new-user")
    assert project["project_bi"] == day["project_bi"] == _blind(
        "profile-project:new-root"
    )
    assert json.loads(project["data"][4:])["coverage"]["collaboration"] == "partial"
    assert day["commits"] == 4


@pytest.mark.parametrize("client_updated_at", ["0", str((1 << 53) - 1)])
def test_ingest_first_public_day_uses_worker_time_not_client_timestamp(
        ingest_harness, client_updated_at):
    record = _catalog_record(_snapshot(), updatedAt=client_updated_at)

    ingest_harness.publish(record)

    project = ingest_harness.db.execute(
        "SELECT first_public_day FROM profile_contribution_projects"
    ).fetchone()
    assert project[0] == "2026-07-14"


def test_ingest_forced_older_newer_interleaving_keeps_newest_generation(
        ingest_harness):
    ingest_harness.add_node(
        _blind("alice-node"), _blind("alice-user"), PUBLISHER_KEY
    )
    initial = _snapshot(days=[["2026-07-13", PUBLISHER_KEY, 1, 0, 0, 0]])
    ingest_harness.run_ingest(_ingest_data(initial), _catalog_record(initial))
    ingest_harness.db.execute(
        "INSERT OR REPLACE INTO repositories (key_bi, owner_bi, data, is_private) "
        "VALUES (?,?,?,0)",
        (_blind("alice-node/forkmesh"), _blind("alice-node"), "record"),
    )
    ingest_harness.db.commit()
    older = _snapshot(
        capturedAt=NOW_MS + 1000,
        head="older-new",
        days=[["2026-07-13", PUBLISHER_KEY, 2, 0, 0, 0]],
    )
    newer = _snapshot(
        capturedAt=NOW_MS + 2000,
        head="newest",
        days=[["2026-07-13", PUBLISHER_KEY, 3, 0, 0, 0]],
    )

    async def scenario():
        older_paused = asyncio.Event()
        release_older = asyncio.Event()
        paused = False

        async def hook(_sql, _args):
            nonlocal paused
            task = asyncio.current_task()
            if task and task.get_name() == "older" and not paused:
                paused = True
                older_paused.set()
                await release_older.wait()

        ingest_harness.project_read_hook = hook
        older_task = asyncio.create_task(
            ingest_harness.ingest_async(
                _ingest_data(older), _catalog_record(older)
            ),
            name="older",
        )
        await asyncio.wait_for(older_paused.wait(), timeout=1)
        newer_result = await ingest_harness.ingest_async(
            _ingest_data(newer), _catalog_record(newer)
        )
        release_older.set()
        older_result = await older_task
        return older_result, newer_result

    asyncio.run(scenario())

    project = ingest_harness.db.execute(
        "SELECT captured_at FROM profile_contribution_projects"
    ).fetchone()
    assert project[0] == newer["capturedAt"]
    assert [row[0] for row in ingest_harness.db.execute(
        "SELECT commits FROM profile_contribution_days"
    ).fetchall()] == [3]


def test_ingest_inactive_receipt_replay_is_classified_against_active_generation(
        ingest_harness):
    ingest_harness.add_node(
        _blind("alice-node"), _blind("alice-user"), PUBLISHER_KEY
    )
    active = _snapshot(days=[["2026-07-13", PUBLISHER_KEY, 5, 0, 0, 0]])
    ingest_harness.run_ingest(_ingest_data(active), _catalog_record(active))
    inactive = _snapshot(capturedAt=NOW_MS - 1, head="inactive")
    encoded = _encode(inactive)
    raw = base64.urlsafe_b64decode(
        encoded + "=" * ((4 - len(encoded) % 4) % 4)
    )
    digest = hashlib.sha256(raw).hexdigest()
    generation_bi = _blind(
        "profile-generation:" + _blind("alice-node/forkmesh") + ":" + digest
    )
    ingest_harness.db.execute(
        "INSERT INTO profile_contribution_receipts "
        "(generation_bi, snapshot_hash, source_account_bi, source_repo_bi, "
        "captured_at, head, day_rows, language_rows, created_at) "
        "VALUES (?,?,?,?,?,?,?,?,?)",
        (
            generation_bi, digest, _blind("alice-node"),
            _blind("alice-node/forkmesh"), inactive["capturedAt"],
            inactive["head"], 0, 0, NOW_MS,
        ),
    )
    ingest_harness.db.execute(
        "INSERT OR REPLACE INTO repositories (key_bi, owner_bi, data, is_private) "
        "VALUES (?,?,?,0)",
        (_blind("alice-node/forkmesh"), _blind("alice-node"), "record"),
    )
    ingest_harness.db.commit()

    result = ingest_harness.run_ingest(
        _ingest_data(inactive), _catalog_record(inactive)
    )

    assert result == {
        "accepted": False,
        "warning": "contribution_snapshot_older",
    }


@pytest.mark.parametrize(
    ("first_visibility", "last_visibility", "expected_private"),
    [("public", "private", 1), ("private", "public", 0)],
)
def test_ingest_catalog_visibility_batch_last_write_wins_atomically(
        ingest_harness, first_visibility, last_visibility, expected_private):
    base = _catalog_record(_snapshot())
    ingest_harness.publish({**base, "visibility": first_visibility})
    ingest_harness.publish({**base, "visibility": last_visibility})

    repository = ingest_harness.db.execute(
        "SELECT is_private FROM repositories"
    ).fetchone()
    project = ingest_harness.db.execute(
        "SELECT is_public FROM profile_contribution_projects"
    ).fetchone()
    assert repository[0] == expected_private
    assert project is not None
    assert project[0] == (0 if expected_private else 1)


def test_ingest_snapshot_cannot_activate_after_repository_becomes_private(
        ingest_harness):
    ingest_harness.add_node(
        _blind("alice-node"), _blind("alice-user"), PUBLISHER_KEY
    )
    active = _snapshot(days=[["2026-07-13", PUBLISHER_KEY, 1, 0, 0, 0]])
    ingest_harness.run_ingest(_ingest_data(active), _catalog_record(active))
    ingest_harness.db.execute(
        "INSERT OR REPLACE INTO repositories (key_bi, owner_bi, data, is_private) "
        "VALUES (?,?,?,1)",
        (_blind("alice-node/forkmesh"), _blind("alice-node"), "private"),
    )
    ingest_harness.db.execute(
        "UPDATE profile_contribution_projects SET is_public=0"
    )
    ingest_harness.db.commit()
    newer = _snapshot(capturedAt=NOW_MS + 1, head="private-newer")

    result = ingest_harness.run_ingest(
        _ingest_data(newer), _catalog_record(newer), publish=False
    )

    project = ingest_harness.db.execute(
        "SELECT is_public, captured_at FROM profile_contribution_projects"
    ).fetchone()
    assert result == {
        "accepted": False,
        "warning": "contribution_repository_private",
    }
    assert tuple(project) == (0, NOW_MS)


def test_ingest_actor_resolution_is_ambiguity_aware_and_requires_device_proof(
        ingest_harness):
    user_a = _blind("user-a")
    user_b = _blind("user-b")
    same_user_key = base64.urlsafe_b64encode(b"\x04" * 32).decode().rstrip("=")
    unproven_key = base64.urlsafe_b64encode(b"\x05" * 32).decode().rstrip("=")
    ingest_harness.add_node(_blind("primary-a"), user_a, DEVICE_KEY)
    ingest_harness.add_device(user_b, DEVICE_KEY, proven=True)
    ingest_harness.add_node(_blind("primary-same"), user_a, same_user_key)
    ingest_harness.add_device(user_a, same_user_key, proven=True)
    ingest_harness.add_device(user_a, unproven_key, proven=False)

    resolved = asyncio.run(
        ingest_harness.namespace["_contribution_actor_user_bis"](
            ingest_harness.env, [DEVICE_KEY, same_user_key, unproven_key]
        )
    )

    assert DEVICE_KEY not in resolved
    assert resolved[same_user_key] == user_a
    assert unproven_key not in resolved


def test_ingest_actor_resolution_uses_one_set_query_at_contract_row_limit(
        ingest_harness):
    actor_keys = [
        base64.urlsafe_b64encode(index.to_bytes(32, "big"))
        .decode()
        .rstrip("=")
        for index in range(MAX_CONTRIBUTION_DAYS)
    ]
    ingest_harness.select_statements.clear()

    result = asyncio.run(
        ingest_harness.namespace["_contribution_actor_user_bis"](
            ingest_harness.env, actor_keys
        )
    )

    assert result == {}
    assert len(ingest_harness.select_statements) == 1
    assert "json_each" in ingest_harness.select_statements[0][0]


def test_ingest_max_day_generation_executes_under_d1_free_query_limit(
        ingest_harness):
    actor_keys = sorted(
        base64.urlsafe_b64encode(index.to_bytes(32, "big"))
        .decode()
        .rstrip("=")
        for index in range(MAX_CONTRIBUTION_DAYS)
    )
    snapshot = _snapshot(days=[
        ["2026-07-13", key, 1, 0, 0, 0] for key in actor_keys
    ])
    ingest_harness.db.executemany(
        "INSERT INTO nodes (node_bi, user_bi, pubkey) VALUES (?,?,?)",
        [(_blind("node:%d" % index), _blind("user:%d" % index), key)
         for index, key in enumerate(actor_keys)],
    )
    ingest_harness.db.execute(
        "INSERT INTO profile_contribution_projects "
        "(source_repo_bi, source_account_bi, owner_user_bi, project_bi, "
        "first_public_day, is_public, active_generation_bi, captured_at, "
        "verified_from, data) VALUES (?,?,?,?,?,1,NULL,0,NULL,?)",
        (
            _blind("alice-node/forkmesh"), _blind("alice-node"),
            _blind("alice-user"), _blind("profile-project:stable-root"),
            "2026-07-14", "enc:{}",
        ),
    )
    ingest_harness.db.execute(
        "INSERT OR REPLACE INTO repositories (key_bi, owner_bi, data, is_private) "
        "VALUES (?,?,?,0)",
        (_blind("alice-node/forkmesh"), _blind("alice-node"), "record"),
    )
    ingest_harness.db.commit()
    digest = "a" * 64
    canonical = snapshot_signature_canonical(
        "alice-node", "forkmesh", str(NOW_MS), digest
    )
    signature = "test-" + base64.urlsafe_b64encode(
        hashlib.sha256(canonical).digest()
    ).decode().rstrip("=")
    real_module = ingest_harness.namespace["contributions"]
    ingest_harness.namespace["contributions"] = SimpleNamespace(
        decode_snapshot_payload=lambda _encoded, now_ms=None: (
            snapshot, b"synthetic-max-snapshot", digest
        ),
        snapshot_signature_canonical=snapshot_signature_canonical,
        language_for_extension=real_module.language_for_extension,
        contribution_date_range=real_module.contribution_date_range,
    )
    ingest_harness.statements.clear()
    ingest_harness.select_statements.clear()

    result = ingest_harness.run_ingest(
        {
            "contributionPayload": _encode(_snapshot()),
            "contributionSig": signature,
        },
        _catalog_record(snapshot),
    )

    statement_count = (
        len(ingest_harness.statements) + len(ingest_harness.select_statements)
    )
    assert result == {"accepted": True, "warning": ""}
    assert ingest_harness.db.execute(
        "SELECT COUNT(*) FROM profile_contribution_days"
    ).fetchone()[0] == MAX_CONTRIBUTION_DAYS
    assert statement_count < 50
    assert max(
        len(args) for _sql, args in
        ingest_harness.statements + ingest_harness.select_statements
    ) <= 100
    assert any("json_each" in sql for sql, _args in ingest_harness.statements)


def _linked_contribution_identity_harness():
    old_user_bi = "bi:old-user"
    node_bi = "bi:linked-node"
    accounts = {
        "linked-node": {
            "name": "linked-node",
            "kind": "node",
            "owner": "old-user",
            "pubkey": PUBLISHER_KEY,
        }
    }
    nodes = {
        node_bi: {
            "node_bi": node_bi,
            "user_bi": old_user_bi,
            "name": "linked-node",
            "data": dict(accounts["linked-node"]),
        }
    }

    async def d1_all(_env, sql, *args):
        if "FROM nodes WHERE user_bi" in sql:
            return [
                dict(row) for row in nodes.values()
                if row["user_bi"] == args[0] and row["node_bi"] != args[1]
            ]
        raise AssertionError("unexpected d1_all: " + sql)

    async def d1_first(_env, sql, *args):
        if "SELECT user_bi FROM nodes WHERE node_bi" in sql:
            row = nodes.get(args[0])
            return {"user_bi": row["user_bi"]} if row else None
        raise AssertionError("unexpected d1_first: " + sql)

    async def decrypt_row(_env, value):
        return dict(value)

    async def account_row(_env, name):
        value = accounts.get(name)
        return "bi:" + name, (dict(value) if value else None)

    async def save_account(_env, account_bi, record, **_kwargs):
        name = record["name"]
        accounts[name] = dict(record)
        owner = record.get("owner", "")
        nodes[account_bi]["user_bi"] = "bi:" + owner if owner else None
        nodes[account_bi]["data"] = dict(record)

    namespace = _load_entry_contribution_functions({
        "clean_string": lambda value, maximum=240: str(value or "")[:maximum],
        "d1_all": d1_all,
        "d1_first": d1_first,
        "decrypt_row": decrypt_row,
        "_account_row": account_row,
        "_save_account": save_account,
        "MAX_NODE_NAME": 64,
    })
    return namespace, accounts, nodes, old_user_bi, node_bi, save_account


def test_ingest_user_rename_retargets_linked_node_identity_and_survives_remirror():
    ns, accounts, nodes, old_user_bi, node_bi, save_account = (
        _linked_contribution_identity_harness()
    )

    asyncio.run(ns["_contribution_retarget_linked_nodes"](
        object(), old_user_bi, "old-user", "bi:new-user", "new-user"
    ))

    assert nodes[node_bi]["user_bi"] == "bi:new-user"
    assert accounts["linked-node"]["owner"] == "new-user"
    asyncio.run(save_account(
        object(), node_bi, dict(accounts["linked-node"])
    ))
    resolved = asyncio.run(ns["_contribution_owner_user_bi"](
        object(), node_bi, accounts["linked-node"]
    ))
    assert resolved == "bi:new-user"


def test_ingest_user_delete_unlinks_node_and_username_reuse_cannot_inherit_it():
    ns, accounts, nodes, old_user_bi, node_bi, save_account = (
        _linked_contribution_identity_harness()
    )

    asyncio.run(ns["_contribution_retarget_linked_nodes"](
        object(), old_user_bi, "old-user", None, ""
    ))

    assert nodes[node_bi]["user_bi"] is None
    assert not accounts["linked-node"].get("owner")
    asyncio.run(save_account(
        object(), node_bi, dict(accounts["linked-node"])
    ))
    accounts["old-user"] = {
        "name": "old-user", "kind": "user", "pubkey": ACTOR_KEY,
    }
    resolved = asyncio.run(ns["_contribution_owner_user_bi"](
        object(), node_bi, accounts["linked-node"]
    ))
    assert resolved == node_bi


def test_ingest_account_rename_and_delete_call_linked_node_retargeting():
    source = ENTRY.read_text(encoding="utf-8")
    rename = source[source.index("async def _rename_account_namespace"):]
    rename = rename[:rename.index("async def _delete_bounties_namespace")]
    delete = source[source.index("async def _delete_account_namespace"):]
    delete = delete[:delete.index("def _owned_nodes")]

    assert (
        "await _contribution_retarget_linked_nodes(\n"
        "        env, name_bi, old_name, new_name_bi, new_name)"
        in rename
    )
    assert (
        "await _contribution_retarget_linked_nodes(\n"
        "        env, name_bi, name, None, \"\")"
        in delete
    )


class _ContributionApiRequest:
    def __init__(self, url):
        self.url = url


class _ContributionApiHarness(_IngestHarness):
    def __init__(self):
        super().__init__()
        self.profiles = {
            "alice": {
                "name": "alice",
                "kind": "user",
                "status": "active",
            }
        }
        self.cache_entries = {}
        self.cache_matches = []
        self.cache_puts = []
        self.cache_deletes = []

        async def account_identity_rec_by_bi(_env, name_bi):
            for name, record in self.profiles.items():
                if _blind(name) == name_bi:
                    return dict(record)
            return None

        async def account_row(_env, name):
            record = self.profiles.get(name)
            return _blind(name), (dict(record) if record else None)

        async def edge_cache_match(key):
            self.cache_matches.append(key)
            return self.cache_entries.get(key)

        async def edge_cache_put(key, response):
            self.cache_puts.append((key, response))
            self.cache_entries[key] = response

        async def edge_cache_delete(key):
            self.cache_deletes.append(key)
            self.cache_entries.pop(key, None)

        async def contribution_internal_cache_get(key):
            self.cache_matches.append(key)
            return self.cache_entries.get(key)

        async def contribution_internal_cache_put(key, payload):
            self.cache_puts.append((key, payload))
            self.cache_entries[key] = payload

        async def response_json(response):
            return response if isinstance(response, dict) else {}

        async def issue_reporter_tracker_summary(_env):
            return {"revision": "empty", "counts": {}, "activity": []}

        def json_response(data, status=200, cache_seconds=None,
                          cache_control=None, **_kwargs):
            return {
                "status": status,
                "data": data,
                "cacheSeconds": cache_seconds,
                "cacheControl": cache_control,
            }

        class _ApiDate:
            @staticmethod
            def now():
                return NOW_MS

        self.namespace.update({
            "Date": _ApiDate,
            "parse_qs": parse_qs,
            "quote": quote,
            "urlparse": urlparse,
            "json_response": json_response,
            "edge_cache_match": edge_cache_match,
            "edge_cache_put": edge_cache_put,
            "edge_cache_delete": edge_cache_delete,
            "_response_json": response_json,
            "ISSUE_REPORTER_TRACKER_CACHE_KEY": (
                "https://forkmesh.internal/tracker/issue-reporter-summary"
            ),
            "re": re,
            "_contribution_internal_cache_get": (
                contribution_internal_cache_get
            ),
            "_contribution_internal_cache_put": (
                contribution_internal_cache_put
            ),
            "_issue_reporter_tracker_summary": issue_reporter_tracker_summary,
            "_account_identity_rec_by_bi": account_identity_rec_by_bi,
            "_account_row": account_row,
            "valid_node_name": lambda value: bool(
                re.fullmatch(r"[a-z0-9][a-z0-9._:-]{0,63}", value or "")
            ),
            "MAX_NODE_NAME": 64,
            "MAX_CATALOG_REPOS": 200,
            "MAX_REPO_SEGMENT": 80,
            "PROFILE_CONTRIBUTION_CACHE_PREFIX": (
                "https://forkmesh.internal/api/profile-contributions/"
            ),
            "PROFILE_CONTRIBUTION_CACHE_TTL": 30,
        })

    def api(self, query="", name="alice"):
        handler = self.namespace.get("_contribution_profile_api")
        assert callable(handler), "missing public contribution API handler"
        url = "https://forkmesh.test/api/accounts/%s/contributions%s" % (
            name,
            ("?" + query) if query else "",
        )
        return asyncio.run(handler(
            self.env, _ContributionApiRequest(url), name
        ))

    def add_source(
            self, source_repo_bi, project_bi, *, owner_user_bi=None,
            owner="alice", name="project", first_public_day="2026-01-02",
            captured_at=1000, verified_from="2025-01-01", coverage=None,
            is_public=1, repository_private=0, generation_bi=None,
            has_snapshot=True):
        owner_user_bi = owner_user_bi or _blind("alice")
        generation_bi = None if not has_snapshot else (
            generation_bi if generation_bi is not None
            else "generation:" + source_repo_bi
        )
        project_data = {"owner": owner, "name": name}
        if has_snapshot:
            project_data["coverage"] = coverage or {
                "commits": "complete",
                "collaboration": "complete",
                "languages": "complete",
            }
        encrypted = "enc:" + json.dumps(
            project_data, sort_keys=True, separators=(",", ":")
        )
        self.db.execute(
            "INSERT OR REPLACE INTO repositories "
            "(key_bi, owner_bi, data, is_private) VALUES (?,?,?,?)",
            (source_repo_bi, "account:" + owner, "record", repository_private),
        )
        self.db.execute(
            "INSERT OR REPLACE INTO profile_contribution_projects "
            "(source_repo_bi, source_account_bi, owner_user_bi, project_bi, "
            "first_public_day, is_public, active_generation_bi, captured_at, "
            "verified_from, data) VALUES (?,?,?,?,?,?,?,?,?,?)",
            (
                source_repo_bi, "account:" + owner, owner_user_bi,
                project_bi, first_public_day, is_public, generation_bi,
                captured_at if has_snapshot else 0,
                verified_from if has_snapshot else None,
                encrypted,
            ),
        )
        self.db.commit()
        return generation_bi

    def add_day(
            self, generation_bi, source_repo_bi, project_bi, day, *,
            subject_user_bi=None, commits=0, issues=0, pulls=0, reviews=0,
            captured_at=1000, owner="alice", name="project"):
        subject_user_bi = subject_user_bi or _blind("alice")
        encrypted = "enc:" + json.dumps(
            {"owner": owner, "name": name},
            sort_keys=True,
            separators=(",", ":"),
        )
        self.db.execute(
            "INSERT INTO profile_contribution_days "
            "(generation_bi, subject_user_bi, source_account_bi, "
            "source_repo_bi, project_bi, day, commits, issues, pulls, "
            "reviews, captured_at, data) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
            (
                generation_bi, subject_user_bi, "account:" + owner,
                source_repo_bi, project_bi, day, commits, issues, pulls,
                reviews, captured_at, encrypted,
            ),
        )
        self.db.commit()

    def add_language(
            self, generation_bi, source_repo_bi, project_bi, language,
            byte_count, *, owner_user_bi=None, files=1, captured_at=1000):
        self.db.execute(
            "INSERT INTO profile_contribution_languages "
            "(generation_bi, owner_user_bi, source_repo_bi, project_bi, "
            "language, bytes, files, captured_at) VALUES (?,?,?,?,?,?,?,?)",
            (
                generation_bi, owner_user_bi or _blind("alice"),
                source_repo_bi, project_bi, language, byte_count, files,
                captured_at,
            ),
        )
        self.db.commit()


@pytest.fixture
def contribution_api_harness():
    harness = _ContributionApiHarness()
    try:
        yield harness
    finally:
        harness.close()


def _populate_contribution_api(harness):
    owned_project = "project:owned"
    old_generation = harness.add_source(
        "repo:owned-old", owned_project, owner="alice", name="owned",
        first_public_day="2026-01-02", captured_at=1000,
    )
    new_generation = harness.add_source(
        "repo:owned-new", owned_project, owner="alice", name="owned",
        first_public_day="2026-02-03", captured_at=2000,
    )
    harness.add_day(
        old_generation, "repo:owned-old", owned_project, "2026-07-10",
        commits=99, captured_at=1000, owner="alice", name="owned",
    )
    harness.add_day(
        new_generation, "repo:owned-new", owned_project, "2026-07-10",
        commits=3, captured_at=2000, owner="alice", name="owned",
    )
    language_bytes = {
        "Python": 600,
        "C++": 500,
        "Rust": 400,
        "Go": 300,
        "Java": 200,
        "Ruby": 100,
        "Other": 50,
    }
    harness.add_language(
        old_generation, "repo:owned-old", owned_project, "C", 9999,
        captured_at=1000,
    )
    for language, byte_count in language_bytes.items():
        harness.add_language(
            new_generation, "repo:owned-new", owned_project,
            language, byte_count, captured_at=2000,
        )

    foreign_project = "project:foreign"
    foreign_generation = harness.add_source(
        "repo:foreign", foreign_project, owner_user_bi=_blind("bob"),
        owner="bob", name="foreign", first_public_day="2026-03-04",
        captured_at=3000,
        coverage={
            "commits": "partial",
            "collaboration": "complete",
            "languages": "partial",
        },
    )
    harness.add_day(
        foreign_generation, "repo:foreign", foreign_project, "2026-07-11",
        issues=2, pulls=1, reviews=1, captured_at=3000,
        owner="bob", name="foreign",
    )

    private_project = "project:private"
    private_generation = harness.add_source(
        "repo:private", private_project, owner="alice", name="private",
        captured_at=9000, repository_private=1,
    )
    harness.add_day(
        private_generation, "repo:private", private_project, "2026-07-12",
        commits=500, captured_at=9000, owner="alice", name="private",
    )
    harness.add_language(
        private_generation, "repo:private", private_project, "Shell", 50000,
        captured_at=9000,
    )

    harness.add_day(
        "generation:inactive", "repo:owned-new", owned_project,
        "2026-07-13", commits=700, captured_at=10000,
        owner="alice", name="owned",
    )


def test_contribution_api_route_precedes_single_account_lookup():
    urls = URLS.read_text(encoding="utf-8")
    entry = ENTRY.read_text(encoding="utf-8")
    assert (
        'ACCOUNT_CONTRIBUTIONS_RE = re.compile(\n'
        '    r"^/api/accounts/([^/]+)/contributions$"\n'
        ')' in urls
    )
    assert '"ACCOUNT_CONTRIBUTIONS_RE"' in entry
    assert "globals()[_url_export_name] = _urls.export(_url_export_name)" in entry
    route = entry[entry.index("async def accounts_handler"):]
    assert route.index("ACCOUNT_CONTRIBUTIONS_RE.match(url.path)") < route.index(
        "ACCOUNTS_RE.match(url.path)"
    )


def test_contribution_api_exact_public_aggregation_and_mirror_dedup(
        contribution_api_harness):
    _populate_contribution_api(contribution_api_harness)

    response = contribution_api_harness.api(
        "from=2026-01-01&to=2026-12-31"
    )

    assert response["status"] == 200
    assert response["data"] == {
        "ok": True,
        "profile": "alice",
        "range": {"from": "2026-01-01", "to": "2026-12-31"},
        "total": 8,
        "repositoryCount": 1,
        "typeTotals": {
            "commits": 3,
            "issues": 2,
            "pulls": 1,
            "reviews": 1,
            "repositories": 1,
        },
        "days": [
            {
                "date": "2026-01-02", "commits": 0, "issues": 0,
                "pulls": 0, "reviews": 0, "repositories": 1,
            },
            {
                "date": "2026-07-10", "commits": 3, "issues": 0,
                "pulls": 0, "reviews": 0, "repositories": 0,
            },
            {
                "date": "2026-07-11", "commits": 0, "issues": 2,
                "pulls": 1, "reviews": 1, "repositories": 0,
            },
        ],
        "languages": [
            {"name": "Python", "bytes": 600, "percentage": 27.9,
             "color": "#3572A5"},
            {"name": "C++", "bytes": 500, "percentage": 23.3,
             "color": "#f34b7d"},
            {"name": "Rust", "bytes": 400, "percentage": 18.6,
             "color": "#dea584"},
            {"name": "Go", "bytes": 300, "percentage": 14.0,
             "color": "#00ADD8"},
            {"name": "Java", "bytes": 200, "percentage": 9.3,
             "color": "#b07219"},
            {"name": "Other", "bytes": 150, "percentage": 7.0,
             "color": "#8b949e"},
        ],
        "recentActivity": [
            {
                "date": "2026-07-11", "kind": "issues", "count": 2,
                "repository": {
                    "owner": "bob", "name": "foreign",
                    "url": "/bob/foreign",
                },
            },
            {
                "date": "2026-07-11", "kind": "pulls", "count": 1,
                "repository": {
                    "owner": "bob", "name": "foreign",
                    "url": "/bob/foreign",
                },
            },
            {
                "date": "2026-07-11", "kind": "reviews", "count": 1,
                "repository": {
                    "owner": "bob", "name": "foreign",
                    "url": "/bob/foreign",
                },
            },
            {
                "date": "2026-07-10", "kind": "commits", "count": 3,
                "repository": {
                    "owner": "alice", "name": "owned",
                    "url": "/alice/owned",
                },
            },
            {
                "date": "2026-01-02", "kind": "repositories", "count": 1,
                "repository": {
                    "owner": "alice", "name": "owned",
                    "url": "/alice/owned",
                },
            },
        ],
        "coverage": {
            "status": "complete",
            "verifiedFrom": "2025-01-01",
            "updatedAt": 3000,
            "missing": [],
        },
    }
    assert len(contribution_api_harness.select_statements) <= 6
    assert all(
        "is_public" in sql and "is_private" in sql
        for sql, _args in contribution_api_harness.select_statements
    )
    assert any(
        "LIMIT 20" in sql
        for sql, _args in contribution_api_harness.select_statements
    )


def test_contribution_api_adds_reviewed_reporter_issues_without_double_count(
        contribution_api_harness):
    reporter_key = "R" * 43
    contribution_api_harness.profiles["md-mehboob"] = {
        "name": "md-mehboob", "kind": "user", "status": "active",
    }
    async def cached_tracker_summary(_env):
        return {
            "revision": "b" * 64,
            "counts": {"md-mehboob": 106},
            "activity": [
                ["md-mehboob", "2026-08-12", reporter_key, 4],
                ["md-mehboob", "2026-08-13", reporter_key, 3],
                ["md-mehboob", "2025-08-12", reporter_key, 99],
            ],
        }

    contribution_api_harness.namespace[
        "_contribution_cached_reporter_summary"
    ] = cached_tracker_summary

    response = contribution_api_harness.api(
        "from=2026-01-01&to=2026-12-31", name="md-mehboob")
    data = response["data"]

    assert data["total"] == 7
    assert data["typeTotals"]["issues"] == 7
    assert data["days"] == [
        {"date": "2026-08-12", "commits": 0, "issues": 4,
         "pulls": 0, "reviews": 0, "repositories": 0},
        {"date": "2026-08-13", "commits": 0, "issues": 3,
         "pulls": 0, "reviews": 0, "repositories": 0},
    ]
    assert data["recentActivity"] == [
        {"date": "2026-08-13", "kind": "issues", "count": 3,
         "repository": {"owner": "forkmesh", "name": "forkmesh",
                        "url": "/forkmesh/forkmesh"}},
        {"date": "2026-08-12", "kind": "issues", "count": 4,
         "repository": {"owner": "forkmesh", "name": "forkmesh",
                        "url": "/forkmesh/forkmesh"}},
    ]

    # Once the same signer is registered to the profile, the signed source row
    # owns this attribution and the reviewed supplement must not duplicate it.
    contribution_api_harness.db.execute(
        "INSERT INTO nodes (node_bi,user_bi,pubkey) VALUES (?,?,?)",
        ("node:reporter", _blind("md-mehboob"), reporter_key),
    )
    contribution_api_harness.db.commit()
    contribution_api_harness.cache_entries.clear()
    second = contribution_api_harness.api(
        "from=2026-01-01&to=2026-12-31", name="md-mehboob")
    assert second["data"]["total"] == 0


def test_contribution_api_mirror_tie_break_uses_source_key(
        contribution_api_harness):
    project_bi = "project:tied"
    first_generation = contribution_api_harness.add_source(
        "repo:a", project_bi, owner="alice", name="alpha",
        captured_at=5000,
    )
    second_generation = contribution_api_harness.add_source(
        "repo:b", project_bi, owner="alice", name="beta",
        captured_at=5000,
    )
    contribution_api_harness.add_day(
        first_generation, "repo:a", project_bi, "2026-06-01",
        commits=2, captured_at=5000, owner="alice", name="alpha",
    )
    contribution_api_harness.add_day(
        second_generation, "repo:b", project_bi, "2026-06-01",
        commits=9, captured_at=5000, owner="alice", name="beta",
    )
    contribution_api_harness.add_language(
        first_generation, "repo:a", project_bi, "Python", 10,
        captured_at=5000,
    )
    contribution_api_harness.add_language(
        second_generation, "repo:b", project_bi, "Rust", 99,
        captured_at=5000,
    )

    result = contribution_api_harness.api(
        "from=2026-01-01&to=2026-12-31"
    )["data"]

    assert result["typeTotals"]["commits"] == 2
    assert result["languages"] == [
        {"name": "Python", "bytes": 10, "percentage": 100.0,
         "color": "#3572A5"}
    ]
    assert result["recentActivity"][0]["repository"] == {
        "owner": "alice", "name": "alpha", "url": "/alice/alpha",
    }


def test_contribution_api_newest_mirror_absolutely_replaces_sparse_older_rows(
        contribution_api_harness):
    project_bi = "project:absolute"
    older = contribution_api_harness.add_source(
        "repo:absolute-old", project_bi, owner="alice", name="absolute",
        captured_at=1000,
    )
    newer = contribution_api_harness.add_source(
        "repo:absolute-new", project_bi, owner="alice", name="absolute",
        captured_at=2000,
    )
    contribution_api_harness.add_day(
        older, "repo:absolute-old", project_bi, "2026-06-01",
        commits=5, captured_at=1000, owner="alice", name="absolute",
    )
    contribution_api_harness.add_day(
        newer, "repo:absolute-new", project_bi, "2026-06-02",
        commits=1, captured_at=2000, owner="alice", name="absolute",
    )

    result = contribution_api_harness.api(
        "from=2026-01-01&to=2026-12-31"
    )["data"]

    assert result["typeTotals"]["commits"] == 1
    assert all(day["date"] != "2026-06-01" for day in result["days"])
    assert all(
        item["date"] != "2026-06-01"
        for item in result["recentActivity"]
    )


def test_contribution_api_uses_older_mirror_before_newer_verified_boundary(
        contribution_api_harness):
    project_bi = "project:historical-mirror"
    older = contribution_api_harness.add_source(
        "repo:historical-old", project_bi,
        owner="alice", name="historical-mirror",
        captured_at=1000, verified_from="2020-01-01",
    )
    newer = contribution_api_harness.add_source(
        "repo:historical-new", project_bi,
        owner="alice", name="historical-mirror",
        captured_at=2000, verified_from="2025-01-01",
    )
    contribution_api_harness.add_day(
        older, "repo:historical-old", project_bi, "2024-06-01",
        commits=7, captured_at=1000,
        owner="alice", name="historical-mirror",
    )
    contribution_api_harness.add_day(
        newer, "repo:historical-new", project_bi, "2026-06-01",
        commits=3, captured_at=2000,
        owner="alice", name="historical-mirror",
    )

    result = contribution_api_harness.api(
        "from=2024-01-01&to=2024-12-31"
    )["data"]

    assert result["typeTotals"]["commits"] == 7
    assert result["days"] == [{
        "date": "2024-06-01", "commits": 7, "issues": 0,
        "pulls": 0, "reviews": 0, "repositories": 0,
    }]
    assert result["recentActivity"] == [{
        "date": "2024-06-01", "kind": "commits", "count": 7,
        "repository": {
            "owner": "alice", "name": "historical-mirror",
            "url": "/alice/historical-mirror",
        },
    }]
    assert result["coverage"] == {
        "status": "complete",
        "verifiedFrom": "2020-01-01",
        "updatedAt": 1000,
        "missing": [],
    }


def test_contribution_api_owned_coverage_merges_every_selected_day_source(
        contribution_api_harness):
    project_bi = "project:mixed-coverage"
    older = contribution_api_harness.add_source(
        "repo:mixed-coverage-old", project_bi,
        owner="alice", name="mixed-coverage",
        captured_at=1000, verified_from="2020-01-01",
    )
    newer = contribution_api_harness.add_source(
        "repo:mixed-coverage-new", project_bi,
        owner="alice", name="mixed-coverage",
        captured_at=2000, verified_from="2025-06-01",
        coverage={
            "commits": "complete",
            "collaboration": "partial",
            "languages": "complete",
        },
    )
    contribution_api_harness.add_day(
        older, "repo:mixed-coverage-old", project_bi, "2025-03-01",
        issues=1, captured_at=1000,
        owner="alice", name="mixed-coverage",
    )
    contribution_api_harness.add_day(
        newer, "repo:mixed-coverage-new", project_bi, "2025-07-01",
        issues=2, captured_at=2000,
        owner="alice", name="mixed-coverage",
    )

    result = contribution_api_harness.api(
        "from=2025-01-01&to=2025-12-31"
    )["data"]

    assert result["typeTotals"]["issues"] == 3
    assert [
        (day["date"], day["issues"])
        for day in result["days"]
    ] == [("2025-03-01", 1), ("2025-07-01", 2)]
    assert result["coverage"] == {
        "status": "partial",
        "verifiedFrom": "2025-06-01",
        "updatedAt": 2000,
        "missing": ["collaboration", "history"],
    }


def test_contribution_api_repository_count_excludes_foreign_activity_projects(
        contribution_api_harness):
    project_bi = "project:foreign-only"
    generation = contribution_api_harness.add_source(
        "repo:foreign-only", project_bi, owner_user_bi=_blind("bob"),
        owner="bob", name="foreign-only", captured_at=3000,
    )
    contribution_api_harness.add_day(
        generation, "repo:foreign-only", project_bi, "2026-06-03",
        issues=2, owner="bob", name="foreign-only", captured_at=3000,
    )

    result = contribution_api_harness.api(
        "from=2026-01-01&to=2026-12-31"
    )["data"]

    assert result["typeTotals"]["issues"] == 2
    assert result["repositoryCount"] == 0


def test_contribution_api_recent_activity_is_capped_at_twenty(
        contribution_api_harness):
    project_bi = "project:recent"
    generation = contribution_api_harness.add_source(
        "repo:recent", project_bi, owner="alice", name="recent",
    )
    for day in range(1, 26):
        contribution_api_harness.add_day(
            generation, "repo:recent", project_bi,
            "2026-06-%02d" % day, commits=1,
            owner="alice", name="recent",
        )

    activity = contribution_api_harness.api(
        "from=2026-01-01&to=2026-12-31"
    )["data"]["recentActivity"]

    assert len(activity) == 20
    assert activity[0]["date"] == "2026-06-25"
    assert activity[-1]["date"] == "2026-06-06"


def test_contribution_api_stable_zero_response_and_unavailable_coverage(
        contribution_api_harness):
    response = contribution_api_harness.api()

    assert response == {
        "status": 200,
        "data": {
            "ok": True,
            "profile": "alice",
            "range": {"from": "2025-07-14", "to": "2026-07-13"},
            "total": 0,
            "repositoryCount": 0,
            "typeTotals": {
                "commits": 0,
                "issues": 0,
                "pulls": 0,
                "reviews": 0,
                "repositories": 0,
            },
            "days": [],
            "languages": [],
            "recentActivity": [],
            "coverage": {
                "status": "unavailable",
                "verifiedFrom": None,
                "updatedAt": 0,
                "missing": ["commits", "collaboration", "languages"],
            },
        },
        "cacheSeconds": None,
        "cacheControl": "no-store, max-age=0, must-revalidate",
    }


def test_contribution_api_does_not_cold_scan_issue_tracker(
        contribution_api_harness):
    async def live_tracker_should_not_run(_env):
        raise AssertionError("profile contributions must not cold-scan issues")

    contribution_api_harness.namespace[
        "_issue_reporter_tracker_summary"
    ] = live_tracker_should_not_run

    response = contribution_api_harness.api()

    assert response["status"] == 200
    assert response["data"]["total"] == 0
    assert (
        "https://forkmesh.internal/tracker/issue-reporter-summary"
        in contribution_api_harness.cache_matches
    )


def test_contribution_api_partial_coverage_reports_missing_dimensions_and_history(
        contribution_api_harness):
    contribution_api_harness.add_source(
        "repo:partial", "project:partial", owner="alice", name="partial",
        verified_from="2026-03-01", captured_at=7000,
        coverage={
            "commits": "partial",
            "collaboration": "complete",
            "languages": "partial",
        },
    )

    coverage = contribution_api_harness.api(
        "from=2026-01-01&to=2026-12-31"
    )["data"]["coverage"]

    assert coverage == {
        "status": "partial",
        "verifiedFrom": "2026-03-01",
        "updatedAt": 7000,
        "missing": ["commits", "languages", "history"],
    }


def test_contribution_api_owned_public_project_without_snapshot_is_partial(
        contribution_api_harness):
    contribution_api_harness.add_source(
        "repo:old-node", "project:old-node", owner="alice", name="old-node",
        first_public_day="2026-04-05", has_snapshot=False,
    )

    result = contribution_api_harness.api(
        "from=2026-01-01&to=2026-12-31"
    )["data"]

    assert result["repositoryCount"] == 1
    assert result["typeTotals"]["repositories"] == 1
    assert result["coverage"] == {
        "status": "partial",
        "verifiedFrom": None,
        "updatedAt": 0,
        "missing": ["commits", "collaboration", "languages"],
    }


@pytest.mark.parametrize("profile", [None, {"name": "alice", "status": "reserved"},
                                      {"name": "alice", "status": "active",
                                       "profile_private": True}])
def test_contribution_api_missing_inactive_and_private_profiles_are_not_found(
        contribution_api_harness, profile):
    if profile is None:
        contribution_api_harness.profiles.clear()
    else:
        contribution_api_harness.profiles["alice"] = profile

    response = contribution_api_harness.api()

    assert response["status"] == 404
    assert response["data"] == {"error": "not_found"}
    assert contribution_api_harness.cache_matches == []
    assert contribution_api_harness.cache_puts == []


@pytest.mark.parametrize(
    "query",
    [
        "from=2026-01-01&from=2026-01-02&to=2026-01-03",
        "from=&to=2026-01-03",
        "from=2024-12-31&to=2026-01-01",
        "from=2026-01-02&to=2026-01-01",
        "from=2026-1-01&to=2026-01-02",
        "from=2026-01-01&to=2026-01-02&viewer=alice",
    ],
)
def test_contribution_api_rejects_non_scalar_or_invalid_ranges_without_cache(
        contribution_api_harness, query):
    response = contribution_api_harness.api(query)

    assert response["status"] == 400
    assert response["data"] == {"error": "invalid_contribution_range"}
    assert contribution_api_harness.cache_matches == []
    assert contribution_api_harness.cache_puts == []


def test_contribution_api_internal_cache_uses_revision_and_browser_no_store(
        contribution_api_harness):
    project_bi = "project:cache"
    generation = contribution_api_harness.add_source(
        "repo:cache", project_bi, owner="alice", name="cache",
    )
    contribution_api_harness.add_day(
        generation, "repo:cache", project_bi, "2026-07-10",
        commits=3, owner="alice", name="cache",
    )
    revision = contribution_api_harness.namespace.get(
        "_contribution_profile_revision"
    )
    assert callable(revision), "missing contribution profile revision"
    old_revision = asyncio.run(revision(
        contribution_api_harness.env, _blind("alice"),
        "2025-07-14", "2026-07-13",
    ))
    old_key = (
        "https://forkmesh.internal/api/profile-contributions/"
        + quote(_blind("alice"), safe="")
        + "?from=2025-07-14&to=2026-07-13&revision=" + old_revision
    )
    contribution_api_harness.cache_entries[old_key] = {
        "ok": True, "profile": "alice", "total": 999,
    }
    contribution_api_harness.db.execute(
        "UPDATE repositories SET is_private=1 WHERE key_bi='repo:cache'"
    )
    contribution_api_harness.db.commit()
    contribution_api_harness.select_statements.clear()

    response = contribution_api_harness.api()

    assert response["status"] == 200
    assert response["data"]["total"] == 0
    assert old_key not in contribution_api_harness.cache_matches
    assert all(
        key == "https://forkmesh.internal/tracker/issue-reporter-summary"
        or "revision=" in key
        for key in contribution_api_harness.cache_matches
    )
    assert response["cacheSeconds"] is None
    assert response["cacheControl"] == "no-store, max-age=0, must-revalidate"


def test_contribution_api_historical_foreign_mirror_privacy_changes_revision(
        contribution_api_harness):
    project_bi = "project:foreign-history-cache"
    older = contribution_api_harness.add_source(
        "repo:foreign-history-old", project_bi,
        owner_user_bi=_blind("bob"), owner="bob", name="history",
        captured_at=1000, verified_from="2020-01-01",
    )
    newer = contribution_api_harness.add_source(
        "repo:foreign-history-new", project_bi,
        owner_user_bi=_blind("bob"), owner="bob", name="history",
        captured_at=2000, verified_from="2025-01-01",
    )
    contribution_api_harness.add_day(
        older, "repo:foreign-history-old", project_bi, "2024-06-01",
        commits=7, captured_at=1000, owner="bob", name="history",
    )
    contribution_api_harness.add_day(
        newer, "repo:foreign-history-new", project_bi, "2026-06-01",
        commits=3, captured_at=2000, owner="bob", name="history",
    )
    query = "from=2024-01-01&to=2024-12-31"

    first = contribution_api_harness.api(query)

    assert first["data"]["total"] == 7
    assert first["data"]["coverage"] == {
        "status": "complete",
        "verifiedFrom": "2020-01-01",
        "updatedAt": 1000,
        "missing": [],
    }
    assert len(contribution_api_harness.cache_puts) == 1
    stale_key = contribution_api_harness.cache_puts[0][0]

    contribution_api_harness.db.execute(
        "UPDATE repositories SET is_private=1 "
        "WHERE key_bi='repo:foreign-history-old'"
    )
    contribution_api_harness.db.commit()
    contribution_api_harness.cache_matches.clear()

    second = contribution_api_harness.api(query)

    assert second["data"]["total"] == 0
    assert stale_key not in contribution_api_harness.cache_matches


def test_contribution_api_revalidates_revision_before_cache_put_and_response(
        contribution_api_harness):
    project_bi = "project:race"
    generation = contribution_api_harness.add_source(
        "repo:race", project_bi, owner="alice", name="race",
    )
    contribution_api_harness.add_day(
        generation, "repo:race", project_bi, "2026-07-10",
        commits=4, owner="alice", name="race",
    )
    revision = contribution_api_harness.namespace.get(
        "_contribution_profile_revision"
    )
    assert callable(revision), "missing contribution profile revision"
    revision_calls = 0
    revision_values = []

    async def revision_with_privacy_race(*args):
        nonlocal revision_calls
        revision_calls += 1
        if revision_calls == 2:
            contribution_api_harness.db.execute(
                "UPDATE repositories SET is_private=1 WHERE key_bi='repo:race'"
            )
            contribution_api_harness.db.commit()
        value = await revision(*args)
        revision_values.append(value)
        return value

    contribution_api_harness.namespace[
        "_contribution_profile_revision"
    ] = revision_with_privacy_race

    response = contribution_api_harness.api()

    assert revision_calls >= 4
    assert revision_values[0] != revision_values[1]
    assert response["data"]["total"] == 0
    assert len(contribution_api_harness.cache_puts) == 1
    put_key = contribution_api_harness.cache_puts[0][0]
    assert revision_values[0] not in put_key
    assert revision_values[-1] in put_key
    assert response["cacheControl"] == "no-store, max-age=0, must-revalidate"


def test_language_name_colors_are_stable_for_stored_language_rows():
    color = getattr(contributions, "language_color", None)
    assert callable(color), "missing language-name color helper"
    assert color("C++") == "#f34b7d"
    assert color("TypeScript") == "#3178c6"
    assert color("Other") == "#8b949e"
    assert color("Unrecognized") == "#8b949e"
