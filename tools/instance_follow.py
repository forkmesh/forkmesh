#!/usr/bin/env python3
import argparse
import ast
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

GENERATED = (
    re.compile(r"^app/public/dashboard\.js$"),
    re.compile(r"^app/public/dashboard-page-[^/]+\.js$"),
    re.compile(r"^app/public/dashboard/.+/index\.html$"),
)
UNION = ("changelog/changelog.json", "www/public/changelog.html")
ENTRY = "app/src/entry.py"
MIGRATION_TEST = "app/tests/test_local_migrations.py"
LATEST_ASSERT = re.compile(r'(assert migration_name == ")\d{4}_[a-z0-9_]+\.sql(")')
SCHEMA = "app/src/schema.py"
MIGRATIONS = "app/migrations"
MIGRATION_NAME = re.compile(r"^(\d{4})_([a-z0-9_]+)\.sql$")
STAMP = re.compile(r"schema_fingerprint:[0-9a-f]{64}")
ENTRY_FINGERPRINT = re.compile(
    r'(_SCHEMA_FINGERPRINT = \(\s*")([0-9a-f]{64})("\s*\))')
FINGERPRINT_LINE = re.compile(r'\s*"[0-9a-f]{64}"\s*')
CONFLICT = re.compile(
    r"<<<<<<< [^\n]*\n(.*?)=======\n(.*?)>>>>>>> [^\n]*\n", re.S)
MARKER = re.compile(r"^(<<<<<<<|>>>>>>>) ", re.M)
GIT_TIMEOUT = 30 * 60


class Stop(Exception):
    def __init__(self, state, message, **extra):
        super().__init__(message)
        self.result = dict(extra, state=state, message=message)


def git(work, *args, check=True, timeout=GIT_TIMEOUT):
    completed = subprocess.run(
        ["git", "-C", str(work), *args], capture_output=True, text=True,
        timeout=timeout, check=False)
    if check and completed.returncode != 0:
        raise Stop("failed", "git %s failed: %s" % (
            args[0], (completed.stderr or completed.stdout).strip()[-300:]))
    return completed.stdout


def prepare(work, args):
    if not (work / ".git").is_dir():
        if work.exists() and any(work.iterdir()):
            raise Stop("failed", "%s is not a follow work tree." % work)
        work.mkdir(parents=True, exist_ok=True)
        git(work, "init", "--quiet", "--initial-branch=main")
    elif git(work, "for-each-ref", "--count=1").strip() and not git(
            work, "for-each-ref", "--count=1", "refs/follow/").strip():
        raise Stop("failed", "%s is not a follow work tree." % work)
    git(work, "config", "user.name", args.name)
    git(work, "config", "user.email", args.email)
    git(work, "config", "core.hooksPath", "/dev/null")
    git(work, "merge", "--abort", check=False)
    options = ["-c", "http.curloptResolve=" + args.resolve] if args.resolve \
        else []
    for url, ref, local in ((args.source_url, args.source_ref, "source"),
                            (args.follow_url, args.follow_ref, "upstream")):
        completed = subprocess.run(
            ["git", *options, "-C", str(work), "fetch", "--quiet",
             "--no-tags", url,
             "+refs/heads/%s:refs/follow/%s" % (ref, local)],
            capture_output=True, text=True, timeout=GIT_TIMEOUT, check=False)
        if completed.returncode != 0:
            raise Stop("failed", "Could not fetch %s: %s" % (
                url, completed.stderr.strip()[-200:]))
    git(work, "checkout", "--force", "--quiet", "-B", "main",
        "refs/follow/source")
    git(work, "clean", "-fdxq")


def rev(work, name):
    return git(work, "rev-parse", "--verify", name + "^{commit}").strip()


def conflicted(work):
    return sorted(filter(None, git(
        work, "diff", "--name-only", "--diff-filter=U").splitlines()))


def generated(path):
    return any(pattern.match(path) for pattern in GENERATED)


def union(work, path):
    file = work / path
    text = file.read_text(encoding="utf-8")

    def keep_both(match):
        ours, theirs = match.group(1), match.group(2)
        if path.endswith(".json") and ours.strip() and theirs.strip() \
                and not ours.rstrip().endswith(","):
            ours = ours.rstrip("\n") + ",\n"
        return ours + theirs

    text = CONFLICT.sub(keep_both, text)
    if MARKER.search(text):
        return False
    if path.endswith(".json"):
        try:
            json.loads(text)
        except ValueError:
            return False
    file.write_text(text, encoding="utf-8")
    git(work, "add", "--", path)
    return True


def entry_only_fingerprint(work):
    file = work / ENTRY
    text = file.read_text(encoding="utf-8")
    blocks = CONFLICT.findall(text)
    if not blocks or not all(
            FINGERPRINT_LINE.fullmatch(ours) and FINGERPRINT_LINE.fullmatch(theirs)
            for ours, theirs in blocks):
        return False
    file.write_text(CONFLICT.sub(lambda m: m.group(1), text),
                    encoding="utf-8")
    git(work, "add", "--", ENTRY)
    return True


