#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
BUCKETS = ("merged", "ready", "behind", "conflicted", "stale", "orphaned")
SKIP = {"main", "master", "HEAD", "forkmesh/pulls"}
SKIP_PREFIXES = ("forkmesh/pull/",)
STALE_DAYS = 14


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True)


def output(repo, *args):
    return git(repo, *args).stdout.strip()


def skipped(name):
    return name in SKIP or name.startswith(SKIP_PREFIXES)


def refs(repo, prefix):
    found = {}
    fmt = "%(refname)\t%(objectname)\t%(committerdate:unix)\t%(authorname)\t%(subject)"
    for line in output(repo, "for-each-ref", "--format=" + fmt, prefix).splitlines():
        name, sha, stamp, author, subject = (line.split("\t") + [""] * 5)[:5]
        name = name[len(prefix):]
        if not skipped(name):
            found[name] = {"sha": sha, "committed": int(stamp or 0),
                           "author": author, "subject": subject}
    return found


def worktrees(repo):
    by_branch, missing = {}, []
    for block in output(repo, "worktree", "list", "--porcelain").split("\n\n"):
        fields = dict((line.split(" ", 1) + [""])[:2] for line in block.splitlines())
        path = fields.get("worktree", "")
        branch = fields.get("branch", "")
        branch = branch[len("refs/heads/"):] if branch.startswith("refs/heads/") else ""
        gone = "prunable" in fields or (path and not os.path.isdir(path))
        if branch:
            by_branch[branch] = {"path": path, "gone": gone}
        elif gone:
            missing.append(path)
    return by_branch, missing


def sessions(sessions_dir):
    found = []
    if not sessions_dir.is_dir():
        return found
    for entry in sorted(sessions_dir.iterdir()):
        try:
            meta = json.loads((entry / "meta.json").read_text())
        except (OSError, ValueError):
            continue
        found.append({"id": str(meta.get("id", entry.name)), "status": meta.get("status", ""),
                      "branch": meta.get("branchName") or "",
                      "title": (meta.get("issueTitle") or "")[:80]})
    return found


def classify(repo, main_sha, main_tree, main_files_cache, row, now):
    counts = output(repo, "rev-list", "--left-right", "--count", "%s...%s" % (main_sha, row["sha"]))
    behind, ahead = (int(part) for part in (counts.split() + ["0", "0"])[:2])
    row["ahead"], row["behind"] = ahead, behind
    if ahead == 0:
        return "merged"
    merged = git(repo, "merge-tree", "--write-tree", "--name-only", "--no-messages",
                 main_sha, row["sha"])
    if merged.returncode == 0 and merged.stdout.split()[:1] == [main_tree]:
        row["content_merged"] = True
        return "merged"
    if row.get("worktree_gone"):
        return "orphaned"
    if now - row["committed"] > STALE_DAYS * 86400:
        return "stale"
    if merged.returncode == 1:
        row["conflicts"] = merged.stdout.splitlines()[1:]
        return "conflicted"
    if merged.returncode != 0:
        row["error"] = merged.stderr.strip()[-200:]
        return "conflicted"
    if behind == 0:
        return "ready"
    base = output(repo, "merge-base", main_sha, row["sha"])
    if base not in main_files_cache:
        main_files_cache[base] = set(output(repo, "diff", "--name-only", base, main_sha).splitlines())
    touched = set(output(repo, "diff", "--name-only", base, row["sha"]).splitlines())
    overlap = sorted(touched & main_files_cache[base])
    if overlap:
        row["overlap"] = overlap[:20]
        return "behind"
    return "ready"


def inventory(repo, sessions_dir, now):
    main_sha = output(repo, "rev-parse", "main")
    main_tree = output(repo, "rev-parse", "main^{tree}")
    local = refs(repo, "refs/heads/")
    remote = {name: row for name, row in refs(repo, "refs/remotes/origin/").items()
              if name not in local}
    trees, missing_trees = worktrees(repo)
    agent = sessions(sessions_dir)
    running = {item["branch"] for item in agent if item["status"] == "running"}
    rows = []
    for scope, found in (("local", local), ("origin", remote)):
        for name, row in sorted(found.items()):
            row.update({"branch": name, "scope": scope})
            tree = trees.get(name) if scope == "local" else None
            if tree:
                row["worktree"] = tree["path"]
                row["worktree_gone"] = tree["gone"]
            if name in running:
                row["running"] = True
            rows.append(row)
    cache = {}
    with ThreadPoolExecutor(max_workers=8) as pool:
        buckets = list(pool.map(lambda row: classify(repo, main_sha, main_tree, cache, row, now), rows))
    for row, bucket in zip(rows, buckets):
        row["bucket"] = bucket
    known = set(local) | set(remote)
    orphan_sessions = [item for item in agent if item["branch"] and item["branch"] not in known
                       and item["status"] != "running"]
    return {"main": main_sha, "rows": rows, "missing_worktrees": missing_trees,
            "orphan_sessions": orphan_sessions, "worktrees": len(trees) + len(missing_trees),
            "sessions": len(agent), "running": sorted(running)}


