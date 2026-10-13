#!/usr/bin/env python3
"""Production mainnet defaults and fail-closed reward cluster validation."""

import ast
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import urlparse

import pytest


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
SOURCE = ENTRY.read_text(encoding="utf-8")

spec = importlib.util.spec_from_file_location(
    "validate_reward_mainnet",
    ROOT.parent / "tools" / "validate_reward_mainnet.py",
)
preflight = importlib.util.module_from_spec(spec)
spec.loader.exec_module(preflight)


def _load_config_helpers():
    wanted = {"_reward_network", "_reward_rpc_url"}
    nodes = [
        node for node in ast.parse(SOURCE).body
        if isinstance(node, ast.FunctionDef) and node.name in wanted
    ]
    namespace = {
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "urlparse": urlparse,
        "REWARD_CLUSTER_GENESIS_HASHES": {
            "mainnet-beta": "main",
            "devnet": "dev",
            "testnet": "test",
        },
        "REWARD_CANONICAL_RPC_HOSTS": {
            "api.mainnet-beta.solana.com": "mainnet-beta",
            "api.devnet.solana.com": "devnet",
            "api.testnet.solana.com": "testnet",
        },
    }
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=nodes, type_ignores=[])), str(ENTRY), "exec"),
         namespace)
    return namespace


def test_production_defaults_mainnet_and_invalid_or_mismatched_config_fails():
    helpers = _load_config_helpers()
    default = SimpleNamespace(
        COMMUNITY_REWARD_RPC_URL="https://api.mainnet-beta.solana.com")
    assert helpers["_reward_network"](default) == "mainnet-beta"
    assert helpers["_reward_rpc_url"](default) == (
        "https://api.mainnet-beta.solana.com")

    invalid = SimpleNamespace(
        COMMUNITY_REWARD_NETWORK="invalid",
        COMMUNITY_REWARD_RPC_URL="https://api.mainnet-beta.solana.com")
    assert helpers["_reward_network"](invalid) == ""
    assert helpers["_reward_rpc_url"](invalid) == ""

    mismatch = SimpleNamespace(
        COMMUNITY_REWARD_NETWORK="mainnet-beta",
        COMMUNITY_REWARD_RPC_URL="https://api.devnet.solana.com")
    assert helpers["_reward_rpc_url"](mismatch) == ""

    explicit_dev = SimpleNamespace(
        COMMUNITY_REWARD_NETWORK="devnet",
        COMMUNITY_REWARD_RPC_URL="https://api.devnet.solana.com")
    assert helpers["_reward_network"](explicit_dev) == "devnet"
    assert helpers["_reward_rpc_url"](explicit_dev).endswith(
        "api.devnet.solana.com")


def test_cluster_fingerprints_are_complete_canonical_genesis_hashes():
    expected = {
        "mainnet-beta": "5eykt4UsFv8P8NJdTREpY1vzqKqZKvdpKuc147dw2N9d",
        "devnet": "EtWTRABZaYq6iMfeYKouRu166VU2xqa1wcaWoxPkrZBG",
        "testnet": "4uhcVJyU9pJkvQyS88uRDiswHXSCkY3zQawwpjk2NsNY",
    }
    tree = ast.parse(SOURCE)
    assignment = next(
        node for node in tree.body
        if isinstance(node, ast.Assign)
        and any(
            isinstance(target, ast.Name)
            and target.id == "REWARD_CLUSTER_GENESIS_HASHES"
            for target in node.targets
        )
    )
    assert ast.literal_eval(assignment.value) == expected
    assert preflight.MAINNET_GENESIS_HASH == expected["mainnet-beta"]


class _RpcResponse:
    status = 200

    def __init__(self, result):
        self.result = result

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False

    def read(self):
        return json.dumps({
            "jsonrpc": "2.0", "id": 1, "result": self.result,
        }).encode()


def test_read_only_mainnet_preflight_checks_genesis_account_and_balance():
    methods = []

    def opener(request, timeout):
        payload = json.loads(request.data)
        methods.append(payload["method"])
        if payload["method"] == "getGenesisHash":
            return _RpcResponse(preflight.MAINNET_GENESIS_HASH)
        if payload["method"] == "getAccountInfo":
            return _RpcResponse({"value": {
                "data": ["", "base64"], "executable": False,
                "lamports": 42, "owner": "11111111111111111111111111111111",
            }})
        return _RpcResponse({"value": 42})

    result = preflight.validate_mainnet_pool(
        "https://api.mainnet-beta.solana.com",
        "11111111111111111111111111111111",
        opener=opener,
    )
    assert methods == ["getGenesisHash", "getAccountInfo", "getBalance"]
    assert result["readOnly"] is True
    assert result["signed"] is False
    assert result["broadcast"] is False
    assert result["balanceLamports"] == 42


def test_read_only_preflight_rejects_wrong_cluster_or_missing_pool():
    def wrong_cluster(_request, timeout):
        return _RpcResponse("devnet-genesis")

    with pytest.raises(RuntimeError, match="not Solana mainnet"):
        preflight.validate_mainnet_pool(
            "https://rpc.example",
            "11111111111111111111111111111111",
            opener=wrong_cluster,
        )
    with pytest.raises(ValueError, match="development/test RPC"):
        preflight.validate_mainnet_pool(
            "https://api.devnet.solana.com",
            "11111111111111111111111111111111",
        )


def test_production_and_development_wrangler_networks_are_explicit():
    import tomllib

    config = tomllib.loads(
        (ROOT / "wrangler.toml").read_text(encoding="utf-8"))
    # ForkMesh Next starts with Solana rewards off mainnet (docs/PLAN.md:
    # integrations start off), so its manifest names devnet explicitly.
    assert config["vars"]["COMMUNITY_REWARD_NETWORK"] == "devnet"
    assert config["vars"]["COMMUNITY_REWARD_RPC_URL"] == (
        "https://api.devnet.solana.com")
    assert config["env"]["dev"]["vars"]["COMMUNITY_REWARD_NETWORK"] == "devnet"
    assert config["env"]["dev"]["vars"]["COMMUNITY_REWARD_RPC_URL"] == (
        "https://api.devnet.solana.com")
