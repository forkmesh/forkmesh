"""Secret gate: one contract across boundaries, measured on a maintained corpus.

Covers the corpus bounds at every boundary, parity with the desktop scanner,
organization/repository policy, signed overrides, client hooks that stop a
secret before any remote receives it, node pre-receive refusal, and the
response plan for secrets found after replication.
"""

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import node_operator_policy as policy_mod  # noqa: E402
import secret_gate as gate  # noqa: E402


CORPUS = ROOT / "conformance" / "secrets" / "v1" / "corpus.json"
BUILDER = ROOT / "conformance" / "secrets" / "v1" / "build_corpus.py"
DOC = ROOT / "www" / "docs" / "security" / "secret-scanning.md"


def synthetic(seed, length):
    alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
    digest = b""
    counter = 0
    while len(digest) < length:
        digest += hashlib.sha256(("%s:%d" % (seed, counter)).encode()).digest()
        counter += 1
    return "".join(alphabet[byte % len(alphabet)] for byte in digest[:length])


# Built at runtime so this file never holds a contiguous token.
TOKEN = "gh" + "p_" + synthetic("test-secret-gate", 36)
TOKEN_LINE = "GITHUB_TOKEN=%s\n" % TOKEN
PASSWORD_LINE = 'DB_PASS' + 'WORD = "%s"\n' % synthetic("pw", 24)

ENV = {
    "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
    "GIT_CONFIG_GLOBAL": os.devnull,
    "GIT_CONFIG_NOSYSTEM": "1",
    "GIT_AUTHOR_NAME": "Dev",
    "GIT_AUTHOR_EMAIL": "dev@example.test",
    "GIT_COMMITTER_NAME": "Dev",
    "GIT_COMMITTER_EMAIL": "dev@example.test",
    "LANG": "C",
}


def git(*arguments, cwd=None, check=True):
    completed = subprocess.run(
        ["git", *arguments], cwd=cwd, capture_output=True, text=True,
        env=dict(ENV, HOME=str(cwd or ROOT)), timeout=120, check=False)
    if check and completed.returncode != 0:
        raise AssertionError(completed.stderr)
    return completed


def make_client(tmp_path, label="client"):
    client = tmp_path / label
    git("init", "-q", "-b", "main", str(client))
    (client / "README.md").write_text("# app\n", encoding="utf-8")
    git("add", ".", cwd=client)
    git("commit", "-q", "--no-verify", "-m", "init", cwd=client)
    return client


def commit(client, relative, text, *extra):
    target = client / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text, encoding="utf-8")
    git("add", ".", cwd=client)
    return git("commit", "-q", *extra, "-m", "add " + relative, cwd=client,
               check=False)


def blob_of(client, relative):
    return git("rev-parse", "HEAD:" + relative, cwd=client).stdout.strip()


def org(**document):
    return gate.normalize_policy(document)


def repo(**document):
    return gate.normalize_policy(document, repository=True)


# --------------------------------------------------------------------------
# Contract and corpus


def test_corpus_is_generated_from_its_readable_source():
    completed = subprocess.run(
        [sys.executable, str(BUILDER), "--check"], capture_output=True,
        text=True, timeout=60, check=False)
    assert completed.returncode == 0, completed.stderr


def test_corpus_rates_are_bounded_at_every_boundary(tmp_path):
    corpus = json.loads(CORPUS.read_text(encoding="utf-8"))
    result = gate.evaluate_corpus(CORPUS, tmp_path)
    assert result["ok"], json.dumps(result, indent=2)
    assert set(result["boundaries"]) == set(corpus["boundaries"])
    assert result["rulesWithoutMaliciousCases"] == []
    labels = {case["label"] for case in corpus["cases"]}
    assert labels == {"benign", "malicious"}
    for boundary, measured in result["boundaries"].items():
        assert measured["malicious"] >= len(gate.RULES), boundary
        assert measured["benign"] >= 15, boundary
        assert (measured["falseNegativeRate"]
                <= corpus["bounds"]["maxFalseNegativeRate"])
        assert (measured["falsePositiveRate"]
                <= corpus["bounds"]["maxFalsePositiveRate"])
        # Only documented limitations are misclassified.
        assert measured["unexpected"] == []


