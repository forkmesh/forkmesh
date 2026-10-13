#!/usr/bin/env python3
"""Category comparison claim governance (issue #509).

``conformance/category-comparisons/v1/ledger.json`` is the only place the
public comparison page gets a claim from. These tests keep every ForkMesh
claim tied to repository evidence, every claim about another category tied to
a dated primary source, the page on a fixed review cadence, and the next-step
links honest and measurable.
"""

import copy
import datetime as dt
import importlib.util
import json
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "category_comparisons.py"
LEDGER = ROOT / "conformance" / "category-comparisons" / "v1" / "ledger.json"
PAGE = ROOT / "www" / "docs" / "category-comparisons.md"


def _tool():
    spec = importlib.util.spec_from_file_location("category_comparisons", TOOL)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _ledger():
    return json.loads(LEDGER.read_text(encoding="utf-8"))


def _reviewed(ledger):
    return dt.date.fromisoformat(ledger["reviewedOn"])


def _problems(tool, ledger, today=None):
    """Problems for a modified ledger, ignoring the stale-page check it causes."""
    report = tool.build_report(ledger, today=today or _reviewed(ledger))
    return [problem for problem in report["problems"] if "generated block" not in problem]


def _claim(ledger, claim_id):
    return next(claim for claim in ledger["matrix"] if claim["id"] == claim_id)


def test_ledger_passes_the_check_today():
    tool = _tool()
    report = tool.build_report(tool.load_ledger())
    assert report["ok"], report["problems"]
    assert report["daysUntilReviewDue"] >= 0


def test_matrix_separates_protocol_and_hosted_columns_and_covers_every_dimension():
    ledger = _ledger()
    scopes = {subject["id"]: subject["scope"] for subject in ledger["subjects"]}
    assert scopes["forkmesh-protocol"] == "protocol"
    assert scopes["forkmesh-hosted"] == "hosted-service"
    assert {
        "centralized-hosting", "self-hosted-forge", "decentralized-collaboration",
        "replicated-storage", "basic-git-server",
    } <= set(scopes)
    assert {dimension["id"] for dimension in ledger["dimensions"]} == {
        "ownership", "availability", "workflow", "operations", "privacy",
        "portability", "cost", "maturity", "failure-modes",
    }
    cells = {(claim["subject"], claim["dimension"]) for claim in ledger["matrix"]}
    assert len(cells) == len(ledger["matrix"]) == len(scopes) * len(ledger["dimensions"])


def test_forkmesh_cells_always_cite_repository_evidence():
    tool = _tool()
    ledger = _ledger()
    claim = _claim(ledger, "fm-hosted-cost")
    claim["basis"] = "no-claim"
    claim.pop("evidence")
    claim.pop("verifiedOn")
    assert any("ForkMesh claim must cite evidence" in p for p in _problems(tool, ledger))

    ledger = _ledger()
    _claim(ledger, "fm-protocol-cost")["evidence"] = [{"kind": "source", "id": "git-bundle"}]
    assert any("needs a test, specification, or product evidence" in p for p in _problems(tool, ledger))


def test_vanished_tests_files_and_headings_fail():
    tool = _tool()
    ledger = _ledger()
    evidence = _claim(ledger, "fm-protocol-workflow")["evidence"]
    evidence.append({"kind": "test", "ref": "app/tests/test_git_http.py::test_that_was_renamed"})
    evidence.append({"kind": "specification", "path": "www/docs/removed.md"})
    evidence.append({"kind": "product-evidence", "path": "www/docs/git-compatibility.md", "anchor": "no-such-heading"})
    problems = _problems(tool, ledger)
    assert any("test_that_was_renamed" in p and "does not exist" in p for p in problems)
    assert any("removed.md" in p for p in problems)
    assert any("#no-such-heading" in p for p in problems)


