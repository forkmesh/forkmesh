"""Focused tests for the shared Discord rate-limit/catalog coordinator."""

import ast
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

from discord_rate import DiscordRateCoordinator  # noqa: E402


def test_worker_entry_imports_discord_dependencies_before_using_them():
    source = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
    tree = ast.parse(source)
    assert 'organization_discord = _LazyModule("organization_discord")' in source
    coordinator = next(
        node for node in tree.body
        if isinstance(node, ast.FunctionDef)
        and node.name == "_discord_rate_coordinator"
    )
    assignment = next(
        node for node in tree.body
        if isinstance(node, ast.Assign)
        and any(
            isinstance(target, ast.Name)
            and target.id == "_discord_rate_coordinator_instance"
            for target in node.targets
        )
    )
    body = ast.get_source_segment(source, coordinator)
    assert '_LazyModule("discord_rate").DiscordRateCoordinator()' in body
    assert assignment.lineno < coordinator.lineno


def test_bucket_reset_and_429_block_only_the_matching_provider_bucket():
    coordinator = DiscordRateCoordinator()
    path = "/guilds/100000000000000001/channels"
    other = "/guilds/100000000000000001/roles"
    coordinator.observe(path, 200, {
        "x-ratelimit-bucket": "catalog-a",
        "x-ratelimit-remaining": "0",
        "x-ratelimit-reset-after": "1.5",
    }, [], 1_000)
    assert coordinator.reserve(path, 1_200) == 1_300
    assert coordinator.reserve(other, 1_200) == 0
    delay = coordinator.observe(path, 429, {
        "x-ratelimit-bucket": "catalog-a",
        "retry-after": "2",
    }, {"retry_after": 1.0}, 2_500)
    assert delay == 2_000
    assert coordinator.reserve(path, 3_000) == 1_500


def test_global_429_blocks_every_path_and_catalog_is_short_lived():
    coordinator = DiscordRateCoordinator()
    first = "/guilds/100000000000000001/channels"
    second = "/channels/200000000000000001/messages?limit=50"
    delay = coordinator.observe(first, 429, {
        "x-ratelimit-global": "true",
        "retry-after": "3",
    }, {"global": True, "retry_after": 3}, 10_000)
    assert delay == 3_000
    assert coordinator.reserve(second, 10_001) == 2_999
    payload = [{"id": "channel"}]
    coordinator.catalog_put(first, payload, 20_000)
    assert coordinator.catalog_get(first, 20_100) == payload
    assert coordinator.catalog_get(first, 25_001) is None


def test_429_without_provider_bucket_uses_a_safe_path_fallback():
    coordinator = DiscordRateCoordinator()
    path = "/channels/200000000000000001/messages?limit=50"
    delay = coordinator.observe(path, 429, {}, {}, 4_000)
    assert delay == 1_000
    assert coordinator.reserve(path, 4_500) == 500


def test_absolute_reset_and_global_scope_are_honored():
    coordinator = DiscordRateCoordinator()
    path = "/guilds/100000000000000001/channels"
    delay = coordinator.observe(path, 200, {
        "x-ratelimit-bucket": "catalog-a",
        "x-ratelimit-remaining": "0",
        "x-ratelimit-reset": "12.5",
    }, [], 10_000)
    assert delay == 2_500
    assert coordinator.reserve(path, 11_000) == 1_500
    global_delay = coordinator.observe(path, 429, {
        "x-ratelimit-scope": "global",
        "retry-after": "1",
    }, {}, 20_000)
    assert global_delay == 1_000
    assert coordinator.reserve("/guilds/100000000000000002/roles", 20_100) == 900
