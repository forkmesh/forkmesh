from __future__ import annotations

import asyncio
import hashlib
import json
import os
import pwd
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HOSTED_ROOT = "/var/lib/forkmesh/hosted"
APP_USER = "forkmesh-app"
NODE_USER = "forkmesh-node"
NAME_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,37}[a-z0-9])?$")
REPO_RE = re.compile(r"^[A-Za-z0-9._-]{1,64}$")
ID_RE = re.compile(r"^hr[0-9a-f]{24}$")
FORK_RE = re.compile(r"^([a-z0-9][a-z0-9._-]{0,79})/([A-Za-z0-9._-]{1,80})$")
OID_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
MAINTENANCE_MARKER = "forkmesh-maintenance-due"
DEFAULT_BRANCHES = ("main", "master", "trunk", "develop")
REF_UPDATES_MAX = 40
COMMIT_COUNT_LIMIT = 10000
GIT_TIMEOUT = 6 * 60 * 60


class RepoError(Exception):
    pass


def read_spec(args):
    owner = str(args.owner or "").strip().lower()
    organization = str(args.organization or "").strip().lower()
    name = str(args.name or "").strip()
    if not NAME_RE.fullmatch(owner):
        raise RepoError("invalid --owner")
    if organization and not NAME_RE.fullmatch(organization):
        raise RepoError("invalid --organization")
    if (not REPO_RE.fullmatch(name) or name.startswith(".")
            or name.lower().endswith(".git")):
        raise RepoError("invalid --name")
    source = Path(args.source).expanduser()
    if not source.exists():
        raise RepoError("--from does not exist")
    root = Path(args.hosted_root)
    if not root.is_absolute():
        raise RepoError("--hosted-root must be absolute")
    fork_of = str(getattr(args, "fork_of", "") or "").strip().strip("/")
    if fork_of and not FORK_RE.fullmatch(fork_of.lower()):
        raise RepoError("invalid --fork-of (owner/repository)")
    if fork_of.lower() in ((owner + "/" + name).lower(),
                           (organization + "/" + name).lower()):
        raise RepoError("a repository cannot be a fork of itself")
    protect = getattr(args, "protect_default_branch", None)
    actions = getattr(args, "actions", None)
    return {
        "owner": owner,
        "organization": organization,
        "name": name,
        "description": " ".join(str(args.description or "").split())[:240],
        "visibility": "private" if args.visibility == "private" else "public",
        "source": str(source.resolve()),
        "root": str(root),
        "force": bool(args.force),
        "hooks": not args.no_hooks,
        "origin": str(args.origin or "").rstrip("/"),
        "forkOf": fork_of.lower(),
        "protect": protect if isinstance(protect, bool) else None,
        "actions": {"on": True, "off": False}.get(actions),
    }


def _modules(entry):
    hosted_git = entry._hosted_git._load()
    return hosted_git, hosted_git._hosted_repos, entry._core_push._load()


def _origin(env, spec):
    if spec["origin"]:
        return spec["origin"]
    for name in ("PUBLIC_BASE_URL", "APP_ORIGIN"):
        value = str(getattr(env, name, "") or "").strip().rstrip("/")
        if value.startswith(("https://", "http://")):
            return value
    raise RepoError("no PUBLIC_BASE_URL in the app settings; pass --origin")


async def _endpoint(hosted_git, env, repo_id, pushed_by=""):
    candidates = await hosted_git._core_candidates(env, repo_id, pushed_by)
    if not candidates:
        raise RepoError("no core git node is registered yet; start "
                        "forkmesh-mirror-node and retry")
    return candidates[0]


async def _check_member(entry, env, spec):
    if not spec["organization"]:
        return
    org_bi, org = await entry._org_row(env, spec["organization"])
    if not org:
        raise RepoError("organization_not_found")
    permission = await entry._org_permission(env, org_bi, spec["owner"])
    if entry.team_permission_rank(permission) < entry.team_permission_rank(
            "maintain"):
        raise RepoError("%s needs the maintain permission in %s"
                        % (spec["owner"], spec["organization"]))


