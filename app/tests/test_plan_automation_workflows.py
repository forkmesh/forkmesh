#!/usr/bin/env python3
"""Static workflow contracts for scheduled automation."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ISSUE_WORKFLOW = ROOT / ".github" / "workflows" / "github-issue-sync.yml"
WORLD_BROWSER_WORKFLOW = ROOT / ".github" / "workflows" / "world-browser-e2e.yml"
CHECKOUT_PIN = "actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1"
UPLOAD_PIN = (
    "actions/upload-artifact@cf430e030ddbb5b0abf93d22962f4752f3646cd9"
)


def test_issue_sync_runs_daily_and_manually_with_minimal_permissions():
    workflow = ISSUE_WORKFLOW.read_text(encoding="utf-8")
    assert "schedule:" in workflow
    assert 'cron: "17 04 * * *"' in workflow
    assert "workflow_dispatch:" in workflow
    assert "dry_run:" in workflow
    assert "contents: read" in workflow
    assert "issues: write" in workflow
    assert "actions: write" not in workflow
    assert "contents: write" not in workflow
    assert "persist-credentials: false" in workflow
    assert CHECKOUT_PIN in workflow
    job_prefix = workflow.split("steps:", 1)[0]
    assert "SOURCE_GITHUB_TOKEN" not in job_prefix
    assert "DESTINATION_GITHUB_TOKEN" not in job_prefix


def test_issue_sync_has_mapping_report_retry_implementation_and_failure_notice():
    workflow = ISSUE_WORKFLOW.read_text(encoding="utf-8")
    implementation = (ROOT / "tools" / "github_issue_sync.py").read_text(
        encoding="utf-8"
    )
    assert "github_issue_sync.py sync" in workflow
    assert "--dry-run" in workflow
    assert "notify-failure" in workflow
    assert "if: failure()" in workflow
    assert UPLOAD_PIN in workflow
    assert "forkmesh-issue-sync:v1" in implementation
    assert "RETRYABLE_HTTP" in implementation
    assert "mapping_payload" in implementation


def test_unconfigured_scheduled_issue_sync_skips_instead_of_failing():
    workflow = ISSUE_WORKFLOW.read_text(encoding="utf-8")
    validate = workflow.split("- name: Validate one-way", 1)[1].split("- name:", 1)[0]
    unset = validate.split('if [ -z "$DESTINATION_REPOSITORY" ]; then', 1)[1]
    scheduled, manual = unset.split("exit 0", 1)
    assert '"schedule" ]' in scheduled
    assert "configured=false" in scheduled
    assert "exit 2" in manual.split("fi\n", 2)[1]
    assert "configured=true" in validate
    sync = workflow.split("- name: Synchronize only", 1)[1].split("run:", 1)[0]
    assert "if: steps.configuration.outputs.configured == 'true'" in sync


def test_world_browser_e2e_exercises_runtime_webgl_touch_and_privacy():
    workflow = WORLD_BROWSER_WORKFLOW.read_text(encoding="utf-8")
    package = (
        ROOT / "world" / "browser_tests" / "package.json"
    ).read_text(encoding="utf-8")
    tests = (
        ROOT
        / "world"
        / "browser_tests"
        / "tests"
        / "world.spec.js"
    ).read_text(encoding="utf-8")

    assert "pull_request:" in workflow
    assert "contents: read" in workflow
    assert CHECKOUT_PIN in workflow
    assert "persist-credentials: false" in workflow
    assert "npm ci" in workflow
    assert "playwright install --with-deps --only-shell chromium" in workflow
    assert '"@playwright/test": "1.61.0"' in package
    assert "routeWebSocket" in tests
    assert "two live clients synchronize movement" in tests
    assert "landscape thumbstick remains visible and moves the avatar" in tests
    assert "must-not-cross" in tests
