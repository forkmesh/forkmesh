import json
from pathlib import Path
import shutil
import subprocess

import pytest


CRM_PACKAGE = Path(__file__).resolve().parents[1]
FEATURE = (CRM_PACKAGE / "content" / "desktop" / "CrmFeature.cpp").read_text(encoding="utf-8")
FEATURE_HEADER = (CRM_PACKAGE / "content" / "desktop" / "CrmFeature.h").read_text(
    encoding="utf-8")
WEB_VIEW = (CRM_PACKAGE / "content" / "web" / "view.html").read_text(encoding="utf-8")
WEB_SCRIPT = (CRM_PACKAGE / "content" / "web" / "page.js").read_text(encoding="utf-8")
NODE = shutil.which("node")
CALLBACK = "https://app.forkmesh.com/api/crm/-/zoho/callback"
SECRETS = ("PACKAGE_CRM_ZOHO_OAUTH_CLIENT_ID", "PACKAGE_CRM_ZOHO_OAUTH_CLIENT_SECRET",
           "PACKAGE_CRM_ZOHO_OAUTH_REDIRECT_URI")


def _desktop(name):
    return FEATURE.split(f"\n{name}(", 1)[1].split("\n}\n", 1)[0]


CONTACTS = [
    {"id": "a" * 32, "name": "Kevie", "email": "kevie@example.com", "notes":
     "from the hack firetalk\n\nDRAFT, not sent. A person sends it from the compose form:\n"
     "Subject: Following up\nHi Kevie, following up.\n\n[2026-10-01 jett] The draft in the "
     "note above is still NOT sent. DRAFT, not sent. quoted mid-line."},
    {"id": "b" * 32, "name": "Andy", "mastodon": "@andy@infosec.exchange", "notes":
     "https://mastodon.social/@forkmesh/117309847215572825\n\nDRAFT, not sent. A person posts "
     "it as a reply in the same Mastodon thread only if no reply arrived:\nHi Andy, one "
     "follow-up."},
    {"id": "c" * 32, "name": "", "mastodon": "@joanna@mastodon.social", "notes":
     "DRAFT, not sent. Reply in the Mastodon thread:\nHi Joanna."},
    {"id": "d" * 32, "name": "Sent", "email": "sent@example.com", "notes":
     "DRAFT, sent 2026-10-04 by jett. Compose form:\nSubject: Old\nAlready gone."},
    {"id": "e" * 32, "name": "Quote", "email": "quote@example.com", "notes":
     "Someone wrote DRAFT, not sent. inside a sentence."},
]
EMAILS = [
    {"contactId": "c" * 32, "provider": "mastodon", "sentAt": 1,
     "providerUrl": "https://mastodon.social/@forkmesh/1"},
    {"contactId": "c" * 32, "provider": "mastodon", "sentAt": 2,
     "providerUrl": "https://mastodon.social/@forkmesh/117272405379425610"},
]


@pytest.mark.skipif(NODE is None, reason="node is not installed")
def test_web_parser_finds_each_unsent_draft_once_with_its_channel_and_thread():
    start = WEB_SCRIPT.index("  const CRM_DRAFT_MARK")
    end = WEB_SCRIPT.index("  function renderCrmDraftsLabel()")
    script = (f"const state = {{crmView: {{contacts: {json.dumps(CONTACTS)}, "
              f"emails: {json.dumps(EMAILS)}}}}};\n" + WEB_SCRIPT[start:end]
              + "\nprocess.stdout.write(JSON.stringify(crmNoteDrafts()));\n")
    result = subprocess.run([NODE, "--no-warnings", "-e", script], capture_output=True,
                            text=True, timeout=60, env={"PATH": "/usr/local/bin:/usr/bin:/bin"})
    assert result.returncode == 0, result.stderr
    drafts = json.loads(result.stdout)
    assert [draft["contactId"][0] for draft in drafts] == ["a", "b", "c"]
    kevie, andy, joanna = drafts
    assert kevie == {"contactId": "a" * 32, "name": "Kevie", "mastodon": False,
                     "to": "kevie@example.com", "subject": "Following up",
                     "body": "Hi Kevie, following up.", "thread": ""}
    assert andy["mastodon"] and andy["subject"] == "" and andy["body"] == "Hi Andy, one follow-up."
    assert andy["thread"] == "https://mastodon.social/@forkmesh/117309847215572825"
    assert joanna["name"] == "@joanna@mastodon.social"
    assert joanna["thread"] == "https://mastodon.social/@forkmesh/117272405379425610"


