#!/usr/bin/env python3
"""Contracts for the evidence-only Hacker News launch readiness packet."""

import hashlib
import json
import re
from collections import Counter
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
REPORT = ROOT / "www" / "docs" / "launch" / "y-combinator-news-readiness.md"
LATEST = ROOT / ".forkmesh" / "releases" / "latest"
CANDIDATE = ROOT / ".forkmesh" / "releases" / "candidate-0.7.95"
CLEAN_MACHINE = ROOT / "www" / "docs" / "launch" / "clean-machine-evidence.md"
TRANSCRIPT_TOOL = ROOT / "tools" / "clean_machine_transcript.sh"


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def test_readiness_packet_has_every_required_evidence_section():
    report = _read(REPORT)
    headings = (
        "## 1. Precise technical position",
        "## 2. Five-minute no-signup show path",
        "## 3. Release, install, verification, and cleanup",
        "## 4. Reproducible claim ledger",
        "## 5. Security and trust FAQ",
        "## 6. Agent-system FAQ",
        "## 7. Capability comparison without slogans",
        "## 8. Demo and launch operations",
        "## 9. HN compliance gate",
        "## 10. Verdict and ordered blockers",
    )
    positions = [report.index(heading) for heading in headings]
    assert positions == sorted(positions)

    for evidence_class in (
        "Measured fact",
        "Repository evidence",
        "Deployed observation",
        "Inference",
        "Missing proof",
    ):
        assert evidence_class in report

    for boundary in (
        "worktree",
        "operating-system sandbox",
        "prompt injection",
        "secrets",
        "telemetry",
        "data deleted",
        "runaway loop",
        "hard dollar ceiling",
        "concurrency",
        "relaunch",
        "branch",
        "Stop work",
    ):
        assert boundary.lower() in report.lower()


def test_report_release_facts_follow_signed_manifest_metadata():
    report = _read(REPORT)
    latest = json.loads(_read(LATEST / "release.json"))
    candidate = json.loads(_read(CANDIDATE / "release.json"))

    for release in (latest, candidate):
        assert release["tag"] in report
        assert release["tag_commit"] in report
        assert release["build_commit"] in report
        sums = (LATEST if release is latest else CANDIDATE) / "SHASUMS256.txt"
        assert hashlib.sha256(sums.read_bytes()).hexdigest() == release["checksums_sha256"]
    assert latest["checksums_sha256"] in report

    # A release that adds or drops a platform must refresh the report.
    assert {asset["os"] for asset in latest["assets"]} == {"linux", "android", "windows", "macos"}
    assert "no Linux arm64 or Windows arm64 asset" in " ".join(report.split())
    mac_digests = {asset["blob_sha256"] for asset in latest["assets"] if asset["os"] == "macos"}
    if len(mac_digests) == 1:
        assert "the DMG is universal" in " ".join(report.split())
    # Every downloaded blob hash the report cites must be the manifest's own.
    for asset in latest["assets"]:
        if asset["name"] in {
            "forkmesh-linux-x86_64",
            "forkmesh-mirror-node-linux-x86_64",
            "forkmesh-windows-x86_64-setup.exe",
        }:
            assert asset["blob_sha256"]
    assert "matched their manifest `blob_sha256` exactly" in " ".join(report.split())
    assert "Build-workflow intent does not" in report
    assert "Stale candidate" in report
    assert "`96c5e6391`" in report


