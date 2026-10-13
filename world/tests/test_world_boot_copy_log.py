import json
import shutil
import subprocess
from pathlib import Path

import pytest

WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
BOOT = WORLD / "world-boot.js"
APP = (WORLD / "world.js").read_text(encoding="utf-8")
NODE = shutil.which("node")


@pytest.mark.skipif(NODE is None, reason="node is required")
def test_copied_log_has_the_audit_section_and_ends_with_the_agent_note():
    script = """
      const boot = await import(%s);
      const log = boot.createWorldBootLog({ now: () => 5 });
      log.add("boot", "Load log started");
      for (let index = 0; index < 140; index += 1) log.add("event", `line ${index}`);
      const tips = [{ level: "caution", title: "Slow scene", detail: "trim it" }];
      const full = boot.worldBootCopyText(
        log.text({ summary: ["World load summary · interactive 1.10 s", ""], suggestions: tips }),
        "## Scene audit counts\\n12 duplicate geometries",
      );
      const pending = boot.worldBootCopyText(log.text(), "");
      console.log(JSON.stringify({ full, pending, note: boot.WORLD_BOOT_AGENT_NOTE }));
    """ % json.dumps(BOOT.as_uri())
    result = subprocess.run(
        [NODE, "--input-type=module", "-e", script],
        check=True, capture_output=True, text=True, timeout=60,
    )
    data = json.loads(result.stdout)
    full = data["full"]
    assert full.endswith(data["note"])
    assert data["note"].startswith("Note to the agent reading this log:")
    assert "Suggestions:" not in full
    assert full.startswith("World load summary · interactive 1.10 s\n\nOptimization suggestions\n- [caution] Slow scene — trim it\n")
    assert full.index("Optimization suggestions") < full.index("Load log started") < full.index("line 139")
    assert full.index("line 139") < full.index("=== Optimization audit ===")
    assert full.index("=== Optimization audit ===") < full.index("12 duplicate geometries")
    assert full.index("12 duplicate geometries") < full.index(data["note"])
    assert "Optimization audit: pending" in data["pending"]
    assert data["pending"].endswith(data["note"])


def test_the_world_feeds_the_optimize_report_into_the_copied_log():
    assert "WORLD_BOOT?.setReportProvider(() => this.perfCopyReport());" in APP
    copy = APP.split("  perfCopyReport() {", 1)[1].split("\n  }\n", 1)[0]
    assert "this.runPerfAudit();" in copy
    assert "this.perfReportMarkdown({ logTail: false })" in copy
    report = APP.split("  perfReportMarkdown({ logTail = true } = {}) {", 1)[1].split(
        "\n  async copyPerfReport", 1)[0]
    for piece in (
        "## Scene audit counts",
        "duplicate geometries",
        "duplicate materials",
        "instancing candidates",
        "culling off",
        "GPU textures",
        "programs",
        "- Evidence:",
        "- Fix:",
        "items.slice(0, 20)",
        "more`",
    ):
        assert piece in report, piece
