"""Local-model (Ollama / Gemma 4) coverage for the embedded coding agent.

The native Ollama route, text-embedded tool-call recovery, argument coercion,
bounded tool output, output-cutoff nudges, tool-result elision, and the
staging git_commit all exist so a small local model completes real tasks.
"""

from __future__ import annotations

import asyncio
import importlib.util
import json
import subprocess
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "tools" / "forkmesh_coding_agent.py"

spec = importlib.util.spec_from_file_location("forkmesh_local_agent", SOURCE)
agent = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = agent
spec.loader.exec_module(agent)


SHOW = {
    "capabilities": ["completion", "tools", "thinking"],
    "model_info": {"gemma4.context_length": 131072, "gemma4.block_count": 42},
}


class _Lines:
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


def _ollama(config=None, *, show=SHOW, tags=None, transport=None, log=None):
    calls = {"fetch": [], "requests": []}

    async def fetch(url, body, _timeout, _token):
        calls["fetch"].append((url, body))
        if url.endswith("/api/tags"):
            return {"models": [{"name": name} for name in (tags or [])]}
        if url.endswith("/api/show"):
            if isinstance(show, dict) and "__by_model__" in show:
                info = show["__by_model__"].get(body["model"])
                if info is None:
                    raise agent.ProviderError("HTTP 404: not found", status=404)
                return info
            if show is None:
                raise agent.ProviderError("HTTP 404: model not found", status=404)
            return show
        raise AssertionError("unexpected fetch " + url)

    async def default_transport(url, headers, body, _timeout, stream, _token):
        calls["requests"].append((url, headers, json.loads(body.decode("utf-8")), stream))
        return b'{"model":"gemma4:e4b","message":{"role":"assistant","content":"done"},"done":true,"done_reason":"stop","prompt_eval_count":12,"eval_count":3}'

    provider = agent.OllamaProvider(
        config or agent.AgentConfig(provider="local-llama", model="gemma4:e4b", endpoint="http://127.0.0.1:11434/v1/chat/completions"),
        fetch=fetch,
        transport=transport or default_transport,
        logger=log,
    )
    return provider, calls


def test_ollama_provider_sends_native_payload_with_context_thinking_and_keep_alive():
    config = agent.AgentConfig(
        provider="local-llama",
        model="gemma4:e4b",
        endpoint="http://127.0.0.1:11434/v1/chat/completions",
        token_budget=32000,
        max_tokens=4096,
        thinking="off",
        keep_alive="45m",
    )
    lines = []
    provider, calls = _ollama(config, log=lines.append)
    messages = [
        {"role": "system", "content": "sys"},
        {"role": "developer", "content": "dev"},
        {"role": "user", "content": "task"},
        {"role": "assistant", "content": "", "tool_calls": [
            {"id": "c1", "type": "function", "function": {"name": "grep", "arguments": "{\"pattern\": \"x\"}"}}]},
        {"role": "tool", "content": "a.py:1:x", "tool_call_id": "c1", "name": "grep"},
    ]
    tools = [{"type": "function", "function": {"name": "grep", "parameters": {"type": "object", "properties": {}}}}]
    response = asyncio.run(provider.complete(messages, tools))
    assert response.content == "done"
    assert (response.input_tokens, response.output_tokens) == (12, 3)
    url, headers, payload, stream = calls["requests"][0]
    assert url == "http://127.0.0.1:11434/api/chat"
    assert "authorization" not in headers
    assert stream is False
    assert payload["model"] == "gemma4:e4b"
    assert payload["think"] is False
    assert payload["keep_alive"] == "45m"
    assert payload["tools"] == tools
    assert payload["options"]["num_predict"] == 4096
    assert payload["options"]["num_ctx"] == agent.OllamaProvider.context_size(32000, 4096, 131072)
    # developer context folds into the system prompt; tool calls carry object
    # arguments and tool replies name their tool, as Ollama's template expects.
    wired = payload["messages"]
    assert [m["role"] for m in wired] == ["system", "user", "assistant", "tool"]
    assert wired[0]["content"] == "sys\n\ndev"
    assert wired[2]["tool_calls"] == [{"function": {"name": "grep", "arguments": {"pattern": "x"}}}]
    assert wired[3]["tool_name"] == "grep"
    # The desktop's network panel counts these lines.
    assert any("[net]" in line and "request #1" in line for line in lines)
    assert any("[net]" in line and "response #1" in line and "in=12 out=3" in line for line in lines)
    assert any(line.startswith("==> [local] gemma4:e4b") and "thinking off" in line for line in lines)


