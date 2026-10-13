#!/usr/bin/env python3
"""Summarize mw_refs output: which MainWindow members each file uses, what
could move out of MainWindow.h into per-file parts, and how many recent
MainWindow.h commits that would have kept out of the header.

    summarize.py <records-dir> <repo> [--days 7] [--json out.json]
"""
import argparse
import collections
import glob
import json
import os
import re
import subprocess


FEATURES = {
    "Agents": "AgentAccess AgentActions AgentCliSession AgentCloud AgentDetail "
              "AgentEvents AgentLanding AgentLaunch AgentList Agents AgentSync "
              "AgentTranscript AgentUsage AgentVisual Teammates CommandPermissions",
    "Chat": "Chat Messages Coves Membership Organizations Users",
    "Repos": "RepoDetail RepoFetch Repos RepoSettings RepoSync Branches "
             "SourceControl Pulls MergeQueue Issues Discussions Releases Files "
             "CodeMap Views Ledger",
    "Hosts": "Hosts HostInstall Nodes ControlNode LocalControl VultrMirrors "
             "MirrorPush NetworkRepos NetworkDiagnostics Diagnostics Logs "
             "ComponentHealth Memory",
    "Settings": "Settings Accounts Setup Onboarding ProjectPicker Projects "
                "Shortcuts",
    "Tools": "Calendar Notes Tasks Timers Search Stats Data Market Ads Tips "
             "RewardPool PackageExtensions Mcp Actions",
}
FEATURE_OF = {f"MainWindow{n}.cpp": feat
              for feat, names in FEATURES.items() for n in names.split()}


def feature(path):
    return FEATURE_OF.get(os.path.basename(path), "shell")


def load(records):
    members = {}
    defs = collections.defaultdict(set)
    refs = set()
    for path in glob.glob(os.path.join(records, "*.tsv")):
        for line in open(path, errors="replace"):
            f = line.rstrip("\n").split("\t")
            if f[0] == "M" and len(f) >= 8:
                members.setdefault(f[1], dict(
                    name=f[2], kind=f[3], access=f[4], virtual=f[5] == "1",
                    static=f[6] == "1", line=int(f[7])))
            elif f[0] == "D" and len(f) >= 5:
                defs[f[1]].add(os.path.realpath(f[3]))
            elif f[0] == "R" and len(f) >= 8:
                refs.add((f[1], os.path.realpath(f[3]), int(f[4]), int(f[5]),
                          f[6], f[7] == "1"))
    return members, defs, refs


def header_sections(header):
    """Line -> section for signals/slots and Q_INVOKABLE markers."""
    kinds = {}
    section = "plain"
    for n, text in enumerate(open(header, errors="replace"), 1):
        s = text.strip()
        if re.match(r"^(public|protected|private)\s*(Q_SLOTS|slots)\s*:", s):
            section = "slot"
        elif re.match(r"^(Q_SIGNALS|signals)\s*:", s):
            section = "signal"
        elif re.match(r"^(public|protected|private)\s*:", s):
            section = "plain"
        kinds[n] = "invokable" if "Q_INVOKABLE" in s else section
    return kinds


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("records")
    ap.add_argument("repo")
    ap.add_argument("--days", type=int, default=7)
    ap.add_argument("--json")
    a = ap.parse_args()
    header = os.path.realpath(os.path.join(a.repo, "desktop/src/MainWindow.h"))
    members, defs, refs = load(a.records)
    sections = header_sections(header)

    users = collections.defaultdict(set)
    pointer_to_member = set()
    for usr, path, line, col, method, implicit in refs:
        users[usr].add(path)
    for usr, files in defs.items():
        users[usr] |= files
    # `&MainWindow::name` (connect targets) cannot point into a part.
    for usr, path, line, col, method, implicit in refs:
        try:
            text = open(path, errors="replace").read().splitlines()[line - 1]
        except (OSError, IndexError):
            continue
        if "&MainWindow::" + members.get(usr, {}).get("name", "\0") in text:
            pointer_to_member.add(usr)

    def movable(usr, m):
        if m["kind"] not in ("field", "method"):
            return False
        if m["virtual"] or m["static"]:
            return False
        if sections.get(m["line"], "plain") != "plain":
            return False
        if usr in pointer_to_member:
            return False
        files = users.get(usr, set())
        return len(files) == 1 and header not in files

    by_kind = collections.Counter(m["kind"] for m in members.values())
    fanout = collections.Counter(min(len(users.get(u, ())), 5) for u in members)
    move = {u: next(iter(users[u])) for u, m in members.items() if movable(u, m)}

    def feature_movable(usr, m):
        if m["kind"] not in ("field", "method") or m["virtual"] or m["static"]:
            return False
        if sections.get(m["line"], "plain") != "plain" or usr in pointer_to_member:
            return False
        files = users.get(usr, set())
        feats = {feature(f) for f in files}
        return bool(files) and header not in files and len(feats) == 1 \
            and "shell" not in feats
    feature_move = {u: feature(next(iter(users[u])))
                    for u, m in members.items() if feature_movable(u, m)}
    feature_names = {members[u]["name"] for u in feature_move} - {
        m["name"] for u, m in members.items() if u not in feature_move}
    by_file = collections.Counter(os.path.basename(f) for f in move.values())
    names_movable = {members[u]["name"] for u in move}
    names_fixed = {m["name"] for u, m in members.items() if u not in move}
    # A name is movable only if every overload of it is.
    names_movable -= names_fixed

    print(f"MainWindow members: {len(members)}  {dict(by_kind)}")
    print("files using each member (5 = five or more):",
          dict(sorted(fanout.items())))
    print(f"movable into a single file's part: {len(move)} members "
          f"({len(names_movable)} names with every overload movable)")
    print("top files:", by_file.most_common(12))
    print(f"movable into a single feature's part: {len(feature_move)} members",
          dict(collections.Counter(feature_move.values()).most_common()))

    # Replay recent history: would a commit still have changed MainWindow.h?
    log = subprocess.check_output(
        ["git", "-C", a.repo, "log", f"--since={a.days} days ago",
         "--no-merges", "--format=%h", "--", "desktop/src/MainWindow.h"],
        text=True).split()
    kept_out = 0
    kept_out_feature = 0
    blockers = collections.Counter()
    for c in log:
        diff = subprocess.check_output(
            ["git", "-C", a.repo, "show", "--format=", "-U0", c, "--",
             "desktop/src/MainWindow.h"], text=True, errors="replace")
        touched = set()
        other = False
        for line in diff.splitlines():
            if not line or line[0] not in "+-" or line.startswith(("+++", "---")):
                continue
            s = line[1:].strip()
            if not s or s.startswith("//") or s.startswith("#"):
                continue
            ids = set(re.findall(r"\b(m_\w+)\b", s)) | set(
                re.findall(r"\b(\w+)\s*\(", s))
            ids = {i for i in ids if i in names_movable or i in names_fixed}
            touched |= ids
        if touched and touched <= names_movable:
            kept_out += 1
        if touched and touched <= feature_names:
            kept_out_feature += 1
        else:
            for n in touched - feature_names:
                blockers[n] += 1
    print(f"last {a.days} days, of {len(log)} MainWindow.h commits: "
          f"{kept_out} touched only single-file members, "
          f"{kept_out_feature} only single-feature members")
    print("members that most often kept a commit in MainWindow.h:",
          blockers.most_common(15))

    if a.json:
        json.dump({
            "move": {members[u]["name"] + "@" + str(members[u]["line"]):
                     os.path.basename(f) for u, f in move.items()},
        }, open(a.json, "w"), indent=1)


if __name__ == "__main__":
    main()
