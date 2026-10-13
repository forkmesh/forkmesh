#!/usr/bin/env python3
import argparse
import datetime
import json
import math
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / "tools" / "world_bench.mjs"
DEV_SERVER = ROOT / "world" / "tools" / "world_dev_server.py"
DEFAULT_BUDGET = ROOT / "world" / "perf-budget.json"
PRIMARY_CHECKOUT = Path.home() / "projects" / "forkmesh" / "src"
DRIVER_TIMEOUT = 1800


def cache_root():
    try:
        sys.path.insert(0, str(ROOT / "tools"))
        import forkmesh_paths
        return Path(forkmesh_paths.resolve().cache) / "world-bench"
    except Exception:
        return Path.home() / "projects" / "forkmesh" / "cache" / "world-bench"


CACHE_DIR = cache_root()
DEFAULT_GOLDEN_ROOT = CACHE_DIR / "golden"

FIXED_NOW_MS = 1_782_039_600_000
FROZEN_AT_MS = 3_600_000
QUALITY_TIER = "desktop-high"
GLOBAL_MASK = (
    "forkbot-rolling-droid",
    "forkmesh-reflective-fm-cube",
    "reward-pool-fireball-sun",
    "reward-pool-fireball-corona",
)

POSES = (
    {
        "name": "spawn",
        "place": {"kind": "town", "at": [0, 0.1, 24]},
        "camera": {"position": [0, 4.605, 34], "target": [0, 1.605, 18]},
    },
    {
        "name": "office-lobby",
        "place": {"kind": "office", "floor": "lobby", "at": [0, 0.38, -207]},
        "camera": {"position": [0, 11.862, -177.012], "target": [0, 8.905, -186.565]},
    },
    {
        "name": "office-rooftop",
        "place": {"kind": "office", "floor": "rooftop", "at": [4.5, 64.38, -207]},
        "camera": {"position": [4.5, 85.694, -167.423], "target": [4.5, 81.345, -176.428]},
    },
    {
        "name": "office-engineering",
        "place": {"kind": "office", "floor": "engineering", "at": [0, 32.38, -207]},
        "camera": {"position": [0, 43.86, -177.002], "target": [0, 40.904, -186.555]},
    },
    {
        "name": "beach",
        "place": {"kind": "beach"},
        "camera": {"position": [443, 10.035, -80], "target": [477, 0.035, -80]},
    },
    {
        "name": "members-circle",
        "place": {"kind": "town", "at": [0, 0.1, 108]},
        "camera": {"position": [0, 20, 98], "target": [0, 1, 130]},
        "unbudgeted": ["duplicateGeometries"],
    },
    {
        "name": "arrival-stats",
        "place": {"kind": "town", "at": [0, 0.1, 24]},
        "camera": {"position": [0, 2.3, 20.5], "target": [0, 1.5, 15.2]},
    },
    {
        "name": "repository-portals",
        "place": {"kind": "town", "at": [104, 0.01, 25]},
        "camera": {"position": [100, 9, 22], "target": [130, 3, 31]},
    },
    {
        "name": "town-overview",
        "place": {"kind": "town", "at": [0, 0.1, 24]},
        "camera": {"position": [0, 150, 230], "target": [0, 0, -40]},
    },
)
POSE_NAMES = tuple(pose["name"] for pose in POSES)

TIME_METRICS = (
    "renderMsP50",
    "renderMsP90",
    "firstFrameMs",
    "interactiveMs",
    "interactiveFrameMs",
)
COUNT_METRICS = (
    "draws",
    "triangles",
    "programs",
    "geometries",
    "textures",
    "loadLongTasks",
    "duplicateGeometries",
    "duplicateMaterials",
    "instancingCandidates",
    "frustumCulledOff",
)
METRICS = TIME_METRICS + COUNT_METRICS

DEFAULT_POLICY = {
    "timeFactor": 2.0,
    "timeAbsoluteMs": 5.0,
    "counts": {
        "draws": {"factor": 1.0, "absolute": 0},
        "triangles": {"factor": 1.0, "absolute": 0},
        "programs": {"factor": 1.5, "absolute": 16},
        "geometries": {"factor": 1.03, "absolute": 4},
        "textures": {"factor": 1.15, "absolute": 32},
        "loadLongTasks": {"factor": 2.0, "absolute": 8},
        "duplicateGeometries": {"factor": 1.05, "absolute": 3},
        "duplicateMaterials": {"factor": 1.05, "absolute": 3},
        "instancingCandidates": {"factor": 1.05, "absolute": 3},
        "frustumCulledOff": {"factor": 1.05, "absolute": 3},
    },
}
DEFAULT_GOLDEN = {"width": 320, "height": 180, "tolerance": 32, "maxChangedPct": 0.5}
DEFAULT_FIXTURES = {"members": 180, "avatars": 48, "repositories": 72, "mirrorNodes": 12}


