#!/usr/bin/env python3
"""Controller-free SSH writes: local authorization, PR spooling, and forks.

Mirrors that operate as independent public Git servers accept pushes, signed
pull-request submissions, and forks while the relay Worker is unreachable.
These tests pin the local-authorization broker mode, the forkmesh-pull-submit
and forkmesh-fork forced-command verbs, the pull-request ref namespace
restriction, and the spool-forward replay contract.
"""

import base64
import hashlib
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


PROJECT_ROOT = Path(__file__).resolve().parents[2]


def _module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


ssh_gateway = _module(
    PROJECT_ROOT / "tools" / "ssh_gateway.py",
    "forkmesh_ssh_gateway_independent",
)
broker = _module(
    PROJECT_ROOT / "tools" / "ssh_authorization_broker.py",
    "forkmesh_ssh_broker_independent",
)


KEY_ID = "sk_" + "a" * 24
OTHER_KEY_ID = "sk_" + "b" * 24


def _ed25519_keypair(tmp_path):
    private = tmp_path / "identity.pem"
    subprocess.run(
        ["openssl", "genpkey", "-algorithm", "ed25519", "-out", str(private)],
        check=True,
        capture_output=True,
    )
    der = subprocess.run(
        ["openssl", "pkey", "-in", str(private), "-pubout", "-outform", "DER"],
        check=True,
        capture_output=True,
    ).stdout
    assert der[:12] == ssh_gateway.ED25519_SPKI_PREFIX
    author = base64.urlsafe_b64encode(der[12:]).decode("ascii").rstrip("=")
    return private, author


def _sign(tmp_path, private, message):
    message_path = tmp_path / "message.bin"
    signature_path = tmp_path / "signature.bin"
    message_path.write_bytes(message)
    subprocess.run(
        [
            "openssl", "pkeyutl", "-sign", "-inkey", str(private),
            "-rawin", "-in", str(message_path), "-out", str(signature_path),
        ],
        check=True,
        capture_output=True,
    )
    raw = signature_path.read_bytes()
    return base64.urlsafe_b64encode(raw).decode("ascii").rstrip("=")


def _signed_pull(tmp_path, private, author, **overrides):
    pull = {
        "title": "Add widget polish",
        "base": "main",
        "head": "feature/polish",
        "patch": "diff --git a/a b/a\n",
        "commits": "",
        "description": "polish",
        "author": author,
        "ts": 1_723_000_000_000,
    }
    pull.update(overrides)
    fields = [pull["title"], pull["base"], pull["head"], pull["patch"]]
    content = "\x00".join(fields + [pull["commits"]])
    content_hash = hashlib.sha256(content.encode("utf-8")).hexdigest()
    canonical = (
        "forkmesh-pull-event-v1\n" + pull["author"] + "\n"
        + str(pull["ts"]) + "\n" + content_hash
    ).encode("utf-8")
    pull["sig"] = _sign(tmp_path, private, canonical)
    return pull


def _signed_comment(tmp_path, private, author, number):
    event = {
        "type": "comment",
        "body": "Looks good to me.",
        "author": author,
        "ts": 1_723_000_100_000,
    }
    content_hash = hashlib.sha256(event["body"].encode("utf-8")).hexdigest()
    canonical = (
        "forkmesh-pull-comment-v1\ncomment\n" + str(number) + "\n"
        + author + "\n" + str(event["ts"]) + "\n" + content_hash
    ).encode("utf-8")
    event["sig"] = _sign(tmp_path, private, canonical)
    return event


def _spool_config(tmp_path, **overrides):
    spool = tmp_path / "spool"
    spool.mkdir(mode=0o700, exist_ok=True)
    values = {
        "repositories": {
            ("alice-node", "widget"): (
                tmp_path / "repos" / "alice-node" / "widget.git",
                True,
                16 * 1024 * 1024 * 1024,
            )
        },
        "repository_root": tmp_path / "repos",
        "pull_submission_spool": spool,
        "pull_submission_max_bytes": ssh_gateway.DEFAULT_SUBMISSION_MAX_BYTES,
        "dynamic_repositories_file": None,
    }
    values.update(overrides)
    return SimpleNamespace(**values)


def _authorize_api(account=""):
    def fake(_config, payload):
        result = {
            "authorized": True,
            "keyId": payload["keyId"],
            "owner": payload["owner"],
            "repository": payload["repository"],
            "operation": payload["operation"],
            "repositoryRelativePath": (
                payload["owner"] + "/" + payload["repository"] + ".git"
            ),
        }
        if account:
            result["account"] = account
        return result

    return fake


# ---------------------------------------------------------------------------
# Canonicalization parity with app/src/events.py
# ---------------------------------------------------------------------------