def test_ollama_context_size_leaves_headroom_and_respects_the_model():
    assert agent.OllamaProvider.context_size(32000, 4096, 131072) == 47104
    assert agent.OllamaProvider.context_size(32000, 4096, 8192) == 8192
    assert agent.OllamaProvider.context_size(1000, 256, 0) == 4096


def test_ollama_thinking_levels_and_non_thinking_models():
    assert agent._thinking_value("off") is False
    assert agent._thinking_value("") is False
    assert agent._thinking_value("on") is True
    assert agent._thinking_value("HIGH") == "high"
    provider, calls = _ollama(show={"capabilities": ["completion", "tools"], "model_info": {}})
    provider.config.thinking = "on"
    asyncio.run(provider.complete([{"role": "user", "content": "x"}], []))
    payload = calls["requests"][0][2]
    assert "think" not in payload  # the model cannot think; Ollama would 400


def test_ollama_provider_refuses_models_without_tool_calling():
    provider, _calls = _ollama(show={"capabilities": ["completion"], "model_info": {}})
    with pytest.raises(agent.ProviderError, match="does not support tool calling"):
        asyncio.run(provider.complete([{"role": "user", "content": "x"}], []))


def test_ollama_provider_explains_a_missing_model():
    provider, _calls = _ollama(show=None)
    with pytest.raises(agent.ProviderError, match="No local Llama model named gemma4:e4b is installed"):
        asyncio.run(provider.complete([{"role": "user", "content": "x"}], []))


def test_ollama_discovery_prefers_a_tools_capable_model():
    config = agent.AgentConfig(provider="local-llama", endpoint="http://127.0.0.1:11434/v1/chat/completions")
    provider, calls = _ollama(
        config,
        show={"__by_model__": {
            "embed:latest": {"capabilities": ["embedding"], "model_info": {}},
            "gemma4:e4b": SHOW,
        }},
        tags=["embed:latest", "gemma4:e4b"],
    )
    asyncio.run(provider.complete([{"role": "user", "content": "x"}], []))
    assert provider.config.model == "gemma4:e4b"
    assert calls["requests"][0][2]["model"] == "gemma4:e4b"


def test_ollama_stream_decodes_ndjson_tool_calls_thinking_and_speed():
    chunks = [
        {"model": "gemma4:e4b", "message": {"role": "assistant", "content": "", "thinking": "plan"}, "done": False},
        {"model": "gemma4:e4b", "message": {"role": "assistant", "content": "Look"}, "done": False},
        {"model": "gemma4:e4b", "message": {"role": "assistant", "content": "", "tool_calls": [
            {"function": {"index": 0, "name": "grep", "arguments": {"pattern": "def x"}}}]}, "done": False},
        {"model": "gemma4:e4b", "message": {"role": "assistant", "content": ""}, "done": True,
         "done_reason": "stop", "prompt_eval_count": 88, "eval_count": 16, "eval_duration": 2_000_000_000},
    ]

    async def transport(_url, headers, body, _timeout, stream, _token):
        assert stream is True
        assert headers["accept"] == "application/x-ndjson"
        assert json.loads(body.decode("utf-8"))["stream"] is True
        return _Lines([(json.dumps(chunk) + "\n").encode("utf-8") for chunk in chunks])

    lines = []
    provider, _calls = _ollama(transport=transport, log=lines.append)
    provider.config.show_reasoning = True

    async def collect():
        return [event async for event in provider.stream([{"role": "user", "content": "x"}], [])]

    events = asyncio.run(collect())
    assert [e.type for e in events] == ["reasoning", "text", "done"]
    final = events[-1].response
    assert final.content == "Look"
    assert final.reasoning == "plan"
    assert [(c.name, c.arguments) for c in final.tool_calls] == [("grep", {"pattern": "def x"})]
    assert final.tool_calls[0].id
    assert (final.input_tokens, final.output_tokens, final.finish_reason) == (88, 16, "stop")
    assert any("8.0 tok/s" in line and "stop=tool_use" in line for line in lines)


