#!/usr/bin/env python3
"""Disaster-mode recovery runbook, bootstrap kit, drills, and exercises (issue #518).

The runbook source is conformance/disaster-recovery/v1/runbook.json. This tool
checks it against the repository, renders the printable runbook and the
exercise register, builds and verifies a signed bootstrap kit that works with
no network, manages the community roster and recovery-key rotations, runs live
recovery drills in scratch space, and reports what closure still needs.

  forkmesh_disaster_recovery.py validate [--json]
  forkmesh_disaster_recovery.py render [--check]
  forkmesh_disaster_recovery.py gate
  forkmesh_disaster_recovery.py closure [--json]
  forkmesh_disaster_recovery.py roster check ROSTER [--json]
  forkmesh_disaster_recovery.py roster profile ROSTER --out PROFILE
  forkmesh_disaster_recovery.py roster keys ROSTER [--role export|kit|recovery]
  forkmesh_disaster_recovery.py key rotate --roster ROSTER --recovery-key SEED \\
      --revoke KEY... --successor KEY --holder NAME --reason TEXT --issued DATE --out FILE
  forkmesh_disaster_recovery.py key cosign ROTATION --recovery-key SEED
  forkmesh_disaster_recovery.py key apply ROSTER ROTATION --out NEW_ROSTER
  forkmesh_disaster_recovery.py kit build OUT_DIR --signing-key SEED [--roster ROSTER] [--built DATE]
  forkmesh_disaster_recovery.py kit verify KIT_DIR --trusted-key KEY [--today DATE] [--json]
  forkmesh_disaster_recovery.py drill run SCENARIO|all [--fault NAME] [--work DIR] \\
      [--report FILE] [--telemetry FILE] [--json]
  forkmesh_disaster_recovery.py attest REPORT...

Signing keys are base64url Ed25519 seeds written by
`forkmesh_offline_sync.py keygen`.

Exit codes: 0 done; 1 validation, drill, gate, or closure failed; 2 usage or
unreadable input; 3 a kit, roster, or rotation did not verify; 4 the kit
verified but is past its review date.
"""

from __future__ import annotations

import argparse
import contextlib
import datetime as _dt
import hashlib
import http.server
import json
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

# A kit must hold exactly its signed files, so running it never writes caches.
if __name__ == "__main__":
    sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))

import forkmesh_offline_sync as offline  # noqa: E402
import verify_migration_report as _ed25519  # noqa: E402


ROOT = Path(__file__).resolve().parents[1]
RUNBOOK_REL = "conformance/disaster-recovery/v1/runbook.json"
EXERCISES_REL = "conformance/disaster-recovery/v1/exercises"
EVIDENCE_REL = "conformance/disaster-recovery/v1/evidence"
RUNBOOK_DOC_REL = "www/docs/disaster-recovery/runbook.md"
REGISTER_DOC_REL = "www/docs/disaster-recovery/exercise-register.md"
KIT_MANIFEST = "forkmesh-recovery-kit.json"
KIT_ROSTER = "roster.json"

RUNBOOK_SCHEMA = "forkmesh-disaster-runbook-v1"
KIT_SCHEMA = "forkmesh-recovery-kit-v1"
KIT_SIGNED_SCHEMA = "forkmesh-recovery-kit-signed-v1"
ROSTER_SCHEMA = "forkmesh-recovery-roster-v1"
ROTATION_SCHEMA = "forkmesh-recovery-key-rotation-v1"
ROTATION_SIGNED_SCHEMA = "forkmesh-recovery-key-rotation-signed-v1"
REPORT_SCHEMA = "forkmesh-disaster-drill-report-v1"
EXERCISE_SCHEMA = "forkmesh-disaster-exercise-v1"
TELEMETRY_SCHEMA = "forkmesh-disaster-drill-telemetry-v1"

EXIT_OK = 0
EXIT_FAILED = 1
EXIT_USAGE = 2
EXIT_UNVERIFIED = 3
EXIT_STALE = 4

TOPICS = ("offline-bootstrap", "local-network", "removable-media", "independent-discovery",
          "key-recovery", "emergency-mirror", "clean-restoration")
REQUIRED_ROLES = ("coordinator", "key-custodian", "mirror-operator", "verifier", "scribe")
REQUIRED_SCENARIOS = ("origin-loss", "main-service-loss", "dns-loss", "key-loss",
                      "regional-isolation", "corrupted-backup")
DATA_LOSS_POLICIES = ("none", "since-last-good-copy")
OUTCOMES = ("recovered", "escalate", "no-incident")
KEY_ROLES = ("export", "kit", "recovery")
KEY_STATUSES = ("active", "revoked")
# Fault injections each drill implements; the runbook may list only these.
DRILL_FAULTS = {
    "origin-loss": ("no-local-clone",),
    "main-service-loss": ("no-surviving-mirror",),
    "dns-loss": ("no-pinned-address",),
    "key-loss": ("threshold-unreachable",),
    "regional-isolation": ("unpinned-region-key",),
    "corrupted-backup": ("all-copies-corrupt",),
    "offline-bootstrap": ("wrong-kit-key",),
}
EXERCISE_KINDS = ("tabletop", "live")
EXERCISE_STATUSES = ("planned", "completed")
FAMILIARITY = ("unfamiliar", "familiar", "automation")
SCENARIO_RESULTS = ("recovered", "partial", "failed", "not-run")
FINDING_KINDS = ("runbook-defect", "missing-prerequisite", "unsafe-step", "tooling-gap")
FINDING_STATUSES = ("open", "fixed", "accepted")
OWNER_KINDS = ("person", "role", "issue")
PLATFORMS = ("linux", "macos", "windows")

_DATE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
_ID = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
_TOOL = re.compile(r"^tools/[A-Za-z0-9_]+\.py$")
_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_EXERCISE_ID = re.compile(r"^DRX-\d{4}$")


class UsageError(Exception):
    """Bad arguments or unreadable input (exit 2)."""


class VerificationError(Exception):
    """A kit, roster, or rotation failed its checks (exit 3)."""

    def __init__(self, problems):
        self.problems = list(problems)
        super().__init__("; ".join(self.problems))


# --- Shared helpers -----------------------------------------------------------

canonical_json = offline.canonical_json


def sha256_json(value) -> str:
    return hashlib.sha256(canonical_json(value)).hexdigest()


