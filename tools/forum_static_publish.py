#!/usr/bin/env python3
from __future__ import annotations

import argparse
import base64
import json
import os
import re
import subprocess
import sys
import urllib.parse
from pathlib import Path
from typing import Any

MANIFEST = "manifest.json"
NAME_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{0,39}$")
BRANCH_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]{0,100}$")
DEFAULT_AUTHOR = ("forum static export", "static-export@forkmesh.invalid")


class PublishError(RuntimeError):
    pass


def load_targets(path: Path | None, specs: list[str]) -> list[dict[str, Any]]:
    targets: list[dict[str, Any]] = []
    if path is not None:
        try:
            doc = json.loads(Path(path).read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise PublishError("cannot read %s: %s" % (path, error))
        items = doc.get("targets") if isinstance(doc, dict) else doc
        if not isinstance(items, list):
            raise PublishError("%s must hold a list of targets" % path)
        targets.extend(item for item in items if isinstance(item, dict))
    for spec in specs:
        name, _, rest = spec.partition("=")
        url, _, branch = rest.rpartition("#") if "#" in rest else (rest, "", "gh-pages")
        targets.append({"name": name, "url": url, "branch": branch or "gh-pages"})
    seen = set()
    for target in targets:
        name = str(target.get("name") or "")
        if not NAME_RE.match(name) or name in seen:
            raise PublishError("target name %r is not valid or is used twice" % name)
        seen.add(name)
        if not str(target.get("url") or "").strip():
            raise PublishError("target %s has no url" % name)
        target.setdefault("branch", "gh-pages")
        if not BRANCH_RE.match(str(target["branch"])) or ".." in str(target["branch"]):
            raise PublishError("target %s has an invalid branch" % name)
        if re.search(r"://[^/@]*:[^/@]*@", str(target["url"])):
            raise PublishError("target %s has a password in its url; use tokenFile or tokenEnv" % name)
    return targets


def env_key(name: str) -> str:
    return re.sub(r"[^A-Z0-9]", "_", name.upper())


def credentials(target: dict[str, Any], environ: dict[str, str]) -> tuple[dict[str, str], list[str]]:
    env: dict[str, str] = {"GIT_TERMINAL_PROMPT": "0", "GIT_ASKPASS": "true", "SSH_ASKPASS": "true"}
    secrets: list[str] = []
    url = str(target["url"])
    key = env_key(target["name"])
    token = ""
    token_env = str(target.get("tokenEnv") or "FORKMESH_PUBLISH_TOKEN_" + key)
    if environ.get(token_env):
        token = environ[token_env].strip()
    elif target.get("tokenFile"):
        try:
            token = Path(str(target["tokenFile"])).read_text(encoding="utf-8").strip()
        except OSError as error:
            raise PublishError("target %s: cannot read its token file: %s" % (target["name"], error))
    config: list[tuple[str, str]] = [("credential.helper", ""), ("core.autocrlf", "false")]
    if token:
        parts = urllib.parse.urlsplit(url)
        if parts.scheme != "https":
            raise PublishError("target %s: a token needs an https url" % target["name"])
        user = str(target.get("username") or environ.get("FORKMESH_PUBLISH_USER_" + key) or "x-access-token")
        basic = base64.b64encode(("%s:%s" % (user, token)).encode()).decode()
        config.append(("http.%s://%s/.extraHeader" % (parts.scheme, parts.netloc),
                       "Authorization: Basic " + basic))
        secrets += [token, basic]
    ssh_key = str(target.get("sshKey") or environ.get("FORKMESH_PUBLISH_SSH_KEY_" + key) or "")
    if ssh_key:
        command = ["ssh", "-i", ssh_key, "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes"]
        if target.get("knownHosts"):
            command += ["-o", "UserKnownHostsFile=" + str(target["knownHosts"]),
                        "-o", "StrictHostKeyChecking=yes"]
        env["GIT_SSH_COMMAND"] = " ".join(_quote(part) for part in command)
    env["GIT_CONFIG_COUNT"] = str(len(config))
    for index, (name, value) in enumerate(config):
        env["GIT_CONFIG_KEY_%d" % index] = name
        env["GIT_CONFIG_VALUE_%d" % index] = value
    return env, secrets


def _quote(text: str) -> str:
    return text if re.fullmatch(r"[A-Za-z0-9_./=:@-]+", text) else "'" + text.replace("'", "'\\''") + "'"


class Git:
    def __init__(self, git_dir: Path, env: dict[str, str], secrets: list[str]) -> None:
        self.git_dir = git_dir
        self.env = {**os.environ, **env}
        for name in list(self.env):
            if name.startswith("GIT_CONFIG_") and name not in env:
                del self.env[name]
        self.secrets = [s for s in secrets if s]

    def clean(self, text: str) -> str:
        for secret in self.secrets:
            text = text.replace(secret, "***")
        return text

    def run(self, *args: str, stdin: str | None = None, extra: dict[str, str] | None = None,
            check: bool = True) -> subprocess.CompletedProcess:
        env = {**self.env, **(extra or {})}
        result = subprocess.run(["git", "--git-dir", str(self.git_dir), *args], input=stdin,
                                capture_output=True, text=True, env=env, timeout=1800)
        if check and result.returncode != 0:
            raise PublishError("git %s failed: %s" % (args[0], self.clean(result.stderr.strip())[-600:]))
        return result


def site_files(site: Path) -> list[str]:
    try:
        manifest = json.loads((site / MANIFEST).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        raise PublishError("%s has no readable %s; run forum_static_export.py first" % (site, MANIFEST))
    names = sorted(set(manifest.get("files") or {}) | {MANIFEST})
    for name in names:
        parts = name.split("/")
        if name.startswith("/") or any(part in ("", ".", "..", ".git") for part in parts):
            raise PublishError("manifest lists an unsafe path %r" % name)
        if not (site / name).is_file():
            raise PublishError("manifest lists %s, which is missing; run the export again" % name)
    return names


def build_tree(git: Git, site: Path, names: list[str], index: Path) -> str:
    extra = {"GIT_INDEX_FILE": str(index)}
    git.run("read-tree", "--empty", extra=extra)
    shas = git.run("hash-object", "-w", "--no-filters", "--stdin-paths",
                   stdin="".join(str(site / name) + "\n" for name in names)).stdout.split()
    if len(shas) != len(names):
        raise PublishError("git hash-object returned %d ids for %d files" % (len(shas), len(names)))
    git.run("update-index", "--add", "--index-info", extra=extra,
            stdin="".join("100644 %s\t%s\n" % (sha, name) for sha, name in zip(shas, names)))
    return git.run("write-tree", extra=extra).stdout.strip()


def publish_target(site: Path, target: dict[str, Any], cache: Path, *, dry_run: bool,
                   message: str, author: tuple[str, str], environ: dict[str, str]) -> dict[str, Any]:
    name, url, branch = target["name"], str(target["url"]), str(target["branch"])
    env, secrets = credentials(target, environ)
    git_dir = cache / ("%s.git" % name)
    if not (git_dir / "HEAD").is_file():
        cache.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "init", "--quiet", "--bare", str(git_dir)], check=True,
                       capture_output=True)
    git = Git(git_dir, env, secrets)
    report: dict[str, Any] = {"target": name, "branch": branch, "dryRun": dry_run}
    listed = git.run("ls-remote", "--heads", url, "refs/heads/" + branch, check=False)
    if listed.returncode != 0:
        raise PublishError("cannot reach target %s: %s" % (name, git.clean(listed.stderr.strip())[-400:]))
    parent = listed.stdout.split()[0] if listed.stdout.strip() else ""
    tracking = "refs/remotes/target/" + branch
    if parent:
        git.run("fetch", "--quiet", "--no-tags", url, "+refs/heads/%s:%s" % (branch, tracking))
        if git.run("rev-parse", tracking).stdout.strip() != parent:
            raise PublishError("target %s moved while fetching; try again" % name)
    names = site_files(site)
    tree = build_tree(git, site, names, git_dir / "export-index")
    report.update({"parent": parent, "tree": tree, "files": len(names)})
    if parent and git.run("rev-parse", parent + "^{tree}").stdout.strip() == tree:
        report["status"] = "up-to-date"
        return report
    if parent:
        changed = git.run("diff-tree", "-r", "--name-only", "--no-commit-id", parent, tree).stdout.split("\n")
        report["changed"] = len([line for line in changed if line])
    else:
        report["changed"] = len(names)
    who = {"GIT_AUTHOR_NAME": author[0], "GIT_AUTHOR_EMAIL": author[1],
           "GIT_COMMITTER_NAME": author[0], "GIT_COMMITTER_EMAIL": author[1]}
    args = ["commit-tree", tree, "-m", message] + (["-p", parent] if parent else [])
    commit = git.run(*args, extra=who).stdout.strip()
    report["commit"] = commit
    if dry_run:
        report["status"] = "would-push"
        return report
    git.run("update-ref", "refs/heads/" + branch, commit)
    pushed = git.run("push", "--quiet", "--porcelain", url, "%s:refs/heads/%s" % (commit, branch), check=False)
    if pushed.returncode != 0:
        raise PublishError("target %s refused the push (it may have moved; the next run commits on top): %s" % (
            name, git.clean((pushed.stderr or pushed.stdout).strip())[-400:]))
    report["status"] = "pushed"
    return report