def test_category_claims_need_primary_sources_that_name_the_vendor():
    tool = _tool()
    ledger = _ledger()
    _claim(ledger, "central-portability")["evidence"] = [
        {"kind": "specification", "path": "www/docs/repository-imports.md"}]
    assert any("only dated primary sources" in p for p in _problems(tool, ledger))

    ledger = _ledger()
    _claim(ledger, "forge-operations")["text"] += " Forgejo behaves the same way."
    assert any("names Forgejo without a Forgejo source" in p for p in _problems(tool, ledger))

    ledger = _ledger()
    _claim(ledger, "central-cost")["text"] = "GitHub is expensive."
    assert any("may not name GitHub" in p for p in _problems(tool, ledger))

    ledger = _ledger()
    ledger["sources"][0]["url"] = "http://example.invalid/"
    ledger["sources"][1]["quote"] = ""
    problems = _problems(tool, ledger)
    assert any("url must be https" in p for p in problems)
    assert any("missing quote" in p for p in problems)


def test_claims_go_stale_on_the_review_cadence():
    tool = _tool()
    ledger = _ledger()
    due = _reviewed(ledger) + dt.timedelta(days=ledger["reviewCadenceDays"])
    assert not _problems(tool, ledger, today=due)
    late = _problems(tool, ledger, today=due + dt.timedelta(days=1))
    assert any(p.startswith("page review: stale") for p in late)
    assert any(p.startswith("source github-terms: stale") for p in late)
    assert any(p.startswith("claim fm-protocol-ownership: stale") for p in late)

    ledger = _ledger()
    _claim(ledger, "fm-hosted-cost")["verifiedOn"] = (_reviewed(ledger) + dt.timedelta(days=1)).isoformat()
    assert any("is in the future" in p for p in _problems(tool, ledger))


def test_overclaiming_copy_fails_in_claims_and_next_steps():
    tool = _tool()
    ledger = _ledger()
    _claim(ledger, "fm-hosted-availability")["text"] = "Repositories are always available."
    ledger["nextSteps"][0]["boundary"] = "Unlimited repositories, guaranteed."
    problems = _problems(tool, ledger)
    assert any("fm-hosted-availability: overclaiming phrase" in p for p in problems)
    assert any("next step trial: overclaiming phrase" in p for p in problems)


def test_every_category_has_not_for_you_guidance_and_sections_are_required():
    tool = _tool()
    ledger = _ledger()
    categories = {s["id"] for s in ledger["subjects"] if s["kind"] == "category"}
    assert categories <= {item for entry in ledger["notForYou"] for item in entry["instead"]}
    ledger["notForYou"] = [e for e in ledger["notForYou"] if "replicated-storage" not in e["instead"]]
    ledger["migrationCost"] = []
    problems = _problems(tool, ledger)
    assert "category replicated-storage: no not-for-you entry points to it" in problems
    assert "Migration cost: section is empty" in problems


def test_next_steps_cover_trial_self_host_and_community_with_allowlisted_attribution():
    tool = _tool()
    ledger = _ledger()
    kinds = {step["kind"]: step for step in ledger["nextSteps"]}
    assert set(kinds) == {"trial", "self-host", "community"}
    assert kinds["trial"]["url"].startswith("https://app.forkmesh.com/signup?")
    assert "no SLO" in kinds["trial"]["boundary"]
    assert kinds["self-host"]["measurementGap"]

    ledger["nextSteps"][0]["url"] = "https://app.forkmesh.com/signup?utm_source=docs&utm_medium=banner&utm_campaign=category_comparison_509"
    ledger["nextSteps"] = [step for step in ledger["nextSteps"] if step["kind"] != "community"]
    problems = _problems(tool, ledger)
    assert any("utm_medium=['banner'] is not on ATTRIBUTION_ALLOWED_MEDIUMS" in p for p in problems)
    assert "next steps: missing a community path" in problems


