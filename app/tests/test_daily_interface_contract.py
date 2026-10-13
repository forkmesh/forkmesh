import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BUDGETS = ROOT / "www/docs/engineering/daily-interface-budgets.json"
QUALITY = ROOT / "www/docs/engineering/quality-gates.json"
SHELL = ROOT / "app/public/dashboard/shell.html"
INTERACTION = ROOT / "app/public/dashboard/js/09-daily-interface.js"
WORKFLOW = ROOT / ".github/workflows/world-browser-e2e.yml"


def test_daily_budget_matrix_is_complete_and_positive():
    config = json.loads(BUDGETS.read_text(encoding="utf-8"))
    assert config["schemaVersion"] == 1
    rows = {row["id"]: row for row in config["budgets"]}
    assert set(rows) == {
        "daily-startup",
        "daily-repository-browse",
        "daily-search",
        "daily-diff-render",
        "daily-navigation",
        "daily-feedback",
    }
    for row in rows.values():
        assert 0 < row["localMaximum"] <= row["remoteMaximum"]
        assert row["measure"].strip()
    assert config["accessibility"] == {
        "criticalViolations": 0,
        "seriousViolations": 0,
        "minimumTextContrast": 4.5,
        "minimumLargeTextContrast": 3.0,
        "minimumCoarsePointerTargetCssPixels": 44,
        "supportedZoomPercent": 200,
        "reducedMotionMaximumAnimationMilliseconds": 0,
    }


def test_runtime_and_central_gate_register_every_daily_budget():
    budget_config = json.loads(BUDGETS.read_text(encoding="utf-8"))
    quality_config = json.loads(QUALITY.read_text(encoding="utf-8"))
    runtime = INTERACTION.read_text(encoding="utf-8")
    registered = {row["id"]: row for row in quality_config["performanceBudgets"]}
    runtime_names = {
        "daily-startup": "startup",
        "daily-repository-browse": "browse",
        "daily-search": "search",
        "daily-diff-render": "diff",
        "daily-navigation": "navigation",
        "daily-feedback": "feedback",
    }
    for row in budget_config["budgets"]:
        assert row["id"] in registered
        assert registered[row["id"]]["maximum"] == row["remoteMaximum"]
        values = (
            f'{runtime_names[row["id"]]}: Object.freeze('
            f'{{ local: {row["localMaximum"]}, remote: {row["remoteMaximum"]} }})'
        )
        assert values in runtime


def test_shared_accessibility_contract_is_authored_once_for_every_page():
    shell = SHELL.read_text(encoding="utf-8")
    interaction = INTERACTION.read_text(encoding="utf-8")
    assert 'href="#dashboardMain">Skip to main content</a>' in shell
    assert ":focus-visible" in shell
    assert "@media (prefers-reduced-motion: reduce)" in shell
    assert "@media (forced-colors: active)" in shell
    assert "@media (pointer: coarse)" in shell
    assert 'palette.setAttribute("role", "dialog")' in interaction
    assert 'status.setAttribute("aria-live", "polite")' in interaction
    assert 'event.key.toLowerCase() === "k"' in interaction


def test_browser_budget_and_accessibility_journey_blocks_ci():
    workflow = WORKFLOW.read_text(encoding="utf-8")
    assert "npm run test:daily-interface" in workflow
    assert "daily-interface-report" in workflow