def test_corpus_misclassification_outside_known_limitations_fails(tmp_path):
    corpus = json.loads(CORPUS.read_text(encoding="utf-8"))
    for case in corpus["cases"]:
        if case["id"] == "b-short-password":
            case["content"] = [TOKEN_LINE[:10], TOKEN_LINE[10:]]
    mutated = tmp_path / "corpus.json"
    mutated.write_text(json.dumps(corpus), encoding="utf-8")
    result = gate.evaluate_corpus(mutated, tmp_path / "scratch")
    assert result["ok"] is False
    assert all("b-short-password" in item["unexpected"]
               for item in result["boundaries"].values())


def _desktop_patterns():
    source = (ROOT / "desktop" / "src" / "RepoSecurity.cpp").read_text(
        encoding="utf-8")
    block = source.split("const QList<SecretPattern> &secretPatterns()", 1)[1]
    block = block.split("return kPatterns;", 1)[0]
    entries = re.findall(
        r'\{QStringLiteral\("([^"]+)"\),\s*QRegularExpression\(QStringLiteral\('
        r'((?:\s*"(?:[^"\\]|\\.)*")+)\)\)\}', block)
    patterns = {}
    for title, literals in entries:
        text = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', literals))
        patterns[title] = re.sub(r"\\(.)", r"\1", text)
    return patterns, source


def test_desktop_scanner_carries_the_contract_patterns():
    patterns, source = _desktop_patterns()
    assert patterns == {rule["title"]: rule["pattern"] for rule in gate.RULES}
    markers_block = source.split("bool looksLikePlaceholderValue", 1)[1]
    markers_block = markers_block.split("};", 1)[0]
    markers = tuple(re.findall(r'QStringLiteral\("([^"]*)"\)', markers_block))
    assert markers == gate.PLACEHOLDER_MARKERS
    assert gate.IGNORE_MARKER in source


def test_public_report_scanner_detects_through_the_contract(tmp_path):
    import security_scan

    (tmp_path / "settings.py").write_text(TOKEN_LINE, encoding="utf-8")
    (tmp_path / "dev.py").write_text(
        'password = "correct-horse-battery-staple-example"\n', encoding="utf-8")
    policy = json.loads(
        (ROOT / "www" / "docs" / "security-scan-policy.json").read_text())
    assert "secretRules" not in policy
    assert policy["secretContract"] == gate.CONTRACT_ID
    findings = security_scan.scan_secrets(
        tmp_path, [tmp_path / "settings.py", tmp_path / "dev.py"], policy)
    assert [(item["ruleId"], item["path"]) for item in findings] == [
        ("github-token", "settings.py")]
    assert TOKEN not in json.dumps(findings)
    with pytest.raises(security_scan.ScanError):
        security_scan.scan_secrets(tmp_path, [], dict(policy, secretContract="x"))


def test_new_files_do_not_trip_the_contract():
    paths = [
        CORPUS, BUILDER, ROOT / "tools" / "secret_gate.py", Path(__file__),
        DOC, ROOT / "www" / "docs" / "operations" / "node-operator-policy.md",
    ]
    report = gate.scan_files(
        [(str(path.relative_to(ROOT)), path.read_bytes()) for path in paths])
    assert report["findings"] == []