def policy_with_defaults(policy=None):
    merged = json.loads(json.dumps(DEFAULT_POLICY))
    if not policy:
        return merged
    for key in ("timeFactor", "timeAbsoluteMs"):
        if key in policy:
            merged[key] = float(policy[key])
    for metric, rule in (policy.get("counts") or {}).items():
        merged["counts"][metric] = {**merged["counts"].get(metric, {"factor": 1.0, "absolute": 0}), **rule}
    return merged


def ceiling_for(metric, baseline, policy=None):
    policy = policy_with_defaults(policy)
    baseline = float(baseline)
    if metric in TIME_METRICS:
        ceiling = max(baseline * policy["timeFactor"], baseline + policy["timeAbsoluteMs"])
        return round(ceiling, 2)
    rule = policy["counts"].get(metric, {"factor": 1.0, "absolute": 0})
    ceiling = max(baseline * float(rule["factor"]), baseline + float(rule["absolute"]))
    return int(math.ceil(ceiling - 1e-9))


def pose_metrics(result):
    if not isinstance(result, dict) or result.get("failed"):
        return {}
    metrics = {}
    for key, value in (result.get("counts") or {}).items():
        if key in COUNT_METRICS:
            metrics[key] = value
    live = result.get("live") or {}
    for key in ("renderMsP50", "renderMsP90"):
        if key in live and live.get("frames"):
            metrics[key] = live[key]
    load = result.get("load") or {}
    for key in ("firstFrameMs", "interactiveMs", "interactiveFrameMs", "loadLongTasks"):
        if key in load:
            metrics[key] = load[key]
    return {key: value for key, value in metrics.items() if isinstance(value, (int, float))}


UNBUDGETED = {pose["name"]: frozenset(pose.get("unbudgeted", ())) for pose in POSES}


def report_metrics(report):
    return {
        name: {
            metric: value
            for metric, value in pose_metrics(result).items()
            if metric not in UNBUDGETED.get(name, ())
        }
        for name, result in (report.get("poses") or {}).items()
    }


def merge_metrics(runs):
    merged = {}
    for run in runs:
        for pose, metrics in run.items():
            target = merged.setdefault(pose, {})
            for metric, value in metrics.items():
                target[metric] = max(target.get(metric, value), value)
    return merged


def merge_reports(reports):
    return merge_metrics([report_metrics(report) for report in reports])


def report_gpu(reports):
    gpus = sorted({report.get("gpu") or "" for report in reports} - {""})
    return gpus[0] if len(gpus) == 1 else " | ".join(gpus)


def build_budget(merged, gpu="", policy=None, golden=None, previous=None):
    policy = policy_with_defaults(policy if policy is not None else (previous or {}).get("policy"))
    poses = {}
    for pose in sorted(merged, key=lambda name: POSE_NAMES.index(name) if name in POSE_NAMES else len(POSE_NAMES)):
        entries = {}
        for metric in METRICS:
            if metric not in merged[pose]:
                continue
            baseline = merged[pose][metric]
            entries[metric] = {"baseline": baseline, "ceiling": ceiling_for(metric, baseline, policy)}
        poses[pose] = entries
    return {
        "tool": "world_bench",
        "version": 1,
        "gpu": gpu,
        "updatedAt": datetime.date.today().isoformat(),
        "policy": policy,
        "golden": {**DEFAULT_GOLDEN, **((previous or {}).get("golden") or {}), **(golden or {})},
        "poses": poses,
    }


def check_budget(merged, budget, gpu="", software=False):
    findings = []
    budget_gpu = budget.get("gpu") or ""
    times_comparable = not software and (not budget_gpu or not gpu or budget_gpu == gpu)
    for pose, entries in (budget.get("poses") or {}).items():
        actual = merged.get(pose)
        if actual is None:
            findings.append({"pose": pose, "metric": "*", "status": "missing"})
            continue
        for metric, entry in entries.items():
            if metric not in actual:
                findings.append({"pose": pose, "metric": metric, "status": "missing"})
                continue
            if metric in TIME_METRICS and not times_comparable:
                findings.append({"pose": pose, "metric": metric, "status": "skipped", "value": actual[metric]})
                continue
            value = actual[metric]
            ceiling = entry["ceiling"]
            status = "over" if value > ceiling else "ok"
            findings.append({
                "pose": pose,
                "metric": metric,
                "status": status,
                "value": value,
                "ceiling": ceiling,
                "baseline": entry.get("baseline"),
            })
    return findings


