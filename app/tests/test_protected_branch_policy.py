"""Protected branches: refused ref updates never move refs; admitted ones are audited.

The node operator policy's gateway-owned pre-receive hook enforces branch rules
for both the HTTPS mirror gateway and the SSH gateway: deletion and force
updates of protected refs, stale expected old values, and commits that no
allowed SSH signer signed are refused with copy-ready recovery steps.
"""

import importlib.util
import io
import json
import os
from pathlib import Path
import pwd
import shutil
import subprocess
import sys
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import node_operator_policy as policy_mod  # noqa: E402


def _module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


ssh_gateway = _module(
    ROOT / "tools" / "ssh_gateway.py", "forkmesh_ssh_gateway_protected_branches"
)

KEY_ID = "sk_" + "a" * 24


def git(*arguments, cwd=None, check=True):
    completed = subprocess.run(
        ["git", *arguments], cwd=cwd, capture_output=True, text=True,
        check=False, timeout=60,
    )
    if check and completed.returncode != 0:
        raise AssertionError(completed.stderr)
    return completed


def write_policy(path, **overrides):
    value = {"schemaVersion": 1, "profile": "home", **overrides}
    path.write_text(json.dumps(value), encoding="utf-8")
    path.chmod(0o600)
    return path


@pytest.fixture
def node(tmp_path):
    root = tmp_path / "repos"
    root.mkdir(mode=0o700)
    state = tmp_path / "state"
    state.mkdir(mode=0o700)
    policy_path = write_policy(
        tmp_path / "policy.json",
        branches={"rules": [{"pattern": "main"}, {"pattern": "release/*"}]},
    )
    hooks = policy_mod.install_hook(tmp_path / "hooks", policy_path, state, root)
    return {
        "tmp": tmp_path, "root": root, "state": state,
        "policy": policy_path, "hooks": hooks,
    }


def make_bare(path):
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    git("init", "--bare", "-b", "main", str(path))
    return path


def make_client(tmp_path, label):
    client = tmp_path / ("client-" + label)
    git("init", "-b", "main", str(client))
    git("config", "user.email", "dev@example.test", cwd=client)
    git("config", "user.name", "Dev", cwd=client)
    git("config", "commit.gpgsign", "false", cwd=client)
    return client


def commit(client, relative, text, *sign):
    (client / relative).write_text(text, encoding="utf-8")
    git("add", ".", cwd=client)
    git(*sign, "commit", "-m", "change " + relative, cwd=client)
    return git("rev-parse", "HEAD", cwd=client).stdout.strip()


def push(hooks, client, git_dir, refspec, *extra, environment=""):
    receive = "%sgit -c core.hooksPath=%s receive-pack" % (environment, hooks)
    return git(
        "push", "--receive-pack=" + receive, str(git_dir), *extra, refspec,
        cwd=client, check=False,
    )


def ref(git_dir, name):
    return git(
        "--git-dir", str(git_dir), "rev-parse", "--verify", "-q", name,
        check=False,
    ).stdout.strip()


def audit_actions(state):
    path = state / "audit.jsonl"
    if not path.exists():
        return []
    return [json.loads(line)["action"] for line in path.read_text().splitlines()]


# --------------------------------------------------------------------------
# Policy shape


@pytest.mark.parametrize("branches, message", [
    ({"rules": [{"pattern": "main", "force": True}]}, "unknown"),
    ({"rules": [{"pattern": "../main"}]}, "pattern"),
    ({"rules": [{"pattern": ""}]}, "pattern"),
    ({"rules": [{"pattern": "main branch"}]}, "pattern"),
    ({"rules": [{"pattern": "main", "blockDeletion": "yes"}]}, "boolean"),
    ({"rules": [{"pattern": "main", "requireSignedCommits": True}]},
     "allowedSignersPath is required"),
    ({"rules": [], "allowedSignersPath": "signers"}, "absolute"),
    ({"rules": [{"pattern": "main", "repositories": ["Not An Owner"]}]},
     "owner"),
    ({"rules": {"pattern": "main"}}, "array"),
])
def test_invalid_branch_rules_are_refused(branches, message):
    with pytest.raises(policy_mod.PolicyError, match=message):
        policy_mod.normalize_policy(
            {"schemaVersion": 1, "profile": "home", "branches": branches})