def test_findings_never_carry_values_or_value_digests():
    report = gate.scan_files([("app/.env", TOKEN_LINE.encode())])
    (finding,) = report["findings"]
    assert finding["rule"] == "github-token" and finding["outcome"] == "block"
    serialized = json.dumps(report)
    assert TOKEN not in serialized
    assert hashlib.sha256(TOKEN.encode()).hexdigest()[:16] not in serialized
    # The same value at another path or in other content is a new finding.
    other = gate.scan_files([("app/.env", (TOKEN_LINE + "#\n").encode())])
    assert other["findings"][0]["fingerprint"] != finding["fingerprint"]
    plan = gate.response_plan(report)
    assert plan["rotationByRule"][0]["rule"] == "github-token"


def test_review_diff_scans_only_added_lines():
    diff = "\n".join([
        "diff --git a/app.env b/app.env",
        "--- a/app.env", "+++ b/app.env", "@@ -1,2 +1,2 @@",
        "-" + TOKEN_LINE.strip(), " keep=1", "+" + PASSWORD_LINE.strip(), ""])
    report = gate.scan_diff(diff)
    assert [(f["rule"], f["line"]) for f in report["findings"]] == [
        ("generic-secret-assignment", 2)]


# --------------------------------------------------------------------------
# Policy


def test_repository_policy_can_only_tighten():
    organization = org(mode="warn", rules={"github-token": "block"})
    loosening = repo(mode="off", rules={"github-token": "off"})
    tightening = repo(rules={"generic-secret-assignment": "block"})
    effective = gate.effective_policy(organization, [loosening, tightening])
    assert effective["ruleActions"]["github-token"] == "block"
    assert effective["ruleActions"]["generic-secret-assignment"] == "block"
    assert effective["ruleActions"]["aws-access-key"] == "warn"


def test_repository_allowlist_and_custom_rules_need_organization_opt_in():
    files = [("tests/fixtures/token.txt", TOKEN_LINE.encode()),
             ("src/acme.txt", b"acme_" + b"k" * 3 + synthetic("a", 30).encode())]
    repository = repo(
        allowlist=[{"paths": ["tests/fixtures/*"], "reason": "fake fixtures"}],
        customRules=[{"id": "custom-acme", "pattern": r"acme_[A-Za-z0-9]{30,}"}])
    closed = gate.scan_files(files, organization=org(),
                             repository_policies=[repository])
    assert closed["counts"]["block"] == 1
    assert closed["ignoredRepositoryAllowlist"] == 1
    assert closed["ignoredRepositoryCustomRules"] == 1
    opened = gate.scan_files(
        files, organization=org(allowRepositoryAllowlist=True,
                                allowRepositoryCustomRules=True),
        repository_policies=[repository])
    outcomes = sorted((f["rule"], f["outcome"]) for f in opened["findings"])
    assert outcomes == [("custom-acme", "block"), ("github-token", "allowlisted")]


def test_organization_can_refuse_inline_ignore_markers():
    text = TOKEN_LINE.strip() + "  # " + gate.IGNORE_MARKER + "\n"
    assert gate.scan_files([("a.env", text.encode())])["findings"] == []
    strict = gate.scan_files([("a.env", text.encode())],
                             organization=org(honorIgnoreMarkers=False))
    assert strict["blocked"] is True


@pytest.mark.parametrize("document, message", [
    ({"rules": {"not-a-rule": "block"}}, "unknown fields"),
    ({"mode": "deny"}, "mode"),
    ({"customRules": [{"id": "acme", "pattern": "x"}]}, "custom-"),
    ({"customRules": [{"id": "custom-a", "pattern": "("}]}, "compile"),
    ({"customRules": [{"id": "custom-a", "pattern": "a*"}]}, "empty"),
    ({"allowlist": [{"paths": ["**"], "reason": "everything"}]}, "every path"),
    ({"allowlist": [{"paths": ["tests/*"], "reason": "x"}]}, "reason"),
    ({"overrides": {"allowedSignersPath": "signers"}}, "absolute"),
])
def test_invalid_policies_are_refused(document, message):
    with pytest.raises(gate.GateError, match=message):
        gate.normalize_policy(document)