def commit_message(site: Path) -> str:
    try:
        manifest = json.loads((site / MANIFEST).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        manifest = {}
    source = manifest.get("source") if isinstance(manifest.get("source"), dict) else {}
    lines = ["Static copy: %s threads, %s comments" % (manifest.get("threads", "?"), manifest.get("comments", "?"))]
    if source.get("url"):
        lines.append("\nFrom %s" % source["url"])
    if source.get("commit"):
        lines.append("Source commit %s" % source["commit"])
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Commit a forum_static_export.py site on top of a branch "
                                                 "of one or more git remotes (GitHub Pages, Codeberg Pages, "
                                                 "any git URL) and push it. Never force-pushes.")
    parser.add_argument("--site", type=Path, required=True)
    parser.add_argument("--targets", type=Path, help="JSON file: {\"targets\": [{name, url, branch, ...}]}")
    parser.add_argument("--target", action="append", default=[], metavar="NAME=URL#BRANCH")
    parser.add_argument("--only", action="append", default=[], metavar="NAME")
    parser.add_argument("--cache", type=Path, default=None,
                        help="folder for the local copies of each target (default: <site>/../.publish-cache)")
    parser.add_argument("--message", default="")
    parser.add_argument("--author-name", default=DEFAULT_AUTHOR[0])
    parser.add_argument("--author-email", default=DEFAULT_AUTHOR[1])
    parser.add_argument("--dry-run", action="store_true", help="build the commit but push nothing")
    args = parser.parse_args(argv)
    try:
        targets = load_targets(args.targets, args.target)
    except PublishError as error:
        print(json.dumps({"ok": False, "error": str(error)}))
        return 2
    if args.only:
        targets = [t for t in targets if t["name"] in args.only]
    if not targets:
        print(json.dumps({"ok": False, "error": "no targets"}))
        return 2
    site = args.site.resolve()
    cache = (args.cache or site.parent / ".publish-cache").resolve()
    if cache == site or site in cache.parents:
        print(json.dumps({"ok": False, "error": "the cache folder must be outside the site"}))
        return 2
    message = args.message or commit_message(site)
    results, failed = [], 0
    for target in targets:
        try:
            results.append(publish_target(site, target, cache, dry_run=args.dry_run, message=message,
                                          author=(args.author_name, args.author_email),
                                          environ=dict(os.environ)))
        except (PublishError, subprocess.SubprocessError, OSError) as error:
            failed += 1
            results.append({"target": target["name"], "status": "failed", "error": str(error)})
    print(json.dumps({"ok": not failed, "results": results}, indent=1))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
