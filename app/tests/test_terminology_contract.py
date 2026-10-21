#!/usr/bin/env python3
"""Familiar developer terminology contract (issue #508).

www/docs/terminology.json is the approved glossary, discouraged-term list, and
compatibility-alias table; tools/terminology_lint.py enforces it on primary
surfaces. These tests pin the lint's behavior (including the ways copy can
fail it), keep primary surfaces clean, and prove every alias still resolves.
"""

import copy
import json
import re
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
for extra in (ROOT / "tools", ROOT / "app" / "src", ROOT / "app" / "tools"):
    if str(extra) not in sys.path:
        sys.path.insert(0, str(extra))

import static_routes  # noqa: E402
import terminology_lint as lint  # noqa: E402

GUIDE = ROOT / "www" / "docs" / "terminology.md"
CONTRACT = lint.load_contract()


def _lint_source(tmp_path, name, source, contract=CONTRACT):
    path = tmp_path / name
    path.write_text(source, encoding="utf-8")
    return lint.lint([path], contract, root=tmp_path, check_allowances=False)


def _terms(result):
    return [finding.term for finding in result.findings]


# --- Glossary and guide -----------------------------------------------------


def test_glossary_defines_every_approved_term_in_contract_and_guide():
    guide = GUIDE.read_text(encoding="utf-8")
    approved = {item["term"]: item for item in CONTRACT["approvedTerms"]}
    for term in lint.REQUIRED_APPROVED_TERMS:
        assert approved[term]["definition"]
        assert f"| **{term}** |" in guide, term
    for item in CONTRACT["protocolTerms"]:
        assert item["explanation"]
        assert f"| {item['term']} |" in guide, item["term"]
    for item in CONTRACT["discouragedTerms"]:
        assert item["category"] in lint.CATEGORIES


def test_guide_documents_measurement_comprehension_and_evidence():
    normalized = " ".join(GUIDE.read_text(encoding="utf-8").split())
    for marker in (
        "discouragedTermCount",
        "Cloudflare zone analytics",
        "Support vocabulary",
        "every five-person cohort",
        "maintainers, contributors, and operators",
        "A cohort passes at 4/5 or better",
        "Do not mark this gate complete with blank",
        "Rollback.",
        "never renamed for wording",
    ):
        assert marker in normalized, marker
    for relative in re.findall(r"`((?:app|www)/tests/[^`]+\.py)`", normalized):
        assert (ROOT / relative).is_file(), relative


def test_guide_is_published_in_the_docs_library_and_quality_gates():
    import docs_site

    readme = (ROOT / "www" / "docs" / "README.md").read_text(encoding="utf-8")
    assert "(terminology.md)" in readme
    assert "terminology.md" in docs_site.GUIDE_ORDER

    gates = json.loads(
        (ROOT / "www" / "docs" / "engineering" / "quality-gates.json").read_text(encoding="utf-8"))
    gate = next(item for item in gates["gates"] if item["id"] == "terminology-contract")
    assert "app/tests/test_terminology_contract.py" in gate["command"]
    for profile in ("core", "release"):
        assert "terminology-contract" in gates["profiles"][profile]


# --- Primary surfaces -------------------------------------------------------


def test_primary_surfaces_have_no_unexplained_discouraged_terms():
    result = lint.lint(lint.resolve_surfaces(CONTRACT["primarySurfaces"]), CONTRACT)
    assert result.findings == []
    assert result.stale_allowances == []
    assert result.unexplained_allowances == []
    # Every journey the guide calls primary is actually scanned.
    for relative in (
        "www/public/index.html",
        "www/public/docs/onboarding/index.html",
        "www/docs/getting-started.md",
        "app/public/dashboard/js/06-repo-content.js",
        "app/public/dashboard/partials/views/microjobs.html",
    ):
        assert relative in result.scanned, relative


def test_cli_exit_status_and_json_report(capsys):
    assert lint.main(["--json"]) == 0
    report = json.loads(capsys.readouterr().out)
    assert report[0]["scope"] == "primary"
    assert report[0]["discouragedTermCount"] == 0
    assert report[0]["scannedFiles"] > 20


def test_cli_fails_on_a_file_with_a_discouraged_term(tmp_path, capsys):
    page = tmp_path / "page.html"
    page.write_text("<p>Open a merge request.</p>\n", encoding="utf-8")
    assert lint.main([str(page)]) == 1
    output = capsys.readouterr().out
    assert f'{page.as_posix()}:1: [merge-request] "merge request" -> use pull request' in output

    page.write_text("<p>Open a pull request.</p>\n", encoding="utf-8")
    assert lint.main([str(page)]) == 0