def test_web_drafts_menu_counts_and_sends_and_marks_the_note():
    assert "data-crm-drafts-open data-crm-requires-install" in WEB_VIEW
    assert "<span data-crm-drafts-label>Drafts</span>" in WEB_VIEW
    assert "data-crm-drafts-dialog" in WEB_VIEW and "data-crm-drafts-body" in WEB_VIEW
    assert "renderCrmDraftsLabel(); crmMessage(\"error\", \"\"); renderCrm();" in WEB_SCRIPT
    assert "label.textContent = count ? `Drafts (${count})` : \"Drafts\";" in WEB_SCRIPT
    send = WEB_SCRIPT.split("async function sendCrmDraft(", 1)[1].split("\n  }\n", 1)[0]
    assert "openCrmEmail(contact);" in send and "state.crmView.draftContactId = draft.contactId;" in send
    assert send.index("navigator.clipboard") < send.index("window.open(draft.thread")
    assert "markCrmDraftSent(draft.contactId, true)" in send
    mail = WEB_SCRIPT.split("async function sendCrmEmail(", 1)[1].split("\n  }\n", 1)[0]
    assert "if (draft && draft === body.contactId) await markCrmDraftSent(draft, false);" in mail
    assert "state.crmView.draftContactId = \"\";" in WEB_SCRIPT.split(
        "function openCrmEmail(", 1)[1].split("\n  }\n", 1)[0]
    mark = WEB_SCRIPT.split("async function markCrmDraftSent(", 1)[1].split("\n  }\n", 1)[0]
    assert "`DRAFT, sent ${crmDay(Date.now())}${by}.`" in mark
    assert "\"PATCH\", body" in mark and "body.lastContactedAt = Date.now();" in mark


def test_web_zoho_guide_has_the_redirect_secrets_and_both_setups():
    guide = WEB_VIEW.split("<dialog data-crm-zoho-guide-dialog", 1)[1].split("</dialog>", 1)[0]
    assert guide.count(CALLBACK) == 2
    assert all(name in guide for name in SECRETS)
    assert "Server-based Applications" in guide and "Multi-DC" in guide
    assert "optional_secrets" in guide and "tools/next_vault.py sync --execute" in guide
    assert "Self Client" in guide and "ZohoMail.messages.READ,ZohoMail.messages.CREATE,ZohoMail.folders.READ,ZohoMail.accounts.READ" in guide
    assert "data-crm-zoho-guide data-crm-requires-install" in WEB_VIEW
    assert f'const CRM_ZOHO_CALLBACK = "{CALLBACK}";' in WEB_SCRIPT
    step = WEB_SCRIPT.split("function runCrmZohoStep(", 1)[1].split("\n  }\n", 1)[0]
    assert step.rstrip().endswith("else openCrmZohoGuide();")
    opener = WEB_SCRIPT.split("function openCrmZohoGuide(", 1)[1].split("\n  }\n", 1)[0]
    assert "connect.disabled = !(canManage && (zoho.oneClick || crmZohoSetupAvailable()) && !zoho.connected);" in opener
    assert "curl -s" not in guide and "Connect with code" in guide


def test_web_settings_redeem_a_self_client_code():
    assert "data-crm-zoho-code" in WEB_VIEW and "data-crm-zoho-exchange" in WEB_VIEW
    exchange = WEB_SCRIPT.split("async function exchangeCrmZohoCode(", 1)[1].split("\n  }\n", 1)[0]
    assert 'crmOrganizationPath("/zoho/exchange"), "POST"' in exchange
    assert 'await syncCrmZoho(false, "organization");' in exchange
    assert '$(`[data-crm-zoho-exchange]`)?.addEventListener("click", () => void exchangeCrmZohoCode());' in WEB_SCRIPT