def test_branch_rules_default_to_protecting_history_and_scope_repositories():
    policy = policy_mod.normalize_policy({
        "schemaVersion": 1, "profile": "home",
        "branches": {"rules": [
            {"pattern": "release/*"},
            {"pattern": "refs/tags/v*", "repositories": ["alice/app"],
             "blockForcePush": False},
        ]},
    })
    release, tags = policy["branches"]["rules"]
    assert release == {
        "pattern": "refs/heads/release/*", "repositories": [],
        "blockDeletion": True, "blockForcePush": True,
        "requireSignedCommits": False,
    }
    assert tags["repositories"] == ["alice/app"]
    assert policy_mod.branch_rules_for(
        policy, "bob", "lib", "refs/heads/release/1.0") == [release]
    assert policy_mod.branch_rules_for(
        policy, "bob", "lib", "refs/heads/main") == []
    assert policy_mod.branch_rules_for(
        policy, "alice", "app", "refs/tags/v1") == [tags]
    assert policy_mod.branch_rules_for(
        policy, "bob", "lib", "refs/tags/v1") == []
    # A policy without branches keeps accepting every ref update.
    assert policy_mod.normalize_policy(
        {"schemaVersion": 1, "profile": "home"})["branches"] == {
            "rules": [], "allowedSignersPath": ""}


def test_preview_flags_new_branch_protection_as_reducing(node):
    current = policy_mod.normalize_policy({"schemaVersion": 1, "profile": "home"})
    candidate = policy_mod.load_policy(node["policy"])
    report = policy_mod.preview(candidate, node["root"], node["state"], current)
    assert report["availabilityReducing"] is True
    assert any("branch protections" in item for item in report["consequences"])
    loosened = policy_mod.preview(current, node["root"], node["state"], candidate)
    assert loosened["availabilityReducing"] is False


# --------------------------------------------------------------------------
# Enforcement through a real receive-pack


def test_force_update_and_deletion_of_protected_refs_leave_refs_unchanged(node):
    git_dir = make_bare(node["root"] / "alice" / "app.git")
    client = make_client(node["tmp"], "force")
    first = commit(client, "README.md", "one\n")
    assert push(node["hooks"], client, git_dir, "main:main").returncode == 0
    second = commit(client, "README.md", "two\n")
    assert push(node["hooks"], client, git_dir, "main:main").returncode == 0
    assert push(
        node["hooks"], client, git_dir, "main:refs/heads/release/1").returncode == 0
    assert ref(git_dir, "refs/heads/main") == second

    git("reset", "--hard", first, cwd=client)
    rewritten = commit(client, "README.md", "rewritten\n")
    refused = push(node["hooks"], client, git_dir, "main:main", "--force")
    assert refused.returncode != 0
    assert "protected-branch-force-update" in refused.stderr
    assert "to recover (protected-branch-force-update)" in refused.stderr
    assert "git rebase origin/main" in refused.stderr
    assert ref(git_dir, "refs/heads/main") == second
    assert git("--git-dir", str(git_dir), "cat-file", "-e", rewritten,
               check=False).returncode != 0

    deleted = push(node["hooks"], client, git_dir, ":refs/heads/release/1")
    assert deleted.returncode != 0
    assert "protected-branch-deletion" in deleted.stderr
    assert ref(git_dir, "refs/heads/release/1") == second

    # Unprotected refs keep stock Git behaviour, including explicit force.
    assert push(node["hooks"], client, git_dir, "main:topic", "--force").returncode == 0
    git("reset", "--hard", second, cwd=client)
    assert push(node["hooks"], client, git_dir, "main:topic", "--force").returncode == 0
    assert push(node["hooks"], client, git_dir, ":topic").returncode == 0

    actions = audit_actions(node["state"])
    assert actions.count("admit-push") == 3
    assert actions.count("reject-push") == 2
    assert policy_mod.verify_audit(node["state"])["ok"] is True
    summary = policy_mod.status_summary(node["state"])
    assert summary["admittedProtectedPushes"] == 3
    assert summary["rejectedPushes"] == 2
    admitted = json.loads(
        (node["state"] / "audit.jsonl").read_text().splitlines()[0])
    assert admitted["detail"]["refs"] == [{
        "ref": "refs/heads/main", "old": "0" * 40, "new": first,
        "patterns": ["refs/heads/main"],
    }]
    # Audit records carry refs and object ids only, never content or keys.
    assert "one" not in json.dumps(admitted["detail"]["refs"][0]["patterns"])


