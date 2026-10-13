"""Username and encrypted-chat moderation contracts."""
import ast
import asyncio
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = (
    ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "route_handlers.py").read_text(encoding="utf-8")
)
CHAT_MODERATION = ROOT / "public" / "chat-moderation.js"
CHAT_MODERATION_TEXT = CHAT_MODERATION.read_text(encoding="utf-8")
SERVER_NODE = ROOT.parent / "desktop" / "src" / "ServerNode.cpp"
SERVER_NODE_TEXT = SERVER_NODE.read_text(encoding="utf-8")


def _entry_namespace(*names):
    wanted = set(names) | {
        "BLOCKED_TERM_HASHES",
        "BLOCKED_TERM_LENGTHS",
        "USERNAME_MODERATION_AI_DEFAULT_MODEL",
        "USERNAME_MODERATION_CONFIRM_AI_DEFAULT_MODEL",
        "USERNAME_MODERATION_SYSTEM_PROMPT",
        "USERNAME_MODERATION_CONFIRM_PROMPT",
    }
    tree = ast.parse(ENTRY_TEXT)
    selected = []
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if node.name in wanted:
                selected.append(node)
        elif isinstance(node, ast.Assign):
            assigned = {
                target.id for target in node.targets
                if isinstance(target, ast.Name)
            }
            if assigned & wanted:
                selected.append(node)
    namespace = {"re": re, "hashlib": hashlib, "json": json}
    exec(compile(ast.Module(selected, type_ignores=[]), str(ENTRY), "exec"), namespace)
    return namespace


def _decoded(hex_value):
    return bytes.fromhex(hex_value).decode("ascii")


def test_username_digest_filter_catches_exact_and_obfuscated_forms():
    namespace = _entry_namespace("_moderation_forms", "username_has_blocked_term")
    blocked = namespace["username_has_blocked_term"]
    first = _decoded("6675636b")
    second = _decoded("73686974")

    assert blocked(first)
    assert blocked("-".join(first))
    assert blocked(first[0] + first[1] * 3 + first[2:])
    assert blocked(second[:2] + "1" + second[3:])
    assert not blocked(_decoded("7363756e74686f727065"))
    assert not blocked("helpful-builder")


def test_username_workers_ai_classifier_uses_structured_native_binding():
    namespace = _entry_namespace("_username_ai_blocked")
    calls = []
    logs = []

    class AI:
        async def run(self, model, payload):
            calls.append((model, payload))
            return {"response": {"allowed": False}}

    class Env:
        USERNAME_MODERATION_AI_MODEL = "test-model"

    env = Env()
    env.AI = AI()

    namespace.update({
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "js_nullish": lambda _value: False,
        "to_js": lambda value: value,
        "_safe_error_text": str,
        "log_error": lambda *args: logs.append(args),
    })
    result = asyncio.run(namespace["_username_ai_blocked"](env, "safe-shape"))

    assert result is True
    assert len(calls) == 1
    model, payload = calls[0]
    assert model == "test-model"
    assert payload["response_format"]["type"] == "json_schema"
    assert payload["response_format"]["json_schema"]["required"] == ["allowed"]
    assert logs == []


def test_every_account_name_write_path_runs_moderation():
    # Every write path moderates. A first claim gets the full check; a name its
    # owner is only re-presenting gets the deterministic filter, so a flipped AI
    # verdict cannot strand an established owner (adhoc #70).
    first_claim = ENTRY_TEXT.count("await username_moderation_error(env, name)")
    re_presented = ENTRY_TEXT.count(
        "established_username_moderation_error(name)")
    assert first_claim >= 2
    assert re_presented >= 2
    assert first_claim + re_presented >= 4
    rename = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _rename_account_namespace"):
        ENTRY_TEXT.index("async def _account_signup")
    ]
    assert "await username_moderation_error(env, new_name)" in rename
    assert 'return name_bi, rec, moderation_error' in rename


def test_account_finalize_never_re_asks_the_ai_about_a_reserved_name():
    finalize = ENTRY_TEXT[ENTRY_TEXT.index("async def _account_finalize"):]
    finalize = finalize[:finalize.index("moderation_error")] + finalize[
        finalize.index("moderation_error"):
        finalize.index("# Email + password are optional")]
    assert "established_username_moderation_error(name)" in finalize
    assert "await username_moderation_error" not in finalize


def _moderation_namespace(ai_answers):
    """Entry namespace wired to a scripted Workers AI, recording every call."""
    namespace = _entry_namespace(
        "_moderation_forms", "username_has_blocked_term",
        "_username_ai_blocked", "_username_ai_rejects",
        "username_moderation_error", "established_username_moderation_error",
        "USERNAME_MODERATION_SYSTEM_PROMPT",
        "USERNAME_MODERATION_CONFIRM_PROMPT",
    )
    prompts = []

    class AI:
        async def run(self, model, payload):
            prompts.append((model, payload["messages"][0]["content"]))
            return {"response": {"allowed": ai_answers[len(prompts) - 1]}}

    class Env:
        USERNAME_MODERATION_AI_MODEL = "test-model"

    env = Env()
    env.AI = AI()
    namespace.update({
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "js_nullish": lambda _value: False,
        "to_js": lambda value: value,
        "_safe_error_text": str,
        "log_error": lambda *_args: None,
    })
    return namespace, env, prompts


