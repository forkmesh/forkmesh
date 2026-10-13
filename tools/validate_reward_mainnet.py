#!/usr/bin/env python3
"""Read-only Solana mainnet reward-pool preflight.

This tool never accepts a private key, builds a transaction, signs, simulates,
or broadcasts. It verifies the RPC cluster, confirms the configured public
account exists, and reads its finalized public balance.
"""

from __future__ import annotations

import argparse
import json
import re
from urllib.parse import urlparse
from urllib.request import Request, urlopen


MAINNET_GENESIS_HASH = (
    "5eykt4UsFv8P8NJdTREpY1vzqKqZKvdpKuc147dw2N9d"
)
SOLANA_ADDRESS_RE = re.compile(r"^[1-9A-HJ-NP-Za-km-z]{32,44}$")


def validate_rpc_url(value: str) -> str:
    raw = str(value or "").strip()
    parsed = urlparse(raw)
    if (
        parsed.scheme.lower() != "https"
        or not parsed.hostname
        or parsed.username
        or parsed.password
        or parsed.query
        or parsed.fragment
    ):
        raise ValueError(
            "RPC must be credential-free HTTPS without query or fragment")
    if parsed.hostname.lower() in {
        "api.devnet.solana.com", "api.testnet.solana.com",
    }:
        raise ValueError("development/test RPC cannot validate mainnet")
    return raw


def rpc_call(rpc_url, method, params, *, opener=urlopen):
    body = json.dumps({
        "jsonrpc": "2.0",
        "id": 1,
        "method": method,
        "params": params,
    }, separators=(",", ":")).encode()
    request = Request(
        validate_rpc_url(rpc_url),
        data=body,
        headers={
            "content-type": "application/json",
            "accept": "application/json",
            "user-agent": "forkmesh-mainnet-readonly-preflight/1",
        },
        method="POST",
    )
    with opener(request, timeout=15) as response:
        if int(getattr(response, "status", 200)) != 200:
            raise RuntimeError("RPC returned non-200 status")
        value = json.loads(response.read().decode())
    if not isinstance(value, dict) or value.get("error"):
        raise RuntimeError("RPC returned an error")
    return value.get("result")


def validate_mainnet_pool(rpc_url, pool_address, *, opener=urlopen):
    address = str(pool_address or "").strip()
    if not SOLANA_ADDRESS_RE.fullmatch(address):
        raise ValueError("invalid public Solana pool address")
    genesis = rpc_call(rpc_url, "getGenesisHash", [], opener=opener)
    if genesis != MAINNET_GENESIS_HASH:
        raise RuntimeError("RPC genesis hash is not Solana mainnet-beta")
    account = rpc_call(
        rpc_url,
        "getAccountInfo",
        [address, {"encoding": "base64", "commitment": "finalized"}],
        opener=opener,
    )
    if not isinstance(account, dict) or not isinstance(
            account.get("value"), dict):
        raise RuntimeError("public pool account does not exist on mainnet")
    balance = rpc_call(
        rpc_url,
        "getBalance",
        [address, {"commitment": "finalized"}],
        opener=opener,
    )
    if not isinstance(balance, dict) or not isinstance(
            balance.get("value"), int):
        raise RuntimeError("could not read finalized public pool balance")
    return {
        "ok": True,
        "network": "mainnet-beta",
        "rpcOrigin": (
            urlparse(validate_rpc_url(rpc_url)).scheme
            + "://"
            + urlparse(validate_rpc_url(rpc_url)).netloc
        ),
        "poolAddress": address,
        "balanceLamports": balance["value"],
        "readOnly": True,
        "transactionCreated": False,
        "signed": False,
        "broadcast": False,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--rpc-url", default="https://api.mainnet-beta.solana.com")
    parser.add_argument("--pool-address", required=True)
    args = parser.parse_args(argv)
    print(json.dumps(
        validate_mainnet_pool(args.rpc_url, args.pool_address),
        sort_keys=True,
    ))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
