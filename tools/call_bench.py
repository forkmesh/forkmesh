#!/usr/bin/env python3
import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SERVER = ROOT / "server"


def default_out():
    stamp = time.strftime("%Y%m%d-%H%M%S")
    scratch = os.environ.get("FORKMESH_AGENT_SCRATCH")
    if scratch:
        path = Path(scratch) / "call-bench" / stamp
        path.mkdir(parents=True, exist_ok=True)
        return path
    return Path(tempfile.mkdtemp(prefix=f"call-bench-{stamp}-"))


def num(value, digits=0):
    if value is None:
        return "-"
    if isinstance(value, (int, float)):
        if digits == 0:
            return f"{value:.0f}"
        return f"{value:.{digits}f}"
    return str(value)


def joined(value):
    if not value:
        return "never"
    return num(value)


def recovery(value):
    if value is None:
        return "n/a"
    if value < 0:
        return "none"
    return f"{value:.0f} s"


def table(header, rows):
    lines = ["| " + " | ".join(header) + " |", "| " + " | ".join("---" for _ in header) + " |"]
    lines += ["| " + " | ".join(row) + " |" for row in rows]
    return "\n".join(lines)


def freezes(direction):
    return f"{num(direction.get('freezeCount'), 1)} / {num(direction.get('freezeSeconds'), 1)} s"


def stalls(direction):
    return f"{num(direction.get('gapFreezeCount'), 1)} / {num(direction.get('gapFreezeSeconds'), 1)} s"


def delay(direction):
    if not direction.get("delaySamples"):
        return "-"
    return f"{num(direction.get('delayP50Ms'))} / {num(direction.get('delayP95Ms'))}"


def repair(direction):
    return f"{num(direction.get('nack'))} / {num(direction.get('pli'))} / {num(direction.get('fir'))}"


def relay_losses(result, way):
    lost = sum(client.get(way, {}).get("lost", 0) for client in result.get("relay") or [])
    overflow = sum(client.get(way, {}).get("overflow", 0) for client in result.get("relay") or [])
    packets = sum(client.get(way, {}).get("packets", 0) for client in result.get("relay") or [])
    if not packets:
        return "-"
    return f"{100 * lost / packets:.1f}% / {100 * overflow / packets:.1f}%"