def read_json(path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise UsageError("cannot read %s: %s" % (path, exc)) from exc


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def parse_date(text, label="date"):
    if not isinstance(text, str) or not _DATE.match(text):
        raise UsageError("%s must be YYYY-MM-DD" % label)
    try:
        return _dt.date.fromisoformat(text)
    except ValueError as exc:
        raise UsageError("%s is not a calendar date: %s" % (label, text)) from exc


def heading_anchor(heading: str) -> str:
    text = re.sub(r"[^\w\- ]", "", heading.strip().lower())
    return text.replace(" ", "-")


def load_runbook(root=ROOT):
    return read_json(Path(root) / RUNBOOK_REL)


def runbook_digest(runbook) -> str:
    return sha256_json(runbook)


def _nonempty_str(value):
    return isinstance(value, str) and value.strip() != ""


def _str_list(value):
    return isinstance(value, list) and bool(value) and all(_nonempty_str(v) for v in value)


# --- Runbook validation ---------------------------------------------------------

def _doc_errors(label, reference, root):
    if not _nonempty_str(reference):
        return ["%s: a documentation reference must be a path" % label]
    path, _, anchor = reference.partition("#")
    target = Path(root) / path
    if not target.is_file():
        return ["%s: %s does not exist" % (label, path)]
    if anchor:
        headings = {heading_anchor(line.lstrip("#"))
                    for line in target.read_text(encoding="utf-8").splitlines()
                    if line.startswith("#")}
        if anchor not in headings:
            return ["%s: %s has no heading #%s" % (label, path, anchor)]
    return []


def command_tools(argv):
    """The repository scripts a command runs (for kit completeness)."""
    if isinstance(argv, list) and len(argv) > 1 and argv[0] == "python3" and _TOOL.match(str(argv[1])):
        return [argv[1]]
    return []


def _command_errors(label, argv, runbook, root, require_sources):
    if not isinstance(argv, list) or not argv or not all(isinstance(a, str) and a for a in argv):
        return ["%s: run must be a non-empty argument list" % label]
    program = argv[0]
    if program == "git":
        return [] if len(argv) > 1 else ["%s: git needs a subcommand" % label]
    if program == "python3":
        if argv[1:3] == ["-m", "http.server"]:
            return []
        if len(argv) < 2 or not _TOOL.match(argv[1]):
            return ["%s: python3 may run only tools/*.py or -m http.server" % label]
        script = Path(root) / argv[1]
        if not script.is_file():
            return ["%s: %s does not exist" % (label, argv[1])]
        source = script.read_text(encoding="utf-8")
        errors = []
        tokens = argv[2:]
        if "--" in tokens:
            tokens = tokens[:tokens.index("--")]
        positional = True
        for token in tokens:
            if token.startswith("-"):
                positional = False
                flag = token.split("=", 1)[0]
                if flag.startswith("--") and '"%s"' % flag not in source:
                    errors.append("%s: %s has no option %s" % (label, argv[1], flag))
            elif positional and not token.startswith("<"):
                if 'add_parser("%s"' % token not in source:
                    errors.append("%s: %s has no command %s" % (label, argv[1], token))
            else:
                positional = False
        return errors
    programs = runbook.get("programs") if isinstance(runbook.get("programs"), dict) else {}
    if program not in programs:
        return ["%s: unknown program %s" % (label, program)]
    source_path = Path(root) / str(programs[program].get("source", ""))
    if not source_path.is_file():
        return ["%s: %s source %s does not exist" % (label, program, source_path.name)] \
            if require_sources else []
    source = source_path.read_text(encoding="utf-8")
    errors = []
    for token in argv[1:]:
        if token.startswith("--"):
            name = token[2:].split("=", 1)[0]
            if not re.search(r'flag\.\w+\("%s"' % re.escape(name), source):
                errors.append("%s: %s has no flag --%s" % (label, program, name))
    return errors


def tree_errors(label, tree, procedures, scenarios=None):
    """Structural checks for a decision tree; scenario nodes are allowed only in triage."""
    if not isinstance(tree, dict) or not isinstance(tree.get("nodes"), dict) or not tree["nodes"]:
        return ["%s: a decision tree needs nodes" % label]
    nodes, errors = tree["nodes"], []
    if tree.get("start") not in nodes:
        return ["%s: start node %r does not exist" % (label, tree.get("start"))]
    edges = {}
    for node_id, node in nodes.items():
        where = "%s node %s" % (label, node_id)
        if not _ID.match(str(node_id)) or not isinstance(node, dict):
            errors.append("%s: invalid node" % where)
            edges[node_id] = []
            continue
        if "question" in node:
            if not _nonempty_str(node["question"]):
                errors.append("%s: the question is empty" % where)
            targets = [node.get("yes"), node.get("no")]
        elif "procedure" in node:
            if node["procedure"] not in procedures:
                errors.append("%s: unknown procedure %s" % (where, node["procedure"]))
            targets = [node.get("next")]
        elif "outcome" in node:
            if node["outcome"] not in OUTCOMES:
                errors.append("%s: outcome must be one of %s" % (where, ", ".join(OUTCOMES)))
            if not _nonempty_str(node.get("text")):
                errors.append("%s: an outcome needs text" % where)
            targets = []
        elif "scenario" in node and scenarios is not None:
            if node["scenario"] not in scenarios:
                errors.append("%s: unknown scenario %s" % (where, node["scenario"]))
            targets = []
        else:
            errors.append("%s: a node is a question, procedure, %soutcome" % (
                where, "scenario, or " if scenarios is not None else "or "))
            targets = []
        for target in targets:
            if target not in nodes:
                errors.append("%s: points to missing node %r" % (where, target))
        edges[node_id] = [t for t in targets if t in nodes]
    # Every node reachable, and no cycles, so every walk ends at an outcome.
    state = {}

    def visit(node_id, trail):
        if state.get(node_id) == "open":
            errors.append("%s: cycle through %s" % (label, " -> ".join(trail + [node_id])))
            return
        if state.get(node_id) == "done":
            return
        state[node_id] = "open"
        for target in edges.get(node_id, []):
            visit(target, trail + [node_id])
        state[node_id] = "done"

    visit(tree["start"], [])
    for node_id in nodes:
        if node_id not in state:
            errors.append("%s: node %s is unreachable" % (label, node_id))
    return errors


def tree_procedures(tree):
    return {n["procedure"] for n in (tree.get("nodes") or {}).values()
            if isinstance(n, dict) and "procedure" in n}


def runbook_errors(runbook, root=ROOT, require_sources=True):
    if not isinstance(runbook, dict) or runbook.get("schema") != RUNBOOK_SCHEMA:
        return ["not a %s document" % RUNBOOK_SCHEMA]
    errors = []
    if not re.match(r"^\d+\.\d+\.\d+$", str(runbook.get("version", ""))):
        errors.append("version must be MAJOR.MINOR.PATCH")
    if not _DATE.match(str(runbook.get("updated", ""))):
        errors.append("updated must be YYYY-MM-DD")
    interval = runbook.get("reviewIntervalDays")
    if not isinstance(interval, int) or isinstance(interval, bool) or not 1 <= interval <= 366:
        errors.append("reviewIntervalDays must be between 1 and 366")
    if sorted(runbook.get("topics") or []) != sorted(TOPICS):
        errors.append("topics must be exactly %s" % ", ".join(TOPICS))

    def unique(items, label):
        if not isinstance(items, list) or not items:
            errors.append("%s must be a non-empty list" % label)
            return {}
        seen = {}
        for item in items:
            item_id = item.get("id") if isinstance(item, dict) else None
            if not _ID.match(str(item_id or "")):
                errors.append("%s: invalid id %r" % (label, item_id))
            elif item_id in seen:
                errors.append("%s: duplicate id %s" % (label, item_id))
            else:
                seen[item_id] = item
        return seen

    roles = unique(runbook.get("roles"), "roles")
    for role_id in REQUIRED_ROLES:
        if role_id not in roles:
            errors.append("roles: missing required role %s" % role_id)
    for role_id, role in roles.items():
        if not _nonempty_str(role.get("title")) or not _nonempty_str(role.get("summary")):
            errors.append("role %s needs a title and summary" % role_id)
        if not _str_list(role.get("checklist")):
            errors.append("role %s needs a checklist" % role_id)

    prerequisites = unique(runbook.get("prerequisites"), "prerequisites")
    for prereq_id, prereq in prerequisites.items():
        if not _nonempty_str(prereq.get("title")) or not _nonempty_str(prereq.get("check")):
            errors.append("prerequisite %s needs a title and a check" % prereq_id)
        if prereq.get("owner") not in roles:
            errors.append("prerequisite %s: unknown owner role %r" % (prereq_id, prereq.get("owner")))

    procedures = unique(runbook.get("procedures"), "procedures")
    commands = []
    for proc_id, proc in procedures.items():
        label = "procedure %s" % proc_id
        if not _nonempty_str(proc.get("title")):
            errors.append("%s needs a title" % label)
        if proc.get("topic") not in TOPICS:
            errors.append("%s: unknown topic %r" % (label, proc.get("topic")))
        if proc.get("role") not in roles:
            errors.append("%s: unknown lead role %r" % (label, proc.get("role")))
        if not isinstance(proc.get("docs"), list) or not proc["docs"]:
            errors.append("%s needs at least one documentation reference" % label)
        for reference in proc.get("docs") or []:
            errors.extend(_doc_errors(label, reference, root))
        for section, text_key in (("steps", "action"), ("verify", "check"), ("rollback", "action")):
            entries = proc.get(section)
            if not isinstance(entries, list) or not entries:
                errors.append("%s needs %s" % (label, section))
                continue
            for index, entry in enumerate(entries, 1):
                where = "%s %s %d" % (label, section, index)
                if not isinstance(entry, dict) or not _nonempty_str(entry.get(text_key)):
                    errors.append("%s needs %s text" % (where, text_key))
                    continue
                if section == "steps" and entry.get("role") not in roles:
                    errors.append("%s: unknown role %r" % (where, entry.get("role")))
                if "unsafe" in entry and not _nonempty_str(entry["unsafe"]):
                    errors.append("%s: an unsafe warning must have text" % where)
                if "run" in entry:
                    errors.extend(_command_errors(where, entry["run"], runbook, root, require_sources))
                    commands.append(entry["run"])
    for topic in TOPICS:
        if not any(p.get("topic") == topic for p in procedures.values()):
            errors.append("topic %s has no procedure" % topic)

    scenarios = unique(runbook.get("scenarios"), "scenarios")
    for scenario_id in REQUIRED_SCENARIOS:
        if scenario_id not in scenarios:
            errors.append("scenarios: missing required scenario %s" % scenario_id)
    used_procedures, used_prerequisites = set(), set()
    triage = runbook.get("triage")
    errors.extend(tree_errors("triage", triage, procedures, scenarios))
    if isinstance(triage, dict):
        used_procedures |= tree_procedures(triage)
        routed = {n.get("scenario") for n in (triage.get("nodes") or {}).values() if isinstance(n, dict)}
        for scenario_id in scenarios:
            if scenario_id not in routed:
                errors.append("triage never reaches scenario %s" % scenario_id)
    for scenario_id, scenario in scenarios.items():
        label = "scenario %s" % scenario_id
        if not _nonempty_str(scenario.get("title")) or not _str_list(scenario.get("signals")):
            errors.append("%s needs a title and signals" % label)
        objectives = scenario.get("objectives") if isinstance(scenario.get("objectives"), dict) else {}
        for key in ("recoveryMinutes", "automatedBudgetSeconds"):
            value = objectives.get(key)
            if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
                errors.append("%s: objectives.%s must be a positive integer" % (label, key))
        if objectives.get("dataLoss") not in DATA_LOSS_POLICIES:
            errors.append("%s: objectives.dataLoss must be one of %s" % (label, ", ".join(DATA_LOSS_POLICIES)))
        needs = scenario.get("prerequisites")
        if not isinstance(needs, list) or not needs:
            errors.append("%s needs prerequisites" % label)
        for prereq_id in needs or []:
            if prereq_id not in prerequisites:
                errors.append("%s: unknown prerequisite %s" % (label, prereq_id))
            used_prerequisites.add(prereq_id)
        tree = scenario.get("decisionTree")
        errors.extend(tree_errors(label, tree, procedures))
        if isinstance(tree, dict) and isinstance(tree.get("nodes"), dict):
            used_procedures |= tree_procedures(tree)
            if not any(isinstance(n, dict) and n.get("outcome") == "recovered" for n in tree["nodes"].values()):
                errors.append("%s: no path ends in recovered" % label)
        drill = scenario.get("drill") if isinstance(scenario.get("drill"), dict) else None
        if scenario_id not in DRILL_FAULTS or drill is None:
            errors.append("%s has no automated drill" % label)
            continue
        in_tree = tree_procedures(tree) if isinstance(tree, dict) else set()
        for proc_id in drill.get("procedures") or []:
            if proc_id not in in_tree:
                errors.append("%s: drill procedure %s is not in its decision tree" % (label, proc_id))
        if not drill.get("procedures"):
            errors.append("%s: the drill names no procedures" % label)
        for fault in drill.get("faults") or []:
            if fault not in DRILL_FAULTS[scenario_id]:
                errors.append("%s: the drill does not implement fault %s" % (label, fault))
    for proc_id in procedures:
        if proc_id not in used_procedures:
            errors.append("procedure %s is not used by any decision tree" % proc_id)
    for prereq_id in prerequisites:
        if prereq_id not in used_prerequisites:
            errors.append("prerequisite %s is not needed by any scenario" % prereq_id)

    kit = runbook.get("kit") if isinstance(runbook.get("kit"), dict) else {}
    files = kit.get("files")
    if not _str_list(files):
        errors.append("kit.files must list the kit's files")
        files = []
    for path in files:
        if ".." in Path(path).parts or Path(path).is_absolute():
            errors.append("kit file %s must be a relative path inside the repository" % path)
        elif not (Path(root) / path).is_file():
            errors.append("kit file %s does not exist" % path)
    for required in (RUNBOOK_REL, RUNBOOK_DOC_REL, "tools/forkmesh_disaster_recovery.py"):
        if required not in files:
            errors.append("kit.files must include %s" % required)
    for proc in procedures.values():
        for reference in proc.get("docs") or []:
            if str(reference).partition("#")[0] not in files:
                errors.append("kit.files is missing %s, which procedure %s cites" % (
                    str(reference).partition("#")[0], proc.get("id")))
    for argv in commands:
        for tool in command_tools(argv):
            if tool not in files:
                errors.append("kit.files is missing %s, which the runbook runs" % tool)
    return sorted(set(errors), key=errors.index)


# --- Rendering ------------------------------------------------------------------

def _display_command(argv):
    parts = []
    for token in argv:
        if "<" in token or re.match(r"^[\w@%:/.=,+-]+$", token):
            parts.append(token)
        else:
            parts.append(shlex.quote(token))
    return " ".join(parts)


def _scenario_heading(scenario):
    return "%s (`%s`)" % (scenario["title"], scenario["id"])


def _procedure_heading(proc):
    return "%s (`%s`)" % (proc["title"], proc["id"])


def _render_tree(tree, procedures, scenarios):
    order, queue = [], [tree["start"]]
    while queue:
        node_id = queue.pop(0)
        if node_id in order:
            continue
        order.append(node_id)
        node = tree["nodes"][node_id]
        queue.extend(t for t in (node.get("yes"), node.get("no"), node.get("next")) if t)
    number = {node_id: index for index, node_id in enumerate(order, 1)}
    lines = []
    for node_id in order:
        node = tree["nodes"][node_id]
        if "question" in node:
            text = "**%s** Yes: go to %d. No: go to %d." % (
                node["question"], number[node["yes"]], number[node["no"]])
        elif "procedure" in node:
            proc = procedures[node["procedure"]]
            text = "Do [%s](#%s), then go to %d." % (
                proc["title"], heading_anchor(_procedure_heading(proc).replace("`", "")),
                number[node["next"]])
        elif "scenario" in node:
            scenario = scenarios[node["scenario"]]
            text = "Go to scenario [%s](#%s)." % (
                scenario["title"], heading_anchor(_scenario_heading(scenario).replace("`", "")))
        else:
            label = {"recovered": "Recovered.", "escalate": "Escalate.", "no-incident": "No incident."}
            text = "**%s** %s" % (label[node["outcome"]], node["text"])
        lines.append("%d. %s" % (number[node_id], text))
    return lines


def render_runbook(runbook):
    procedures = {p["id"]: p for p in runbook["procedures"]}
    scenarios = {s["id"]: s for s in runbook["scenarios"]}
    roles = {r["id"]: r for r in runbook["roles"]}
    prerequisites = {p["id"]: p for p in runbook["prerequisites"]}
    out = [
        "# ForkMesh disaster recovery runbook",
        "",
        "<!-- Generated by tools/forkmesh_disaster_recovery.py render from %s. "
        "Do not edit by hand. -->" % RUNBOOK_REL,
        "",
        "Version **%s**, updated %s. Runbook digest: `%s`." % (
            runbook["version"], runbook["updated"], runbook_digest(runbook)),
        "",
        "This runbook works with no network and no forkmesh.com. Print it, write the",
        "digest above and the kit public key on the printed copy, and keep it with",
        "the signed bootstrap kit. `kit verify` prints the same digest, so a reader",
        "can tell that the paper and the kit agree. Review it at least every %d days." % (
            runbook["reviewIntervalDays"]),
        "",
        "## How to use this runbook",
        "",
        "1. The coordinator assigns the roles below. The verifier never checks a copy they produced.",
        "2. Walk the triage tree. It sends you to one scenario.",
        "3. Follow that scenario's decision tree. Each step names a procedure.",
        "4. Do not skip a procedure's verification. Stop at any unsafe step until the verifier agrees.",
        "5. The scribe fills in the exercise worksheet at the end, for incidents as well as drills.",
        "",
        "Commands use placeholders in angle brackets, such as `<KIT>`. Run them from",
        "the kit directory, which holds the same `tools/` as the repository.",
        "",
        "## Triage",
        "",
    ]
    out += _render_tree(runbook["triage"], procedures, scenarios)
    out += ["", "## Roles", ""]
    for role in runbook["roles"]:
        out += ["### %s (`%s`)" % (role["title"], role["id"]), "", role["summary"], ""]
        out += ["- [ ] %s" % item for item in role["checklist"]]
        out.append("")
    out += ["## Prerequisites", "",
            "Check these before a disaster. The scribe records any that were missing.", "",
            "| Prerequisite | Owner | How to check | Needed by |", "| --- | --- | --- | --- |"]
    for prereq in runbook["prerequisites"]:
        needed = [s["id"] for s in runbook["scenarios"] if prereq["id"] in s["prerequisites"]]
        out.append("| %s (`%s`) | %s | %s | %s |" % (
            prereq["title"], prereq["id"], roles[prereq["owner"]]["title"], prereq["check"],
            ", ".join("`%s`" % n for n in needed)))
    out += ["", "## Scenarios", ""]
    for scenario in runbook["scenarios"]:
        objectives = scenario["objectives"]
        out += ["### %s" % _scenario_heading(scenario), "", "Signals:", ""]
        out += ["- %s" % signal for signal in scenario["signals"]]
        out += ["", "Objectives: recover within %d minutes; data loss allowed: %s; automated drill budget %d seconds." % (
            objectives["recoveryMinutes"],
            "none" if objectives["dataLoss"] == "none" else "commits newer than the last verified copy",
            objectives["automatedBudgetSeconds"]), ""]
        out.append("Prerequisites: %s." % ", ".join(
            "%s (`%s`)" % (prerequisites[p]["title"], p) for p in scenario["prerequisites"]))
        out += ["", "Decision tree:", ""]
        out += _render_tree(scenario["decisionTree"], procedures, scenarios)
        drill = scenario["drill"]
        out += ["", "Automated drill:", "", "```sh",
                "python3 tools/forkmesh_disaster_recovery.py drill run %s" % scenario["id"]]
        out += ["python3 tools/forkmesh_disaster_recovery.py drill run %s --fault %s" % (scenario["id"], fault)
                for fault in drill.get("faults") or []]
        out += ["```", ""]
    out += ["## Procedures", ""]
    for proc in runbook["procedures"]:
        out += ["### %s" % _procedure_heading(proc), "",
                "Topic: `%s`. Lead: %s. Reference: %s." % (
                    proc["topic"], roles[proc["role"]]["title"],
                    ", ".join("`%s`" % d for d in proc["docs"])), "", "Steps:", ""]
        for index, step in enumerate(proc["steps"], 1):
            out.append("%d. **%s:** %s" % (index, roles[step["role"]]["title"], step["action"]))
            if "run" in step:
                out += ["", "   ```sh", "   " + _display_command(step["run"]), "   ```"]
            if step.get("expect"):
                out += ["", "   Expect: %s" % step["expect"]]
            if step.get("unsafe"):
                out += ["", "   **Unsafe:** %s" % step["unsafe"]]
            if "run" in step or step.get("expect") or step.get("unsafe"):
                out.append("")
        out += ["", "Verify:", ""]
        for check in proc["verify"]:
            line = "- [ ] %s" % check["check"]
            if "run" in check:
                line += " `%s`" % _display_command(check["run"])
            out.append(line)
        out += ["", "Roll back:", ""]
        for item in proc["rollback"]:
            line = "- %s" % item["action"]
            if "run" in item:
                line += " `%s`" % _display_command(item["run"])
            out.append(line)
        out.append("")
    out += [
        "## Exercise worksheet",
        "",
        "Fill in one worksheet per scenario, for drills and real incidents alike.",
        "",
        "| Field | Record |",
        "| --- | --- |",
        "| Exercise or incident | |",
        "| Kind (tabletop or live) | |",
        "| Scenario | |",
        "| Runbook version and digest | |",
        "| Participants, roles, and whether each was unfamiliar with ForkMesh | |",
        "| Only the offline runbook and kit were used (yes or no) | |",
        "| Declared at | |",
        "| Recovered and verified at (recovery time) | |",
        "| Verified repository network restored (yes or no) | |",
        "| Data loss: refs, commits, issues, pull requests | |",
        "| Missing prerequisites, each with a finding | |",
        "| Unsafe steps taken or refused, each with a finding | |",
        "| Runbook defects | |",
        "| Findings and their corrective owners | |",
        "",
    ]
    return "\n".join(out)


# --- Signatures -----------------------------------------------------------------

def _b64(raw: bytes) -> str:
    return offline._b64encode(raw)


def decode_key(value, label="key"):
    try:
        raw = offline._b64decode(value)
    except (ValueError, TypeError):
        raw = b""
    if len(raw) != 32:
        raise UsageError("%s must be a base64url 32-byte Ed25519 public key" % label)
    return raw


def _signature(seed: bytes, schema: str, digest: str):
    public = offline.ed25519_public_key(seed)
    return {"algorithm": "ed25519", "keyId": _b64(public)[:16], "publicKey": _b64(public),
            "signature": _b64(offline.ed25519_sign(seed, ("%s\n%s" % (schema, digest)).encode("ascii")))}


def _signature_valid(signature, schema: str, digest: str):
    """Return the signer's raw public key when SIGNATURE verifies, else None."""
    if not isinstance(signature, dict) or signature.get("algorithm") != "ed25519":
        return None
    try:
        public = offline._b64decode(signature.get("publicKey"))
        raw = offline._b64decode(signature.get("signature"))
    except (ValueError, TypeError):
        return None
    message = ("%s\n%s" % (schema, digest)).encode("ascii")
    return public if _ed25519.ed25519_verify(public, raw, message) else None


# --- Community roster -----------------------------------------------------------

def _url_host(url):
    import urllib.parse
    try:
        return (urllib.parse.urlsplit(str(url)).hostname or "").lower()
    except ValueError:
        return ""


def _literal_ip(text):
    import ipaddress
    try:
        return ipaddress.ip_address(str(text).strip("[]"))
    except ValueError:
        return None


def roster_problems(roster):
    """Return (problems, warnings, summary) for a community roster."""
    if not isinstance(roster, dict) or roster.get("schema") != ROSTER_SCHEMA:
        return ["not a %s document" % ROSTER_SCHEMA], [], {}
    problems, warnings = [], []
    if not _nonempty_str(roster.get("community")):
        problems.append("the roster names no community")
    if not _DATE.match(str(roster.get("updated", ""))):
        problems.append("updated must be YYYY-MM-DD")
    keys = roster.get("keys")
    if not isinstance(keys, list) or not keys:
        problems.append("the roster lists no keys")
        keys = []
    seen_ids, seen_keys = set(), set()
    for key in keys:
        if not isinstance(key, dict):
            problems.append("invalid key entry")
            continue
        label = "key %s" % key.get("keyId")
        if not _nonempty_str(key.get("keyId")) or key["keyId"] in seen_ids:
            problems.append("%s: keyId is missing or repeated" % label)
        seen_ids.add(key.get("keyId"))
        try:
            raw = decode_key(key.get("publicKey"), label)
        except UsageError as exc:
            problems.append(str(exc))
            raw = None
        if raw is not None and raw in seen_keys:
            problems.append("%s: the public key is listed twice" % label)
        seen_keys.add(raw)
        if key.get("role") not in KEY_ROLES:
            problems.append("%s: role must be one of %s" % (label, ", ".join(KEY_ROLES)))
        if key.get("status") not in KEY_STATUSES:
            problems.append("%s: status must be active or revoked" % label)
        if not _nonempty_str(key.get("holder")):
            problems.append("%s: names no holder" % label)
    active = [k for k in keys if isinstance(k, dict) and k.get("status") == "active"]
    recovery = [k for k in active if k.get("role") == "recovery"]
    if not any(k.get("role") == "export" for k in active):
        problems.append("the roster has no active export key")
    threshold = roster.get("recoveryThreshold")
    if not isinstance(threshold, int) or isinstance(threshold, bool) or threshold < 1:
        problems.append("recoveryThreshold must be a positive integer")
        threshold = 1
    holders = {k.get("holder") for k in recovery}
    if len(recovery) < threshold or len(holders) < threshold:
        problems.append("recoveryThreshold %d is not met by distinct active recovery key holders (%d)" % (
            threshold, len(holders)))
    elif threshold == 1:
        warnings.append("a single recovery key custodian can rotate keys alone")
    export_holders = {k.get("holder") for k in active if k.get("role") == "export"}
    if export_holders & holders:
        warnings.append("an export key holder also holds a recovery key")
    mirrors = roster.get("mirrors")
    if not isinstance(mirrors, list) or not mirrors:
        problems.append("the roster lists no mirrors")
        mirrors = []
    dns_free = []
    for mirror in mirrors:
        if not isinstance(mirror, dict) or not _nonempty_str(mirror.get("name")) or not _str_list(mirror.get("urls")):
            problems.append("each mirror needs a name and urls")
            continue
        pins = mirror.get("pinnedHosts") or {}
        if not isinstance(pins, dict) or not all(_literal_ip(v) is not None for v in pins.values()):
            problems.append("mirror %s: pinnedHosts must map names to literal addresses" % mirror["name"])
            pins = {}
        for url in mirror["urls"]:
            host = _url_host(url)
            if _literal_ip(host) is not None or host.endswith(".onion") or host in pins:
                dns_free.append(url)
    if mirrors and not dns_free:
        warnings.append("no mirror address works without DNS (add a literal address or pinnedHosts)")
    for contact in roster.get("contacts") or []:
        if not isinstance(contact, dict) or not all(_nonempty_str(contact.get(f)) for f in ("role", "name", "channel")):
            problems.append("each contact needs a role, name, and channel")
    rotations = roster.get("rotations") or []
    if not isinstance(rotations, list) or not all(_SHA256.match(str(r)) for r in rotations):
        problems.append("rotations must list statement digests")
    summary = {
        "community": roster.get("community"),
        "roster": sha256_json(roster),
        "keys": [{k: key.get(k) for k in ("keyId", "role", "status", "holder", "publicKey")}
                 for key in keys if isinstance(key, dict)],
        "recoveryThreshold": threshold,
        "mirrors": [{"name": m.get("name"), "urls": m.get("urls")} for m in mirrors if isinstance(m, dict)],
        "dnsFree": bool(dns_free),
        "dnsFreeUrls": dns_free,
    }
    return problems, warnings, summary


def roster_keys(roster, role):
    return {offline._b64decode(k["publicKey"]) for k in roster.get("keys") or []
            if isinstance(k, dict) and k.get("status") == "active" and k.get("role") == role}


def roster_transport_profile(roster):
    """A lan-only transport profile that dials the roster's pinned local addresses."""
    import forkmesh_transports as transports
    hosts = {}
    for mirror in roster.get("mirrors") or []:
        for name, address in (mirror.get("pinnedHosts") or {}).items():
            if transports.classify_host(str(address)) == "local-address":
                hosts[name.lower()] = str(address)
    return {"schema": transports.PROFILE_SCHEMA, "posture": "lan-only",
            "adapters": [{"id": "community-lan", "kind": "lan", "allowPlaintext": True, "hosts": hosts}]}


# --- Recovery-key rotation ------------------------------------------------------

def make_rotation(roster, recovery_seed, *, revoke, successor, holder, reason, issued, role="export"):
    problems, _, _ = roster_problems(roster)
    if problems:
        raise VerificationError(["the roster does not verify: " + p for p in problems])
    if role not in ("export", "kit"):
        raise UsageError("a rotation installs an export or kit key; recovery keys change only with a new kit")
    parse_date(issued, "--issued")
    statement = {
        "schema": ROTATION_SCHEMA,
        "community": roster["community"],
        "revoke": sorted(_b64(decode_key(k, "--revoke")) for k in revoke),
        "successor": {"publicKey": _b64(decode_key(successor, "--successor")), "role": role, "holder": holder},
        "reason": reason,
        "issued": issued,
    }
    if not statement["revoke"] or not _nonempty_str(holder) or not _nonempty_str(reason):
        raise UsageError("a rotation needs --revoke, --holder, and --reason")
    envelope = {"schema": ROTATION_SIGNED_SCHEMA, "statement": statement,
                "statementSha256": sha256_json(statement), "signatures": []}
    return cosign_rotation(envelope, recovery_seed, roster)


def cosign_rotation(envelope, recovery_seed, roster=None):
    digest = sha256_json(envelope.get("statement"))
    if envelope.get("statementSha256") != digest:
        raise VerificationError(["statementSha256 does not match the statement"])
    public = _b64(offline.ed25519_public_key(recovery_seed))
    if roster is not None and offline._b64decode(public) not in roster_keys(roster, "recovery"):
        raise VerificationError(["signer_not_recovery_key: %s is not an active recovery key" % public[:16]])
    if any(s.get("publicKey") == public for s in envelope.get("signatures") or []):
        raise UsageError("this recovery key already signed the rotation")
    envelope = dict(envelope)
    envelope["signatures"] = list(envelope.get("signatures") or []) + [
        _signature(recovery_seed, ROTATION_SCHEMA, digest)]
    return envelope


def apply_rotation(roster, envelope):
    """Return the roster after ENVELOPE, or raise VerificationError with coded problems."""
    problems, _, _ = roster_problems(roster)
    if problems:
        raise VerificationError(["the roster does not verify: " + p for p in problems])
    if not isinstance(envelope, dict) or envelope.get("schema") != ROTATION_SIGNED_SCHEMA:
        raise VerificationError(["not a %s document" % ROTATION_SIGNED_SCHEMA])
    statement = envelope.get("statement")
    if not isinstance(statement, dict) or statement.get("schema") != ROTATION_SCHEMA:
        raise VerificationError(["not a %s statement" % ROTATION_SCHEMA])
    digest = sha256_json(statement)
    problems = []
    if envelope.get("statementSha256") != digest:
        problems.append("statement_digest: statementSha256 does not match the statement")
    if statement.get("community") != roster["community"]:
        problems.append("community_mismatch: the rotation names another community")
    if digest in (roster.get("rotations") or []):
        problems.append("already_applied: this rotation is already in the roster")
    recovery = roster_keys(roster, "recovery")
    signers = set()
    for signature in envelope.get("signatures") or []:
        public = _signature_valid(signature, ROTATION_SCHEMA, digest)
        if public is None:
            problems.append("bad_signature: a signature does not verify")
        elif public not in recovery:
            problems.append("signer_not_recovery_key: %s is not an active recovery key" % _b64(public)[:16])
        else:
            signers.add(public)
    threshold = roster["recoveryThreshold"]
    if len(signers) < threshold:
        problems.append("below_threshold: %d of %d recovery signatures" % (len(signers), threshold))
    by_key = {k["publicKey"]: k for k in roster["keys"]}
    for revoked in statement.get("revoke") or []:
        entry = by_key.get(revoked)
        if entry is None or entry.get("status") != "active":
            problems.append("revoke_unknown: %s is not an active key" % str(revoked)[:16])
        elif entry.get("role") == "recovery":
            problems.append("revoke_recovery_key: recovery keys change only with a new kit")
    successor = statement.get("successor") if isinstance(statement.get("successor"), dict) else {}
    if successor.get("publicKey") in by_key:
        problems.append("successor_known: the successor key is already in the roster")
    if successor.get("role") not in ("export", "kit"):
        problems.append("successor_role: the successor must be an export or kit key")
    try:
        decode_key(successor.get("publicKey"), "successor")
        parse_date(statement.get("issued"), "issued")
    except UsageError as exc:
        problems.append("statement_invalid: %s" % exc)
    if problems:
        raise VerificationError(problems)
    updated = json.loads(json.dumps(roster))
    for key in updated["keys"]:
        if key["publicKey"] in statement["revoke"]:
            key["status"] = "revoked"
    updated["keys"].append({"keyId": successor["publicKey"][:16], "publicKey": successor["publicKey"],
                            "role": successor["role"], "holder": successor["holder"], "status": "active"})
    updated["rotations"] = list(updated.get("rotations") or []) + [digest]
    updated["updated"] = statement["issued"]
    return updated


# --- Bootstrap kit --------------------------------------------------------------

def _kit_files(kit_dir: Path):
    listed, links = [], []
    for path in sorted(kit_dir.rglob("*")):
        if path.is_symlink():
            links.append(path.relative_to(kit_dir).as_posix())
        elif path.is_file() and path.relative_to(kit_dir).as_posix() != KIT_MANIFEST:
            listed.append(path.relative_to(kit_dir).as_posix())
    return listed, links


def _file_digest(path: Path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 16), b""):
            digest.update(block)
    return digest.hexdigest()


