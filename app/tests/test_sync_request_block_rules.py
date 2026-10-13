import importlib.util
from pathlib import Path
import sys

import pytest


MODULE_PATH = (
    Path(__file__).resolve().parents[1] /
    "tools" / "sync_request_block_rules.py")
SPEC = importlib.util.spec_from_file_location("sync_request_block_rules", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def test_expression_supports_every_database_rule_type():
    expression = MODULE.build_expression([
        {"match_type": "user_agent_keyword", "pattern": "SampleCrawler"},
        {"match_type": "ip", "pattern": "203.0.113.7"},
        {"match_type": "cidr", "pattern": "198.51.100.8/24"},
        {"match_type": "ip_range", "pattern": "192.0.2.10-192.0.2.20"},
    ])
    assert 'lower(http.user_agent) contains "samplecrawler"' in expression
    assert (
        'not (cf.client.bot and lower(http.user_agent) contains "google")'
        in expression
    )
    assert "203.0.113.7" in expression
    assert "198.51.100.0/24" in expression
    assert "192.0.2.10..192.0.2.20" in expression


def test_verified_google_exception_applies_only_to_user_agent_rules():
    agent_expression = MODULE.build_expression([
        {"match_type": "user_agent_keyword", "pattern": "bot"},
        {"match_type": "user_agent_keyword", "pattern": "spider"},
    ])
    assert agent_expression == (
        '(((lower(http.user_agent) contains "bot") or '
        '(lower(http.user_agent) contains "spider")) and not '
        '(cf.client.bot and lower(http.user_agent) contains "google") and not '
        '(lower(http.user_agent) contains "chatgpt-user" or '
        'lower(http.user_agent) contains "oai-searchbot"))'
    )

    address_expression = MODULE.build_expression([
        {"match_type": "ip", "pattern": "203.0.113.7"},
    ])
    assert "cf.client.bot" not in address_expression


def test_empty_database_has_no_waf_expression():
    assert MODULE.build_expression([]) == ""


def test_invalid_enabled_rule_is_rejected_instead_of_omitted():
    with pytest.raises(ValueError):
        MODULE.build_expression([
            {"match_type": "cidr", "pattern": "invalid-network"},
        ])


def test_wrangler_json_parser_extracts_rows_without_logging_patterns():
    raw = '''INFO wrapper\n[{"results":[{"match_type":"ip",\
"pattern":"203.0.113.9"}],"success":true}]'''
    assert MODULE.wrangler_rows(raw) == [
        {"match_type": "ip", "pattern": "203.0.113.9"}]


def test_rqlite_associative_json_yields_rows():
    raw = ('{"results":[{"types":{"match_type":"text","pattern":"text"},'
           '"rows":[{"match_type":"ip","pattern":"203.0.113.9"},'
           '{"match_type":"user_agent_keyword","pattern":"samplecrawler"}]}]}')
    assert MODULE.wrangler_rows(raw) == [
        {"match_type": "ip", "pattern": "203.0.113.9"},
        {"match_type": "user_agent_keyword", "pattern": "samplecrawler"}]


def test_rqlite_empty_table_yields_no_rows():
    assert MODULE.wrangler_rows('{"results":[{"types":{}}]}') == []


def test_database_error_is_raised_instead_of_publishing_an_empty_policy():
    with pytest.raises(ValueError):
        MODULE.wrangler_rows(
            '{"results":[{"error":"no such table: request_block_rules"}]}')


def test_shell_entry_point_reads_the_rqlite_table_not_d1():
    script = (MODULE_PATH.parents[1] / "sync_request_block_rules.sh").read_text(
        encoding="utf-8")
    assert "--rqlite" in script
    assert "d1 execute" not in script
    assert "FROM request_block_rules" in MODULE.ENABLED_RULES_SQL
    assert "enabled=1" in MODULE.ENABLED_RULES_SQL


def test_repository_shortcut_runs_the_rqlite_sync_with_arguments():
    shortcut = (MODULE_PATH.parents[2] / "tools"
                / "forkmesh-sync-block-rules.sh")
    script = shortcut.read_text(encoding="utf-8")
    assert "app/tools/sync_request_block_rules.py" in script
    assert '--rqlite "$@"' in script
    assert shortcut.stat().st_mode & 0o111


def test_dry_run_prints_the_expression_without_credentials(monkeypatch,
                                                            capsys):
    monkeypatch.setattr(MODULE, "rqlite_rows", lambda: [
        {"match_type": "user_agent_keyword", "pattern": "bot"}])
    monkeypatch.setattr(MODULE, "sync", lambda *_args: pytest.fail("synced"))
    monkeypatch.setattr(sys, "argv", ["sync", "--rqlite", "--dry-run"])
    assert MODULE.main() == 0
    out = capsys.readouterr().out
    assert "agent-notes" not in out and "agent-guestbook" not in out
    assert "would publish 1 enabled database rule(s)" in out


def test_missing_legacy_env_file_falls_back_to_fleet_cloudflare_env(
        monkeypatch, tmp_path):
    monkeypatch.setenv("FMNEXT_CONFIG_DIR", str(tmp_path))
    legacy = MODULE_PATH.parents[1] / ".env.production"
    if legacy.exists():
        pytest.skip("a local .env.production takes precedence")
    assert MODULE.default_env_file() == tmp_path / "cloudflare.env"