def tighten_budget(budget, merged, gpu="", software=False):
    policy = policy_with_defaults(budget.get("policy"))
    budget_gpu = budget.get("gpu") or ""
    times_comparable = not software and (not budget_gpu or not gpu or budget_gpu == gpu)
    updated = json.loads(json.dumps(budget))
    changes = []
    for pose, entries in (updated.get("poses") or {}).items():
        for metric, entry in entries.items():
            value = (merged.get(pose) or {}).get(metric)
            if value is None or (metric in TIME_METRICS and not times_comparable):
                continue
            ceiling = ceiling_for(metric, value, policy)
            if ceiling < entry["ceiling"]:
                changes.append({"pose": pose, "metric": metric, "from": entry["ceiling"], "to": ceiling})
                entries[metric] = {"baseline": value, "ceiling": ceiling}
    if changes:
        updated["updatedAt"] = datetime.date.today().isoformat()
    return updated, changes


def golden_findings(reports, max_changed_pct):
    findings = []
    for report in reports:
        for pose, result in (report.get("poses") or {}).items():
            golden = (result or {}).get("golden") or {}
            if result.get("failed"):
                findings.append({"pose": pose, "status": "failed", "detail": result["failed"].splitlines()[0]})
            elif golden.get("blessed"):
                findings.append({"pose": pose, "status": "blessed", "baseline": golden.get("baseline")})
            elif golden.get("missing"):
                findings.append({"pose": pose, "status": "missing", "baseline": golden.get("baseline")})
            elif "changedPct" in golden:
                changed = golden["changedPct"]
                status = "ok" if golden.get("sameSize", True) and changed <= max_changed_pct else "changed"
                findings.append({
                    "pose": pose,
                    "status": status,
                    "changedPct": changed,
                    "maxDelta": golden.get("maxDelta"),
                    "diff": golden.get("diff", ""),
                })
    return findings


def format_value(value):
    if isinstance(value, float):
        return f"{value:.2f}".rstrip("0").rstrip(".")
    return str(value)


def format_findings(findings):
    lines = []
    for item in findings:
        if item["status"] == "over":
            lines.append(
                f"FAIL  {item['pose']:<20} {item['metric']:<20} {format_value(item['value'])} > "
                f"{format_value(item['ceiling'])} (baseline {format_value(item['baseline'])})"
            )
        elif item["status"] == "missing":
            lines.append(f"MISS  {item['pose']:<20} {item['metric']:<20} not measured")
    over = sum(1 for item in findings if item["status"] == "over")
    ok = sum(1 for item in findings if item["status"] == "ok")
    skipped = sum(1 for item in findings if item["status"] == "skipped")
    missing = sum(1 for item in findings if item["status"] == "missing")
    lines.append(f"budget: {ok} within, {over} over, {missing} missing, {skipped} skipped")
    return "\n".join(lines)


def format_golden(findings):
    lines = []
    for item in findings:
        status = item["status"]
        if status in ("ok", "changed"):
            flag = "FAIL " if status == "changed" else "ok   "
            line = f"{flag} golden {item['pose']:<20} {item['changedPct']}% changed (max delta {item['maxDelta']})"
            if status == "changed" and item.get("diff"):
                line += f" diff {item['diff']}"
            lines.append(line)
        elif status == "failed":
            lines.append(f"FAIL  golden {item['pose']:<20} pose did not run: {item['detail']}")
        else:
            lines.append(f"{status:<5} golden {item['pose']:<20} {item.get('baseline', '')}")
    return "\n".join(lines)


def format_table(merged):
    columns = ("draws", "triangles", "programs", "geometries", "textures", "renderMsP50", "renderMsP90",
               "firstFrameMs", "interactiveMs", "loadLongTasks")
    header = f"{'pose':<20}" + "".join(f"{name:>14}" for name in columns)
    rows = [header]
    for pose in sorted(merged, key=lambda name: POSE_NAMES.index(name) if name in POSE_NAMES else 99):
        metrics = merged[pose]
        rows.append(f"{pose:<20}" + "".join(f"{format_value(metrics.get(name, '-')):>14}" for name in columns))
    return "\n".join(rows)


