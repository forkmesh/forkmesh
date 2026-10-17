#!/usr/bin/env python3
"""ForkBot chat-to-issue contracts."""

import ast
import asyncio
import base64
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import tomllib
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from urllib.parse import quote

import pytest

from worker_test_helpers import json_from_request_double


# MainWindowChat.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_CHAT_SOURCES = (
    "MainWindowChat.cpp",
    "MainWindowNetworkInternal.h",
    "MainWindowDiagnostics.cpp",
    "MainWindowLogs.cpp",
    "MainWindowNavigation.cpp",
    "MainWindowNetworkRepos.cpp",
    "MainWindowHosts.cpp",
    "MainWindowNodes.cpp",
    "MainWindowNetworkDiagnostics.cpp",
    "MainWindowUsers.cpp",
    "MainWindowVultrMirrors.cpp",
    "MainWindowHostInstall.cpp",
)

# MainWindowAgents.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_AGENT_SOURCES = (
    "MainWindowAgents.cpp",
    "MainWindowAgentsInternal.h",
    "MainWindowAgentSync.cpp",
    "MainWindowAgentUsage.cpp",
    "MainWindowAgentList.cpp",
    "MainWindowAgentDetail.cpp",
    "MainWindowAgentLaunch.cpp",
    "MainWindowAgentCliSession.cpp",
    "MainWindowAgentEvents.cpp",
    "MainWindowAgentTranscript.cpp",
    "MainWindowAgentLanding.cpp",
    "MainWindowAgentActions.cpp",
)

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
CATALOG = ENTRY.parent / "catalog.py"
WRANGLER = ROOT / "wrangler.toml"
PUBLIC = ROOT / "public"
# The ForkBot domain now lives in its own lazily-imported module (entry.py
# reaches it through a _LazyModule proxy so Cloudflare's startup-memory
# validation does not pay for it on every isolate). Source-text assertions
# and the AST loader below read both files as one program.
FORKBOT = ENTRY.parent / "forkbot.py"
ENTRY_TEXT = (
    (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "forkbot.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "fediverse_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)
    + "\n"
    + FORKBOT.read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "app_runtime.py").read_text(encoding="utf-8")
)
CATALOG_TEXT = CATALOG.read_text(encoding="utf-8")
CHAT_TEXT = (PUBLIC / "chat.js").read_text(encoding="utf-8")
DASHBOARD_CHAT_TEXT = (PUBLIC / "dashboard-chat.js").read_text(encoding="utf-8")
CHAT_HTML_TEXT = (PUBLIC / "chat.html").read_text(encoding="utf-8")
CHAT_CSS_TEXT = (PUBLIC / "chat.css").read_text(encoding="utf-8")
DASHBOARD_TEXT = (PUBLIC / "dashboard.js").read_text(encoding="utf-8")
DASHBOARD_HOME_TEXT = (
    PUBLIC / "dashboard" / "partials" / "views" / "home.html"
).read_text(encoding="utf-8")
WRANGLER_DATA = tomllib.loads(WRANGLER.read_text(encoding="utf-8"))

FUNCS = {
    "clean_string",
    "method_name",
    "repo_web_href",
    "_forkbot_extract_mention_command",
    "_forkbot_parse_issue_command",
    "_forkbot_command_hints_issue",
    "_forkbot_polite_prefix",
    "_forkbot_parse_count_command",
    "_forkbot_parse_list_command",
    "_forkbot_parse_search_command",
    "_forkbot_parse_agent_command",
    "_forkbot_parse_show_command",
    "_forkbot_parse_help_command",
    "_forkbot_help_message",
    "_forkbot_repo_gateway_json",
    "_forkbot_missing_tree",
    "_forkbot_issue_json_path",
    "_forkbot_issue_json_candidates",
    "_forkbot_blob_text",
    "_forkbot_parse_issue_record",
    "_forkbot_recent_issue_numbers",
    "_forkbot_load_issue_records",
    "_forkbot_all_issue_records",
    "_forkbot_search_issues",
    "_forkbot_issue_lines",
    "_forkbot_gateway_offline_reply",
    "_forkbot_action_count",
    "_forkbot_action_list",
    "_forkbot_action_search",
    "_forkbot_action_show",
    "_forkbot_action_start_agent",
    "_forkbot_enqueue_agent_request",
    "_forkbot_context_text",
    "js_nullish",
    "log_error",
    "_safe_error_text",
    "_forkbot_issue_title",
    "_forkbot_fallback_issue_fields",
    "_forkbot_json_object_from_text",
    "_forkbot_clean_ai_issue_fields",
    "_forkbot_run_ai",
    "_forkbot_ai_default_model",
    "_forkbot_ai_model_options",
    "_forkbot_resolve_ai_model",
    "_forkbot_ai_model_not_found_error",
    "_forkbot_ai_fallback_models",
    "forkbot_models_handler",
    "ai_ask_handler",
    "ai_workspace_name_handler",
    "_ai_workspace_name_fallback",
    "_ai_ask_rate_check",
    "ai_agent_handler",
    "_ai_agent_rate_check",
    "_forkbot_run_ai_agent",
    "_ai_agent_normalized_result",
    "_ai_agent_clean_messages",
    "_ai_agent_clean_tools",
    "_ai_agent_clean_tool_calls",
    "_ai_agent_tool_arguments",
    "valid_node_name",
    "_forkbot_ai_issue_fields",
    "_forkbot_ai_interpret",
    "_forkbot_next_issue_number",
    "_forkbot_attributed_body",
    "_ap_org_alias_owner",
    "_forkbot_rekey_alias_inbox",
    "_forkbot_enqueue_issue",
    "forkbot_chat_handler",
}

CONSTANTS = {
    "FORKBOT_NAME",
    "FORKBOT_AUTHOR",
    "FORKBOT_DEFAULT_OWNER",
    "FORKBOT_DEFAULT_REPO",
    "FORKBOT_MAX_COMMAND",
    "FORKBOT_AI_DEFAULT_MODEL",
    "FORKBOT_AI_MODEL_CHOICES",
    "AI_ASK_MAX_PROMPT",
    "AI_ASK_MAX_REPLY",
    "AI_ASK_MAX_TOKENS",
    "AI_ASK_RATE_WINDOW_MS",
    "AI_ASK_MAX_PER_WINDOW",
    "AI_ASK_SYSTEM_PROMPT",
    "AI_WORKSPACE_NAME_MAX_PROMPT",
    "AI_WORKSPACE_NAME_MAX_TOKENS",
    "AI_WORKSPACE_NAME_SYSTEM_PROMPT",
    "AI_WORKSPACE_NAME_FALLBACK_SKIP",
    "AI_AGENT_MAX_MESSAGES",
    "AI_AGENT_MAX_CONTENT",
    "AI_AGENT_MAX_TOOLS",
    "AI_AGENT_MAX_TOKENS",
    "AI_AGENT_MAX_REPLY",
    "AI_AGENT_TICKET_MAX_AGE_MS",
    "AI_AGENT_RATE_WINDOW_MS",
    "AI_AGENT_MAX_PER_WINDOW",
    "AI_AGENT_CONTEXT_OVERFLOW_MARKERS",
    "LOGIN_MAX_SKEW_MS",
    "NODE_NAME_RE",
    "FORKBOT_CONTEXT_MAX_MESSAGES",
    "FORKBOT_CONTEXT_MAX_CHARS",
    "FORKBOT_LIST_DEFAULT",
    "FORKBOT_LIST_MAX",
    "FORKBOT_SEARCH_MAX",
    "FORKBOT_GATEWAY_TIMEOUT_MS",
    "FORKBOT_GATEWAY_MAX_ACTION",
    "FORKBOT_ISSUE_FIELDS_SCHEMA",
    "FORKBOT_INTENT_SCHEMA",
    "AI_WORKSPACE_NAME_SCHEMA",
    "MAX_ISSUE_BYTES",
    "MAX_PENDING_ISSUES",
    "MAX_PENDING_PER_AUTHOR",
    "MAX_BLOB_BATCH",
    "MAX_NODE_NAME",
    "EXPECTED_DEGRADED_HEADER",
}


def _load_forkbot(extra_globals=None):
    tree = ast.parse(CATALOG_TEXT + "\n" + ENTRY_TEXT, filename=str(ENTRY))
    selected = []
    for node in tree.body:
        if isinstance(node, ast.Assign):
            targets = {t.id for t in node.targets if isinstance(t, ast.Name)}
            if targets & CONSTANTS:
                selected.append(node)
        elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name in FUNCS:
            selected.append(node)
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    ns = {"json": json, "re": re, "quote": quote, "asyncio": asyncio,
          "base64": base64, "to_js": lambda value: value,
          "bounded_json_request": json_from_request_double}
    if extra_globals:
        ns.update(extra_globals)
    exec(compile(module, str(ENTRY), "exec"), ns)
    return ns


class _Date:
    @staticmethod
    def now():
        return 1_783_463_371_000


class _Request:
    method = "POST"
    url = "https://forkmesh.test/api/forkbot/chat"

    def __init__(self, body):
        self._body = body

    async def json(self):
        return self._body


def _json_response(data, status=200, extra_headers=None, **_kwargs):
    response = {"status": status, "data": data}
    if extra_headers:
        response["headers"] = dict(extra_headers)
    return response


class _HostResponse:
    def __init__(self, data):
        self._data = data
        self.status = 200

    async def json(self):
        return self._data


class _FakeGateway:
    """Direct-HTTPS gateway fixture for tree/blobs/search responses.
    `tree` answers every /tree read; `trees` (path -> payload) answers per
    path instead, for split open//closed/ layout fixtures."""

    def __init__(self, tree=None, blobs=None, search=None, trees=None):
        self.urls = []
        self._tree = tree
        self._trees = trees
        self._blobs = blobs
        self._search = search

    async def fetch(self, url):
        self.urls.append(url)
        if "/tree?" in url:
            if self._trees is not None:
                from urllib.parse import parse_qs, urlparse
                path = parse_qs(urlparse(url).query).get("path", [""])[0]
                data = self._trees.get(path)
            else:
                data = self._tree
        elif "/blobs?" in url:
            data = self._blobs
        elif "/search?" in url:
            data = self._search
        else:
            data = None
        if data is None:
            raise RuntimeError("host offline")
        return _HostResponse(data)


def _issue_tree(numbers):
    return {"ok": True, "entries": [
        {"type": "tree", "name": str(n)} for n in numbers]}


def _issue_blobs(records):
    blobs = {}
    for record in records:
        path = ".forkmesh/issues/%d/issue-%d.json" % (
            record["number"], record["number"])
        blobs[path] = {"ok": True, "encoding": "utf8",
                       "content": json.dumps(record)}
    return {"ok": True, "blobs": blobs}


