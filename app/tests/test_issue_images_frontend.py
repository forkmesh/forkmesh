#!/usr/bin/env python3
"""Web issue compose form: image attachments (adhoc #85).

Attached images travel as base64 data: URLs embedded in the issue body — no
separate upload channel — so they share the body's MAX_ISSUE_BYTES cap in the
worker. These tests pin the client-side guards (count/size limits, image-only
input), the placeholder flow that keeps the textarea readable, and the
swap-back right before signing so the signed hash covers what is sent.
"""

import re
from pathlib import Path

from _dashboard_bundle import assembled_dashboard_js


PUBLIC = Path(__file__).resolve().parents[1] / "public"
ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
# dashboard.js is built from ordered public/dashboard/js/*.js fragments before
# deploy (see src/dashboard_bundle.py).
DASHBOARD_JS = assembled_dashboard_js()
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")


def _js_const(name):
    match = re.search(r"const %s = ([0-9*\s]+);" % name, DASHBOARD_JS)
    assert match, "missing constant %s in dashboard.js" % name
    return eval(match.group(1))  # trusted repo source, arithmetic only


def test_compose_form_has_attach_controls_and_image_only_input():
    assert "data-repo-issue-attach-image" in DASHBOARD_JS
    assert "data-repo-issue-attach-hint" in DASHBOARD_JS
    assert "data-repo-issue-attachments" in DASHBOARD_JS
    # The file input accepts a specific image whitelist, and any file that
    # slips past it is still rejected with a visible hint.
    assert 'accept="image/png,image/jpeg,image/gif,image/webp"' in DASHBOARD_JS
    assert 'if (!file.type.startsWith("image/"))' in DASHBOARD_JS
    assert "not an image" in DASHBOARD_JS


def test_attachment_budget_fits_inside_the_worker_issue_cap():
    max_count = _js_const("ISSUE_IMAGE_MAX_COUNT")
    max_bytes = _js_const("ISSUE_IMAGE_MAX_BYTES")
    max_total = _js_const("ISSUE_IMAGE_MAX_TOTAL_BYTES")
    assert 0 < max_bytes <= max_total
    assert max_count >= 1

    # Worker-side cap the embedded data: URLs must share with the issue text.
    entry_match = re.search(r"MAX_ISSUE_BYTES = ([0-9*\s]+)", ENTRY_TEXT)
    assert entry_match
    max_issue_bytes = eval(entry_match.group(1))
    # base64 expands raw bytes 4/3; the total image budget must leave room for
    # the title/description text too, or every attachment-bearing issue would
    # bounce with issue_too_large. Today that headroom is exactly 4 KB
    # (45 KB raw -> 60 KB encoded of the 64 KB cap); don't let it shrink.
    assert max_issue_bytes - max_total * 4 / 3 >= 4 * 1024

    # The guards are actually enforced per selection: a hard raw-size cap, the
    # count cap, and a remaining-budget computation that feeds the crop/resize
    # flow for anything that would blow the shared total.
    assert "file.size > ISSUE_IMAGE_RAW_MAX_BYTES" in DASHBOARD_JS
    assert ("const budget = Math.min(ISSUE_IMAGE_MAX_BYTES, "
            "ISSUE_IMAGE_MAX_TOTAL_BYTES - total);") in DASHBOARD_JS
    assert "images.length >= ISSUE_IMAGE_MAX_COUNT" in DASHBOARD_JS


def test_body_carries_placeholders_until_signing():
    # The textarea gets a short pending-image id, not a multi-KB data URL, so
    # the body stays readable and editable while composing.
    assert "forkmesh-pending-image:" in DASHBOARD_JS
    assert "![${name}](${id})" in DASHBOARD_JS

    # Right before signing/sending, every placeholder is swapped back out for
    # its real data: URL — the signed content hash must cover the sent bytes.
    submit = DASHBOARD_JS[
        DASHBOARD_JS.index("async function handleIssueComposeSubmit"):
        DASHBOARD_JS.index("function renderRepoDetail")
    ]
    assert "body = body.split(img.id).join(img.dataUrl);" in submit
    # The swapped body (not the raw textarea value) is what gets submitted;
    # the trailing args carry the agent-assign flow (adhoc #105).
    assert "const result = await submitWebIssue(" in submit
    assert "repo, title, body, assignAgent, agentModel, agentProvider" in submit
    # And the queue resets after a successful send.
    assert "images.length = 0;" in submit


def test_issue_submission_explains_each_waiting_phase():
    submit = DASHBOARD_JS[
        DASHBOARD_JS.index("async function handleIssueComposeSubmit"):
        DASHBOARD_JS.index("function renderRepoDetail")
    ]
    signer = DASHBOARD_JS[
        DASHBOARD_JS.index("async function submitWebIssue"):
        DASHBOARD_JS.index("// A checklist", DASHBOARD_JS.index(
            "async function submitWebIssue"))
    ]
    routing = DASHBOARD_JS[
        DASHBOARD_JS.index("async function fetchRepoMutation"):
        DASHBOARD_JS.index("const PULL_METADATA_BRANCH")
    ]
    # The vague one-state spinner used to hide whether signing, route failover,
    # or the relay itself was slow. Keep the user-facing phases coupled to the
    # underlying boundaries instead of adding a timer that guesses.
    for text in (
        "Preparing and signing your issue",
        "Sending your signed issue to an available mirror",
        "trying another available mirror",
        "Sending your signed issue to the repository",
        "accepted — synchronizing it with the repository",
    ):
        assert text in submit
    assert "onProgress?.({ phase: \"signing\" })" in signer
    assert "onProgress?.({ phase: \"accepted\", number:" in signer
    assert 'phase: "delivering"' in routing
    assert "{ milestone, project }, updateSubmissionProgress" in submit


def test_removing_a_chip_also_strips_its_placeholder_from_the_body():
    assert "data-repo-issue-attachment-remove" in DASHBOARD_JS
    assert ("bodyInput.value.split(`\\n![${removed.name}](${removed.id})\\n`)"
            ".join(\"\\n\")") in DASHBOARD_JS


def test_oversize_images_get_the_resize_flow_not_a_flat_rejection():
    # An image over the remaining budget opens the crop/compress modal instead
    # of bouncing; only an exhausted budget or the hard raw cap rejects, and
    # both hints say why. The server-side issue_too_large error still mentions
    # the images too.
    assert "openImageResizeModal(file, budget)" in DASHBOARD_JS
    assert "crop or compress it to fit under" in DASHBOARD_JS
    assert ("Attached images already use up the issue's size limit"
            in DASHBOARD_JS)
    assert "is too large to attach (max" in DASHBOARD_JS
    assert "please shorten it or attach smaller images" in DASHBOARD_JS
