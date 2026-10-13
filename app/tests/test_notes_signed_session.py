import ast
import asyncio
import hashlib
import json
from pathlib import Path
from urllib.parse import parse_qs, quote, urlparse


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
SOURCE = ENTRY.read_text(encoding="utf-8")
FUNCTIONS = {"_plugin_scope_signed_session", "_signed_account_session"}
CLASSES = {"_NotesRuntime"}
CONSTANTS = {"NOTES_REQUEST_PROOF"}
NOTE_ID = "b" * 32
TIMESTAMP = "1700000000000"


class _BaseRuntime:
    def __init__(self, env, request):
        self.env = env
        self.request = request

    def same_origin(self):
        return False


def _load(record=None, session=("", None)):
    tree = ast.parse(SOURCE, filename=str(ENTRY))
    selected = []
    for node in tree.body:
        if isinstance(node, ast.AsyncFunctionDef) and node.name in FUNCTIONS:
            selected.append(node)
        elif isinstance(node, ast.ClassDef) and node.name in CLASSES:
            selected.append(node)
        elif isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id in CONSTANTS
            for target in node.targets
        ):
            selected.append(node)

    async def verify(_pubkey, signature, canonical):
        return signature == hashlib.sha256(canonical).hexdigest()

    async def account_row(_env, name):
        return "bi:" + name, record or {
            "name": name, "status": "active", "kind": "user"}

    async def signing_pubkeys(_env, _account_bi, _record):
        return ["account-key"]

    async def account_session_record(_env, _request, _data):
        return session

    async def sha256_hex(value):
        return hashlib.sha256(str(value).encode("utf-8")).hexdigest()

    namespace = {
        "re": __import__("re"),
        "json": json,
        "urlparse": urlparse,
        "parse_qs": parse_qs,
        "MAX_NODE_NAME": 64,
        "method_name": lambda request: request.method,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "_ts_ok": lambda value: value == TIMESTAMP,
        "ed25519_verify": verify,
        "_account_row": account_row,
        "_account_signing_pubkeys": signing_pubkeys,
        "_account_kind": lambda value: value.get("kind", "node"),
        "_account_session_record": account_session_record,
        "_WorldCommunityRuntime": _BaseRuntime,
        "sha256_hex": sha256_hex,
    }
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[])), str(ENTRY), "exec"),
        namespace)
    return namespace


class Request:
    def __init__(self, method, url, body="", headers=None):
        self.method = method
        self.url = url
        self.body = body
        self.headers = dict(headers or {})
        if body:
            self.headers["content-length"] = str(len(body.encode("utf-8")))

    async def text(self):
        return self.body


def signed_request(namespace, method, path, body="", signed_body=None):
    node = "alice"
    resource = method + "\n" + path
    if method != "GET":
        signed = body if signed_body is None else signed_body
        resource += "\n" + hashlib.sha256(signed.encode()).hexdigest()
    canonical = (namespace["NOTES_REQUEST_PROOF"] + "\n" + node + "\n"
                 + resource + "\n" + TIMESTAMP)
    signature = hashlib.sha256(canonical.encode()).hexdigest()
    return Request(method, "https://forkmesh.test" + path + "?node=" + node
                   + "&ts=" + TIMESTAMP + "&sig=" + quote(signature), body)


async def _resolve(namespace, request):
    runtime = namespace["_NotesRuntime"](object(), request)
    data = {}
    if request.method != "GET":
        data, error = await runtime.json_body(1 << 20)
        assert error == ""
    return runtime, await runtime.session(data)


def test_key_signed_desktop_lists_and_edits_notes_without_a_password_session():
    namespace = _load()
    operations = (
        ("GET", "/api/notes", ""),
        ("POST", "/api/notes", '{"title":"a"}'),
        ("GET", "/api/notes/" + NOTE_ID, ""),
        ("PATCH", "/api/notes/" + NOTE_ID, '{"title":"b","version":2}'),
        ("DELETE", "/api/notes/" + NOTE_ID, ""),
        ("GET", "/api/notes/" + NOTE_ID + "/versions", ""),
        ("POST", "/api/notes/" + NOTE_ID + "/shares", '{"name":"bob"}'),
    )
    for method, path, body in operations:
        runtime, (account_bi, record) = asyncio.run(_resolve(
            namespace, signed_request(namespace, method, path, body)))
        assert account_bi == "bi:alice", (method, path)
        assert record["name"] == "alice"
        assert runtime.same_origin()


def test_signature_is_bound_to_method_path_and_body():
    namespace = _load()
    crossed = signed_request(namespace, "GET", "/api/notes/" + NOTE_ID)
    crossed.method = "DELETE"
    assert asyncio.run(_resolve(namespace, crossed))[1] == ("", None)

    moved = signed_request(namespace, "GET", "/api/notes")
    moved.url = moved.url.replace("/api/notes", "/api/notes/" + NOTE_ID)
    assert asyncio.run(_resolve(namespace, moved))[1] == ("", None)

    tampered = signed_request(namespace, "PATCH", "/api/notes/" + NOTE_ID,
                              '{"title":"evil"}', '{"title":"b"}')
    assert asyncio.run(_resolve(namespace, tampered))[1] == ("", None)

    elsewhere = signed_request(namespace, "GET", "/api/calendar")
    assert asyncio.run(_resolve(namespace, elsewhere))[1] == ("", None)


def test_device_proof_requires_an_active_user_account():
    namespace = _load({"name": "alice", "status": "disabled", "kind": "user"})
    request = signed_request(namespace, "GET", "/api/notes")
    assert asyncio.run(_resolve(namespace, request))[1] == ("", None)


def test_password_session_still_wins_and_unsigned_requests_are_refused():
    session = ("bi:carol", {"name": "carol"})
    namespace = _load(session=session)
    request = Request("GET", "https://forkmesh.test/api/notes",
                      headers={"authorization": "Bearer token"})
    runtime, resolved = asyncio.run(_resolve(namespace, request))
    assert resolved == session
    assert runtime.same_origin()

    bare = _load()
    runtime, resolved = asyncio.run(_resolve(
        bare, Request("POST", "https://forkmesh.test/api/notes", "{}")))
    assert resolved == ("", None)
    assert not runtime.same_origin()
