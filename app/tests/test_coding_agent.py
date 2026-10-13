"""Unit coverage for the embedded provider-neutral coding agent."""

from __future__ import annotations

import asyncio
import contextlib
import importlib.util
import json
import os
import re
import shlex
import socket
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "tools" / "forkmesh_coding_agent.py"
HEADER = ROOT / "desktop" / "src" / "CloudflareAgentScript.h"
CLAUDE_HEADER = ROOT / "desktop" / "src" / "ClaudeAgentScript.h"
SYNC = ROOT / "tools" / "sync_coding_agent_header.py"

spec = importlib.util.spec_from_file_location("forkmesh_coding_agent", SOURCE)
agent = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = agent
spec.loader.exec_module(agent)


def test_embedded_agent_is_generated_from_testable_source():
    sync_spec = importlib.util.spec_from_file_location("sync_coding_agent", SYNC)
    sync = importlib.util.module_from_spec(sync_spec)
    sync_spec.loader.exec_module(sync)
    assert HEADER.read_text(encoding="utf-8") == sync.render(
        SOURCE.read_text(encoding="utf-8")
    )


def test_tool_call_display_parser_accepts_only_json_objects():
    assert agent._json_object('{"name":"search_files"}') == {
        "name": "search_files"}
    assert agent._json_object("[") == {}
    assert agent._json_object("[]") == {}


def test_embedded_claude_agent_isolates_tool_credentials(monkeypatch):
    header = CLAUDE_HEADER.read_text(encoding="utf-8")
    match = re.search(r'R"PYAGENT\((.*)\)PYAGENT"', header, re.DOTALL)
    assert match
    source = match.group(1)
    assert "shell=True" not in source
    monkeypatch.setenv("ANTHROPIC_API_KEY", "provider-secret")
    monkeypatch.setenv("SSH_AUTH_SOCK", "/private/agent.sock")
    monkeypatch.setenv("FORKMESH_TEST_VISIBLE", "preserved")
    namespace = {"__name__": "embedded_claude_agent_test"}
    exec(compile(source, str(CLAUDE_HEADER), "exec"), namespace)
    script = (
        "import json,os; print(json.dumps({name: os.environ.get(name) "
        "for name in ('ANTHROPIC_API_KEY','SSH_AUTH_SOCK',"
        "'FORKMESH_TEST_VISIBLE','HOME')}))"
    )
    result = namespace["run_bash"](
        "%s -c %s" % (shlex.quote(sys.executable), shlex.quote(script)))
    data = json.loads(result.splitlines()[1])
    assert data["ANTHROPIC_API_KEY"] is None
    assert data["SSH_AUTH_SOCK"] is None
    assert data["FORKMESH_TEST_VISIBLE"] == "preserved"
    assert data["HOME"] == namespace["COMMAND_HOME"].name
    namespace["COMMAND_HOME"].cleanup()


def test_configuration_loads_every_provider_option(monkeypatch):
    values = {
        "FORKMESH_AGENT_PROVIDER": "local-llama",
        "FORKMESH_AGENT_MODEL": "installed-model",
        "FORKMESH_AGENT_ENDPOINT": "http://local.invalid/v1/chat/completions",
        "FORKMESH_AGENT_API_TOKEN": "secret",
        "FORKMESH_AGENT_TEMPERATURE": "0.7",
        "FORKMESH_AGENT_MAX_OUTPUT_TOKENS": "1234",
        "FORKMESH_AGENT_TIMEOUT": "45",
        "FORKMESH_AGENT_STREAMING": "false",
        "FORKMESH_AGENT_RETRY_COUNT": "7",
    }
    for name, value in values.items():
        monkeypatch.setenv(name, value)
    config = agent.AgentConfig.from_env()
    assert dataclass_values(config) == {
        "provider": "local-llama",
        "model": "installed-model",
        "endpoint": "http://local.invalid/v1/chat/completions",
        "api_token": "secret",
        "temperature": 0.7,
        "max_tokens": 1234,
        "timeout": 45.0,
        "streaming": False,
        "retry_count": 7,
    }


def test_local_configuration_does_not_inherit_cloudflare_credentials(monkeypatch):
    monkeypatch.setenv("FORKMESH_AGENT_PROVIDER", "local-llama")
    monkeypatch.delenv("FORKMESH_AGENT_API_TOKEN", raising=False)
    monkeypatch.delenv("FORKMESH_AGENT_ACCOUNT_ID", raising=False)
    monkeypatch.setenv("CLOUDFLARE_API_TOKEN", "must-not-leak")
    monkeypatch.setenv("CLOUDFLARE_ACCOUNT_ID", "must-not-leak")
    config = agent.AgentConfig.from_env()
    assert config.api_token == ""
    assert config.account_id == ""


def dataclass_values(config):
    names = (
        "provider",
        "model",
        "endpoint",
        "api_token",
        "temperature",
        "max_tokens",
        "timeout",
        "streaming",
        "retry_count",
    )
    return {name: getattr(config, name) for name in names}


def test_cloudflare_provider_derives_endpoint_without_hardcoded_model():
    config = agent.AgentConfig(
        provider="cloudflare", account_id="account / one", api_token="token"
    )
    provider = agent.CloudflareProvider(config)
    assert provider.quiet_tool_turns is True
    assert provider.config.model == ""
    assert provider.config.endpoint.endswith(
        "/accounts/account%20%2F%20one/ai/v1/chat/completions"
    )
    assert provider._headers()["authorization"] == "Bearer token"
    provider.config.model = "runtime-model"
    wire = provider._payload(
        [
            {"role": "system", "content": "system"},
            {"role": "developer", "content": "repository context"},
            {"role": "user", "content": "task"},
        ],
        [],
        False,
    )["messages"]
    assert wire == [
        {"role": "system", "content": "system\n\nrepository context"},
        {"role": "user", "content": "task"},
    ]