def test_ollama_stream_reports_output_cut_off():
    chunks = [
        {"model": "m", "message": {"role": "assistant", "content": "partial"}, "done": False},
        {"model": "m", "message": {"role": "assistant", "content": ""}, "done": True, "done_reason": "length"},
    ]

    async def transport(*_args):
        return _Lines([(json.dumps(chunk) + "\n").encode("utf-8") for chunk in chunks])

    provider, _calls = _ollama(transport=transport)

    async def collect():
        return [event async for event in provider.stream([{"role": "user", "content": "x"}], [])]

    assert asyncio.run(collect())[-1].response.finish_reason == "length"


def test_openai_stream_reads_ollama_reasoning_alias_and_finish_reason():
    chunks = [
        {"model": "m", "choices": [{"delta": {"content": "", "reasoning": "think"}}]},
        {"model": "m", "choices": [{"delta": {"content": "hi"}, "finish_reason": "length"}]},
    ]
    lines = [("data: " + json.dumps(chunk) + "\n").encode("utf-8") for chunk in chunks] + [b"data: [DONE]\n"]

    async def transport(*_args):
        return _Lines(lines)

    provider = agent.OpenAICompatibleProvider(
        agent.AgentConfig(provider="openai", model="m", endpoint="http://x.invalid/v1/chat/completions"),
        transport=transport,
    )

    async def collect():
        return [event async for event in provider.stream([{"role": "user", "content": "x"}], [])]

    final = asyncio.run(collect())[-1].response
    assert final.reasoning == "think"
    assert final.finish_reason == "length"


def test_detect_local_api_by_path_and_probe():
    assert agent.detect_local_api("http://127.0.0.1:11434/api/chat") == "ollama"
    # Nothing listens on the discard port, so the probe falls back to OpenAI.
    assert agent.detect_local_api("http://127.0.0.1:9/v1/chat/completions", timeout=0.5) == "openai"


def test_provider_factory_honours_forced_local_api(monkeypatch):
    config = agent.AgentConfig(provider="local-llama", model="m", endpoint="http://127.0.0.1:9/v1/chat/completions", local_api="ollama")
    assert isinstance(agent._provider_from_config(config), agent.OllamaProvider)
    config.local_api = "openai"
    assert isinstance(agent._provider_from_config(config), agent.LocalLlamaProvider)
    monkeypatch.setenv("FORKMESH_AGENT_THINKING", "High")
    monkeypatch.setenv("FORKMESH_AGENT_LOCAL_API", "Ollama")
    monkeypatch.setenv("FORKMESH_AGENT_KEEP_ALIVE", "1h")
    loaded = agent.AgentConfig.from_env()
    assert (loaded.thinking, loaded.local_api, loaded.keep_alive) == ("high", "ollama", "1h")


# --- text-embedded tool calls -------------------------------------------------

NAMES = ["grep", "read_file", "git_commit"]


@pytest.mark.parametrize(
    "content",
    [
        '```json\n{"name": "grep", "arguments": {"pattern": "def x", "path": "src"}}\n```',
        '```tool_code\n{"name": "grep", "parameters": {"pattern": "def x", "path": "src"}}\n```',
        '<tool_call>{"name": "grep", "arguments": {"pattern": "def x", "path": "src"}}</tool_call>',
        'I will search.\n{"name": "grep", "arguments": {"pattern": "def x", "path": "src"}}',
        '{"function": {"name": "grep", "arguments": "{\\"pattern\\": \\"def x\\", \\"path\\": \\"src\\"}"}}',
        '{"tool": "grep", "input": {"pattern": "def x", "path": "src"}}',
    ],
)
def test_tool_calls_written_as_text_are_recovered(content):
    calls, remainder = agent._text_tool_calls(content, NAMES)
    assert [(c.name, c.arguments) for c in calls] == [("grep", {"pattern": "def x", "path": "src"})]
    assert "def x" not in remainder


def test_text_recovery_ignores_ordinary_json_and_unknown_tools():
    assert agent._text_tool_calls('Config is {"debug": true}.', NAMES)[0] == []
    assert agent._text_tool_calls('{"name": "rm_rf", "arguments": {}}', NAMES)[0] == []
    assert agent._text_tool_calls("Done. Nothing else to do.", NAMES) == ([], "Done. Nothing else to do.")


