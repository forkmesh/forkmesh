"""Exact method, path, and body binding for native plugin-API authentication.

The generic resolver lives in entry.py; each packaged plugin binds it to its
own REQUEST_PROOF and PREFIX (read here from the crm package's worker module).
"""

import ast
import asyncio
import hashlib
from pathlib import Path
from urllib.parse import parse_qs, quote, urlparse


CRM_PACKAGE = Path(__file__).resolve().parents[1]
ENTRY = CRM_PACKAGE.parents[2] / "app" / "src" / "entry.py"
SOURCE = ENTRY.read_text(encoding="utf-8")
CRM_MODULE = CRM_PACKAGE / "content" / "worker" / "crm.py"
FUNCTION = "_plugin_scope_signed_session"
LIFECYCLE = "_plugin_lifecycle_signed_session"
RESOLVER = "_signed_account_session"
NAMES = {FUNCTION, LIFECYCLE, RESOLVER}
CONSTANTS = {"PLUGIN_REQUEST_PROOF"}
BODY_DIGEST = "d" * 64


def _crm_constant(name):
    tree = ast.parse(CRM_MODULE.read_text(encoding="utf-8"))
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id == name
            for target in node.targets
        ):
            return ast.literal_eval(node.value)
    raise AssertionError(f"missing {name} in the crm package module")


def _load(record=None):
    tree = ast.parse(SOURCE, filename=str(ENTRY))
    selected = []
    for node in tree.body:
        if isinstance(node, ast.AsyncFunctionDef) and node.name in NAMES:
            selected.append(node)
        elif isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id in CONSTANTS
            for target in node.targets
        ):
            selected.append(node)

    async def verify(_pubkey, signature, canonical):
        return signature == hashlib.sha256(canonical).hexdigest()

    async def account_row(_env, name):
        value = record or {"name": name, "status": "active", "kind": "user"}
        opened.append(name)
        return "bi:" + name, value

    async def signing_pubkeys(_env, _account_bi, _record):
        return ["account-key"]

    opened = []

    namespace = {
        "re": __import__("re"),
        "urlparse": urlparse,
        "parse_qs": parse_qs,
        "MAX_NODE_NAME": 64,
        "method_name": lambda request: request.method,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "_ts_ok": lambda value: value == "1700000000000",
        "ed25519_verify": verify,
        "_account_row": account_row,
        "_account_signing_pubkeys": signing_pubkeys,
        "_account_kind": lambda value: value.get("kind", "node"),
    }
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[])), str(ENTRY), "exec"), namespace)
    namespace["opened_accounts"] = opened
    return namespace


class Request:
    def __init__(self, method, url):
        self.method = method
        self.url = url


def _signed(namespace, method, path, digest="", proof=None):
    node = "alice"
    timestamp = "1700000000000"
    resource = method + "\n" + path
    if method != "GET":
        resource += "\n" + digest
    proof = proof or _crm_constant("REQUEST_PROOF")
    canonical = (proof + "\n" + node + "\n" +
                 resource + "\n" + timestamp).encode()
    signature = hashlib.sha256(canonical).hexdigest()
    return Request(method, "https://forkmesh.test" + path + "?node=" + node +
                   "&ts=" + timestamp + "&sig=" + quote(signature))


def _crm_resolve(namespace):
    proof = _crm_constant("REQUEST_PROOF")
    prefix = _crm_constant("PREFIX")

    async def resolve(env, request, digest=""):
        return await namespace[FUNCTION](env, request, digest, proof, prefix)

    return resolve


def test_crm_device_proof_is_bound_to_exact_method_path_and_mutation_body():
    namespace = _load()
    resolve = _crm_resolve(namespace)

    get_request = _signed(namespace, "GET", "/api/crm/acme")
    assert asyncio.run(resolve(object(), get_request)) == (
        "bi:alice", {"name": "alice", "status": "active", "kind": "user"})

    create = _signed(
        namespace, "POST", "/api/crm/acme/contacts", BODY_DIGEST)
    assert asyncio.run(resolve(object(), create, BODY_DIGEST))[0] == "bi:alice"
    assert asyncio.run(resolve(object(), create, "e" * 64)) == ("", None)

    create.method = "PATCH"
    assert asyncio.run(resolve(object(), create, BODY_DIGEST)) == ("", None)

    outside = _signed(namespace, "GET", "/api/calendar")
    assert asyncio.run(resolve(object(), outside)) == ("", None)

    # One account read per resolved request. The record carries the member's
    # avatar, so opening it twice per call is what exhausted the Worker's
    # resource limits on plain package API reads.
    namespace["opened_accounts"].clear()
    assert asyncio.run(resolve(object(), get_request))[0] == "bi:alice"
    assert namespace["opened_accounts"] == ["alice"]


def test_crm_device_proof_requires_an_active_user_account():
    namespace = _load({"name": "alice", "status": "disabled", "kind": "user"})
    request = _signed(namespace, "GET", "/api/crm")
    assert asyncio.run(_crm_resolve(namespace)(object(), request)) == ("", None)


def test_plugin_lifecycle_proof_is_bound_to_its_own_api_and_constant():
    namespace = _load()
    lifecycle = namespace[LIFECYCLE]
    crm = _crm_resolve(namespace)
    assert namespace["PLUGIN_REQUEST_PROOF"] != _crm_constant("REQUEST_PROOF")

    state = _signed(namespace, "GET", "/api/plugins/acme",
                    proof=namespace["PLUGIN_REQUEST_PROOF"])
    assert asyncio.run(lifecycle(object(), state))[0] == "bi:alice"

    uninstall = _signed(namespace, "POST", "/api/plugins/acme/crm/uninstall",
                        BODY_DIGEST, proof=namespace["PLUGIN_REQUEST_PROOF"])
    assert asyncio.run(lifecycle(object(), uninstall, BODY_DIGEST))[0] == "bi:alice"
    assert asyncio.run(lifecycle(object(), uninstall, "e" * 64)) == ("", None)

    # A proof minted for one API family never authorizes the other.
    crossed = _signed(namespace, "GET", "/api/plugins/acme")
    assert asyncio.run(lifecycle(object(), crossed)) == ("", None)
    assert asyncio.run(crm(object(), crossed)) == ("", None)
    crm_scoped = _signed(namespace, "GET", "/api/crm/acme")
    assert asyncio.run(lifecycle(object(), crm_scoped)) == ("", None)
