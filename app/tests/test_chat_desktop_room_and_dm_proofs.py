import ast
import asyncio
import hashlib
from pathlib import Path
import re
import sys
from types import SimpleNamespace
from urllib.parse import parse_qs, urlparse


ROOT = Path(__file__).resolve().parents[2]
ENTRY_PATH = ROOT / "app" / "src" / "entry.py"
ENTRY = ENTRY_PATH.read_text(encoding="utf-8")
TS = "1700000000000"
sys.path.insert(0, str(ROOT / "app" / "src"))

import chat_reads_api  # noqa: E402


class _Request:
    def __init__(self, method, url):
        self.method = method
        self.url = url


def _harness():
    tree = ast.parse(ENTRY, filename=str(ENTRY_PATH))
    wanted = {
        "_chat_channel_settings_fields",
        "_chat_channel_create_fields",
        "_chat_channel_signed_session",
        "_chat_direct_signed_session",
    }
    functions = [
        node for node in tree.body
        if isinstance(node, (ast.AsyncFunctionDef, ast.FunctionDef))
        and node.name in wanted
    ]
    assert {node.name for node in functions} == wanted
    constants = {}
    for node in tree.body:
        if (
            isinstance(node, ast.Assign)
            and len(node.targets) == 1
            and isinstance(node.targets[0], ast.Name)
            and re.fullmatch(r"CHAT_(CHANNEL|DIRECT|READS)_[A-Z_]+", node.targets[0].id)
            and isinstance(node.value, ast.Constant)
        ):
            constants[node.targets[0].id] = node.value.value
    record = {"name": "alice", "status": "active", "kind": "user"}

    async def owner_pubkey(_env, name):
        return "PK-" + name if name == "alice" else ""

    async def account_row(_env, name):
        return ("bi:alice", record) if name == "alice" else ("", None)

    async def sha256_hex(value):
        return hashlib.sha256(value.encode()).hexdigest()

    async def verify(pubkey, signature, canonical):
        return signature == hashlib.sha256(pubkey.encode() + canonical).hexdigest()

    namespace = dict(constants)
    namespace.update({
        "parse_qs": parse_qs,
        "urlparse": urlparse,
        "method_name": lambda request: request.method,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "MAX_NODE_NAME": 63,
        "_ts_ok": lambda _ts: True,
        "_owner_pubkey": owner_pubkey,
        "_account_row": account_row,
        "_account_kind": lambda value: value.get("kind", "user"),
        "ed25519_verify": verify,
        "sha256_hex": sha256_hex,
        "CHAT_CHANNELS_RE": re.compile(r"^/api/chat/channels/?$"),
        "CHAT_CHANNEL_RE": re.compile(r"^/api/chat/channels/([0-9a-f]{32})/?$"),
        "CHAT_CHANNEL_MEMBERS_RE": re.compile(
            r"^/api/chat/channels/([0-9a-f]{32})/members/?$"),
        "CHAT_CHANNEL_ROOM_ACCESS_RE": re.compile(
            r"^/api/chat/channels/([0-9a-f]{32})/room-access/?$"),
        "CHAT_CHANNEL_HISTORY_RE": re.compile(
            r"^/api/chat/channels/([0-9a-f]{32})/history/?$"),
        "CHAT_CHANNEL_WEBHOOKS_RE": re.compile(
            r"^/api/chat/channels/([0-9a-f]{32})/webhooks/?$"),
        "CHAT_CHANNEL_WEBHOOK_RE": re.compile(
            r"^/api/chat/channels/([0-9a-f]{32})/webhooks/([0-9a-f]{32})/?$"),
        "CHAT_DIRECT_MESSAGES_RE": re.compile(r"^/api/chat/direct-messages/?$"),
        "CHAT_DIRECT_MESSAGE_ROOM_ACCESS_RE": re.compile(
            r"^/api/chat/direct-messages/([0-9a-f]{32})/room-access/?$"),
        "CHAT_DIRECT_MESSAGE_READ_RE": re.compile(
            r"^/api/chat/direct-messages/([0-9a-f]{32})/read/?$"),
        "CHAT_READS_RE": re.compile(r"^/api/chat/reads/?$"),
        "CHAT_BLOCKS_RE": re.compile(r"^/api/chat/blocks/?$"),
        "chat_reads_api": chat_reads_api,
        "Date": SimpleNamespace(now=lambda: int(TS)),
    })
    module = ast.fix_missing_locations(ast.Module(body=functions, type_ignores=[]))
    exec(compile(module, str(ENTRY_PATH), "exec"), namespace)
    return namespace, record


def _url(path, canonical, node="alice"):
    signature = hashlib.sha256(("PK-" + node).encode() + canonical.encode()).hexdigest()
    return f"https://forkmesh.test{path}?node={node}&ts={TS}&sig={signature}"


def _digest(text):
    return hashlib.sha256(text.encode()).hexdigest()


