#!/usr/bin/env python3
"""Public redaction and security artifact schema contracts."""

import json
import hashlib
from io import BytesIO
from pathlib import Path
import sys
from urllib.error import HTTPError

import pytest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import security_scan as scan_module  # noqa: E402


def test_public_report_redacts_secret_values_and_source_excerpts(tmp_path):
    raw_token = "ghp_" + "A" * 40
    (tmp_path / "unsafe.py").write_text(
        f'TOKEN = "{raw_token}"\nvalue = eval("1 + 1")\n',
        encoding="utf-8",
    )
    report = scan_module.build_report(
        tmp_path,
        repository_name="test/repository",
        offline=True,
    )
    serialized = json.dumps(report)

    assert raw_token not in serialized
    assert 'eval("1 + 1")' not in serialized
    assert report["visibility"]["redacted"] is True
    assert report["visibility"]["containsSourceExcerpts"] is False
    assert report["visibility"]["containsSecretValues"] is False
    assert report["scan"]["completion"] == "scope_limited"
    assert report["status"] in {"Review required", "Critical findings detected"}
    assert any(item["category"] == "secret" for item in report["findings"])
    assert any(item["ruleId"] == "python-dynamic-execution" for item in report["findings"])
    assert all(
        set(item["evidence"]) == {"redacted", "fingerprint"}
        for item in report["findings"]
    )
    assert report["notices"] == scan_module.NOTICES


def test_public_finding_id_never_hashes_secret_evidence(tmp_path):
    first_secret = "ghp_" + "A" * 40
    second_secret = "ghp_" + "B" * 40
    source = tmp_path / "src"
    source.mkdir()
    settings = source / "settings.py"
    settings.write_text(f'TOKEN = "{first_secret}"\n', encoding="utf-8")
    first_report = scan_module.build_report(
        tmp_path, repository_name="test/repository", offline=True)
    settings.write_text(f'TOKEN = "{second_secret}"\n', encoding="utf-8")
    second_report = scan_module.build_report(
        tmp_path, repository_name="test/repository", offline=True)
    first = next(
        item for item in first_report["findings"]
        if item["category"] == "secret")
    second = next(
        item for item in second_report["findings"]
        if item["category"] == "secret")

    # Public correlation is location/rule based. Changing the secret in place
    # neither changes the identifier nor exposes a value-derived verifier.
    assert first["id"] == second["id"]
    assert first["evidence"]["fingerprint"] == second["evidence"]["fingerprint"]
    serialized = json.dumps([first, second], sort_keys=True)
    for secret in (first_secret, second_secret):
        assert secret not in serialized
        assert hashlib.sha256(secret.encode()).hexdigest() not in serialized
        legacy_material = (
            f"forkmesh-public-v1\0{first['ruleId']}\0"
            f"{first['path']}\0{first['line']}\0{secret}"
        )
        assert hashlib.sha256(
            legacy_material.encode()).hexdigest()[:16] not in serialized


def test_dependency_inventory_only_includes_public_registry_lock_entries(tmp_path):
    package_dir = tmp_path / "extension"
    package_dir.mkdir()
    (package_dir / "package-lock.json").write_text(
        json.dumps(
            {
                "packages": {
                    "": {"name": "private-root", "version": "1.0.0"},
                    "node_modules/public-package": {
                        "version": "2.3.4",
                        "resolved": "https://registry.npmjs.org/public-package/-/public-package-2.3.4.tgz",
                    },
                    "node_modules/private-package": {
                        "version": "9.9.9",
                        "resolved": "https://packages.example.invalid/private.tgz",
                    },
                }
            }
        ),
        encoding="utf-8",
    )
    inventory = scan_module.dependency_inventory(tmp_path)
    assert [(item.ecosystem, item.name, item.version) for item in inventory] == [
        ("npm", "public-package", "2.3.4")
    ]


def test_scanner_never_follows_repository_symlinks_outside_scan_root(tmp_path):
    repository = tmp_path / "repository"
    repository.mkdir()
    outside = tmp_path / "outside"
    outside.mkdir()
    raw_token = "ghp_" + "Z" * 40
    secret_source = outside / "secret.py"
    secret_source.write_text(f'TOKEN = "{raw_token}"\n', encoding="utf-8")
    public_lock = outside / "package-lock.json"
    public_lock.write_text(
        json.dumps(
            {
                "packages": {
                    "node_modules/escaped-package": {
                        "version": "1.2.3",
                        "resolved": (
                            "https://registry.npmjs.org/escaped-package/"
                            "-/escaped-package-1.2.3.tgz"
                        ),
                    }
                }
            }
        ),
        encoding="utf-8",
    )
    try:
        (repository / "escaped.py").symlink_to(secret_source)
        (repository / "package-lock.json").symlink_to(public_lock)
    except OSError as exc:
        pytest.skip(f"symbolic links are unavailable: {exc}")

    report = scan_module.build_report(
        repository,
        repository_name="test/repository",
        offline=True,
    )

    assert raw_token not in json.dumps(report)
    assert report["findings"] == []
    assert report["scan"]["filesInspected"] == 0
    assert report["scan"]["filesExcluded"] == 2
    assert scan_module.dependency_inventory(repository) == []


