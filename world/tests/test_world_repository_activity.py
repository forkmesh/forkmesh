"""In-world repository commit activity display contracts."""

from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
WORLD_JS = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(
    encoding="utf-8"
)
SCENE_JS = world_scene_source()
GATEWAY = (ROOT.parent / "tools" / "mirror_gateway.py").read_text(
    encoding="utf-8"
)


def test_world_reads_commit_activity_at_the_selected_immutable_ref():
    for marker in (
        "function normalizeRepositoryCommitActivity(",
        "function repositoryActivityWindowStarts(",
        "const historyRequest = this.fetchJSON(`${base}/history${ref}`",
        "immutableGitOid(historyResult.value?.commit) === commit",
        "commitActivity: normalizeRepositoryCommitActivity(",
        "this.world.updateRepositoryActivity?.(active.commitActivity",
    ):
        assert marker in WORLD_JS


def test_world_activity_chart_matches_the_web_52_week_language_and_rates():
    for marker in (
        "function repositoryCommitActivityTexture(",
        "COMMIT ACTIVITY · ${subtitle}",
        '"52 WEEKS AGO"',
        '"THIS WEEK"',
        '"THIS MONTH"',
        "commitsPerHour",
        "repository-commit-activity-chart",
        "repository-commit-activity-pedestal",
        "backing.rotation.x = -Math.PI / 4",
        "function updateRepositoryActivity(activity = {}, selection = {})",
        "updateRepositoryActivity,",
    ):
        assert marker in SCENE_JS


def test_mirror_history_reports_exact_utc_activity_windows():
    for marker in (
        '"rev-list",',
        '"--count",',
        '"--format=%ct"',
        "stamps.append(int(row))",
        "sum(1 for stamp in stamps if stamp >= start_seconds)",
        '"timezone": "UTC"',
        '"weekStartsOn": "monday"',
        '"today": int(today_start.timestamp())',
        '"week": week_start_seconds',
        '"month": int(month_start.timestamp())',
    ):
        assert marker in GATEWAY
