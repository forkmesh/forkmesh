import importlib.util
import json
import subprocess
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "quality_gate.py"
CONFIG = ROOT / "www" / "docs" / "engineering" / "quality-gates.json"


def _module():
    spec = importlib.util.spec_from_file_location("forkmesh_quality_gate", TOOL)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_quality_gate_config_validates_from_repository_root():
    result = subprocess.run(
        [sys.executable, str(TOOL), "validate"],
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr
    assert "configuration is valid" in result.stdout


def test_pull_request_evidence_rejects_placeholders_and_accepts_explanations():
    module = _module()
    config = json.loads(CONFIG.read_text(encoding="utf-8"))
    placeholder = "\n".join(
        f"### {item['heading']}\n\n<!-- still empty -->"
        for item in config["requiredPullRequestEvidence"]
    )
    assert module.pull_request_evidence_errors(placeholder, config) == [
        item["heading"] for item in config["requiredPullRequestEvidence"]
    ]

    complete = "\n".join(
        f"### {item['heading']}\n\nNot applicable — documentation-only change."
        for item in config["requiredPullRequestEvidence"]
    )
    assert module.pull_request_evidence_errors(complete, config) == []


def test_budget_increase_requires_a_current_matching_waiver(monkeypatch, tmp_path):
    module = _module()
    current = json.loads(CONFIG.read_text(encoding="utf-8"))
    prior = json.loads(CONFIG.read_text(encoding="utf-8"))
    current["performanceBudgets"][0]["maximum"] = 0.75
    monkeypatch.setattr(module, "ROOT", tmp_path)
    for budget in prior["performanceBudgets"]:
        enforcing_path_text, test_name = budget["enforcedBy"].split("::", 1)
        enforcing_path = tmp_path / enforcing_path_text
        enforcing_path.parent.mkdir(parents=True, exist_ok=True)
        enforcing_path.write_text(
            f"def {test_name}():\n    pass\n", encoding="utf-8")

    with pytest.raises(module.GateError, match="weakening requires"):
        module.compare_budgets(prior, current)


def test_quality_gate_config_rejects_a_stale_enforcing_test_reference():
    module = _module()
    config = json.loads(CONFIG.read_text(encoding="utf-8"))
    config["performanceBudgets"][0]["enforcedBy"] = (
        "app/tests/test_perf_budgets.py::test_removed_budget")

    with pytest.raises(module.GateError, match="enforcing test does not exist"):
        module.validate_config(config)


def test_javascript_budget_reference_requires_a_real_playwright_test(
        monkeypatch, tmp_path):
    module = _module()
    monkeypatch.setattr(module, "ROOT", tmp_path)
    test_path = tmp_path / "app" / "browser_tests" / "daily-interface.spec.js"
    test_path.parent.mkdir(parents=True)
    test_path.write_text(
        """
// test("commented budget", async () => {});
/* test("removed budget", async () => {}); */
test("daily interface search stays within budget", async ({ page }) => {
  await page.goto("https://forkmesh.test/dashboard/repos");
});
""",
        encoding="utf-8",
    )
    budget = {
        "id": "daily-search",
        "enforcedBy": (
            "app/browser_tests/daily-interface.spec.js::"
            "daily interface search stays within budget"
        ),
    }

    module._validate_enforcing_test(budget)

    for stale_title in ("commented budget", "removed budget", "renamed budget"):
        budget["enforcedBy"] = (
            f"app/browser_tests/daily-interface.spec.js::{stale_title}")
        with pytest.raises(
                module.GateError, match="enforcing test does not exist"):
            module._validate_enforcing_test(budget)


def test_all_gate_commands_are_argument_arrays_without_shell_execution():
    config = json.loads(CONFIG.read_text(encoding="utf-8"))
    for gate in config["gates"]:
        assert isinstance(gate["command"], list)
        assert gate["command"]
        assert all(isinstance(part, str) and part for part in gate["command"])


def test_run_profile_runs_every_gate_and_annotates_each_failure(
        monkeypatch, capsys):
    module = _module()
    config = {
        "gates": [
            {"id": "first", "command": ["{python}", "-c",
                                        "print('boom: 50% done'); raise SystemExit(3)"]},
            {"id": "second", "command": ["{python}", "-c", "print('fine')"]},
            {"id": "third", "command": ["{python}", "-c",
                                        "import sys; sys.stderr.write('bad\\n'); sys.exit(1)"]},
        ],
        "profiles": {"core": ["first", "second", "third"]},
    }
    monkeypatch.setenv("GITHUB_ACTIONS", "true")
    with pytest.raises(module.GateError) as error:
        module.run_profile(config, "core")
    assert str(error.value) == (
        "failed gates: first (exit code 3), third (exit code 1)")
    out = capsys.readouterr().out
    assert "fine" in out
    assert "::error title=Quality gate first failed::boom: 50%25 done" in out
    assert "::error title=Quality gate third failed::bad" in out


def test_annotation_keeps_failure_lines_that_scrolled_out_of_the_tail(capsys):
    module = _module()
    script = (
        "print('--- FAIL: TestCall (4.5s)')\n"
        "for n in range(80): print(f'ok  pkg{n}')\n"
        "raise SystemExit(1)")
    returncode, tail = module._run_gate(["python3", "-c", script])
    capsys.readouterr()
    assert returncode == 1
    assert tail[0] == "--- FAIL: TestCall (4.5s)"
    assert tail[1] == "..."
    assert tail[-1] == "ok  pkg79"
    assert len(tail) == 2 + module.ANNOTATION_TAIL_LINES


def test_github_annotation_escapes_newlines_and_keeps_the_tail():
    module = _module()
    line = module.github_annotation(
        "go", 1, ["--- FAIL: TestCall", "x" * 5000, "FAIL\tpkg"])
    assert line.startswith("::error title=Quality gate go failed::")
    message = line[len("::error title=Quality gate go failed::"):]
    assert "\n" not in line
    assert message.endswith("%0AFAIL\tpkg")
    assert len(message) <= module.ANNOTATION_MAX_CHARS + 10
    assert module.github_annotation("go", 2, []).endswith("::exit code 2")