def test_hn_constraints_and_research_baseline_remain_linked():
    report = _read(REPORT)
    normalized = " ".join(report.split())
    for url in (
        "https://news.ycombinator.com/showhn.html",
        "https://news.ycombinator.com/newsguidelines.html",
        "https://news.ycombinator.com/showlim",
    ):
        assert url in report
    for item_id in (
        "46990733",
        "47520220",
        "47877726",
        "47303711",
        "47281538",
        "47153941",
        "46765489",
        "47376584",
        "47075901",
        "49113279",
        "46864977",
        "47441323",
        "48892859",
        "49258045",
        "49102636",
        "47933429",
        "48121439",
        "46711958",
        "49713894",
        "49716282",
    ):
        assert f"https://news.ycombinator.com/item?id={item_id}" in report

    ledger = report.split("<!-- hn-required-ledger:start -->", 1)[1].split(
        "<!-- hn-required-ledger:end -->", 1
    )[0]
    rows = [line for line in ledger.splitlines() if line.startswith("| [")]
    assert len(rows) == 8
    cells = [[cell.strip() for cell in row.strip("|").split("|")] for row in rows]
    assert Counter(cell[1] for cell in cells) == {"Successful": 8}
    assert all(cell[2] == "Not needed" for cell in cells)
    assert all(cell[3].startswith("Refreshed") for cell in cells)
    assert "eight requested, eight successful, zero rate-limited, and zero otherwise failed" in normalized
    # A fallback is only ever counted after a direct failure on the same URL.
    assert "second route tried only after a direct failure" in normalized
    assert "temporarily restricting Show HNs" in normalized

    freshness = report.split("<!-- hn-freshness-ledger:start -->", 1)[1].split(
        "<!-- hn-freshness-ledger:end -->", 1
    )[0]
    freshness_rows = [
        [cell.strip() for cell in line.strip().strip("|").split("|")]
        for line in freshness.splitlines()
        if line.startswith("| ") and not line.startswith("| Evidence class")
    ]
    freshness_rows = [row for row in freshness_rows if row[1].isdigit()]
    assert len(freshness_rows) >= 8
    for row in freshness_rows:
        requested, success, limited, other, fallback = (int(value) for value in row[1:6])
        # Failed and rate-limited paths must never be folded into success.
        assert requested == success + limited + other, row[0]
        assert fallback <= limited + other, row[0]

    for prohibited in (
        "human must write",
        "Do not ask anyone to upvote or comment",
        "Nobody may be asked to submit, upvote, comment",
    ):
        assert prohibited in normalized


def test_every_dated_ledger_matches_the_evidence_refresh_date():
    report = _read(REPORT)
    refreshed = re.search(r"^Evidence refreshed: \*\*(\d{4}-\d{2}-\d{2}) ", report, re.M).group(1)
    assert f"### Freshness and retrieval ledger — {refreshed}" in report
    assert f"| Required URL requested {refreshed} |" in report
    assert f"| Target | Measure | Baseline {refreshed} |" in report

    since = re.search(r"^## Status since (\d{4}-\d{2}-\d{2})$", report, re.M).group(1)
    assert since < refreshed
    table = report.split(f"## Status since {since}", 1)[1].split("\n## ", 1)[0]
    areas = [
        line.strip().strip("|").split("|")[0].strip()
        for line in table.splitlines()
        if line.startswith("| ") and not line.startswith("| ---")
    ][1:]
    assert areas and len(areas) == len(set(areas))


def test_deployed_download_and_homepage_claim_drift_remain_launch_blockers():
    report = _read(REPORT)
    normalized = " ".join(report.split())
    for evidence in (
        "live `/desktop` page is manifest-driven",
        "no implicit source fallback is promised",
        "Linux · macOS, Windows & Android downloads",
        "set dressing, not data",
        "claim-by-claim evidence ledger",
        "`/api/status` reported every system operational",
        "needs an account and operator control",
    ):
        assert evidence in normalized


def test_homepage_install_note_names_only_the_curl_platform():
    html = _read(ROOT / "www" / "public" / "index.html")
    assert "macOS &amp; Linux</span>" not in html
    assert '<a href="#downloads">macOS, Windows &amp; Android downloads</a>' in html


def test_homepage_platform_claims_track_the_signed_manifest():
    """#704: the homepage may name an operating system only while the signed
    latest manifest publishes an asset for it. Pinned to the manifest rather
    than to today's wording, so a release that drops a platform fails here
    instead of shipping a hero that promises a download nobody can get."""
    latest = json.loads(_read(LATEST / "release.json"))
    manifest_os = {asset["os"] for asset in latest["assets"]}
    assert manifest_os, "signed latest manifest lists no assets"
    html = re.sub(
        r"<!--.*?-->", "", _read(ROOT / "www" / "public" / "index.html"), flags=re.S
    )

    # The machine-readable claim must be exactly the manifest's platform set.
    declared = re.findall(r'"operatingSystem":\s*"([^"]*)"', html)
    assert declared, "no operatingSystem claim found"
    for value in declared:
        assert {name.strip().lower() for name in value.split(",")} == manifest_os, value

    # The visible install note must agree with it, name by name.
    note = re.search(r'<span class="hero-agent-tab-note">(.*?)</span>', html, flags=re.S)
    assert note, "hero install note not found"
    for key, label in (
        ("macos", "macOS"),
        ("linux", "Linux"),
        ("windows", "Windows"),
        ("android", "Android"),
    ):
        assert (label in note.group(1)) == (key in manifest_os), label