def test_pull_event_canonicals_match_relay_wire_format():
    pull = {
        "title": "T", "base": "b", "head": "h", "patch": "p",
        "commits": "", "author": "AUTHOR", "ts": 7,
    }
    # With no commits mbox both the 5-field and legacy 4-field forms verify,
    # exactly like events.verify_pull_event.
    five = hashlib.sha256(b"T\x00b\x00h\x00p\x00").hexdigest()
    four = hashlib.sha256(b"T\x00b\x00h\x00p").hexdigest()
    assert ssh_gateway._pull_event_canonicals(pull) == [
        ("forkmesh-pull-event-v1\nAUTHOR\n7\n" + five).encode(),
        ("forkmesh-pull-event-v1\nAUTHOR\n7\n" + four).encode(),
    ]
    pull["commits"] = "mbox"
    # A submission carrying commits never falls back to the unsigned 4-field
    # form: an attacker must not append an unsigned mbox to a legacy payload.
    with_commits = hashlib.sha256(b"T\x00b\x00h\x00p\x00mbox").hexdigest()
    assert ssh_gateway._pull_event_canonicals(pull) == [
        ("forkmesh-pull-event-v1\nAUTHOR\n7\n" + with_commits).encode(),
    ]


def test_pull_comment_content_matches_relay_per_type_fields():
    assert ssh_gateway._pull_comment_content(
        {"type": "comment", "body": "B"}
    ) == "B"
    assert ssh_gateway._pull_comment_content(
        {"type": "review", "state": "approve", "body": "B"}
    ) == "approve\x00B"
    assert ssh_gateway._pull_comment_content(
        {"type": "line-comment", "path": "a.py", "side": "new",
         "line": 3, "body": "B"}
    ) == "a.py\x00new\x003\x00B"
    assert ssh_gateway._pull_comment_content(
        {"type": "thread-comment", "threadId": "t1", "path": "a.py",
         "side": "old", "lineStart": 1, "lineEnd": 2, "body": "B",
         "suggestionPatch": "s"}
    ) == "t1\x00a.py\x00old\x001\x002\x00B\x00s"
    assert ssh_gateway._pull_comment_content(
        {"type": "thread-reply", "threadId": "t1", "parentId": "p1",
         "body": "B"}
    ) == "t1\x00p1\x00B"
    assert ssh_gateway._pull_comment_content(
        {"type": "thread-state", "threadId": "t1", "state": "resolved",
         "body": ""}
    ) == "t1\x00resolved\x00"
    assert ssh_gateway._pull_comment_content(
        {"type": "suggestion-state", "threadId": "t1", "state": "applied",
         "appliedCommit": "c" * 40, "body": ""}
    ) == "t1\x00applied\x00" + "c" * 40 + "\x00"


def test_pull_submission_signatures_bind_the_repository(tmp_path, monkeypatch):
    private, author = _ed25519_keypair(tmp_path)
    event = {
        "type": "comment", "body": "Looks good to me.", "author": author,
        "ts": 1_723_000_100_000,
    }
    body = (
        "comment\n3\n" + author + "\n" + str(event["ts"]) + "\n"
        + hashlib.sha256(event["body"].encode("utf-8")).hexdigest()
    )
    home = ssh_gateway._event_signing_repo_keys(("Alice-Node", "Widget"))
    other = ssh_gateway._event_signing_repo_keys(("mallory", "other"))
    assert home == ("alice-node/widget",)
    v2 = dict(event, sig=_sign(
        tmp_path, private,
        ("forkmesh-pull-comment-v2\nalice-node/widget\n" + body).encode()))
    submission = {"event": v2, "number": 3}
    assert ssh_gateway._verify_pull_submission(submission, 1 << 20, home)
    with pytest.raises(ssh_gateway.VerbError):
        ssh_gateway._verify_pull_submission(submission, 1 << 20, other)

    v1 = dict(event, sig=_sign(
        tmp_path, private, ("forkmesh-pull-comment-v1\n" + body).encode()))
    legacy = {"event": v1, "number": 3}
    assert ssh_gateway._verify_pull_submission(legacy, 1 << 20, other)
    monkeypatch.setattr(ssh_gateway, "EVENT_SIGNATURE_V1_INTAKE_UNTIL_MS", 0)
    with pytest.raises(ssh_gateway.VerbError):
        ssh_gateway._verify_pull_submission(legacy, 1 << 20, other)
    assert ssh_gateway._verify_pull_submission(submission, 1 << 20, home)

    pull = _signed_pull(tmp_path, private, author)
    content = "\x00".join([pull["title"], pull["base"], pull["head"],
                           pull["patch"], pull["commits"]])
    pull["sig"] = _sign(tmp_path, private, (
        "forkmesh-pull-event-v2\nalice-node/widget\n" + author + "\n"
        + str(pull["ts"]) + "\n"
        + hashlib.sha256(content.encode("utf-8")).hexdigest()).encode())
    assert ssh_gateway._verify_pull_submission({"pull": pull}, 1 << 20, home)
    with pytest.raises(ssh_gateway.VerbError):
        ssh_gateway._verify_pull_submission({"pull": pull}, 1 << 20, other)


def test_ed25519_verifier_accepts_valid_and_rejects_tampered(tmp_path):
    private, author = _ed25519_keypair(tmp_path)
    message = b"forkmesh-pull-event-v1\nA\n1\nabc"
    signature = _sign(tmp_path, private, message)
    assert ssh_gateway._verify_ed25519_signature(author, signature, message)
    assert not ssh_gateway._verify_ed25519_signature(
        author, signature, message + b"x"
    )
    assert not ssh_gateway._verify_ed25519_signature(
        author, "A" * 86, message
    )
    assert not ssh_gateway._verify_ed25519_signature(
        "notakey", signature, message
    )


# ---------------------------------------------------------------------------
# forkmesh verb parsing
# ---------------------------------------------------------------------------