async def prepare(runtime, spec):
    entry, env = runtime.entry, runtime.env
    hosted_git, hosted_repos, _ = _modules(entry)
    replica = hosted_git._ReplicaRuntime(env)
    await entry.ensure_schema(env)
    await hosted_repos.ensure_tables(replica)
    if not hosted_repos.enabled(env):
        raise RepoError("HOSTED_GIT_CORE_NODES is not set in the app settings")
    owner, name = spec["owner"], spec["name"]
    _, account = await entry._account_row(env, owner)
    if not account or entry._account_kind(account) != "user":
        raise RepoError("no user account named %s" % owner)
    await _check_member(entry, env, spec)
    key_bi = await entry.blind_index(env, owner + "/" + name)
    row = await hosted_repos.find_by_key(replica, key_bi)
    if row:
        data = await entry.decrypt_row(env, row.get("data"))
        linked = str((data or {}).get("organization") or "")
        if linked != spec["organization"]:
            raise RepoError("%s/%s exists with organization %r"
                            % (owner, name, linked))
        catalog = await entry.d1_first(
            env, "SELECT data FROM repositories WHERE key_bi=?", key_bi)
        record = await entry.decrypt_row(env, (catalog or {}).get("data"))
        if (not isinstance(record, dict)
                or record.get("source") != hosted_git.HOSTED_SOURCE):
            raise RepoError("%s/%s is not a hosted repository" % (owner, name))
        if spec["organization"]:
            org_bi, _ = await entry._org_row(env, spec["organization"])
            await hosted_repos.link_organization(
                replica, org_bi, name, owner, int(entry.Date.now()))
            entry._ORG_ALIAS_MEMO.clear()
        endpoint = await _endpoint(hosted_git, env, row["id"],
                                   str(row.get("pushed_by") or ""))
        return {"id": row["id"], "created": False,
                "node": endpoint.get("node", "")}
    problem, target = await hosted_git.core_repository_target(
        env, replica, owner, owner, name, spec["organization"])
    if problem:
        raise RepoError(" ".join(part for part in problem[::2] if part))
    repo_id = "hr" + os.urandom(12).hex()
    endpoint = await _endpoint(hosted_git, env, repo_id)
    maintainer = str(endpoint.get("publicKey") or "")[:120]
    if not maintainer:
        raise RepoError("the core git node has no public key yet")
    now = int(entry.Date.now())
    record = hosted_git.core_catalog_record(
        target, spec["visibility"], spec["description"], _origin(env, spec),
        maintainer, now)
    if not record:
        raise RepoError("invalid_name")
    await hosted_git.store_core_repository(
        env, replica, target, record, actor=owner, repo_id=repo_id,
        visibility=spec["visibility"], init=None, endpoint=endpoint, now=now)
    return {"id": repo_id, "created": True, "node": endpoint.get("node", "")}


async def read_parent(runtime, spec):
    entry, env = runtime.entry, runtime.env
    await entry.ensure_schema(env)
    owner, _, name = spec["forkOf"].partition("/")
    node = await entry._org_repo_node(env, owner, name) or owner
    record = None
    for candidate in (name, name.lower()):
        record = await entry._repo_catalog_record(env, node, candidate)
        if isinstance(record, dict):
            break
    if not isinstance(record, dict):
        raise RepoError("--fork-of %s: no such repository on this instance"
                        % spec["forkOf"])
    if record.get("visibility") != "public":
        raise RepoError("--fork-of %s is not public" % spec["forkOf"])
    stored_name = str(record.get("name") or name).lower()
    public = owner if node != owner else await _public_owner(
        entry, env, node, stored_name)
    parent = public + "/" + stored_name
    if node == spec["owner"] and stored_name == spec["name"].lower():
        raise RepoError("a repository cannot be a fork of itself")
    commits = [value for value in (str(record.get("rootCommit") or ""),
                                   str(record.get("commit") or ""))
               if OID_RE.fullmatch(value)]
    if not commits:
        raise RepoError("--fork-of %s has no published commits to compare"
                        % spec["forkOf"])
    return {"forkedFrom": parent, "commits": commits}