# --------------------------------------------------------------------------
# Signed overrides


@pytest.fixture
def signer(tmp_path):
    key = tmp_path / "keys" / "alice"
    key.parent.mkdir()
    subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C",
                    "alice", "-f", str(key)], check=True, timeout=60)
    allowed = tmp_path / "keys" / "allowed_signers"
    allowed.write_text('alice@example.test namespaces="%s" %s' % (
        gate.OVERRIDE_NAMESPACE, (key.with_suffix(".pub")).read_text()),
        encoding="utf-8")
    return {"key": key, "allowed": allowed}


def _override(signer, fingerprints, **extra):
    options = dict(key=signer["key"], signer="alice@example.test",
                   repository="alice/app", fingerprints=fingerprints,
                   reason="synthetic fixture for the token parser", days=7)
    options.update(extra)
    return gate.sign_override(**options)


def test_signed_override_lifts_only_the_named_blocks(signer):
    files = [("tests/a.env", TOKEN_LINE.encode()),
             ("tests/b.env", PASSWORD_LINE.encode())]
    policy = org(overrides={"allowedSignersPath": str(signer["allowed"])})
    first = gate.scan_files(files, organization=policy)
    by_path = {f["path"]: f for f in first["findings"]}
    document = _override(signer, [by_path["tests/a.env"]["fingerprint"]])
    report = gate.scan_files(
        files, organization=policy, repository="alice/app",
        override_documents=[("o.json", json.dumps(document).encode())])
    outcomes = {f["path"]: f["outcome"] for f in report["findings"]}
    assert outcomes == {"tests/a.env": "overridden", "tests/b.env": "block"}
    assert report["overrides"][0]["signer"] == "alice@example.test"
    assert report["blocked"] is True


def test_invalid_overrides_are_ignored_and_reported(signer):
    files = [("a.env", TOKEN_LINE.encode())]
    policy = org(overrides={"allowedSignersPath": str(signer["allowed"])})
    fingerprint = gate.scan_files(files, organization=policy)["findings"][0][
        "fingerprint"]
    valid = _override(signer, [fingerprint])
    tampered = dict(valid, fingerprints=[fingerprint, "0" * 32])
    other_repo = _override(signer, [fingerprint], repository="bob/app")
    expired = _override(signer, [fingerprint], now=1_000_000_000.0)
    unsigned = dict(valid, signature="")
    impostor = dict(valid, signer="mallory@example.test")
    cases = {
        "tampered": (tampered, "does not verify"),
        "other-repo": (other_repo, "different repository"),
        "expired": (expired, "expired"),
        "unsigned": (unsigned, "not signed"),
        "impostor": (impostor, "does not verify"),
    }
    report = gate.scan_files(
        files, organization=policy, repository="alice/app",
        override_documents=[(name, json.dumps(doc).encode())
                            for name, (doc, _msg) in cases.items()])
    assert report["blocked"] is True
    reasons = {item["source"]: item["reason"] for item in report["invalidOverrides"]}
    for name, (_doc, message) in cases.items():
        assert message in reasons[name], (name, reasons[name])
    no_signers = gate.scan_files(
        files, organization=org(), repository="alice/app",
        override_documents=[("valid", json.dumps(valid).encode())])
    assert no_signers["blocked"] is True
    assert "accepts no overrides" in no_signers["invalidOverrides"][0]["reason"]


# --------------------------------------------------------------------------
# Client hooks: blocked before any remote receipt


def _install(client, tmp_path, **policy):
    policy_path = tmp_path / "org-policy.json"
    policy_path.write_text(json.dumps(policy), encoding="utf-8")
    return gate.install_hooks(client, organization_policy=policy_path,
                              repository="alice/app")