def test_forkmesh_verbs_parse_exactly_and_git_commands_pass_through():
    assert ssh_gateway._parse_forkmesh_command(
        "forkmesh-pull-submit 'alice-node/widget.git'"
    ) == ("forkmesh-pull-submit", "alice-node", "widget")
    assert ssh_gateway._parse_forkmesh_command(
        "forkmesh-fork '/alice-node/widget'"
    ) == ("forkmesh-fork", "alice-node", "widget")
    assert ssh_gateway._parse_forkmesh_command(
        "git-upload-pack 'alice-node/widget.git'"
    ) is None
    assert ssh_gateway._parse_forkmesh_command("bash") is None
    for command in (
        "forkmesh-fork '../../etc'",
        "forkmesh-fork 'alice-node/widget' extra",
        "forkmesh-pull-submit 'alice-node/widget;id'",
    ):
        with pytest.raises(ssh_gateway.GatewayError):
            ssh_gateway._parse_forkmesh_command(command)


# ---------------------------------------------------------------------------
# forkmesh-pull-submit: spool acceptance
# ---------------------------------------------------------------------------


def _submit(monkeypatch, config, body, key_id=KEY_ID, account=""):
    monkeypatch.setattr(ssh_gateway, "_api", _authorize_api(account))
    monkeypatch.setattr(
        ssh_gateway.sys, "stdin", SimpleNamespace(buffer=io.BytesIO(body))
    )
    return ssh_gateway._serve_pull_submit(
        config, key_id, "alice-node", "widget"
    )


def test_pull_submit_spools_a_verified_submission_once(
    monkeypatch, tmp_path, capsys
):
    private, author = _ed25519_keypair(tmp_path)
    config = _spool_config(tmp_path)
    body = json.dumps(
        {"pull": _signed_pull(tmp_path, private, author)}
    ).encode()
    assert _submit(monkeypatch, config, body) == 0
    first = json.loads(capsys.readouterr().out)
    assert first["ok"] is True and first["duplicate"] is False
    spooled = list(config.pull_submission_spool.glob("*.json"))
    assert len(spooled) == 1
    assert spooled[0].name.startswith(KEY_ID + ".")
    assert (spooled[0].stat().st_mode & 0o777) == 0o600
    wrapper = json.loads(spooled[0].read_text(encoding="utf-8"))
    assert wrapper["type"] == ssh_gateway.PULL_SUBMISSION_TYPE
    assert wrapper["owner"] == "alice-node"
    assert wrapper["repository"] == "widget"
    assert wrapper["submission"]["pull"]["author"] == author
    # The same signed act spools exactly once.
    assert _submit(monkeypatch, config, body) == 0
    second = json.loads(capsys.readouterr().out)
    assert second["duplicate"] is True
    assert len(list(config.pull_submission_spool.glob("*.json"))) == 1


def test_pull_submit_accepts_signed_comment_events(
    monkeypatch, tmp_path, capsys
):
    private, author = _ed25519_keypair(tmp_path)
    config = _spool_config(tmp_path)
    body = json.dumps(
        {"event": _signed_comment(tmp_path, private, author, 7), "number": 7}
    ).encode()
    assert _submit(monkeypatch, config, body) == 0
    assert json.loads(capsys.readouterr().out)["ok"] is True


def test_pull_submit_rejects_bad_signatures_shapes_and_oversize(
    monkeypatch, tmp_path
):
    private, author = _ed25519_keypair(tmp_path)
    config = _spool_config(tmp_path)
    tampered = _signed_pull(tmp_path, private, author)
    tampered["title"] = "Different title"
    for body, message in (
        (json.dumps({"pull": tampered}).encode(), "signature"),
        (b"not json", "invalid"),
        (json.dumps({"pull": {}, "extra": 1}).encode(), "invalid"),
        (json.dumps({"event": {"type": "comment"}, "number": 0}).encode(),
         "invalid"),
    ):
        with pytest.raises(ssh_gateway.GatewayError, match=message):
            _submit(monkeypatch, config, body)
    small = _spool_config(tmp_path, pull_submission_max_bytes=200)
    oversized = _signed_pull(tmp_path, private, author, patch="x" * 400)
    with pytest.raises(ssh_gateway.GatewayError, match="too large"):
        _submit(monkeypatch, small, json.dumps({"pull": oversized}).encode())
    disabled = _spool_config(tmp_path, pull_submission_spool=None)
    with pytest.raises(ssh_gateway.VerbError, match="not enabled"):
        _submit(monkeypatch, disabled, b"{}")


def test_pull_submit_enforces_per_key_and_global_spool_quotas(
    monkeypatch, tmp_path
):
    private, author = _ed25519_keypair(tmp_path)
    config = _spool_config(tmp_path)
    for index in range(ssh_gateway.MAX_SPOOLED_SUBMISSIONS_PER_KEY):
        name = KEY_ID + "." + format(index, "032x") + ".json"
        (config.pull_submission_spool / name).write_text("{}")
    body = json.dumps(
        {"pull": _signed_pull(tmp_path, private, author)}
    ).encode()
    with pytest.raises(ssh_gateway.VerbError, match="quota"):
        _submit(monkeypatch, config, body)
    for index in range(ssh_gateway.MAX_SPOOLED_SUBMISSIONS):
        name = OTHER_KEY_ID + "." + format(index, "032x") + ".json"
        (config.pull_submission_spool / name).write_text("{}")
    with pytest.raises(ssh_gateway.VerbError, match="full"):
        _submit(monkeypatch, config, body)


