#!/usr/bin/env python3
"""Focused tests for coding-agent repository directory guardrails.

Run: python3 tools/test_forkmesh_coding_agent_guardrails.py
"""

from __future__ import annotations

import asyncio
import importlib.util
import os
import sys
import tempfile
from pathlib import Path


HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location(
    "forkmesh_coding_agent_under_test", HERE / "forkmesh_coding_agent.py"
)
assert SPEC and SPEC.loader
agent = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = agent
SPEC.loader.exec_module(agent)

failures = 0


def check(condition: bool, message: str) -> None:
    global failures
    print(("ok   " if condition else "FAIL ") + message)
    if not condition:
        failures += 1


async def exercise_registry(root: Path) -> None:
    policy = agent.RepositoryAccessPolicy(root, ["src"], enabled=True)
    calls: list[str] = []
    registry = agent.ToolRegistry(policy)
    registry.register(
        agent.RegisteredTool(
            "read",
            "test reader",
            {
                "type": "object",
                "properties": {"path": {"type": "string"}},
                "required": ["path"],
            },
            lambda arguments: calls.append(arguments["path"]) or "read",
        )
    )

    allowed = await registry.execute(
        agent.ToolCall("allowed", "read", {"path": "src/ok.txt"})
    )
    denied = await registry.execute(
        agent.ToolCall("denied", "read", {"path": "private/secret.txt"})
    )
    check(allowed == "read" and calls == ["src/ok.txt"],
          "an allowed structured tool call reaches its handler")
    check("Guardrail blocked" in denied and calls == ["src/ok.txt"],
          "a denied structured tool call is stopped before its handler")


def main() -> int:
    names = ("FORKMESH_AGENT_ALLOWED_DIRECTORIES", "FORKMESH_AGENT_GUARDRAILS")
    prior = {name: os.environ.get(name) for name in names}
    try:
        for name in names:
            os.environ.pop(name, None)
        config = agent.AgentConfig.from_env()
        check(config.guardrails_enabled and config.allowed_directories == ["."],
              "guardrails default to the repository root")
        os.environ["FORKMESH_AGENT_GUARDRAILS"] = "off"
        config = agent.AgentConfig.from_env()
        check(not config.guardrails_enabled,
              "FORKMESH_AGENT_GUARDRAILS=off turns the guardrails off")
        os.environ["FORKMESH_AGENT_ALLOWED_DIRECTORIES"] = '["src"]'
        config = agent.AgentConfig.from_env()
        check(config.guardrails_enabled and config.allowed_directories == ["src"],
              "the desktop allowlist environment enables tool-call guardrails")
    finally:
        for name, value in prior.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value

    with tempfile.TemporaryDirectory(prefix="forkmesh-agent-guard-") as tmp:
        root = Path(tmp) / "repo"
        (root / "src").mkdir(parents=True)
        (root / "private").mkdir()
        outside = Path(tmp) / "outside"
        outside.mkdir()
        (root / "src" / "ok.txt").write_text("ok", encoding="utf-8")
        (root / "private" / "secret.txt").write_text("secret", encoding="utf-8")
        (root / "src" / "escape").symlink_to(outside, target_is_directory=True)

        policy = agent.RepositoryAccessPolicy(root, ["src"], enabled=True)
        check(policy.resolve("src/ok.txt") == root / "src" / "ok.txt",
              "an allowed repository path resolves normally")
        for path, description in (
            ("private/secret.txt", "a sibling repository directory is denied"),
            ("../outside", "a lexical repository escape is denied"),
            ("src/escape/file.txt", "a symlink repository escape is denied"),
        ):
            try:
                policy.resolve(path)
            except (PermissionError, ValueError):
                check(True, description)
            else:
                check(False, description)

        asyncio.run(exercise_registry(root))

    print("\n%d guardrail test(s) failed." % failures if failures else
          "\nAll coding-agent guardrail tests passed.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