def test_pre_commit_hook_blocks_before_the_commit_exists(tmp_path):
    client = make_client(tmp_path)
    before = git("rev-parse", "HEAD", cwd=client).stdout
    result = _install(client, tmp_path)
    assert len(result["installed"]) == 2
    committed = commit(client, "deploy/.env", TOKEN_LINE)
    assert committed.returncode != 0
    assert "blocked this commit" in committed.stderr
    assert "github-token" in committed.stderr
    assert gate.PERMANENCE_WARNING in committed.stderr.replace("\n", " ")
    assert TOKEN not in committed.stderr
    assert git("rev-parse", "HEAD", cwd=client).stdout == before


def test_warn_policy_commits_with_a_permanence_warning(tmp_path):
    client = make_client(tmp_path)
    _install(client, tmp_path, mode="warn")
    committed = commit(client, "deploy/.env", TOKEN_LINE)
    assert committed.returncode == 0, committed.stderr
    assert "warning" in committed.stderr
    assert "cannot recall" in committed.stderr


def test_pre_push_hook_blocks_before_the_remote_receives_objects(tmp_path):
    remote = tmp_path / "remote.git"
    git("init", "-q", "--bare", "-b", "main", str(remote))
    client = make_client(tmp_path)
    git("push", "-q", str(remote), "main", cwd=client)
    remote_tip = git("--git-dir", str(remote), "rev-parse", "main").stdout
    _install(client, tmp_path)
    # Bypassing the local commit hook still cannot bypass the push boundary.
    assert commit(client, "src/config.py", TOKEN_LINE, "--no-verify").returncode == 0
    blob = blob_of(client, "src/config.py")
    pushed = git("push", str(remote), "main", cwd=client, check=False)
    assert pushed.returncode != 0
    assert "blocked this push" in pushed.stderr
    assert git("--git-dir", str(remote), "rev-parse", "main").stdout == remote_tip
    assert git("--git-dir", str(remote), "cat-file", "-e", blob,
               check=False).returncode != 0


def test_pre_push_hook_with_policy_options_lets_a_clean_push_through(tmp_path):
    remote = tmp_path / "remote.git"
    git("init", "-q", "--bare", "-b", "main", str(remote))
    client = make_client(tmp_path)
    git("push", "-q", str(remote), "main", cwd=client)
    _install(client, tmp_path)
    assert commit(client, "notes.txt", "nothing secret\n", "--no-verify").returncode == 0
    pushed = git("push", str(remote), "main", cwd=client, check=False)
    assert pushed.returncode == 0, pushed.stderr
    assert "unrecognized arguments" not in pushed.stderr


def test_hooks_installed_with_options_after_the_stage_still_parse(tmp_path):
    remote = tmp_path / "remote.git"
    git("init", "-q", "--bare", "-b", "main", str(remote))
    client = make_client(tmp_path)
    git("push", "-q", str(remote), "main", cwd=client)
    _install(client, tmp_path)
    hook = client / ".git" / "hooks" / "pre-push"
    hook.write_text(
        '#!/bin/sh\n%s\nexec python3 -I %s hook pre-push --repository alice/app "$@"\n'
        % (gate.HOOK_MARKER, Path(gate.__file__).resolve()), encoding="utf-8")
    assert commit(client, "notes.txt", "nothing secret\n", "--no-verify").returncode == 0
    clean = git("push", str(remote), "main", cwd=client, check=False)
    assert clean.returncode == 0, clean.stderr
    assert commit(client, "src/config.py", TOKEN_LINE, "--no-verify").returncode == 0
    blocked = git("push", str(remote), "main", cwd=client, check=False)
    assert blocked.returncode != 0
    assert "blocked this push" in blocked.stderr


def test_install_hooks_preserves_foreign_hooks(tmp_path):
    client = make_client(tmp_path)
    hook = client / ".git" / "hooks" / "pre-commit"
    hook.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    result = _install(client, tmp_path)
    assert result["preservedForeignHooks"] == [str(hook)]
    assert hook.read_text() == "#!/bin/sh\nexit 0\n"
    assert (client / ".git" / "hooks" / "pre-push").exists()