# ---------------------------------------------------------------------------
# spool-forward replay
# ---------------------------------------------------------------------------


def _write_spool_entry(spool, name, owner="alice-node", repository="widget"):
    wrapper = {
        "schemaVersion": 1,
        "type": ssh_gateway.PULL_SUBMISSION_TYPE,
        "owner": owner,
        "repository": repository,
        "receivedAtMs": 1,
        "keyId": KEY_ID,
        "submission": {"pull": {"title": name}},
    }
    descriptor = os.open(
        spool / name, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600
    )
    os.write(descriptor, json.dumps(wrapper).encode())
    os.close(descriptor)


def test_spool_forward_moves_delivered_and_rejected_submissions(
    monkeypatch, tmp_path, capsys
):
    config = _spool_config(tmp_path)
    spool = config.pull_submission_spool
    _write_spool_entry(spool, KEY_ID + "." + "1" * 32 + ".json")
    _write_spool_entry(spool, KEY_ID + "." + "2" * 32 + ".json")
    _write_spool_entry(spool, KEY_ID + "." + "3" * 32 + ".json")
    statuses = iter((201, 400, 503))
    requests = []

    class Response:
        def __init__(self, status):
            self.status = status

        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return False

        def read(self, _maximum):
            return b"{}"

    class Opener:
        def open(self, request, timeout):
            requests.append(request)
            status = next(statuses)
            if status >= 400:
                raise ssh_gateway.HTTPError(
                    request.full_url, status, "err", {}, io.BytesIO(b"{}")
                )
            return Response(status)

    monkeypatch.setattr(
        ssh_gateway, "build_opener", lambda *handlers: Opener()
    )
    assert ssh_gateway._spool_forward(
        config, "https://app.forkmesh.example", 25
    ) == 0
    summary = json.loads(capsys.readouterr().out)
    assert summary == {
        "ok": True, "forwarded": 1, "rejected": 1, "deferred": 1,
    }
    assert len(list((spool / "forwarded").glob("*.json"))) == 1
    assert len(list((spool / "rejected").glob("*.json"))) == 1
    assert len(list(spool.glob("*.json"))) == 1
    assert requests[0].full_url == (
        "https://app.forkmesh.example/api/repo/alice-node/widget/pulls"
    )
    assert requests[0].get_header("Content-type") == "application/json"
    assert not requests[0].has_header("Authorization")
    with pytest.raises(ssh_gateway.GatewayError, match="API origin"):
        ssh_gateway._spool_forward(config, "http://insecure.example", 25)


def test_spool_forward_defers_everything_when_controller_is_unreachable(
    monkeypatch, tmp_path, capsys
):
    config = _spool_config(tmp_path)
    spool = config.pull_submission_spool
    _write_spool_entry(spool, KEY_ID + "." + "4" * 32 + ".json")
    _write_spool_entry(spool, KEY_ID + "." + "5" * 32 + ".json")

    class Opener:
        def open(self, request, timeout):
            raise ssh_gateway.URLError("unreachable")

    monkeypatch.setattr(
        ssh_gateway, "build_opener", lambda *handlers: Opener()
    )
    assert ssh_gateway._spool_forward(
        config, "https://app.forkmesh.example", 25
    ) == 0
    summary = json.loads(capsys.readouterr().out)
    assert summary["forwarded"] == 0 and summary["deferred"] == 2
    assert len(list(spool.glob("*.json"))) == 2


# ---------------------------------------------------------------------------
# forkmesh-fork
# ---------------------------------------------------------------------------


def _source_repository(tmp_path):
    root = tmp_path / "repos"
    work = tmp_path / "seed"
    bare = root / "alice-node" / "widget.git"
    bare.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["git", "init", "-b", "main", str(work)],
        check=True, capture_output=True)
    subprocess.run(
        ["git", "-C", str(work), "config", "user.name", "Test"], check=True)
    subprocess.run(
        ["git", "-C", str(work), "config", "user.email", "t@example.invalid"],
        check=True)
    (work / "README.md").write_text("fork me\n", encoding="utf-8")
    subprocess.run(["git", "-C", str(work), "add", "README.md"], check=True)
    subprocess.run(
        ["git", "-C", str(work), "commit", "-m", "seed"],
        check=True, capture_output=True)
    subprocess.run(
        ["git", "-C", str(work), "branch", "forkmesh/pulls"], check=True)
    subprocess.run(
        ["git", "-C", str(work), "branch", "feature"], check=True)
    subprocess.run(["git", "-C", str(work), "tag", "v1"], check=True)
    subprocess.run(
        ["git", "clone", "--bare", str(work), str(bare)],
        check=True, capture_output=True)
    return root, bare


def _real_limits(tmp_path):
    capacity = tmp_path / "capacity"
    capacity.mkdir(mode=0o700, exist_ok=True)
    return ssh_gateway.GatewayLimits(
        {**ssh_gateway.DEFAULT_LIMITS, "capacityDirectory": str(capacity)}
    )