def gate_failed(budget_findings, golden_results):
    return any(item["status"] in ("over", "missing") for item in budget_findings) or any(
        item["status"] in ("changed", "failed") for item in golden_results
    )


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        prog="world_bench",
        description="Measure the ForkMesh World at fixed camera poses and gate on a performance budget and golden frames.",
    )
    parser.add_argument("--report", action="append", default=[], metavar="JSON",
                        help="use an existing report instead of running (repeatable; merged by maximum)")
    parser.add_argument("--runs", type=int, default=1, help="bench runs to merge by maximum (default 1)")
    parser.add_argument("--pose", action="append", default=[], choices=POSE_NAMES, help="limit to these poses")
    parser.add_argument("--budget", type=Path, default=DEFAULT_BUDGET, help="budget file (default world/perf-budget.json)")
    parser.add_argument("--write-budget", action="store_true", help="write a fresh budget from the merged runs")
    parser.add_argument("--update-baseline", action="store_true",
                        help="refresh baselines in the existing budget, keeping its policy")
    parser.add_argument("--tighten", action="store_true", help="lower ceilings this run beat")
    parser.add_argument("--check", action="store_true", help="exit 1 when any metric exceeds its ceiling")
    parser.add_argument("--gate", action="store_true",
                        help="--check plus golden-frame comparison; skips (exit 0) without a GPU or node")
    parser.add_argument("--timeout", type=int, default=DRIVER_TIMEOUT, help="seconds before a run is abandoned")
    parser.add_argument("--bless", action="store_true", help="write this run's frames as the golden images")
    parser.add_argument("--golden-root", type=Path, default=DEFAULT_GOLDEN_ROOT, help="golden image root")
    parser.add_argument("--out", type=Path, help="report path (default under the cache directory)")
    parser.add_argument("--shots-dir", type=Path, help="where frames and diff images go")
    parser.add_argument("--measure-seconds", type=float, default=4.0, help="live frames sampled per pose")
    parser.add_argument("--software", action="store_true", help="use SwiftShader; timings are not compared")
    parser.add_argument("--full-shots", action="store_true", help="also save full-size frames")
    parser.add_argument("--json", action="store_true", help="print findings as JSON")
    parser.add_argument("--probe", type=Path, help="JavaScript run in each page after capture; result lands in the report")
    args = parser.parse_args(argv)
    if args.runs < 1:
        parser.error("--runs must be at least 1")
    if sum(bool(flag) for flag in (args.write_budget, args.update_baseline, args.tighten)) > 1:
        parser.error("--write-budget, --update-baseline and --tighten are exclusive")
    if args.bless and args.report:
        parser.error("--bless needs a live run, not --report")
    return args


def gpu_present():
    return any(Path("/dev/dri").glob("renderD*"))


