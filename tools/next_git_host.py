#!/usr/bin/env python3
"""Private Git over HTTPS on the Next core nodes (for Codemagic builds).

    next_git_host.py setup [nN ...] [--execute]   # dirs, password, Caddy snippet, reload
    next_git_host.py push  [nN ...] [--execute]   # main + tags to every node over SSH
    next_git_host.py url   [--node nN]            # clone URL, never the password

Each core node keeps a bare copy of this repository at
``/var/lib/forkmesh-next/git/forkmesh-next.git`` (root:caddy, dirs 0750,
files 0640). ``push`` sends ``main`` and the tags over SSH as root with the
fleet key, then runs ``git update-server-info`` so Git's dumb-HTTP protocol
can clone it through plain file serving. Caddy serves it under ``/git/*`` on
``nN.next.forkmesh.com`` and ``app.next.forkmesh.com`` behind basic auth
(user ``fmnext-ci``) from ``/etc/forkmesh-next/caddy/git-private.caddy``,
which the rendered Caddyfile imports by glob (deploy/core/render.py): a node
without the file has no ``/git/`` route at all.

The password is ``GIT_CI_PASSWORD`` in ``<config>/secrets.env`` (32 random
bytes, base64url; ``setup`` generates it when absent). It is never printed
or put on a command line: ``setup`` sends it on SSH stdin to
``caddy hash-password`` on each node, and only the bcrypt hash is written.
Rotating it: delete the line, run ``setup --execute``, then give the
Codemagic app the new URL (docs/OPERATIONS.md "Native app builds").

All SSH goes through ``next_common.Ssh`` (the fleet key, root, batch mode),
the same sessions every other next_* tool opens. **Dry-run is the default.**
"""

from __future__ import annotations

import argparse
import base64
import os
import secrets as _secrets
import shlex
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import next_common as common  # noqa: E402
from next_common import ToolError  # noqa: E402

GIT_USER = "fmnext-ci"
PASSWORD_NAME = "GIT_CI_PASSWORD"
GIT_ROOT = "/var/lib/forkmesh-next/git"
REPO_NAME = "forkmesh-next.git"
REPO_PATH = GIT_ROOT + "/" + REPO_NAME
SNIPPET_PATH = "/etc/forkmesh-next/caddy/git-private.caddy"
URL_PATH = "/git/" + REPO_NAME
REFSPECS = ("refs/heads/main:refs/heads/main", "refs/tags/*:refs/tags/*")


def ssh_known_hosts() -> Path:
    configured = os.environ.get("FMNEXT_KNOWN_HOSTS")
    if configured:
        return Path(configured)
    return common.ssh_key_path().parent / "known_hosts"


def make_ssh():
    return common.Ssh(known_hosts=ssh_known_hosts())


# --------------------------------------------------------------------------
# the password (never printed)


def secrets_path() -> Path:
    return common.config_dir() / "secrets.env"


def generate_password() -> str:
    return base64.urlsafe_b64encode(_secrets.token_bytes(32)).decode().rstrip("=")


def read_password(required=True) -> str:
    path = secrets_path()
    values = common.read_env_file(path) if path.is_file() else {}
    value = values.get(PASSWORD_NAME, "")
    if not value and required:
        raise ToolError("%s is not set in %s (run setup --execute)" % (PASSWORD_NAME, path))
    return value


def ensure_password(runner) -> str | None:
    """The existing password, or a new one appended to secrets.env (0600)."""
    existing = read_password(required=False)
    if existing:
        return existing
    path = secrets_path()

    def write():
        value = generate_password()
        text = path.read_text("utf-8") if path.is_file() else ""
        if text and not text.endswith("\n"):
            text += "\n"
        text += "%s=%s\n" % (PASSWORD_NAME, value)
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_name(path.name + ".tmp")
        fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            handle.write(text)
        os.replace(temporary, path)
        return value
    return runner.step("generate %s in %s (never printed)" % (PASSWORD_NAME, path), write)