def _fork_config(tmp_path, root, bare):
    registry = tmp_path / "dynamic-repositories.json"
    true_program = str(Path(shutil.which("true") or "/usr/bin/true").resolve())
    return SimpleNamespace(
        repositories={
            ("alice-node", "widget"): (bare, True, 16 * 1024 * 1024 * 1024)
        },
        repository_root=root,
        dynamic_repositories_file=registry,
        pull_submission_spool=None,
        pull_submission_max_bytes=ssh_gateway.DEFAULT_SUBMISSION_MAX_BYTES,
        limits=_real_limits(tmp_path),
        refresh_notifier=Path(true_program),
    )


def test_fork_creates_registry_entry_and_strips_source_ledger(
    monkeypatch, tmp_path, capsys
):
    root, bare = _source_repository(tmp_path)
    config = _fork_config(tmp_path, root, bare)
    monkeypatch.setattr(ssh_gateway, "_api", _authorize_api(account="carol"))
    assert ssh_gateway._serve_fork(
        config, KEY_ID, "alice-node", "widget"
    ) == 0
    result = json.loads(capsys.readouterr().out)
    assert result == {
        "ok": True,
        "owner": "carol",
        "repository": "widget",
        "forkedFrom": "alice-node/widget",
    }
    target = root / "carol" / "widget.git"
    refs = subprocess.run(
        ["git", "--git-dir", str(target), "for-each-ref",
         "--format=%(refname)"],
        check=True, capture_output=True, text=True).stdout.split()
    assert "refs/heads/main" in refs
    assert "refs/heads/feature" in refs
    assert "refs/tags/v1" in refs
    assert not any(ref.startswith("refs/heads/forkmesh/") for ref in refs)
    head = subprocess.run(
        ["git", "--git-dir", str(target), "symbolic-ref", "HEAD"],
        check=True, capture_output=True, text=True).stdout.strip()
    assert head == "refs/heads/main"
    registry = json.loads(
        config.dynamic_repositories_file.read_text(encoding="utf-8")
    )
    assert registry["type"] == ssh_gateway.DYNAMIC_REPOSITORIES_TYPE
    entry = registry["repositories"][0]
    assert entry["owner"] == "carol"
    assert entry["name"] == "widget"
    assert entry["path"] == str(target)
    assert entry["forkedFrom"] == "alice-node/widget"
    assert entry["createdByKeyId"] == KEY_ID
    # Forking the same repository again is a no-op error, not a overwrite.
    with pytest.raises(ssh_gateway.VerbError, match="already exists"):
        ssh_gateway._serve_fork(config, KEY_ID, "alice-node", "widget")


def test_fork_requires_local_account_and_registry(
    monkeypatch, tmp_path
):
    root, bare = _source_repository(tmp_path)
    config = _fork_config(tmp_path, root, bare)
    monkeypatch.setattr(ssh_gateway, "_api", _authorize_api(account=""))
    with pytest.raises(ssh_gateway.VerbError, match="local authorization"):
        ssh_gateway._serve_fork(config, KEY_ID, "alice-node", "widget")
    config.dynamic_repositories_file = None
    with pytest.raises(ssh_gateway.VerbError, match="not enabled"):
        ssh_gateway._serve_fork(config, KEY_ID, "alice-node", "widget")


def test_fork_registry_round_trips_into_gateway_and_broker_loaders(
    monkeypatch, tmp_path
):
    root, bare = _source_repository(tmp_path)
    config = _fork_config(tmp_path, root, bare)
    monkeypatch.setattr(ssh_gateway, "_api", _authorize_api(account="carol"))
    assert ssh_gateway._serve_fork(
        config, KEY_ID, "alice-node", "widget"
    ) == 0
    entries = ssh_gateway._load_dynamic_repositories(config)
    assert entries == [
        {
            "owner": "carol",
            "name": "widget",
            "path": root / "carol" / "widget.git",
        }
    ]
    broker_config = SimpleNamespace(
        dynamic_repositories_file=config.dynamic_repositories_file,
        git_uid=os.geteuid(),
    )
    assert broker._load_dynamic_repositories(broker_config) == {
        ("carol", "widget"): "widget"
    }


# ---------------------------------------------------------------------------
# gateway configuration: registry merge and spool fields
# ---------------------------------------------------------------------------


def _gateway_config(tmp_path, **overrides):
    root = tmp_path / "repos"
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
        "limits": {
            **ssh_gateway.DEFAULT_LIMITS,
            "capacityDirectory": str(capacity),
        },
        "repositories": [
            {
                "owner": "alice-node",
                "name": "widget",
                "path": "alice-node/widget.git",
                "access": "read-write",
            }
        ],
    }
    config.update(overrides)
    path = tmp_path / "gateway.json"
    path.write_text(json.dumps(config), encoding="utf-8")
    path.chmod(0o644)
    return path, root


def _registry_file(tmp_path, entries):
    registry = tmp_path / "dynamic-repositories.json"
    registry.write_text(
        json.dumps(
            {
                "schemaVersion": 1,
                "type": ssh_gateway.DYNAMIC_REPOSITORIES_TYPE,
                "repositories": entries,
            }
        ),
        encoding="utf-8",
    )
    registry.chmod(0o644)
    return registry