def build_kit(out_dir, seed, *, root=ROOT, roster=None, built=None):
    root, out_dir = Path(root), Path(out_dir)
    runbook = load_runbook(root)
    errors = runbook_errors(runbook, root)
    if errors:
        raise UsageError("the runbook does not validate: " + "; ".join(errors))
    if (root / RUNBOOK_DOC_REL).read_text(encoding="utf-8") != render_runbook(runbook):
        raise UsageError("%s is out of date; run render first" % RUNBOOK_DOC_REL)
    if roster is not None:
        problems, _, _ = roster_problems(roster)
        if problems:
            raise UsageError("the roster does not verify: " + "; ".join(problems))
    if out_dir.exists() and any(out_dir.iterdir()):
        raise UsageError("%s is not empty" % out_dir)
    built_date = parse_date(built, "--built") if built else _dt.date.today()
    for rel in runbook["kit"]["files"]:
        target = out_dir / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(root / rel, target)
    if roster is not None:
        (out_dir / KIT_ROSTER).write_text(json.dumps(roster, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    files, _ = _kit_files(out_dir)
    manifest = {
        "schema": KIT_SCHEMA,
        "runbookVersion": runbook["version"],
        "runbookSha256": runbook_digest(runbook),
        "built": built_date.isoformat(),
        "reviewBy": (built_date + _dt.timedelta(days=runbook["reviewIntervalDays"])).isoformat(),
        "roster": KIT_ROSTER if roster is not None else None,
        "files": {rel: {"sha256": _file_digest(out_dir / rel), "size": (out_dir / rel).stat().st_size}
                  for rel in files},
    }
    digest = sha256_json(manifest)
    envelope = {"schema": KIT_SIGNED_SCHEMA, "manifest": manifest, "manifestSha256": digest,
                "signature": _signature(seed, KIT_SCHEMA, digest)}
    (out_dir / KIT_MANIFEST).write_text(json.dumps(envelope, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return envelope


def verify_kit(kit_dir, trusted_keys, today=None):
    """Check a kit with no network. Returns a report whose state is verified, stale, or failed."""
    kit_dir = Path(kit_dir)
    report = {"state": "failed", "problems": [], "warnings": [], "manifestSha256": None,
              "runbookVersion": None, "runbookSha256": None, "reviewBy": None,
              "signer": None, "trusted": False, "files": 0}
    problems = report["problems"]
    try:
        envelope = json.loads((kit_dir / KIT_MANIFEST).read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        problems.append("cannot read %s: %s" % (KIT_MANIFEST, exc))
        return report
    manifest = envelope.get("manifest") if isinstance(envelope, dict) else None
    if not isinstance(envelope, dict) or envelope.get("schema") != KIT_SIGNED_SCHEMA or \
            not isinstance(manifest, dict) or manifest.get("schema") != KIT_SCHEMA:
        problems.append("not a %s kit" % KIT_SIGNED_SCHEMA)
        return report
    digest = sha256_json(manifest)
    report.update(manifestSha256=digest, runbookVersion=manifest.get("runbookVersion"),
                  runbookSha256=manifest.get("runbookSha256"), reviewBy=manifest.get("reviewBy"))
    if envelope.get("manifestSha256") != digest:
        problems.append("manifestSha256 does not match the manifest")
    public = _signature_valid(envelope.get("signature"), KIT_SCHEMA, digest)
    if public is None:
        problems.append("the kit signature does not verify")
    else:
        report["signer"] = _b64(public)
        report["trusted"] = public in trusted_keys
        if not trusted_keys:
            problems.append("no trusted kit key given; read it from paper, never from the kit")
        elif not report["trusted"]:
            problems.append("the kit is signed by a key that is not trusted")
    listed = manifest.get("files") if isinstance(manifest.get("files"), dict) else {}
    present, links = _kit_files(kit_dir)
    report["files"] = len(listed)
    for link in links:
        problems.append("the kit contains a symbolic link: %s" % link)
    for rel in sorted(set(present) - set(listed)):
        problems.append("unlisted file in the kit: %s" % rel)
    for rel in sorted(set(listed) - set(present)):
        problems.append("missing file: %s" % rel)
    for rel in sorted(set(listed) & set(present)):
        entry = listed[rel] if isinstance(listed[rel], dict) else {}
        if _file_digest(kit_dir / rel) != entry.get("sha256"):
            problems.append("digest mismatch: %s" % rel)
    if problems:
        return report
    runbook = json.loads((kit_dir / RUNBOOK_REL).read_text(encoding="utf-8"))
    if runbook_digest(runbook) != manifest.get("runbookSha256"):
        problems.append("the kit runbook does not match runbookSha256")
    for error in runbook_errors(runbook, kit_dir, require_sources=False):
        problems.append("runbook: %s" % error)
    if not problems and (kit_dir / RUNBOOK_DOC_REL).read_text(encoding="utf-8") != render_runbook(runbook):
        problems.append("the printable runbook does not match the runbook source")
    if manifest.get("roster"):
        roster = json.loads((kit_dir / manifest["roster"]).read_text(encoding="utf-8"))
        roster_errors, roster_warnings, _ = roster_problems(roster)
        problems.extend("roster: %s" % p for p in roster_errors)
        report["warnings"].extend("roster: %s" % w for w in roster_warnings)
    if problems:
        return report
    today = today or _dt.date.today()
    try:
        review_by = parse_date(manifest.get("reviewBy"), "reviewBy")
    except UsageError as exc:
        problems.append(str(exc))
        return report
    report["state"] = "stale" if today > review_by else "verified"
    return report


# --- Live drills ----------------------------------------------------------------

class DrillError(Exception):
    """A drill could not continue; the report records why."""


def _git_env():
    env = dict(os.environ)
    env.update({
        "GIT_AUTHOR_NAME": "Recovery Drill", "GIT_AUTHOR_EMAIL": "drill@example.invalid",
        "GIT_COMMITTER_NAME": "Recovery Drill", "GIT_COMMITTER_EMAIL": "drill@example.invalid",
        "GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": os.devnull, "GIT_TERMINAL_PROMPT": "0",
        "NO_PROXY": "127.0.0.1,localhost", "no_proxy": "127.0.0.1,localhost",
    })
    return env


def _git(cwd, *args, check=True):
    completed = subprocess.run(
        ["git", "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false",
         "-c", "init.defaultBranch=main", *[str(a) for a in args]],
        cwd=str(cwd), env=_git_env(), capture_output=True, check=False, timeout=300)
    if check and completed.returncode != 0:
        raise DrillError("git %s failed: %s" % (args[0], completed.stderr.decode("utf-8", "replace").strip()[-300:]))
    return completed


def _git_out(cwd, *args):
    return _git(cwd, *args).stdout.decode("utf-8", "replace").strip()


def _commit(repo, files, message):
    for name, content in files.items():
        path = Path(repo) / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        _git(repo, "add", name)
    _git(repo, "commit", "-q", "-m", message)
    return _git_out(repo, "rev-parse", "HEAD")


def _remove(path):
    def make_writable(function, target, *_):
        os.chmod(target, 0o700)
        function(target)

    resolved = Path(path).resolve()
    if not any(p.name.startswith("forkmesh-drill-") for p in (resolved, *resolved.parents)):
        raise DrillError("refusing to remove %s: it is not inside a drill work folder" % path)
    if Path(path).exists():
        if sys.version_info >= (3, 12):
            shutil.rmtree(path, onexc=make_writable)
        else:
            shutil.rmtree(path, onerror=make_writable)


def _refs(repo):
    return {name: oid for name, oid in offline.list_refs(repo).items()
            if not name.startswith(offline.RESERVED_REF_PREFIX)}


def _pins_missing(repo, pins):
    local = _refs(repo)
    return sorted(name for name, oid in pins.items()
                  if not (local.get(name) == oid or (local.get(name) and offline._is_ancestor(repo, oid, local[name]))))


def _fsck(repo):
    return _git(repo, "fsck", "--connectivity-only", check=False).returncode == 0


def _corrupt(path):
    data = bytearray(Path(path).read_bytes())
    data[len(data) // 2] ^= 0xFF
    Path(path).write_bytes(bytes(data))


def _verify_media(media, trusted):
    manifest, _, problems = offline.verify_envelope(
        read_json(Path(media) / offline.MANIFEST_FILE), trusted)
    if not problems:
        problems = offline.bundle_problems(Path(media) / offline.BUNDLE_FILE, manifest)
    return manifest, problems


def _export(repo, media, seed):
    Path(media).parent.mkdir(parents=True, exist_ok=True)
    return offline.export_media(repo, media, seed, selection=offline.normalize_selection(),
                                repository="drill/community")


@contextlib.contextmanager
def _serve(directory):
    """Serve DIRECTORY read-only over HTTP on loopback, as `python3 -m http.server` does."""
    class Quiet(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=str(directory), **kwargs)

        def log_message(self, *args):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Quiet)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server.server_address[1]
    finally:
        server.shutdown()
        server.server_close()


def _roster(export_seed, recovery_seeds, mirrors, threshold=2):
    keys = [{"keyId": "export-1", "publicKey": _b64(offline.ed25519_public_key(export_seed)),
             "role": "export", "holder": "mirror operator", "status": "active"}]
    keys += [{"keyId": "recovery-%d" % index, "publicKey": _b64(offline.ed25519_public_key(seed)),
              "role": "recovery", "holder": "custodian %d" % index, "status": "active"}
             for index, seed in enumerate(recovery_seeds, 1)]
    return {"schema": ROSTER_SCHEMA, "community": "drill community",
            "updated": _dt.date.today().isoformat(), "recoveryThreshold": threshold, "keys": keys,
            "mirrors": mirrors, "rotations": [],
            "contacts": [{"role": "coordinator", "name": "drill coordinator", "channel": "in person"}]}


class Community:
    """An origin, a mirror, a maintainer clone, and a signed export, built before the disaster."""

    def __init__(self, work: Path):
        self.work = work
        self.maintainer = work / "maintainer"
        self.maintainer.mkdir(parents=True)
        _git(self.maintainer, "init", "-q", "-b", "main")
        _commit(self.maintainer, {
            "README.md": b"# Community project\n",
            ".forkmesh/issues/open/1/issue-1.json": b'{"number":1,"title":"Keep working offline"}\n',
        }, "initial")
        _git(self.maintainer, "tag", "v1.0")
        _git(self.maintainer, "checkout", "-q", "-b", "forkmesh/pulls")
        _commit(self.maintainer, {".forkmesh/pulls/1/pull-1.json": b'{"number":1,"title":"Recovery notes"}\n'},
                "open pull request 1")
        _git(self.maintainer, "checkout", "-q", "main")
        _commit(self.maintainer, {"src/app.txt": b"version 1\n"}, "add app")
        self.origin = work / "origin.git"
        _git(work, "init", "-q", "--bare", self.origin)
        _git(self.maintainer, "push", "-q", self.origin, "refs/heads/*:refs/heads/*", "refs/tags/*:refs/tags/*")
        self.mirror = work / "mirror-a.git"
        _git(work, "clone", "-q", "--mirror", self.origin, self.mirror)
        self.export_seed = os.urandom(32)
        self.recovery_seeds = [os.urandom(32) for _ in range(3)]
        self.roster = _roster(self.export_seed, self.recovery_seeds,
                              [{"name": "mirror a", "urls": ["http://192.168.1.20:8080/project.git"]}])
        self.media = work / "usb" / "export-1"
        _export(self.mirror, self.media, self.export_seed)


class Drill:
    def __init__(self, scenario, work: Path, fault=None):
        self.scenario, self.work, self.fault = scenario, work, fault
        self.steps, self.checks, self.refused, self.taken, self.missing = [], [], [], [], []
        self.started = self.finished = None
        self.data_loss = {"refsLost": [], "commitsLost": 0}

    @contextlib.contextmanager
    def step(self, procedure, action):
        entry = {"procedure": procedure, "action": action, "ok": False}
        self.steps.append(entry)
        began = time.monotonic()
        try:
            yield entry
            entry["ok"] = True
        finally:
            entry["ms"] = int((time.monotonic() - began) * 1000)

    def check(self, name, ok, detail=""):
        entry = {"check": name, "ok": bool(ok)}
        if detail and not ok:
            entry["detail"] = str(detail)[:300]
        self.checks.append(entry)
        return bool(ok)

    def refuse(self, step, reason):
        self.refused.append({"step": step, "reason": str(reason)[:200]})

    def need(self, prerequisite, detail, blocking=True):
        self.missing.append({"prerequisite": prerequisite, "detail": detail, "blocking": blocking})

    def disaster(self):
        self.started = time.monotonic()

    def measure_loss(self, truth, restored):
        """Refs and commits the truth copy held that RESTORED does not."""
        truth_refs, lost = _refs(truth), []
        restored_refs = _refs(restored) if Path(restored).exists() else {}
        present = offline._present(truth, sorted(set(restored_refs.values())))
        for name, oid in truth_refs.items():
            local = restored_refs.get(name)
            if not (local == oid or (local in present and offline._is_ancestor(truth, oid, local))):
                lost.append(name)
        stdin = "\n".join(sorted(set(truth_refs.values())) + ["^" + oid for oid in sorted(present)]) + "\n"
        count = offline._git_text(truth, "rev-list", "--count", "--stdin", stdin=stdin.encode()).strip()
        self.data_loss.update(refsLost=sorted(lost), commitsLost=int(count or 0))


def drill_origin_loss(d):
    c = Community(d.work)
    _commit(c.maintainer, {"src/app.txt": b"version 2\n"}, "work pushed after the last mirror sync")
    _git(c.maintainer, "push", "-q", c.origin, "main")
    truth = d.work / "oracle.git"  # what existed; the recovery never reads it
    _git(d.work, "clone", "-q", "--mirror", c.origin, truth)
    if d.fault == "no-local-clone":
        _remove(c.maintainer)
        d.need("independent-copies", "the newest commit existed only on the origin and one lost clone", blocking=False)
    _remove(c.origin)
    d.disaster()
    trusted = roster_keys(c.roster, "export")
    with d.step("inventory-surviving-copies", "list refs of every surviving copy"):
        copies = [p for p in (c.mirror, c.maintainer) if p.exists()]
        d.check("at least one copy survives", copies)
    with d.step("inventory-surviving-copies", "verify the removable-media export against the pinned export key"):
        manifest, problems = _verify_media(c.media, trusted)
        d.check("the signed export verifies", not problems, "; ".join(problems))
        d.check("the surviving mirror contains every signed pin", not _pins_missing(c.mirror, manifest["refs"]))
    restored = d.work / "origin-restored.git"
    with d.step("restore-origin-from-mirror", "clone the new origin from the verified mirror"):
        _git(d.work, "clone", "-q", "--mirror", c.mirror, restored)
    if c.maintainer.exists():
        with d.step("restore-origin-from-mirror", "push newer work from the local clone as fast-forwards"):
            _git(c.maintainer, "push", "-q", "--atomic", restored, "refs/heads/*:refs/heads/*", "refs/tags/*:refs/tags/*")
        with d.step("restore-origin-from-mirror", "push a stale mirror's main without --force"):
            stale = _git(c.mirror, "push", restored, "refs/heads/main:refs/heads/main", check=False)
            if stale.returncode != 0:
                d.refuse("overwrite restored history with a stale copy", "non-fast-forward push rejected")
            d.check("a stale copy cannot overwrite restored history", stale.returncode != 0)
    with d.step("restore-origin-from-mirror", "verify the restored origin"):
        d.check("connectivity check passes", _fsck(restored))
        d.check("collaboration records survive", _git(
            restored, "cat-file", "-e", "refs/heads/forkmesh/pulls:.forkmesh/pulls/1/pull-1.json",
            check=False).returncode == 0)
        d.check("every signed pin is contained", not _pins_missing(restored, manifest["refs"]))
    d.measure_loss(truth, restored)


def drill_main_service_loss(d):
    c = Community(d.work)
    primary = d.work / "primary"
    _git(d.work, "clone", "-q", "--mirror", c.origin, primary / "project.git")
    _git(primary / "project.git", "update-server-info")
    with _serve(primary) as port:
        url = "http://127.0.0.1:%d/project.git" % port
        d.check("the hosted copy answered before the outage", _git(d.work, "ls-remote", url, check=False).returncode == 0)
    if d.fault == "no-surviving-mirror":
        for path in (c.mirror, c.maintainer, c.media.parent):
            _remove(path)
        d.need("independent-copies", "no mirror, clone, or export survived outside the hosted service")
    d.disaster()
    with d.step("serve-static-emergency-mirror", "detect the outage as unreachable, never as empty"):
        d.check("the lost service reports unreachable", _git(d.work, "ls-remote", url, check=False).returncode != 0)
    serve_dir = d.work / "emergency"
    with d.step("serve-static-emergency-mirror", "copy a verified repository into the serve directory"):
        if not c.mirror.exists():
            raise DrillError("no surviving mirror or clone to serve")
        manifest, problems = _verify_media(c.media, roster_keys(c.roster, "export"))
        d.check("the signed export verifies", not problems, "; ".join(problems))
        _git(d.work, "clone", "-q", "--mirror", c.mirror, serve_dir / "project.git")
        _git(serve_dir / "project.git", "update-server-info")
    newcomer = d.work / "newcomer.git"
    with _serve(serve_dir) as port:
        emergency = "http://127.0.0.1:%d/project.git" % port
        with d.step("serve-static-emergency-mirror", "a newcomer clones the emergency mirror over static HTTP"):
            _git(d.work, "clone", "-q", "--mirror", emergency, newcomer)
        with d.step("serve-static-emergency-mirror", "push to the read-only emergency mirror"):
            push = _git(c.maintainer, "push", emergency, "main:refs/heads/unreviewed", check=False)
            if push.returncode != 0:
                d.refuse("write to an unauthenticated emergency mirror", "static HTTP mirrors are read-only")
            d.check("the emergency mirror refuses writes", push.returncode != 0)
    with d.step("serve-static-emergency-mirror", "verify the newcomer's clone against signed pins"):
        d.check("connectivity check passes", _fsck(newcomer))
        d.check("every signed pin matches", not _pins_missing(newcomer, manifest["refs"]))
    d.measure_loss(c.origin, newcomer)


def drill_dns_loss(d):
    import forkmesh_transports as transports
    c = Community(d.work)
    serve_dir = d.work / "lan"
    _git(d.work, "clone", "-q", "--mirror", c.mirror, serve_dir / "project.git")
    _git(serve_dir / "project.git", "update-server-info")
    trusted = roster_keys(c.roster, "export")
    with _serve(serve_dir) as port:
        name = "mirror.community.example"
        url = "http://%s:%d/project.git" % (name, port)
        pins = {} if d.fault == "no-pinned-address" else {name: "127.0.0.1"}
        roster = dict(c.roster, mirrors=[{"name": "lan mirror", "urls": [url], "pinnedHosts": pins}])
        dest = d.work / "clone.git"
        d.disaster()
        with d.step("clone-over-lan-without-dns", "write a lan-only profile from the roster"):
            problems, _, summary = roster_problems(roster)
            d.check("the roster verifies", not problems, "; ".join(problems))
            if not summary.get("dnsFree"):
                d.need("community-roster", "no roster address works without DNS")
            profile = transports.validate_profile(roster_transport_profile(roster))
        with d.step("clone-over-lan-without-dns", "confirm the route needs no DNS"):
            planned = transports.plan(profile, url)
            if not planned["ok"]:
                d.refuse("resolve the mirror's name through DNS",
                         "lan-only posture: %s" % ", ".join(r["reason"] for r in planned["refused"]))
                raise DrillError("no route to the mirror without DNS")
            d.check("the route dials a pinned address", planned["routes"][0]["dns"] in ("pinned", "none"))
        with d.step("clone-over-lan-without-dns", "clone over the pinned route"):
            report = transports.run_git(profile, url, ["clone", "--mirror", "-q", "@URL", str(dest)], trace="auto", timeout=300)
            d.check("no DNS query or off-route connection was seen", report["state"] != "exposed")
            d.check("the clone succeeded", transports.exit_code_for(report) == 0,
                    (report.get("git") or {}).get("stderr", report.get("reason")))
            if report.get("leakTrace") != "traced":
                d.need("recovery-toolchain", "strace is unavailable here, so DNS leaks were not traced", blocking=False)
    with d.step("clone-over-lan-without-dns", "verify the clone against signed pins"):
        manifest, problems = _verify_media(c.media, trusted)
        d.check("the signed export verifies", not problems, "; ".join(problems))
        d.check("connectivity check passes", _fsck(dest))
        d.check("every signed pin matches", not _pins_missing(dest, manifest["refs"]))
    d.measure_loss(c.origin, dest)


def drill_key_loss(d):
    c = Community(d.work)
    attacker = d.work / "attacker"
    _git(d.work, "clone", "-q", c.origin, attacker)
    malicious = _commit(attacker, {"src/app.txt": b"unreviewed change\n"}, "change signed with a stolen key")
    forged = d.work / "usb" / "forged"
    _export(attacker, forged, c.export_seed)
    stolen, c.export_seed = c.export_seed, None
    lost = _b64(offline.ed25519_public_key(stolen))
    successor_seed = os.urandom(32)
    successor = _b64(offline.ed25519_public_key(successor_seed))
    reachable = c.recovery_seeds[:1] if d.fault == "threshold-unreachable" else c.recovery_seeds[:2]
    issued = _dt.date.today().isoformat()
    rotation_args = dict(revoke=[lost], successor=successor, holder="mirror operator", reason="export key lost", issued=issued)
    d.disaster()
    with d.step("rotate-lost-export-key", "the lost key tries to sign its own replacement"):
        try:
            make_rotation(c.roster, stolen, **rotation_args)
            refused = False
        except VerificationError as exc:
            refused = exc.problems[0].startswith("signer_not_recovery_key")
            d.refuse("let the lost key sign its replacement", exc.problems[0].split(":")[0])
        d.check("the lost key cannot sign its replacement", refused)
    with d.step("rotate-lost-export-key", "the first custodian signs; one signature is below the threshold"):
        rotation = make_rotation(c.roster, reachable[0], **rotation_args)
        try:
            apply_rotation(c.roster, rotation)
            below = False
        except VerificationError as exc:
            below = any(p.startswith("below_threshold") for p in exc.problems)
            d.refuse("apply a rotation below the recovery threshold", "below_threshold")
        d.check("a rotation below the threshold is refused", below)
    with d.step("rotate-lost-export-key", "further custodians cosign and the rotation is applied"):
        for seed in reachable[1:]:
            rotation = cosign_rotation(rotation, seed, c.roster)
        if len(reachable) < c.roster["recoveryThreshold"]:
            d.need("recovery-key-threshold", "only %d of %d recovery custodians could sign" % (
                len(reachable), c.roster["recoveryThreshold"]))
        roster = apply_rotation(c.roster, rotation)
        revoked = [k for k in roster["keys"] if k["publicKey"] == lost]
        d.check("the lost key is revoked", revoked and revoked[0]["status"] == "revoked")
    media = d.work / "usb" / "export-2"
    with d.step("carry-removable-media", "re-export with the successor key"):
        _export(c.mirror, media, successor_seed)
    trusted = roster_keys(roster, "export")
    region = d.work / "region.git"
    _git(d.work, "init", "-q", "--bare", region)
    with d.step("carry-removable-media", "import media signed by the revoked key"):
        try:
            offline.import_media(forged, region, trusted_keys=trusted)
            refused = False
        except offline.VerificationError as exc:
            refused = True
            d.refuse("import media signed by the revoked key", exc.problems[0])
        d.check("media signed by the revoked key is refused and changes nothing", refused and not _refs(region))
    with d.step("carry-removable-media", "import media signed by the successor key"):
        offline.import_media(media, region, trusted_keys=trusted)
        d.check("connectivity check passes", _fsck(region))
        d.check("the change signed with the stolen key is absent",
                _git(region, "cat-file", "-e", malicious, check=False).returncode != 0)
    d.measure_loss(c.origin, region)


def drill_regional_isolation(d):
    c = Community(d.work)
    seed_a, seed_b = os.urandom(32), os.urandom(32)
    key_a, key_b = offline.ed25519_public_key(seed_a), offline.ed25519_public_key(seed_b)
    region_a, region_b = d.work / "region-a.git", d.work / "region-b.git"
    tips = []
    for region, name in ((region_a, "a"), (region_b, "b")):
        _git(d.work, "clone", "-q", "--mirror", c.origin, region)
        clone = d.work / ("work-" + name)
        _git(d.work, "clone", "-q", region, clone)
        tips.append(_commit(clone, {"src/app.txt": ("region %s\n" % name).encode(),
                                    ".forkmesh/issues/open/%s/issue.json" % name: b'{"title":"regional work"}\n'},
                            "region %s work" % name))
        _git(clone, "push", "-q", "origin", "main")
    feature = d.work / "work-b"
    _git(feature, "checkout", "-q", "-b", "feature-b")
    tips.append(_commit(feature, {"feature.txt": b"b\n"}, "region b feature"))
    _git(feature, "push", "-q", "origin", "feature-b")
    trusted_in_b = {key_b} if d.fault == "unpinned-region-key" else {key_a}
    d.disaster()
    with d.step("reconcile-after-isolation", "region A exports to removable media"):
        _export(region_a, d.work / "usb" / "a-to-b", seed_a)
    with d.step("reconcile-after-isolation", "region B imports region A's media"):
        if key_a not in trusted_in_b:
            d.need("pinned-keys-on-paper", "region B had not pinned region A's export key")
        into_b = offline.import_media(d.work / "usb" / "a-to-b", region_b, trusted_keys=trusted_in_b)
    with d.step("reconcile-after-isolation", "region B exports to removable media"):
        _export(region_b, d.work / "usb" / "b-to-a", seed_b)
    with d.step("reconcile-after-isolation", "region A imports region B's media"):
        into_a = offline.import_media(d.work / "usb" / "b-to-a", region_a, trusted_keys={key_b})
    with d.step("reconcile-after-isolation", "check that diverged work was kept aside, not overwritten"):
        kept = [item for result in (into_a, into_b) for item in result["plan"] if item.get("keptAt")]
        if kept:
            d.refuse("overwrite one region's main with the other's", "diverged refs kept under %sconflicts/" % offline.RESERVED_REF_PREFIX)
        d.check("each region's main is unchanged",
                _git_out(region_a, "rev-parse", "refs/heads/main") == tips[0]
                and _git_out(region_b, "rev-parse", "refs/heads/main") == tips[1])
        d.check("diverged main was kept aside in both regions", len(kept) >= 2)
    lost = []
    with d.step("reconcile-after-isolation", "verify every commit from both regions is in both repositories"):
        for region in (region_a, region_b):
            d.check("connectivity check passes", _fsck(region))
            reachable = set(_git_out(region, "rev-list", "--all").split())
            lost += [tip for tip in tips if tip not in reachable]
    d.data_loss.update(commitsLost=len(lost), refsLost=[])


def drill_corrupted_backup(d):
    c = Community(d.work)
    _commit(c.maintainer, {"src/app.txt": b"version 2\n"}, "work after the first backup")
    _git(c.maintainer, "push", "-q", c.origin, "main")
    newest = d.work / "usb" / "export-2"
    _export(c.origin, newest, c.export_seed)
    truth = d.work / "oracle.git"
    _git(d.work, "clone", "-q", "--mirror", c.origin, truth)
    _corrupt(newest / offline.BUNDLE_FILE)
    if d.fault == "all-copies-corrupt":
        _corrupt(c.media / offline.BUNDLE_FILE)
    for path in (c.origin, c.mirror, c.maintainer):
        _remove(path)
    d.disaster()
    trusted = roster_keys(c.roster, "export")
    restored = d.work / "restored.git"
    _git(d.work, "init", "-q", "--bare", restored)
    chosen = None
    with d.step("choose-good-backup", "verify copies newest first"):
        for label, media in (("the newest backup", newest), ("the previous backup", c.media)):
            manifest, problems = _verify_media(media, trusted)
            if problems:
                d.refuse("import %s" % label, problems[0])
                continue
            chosen = (media, manifest)
            break
        d.check("a corrupted copy is refused before import", d.refused)
    with d.step("choose-good-backup", "importing the corrupted copy changes nothing"):
        try:
            offline.import_media(newest, restored, trusted_keys=trusted)
            unchanged = False
        except offline.VerificationError:
            unchanged = not _refs(restored)
        d.check("a corrupted import leaves the repository unchanged", unchanged)
    if chosen is None:
        d.need("independent-copies", "no backup copy verified")
        d.need("tested-backup", "every backup was corrupt and none had been verified recently")
        d.measure_loss(truth, restored)
        raise DrillError("no backup copy verifies")
    with d.step("choose-good-backup", "import the newest verified copy"):
        offline.import_media(chosen[0], restored, trusted_keys=trusted)
        d.check("connectivity check passes", _fsck(restored))
        d.check("every signed pin of the chosen copy matches", not _pins_missing(restored, chosen[1]["refs"]))
    d.measure_loss(truth, restored)
    # Allowed loss: only commits newer than the copy that verified.
    d.data_loss["withinPolicy"] = not _pins_missing(restored, chosen[1]["refs"])


_OFFLINE_RUNNER = """
import runpy, sys
def guard(event, args):
    if event in ("socket.connect", "socket.getaddrinfo", "socket.gethostbyname", "urllib.Request"):
        sys.stderr.write("network blocked: %s\\n" % event)
        raise OSError("network blocked during the offline drill")
sys.addaudithook(guard)
sys.argv = sys.argv[1:]
runpy.run_path(sys.argv[0], run_name="__main__")
"""


def _run_offline(tool, *args):
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    return subprocess.run([sys.executable, "-c", _OFFLINE_RUNNER, str(tool), *map(str, args)],
                          capture_output=True, text=True, env=env, timeout=300, check=False)


def drill_offline_bootstrap(d):
    c = Community(d.work)
    kit_seed = os.urandom(32)
    kit_key = _b64(offline.ed25519_public_key(kit_seed))
    build = d.work / "kit-build"
    build_kit(build, kit_seed, root=ROOT, roster=c.roster)
    carried = d.work / "air-gapped" / "kit"
    shutil.copytree(build, carried)
    paper_key = _b64(offline.ed25519_public_key(os.urandom(32))) if d.fault == "wrong-kit-key" else kit_key
    tool = carried / "tools" / "forkmesh_disaster_recovery.py"
    d.disaster()
    with d.step("verify-bootstrap-kit", "verify the carried kit with the network blocked"):
        result = _run_offline(tool, "kit", "verify", carried, "--trusted-key", paper_key, "--json")
        if result.returncode != 0 and paper_key != kit_key:
            d.need("pinned-keys-on-paper", "the kit key on paper does not match the kit")
        d.check("the kit verifies offline against the key on paper", result.returncode == 0, result.stdout[-300:])
        d.check("verification made no network attempt", "network blocked" not in result.stderr)
    with d.step("verify-bootstrap-kit", "a kit with a changed runbook is refused"):
        tampered = d.work / "tampered"
        shutil.copytree(carried, tampered)
        doc = tampered / RUNBOOK_DOC_REL
        doc.write_text(doc.read_text(encoding="utf-8").replace("Never take", "You may take"), encoding="utf-8")
        report = verify_kit(tampered, {offline._b64decode(kit_key)})
        if report["state"] == "failed":
            d.refuse("run tools from a kit with a changed runbook", report["problems"][0])
        d.check("a tampered kit is refused", report["state"] == "failed")
    with d.step("discover-from-roster", "read mirrors and keys from the verified roster"):
        result = _run_offline(tool, "roster", "check", carried / KIT_ROSTER, "--json")
        d.check("roster check runs from the kit with no network", result.returncode == 0, result.stderr[-300:])
    with d.step("discover-from-roster", "the kit's tools restore a repository from its media"):
        region = d.work / "newcomer.git"
        _git(d.work, "init", "-q", "--bare", region)
        export_key = _b64(offline.ed25519_public_key(c.export_seed))
        result = _run_offline(carried / "tools" / "forkmesh_offline_sync.py", "import", c.media, region,
                              "--trusted-key", export_key, "--json")
        d.check("the kit's offline sync imports verified media", result.returncode == 0, result.stderr[-300:])
        d.check("connectivity check passes", _fsck(region))
    d.measure_loss(c.origin, region)


DRILLS = {
    "origin-loss": drill_origin_loss,
    "main-service-loss": drill_main_service_loss,
    "dns-loss": drill_dns_loss,
    "key-loss": drill_key_loss,
    "regional-isolation": drill_regional_isolation,
    "corrupted-backup": drill_corrupted_backup,
    "offline-bootstrap": drill_offline_bootstrap,
}


def platform_name():
    if sys.platform.startswith("linux"):
        return "linux"
    if sys.platform == "darwin":
        return "macos"
    if sys.platform.startswith(("win", "cygwin", "msys")):
        return "windows"
    return sys.platform


def _git_version():
    try:
        text = subprocess.run(["git", "--version"], capture_output=True, text=True, check=False).stdout
    except OSError:
        return None
    match = re.search(r"(\d+)\.(\d+)", text)
    return (int(match.group(1)), int(match.group(2))) if match else None


def run_drill(scenario_id, *, fault=None, work=None, runbook=None):
    runbook = runbook or load_runbook()
    scenarios = {s["id"]: s for s in runbook["scenarios"]}
    if scenario_id not in scenarios or scenario_id not in DRILLS:
        raise UsageError("unknown scenario %s (choose from %s)" % (scenario_id, ", ".join(scenarios)))
    if fault is not None and fault not in DRILL_FAULTS[scenario_id]:
        raise UsageError("%s has no fault %s (choose from %s)" % (
            scenario_id, fault, ", ".join(DRILL_FAULTS[scenario_id])))
    scenario = scenarios[scenario_id]
    if work is not None:
        Path(work).mkdir(parents=True, exist_ok=True)
    base = Path(tempfile.mkdtemp(prefix="forkmesh-drill-%s-" % scenario_id, dir=work))
    drill = Drill(scenario, base, fault)
    version = _git_version()
    if version is None or version < (2, 40):
        drill.need("recovery-toolchain", "Git 2.40 or newer is not available", blocking=version is None)
    ran_at = _dt.datetime.now(_dt.timezone.utc).replace(microsecond=0).isoformat()
    try:
        DRILLS[scenario_id](drill)
    except (DrillError, VerificationError, UsageError, offline.VerificationError, offline.UsageError,
            offline.GitError, OSError, subprocess.SubprocessError, KeyError, ValueError) as exc:
        drill.check("the drill ran to completion", False, "%s: %s" % (type(exc).__name__, exc))
    finally:
        drill.finished = time.monotonic()
        if work is None:
            _remove(base)
    objectives = scenario["objectives"]
    recovery_ms = int((drill.finished - drill.started) * 1000) if drill.started else None
    within = recovery_ms is not None and recovery_ms <= objectives["automatedBudgetSeconds"] * 1000
    loss = dict(drill.data_loss, policy=objectives["dataLoss"])
    if "withinPolicy" not in loss or objectives["dataLoss"] == "none":
        loss["withinPolicy"] = loss["commitsLost"] == 0 and not loss["refsLost"]
    verdict = "pass" if (drill.started is not None and drill.checks and all(c["ok"] for c in drill.checks)
                         and within and loss["withinPolicy"]
                         and not any(m["blocking"] for m in drill.missing)) else "fail"
    return {
        "schema": REPORT_SCHEMA,
        "scenario": scenario_id,
        "fault": fault,
        "mode": "automated-live",
        "ranAt": ran_at,
        "runbookVersion": runbook["version"],
        "runbookSha256": runbook_digest(runbook),
        "platform": {"os": platform_name(), "python": platform.python_version(),
                     "git": ".".join(map(str, version)) if version else None},
        "objective": objectives,
        "recoveryMs": recovery_ms,
        "withinObjective": within,
        "dataLoss": loss,
        "missingPrerequisites": drill.missing,
        "unsafeSteps": {"refused": drill.refused, "taken": drill.taken},
        "steps": drill.steps,
        "verification": drill.checks,
        "verdict": verdict,
    }


def drill_telemetry(report):
    """Count-only telemetry: no paths, keys, names, or addresses."""
    return {"schema": TELEMETRY_SCHEMA, "event": "drill", "scenario": report["scenario"],
            "fault": report["fault"], "verdict": report["verdict"], "os": report["platform"]["os"],
            "recoveryMs": report["recoveryMs"], "commitsLost": report["dataLoss"]["commitsLost"],
            "missingPrerequisites": len(report["missingPrerequisites"]),
            "unsafeRefused": len(report["unsafeSteps"]["refused"]),
            "unsafeTaken": len(report["unsafeSteps"]["taken"]),
            "checksFailed": sum(1 for c in report["verification"] if not c["ok"])}


# --- Exercises, evidence, and closure ------------------------------------------------

def load_exercises(root=ROOT):
    directory = Path(root) / EXERCISES_REL
    if not directory.is_dir():
        return []
    return [(path.name, read_json(path)) for path in sorted(directory.glob("*.json"))]


def load_evidence(root, reference):
    """Return (report, problem) for an exercise's `evidence/<sha256>.json` reference."""
    match = re.match(r"^evidence/([0-9a-f]{64})\.json$", str(reference or ""))
    if not match:
        return None, "drillReport must be evidence/<sha256>.json"
    path = Path(root) / EVIDENCE_REL / (match.group(1) + ".json")
    if not path.is_file():
        return None, "%s does not exist" % reference
    report = read_json(path)
    if sha256_json(report) != match.group(1):
        return None, "%s does not match its digest" % reference
    if not isinstance(report, dict) or report.get("schema") != REPORT_SCHEMA:
        return None, "%s is not a %s" % (reference, REPORT_SCHEMA)
    return report, None


def attest(paths, root=ROOT):
    """Retain drill reports as content-addressed evidence; returns their references."""
    references = []
    for path in paths:
        value = read_json(path)
        for report in value if isinstance(value, list) else [value]:
            if not isinstance(report, dict) or report.get("schema") != REPORT_SCHEMA or \
                    report.get("scenario") not in DRILL_FAULTS:
                raise UsageError("%s is not a %s" % (path, REPORT_SCHEMA))
            digest = sha256_json(report)
            write_json(Path(root) / EVIDENCE_REL / (digest + ".json"), report)
            references.append("evidence/%s.json" % digest)
    return references


def exercise_errors(name, exercise, runbook, root=ROOT):
    label = name
    if not isinstance(exercise, dict) or exercise.get("schema") != EXERCISE_SCHEMA:
        return ["%s: not a %s record" % (label, EXERCISE_SCHEMA)]
    errors = []
    if not _EXERCISE_ID.match(str(exercise.get("id", ""))) or name != "%s.json" % exercise.get("id"):
        errors.append("%s: id must be DRX-NNNN and match the file name" % label)
    if not _nonempty_str(exercise.get("title")):
        errors.append("%s: needs a title" % label)
    if exercise.get("kind") not in EXERCISE_KINDS:
        errors.append("%s: kind must be tabletop or live" % label)
    status = exercise.get("status")
    if status not in EXERCISE_STATUSES:
        errors.append("%s: status must be planned or completed" % label)
    if not _DATE.match(str(exercise.get("date", ""))):
        errors.append("%s: date must be YYYY-MM-DD" % label)
    if not _SHA256.match(str(exercise.get("runbookSha256", ""))):
        errors.append("%s: runbookSha256 must be a digest" % label)
    elif exercise.get("runbookVersion") == runbook.get("version") and \
            exercise["runbookSha256"] != runbook_digest(runbook):
        errors.append("%s: runbook %s changed without a version bump" % (label, runbook.get("version")))
    if not isinstance(exercise.get("offlineRunbookOnly"), bool):
        errors.append("%s: offlineRunbookOnly must be true or false" % label)
    roles = {r["id"] for r in runbook.get("roles") or []}
    scenario_ids = {s["id"] for s in runbook.get("scenarios") or []}
    participants = exercise.get("participants")
    if not isinstance(participants, list) or not participants:
        errors.append("%s: lists no participants" % label)
        participants = []
    for person in participants:
        if not isinstance(person, dict) or not _nonempty_str(person.get("name")) or \
                person.get("familiarity") not in FAMILIARITY or \
                not _str_list(person.get("roles")) or not set(person["roles"]) <= roles:
            errors.append("%s: each participant needs a name, runbook roles, and familiarity" % label)
    findings = {}
    for finding in exercise.get("findings") or []:
        fid = finding.get("id") if isinstance(finding, dict) else None
        where = "%s finding %s" % (label, fid)
        if not _nonempty_str(fid) or fid in findings:
            errors.append("%s: id is missing or repeated" % where)
            continue
        findings[fid] = finding
        if finding.get("kind") not in FINDING_KINDS:
            errors.append("%s: kind must be one of %s" % (where, ", ".join(FINDING_KINDS)))
        if not _nonempty_str(finding.get("summary")):
            errors.append("%s: needs a summary" % where)
        owner = finding.get("correctiveOwner")
        if not isinstance(owner, dict) or owner.get("kind") not in OWNER_KINDS or not _nonempty_str(owner.get("ref")):
            errors.append("%s: needs a corrective owner (person, role, or issue)" % where)
        elif owner["kind"] == "role" and owner["ref"] not in roles:
            errors.append("%s: owner role %s is not in the runbook" % (where, owner["ref"]))
        elif owner["kind"] == "issue" and not re.match(r"^#\d+$", owner["ref"]):
            errors.append("%s: an issue owner is written #NNN" % where)
        if finding.get("status") not in FINDING_STATUSES:
            errors.append("%s: status must be open, fixed, or accepted" % where)
        elif finding["status"] == "fixed" and not _nonempty_str(finding.get("fixedBy")):
            errors.append("%s: a fixed finding names what fixed it" % where)
    rows = exercise.get("scenarios")
    if not isinstance(rows, list) or not rows:
        errors.append("%s: lists no scenarios" % label)
        rows = []
    for row in rows:
        where = "%s scenario %s" % (label, row.get("scenario") if isinstance(row, dict) else None)
        if not isinstance(row, dict) or row.get("scenario") not in scenario_ids:
            errors.append("%s: unknown scenario" % where)
            continue
        if row.get("result") not in SCENARIO_RESULTS:
            errors.append("%s: result must be one of %s" % (where, ", ".join(SCENARIO_RESULTS)))
            continue
        if status == "planned" or row["result"] == "not-run":
            if status == "completed" and row["result"] == "not-run" and not _nonempty_str(row.get("reason")):
                errors.append("%s: a scenario that was not run needs a reason" % where)
            if status == "planned" and row["result"] != "not-run":
                errors.append("%s: a planned exercise has no results" % where)
            continue
        minutes = row.get("recoveryMinutes")
        if not isinstance(minutes, (int, float)) or isinstance(minutes, bool) or minutes < 0:
            errors.append("%s: recoveryMinutes must be a number" % where)
        if not isinstance(row.get("verifiedRepositoryNetwork"), bool):
            errors.append("%s: verifiedRepositoryNetwork must be true or false" % where)
        loss = row.get("dataLoss")
        if not isinstance(loss, dict) or not isinstance(loss.get("commitsLost"), int) or \
                loss["commitsLost"] < 0 or not _nonempty_str(loss.get("summary")):
            errors.append("%s: dataLoss needs commitsLost and a summary" % where)
        for missing in row.get("missingPrerequisites") if isinstance(row.get("missingPrerequisites"), list) else [None]:
            if not isinstance(missing, dict) or not _nonempty_str(missing.get("prerequisite")) or \
                    missing.get("finding") not in findings:
                errors.append("%s: each missing prerequisite names a finding with an owner" % where)
        for unsafe in row.get("unsafeSteps") if isinstance(row.get("unsafeSteps"), list) else [None]:
            if not isinstance(unsafe, dict) or not _nonempty_str(unsafe.get("description")) or \
                    not isinstance(unsafe.get("taken"), bool):
                errors.append("%s: each unsafe step needs a description and taken" % where)
            elif (unsafe["taken"] or unsafe.get("finding") is not None) and unsafe.get("finding") not in findings:
                errors.append("%s: an unsafe step that was taken names a finding with an owner" % where)
        if row.get("drillReport") is None:
            continue
        report, problem = load_evidence(root, row["drillReport"])
        if problem:
            errors.append("%s: %s" % (where, problem))
            continue
        expected_result = "recovered" if report["verdict"] == "pass" else "failed"
        mismatches = []
        if report["scenario"] != row["scenario"]:
            mismatches.append("scenario")
        if report["runbookSha256"] != exercise.get("runbookSha256"):
            mismatches.append("runbookSha256")
        if report["recoveryMs"] is None or minutes != round(report["recoveryMs"] / 60000, 4):
            mismatches.append("recoveryMinutes")
        if row["result"] != expected_result:
            mismatches.append("result")
        if isinstance(loss, dict) and loss.get("commitsLost") != report["dataLoss"]["commitsLost"]:
            mismatches.append("dataLoss.commitsLost")
        recorded = {m.get("prerequisite") for m in row.get("missingPrerequisites") or [] if isinstance(m, dict)}
        if {m["prerequisite"] for m in report["missingPrerequisites"]} - recorded:
            mismatches.append("missingPrerequisites")
        unsafe_rows = [u for u in row.get("unsafeSteps") or [] if isinstance(u, dict)]
        if len([u for u in unsafe_rows if not u.get("taken")]) != len(report["unsafeSteps"]["refused"]) or \
                len([u for u in unsafe_rows if u.get("taken")]) != len(report["unsafeSteps"]["taken"]):
            mismatches.append("unsafeSteps")
        if mismatches:
            errors.append("%s: does not match its drill report (%s)" % (where, ", ".join(mismatches)))
    return errors


def closure_checks(runbook, exercises, root=ROOT):
    current = runbook_digest(runbook)
    required = [s["id"] for s in runbook["scenarios"]]
    completed = [e for _, e in exercises if isinstance(e, dict) and e.get("status") == "completed"]

    def humans(exercise):
        return [p for p in exercise.get("participants") or [] if p.get("familiarity") != "automation"]

    def rows(exercise):
        return [r for r in exercise.get("scenarios") or [] if isinstance(r, dict)]

    evidence = {}
    for exercise in completed:
        for row in rows(exercise):
            if row.get("drillReport"):
                report, problem = load_evidence(root, row["drillReport"])
                if report and not problem and report["verdict"] == "pass" and report["runbookSha256"] == current:
                    evidence.setdefault(report["scenario"], set()).add(report["platform"]["os"])
    checks = []

    def add(check_id, title, missing):
        checks.append({"id": check_id, "title": title, "ok": not missing, "missing": missing})

    add("automated-drills-current", "Every scenario has a passing automated drill on the current runbook",
        [s for s in required if s not in evidence])
    add("platform-evidence", "Automated drills pass on Linux, macOS, and Windows",
        ["%s on %s" % (s, os_name) for s in required for os_name in PLATFORMS if os_name not in evidence.get(s, set())])
    for kind, check_id, title, result_ok in (
            ("tabletop", "tabletop-drills-recorded", "Every scenario has a tabletop exercise with people", lambda r: r != "not-run"),
            ("live", "live-drills-recorded", "Every scenario has a live exercise in which people recovered", lambda r: r == "recovered")):
        covered = {r["scenario"] for e in completed if e.get("kind") == kind and len(humans(e)) >= 2
                   for r in rows(e) if result_ok(r.get("result"))}
        add(check_id, title, [s for s in required if s not in covered])
    unfamiliar = any(
        e.get("kind") == "live" and e.get("offlineRunbookOnly") is True and len(humans(e)) >= 2
        and all(p.get("familiarity") == "unfamiliar" for p in humans(e))
        and any(r.get("result") == "recovered" and r.get("verifiedRepositoryNetwork") is True for r in rows(e))
        for e in completed)
    add("unfamiliar-team-restored",
        "A team unfamiliar with ForkMesh restored a verified repository network from the offline runbook alone",
        [] if unfamiliar else ["no such live exercise is recorded"])
    add("findings-owned", "Every open finding has a named person as corrective owner",
        ["%s %s (owner: %s %s)" % (e.get("id"), f.get("id"), f.get("correctiveOwner", {}).get("kind"),
                                   f.get("correctiveOwner", {}).get("ref"))
         for _, e in exercises for f in e.get("findings") or []
         if f.get("status") == "open" and f.get("correctiveOwner", {}).get("kind") != "person"])
    return checks


def _minutes(value):
    if not isinstance(value, (int, float)):
        return "n/a"
    return "%.1f s" % (value * 60) if value < 1 else "%g min" % value


def render_register(runbook, exercises, root=ROOT):
    out = [
        "# Disaster recovery exercise register",
        "",
        "<!-- Generated by tools/forkmesh_disaster_recovery.py render from %s. "
        "Do not edit by hand. -->" % EXERCISES_REL,
        "",
        "Every tabletop and live exercise of the [disaster recovery runbook](runbook.md) is",
        "recorded here with its recovery time, data loss, missing prerequisites, unsafe",
        "steps, and a corrective owner for each finding. The guide is [README.md](README.md).",
        "",
        "Current runbook: version %s, digest `%s`." % (runbook["version"], runbook_digest(runbook)),
        "",
        "## Closure status",
        "",
        "| Check | Status | Still missing |",
        "| --- | --- | --- |",
    ]
    for check in closure_checks(runbook, exercises, root):
        missing = check["missing"]
        shown = ", ".join(missing[:8]) + (" and %d more" % (len(missing) - 8) if len(missing) > 8 else "")
        out.append("| %s (`%s`) | %s | %s |" % (check["title"], check["id"], "met" if check["ok"] else "open", shown or "nothing"))
    out += ["", "## Exercises", ""]
    if not exercises:
        out += ["No exercises are recorded yet.", ""]
    for _, exercise in exercises:
        out += ["### %s: %s" % (exercise["id"], exercise["title"]), "",
                "Kind: %s. Status: %s. Date: %s. Runbook %s. Offline runbook only: %s." % (
                    exercise["kind"], exercise["status"], exercise["date"], exercise["runbookVersion"],
                    "yes" if exercise["offlineRunbookOnly"] else "no"), ""]
        if exercise.get("summary"):
            out += [exercise["summary"], ""]
        out += ["| Participant | Roles | Familiarity |", "| --- | --- | --- |"]
        out += ["| %s | %s | %s |" % (p["name"], ", ".join(p["roles"]), p["familiarity"]) for p in exercise["participants"]]
        out += ["", "| Scenario | Result | Recovery time | Data loss | Missing prerequisites | Unsafe steps | Evidence |",
                "| --- | --- | --- | --- | --- | --- | --- |"]
        for row in exercise["scenarios"]:
            missing = ", ".join("`%s` (%s)" % (m["prerequisite"], m["finding"]) for m in row.get("missingPrerequisites") or [])
            unsafe = row.get("unsafeSteps") or []
            unsafe_text = "%d refused, %d taken" % (len([u for u in unsafe if not u["taken"]]), len([u for u in unsafe if u["taken"]]))
            evidence = ""
            if row.get("drillReport"):
                report, _ = load_evidence(root, row["drillReport"])
                evidence = "`%s` (%s, %s)" % (row["drillReport"][9:25], report["verdict"], report["platform"]["os"]) if report else "missing"
            loss = row.get("dataLoss") or {}
            out.append("| `%s` | %s | %s | %s | %s | %s | %s |" % (
                row["scenario"], row["result"], _minutes(row.get("recoveryMinutes")),
                loss.get("summary", row.get("reason", "")), missing or "none",
                unsafe_text if row["result"] != "not-run" else "", evidence))
        findings = exercise.get("findings") or []
        if findings:
            out += ["", "| Finding | Kind | Summary | Corrective owner | Status |", "| --- | --- | --- | --- | --- |"]
            for finding in findings:
                owner = finding["correctiveOwner"]
                out.append("| %s | %s | %s | %s %s | %s |" % (
                    finding["id"], finding["kind"], finding["summary"], owner["kind"], owner["ref"],
                    finding["status"] + (" (%s)" % finding["fixedBy"] if finding.get("fixedBy") else "")))
        out.append("")
    return "\n".join(out)


def gate_problems(root=ROOT):
    runbook = load_runbook(root)
    problems = ["runbook: %s" % e for e in runbook_errors(runbook, root)]
    exercises = load_exercises(root)
    for name, exercise in exercises:
        problems.extend(exercise_errors(name, exercise, runbook, root))
    if not problems:
        for rel, text in ((RUNBOOK_DOC_REL, render_runbook(runbook)),
                          (REGISTER_DOC_REL, render_register(runbook, exercises, root))):
            path = Path(root) / rel
            if not path.is_file() or path.read_text(encoding="utf-8") != text:
                problems.append("%s is out of date; run render" % rel)
    return problems


# --- Command line ---------------------------------------------------------------------

def _emit(value, as_json, lines):
    if as_json:
        print(json.dumps(value, indent=2, sort_keys=True))
    else:
        for line in lines:
            print(line)


def _describe_report(report):
    loss = report["dataLoss"]
    line = "%s%s: %s in %s ms, %d commit(s) lost, %d unsafe step(s) refused" % (
        report["scenario"], " (fault %s)" % report["fault"] if report["fault"] else "", report["verdict"],
        report["recoveryMs"], loss["commitsLost"], len(report["unsafeSteps"]["refused"]))
    lines = [line]
    lines += ["  failed: %s%s" % (c["check"], " (%s)" % c["detail"] if c.get("detail") else "")
              for c in report["verification"] if not c["ok"]]
    lines += ["  missing prerequisite: %s (%s)" % (m["prerequisite"], m["detail"]) for m in report["missingPrerequisites"]]
    return lines


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    validate = commands.add_parser("validate", help="check the runbook against the repository")
    validate.add_argument("--json", action="store_true")
    render = commands.add_parser("render", help="write the printable runbook and exercise register")
    render.add_argument("--check", action="store_true", help="fail if the committed output differs")
    commands.add_parser("gate", help="validate the runbook and exercises and check rendered output")
    closure = commands.add_parser("closure", help="report what issue closure still needs")
    closure.add_argument("--json", action="store_true")

    roster = commands.add_parser("roster", help="community roster commands").add_subparsers(dest="action", required=True)
    roster_check = roster.add_parser("check", help="verify a roster and list its keys and mirrors")
    roster_check.add_argument("roster")
    roster_check.add_argument("--json", action="store_true")
    roster_profile = roster.add_parser("profile", help="write a lan-only transport profile from pinned addresses")
    roster_profile.add_argument("roster")
    roster_profile.add_argument("--out", required=True)
    roster_keys_cmd = roster.add_parser("keys", help="print a role's active public keys")
    roster_keys_cmd.add_argument("roster")
    roster_keys_cmd.add_argument("--role", choices=KEY_ROLES, default="export")

    key = commands.add_parser("key", help="recovery-key rotation commands").add_subparsers(dest="action", required=True)
    rotate = key.add_parser("rotate", help="write and sign a key rotation statement")
    rotate.add_argument("--roster", required=True)
    rotate.add_argument("--recovery-key", required=True, help="a recovery key custodian's seed file")
    rotate.add_argument("--revoke", action="append", required=True, help="public key to revoke")
    rotate.add_argument("--successor", required=True, help="successor public key")
    rotate.add_argument("--successor-role", choices=("export", "kit"), default="export")
    rotate.add_argument("--holder", required=True)
    rotate.add_argument("--reason", required=True)
    rotate.add_argument("--issued", required=True, help="YYYY-MM-DD")
    rotate.add_argument("--out", required=True)
    cosign = key.add_parser("cosign", help="add a recovery key signature to a rotation")
    cosign.add_argument("rotation")
    cosign.add_argument("--recovery-key", required=True)
    cosign.add_argument("--roster")
    apply = key.add_parser("apply", help="verify a rotation and write the updated roster")
    apply.add_argument("roster")
    apply.add_argument("rotation")
    apply.add_argument("--out", required=True)

    kit = commands.add_parser("kit", help="bootstrap kit commands").add_subparsers(dest="action", required=True)
    kit_build = kit.add_parser("build", help="copy and sign the offline bootstrap kit")
    kit_build.add_argument("out_dir")
    kit_build.add_argument("--signing-key", required=True)
    kit_build.add_argument("--roster")
    kit_build.add_argument("--built", help="build date YYYY-MM-DD (default today)")
    kit_verify = kit.add_parser("verify", help="verify a kit with no network")
    kit_verify.add_argument("kit_dir")
    kit_verify.add_argument("--trusted-key", action="append", default=[])
    kit_verify.add_argument("--today", help="check freshness as of YYYY-MM-DD")
    kit_verify.add_argument("--json", action="store_true")

    drill = commands.add_parser("drill", help="live recovery drills").add_subparsers(dest="action", required=True)
    drill_run = drill.add_parser("run", help="run a scenario's drill in scratch space")
    drill_run.add_argument("scenario", help="scenario id, or all")
    drill_run.add_argument("--fault", help="inject one of the scenario's faults")
    drill_run.add_argument("--work", help="keep scratch directories under this path")
    drill_run.add_argument("--report", help="write the report (a list for all) to this file")
    drill_run.add_argument("--telemetry", help="append count-only JSON lines")
    drill_run.add_argument("--json", action="store_true")

    attest_cmd = commands.add_parser("attest", help="retain drill reports as content-addressed evidence")
    attest_cmd.add_argument("reports", nargs="+")
    return parser


def _dispatch(args):
    if args.command == "validate":
        runbook = load_runbook()
        errors = runbook_errors(runbook)
        _emit({"ok": not errors, "errors": errors, "runbookVersion": runbook.get("version"),
               "runbookSha256": runbook_digest(runbook)}, args.json,
              errors or ["runbook %s is valid (digest %s)" % (runbook.get("version"), runbook_digest(runbook))])
        return EXIT_OK if not errors else EXIT_FAILED

    if args.command == "render":
        runbook = load_runbook()
        # The printable runbook is a kit file, so a first render must not require it.
        errors = [e for e in runbook_errors(runbook) if e != "kit file %s does not exist" % RUNBOOK_DOC_REL]
        if errors:
            for error in errors:
                print("runbook: %s" % error, file=sys.stderr)
            return EXIT_FAILED
        exercises = load_exercises()
        stale = []
        for rel, text in ((RUNBOOK_DOC_REL, render_runbook(runbook)), (REGISTER_DOC_REL, render_register(runbook, exercises))):
            path = ROOT / rel
            current = path.read_text(encoding="utf-8") if path.is_file() else None
            if current != text:
                stale.append(rel)
                if not args.check:
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(text, encoding="utf-8")
        if args.check and stale:
            for rel in stale:
                print("%s is out of date; run render" % rel, file=sys.stderr)
            return EXIT_FAILED
        print("rendered output is current" if args.check or not stale else "wrote %s" % ", ".join(stale))
        return EXIT_OK

    if args.command == "gate":
        problems = gate_problems()
        for problem in problems:
            print(problem, file=sys.stderr)
        if not problems:
            print("disaster recovery gate passed")
        return EXIT_OK if not problems else EXIT_FAILED

    if args.command == "closure":
        checks = closure_checks(load_runbook(), load_exercises())
        lines = ["%s %s%s" % ("met " if c["ok"] else "open", c["id"],
                               "" if c["ok"] else ": " + "; ".join(c["missing"][:6])) for c in checks]
        _emit({"ok": all(c["ok"] for c in checks), "checks": checks}, args.json, lines)
        return EXIT_OK if all(c["ok"] for c in checks) else EXIT_FAILED

    if args.command == "roster":
        roster = read_json(args.roster)
        if args.action == "check":
            problems, warnings, summary = roster_problems(roster)
            lines = ["roster problem: %s" % p for p in problems] + ["warning: %s" % w for w in warnings]
            if not problems:
                lines.insert(0, "roster for %s verifies (digest %s)" % (summary["community"], summary["roster"]))
                lines += ["key %s %s %s held by %s: %s" % (k["keyId"], k["role"], k["status"], k["holder"], k["publicKey"])
                          for k in summary["keys"]]
                lines += ["mirror %s: %s" % (m["name"], " ".join(m["urls"])) for m in summary["mirrors"]]
            _emit({"ok": not problems, "problems": problems, "warnings": warnings, **summary}, args.json, lines)
            return EXIT_OK if not problems else EXIT_UNVERIFIED
        problems, _, _ = roster_problems(roster)
        if problems:
            raise VerificationError(problems)
        if args.action == "profile":
            profile = roster_transport_profile(roster)
            write_json(args.out, profile)
            print("wrote a lan-only profile with %d pinned host(s)" % len(profile["adapters"][0]["hosts"]))
            return EXIT_OK
        for public in sorted(_b64(k) for k in roster_keys(roster, args.role)):
            print(public)
        return EXIT_OK

    if args.command == "key":
        if args.action == "rotate":
            if Path(args.out).exists():
                raise UsageError("%s already exists" % args.out)
            envelope = make_rotation(read_json(args.roster), offline.load_seed(args.recovery_key),
                                     revoke=args.revoke, successor=args.successor, holder=args.holder,
                                     reason=args.reason, issued=args.issued, role=args.successor_role)
            write_json(args.out, envelope)
            print("rotation %s signed (1 signature)" % envelope["statementSha256"])
            return EXIT_OK
        if args.action == "cosign":
            envelope = cosign_rotation(read_json(args.rotation), offline.load_seed(args.recovery_key),
                                       read_json(args.roster) if args.roster else None)
            write_json(args.rotation, envelope)
            print("rotation %s now has %d signature(s)" % (envelope["statementSha256"], len(envelope["signatures"])))
            return EXIT_OK
        if Path(args.out).exists():
            raise UsageError("%s already exists" % args.out)
        updated = apply_rotation(read_json(args.roster), read_json(args.rotation))
        write_json(args.out, updated)
        print("wrote the rotated roster (digest %s)" % sha256_json(updated))
        return EXIT_OK

    if args.command == "kit":
        if args.action == "build":
            envelope = build_kit(args.out_dir, offline.load_seed(args.signing_key),
                                 roster=read_json(args.roster) if args.roster else None, built=args.built)
            manifest = envelope["manifest"]
            print("built kit with %d files, review by %s" % (len(manifest["files"]), manifest["reviewBy"]))
            print("manifest digest: %s" % envelope["manifestSha256"])
            print("kit public key (write it on paper): %s" % envelope["signature"]["publicKey"])
            return EXIT_OK
        trusted = {decode_key(k, "--trusted-key") for k in args.trusted_key}
        report = verify_kit(args.kit_dir, trusted, parse_date(args.today, "--today") if args.today else None)
        lines = ["kit %s: %s" % (report["state"], args.kit_dir)]
        lines += ["problem: %s" % p for p in report["problems"]] + ["warning: %s" % w for w in report["warnings"]]
        if report["state"] != "failed":
            lines += ["manifest digest: %s" % report["manifestSha256"],
                      "runbook %s digest: %s" % (report["runbookVersion"], report["runbookSha256"]),
                      "review by: %s" % report["reviewBy"]]
        if report["state"] == "stale":
            lines.append("the kit is past its review date; rebuild it from a current roster")
        _emit(report, args.json, lines)
        return {"verified": EXIT_OK, "stale": EXIT_STALE}.get(report["state"], EXIT_UNVERIFIED)

    if args.command == "drill":
        runbook = load_runbook()
        scenario_ids = [s["id"] for s in runbook["scenarios"]] if args.scenario == "all" else [args.scenario]
        if args.fault and args.scenario == "all":
            raise UsageError("--fault needs a single scenario")
        reports = [run_drill(s, fault=args.fault, work=args.work, runbook=runbook) for s in scenario_ids]
        if args.report:
            write_json(args.report, reports if args.scenario == "all" else reports[0])
        if args.telemetry:
            with open(args.telemetry, "a", encoding="utf-8") as handle:
                for report in reports:
                    handle.write(json.dumps(drill_telemetry(report), sort_keys=True) + "\n")
        _emit(reports if args.scenario == "all" else reports[0], args.json,
              [line for report in reports for line in _describe_report(report)])
        return EXIT_OK if all(r["verdict"] == "pass" for r in reports) else EXIT_FAILED

    if args.command == "attest":
        for reference in attest(args.reports):
            print(reference)
        return EXIT_OK
    raise UsageError("unknown command")


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        return _dispatch(args)
    except (UsageError, offline.UsageError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        return EXIT_USAGE
    except (VerificationError, offline.VerificationError) as exc:
        for problem in exc.problems:
            print("verification failed: %s" % problem, file=sys.stderr)
        return EXIT_UNVERIFIED


if __name__ == "__main__":
    raise SystemExit(main())