def test_text_recovery_handles_arrays_and_keeps_surrounding_prose():
    content = 'Two steps:\n```json\n[{"name": "grep", "arguments": {"pattern": "a"}}, {"name": "read_file", "arguments": {"path": "b"}}]\n```\nthen commit.'
    calls, remainder = agent._text_tool_calls(content, NAMES)
    assert [c.name for c in calls] == ["grep", "read_file"]
    assert "Two steps" in remainder and "then commit" in remainder


def test_agent_loop_runs_recovered_text_tool_calls(tmp_path):
    events = []
    seen = []

    class Provider(agent.Provider):
        def __init__(self):
            self.turn = 0

        async def complete(self, messages, tools, cancellation=None):
            self.turn += 1
            seen.append(messages)
            if self.turn == 1:
                return agent.ChatResponse(content='```json\n{"name": "list_directory", "arguments": {"path": "."}}\n```')
            return agent.ChatResponse(content="Listed the tree.")

    class Git:
        def status(self):
            return "(exit code 0)\n## main"

    config = agent.AgentConfig(provider="local-llama", model="m", streaming=False)
    memory = agent.Memory()
    (tmp_path / "hello.txt").write_text("hi", encoding="utf-8")
    registry = agent.build_tool_registry(tmp_path, config, memory)
    session = agent.Session(memory=memory)
    runner = agent.Agent(Provider(), agent.Planner(Git()), session, registry, agent.PromptBuilder(), config, on_event=events.append)
    assert asyncio.run(runner.run("look around")) == "Listed the tree."
    assert [e.type for e in events if e.type in {"text_tool_calls", "tool_call", "tool_result"}] == ["text_tool_calls", "tool_call", "tool_result"]
    assert "hello.txt" in [e.text for e in events if e.type == "tool_result"][0]
    # The recovered call is echoed back structurally so the next turn sees a
    # proper assistant tool call followed by its tool reply.
    roles = [m["role"] for m in seen[1]]
    assert roles[-2:] == ["assistant", "tool"]
    assert seen[1][-2]["tool_calls"][0]["function"]["name"] == "list_directory"


def test_agent_loop_nudges_once_when_the_reply_is_cut_off(tmp_path):
    events = []
    seen = []

    class Provider(agent.Provider):
        def __init__(self):
            self.turn = 0

        async def complete(self, messages, tools, cancellation=None):
            self.turn += 1
            seen.append(messages)
            if self.turn == 1:
                return agent.ChatResponse(content='{"name": "write_file", "arguments": {"path": "a.py", "content": "def f():\\n    ret', finish_reason="length")
            if self.turn == 2:
                assert not (tmp_path / "a.py").exists()  # the cut-off call never ran
                return agent.ChatResponse(tool_calls=[agent.ToolCall("w", "write_file", {"path": "a.py", "content": "def f():\n    return 1\n"})])
            return agent.ChatResponse(content="Wrote it in smaller steps.")

    class Git:
        def status(self):
            return "(exit code 0)\n## main"

    config = agent.AgentConfig(provider="local-llama", model="m", streaming=False)
    memory = agent.Memory()
    registry = agent.build_tool_registry(tmp_path, config, memory)
    runner = agent.Agent(Provider(), agent.Planner(Git()), agent.Session(memory=memory), registry, agent.PromptBuilder(), config, on_event=events.append)
    assert asyncio.run(runner.run("write a.py")) == "Wrote it in smaller steps."
    nudges = [e.text for e in events if e.type == "nudge"]
    assert len(nudges) == 1 and "cut off at the output limit" in nudges[0]
    assert seen[1][-1]["role"] == "user" and "smaller steps" in seen[1][-1]["content"]
    assert (tmp_path / "a.py").read_text(encoding="utf-8") == "def f():\n    return 1\n"


# --- tool registry -----------------------------------------------------------

