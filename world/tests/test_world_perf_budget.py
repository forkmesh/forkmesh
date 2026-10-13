import importlib.util
import json
from pathlib import Path

import pytest

REPOSITORY = Path(__file__).resolve().parents[2]
MODULE = REPOSITORY / "tools" / "world_bench.py"
BUDGET = REPOSITORY / "world" / "perf-budget.json"

spec = importlib.util.spec_from_file_location("world_bench", MODULE)
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)

EXPECTED_POSES = {
    "spawn",
    "office-lobby",
    "office-rooftop",
    "office-engineering",
    "beach",
    "members-circle",
    "arrival-stats",
    "repository-portals",
    "town-overview",
}


def pose_result(draws=100, triangles=5000, programs=50, geometries=40, textures=60,
                p50=5.0, p90=8.0, first=1500.0, interactive=4000.0, long_tasks=6):
    return {
        "counts": {
            "draws": draws,
            "triangles": triangles,
            "programs": programs,
            "geometries": geometries,
            "textures": textures,
        },
        "live": {"frames": 200, "renderMsP50": p50, "renderMsP90": p90},
        "load": {
            "firstFrameMs": first,
            "interactiveMs": interactive,
            "interactiveFrameMs": interactive + 200,
            "loadLongTasks": long_tasks,
        },
        "golden": {"changedPct": 0, "maxDelta": 0, "sameSize": True},
    }


def report(poses, gpu="GPU A"):
    return {"gpu": gpu, "poses": poses}


def test_pose_table_covers_every_named_pose_with_a_camera():
    assert set(bench.POSE_NAMES) == EXPECTED_POSES
    assert len(bench.POSE_NAMES) == len(set(bench.POSE_NAMES))
    for pose in bench.POSES:
        assert len(pose["camera"]["position"]) == 3
        assert len(pose["camera"]["target"]) == 3
        assert pose["place"]["kind"] in {"town", "office", "beach"}


def test_time_ceiling_takes_the_larger_of_factor_and_absolute_slack():
    assert bench.ceiling_for("renderMsP50", 2.0) == 7.0
    assert bench.ceiling_for("renderMsP90", 10.0) == 20.0
    assert bench.ceiling_for("firstFrameMs", 1500) == 3000.0
    assert bench.ceiling_for("renderMsP50", 0) == 5.0


def test_time_ceiling_follows_a_custom_policy():
    policy = {"timeFactor": 1.5, "timeAbsoluteMs": 2.0}
    assert bench.ceiling_for("renderMsP50", 10.0, policy) == 15.0
    assert bench.ceiling_for("renderMsP50", 1.0, policy) == 3.0


def test_exact_counts_have_no_slack():
    assert bench.ceiling_for("draws", 541) == 541
    assert bench.ceiling_for("triangles", 114690) == 114690


def test_noisy_counts_get_small_slack_rounded_up():
    assert bench.ceiling_for("programs", 100) == 150
    assert bench.ceiling_for("programs", 10) == 26
    assert bench.ceiling_for("geometries", 400) == 412
    assert bench.ceiling_for("textures", 1000) == 1150
    assert bench.ceiling_for("loadLongTasks", 3) == 11
    assert isinstance(bench.ceiling_for("geometries", 401), int)


def test_count_policy_override_merges_with_defaults():
    policy = {"counts": {"draws": {"absolute": 3}}}
    assert bench.ceiling_for("draws", 100, policy) == 103
    assert bench.ceiling_for("triangles", 100, policy) == 100


def test_merge_takes_the_maximum_of_each_metric_across_runs():
    first = report({"spawn": pose_result(draws=100, p50=5.0, programs=60)})
    second = report({"spawn": pose_result(draws=98, p50=7.5, programs=55),
                     "beach": pose_result(draws=30)})
    merged = bench.merge_reports([first, second])
    assert merged["spawn"]["draws"] == 100
    assert merged["spawn"]["renderMsP50"] == 7.5
    assert merged["spawn"]["programs"] == 60
    assert merged["beach"]["draws"] == 30


def test_failed_pose_contributes_no_metrics():
    failed = report({"spawn": {"failed": "Timeout", "counts": {"draws": 9999}}})
    assert bench.merge_reports([failed]) == {"spawn": {}}


def test_live_timings_are_ignored_without_frames():
    result = pose_result()
    result["live"]["frames"] = 0
    metrics = bench.pose_metrics(result)
    assert "renderMsP50" not in metrics
    assert metrics["draws"] == 100


def test_build_budget_records_baseline_and_ceiling_per_pose():
    merged = bench.merge_reports([report({"spawn": pose_result(), "beach": pose_result(draws=30)})])
    budget = bench.build_budget(merged, gpu="GPU A")
    assert budget["gpu"] == "GPU A"
    assert list(budget["poses"]) == ["spawn", "beach"]
    assert budget["poses"]["spawn"]["draws"] == {"baseline": 100, "ceiling": 100}
    assert budget["poses"]["spawn"]["renderMsP50"] == {"baseline": 5.0, "ceiling": 10.0}
    assert budget["golden"]["tolerance"] == 32