def _env_and_calls(ai=None, catalog_issue_max=None, host=None, admins=(),
                   org_node="", signature_ok=True, signed_model=None,
                   ai_ask_used=0, session_account=""):
    # signed_model: the model id the caller's signature actually covers, so a
    # test can prove /api/ai/ask verifies the client's own pick rather than a
    # server-resolved substitute.
    # org_node: the account backing forkmesh/forkmesh when that public name is
    # an organization alias ("" = a plain node name that resolves to itself).
    calls = {"inserted": [], "contributors": [], "side_effects": [],
             "hostNotifies": [], "rekeyed": [], "aiAskRate": [], "signed": [],
             "sessions": []}

    class _Env:
        AI = ai
        FORKBOT_AI_MODEL = ""

    async def ensure_schema(_env):
        return None

    async def blind_index(_env, value):
        return "bi:" + str(value)

    # Per-repo issue-number allocator state (issue_seq table). Seeded from the
    # catalog record's issueMaxNumber the first time a repo is used.
    seq = {}

    # Per-account Workers AI window for POST /api/ai/ask. ai_ask_used seeds how
    # many prompts this account already spent in the current window.
    ask_window = {"count": ai_ask_used, "start": int(_Date.now())}

    async def d1_first(_env, sql, *args):
        if "INSERT INTO ai_ask_rate" in sql:
            ask_window["count"] += 1
            calls["aiAskRate"].append(args[0])
            return {"count": ask_window["count"],
                    "window_start_ts": ask_window["start"]}
        if "SELECT COUNT(*) AS c FROM issue_inbox" in sql:
            return {"c": 0}
        if "WHERE repo_bi=? AND submitter_bi=?" in sql:
            return {"c": 0}
        if "FROM repositories WHERE key_bi=?" in sql:
            # A stored catalog record whose issueMaxNumber seeds the allocator.
            return {"data": {"issueMaxNumber": str(catalog_issue_max)}} \
                if catalog_issue_max is not None else None
        if sql.startswith("INSERT INTO issue_seq"):
            key, inserted_next, seed_next = args
            if key in seq:
                seq[key] = max(seq[key], seed_next) + 1
            else:
                seq[key] = inserted_next
            return {"number": seq[key] - 1}
        if "SELECT next_number FROM issue_seq" in sql:
            return {"next_number": seq.get(args[0], 1)}
        raise AssertionError("unexpected d1_first: " + sql)

    async def d1_run(_env, sql, *args):
        if sql.startswith("INSERT INTO issue_inbox"):
            calls["inserted"].append(args)
            return None
        if sql.startswith("INSERT INTO issue_seq"):
            repo_bi, seed_next = args[0], args[1]
            seq[repo_bi] = max(seq.get(repo_bi, 0), seed_next)
            return None
        if sql.startswith("UPDATE issue_seq"):
            seq[args[1]] = args[0]
            return None
        if sql.startswith("UPDATE issue_inbox SET repo_bi"):
            calls["rekeyed"].append(args)
            return None
        raise AssertionError("unexpected d1_run: " + sql)

    async def decrypt_row(_env, data):
        return dict(data) if isinstance(data, dict) else None

    async def encrypt_row(_env, obj):
        return dict(obj)

    async def record_contributor(_env, author, kind, _display_name=""):
        calls["contributors"].append((author, kind))

    async def side_effect(awaitable):
        calls["side_effects"].append(awaitable)

    async def inbox_author_over_quota(*_args):
        return False

    def notify_pending_inbox(*args, **kwargs):
        return ("pending", args, kwargs)

    def notify_mentions(*args, **kwargs):
        return ("mentions", args, kwargs)

    async def is_admin(_env, name):
        return str(name or "").lower() in {str(a).lower() for a in admins}

    async def org_repo_node(_env, _org, _repo):
        return org_node

    async def notify_repo_host(_env, owner, repo, topic):
        calls["hostNotifies"].append((owner, repo, topic))

    # log_error's real implementation from entry.py fans out to these two;
    # stubbed here (like _load_capture_worker_exception in
    # test_sentry_worker.py) so AI-failure logging doesn't need a live DSN/D1.
    async def capture_sentry_error(*_args, **_kwargs):
        return False

    async def write_error_log(*_args, **_kwargs):
        return None

    async def verify_owner_signature(_env, owner, sig, canonical):
        text = canonical.decode()
        calls["signed"].append((owner, sig, text))
        if signed_model is not None:
            return text.split("\n")[3] == signed_model
        return bool(signature_ok)

    async def account_session_record(_env, request, data=None):
        calls["sessions"].append((request, data))
        if not session_account:
            return "", None
        return "bi:" + session_account, {
            "name": session_account, "kind": "user", "status": "active"}

    async def sha256_hex(text):
        return hashlib.sha256(str(text).encode("utf-8")).hexdigest()

    runtime = {
        "_is_admin": is_admin,
        "_verify_owner_signature": verify_owner_signature,
        "_account_session_record": account_session_record,
        "sha256_hex": sha256_hex,
        "Date": _Date,
        "json_response": _json_response,
        "ensure_schema": ensure_schema,
        "blind_index": blind_index,
        "d1_first": d1_first,
        "d1_run": d1_run,
        "encrypt_row": encrypt_row,
        "decrypt_row": decrypt_row,
        "_record_contributor": record_contributor,
        "_best_effort_inbox_side_effect": side_effect,
        "_inbox_author_over_quota": inbox_author_over_quota,
        "notify_pending_inbox": notify_pending_inbox,
        "notify_mentions": notify_mentions,
        "_org_repo_node": org_repo_node,
        "notify_repo_host": notify_repo_host,
        "capture_sentry_error": capture_sentry_error,
        "_write_error_log": write_error_log,
        "_public_base_url": lambda _env: "https://forkmesh.test",
        "safe_segment": lambda value: str(value or ""),
    }
    if host is not None:
        gateway_fetch = host.fetch
    else:
        async def gateway_fetch(_url):
            raise RuntimeError("gateway offline")
    # The gateway read is bounded by a Workers-native AbortSignal rather than
    # asyncio.wait_for (see tests/test_worker_task_concurrency.py), so the
    # helper — not the bare fetch — is what forkbot calls.
    async def js_fetch_with_timeout(url, _init, _timeout_seconds):
        return await gateway_fetch(url)
    runtime["js_fetch_with_timeout"] = js_fetch_with_timeout
    ns = _load_forkbot(runtime)
    return _Env(), calls, ns


def test_forkbot_parses_explicit_issue_mentions_only():
    ns = _load_forkbot()
    extract = ns["_forkbot_extract_mention_command"]
    parse = ns["_forkbot_parse_issue_command"]

    assert extract("forkbot create an issue to fix relay retries") == (
        "create an issue to fix relay retries")
    assert extract("@forkbot: file issue about failed tests") == (
        "file issue about failed tests")
    assert extract("notforkbot create an issue to nope") == ""
    assert parse("create an issue to fix relay retries") == {
        "description": "fix relay retries"
    }
    assert parse("please open issue: worker logs dropped") == {
        "description": "worker logs dropped"
    }
    assert parse("tell me a joke") is None


def test_discord_report_uses_linked_username_and_preserves_its_reward():
    env, calls, ns = _env_and_calls()
    reward = {
        "id": "reward-discord-a1b2c3d4e5f6",
        "recipient": "gaurav54321",
        "amountLamports": 3_000_000,
        "condition": "accepted_report",
        "status": "pending_wallet",
    }

    ok, item = asyncio.run(ns["_forkbot_enqueue_issue"](
        env, "forkmesh", "forkmesh", "Profile settings fail", "Broken.",
        "gaurav54321", source="discord", labels=["bug", "discord"],
        reward=reward))

    assert ok is True
    assert item["event"]["author"] == "gaurav54321"
    assert item["event"]["authorName"] == "gaurav54321"
    assert item["submitter"] == "gaurav54321"
    assert item["meta"]["reporter"] == "gaurav54321"
    assert item["meta"]["rewardEstimateLamports"] == 3_000_000
    assert item["meta"]["rewardRecipient"] == "gaurav54321"
    assert item["meta"]["rewardId"] == reward["id"]
    assert item["meta"]["rewardCondition"] == "accepted_report"
    assert item["meta"]["rewardStatus"] == "pending_wallet"
    assert calls["inserted"][0][2] == "bi:gaurav54321"


def test_discord_reward_cannot_credit_another_user_or_escape_the_range():
    env, _calls, ns = _env_and_calls()

    ok, item = asyncio.run(ns["_forkbot_enqueue_issue"](
        env, "forkmesh", "forkmesh", "Profile settings fail", "Broken.",
        "gaurav54321", source="discord", reward={
            "id": "reward-discord-a1b2c3d4e5f6",
            "recipient": "someone-else",
            "amountLamports": 5_000_001,
            "condition": "accepted_report",
            "status": "pending_wallet",
        }))

    assert ok is True
    assert "rewardEstimateLamports" not in item["meta"]


def test_forkbot_regex_recognizes_natural_phrasings():
    # The offline fast path now covers common natural wordings, not just
    # "create/open/file/make/report an issue".
    ns = _load_forkbot()
    parse = ns["_forkbot_parse_issue_command"]
    assert parse("can you log a bug about the flaky login test") == {
        "description": "the flaky login test"}
    assert parse("track a task to add dark mode") == {
        "description": "add dark mode"}
    assert parse("please raise a ticket regarding slow clones") == {
        "description": "slow clones"}
    assert parse("open a feature request for saved searches") == {
        "description": "saved searches"}
    # Still not an issue request.
    assert parse("what's the weather") is None


def test_forkbot_context_text_bounds_and_formats_conversation():
    ns = _load_forkbot()
    fmt = ns["_forkbot_context_text"]
    assert fmt(None) == ""
    assert fmt([]) == ""
    out = fmt([
        {"sender": "alice", "text": "the login page 500s on submit"},
        {"sender": "bob", "text": "yeah since the auth deploy"},
        {"not": "a dict"},
        {"sender": "carol", "text": "   "},
    ])
    assert out == (
        "alice: the login page 500s on submit\n"
        "bob: yeah since the auth deploy")
    # Never exceeds the message cap.
    many = [{"sender": "u", "text": "m%d" % i} for i in range(50)]
    assert len(fmt(many).splitlines()) == ns["FORKBOT_CONTEXT_MAX_MESSAGES"]


def test_forkbot_chat_handler_queues_default_repo_issue():
    # Catalog says the repo's highest issue number is 6, so the proposed number
    # (what the desktop's nextNumber() would assign) is 7.
    env, calls, ns = _env_and_calls(catalog_issue_max=6)

    response = asyncio.run(ns["forkbot_chat_handler"](
        env,
        _Request({
            "message": "forkbot create an issue to make crash logs searchable",
            "sender": "alice",
        }),
    ))

    assert response["status"] == 201
    assert response["data"]["owner"] == "forkmesh"
    assert response["data"]["repo"] == "forkmesh"
    assert response["data"]["title"] == "make crash logs searchable"
    assert response["data"]["issueNumber"] == 7
    assert response["data"]["issueUrl"] == "/forkmesh/forkmesh/issues"
    assert "#7" in response["data"]["botMessage"]
    assert "/forkmesh/forkmesh/issues" in response["data"]["botMessage"]

    assert len(calls["inserted"]) == 1
    repo_bi, item, submitter_bi = calls["inserted"][0]
    assert repo_bi == "bi:forkmesh/forkmesh"
    assert submitter_bi == "bi:forkbot"
    # The proposed number lands in the stored item so the desktop honors it and
    # the chat reply can echo it.
    assert item["number"] == 7
    assert item["issueNumber"] == 7
    assert item["titleIfNew"] == "make crash logs searchable"
    assert item["submitter"] == "forkbot"
    assert item["requestedBy"] == "alice"
    assert item["meta"]["labels"] == ["forkbot"]
    assert item["meta"]["reporter"] == "alice"
    assert item["event"]["type"] == "open"
    assert item["event"]["author"] == "forkbot"
    assert item["event"]["authorName"] == "forkbot"
    assert item["event"]["sig"] == ""
    # The stored body credits ForkBot and the requester so a reader of the
    # merged issue can see where it came from.
    assert item["event"]["body"] == (
        "make crash logs searchable\n\n"
        "---\n_Filed by ForkBot at @alice's request via chat._")
    assert calls["contributors"] == [("alice", "issues")]
    # The owner node is pushed a sync event like every other inbox write, so
    # the issue lands in the repository now instead of on the slow fallback
    # poll (or never, if the node happens to be restarting).
    assert calls["hostNotifies"] == [("forkmesh", "forkmesh", "issues")]


def test_logged_in_user_opens_forkbot_assisted_issue():
    env, calls, ns = _env_and_calls(
        catalog_issue_max=6, session_account="alice")

    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot create an issue to make crash logs searchable",
        # The authenticated session, not this spoofable display name, owns the
        # attribution decision.
        "sender": "mallory",
    })))

    assert response["status"] == 201
    repo_bi, item, submitter_bi = calls["inserted"][0]
    assert repo_bi == "bi:forkmesh/forkmesh"
    assert submitter_bi == "bi:alice"
    assert item["submitter"] == "alice"
    assert item["requestedBy"] == "alice"
    assert item["meta"]["reporter"] == "alice"
    assert item["event"]["author"] == "alice"
    assert item["event"]["authorName"] == "alice"
    assert item["event"]["body"] == (
        "make crash logs searchable\n\n"
        "---\n_ForkBot helped file this issue via chat._")
    assert calls["contributors"] == [("alice", "issues")]
    pending = [effect for effect in calls["side_effects"]
               if effect[0] == "pending"]
    assert pending and pending[0][1][4] == "alice"
    assert len(calls["sessions"]) == 1


def test_forkbot_queues_issues_under_the_org_alias_backing_node():
    # forkmesh/forkmesh is an organization alias: the repo really lives on the
    # linked node. Every drain reads the inbox under the NODE's blind index
    # (/api/repo/... is org_alias_rewrite'd, and /api/sync selects the rows the
    # account owns), so keying the row on the alias dead-lettered it — the
    # issue was accepted in chat and never synced back to the repository.
    env, calls, ns = _env_and_calls(catalog_issue_max=6, org_node="jett")

    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot create an issue to make crash logs searchable",
        "sender": "alice",
    })))

    assert response["status"] == 201
    repo_bi, item, _submitter_bi = calls["inserted"][0]
    assert repo_bi == "bi:jett/forkmesh"
    # The public identity stays on the organization name.
    assert response["data"]["owner"] == "forkmesh"
    assert response["data"]["issueUrl"] == "/forkmesh/forkmesh/issues"
    assert item["titleIfNew"] == "make crash logs searchable"
    # Owner-directed side effects address the account that actually holds the
    # inbox and runs the node.
    assert calls["hostNotifies"] == [("jett", "forkmesh", "issues")]
    pending = [effect for effect in calls["side_effects"]
               if effect[0] == "pending"]
    assert pending and pending[0][1][1] == "jett"
    # Submissions an earlier release stranded under the alias key are moved
    # onto the node's key so they finally sync too.
    assert calls["rekeyed"] == [("bi:jett/forkmesh", "bi:forkmesh/forkmesh")]


def test_forkbot_confirmation_and_the_feed_ping_name_one_repository():
    # issue #650: the chat confirmation names the repo the sender asked for
    # (the public organization alias), while the inbox row is keyed to the
    # backing node. A ping that carried the node name made the dashboard Feed
    # entry and the confirmation above it describe one submission as two
    # different repositories, which reads as a misfile. Delivery still follows
    # the node; only the displayed name is pinned to the public one.
    env, calls, ns = _env_and_calls(org_node="jett")

    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot create an issue to improve the repository README",
        "sender": "alice",
    })))

    assert "in forkmesh/forkmesh" in response["data"]["botMessage"]
    pending = next(effect for effect in calls["side_effects"]
                   if effect[0] == "pending")
    assert pending[1][1] == "jett"                    # delivered to the host
    assert pending[2]["public_owner"] == "forkmesh"   # displayed as the alias
    # The mention ping is pure display for whoever was named, so it reads the
    # public repo it has always linked to.
    mention = next(effect for effect in calls["side_effects"]
                   if effect[0] == "mentions")
    assert mention[1][1] == "forkmesh"


def test_forkbot_does_not_rekey_when_the_owner_is_a_plain_node():
    env, calls, ns = _env_and_calls()
    asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot open an issue about flaky login tests",
        "sender": "alice",
    })))
    assert calls["inserted"][0][0] == "bi:forkmesh/forkmesh"
    assert calls["rekeyed"] == []