def test_registry_coerces_argument_types_and_explains_missing_ones(tmp_path):
    config = agent.AgentConfig(provider="local-llama", model="m")
    registry = agent.build_tool_registry(tmp_path, config, agent.Memory())
    (tmp_path / "a.txt").write_text("one\ntwo\nthree\nfour\n", encoding="utf-8")
    result = asyncio.run(registry.execute(agent.ToolCall("1", "read_file", {"path": "a.txt", "start_line": "2", "end_line": "3"})))
    assert result == "two\nthree\n"
    result = asyncio.run(registry.execute(agent.ToolCall("2", "replace_text", {"path": "a.txt", "old": "o", "new": "0", "count": "2"})))
    assert result.startswith("Applied")
    assert (tmp_path / "a.txt").read_text(encoding="utf-8") == "0ne\ntw0\nthree\nfour\n"
    result = asyncio.run(registry.execute(agent.ToolCall("3", "grep", {"path": "."})))
    assert result.startswith("Missing required argument(s) for grep: pattern. Parameters: pattern (string, required), path (string), regex (boolean).")
    result = asyncio.run(registry.execute(agent.ToolCall("4", "nope", {})))
    assert result.startswith("Unknown tool: nope. Available tools: apply_patch, bash")
    result = asyncio.run(registry.execute(agent.ToolCall("5", "read_file", {"path": "example/calc/x.py"})))
    assert result.startswith("Tool error: example/calc/x.py does not exist (paths are relative to the repository root")
    assert str(tmp_path) not in result




def test_git_commit_stages_everything_unless_told_otherwise(tmp_path):
    subprocess.run(["git", "init", "-q", str(tmp_path)], check=True)
    subprocess.run(["git", "-C", str(tmp_path), "config", "user.email", "t@example.com"], check=True)
    subprocess.run(["git", "-C", str(tmp_path), "config", "user.name", "T"], check=True)
    config = agent.AgentConfig(provider="local-llama", model="m")
    registry = agent.build_tool_registry(tmp_path, config, agent.Memory())
    (tmp_path / "new.py").write_text("x = 1\n", encoding="utf-8")
    (tmp_path / "other.py").write_text("y = 2\n", encoding="utf-8")
    result = asyncio.run(registry.execute(agent.ToolCall("1", "git_commit", {"message": "add new", "paths": ["new.py"]})))
    assert "add new" in result
    tracked = subprocess.run(["git", "-C", str(tmp_path), "ls-files"], capture_output=True, text=True, check=True).stdout.split()
    assert tracked == ["new.py"]
    result = asyncio.run(registry.execute(agent.ToolCall("2", "git_commit", {"message": "add the rest"})))
    assert "add the rest" in result
    tracked = subprocess.run(["git", "-C", str(tmp_path), "ls-files"], capture_output=True, text=True, check=True).stdout.split()
    assert tracked == ["new.py", "other.py"]
    log = asyncio.run(registry.execute(agent.ToolCall("3", "git_log", {"count": "5"})))
    assert "add the rest" in log and "add new" in log
    schemas = {s["function"]["name"]: s["function"] for s in registry.schemas()}
    assert schemas["git_commit"]["parameters"]["properties"]["paths"]["type"] == "array"
    assert "stage every change" in schemas["git_commit"]["description"].lower()


def test_planner_seeds_the_tracked_file_tree(tmp_path):
    subprocess.run(["git", "init", "-q", str(tmp_path)], check=True)
    (tmp_path / "pkg").mkdir()
    (tmp_path / "pkg" / "mod.py").write_text("", encoding="utf-8")
    subprocess.run(["git", "-C", str(tmp_path), "add", "-A"], check=True)
    executor = agent.CommandExecutor(tmp_path)
    memory = agent.Memory()
    asyncio.run(agent.Planner(agent.GitManager(executor)).prepare("task", memory))
    assert "pkg/mod.py" in memory.repository_context
    assert "names the project, not a directory" in memory.repository_context



def test_default_system_prompt_spells_out_the_small_model_protocol():
    prompt = agent.DEFAULT_SYSTEM_PROMPT
    for phrase in ("replace_text", "git_commit", "stop calling tools", "Never invent", "start_line", "Do not repeat a successful mutation"):
        assert phrase in prompt


# --- verification gate ----------------------------------------------------------

