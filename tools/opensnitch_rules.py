#!/usr/bin/env python3
from __future__ import annotations

import argparse
import datetime
import json
import os
import pwd
import re
import shutil
import sys
import tempfile
from pathlib import Path

RULES_DIR = Path("/etc/opensnitchd/rules")
PREFIX = "000-forkmesh-"
PROGRAM_PATTERN = r"(?i)^(/home/[^/]+|/root|/opt|/usr/local|/tmp/\.mount_[^/]+)/(.*/)?[^/]*forkmesh[^/]*$"
LOOPBACK_PATTERN = r"^(127\.[0-9]+\.[0-9]+\.[0-9]+|::1)$"
AGENT_CLI_PATHS = (r"/\.local/share/claude/versions/[^/]+$",
                   r"/\.config/Claude/claude-code/[^/]+/[^/]+/claude$",
                   r"/\.codex/packages/standalone/[^/]+/bin/codex$")


def operator(kind: str, operand: str, data: str) -> dict:
    return {"type": kind, "operand": operand, "sensitive": False, "data": data, "list": None}


def all_of(*operators: dict) -> dict:
    items = list(operators)
    return {"type": "list", "operand": "list", "sensitive": False,
            "data": json.dumps(items), "list": items}


def rule(name: str, description: str, op: dict, stamp: str) -> dict:
    return {"created": stamp, "updated": stamp, "name": PREFIX + name,
            "description": description, "enabled": True, "precedence": True,
            "action": "allow", "duration": "always", "operator": op}


def agent_cli_pattern(home: Path) -> str:
    return "|".join("^" + re.escape(str(home)) + path for path in AGENT_CLI_PATHS)


def build_rules(uid: int, home: Path, loopback: bool = True,
                dev_trees: bool | None = None, agent_clis: bool = True) -> list[dict]:
    stamp = datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")
    user = operator("simple", "user.id", str(uid))
    rules = [rule("programs",
                  "ForkMesh desktop, CLI, AppImage and test programs, wherever they are built or installed",
                  all_of(user, operator("regexp", "process.path", PROGRAM_PATTERN)), stamp)]
    projects = home / "projects" / "forkmesh"
    if dev_trees is None:
        dev_trees = projects.is_dir()
    if dev_trees:
        trees = "^" + re.escape(str(projects)) + "/|^" + re.escape(str(home / ".cache")) + "/fm[^/]*/"
        rules.append(rule("dev-trees",
                          "Programs built in the ForkMesh checkout, its worktrees and the agents' ~/.cache/fm* build folders",
                          all_of(user, operator("regexp", "process.path", trees)), stamp))
    if agent_clis:
        rules.append(rule("agent-clis",
                          "The agents' Claude Code and Codex programs, whose path changes with every update",
                          all_of(user, operator("regexp", "process.path", agent_cli_pattern(home))),
                          stamp))
    if loopback:
        rules.append(rule("loopback",
                          "Connections to this machine (127.0.0.0/8, ::1): the desktop's local services, deploy tunnels and publishers",
                          all_of(user, operator("regexp", "dest.ip", LOOPBACK_PATTERN)), stamp))
    return rules


def write_rules(rules: list[dict], directory: Path) -> list[Path]:
    directory.mkdir(parents=True, exist_ok=True)
    written = []
    for item in rules:
        path = directory / f"{item['name']}.json"
        path.write_text(json.dumps(item, indent=2) + "\n")
        written.append(path)
    return written


def target_user(name: str | None) -> pwd.struct_passwd:
    if name:
        return pwd.getpwnam(name)
    sudo_user = os.environ.get("SUDO_USER")
    if os.geteuid() == 0 and sudo_user:
        return pwd.getpwnam(sudo_user)
    return pwd.getpwuid(os.getuid())


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Allow ForkMesh's network connections in OpenSnitch, so new ForkMesh builds, "
                    "updated agent programs and test programs never raise a firewall prompt (or time out into a deny rule).")
    parser.add_argument("command", choices=["print", "write", "install"],
                        help="print the rules, write them to --out, or install them into "
                             f"{RULES_DIR} (run with sudo)")
    parser.add_argument("--user", help="the account whose programs are allowed (default: the "
                                       "sudo caller, else the current user)")
    parser.add_argument("--out", type=Path, help="folder for the write command")
    parser.add_argument("--rules-dir", type=Path, default=RULES_DIR,
                        help="OpenSnitch rules folder for the install command")
    parser.add_argument("--no-loopback", action="store_true",
                        help="leave connections to 127.0.0.1 and ::1 to other rules")
    parser.add_argument("--no-dev-trees", action="store_true",
                        help="do not allow every program built under ~/projects/forkmesh "
                             "and ~/.cache/fm*")
    parser.add_argument("--no-agent-clis", action="store_true",
                        help="do not allow every version of Claude Code and Codex")
    args = parser.parse_args(argv)
    account = target_user(args.user)
    rules = build_rules(account.pw_uid, Path(account.pw_dir), loopback=not args.no_loopback,
                        dev_trees=False if args.no_dev_trees else None,
                        agent_clis=not args.no_agent_clis)
    if args.command == "print":
        print(json.dumps(rules, indent=2))
        return 0
    if args.command == "write":
        if not args.out:
            parser.error("write needs --out")
        for path in write_rules(rules, args.out):
            print(path)
        return 0
    if not args.rules_dir.is_dir():
        print(f"{args.rules_dir} does not exist; is OpenSnitch installed?", file=sys.stderr)
        return 2
    if os.geteuid() != 0 and not os.access(args.rules_dir, os.W_OK):
        print(f"{args.rules_dir} needs root: sudo python3 {sys.argv[0]} install", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory() as scratch:
        for path in write_rules(rules, Path(scratch)):
            destination = args.rules_dir / path.name
            shutil.copyfile(path, destination)
            os.chmod(destination, 0o600)
            print(f"installed {destination}")
    print(f"OpenSnitch reloads {args.rules_dir} on its own; these rules take precedence over "
          "older per-path deny rules for the same programs.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
