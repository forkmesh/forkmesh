#!/usr/bin/env python3
"""Package AI stays host-bounded (allowlist, quota, JSON) and reminders ride cron."""

import ast
import asyncio
import json
from pathlib import Path
import sys


APP = Path(__file__).resolve().parents[1]
ENTRY = APP / "src" / "entry.py"
sys.path.insert(0, str(APP / "src"))


def _functions(*names, **namespace):
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    wanted = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Assign))
        and (getattr(node, "name", "") in names or (
            isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id.startswith("PLUGIN_AI_")
                for target in node.targets)))
    ]
    exec(compile(ast.Module(body=wanted, type_ignores=[]), str(ENTRY), "exec"), namespace)
    return namespace


class _Limited:
    async def text(self):
        return json.dumps({"error": "rate_limited", "retryAfterMs": 4200})


class _AI:
    def __init__(self, answers):
        self.answers = list(answers)
        self.calls = []

    async def run(self, model, payload):
        self.calls.append((model, payload))
        answer = self.answers.pop(0)
        if isinstance(answer, Exception):
            raise answer
        return answer


class _Env:
    def __init__(self, ai):
        self.AI = ai


def _harness(ai, limited=None):
    logs = []

    async def rate(env, key):
        logs.append(("rate", key))
        return limited

    async def log_error(env, status, method, path, message, **kwargs):
        logs.append(("log", path, message))

    import re
    namespace = _functions(
        "_plugin_ai_text", "_plugin_ai_json",
        re=re, json=json, _ai_ask_rate_check=rate, js_nullish=lambda v: v is None,
        log_error=log_error, clean_string=lambda v, n: str(v or "")[:n],
        to_js=lambda v: v, _forkbot_ai_fallback_models=lambda env: ["@cf/text/a", "@cf/text/b"],
        _forkbot_ai_model_not_found_error=lambda e: "no such model" in str(e).lower(),
        _forkbot_json_object_from_text=_json_from_text, _safe_error_text=str,
    )
    return namespace, logs


def _json_from_text(text):
    if not isinstance(text, str):
        return None
    start, end = text.find("{"), text.rfind("}")
    if start < 0 or end <= start:
        return None
    try:
        parsed = json.loads(text[start:end + 1])
    except ValueError:
        return None
    return parsed if isinstance(parsed, dict) else None


def test_vision_calls_use_the_vision_allowlist_json_mode_first_and_quota_key():
    ai = _AI([{"response": {"items": [{"name": "apple", "calories": 95}]}}])
    namespace, logs = _harness(ai)
    image = "data:image/jpeg;base64,QUJD"
    result = asyncio.run(namespace["_plugin_ai_json"](
        _Env(ai), "bi:alice", "system", "what is this", {"type": "object"},
        image_data_url=image, max_tokens=5000))
    assert result == {"ok": True, "data": {"items": [{"name": "apple", "calories": 95}]},
                      "model": "@cf/meta/llama-4-scout-17b-16e-instruct"}
    assert logs == [("rate", "bi:alice:plugin-ai")]
    model, payload = ai.calls[0]
    assert payload["response_format"]["type"] == "json_schema"
    assert payload["max_tokens"] == namespace["PLUGIN_AI_MAX_TOKENS"]
    user = payload["messages"][1]["content"]
    assert user[0] == {"type": "text", "text": "what is this"}
    assert user[1]["image_url"]["url"] == image


def test_invalid_images_and_quota_never_reach_the_binding():
    ai = _AI([])
    namespace, logs = _harness(ai)
    bad = asyncio.run(namespace["_plugin_ai_json"](
        _Env(ai), "bi:alice", "s", "u", None, image_data_url="data:text/html;base64,QQ=="))
    assert bad == {"ok": False, "error": "invalid_image"} and not ai.calls
    namespace, logs = _harness(ai, limited=_Limited())
    limited = asyncio.run(namespace["_plugin_ai_json"](_Env(ai), "bi:alice", "s", "u", None))
    assert limited == {"ok": False, "error": "rate_limited", "retryAfterMs": 4200}
    assert not ai.calls
    namespace, logs = _harness(ai)
    missing = asyncio.run(namespace["_plugin_ai_json"](_Env(None), "bi:alice", "s", "u", None))
    assert missing == {"ok": False, "error": "missing_binding"}
    assert not any(entry[1] == "plugin/ai" for entry in logs)


def test_text_calls_fall_back_across_models_and_plain_prompts_and_parse_json_text():
    ai = _AI([
        Exception("5007: No such model @cf/text/a"),
        Exception("json mode unsupported"),
        {"choices": [{"message": {"content": [{"type": "text", "text": "Sure: {\"kcal\": 12}"}]}}]},
    ])
    namespace, logs = _harness(ai)
    result = asyncio.run(namespace["_plugin_ai_json"](
        _Env(ai), "bi:bob", "s", "two eggs", {"type": "object"}))
    assert result == {"ok": True, "data": {"kcal": 12}, "model": "@cf/text/b"}
    assert [call[0] for call in ai.calls] == ["@cf/text/a", "@cf/text/b", "@cf/text/b"]
    assert "response_format" in ai.calls[1][1] and "response_format" not in ai.calls[2][1]
    assert ai.calls[2][1]["messages"][1]["content"] == "two eggs"

    ai = _AI([{"response": "no json here"}, {"response": "still none"}, "nope", "nada"])
    namespace, logs = _harness(ai)
    result = asyncio.run(namespace["_plugin_ai_json"](_Env(ai), "bi:bob", "s", "u", {"type": "object"}))
    assert result == {"ok": False, "error": "unusable_response"}
    assert logs[-1][2].endswith("no usable JSON answer")