def latest_only(work):
    file = work / MIGRATION_TEST
    text = file.read_text(encoding="utf-8")
    blocks = CONFLICT.findall(text)
    if not blocks or not all(
            LATEST_ASSERT.fullmatch(ours.strip())
            and LATEST_ASSERT.fullmatch(theirs.strip())
            for ours, theirs in blocks):
        return False
    file.write_text(CONFLICT.sub(lambda m: m.group(1), text),
                    encoding="utf-8")
    git(work, "add", "--", MIGRATION_TEST)
    return True


def resolve_conflicts(work):
    unresolved, rebuild = [], False
    for path in conflicted(work):
        if generated(path):
            rebuild = True
            if git(work, "ls-tree", "MERGE_HEAD", "--", path).strip():
                git(work, "checkout", "--theirs", "--", path)
                git(work, "add", "--", path)
            else:
                git(work, "rm", "--quiet", "--force", "--", path)
        elif path in UNION and union(work, path):
            continue
        elif path == ENTRY and entry_only_fingerprint(work):
            continue
        elif path == MIGRATION_TEST and latest_only(work):
            continue
        else:
            unresolved.append(path)
    return unresolved, rebuild


def migration_files(work, tree):
    names = git(work, "ls-tree", "--name-only", tree,
                MIGRATIONS + "/").splitlines()
    found = {}
    for name in names:
        match = MIGRATION_NAME.match(name.rsplit("/", 1)[-1])
        if match:
            found[match.group(2)] = int(match.group(1))
    return found


def renumber_migrations(work, ours_tree):
    ours = migration_files(work, ours_tree)
    top = max(ours.values(), default=0)
    current = {}
    for file in (work / MIGRATIONS).glob("*.sql"):
        match = MIGRATION_NAME.match(file.name)
        if match:
            current[file.name] = (int(match.group(1)), match.group(2))
    renames, added = {}, []
    for name, (number, suffix) in sorted(current.items()):
        if suffix in ours:
            if number != ours[suffix]:
                for folder in (MIGRATIONS, MIGRATIONS + "/down"):
                    if (work / folder / name).exists():
                        git(work, "rm", "--quiet", "--force", "--",
                            folder + "/" + name)
            continue
        added.append((number, name, suffix))
    for number, name, suffix in sorted(added):
        if number > top:
            top = number
            continue
        top += 1
        new = "%04d_%s.sql" % (top, suffix)
        for folder in (MIGRATIONS, MIGRATIONS + "/down"):
            if (work / folder / name).exists():
                git(work, "mv", "--", folder + "/" + name, folder + "/" + new)
        renames[name[:-4]] = new[:-4]
    if renames:
        pattern = re.compile("|".join(map(re.escape, renames)))
        for path in git(work, "grep", "-I", "-l", "-E", "|".join(
                map(re.escape, renames)), check=False).splitlines():
            if path.startswith(MIGRATIONS + "/"):
                continue
            file = work / path
            text = file.read_text(encoding="utf-8")
            file.write_text(pattern.sub(lambda m: renames[m.group(0)], text),
                            encoding="utf-8")
            git(work, "add", "--", path)
    return renames, [name for _number, name, _suffix in added]


def literal(path, name):
    tree = ast.parse(path.read_text(encoding="utf-8"))
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(t, ast.Name) and t.id == name for t in node.targets):
            return ast.literal_eval(node.value)
    raise Stop("conflict", "%s has no %s" % (path.name, name))


def runtime_fingerprint(work):
    entry = work / ENTRY
    statements = (literal(entry, "SCHEMA_PRE_CREATE_ALTER_STATEMENTS")
                  + literal(work / SCHEMA, "SCHEMA_STATEMENTS")
                  + literal(entry, "SCHEMA_ALTER_STATEMENTS"))
    return hashlib.sha256("\n".join(statements).encode("utf-8")).hexdigest()


def restamp(work, added):
    fingerprint = runtime_fingerprint(work)
    entry = work / ENTRY
    text = entry.read_text(encoding="utf-8")
    stamped = ENTRY_FINGERPRINT.search(text)
    if not stamped:
        raise Stop("conflict", "entry.py has no schema fingerprint literal",
                   files=[ENTRY])
    newest = sorted(file.name for file in (work / MIGRATIONS).glob("*.sql")
                    if MIGRATION_NAME.match(file.name))[-1]
    if newest not in added:
        if stamped.group(2) != fingerprint:
            raise Stop("conflict", "The merged schema changed without a new "
                       "ForkMesh migration to stamp it.", files=[ENTRY])
        return fingerprint
    for folder in (MIGRATIONS, MIGRATIONS + "/down"):
        file = work / folder / newest
        if file.exists():
            file.write_text(STAMP.sub("schema_fingerprint:" + fingerprint,
                                      file.read_text(encoding="utf-8")),
                            encoding="utf-8")
            git(work, "add", "--", folder + "/" + newest)
    entry.write_text(ENTRY_FINGERPRINT.sub(
        lambda m: m.group(1) + fingerprint + m.group(3), text),
        encoding="utf-8")
    git(work, "add", "--", ENTRY)
    test = work / MIGRATION_TEST
    if test.exists():
        test.write_text(LATEST_ASSERT.sub(
            lambda m: m.group(1) + newest + m.group(2),
            test.read_text(encoding="utf-8")), encoding="utf-8")
        git(work, "add", "--", MIGRATION_TEST)
    return fingerprint