async def _public_owner(entry, env, node, name):
    rows = await entry.d1_all(
        env, "SELECT o.name AS name FROM org_repos r JOIN orgs o ON "
        "o.org_bi=r.org_bi WHERE r.node_owner=? AND r.repo=? LIMIT 2",
        node, name)
    names = [str(row.get("name") or "").lower() for row in rows or []]
    return names[0] if len(names) == 1 and NAME_RE.fullmatch(names[0]) \
        else node


def shares_history(git, git_dir, parent, cwd=None):
    commits = parent.get("commits") or []
    if _present(git, git_dir, commits, cwd=cwd):
        return True
    prefix = ["-C", str(cwd)] if cwd else []
    completed = git.run(None if cwd else git_dir, *prefix, "rev-list",
                        "--max-parents=0", "--branches", "--tags",
                        check=False)
    roots = set(completed.stdout.decode("utf-8", "replace").split())
    return bool(roots & set(commits))


async def configure(runtime, spec, parent):
    entry, env = runtime.entry, runtime.env
    hosted_git, _, _ = _modules(entry)
    owner, name = spec["owner"], spec["name"]
    changed = {}
    if parent:
        key_bi = await entry.blind_index(env, owner + "/" + name)
        row = await entry.d1_first(
            env, "SELECT owner_bi,data FROM repositories WHERE key_bi=?",
            key_bi)
        catalog = await entry.decrypt_row(env, (row or {}).get("data"))
        if not isinstance(catalog, dict):
            raise RepoError("the catalog record of %s/%s is missing"
                            % (owner, name))
        prior = dict(catalog)
        if catalog.get("forkedFrom") != parent["forkedFrom"]:
            catalog["forkedFrom"] = parent["forkedFrom"]
            digest = str(catalog.get("stateHash") or "")
            await hosted_git._pin_hosted_state(
                env, owner, name, key_bi, row, catalog, digest,
                notify=False, pusher=owner)
            changed["forkedFrom"] = parent["forkedFrom"]
        await entry._index_repo_fork_edge(
            env, catalog, prior, (row or {}).get("owner_bi"), key_bi)
    features = {}
    if spec["protect"] is not None:
        features["protectDefaultBranch"] = spec["protect"]
    if spec["actions"] is not None:
        features["actions"] = spec["actions"]
    if features:
        current = await entry._repo_features_get(env, owner, name)
        wanted = dict(current, **features)
        if wanted != current:
            await entry.d1_run(
                env,
                "INSERT INTO repo_feature_settings (repo_bi, data, "
                "updated_at) VALUES (?,?,?) ON CONFLICT(repo_bi) DO UPDATE "
                "SET data=excluded.data, updated_at=excluded.updated_at",
                await entry._repo_features_bi(env, owner, name),
                json.dumps(wanted, sort_keys=True), int(entry.Date.now()))
            await entry._audit_sensitive_action(
                env, owner, "repository.protection_settings", "repository",
                owner + "/" + name, "success",
                dict(features, credential="operator"))
            changed.update(features)
    return changed


class Git:
    def __init__(self, user=""):
        self.kwargs = {}
        if user:
            entry = pwd.getpwnam(user)
            self.kwargs = {
                "user": entry.pw_uid, "group": entry.pw_gid,
                "extra_groups": [], "umask": 0o077,
                "env": {"HOME": entry.pw_dir, "PATH": os.environ.get(
                    "PATH", "/usr/bin:/bin"), "LANG": "C"},
            }
        self.ids = (entry.pw_uid, entry.pw_gid) if user else None

    def run(self, git_dir, *args, check=True, timeout=GIT_TIMEOUT,
            input=None):
        command = ["git", "-c", "safe.directory=*"]
        if git_dir:
            command.append("--git-dir=" + str(git_dir))
        completed = subprocess.run(
            command + list(args), capture_output=True, timeout=timeout,
            input=input, **self.kwargs)
        if check and completed.returncode != 0:
            raise RepoError("git %s: %s" % (args[0], completed.stderr.decode(
                "utf-8", "replace").strip()[-400:]))
        return completed

    def out(self, git_dir, *args):
        return self.run(git_dir, *args).stdout.decode("utf-8", "replace")

    def chown(self, path):
        if self.ids:
            os.chown(path, *self.ids)