def test_cloudflare_prompt_and_turns_hide_working_text(tmp_path):
    class QuietProvider(_ScriptedProvider):
        quiet_tool_turns = True

    provider = QuietProvider([
        agent.ChatResponse(
            content="I will inspect the file first.",
            tool_calls=[agent.ToolCall(
                "call-1", "write_file", {"path": "done.txt", "content": "done"})]),
        agent.ChatResponse(
            content="<think>private scratch work</think>Updated done.txt."),
    ])
    runner, _session, events = _scripted_agent(tmp_path, provider)

    assert "tool turns silent" in agent.DEFAULT_SYSTEM_PROMPT
    assert asyncio.run(runner.run("write done.txt")) == \
        "Updated done.txt."
    assert (tmp_path / "done.txt").read_text(encoding="utf-8") == "done"
    assert [event.text for event in events if event.type == "text"] == [
        "Updated done.txt."]


def test_cloudflare_provider_uses_direct_api_when_a_legacy_ticket_is_present(
        monkeypatch):
    monkeypatch.setenv("FORKMESH_AI_AGENT_AUTH", '{"url":"https://relay.invalid"}')
    provider = agent._provider_from_config(agent.AgentConfig(
        provider="cloudflare-ai", account_id="account", api_token="token"))
    assert isinstance(provider, agent.CloudflareProvider)


def test_openai_provider_normalizes_tools_and_retries():
    attempts = []

    async def transport(url, headers, body, timeout, stream, cancellation):
        attempts.append(json.loads(body))
        if len(attempts) == 1:
            raise agent.ProviderError("busy", status=503, retryable=True)
        return json.dumps(
            {
                "model": "chosen",
                "choices": [
                    {
                        "message": {
                            "content": "",
                            "tool_calls": [
                                {
                                    "id": "call-1",
                                    "function": {
                                        "name": "read_file",
                                        "arguments": '{"path":"README.md"}',
                                    },
                                }
                            ],
                        }
                    }
                ],
                "usage": {"prompt_tokens": 4, "completion_tokens": 3},
            }
        ).encode()

    config = agent.AgentConfig(
        provider="compatible",
        model="runtime-model",
        endpoint="https://example.invalid/v1/chat/completions",
        streaming=False,
        retry_count=1,
        retry_base_delay=0,
    )
    provider = agent.OpenAICompatibleProvider(config, transport=transport)
    response = asyncio.run(provider.complete([{"role": "user", "content": "x"}], []))
    assert len(attempts) == 2
    assert attempts[0]["model"] == "runtime-model"
    assert response.tool_calls == [
        agent.ToolCall("call-1", "read_file", {"path": "README.md"})
    ]
    assert (response.input_tokens, response.output_tokens, response.model) == (
        4,
        3,
        "chosen",
    )


class _SseResponse:
    def __init__(self, lines):
        self.lines = lines

    def __iter__(self):
        return iter(self.lines)

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return None

    def close(self):
        return None


def test_streaming_assembles_text_usage_and_fragmented_tool_calls():
    chunks = [
        {"model": "m", "choices": [{"delta": {"content": "Hi "}}]},
        {
            "choices": [
                {
                    "delta": {
                        "tool_calls": [
                            {
                                "index": 0,
                                "id": "c1",
                                "function": {
                                    "name": "read_file",
                                    "arguments": '{"path":',
                                },
                            }
                        ]
                    }
                }
            ]
        },
        {
            "choices": [
                {
                    "delta": {
                        "content": "there",
                        "tool_calls": [
                            {
                                "index": 0,
                                "function": {"arguments": '"README.md"}'},
                            }
                        ],
                    }
                }
            ]
        },
        {"choices": [], "usage": {"prompt_tokens": 8, "completion_tokens": 5}},
    ]
    response = _SseResponse(
        [("data: " + json.dumps(chunk) + "\n").encode() for chunk in chunks]
        + [b"data: [DONE]\n"]
    )

    async def transport(*_args):
        return response

    provider = agent.OpenAICompatibleProvider(
        agent.AgentConfig(provider="x", model="m", endpoint="https://invalid", streaming=True),
        transport=transport,
    )

    async def collect():
        return [event async for event in provider.stream([], [])]

    events = asyncio.run(collect())
    final = events[-1].response
    assert "".join(event.text for event in events if event.type == "text") == "Hi there"
    assert final.tool_calls[0].arguments == {"path": "README.md"}
    assert (final.input_tokens, final.output_tokens) == (8, 5)


def test_cancellation_stops_provider_before_transport():
    called = False

    async def transport(*_args):
        nonlocal called
        called = True

    token = agent.CancellationToken()
    token.cancel()
    provider = agent.OpenAICompatibleProvider(
        agent.AgentConfig(provider="x", model="m", endpoint="https://invalid"),
        transport=transport,
    )
    with pytest.raises(asyncio.CancelledError):
        asyncio.run(provider.complete([], [], token))
    assert called is False


def test_conversation_prompt_memory_and_session_round_trip(tmp_path):
    conversation = agent.Conversation()
    conversation.append("user", "hello")
    memory = agent.Memory(
        token_budget=1000,
        repository_context="clean main",
        project_summary="small project",
        recently_edited_files=["a.py"],
    )
    messages = agent.PromptBuilder("system", "developer").build(conversation, memory)
    assert [message["role"] for message in messages] == [
        "system",
        "developer",
        "developer",
        "user",
    ]
    session = agent.Session(conversation=conversation, memory=memory)
    path = tmp_path / "session.json"
    session.save(path)
    restored = agent.Session.load(path)
    assert restored.id == session.id
    assert restored.conversation.messages == conversation.messages
    assert restored.memory.recently_edited_files == ["a.py"]


