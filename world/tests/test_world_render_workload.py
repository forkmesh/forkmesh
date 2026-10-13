#!/usr/bin/env python3
"""A hardware-independent completion budget for issue #564."""

import json
from pathlib import Path
import subprocess
from world_scene_source import world_scene_source


WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
SCENE_MODULE = WORLD / "world-scene.js"
SCENE = world_scene_source()
BROWSER = (
    Path(__file__).resolve().parents[1]
    / "browser_tests"
    / "tests"
    / "world.spec.js"
).read_text(encoding="utf-8")


def run_scene_script(body):
    script = f"""
      import * as scene from {json.dumps(SCENE_MODULE.as_uri())};
      {body}
    """
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    return json.loads(result.stdout)


def test_workload_budget_has_exact_inclusive_boundaries():
    observed = run_scene_script(
        """
        const budget = scene.WORLD_RENDER_WORKLOAD_BUDGET;
        console.log(JSON.stringify({
          budget,
          empty: scene.worldRenderWorkloadStatus(),
          exact: scene.worldRenderWorkloadStatus(
            budget.drawCalls,
            budget.triangles,
          ),
          callsOver: scene.worldRenderWorkloadStatus(
            budget.drawCalls + 1,
            budget.triangles,
          ),
          trianglesOver: scene.worldRenderWorkloadStatus(
            budget.drawCalls,
            budget.triangles + 1,
          ),
        }));
        """
    )
    assert observed["budget"] == {"drawCalls": 450, "triangles": 750_000}
    assert observed["empty"]["withinBudget"] is True
    assert observed["exact"]["withinBudget"] is True
    assert observed["callsOver"]["withinBudget"] is False
    assert observed["trianglesOver"]["withinBudget"] is False


def test_critical_browser_suite_measures_real_default_town_workload():
    assert (
        'test("@critical default town stays inside the renderer workload budget"'
        in BROWSER
    )
    for contract in (
        'await page.keyboard.down("KeyW");',
        "diagnostics.renderWorkload",
        'testInfo.attach("world-render-workload.json"',
        "WORLD_RENDER_DRAW_CALL_BUDGET = 450",
        "WORLD_RENDER_TRIANGLE_BUDGET = 750_000",
        "expect(measured.withinBudget).toBe(true);",
    ):
        assert contract in BROWSER
    assert "renderWorkload: worldRenderWorkloadStatus(" in SCENE
    assert "WORLD_RENDER_WORKLOAD_BUDGET.drawCalls" in SCENE
    assert "WORLD_RENDER_WORKLOAD_BUDGET.triangles" in SCENE