# --------------------------------------------------------------------------
# Node boundary: pre-receive and stored data


def write_node_policy(path, secrets):
    path.write_text(json.dumps({"schemaVersion": 1, "profile": "home",
                                "secrets": secrets}), encoding="utf-8")
    path.chmod(0o600)
    return path


@pytest.fixture
def node(tmp_path):
    root = tmp_path / "repos"
    root.mkdir(mode=0o700)
    state = tmp_path / "state"
    state.mkdir(mode=0o700)
    git_dir = root / "alice" / "app.git"
    git_dir.parent.mkdir(mode=0o700)
    git("init", "-q", "--bare", "-b", "main", str(git_dir))
    return {"tmp": tmp_path, "root": root, "state": state, "git_dir": git_dir}


def node_push(node, client, secrets):
    policy = write_node_policy(node["tmp"] / "policy.json", secrets)
    hooks = policy_mod.install_hook(node["tmp"] / "hooks", policy,
                                    node["state"], node["root"])
    return git("push", "--receive-pack=git -c core.hooksPath=%s receive-pack"
               % hooks, str(node["git_dir"]), "main:main", cwd=client,
               check=False)


def test_existing_policies_keep_their_digest_without_secrets():
    plain = policy_mod.normalize_policy({"schemaVersion": 1, "profile": "home"})
    assert "secrets" not in plain


def test_node_refuses_a_push_that_adds_a_blocked_secret(node):
    client = make_client(node["tmp"])
    assert node_push(node, client, {"mode": "block"}).returncode == 0
    tip = git("--git-dir", str(node["git_dir"]), "rev-parse", "main").stdout
    commit(client, "deploy/.env", TOKEN_LINE, "--no-verify")
    blob = blob_of(client, "deploy/.env")
    pushed = node_push(node, client, {"mode": "block"})
    assert pushed.returncode != 0
    assert "secret: probable GitHub token" in pushed.stderr
    assert "cannot recall" in pushed.stderr
    assert TOKEN not in pushed.stderr
    assert git("--git-dir", str(node["git_dir"]), "rev-parse", "main").stdout == tip
    assert git("--git-dir", str(node["git_dir"]), "cat-file", "-e", blob,
               check=False).returncode != 0
    (evidence,) = (node["state"] / "evidence").glob("evidence-*.json")
    record = json.loads(evidence.read_text())
    assert record["findings"][0]["secretRule"] == "github-token"
    assert TOKEN not in evidence.read_text()
    summary = policy_mod.status_summary(node["state"])
    assert summary["secrets"]["blockedPushes"] == 1
    assert summary["audit"]["ok"] is True


def test_node_admits_warnings_and_signed_overrides_with_audit(node, signer):
    client = make_client(node["tmp"])
    secrets = {"mode": "block", "rules": {"generic-secret-assignment": "warn"},
               "overrides": {"allowedSignersPath": str(signer["allowed"])}}
    target = client / "tests" / "fixture.env"
    target.parent.mkdir(parents=True)
    target.write_text(TOKEN_LINE, encoding="utf-8")
    (client / "config.py").write_text(PASSWORD_LINE, encoding="utf-8")
    oid = gate.blob_oid(TOKEN_LINE.encode())
    document = _override(signer, [gate.fingerprint(
        "github-token", "tests/fixture.env", oid)])
    overrides = client / ".forkmesh" / "secret-overrides"
    overrides.mkdir(parents=True)
    (overrides / "fixture.json").write_text(json.dumps(document), encoding="utf-8")
    git("add", ".", cwd=client)
    git("commit", "-q", "--no-verify", "-m", "fixtures", cwd=client)
    pushed = node_push(node, client, secrets)
    assert pushed.returncode == 0, pushed.stderr
    assert "warning" in pushed.stderr
    assert "signed override from alice@example.test" in pushed.stderr
    records = [json.loads(line) for line in
               (node["state"] / "audit.jsonl").read_text().splitlines()]
    (admitted,) = [r for r in records if r["action"] == "admit-push-secrets"]
    assert admitted["detail"]["overrides"][0]["signer"] == "alice@example.test"
    summary = policy_mod.status_summary(node["state"])["secrets"]
    assert summary["warnings"] == 1 and summary["overriddenFindings"] == 1