def test_memory_automatically_summarizes_long_history():
    conversation = agent.Conversation(
        [{"role": "user", "content": "x" * 900} for _ in range(10)]
    )
    memory = agent.Memory(token_budget=1000)
    assert memory.compact(conversation) is True
    assert memory.summaries
    assert any(
        message["role"] == "developer"
        and message["content"].startswith("Earlier session summary")
        for message in conversation.messages
    )


def test_file_editor_preview_multifile_patch_and_undo(tmp_path):
    memory = agent.Memory()
    editor = agent.FileEditor(tmp_path, memory)
    (tmp_path / "a.txt").write_text("old\n", encoding="utf-8")
    first = editor.preview_write("a.txt", "new\n")
    second = editor.preview_write("b.txt", "created\n")
    assert "-old" in first.diff and "+new" in first.diff
    editor.apply([first, second])
    assert (tmp_path / "a.txt").read_text() == "new\n"
    assert memory.recently_edited_files == ["a.txt", "b.txt"]
    editor.undo()
    assert (tmp_path / "a.txt").read_text() == "old\n"
    assert not (tmp_path / "b.txt").exists()

    init_git(tmp_path)
    patch = """diff --git a/a.txt b/a.txt
--- a/a.txt
+++ b/a.txt
@@ -1 +1 @@
-old
+patched
"""
    assert editor.apply_patch(patch, preview=True) == patch
    editor.apply_patch(patch)
    assert (tmp_path / "a.txt").read_text() == "patched\n"
    editor.undo()
    assert (tmp_path / "a.txt").read_text() == "old\n"


def init_git(path):
    subprocess.run(["git", "init", "-q", str(path)], check=True)


def test_file_editor_rejects_path_escape(tmp_path):
    editor = agent.FileEditor(tmp_path)
    with pytest.raises(ValueError, match="escapes"):
        editor.write("../outside.txt", "no")


def test_search_engine_lists_finds_and_greps(tmp_path):
    (tmp_path / "src").mkdir()
    (tmp_path / "src" / "main.py").write_text("needle = 1\n")
    search = agent.SearchEngine(tmp_path)
    assert search.list_directory() == "src/"
    assert search.search_files("*.py") == "src/main.py"
    assert search.grep("needle") == "src/main.py:1:needle = 1"


def test_command_executor_and_git_manager(tmp_path):
    init_git(tmp_path)
    executor = agent.CommandExecutor(tmp_path, timeout=5)
    git = agent.GitManager(executor)
    assert "hello" in executor.execute("printf hello")
    assert "No such" not in git.status()
    assert "exit code 0" in git.branch()


def test_command_executor_isolates_parent_credentials(monkeypatch, tmp_path):
    monkeypatch.setenv("FORKMESH_AI_AGENT_AUTH", "relay-ticket")
    monkeypatch.setenv("CLOUDFLARE_API_TOKEN", "provider-secret")
    monkeypatch.setenv("AWS_SHARED_CREDENTIALS_FILE", "/private/aws")
    monkeypatch.setenv("SSH_AUTH_SOCK", "/private/agent.sock")
    monkeypatch.setenv("FORKMESH_TEST_VISIBLE", "preserved")
    monkeypatch.setenv("HOME", "/private/home")
    executor = agent.CommandExecutor(tmp_path, timeout=5)
    names = (
        "FORKMESH_AI_AGENT_AUTH", "CLOUDFLARE_API_TOKEN",
        "AWS_SHARED_CREDENTIALS_FILE", "SSH_AUTH_SOCK",
        "FORKMESH_TEST_VISIBLE", "HOME",
    )
    script = (
        "import json,os; print(json.dumps({name: os.environ.get(name) "
        "for name in %r}))" % (names,)
    )
    result = executor.execute(
        "%s -c %s" % (shlex.quote(sys.executable), shlex.quote(script)))
    data = json.loads(result.splitlines()[1])
    assert data == {
        "FORKMESH_AI_AGENT_AUTH": None,
        "CLOUDFLARE_API_TOKEN": None,
        "AWS_SHARED_CREDENTIALS_FILE": None,
        "SSH_AUTH_SOCK": None,
        "FORKMESH_TEST_VISIBLE": "preserved",
        "HOME": executor._private_home.name,
    }


def test_tool_registry_exposes_required_extensible_catalog(tmp_path):
    config = agent.AgentConfig(provider="test", test_command="printf tests", lint_command="printf lint")
    memory = agent.Memory()
    registry = agent.build_tool_registry(tmp_path, config, memory)
    names = {schema["function"]["name"] for schema in registry.schemas()}
    assert {
        "read_file",
        "write_file",
        "replace_text",
        "search_files",
        "grep",
        "list_directory",
        "execute_command",
        "git_status",
        "git_diff",
        "git_commit",
        "run_tests",
        "lint_project",
    } <= names
    result = asyncio.run(
        registry.execute(agent.ToolCall("1", "write_file", {"path": "x", "content": "y"}))
    )
    assert result.startswith("Applied")
    assert (tmp_path / "x").read_text() == "y"


class _FakeGit:
    def status(self):
        return "(exit code 0)\n## main"


class _FakeProvider(agent.Provider):
    def __init__(self):
        self.turn = 0
        self.requests = []

    async def complete(self, messages, tools, cancellation=None):
        self.requests.append((messages, tools))
        self.turn += 1
        if self.turn == 1:
            return agent.ChatResponse(
                tool_calls=[agent.ToolCall("c1", "write_file", {"path": "done.txt", "content": "yes"})]
            )
        return agent.ChatResponse(content="Done.", input_tokens=2, output_tokens=1, model="fake")