def test_replace_text_warns_when_a_definition_disappears(tmp_path):
    editor = agent.FileEditor(tmp_path)
    (tmp_path / "calc.py").write_text("def add(a, b):\n    return a + b\n\n\ndef subtract(a, b):\n    return a - b\n", encoding="utf-8")
    result = editor.replace_text("calc.py", "def subtract(a, b):\n    return a - b", "def multiply(a, b):\n    return a * b")
    assert "Note: subtract no longer defined in calc.py after this edit; restore it" in result
    result = editor.replace_text("calc.py", "return a * b", "return a * b  # product")
    assert "Note:" not in result
    assert agent._dropped_definitions("class Foo:\n  pass\nfunction bar() {}", "// Foo is mentioned only\nfunction bar() {}") == ["Foo"]


def test_detect_test_command_recognises_common_layouts(tmp_path):
    assert agent.detect_test_command(tmp_path) == ""
    (tmp_path / "tests").mkdir()
    (tmp_path / "tests" / "test_x.py").write_text("", encoding="utf-8")
    command = agent.detect_test_command(tmp_path)
    assert command.endswith("-m pytest -q -x --no-header -p no:cacheprovider")
    node = tmp_path / "node"
    node.mkdir()
    (node / "package.json").write_text(json.dumps({"scripts": {"test": "vitest run"}}), encoding="utf-8")
    assert agent.detect_test_command(node) == "npm test --silent"
    (node / "package.json").write_text(json.dumps({"scripts": {"test": "echo \"Error: no test specified\" && exit 1"}}), encoding="utf-8")
    assert agent.detect_test_command(node) == ""
    go = tmp_path / "go"
    go.mkdir()
    (go / "go.mod").write_text("module x\n", encoding="utf-8")
    assert agent.detect_test_command(go) == "go test ./..."


def test_command_environment_keeps_user_site_packages_reachable(tmp_path, monkeypatch):
    home = tmp_path / "home"
    (home / ".local").mkdir(parents=True)
    monkeypatch.setenv("HOME", str(home))
    monkeypatch.delenv("PYTHONUSERBASE", raising=False)
    env = agent.CommandExecutor._command_environment(str(tmp_path / "private"))
    assert env["HOME"] == str(tmp_path / "private")
    assert env["PYTHONUSERBASE"] == str(home / ".local")


def test_control_tokens_are_stripped_from_replies():
    assert agent._strip_control_tokens("Done.<channel|>") == "Done."
    assert agent._strip_control_tokens("<|im_end|>ok<end_of_turn>") == "ok"
    assert agent._strip_control_tokens("a < b and <div>") == "a < b and <div>"


def _loop(tmp_path, provider, verifier, events):
    class Git:
        def status(self):
            return "(exit code 0)\n## main"

    config = agent.AgentConfig(provider="local-llama", model="m", streaming=False)
    memory = agent.Memory()
    registry = agent.build_tool_registry(tmp_path, config, memory)
    return agent.Agent(provider, agent.Planner(Git()), agent.Session(memory=memory), registry, agent.PromptBuilder(), config, on_event=events.append, verifier=verifier)


def test_failing_tests_go_back_to_the_model_once(tmp_path):
    events, seen, runs = [], [], []

    class Provider(agent.Provider):
        def __init__(self):
            self.turn = 0

        async def complete(self, messages, tools, cancellation=None):
            self.turn += 1
            seen.append(messages)
            if self.turn == 1:
                return agent.ChatResponse(tool_calls=[agent.ToolCall("1", "write_file", {"path": "a.py", "content": "x = 1\n"})])
            return agent.ChatResponse(content="Done (turn %d)." % self.turn)

    async def verifier():
        runs.append(1)
        return "(exit code 1)\nFAILED tests/test_a.py::test_x - AssertionError"

    result = asyncio.run(_loop(tmp_path, Provider(), verifier, events).run("edit a.py"))
    # First "done" ran the tests and bounced; the second is accepted even
    # though they still fail, so a broken pre-existing suite cannot loop.
    assert result == "Done (turn 3)."
    assert len(runs) == 2
    nudges = [e.text for e in events if e.type == "nudge"]
    assert len(nudges) == 1 and "test command fails" in nudges[0] and "AssertionError" in nudges[0]
    assert seen[2][-1]["role"] == "user" and "Fix the failures" in seen[2][-1]["content"]
    assert [e.type for e in events if e.type in {"verify", "verified", "unverified"}] == ["verify", "verify", "unverified"]