def test_update_baseline_keeps_the_previous_policy():
    previous = {"policy": {"timeFactor": 3.0, "timeAbsoluteMs": 1.0}, "golden": {"maxChangedPct": 1.0}}
    merged = bench.merge_reports([report({"spawn": pose_result(p50=4.0)})])
    budget = bench.build_budget(merged, previous=previous)
    assert budget["poses"]["spawn"]["renderMsP50"]["ceiling"] == 12.0
    assert budget["golden"]["maxChangedPct"] == 1.0


def test_check_passes_within_budget_and_names_pose_and_metric_when_over():
    base = bench.merge_reports([report({"spawn": pose_result(), "beach": pose_result(draws=30)})])
    budget = bench.build_budget(base, gpu="GPU A")
    same = bench.check_budget(base, budget, gpu="GPU A")
    assert not bench.gate_failed(same, [])
    worse = bench.merge_reports([report({"spawn": pose_result(draws=101, p90=30.0),
                                         "beach": pose_result(draws=30)})])
    findings = bench.check_budget(worse, budget, gpu="GPU A")
    over = {(item["pose"], item["metric"]) for item in findings if item["status"] == "over"}
    assert over == {("spawn", "draws"), ("spawn", "renderMsP90")}
    assert bench.gate_failed(findings, [])
    text = bench.format_findings(findings)
    assert "FAIL  spawn" in text
    assert "draws" in text and "101 > 100" in text
    assert "renderMsP90" in text and "30 > 16" in text
    assert "2 over" in text


def test_missing_pose_or_metric_fails_the_check():
    budget = bench.build_budget(bench.merge_reports([report({"spawn": pose_result()})]))
    findings = bench.check_budget({}, budget)
    assert findings == [{"pose": "spawn", "metric": "*", "status": "missing"}]
    assert bench.gate_failed(findings, [])


def test_timings_are_skipped_on_another_gpu_but_counts_still_checked():
    budget = bench.build_budget(bench.merge_reports([report({"spawn": pose_result()})]), gpu="GPU A")
    slower = bench.merge_reports([report({"spawn": pose_result(p50=90.0, draws=120)})])
    findings = bench.check_budget(slower, budget, gpu="GPU B")
    statuses = {item["metric"]: item["status"] for item in findings}
    assert statuses["renderMsP50"] == "skipped"
    assert statuses["draws"] == "over"
    software = bench.check_budget(slower, budget, gpu="GPU A", software=True)
    assert {item["status"] for item in software if item["metric"] in bench.TIME_METRICS} == {"skipped"}


def test_golden_findings_fail_only_on_real_change():
    changed = pose_result()
    changed["golden"] = {"changedPct": 3.2, "maxDelta": 200, "sameSize": True, "diff": "/tmp/x.diff.png"}
    noise = pose_result()
    noise["golden"] = {"changedPct": 0.1, "maxDelta": 40, "sameSize": True}
    missing = pose_result()
    missing["golden"] = {"missing": True, "baseline": "/tmp/none.png"}
    results = bench.golden_findings([report({"spawn": changed, "beach": noise, "office-lobby": missing})], 0.5)
    statuses = {item["pose"]: item["status"] for item in results}
    assert statuses == {"spawn": "changed", "beach": "ok", "office-lobby": "missing"}
    assert bench.gate_failed([], results)
    assert not bench.gate_failed([], [item for item in results if item["pose"] != "spawn"])
    text = bench.format_golden(results)
    assert "FAIL  golden spawn" in text and "/tmp/x.diff.png" in text


def test_resized_golden_counts_as_changed():
    result = pose_result()
    result["golden"] = {"changedPct": 0, "maxDelta": 0, "sameSize": False}
    assert bench.golden_findings([report({"spawn": result})], 0.5)[0]["status"] == "changed"


def test_cli_parses_repeated_reports_and_poses(tmp_path):
    args = bench.parse_args(["--report", "a.json", "--report", "b.json", "--pose", "spawn",
                             "--pose", "beach", "--check", "--budget", str(tmp_path / "b.json")])
    assert args.report == ["a.json", "b.json"]
    assert args.pose == ["spawn", "beach"]
    assert args.check and not args.gate
    assert args.budget == tmp_path / "b.json"
    defaults = bench.parse_args([])
    assert defaults.budget == bench.DEFAULT_BUDGET
    assert defaults.runs == 1


@pytest.mark.parametrize("argv", [
    ["--pose", "nowhere"],
    ["--runs", "0"],
    ["--write-budget", "--update-baseline"],
    ["--tighten", "--write-budget"],
    ["--bless", "--report", "a.json"],
])
def test_cli_rejects_invalid_combinations(argv):
    with pytest.raises(SystemExit):
        bench.parse_args(argv)