def test_desktop_create_proof_binds_name_visibility_and_invitees():
    namespace, record = _harness()
    resolve = namespace["_chat_channel_signed_session"]
    fields = "name=not-very-secret\nvisibility=private\nmembers=bob,carol"
    assert _digest(fields) == (
        "cb57d4a3c1ff43d0506249ea83dbc35e5700b9755652b84d25beddf57bb3eff1"
    )
    url = _url(
        "/api/chat/channels",
        f"forkmesh-chat-channel-create-v1\nalice\n{TS}\n{_digest(fields)}",
    )
    body = {
        "name": " Not-Very-Secret ",
        "visibility": "private",
        "members": ["Bob", "carol"],
    }
    assert asyncio.run(resolve(object(), _Request("POST", url), body)) == (
        "bi:alice", record)
    for swapped in (
        {**body, "visibility": "public"},
        {**body, "members": ["bob", "carol", "mallory"]},
        {**body, "members": ["bob"]},
        {**body, "name": "other"},
        {"name": body["name"], "members": body["members"]},
    ):
        assert asyncio.run(
            resolve(object(), _Request("POST", url), swapped)) == ("", None)


def test_desktop_member_proof_binds_channel_method_and_username():
    namespace, record = _harness()
    resolve = namespace["_chat_channel_signed_session"]
    channel = "a" * 32
    path = f"/api/chat/channels/{channel}/members"
    add = _url(
        path,
        f"forkmesh-chat-channel-members-v1\nalice\n{channel}\n{TS}\nPOST\n"
        + _digest("bob"),
    )
    assert asyncio.run(
        resolve(object(), _Request("POST", add), {"username": " Bob "})
    ) == ("bi:alice", record)
    assert asyncio.run(
        resolve(object(), _Request("POST", add), {"username": "carol"})
    ) == ("", None)
    assert asyncio.run(
        resolve(object(), _Request("DELETE", add), {"username": "bob"})
    ) == ("", None)
    listing = _url(
        path,
        f"forkmesh-chat-channel-members-v1\nalice\n{channel}\n{TS}\nGET\n"
        + _digest(""),
    )
    assert asyncio.run(resolve(object(), _Request("GET", listing), None)) == (
        "bi:alice", record)
    other = _url(
        f"/api/chat/channels/{'b' * 32}/members",
        f"forkmesh-chat-channel-members-v1\nalice\n{channel}\n{TS}\nGET\n"
        + _digest(""),
    )
    assert asyncio.run(resolve(object(), _Request("GET", other), None)) == (
        "", None)


def test_desktop_direct_message_proofs_are_resource_bound():
    namespace, record = _harness()
    resolve = namespace["_chat_direct_signed_session"]
    listing = _url(
        "/api/chat/direct-messages",
        f"forkmesh-chat-direct-list-v1\nalice\n{TS}",
    )
    assert asyncio.run(resolve(object(), _Request("GET", listing), None)) == (
        "bi:alice", record)
    assert asyncio.run(
        resolve(object(), _Request("POST", listing), {"username": "bob"})
    ) == ("", None)

    start = _url(
        "/api/chat/direct-messages",
        f"forkmesh-chat-direct-start-v1\nalice\n{TS}\n" + _digest("bob"),
    )
    assert asyncio.run(
        resolve(object(), _Request("POST", start), {"username": "Bob"})
    ) == ("bi:alice", record)
    assert asyncio.run(
        resolve(object(), _Request("POST", start), {"username": "carol"})
    ) == ("", None)

    conversation = "c" * 32
    access = _url(
        f"/api/chat/direct-messages/{conversation}/room-access",
        f"forkmesh-chat-direct-access-v1\nalice\n{conversation}\n{TS}",
    )
    assert asyncio.run(resolve(object(), _Request("GET", access), None)) == (
        "bi:alice", record)
    replayed = _url(
        f"/api/chat/direct-messages/{'d' * 32}/room-access",
        f"forkmesh-chat-direct-access-v1\nalice\n{conversation}\n{TS}",
    )
    assert asyncio.run(resolve(object(), _Request("GET", replayed), None)) == (
        "", None)
    read = _url(
        f"/api/chat/direct-messages/{conversation}/read",
        f"forkmesh-chat-direct-read-v1\nalice\n{conversation}\n{TS}",
    )
    assert asyncio.run(resolve(object(), _Request("POST", read), None)) == (
        "bi:alice", record)
    assert asyncio.run(resolve(object(), _Request("POST", access), None)) == (
        "", None)


def test_direct_message_runtime_accepts_desktop_proofs_after_sessions():
    start = ENTRY.index("class _ChatDirectMessagesRuntime(")
    body = ENTRY[start:ENTRY.index("async def room_access", start)]
    assert "await super().session(data)" in body
    assert "_chat_direct_signed_session(" in body


def test_desktop_read_mark_proofs_bind_the_marks():
    namespace, record = _harness()
    resolve = namespace["_chat_direct_signed_session"]
    listing = _url(
        "/api/chat/reads", f"forkmesh-chat-reads-list-v1\nalice\n{TS}")
    assert asyncio.run(resolve(object(), _Request("GET", listing), None)) == (
        "bi:alice", record)
    dm = "direct:" + "c" * 32
    marks = {"#general": 5, dm: 7}
    digest = _digest(f"#general 5\n{dm} 7")
    assert chat_reads_api.marks_digest(marks) == digest
    post = _url(
        "/api/chat/reads",
        f"forkmesh-chat-reads-mark-v1\nalice\n{TS}\n{digest}")
    assert asyncio.run(
        resolve(object(), _Request("POST", post), {"marks": marks})
    ) == ("bi:alice", record)
    for swapped in ({"#general": 5, dm: 8}, {"#general": 5}, None, {}):
        assert asyncio.run(
            resolve(object(), _Request("POST", post), {"marks": swapped})
        ) == ("", None)
    assert asyncio.run(
        resolve(object(), _Request("POST", listing), {"marks": marks})
    ) == ("", None)