def test_attribution_defaults_match_the_deployed_allowlists_for_comparison_links():
    tool = _tool()
    sys.path.insert(0, str(ROOT / "app" / "src"))
    import attribution

    allowlists = tool.attribution_allowlists()
    assert set(attribution.DEFAULT_ALLOWED_SOURCES) == allowlists["utm_source"]
    assert set(attribution.DEFAULT_ALLOWED_MEDIUMS) == allowlists["utm_medium"]
    assert set(attribution.DEFAULT_ALLOWED_CAMPAIGNS) == allowlists["utm_campaign"]
    assert {"docs"} <= allowlists["utm_source"]
    assert {"comparison"} <= allowlists["utm_medium"]
    assert {"category_comparison_509"} <= allowlists["utm_campaign"]


def test_corrections_need_a_summary_and_claim_ids_and_are_published(tmp_path):
    tool = _tool()
    ledger = _ledger()
    ledger["corrections"] = [{"date": ledger["reviewedOn"], "claims": [], "summary": ""}]
    assert any("needs a summary and the corrected claim ids" in p for p in _problems(tool, ledger))
    ledger["corrections"] = [{"date": ledger["reviewedOn"], "claims": ["fm-hosted-cost"],
                              "summary": "Pro price restated."}]
    assert "Pro price restated. (claims: fm-hosted-cost)" in tool.render_block(ledger)


def test_published_page_block_is_rendered_from_the_ledger():
    tool = _tool()
    ledger = tool.load_ledger()
    text = PAGE.read_text(encoding="utf-8")
    block = tool.extract_block(text)
    assert block == tool.render_block(ledger)
    for marker in ("## Not for you", "## Known limitations", "## Migration cost",
                   "## Hosted dependencies", "## Next steps", "## Primary sources",
                   "## Corrections", f"Reviewed **{ledger['reviewedOn']}**"):
        assert marker in block, marker
    for source in ledger["sources"]:
        assert source["url"] in block

    stale = copy.deepcopy(ledger)
    stale["matrix"][0]["text"] += " Edited."
    assert tool.extract_block(text) != tool.render_block(stale)


def test_cli_report_prints_freshness_measures_and_check_exits_cleanly(tmp_path):
    output = tmp_path / "report.json"
    result = subprocess.run(
        [sys.executable, str(TOOL), "report", "--output", str(output)],
        cwd=ROOT, capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stderr
    report = json.loads(output.read_text(encoding="utf-8"))
    for key in ("nextReviewDue", "oldestClaimVerifiedOn", "matrixCellsByBasis",
                "corrections", "measuredNextSteps", "unmeasuredNextSteps"):
        assert key in report, key

    late = _reviewed(_ledger()) + dt.timedelta(days=_ledger()["reviewCadenceDays"] + 1)
    result = subprocess.run(
        [sys.executable, str(TOOL), "check", "--today", late.isoformat()],
        cwd=ROOT, capture_output=True, text=True, check=False)
    assert result.returncode == 1
    assert "stale" in result.stdout


def test_page_is_published_in_the_docs_library_readme_quality_gates_and_workflow():
    sys.path.insert(0, str(ROOT / "app" / "tools"))
    import docs_site

    assert "category-comparisons.md" in docs_site.GUIDE_ORDER
    assert "(category-comparisons.md)" in (ROOT / "www" / "docs" / "README.md").read_text(encoding="utf-8")
    assert "(www/docs/category-comparisons.md)" in (ROOT / "README.md").read_text(encoding="utf-8")
    gates = json.loads((ROOT / "www" / "docs" / "engineering" / "quality-gates.json").read_text(encoding="utf-8"))
    gate = next(g for g in gates["gates"] if g["id"] == "category-comparison-claims")
    assert "app/tests/test_category_comparisons.py" in gate["command"]
    for profile in ("core", "release"):
        assert "category-comparison-claims" in gates["profiles"][profile]
    workflow = (ROOT / ".github" / "workflows" / "category-comparisons.yml").read_text(encoding="utf-8")
    for needle in ("ubuntu-latest, macos-latest, windows-latest", "schedule:",
                   "tools/category_comparisons.py report", "app/tests/test_category_comparisons.py"):
        assert needle in workflow, needle