def _refs(git, git_dir, *patterns):
    refs = {}
    for line in git.out(git_dir, "for-each-ref",
                        "--format=%(objectname) %(refname)",
                        *patterns).splitlines():
        oid, _, ref = line.partition(" ")
        if OID_RE.fullmatch(oid) and ref:
            refs[ref] = oid
    return refs


def _ensure_bare(git, root, git_dir):
    if git_dir.is_symlink() or (git_dir.exists() and not git_dir.is_dir()):
        raise RepoError("%s is not a bare repository" % git_dir)
    if git_dir.is_dir():
        if git.run(git_dir, "rev-parse", "--is-bare-repository",
                   check=False).stdout.strip() != b"true":
            raise RepoError("%s is not a bare repository" % git_dir)
        return False
    if not root.is_dir():
        raise RepoError("%s does not exist; is the git node set up?" % root)
    target = root / (".new-" + git_dir.name)
    git.run(None, "init", "--quiet", "--bare", "--initial-branch=main",
            str(target))
    os.chmod(target, 0o700)
    os.rename(target, git_dir)
    return True


def _is_bundle(path):
    try:
        with open(path, "rb") as handle:
            head = handle.read(20)
    except OSError:
        return False
    return head.startswith((b"# v2 git bundle", b"# v3 git bundle"))


def _source_refs(local, source):
    refs = {}
    for line in local.out(None, "-C", str(source), "for-each-ref",
                          "--format=%(objectname) %(refname)",
                          "refs/heads", "refs/tags").splitlines():
        oid, _, ref = line.partition(" ")
        if OID_RE.fullmatch(oid) and ref:
            refs[ref] = oid
    return refs


def _present(git, git_dir, oids, cwd=None):
    oids = sorted(set(oids))
    if not oids:
        return set()
    prefix = ["-C", str(cwd)] if cwd else []
    completed = git.run(None if cwd else git_dir, *prefix, "cat-file",
                        "--batch-check", input=("\n".join(oids) + "\n")
                        .encode(), check=False)
    found = set()
    for line in completed.stdout.decode("utf-8", "replace").splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] != "missing" and parts[0] in oids:
            found.add(parts[0])
    return found


def _stage_source(git, spec, scratch, current):
    source = Path(spec["source"])
    if not git.ids:
        return str(source), None
    bundle = Path(scratch) / "source.bundle"
    if source.is_file() and _is_bundle(source):
        shutil.copyfile(source, bundle)
        git.chown(bundle)
        return str(bundle), None
    local = Git()
    refs = _source_refs(local, source)
    haves = _present(local, None, current.values(), cwd=source)
    revisions = ["--branches", "--tags"] + ["^" + oid for oid in sorted(
        haves)]
    completed = local.run(None, "-C", str(source), "bundle", "create",
                          "--quiet", str(bundle), "--stdin",
                          input=("\n".join(revisions) + "\n").encode(),
                          check=False)
    if completed.returncode != 0:
        if bundle.exists():
            bundle.unlink()
        if not refs:
            raise RepoError("git bundle: %s" % completed.stderr.decode(
                "utf-8", "replace").strip()[-400:])
        return "", refs
    git.chown(bundle)
    return str(bundle), None


def _stored_bytes(git, git_dir):
    completed = git.run(git_dir, "rev-list", "--all", "--objects",
                        "--disk-usage", check=False)
    try:
        if completed.returncode == 0:
            return int(completed.stdout.decode().strip() or 0)
    except ValueError:
        pass
    total = 0
    for line in git.out(git_dir, "count-objects", "-v").splitlines():
        key, _, value = line.partition(": ")
        if key in ("size", "size-pack") and value.strip().isdigit():
            total += int(value.strip()) * 1024
    return total


def _pack_objects(path):
    try:
        with open(path, "rb") as handle:
            header = handle.read(12)
    except OSError:
        return 0
    if len(header) < 12 or header[:4] != b"PACK":
        return 0
    return int.from_bytes(header[8:12], "big")


def _packs(folder):
    found = []
    for pack in sorted(Path(folder).glob("pack-*.pack")):
        if (pack.with_suffix(".idx").is_file()
                and _pack_objects(pack) > 0):
            found.append(pack)
    return found


