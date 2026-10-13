"""Resource- and body-bound desktop authentication for calendar requests."""

import ast
import asyncio
import hashlib
from pathlib import Path
from urllib.parse import parse_qs, quote, urlparse


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
SOURCE = ENTRY.read_text(encoding="utf-8")
FUNCTION = "_calendar_signed_session"
RESOLVER = "_signed_account_session"
CONSTANTS = {
    "CALENDAR_LIST_PROOF",
    "CALENDAR_CREATE_PROOF",
    "CALENDAR_READ_PROOF",
    "CALENDAR_UPDATE_PROOF",
    "CALENDAR_DELETE_PROOF",
    "CALENDAR_RSVP_PROOF",
    "CALENDAR_COLLECTION_RE",
    "CALENDAR_ITEM_RE",
    "CALENDAR_RSVP_RE",
}
EVENT_ID = "a" * 32
BODY_DIGEST = "d" * 64


def _load(record=None):
    tree = ast.parse(SOURCE, filename=str(ENTRY))
    selected = []
    for node in tree.body:
        if isinstance(node, ast.AsyncFunctionDef) and node.name in (
                FUNCTION, RESOLVER):
            selected.append(node)
        elif isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id in CONSTANTS
            for target in node.targets
        ):
            selected.append(node)

    opened = []

    async def verify(_pubkey, signature, canonical):
        return signature == hashlib.sha256(canonical).hexdigest()

    async def account_row(_env, name):
        value = record or {
            "name": name, "status": "active", "kind": "user"}
        opened.append(name)
        return "bi:" + name, value

    async def signing_pubkeys(_env, _account_bi, _record):
        return ["account-key"]

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


def signed_request(namespace, method, path, proof, resource=""):
    node = "alice"
    timestamp = "1700000000000"
    canonical = namespace[proof] + "\n" + node + "\n"
    if resource:
        canonical += resource + "\n"
    signature = hashlib.sha256((canonical + timestamp).encode()).hexdigest()
    return Request(method, "https://forkmesh.test" + path + "?node=" + node +
                   "&ts=" + timestamp + "&sig=" + quote(signature))


def test_each_calendar_operation_accepts_only_its_matching_device_proof():
    namespace = _load()
    resolve = namespace[FUNCTION]
    operations = (
        ("GET", "/api/calendar", "CALENDAR_LIST_PROOF", "", ""),
        ("POST", "/api/calendar", "CALENDAR_CREATE_PROOF",
         BODY_DIGEST, BODY_DIGEST),
        ("GET", "/api/calendar/" + EVENT_ID, "CALENDAR_READ_PROOF",
         EVENT_ID, ""),
        ("PATCH", "/api/calendar/" + EVENT_ID, "CALENDAR_UPDATE_PROOF",
         EVENT_ID + "\n" + BODY_DIGEST, BODY_DIGEST),
        ("DELETE", "/api/calendar/" + EVENT_ID, "CALENDAR_DELETE_PROOF",
         EVENT_ID, ""),
        ("POST", "/api/calendar/" + EVENT_ID + "/rsvp",
         "CALENDAR_RSVP_PROOF", EVENT_ID + "\n" + BODY_DIGEST, BODY_DIGEST),
    )
    for method, path, proof, resource, digest in operations:
        account_bi, record = asyncio.run(resolve(
            object(), signed_request(namespace, method, path, proof, resource),
            digest))
        assert account_bi == "bi:alice"
        assert record["name"] == "alice"

    crossed = signed_request(
        namespace, "DELETE", "/api/calendar/" + EVENT_ID,
        "CALENDAR_UPDATE_PROOF", EVENT_ID)
    assert asyncio.run(resolve(object(), crossed)) == ("", None)

    update = signed_request(
        namespace, "PATCH", "/api/calendar/" + EVENT_ID,
        "CALENDAR_UPDATE_PROOF", EVENT_ID + "\n" + BODY_DIGEST)
    assert asyncio.run(resolve(object(), update, "e" * 64)) == ("", None)


def test_calendar_device_proof_requires_an_active_user_account():
    namespace = _load({"name": "alice", "status": "disabled", "kind": "user"})
    request = signed_request(namespace, "GET", "/api/calendar",
                             "CALENDAR_LIST_PROOF")
    assert asyncio.run(namespace[FUNCTION](object(), request)) == ("", None)