def summarize(result, now):
    counts = {scope: {bucket: 0 for bucket in BUCKETS} for scope in ("local", "origin")}
    content = {"local": 0, "origin": 0}
    for row in result["rows"]:
        counts[row["scope"]][row["bucket"]] += 1
        content[row["scope"]] += 1 if row.get("content_merged") else 0
    return {"date": time.strftime("%Y-%m-%d", time.localtime(now)), "at": int(now),
            "main": result["main"], "counts": counts, "merged_by_content": content,
            "local_total": sum(counts["local"].values()),
            "origin_only_total": sum(counts["origin"].values()),
            "worktrees": result["worktrees"],
            "missing_worktrees": len(result["missing_worktrees"]),
            "orphan_sessions": len(result["orphan_sessions"]),
            "running_sessions": len(result["running"])}


def previous(out_dir, today):
    older = sorted(path for path in out_dir.glob("*.json") if path.stem < today)
    if not older:
        return None
    try:
        return json.loads(older[-1].read_text())["summary"]
    except (OSError, ValueError, KeyError):
        return None


def delta(now, then):
    if then is None:
        return ""
    change = now - then
    return " (%+d)" % change if change else " (=)"


def report(summary, before, rows, limit):
    print("Branch inventory %s, main %s" % (summary["date"], summary["main"][:12]))
    if before:
        print("compared with %s" % before["date"])
    for scope in ("local", "origin"):
        label = "local branches" if scope == "local" else "origin-only branches"
        parts = []
        for bucket in BUCKETS:
            value = summary["counts"][scope][bucket]
            then = before["counts"][scope][bucket] if before else None
            parts.append("%s %d%s" % (bucket, value, delta(value, then)))
        print("  %s: %s" % (label, ", ".join(parts)))
        print("    of the merged, %d only by content (tip not on main, merging changes nothing)"
              % summary["merged_by_content"][scope])
    for key in ("worktrees", "missing_worktrees", "orphan_sessions", "running_sessions"):
        print("  %s: %d%s" % (key.replace("_", " "), summary[key],
                              delta(summary[key], before.get(key) if before else None)))
    for bucket in ("ready", "behind", "conflicted"):
        chosen = [row for row in rows if row["bucket"] == bucket and row["scope"] == "local"]
        if not chosen:
            continue
        print("%s (local):" % bucket)
        for row in sorted(chosen, key=lambda row: -row["committed"])[:limit]:
            extra = ""
            if row.get("conflicts"):
                extra = "  conflicts: " + ", ".join(row["conflicts"][:5])
            elif row.get("overlap"):
                extra = "  overlaps main in: " + ", ".join(row["overlap"][:5])
            print("  %s +%d -%d %s%s%s" % (row["branch"], row["ahead"], row["behind"],
                                           time.strftime("%Y-%m-%d", time.localtime(row["committed"])),
                                           "  [running]" if row.get("running") else "", extra))


def default_layout():
    sys.path.insert(0, str(TOOLS))
    import forkmesh_paths
    return forkmesh_paths.resolve()


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Sort every unmerged branch into merged, ready, behind, conflicted, "
                    "stale or orphaned and save a dated snapshot (docs/tools/branch-inventory.md).")
    parser.add_argument("--repo", default=str(TOOLS.parent))
    parser.add_argument("--repository", default="forkmesh/forkmesh")
    parser.add_argument("--agents", help="desktop agent store (default: <data>/agents)")
    parser.add_argument("--out-dir", help="snapshot folder (default: <logs>/branch-inventory)")
    parser.add_argument("--no-save", action="store_true")
    parser.add_argument("--limit", type=int, default=40)
    parser.add_argument("--json", action="store_true", help="print the snapshot as JSON")
    args = parser.parse_args(argv)

    repo = Path(args.repo).resolve()
    layout = None if (args.agents and args.out_dir) else default_layout()
    agents_dir = Path(args.agents) if args.agents else layout.data_path("agents")
    out_dir = Path(args.out_dir) if args.out_dir else layout.logs / "branch-inventory"
    now = time.time()
    result = inventory(repo, agents_dir / "sessions" / args.repository.replace("/", "-"), now)
    summary = summarize(result, now)
    snapshot = {"summary": summary, "rows": result["rows"],
                "missing_worktrees": result["missing_worktrees"],
                "orphan_sessions": result["orphan_sessions"]}
    before = previous(out_dir, summary["date"]) if out_dir.is_dir() else None
    if args.json:
        print(json.dumps(snapshot, indent=1))
    else:
        report(summary, before, result["rows"], args.limit)
    if not args.no_save:
        out_dir.mkdir(parents=True, exist_ok=True)
        target = out_dir / (summary["date"] + ".json")
        target.write_text(json.dumps(snapshot, indent=1))
        print("saved %s" % target, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
