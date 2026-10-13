"""Read-only Solana plumbing for the ForkMesh relay.

Self-contained helpers for public-address encoding and a fail-over JSON-RPC
client across keyless public endpoints. Transaction construction, private-key
import, signing, price speculation, and broadcasting deliberately do not exist
in the Worker bundle.

Split out of ``entry.py`` (adhoc #215). These functions depend only on the
stdlib and the Worker runtime's ``js``/``pyodide`` bridge — never on other
``entry`` state — so the import is strictly one-directional (``entry`` imports
from here). Amount formatting and the ``solana:`` pay-URI stay in ``entry``.
"""

import base64
import json

from js import Object
from pyodide.ffi import to_js as _to_js


def _cron_subrequest_charge(count=1):
    del count


def _bind_runtime(runtime):
    """Receive the task-local Free-plan subrequest guard from entry.py."""
    global _cron_subrequest_charge
    callback = runtime.get("_cron_subrequest_charge")
    if callable(callback):
        _cron_subrequest_charge = callback


def to_js(value):
    return _to_js(value, dict_converter=Object.fromEntries)


BASE58_ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"

def _base58_encode(data):
    n = int.from_bytes(data, "big")
    out = ""
    while n:
        n, rem = divmod(n, 58)
        out = BASE58_ALPHABET[rem] + out
    pad = 0
    for b in data:
        if b == 0:
            pad += 1
        else:
            break
    return "1" * pad + (out or "1")


def _b64url_encode(data):
    return base64.urlsafe_b64encode(data).decode().rstrip("=")


# Reliability: the canonical public RPC (api.mainnet-beta.solana.com) rate-limits
# / blocks datacenter (Cloudflare) egress, which can make public balance and
# finality reads unavailable. We fail over across several keyless public
# endpoints, and an operator can prepend their OWN node (or a keyed provider)
# via SOLANA_RPC_URL (space/comma separated).
_SOLANA_PUBLIC_RPCS = (
    "https://solana-rpc.publicnode.com",
    "https://rpc.ankr.com/solana",
    "https://solana.drpc.org",
    "https://api.mainnet-beta.solana.com",
)
_SOLANA_TEST_RPCS = {
    "devnet": ("https://api.devnet.solana.com",),
    "testnet": ("https://api.testnet.solana.com",),
}
# Remember the endpoint that last answered so we hit it first instead of
# re-walking dead hosts on every poll.
_SOLANA_RPC_PREFERRED = {"url": ""}
SOLANA_RPC_TIMEOUT_MS = 2500
# Defense in depth: the shared Worker RPC helper is incapable of submitting a
# transaction or invoking an arbitrary method, even if a future caller passes a
# dynamic string. Add new entries only for independently reviewed public reads.
_SOLANA_READ_ONLY_METHODS = frozenset({
    "getAccountInfo",
    "getBalance",
    "getGenesisHash",
    "getLatestBlockhash",
    # Newest-first signature listing; the wallet chest chip reads only the
    # most recent entry's blockTime to bucket transaction recency.
    "getSignaturesForAddress",
    "getSignatureStatuses",
    "getTransaction",
})


def _solana_endpoints(env):
    endpoints = []
    configured = (getattr(env, "SOLANA_RPC_URL", "") or "").replace(",", " ").split()
    for part in configured:
        part = part.strip()
        if part and part not in endpoints:
            endpoints.append(part)
    network = str(getattr(env, "SOLANA_NETWORK", "") or "").strip().lower()
    defaults = _SOLANA_TEST_RPCS.get(network, _SOLANA_PUBLIC_RPCS)
    for default in defaults:
        if default not in endpoints:
            endpoints.append(default)
    # Try the last-good endpoint first.
    preferred = _SOLANA_RPC_PREFERRED["url"]
    if preferred in endpoints:
        endpoints.remove(preferred)
        endpoints.insert(0, preferred)
    return endpoints


# Custodial deposit sweeps are the one flow allowed to submit a transaction.
# It stays a separate name from _solana_rpc so the read-only helper keeps its
# "cannot ever send" property: a caller must ask for the write path by name.
_SOLANA_WRITE_METHODS = frozenset({"sendTransaction"})


async def _solana_rpc_write(env, method, params):
    """Submit a signed transaction. Only for custodial deposit sweeps."""
    if method not in _SOLANA_WRITE_METHODS:
        return None
    return await _solana_rpc_call(env, method, params)


async def _solana_rpc(env, method, params):
    if method not in _SOLANA_READ_ONLY_METHODS:
        return None
    return await _solana_rpc_call(env, method, params)


async def _solana_rpc_call(env, method, params):
    from js import AbortSignal
    from js import fetch as js_fetch
    payload = json.dumps(
        {"jsonrpc": "2.0", "id": 1, "method": method, "params": params})
    for endpoint in _solana_endpoints(env):
        try:
            _cron_subrequest_charge()
            resp = await js_fetch(
                endpoint,
                to_js({
                    "method": "POST",
                    "headers": {
                        "content-type": "application/json",
                        "accept": "application/json",
                    },
                    "body": payload,
                    # A native signal bounds both headers and body without
                    # creating a nested Pyodide asyncio task.
                    "signal": AbortSignal.timeout(SOLANA_RPC_TIMEOUT_MS),
                }),
            )
            if not (200 <= int(getattr(resp, "status", 0)) < 300):
                continue
            data = json.loads(await resp.text())
        except Exception:
            continue
        # A well-formed JSON-RPC reply carries "result"; anything else (including
        # a rate-limit error object) means try the next endpoint.
        if isinstance(data, dict) and "result" in data:
            _SOLANA_RPC_PREFERRED["url"] = endpoint
            return data
    return None