class FakeOSVResponse:
    headers = {}

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        return False

    def read(self):
        return json.dumps(
            {
                "results": [
                    {
                        "vulns": [
                            {
                                "id": "OSV-TEST-1",
                                "aliases": ["CVE-2099-0001"],
                                "database_specific": {"severity": "HIGH"},
                                "summary": "Exploit details that must not be copied",
                            }
                        ]
                    }
                ]
            }
        ).encode()


class FakeIngestResponse:
    status = 204

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        return False

    def read(self):
        return b""


def test_osv_findings_include_advisory_ids_without_exploit_text():
    dependency = scan_module.Dependency("npm", "example", "1.0.0", "package-lock.json")
    findings = scan_module.query_osv(
        [dependency], opener=lambda request, timeout: FakeOSVResponse()
    )
    assert len(findings) == 1
    assert findings[0]["advisoryIds"] == ["CVE-2099-0001", "OSV-TEST-1"]
    assert "Exploit details" not in json.dumps(findings)
    assert findings[0]["evidence"]["redacted"] is True


def test_published_schema_requires_redaction_and_scan_limitations():
    schema = json.loads(
        (ROOT / "docs" / "security-scan.schema.json").read_text(encoding="utf-8")
    )
    required = set(schema["required"])
    assert {"visibility", "scanner", "policy", "scan", "findings", "notices"} <= required
    visibility = schema["properties"]["visibility"]["properties"]
    assert visibility["containsSourceExcerpts"]["const"] is False
    assert visibility["containsSecretValues"]["const"] is False
    finding_evidence = (
        schema["properties"]["findings"]["items"]["properties"]["evidence"]
    )
    assert finding_evidence["additionalProperties"] is False
    assert finding_evidence["properties"]["redacted"]["const"] is True


def test_fatal_scanner_error_still_has_a_public_redacted_status_artifact(tmp_path):
    report = scan_module.failed_report(
        tmp_path,
        repository_name="test/repository",
        policy_path=tmp_path / "missing-policy.json",
    )
    assert report["status"] == "Scan failed"
    assert report["scan"]["completion"] == "failed"
    assert report["visibility"]["redacted"] is True
    assert report["findings"] == []
    assert "private diagnostic details" in report["scan"]["scopeLimitations"][0]


def test_rich_report_converts_to_compact_world_clipboard(tmp_path):
    (tmp_path / "safe.py").write_text("value = 1\n", encoding="utf-8")
    rich = scan_module.build_report(
        tmp_path, repository_name="test/repository", offline=True
    )
    compact = scan_module.clipboard_report(rich)
    schema = json.loads(
        (ROOT / "docs" / "security-clipboard.schema.json").read_text(
            encoding="utf-8"
        )
    )

    assert set(schema["required"]) <= set(compact)
    assert compact["status"] == rich["status"]
    assert compact["commitHash"] == rich["repository"]["commit"]
    assert compact["findings"]["critical"] == rich["summary"]["bySeverity"]["critical"]
    assert compact["categories"] == {
        "dependency": rich["summary"]["byCategory"]["dependency"],
        "secretDetection": rich["summary"]["byCategory"]["secret"],
        "staticAnalysis": rich["summary"]["byCategory"]["static"],
    }
    assert compact["publicRedaction"] == {
        "sourceCode": True,
        "secrets": True,
        "sensitivePrompts": True,
        "exploitDetails": True,
    }
    assert compact["limitations"] == scan_module.NOTICES


def test_authenticated_ingest_posts_both_redacted_schemas_without_token_in_body(
    tmp_path,
):
    rich = scan_module.build_report(
        tmp_path, repository_name="test/repository", offline=True
    )
    compact = scan_module.clipboard_report(rich)
    captured = {}

    def opener(request, timeout):
        captured["request"] = request
        captured["timeout"] = timeout
        return FakeIngestResponse()

    token = "ingest-token-that-must-not-enter-the-report"
    scan_module.publish_scan_ingest(
        rich,
        compact,
        url=(
            "https://forkmesh.example/api/repo/test/repository/"
            "security-scans/ingest"
        ),
        token=token,
        opener=opener,
    )

    request = captured["request"]
    assert request.get_header("Authorization") == f"Bearer {token}"
    payload = json.loads(request.data)
    assert payload["type"] == "forkmesh.security-scan-ingest"
    assert payload["rich"] == rich
    assert payload["clipboard"] == compact
    assert token not in request.data.decode()


