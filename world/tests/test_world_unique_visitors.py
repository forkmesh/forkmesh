"""Privacy and cardinality tests for the Arrival Grid unique counter."""

import hashlib
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2] / "app"
sys.path.insert(0, str(ROOT / "src"))

import world_visitors as visitors  # noqa: E402


def _digest(value):
    return hashlib.sha256(str(value).encode()).hexdigest()


def _rows(values):
    merged = {}
    for value in values:
        register_id, rank = visitors.hll_register(_digest(value))
        merged[register_id] = max(merged.get(register_id, 0), rank)
    return [
        {"register_id": register_id, "rank": rank}
        for register_id, rank in merged.items()
    ]


def test_generalized_agent_discards_versions_models_and_build_details():
    chrome_a = (
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 Chrome/140.0.7212.9 Safari/537.36")
    chrome_b = (
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64; SecretModel/123) "
        "AppleWebKit/537.36 Chrome/999.8.7.6 Safari/537.36")
    assert visitors.generalized_user_agent(chrome_a) == (
        "chromium:windows:desktop")
    assert visitors.generalized_user_agent(chrome_b) == (
        "chromium:windows:desktop")
    assert visitors.generalized_user_agent(
        "Mozilla/5.0 (iPhone) Version/18.0 Mobile Safari/604.1"
    ) == "safari:ios:mobile"
    assert visitors.generalized_user_agent(
        "Googlebot/2.1 (+https://www.google.com/bot.html)"
    ) == "automated:other-os:desktop"
    assert visitors.generalized_user_agent("") == "unknown-agent"


def test_edge_address_requires_one_real_ip_and_canonicalizes_transiently():
    assert visitors.canonical_edge_address("203.0.113.7") == "203.0.113.7"
    assert visitors.canonical_edge_address(
        "2001:0db8:0000:0000:0000:0000:0000:0001"
    ) == "2001:db8::1"
    for unsafe in (
        "",
        "unknown",
        "example.com",
        "203.0.113.7:443",
        "203.0.113.7, 10.0.0.1",
        "fe80::1%eth0",
        "x" * 65,
    ):
        assert visitors.canonical_edge_address(unsafe) == ""


def test_hll_projection_is_deterministic_bounded_and_rejects_raw_values():
    token = _digest("opaque")
    assert visitors.hll_register(token) == visitors.hll_register(token)
    register_id, rank = visitors.hll_register(token)
    assert 0 <= register_id < visitors.HLL_REGISTER_COUNT
    assert 1 <= rank <= visitors.HLL_MAX_RANK
    for invalid in ("", "127.0.0.1", "Mozilla/5.0", "a" * 63, "g" * 64):
        with pytest.raises(ValueError):
            visitors.hll_register(invalid)


def test_hll_deduplicates_repeats_and_is_accurate_at_plaque_scale():
    one = _rows(["same visitor"] * 100)
    assert visitors.hll_estimate(one) == 1

    thousand = _rows(range(1000))
    estimate = visitors.hll_estimate(thousand)
    assert 900 <= estimate <= 1100

    # Merging repeated register updates is idempotent.
    assert visitors.hll_estimate(thousand + thousand) == estimate


def test_unique_summary_is_aggregate_only_and_zero_safe():
    result = visitors.unique_visit_summary({
        "total": _rows(range(25)),
        "today": _rows(range(7)),
        "pastHour": _rows(range(2)),
    })
    assert set(result) == {
        "total",
        "today",
        "yesterday",
        "yesterdaySameTime",
        "pastHour",
        "pastHourYesterday",
    }
    assert 23 <= result["total"] <= 27
    assert result["today"] == 7
    assert result["pastHour"] == 2
    assert result["yesterday"] == 0


def test_unique_window_boundaries_are_utc_and_match_comparisons():
    day = 24 * 60 * 60 * 1000
    hour = 60 * 60 * 1000
    now = 3 * day + 5 * hour + 30 * 60 * 1000
    windows = visitors.unique_visit_window_bounds(now)
    assert windows == {
        "today": (3 * day, None),
        "yesterday": (2 * day, 3 * day),
        "yesterdaySameTime": (2 * day, 2 * day + 5 * hour + 30 * 60 * 1000),
        "pastHour": (now - hour, None),
        "pastHourYesterday": (now - day - hour, now - day),
    }


def test_unique_visitor_migration_is_fixed_footprint_and_identifier_free():
    migration = (
        ROOT / "migrations" / "0077_world_unique_visitors.sql"
    ).read_text(encoding="utf-8")
    ddl = migration.lower()
    assert "world_visit_unique_hll" in ddl
    assert "primary key (bucket_start, register_id)" in ddl
    assert "register_id < 1024" in ddl
    assert "rank <= 247" in ddl
    for forbidden_column in (
        "ip_address", "user_agent", "visitor_token", "account_id", "country",
    ):
        assert forbidden_column not in ddl
