#!/usr/bin/env python3
"""Web pull composer: a failed submission says which half failed (web #49).

A web pull request is signed from a mirror's comparison of the two branches,
so the compare read runs before anything is signed or delivered. A mirror
answers 404 ``not_found`` for a branch name it cannot resolve yet - the common
case for a branch that was just pushed - and the composer reported that as
"Could not send the pull request (not_found, HTTP 404). Please try again.",
which reads as a rejected submission. These run the real helper from
``public/dashboard/js/07-repo-compose-branch.js`` in node.
"""

import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
JS_DIR = ROOT / "public" / "dashboard" / "js"
FRAGMENT = JS_DIR / "07-repo-compose-branch.js"
IO_FRAGMENT = JS_DIR / "03-issues-profile-io.js"


def _helpers_source():
    text = FRAGMENT.read_text(encoding="utf-8")
    start = text.index("  function webSubmissionErrorDetail(")
    end = text.index("  async function handleIssueComposeSubmit(", start)
    return text[start:end]


def _error_text(error_js, base="main", head="agent/fix", target="forkmesh/forkmesh"):
    script = (
        _helpers_source()
        + "\nconst error = (() => { " + error_js + " })();"
        + "\nconsole.log(JSON.stringify(webPullSubmitErrorText(error, "
        + ", ".join(json.dumps(value) for value in (base, head, target))
        + ")));\n"
    )
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True, text=True, capture_output=True,
    )
    return json.loads(result.stdout)


def _comparison_error(code, status, cause_js="null"):
    # The shape submitWebPullOpen() throws when the compare read fails.
    return (
        'const e = new Error("comparison_unavailable");'
        f" e.status = {status}; e.code = {json.dumps(code)};"
        f" e.cause = (() => {{ {cause_js} }})(); return e;"
    )


def _mutation_error(code, status):
    # The shape fetchRepoMutation() throws for a rejected POST.
    return (
        f"const e = new Error({json.dumps(code or f'HTTP {status}')});"
        f" e.status = {status}; e.code = {json.dumps(code)}; return e;"
    )


def test_the_compare_read_tags_its_failures_before_anything_is_signed():
    # submitWebPullOpen must mark the comparison step, or the composer cannot
    # tell a comparison that never ran from a proposal the relay refused.
    source = IO_FRAGMENT.read_text(encoding="utf-8")
    start = source.index("  async function submitWebPullOpen(")
    end = source.index("const { privateKey, pub } = await getWebIssueKey();", start)
    opening = source[start:end]
    assert 'new Error("comparison_unavailable")' in opening
    assert "error.cause = cause;" in opening


def test_an_unresolved_branch_is_not_reported_as_a_failed_submission():
    text = _error_text(_comparison_error("not_found", 404))
    assert "Nothing was sent" in text
    assert "main or agent/fix" in text
    assert "sync to arrive" in text
    # The sentence that sent this report in the first place.
    assert "Could not send the pull request" not in text
    assert "not_found, HTTP 404" not in text


def test_an_oversized_comparison_says_so_instead_of_try_again():
    text = _error_text(_comparison_error("", 413))
    assert "larger than a mirror will serve" in text
    assert "Nothing was sent" in text


def test_a_dropped_connection_during_the_comparison_names_the_connection():
    text = _error_text(_comparison_error(
        "network_error", 0,
        'return new TypeError("Failed to fetch");'))
    assert "check your connection" in text


def test_an_unnamed_comparison_failure_still_carries_its_detail():
    text = _error_text(_comparison_error("mirror_unavailable", 503))
    assert "could not be compared" in text
    assert "mirror_unavailable, HTTP 503" in text


def test_delivery_rejections_keep_their_specific_sentences():
    assert "inbox is full" in _error_text(_mutation_error("inbox_full", 429))
    assert "submission limit" in _error_text(_mutation_error("author_quota", 429))
    assert "too large" in _error_text(_mutation_error("pull_too_large", 413))
    assert "signature" in _error_text(_mutation_error("bad_signature", 401))
    assert "durable pull-request number" in _error_text(
        _mutation_error("number_unavailable", 503))
    assert "do not contain any changes" in _error_text(
        'return new Error("no_changes");')
    assert "invalid branch comparison" in _error_text(
        'return new Error("invalid_comparison");')


def test_an_undeliverable_address_is_named_rather_than_retried():
    # The delivery route answers 404 for a repository it has no public record
    # of; "try again" is the one thing that cannot help there.
    text = _error_text(_mutation_error("not_found", 404))
    assert "forkmesh/forkmesh" in text
    assert "no public record" in text
    assert "try again" not in text.lower()


def test_a_browser_without_ed25519_is_told_so():
    text = _error_text(
        'const e = new Error("Algorithm: Unrecognized name");'
        ' e.name = "NotSupportedError"; return e;')
    assert "Ed25519" in text


def test_the_fallback_still_carries_the_code_or_status_it_got():
    assert _error_text(_mutation_error("invalid_json", 400)) == (
        "Could not send the pull request (invalid_json, HTTP 400). "
        "Please try again.")
    assert _error_text(_mutation_error("", 502)) == (
        "Could not send the pull request (HTTP 502). Please try again.")