def _new_packs(git, stage, scratch, current, staged):
    if not current:
        return _packs(Path(stage) / "objects" / "pack")
    revisions = sorted(set(staged.values())) + [
        "^" + oid for oid in sorted(set(current.values()))]
    git.run(stage, "-c", "pack.threads=1", "-c", "pack.windowMemory=64m",
            "pack-objects", "--revs", "--local", "-q",
            str(Path(scratch) / "pack"),
            input=("\n".join(revisions) + "\n").encode())
    return _packs(scratch)


def _migrate(git, packs, git_dir):
    target = Path(git_dir) / "objects" / "pack"
    for pack in packs:
        for suffix in (".pack", ".rev", ".idx"):
            source = pack.with_suffix(suffix)
            destination = target / source.name
            if not source.is_file() or destination.exists():
                continue
            try:
                os.rename(source, destination)
            except OSError:
                temporary = target / ("tmp_import_" + source.name)
                shutil.copyfile(source, temporary)
                os.chmod(temporary, 0o444)
                git.chown(temporary)
                os.rename(temporary, destination)


def _mark_maintenance(git, git_dir):
    marker = Path(git_dir) / MAINTENANCE_MARKER
    try:
        marker.touch(mode=0o600)
        git.chown(marker)
    except OSError:
        pass


def _remove_scratch(scratch, parent):
    tools = str(Path(__file__).resolve().parents[3] / "tools")
    if tools not in sys.path:
        sys.path.insert(0, tools)
    import forkmesh_safe_delete
    forkmesh_safe_delete.remove_tree(scratch, parent, ignore_errors=True)


def _plan(git, git_dir, current, staged, force):
    updates, rejected = [], []
    for target, new in sorted(staged.items()):
        old = current.get(target, "")
        if old == new:
            continue
        if old and target.startswith("refs/heads/"):
            ahead = git.run(git_dir, "merge-base", "--is-ancestor", old, new,
                            check=False).returncode == 0
            if not ahead and not force:
                rejected.append(target)
                continue
        elif old and not force:
            rejected.append(target)
            continue
        updates.append({"ref": target, "old": old, "new": new})
    return updates, rejected


def _default_branch(git, git_dir, heads):
    for candidate in DEFAULT_BRANCHES:
        if "refs/heads/" + candidate in heads:
            return candidate
    head = git.run(git_dir, "symbolic-ref", "--quiet", "HEAD",
                   check=False).stdout.decode().strip()
    if head in heads:
        return head[len("refs/heads/"):]
    return sorted(heads)[0][len("refs/heads/"):] if heads else "main"


def routing_digest(git, git_dir):
    head = git.run(git_dir, "symbolic-ref", "--quiet", "HEAD",
                   check=False).stdout.decode().strip()
    if not head.startswith("refs/heads/"):
        head = "refs/heads/main"
    lines = [line for line in git.out(
        git_dir, "for-each-ref", "--sort=refname",
        "--format=%(objectname) %(refname)", head, "refs/tags/").split("\n")
        if line]
    return hashlib.sha256("\n".join(lines).encode("utf-8")).hexdigest()


def _commit_count(git, git_dir, old, new):
    revisions = [new] if not old else [new, "^" + old]
    completed = git.run(git_dir, "rev-list", "--count",
                        "--max-count=%d" % COMMIT_COUNT_LIMIT, *revisions,
                        "--", check=False)
    try:
        return int(completed.stdout.decode().strip() or 0)
    except ValueError:
        return 0


def _stage(git, spec, git_dir, scratch, current):
    stage = Path(scratch) / "stage.git"
    git.run(None, "init", "--quiet", "--bare", str(stage))
    alternates = stage / "objects" / "info" / "alternates"
    alternates.write_text(str(Path(git_dir).resolve() / "objects") + "\n")
    git.chown(alternates)
    source, known = _stage_source(git, spec, scratch, current)
    if known is not None:
        if _present(git, stage, known.values()) != set(known.values()):
            raise RepoError("git bundle could not package the source")
        return stage, known
    try:
        git.run(stage, "-c", "fetch.unpackLimit=1", "-c",
                "transfer.unpackLimit=1", "fetch", "--quiet", "--no-tags",
                "--no-write-fetch-head", "--", source,
                "+refs/heads/*:refs/heads/*", "+refs/tags/*:refs/tags/*")
    finally:
        bundle = Path(scratch) / "source.bundle"
        if bundle.exists():
            bundle.unlink()
    return stage, _refs(git, stage, "refs/heads", "refs/tags")