# --- What the lint reads, and what it ignores -------------------------------


def test_html_checks_text_and_accessible_attributes_but_not_markup_or_code(tmp_path):
    result = _lint_source(tmp_path, "page.html", """<main>
  <p class="peer-to-peer">Open a merge
    request from your fork.</p>
  <button aria-label="Approve on the Qt desktop node">Approve</button>
  <a href="https://example.com/merge-requests" data-x="Qt client">Docs</a>
  <script>const label = "merge request";</script>
  <pre><code>git push # peer-to-peer</code></pre>
  <!-- mesh nodes -->
</main>
""")
    assert sorted(_terms(result)) == ["merge-request", "qt-product-name"]
    lines = {finding.term: finding.line for finding in result.findings}
    assert lines == {"merge-request": 2, "qt-product-name": 4}


def test_markdown_skips_code_and_link_targets(tmp_path):
    result = _lint_source(tmp_path, "guide.md", """# Guide

Read the [desktop guide](qt-client.md) and `peer-to-peer` code.

```sh
echo "merge request"
```

Each mirror is a delegate of the owner.
""")
    assert _terms(result) == ["delegate"]
    assert result.findings[0].line == 9


def test_code_checks_sentence_literals_including_template_text(tmp_path):
    result = _lint_source(tmp_path, "view.js", """
// A merge request comment is not user-facing.
const kind = "delegate";
const message = "The P2P mesh is down.";
const html = `<p>${count > 1 ? `${count} seed nodes` : "One mirror"} ready for
  your peer approval</p>`;
const url = "https://example.com/merge_requests";
""")
    assert _terms(result) == ["peer-to-peer", "seed-node", "peer-approval"]
    assert [finding.line for finding in result.findings] == [4, 5, 6]


def test_inline_marker_allows_one_named_term(tmp_path):
    result = _lint_source(tmp_path, "view.js", """
// terminology: allow=merge-request
const imported = "GitLab merge request payloads";
const other = "GitLab merge request payloads";
""")
    assert _terms(result) == ["merge-request"]
    assert result.findings[0].line == 4
    assert result.allowed == 1


def test_explain_allowance_requires_its_explanation_and_stale_ones_fail(tmp_path):
    contract = copy.deepcopy(CONTRACT)
    contract["allowances"] = [
        {"path": "a.md", "term": "attestation", "reason": "explained",
         "explanation": "the signed list of branch and tag commits"},
        {"path": "b.md", "term": "lease", "reason": "no longer used"},
    ]
    explained = tmp_path / "a.md"
    explained.write_text(
        "The refs attestation (the signed list of branch and tag\ncommits).\n",
        encoding="utf-8")
    unused = tmp_path / "b.md"
    unused.write_text("No protocol terms here.\n", encoding="utf-8")

    result = lint.lint([explained, unused], contract, root=tmp_path)
    assert result.findings == [] and result.allowed == 1
    assert [item["path"] for item in result.stale_allowances] == ["b.md"]
    assert not result.ok

    explained.write_text("The refs attestation is recorded.\n", encoding="utf-8")
    result = lint.lint([explained], contract, root=tmp_path)
    assert [item["path"] for item in result.unexplained_allowances] == ["a.md"]
    assert not result.ok


def test_malformed_contracts_are_rejected():
    for mutate, message in (
        (lambda c: c.update(schemaVersion=2), "schemaVersion"),
        (lambda c: c["approvedTerms"].pop(0), "missing"),
        (lambda c: c["discouragedTerms"][0].update(pattern="("), "invalid pattern"),
        (lambda c: c["discouragedTerms"][1].update(id=c["discouragedTerms"][0]["id"]),
         "duplicate"),
        (lambda c: c["primarySurfaces"].append("no/such/*.html"), "matches no file"),
        (lambda c: c["allowances"].append({"path": "x", "term": "nope", "reason": "r"}),
         "unknown term"),
        (lambda c: c["compatibilityAliases"].append(copy.deepcopy(c["compatibilityAliases"][0])),
         "duplicate compatibility alias"),
    ):
        contract = copy.deepcopy(CONTRACT)
        mutate(contract)
        with pytest.raises(lint.ContractError, match=message):
            lint.validate_contract(contract)


