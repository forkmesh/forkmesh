"""Protocol suite v1: consistency, release gate, and three-implementation vector parity."""

import ast
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
SUITE = ROOT / "conformance" / "protocol" / "v1"
sys.path.insert(0, str(ROOT / "app" / "src"))
sys.path.insert(0, str(SUITE))

import collaboration  # noqa: E402
import git_http  # noqa: E402
import portable_sessions  # noqa: E402
import repository_migration  # noqa: E402
import verify_suite  # noqa: E402

SESSION_ERRORS = {
    "portable session token is invalid": "token_invalid",
    "portable session claim is invalid": "claim_invalid",
    "portable session is expired or not current": "not_current",
    "portable session claim is not canonical": "claim_noncanonical",
    "portable session signature is invalid": "signature_invalid",
    "portable session encoding is invalid": "encoding_invalid",
}


def _vectors(name):
    return json.loads((SUITE / "vectors" / f"{name}.json").read_text(encoding="utf-8"))


def _collab_outcome(call):
    try:
        return True, call()
    except collaboration.CollaborationError as exc:
        return False, exc.code


def _assert_outcome(case, valid, value, expected_value=None):
    expect = case["expect"]
    assert valid == expect["valid"], (case["id"], value)
    if not valid:
        assert value == expect["error"], case["id"]
    elif expected_value is not None:
        assert value == expected_value, case["id"]


def test_suite_is_consistent_and_candidate_blockers_are_reported():
    errors, info = verify_suite.check_consistency(ROOT)

    assert errors == []
    assert info["status"] == "candidate"
    assert info["stableReleaseBlockers"] == ["TM-004", "TM-006", "TM-008", "TM-014", "TM-018"]


def test_committed_vectors_match_reference_generation():
    completed = subprocess.run(
        [sys.executable, str(SUITE / "generate_vectors.py"), "--check"],
        capture_output=True,
        text=True,
        check=False,
        cwd=ROOT,
        timeout=300,
    )

    assert completed.returncode == 0, completed.stderr


def test_clean_room_verifier_passes_every_claimed_vector():
    report = verify_suite.run_vectors(ROOT)

    assert {name for name, entry in report.items() if not entry["claimed"]} == {"fold"}
    for name, entry in report.items():
        if entry["claimed"]:
            assert entry["cases"] > 0, name
            assert entry["failures"] == [], name

    completed = subprocess.run(
        [sys.executable, str(SUITE / "verify_suite.py")],
        capture_output=True,
        text=True,
        check=False,
        cwd=ROOT,
        timeout=300,
    )
    assert completed.returncode == 0, completed.stdout + completed.stderr
    assert json.loads(completed.stdout)["consistencyErrors"] == []


def test_clean_room_verifier_imports_only_the_standard_library():
    tree = ast.parse((SUITE / "verify_suite.py").read_text(encoding="utf-8"))
    imported = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            imported.update(alias.name.split(".")[0] for alias in node.names)
        elif isinstance(node, ast.ImportFrom):
            imported.add((node.module or "").split(".")[0])

    assert imported <= {
        "__future__", "argparse", "base64", "hashlib", "json", "pathlib", "re", "sys", "typing", "unicodedata",
    }
    assert "sys.path" not in (SUITE / "verify_suite.py").read_text(encoding="utf-8")