def test_plugin_data_runs_every_reminder_hook_and_tolerates_dropped_schemas():
    import plugin_data

    calls = []

    class _Quiet:
        pass

    class _Rings:
        @staticmethod
        async def deliver_reminders(runtime):
            calls.append(runtime)
            return {"ok": True, "delivered": 2}

    class _Dropped:
        @staticmethod
        async def deliver_reminders(runtime):
            raise RuntimeError("D1_ERROR: no such table: calorie_profiles")

    registry = {"a-quiet": {"module": "q"}, "b-rings": {"module": "r"}, "c-dropped": {"module": "d"}}
    modules = {"a-quiet": _Quiet, "b-rings": _Rings, "c-dropped": _Dropped}
    original = (plugin_data.REGISTRY, plugin_data._data_module)
    plugin_data.REGISTRY = registry
    plugin_data._data_module = lambda plugin: modules[plugin]
    try:
        result = asyncio.run(plugin_data.deliver_reminders("runtime"))
    finally:
        plugin_data.REGISTRY, plugin_data._data_module = original
    assert result == {"ok": True, "delivered": 2} and calls == ["runtime"]


def test_cron_rings_package_reminders_after_calendar_reminders():
    source = ENTRY.read_text(encoding="utf-8")
    phase = source.split('if phase == "status-alert-pings":', 1)[1].split('if phase == "status-alert-email"', 1)[0]
    assert "await deliver_calendar_reminders(env)" in phase
    assert "await deliver_plugin_reminders(env)" in phase
    assert phase.index("deliver_calendar_reminders") < phase.index("deliver_plugin_reminders")
    assert "async def ai_json(self, account_bi, system_prompt, user_prompt, schema," in source


def test_package_answers_for_an_absent_ai_provider_are_expected_degraded():
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    runtime = next(node for node in tree.body
                   if isinstance(node, ast.ClassDef)
                   and node.name == "_PluginApiRuntime")
    method = next(node for node in runtime.body
                  if isinstance(node, ast.FunctionDef)
                  and node.name == "response")
    calls = []

    class _Base:
        def response(self, data, status=200, cache_control=None,
                     extra_headers=None):
            calls.append((status, extra_headers))

    namespace = _functions(
        EXPECTED_DEGRADED_HEADERS={"x-forkmesh-expected-degraded": "1"})
    namespace["_Base"] = _Base
    klass = ast.ClassDef(
        name="_Runtime", bases=[ast.Name("_Base", ast.Load())], keywords=[],
        body=[method], decorator_list=[], type_params=[])
    module = ast.fix_missing_locations(ast.Module(body=[klass], type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    runtime = namespace["_Runtime"]()
    runtime.response({"error": "missing_binding"}, 503,
                     extra_headers={"allow": "POST"})
    runtime.response({"error": "ai_unavailable"}, 503)
    runtime.response({"error": "provider_error"}, 502)
    runtime.response({"error": "missing_binding"}, 400)
    assert calls == [
        (503, {"allow": "POST", "x-forkmesh-expected-degraded": "1"}),
        (503, {"x-forkmesh-expected-degraded": "1"}),
        (502, None),
        (400, None),
    ]


def _reminder_wrappers(fail):
    import contextvars
    import types

    logs = []

    async def ensure_schema(env):
        return None

    async def log_error(env, status, method, path, message, **kwargs):
        logs.append((path, message))

    async def hook(*args):
        raise fail

    namespace = _functions(
        "_cron_budget_exhausted", "deliver_plugin_reminders",
        "deliver_task_persistent_reminders",
        ensure_schema=ensure_schema, log_error=log_error, _safe_error_text=str,
        _CRON_D1_BUDGET_VAR=contextvars.ContextVar("budget", default=None),
        plugin_data=types.SimpleNamespace(deliver_reminders=hook),
        world_office_tasks=types.SimpleNamespace(deliver_persistent_reminders=hook),
        _PluginApiRuntime=lambda env, request: None,
        _OfficeMarketingTasksRuntime=lambda env, request: None,
        Date=types.SimpleNamespace(now=lambda: 0),
    )
    return namespace, logs


def test_reminder_wrappers_hand_an_exhausted_cron_budget_back_to_the_runner():
    fail = RuntimeError("cron phase subrequest budget exhausted")
    namespace, logs = _reminder_wrappers(fail)
    namespace["_CRON_D1_BUDGET_VAR"].set({"used": 50, "exhausted": True})
    for name in ("deliver_plugin_reminders", "deliver_task_persistent_reminders"):
        try:
            asyncio.run(namespace[name]("env"))
        except RuntimeError as error:
            assert error is fail
        else:
            raise AssertionError(name + " swallowed the exhausted budget")
    assert logs == []


def test_reminder_wrappers_still_log_a_real_package_failure():
    namespace, logs = _reminder_wrappers(RuntimeError("boom"))
    namespace["_CRON_D1_BUDGET_VAR"].set({"used": 3, "exhausted": False})
    assert asyncio.run(namespace["deliver_plugin_reminders"]("env")) == {"ok": False}
    assert asyncio.run(namespace["deliver_task_persistent_reminders"]("env")) == {"ok": False}
    assert [path for path, _ in logs] == ["plugins/reminders", "organization/tasks/reminders"]