def test_planner_and_agent_loop_execute_tools_and_persist(tmp_path):
    provider = _FakeProvider()
    config = agent.AgentConfig(
        provider="test",
        streaming=False,
        session_path=str(tmp_path / "session.json"),
    )
    session = agent.Session(memory=agent.Memory(token_budget=2000))
    registry = agent.build_tool_registry(tmp_path, config, session.memory)
    runner = agent.Agent(
        provider,
        agent.Planner(_FakeGit()),
        session,
        registry,
        agent.PromptBuilder("system"),
        config,
    )
    answer = asyncio.run(runner.run("make it"))
    assert answer == "Done."
    assert (tmp_path / "done.txt").read_text() == "yes"
    assert [message["role"] for message in session.conversation.messages] == [
        "user",
        "assistant",
        "tool",
        "assistant",
    ]
    assert agent.Session.load(config.session_path).metadata["model"] == "fake"


def test_local_llama_uses_configured_installed_model_without_discovery():
    calls = []

    async def transport(_url, _headers, body, _timeout, _stream, _token):
        calls.append(json.loads(body))
        return b'{"choices":[{"message":{"content":"local"}}]}'

    provider = agent.LocalLlamaProvider(
        agent.AgentConfig(
            provider="local-llama",
            model="already-installed",
            endpoint="http://127.0.0.1:11434/v1/chat/completions",
            streaming=False,
        ),
        transport=transport,
    )
    response = asyncio.run(provider.complete([], []))
    assert response.content == "local"
    assert calls[0]["model"] == "already-installed"


def test_local_llama_discovers_first_ollama_model():
    requested_urls = []
    completion_models = []

    async def discover(url, timeout, cancellation):
        requested_urls.append((url, timeout))
        cancellation.raise_if_cancelled()
        return "installed-at-runtime"

    async def transport(_url, _headers, body, _timeout, _stream, _token):
        completion_models.append(json.loads(body)["model"])
        return b'{"choices":[{"message":{"content":"local"}}]}'

    provider = agent.LocalLlamaProvider(
        agent.AgentConfig(
            provider="local-llama",
            endpoint="http://127.0.0.1:11434/v1/chat/completions",
            streaming=False,
        ),
        transport=transport,
        model_discovery=discover,
    )
    response = asyncio.run(provider.complete([], []))
    assert response.content == "local"
    assert requested_urls == [("http://127.0.0.1:11434/api/tags", 600.0)]
    assert completion_models == ["installed-at-runtime"]


def test_cloudflare_provider_preserves_tools_usage_and_role_compatibility():
    requests = []

    async def transport(_url, _headers, body, _timeout, _stream, _token):
        requests.append(json.loads(body))
        return json.dumps(
            {
                "model": "cloudflare-model",
                "choices": [{"message": {"content": "", "tool_calls": [{
                    "id": "call-1",
                    "type": "function",
                    "function": {
                        "name": "read_file",
                        "arguments": '{"path":"README.md"}',
                    },
                }]}}],
                "usage": {"prompt_tokens": 9, "completion_tokens": 2},
            }
        ).encode()

    provider = agent.CloudflareProvider(
        agent.AgentConfig(
            provider="cloudflare-ai",
            model="configured-at-runtime",
            account_id="account",
            api_token="token",
            streaming=False,
            retry_base_delay=0,
        ),
        transport=transport,
    )
    response = asyncio.run(
        provider.complete(
            [
                {"role": "system", "content": "system"},
                {"role": "developer", "content": "context"},
                {"role": "user", "content": "task"},
            ],
            [{"type": "function", "function": {"name": "read_file"}}],
        )
    )
    assert requests[0]["messages"] == [
        {"role": "system", "content": "system\n\ncontext"},
        {"role": "user", "content": "task"},
    ]
    assert response.tool_calls == [
        agent.ToolCall("call-1", "read_file", {"path": "README.md"})
    ]
    assert (response.input_tokens, response.output_tokens) == (9, 2)
    assert provider.config.endpoint.endswith("/accounts/account/ai/v1/chat/completions")
    assert provider._headers()["authorization"] == "Bearer token"


class _ScriptedProvider(agent.Provider):
    """Replays scripted turns; an Exception entry is raised for that turn."""

    def __init__(self, turns):
        self.turns = list(turns)
        self.requests = []

    async def complete(self, messages, tools, cancellation=None):
        self.requests.append(messages)
        turn = self.turns.pop(0)
        if isinstance(turn, Exception):
            raise turn
        return turn


def _scripted_agent(tmp_path, provider, git=None, **config_fields):
    config = agent.AgentConfig(provider="test", streaming=False, **config_fields)
    session = agent.Session(memory=agent.Memory(token_budget=config.token_budget))
    registry = agent.build_tool_registry(tmp_path, config, session.memory)
    events = []
    runner = agent.Agent(
        provider,
        agent.Planner(git or _FakeGit()),
        session,
        registry,
        agent.PromptBuilder("system"),
        config,
        on_event=events.append,
    )
    return runner, session, events


def test_agent_runs_tool_calls_the_model_wrote_as_text(tmp_path):
    provider = _ScriptedProvider([
        agent.ChatResponse(content=(
            'I will create it.\n<|python_tag|>{"name": "write_file", '
            '"parameters": {"path": "a.txt", "content": "hi"}}')),
        agent.ChatResponse(content="Done."),
    ])
    runner, session, events = _scripted_agent(tmp_path, provider)
    assert asyncio.run(runner.run("make a.txt")) == "Done."
    assert (tmp_path / "a.txt").read_text() == "hi"
    assert any(event.type == "text_tool_calls" for event in events)
    assistant, tool = session.conversation.messages[1:3]
    assert assistant["content"] == "I will create it."
    assert assistant["tool_calls"][0]["function"]["name"] == "write_file"
    assert tool["tool_call_id"] == assistant["tool_calls"][0]["id"]