def test_reference_implementations_pass_committed_vectors():
    canonical = _vectors("canonical-json")
    for case in canonical["parse"]:
        raw = bytes.fromhex(case["rawHex"])
        _assert_outcome(case, *_collab_outcome(lambda: collaboration.parse_canonical_event(raw)))
    for case in canonical["encode"]:
        valid, value = _collab_outcome(lambda: collaboration.canonical_json(case["value"]).decode("utf-8"))
        _assert_outcome(case, valid, value, case["expect"].get("canonicalText"))

    identifiers = _vectors("identifiers")
    for case in identifiers["repositories"]:
        valid, value = _collab_outcome(lambda: collaboration.repository_id(case["verifiedRoot"]))
        _assert_outcome(case, valid, value, case["expect"].get("repositoryId"))
    for case in identifiers["policies"]:
        valid, value = _collab_outcome(lambda: collaboration.policy_id(case["policy"]))
        _assert_outcome(case, valid, value, case["expect"].get("policyId"))
    for case in identifiers["objects"]:
        valid, value = _collab_outcome(lambda: collaboration.object_id(
            case["repository"], case["family"], case["creator"], case["objectKey"], case["references"]
        ))
        _assert_outcome(case, valid, value, case["expect"].get("objectId"))
    for case in identifiers["refsState"]:
        text = git_http.advertised_refs_canonical(bytes.fromhex(case["advertisementHex"]))
        assert text == case["expect"]["canonicalText"], case["id"]
        assert hashlib.sha256(text.encode()).hexdigest() == case["expect"]["stateHash"], case["id"]
    for case in identifiers["migration"]:
        value = {
            "migrationId": lambda: repository_migration.migration_id(case["repositoryId"]),
            "objectKey": lambda: repository_migration.object_key(case["objectType"], case["sourceId"]),
            "stateHash": lambda: repository_migration.state_hash(case["document"]),
        }[case["operation"]]()
        assert value == case["expect"]["value"], case["id"]

    signatures = _vectors("signatures")
    for case in signatures["ed25519"]:
        valid = collaboration._ed25519_verify_uncached(
            bytes.fromhex(case["publicKeyHex"]), bytes.fromhex(case["signatureHex"]), bytes.fromhex(case["messageHex"])
        )
        assert valid == case["expect"]["valid"], case["id"]
    for case in signatures["canonicalStrings"]:
        valid = collaboration.ed25519_verify(
            collaboration._b64url_decode(case["publicKey"], 32, "key"),
            collaboration._b64url_decode(case["signature"], 64, "signature"),
            case["messageText"].encode("utf-8"),
        )
        assert valid == case["expect"]["valid"], case["id"]

    events = _vectors("events")
    policy = events["policies"]["primary"]
    for case in events["cases"]:
        raw = bytes.fromhex(case["rawHex"])

        def verify():
            event = collaboration.parse_canonical_event(raw)
            collaboration.verify_event(event, policy)
            return event["eventId"]

        valid, value = _collab_outcome(verify)
        _assert_outcome(case, valid, value, case["expect"].get("eventId"))

    folds = _vectors("fold")
    for scenario in folds["scenarios"]:
        result = collaboration.SignedEventLog.from_ndjson(scenario["ndjson"]).fold(folds["policies"]["primary"])
        assert result.state_digest == scenario["expect"]["stateDigest"], scenario["id"]
        for key, value in scenario["expect"]["telemetry"].items():
            assert result.telemetry[key] == value, (scenario["id"], key)
        assert result.telemetry["rejectionReasons"] == scenario["expect"]["rejectionReasons"]

    sessions = _vectors("sessions")
    router = collaboration._b64url_decode(sessions["routerPublicKey"], 32, "router")
    for case in sessions["cases"]:
        try:
            claim = portable_sessions.parse_token(
                case["token"],
                now_ms=case["nowMs"],
                verify=lambda message, signature: collaboration.ed25519_verify(router, signature, message),
            )
            _assert_outcome(case, True, claim, case["expect"].get("claim"))
        except portable_sessions.PortableSessionError as exc:
            _assert_outcome(case, False, SESSION_ERRORS[str(exc)])


