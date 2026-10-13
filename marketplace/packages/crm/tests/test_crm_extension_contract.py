"""CRM surfaces the ForkMesh browser extension and shared sheets rely on.

The extension itself is its own Market package (``browser-extension``); its
files are tested there. What stays here is this package's half of the
contract: the link endpoint, the dashboard surfaces, and the page that sends
people to the extension's own page.
"""

import json
from pathlib import Path


CRM_PACKAGE = Path(__file__).resolve().parents[1]
REPO = CRM_PACKAGE.parents[2]
WORKER = (CRM_PACKAGE / "content" / "worker" / "crm.py").read_text(encoding="utf-8")
WEB_VIEW = (CRM_PACKAGE / "content" / "web" / "view.html").read_text(encoding="utf-8")
WEB_SCRIPT = (CRM_PACKAGE / "content" / "web" / "page.js").read_text(encoding="utf-8")
FEATURE = (CRM_PACKAGE / "content" / "desktop" / "CrmFeature.cpp").read_text(encoding="utf-8")
ENTRY = (REPO / "app" / "src" / "entry.py").read_text(encoding="utf-8")
PACKAGE_MANIFEST = json.loads((CRM_PACKAGE / "manifest.json").read_text(encoding="utf-8"))


def test_worker_link_endpoint_keeps_the_provider_message_on_the_record():
    assert '"lookup|link|copy"' in WORKER or "lookup|link|copy" in WORKER
    assert 'EMAIL_PROVIDERS = frozenset({"gmail", "zoho", "mastodon", "other"})' in WORKER
    assert "async def _link_email(" in WORKER
    assert "async def _lookup_contact(" in WORKER
    assert 'https://mail.google.com/mail/u/%d/#all/%s' in WORKER
    # Linked mail is a contact's history: the record's "last contacted"
    # moves forward with it.
    assert 'if sent_at > _integer(contact_data.get("lastContactedAt")):' in WORKER
    # The host can vouch for share names; the package asks, never assumes.
    assert "async def account_exists(self, name):" in ENTRY
    assert 'getattr(runtime, "account_exists", None)' in WORKER
    assert "crm_view_shares" in WORKER
    assert '"DROP TABLE IF EXISTS crm_view_shares"' in WORKER
    assert 'DELETE FROM crm_view_shares WHERE org_bi=?' in WORKER


def test_dashboard_has_the_import_tab_shared_tabs_and_email_chips():
    assert "data-crm-tab-import" in WEB_VIEW
    assert ">Import contacts</button>" in WEB_VIEW.replace("\n", "")
    assert '$(`[data-crm-tab-import]`)?.addEventListener("click", () => crmPickCsv(""))' in WEB_SCRIPT
    assert "data-crm-sheet-users" in WEB_VIEW
    assert "data-crm-shared-banner" in WEB_VIEW
    assert "data-crm-shared-copy" in WEB_VIEW
    assert "data-crm-tab-shared" in WEB_SCRIPT
    assert "/api/crm/-/shared/" in WEB_SCRIPT
    assert 'crmOrganizationPath("/views/copy")' in WEB_SCRIPT
    assert "state.crmView.sharedViews = payload.sharedViews || []" in WEB_SCRIPT
    # A shared sheet is read-only: no inline editing, no row actions, and no
    # column layout writes.
    assert "if (crmActiveShared()) return;" in WEB_SCRIPT
    assert "column.readonly || crmActiveShared()" in WEB_SCRIPT
    # Linked emails sit on the record and reopen the message.
    assert "function crmEmailChips(contact)" in WEB_SCRIPT
    assert "data-crm-email-open" in WEB_SCRIPT
    assert 'target="_blank" rel="noopener noreferrer"' in WEB_SCRIPT
    assert "data-crm-link-provider" in WEB_VIEW
    assert "data-crm-link-direction" in WEB_VIEW
    permissions = {item["id"] for item in PACKAGE_MANIFEST["permissions"]}
    assert {"browser.mail.link", "organization.crm.share"} <= permissions
    # The extension moved to its own package; this one ships none.
    assert "browser-extension" not in PACKAGE_MANIFEST["entrypoints"]
    assert not (CRM_PACKAGE / "content" / "extension").exists()


def test_dashboard_creates_a_pipeline_as_a_flow_and_its_own_tab():
    assert "data-crm-pipeline-new" in WEB_VIEW
    assert "data-crm-pipeline-dialog" in WEB_VIEW
    assert 'crmOrganizationPath("/views"), "POST", { name, flow: id }' in WEB_SCRIPT
    assert "flows: [...flows, { id, name, icon, stages" in WEB_SCRIPT
    assert "function crmContactInView(contact, view)" in WEB_SCRIPT
    assert "if (crmContactInView(contact, view))" in WEB_SCRIPT


def test_the_crm_sends_people_to_the_extension_package_and_pins_no_build():
    assert 'href="https://mail.google.com/mail/"' in WEB_VIEW
    assert "data-crm-open-zoho" in WEB_VIEW
    assert '"com.au": "mail.zoho.com.au"' in WEB_SCRIPT
    # The install link is the extension package's page, not a zip of ours: a
    # new extension release must not need a CRM release.
    assert 'const CRM_EXTENSION_PAGE = "/dashboard/extension";' in WEB_SCRIPT
    assert 'const CRM_EXTENSION_MINIMUM = "2.0.0";' in WEB_SCRIPT
    assert "link.href = CRM_EXTENSION_PAGE;" in WEB_SCRIPT
    assert "crmVersionBehind(installed, CRM_EXTENSION_MINIMUM)" in WEB_SCRIPT
    for stale in ("CRM_EXTENSION_VERSION", "CRM_EXTENSION_ZIP", "dashboard-plugins/crm/"):
        assert stale not in WEB_SCRIPT and stale not in FEATURE
    assert "data-crm-extension-install" in WEB_VIEW and " download " not in WEB_VIEW.split(
        "data-crm-extension-install", 1)[1][:40]
    assert 'href="/dashboard/extension"' in WEB_VIEW
    # Websites cannot open chrome://extensions; the extension's bridge does,
    # and it still answers the message this page has always sent.
    assert "data-crm-extensions-open" in WEB_VIEW
    assert 'source: "forkmesh-crm-page", type: "open-extensions"' in WEB_SCRIPT
    assert 'kExtensionPage[] = "https://app.forkmesh.com/dashboard/extension"' in FEATURE
    assert "kExtensionVersion" not in FEATURE