# --------------------------------------------------------------------------
# node scripts


def _snippet_text(password_hash: str, root: str) -> str:
    return "\n".join([
        "# Managed by tools/next_git_host.py. Private Git (dumb HTTP) for CI:",
        "# every path under /git/ needs the %s basic-auth password." % GIT_USER,
        "handle /git/* {",
        "\tbasic_auth {",
        "\t\t%s %s" % (GIT_USER, password_hash),
        "\t}",
        "\theader Cache-Control no-store",
        "\turi strip_prefix /git",
        "\troot * %s" % root,
        "\tfile_server",
        "}",
        "",
    ])


def snippet(password_hash: str, root: str = GIT_ROOT) -> str:
    """The Caddy handle; the rendered Caddyfile imports it inside the
    per-node and app.next site blocks."""
    if not password_hash.startswith("$2") or any(c.isspace() for c in password_hash):
        raise ToolError("not a bcrypt hash")
    return _snippet_text(password_hash, root)


def fix_perms_script() -> str:
    q = shlex.quote
    return "\n".join([
        "chown -R root:caddy %s" % q(GIT_ROOT),
        "find %s -type d -exec chmod 0750 {} +" % q(GIT_ROOT),
        "find %s -type f -exec chmod 0640 {} +" % q(GIT_ROOT),
    ])


def setup_script() -> str:
    """Runs as root on a node; the password arrives on stdin (one line)."""
    q = shlex.quote
    snip = q(SNIPPET_PATH)
    return "\n".join([
        "IFS= read -r pw",
        "[ -n \"$pw\" ] || { echo 'no password on stdin' >&2; exit 1; }",
        "install -d -o root -g caddy -m 0750 %s" % q(GIT_ROOT),
        "if [ ! -e %s/HEAD ]; then git init -q --bare --initial-branch=main %s; fi"
        % (q(REPO_PATH), q(REPO_PATH)),
        "git -C %s update-server-info" % q(REPO_PATH),
        fix_perms_script(),
        # The plaintext goes to caddy on a pipe from a shell builtin: never
        # argv, the environment or a file.
        "hash=$(printf '%s\\n' \"$pw\" | caddy hash-password)",
        "unset pw",
        "case \"$hash\" in '$2'*) ;; *) echo 'caddy hash-password failed' >&2; exit 1;; esac",
        "tmp=$(mktemp /etc/forkmesh-next/caddy/.git-private.XXXXXX)",
        "trap 'rm -f \"$tmp\"' EXIT",
        "cat >\"$tmp\" <<'FMNEXT_EOF'",
        _snippet_text("@HASH@", GIT_ROOT).rstrip("\n"),
        "FMNEXT_EOF",
        # A bcrypt hash has '$', '.' and '/', never '|'.
        "sed -i \"s|@HASH@|$hash|\" \"$tmp\"",
        "chown root:caddy \"$tmp\" && chmod 0640 \"$tmp\"",
        "mv -f \"$tmp\" %s" % snip,
        "trap - EXIT",
        # Parse only (no provisioning); a failed reload keeps the old config
        # running, so drop the snippet then: a later restart must not fail.
        "if ! caddy adapt --config /etc/caddy/Caddyfile --adapter caddyfile >/dev/null 2>&1 "
        "|| ! systemctl reload caddy; then rm -f %s; systemctl reload caddy || true; "
        "echo 'Caddy refused the git-private snippet; removed it' >&2; exit 1; fi" % snip,
    ])


def after_push_script() -> str:
    return "\n".join([
        "git -C %s gc --auto --quiet" % shlex.quote(REPO_PATH),
        "git -C %s update-server-info" % shlex.quote(REPO_PATH),
        fix_perms_script(),
        "git -C %s rev-parse refs/heads/main" % shlex.quote(REPO_PATH),
    ])


# --------------------------------------------------------------------------
# commands