def test_text_tool_call_recovery_only_accepts_declared_tools():
    names = ["bash", "read_file"]
    calls, rest = agent._text_tool_calls(
        'Config is {"name": "demo", "version": 2}. '
        '```json\n[{"type": "function", "function": {"name": "bash", '
        '"arguments": "{\\"command\\": \\"ls\\"}"}}]\n```', names)
    assert calls == [agent.ToolCall("", "bash", {"command": "ls"})]
    assert rest.startswith('Config is {"name": "demo"')
    assert agent._text_tool_calls('{"name": "rm_rf", "parameters": {}}', names) == (
        [], '{"name": "rm_rf", "parameters": {}}')
    assert agent._text_tool_calls("All done.", names) == ([], "All done.")


def test_tool_registry_repairs_small_model_argument_shapes(tmp_path):
    registry = agent.build_tool_registry(
        tmp_path, agent.AgentConfig(provider="test"), agent.Memory())
    # "false" as a string used to be truthy, leaving the edit unapplied as a
    # preview while the model believed the file was written.
    result = asyncio.run(registry.execute(agent.ToolCall(
        "1", "write_file", {"path": "x", "content": "y", "preview": "false"})))
    assert result.startswith("Applied")
    nested = asyncio.run(registry.execute(agent.ToolCall(
        "2", "replace_text",
        {"parameters": {"path": "x", "old": "y", "new": "z", "count": "1"}})))
    assert nested.startswith("Applied")
    assert (tmp_path / "x").read_text() == "z"
    missing = asyncio.run(registry.execute(agent.ToolCall("3", "read_file", {})))
    assert "Missing required argument(s) for read_file: path" in missing
    assert "path (string, required)" in missing
    unknown = asyncio.run(registry.execute(agent.ToolCall("4", "cat", {})))
    assert "Available tools:" in unknown and "read_file" in unknown


def test_truncated_tool_arguments_are_reported_not_run_empty(tmp_path):
    calls = agent.OpenAICompatibleProvider._tool_calls([
        {"id": "a", "function": {"name": "write_file",
                                 "arguments": '{"path": "x", "content": "unterminated'}},
        {"id": "b", "name": "bash", "arguments": {}, "invalidArguments": '{"command": "l'},
        {"id": "c", "function": {"name": "bash", "arguments": '"{\\"command\\": \\"ls\\"}"'}},
    ])
    assert calls[2].arguments == {"command": "ls"}
    registry = agent.build_tool_registry(
        tmp_path, agent.AgentConfig(provider="test"), agent.Memory())
    for call in calls[:2]:
        result = asyncio.run(registry.execute(call))
        assert "were not valid JSON, so it did not run" in result
    assert not (tmp_path / "x").exists()


def test_streaming_separates_tool_calls_sent_without_an_index():
    chunks = [
        {"choices": [{"delta": {"tool_calls": [
            {"id": "a", "function": {"name": "bash", "arguments": {"command": "ls"}}}]}}]},
        {"choices": [{"delta": {"tool_calls": [
            {"id": "b", "function": {"name": "read_file", "arguments": '{"path": '}}]}}]},
        {"choices": [{"delta": {"tool_calls": [
            {"id": "b", "function": {"name": "read_file", "arguments": '"README.md"}'}}]}}]},
    ]

    async def transport(_url, _headers, _body, _timeout, _stream, _token):
        return _SseResponse([("data: " + json.dumps(chunk)).encode() for chunk in chunks])

    provider = agent.OpenAICompatibleProvider(
        agent.AgentConfig(provider="openai", endpoint="https://example.invalid", model="m"),
        transport=transport,
    )

    async def collect():
        return [event async for event in provider.stream([], [])]

    final = asyncio.run(collect())[-1].response
    assert final.tool_calls == [
        agent.ToolCall("a", "bash", {"command": "ls"}),
        agent.ToolCall("b", "read_file", {"path": "README.md"}),
    ]


def test_compaction_keeps_the_task_and_whole_tool_exchanges():
    messages = [{"role": "user", "content": "the task " * 50}]
    for turn in range(12):
        messages.append({"role": "assistant", "content": "",
                         "tool_calls": [{"id": "c%d" % turn, "type": "function",
                                         "function": {"name": "bash", "arguments": "{}"}}]})
        messages.append({"role": "tool", "tool_call_id": "c%d" % turn, "content": "out " * 300})
    conversation = agent.Conversation(messages)
    memory = agent.Memory(token_budget=2000)
    assert memory.compact(conversation) is True
    rebuilt = conversation.messages
    assert rebuilt[0]["role"] == "developer"
    assert rebuilt[0]["content"].startswith("Earlier session summary")
    assert "[called bash]" in rebuilt[0]["content"]
    assert rebuilt[1] == messages[0]
    assert rebuilt[2]["role"] == "assistant"
    # Compacting again folds the old summary in rather than stacking another.
    conversation.messages.extend(messages[1:9])
    memory.compact(conversation)
    summaries = [m for m in conversation.messages
                 if str(m.get("content")).startswith("Earlier session summary")]
    assert len(summaries) == 1
    assert conversation.messages[2]["role"] != "tool"


class _TrackingGit:
    def __init__(self, dirty=False):
        self.dirty = dirty

    def status(self):
        return "(exit code 0)\n## main" + ("\n M a.txt" if self.dirty else "")


def test_agent_recovers_from_a_context_window_overflow(tmp_path):
    turns = [agent.ChatResponse(tool_calls=[agent.ToolCall(
        "t%d" % n, "search_files", {"pattern": "p%d" % n})]) for n in range(8)]
    turns.append(agent.ProviderError('HTTP 413: {"error": "context_too_long"}', status=413))
    turns.append(agent.ChatResponse(content="Done."))
    provider = _ScriptedProvider(turns)
    runner, session, events = _scripted_agent(tmp_path, provider)
    assert asyncio.run(runner.run("find things")) == "Done."
    assert any(event.type == "compacted" for event in events)
    retried = provider.requests[-1]
    assert any(str(m.get("content", "")).startswith("Earlier session summary") for m in retried)
    assert len(retried) < len(provider.requests[-2])


