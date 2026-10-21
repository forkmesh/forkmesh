import ast
import base64
import hashlib
import hmac
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
CHANNELS = (ROOT / "src" / "chat_channels_api.py").read_text(encoding="utf-8")
SECRET = "browser-test-secret-0123456789abcdef"
CHANNEL = "0123456789abcdef0123456789abcdef"
GO_TICKET = (
    "eyJyb29tIjoiMjA5MDU0YzFjNjhkMzI1YzE0OWQ2NjdhZGMwZGY1ZjYwYjBmMGIzNTE0Yz"
    "Y1MWVhOGJiODhkNzE1NjQwYjk1ZSIsInBlZXIiOiJhYmFiYWJhYmFiYWJhYmFiYWJhYmFi"
    "YWJhYmFiYWJhYiIsImV4cCI6MTgwMDAwMDA2MH0.eq6TkYD-Bi8gljNRwWNVEBm0nk624Y"
    "O2ndgxdkHven4")


def _ticket_functions():
    namespace = {"base64": base64, "hmac": hmac, "json": json, "hashlib": hashlib}
    for node in ast.parse(ENTRY).body:
        named = (
            isinstance(node, ast.FunctionDef)
            and node.name in ("chat_call_room", "chat_call_ticket", "chat_call_nodes",
                              "chat_call_health_url", "chat_call_pick"))
        constant = isinstance(node, ast.Assign) and any(
            getattr(target, "id", "") in ("CHAT_CALL_TICKET_TTL_MS", "CHAT_CALL_NODES_MAX")
            for target in node.targets)
        if named or constant:
            exec(compile(ast.Module([node], []), "entry.py", "exec"), namespace)
    return namespace


def test_the_ticket_is_the_one_the_calls_server_verifies():
    functions = _ticket_functions()
    ticket = functions["chat_call_ticket"](
        SECRET, CHANNEL, "ab" * 16, 1_800_000_000_000)
    assert ticket["ticket"] == GO_TICKET
    assert ticket["room"] == functions["chat_call_room"](SECRET, CHANNEL)
    assert CHANNEL not in ticket["ticket"]
    other = functions["chat_call_ticket"](
        SECRET, "f" * 32, "ab" * 16, 1_800_000_000_000)
    assert other["room"] != ticket["room"]


def test_only_members_of_a_private_channel_get_a_ticket():
    handler = CHANNELS[CHANNELS.index("async def _call_ticket("):]
    handler = handler[:handler.index("\n\n\n")]
    assert "await _readable_channel(" in handler
    assert '_visibility(record) == "public"' in handler
    assert '"private_channel_required"' in handler
    assert '"calls_unavailable"' in handler
    route = CHANNELS[CHANNELS.index("    if call_ticket:"):]
    route = route[:route.index("\n\n")]
    assert 'method != "POST"' in route
    runtime = ENTRY[ENTRY.index("    async def call_ticket(self, channel_id):"):]
    runtime = runtime[:runtime.index("\n\n")]
    assert "FORKMESH_CALLS_SECRET" in runtime
    assert "FORKMESH_CALLS_NODES" in runtime
    assert "len(secret) < 32 or not nodes" in runtime
    assert "await _chat_call_healthy(nodes)" in runtime
    assert "await runtime.call_ticket(channel_id)" in CHANNELS


NODES = ",".join(
    "wss://n%d.next.forkmesh.com/calls/ws" % n for n in (2, 3, 4))


def test_the_node_list_keeps_only_call_endpoints():
    functions = _ticket_functions()
    nodes = functions["chat_call_nodes"](
        NODES + ",ws://n9.next.forkmesh.com/calls/ws, wss://evil@x/calls/ws,"
        "wss://n2.next.forkmesh.com/calls/ws,wss://n5.next.forkmesh.com/other")
    assert nodes == NODES.split(",")
    assert functions["chat_call_health_url"](nodes[1]) == (
        "https://n3.next.forkmesh.com/calls/health")


def test_every_participant_of_a_room_lands_on_the_same_live_node():
    functions = _ticket_functions()
    pick = functions["chat_call_pick"]
    nodes = NODES.split(",")
    rooms = [functions["chat_call_room"](SECRET, "%032x" % n) for n in range(60)]
    chosen = [pick(room, nodes, set(nodes)) for room in rooms]
    assert chosen == [pick(room, nodes, set(nodes)) for room in rooms]
    assert set(chosen) == set(nodes)
    for room, first in zip(rooms, chosen):
        survivors = set(nodes) - {first}
        moved = pick(room, nodes, survivors)
        assert moved in survivors
        assert moved == pick(room, nodes, survivors)
        for other in nodes:
            if other != first and pick(room, nodes, set(nodes) - {other}) != first:
                raise AssertionError("losing an unrelated node moved a call")
    assert pick(rooms[0], nodes, set()) == chosen[0]
    assert pick(rooms[0], [], set()) == ""