def test_ingest_retries_transient_http_without_reading_or_logging_response_body(
    tmp_path,
):
    rich = scan_module.build_report(
        tmp_path, repository_name="test/repository", offline=True
    )
    compact = scan_module.clipboard_report(rich)
    calls = []
    sleeps = []

    def opener(request, timeout):
        calls.append(request)
        if len(calls) == 1:
            raise HTTPError(
                request.full_url,
                503,
                "temporary",
                {"Retry-After": "0"},
                BytesIO(b'{"privateDiagnostic":"must not be read"}'),
            )
        return FakeIngestResponse()

    scan_module.publish_scan_ingest(
        rich,
        compact,
        url=(
            "https://forkmesh.example/api/repo/test/repository/"
            "security-scans/ingest"
        ),
        token="private-token-" + "x" * 32,
        opener=opener,
        sleeper=sleeps.append,
    )
    assert len(calls) == 2
    assert sleeps == [0.0]


def test_ingest_requires_https(tmp_path):
    rich = scan_module.build_report(
        tmp_path, repository_name="test/repository", offline=True
    )
    compact = scan_module.clipboard_report(rich)
    with pytest.raises(scan_module.ScanError, match="must be HTTPS"):
        scan_module.publish_scan_ingest(
            rich,
            compact,
            url=(
                "http://forkmesh.example/api/repo/test/repository/"
                "security-scans/ingest"
            ),
            token="private-token-" + "x" * 32,
            opener=lambda *args, **kwargs: (_ for _ in ()).throw(
                AssertionError("network must not be called")
            ),
        )
def test_ingest_rejects_route_mismatch_short_token_and_clipboard_tampering(
    tmp_path,
):
    rich = scan_module.build_report(
        tmp_path, repository_name="test/repository", offline=True
    )
    compact = scan_module.clipboard_report(rich)
    opener = lambda *args, **kwargs: (_ for _ in ()).throw(
        AssertionError("network must not be called")
    )
    with pytest.raises(scan_module.ScanError, match="does not match"):
        scan_module.publish_scan_ingest(
            rich,
            compact,
            url=(
                "https://example.test/api/repo/test/other/"
                "security-scans/ingest"
            ),
            token="x" * 48,
            opener=opener,
        )
    with pytest.raises(scan_module.ScanError, match="at least 32"):
        scan_module.publish_scan_ingest(
            rich,
            compact,
            url=(
                "https://example.test/api/repo/test/repository/"
                "security-scans/ingest"
            ),
            token="too-short",
            opener=opener,
        )
    tampered = json.loads(json.dumps(compact))
    tampered["findings"]["critical"] += 1
    with pytest.raises(scan_module.ScanError, match="does not match"):
        scan_module.publish_scan_ingest(
            rich,
            tampered,
            url=(
                "https://example.test/api/repo/test/repository/"
                "security-scans/ingest"
            ),
            token="x" * 48,
            opener=opener,
        )


def test_scanner_bounds_findings_and_marks_truncation(monkeypatch, tmp_path):
    findings = [
        scan_module.make_finding(
            category="static",
            severity="low",
            rule_id="bounded-test",
            path="src/file-%d.py" % index,
            line=index + 1,
            summary="Review this finding.",
            recommendation="Review the affected code.",
        )
        for index in range(scan_module.MAX_PUBLIC_FINDINGS + 2)
    ]
    monkeypatch.setattr(
        scan_module, "select_scan_files", lambda *_args: ([], 0))
    monkeypatch.setattr(
        scan_module, "dependency_inventory", lambda *_args: [])
    monkeypatch.setattr(
        scan_module, "scan_secrets", lambda *_args: findings)
    monkeypatch.setattr(
        scan_module, "scan_static", lambda *_args: [])
    report = scan_module.build_report(
        tmp_path,
        repository_name="test/repository",
        policy_path=ROOT / "docs" / "security-scan-policy.json",
        offline=True,
    )
    assert len(report["findings"]) == scan_module.MAX_PUBLIC_FINDINGS
    assert report["summary"]["total"] == scan_module.MAX_PUBLIC_FINDINGS
    assert any(
        "omitted 2 additional findings" in limitation
        for limitation in report["scan"]["scopeLimitations"]
    )