def test_javascript_implementation_passes_claimed_vectors():
    node = shutil.which("node")
    assert node, "Node.js is required by the collaboration conformance gate"
    completed = subprocess.run(
        [node, str(ROOT / "app" / "tests" / "run_protocol_vectors_js.js")],
        capture_output=True,
        text=True,
        check=False,
        cwd=ROOT,
        timeout=120,
    )
    assert completed.returncode == 0, completed.stderr
    results = json.loads(completed.stdout)

    assert {item["set"] for item in results} == {"canonical-json", "identifiers", "events", "fold"}
    assert [item for item in results if not item["ok"]] == []
    # FM-SUITE-003 is a SHOULD: every rejection is required, but code
    # deviations must be exactly the ones the versioned manifest records.
    manifest = json.loads((SUITE / "manifest.json").read_text(encoding="utf-8"))
    recorded = next(
        item for item in manifest["implementations"] if item["id"] == "webcrypto-javascript"
    )["codeDeviations"]
    code_mismatches = {
        item["id"]: item["actualError"]
        for item in results
        if item.get("expectedError") and item["expectedError"] != item["actualError"]
    }
    assert code_mismatches == recorded


def test_envelope_size_limit_is_enforced_by_every_implementation():
    at_limit = b'{"a":"' + b"x" * (1_048_576 - 8) + b'"}'
    over_limit = b'{"a":"' + b"x" * (1_048_576 - 7) + b'"}'
    assert len(at_limit) == 1_048_576

    assert collaboration.parse_canonical_event(at_limit) == {"a": "x" * (1_048_576 - 8)}
    assert verify_suite.parse_canonical_document(at_limit)["a"] == "x" * (1_048_576 - 8)
    with pytest.raises(collaboration.CollaborationError, match="invalid_event_size"):
        collaboration.parse_canonical_event(over_limit)
    with pytest.raises(verify_suite.Reject, match="invalid_event_size"):
        verify_suite.parse_canonical_document(over_limit)

    node = shutil.which("node")
    assert node, "Node.js is required by the collaboration conformance gate"
    script = (
        "globalThis.crypto ??= require('node:crypto').webcrypto;"
        "const c = require('./app/public/collaboration.js');"
        "const probe = (n) => { try { c.parseCanonicalEvent('{\"a\":\"' + 'x'.repeat(n) + '\"}'); return 'ok'; }"
        " catch (e) { return e.code; } };"
        "process.stdout.write(JSON.stringify([probe(1048568), probe(1048569)]));"
    )
    completed = subprocess.run(
        [node, "-e", script], capture_output=True, text=True, check=False, cwd=ROOT, timeout=60
    )
    assert completed.returncode == 0, completed.stderr
    assert json.loads(completed.stdout) == ["ok", "invalid_event_size"]


def test_worker_verifiers_build_the_published_canonical_strings():
    entry = (ROOT / "app" / "src" / "entry.py").read_text(encoding="utf-8")
    catalog = (ROOT / "app" / "src" / "catalog_routes.py").read_text(encoding="utf-8")

    for fragment in (
        r'("forkmesh-view-v1\n" + owner + "\n" + repo + "\n" + str(ts)).encode()',
        r'canonical = ("forkmesh-rotate-v1\n" + old_pubkey + "\n" +',
        r'new_pubkey + "\n" + str(ts)).encode()',
        'GIT_PUSH_SIGNATURE_PREFIX = "forkmesh-git-push-v1"',
        "GIT_PUSH_SIGNATURE_PREFIX, owner, repo, method, ts, body_sha256,",
        'method not in ("GET", "POST")',
        "len(ts) > 16",
        'owner = str(owner or "").strip().lower()',
        'method = str(method or "").strip().upper()',
        'body_sha256 = str(body_sha256 or "").strip().lower()',
        "LOGIN_MAX_SKEW_MS = 5 * 60 * 1000",
        "abs(int(Date.now()) - int(ts)) <= LOGIN_MAX_SKEW_MS",
    ):
        assert fragment in entry, fragment
    for fragment in (
        r'"forkmesh-catalog-v1\n" + owner + "\n" + record["name"] +',
        r'"\n" + record["updatedAt"]',
        r'canonical = ("forkmesh-catalog-v2\n" + record_hash).encode()',
        'signed_record.pop("signature", None)',
        "signed_record, sort_keys=True,",
        '"requiredCatalogSigVersion": 2,',
        r'"forkmesh-repostate-v1\n" + owner + "\n" + record["name"] +',
        r'"\n" + state_hash + "\n" + record["updatedAt"]',
        r'"forkmesh-private-manifest-v1\n" + owner + "\n"',
    ):
        assert fragment in catalog, fragment
    assert verify_suite.FRESHNESS_WINDOW_MS == 5 * 60 * 1000


