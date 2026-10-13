#!/usr/bin/env python3
"""Fixture evidence for issue #490: transparent, signed migration reports.

Each test drives the real ``RepositoryImportService`` over SQLite through the
provider fixtures from ``test_repository_migration`` and checks the exported
report with ``tools/verify_migration_report.py``, which imports no ForkMesh
code.
"""

import json
import subprocess
import sys
from copy import deepcopy
from pathlib import Path

TESTS = Path(__file__).resolve().parent
if str(TESTS) not in sys.path:
    sys.path.insert(0, str(TESTS))

from test_repository_migration import (  # noqa: E402
    REPORT_PUBLIC_KEY, ROOT, Harness, migration, report_verifier,
)
import repository_migration_report as reports  # noqa: E402


PROVIDERS = ("github", "gitlab", "codeberg")
VERIFIER = ROOT.parent / "tools" / "verify_migration_report.py"


def _base(job_id):
    return "/api/repository-imports/migrations/" + job_id


def _resume(harness, job_id, **extra):
    return harness.call("POST", _base(job_id) + "/resume", dict(
        {"providerToken": "fixture-token", "sessionToken": "alice"}, **extra))


def _report(harness, job_id, query=""):
    response = harness.call("GET", _base(job_id) + "/report" + query)
    assert response["status"] == 200, response
    return response["data"]


def _objects(harness, job_id):
    records, cursor = [], ""
    while True:
        query = "?limit=200" + ("&after=" + cursor if cursor else "")
        page = harness.call("GET", _base(job_id) + "/objects" + query)["data"]
        records.extend(page["objects"])
        if not page["hasMore"]:
            return records
        cursor = page["nextCursor"]


def _intercept(harness, handler):
    """Let a test rewrite individual provider responses."""
    original = harness.fixture.fetch

    async def fetch(env, provider, path, token):
        replaced = handler(path)
        if replaced is not None:
            harness.fixture.calls.append((path, token))
            return replaced
        return await original(env, provider, path, token)

    harness.service.d["provider_fetch"] = fetch


def _complete(harness):
    started = harness.start()
    job_id = started["data"]["migration"]["id"]
    final = harness.finish(job_id)[-1]
    assert final["data"]["migration"]["status"] == "completed", final
    return job_id, final["data"]["migration"]


def _verify(bundle, records=None, public_key=REPORT_PUBLIC_KEY):
    return report_verifier.verify_bundle(bundle, public_key, "", records)


def _failed_checks(result):
    return [check["check"] for check in result.checks if check["result"] == "fail"]


def test_fixture_totals_reconcile_with_no_unclassified_remainder():
    for provider in PROVIDERS:
        harness = Harness(provider)
        if provider == "github":
            # GitHub lists pull requests on the issues endpoint as well; they
            # must be counted as skipped here, not lost or double-imported.
            _intercept(harness, lambda path: {
                "status": 200, "headers": {},
                "data": [
                    harness.fixture._item(201, number=11, labels=[{"id": 401, "name": "bug"}]),
                    harness.fixture._item(301, number=22, pull_request={"url": "/pulls/22"}),
                ],
            } if "/issues?" in path else None)
        job_id, job = _complete(harness)
        bundle = _report(harness, job_id)
        assert bundle["draft"] is False
        report = bundle["report"]
        rows = report["reconciliation"]["rows"]
        assert set(rows) == set(migration.OBJECT_TYPES) | {reports.ARTIFACT_TYPE}
        for kind, row in rows.items():
            assert row["unclassified"] == 0, (provider, kind, row)
            assert row["source"] == sum(row[key] for key in reports.CLASSES)
        for kind in migration.OBJECT_TYPES:
            assert rows[kind]["source"] > 0, (provider, kind)
            assert rows[kind]["imported"] == job["counts"][kind]
        # The release asset in every fixture is declared but its bytes are
        # not copied: an external record with an explicit remediation.
        assert rows[reports.ARTIFACT_TYPE] == dict(
            rows[reports.ARTIFACT_TYPE], source=1, external=1, imported=0)
        if provider == "github":
            assert rows["issues"]["skipped"] == 1
            assert rows["issues"]["skippedReasons"]["representedByAnotherType"] == 1
        totals = report["reconciliation"]["totals"]
        assert totals["unclassified"] == 0 and report["reconciliation"]["reconciled"]
        assert report["outcome"] == "verified"
        assert report["refs"]["count"] == 2
        assert {ref["kind"] for ref in report["refs"]["items"]} == {"branch", "tag"}
        assert report["checksums"]["objectManifest"]["count"] == job["objectCount"]
        assert report["checksums"]["state"]["hash"] == job["state"]["hash"]
        assert any(dep["kind"] == "release_asset_bytes"
                   for dep in report["externalDependencies"])
        assert any("wiki" in dep["detail"] for dep in report["externalDependencies"])
        assert report["summary"][0].startswith("ForkMesh imported")
        assert "Every source record is accounted for." in report["summary"]
        assert bundle["signature"]["algorithm"] == "ed25519"
        # Each verification result links to reproducible evidence.
        for check in report["verification"]:
            assert check["evidence"]["kind"] in ("report_section", "export", "receipt")
        manifest_check = next(
            check for check in report["verification"] if check["id"] == "object-manifest")
        assert manifest_check["evidence"]["path"] == _base(job_id) + "/objects"
        result = _verify(bundle, _objects(harness, job_id))
        assert result.ok, result.checks
        assert job["report"]["latest"]["outcome"] == "verified"