def rebuild_dashboard(work):
    completed = subprocess.run(
        [sys.executable, "app/tools/build_dashboard_assets.py"], cwd=work,
        capture_output=True, text=True, timeout=600, check=False)
    if completed.returncode != 0:
        raise Stop("conflict", "Rebuilding the dashboard failed: " +
                   (completed.stderr or completed.stdout).strip()[-200:],
                   files=["app/public/dashboard.js"])
    git(work, "add", "--all", "--", "app/public")


def check(work, base):
    changed = git(work, "diff", "--cached", "--name-only",
                  "--diff-filter=AM", base).splitlines()
    marked = [path for path in changed if (work / path).is_file()
              and not path.endswith((".png", ".jpg", ".gif", ".webp"))
              and MARKER.search((work / path).read_text(
                  encoding="utf-8", errors="replace"))]
    if marked:
        raise Stop("conflict", "Conflict markers remain.", files=marked)
    python = [path for path in changed if path.endswith(".py")]
    if python:
        completed = subprocess.run(
            [sys.executable, "-m", "py_compile", *python], cwd=work,
            capture_output=True, text=True, timeout=600, check=False)
        if completed.returncode != 0:
            raise Stop("conflict", "Merged Python does not compile: " +
                       completed.stderr.strip()[-200:], files=python)
    node = shutil.which("node")
    bundle = work / "app/public/dashboard.js"
    if node and bundle.exists():
        completed = subprocess.run(
            [node, "--check", str(bundle)], capture_output=True, text=True,
            timeout=300, check=False)
        if completed.returncode != 0:
            raise Stop("conflict", "The merged dashboard bundle does not "
                       "parse.", files=["app/public/dashboard.js"])


def follow(args):
    work = Path(args.work)
    prepare(work, args)
    source, upstream = rev(work, "refs/follow/source"), rev(
        work, "refs/follow/upstream")
    behind = int(git(work, "rev-list", "--count",
                     source + ".." + upstream).strip() or 0)
    base = {"upstream": upstream, "source": source, "behind": behind}
    if behind == 0:
        return dict(base, state="current", commit=source)
    merged = subprocess.run(
        ["git", "-C", str(work), "merge", "--quiet", "--no-ff", "--no-commit",
         upstream], capture_output=True, text=True, timeout=GIT_TIMEOUT,
        check=False)
    if merged.returncode not in (0, 1):
        raise Stop("failed", "git merge failed: " + merged.stderr.strip()[-200:])
    unresolved, rebuild = resolve_conflicts(work)
    if unresolved:
        git(work, "merge", "--abort", check=False)
        raise Stop("conflict", "%d file%s need a manual sync." % (
            len(unresolved), "" if len(unresolved) == 1 else "s"),
            files=unresolved[:50], **base)
    try:
        renames, added = renumber_migrations(work, source)
        fingerprint = restamp(work, [renames.get(name[:-4], name[:-4]) + ".sql"
                                     for name in added])
        if rebuild or git(work, "diff", "--cached", "--name-only", source,
                          "--", "app/public/dashboard/").strip():
            rebuild_dashboard(work)
        check(work, source)
    except Stop as stop:
        git(work, "merge", "--abort", check=False)
        stop.result.update(base)
        raise
    except (OSError, ValueError):
        git(work, "merge", "--abort", check=False)
        raise
    git(work, "commit", "--quiet", "--no-verify", "-m",
        "Merge ForkMesh %s into %s" % (upstream[:9], args.source_ref))
    commit = rev(work, "HEAD")
    if args.publish:
        git(work, "push", "--quiet", "--force", args.publish,
            "HEAD:refs/heads/" + args.source_ref)
    return dict(base, state="merged", commit=commit, fingerprint=fingerprint,
                renumbered=renames)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Merge ForkMesh's branch into a self-hosted instance's "
        "source branch.")
    parser.add_argument("--work", required=True)
    parser.add_argument("--source-url", required=True)
    parser.add_argument("--source-ref", default="main")
    parser.add_argument("--follow-url", required=True)
    parser.add_argument("--follow-ref", default="main")
    parser.add_argument("--resolve", default="")
    parser.add_argument("--publish", default="")
    parser.add_argument("--name", default="ForkMesh follow")
    parser.add_argument("--email", default="noreply@localhost")
    args = parser.parse_args(argv)
    try:
        result = follow(args)
    except Stop as stop:
        result = stop.result
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        result = {"state": "failed", "message": str(exc)[:300]}
    print(json.dumps(result, sort_keys=True))
    return 0 if result["state"] in ("current", "merged") else 1


if __name__ == "__main__":
    sys.exit(main())