def test_forkbot_agent_requests_reach_the_org_alias_backing_node_owner():
    host = _FakeGateway(
        tree=_issue_tree([4, 5, 6]),
        blobs=_issue_blobs([{"number": 6, "title": "Fix relay retries",
                             "status": "open"}]),
    )
    env, calls, ns = _env_and_calls(
        host=host, org_node="jett", session_account="jett")
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot start an agent on the most recent issue",
        "sender": "jett",  # the account backing the org alias owns the repo
    })))

    assert response["status"] == 201
    assert response["data"]["action"] == "agent_requested"
    repo_bi, item, _submitter_bi = calls["inserted"][0]
    assert repo_bi == "bi:jett/forkmesh"
    assert item["meta"]["wantsAgent"] is True
    assert calls["hostNotifies"] == [("jett", "forkmesh", "issues")]


def test_forkbot_agent_requests_ignore_the_claimed_chat_sender():
    host = _FakeGateway(
        tree=_issue_tree([4, 5, 6]),
        blobs=_issue_blobs([{"number": 6, "title": "Fix relay retries",
                             "status": "open"}]),
    )
    for session_account in ("", "mallory"):
        env, calls, ns = _env_and_calls(
            host=host, session_account=session_account)
        response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
            "message": "forkbot start an agent on the most recent issue",
            "sender": "forkmesh",
        })))
        assert response["data"]["action"] == "agent_denied"
        assert calls["inserted"] == []


def test_forkbot_uses_workers_ai_for_issue_title_and_body_when_available():
    class _AI:
        async def run(self, model, payload):
            self.model = model
            self.payload = payload
            return {
                "response": json.dumps({
                    "title": "Make logs searchable",
                    "body": "Index the main log so crash reports can be found.",
                })
            }

    ai = _AI()
    env, calls, ns = _env_and_calls(ai=ai)

    response = asyncio.run(ns["forkbot_chat_handler"](
        env,
        _Request({"message": "forkbot create an issue to logs need search"}),
    ))

    assert response["status"] == 201
    assert ai.model == ns["FORKBOT_AI_DEFAULT_MODEL"]
    assert ai.payload["messages"][0]["role"] == "system"
    assert calls["inserted"][0][1]["titleIfNew"] == "Make logs searchable"
    # No sender in the request -> the footer still credits ForkBot/the source.
    assert calls["inserted"][0][1]["event"]["body"] == (
        "Index the main log so crash reports can be found.\n\n"
        "---\n_Filed by ForkBot via chat._")


def test_forkbot_unknown_command_returns_help_without_enqueueing():
    # No AI configured and no regex match -> help, nothing enqueued.
    env, calls, ns = _env_and_calls()

    response = asyncio.run(ns["forkbot_chat_handler"](
        env, _Request({"message": "forkbot what can you do?"})))

    assert response["status"] == 200
    assert response["data"]["action"] == "help"
    assert "open an issue" in response["data"]["botMessage"]
    assert calls["inserted"] == []


def test_forkbot_ai_intent_creates_issue_from_natural_request():
    # The wording matches no command regex; the AI classifier recognizes the
    # intent from meaning and drafts the issue.
    class _AI:
        async def run(self, model, payload):
            self.payload = payload
            return {"response": json.dumps({
                "intent": "create_issue",
                "title": "Flaky login test",
                "body": "The login integration test fails intermittently.",
            })}

    env, calls, ns = _env_and_calls(ai=_AI(), catalog_issue_max=3)
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot the login test keeps flaking, can you sort that out?",
        "sender": "alice",
    })))

    assert response["status"] == 201
    assert response["data"]["issueNumber"] == 4
    assert calls["inserted"][0][1]["titleIfNew"] == "Flaky login test"
    assert calls["inserted"][0][1]["event"]["body"] == (
        "The login integration test fails intermittently.\n\n"
        "---\n_Filed by ForkBot at @alice's request via chat._")


def test_forkbot_ai_intent_none_returns_help_without_enqueueing():
    class _AI:
        async def run(self, model, payload):
            return {"response": json.dumps({"intent": "none"})}

    env, calls, ns = _env_and_calls(ai=_AI())
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot good morning, how are you?",
    })))

    assert response["status"] == 200
    assert response["data"]["action"] == "help"
    assert calls["inserted"] == []


def test_forkbot_forwards_conversation_context_to_the_ai():
    # "log that" only makes sense with the prior conversation; the recent
    # messages must reach the AI prompt.
    class _AI:
        async def run(self, model, payload):
            self.payload = payload
            return {"response": json.dumps({
                "intent": "create_issue",
                "title": "Clone hangs on large repos",
                "body": "Cloning a large repo stalls at 90%.",
            })}

    ai = _AI()
    env, calls, ns = _env_and_calls(ai=ai, catalog_issue_max=0)
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot log that as an issue",
        "sender": "alice",
        "context": [
            {"sender": "bob", "text": "cloning the monorepo hangs at 90%"},
            {"sender": "alice", "text": "same here, never finishes"},
        ],
    })))

    assert response["status"] == 201
    assert response["data"]["issueNumber"] == 1
    user_prompt = ai.payload["messages"][1]["content"]
    assert "cloning the monorepo hangs at 90%" in user_prompt
    assert "log that as an issue" in user_prompt


def test_forkbot_issue_numbers_increment_and_reanchor_to_catalog():
    seq = {}

    async def d1_first(_env, sql, *args):
        if "FROM repositories WHERE key_bi=?" in sql:
            return {"data": {
                "issueMaxNumber": str(_env.catalog_max),
                "pullCount": "2",
            }}
        if sql.startswith("INSERT INTO issue_seq"):
            key, inserted_next, seed_next = args
            if key in seq:
                seq[key] = max(seq[key], seed_next) + 1
            else:
                seq[key] = inserted_next
            return {"number": seq[key] - 1}
        if "SELECT next_number FROM issue_seq" in sql:
            return {"next_number": seq.get(args[0], 1)}
        raise AssertionError(sql)

    async def d1_run(_env, sql, *args):
        if sql.startswith("INSERT INTO issue_seq"):
            seq[args[0]] = max(seq.get(args[0], 0), args[1])
        elif sql.startswith("UPDATE issue_seq"):
            seq[args[1]] = args[0]
        else:
            raise AssertionError(sql)

    async def decrypt_row(_env, data):
        return dict(data)

    g = _load_forkbot({
        "d1_first": d1_first, "d1_run": d1_run, "decrypt_row": decrypt_row,
    })
    alloc = g["_forkbot_next_issue_number"]

    class _Env:
        catalog_max = 4

    env = _Env()
    # Seeds from catalog max (4) -> proposes 5, then 6 monotonically.
    assert asyncio.run(alloc(env, "bi:r", "o", "r")) == 5
    assert asyncio.run(alloc(env, "bi:r", "o", "r")) == 6
    # The desktop merged up to 9 and republished; the allocator re-anchors up.
    env.catalog_max = 9
    assert asyncio.run(alloc(env, "bi:r", "o", "r")) == 10
    # A stale/lower catalog max never drags the counter backward.
    env.catalog_max = 2
    assert asyncio.run(alloc(env, "bi:r", "o", "r")) == 11
    # Pull requests use an independent namespaced sequence while sharing the
    # already-deployed issue_seq table.
    assert asyncio.run(alloc(env, "bi:r", "o", "r", "pull")) == 3
    assert asyncio.run(alloc(env, "bi:r", "o", "r", "pull")) == 4
    assert seq["bi:r"] == 12
    assert seq["pull:bi:r"] == 5


def test_forkbot_attributed_body_credits_source_and_requester():
    ns = _load_forkbot()
    attribute = ns["_forkbot_attributed_body"]
    # Chat request with a known sender.
    assert attribute("fix the thing", "forkbot", "alice") == (
        "fix the thing\n\n---\n_Filed by ForkBot at @alice's request via chat._")
    # Anonymous chat request falls back to a source-only credit.
    assert attribute("fix the thing", "forkbot", "forkbot") == (
        "fix the thing\n\n---\n_Filed by ForkBot via chat._")
    # An empty body still carries the credit.
    assert attribute("", "forkbot", "alice") == (
        "_Filed by ForkBot at @alice's request via chat._")
    # The fediverse path builds its own attribution, so it is left untouched.
    assert attribute("mention body", "fediverse", "@bob@example.social") == (
        "mention body")


def test_forkbot_route_and_workers_ai_binding_are_configured():
    assert '"/api/forkbot/chat"' in ENTRY_TEXT
    assert "return await forkbot_chat_handler(self.env, request)" in ENTRY_TEXT
    assert '"/api/forkbot/models"' in ENTRY_TEXT
    assert "return await forkbot_models_handler(self.env, request)" in ENTRY_TEXT
    assert WRANGLER_DATA["ai"]["binding"] == "AI"
    # The deployed model is the code default; the manifest no longer restates
    # it (Workers Free caps a Worker at 64 variables).
    assert "FORKBOT_AI_MODEL" not in WRANGLER_DATA["vars"]
    assert _load_forkbot()["FORKBOT_AI_DEFAULT_MODEL"].startswith("@cf/")


def test_forkbot_model_catalog_lists_the_deployed_default():
    # Every pickable id is a Workers AI model id, and the model the deployment
    # actually runs is one of them (else the picker's "default" option would
    # be a model the relay never uses). The deployed model is the code default
    # unless FORKBOT_AI_MODEL is bound, which the manifest deliberately is not.
    ns = _load_forkbot()
    ids = [entry[0] for entry in ns["FORKBOT_AI_MODEL_CHOICES"]]
    assert ids
    assert all(model_id.startswith("@cf/") for model_id in ids)
    assert len(set(ids)) == len(ids)
    assert ns["FORKBOT_AI_DEFAULT_MODEL"] in ids
    assert "FORKBOT_AI_MODEL" not in WRANGLER_DATA["vars"]
    assert set(ids) == {
        "@cf/meta/llama-3.3-70b-instruct-fp8-fast",
        "@cf/meta/llama-4-scout-17b-16e-instruct",
        "@cf/google/gemma-4-26b-a4b-it",
        "@cf/meta/llama-3.1-8b-instruct-fast",
    }
    # Cloudflare retired both of these on 2026-05-30. Keep an explicit guard
    # so the server and offline desktop picker cannot accidentally reintroduce
    # an id that looks valid but no longer answers.
    assert "@cf/meta/llama-3.1-8b-instruct" not in ids
    assert "@cf/google/gemma-3-12b-it" not in ids


def test_forkbot_model_options_put_the_configured_default_first():
    ns = _load_forkbot()
    options = ns["_forkbot_ai_model_options"](
        type("E", (), {"FORKBOT_AI_MODEL":
                       "@cf/meta/llama-3.1-8b-instruct-fast"})())
    assert options[0]["id"] == "@cf/meta/llama-3.1-8b-instruct-fast"
    assert options[0]["default"] is True
    assert [option["default"] for option in options[1:]] == \
        [False] * (len(options) - 1)
    # A self-hosted relay pointing at its own model still gets a selectable
    # default option for it rather than a picker that cannot choose it.
    custom = ns["_forkbot_ai_model_options"](
        type("E", (), {"FORKBOT_AI_MODEL": "@cf/self/hosted-model"})())
    assert custom[0]["id"] == "@cf/self/hosted-model"
    assert custom[0]["default"] is True
    assert len(custom) == len(ns["FORKBOT_AI_MODEL_CHOICES"]) + 1


def test_forkbot_resolves_only_allowlisted_model_picks():
    ns = _load_forkbot()
    env = type("E", (), {"FORKBOT_AI_MODEL": ""})()
    resolve = ns["_forkbot_resolve_ai_model"]
    default = ns["FORKBOT_AI_DEFAULT_MODEL"]
    assert resolve(env, "@cf/meta/llama-4-scout-17b-16e-instruct") == \
        "@cf/meta/llama-4-scout-17b-16e-instruct"
    # Empty, unknown, and non-string picks all fall back to the default rather
    # than erroring: a stale browser pick must not turn ForkBot off, and an
    # unlisted id must never reach env.AI.run (it bills whatever it is handed).
    assert resolve(env, "") == default
    assert resolve(env, "@cf/expensive/not-offered") == default
    assert resolve(env, None) == default
    assert resolve(env, 17) == default


def test_forkbot_models_endpoint_lists_picks_for_the_composer():
    _env, _calls, ns = _env_and_calls()
    request = _Request({})
    request.method = "GET"
    response = asyncio.run(ns["forkbot_models_handler"](
        type("E", (), {"FORKBOT_AI_MODEL": ""})(), request))
    assert response["status"] == 200
    assert response["data"]["provider"] == "cloudflare-workers-ai"
    assert response["data"]["default"] == ns["FORKBOT_AI_DEFAULT_MODEL"]
    first = response["data"]["models"][0]
    assert first["id"] == ns["FORKBOT_AI_DEFAULT_MODEL"]
    assert first["label"] and first["description"]

    request.method = "POST"
    assert asyncio.run(ns["forkbot_models_handler"](
        type("E", (), {"FORKBOT_AI_MODEL": ""})(), request))["status"] == 405


def test_forkbot_chat_sends_the_prompt_to_the_picked_model():
    class _AI:
        async def run(self, model, payload):
            self.model = model
            self.payload = payload
            return {"response": json.dumps(
                {"title": "Cache clone refs", "body": "Refs re-read per clone."})}

    ai = _AI()
    env, calls, ns = _env_and_calls(ai=ai)
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot create an issue to cache clone refs",
        "model": "@cf/meta/llama-4-scout-17b-16e-instruct",
    })))
    assert response["status"] == 201
    assert ai.model == "@cf/meta/llama-4-scout-17b-16e-instruct"
    # The reply names the model that answered, so the composer can show it.
    assert response["data"]["model"] == "@cf/meta/llama-4-scout-17b-16e-instruct"
    assert calls["inserted"][0][1]["titleIfNew"] == "Cache clone refs"

    # An unlisted pick is ignored in favor of the deployment default.
    ai2 = _AI()
    env2, _calls2, ns2 = _env_and_calls(ai=ai2)
    asyncio.run(ns2["forkbot_chat_handler"](env2, _Request({
        "message": "forkbot create an issue to cache clone refs",
        "model": "@cf/somebody/expensive-model",
    })))
    assert ai2.model == ns2["FORKBOT_AI_DEFAULT_MODEL"]