def fill(spec, info, max_bytes, git_user=""):
    if not ID_RE.fullmatch(str(info.get("id") or "")):
        raise RepoError("invalid repository id")
    git = Git(git_user)
    root = Path(spec["root"])
    git_dir = root / (info["id"] + ".git")
    _ensure_bare(git, root, git_dir)
    base = root.parent / "tmp"
    parent = base if base.is_dir() else root
    scratch = tempfile.mkdtemp(prefix="instance-repo-", dir=str(parent))
    git.chown(scratch)
    try:
        current = _refs(git, git_dir, "refs/heads", "refs/tags")
        stage, staged = _stage(git, spec, git_dir, scratch, current)
        updates, rejected = _plan(git, stage, current, staged, spec["force"])
        if rejected:
            raise RepoError("not a fast-forward or a moved tag: %s (rerun "
                            "with --force)" % ", ".join(rejected[:10]))
        if updates:
            packs = _new_packs(git, stage, scratch, current, staged)
            added = sum(pack.stat().st_size + pack.with_suffix(".idx").stat()
                        .st_size for pack in packs)
            stored = _stored_bytes(git, git_dir)
            if stored + added > max_bytes:
                raise RepoError(
                    "the repository would take %d bytes (%d stored, %d new), "
                    "more than HOSTED_REPO_MAX_BYTES=%d"
                    % (stored + added, stored, added, max_bytes))
            _migrate(git, packs, git_dir)
    finally:
        _remove_scratch(scratch, parent)
    if updates:
        commands = ["update %s %s %s" % (u["ref"], u["new"], u["old"] or (
            "0" * len(u["new"]))) for u in updates]
        completed = subprocess.run(
            ["git", "-c", "safe.directory=*", "--git-dir=" + str(git_dir),
             "update-ref", "--stdin"],
            input=("\n".join(commands) + "\n").encode(), capture_output=True,
            timeout=GIT_TIMEOUT, **git.kwargs)
        if completed.returncode != 0:
            raise RepoError("update-ref: " + completed.stderr.decode(
                "utf-8", "replace").strip()[-400:])
        _mark_maintenance(git, git_dir)
    heads = _refs(git, git_dir, "refs/heads")
    tags = _refs(git, git_dir, "refs/tags")
    default = _default_branch(git, git_dir, heads)
    if heads:
        git.run(git_dir, "symbolic-ref", "HEAD", "refs/heads/" + default)
    commit = heads.get("refs/heads/" + default, "")
    default_ref = "refs/heads/" + default
    updates.sort(key=lambda u: (u["ref"] != default_ref, u["ref"]))
    for update in updates[:REF_UPDATES_MAX]:
        if update["ref"].startswith("refs/heads/"):
            update["commits"] = _commit_count(
                git, git_dir, update["old"], update["new"])
    root_commit = ""
    if commit:
        roots = git.out(git_dir, "rev-list", "--max-parents=0",
                        commit).split()
        root_commit = roots[-1] if roots else ""
    return {
        "branches": sorted(ref[len("refs/heads/"):] for ref in heads),
        "tags": len(tags),
        "defaultBranch": default,
        "commit": commit,
        "commitCount": _commit_count(git, git_dir, "", commit) if commit
        else 0,
        "rootCommit": root_commit,
        "digest": routing_digest(git, git_dir) if heads or tags else "",
        "updates": updates,
        "sizeBytes": _stored_bytes(git, git_dir),
        "gitDir": str(git_dir),
    }


def _zero(oid, length=40):
    return oid or "0" * length