def test_stale_expected_old_value_is_refused_with_recovery(node):
    git_dir = make_bare(node["root"] / "alice" / "app.git")
    client = make_client(node["tmp"], "stale")
    first = commit(client, "README.md", "one\n")
    second = commit(client, "README.md", "two\n")
    assert push(node["hooks"], client, git_dir, "main:main").returncode == 0

    def run(line):
        stderr = io.StringIO()
        code = policy_mod.run_pre_receive(
            line + "\n", git_dir, node["policy"], node["state"], None, stderr)
        return code, stderr.getvalue()

    # A writer that fetched before `second` landed still expects `first`.
    code, output = run("%s %s refs/heads/main" % (first, second))
    assert code == 1
    assert "stale-ref" in output and "git fetch origin" in output
    # Creating a protected ref that already exists is stale too.
    code, output = run("%s %s refs/heads/main" % ("0" * 40, second))
    assert code == 1 and "stale-ref" in output
    code, _output = run("%s %s refs/heads/main" % (second, second))
    assert code == 0
    assert ref(git_dir, "refs/heads/main") == second

    # Stock Git refuses a stale lease without contacting the hook or the ref.
    git("reset", "--hard", first, cwd=client)
    leased = push(
        node["hooks"], client, git_dir, "main:main",
        "--force-with-lease=main:" + first)
    assert leased.returncode != 0 and "stale info" in leased.stderr
    assert ref(git_dir, "refs/heads/main") == second


@pytest.mark.skipif(shutil.which("ssh-keygen") is None, reason="needs ssh-keygen")
def test_signed_commit_rule_admits_only_allowed_ssh_signers(node):
    keys = node["tmp"] / "keys"
    keys.mkdir(mode=0o700)
    for label in ("trusted", "stranger"):
        subprocess.run(
            ["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", label,
             "-f", str(keys / label)],
            check=True, capture_output=True, timeout=60)
    signers = keys / "allowed_signers"
    signers.write_text(
        'dev@example.test namespaces="git" %s'
        % (keys / "trusted.pub").read_text(), encoding="utf-8")
    signers.chmod(0o600)
    write_policy(node["policy"], branches={
        "rules": [{"pattern": "main", "requireSignedCommits": True}],
        "allowedSignersPath": str(signers),
    })

    def signed(label):
        return ("-c", "gpg.format=ssh",
                "-c", "user.signingkey=" + str(keys / (label + ".pub")),
                "-c", "commit.gpgsign=true")

    git_dir = make_bare(node["root"] / "alice" / "app.git")
    client = make_client(node["tmp"], "signed")
    base = commit(client, "README.md", "trusted\n", *signed("trusted"))
    assert push(node["hooks"], client, git_dir, "main:main").returncode == 0

    unsigned = commit(client, "notes.md", "unsigned\n")
    refused = push(node["hooks"], client, git_dir, "main:main")
    assert refused.returncode != 0
    assert "commit-signature" in refused.stderr
    assert "is not signed" in refused.stderr and unsigned in refused.stderr
    assert "git config gpg.format ssh" in refused.stderr
    assert ref(git_dir, "refs/heads/main") == base

    # An unsigned commit accepted on an unprotected branch cannot be
    # fast-forwarded into the protected one once it is already stored.
    assert push(node["hooks"], client, git_dir, "main:topic").returncode == 0
    refused = push(node["hooks"], client, git_dir, "main:main")
    assert refused.returncode != 0 and "commit-signature" in refused.stderr
    assert ref(git_dir, "refs/heads/main") == base

    git("reset", "--hard", base, cwd=client)
    commit(client, "notes.md", "stranger\n", *signed("stranger"))
    refused = push(node["hooks"], client, git_dir, "main:main")
    assert refused.returncode != 0
    assert "not in this node's allowed signers" in refused.stderr
    assert ref(git_dir, "refs/heads/main") == base

    git("reset", "--hard", base, cwd=client)
    trusted = commit(client, "notes.md", "trusted again\n", *signed("trusted"))
    accepted = push(node["hooks"], client, git_dir, "main:main")
    assert accepted.returncode == 0, accepted.stderr
    assert ref(git_dir, "refs/heads/main") == trusted

    # A signers file others can rewrite fails closed.
    signers.chmod(0o666)
    commit(client, "more.md", "more\n", *signed("trusted"))
    refused = push(node["hooks"], client, git_dir, "main:main")
    assert refused.returncode != 0 and "policy is unavailable" in refused.stderr
    assert ref(git_dir, "refs/heads/main") == trusted