def _ask_request(body, method="POST"):
    request = _Request(body)
    request.method = method
    return request


def _signed_ask_body(ns, prompt, model, account="jett", ts=None):
    return {
        "nodeName": account,
        "prompt": prompt,
        "model": model,
        "ts": str(ts if ts is not None else _Date.now()),
        "sig": "sig-" + account,
    }


def test_ai_ask_sends_the_prompt_to_the_picked_model():
    class _AI:
        async def run(self, model, payload):
            self.model = model
            self.payload = payload
            return {"response": "Rebase onto main, then force-push the branch."}

    ai = _AI()
    env, calls, ns = _env_and_calls(ai=ai)
    response = asyncio.run(ns["ai_ask_handler"](env, _ask_request(
        _signed_ask_body(ns, "how do I redo this branch?",
                         "@cf/meta/llama-4-scout-17b-16e-instruct"))))

    assert response["status"] == 200
    assert response["data"]["model"] == "@cf/meta/llama-4-scout-17b-16e-instruct"
    assert response["data"]["reply"] == (
        "Rebase onto main, then force-push the branch.")
    assert ai.model == "@cf/meta/llama-4-scout-17b-16e-instruct"
    # A plain answer, not ForkBot's JSON-mode issue schema, and bounded output.
    assert "response_format" not in ai.payload
    assert ai.payload["max_tokens"] == ns["AI_ASK_MAX_TOKENS"]
    assert ai.payload["messages"][0]["content"] == ns["AI_ASK_SYSTEM_PROMPT"]
    # The signature covers the account, timestamp, model and a digest of the
    # prompt, so it cannot be replayed with different text or a costlier model.
    _owner, _sig, canonical = calls["signed"][0]
    lines = canonical.split("\n")
    assert lines[0] == "forkmesh-ai-ask-v1"
    assert lines[1] == "jett"
    assert lines[3] == "@cf/meta/llama-4-scout-17b-16e-instruct"
    assert lines[4] == hashlib.sha256(
        b"how do I redo this branch?").hexdigest()
    # Every answered prompt is counted against this account's window.
    assert calls["aiAskRate"] == ["bi:jett"]


def test_ai_workspace_name_returns_a_structured_short_description():
    class _AI:
        async def run(self, model, payload):
            self.model = model
            self.payload = payload
            return {"response": {"description": "clean cloudflare prompts"}}

    ai = _AI()
    env, calls, ns = _env_and_calls(ai=ai)
    prompt = "Please set up Cloudflare so it can clean our agent prompts"
    model = "@cf/meta/llama-3.1-8b-instruct-fast"
    response = asyncio.run(ns["ai_workspace_name_handler"](
        env, _ask_request(_signed_ask_body(ns, prompt, model))))

    assert response == {"status": 200, "data": {
        "ok": True,
        "model": model,
        "description": "clean cloudflare prompts",
    }}
    assert ai.model == model
    assert ai.payload["max_tokens"] == ns["AI_WORKSPACE_NAME_MAX_TOKENS"]
    assert ai.payload["messages"][0]["content"] == \
        ns["AI_WORKSPACE_NAME_SYSTEM_PROMPT"]
    assert ai.payload["response_format"]["type"] == "json_schema"
    _owner, _sig, canonical = calls["signed"][0]
    lines = canonical.split("\n")
    assert lines[:4] == ["forkmesh-ai-workspace-name-v1", "jett",
                         str(_Date.now()), model]
    assert lines[4] == hashlib.sha256(prompt.encode()).hexdigest()
    assert calls["aiAskRate"] == ["bi:jett"]


def test_ai_workspace_name_rejects_unsigned_and_falls_back_on_unusable_results():
    env, _calls, ns = _env_and_calls(signature_ok=False)
    body = _signed_ask_body(ns, "fix it", "@cf/meta/llama-3.1-8b-instruct-fast")
    assert asyncio.run(ns["ai_workspace_name_handler"](
        env, _ask_request(body)))["status"] == 401

    class _AI:
        async def run(self, _model, _payload):
            return {"response": {"wrong": "field"}}

    env, _calls, ns = _env_and_calls(ai=_AI())
    response = asyncio.run(ns["ai_workspace_name_handler"](
        env, _ask_request(body)))
    assert response == {"status": 200, "data": {
        "ok": True, "model": "", "description": "fix",
        "fallback": "unusable_response"}}


def test_ai_workspace_name_without_an_ai_binding_answers_a_default_name():
    env, _calls, ns = _env_and_calls(ai=None)
    body = _signed_ask_body(
        ns, "Please fix the login redirect loop on Safari for our users",
        "@cf/meta/llama-3.1-8b-instruct-fast")
    response = asyncio.run(ns["ai_workspace_name_handler"](
        env, _ask_request(body)))
    assert response == {"status": 200, "data": {
        "ok": True, "model": "",
        "description": "fix login redirect loop safari users",
        "fallback": "missing_binding"}}
    assert ns["_ai_workspace_name_fallback"]("the a to") == "agent workspace"


def test_ai_workspace_name_route_is_wired():
    assert '"/api/ai/workspace-name"' in ENTRY_TEXT
    assert "return await ai_workspace_name_handler(self.env, request)" \
        in ENTRY_TEXT


def test_ai_ask_dispatches_to_every_published_model():
    ns = _load_forkbot()
    for model_id, _label, _description in ns["FORKBOT_AI_MODEL_CHOICES"]:
        class _AI:
            async def run(self, model, payload):
                self.model = model
                self.payload = payload
                return {"response": "answer from " + model}

        ai = _AI()
        env, _calls, loaded = _env_and_calls(ai=ai)
        response = asyncio.run(loaded["ai_ask_handler"](
            env,
            _ask_request(_signed_ask_body(
                loaded, "identify yourself", model_id)),
        ))
        assert response["status"] == 200, model_id
        assert response["data"]["model"] == model_id
        assert response["data"]["reply"] == "answer from " + model_id
        assert ai.model == model_id
        assert ai.payload["messages"][1] == {
            "role": "user", "content": "identify yourself"}


def test_ai_ask_reads_current_chat_completion_response_shapes():
    responses = {
        "@cf/meta/llama-4-scout-17b-16e-instruct": {
            "choices": [{"message": {"content": "Llama answered."}}]},
        "@cf/google/gemma-4-26b-a4b-it": {
            "choices": [{"message": {"content": [
                {"type": "text", "text": "Gemma "},
                {"type": "text", "text": "answered."},
            ]}}]},
    }

    class _AI:
        async def run(self, model, _payload):
            return responses[model]

    for model_id, expected in (
        ("@cf/meta/llama-4-scout-17b-16e-instruct", "Llama answered."),
        ("@cf/google/gemma-4-26b-a4b-it", "Gemma answered."),
    ):
        env, _calls, ns = _env_and_calls(ai=_AI())
        response = asyncio.run(ns["ai_ask_handler"](
            env,
            _ask_request(_signed_ask_body(ns, "hello", model_id)),
        ))
        assert response["status"] == 200
        assert response["data"]["model"] == model_id
        assert response["data"]["reply"] == expected


def test_ai_ask_falls_back_without_running_an_unpublished_model():
    class _AI:
        async def run(self, model, _payload):
            self.model = model
            return {"response": "Answered by the published default."}

    ai = _AI()
    env, calls, ns = _env_and_calls(ai=ai)
    response = asyncio.run(ns["ai_ask_handler"](env, _ask_request(
        _signed_ask_body(ns, "why is build flaky?",
                         "@cf/ghost/llama-999"))))
    assert response["status"] == 200
    assert response["data"]["model"] == ns["FORKBOT_AI_DEFAULT_MODEL"]
    assert ai.model == ns["FORKBOT_AI_DEFAULT_MODEL"]
    assert ai.model != "@cf/ghost/llama-999"
    assert calls["aiAskRate"] == ["bi:jett"]


def test_ai_ask_reports_all_published_models_missing():
    class _AI:
        async def run(self, _model, _payload):
            raise RuntimeError("5007: No such model or task")

    env, calls, ns = _env_and_calls(ai=_AI())
    response = asyncio.run(ns["ai_ask_handler"](env, _ask_request(
        _signed_ask_body(ns, "why is build flaky?",
                         "@cf/meta/llama-4-scout-17b-16e-instruct"))))
    assert response["status"] == 502
    assert response["data"]["error"] == "model_not_found"
    assert calls["aiAskRate"] == ["bi:jett"]


def test_ai_ask_retries_with_fallback_model_on_model_not_found():
    class _AI:
        def __init__(self):
            self.models = []

        async def run(self, model, payload):
            self.models.append(model)
            if model == "@cf/meta/llama-4-scout-17b-16e-instruct":
                raise RuntimeError("The AI model was not found.")
            return {"response": "Use fallback model response."}

    env, calls, ns = _env_and_calls(ai=_AI())
    response = asyncio.run(ns["ai_ask_handler"](env, _ask_request(
        _signed_ask_body(ns, "how do I redo this branch?",
                         "@cf/meta/llama-4-scout-17b-16e-instruct"))))

    assert response["status"] == 200
    assert response["data"]["model"] == ns["FORKBOT_AI_DEFAULT_MODEL"]
    assert response["data"]["reply"] == "Use fallback model response."
    assert env.AI.models == [
        "@cf/meta/llama-4-scout-17b-16e-instruct",
        ns["FORKBOT_AI_DEFAULT_MODEL"],
    ]
    # Every answered prompt is counted against this account's window.
    assert calls["aiAskRate"] == ["bi:jett"]


def test_ai_ask_answers_a_pick_this_relay_does_not_allowlist():
    # The desktop signs the model IT picked. When that id is not in this
    # relay's allowlist the prompt still has to be answered by the default
    # model: verifying the *resolved* id instead turned every unknown pick
    # into a bogus 401 "not authorized to sign for the account".
    class _AI:
        async def run(self, model, _payload):
            self.model = model
            return {"response": "Answered by the default model."}

    ai = _AI()
    env, calls, ns = _env_and_calls(
        ai=ai, signed_model="@cf/newer/model-this-relay-never-heard-of")
    response = asyncio.run(ns["ai_ask_handler"](env, _ask_request(
        _signed_ask_body(ns, "how do I redo this branch?",
                         "@cf/newer/model-this-relay-never-heard-of"))))

    assert response["status"] == 200
    assert response["data"]["model"] == ns["FORKBOT_AI_DEFAULT_MODEL"]
    assert ai.model == ns["FORKBOT_AI_DEFAULT_MODEL"]
    # The verified signature covers exactly the string the client sent.
    assert calls["signed"][0][2].split("\n")[3] == (
        "@cf/newer/model-this-relay-never-heard-of")
    # An allowlisted pick keeps verifying against a relay-resolved id too, so
    # older clients that signed whatever the relay handed them still work.
    resolved_env, _resolved_calls, resolved_ns = _env_and_calls(
        ai=_AI(), signed_model=None)
    assert asyncio.run(resolved_ns["ai_ask_handler"](
        resolved_env, _ask_request(_signed_ask_body(
            resolved_ns, "hello", "@cf/google/gemma-4-26b-a4b-it"))
    ))["status"] == 200


def test_ai_ask_reports_model_not_found_apart_from_other_failures():
    # Every candidate rejected as "no such model" is the one failure the person
    # at the composer can act on, so it must not read as "the model did not
    # answer" (nor as the relay missing the endpoint entirely).
    class _AI:
        async def run(self, _model, _payload):
            raise RuntimeError("5007: No such model or task")

    env, _calls, ns = _env_and_calls(ai=_AI())
    response = asyncio.run(ns["ai_ask_handler"](env, _ask_request(
        _signed_ask_body(ns, "why is the build red?", ""))))
    assert response["status"] == 502
    assert response["data"]["error"] == "model_not_found"

    # The desktop consumer of these errors is now the bundled agent script
    # (adhoc #1634): it keeps a bare 404 — a relay that predates the agent
    # endpoint — distinct from auth failures instead of blaming the model.
    script = (ROOT.parent / "desktop" / "src" /
              "CloudflareAgentScript.h").read_text(encoding="utf-8")
    assert "err.code == 404" in script
    assert "does not run AI agents yet" in script
    assert "err.code in (401, 403)" in script


def test_ai_ask_refuses_unsigned_stale_and_unverified_callers():
    env, calls, ns = _env_and_calls(ai=object())
    # No signature at all.
    unsigned = _signed_ask_body(ns, "hello", "")
    unsigned.pop("sig")
    assert asyncio.run(ns["ai_ask_handler"](
        env, _ask_request(unsigned)))["status"] == 401
    # A signature from outside the skew window.
    stale = _signed_ask_body(ns, "hello", "",
                             ts=_Date.now() - ns["LOGIN_MAX_SKEW_MS"] - 1)
    assert asyncio.run(ns["ai_ask_handler"](
        env, _ask_request(stale)))["data"]["error"] == "stale_signature"
    # GET is not an answer path, and an empty prompt is not a question.
    assert asyncio.run(ns["ai_ask_handler"](
        env, _ask_request(_signed_ask_body(ns, "hi", ""), method="GET")
    ))["status"] == 405
    blank = _signed_ask_body(ns, "   ", "")
    assert asyncio.run(ns["ai_ask_handler"](
        env, _ask_request(blank)))["data"]["error"] == "prompt_required"
    # Nothing billed a Workers AI call on any of these paths.
    assert calls["aiAskRate"] == []

    # A key that is not trusted for the claimed account is rejected, so the
    # relay never answers (and never bills) for a self-asserted node name.
    bad_env, bad_calls, bad_ns = _env_and_calls(ai=object(), signature_ok=False)
    denied = asyncio.run(bad_ns["ai_ask_handler"](
        bad_env, _ask_request(_signed_ask_body(bad_ns, "hello", ""))))
    assert denied["status"] == 401
    assert denied["data"]["error"] == "unauthorized"
    assert bad_calls["aiAskRate"] == []


