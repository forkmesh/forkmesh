#!/usr/bin/env python3
import argparse
import json
import re
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import forkmesh_mcp_server as mcp

SIGNING_DOMAIN = "forkmesh-desktop-command-v1"
COMMANDS = {"create": "release.create", "publish": "release.publish"}
SEMVER_TAG_RE = re.compile(r"^v(\d+)\.(\d+)\.(\d+)$")


def next_tag(repo_dir):
    out = subprocess.run(
        ["git", "-C", str(repo_dir), "tag", "--list", "v*", "--sort=-v:refname"],
        capture_output=True, text=True, check=True).stdout.split()
    for tag in out:
        match = SEMVER_TAG_RE.match(tag)
        if match:
            major, minor, patch = (int(part) for part in match.groups())
            return f"v{major}.{minor}.{patch + 1}"
    raise ValueError("no vX.Y.Z tag to count from; pass --tag")


def tag_exists(repo_dir, tag):
    return subprocess.run(
        ["git", "-C", str(repo_dir), "rev-parse", "--verify", "--quiet",
         "refs/tags/" + tag], capture_output=True).returncode == 0


def canonical(owner, repo, command, target, ts):
    return "\n".join((SIGNING_DOMAIN, owner, repo, command, target, ts)).encode()


def build_request(action, tag, repo, account, sign, ts):
    command = COMMANDS[action]
    target = "release:" + tag
    return {
        "ownerAccount": account,
        "command": command,
        "target": target,
        "ts": ts,
        "sig": mcp.b64url(sign(canonical(account, repo, command, target, ts))),
        "payload": {"tag": tag},
    }


def post(origin, account, repo, body):
    url = f"{origin}/api/repo/{account}/{repo}/desktop-commands"
    request = urllib.request.Request(
        url, data=json.dumps(body).encode(), method="POST",
        headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=30) as reply:
            return reply.status, json.loads(reply.read() or b"{}")
    except urllib.error.HTTPError as error:
        try:
            return error.code, json.loads(error.read() or b"{}")
        except ValueError:
            return error.code, {}


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Ask this account's desktop to cut or publish a release, "
                    "signed with this device's identity key.")
    parser.add_argument("action", choices=sorted(COMMANDS),
                        help="create: cut the tag with the dialog's defaults; "
                             "publish: push an existing tag to mirrors")
    parser.add_argument("--tag", help="release tag (create defaults to the next vX.Y.Z)")
    parser.add_argument("--repository", default="forkmesh",
                        help="repository name under the signed-in account")
    parser.add_argument("--repo-dir", default=str(mcp.default_repo()),
                        help="checkout used to work out the next tag")
    parser.add_argument("--dry-run", action="store_true",
                        help="print the request instead of sending it")
    args = parser.parse_args(argv)

    repo_dir = Path(args.repo_dir)
    tag = mcp.release_tag(args.tag, args.action == "publish") or next_tag(repo_dir)
    if args.action == "create" and tag_exists(repo_dir, tag):
        parser.error(f"{tag} already exists")
    account = mcp.crm_account()
    key, _ = mcp.load_identity()
    body = build_request(args.action, tag, args.repository, account, key.sign,
                         str(mcp.now_ms()))
    if args.dry_run:
        print(json.dumps(body, indent=2))
        return 0
    status, reply = post(mcp.chat_origin(), account, args.repository, body)
    print(json.dumps({"status": status, "tag": tag, "reply": reply}))
    return 0 if status == 200 and reply.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