def test_independent_verifier_detects_report_and_content_tampering(tmp_path):
    harness = Harness("gitlab")
    job_id, _ = _complete(harness)
    bundle = _report(harness, job_id)
    records = _objects(harness, job_id)
    assert _verify(bundle, records).ok

    edited = deepcopy(bundle)
    edited["report"]["reconciliation"]["rows"]["issues"]["imported"] += 1
    assert "report:hash" in _failed_checks(_verify(edited, records))

    # Re-hashing an edited report does not help without the signing key.
    rehashed = deepcopy(edited)
    rehashed["reportHash"] = report_verifier.sha256_prefixed(
        report_verifier.canonical_json(rehashed["report"]))
    failures = _failed_checks(_verify(rehashed, records))
    assert "report:signature" in failures and "reconciliation" in failures

    content = deepcopy(records)
    issue = next(record for record in content if record["objectType"] == "issues")
    issue["common"]["body"] = "Rewritten after the report was signed."
    assert "objects:manifest" in _failed_checks(_verify(bundle, content))
    assert "objects:manifest" in _failed_checks(_verify(bundle, records[:-1]))

    wrong_key = report_verifier.verify_bundle(bundle, "A" * 43, "", records)
    assert "report:signature" in _failed_checks(wrong_key)

    report_file = tmp_path / "report.json"
    objects_file = tmp_path / "objects.json"
    report_file.write_text(json.dumps(bundle))
    objects_file.write_text(json.dumps(records))
    command = [sys.executable, str(VERIFIER), str(report_file),
               "--objects", str(objects_file), "--public-key", REPORT_PUBLIC_KEY]
    passed = subprocess.run(command, capture_output=True, text=True, check=False)
    assert passed.returncode == 0, passed.stdout + passed.stderr
    assert json.loads(passed.stdout)["ok"] is True
    objects_file.write_text(json.dumps(content))
    failed = subprocess.run(command, capture_output=True, text=True, check=False)
    assert failed.returncode == 1
    assert json.loads(failed.stdout)["ok"] is False
    # The verifier is independent: it never imports ForkMesh source.
    source = VERIFIER.read_text()
    assert "app/src" not in source and "sys.path" not in source