def test_homepage_downloads_link_only_assets_the_manifest_publishes():
    """#704: `homepage-downloads.js` links `knownAssets` unconditionally and
    gates its status probe on them, so every name listed there must exist in
    the signed latest manifest. Platforms without an asset belong in
    `pendingAssets`, which renders a disabled placeholder instead of a link."""
    latest = json.loads(_read(LATEST / "release.json"))
    published = {asset["name"] for asset in latest["assets"]}
    script = _read(ROOT / "www" / "public" / "homepage-downloads.js")

    known = re.search(r"const knownAssets = new Map\(\[(.*?)\]\);", script, flags=re.S)
    assert known, "knownAssets block not found"
    listed = re.findall(r'"(forkmesh-[^"]+)"', known.group(1))
    assert listed, "knownAssets names no assets"
    for asset_name in listed:
        assert asset_name in published, asset_name


def test_homepage_illustrative_activity_ticker_does_not_render():
    """#704: the mesh-activity ticker is set dressing. It may stay parked in the
    source, but it must not reach the rendered page, where a sighted reader
    would read the fake statuses as live mesh data."""
    raw = _read(ROOT / "www" / "public" / "index.html")
    rendered = re.sub(r"<!--.*?-->", "", raw, flags=re.S)
    assert "set dressing, not data" in raw, "the illustrative annotation was dropped"
    for marker in ("hero-cards-track", 'class="hero-card"'):
        assert marker not in rendered, marker


def test_download_page_discloses_each_missing_signed_asset():
    html = _read(ROOT / "www" / "public" / "desktop.html")
    assert "each card below enables only packages" in html
    assert "Not published in ${manifest.tag}: ${name}" in html
    assert "Native release matrix incomplete" not in html
    assert "never\n        compiles as an implicit fallback" in html


def test_availability_proof_is_part_of_the_show_path():
    """The show path must recompute the public proof, and the report must
    state the operator quorum outcome rather than assert independence."""
    report = _read(REPORT)
    normalized = " ".join(report.split())
    assert "forkmesh_availability_proof.py" in report
    assert "/api/repo/forkmesh/forkmesh/mirrors" in report
    assert "forkmesh-availability-proof-v1" in report
    assert "1 independent operator (target 2)" in normalized
    assert 'availability.proof == "quorum"' in normalized
    assert (ROOT / "tools" / "forkmesh_availability_proof.py").exists()
    assert (ROOT / "www" / "docs" / "availability-proof.md").exists()