def test_repository_cannot_loosen_node_policy_by_committing_its_own(node):
    client = make_client(node["tmp"])
    (client / ".forkmesh").mkdir()
    (client / ".forkmesh" / "secret-scan.json").write_text(json.dumps({
        "mode": "off", "rules": {"github-token": "off"},
        "allowlist": [{"paths": ["deploy/*"], "reason": "trust me please"}]}))
    (client / "deploy").mkdir()
    (client / "deploy" / ".env").write_text(TOKEN_LINE)
    git("add", ".", cwd=client)
    git("commit", "-q", "--no-verify", "-m", "loosen", cwd=client)
    pushed = node_push(node, client, {"mode": "block"})
    assert pushed.returncode != 0
    assert "github-token" in pushed.stderr


def test_secret_found_after_replication_gets_a_response_plan(node):
    client = make_client(node["tmp"])
    commit(client, "deploy/.env", TOKEN_LINE, "--no-verify")
    # Arrived by sync: no hook ran.
    git("push", "-q", str(node["git_dir"]), "main:main", cwd=client)
    policy = policy_mod.load_policy(
        write_node_policy(node["tmp"] / "policy.json", {"mode": "block"}))
    preview = policy_mod.enforce(policy, node["root"], node["state"],
                                 apply=False, actor="op")
    (item,) = preview["violations"]
    assert item["action"] == "none" and item["rules"] == ["secret"]
    result = policy_mod.enforce(policy, node["root"], node["state"],
                                apply=True, actor="op")
    (item,) = result["violations"]
    assert item["action"] == "quarantined"
    plan = item["secretResponse"]
    for section in ("containment", "rotation", "notification", "residualRisk"):
        assert plan[section], section
    assert "cannot erase" in " ".join(plan["residualRisk"])
    assert plan["rotationByRule"][0]["rule"] == "github-token"
    assert policy_mod.is_quarantined(node["state"], "alice", "app")
    summary = policy_mod.status_summary(node["state"])
    assert summary["secrets"]["storedDetections"] == 1
    assert TOKEN not in (node["state"] / "audit.jsonl").read_text()


def test_warn_mode_reports_stored_secrets_without_quarantine(node):
    client = make_client(node["tmp"])
    commit(client, "deploy/.env", TOKEN_LINE, "--no-verify")
    git("push", "-q", str(node["git_dir"]), "main:main", cwd=client)
    policy = policy_mod.load_policy(
        write_node_policy(node["tmp"] / "policy.json", {"mode": "warn"}))
    result = policy_mod.enforce(policy, node["root"], node["state"],
                                apply=True, actor="op")
    (item,) = result["violations"]
    assert item["action"] == "none" and item["rules"] == []
    assert item["secrets"]["warned"] == 1
    assert not policy_mod.is_quarantined(node["state"], "alice", "app")


def test_node_fails_closed_without_the_scanner_module(tmp_path):
    lonely = tmp_path / "tools"
    lonely.mkdir()
    shutil.copy(ROOT / "tools" / "node_operator_policy.py", lonely)
    policy = write_node_policy(tmp_path / "policy.json", {"mode": "block"})
    completed = subprocess.run(
        [sys.executable, "-I", str(lonely / "node_operator_policy.py"),
         "validate", "--policy", str(policy)],
        capture_output=True, text=True, timeout=60, check=False)
    assert completed.returncode == 2
    assert "secret_gate.py" in completed.stderr