def test_cli_writes_budget_then_checks_reports_offline(tmp_path, capsys):
    first = tmp_path / "a.json"
    second = tmp_path / "b.json"
    first.write_text(json.dumps(report({"spawn": pose_result(p50=5.0)})))
    second.write_text(json.dumps(report({"spawn": pose_result(p50=6.0)})))
    budget_path = tmp_path / "budget.json"
    assert bench.main(["--report", str(first), "--report", str(second), "--write-budget",
                       "--budget", str(budget_path)]) == 0
    written = json.loads(budget_path.read_text())
    assert written["poses"]["spawn"]["renderMsP50"] == {"baseline": 6.0, "ceiling": 12.0}
    assert bench.main(["--report", str(first), "--check", "--budget", str(budget_path)]) == 0
    slow = tmp_path / "slow.json"
    slow.write_text(json.dumps(report({"spawn": pose_result(p50=20.0)})))
    assert bench.main(["--report", str(slow), "--check", "--budget", str(budget_path)]) == 1
    out = capsys.readouterr().out
    assert "FAIL  spawn" in out and "renderMsP50" in out


def test_audit_counts_are_budgeted_with_small_slack():
    result = pose_result()
    result["counts"].update(duplicateMaterials=272, frustumCulledOff=7)
    metrics = bench.pose_metrics(result)
    assert metrics["duplicateMaterials"] == 272
    assert bench.ceiling_for("duplicateMaterials", 272) == 286
    assert bench.ceiling_for("frustumCulledOff", 7) == 10


def test_noisy_members_circle_geometry_dedupe_is_reported_but_not_budgeted():
    result = pose_result()
    result["counts"]["duplicateGeometries"] = 448
    merged = bench.merge_reports([report({"members-circle": result, "spawn": result})])
    assert "duplicateGeometries" not in merged["members-circle"]
    assert merged["spawn"]["duplicateGeometries"] == 448
    budget = json.loads(BUDGET.read_text())
    assert "duplicateGeometries" not in budget["poses"]["members-circle"]


def test_tighten_lowers_only_ceilings_the_run_beat():
    budget = bench.build_budget(bench.merge_reports([report({"spawn": pose_result(draws=500, p50=10.0)})]),
                                gpu="GPU A")
    faster = bench.merge_reports([report({"spawn": pose_result(draws=300, p50=5.0)})])
    tightened, changes = bench.tighten_budget(budget, faster, gpu="GPU A")
    moved = {(item["metric"], item["from"], item["to"]) for item in changes}
    assert ("draws", 500, 300) in moved and ("renderMsP50", 20.0, 10.0) in moved
    assert tightened["poses"]["spawn"]["draws"] == {"baseline": 300, "ceiling": 300}
    slower = bench.merge_reports([report({"spawn": pose_result(draws=310, p50=6.0)})])
    again, changes = bench.tighten_budget(tightened, slower, gpu="GPU A")
    assert changes == [] and again["poses"]["spawn"]["draws"]["ceiling"] == 300
    _, other_gpu = bench.tighten_budget(budget, faster, gpu="GPU B")
    assert {item["metric"] for item in other_gpu}.isdisjoint(bench.TIME_METRICS)


def test_gate_skips_cleanly_without_a_gpu(monkeypatch, capsys):
    monkeypatch.setattr(bench, "gpu_present", lambda: False)
    monkeypatch.setattr(bench, "run_bench", lambda *a, **k: pytest.fail("must not run"))
    assert bench.main(["--gate"]) == 0
    assert "skipped: no GPU" in capsys.readouterr().out


def test_gate_skips_cleanly_without_node(monkeypatch, capsys):
    monkeypatch.setattr(bench, "gpu_present", lambda: True)
    monkeypatch.setattr(bench, "node_binary", lambda: None)
    assert bench.main(["--gate"]) == 0
    assert "skipped: node" in capsys.readouterr().out
    with pytest.raises(SystemExit):
        bench.main([])


def test_cli_update_baseline_needs_an_existing_budget(tmp_path):
    source = tmp_path / "a.json"
    source.write_text(json.dumps(report({"spawn": pose_result()})))
    with pytest.raises(SystemExit):
        bench.main(["--report", str(source), "--update-baseline", "--budget", str(tmp_path / "none.json")])


def test_check_without_budget_exits_2(tmp_path):
    source = tmp_path / "a.json"
    source.write_text(json.dumps(report({"spawn": pose_result()})))
    assert bench.main(["--report", str(source), "--check", "--budget", str(tmp_path / "none.json")]) == 2


def test_committed_budget_covers_every_pose_with_consistent_ceilings():
    budget = json.loads(BUDGET.read_text())
    assert set(budget["poses"]) == EXPECTED_POSES
    for pose, entries in budget["poses"].items():
        assert set(bench.COUNT_METRICS) - {"loadLongTasks"} - bench.UNBUDGETED[pose] <= set(entries), pose
        for metric, entry in entries.items():
            assert entry["ceiling"] == bench.ceiling_for(metric, entry["baseline"], budget["policy"]), (pose, metric)