def test_ai_ask_holds_a_per_account_hourly_window():
    class _AI:
        async def run(self, _model, _payload):
            return {"response": "ok"}

    env, _calls, ns = _env_and_calls(
        ai=_AI(), ai_ask_used=ns_max_per_window_seed())
    response = asyncio.run(ns["ai_ask_handler"](env, _ask_request(
        _signed_ask_body(ns, "one more", ""))))
    assert response["status"] == 429
    assert response["data"]["error"] == "rate_limited"
    assert response["data"]["retryAfterMs"] > 0
    assert response["headers"]["x-forkmesh-expected-degraded"] == "1"
    assert int(response["headers"]["Retry-After"]) > 0


def ns_max_per_window_seed():
    # The window is already full, so the next prompt is the one over the line.
    return _load_forkbot()["AI_ASK_MAX_PER_WINDOW"]


def test_ai_ask_reports_an_unusable_model_instead_of_an_empty_answer():
    class _AI:
        async def run(self, _model, _payload):
            raise RuntimeError("model unavailable")

    env, _calls, ns = _env_and_calls(ai=_AI())
    response = asyncio.run(ns["ai_ask_handler"](env, _ask_request(
        _signed_ask_body(ns, "why is the build red?", ""))))
    assert response["status"] == 502
    assert response["data"]["error"] == "ai_unavailable"
    assert response["data"]["model"] == ns["FORKBOT_AI_DEFAULT_MODEL"]


def test_ai_ask_route_and_rate_table_are_wired():
    assert '"/api/ai/ask"' in ENTRY_TEXT
    assert "return await ai_ask_handler(self.env, request)" in ENTRY_TEXT
    schema = (ROOT / "src" / "schema.py").read_text(encoding="utf-8")
    assert "CREATE TABLE IF NOT EXISTS ai_ask_rate" in schema


# --- /api/ai/agent: tool-use turns for the desktop's Workers AI coding agent
#     (adhoc #1634). One request per model turn: full conversation in, the
#     model's reply plus normalized toolCalls out.


def _signed_agent_body(ns, messages, model="", account="jett", ts=None,
                       tools=None, max_tokens=None):
    body = {
        "nodeName": account,
        "messages": messages,
        "model": model,
        "ts": str(ts if ts is not None else _Date.now()),
        "sig": "sig-" + account,
    }
    if tools is not None:
        body["tools"] = tools
    if max_tokens is not None:
        body["max_tokens"] = max_tokens
    return body


AGENT_BASH_TOOL = [{
    "type": "function",
    "function": {
        "name": "bash",
        "description": "Run a shell command in the worktree.",
        "parameters": {"type": "object",
                       "properties": {"command": {"type": "string"}},
                       "required": ["command"]},
    },
}]


def test_ai_agent_turn_passes_tools_and_returns_tool_calls():
    # Legacy Workers AI shape: top-level "response" text with parsed
    # "tool_calls" beside it.
    class _AI:
        async def run(self, model, payload):
            self.model = model
            self.payload = payload
            return {
                "response": "Let me look around first.",
                "tool_calls": [{"name": "bash",
                                "arguments": {"command": "ls"}}],
                "usage": {"prompt_tokens": 120, "completion_tokens": 30},
            }

    ai = _AI()
    env, calls, ns = _env_and_calls(ai=ai)
    response = asyncio.run(ns["ai_agent_handler"](env, _ask_request(
        _signed_agent_body(
            ns, [{"role": "user", "content": "fix the flaky test"}],
            "@cf/meta/llama-3.3-70b-instruct-fp8-fast",
            tools=AGENT_BASH_TOOL))))

    assert response["status"] == 200
    assert response["data"]["model"] == (
        "@cf/meta/llama-3.3-70b-instruct-fp8-fast")
    assert response["data"]["reply"] == "Let me look around first."
    assert response["data"]["toolCalls"] == [
        {"id": "call_0", "name": "bash", "arguments": {"command": "ls"}}]
    assert response["data"]["usage"] == {"inputTokens": 120,
                                         "outputTokens": 30}
    # The tool declaration reached the model, rebuilt (not passed through).
    assert ai.payload["tools"] == [{
        "type": "function",
        "function": {
            "name": "bash",
            "description": "Run a shell command in the worktree.",
            "parameters": AGENT_BASH_TOOL[0]["function"]["parameters"],
        },
    }]
    assert ai.payload["max_tokens"] == ns["AI_AGENT_MAX_TOKENS"]
    # The run ticket covers the account and launch timestamp only (the
    # conversation grows every turn, so it cannot be digest-bound like ask's).
    _owner, _sig, canonical = calls["signed"][0]
    assert canonical.split("\n") == ["forkmesh-ai-agent-v1", "jett",
                                    str(_Date.now())]
    # Metered on the account's agent window, not the composer ask window.
    assert calls["aiAskRate"] == ["bi:jett:agent"]


def test_ai_agent_turn_reads_openai_tool_call_shape():
    # Current chat-completions shape: tool_calls under choices[].message with
    # JSON-string arguments, which come back parsed for the script.
    class _AI:
        async def run(self, _model, _payload):
            return {"choices": [{"message": {
                "content": "Running git status.",
                "tool_calls": [{
                    "id": "chatcmpl-tool-1",
                    "type": "function",
                    "function": {"name": "bash",
                                 "arguments": '{"command": "git status"}'},
                }],
            }}]}

    env, _calls, ns = _env_and_calls(ai=_AI())
    response = asyncio.run(ns["ai_agent_handler"](env, _ask_request(
        _signed_agent_body(
            ns, [{"role": "user", "content": "what changed?"}],
            tools=AGENT_BASH_TOOL))))
    assert response["status"] == 200
    assert response["data"]["reply"] == "Running git status."
    assert response["data"]["toolCalls"] == [{
        "id": "chatcmpl-tool-1", "name": "bash",
        "arguments": {"command": "git status"}}]


def test_ai_agent_turn_replays_history_and_drops_junk():
    class _AI:
        async def run(self, _model, payload):
            self.payload = payload
            return {"response": "All done.", "tool_calls": []}

    ai = _AI()
    env, _calls, ns = _env_and_calls(ai=ai)
    response = asyncio.run(ns["ai_agent_handler"](env, _ask_request(
        _signed_agent_body(ns, [
            {"role": "user", "content": "fix it"},
            {"role": "assistant", "content": "",
             "tool_calls": [{"id": "call_0", "function": {
                 "name": "bash",
                 "arguments": {"command": "ls"}}}]},
            {"role": "tool", "tool_call_id": "call_0", "name": "bash",
             "content": "(exit code 0)\nREADME.md"},
            {"role": "wizard", "content": "not a chat role"},
            "not even a dict",
        ]))))
    assert response["status"] == 200
    sent = ai.payload["messages"]
    assert [m["role"] for m in sent] == ["user", "assistant", "tool"]
    # Echoed assistant tool_calls go to the model in the OpenAI wire shape:
    # arguments re-serialized as a JSON string.
    assert sent[1]["tool_calls"] == [{
        "id": "call_0", "type": "function",
        "function": {"name": "bash", "arguments": '{"command": "ls"}'}}]
    assert sent[2]["tool_call_id"] == "call_0"


def test_ai_agent_ticket_outlives_ask_skew_but_not_the_run_window():
    class _AI:
        async def run(self, _model, _payload):
            return {"response": "ok"}

    env, calls, ns = _env_and_calls(ai=_AI())
    messages = [{"role": "user", "content": "hello"}]
    # A mid-run turn signed at launch: far older than ask's skew window, still
    # inside the run ticket's own window.
    mid_run = _signed_agent_body(
        ns, messages, ts=_Date.now() - ns["LOGIN_MAX_SKEW_MS"] - 1)
    assert asyncio.run(ns["ai_agent_handler"](
        env, _ask_request(mid_run)))["status"] == 200
    # Beyond the run window (or from the future) the ticket is dead.
    expired = _signed_agent_body(
        ns, messages, ts=_Date.now() - ns["AI_AGENT_TICKET_MAX_AGE_MS"] - 1)
    assert asyncio.run(ns["ai_agent_handler"](
        env, _ask_request(expired)))["data"]["error"] == "stale_signature"
    future = _signed_agent_body(
        ns, messages, ts=_Date.now() + ns["LOGIN_MAX_SKEW_MS"] + 1)
    assert asyncio.run(ns["ai_agent_handler"](
        env, _ask_request(future)))["data"]["error"] == "stale_signature"
    # No signature, wrong method, no conversation.
    unsigned = _signed_agent_body(ns, messages)
    unsigned.pop("sig")
    assert asyncio.run(ns["ai_agent_handler"](
        env, _ask_request(unsigned)))["status"] == 401
    assert asyncio.run(ns["ai_agent_handler"](
        env, _ask_request(_signed_agent_body(ns, messages), method="GET")
    ))["status"] == 405
    assert asyncio.run(ns["ai_agent_handler"](
        env, _ask_request(_signed_agent_body(ns, []))
    ))["data"]["error"] == "messages_required"

    # An untrusted key never runs (or bills) a turn.
    bad_env, bad_calls, bad_ns = _env_and_calls(
        ai=object(), signature_ok=False)
    denied = asyncio.run(bad_ns["ai_agent_handler"](
        bad_env, _ask_request(_signed_agent_body(bad_ns, messages))))
    assert denied["status"] == 401
    assert denied["data"]["error"] == "unauthorized"
    assert bad_calls["aiAskRate"] == []


def test_ai_agent_holds_its_own_larger_rate_window():
    ns = _load_forkbot()
    # Agent loops make many turns per task, so their window must be roomier
    # than the composer's one-shot ask allowance.
    assert ns["AI_AGENT_MAX_PER_WINDOW"] > ns["AI_ASK_MAX_PER_WINDOW"]

    class _AI:
        async def run(self, _model, _payload):
            return {"response": "ok"}

    env, calls, loaded = _env_and_calls(
        ai=_AI(), ai_ask_used=ns["AI_AGENT_MAX_PER_WINDOW"])
    response = asyncio.run(loaded["ai_agent_handler"](env, _ask_request(
        _signed_agent_body(loaded,
                           [{"role": "user", "content": "one more"}]))))
    assert response["status"] == 429
    assert response["data"]["error"] == "rate_limited"
    assert response["data"]["retryAfterMs"] > 0
    # The agent window keys on its own suffixed row, leaving ask's untouched.
    assert calls["aiAskRate"] == ["bi:jett:agent"]


def test_ai_agent_caps_the_output_tokens_a_client_may_request():
    class _AI:
        async def run(self, _model, payload):
            self.payload = payload
            return {"response": "ok"}

    ai = _AI()
    env, _calls, ns = _env_and_calls(ai=ai)
    assert asyncio.run(ns["ai_agent_handler"](env, _ask_request(
        _signed_agent_body(ns, [{"role": "user", "content": "hi"}],
                           max_tokens=999999))))["status"] == 200
    assert ai.payload["max_tokens"] == ns["AI_AGENT_MAX_TOKENS"]


def test_ai_agent_keeps_the_newest_turns_over_the_message_cap():
    # Trimming the tail hid the model's latest tool results from it, so a long
    # run kept re-issuing calls it had already made.
    class _AI:
        async def run(self, _model, payload):
            self.payload = payload
            return {"response": "ok"}

    ai = _AI()
    env, _calls, ns = _env_and_calls(ai=ai)
    cap = ns["AI_AGENT_MAX_MESSAGES"]
    history = [{"role": "system", "content": "rules"}]
    for n in range(cap):
        history.append({"role": "assistant", "content": "call %d" % n})
        history.append({"role": "tool", "tool_call_id": "c%d" % n,
                        "content": "result %d" % n})
    history.append({"role": "user", "content": "steer"})
    history.append({"role": "user", "content": "latest"})
    response = asyncio.run(ns["ai_agent_handler"](env, _ask_request(
        _signed_agent_body(ns, history))))
    assert response["status"] == 200
    sent = ai.payload["messages"]
    assert len(sent) <= cap
    assert sent[0] == {"role": "system", "content": "rules"}
    # The front of the kept window never opens on an orphaned tool result.
    assert sent[1]["role"] == "assistant"
    assert sent[-1]["content"] == "latest"


def test_ai_agent_flags_tool_arguments_that_do_not_parse():
    class _AI:
        async def run(self, _model, _payload):
            return {"choices": [{"message": {"content": "", "tool_calls": [
                {"id": "t1", "type": "function", "function": {
                    "name": "bash", "arguments": '{"command": "echo hi'}},
                {"id": "t2", "type": "function", "function": {
                    "name": "bash", "arguments": "{ }"}},
            ]}}]}

    env, _calls, ns = _env_and_calls(ai=_AI())
    response = asyncio.run(ns["ai_agent_handler"](env, _ask_request(
        _signed_agent_body(ns, [{"role": "user", "content": "go"}],
                           tools=AGENT_BASH_TOOL))))
    calls = response["data"]["toolCalls"]
    assert calls[0]["arguments"] == {}
    assert calls[0]["invalidArguments"] == '{"command": "echo hi'
    assert calls[1] == {"id": "t2", "name": "bash", "arguments": {}}


def test_ai_agent_answers_a_context_overflow_with_413_and_no_fallback():
    class _AI:
        def __init__(self):
            self.models = []

        async def run(self, model, _payload):
            self.models.append(model)
            raise RuntimeError(
                "AiError: 5021: The input exceeded the maximum context window")

    ai = _AI()
    env, _calls, ns = _env_and_calls(ai=ai)
    response = asyncio.run(ns["ai_agent_handler"](env, _ask_request(
        _signed_agent_body(ns, [{"role": "user", "content": "go"}]))))
    assert response["status"] == 413
    assert response["data"]["error"] == "context_too_long"
    # Every fallback model has the same or a smaller window: bill one attempt.
    assert len(ai.models) == 1