async def record(runtime, spec, info, filled):
    entry, env = runtime.entry, runtime.env
    hosted_git, _, core_push = _modules(entry)
    owner, name = spec["owner"], spec["name"]
    key_bi = await entry.blind_index(env, owner + "/" + name)
    row = await entry.d1_first(
        env, "SELECT owner_bi,data FROM repositories WHERE key_bi=?", key_bi)
    catalog = await entry.decrypt_row(env, (row or {}).get("data"))
    if not isinstance(catalog, dict):
        raise RepoError("the catalog record of %s/%s is missing"
                        % (owner, name))
    updates = filled["updates"]
    digest = filled["digest"]
    pinned = catalog.get("stateHash", "")
    if digest and (updates or pinned != digest):
        endpoint = await _endpoint(hosted_git, env, info["id"], info["node"])
        if updates:
            await hosted_git._note_core_push(
                env, key_bi, endpoint.get("node", ""))
        catalog.update({
            "branch": filled["defaultBranch"],
            "commit": filled["commit"],
            "branchCount": str(len(filled["branches"])),
            "commitCount": str(filled["commitCount"]),
            "rootCommit": filled["rootCommit"],
            "sizeBytes": int(filled["sizeBytes"]),
        })
        pinned = await hosted_git._pin_hosted_state(
            env, owner, name, key_bi, row, catalog, digest, notify=False,
            pusher=owner)
        if updates:
            report = {"total": len(updates), "updates": [
                dict(update, old=_zero(update["old"], len(update["new"])))
                for update in updates[:REF_UPDATES_MAX]]}
            await core_push.record_accepted_push(
                env, owner, name, endpoint, owner, report,
                core=spec["hooks"], state=digest, credential="operator")
    return {"pinned": pinned}


def _forked(user, work):
    entry = pwd.getpwnam(user)
    read_end, write_end = os.pipe()
    pid = os.fork()
    if pid == 0:
        os.close(read_end)
        try:
            if os.geteuid() == 0:
                os.setgroups([])
            os.setgid(entry.pw_gid)
            os.setuid(entry.pw_uid)
            os.environ["HOME"] = entry.pw_dir
            try:
                result = {"ok": True, "value": work()}
            except RepoError as error:
                result = {"ok": False, "error": str(error)}
            except Exception as error:
                result = {"ok": False, "error": "%s: %s" % (
                    type(error).__name__, error)}
            data = json.dumps(result).encode("utf-8")
            while data:
                data = data[os.write(write_end, data):]
        finally:
            os._exit(0)
    os.close(write_end)
    chunks = []
    while True:
        chunk = os.read(read_end, 65536)
        if not chunk:
            break
        chunks.append(chunk)
    os.close(read_end)
    os.waitpid(pid, 0)
    try:
        result = json.loads(b"".join(chunks) or b"{}")
    except ValueError:
        result = {}
    if not result.get("ok"):
        raise RepoError(result.get("error") or "the %s step failed" % user)
    return result["value"]


def _users(args):
    root = os.geteuid() == 0
    me = pwd.getpwuid(os.geteuid()).pw_name
    users = []
    for value, default in ((args.app_user, APP_USER),
                           (args.git_user, NODE_USER)):
        user = default if value is None and root else (value or "")
        if user == me:
            user = ""
        if user and not root:
            raise RepoError("run as root to act as %s" % user)
        if user:
            try:
                pwd.getpwnam(user)
            except KeyError as error:
                raise RepoError("no system user %s" % user) from error
        users.append(user)
    return users


def check_source_history(spec, parent):
    source = Path(spec["source"])
    if not parent or not source.is_dir():
        return
    if not shares_history(Git(), None, parent, cwd=source):
        raise RepoError("%s shares no history with %s; refusing --fork-of"
                        % (spec["source"], parent["forkedFrom"]))


def check_filled_history(spec, parent, filled, git_user):
    if parent and not shares_history(Git(git_user), filled["gitDir"], parent):
        raise RepoError("%s/%s shares no history with %s; it was filled but "
                        "not recorded as a fork" % (
                            spec["owner"], spec["name"],
                            parent["forkedFrom"]))