def test_config_merges_dynamic_registry_repositories(tmp_path):
    registry = _registry_file(
        tmp_path,
        [
            {
                "owner": "carol",
                "name": "widget",
                "path": str(tmp_path / "repos" / "carol" / "widget.git"),
                "forkedFrom": "alice-node/widget",
            }
        ],
    )
    path, root = _gateway_config(
        tmp_path, dynamicRepositoriesFile=str(registry)
    )
    config = ssh_gateway.Config(path)
    assert ("carol", "widget") in config.repositories
    target, writable, quota = config.repositories[("carol", "widget")]
    assert target == root / "carol" / "widget.git"
    assert writable is True
    assert quota == config.limits.default_repository_max_bytes
    # A missing registry file means no forks yet, never a config failure.
    registry.unlink()
    config = ssh_gateway.Config(path)
    assert ("carol", "widget") not in config.repositories


def test_config_rejects_unsafe_or_colliding_registry_entries(tmp_path):
    outside = _registry_file(
        tmp_path,
        [{"owner": "carol", "name": "widget", "path": "/etc/widget.git"}],
    )
    path, _root = _gateway_config(
        tmp_path, dynamicRepositoriesFile=str(outside)
    )
    with pytest.raises(ssh_gateway.GatewayError, match="dynamic"):
        ssh_gateway.Config(path)
    colliding = _registry_file(
        tmp_path,
        [
            {
                "owner": "alice-node",
                "name": "widget",
                "path": str(tmp_path / "repos" / "elsewhere.git"),
            }
        ],
    )
    path, _root = _gateway_config(
        tmp_path, dynamicRepositoriesFile=str(colliding)
    )
    with pytest.raises(ssh_gateway.GatewayError, match="duplicate"):
        ssh_gateway.Config(path)


def test_config_validates_spool_fields(tmp_path):
    spool = tmp_path / "config-spool"
    spool.mkdir(mode=0o700)
    path, _root = _gateway_config(
        tmp_path,
        pullSubmissionSpool=str(spool),
        pullSubmissionMaxBytes=1024 * 1024,
    )
    config = ssh_gateway.Config(path)
    assert config.pull_submission_spool == spool
    assert config.pull_submission_max_bytes == 1024 * 1024
    path, _root = _gateway_config(tmp_path, pullSubmissionMaxBytes=1024 * 1024)
    with pytest.raises(ssh_gateway.GatewayError):
        ssh_gateway.Config(path)
    path, _root = _gateway_config(
        tmp_path,
        pullSubmissionSpool=str(spool),
        pullSubmissionMaxBytes=7,
    )
    with pytest.raises(ssh_gateway.GatewayError):
        ssh_gateway.Config(path)
    path, _root = _gateway_config(tmp_path, unknownField=True)
    with pytest.raises(ssh_gateway.GatewayError):
        ssh_gateway.Config(path)


# ---------------------------------------------------------------------------
# pull-request ref namespace restriction
# ---------------------------------------------------------------------------


def test_serve_applies_pull_request_ref_namespace(monkeypatch, tmp_path):
    repo_path = Path("/srv/forkmesh/git/alice-node/widget.git")
    config = SimpleNamespace(
        repositories={("alice-node", "widget"): (repo_path, True)},
        repository_root=Path("/srv/forkmesh/git"),
        refresh_notifier=Path("/opt/forkmesh/ssh-refresh-notify"),
        limits=_real_limits(tmp_path),
    )
    monkeypatch.setenv(
        "SSH_ORIGINAL_COMMAND", "git-receive-pack 'alice-node/widget.git'"
    )

    def fake_api(_config, payload):
        return {
            "authorized": True,
            "keyId": payload["keyId"],
            "owner": payload["owner"],
            "repository": payload["repository"],
            "operation": payload["operation"],
            "repositoryRelativePath": (
                payload["owner"] + "/" + payload["repository"] + ".git"
            ),
            "account": "carol",
            "refNamespace": "refs/heads/pr/carol/",
        }

    monkeypatch.setattr(ssh_gateway, "_api", fake_api)
    monkeypatch.setattr(ssh_gateway, "_bare_repository", lambda path: True)
    monkeypatch.setattr(Path, "resolve", lambda self, strict=False: self)
    monkeypatch.setattr(
        ssh_gateway, "_repository_size_bytes", lambda *_args, **_kwargs: 0
    )
    commands = []

    def fake_git(command, environment, **kwargs):
        commands.append(command)
        return 0

    monkeypatch.setattr(ssh_gateway, "_run_git_service", fake_git)
    monkeypatch.setattr(
        ssh_gateway.subprocess,
        "run",
        lambda command, **kwargs: SimpleNamespace(returncode=0),
    )
    assert ssh_gateway._serve(config, KEY_ID) == 0
    git_command = commands[0]
    hide_refs = [
        git_command[index + 1]
        for index, token in enumerate(git_command)
        if token == "-c" and git_command[index + 1].startswith(
            "receive.hideRefs="
        )
    ]
    # Later entries win in git's hideRefs evaluation: hide everything, then
    # re-expose only the account's pull-request namespace.
    assert hide_refs == [
        "receive.hideRefs=refs/forkmesh/",
        "receive.hideRefs=refs",
        "receive.hideRefs=!refs/heads/pr/carol/",
    ]