def test_ai_agent_passes_a_clamped_temperature():
    class _AI:
        def __init__(self):
            self.payloads = []

        async def run(self, _model, payload):
            self.payloads.append(payload)
            return {"response": "ok"}

    ai = _AI()
    env, _calls, ns = _env_and_calls(ai=ai)
    for temperature in (0.2, 7, "hot", None):
        body = _signed_agent_body(ns, [{"role": "user", "content": "go"}])
        if temperature is not None:
            body["temperature"] = temperature
        assert asyncio.run(ns["ai_agent_handler"](
            env, _ask_request(body)))["status"] == 200
    assert [payload.get("temperature") for payload in ai.payloads] == [
        0.2, 1.5, None, None]


def test_ai_agent_route_is_wired():
    assert '"/api/ai/agent"' in ENTRY_TEXT
    assert "return await ai_agent_handler(self.env, request)" in ENTRY_TEXT
    assert 'ai_agent_handler = _forkbot.export("ai_agent_handler")' \
        in ENTRY_TEXT


def test_qt_composer_runs_cloudflare_models_as_agents():
    qt = ROOT.parent / "desktop" / "src"
    internal = "".join(
        (qt / name).read_text(encoding="utf-8")
        for name in ("MainWindowInternal.h", "MainWindowInternal1.cpp")
    )
    chat = "".join(
        (qt / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_CHAT_SOURCES
    )
    issues = (qt / "MainWindowIssues.cpp").read_text(encoding="utf-8")
    launch = (qt / "MainWindowAgentLaunch.cpp").read_text(encoding="utf-8")
    settings = (qt / "MainWindowSettings.cpp").read_text(encoding="utf-8")
    store = (qt / "AgentStore.cpp").read_text(encoding="utf-8")
    runner = (qt / "AgentRunner.cpp").read_text(encoding="utf-8")
    script = (qt / "CloudflareAgentScript.h").read_text(encoding="utf-8")
    assert 'kCloudflareAiProvider = QStringLiteral("cloudflare-ai")' in internal
    assert "return provider == kCloudflareAiProvider;" in internal
    assert "populateCloudflareAiModelCombo" in internal
    assert "populateCloudflareAiModelCombo(&cloudflareModels)" in chat
    assert "addItem(QStringLiteral(\"CF AI\")" in chat
    assert "/ai/models/search" in chat
    assert "kCloudflareAiModelsCacheSetting" in chat
    assert "agentModelIsCloudflareStyle" in internal
    assert 'startsWith(QLatin1String("@cf/"))' in internal
    assert "agentIsCloudflareAiProvider(provider)" in issues
    assert "sendPromptToCloudflareAi" not in issues
    assert "config.command = defaultCloudflareAiCommand();" in launch
    assert 'config.apiKeyName = QStringLiteral("FORKMESH_AGENT_API_TOKEN");' \
        in launch
    assert "cloudflareApiTokenFromVariables(variables)" in launch
    assert 'QStringLiteral("FORKMESH_AGENT_ACCOUNT_ID=") + accountId' in launch
    assert "FORKMESH_AI_AGENT_AUTH" not in launch
    assert "forkmesh_cloudflare_agent.py" in internal
    assert 'env.insert(QStringLiteral("FORKMESH_AGENT_PROVIDER"), provider);' \
        in runner
    assert 'value("API_TOKEN")' in script
    assert 'value("ACCOUNT_ID")' in script
    assert "/ai/v1/chat/completions" in script
    assert 'payload["tools"] = tools' in script
    assert "tool_call_id" in script
    ns = _load_forkbot()
    allowed = {entry[0] for entry in ns["FORKBOT_AI_MODEL_CHOICES"]}
    fallback = re.findall(r'QStringLiteral\("(@cf/[^"]+)"\)', internal)
    assert fallback
    assert set(fallback) == allowed
    assert "Branch/worktree names" in settings
    assert "kAgentWorkspaceNameModelSetting" in settings
    assert "requestCloudflareWorkspaceName(" in launch
    assert "stored->branchName = sharedName" in launch
    assert "stored->worktreeName = sharedName" in launch
    assert 'obj["worktreeName"] = worktreeName' in store
    assert "m_session.worktreeName.isEmpty()" in runner


def _cloudflare_agent_script(tmp_path):
    header = (ROOT.parent / "desktop" / "src" /
              "CloudflareAgentScript.h").read_text(encoding="utf-8")
    body = re.search(r'R"PYAGENT\((.*)\)PYAGENT"', header, re.S)
    assert body, "CloudflareAgentScript.h no longer holds a PYAGENT literal"
    path = tmp_path / "forkmesh_cloudflare_agent.py"
    path.write_text(body.group(1), encoding="utf-8")
    return path


def _run_cloudflare_agent(tmp_path, turns, steer=None, fail=None, reply=None):
    if not shutil.which("git"):
        pytest.skip("git is not installed")

    script = _cloudflare_agent_script(tmp_path)
    seen = []
    requests = []

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            size = int(self.headers.get("content-length") or 0)
            body = json.loads(self.rfile.read(size))
            requests.append(body)
            status = (fail(len(requests)) if fail else 0) or 0
            if status:
                detail = json.dumps({
                    "title": "Error 1101: Worker threw exception",
                    "status": status,
                }).encode("utf-8")
                self.send_response(status)
                self.send_header("content-type", "application/json")
                self.send_header("content-length", str(len(detail)))
                self.end_headers()
                self.wfile.write(detail)
                return
            seen.append(body["messages"])
            calls = turns(len(seen), body["messages"])
            payload = json.dumps({
                "model": body.get("model"),
                "choices": [{
                    "message": {
                        "role": "assistant",
                        "content": (reply(len(seen), calls) if reply
                                    else ("" if calls else "Done.")),
                        "tool_calls": calls,
                    },
                    "finish_reason": "tool_calls" if calls else "stop",
                }],
                "usage": {"prompt_tokens": 1, "completion_tokens": 1},
            }).encode("utf-8")
            self.send_response(200)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

    server = HTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()

    home = tmp_path / "home"
    home.mkdir()
    (home / ".gitconfig").write_text(
        "[user]\n\tname = Real Person\n\temail = real@example.com\n",
        encoding="utf-8")
    repo = tmp_path / "repo"
    repo.mkdir()
    env = dict(os.environ, HOME=str(home))
    env.pop("GIT_CONFIG_GLOBAL", None)
    for name in list(env):
        if name.startswith(("FORKMESH_AGENT_", "CLOUDFLARE_")) or \
                name == "FORKMESH_AI_AGENT_AUTH":
            env.pop(name)
    subprocess.run(["git", "init", "-q", "-b", "main", str(repo)],
                   env=env, check=True)
    (repo / "README.md").write_text("# Title\n", encoding="utf-8")
    subprocess.run(["git", "add", "-A"], cwd=repo, env=env, check=True)
    subprocess.run(["git", "commit", "-qm", "init"], cwd=repo, env=env,
                   check=True)

    prompt = tmp_path / "prompt.md"
    prompt.write_text("add the note", encoding="utf-8")
    env["FORKMESH_AGENT_PROVIDER"] = "cloudflare-ai"
    env["FORKMESH_AGENT_ENDPOINT"] = (
        "http://127.0.0.1:%d/ai/v1/chat/completions"
        % server.server_address[1])
    env["FORKMESH_AGENT_API_TOKEN"] = "token"
    env["FORKMESH_AGENT_STREAMING"] = "0"
    env["FORKMESH_AGENT_RETRY_BASE_DELAY"] = "0"
    env["FORKMESH_AGENT_MODEL"] = "@cf/meta/llama-3.3-70b-instruct-fp8-fast"
    proc = subprocess.Popen(
        [sys.executable, str(script), str(prompt)], cwd=repo, env=env,
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True)
    if steer:
        proc.stdin.write(steer)
        proc.stdin.flush()
    out = proc.stdout.read()
    code = proc.wait(timeout=180)
    server.shutdown()
    return out, code, repo, home, seen


UNTOUCHED_GITCONFIG = "[user]\n\tname = Real Person\n\temail = real@example.com\n"


def test_cloudflare_agent_script_stops_a_model_that_repeats_one_command(
        tmp_path):
    # The shipped failure: llama-3.3 answered every turn with the same
    # `echo ... >> README.md`, appending the line once per turn for as long as
    # anyone let it run.
    append = 'echo "edited by llama" >> README.md'
    out, code, repo, home, seen = _run_cloudflare_agent(
        tmp_path,
        lambda turn, messages: [
            {"id": "c%d" % turn, "name": "bash",
             "arguments": {"command": append}}])

    # The command lands once, however many times it is asked for. The run is
    # pushed back onto the task rather than abandoned on the third repeat
    # (adhoc #279), and still ends by itself well inside the turn budget.
    assert (repo / "README.md").read_text(encoding="utf-8") == \
        "# Title\nedited by llama\n"
    assert "Not run again: this is the command from turn 1" in out
    assert "That turn only repeated earlier work" in out
    assert "The model spent 100 turns re-running commands it had already run" \
        in out
    assert out.count("request #") <= 105
    # Exit 0: whatever the run did commit is still worth capturing as a patch.
    assert code == 0
    # Every turn carries the agent rules these small models need spelled out.
    assert seen[0][0]["role"] == "system"
    assert "stop calling tools" in seen[0][0]["content"]
    assert (home / ".gitconfig").read_text(encoding="utf-8") == \
        UNTOUCHED_GITCONFIG


def test_cloudflare_agent_script_carries_on_after_a_model_repeats_itself(
        tmp_path):
    # adhoc #279: gemma re-sent one search on every turn, the script called
    # three of those a stall and ended the run with nothing done at all. A
    # repeat is now answered with the next step to take, and the run keeps
    # going for as many turns as the task needs.
    def turns(turn, _messages):
        if turn <= 4:
            return [{"id": "c%d" % turn, "name": "bash",
                     "arguments": {"command": "ls README.md"}}]
        if turn == 5:
            return [{"id": "c5", "name": "bash",
                     "arguments": {"command": 'echo "123" >> README.md'}}]
        if turn == 6:
            return [{"id": "c6", "name": "bash", "arguments": {"command":
                     'git config --global user.email "llama@cloudflare.com" && '
                     "git add -A && git commit -qm 123"}}]
        return []

    out, code, repo, _home, _seen = _run_cloudflare_agent(tmp_path, turns)

    assert code == 0
    assert "==> Agent finished." in out
    # The repeats are answered with what to do next instead of ending the run.
    assert "That turn only repeated earlier work" in out
    assert "re-running commands it had already run" not in out
    # The work the task asked for actually happened, four wasted turns later.
    assert (repo / "README.md").read_text(encoding="utf-8") == \
        "# Title\n123\n"
    assert "123" in subprocess.run(
        ["git", "log", "-1", "--pretty=%s"], cwd=repo, capture_output=True,
        text=True).stdout


def test_cloudflare_agent_script_works_the_task_and_takes_steering(tmp_path):
    commands = [
        "tail -n 2 README.md",
        'grep -qF "edited by llama" README.md || '
        'echo "edited by llama" >> README.md',
        "tail -n 2 README.md",  # a legitimate re-read, not a stalled repeat
        'git config --global user.email "llama@cloudflare.com" && '
        "git add -A && git commit -qm note",
    ]
    state = {"steered": False}

    def turns(turn, messages):
        # The steering arrives on stdin before the first turn; the script drains
        # stdin at every turn boundary, so answer it once and then work through
        # the command list.
        pending = any(
            message["role"] == "user" and "STEER" in (message.get("content") or "")
            for message in messages)
        if pending and not state["steered"]:
            state["steered"] = True
            return [{"id": "s", "name": "bash",
                     "arguments": {"command": 'echo "STEERED" >> README.md'}}]
        index = turn - (1 if state["steered"] else 0)
        if index <= len(commands):
            return [{"id": "c%d" % turn, "name": "bash",
                     "arguments": {"command": commands[index - 1]}}]
        return []

    out, code, repo, home, _seen = _run_cloudflare_agent(
        tmp_path, turns,
        steer="\n\nAdditional user instruction:\nSTEER: add a second line\n")

    assert code == 0
    assert "==> Agent finished." in out
    # The steering ForkMesh writes to the running script's stdin reaches the
    # model instead of being swallowed (the prompt itself is a file argument, so
    # nothing used to read stdin at all).
    assert "==> Steering:" in out
    assert "STEERED" in (repo / "README.md").read_text(encoding="utf-8")
    # Re-reading a file the run read earlier is not treated as a stall.
    assert "re-running commands it had already run" not in out
    assert out.count("$ tail -n 2 README.md") == 2
    # `git config --global` is sandboxed into the run, not the user's identity.
    assert (home / ".gitconfig").read_text(encoding="utf-8") == \
        UNTOUCHED_GITCONFIG
    # The work is committed, so ForkMesh can stamp provenance and open a PR.
    assert "note" in subprocess.run(
        ["git", "log", "-1", "--pretty=%s"], cwd=repo, capture_output=True,
        text=True).stdout


def test_cloudflare_agent_script_retries_a_5xx_instead_of_dropping_run(
        tmp_path):
    commands = ['echo "hello" >> README.md',
                "git add -A && git commit -qm hello"]

    def turns(turn, _messages):
        if turn <= len(commands):
            return [{"id": "c%d" % turn, "name": "bash",
                     "arguments": {"command": commands[turn - 1]}}]
        return []

    out, code, repo, _home, seen = _run_cloudflare_agent(
        tmp_path, turns, fail=lambda request: 500 if request == 2 else 0)

    assert code == 0
    # The run carried on rather than reporting the turn as fatal.
    assert "AI agent error 500" not in out
    assert "==> Agent finished." in out
    assert "hello" in (repo / "README.md").read_text(encoding="utf-8")
    assert "hello" in subprocess.run(
        ["git", "log", "-1", "--pretty=%s"], cwd=repo, capture_output=True,
        text=True).stdout
    # The retry replayed the same conversation, so no turn was lost.
    assert len(seen) == 3


def test_cloudflare_agent_script_gives_up_after_repeated_failures(tmp_path):
    attempts = []

    def fail(request):
        attempts.append(request)
        return 503

    out, code, _repo, _home, seen = _run_cloudflare_agent(
        tmp_path, lambda turn, messages: [], fail=fail)

    assert code == 1
    assert not seen
    assert "!! Cloudflare AI agent error 503" in out
    assert len(attempts) == 5


def test_cloudflare_agent_script_sees_through_a_requoted_repeat(tmp_path):
    # The same run twice over: llama appended the line, then "re-ran" it with
    # the other quote style, which the raw string comparison did not recognize
    # as a repeat — so the line landed twice (adhoc #1633).
    commands = ["echo 'hello' >> README.md",
                'echo "hello" >> README.md',
                "git add -A && git commit -qm hello"]

    def turns(turn, _messages):
        if turn <= len(commands):
            return [{"id": "c%d" % turn, "name": "bash",
                     "arguments": {"command": commands[turn - 1]}}]
        return []

    out, code, repo, _home, _seen = _run_cloudflare_agent(tmp_path, turns)

    assert code == 0
    assert "Not run again: this is the command from turn 1" in out
    assert (repo / "README.md").read_text(encoding="utf-8") == \
        "# Title\nhello\n"


def test_cloudflare_agent_script_will_not_call_uncommitted_work_finished(
        tmp_path):
    # Stopping after an edit and before the commit is this provider's most
    # common way to "finish": say what is missing instead of declaring success.
    def turns(turn, _messages):
        if turn == 1:
            return [{"id": "c1", "name": "bash",
                     "arguments": {"command": 'echo "hello" >> README.md'}}]
        if turn == 3:
            return [{"id": "c3", "name": "bash",
                     "arguments": {"command": "git add -A && git commit -qm "
                                              "hello"}}]
        return []

    out, code, repo, _home, seen = _run_cloudflare_agent(tmp_path, turns)

    assert code == 0
    assert "==> Not finished: the worktree has uncommitted changes." in out
    # The model was told what was left, in the conversation and not just the log.
    nudge = seen[2][-1]
    assert nudge["role"] == "user"
    assert "uncommitted changes" in nudge["content"]
    assert "M  README.md" in nudge["content"] or \
        " M README.md" in nudge["content"]
    assert "hello" in subprocess.run(
        ["git", "log", "-1", "--pretty=%s"], cwd=repo, capture_output=True,
        text=True).stdout
    assert "==> Agent finished." in out


def test_cloudflare_agent_script_echoes_tool_calls_in_openai_shape(tmp_path):
    command = ('grep -qF hello README.md || echo "hello" >> README.md && '
               "git add -A && git commit -qm hello")

    def turns(turn, _messages):
        if turn == 1:
            return [{"id": "c1", "name": "bash",
                     "arguments": {"command": command}}]
        return []

    _out, code, _repo, _home, seen = _run_cloudflare_agent(tmp_path, turns)

    assert code == 0
    assistant = seen[1][2]
    assert assistant["role"] == "assistant"
    call = assistant["tool_calls"][0]
    assert call["id"] == "c1"
    assert call["function"]["name"] == "bash"
    assert json.loads(call["function"]["arguments"]) == {"command": command}
    tool = seen[1][3]
    assert tool["role"] == "tool"
    assert tool["tool_call_id"] == "c1"
    assert tool["name"] == "bash"
    assert tool["content"].startswith("(exit code 0)")


def test_cloudflare_agent_script_asks_again_after_a_turn_that_says_nothing(
        tmp_path):
    # A turn with neither a tool call nor an answer used to print "Agent
    # finished" over a run that had done nothing at all.
    out, code, _repo, _home, seen = _run_cloudflare_agent(
        tmp_path, lambda turn, messages: [],
        reply=lambda turn, calls: "" if turn == 1 else "Done.")

    assert code == 0
    assert "==> Empty turn (no tool call, no answer)." in out
    assert seen[1][-1]["content"].startswith(
        "You sent neither a tool call nor an answer")
    # "Done." from a run that never ran a tool is questioned once more before
    # it counts as finished.
    assert "==> No tool used yet." in out
    assert len(seen) == 3
    assert "==> Agent finished." in out


def test_agent_runner_does_not_call_a_user_stop_a_crash():
    # Stopping a session terminates the child, so Qt reports Crashed for a run
    # the user ended on purpose — "!! Process crashed" beside "Stopped." read as
    # a bug in the agent (adhoc #1622).
    runner = (ROOT.parent / "desktop" / "src" /
              "AgentRunner.cpp").read_text(encoding="utf-8")
    assert "if (!m_stopping)\n                    emitLog(QStringLiteral(\"!! \") + " \
        "m_process->errorString());" in runner


def test_web_composers_offer_the_cloudflare_model_picker():
    # Public chat composer: options come from the relay (so deploying a new
    # model needs no site rebuild) and the pick persists per browser.
    assert 'const FORKBOT_MODELS_ENDPOINT = "/api/forkbot/models";' in CHAT_TEXT
    assert 'const FORKBOT_MODEL_KEY = "forkmesh.forkbot.model";' in CHAT_TEXT
    assert "async function loadForkbotModels()" in CHAT_TEXT
    assert "model: forkbotModel()," in CHAT_TEXT
    assert "forkbotModelSelect?.value" in CHAT_TEXT
    assert 'id="chat-forkbot-model"' in CHAT_HTML_TEXT
    assert ".chat-forkbot-model {" in CHAT_CSS_TEXT
    # Dashboard chat shares the same stored pick.
    assert 'const FORKBOT_MODEL_KEY = "forkmesh.forkbot.model";' in \
        DASHBOARD_CHAT_TEXT
    assert "model: forkbotModel()," in DASHBOARD_CHAT_TEXT
    assert "data-home-agent-model" not in DASHBOARD_HOME_TEXT
    assert "loadHomeForkbotModels" not in DASHBOARD_TEXT


def test_web_chats_forward_mentions_and_broadcast_bot_replies():
    for script in (CHAT_TEXT, DASHBOARD_CHAT_TEXT):
        assert 'const FORKBOT_ENDPOINT = "/api/forkbot/chat";' in script
        assert "FORKBOT_MENTION_RE" in script
        assert "async function maybeAskForkbot(text)" in script
        assert "fetch(FORKBOT_ENDPOINT" in script
        assert "FORKBOT_SENDER_ID" in script
        assert (
            'sender: "forkbot"' in script and
            "bot: FORKBOT_SENDER_ID" in script and
            "broadcastForkbotMessage(data.botMessage)" in script
        ) or (
            "sender = FORKBOT_SENDER_ID" in script and
            "bot: botName" in script and
            "broadcastBotMessage(data.botMessage)" in script
        )
        assert "senderId: selfId," in script
        assert "function botLine(" in script
        assert script.count("maybeAskForkbot(") >= 2
        # Recent conversation is buffered and forwarded so ForkBot has context.
        assert "rememberContext(" in script
        assert "context" in script


def test_forkbot_regex_catches_noun_first_issue_commands():
    # "@forkbot issue to add more features" was answered with the help hint —
    # the fast path only knew verb-first phrasings.
    ns = _load_forkbot()
    parse = ns["_forkbot_parse_issue_command"]
    assert parse("issue to add more features to FOrkbot") == {
        "description": "add more features to FOrkbot"}
    assert parse("bug: the mirror panel flickers") == {
        "description": "the mirror panel flickers"}
    assert parse("ticket about slow release downloads") == {
        "description": "slow release downloads"}
    assert parse("how do issues work?") is None


def test_forkbot_files_the_raw_request_when_ai_is_unreachable():
    # env.AI missing/erroring must degrade to filing the plainly-issue-shaped
    # request, not refuse with the help hint (the reported bug: the AI binding
    # failed silently and every natural request got "I can open issues...").
    env, calls, ns = _env_and_calls(ai=None)
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": ("forkbot you should be able to get issue context and "
                    "create issues - create one for that"),
        "sender": "jett",
    })))

    assert response["status"] == 201
    assert calls["inserted"]
    body = calls["inserted"][0][1]["event"]["body"]
    assert "create issues" in body