def _copy_suite(destination):
    manifest = json.loads((SUITE / "manifest.json").read_text(encoding="utf-8"))
    paths = {verify_suite.MANIFEST_PATH, manifest["threatRegister"]["path"]}
    paths |= {entry["path"] for group in ("documents", "schemas", "vectors") for entry in manifest[group]}
    for spec in manifest["requirements"].values():
        paths |= {item[len("test:"):].split("::")[0] for item in spec["evidence"] if item.startswith("test:")}
    for path in paths:
        target = destination / path
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / path, target)
    return destination


def _rebind(root, path):
    manifest_path = root / verify_suite.MANIFEST_PATH
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    digest = hashlib.sha256((root / path).read_bytes()).hexdigest()
    entries = [manifest["threatRegister"]] + [
        entry for group in ("documents", "schemas", "vectors") for entry in manifest[group]
    ]
    for entry in entries:
        if entry["path"] == path:
            entry["sha256"] = digest
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")


def _edit_json(root, path, mutate, rebind=True):
    target = root / path
    value = json.loads(target.read_text(encoding="utf-8"))
    mutate(value)
    target.write_text(json.dumps(value, indent=2), encoding="utf-8")
    if rebind:
        _rebind(root, path)


def _append(root, path, text, rebind=True):
    with (root / path).open("a", encoding="utf-8") as handle:
        handle.write(text)
    if rebind:
        _rebind(root, path)


ENCODING_DOC = "www/docs/protocol/encoding-and-identifiers.md"
EVENTS = "conformance/protocol/v1/vectors/events.json"
THREATS = "conformance/protocol/v1/threats.json"


def _set_threat_status(register):
    next(item for item in register["threats"] if item["id"] == "TM-004")["status"] = "mitigated"


@pytest.mark.parametrize(
    ("mutation", "expected"),
    [
        (lambda root: _append(root, ENCODING_DOC, "\nEdited.\n", rebind=False), "document digest mismatch"),
        (
            lambda root: _edit_json(root, EVENTS, lambda value: value.update(suiteVersion="1.0.0-rc.2")),
            "vector file version or name mismatch",
        ),
        (
            lambda root: _append(root, ENCODING_DOC, "\n- **FM-ENC-099** MUST — Unbound.\n"),
            "FM-ENC-099 appears only in the documents",
        ),
        (
            lambda root: _edit_json(
                root, EVENTS, lambda value: value["cases"][0].update(requirements=["FM-ENC-098"])
            ),
            "cites unknown requirement FM-ENC-098",
        ),
        (
            lambda root: _edit_json(root, THREATS, _set_threat_status),
            "TM-004 severity/status differs",
        ),
        (
            lambda root: _edit_json(
                root, verify_suite.MANIFEST_PATH, lambda value: value.update(status="stable"), rebind=False
            ),
            "stable release blocked by open high-severity threats",
        ),
        (
            lambda root: _edit_json(
                root,
                verify_suite.MANIFEST_PATH,
                lambda value: value["requirements"]["FM-VER-002"].update(level="MUST"),
                rebind=False,
            ),
            "FM-VER-002 level differs",
        ),
    ],
    ids=[
        "document-edit", "vector-version", "undeclared-requirement", "unknown-citation",
        "threat-drift", "premature-stable", "level-drift",
    ],
)
def test_inconsistent_suite_is_rejected(tmp_path, mutation, expected):
    root = _copy_suite(tmp_path)
    assert verify_suite.check_consistency(root)[0] == []

    mutation(root)

    errors, _info = verify_suite.check_consistency(root)
    assert any(expected in error for error in errors), errors