def test_retry_only_failed_records_supersedes_report_and_keeps_history():
    harness = Harness("github")
    broken = {"on": True}

    def issues(path):
        if "/issues?" not in path:
            return None
        return {"status": 200, "headers": {}, "data": [
            harness.fixture._item(201, number=11),
            # A non-finite number cannot be stored without loss; the provider
            # later serves the same record with a representable value.
            harness.fixture._item(
                202, number=12, score=float("nan") if broken["on"] else 1.5),
        ]}

    _intercept(harness, issues)
    job_id, job = _complete(harness)
    first = _report(harness, job_id)
    report = first["report"]
    assert report["outcome"] == "completed_with_failures"
    issues_row = report["reconciliation"]["rows"]["issues"]
    assert issues_row == dict(issues_row, source=2, imported=1, failed=1, unclassified=0)
    assert report["failures"]["records"] == 1
    assert report["failures"]["work"][0]["stream"] == "issues"
    event = next(item for item in report["failures"]["events"] if item.get("sourceId"))
    assert event == dict(event, sourceId="issue:202",
                         reason="provider_object_non_finite_number")
    assert report["remediation"][0]["action"]["path"] == _base(job_id) + "/retry"
    assert job["report"]["latest"]["retryable"] is True
    assert _verify(first, _objects(harness, job_id)).ok

    broken["on"] = False
    calls_before = len(harness.fixture.calls)
    retried = harness.call("POST", _base(job_id) + "/retry", {"sessionToken": "alice"})
    assert retried["status"] == 202 and retried["data"]["queued"] == 1
    assert retried["data"]["migration"]["retrying"] is True
    finished = harness.finish(job_id)[-1]["data"]["migration"]
    assert finished["status"] == "completed" and finished["retrying"] is False
    retry_calls = [path for path, _ in harness.fixture.calls[calls_before:]]
    # Only the failed page was requested again, not the whole repository.
    assert len(retry_calls) == 1 and "/issues?" in retry_calls[0]
    # The retry is recorded on its own; the original transfer span is kept.
    assert finished["transfer"]["durationMs"] == job["transfer"]["durationMs"]
    assert finished["transfer"]["startedAt"] == job["transfer"]["startedAt"]

    second = _report(harness, job_id)
    new = second["report"]
    assert new["sequence"] == 2 and new["outcome"] == "verified"
    assert new["supersedes"] == {"sequence": 1, "reportHash": first["reportHash"]}
    assert new["reconciliation"]["rows"]["issues"]["imported"] == 2
    assert new["reconciliation"]["unclassified"] == 0
    assert new["retries"]["failedWorkRetries"][0]["recovered"] == 1
    assert [entry["sequence"] for entry in second["history"]] == [1, 2]
    assert second["history"][0]["supersededBy"] == 2
    assert second["history"][0]["report"] == report
    assert _verify(second, _objects(harness, job_id)).ok
    historical = _report(harness, job_id, "?sequence=1")
    assert historical["latest"] is False and historical["reportHash"] == first["reportHash"]

    broken_chain = deepcopy(second)
    broken_chain["history"][0]["report"]["outcome"] = "verified"
    assert "history" in _failed_checks(_verify(broken_chain))

    metrics = harness.call("GET", "/api/repository-imports/migration-metrics")["data"]
    assert metrics["all"]["retryRecovery"]["recoveredWork"] == 1
    assert metrics["all"]["reports"]["unclassifiedRecords"] == 0
    actions = [entry[0] for entry in harness.audit]
    assert actions.count("repository_migration.report") == 2
    assert "repository_migration.retry_failed" in actions
    assert "repository_migration.report_export" in actions
    again = harness.call("POST", _base(job_id) + "/retry", {"sessionToken": "alice"})
    assert again["status"] == 409 and again["data"]["error"] == "no_failed_work"


def test_unreadable_page_is_deferred_reported_unmeasured_and_recovered():
    harness = Harness("codeberg")
    outage = {"on": True}
    _intercept(harness, lambda path: {"status": 500, "data": {}, "headers": {}}
               if "/labels" in path and outage["on"] else None)
    started = harness.start()
    job_id = started["data"]["migration"]["id"]
    paused = harness.finish(job_id)[-1]
    assert paused["status"] == 502
    assert paused["data"]["migration"]["status"] == "paused"
    deferred = _resume(harness, job_id, deferFailure=True)
    assert deferred["status"] == 200 and deferred["data"]["deferred"] is True
    assert harness.finish(job_id)[-1]["data"]["migration"]["status"] == "completed"
    report = _report(harness, job_id)["report"]
    assert report["outcome"] == "completed_with_unmeasured_sources"
    assert report["reconciliation"]["sourceTotalsExact"] is False
    assert report["reconciliation"]["unmeasuredSources"][0]["stream"] == "labels"
    assert report["reconciliation"]["rows"]["labels"]["source"] == 0
    assert report["reconciliation"]["unclassified"] == 0

    outage["on"] = False
    assert harness.call(
        "POST", _base(job_id) + "/retry", {"sessionToken": "alice"})["status"] == 202
    harness.finish(job_id)
    recovered = _report(harness, job_id)
    assert recovered["report"]["outcome"] == "verified"
    assert recovered["report"]["reconciliation"]["rows"]["labels"]["imported"] == 1
    assert recovered["report"]["reconciliation"]["sourceTotalsExact"] is True
    assert _verify(recovered, _objects(harness, job_id)).ok