def test_serve_rejects_mismatched_or_malformed_ref_namespace(
    monkeypatch, tmp_path
):
    repo_path = Path("/srv/forkmesh/git/alice-node/widget.git")
    config = SimpleNamespace(
        repositories={("alice-node", "widget"): (repo_path, True)},
        repository_root=Path("/srv/forkmesh/git"),
        refresh_notifier=Path("/opt/forkmesh/ssh-refresh-notify"),
        limits=_real_limits(tmp_path),
    )
    monkeypatch.setenv(
        "SSH_ORIGINAL_COMMAND", "git-receive-pack 'alice-node/widget.git'"
    )
    for namespace, account in (
        ("refs/heads/pr/other/", "carol"),
        ("refs/heads/main", ""),
        ("refs/forkmesh/x/", ""),
    ):
        def fake_api(_config, payload, _namespace=namespace, _account=account):
            result = {
                "authorized": True,
                "keyId": payload["keyId"],
                "owner": payload["owner"],
                "repository": payload["repository"],
                "operation": payload["operation"],
                "repositoryRelativePath": (
                    payload["owner"] + "/" + payload["repository"] + ".git"
                ),
                "refNamespace": _namespace,
            }
            if _account:
                result["account"] = _account
            return result

        monkeypatch.setattr(ssh_gateway, "_api", fake_api)
        with pytest.raises(ssh_gateway.GatewayError, match="invalid"):
            ssh_gateway._serve(config, KEY_ID)


def test_real_receive_pack_denies_pushes_outside_pull_namespace(tmp_path):
    root, bare = _source_repository(tmp_path)
    clone = tmp_path / "contributor"
    subprocess.run(
        ["git", "clone", str(bare), str(clone)],
        check=True, capture_output=True)
    subprocess.run(
        ["git", "-C", str(clone), "config", "user.name", "Carol"], check=True)
    subprocess.run(
        ["git", "-C", str(clone), "config", "user.email", "c@example.invalid"],
        check=True)
    (clone / "change.txt").write_text("change\n", encoding="utf-8")
    subprocess.run(["git", "-C", str(clone), "add", "change.txt"], check=True)
    subprocess.run(
        ["git", "-C", str(clone), "commit", "-m", "change"],
        check=True, capture_output=True)
    receive_pack = (
        "git -c receive.hideRefs=refs/forkmesh/ -c receive.hideRefs=refs "
        "-c 'receive.hideRefs=!refs/heads/pr/carol/' receive-pack"
    )
    allowed = subprocess.run(
        [
            "git", "-C", str(clone), "push",
            "--receive-pack=" + receive_pack,
            str(bare), "HEAD:refs/heads/pr/carol/change",
        ],
        capture_output=True, text=True)
    assert allowed.returncode == 0, allowed.stderr
    denied = subprocess.run(
        [
            "git", "-C", str(clone), "push",
            "--receive-pack=" + receive_pack,
            str(bare), "HEAD:refs/heads/main",
        ],
        capture_output=True, text=True)
    assert denied.returncode != 0
    assert "hidden ref" in denied.stderr


# ---------------------------------------------------------------------------
# broker: local authorization
# ---------------------------------------------------------------------------


def _broker_config(tmp_path, mode="local", dynamic=None):
    return broker.BrokerConfig(
        api_origin="" if mode == "local" else "https://forkmesh.example",
        token="" if mode == "local" else "t" * 48,
        socket_path=tmp_path / "authorize.sock",
        socket_gid=123,
        lookup_uid=1001,
        git_uid=os.geteuid(),
        local_mode=mode,
        grants_file=tmp_path / "grants.json",
        dynamic_repositories_file=dynamic,
    )


def _key_line():
    key_type = b"ssh-ed25519"
    material = b"\x42" * 32
    blob = (
        len(key_type).to_bytes(4, "big")
        + key_type
        + len(material).to_bytes(4, "big")
        + material
    )
    return "ssh-ed25519 " + base64.b64encode(blob).decode()


def _write_grants(tmp_path, grants):
    grants_file = tmp_path / "grants.json"
    grants_file.write_text(
        json.dumps(
            {
                "schemaVersion": 1,
                "type": broker.GRANTS_TYPE,
                "grants": grants,
            }
        ),
        encoding="utf-8",
    )
    grants_file.chmod(0o644)
    return grants_file


def _grant(**overrides):
    grant = {
        "keyId": KEY_ID,
        "publicKey": _key_line(),
        "account": "carol",
        "repositories": [
            {"owner": "alice-node", "name": "widget", "access": "pull-request"}
        ],
    }
    grant.update(overrides)
    return grant


def test_local_lookup_and_authorize_resolve_grants(tmp_path):
    _write_grants(tmp_path, [_grant()])
    config = _broker_config(tmp_path)
    lookup = broker._authorize_local(
        config, {"action": "lookup", "publicKey": _key_line()}
    )
    assert lookup == {"authorized": True, "keyId": KEY_ID}
    with pytest.raises(broker.BrokerError, match="denied"):
        broker._authorize_local(
            config,
            {"action": "lookup", "publicKey": "ssh-ed25519 QUJD"},
        )
    read = broker._authorize_local(
        config,
        {
            "action": "authorize",
            "keyId": KEY_ID,
            "owner": "alice-node",
            "repository": "widget",
            "operation": "git-upload-pack",
        },
    )
    assert read["authorized"] is True
    assert read["repositoryRelativePath"] == "alice-node/widget.git"
    assert read["account"] == "carol"
    assert "refNamespace" not in read
    push = broker._authorize_local(
        config,
        {
            "action": "authorize",
            "keyId": KEY_ID,
            "owner": "alice-node",
            "repository": "widget",
            "operation": "git-receive-pack",
        },
    )
    assert push["refNamespace"] == "refs/heads/pr/carol/"


