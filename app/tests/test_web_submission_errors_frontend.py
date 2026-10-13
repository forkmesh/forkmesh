#!/usr/bin/env python3
"""Web issue form: a failed submission says what actually went wrong (#685).

The form used to answer every rejection it had no sentence for with "Could
not send the issue. Please try again.", so a report could never say whether
the signature, the network, or the server was at fault. These run the real
helpers from ``public/dashboard/js/07-repo-compose-branch.js`` in node.
"""

import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FRAGMENT = ROOT / "public" / "dashboard" / "js" / "07-repo-compose-branch.js"


def _helpers_source():
    text = FRAGMENT.read_text(encoding="utf-8")
    start = text.index("  function webSubmissionErrorDetail(")
    end = text.index("  async function handleIssueComposeSubmit(", start)
    return text[start:end]


def _issue_error_text(error_js):
    script = (
        _helpers_source()
        + "\nconst error = (() => { " + error_js + " })();"
        + "\nconsole.log(JSON.stringify(webIssueSubmitErrorText(error)));\n"
    )
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True, text=True, capture_output=True,
    )
    return json.loads(result.stdout)


def _mutation_error(code, status):
    # The shape fetchRepoMutation() throws for a rejected POST.
    return (
        f"const e = new Error({json.dumps(code or f'HTTP {status}')});"
        f" e.status = {status}; e.code = {json.dumps(code)}; return e;"
    )


def test_known_rejections_keep_their_specific_sentences():
    assert "temporarily full" in _issue_error_text(_mutation_error("inbox_full", 429))
    assert "submission limit" in _issue_error_text(_mutation_error("author_quota", 429))
    assert "too large" in _issue_error_text(_mutation_error("issue_too_large", 413))
    assert "owner or an admin" in _issue_error_text(_mutation_error("not_authorized", 403))


def test_rejections_that_used_to_fall_through_are_named():
    assert "signature" in _issue_error_text(_mutation_error("bad_signature", 401))
    assert "could not be resolved" in _issue_error_text(
        _mutation_error("alias_unresolved", 503))
    # An edge 413 or 429 page carries no JSON error code at all.
    assert "too large" in _issue_error_text(_mutation_error("", 413))
    assert "Too many submissions" in _issue_error_text(_mutation_error("", 429))


def test_a_dropped_connection_is_not_reported_as_a_server_rejection():
    for message in ("Failed to fetch", "Load failed",
                    "NetworkError when attempting to fetch resource."):
        text = _issue_error_text(f"return new TypeError({json.dumps(message)});")
        assert "check your connection" in text


def test_a_browser_without_ed25519_is_told_so():
    text = _issue_error_text(
        'const e = new Error("Algorithm: Unrecognized name");'
        ' e.name = "NotSupportedError"; return e;')
    assert "Ed25519" in text


def test_the_fallback_carries_the_code_or_status_it_got():
    assert _issue_error_text(_mutation_error("invalid_json", 400)) == (
        "Could not send the issue (invalid_json, HTTP 400). Please try again.")
    assert _issue_error_text(_mutation_error("", 502)) == (
        "Could not send the issue (HTTP 502). Please try again.")
    # A code bug is a TypeError too, but not a network failure.
    text = _issue_error_text(
        'return new TypeError("Cannot read properties of undefined");')
    assert "check your connection" not in text
    assert "Cannot read properties of undefined" in text