def test_crash_redo_of_a_page_is_not_counted_twice():
    harness = Harness("github")
    job_id = harness.start()["data"]["migration"]["id"]
    for _ in range(3):
        _resume(harness, job_id)
    snapshot = harness.db.execute(
        "SELECT data,phase FROM repository_migration_jobs WHERE id=?", (job_id,)).fetchone()
    _resume(harness, job_id)
    # Model a Worker that stored the page's objects and then died before its
    # checkpoint write: the next resume fetches the same page again.
    harness.db.execute(
        "UPDATE repository_migration_jobs SET data=?, phase=? WHERE id=?",
        (snapshot["data"], snapshot["phase"], job_id))
    harness.db.commit()
    harness.finish(job_id)
    report = _report(harness, job_id)["report"]
    assert report["reconciliation"]["unclassified"] == 0
    assert report["outcome"] == "verified"


def test_in_progress_draft_abandon_report_rollback_and_no_secret():
    harness = Harness("github")
    secret = "ghp_REPORT_MUST_NOT_CONTAIN_THIS"
    job_id = harness.start(token=secret)["data"]["migration"]["id"]
    _resume(harness, job_id, providerToken=secret)
    refused = harness.call("POST", _base(job_id) + "/retry", {"sessionToken": "alice"})
    assert refused["status"] == 409 and refused["data"]["error"] == "migration_not_completed"
    draft = _report(harness, job_id)
    assert draft["draft"] is True and draft["signature"] is None
    assert draft["report"]["outcome"] == "in_progress"
    assert harness.call("GET", _base(job_id) + "/report", actor="bob")["status"] == 404

    abandoned = harness.call("POST", _base(job_id) + "/abandon", {"sessionToken": "alice"})
    assert abandoned["data"]["migration"]["report"]["latest"]["outcome"] == "abandoned"
    signed = _report(harness, job_id)
    assert signed["draft"] is False and signed["report"]["outcome"] == "abandoned"
    assert "The import was stopped before it finished." in signed["report"]["summary"]
    assert _verify(signed, _objects(harness, job_id)).ok

    _resume(harness, job_id, providerToken=secret, recover=True)
    harness.finish(job_id, token=secret)
    completed = _report(harness, job_id)
    assert completed["report"]["sequence"] == 2
    assert completed["report"]["supersedes"]["reportHash"] == signed["reportHash"]
    assert secret not in json.dumps(completed)

    rolled_back = harness.call("POST", _base(job_id) + "/rollback", {"sessionToken": "alice"})
    assert rolled_back["data"]["rolledBack"] is True
    audit = [entry for entry in harness.audit if entry[0] == "repository_migration.rollback"]
    assert audit[-1][3]["finalReportHash"] == completed["reportHash"]


def test_new_full_scan_keeps_the_signed_report_chain():
    harness = Harness("gitlab")
    job_id, _ = _complete(harness)
    first = _report(harness, job_id)
    harness.start()
    harness.finish(job_id)
    second = _report(harness, job_id)
    assert second["report"]["generation"] == 2
    assert second["report"]["supersedes"]["reportHash"] == first["reportHash"]
    assert second["report"]["reconciliation"]["unclassified"] == 0
    assert _verify(second, _objects(harness, job_id)).ok


def test_report_surfaces_are_documented_and_wired():
    script = (ROOT / "public" / "dashboard" / "js" / "05-repo-list-explorer.js").read_text()
    wiring = (ROOT / "public" / "dashboard" / "js" / "08-repo-detail-network.js").read_text()
    built = (ROOT / "public" / "dashboard.js").read_text()
    for source in (script, built):
        assert "data-repository-migration-report" in source
        assert "data-repository-migration-retry" in source
    assert "downloadRepositoryMigrationReport" in wiring
    assert "retryRepositoryMigration" in wiring
    docs = (ROOT.parent / "www" / "docs" / "repository-imports.md").read_text()
    for phrase in ("/report", "/retry", "deferFailure",
                   "tools/verify_migration_report.py", "unclassified"):
        assert phrase in docs
    workflow = (ROOT.parent / ".github" / "workflows"
                / "repository-migration-conformance.yml").read_text()
    assert "app/tests/test_repository_migration_report.py" in workflow