def test_forkbot_ai_confident_none_still_returns_help():
    class _AI:
        async def run(self, model, payload):
            return {"response": json.dumps({"intent": "none"})}

    env, calls, ns = _env_and_calls(ai=_AI())
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot create some good vibes in here",
    })))
    assert response["status"] == 200
    assert response["data"]["action"] == "help"
    assert calls["inserted"] == []


def test_forkbot_requests_json_mode_then_falls_back_without_it():
    # First attempt carries response_format (Workers AI JSON mode); a model or
    # plan that rejects it gets a plain prompt-only retry.
    class _AI:
        def __init__(self):
            self.payloads = []

        async def run(self, model, payload):
            self.payloads.append(payload)
            if "response_format" in payload:
                raise RuntimeError("response_format not supported")
            return {"response": json.dumps({
                "intent": "create_issue",
                "title": "Login timeouts",
                "body": "The login page keeps timing out.",
            })}

    ai = _AI()
    env, calls, ns = _env_and_calls(ai=ai, catalog_issue_max=0)
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot the login page keeps timing out, sort it out",
    })))

    assert response["status"] == 201
    assert len(ai.payloads) == 2
    assert "response_format" in ai.payloads[0]
    assert ai.payloads[0]["response_format"]["type"] == "json_schema"
    assert "response_format" not in ai.payloads[1]
    assert calls["inserted"][0][1]["titleIfNew"] == "Login timeouts"


def test_forkbot_handles_json_mode_object_responses():
    # With JSON mode active the binding returns the parsed object under
    # "response" instead of a string.
    class _AI:
        async def run(self, model, payload):
            return {"response": {
                "intent": "create_issue",
                "title": "Add dark mode",
                "body": "Users asked for a dark theme.",
            }}

    env, calls, ns = _env_and_calls(ai=_AI(), catalog_issue_max=0)
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot folks keep asking for a dark theme, get it tracked",
    })))
    assert response["status"] == 201
    assert calls["inserted"][0][1]["titleIfNew"] == "Add dark mode"


def test_forkbot_command_hint_matches_work_recording_language():
    ns = _load_forkbot()
    hints = ns["_forkbot_command_hints_issue"]
    assert hints("create one for that")
    assert hints("you should track this bug")
    assert hints("issue to do the thing")
    assert not hints("good morning everyone")
    assert not hints("what's the weather like")


def test_forkbot_parses_list_commands():
    ns = _load_forkbot()
    parse = ns["_forkbot_parse_list_command"]
    assert parse("list issues") == {"count": 5}
    assert parse("list the last few issues") == {"count": 5}
    assert parse("show me the latest 10 issues") == {"count": 10}
    assert parse("please list the last 3 issues") == {"count": 3}
    assert parse("what are the recent issues") == {"count": 5}
    # Count is capped so one chat line can't request a huge blob fan-out.
    assert parse("list the last 500 issues") == {"count": ns["FORKBOT_LIST_MAX"]}
    # Not list requests.
    assert parse("create an issue to fix retries") is None
    assert parse("issues are piling up") is None


def test_forkbot_parses_count_commands():
    ns = _load_forkbot()
    parse = ns["_forkbot_parse_count_command"]
    assert parse("how many issues are there?") == {}
    assert parse("how many issues are there") == {}
    assert parse("how many issues exist") == {}
    assert parse("how many issues do we have") == {}
    assert parse("how many issues") == {}
    assert parse("what is the number of issues") == {}
    assert parse("issue count?") == {}
    assert parse("please how many issues are there") == {}
    # Not count requests.
    assert parse("how many issues are there about relay retries") is None
    assert parse("list the last 5 issues") is None
    assert parse("create an issue to fix retries") is None


def test_forkbot_counts_issues_from_live_mirror():
    host = _FakeGateway(tree=_issue_tree([1, 2, 3, 4, 5, 6, 7, 8]))
    env, calls, ns = _env_and_calls(host=host)
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot how many issues are there?", "sender": "jett",
    })))
    assert response["status"] == 200
    assert response["data"]["action"] == "issues_counted"
    assert response["data"]["count"] == 8
    assert response["data"]["botMessage"] == (
        "There are 8 issues in forkmesh/forkmesh.")
    assert calls["inserted"] == []
    # Only a tree listing was needed — no per-issue blob reads for a count.
    assert any("/tree?" in url for url in host.urls)
    assert not any("/blobs?" in url for url in host.urls)


def test_forkbot_count_reports_zero_issues_and_offline_host():
    host = _FakeGateway(tree=_issue_tree([]))
    env, _calls, ns = _env_and_calls(host=host)
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot how many issues are there?",
    })))
    assert response["data"]["action"] == "issues_counted"
    assert response["data"]["count"] == 0
    assert response["data"]["botMessage"] == (
        "No issues have been filed in forkmesh/forkmesh yet.")

    env, _calls, ns = _env_and_calls()  # gateway offline
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot how many issues are there?",
    })))
    assert response["data"]["action"] == "issues_unavailable"
    assert "live host" in response["data"]["botMessage"]


def test_forkbot_parses_search_commands():
    ns = _load_forkbot()
    parse = ns["_forkbot_parse_search_command"]
    assert parse("search issues for relay retries") == {
        "query": "relay retries"}
    assert parse("find issues about dark mode") == {"query": "dark mode"}
    assert parse("look for issues mentioning flaky login") == {
        "query": "flaky login"}
    assert parse("search the issues: crash logs") == {"query": "crash logs"}
    # No "issue" keyword -> stays with the create/AI paths.
    assert parse("search for happiness") is None
    assert parse("find the bug in the relay") is None