def test_agent_does_not_accept_a_promise_or_a_toolless_answer_as_done(tmp_path):
    provider = _ScriptedProvider([
        agent.ChatResponse(content="Sure."),
        agent.ChatResponse(content="Let me look at the files first."),
        agent.ChatResponse(tool_calls=[agent.ToolCall("1", "list_directory", {})]),
        agent.ChatResponse(content="Done."),
    ])
    runner, session, events = _scripted_agent(tmp_path, provider)
    assert asyncio.run(runner.run("tidy up")) == "Done."
    nudges = [event.text for event in events if event.type == "nudge"]
    assert nudges[0].startswith("You replied without running any tool")
    assert nudges[1].startswith("Your reply describes a next step")
    assert len(provider.requests) == 4


def test_git_commit_stages_changes_when_nothing_is_staged(tmp_path):
    subprocess.run(["git", "init", "-q", "-b", "main", str(tmp_path)], check=True)
    subprocess.run(["git", "-C", str(tmp_path), "config", "user.name", "T"], check=True)
    subprocess.run(["git", "-C", str(tmp_path), "config", "user.email", "t@example.com"], check=True)
    (tmp_path / "new.txt").write_text("hello\n", encoding="utf-8")
    git = agent.GitManager(agent.CommandExecutor(tmp_path, timeout=30))
    assert git.commit("add new").startswith("(exit code 0)")
    assert "new.txt" in subprocess.run(
        ["git", "-C", str(tmp_path), "show", "--name-only", "--pretty="],
        capture_output=True, text=True).stdout
    assert git.restore("") == "A path is required."
    assert not hasattr(git, "stash")


def test_read_file_is_bounded_and_ranged(tmp_path):
    (tmp_path / "big.txt").write_text("".join("line %d\n" % n for n in range(1, 5001)))
    editor = agent.FileEditor(tmp_path)
    head = editor.read_excerpt("big.txt", limit=1000)
    assert head.startswith("line 1\n")
    assert "Call read_file with start_line=" in head
    assert editor.read_excerpt("big.txt", 10, 12) == "line 10\nline 11\nline 12\n"
    output = agent._clip_middle("start " + "x" * 50000 + " FAILED: test_one", 1000)
    assert output.startswith("start ") and output.endswith("FAILED: test_one")


def test_cloudflare_sends_temperature_and_reports_a_missing_model():
    requests = []

    async def transport(_url, _headers, body, _timeout, _stream, _token):
        requests.append(json.loads(body))
        raise agent.ProviderError(
            'HTTP 502: {"error": "model_not_found"}', status=502, retryable=True)

    provider = agent.CloudflareProvider(
        agent.AgentConfig(provider="cloudflare-ai", model="gone", temperature=0.1,
                          account_id="account", api_token="token", streaming=False,
                          retry_count=0, retry_base_delay=0),
        transport=transport,
    )
    with pytest.raises(agent.ProviderError):
        asyncio.run(provider.complete([{"role": "user", "content": "x"}], []))
    assert len(requests) == 1
    assert requests[0]["temperature"] == 0.1


@contextlib.contextmanager
def gate_server(reply):
    directory = tempfile.mkdtemp(prefix="fmgate-", dir="/tmp")
    path = os.path.join(directory, "gate.sock")
    seen = []
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(path)
    listener.listen(8)
    listener.settimeout(0.1)
    stop = threading.Event()

    def serve():
        while not stop.is_set():
            try:
                conn, _ = listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            with conn:
                conn.settimeout(5)
                data = b""
                while not data.endswith(b"\n"):
                    chunk = conn.recv(4096)
                    if not chunk:
                        break
                    data += chunk
                seen.append(json.loads(data))
                if reply is None:
                    stop.wait(2)
                    continue
                answer = reply(seen[-1]) if callable(reply) else reply
                conn.sendall(answer if isinstance(answer, bytes)
                             else json.dumps(answer).encode() + b"\n")

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    try:
        yield path, seen
    finally:
        stop.set()
        listener.close()
        thread.join(3)
        os.unlink(path)
        os.rmdir(directory)


def gated_registry(tmp_path, gate_path, **fields):
    config = agent.AgentConfig(provider="test", gate_socket=gate_path,
                               gate_session="s-7", gate_label="Agent 7", **fields)
    return agent.build_tool_registry(tmp_path, config, agent.Memory())


def run_call(registry, name, arguments):
    return asyncio.run(registry.execute(agent.ToolCall("c", name, arguments)))


def test_gate_allows_a_command_after_asking(tmp_path):
    with gate_server({"decision": "allow"}) as (path, seen):
        registry = gated_registry(tmp_path, path)
        result = run_call(registry, "bash", {"command": "printf ran > out.txt"})
    assert result.startswith("(exit code 0)")
    assert (tmp_path / "out.txt").read_text() == "ran"
    assert seen == [{
        "v": 1, "session": "s-7", "label": "Agent 7", "tool": "Bash",
        "input": {"command": "printf ran > out.txt"},
        "cwd": str(tmp_path.resolve()),
    }]


def test_gate_denial_reaches_the_model_and_the_command_does_not_run(tmp_path):
    with gate_server({"decision": "deny", "reason": "No network for this project."}) as (path, seen):
        registry = gated_registry(tmp_path, path, test_command="printf t > t.txt")
        bash = run_call(registry, "execute_command", {"command": "curl https://x.invalid > out.txt"})
        tests = run_call(registry, "run_tests", {})
        commit = run_call(registry, "git_commit", {"message": "wip", "paths": ["a b.txt"]})
    assert bash.startswith("Not run:") and "No network for this project." in bash
    assert "Not run:" in tests and "Not run:" in commit
    assert not (tmp_path / "out.txt").exists() and not (tmp_path / "t.txt").exists()
    assert [request["input"]["command"] for request in seen] == [
        "curl https://x.invalid > out.txt",
        "printf t > t.txt",
        "git add -- 'a b.txt' && git commit -m wip",
    ]