def test_local_authorize_enforces_access_levels_and_ownership(tmp_path):
    dynamic = tmp_path / "dynamic.json"
    dynamic.write_text(
        json.dumps(
            {
                "schemaVersion": 1,
                "type": broker.DYNAMIC_REPOSITORIES_TYPE,
                "repositories": [
                    {"owner": "carol", "name": "widget", "path": "x"},
                    {"owner": "dave", "name": "widget", "path": "y"},
                ],
            }
        ),
        encoding="utf-8",
    )
    dynamic.chmod(0o644)
    _write_grants(
        tmp_path,
        [
            _grant(
                repositories=[
                    {
                        "owner": "alice-node",
                        "name": "widget",
                        "access": "read-only",
                    }
                ]
            )
        ],
    )
    config = _broker_config(tmp_path, dynamic=dynamic)

    def authorize(owner, repository, operation):
        return broker._authorize_local(
            config,
            {
                "action": "authorize",
                "keyId": KEY_ID,
                "owner": owner,
                "repository": repository,
                "operation": operation,
            },
        )

    with pytest.raises(broker.BrokerError, match="denied"):
        authorize("alice-node", "widget", "git-receive-pack")
    # A grant's own dynamically created fork is implicitly read-write.
    own_fork = authorize("carol", "widget", "git-receive-pack")
    assert own_fork["repositoryRelativePath"] == "carol/widget.git"
    assert "refNamespace" not in own_fork
    # Another account's fork is not.
    with pytest.raises(broker.BrokerError, match="denied"):
        authorize("dave", "widget", "git-upload-pack")
    with pytest.raises(broker.BrokerError, match="denied"):
        authorize("unknown", "widget", "git-upload-pack")


def test_fallback_mode_only_engages_when_worker_is_unreachable(
    monkeypatch, tmp_path
):
    _write_grants(tmp_path, [_grant()])
    config = _broker_config(tmp_path, mode="fallback")
    request = {"action": "lookup", "publicKey": _key_line()}

    def unavailable(_config, _value):
        raise broker.BrokerError(broker.UPSTREAM_UNAVAILABLE)

    monkeypatch.setattr(broker, "_authorize_upstream", unavailable)
    assert broker._authorize_request(config, request) == {
        "authorized": True,
        "keyId": KEY_ID,
    }

    def denied(_config, _value):
        raise broker.BrokerError("authorization service denied access")

    monkeypatch.setattr(broker, "_authorize_upstream", denied)
    with pytest.raises(broker.BrokerError, match="denied access"):
        broker._authorize_request(config, request)
    off = broker.BrokerConfig(
        api_origin="https://forkmesh.example",
        token="t" * 48,
        socket_path=tmp_path / "authorize.sock",
        socket_gid=123,
        lookup_uid=1001,
        git_uid=1002,
    )
    monkeypatch.setattr(broker, "_authorize_upstream", unavailable)
    with pytest.raises(broker.BrokerError, match="unavailable"):
        broker._authorize_request(off, request)


def test_local_authorization_config_shapes_are_strict():
    settings = broker._local_authorization_settings
    assert settings(None, worker_configured=True) == ("off", None, None)
    with pytest.raises(broker.BrokerError):
        settings(None, worker_configured=False)
    mode, grants, dynamic = settings(
        {"mode": "local", "grantsFile": "/etc/forkmesh/grants.json"},
        worker_configured=False,
    )
    assert (mode, grants, dynamic) == (
        "local", Path("/etc/forkmesh/grants.json"), None,
    )
    mode, _grants, dynamic = settings(
        {
            "mode": "fallback",
            "grantsFile": "/etc/forkmesh/grants.json",
            "dynamicRepositoriesFile": "/var/lib/forkmesh/dynamic.json",
        },
        worker_configured=True,
    )
    assert mode == "fallback"
    assert dynamic == Path("/var/lib/forkmesh/dynamic.json")
    for value, worker in (
        ({"mode": "local", "grantsFile": "/g.json"}, True),
        ({"mode": "fallback", "grantsFile": "/g.json"}, False),
        ({"mode": "always", "grantsFile": "/g.json"}, True),
        ({"mode": "local", "grantsFile": "relative.json"}, False),
        ({"mode": "local"}, False),
        ({"mode": "local", "grantsFile": "/g.json", "extra": 1}, False),
    ):
        with pytest.raises(broker.BrokerError):
            settings(value, worker_configured=worker)


def test_local_grants_store_is_validated_strictly(tmp_path):
    config = _broker_config(tmp_path)
    for grants in (
        [_grant(keyId="bad")],
        [_grant(account="Bad Account")],
        [_grant(), _grant()],
        [_grant(repositories=[{"owner": "a", "name": "w"}])],
        [
            _grant(
                repositories=[
                    {"owner": "alice-node", "name": "widget", "access": "root"}
                ]
            )
        ],
    ):
        _write_grants(tmp_path, grants)
        with pytest.raises(broker.BrokerError, match="invalid"):
            broker._authorize_local(
                config, {"action": "lookup", "publicKey": _key_line()}
            )