def test_forkbot_parses_start_agent_commands():
    ns = _load_forkbot()
    parse = ns["_forkbot_parse_agent_command"]
    assert parse("start an agent on the most recent issue") == {
        "issueNumber": 0}
    assert parse("start a coding agent on the latest issue") == {
        "issueNumber": 0}
    assert parse("please run an agent on issue #12") == {"issueNumber": 12}
    assert parse("assign an agent to issue 7") == {"issueNumber": 7}
    assert parse("kick off an agent on the newest issue") == {
        "issueNumber": 0}
    assert parse("start the meeting") is None
    assert parse("the agent crashed") is None


def test_forkbot_parses_show_and_help_commands():
    ns = _load_forkbot()
    show = ns["_forkbot_parse_show_command"]
    assert show("show issue #12") == {"issueNumber": 12}
    assert show("show me the latest issue") == {"issueNumber": 0}
    assert show("what's the status of issue 4") == {"issueNumber": 4}
    # "open issue ..." must stay with the create parser ("open" = filing verb).
    assert show("open issue 4") is None
    assert show("show me the latest issues") is None  # plural -> list command
    helping = ns["_forkbot_parse_help_command"]
    assert helping("help")
    assert helping("what can you do?")
    assert not helping("help me fix the relay")  # falls through to intents
    assert not helping("create an issue to add help docs")


def test_forkbot_lists_recent_issues_from_live_mirror():
    host = _FakeGateway(
        tree=_issue_tree([1, 2, 3, 4, 5, 6, 7, 8]),
        blobs=_issue_blobs([
            {"number": n, "title": "Issue %d" % n, "status": "open",
             "authorName": "jett"} for n in (8, 7, 6, 5, 4)]),
    )
    env, calls, ns = _env_and_calls(host=host)
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot list the last few issues", "sender": "alice",
    })))

    assert response["status"] == 200
    assert response["data"]["action"] == "issues_listed"
    assert [i["number"] for i in response["data"]["issues"]] == [8, 7, 6, 5, 4]
    message = response["data"]["botMessage"]
    assert "#8 Issue 8 (open, by jett)" in message
    assert "#3" not in message
    # More issues exist than were shown -> the reply teaches "ask for more".
    assert "list the last N issues" in message
    assert "/forkmesh/forkmesh/issues" in message
    assert calls["inserted"] == []
    # One tree listing + one batched blob read, over the internal tunnel.
    assert any("/tree?" in url for url in host.urls)
    assert any("/blobs?" in url for url in host.urls)


def test_forkbot_lists_issues_from_split_open_closed_folders():
    # Post-split mirrors (adhoc #14) file issues under .forkmesh/issues/open/
    # and .forkmesh/issues/closed/; the root tree lists the status folders plus
    # any pre-split leftover numbered dir. All three locations must be found
    # and each record read from wherever it lives.
    def _blob(path, record):
        return (path, {"ok": True, "encoding": "utf8",
                       "content": json.dumps(record)})

    host = _FakeGateway(
        trees={
            ".forkmesh/issues": {"ok": True, "entries": [
                {"type": "tree", "name": "open"},
                {"type": "tree", "name": "closed"},
                {"type": "tree", "name": "2"},
            ]},
            ".forkmesh/issues/open": _issue_tree([3]),
            ".forkmesh/issues/closed": _issue_tree([1]),
        },
        blobs={"ok": True, "blobs": dict([
            _blob(".forkmesh/issues/open/3/issue-3.json",
                  {"number": 3, "title": "Split open", "status": "open",
                   "authorName": "jett"}),
            _blob(".forkmesh/issues/closed/1/issue-1.json",
                  {"number": 1, "title": "Split closed", "status": "closed",
                   "authorName": "jett"}),
            _blob(".forkmesh/issues/2/issue-2.json",
                  {"number": 2, "title": "Legacy spot", "status": "open",
                   "authorName": "jett"}),
        ])},
    )
    env, _calls, ns = _env_and_calls(host=host)
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot list the last 3 issues",
    })))
    assert response["data"]["action"] == "issues_listed"
    assert [i["number"] for i in response["data"]["issues"]] == [3, 2, 1]
    message = response["data"]["botMessage"]
    assert "#3 Split open (open, by jett)" in message
    assert "#2 Legacy spot (open, by jett)" in message
    assert "#1 Split closed (closed, by jett)" in message


def test_forkbot_loads_every_raw_tracker_record_in_bounded_batches():
    records = [{"number": number, "author": "reporter"}
               for number in range(1, 62)]
    blobs = {
        ".forkmesh/issues/%d/issue-%d.json" % (record["number"], record["number"]): {
            "ok": True,
            "encoding": "utf8",
            "content": json.dumps(record),
        }
        for record in records
    }
    host = _FakeGateway(
        tree=_issue_tree(range(1, 62)),
        blobs={"ok": True, "blobs": blobs},
    )
    env, _calls, ns = _env_and_calls(host=host)

    loaded = asyncio.run(ns["_forkbot_all_issue_records"](
        env, "forkmesh", "forkmesh"))

    assert loaded == records
    blob_urls = [url for url in host.urls if "/blobs?" in url]
    assert len(blob_urls) == 2
    assert "issue-60.json" in blob_urls[0]
    assert all(url.count("path=") <= ns["MAX_BLOB_BATCH"]
               for url in blob_urls)


def test_forkbot_list_honors_requested_count():
    host = _FakeGateway(
        tree=_issue_tree([10, 11, 12]),
        blobs=_issue_blobs([
            {"number": n, "title": "T%d" % n, "status": "open"}
            for n in (12, 11)]),
    )
    env, _calls, ns = _env_and_calls(host=host)
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot show me the latest 2 issues",
    })))
    assert response["data"]["action"] == "issues_listed"
    assert [i["number"] for i in response["data"]["issues"]] == [12, 11]
    blob_urls = [url for url in host.urls if "/blobs?" in url]
    assert len(blob_urls) == 1 and "issue-10.json" not in blob_urls[0]


def test_forkbot_list_reports_offline_host_instead_of_empty_repo():
    env, calls, ns = _env_and_calls()  # gateway offline
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot list the last 5 issues",
    })))
    assert response["data"]["action"] == "issues_unavailable"
    assert "live host" in response["data"]["botMessage"]
    assert calls["inserted"] == []


def test_forkbot_searches_issues_via_host_grep():
    host = _FakeGateway(search={"ok": True, "issues": [
        {"number": 42, "title": "Relay retries drop frames",
         "snippet": "the relay retries forever"},
        {"number": 7, "title": "Retry backoff", "snippet": ""},
    ], "pulls": [], "code": []})
    env, _calls, ns = _env_and_calls(host=host)
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot search issues for relay retries",
    })))

    assert response["data"]["action"] == "issues_searched"
    assert response["data"]["query"] == "relay retries"
    message = response["data"]["botMessage"]
    assert "#42 Relay retries drop frames" in message
    assert "the relay retries forever" in message
    assert "#7 Retry backoff" in message
    assert any("/search?q=relay%20retries" in url for url in host.urls)


def test_forkbot_search_reports_no_matches():
    host = _FakeGateway(search={"ok": True, "issues": [], "pulls": [],
                             "code": []})
    env, _calls, ns = _env_and_calls(host=host)
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot find issues about jetpacks",
    })))
    assert response["data"]["action"] == "issues_searched"
    assert response["data"]["issues"] == []
    assert "No issues matching" in response["data"]["botMessage"]


def test_forkbot_shows_one_issue():
    host = _FakeGateway(
        tree=_issue_tree([3, 9]),
        blobs=_issue_blobs([{
            "number": 9, "title": "Clone hangs", "status": "open",
            "authorName": "bob",
            "events": [{"type": "open", "authorName": "bob",
                        "body": "Cloning stalls at 90%."}],
        }]),
    )
    env, _calls, ns = _env_and_calls(host=host)
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot show me the latest issue",
    })))
    assert response["data"]["action"] == "issue_shown"
    assert response["data"]["issue"]["number"] == 9
    message = response["data"]["botMessage"]
    assert "#9 Clone hangs (open, opened by bob)" in message
    assert "Cloning stalls at 90%." in message


def test_forkbot_starts_agent_on_most_recent_issue_for_owner():
    host = _FakeGateway(
        tree=_issue_tree([4, 5, 6]),
        blobs=_issue_blobs([{"number": 6, "title": "Fix relay retries",
                             "status": "open"}]),
    )
    env, calls, ns = _env_and_calls(host=host, session_account="forkmesh")
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot start an agent on the most recent issue",
        "sender": "forkmesh",  # the default repo's owner account
    })))

    assert response["status"] == 201
    assert response["data"]["action"] == "agent_requested"
    assert response["data"]["issueNumber"] == 6
    assert "Fix relay retries" in response["data"]["botMessage"]
    assert len(calls["inserted"]) == 1
    _repo_bi, item, submitter_bi = calls["inserted"][0]
    assert submitter_bi == "bi:forkbot"
    # An agent request rides the inbox as a ForkBot comment on the EXISTING
    # issue whose meta carries wantsAgent — the desktop starts the agent when
    # it merges the inbox.
    assert item["number"] == 6
    assert item["event"]["type"] == "comment"
    assert item["meta"]["wantsAgent"] is True
    assert item["requestedBy"] == "forkmesh"
    assert "forkmesh" in item["event"]["body"]


def test_forkbot_starts_agent_on_named_issue_for_admin():
    host = _FakeGateway(
        tree=_issue_tree([4, 5, 6]),
        blobs=_issue_blobs([{"number": 4, "title": "Old bug",
                             "status": "open"}]),
    )
    env, calls, ns = _env_and_calls(
        host=host, admins=("jett",), session_account="jett")
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot start an agent on issue #4",
        "sender": "jett",
    })))
    assert response["status"] == 201
    assert response["data"]["issueNumber"] == 4
    assert calls["inserted"][0][1]["number"] == 4


def test_forkbot_denies_agent_start_for_non_owner():
    host = _FakeGateway(tree=_issue_tree([1, 2]))
    env, calls, ns = _env_and_calls(host=host)
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot start an agent on the latest issue",
        "sender": "mallory",
    })))
    assert response["status"] == 200
    assert response["data"]["action"] == "agent_denied"
    assert calls["inserted"] == []
    # An anonymous mention (no sender) must not inherit owner privileges.
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot start an agent on the latest issue",
    })))
    assert response["data"]["action"] == "agent_denied"
    assert calls["inserted"] == []


def test_forkbot_rejects_agent_start_on_unknown_issue():
    host = _FakeGateway(tree=_issue_tree([1, 2, 3]))
    env, calls, ns = _env_and_calls(host=host, session_account="forkmesh")
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot start an agent on issue #99",
        "sender": "forkmesh",
    })))
    assert response["data"]["action"] == "agent_denied"
    assert "#99" in response["data"]["botMessage"]
    assert calls["inserted"] == []


def test_forkbot_ai_intent_routes_list_search_and_agent():
    # Natural phrasing that no offline regex catches: the AI classifier picks
    # the intent and the handler runs the matching action.
    class _AI:
        def __init__(self, intent):
            self._intent = intent

        async def run(self, model, payload):
            return {"response": json.dumps(self._intent)}

    host = _FakeGateway(
        tree=_issue_tree([1, 2, 3]),
        blobs=_issue_blobs([
            {"number": n, "title": "T%d" % n, "status": "open"}
            for n in (3, 2)]),
        search={"ok": True, "issues": [
            {"number": 2, "title": "T2", "snippet": "relay"}]},
    )
    env, _calls, ns = _env_and_calls(
        ai=_AI({"intent": "list_issues", "count": 2}), host=host)
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot what came in recently?",
    })))
    assert response["data"]["action"] == "issues_listed"
    assert [i["number"] for i in response["data"]["issues"]] == [3, 2]

    env, _calls, ns = _env_and_calls(
        ai=_AI({"intent": "count_issues"}), host=host)
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot so how many total issues do we have logged?",
    })))
    assert response["data"]["action"] == "issues_counted"
    assert response["data"]["count"] == 3

    env, _calls, ns = _env_and_calls(
        ai=_AI({"intent": "search_issues", "query": "relay"}), host=host)
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot anything on file about the relay?",
    })))
    assert response["data"]["action"] == "issues_searched"
    assert response["data"]["issues"][0]["number"] == 2

    env, calls, ns = _env_and_calls(
        ai=_AI({"intent": "start_agent", "issueNumber": 0}), host=host,
        session_account="forkmesh")
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot get someone working on that new one",
        "sender": "forkmesh",
    })))
    assert response["data"]["action"] == "agent_requested"
    assert response["data"]["issueNumber"] == 3


def test_forkbot_help_lists_every_capability():
    env, _calls, ns = _env_and_calls()
    response = asyncio.run(ns["forkbot_chat_handler"](env, _Request({
        "message": "forkbot help",
    })))
    assert response["data"]["action"] == "help"
    message = response["data"]["botMessage"]
    for capability in ("open an issue", "list recent issues",
                       "how many issues there are", "search issues",
                       "start a coding agent"):
        assert capability in message


def test_forkbot_desktop_honors_agent_request_comments():
    # The desktop side of "forkbot start an agent on issue #N": the inbox
    # merge collects comment events whose meta carries wantsAgent and starts
    # an agent on the existing issue, with the same double-start guard as
    # open-event requests.
    issues_cpp = (ROOT.parent / "desktop" / "src" /
                  "MainWindowIssues.cpp").read_text(encoding="utf-8")
    assert "CommentAgentRequest" in issues_cpp
    assert re.search(r"meta\.wantsAgent && number > 0", issues_cpp)
    assert "issueHasAgent" in issues_cpp