def test_git_tools_and_lint_ask_with_their_command_line(tmp_path):
    with gate_server({"decision": "deny", "reason": "no"}) as (path, seen):
        registry = gated_registry(tmp_path, path, lint_command="ruff check .")
        for name, arguments in (
            ("git_status", {}), ("git_diff", {"staged": True}), ("git_log", {"count": 3}),
            ("git_branch", {}), ("git_restore", {"path": "x.py"}), ("lint_project", {}),
            ("read_file", {"path": "missing.txt"}),
        ):
            run_call(registry, name, arguments)
    assert [request["input"]["command"] for request in seen] == [
        "git status --short --branch", "git diff --binary --cached",
        "git log --oneline -n 3", "git branch --show-current",
        "git restore -- x.py", "ruff check .",
    ]


def test_gate_fails_closed_when_unreachable_or_garbled(tmp_path):
    missing = agent.AgentConfig(provider="test", gate_socket=str(tmp_path / "no-such.sock"))
    registry = agent.build_tool_registry(tmp_path, missing, agent.Memory())
    result = run_call(registry, "bash", {"command": "printf ran > out.txt"})
    assert result.startswith("Not run:") and "could not ask the user" in result
    with gate_server(b"not json\n") as (path, _seen):
        result = run_call(gated_registry(tmp_path, path), "bash", {"command": "printf ran > out.txt"})
    assert result.startswith("Not run:") and "unreadable" in result
    with gate_server({"decision": "maybe"}) as (path, _seen):
        result = run_call(gated_registry(tmp_path, path), "bash", {"command": "printf ran > out.txt"})
    assert result.startswith("Not run:") and "unreadable" in result
    with gate_server(None) as (path, _seen):
        denial = agent.ExternalCallGate(path, timeout=0.3).check("ls")
    assert "could not ask the user" in denial
    assert not (tmp_path / "out.txt").exists()


def test_without_a_gate_nothing_is_asked(tmp_path):
    registry = agent.build_tool_registry(tmp_path, agent.AgentConfig(provider="test"), agent.Memory())
    assert registry.gate is None
    assert run_call(registry, "bash", {"command": "printf ok"}).startswith("(exit code 0)")


def test_gate_variables_are_read_and_kept_from_commands(monkeypatch, tmp_path):
    monkeypatch.setenv("FORKMESH_AGENT_GATE", "/tmp/gate.sock")
    monkeypatch.setenv("FORKMESH_AGENT_GATE_SESSION", "abc")
    monkeypatch.setenv("FORKMESH_AGENT_GATE_LABEL", "Fix login")
    config = agent.AgentConfig.from_env()
    assert (config.gate_socket, config.gate_session, config.gate_label) == (
        "/tmp/gate.sock", "abc", "Fix login")
    environment = agent.CommandExecutor(tmp_path, timeout=5).environment
    assert not any(name.startswith("FORKMESH_AGENT_GATE") for name in environment)


def offered(registry):
    return {schema["function"]["name"] for schema in registry.schemas()}


def test_disabled_tool_families_are_not_offered_and_refused(tmp_path):
    config = agent.AgentConfig(provider="test", disabled_tool_families=["bash", "edit"])
    registry = agent.build_tool_registry(tmp_path, config, agent.Memory())
    assert offered(registry) == {"read_file", "write_file", "grep", "search_files", "list_directory"}
    result = run_call(registry, "bash", {"command": "printf ran > out.txt"})
    assert "turned off" in result and "bash" in result
    assert "turned off" in run_call(registry, "replace_text", {"path": "a", "old": "a", "new": "b"})
    assert not (tmp_path / "out.txt").exists()
    assert set(registry.withheld()) == set(agent.TOOL_FAMILIES) - offered(registry)
    unknown = run_call(registry, "cat", {})
    assert "read_file" in unknown and "bash" not in unknown.split("Available tools:")[1]


def test_allowed_tool_families_are_an_allowlist(tmp_path):
    config = agent.AgentConfig(provider="test", allowed_tool_families=["read", "glob", "bash"],
                               disabled_tool_families=["bash"])
    registry = agent.build_tool_registry(tmp_path, config, agent.Memory())
    assert offered(registry) == {"read_file", "list_directory"}
    assert "turned off" in run_call(registry, "write_file", {"path": "x", "content": "y"})
    assert not (tmp_path / "x").exists()


def test_every_tool_has_a_catalog_family(tmp_path):
    registry = agent.build_tool_registry(tmp_path, agent.AgentConfig(provider="test"), agent.Memory())
    assert offered(registry) == set(agent.TOOL_FAMILIES)
    assert set(agent.TOOL_FAMILIES.values()) <= {"bash", "read", "write", "edit", "grep", "glob"}


def test_tool_family_variables_are_parsed(monkeypatch):
    monkeypatch.setenv("FORKMESH_AGENT_DISABLED_TOOL_FAMILIES", " Bash, edit,,bash ")
    monkeypatch.setenv("FORKMESH_AGENT_ALLOWED_TOOL_FAMILIES", "read,glob")
    config = agent.AgentConfig.from_env()
    assert config.disabled_tool_families == ["bash", "edit"]
    assert config.allowed_tool_families == ["read", "glob"]
    monkeypatch.setenv("FORKMESH_AGENT_ALLOWED_TOOL_FAMILIES", "")
    assert agent.AgentConfig.from_env().allowed_tool_families == []