def test_passing_tests_accept_the_result_and_untouched_runs_skip_them(tmp_path):
    events, runs = [], []

    class Provider(agent.Provider):
        def __init__(self, edit):
            self.turn = 0
            self.edit = edit

        async def complete(self, messages, tools, cancellation=None):
            self.turn += 1
            if self.turn == 1 and self.edit:
                return agent.ChatResponse(tool_calls=[agent.ToolCall("1", "write_file", {"path": "a.py", "content": "x = 1\n"})])
            return agent.ChatResponse(content="Done.")

    async def verifier():
        runs.append(1)
        return "(exit code 0)\n3 passed"

    assert asyncio.run(_loop(tmp_path, Provider(True), verifier, events).run("edit")) == "Done."
    assert runs == [1]
    assert [e.type for e in events if e.type in {"verify", "verified", "nudge"}] == ["verify", "verified"]
    events.clear()
    assert asyncio.run(_loop(tmp_path, Provider(False), verifier, events).run("just answer")) == "Done."
    assert runs == [1]  # nothing changed, nothing to verify


# --- cache junk is not work -----------------------------------------------------

def test_cache_junk_never_counts_as_unfinished_work(tmp_path):
    assert agent.is_junk_path("calc/__pycache__/") and agent.is_junk_path("a/b.pyc") and agent.is_junk_path(".pytest_cache/")
    assert not agent.is_junk_path("calc/cache.py") and not agent.is_junk_path("pycache_notes.md")
    status = "(exit code 0)\n## master\n?? calc/__pycache__/\n?? tests/__pycache__/\n M calc/real.py\n?? new.py"
    assert agent.worktree_changes(status) == [" M calc/real.py", "?? new.py"]
    env = agent.CommandExecutor._command_environment(str(tmp_path / "home"))
    assert env["PYTHONDONTWRITEBYTECODE"] == "1"

    events = []

    class Provider(agent.Provider):
        def __init__(self):
            self.turn = 0

        async def complete(self, messages, tools, cancellation=None):
            self.turn += 1
            if self.turn == 1:
                return agent.ChatResponse(tool_calls=[agent.ToolCall("1", "list_directory", {"path": "."})])
            return agent.ChatResponse(content="Done.")

    class Git:
        def status(self):
            return "(exit code 0)\n## master\n?? calc/__pycache__/"

    config = agent.AgentConfig(provider="local-llama", model="m", streaming=False)
    memory = agent.Memory()
    registry = agent.build_tool_registry(tmp_path, config, memory)
    runner = agent.Agent(Provider(), agent.Planner(Git()), agent.Session(memory=memory), registry, agent.PromptBuilder(), config, on_event=events.append)
    assert asyncio.run(runner.run("task")) == "Done."
    assert not [e for e in events if e.type == "nudge"]


def test_git_commit_stage_all_skips_cache_junk(tmp_path):
    subprocess.run(["git", "init", "-q", str(tmp_path)], check=True)
    subprocess.run(["git", "-C", str(tmp_path), "config", "user.email", "t@example.com"], check=True)
    subprocess.run(["git", "-C", str(tmp_path), "config", "user.name", "T"], check=True)
    (tmp_path / "pkg").mkdir()
    (tmp_path / "pkg" / "mod.py").write_text("x = 1\n", encoding="utf-8")
    (tmp_path / "pkg" / "__pycache__").mkdir()
    (tmp_path / "pkg" / "__pycache__" / "mod.cpython-313.pyc").write_bytes(b"\x00")
    (tmp_path / ".pytest_cache").mkdir()
    (tmp_path / ".pytest_cache" / "v").write_text("", encoding="utf-8")
    registry = agent.build_tool_registry(tmp_path, agent.AgentConfig(provider="local-llama", model="m"), agent.Memory())
    result = asyncio.run(registry.execute(agent.ToolCall("1", "git_commit", {"message": "add mod"})))
    assert "add mod" in result
    tracked = subprocess.run(["git", "-C", str(tmp_path), "ls-files"], capture_output=True, text=True, check=True).stdout.split()
    assert tracked == ["pkg/mod.py"]
