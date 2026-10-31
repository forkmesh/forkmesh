from pathlib import Path
import os
import shutil
import subprocess

import pytest


CRM_PACKAGE = Path(__file__).resolve().parents[1]
WEB_VIEW = (CRM_PACKAGE / "content" / "web" / "view.html").read_text(encoding="utf-8")
WEB_SCRIPT = (CRM_PACKAGE / "content" / "web" / "page.js").read_text(encoding="utf-8")
NODE = shutil.which("node")


@pytest.mark.skipif(NODE is None, reason="node is not installed")
def test_the_row_pager_and_background_loader_hold_up_when_run():
    result = subprocess.run(
        [NODE, "--no-warnings", str(CRM_PACKAGE / "tests" / "pagination_checks.js")],
        capture_output=True, text=True, timeout=600,
        env={"PATH": os.environ.get("PATH", "/usr/local/bin:/usr/bin:/bin"), "TZ": "UTC"})
    assert result.returncode == 0, result.stdout[-4000:] + result.stderr[-4000:]
    assert "pagination checks passed" in result.stdout


def test_the_page_asks_for_a_first_page_and_renders_only_the_current_rows():
    load = WEB_SCRIPT.split("async function loadCrm() {", 1)[1].split("\n  }\n", 1)[0]
    assert 'query.set("contacts", String(CRM_FIRST_PAGE));' in load
    assert "void crmLoadRemainingContacts(payload.contactsNext, contactsGeneration);" in load
    render = WEB_SCRIPT.split("function renderCrmContacts()", 1)[1].split("\n  }\n", 1)[0]
    assert "list.innerHTML = page.rows.map(" in render
    assert "cards.innerHTML = page.rows.map(" in render
    assert "crmSelection.shown = contacts.map(" in render
    assert "<div data-crm-pager hidden" in WEB_VIEW