# --- Compatibility aliases --------------------------------------------------


def _concrete(template):
    return (template.replace("OWNER", "forkmesh").replace("REPOSITORY", "forkmesh")
            .replace("NUMBER", "12"))


@pytest.mark.parametrize(
    "alias",
    [item for item in CONTRACT["compatibilityAliases"] if item["kind"] == "route"],
    ids=lambda item: item["id"],
)
def test_every_route_alias_redirects_to_its_canonical_route(alias):
    resolver = getattr(static_routes, alias["resolver"])
    path, _, query = _concrete(alias["alias"]).partition("?")
    canonical = _concrete(alias["canonical"])
    assert resolver(path, query) == canonical
    if alias["resolver"] == "repo_alias_redirect":
        assert static_routes.looks_like_repo_route(canonical)
        assert resolver(path, "tab=files&x=1") == canonical + "?tab=files&x=1"


def test_repo_aliases_preserve_deep_paths_and_leave_other_routes_alone():
    redirect = static_routes.repo_alias_redirect
    assert redirect("/o/r/pulls/7/files", "") == "/o/r/pull/7/files"
    assert redirect("/o/r/pulls/7/", "") == "/o/r/pull/7"
    assert redirect("/o/r/pulls/7/commits", "w=1") == "/o/r/pull/7/commits?w=1"
    assert redirect("/o/r/pull-requests/7/checks", "") == "/o/r/pull/7/checks"
    assert redirect("/o/r/-/merge_requests/7/diffs", "") == "/o/r/pull/7/diffs"
    assert redirect("/o/r/-/pull/7", "") == "/o/r/pull/7"
    assert redirect("/o/r/pull", "q=is%3Aopen") == "/o/r/pulls?q=is%3Aopen"
    assert redirect("/o/r/pull/send", "") == "/o/r/pulls/send"
    assert redirect("/o/r/-/tree/main/src", "") == "/o/r/tree/main/src"
    assert redirect("/o/r/-/compare/main...feature", "") == "/o/r/compare/main...feature"
    assert redirect("/o/my%20repo/merge_requests", "") == "/o/my%20repo/pulls"
    for untouched in (
        "/o/r/pull/7",
        "/o/r/pull/7/files",
        "/o/r/pull/7/commits",
        "/o/r/pull/7/checks",
        "/o/r/pulls",
        "/o/r/pulls/send",
        "/o/r/pulls/new",
        "/o/r",
        "/o/r/tree/pull",
        "/o/r/-/unknown",
        "/o/r/-",
        "/api/r/pull/7",
        "/dashboard/repos/pull",
        "/docs/x/pull",
    ):
        assert redirect(untouched, "") is None, untouched


def test_worker_issues_repo_alias_redirects_before_serving_documents():
    runtime = (ROOT / "app" / "src" / "app_runtime.py").read_text(encoding="utf-8")
    entry = (ROOT / "app" / "src" / "entry.py").read_text(encoding="utf-8")
    assert 'repo_alias_redirect = _static_routes.export("repo_alias_redirect")' in entry
    legacy = runtime.index("legacy_page_redirect(url.path, url.query)")
    alias = runtime.index("repo_alias_redirect(url.path, url.query)")
    health = runtime.index('if url.path in ("/health", "/api/mainnode"):')
    assert legacy < alias < health
    assert 'status=308, headers={"location": repo_alias}' in runtime


def test_renamed_docs_label_keeps_its_url():
    alias = next(item for item in CONTRACT["compatibilityAliases"]
                 if item["id"] == "docs-desktop-app-guide")
    assert (ROOT / "www" / "public" / alias["alias"].strip("/") / "index.html").is_file()
    onboarding = (ROOT / "www" / "public" / "docs" / "onboarding" / "index.html").read_text(
        encoding="utf-8")
    assert '<a href="/docs/qt-client">desktop app guide</a>' in onboarding


def test_wire_names_stay_stable():
    assert "pulls" in static_routes.REPO_TAB_ROUTES
    wire = next(item for item in CONTRACT["compatibilityAliases"] if item["kind"] == "api")
    assert wire["alias"] == wire["canonical"]
    entry = (ROOT / "app" / "src" / "entry.py").read_text(encoding="utf-8")
    assert '"pull": "pull",' in entry
    assert '"/api/repositories"' in entry