def test_launch_gates_are_assigned_and_metrics_stay_tracked():
    shortcut = _read(ROOT / ".forkmesh" / "shortcuts" / "prepare-y-combinator-news-launch.md")
    report = _read(REPORT)
    for text in (shortcut, report):
        normalized = " ".join(text.split())
        for gate in (
            "Second independent operator",
            "Launch blockers closed",
            "Try-it path verified",
            "Human staffed and eligible",
        ):
            assert gate in normalized
        assert "`usage.users.total`" in normalized
        assert "`.forkmesh/issues/open/`" in normalized
        assert "`/api/tasks`" in normalized
    normalized_shortcut = " ".join(shortcut.split())
    assert "`ready` additionally requires all four" in normalized_shortcut
    assert "never launch gates" in normalized_shortcut
    assert "Never grow the count with test, seeded, or bot accounts" in normalized_shortcut
    assert "never evidence of users or adoption" in normalized_shortcut

    gates = report.split("<!-- launch-gates:start -->", 1)[1].split("<!-- launch-gates:end -->", 1)[0]
    gate_rows = [
        [cell.strip() for cell in line.strip().strip("|").split("|")]
        for line in gates.splitlines()
        if line.startswith("| ") and not line.startswith("| Gate") and not line.startswith("| ---")
    ]
    assert [row[0] for row in gate_rows] == [
        "Second independent operator",
        "Launch blockers closed",
        "Try-it path verified",
        "Human staffed and eligible",
    ]
    for row in gate_rows:
        assert row[1] and row[2] and row[3]
        if not row[2].startswith("Met"):
            assert "**blocked**" in report.split("## 10. Verdict", 1)[1].split("\n", 3)[2]
    assert "`quorum`" in gate_rows[0][1] or "quorum" in gate_rows[0][1]

    table = report.split("<!-- launch-goal-targets:start -->", 1)[1].split(
        "<!-- launch-goal-targets:end -->", 1
    )[0]
    rows = [
        [cell.strip() for cell in line.strip().strip("|").split("|")]
        for line in table.splitlines()
        if line.startswith("| ") and not line.startswith("| Target")
    ]
    rows = [row for row in rows if not set(row[0]) <= {"-", " "}]
    assert [row[0] for row in rows] == ["Users", "Open issues", "Open tasks", "Ecosystem listings"]
    for row in rows:
        assert row[2] and row[4] in {"Measured fact", "Repository evidence", "Deployed observation", "Missing proof"}
    assert "never evidence of users or adoption" in rows[3][3]
    assert "also requires all four launch gates below" in " ".join(report.split())


def test_relay_retention_table_is_linked_from_the_security_faq():
    report = _read(REPORT)
    faq = report.split("## 5. Security and trust FAQ", 1)[1].split(
        "## 6. Agent-system FAQ", 1)[0]
    assert "(../relay-data-retention.md)" in faq
    table = _read(ROOT / "www" / "docs" / "relay-data-retention.md")
    wrangler = _read(ROOT / "app" / "wrangler.toml")
    for binding in ("REPOSITORY_METADATA", "forkmesh-notification-email"):
        assert binding in wrangler and f"`{binding}`" in table
    for durable_object in (
        "ForkMeshRoom", "ForkMeshNodes", "ForkMeshCronWatchdog",
        "ForkMeshCronRunner", "ForkMeshDiscordGate", "ForkMeshNoteRoom",
        "ForkMeshWorld", "ForkMeshOfficeRoom",
    ):
        assert durable_object in table
    assert "tools/next_backup.py" in table
    assert "KEEP_DAILY = 7" in _read(ROOT / "tools" / "next_backup.py")
    assert "KEEP_WEEKLY = 4" in _read(ROOT / "tools" / "next_backup.py")
    assert "SystemMaxUse=64M" in _read(
        ROOT / "deploy" / "core" / "templates" / "journald.conf")
    for heading in ("## Where the relay runs", "## Live stores",
                    "## Database tables (rqlite)", "## Backups and copies",
                    "## Logs", "## Outside processors", "## Known gaps"):
        assert heading in table
    assert "[Relay data retention and deletion](relay-data-retention.md)" in _read(
        ROOT / "www" / "docs" / "README.md")


def test_incident_tabletop_names_a_lead_and_controls_that_exist():
    report = _read(REPORT)
    record = _read(ROOT / "www" / "docs" / "launch" / "incident-tabletop-2026-09-16.md")
    assert "(incident-tabletop-2026-09-16.md)" in report
    assert "| Date | 2026-09-16 |" in record
    assert re.search(r"\| Incident lead \| ForkMesh account `[a-z0-9-]+`", record)
    assert "## Row 1: clone fails on every mirror" in record
    assert "18:28:08" in record and "18:30:41" in record
    entry = _read(ROOT / "app" / "src" / "entry.py")
    quoted = re.findall(r"`([A-Z][A-Z_]+) = (\d+)`", record)
    assert len(quoted) >= 8
    for name, value in quoted:
        match = re.search(rf"^{name} = (\d+)\b", entry, re.M)
        assert match and match.group(1) == value, name
    assert "/api/mirrors/admission-override" in _read(
        ROOT / "app" / "src" / "app_runtime.py")
    assert "ADMISSION_OVERRIDE_DEFAULT_TTL = 30 * 60" in entry
    assert "ADMISSION_OVERRIDE_MAX_TTL = 6 * 60 * 60" in entry