def render(summary):
    results = summary.get("results") or []
    peers = summary.get("peers") or []
    impaired = peers[0] if peers else "the impaired client"
    out = [
        f"# Call bench: {summary.get('engine', '?')}",
        "",
        f"Written {summary.get('written', '?')}; {len(peers)} browsers ({', '.join(peers)}), "
        f"{num(summary.get('seconds'))} s per profile measured from the moment {impaired} starts, "
        f"{summary.get('cpus', '?')} CPUs. {impaired} sits behind the impaired link; "
        f"**down** is what {impaired} receives, **up** is {impaired}'s media as the others receive it. "
        "Video and audio figures are means per stream.",
        "",
        "## Join and video",
        "",
        table(
            ["Profile", "Join video ms", "Join audio ms",
             "Down fps", "Down freezes", "Down stalls", "Down height", "Down kbps", "Down delay p50/p95 ms",
             "Up fps", "Up freezes", "Up stalls", "Up height", "Up kbps", "Up delay p50/p95 ms"],
            [[
                r["profile"]["name"], joined(r["join"].get("videoMs")), joined(r["join"].get("audioMs")),
                num(r["down"].get("fps"), 1), freezes(r["down"]), stalls(r["down"]), num(r["down"].get("height")), num(r["down"].get("videoKbps")), delay(r["down"]),
                num(r["up"].get("fps"), 1), freezes(r["up"]), stalls(r["up"]), num(r["up"].get("height")), num(r["up"].get("videoKbps")), delay(r["up"]),
            ] for r in results],
        ),
        "",
        "## Audio",
        "",
        table(
            ["Profile", "Down concealed %", "Down concealment events", "Down jitter buffer ms", "Down packets lost",
             "Up concealed %", "Up concealment events", "Up jitter buffer ms", "Up packets lost"],
            [[
                r["profile"]["name"],
                num(r["down"].get("concealedPct"), 1), num(r["down"].get("concealmentEvents")),
                num(r["down"].get("audioJitterBufferMs")), num(r["down"].get("audioPacketsLost")),
                num(r["up"].get("concealedPct"), 1), num(r["up"].get("concealmentEvents")),
                num(r["up"].get("audioJitterBufferMs")), num(r["up"].get("audioPacketsLost")),
            ] for r in results],
        ),
        "",
        "## Loss repair and the sender",
        "",
        table(
            ["Profile", "Down video loss %", "Down NACK/PLI/FIR", "Down frames dropped",
             "Up video loss %", "Up NACK/PLI/FIR", "Available out kbps", "Sent kbps", "Sent height",
             "Limited by", "RTT ms", "Relay up lost/overflow", "Relay down lost/overflow"],
            [[
                r["profile"]["name"],
                num(r["down"].get("videoLossPct"), 1), repair(r["down"]), num(r["down"].get("framesDropped")),
                num(r["up"].get("videoLossPct"), 1), repair(r["up"]),
                num(r["sender"].get("availableOutgoingKbps")), num(r["sender"].get("videoKbps")),
                num(r["sender"].get("height")), r["sender"].get("qualityLimitation") or "-",
                num(r["sender"].get("rttMs")), relay_losses(r, "up"), relay_losses(r, "down"),
            ] for r in results],
        ),
        "",
        "## CPU",
        "",
        table(
            ["Profile", f"{impaired} CPU %", "Others CPU % (mean)", "Server process CPU %"],
            [[
                r["profile"]["name"],
                num((r.get("cpuPercent") or {}).get(r.get("impaired"))),
                num(sum(v for k, v in (r.get("cpuPercent") or {}).items() if k != r.get("impaired"))
                    / max(1, len(r.get("cpuPercent") or {}) - 1)),
                num(r.get("serverCpuPercent")),
            ] for r in results],
        ),
    ]
    phased = [r for r in results if r.get("phases")]
    if phased:
        rows = []
        for r in phased:
            for phase in r["phases"]:
                rows.append([
                    r["profile"]["name"], f"{num(phase['from'])}-{num(phase['to'])} s",
                    num(phase.get("downFps"), 1), num(phase.get("downKbps")),
                    num(phase.get("upFps"), 1), num(phase.get("upKbps")),
                    num(phase.get("availableOutgoingKbps")), num(phase.get("sentKbps")),
                ])
            rows.append([
                r["profile"]["name"], "recovery after the last change",
                recovery(r["down"].get("recoverySeconds")), recovery(r["down"].get("recoveryKbpsSeconds")),
                recovery(r["up"].get("recoverySeconds")), recovery(r["up"].get("recoveryKbpsSeconds")), "", "",
            ])
        out += [
            "",
            "## Phases",
            "",
            table(["Profile", "Phase", "Down fps", "Down kbps", "Up fps", "Up kbps", "Available out kbps", "Sent kbps"], rows),
        ]
    problems = [f"- {r['profile']['name']}: {e}" for r in results for e in (r.get("errors") or [])]
    if problems:
        out += ["", "## Errors", ""] + problems
    return "\n".join(out) + "\n"


def write_report(results_path):
    summary = json.loads(Path(results_path).read_text())
    markdown = render(summary)
    target = Path(results_path).with_suffix(".md")
    target.write_text(markdown)
    return target, markdown


def run(args):
    out = Path(args.out) if args.out else default_out()
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env["FORKMESH_CALL_BENCH"] = "1"
    env["FORKMESH_CALL_BENCH_OUT"] = str(out)
    if args.profiles:
        env["FORKMESH_CALL_BENCH_PROFILES"] = args.profiles
    if args.seconds:
        env["FORKMESH_CALL_BENCH_SECONDS"] = str(args.seconds)
    if args.chromium:
        env["FORKMESH_TEST_CHROMIUM"] = args.chromium
    command = ["go", "test", "-count=1", "-run", "^TestCallBench$", "-timeout", args.timeout, "-v", "./rtc/bench/"]
    code = subprocess.call(command, cwd=SERVER, env=env)
    results = out / "results.json"
    if results.exists():
        target, markdown = write_report(results)
        print(markdown)
        print(f"wrote {results} and {target}")
    else:
        print(f"no results in {out}", file=sys.stderr)
        code = code or 1
    return code


def main():
    parser = argparse.ArgumentParser(description="Run the call bench and write its Markdown report.")
    sub = parser.add_subparsers(dest="command")
    runner = sub.add_parser("run", help="run the bench (go test) and write results.json and results.md")
    runner.add_argument("--profiles", help="comma-separated profile names (default: all)")
    runner.add_argument("--seconds", type=int, help="measured seconds per profile (default 30)")
    runner.add_argument("--out", help="results folder (default: $FORKMESH_AGENT_SCRATCH/call-bench/<time> or a temp dir)")
    runner.add_argument("--chromium", help="headless Chromium binary")
    runner.add_argument("--timeout", default="90m", help="go test timeout")
    renderer = sub.add_parser("render", help="write results.md from an existing results.json")
    renderer.add_argument("results", help="path to results.json")
    args = parser.parse_args()
    if args.command == "render":
        target, markdown = write_report(args.results)
        print(markdown)
        print(f"wrote {target}")
        return 0
    if args.command is None:
        args = parser.parse_args(["run"])
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