# --------------------------------------------------------------------------
# SSH gateway wiring


def _gateway_config(tmp_path, **overrides):
    root = tmp_path / "gateway-repos"
    root.mkdir(parents=True, exist_ok=True)
    root.chmod(0o755)
    capacity = tmp_path / "config-capacity"
    capacity.mkdir(mode=0o700, exist_ok=True)
    true_program = str(Path(shutil.which("true") or "/usr/bin/true").resolve())
    config = {
        "schemaVersion": 1,
        "authorizationSocket": str(tmp_path / "authorize.sock"),
        "authorizationBrokerUser": pwd.getpwuid(os.geteuid()).pw_name,
        "gatewayExecutable": true_program,
        "refreshNotifier": true_program,
        "repositoryRoot": str(root),
        "limits": {**ssh_gateway.DEFAULT_LIMITS, "capacityDirectory": str(capacity)},
        "repositories": [{
            "owner": "alice-node", "name": "widget",
            "path": "widget.git", "access": "read-write",
        }],
    }
    config.update(overrides)
    path = tmp_path / "gateway.json"
    path.write_text(json.dumps(config), encoding="utf-8")
    path.chmod(0o644)
    return path


@pytest.mark.parametrize("value", [
    "policy.json",
    {"policyPath": "policy.json", "stateDirectory": "/var/lib/state"},
    {"policyPath": "/etc/policy.json"},
    {"policyPath": "/etc/policy.json", "stateDirectory": "/var/lib/state",
     "hooks": "/tmp"},
    {"policyPath": "/etc/../policy.json", "stateDirectory": "/var/lib/state"},
])
def test_ssh_gateway_config_validates_operator_policy_shape(tmp_path, value):
    with pytest.raises(ssh_gateway.GatewayError, match="invalid"):
        ssh_gateway.Config(_gateway_config(tmp_path, operatorPolicy=value))


def test_ssh_gateway_check_reports_operator_policy(tmp_path, node, capsys):
    config = ssh_gateway.Config(_gateway_config(tmp_path, operatorPolicy={
        "policyPath": str(node["policy"]),
        "stateDirectory": str(node["state"]),
    }))
    assert config.operator_policy_path == node["policy"]
    ssh_gateway._check(config)
    report = json.loads(capsys.readouterr().out)
    assert report["operatorPolicyConfigured"] is True
    assert report["operatorPolicyValid"] is True

    node["policy"].unlink()
    assert ssh_gateway._check(config) == 1
    report = json.loads(capsys.readouterr().out)
    assert report["operatorPolicyValid"] is False and report["ok"] is False

    plain = ssh_gateway.Config(_gateway_config(tmp_path))
    assert plain.operator_policy_path is None
    ssh_gateway._check(plain)
    report = json.loads(capsys.readouterr().out)
    assert report["operatorPolicyConfigured"] is False