def test_clean_machine_evidence_is_bound_to_the_signed_manifest():
    """Issue #705: each clean-machine step names the release it exercised, and
    steps no fresh VM has run stay marked as missing proof."""
    report = _read(REPORT)
    evidence = _read(CLEAN_MACHINE)
    tool = _read(TRANSCRIPT_TOOL)
    latest = json.loads(_read(LATEST / "release.json"))
    assets = {asset["name"]: asset for asset in latest["assets"]}

    assert "(clean-machine-evidence.md)" in report
    assert latest["tag"] in evidence
    assert latest["checksums_sha256"] in evidence
    windows = assets["forkmesh-windows-x86_64-setup.exe"]
    assert windows["blob_sha256"] in evidence
    assert str(windows["size"]) in evidence
    assert assets["forkmesh-linux-x86_64"]["blob_sha256"] in evidence

    # The harness pins no version: it verifies whatever latest publishes, and
    # only enforces a tag or checksum when one is passed in.
    assert 'EXPECTED_TAG="${FORKMESH_EXPECTED_TAG:-}"' in tool
    assert 'EXPECTED_CHECKSUMS_SHA256="${FORKMESH_EXPECTED_CHECKSUMS_SHA256:-}"' in tool
    assert latest["tag"] not in tool and latest["checksums_sha256"] not in tool

    # The measured Authenticode signer is the certificate the release host pins.
    pinned = re.search(
        r'DEFAULT_WINDOWS_SIGNER_SHA256 = \(\s*"([0-9a-f]{64})"',
        _read(ROOT / "tools" / "forkmesh-release-host.py"),
    ).group(1)
    fingerprint = re.search(r"sha256 Fingerprint=([0-9A-F:]{95})", evidence).group(1)
    assert fingerprint.replace(":", "").lower() == pinned

    # The quoted purge prompt is the uninstaller's own text.
    uninstall = _read(ROOT / "www" / "public" / "uninstall.sh")
    for line in (
        "This removes ForkMesh and ALL of its data from this computer:",
        "settings, the node identity key (the account cannot be recovered)",
        "Type DELETE to continue: ",
        "Refusing to erase ForkMesh data without confirmation.",
    ):
        assert line in uninstall
        assert line.strip() in " ".join(evidence.split())

    # Until a VM transcript is committed, the GUI and uninstall steps are unproven.
    status = evidence.split("## Status", 1)[1].split("###", 1)[0]
    for step in (
        "Installer run on a fresh machine",
        "First launch and node identity creation",
        "Provider CLI discovery",
        "Continual cycle stops at the human decision card",
        "**Stop work** pauses the loop",
        "Uninstall (default keeps data)",
        "SmartScreen dialog text",
    ):
        row = next(line for line in status.splitlines() if line.startswith(f"| {step} |"))
        if not list(CLEAN_MACHINE.parent.glob("*clean-machine-*.log")):
            assert "**Missing proof**" in row


def test_clean_machine_transcript_refuses_a_host_with_a_live_node():
    """uninstall.sh stops every process named forkmesh; the transcript must never
    run beside a production node or on a machine that already holds ForkMesh data."""
    tool = _read(TRANSCRIPT_TOOL)
    assert "pkill -x forkmesh" in _read(ROOT / "www" / "public" / "uninstall.sh")
    run_body = tool.split("run_transcript() {", 1)[1]
    assert run_body.lstrip().startswith("require_fresh_machine")
    guard = tool.split("require_fresh_machine() {", 1)[1].split("\n}\n", 1)[0]
    assert "pgrep -x forkmesh" in guard
    for path in ('"$data_home/ForkMesh"', '"$config_home/ForkMesh"', '"$HOME/.forkmesh"'):
        assert path in guard
    # An unanswered GUI checkpoint is logged as not observed, never as a pass.
    assert 'log "NOT OBSERVED $(utc)"' in tool