def run(args, spec):
    from .ops import _runtime
    app_user, git_user = _users(args)
    if app_user:
        def in_app(phase, *extra):
            return _forked(app_user, lambda: asyncio.run(
                phase(_runtime(args), spec, *extra)))

        def limit():
            runtime = _runtime(args)
            return _modules(runtime.entry)[1].max_bytes(runtime.env)
        parent = in_app(read_parent) if spec["forkOf"] else {}
        check_source_history(spec, parent)
        info = in_app(prepare)
        max_bytes = _forked(app_user, limit)
        filled = fill(spec, info, max_bytes, git_user)
        check_filled_history(spec, parent, filled, git_user)
        recorded = in_app(record, info, filled)
        configured = in_app(configure, parent)
    else:
        runtime = _runtime(args)

        async def everything():
            parent = await read_parent(runtime, spec) if spec[
                "forkOf"] else {}
            check_source_history(spec, parent)
            info = await prepare(runtime, spec)
            max_bytes = _modules(runtime.entry)[1].max_bytes(runtime.env)
            filled = fill(spec, info, max_bytes, git_user)
            check_filled_history(spec, parent, filled, git_user)
            recorded = await record(runtime, spec, info, filled)
            configured = await configure(runtime, spec, parent)
            return info, filled, recorded, configured
        info, filled, recorded, configured = asyncio.run(everything())
    return {
        "ok": True,
        "owner": spec["owner"],
        "organization": spec["organization"],
        "name": spec["name"],
        "id": info["id"],
        "created": info["created"],
        "branches": filled["branches"],
        "tags": filled["tags"],
        "defaultBranch": filled["defaultBranch"],
        "commit": filled["commit"],
        "refsUpdated": len(filled["updates"]),
        "sizeBytes": filled["sizeBytes"],
        "stateHash": recorded.get("pinned") or "",
        "gitDir": filled["gitDir"],
        "forkedFrom": configured.get("forkedFrom", "") or (
            spec["forkOf"] and "unchanged"),
        "settings": {key: value for key, value in configured.items()
                     if key != "forkedFrom"},
    }


def cmd_instance_repo(args):
    from . import vault
    try:
        if vault.enabled(os.environ):
            raise RepoError("the app vault is on; this command needs the "
                            "data key in the app settings")
        result = run(args, read_spec(args))
    except RepoError as error:
        result = {"ok": False, "error": str(error)}
    print(json.dumps(result, sort_keys=True))
    return 0 if result.get("ok") else 1


def add_parser(sub, common):
    command = sub.add_parser(
        "instance-repo",
        help="create or update a hosted repository from a local git "
             "repository or bundle on this server")
    common(command)
    command.add_argument("--owner", required=True,
                         help="account that owns and creates the repository")
    command.add_argument("--organization", default="",
                         help="organization the repository is listed under")
    command.add_argument("--name", required=True)
    command.add_argument("--description", default="")
    command.add_argument("--visibility", choices=("public", "private"),
                         default="public")
    command.add_argument("--from", dest="source", required=True,
                         help="local git repository or bundle")
    command.add_argument("--force", action="store_true",
                         help="allow non-fast-forward branches and moved tags")
    command.add_argument("--no-hooks", action="store_true",
                         help="skip webhooks, pings and Actions for this fill")
    command.add_argument("--fork-of", dest="fork_of", default="",
                         help="record the repository as a fork of "
                              "<owner>/<repository> on this instance, like "
                              "the web Fork action")
    command.add_argument("--protect-default-branch", dest="protect_default_branch",
                         action="store_true", default=None,
                         help="the default branch changes only by merging a "
                              "pull request an organization owner or admin "
                              "approved")
    command.add_argument("--no-protect-default-branch",
                         dest="protect_default_branch", action="store_false")
    command.add_argument("--actions", choices=("on", "off"), default=None,
                         help="run this repository's workflows on the "
                              "instance runner (on) or never (off)")
    command.add_argument("--hosted-root", default=HOSTED_ROOT)
    command.add_argument("--origin", default="",
                         help="public origin (default PUBLIC_BASE_URL)")
    command.add_argument("--app-user", default=None,
                         help="user for database work (root: forkmesh-app)")
    command.add_argument("--git-user", default=None,
                         help="user owning the hosted root (root: "
                              "forkmesh-node)")
    command.set_defaults(func=cmd_instance_repo)