def test_desktop_toolbar_has_the_zoho_guide_and_the_drafts_count():
    assert 'toolButton(QStringLiteral("book"), QStringLiteral("Zoho guide")' in FEATURE
    assert 'toolButton(QStringLiteral("pencil"), QStringLiteral("Drafts")' in FEATURE
    assert "connect(m_zohoGuideButton, &QToolButton::clicked, this, [this] { showZohoGuide(); });" in FEATURE
    assert "connect(m_noteDraftsButton, &QToolButton::clicked, this, [this] { showNoteDrafts(); });" in FEATURE
    assert "        else if (m_canManageSettings)\n            showZohoGuide();" in FEATURE
    assert "void CrmFeature::refreshContactSnapshot()\n{\n    syncNoteDrafts();" in FEATURE
    sync = _desktop("void CrmFeature::syncNoteDrafts")
    assert 'QStringLiteral("Drafts (%1)").arg(count)' in sync
    for name in ("void showZohoGuide();", "QList<NoteDraft> noteDrafts() const;",
                 "void markNoteDraftSent(const QString &contactId, bool stampContacted);",
                 "std::function<void()> sent = {});"):
        assert name in FEATURE_HEADER


def test_desktop_zoho_guide_carries_the_same_setup():
    assert f'kZohoCallbackUri[] = "{CALLBACK}";' in FEATURE
    assert 'kZohoApiConsole[] = "https://api-console.zoho.com/";' in FEATURE
    html = FEATURE.split("QString zohoGuideHtml(", 1)[1].split("\n}\n", 1)[0]
    assert "Server-based Applications" in html and "Multi-DC" in html and "Self Client" in html
    assert "tools/next_vault.py sync --execute" in html
    assert "curl -s" not in html and "oauth/v2/token" not in html
    assert 'QStringLiteral("Zoho Self Client code"), zohoCode' in FEATURE
    assert "return exchangeZohoCode(exchange);" in FEATURE
    exchange = _desktop("void CrmFeature::exchangeZohoCode")
    assert 'organizationPath(QStringLiteral("/zoho/exchange"))' in exchange
    assert "void exchangeZohoCode(const QJsonObject &body);" in FEATURE_HEADER
    lines = FEATURE.split("QString zohoSecretLines()", 1)[1].split("\n}\n", 1)[0]
    assert all(name in lines for name in SECRETS)
    guide = _desktop("void CrmFeature::showZohoGuide")
    assert "connectNow->setEnabled(m_canManageSettings && oneClick && !connected);" in guide
    assert "editSettings();" in guide and "connectZohoMail();" in guide


def test_desktop_drafts_send_through_compose_or_mastodon_and_rewrite_the_marker():
    parse = _desktop("QList<NoteDraft> CrmFeature::noteDrafts")
    assert "noteDraftAt(notes)" in parse and 'startsWith(QLatin1String("Subject:"))' in parse
    assert "m_contactToots.value(draft.contactId)" in parse
    send = _desktop("void CrmFeature::sendNoteDraft")
    assert "[this, id] { markNoteDraftSent(id, false); }" in send
    assert send.index("clipboard()->setText(draft.body)") < send.index("openUrl(QUrl(draft.thread))")
    assert "markNoteDraftSent(draft.contactId, true);" in send
    mark = _desktop("void CrmFeature::markNoteDraftSent")
    assert 'QStringLiteral("DRAFT, sent %1%2.")' in mark
    assert 'organizationPath(QStringLiteral("/contacts/") + contactId), body' in mark
    compose = _desktop("void CrmFeature::composeEmail")
    assert "if (sent)\n            sent();" in compose


def test_the_desktop_crm_shows_a_zoho_bar_with_reconnect_at_the_top():
    constructor = FEATURE.split("\nCrmFeature::CrmFeature(", 1)[1]
    assert constructor.index("m_zohoBar = new QWidget") < constructor.index("m_summary = new QLabel")
    bar = _desktop("void CrmFeature::updateZohoBar")
    assert '"steps"' in bar and '"canSend"' in bar
    assert 'QStringLiteral("Reconnect Zoho Mail")' in bar
    assert "m_canManageSettings" in bar
    step = _desktop("void CrmFeature::runZohoStep")
    assert step.index("connectZohoMail()") < step.index("editSettings()")
    assert "markZohoReadOnly();" in FEATURE
    assert "void updateZohoBar();" in FEATURE_HEADER
    assert "The CRM only reads mail" not in FEATURE