def selected(fleet, names):
    nodes = common.core_nodes(fleet)
    if not names:
        return nodes
    by_name = {n["name"]: n for n in nodes}
    missing = [n for n in names if n not in by_name]
    if missing:
        raise ToolError("unknown core node(s): %s" % ", ".join(missing))
    return [by_name[n] for n in names]


def setup(fleet, ssh, runner, names=None) -> int:
    password = ensure_password(runner)
    for node in selected(fleet, names):
        ip = node["public_ipv4"]

        def apply(ip=ip):
            ssh.run(ip, setup_script(), input_bytes=(password + "\n").encode())
        runner.step("%s (%s): %s (root:caddy 0750), %s with a fresh bcrypt hash "
                    "(root:caddy 0640), caddy adapt + reload"
                    % (node["name"], ip, REPO_PATH, SNIPPET_PATH), apply)
    return 0


def git_ssh_command(ssh, ip=None) -> str:
    return " ".join(shlex.quote(part) for part in [
        "ssh", "-i", str(ssh.key), "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes",
        "-o", "ConnectTimeout=10", "-o", "StrictHostKeyChecking=yes",
        "-o", "UserKnownHostsFile=%s" % ssh.known_hosts,
        *(ssh.dest(ip) if ip and hasattr(ssh, "dest") else [])])


def push(fleet, ssh, runner, names=None, repo=None, run=subprocess.run) -> int:
    repo = Path(repo or common.REPO)
    head = run(["git", "-C", str(repo), "rev-parse", "refs/heads/main"],
               capture_output=True, text=True, check=False)
    if head.returncode != 0:
        raise ToolError("%s has no main branch" % repo)
    main = head.stdout.strip()
    failed = []
    for node in selected(fleet, names):
        ip = node["public_ipv4"]

        def apply(ip=ip, name=node["name"]):
            env = dict(os.environ, GIT_SSH_COMMAND=git_ssh_command(ssh, ip))
            completed = run(["git", "-C", str(repo), "push", "--quiet",
                             "root@%s:%s" % (ip, REPO_PATH), *REFSPECS],
                            env=env, capture_output=True, text=True, check=False)
            if completed.returncode != 0:
                failed.append(name)
                runner.say("  %s: git push failed: %s" % (name, completed.stderr.strip()[-500:]))
                return
            out = ssh.run(ip, after_push_script())
            served = (out.stdout or b"").decode().strip().splitlines()[-1:]
            ok = served == [main]
            if not ok:
                failed.append(name)
            runner.say("  %s: main %s %s" % (name, main[:12], "served" if ok else
                                              "NOT served (%s)" % served))
        runner.step("%s (%s): git push main %s + tags, update-server-info, perms"
                    % (node["name"], ip, main[:12]), apply)
    if failed:
        raise ToolError("push failed on %s" % ", ".join(failed))
    return 0


def clone_url(fleet, node=None) -> str:
    host = fleet["app_host"] if not node else "%s.%s" % (node, fleet["domain"])
    return "https://%s@%s%s" % (GIT_USER, host, URL_PATH)


def main(argv=None, ssh=None, run=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--fleet")
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("setup", "push"):
        p = sub.add_parser(name)
        p.add_argument("nodes", nargs="*")
        common.add_execute_flags(p)
    p = sub.add_parser("url")
    p.add_argument("--node", help="nN: the per-node name instead of app.next")
    args = parser.parse_args(argv)
    try:
        fleet = common.load_fleet(args.fleet)
        if args.command == "url":
            print(clone_url(fleet, args.node))
            return 0
        runner = common.runner_from_args(args)
        ssh = ssh or make_ssh()
        if args.command == "setup":
            code = setup(fleet, ssh, runner, args.nodes)
        else:
            code = push(fleet, ssh, runner, args.nodes, run=run or subprocess.run)
        if not runner.execute:
            runner.say("(dry run: nothing was changed; pass --execute to apply)")
        return code
    except (ToolError, OSError, KeyError, subprocess.SubprocessError) as error:
        print("next_git_host: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