def test_guardrails_default_on_and_keep_tools_in_the_repository(monkeypatch, tmp_path):
    monkeypatch.delenv("FORKMESH_AGENT_ALLOWED_DIRECTORIES", raising=False)
    monkeypatch.delenv("FORKMESH_AGENT_GUARDRAILS", raising=False)
    config = agent.AgentConfig.from_env()
    assert config.guardrails_enabled and config.allowed_directories == ["."]
    root = tmp_path / "repo"
    root.mkdir()
    (tmp_path / "outside.txt").write_text("secret")
    init_git(root)
    config = agent.AgentConfig(provider="test")
    registry = agent.build_tool_registry(root, config, agent.Memory())
    assert registry.access.enabled
    assert run_call(registry, "write_file", {"path": "in.txt", "content": "ok"}).startswith("Applied")
    assert "escapes" in run_call(registry, "read_file", {"path": "../outside.txt"})
    patch = "--- a/../outside.txt\n+++ b/../outside.txt\n@@ -1 +1 @@\n-secret\n+gone\n"
    assert "escapes" in run_call(registry, "apply_patch", {"patch": patch})
    assert (tmp_path / "outside.txt").read_text() == "secret"


def test_guardrails_turn_off_only_when_asked(monkeypatch):
    monkeypatch.delenv("FORKMESH_AGENT_ALLOWED_DIRECTORIES", raising=False)
    monkeypatch.setenv("FORKMESH_AGENT_GUARDRAILS", "off")
    config = agent.AgentConfig.from_env()
    assert not config.guardrails_enabled and config.allowed_directories == []
    monkeypatch.setenv("FORKMESH_AGENT_GUARDRAILS", "on")
    assert agent.AgentConfig.from_env().guardrails_enabled
    monkeypatch.setenv("FORKMESH_AGENT_GUARDRAILS", "off")
    monkeypatch.setenv("FORKMESH_AGENT_ALLOWED_DIRECTORIES", '["src"]')
    config = agent.AgentConfig.from_env()
    assert config.guardrails_enabled and config.allowed_directories == ["src"]
    monkeypatch.setenv("FORKMESH_AGENT_ALLOWED_DIRECTORIES", "[]")
    config = agent.AgentConfig.from_env()
    assert config.guardrails_enabled and config.allowed_directories == []


def load_claude_script(monkeypatch, **environment):
    for name in ("FORKMESH_AGENT_GATE", "FORKMESH_AGENT_GATE_SESSION", "FORKMESH_AGENT_GATE_LABEL",
                 "FORKMESH_AGENT_DISABLED_TOOL_FAMILIES", "FORKMESH_AGENT_ALLOWED_TOOL_FAMILIES"):
        monkeypatch.delenv(name, raising=False)
    for name, value in environment.items():
        monkeypatch.setenv(name, value)
    header = CLAUDE_HEADER.read_text(encoding="utf-8")
    source = re.search(r'R"PYAGENT\((.*)\)PYAGENT"', header, re.DOTALL).group(1)
    namespace = {"__name__": "embedded_claude_agent_gate_test"}
    exec(compile(source, str(CLAUDE_HEADER), "exec"), namespace)
    return namespace


def test_claude_api_script_asks_the_gate_before_bash(monkeypatch, tmp_path):
    monkeypatch.chdir(tmp_path)
    with gate_server(lambda request: {"decision": "allow" if "allowed" in request["input"]["command"] else "deny",
                                      "reason": "Blocked by the project."}) as (path, seen):
        script = load_claude_script(monkeypatch, FORKMESH_AGENT_GATE=path,
                                    FORKMESH_AGENT_GATE_SESSION="s-1", FORKMESH_AGENT_GATE_LABEL="Docs")
        allowed = script["run_tool"]("printf allowed > ok.txt")
        denied = script["run_tool"]("printf nope > no.txt")
    assert allowed.startswith("(exit code 0)") and (tmp_path / "ok.txt").exists()
    assert denied.startswith("Not run:") and "Blocked by the project." in denied
    assert not (tmp_path / "no.txt").exists()
    assert seen[0]["session"] == "s-1" and seen[0]["label"] == "Docs" and seen[0]["tool"] == "Bash"
    assert seen[0]["cwd"] == str(tmp_path)
    assert not any(name.startswith("FORKMESH_AGENT_GATE") for name in script["COMMAND_ENV"])
    script["COMMAND_HOME"].cleanup()


def test_claude_api_script_fails_closed_and_skips_the_gate_when_unset(monkeypatch, tmp_path):
    monkeypatch.chdir(tmp_path)
    script = load_claude_script(monkeypatch, FORKMESH_AGENT_GATE=str(tmp_path / "gone.sock"))
    result = script["run_tool"]("printf ran > out.txt")
    assert result.startswith("Not run:") and "could not ask the user" in result
    assert not (tmp_path / "out.txt").exists()
    script["COMMAND_HOME"].cleanup()
    script = load_claude_script(monkeypatch)
    assert script["run_tool"]("printf ran > out.txt").startswith("(exit code 0)")
    script["COMMAND_HOME"].cleanup()


@pytest.mark.parametrize("environment", [
    {"FORKMESH_AGENT_DISABLED_TOOL_FAMILIES": "read,bash"},
    {"FORKMESH_AGENT_ALLOWED_TOOL_FAMILIES": "read,edit"},
])
def test_claude_api_script_stops_when_bash_is_off(monkeypatch, capsys, environment):
    script = load_claude_script(monkeypatch, ANTHROPIC_API_KEY="k", **environment)
    script["call_api"] = lambda *_args: pytest.fail("the API must not be called")
    assert script["main"]() == 1
    assert "bash" in capsys.readouterr().out
    script["COMMAND_HOME"].cleanup()