def node_binary():
    found = shutil.which("node")
    if found:
        return found

    def version(path):
        return [int(part) if part.isdigit() else 0 for part in path.parts[-3].lstrip("v").split(".")]

    candidates = sorted((Path.home() / ".nvm" / "versions" / "node").glob("v*/bin/node"), key=version)
    return str(candidates[-1]) if candidates else None


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_for_port(port, timeout=15.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        with socket.socket() as sock:
            sock.settimeout(0.5)
            if sock.connect_ex(("127.0.0.1", port)) == 0:
                return True
        time.sleep(0.1)
    return False


def git_commit():
    try:
        return subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short=12", "HEAD"],
                              capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def run_bench(args, run_index, stamp, golden, node):
    port = free_port()
    server = subprocess.Popen(
        [sys.executable, str(DEV_SERVER), "--port", str(port), "--upstream", "http://127.0.0.1:9"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        if not wait_for_port(port):
            raise SystemExit("world_bench: dev server did not start")
        run_dir = CACHE_DIR / "runs" / f"{stamp}-{run_index}"
        out = args.out if args.out and args.runs == 1 else run_dir / "report.json"
        shots = args.shots_dir or run_dir / "shots"
        selected = [pose for pose in POSES if not args.pose or pose["name"] in args.pose]
        config = {
            "root": str(ROOT),
            "playwrightFrom": [str(PRIMARY_CHECKOUT / "app" / "browser_tests")],
            "origin": f"http://127.0.0.1:{port}",
            "out": str(out),
            "shotsDir": str(shots),
            "goldenRoot": str(args.golden_root),
            "bless": bool(args.bless),
            "software": bool(args.software),
            "fullShots": bool(args.full_shots),
            "probe": str(args.probe.resolve()) if args.probe else "",
            "commit": git_commit(),
            "fixedNow": FIXED_NOW_MS,
            "frozenAtMs": FROZEN_AT_MS,
            "frameDtMs": 20,
            "freezeFrames": 20,
            "freezeRounds": 6,
            "measureSeconds": args.measure_seconds,
            "loadTimeoutMs": 120_000,
            "viewport": {"width": 1280, "height": 720},
            "golden": golden,
            "fixtures": DEFAULT_FIXTURES,
            "settle": {"stableSamples": 5, "intervalMs": 1000, "capMs": 60_000},
            "poses": [
                {**pose, "qualityTier": QUALITY_TIER, "mask": list(GLOBAL_MASK) + pose.get("mask", [])}
                for pose in selected
            ],
        }
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as handle:
            json.dump(config, handle)
            config_path = handle.name
        try:
            result = subprocess.run([node, str(DRIVER), config_path], stdin=subprocess.DEVNULL,
                                    timeout=args.timeout)
        except subprocess.TimeoutExpired:
            raise SystemExit(f"world_bench: driver timed out after {args.timeout} s")
        finally:
            os.unlink(config_path)
        if result.returncode != 0 or not Path(out).exists():
            raise SystemExit(f"world_bench: driver failed (exit {result.returncode})")
        print(f"world_bench: report {out}", file=sys.stderr)
        return json.loads(Path(out).read_text())
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()


def load_budget(path):
    path = Path(path)
    return json.loads(path.read_text()) if path.exists() else None


def main(argv=None):
    args = parse_args(argv)
    budget = load_budget(args.budget)
    golden = {**DEFAULT_GOLDEN, **((budget or {}).get("golden") or {})}
    if args.report:
        reports = [json.loads(Path(path).read_text()) for path in args.report]
    else:
        node = node_binary()
        if args.gate and not args.software and not gpu_present():
            print("world_bench: skipped: no GPU render node (/dev/dri/renderD*)")
            return 0
        if not node:
            if args.gate:
                print("world_bench: skipped: node is not installed")
                return 0
            raise SystemExit("world_bench: node is required")
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        reports = [run_bench(args, index, stamp, golden, node) for index in range(args.runs)]
    if args.pose:
        for report in reports:
            report["poses"] = {name: value for name, value in report.get("poses", {}).items() if name in args.pose}
    gpu = report_gpu(reports)
    software = any(report.get("software") for report in reports)
    merged = merge_reports(reports)
    print(f"world_bench: {len(reports)} run(s) on {gpu or 'unknown GPU'}")
    print(format_table(merged))

    status = 0
    if args.write_budget or args.update_baseline:
        previous = budget if args.update_baseline else None
        if args.update_baseline and previous is None:
            raise SystemExit(f"world_bench: no budget at {args.budget} to update (use --write-budget)")
        if args.update_baseline and args.pose:
            kept = dict(previous.get("poses") or {})
            fresh = build_budget(merged, gpu=gpu, previous=previous)
            kept.update(fresh["poses"])
            fresh["poses"] = {name: kept[name] for name in POSE_NAMES if name in kept}
            written = fresh
        else:
            written = build_budget(merged, gpu=gpu, previous=previous)
        Path(args.budget).write_text(json.dumps(written, indent=2) + "\n")
        print(f"world_bench: wrote budget {args.budget}")
        budget = written

    if args.tighten:
        if budget is None:
            raise SystemExit(f"world_bench: no budget at {args.budget} to tighten")
        budget, changes = tighten_budget(budget, merged, gpu=gpu, software=software)
        for change in changes:
            print(f"tightened {change['pose']:<20} {change['metric']:<20} "
                  f"{format_value(change['from'])} -> {format_value(change['to'])}")
        if changes:
            Path(args.budget).write_text(json.dumps(budget, indent=2) + "\n")
        print(f"world_bench: tightened {len(changes)} ceiling(s)")

    budget_findings = []
    if args.check or args.gate:
        if budget is None:
            print(f"world_bench: no budget at {args.budget} (run with --write-budget)")
            return 2
        scoped = budget
        if args.pose:
            scoped = {**budget, "poses": {k: v for k, v in budget.get("poses", {}).items() if k in args.pose}}
        budget_findings = check_budget(merged, scoped, gpu=gpu, software=software)
        print(format_findings(budget_findings))

    golden_results = []
    if args.gate or args.bless:
        golden_results = golden_findings(reports, float(golden["maxChangedPct"]))
        print(format_golden(golden_results))

    if args.json:
        print(json.dumps({"budget": budget_findings, "golden": golden_results}, indent=1))
    if (args.check or args.gate) and gate_failed(budget_findings, golden_results if args.gate else []):
        status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