def test_a_single_ai_flag_is_not_enough_to_refuse_a_clean_username():
    # The wide-net first pass flags ordinary names; only a confirmed flag is an
    # error, which is what "my username is not offensive" reports came from.
    namespace, env, prompts = _moderation_namespace([False, True])
    error = asyncio.run(
        namespace["username_moderation_error"](env, "helpful-builder"))

    assert error == ""
    assert len(prompts) == 2
    assert prompts[0] == (
        "test-model", namespace["USERNAME_MODERATION_SYSTEM_PROMPT"])
    # A second reader, not a second reading: two passes of one small model
    # would share its blind spots rather than check them.
    assert prompts[1] == (
        namespace["USERNAME_MODERATION_CONFIRM_AI_DEFAULT_MODEL"],
        namespace["USERNAME_MODERATION_CONFIRM_PROMPT"])
    assert prompts[0][0] != prompts[1][0]


def test_an_allowed_username_costs_one_ai_call():
    namespace, env, prompts = _moderation_namespace([True])

    assert asyncio.run(
        namespace["username_moderation_error"](env, "helpful-builder")) == ""
    assert len(prompts) == 1


def test_two_agreeing_ai_passes_still_refuse_the_username():
    namespace, env, prompts = _moderation_namespace([False, False])
    error = asyncio.run(
        namespace["username_moderation_error"](env, "obfuscated-variant"))

    assert error == "inappropriate_node_name"
    assert len(prompts) == 2


def test_digest_filter_refuses_without_consulting_the_ai():
    namespace, env, prompts = _moderation_namespace([True, True])
    first = _decoded("6675636b")

    assert asyncio.run(
        namespace["username_moderation_error"](env, first)
    ) == "inappropriate_node_name"
    assert prompts == []
    assert namespace["established_username_moderation_error"](
        first) == "inappropriate_node_name"
    assert namespace["established_username_moderation_error"](
        "helpful-builder") == ""


def test_username_rejection_has_actionable_first_party_ui_copy():
    surfaces = [
        ROOT / "public" / "signup.js",
        ROOT / "public" / "dashboard" / "js" / "04-account.js",
    ]
    for surface in surfaces:
        assert "inappropriate_node_name" in surface.read_text(encoding="utf-8")
    world = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(
        encoding="utf-8")
    assert "New accounts are created only on ForkMesh's protected signup page" in world
    assert 'data-world-signup-link href="/signup"' in world


def test_moderation_module_is_never_held_as_an_immutable_stale_dependency():
    headers = (ROOT.parent / "www" / "public" / "_headers").read_text(
        encoding="utf-8")
    rule = headers[headers.index("/chat-moderation.js"):]
    rule = rule[:rule.index("\n\n")]
    assert "Cache-Control: no-store" in rule


def test_hash_vocabulary_matches_all_first_party_surfaces():
    digest_pattern = re.compile(r'"([0-9a-f]{64})"')
    worker_hashes = set(digest_pattern.findall(ENTRY_TEXT[
        ENTRY_TEXT.index("BLOCKED_TERM_HASHES"):
        ENTRY_TEXT.index("def valid_node_name")
    ]))
    web_hashes = set(digest_pattern.findall(CHAT_MODERATION_TEXT))
    desktop_hashes = set(digest_pattern.findall(SERVER_NODE_TEXT[
        SERVER_NODE_TEXT.index("blockedChatTermHashes"):
        SERVER_NODE_TEXT.index("bool isBlockedChatRun")
    ]))
    assert worker_hashes
    assert worker_hashes == web_hashes == desktop_hashes


@pytest.mark.skipif(shutil.which("node") is None, reason="Node.js is unavailable")
def test_browser_chat_filter_masks_plain_and_obfuscated_terms():
    script = r'''
import { readFileSync } from "node:fs";
const source = readFileSync(process.argv[2], "utf8");
const url = "data:text/javascript;base64," + Buffer.from(source).toString("base64");
const moderation = await import(url);
const first = Buffer.from("6675636b", "hex").toString("utf8");
const second = Buffer.from("73686974", "hex").toString("utf8");
const input = `keep ${first} and ${second[0]}-${second[1]}-1-${second[3]} done`;
const result = await moderation.moderateChatText(input);
const plain = await moderation.moderateChatPlain({type: "chat", text: input});
process.stdout.write(JSON.stringify({result, plain}));
'''
    completed = subprocess.run(
        ["node", "--input-type=module", "-", str(CHAT_MODERATION)],
        input=script,
        text=True,
        capture_output=True,
        check=True,
    )
    result = json.loads(completed.stdout)
    assert result["result"] == {"text": "keep *** and *** done", "changed": True}
    assert result["plain"]["text"] == "keep *** and *** done"


def test_desktop_filters_before_encryption_and_after_decryption():
    send_encrypted = SERVER_NODE_TEXT[
        SERVER_NODE_TEXT.index("void ServerNode::sendEncrypted"):
        SERVER_NODE_TEXT.index("void ServerNode::sampleSystemStats")
    ]
    assert "moderatedChatObject(plain)" in send_encrypted
    assert "m_seal.sealObject(moderated)" in send_encrypted
    assert "m_crypto.encryptObject(framed)" in send_encrypted
    assert send_encrypted.index("moderatedChatObject(plain)") < send_encrypted.index(
        "m_seal.sealObject(moderated)"
    )
    assert SERVER_NODE_TEXT.count("filteredChatText(") >= 8