@pytest.mark.parametrize("operation", ["git-receive-pack", "git-upload-pack"])
def test_ssh_serve_runs_policy_hook_only_for_receive(
    monkeypatch, tmp_path, node, operation
):
    repo_path = Path("/srv/forkmesh/git/widget.git")
    capacity = tmp_path / "capacity"
    capacity.mkdir(mode=0o700)
    config = SimpleNamespace(
        repositories={("alice-node", "widget"): (repo_path, True)},
        repository_root=Path("/srv/forkmesh/git"),
        refresh_notifier=Path("/opt/forkmesh/ssh-refresh-notify"),
        limits=ssh_gateway.GatewayLimits(
            {**ssh_gateway.DEFAULT_LIMITS, "capacityDirectory": str(capacity)}),
        operator_policy_path=node["policy"],
        operator_policy_state=node["state"],
    )
    monkeypatch.setenv(
        "SSH_ORIGINAL_COMMAND", operation + " 'alice-node/widget.git'")

    def fake_api(_config, payload):
        return {
            "authorized": True,
            "keyId": payload["keyId"],
            "owner": payload["owner"],
            "repository": payload["repository"],
            "operation": payload["operation"],
            "repositoryRelativePath": (
                payload["owner"] + "/" + payload["repository"] + ".git"),
        }

    calls = []
    monkeypatch.setattr(ssh_gateway, "_api", fake_api)
    monkeypatch.setattr(ssh_gateway, "_bare_repository", lambda path: True)
    monkeypatch.setattr(
        ssh_gateway, "_repository_size_bytes", lambda *_args, **_kwargs: 0)
    monkeypatch.setattr(
        ssh_gateway, "_receive_artifacts", lambda *_args, **_kwargs: {})
    real_resolve = Path.resolve
    monkeypatch.setattr(
        Path, "resolve",
        lambda self, strict=False: self if str(self).startswith("/srv/")
        else real_resolve(self))
    monkeypatch.setattr(
        ssh_gateway, "_run_git_service",
        lambda command, environment, **kwargs: calls.append(
            (command, environment)) or 0)
    monkeypatch.setattr(
        ssh_gateway.subprocess, "run",
        lambda command, **kwargs: SimpleNamespace(returncode=0))

    assert ssh_gateway._serve(config, KEY_ID) == 0
    ((command, environment),) = calls
    if operation == "git-receive-pack":
        hooks = node["state"] / "hooks"
        assert "core.hooksPath=" + str(hooks) in command
        assert environment["FORKMESH_POLICY_REPOSITORY"] == "alice-node/widget"
        assert sorted(item.name for item in hooks.iterdir()) == ["pre-receive"]
    else:
        assert "core.hooksPath=" + os.devnull in command
        assert "FORKMESH_POLICY_REPOSITORY" not in environment


def test_ssh_layout_uses_the_gateway_supplied_repository_identity(node):
    write_policy(node["policy"], branches={"rules": [
        {"pattern": "main", "repositories": ["alice-node/widget"]},
    ]})
    # SSH gateway paths are operator-chosen, not <owner>/<name>.git.
    git_dir = make_bare(node["tmp"] / "gateway-root" / "forkmesh-widget.git")
    client = make_client(node["tmp"], "ssh")
    first = commit(client, "README.md", "one\n")
    identity = "FORKMESH_POLICY_REPOSITORY=alice-node/widget "
    assert push(
        node["hooks"], client, git_dir, "main:main",
        environment=identity).returncode == 0
    commit(client, "README.md", "two\n")
    assert push(
        node["hooks"], client, git_dir, "main:main",
        environment=identity).returncode == 0
    git("reset", "--hard", first, cwd=client)
    commit(client, "README.md", "rewrite\n")
    refused = push(
        node["hooks"], client, git_dir, "main:main", "--force",
        environment=identity)
    assert refused.returncode != 0
    assert "protected-branch-force-update" in refused.stderr
    records = [
        json.loads(line)
        for line in (node["state"] / "audit.jsonl").read_text().splitlines()
    ]
    assert {item["subject"] for item in records} == {"alice-node/widget"}
