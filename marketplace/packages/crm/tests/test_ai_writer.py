import json
import re
from pathlib import Path

from test_crm import Runtime, call, crm


CRM_PACKAGE = Path(__file__).resolve().parents[1]
FEATURE = (CRM_PACKAGE / "content" / "desktop" / "CrmFeature.cpp").read_text(encoding="utf-8")
FEATURE_HEADER = (CRM_PACKAGE / "content" / "desktop" / "CrmFeature.h").read_text(
    encoding="utf-8")
WEB_VIEW = (CRM_PACKAGE / "content" / "web" / "view.html").read_text(encoding="utf-8")
WEB_SCRIPT = (CRM_PACKAGE / "content" / "web" / "page.js").read_text(encoding="utf-8")


def _web(name):
    return WEB_SCRIPT.split(f"function {name}(", 1)[1].split("\n  }\n", 1)[0]


def _desktop(name):
    return FEATURE.split(f"\n{name}(", 1)[1].split("\n}\n", 1)[0]


def test_both_next_cards_and_the_detail_card_open_the_writer_on_the_web():
    card = _web("crmNextCardHtml")
    assert 'data-crm-next-ai="${id}" data-crm-next-ai-mode="${mode}"' in card
    assert ">Write with AI</button>" in card
    click = _web("handleCrmNextClick")
    assert 'openCrmAi(ai.dataset.crmNextAi, mode, next.contact?.id === ai.dataset.crmNextAi ? next.reason : "")' in click
    assert 'data-crm-contact-ai="${id}"' in _web("renderCrmContactDetail")
    assert "openCrmAi(ai.dataset.crmContactAi)" in WEB_SCRIPT
    for hook in ("data-crm-ai-dialog", "data-crm-ai-formats", "data-crm-ai-brief",
                 "data-crm-ai-brief-save", "data-crm-ai-prompt", "data-crm-ai-status",
                 "data-crm-ai-copy", 'data-crm-ai-open="claude"', 'data-crm-ai-open="chatgpt"'):
        assert hook in WEB_VIEW
    for binding in ('$(`[data-crm-ai-dialog]`)?.addEventListener("click", handleCrmAiClick)',
                    '$(`[data-crm-ai-dialog]`)?.addEventListener("change", handleCrmAiChange)'):
        assert binding in WEB_SCRIPT


def test_the_web_opens_the_assistant_inside_the_click_and_copies_the_prompt():
    send = _web("sendCrmAi")
    assert send.index('window.open(url, "_blank", "noopener,noreferrer")') < send.index(
        "navigator.clipboard?.writeText(prompt)")
    assert "await" not in send
    assert 'chatgpt: { label: "ChatGPT", url: "https://chatgpt.com/" }' in WEB_SCRIPT
    assert 'claude: { label: "Claude", url: "https://claude.ai/new" }' in WEB_SCRIPT
    assert "const CRM_AI_URL_LIMIT = 14000;" in WEB_SCRIPT


def test_the_web_prompt_reads_message_bodies_and_live_posts():
    messages = _web("crmAiMessages")
    assert "crmOrganizationPath(`/emails/${encodeURIComponent(stub.id)}`)" in messages
    assert "catch { return stub; }" in messages
    assert "CRM_UPDATE_READERS[account.service](account)" in _web("crmAiUpdates")
    assert "CRM_UPDATE_READERS[account.service](account)" in _web("loadCrmContactUpdates")
    assert "const CRM_AI_MESSAGE_LIMIT = 6;" in WEB_SCRIPT


def test_desktop_cards_and_detail_panel_open_the_writer():
    card = _desktop("QWidget *CrmFeature::nextContactCard")
    assert 'addButton(QStringLiteral("Write with AI")' in card
    assert 'showAiWriter(contactId, reach ? QStringLiteral("reach") : QStringLiteral("follow"), reason)' in card
    detail = _desktop("void CrmFeature::showContactDetail")
    assert 'new QPushButton(QStringLiteral("Write with AI"))' in detail
    assert "showAiWriter(contactId); });" in detail
    assert "void showAiWriter(const QString &contactId" in FEATURE_HEADER
    writer = _desktop("void CrmFeature::showAiWriter")
    assert 'organizationPath(QStringLiteral("/emails/") + id)' in writer
    assert "readAccountUpdates(source, dialog," in writer
    assert "m_accountReads.constFind(source)" in writer
    assert "QDesktopServices::openUrl(" in writer and "clipboard->setText(text)" in writer
    assert 'QStringLiteral("messageBrief")' in writer
    assert "saveBrief->setVisible(m_canManageSettings)" in writer
    assert 'constexpr char kAiChatGptUrl[] = "https://chatgpt.com/";' in FEATURE
    assert 'constexpr char kAiClaudeUrl[] = "https://claude.ai/new";' in FEATURE
    assert "constexpr int kAiUrlLimit = 14000;" in FEATURE


def test_the_forkmesh_brief_is_the_same_on_the_web_and_the_desktop():
    web = WEB_SCRIPT.split("const CRM_FORKMESH_BRIEF = [", 1)[1].split("].join", 1)[0]
    desktop = FEATURE.split("const QStringList kForkMeshBrief{", 1)[1].split("\n};", 1)[0]
    web_lines = [json.loads(line) for line in re.findall(r'^\s*(".*"),$', web, re.M)]
    desktop_lines = [json.loads(line) for line in
                     re.findall(r'^\s*QStringLiteral\((".*")\),$', desktop, re.M)]
    assert len(web_lines) >= 10
    assert web_lines == desktop_lines
    assert all(line.isascii() for line in web_lines)
    for link in ("https://forkmesh.com/changelog", "https://forkmesh.com/blog",
                 "https://app.forkmesh.com", "https://world.forkmesh.com"):
        assert any(link in line for line in web_lines)


def test_both_prompts_carry_the_same_sections():
    web = _web("crmAiPrompt")
    desktop = _desktop("QString CrmFeature::aiPrompt")
    for section in ("## About us: ", "## Their recent public posts",
                    "## Our conversation so far (oldest first)", "## Why now",
                    "Not read yet: open their profiles linked above.",
                    "Nothing on record yet: this is the first message."):
        assert section in web
        assert section in desktop
    rules = WEB_SCRIPT.split("const CRM_AI_MESSAGE_RULES = [", 1)[1].split("\n  function crmAiPrompt(", 1)[0]
    for line in ("Reply with the finished message, ready to paste, then two",
                 "Reply with the finished application, ready to paste, then a",
                 "never invent it: list it at the end instead."):
        assert line in rules
        assert line in " ".join(re.findall(r'"([^"]*)"', desktop))
    assert "...(format === \"application\" ? CRM_AI_APPLICATION_RULES : CRM_AI_MESSAGE_RULES)" in web
    assert 'if (draft.format == QLatin1String("application")) {' in desktop


def test_a_grants_pipeline_contact_opens_the_writer_on_a_grant_application():
    assert 'if (crmFlow(contact.flow)?.icon === "grants") return "application";' in _web(
        "crmAiDefaultFormat")
    assert '{ id: "application", label: "Grant application" }' in WEB_SCRIPT
    writer = _desktop("void CrmFeature::showAiWriter")
    assert 'flowIconId(text(contact, "flow")) == QLatin1String("grants")' in writer
    assert 'format->addItem(QStringLiteral("Grant application"), QStringLiteral("application"));' in writer


def test_the_message_brief_is_an_organization_setting():
    settings = crm._settings_data({"messageBrief": "  Line one\n\nLine two  "})
    assert settings["messageBrief"] == "Line one\n\nLine two"
    assert crm._settings_data({"fromName": "Jett"}, settings)["messageBrief"] == (
        "Line one\n\nLine two")
    assert crm.MESSAGE_BRIEF_LIMIT == 8000
    assert len(crm._settings_data({"messageBrief": "x" * 9000})["messageBrief"]) == 8000

    runtime = Runtime()
    assert call(runtime.use("GET"), "/api/crm/acme")["data"]["settings"]["messageBrief"] == ""
    saved = call(runtime.use("PUT", body={"messageBrief": "Acme makes anvils.\nhttps://acme.test"}),
                 "/api/crm/acme/settings")
    assert saved["data"]["settings"]["messageBrief"] == "Acme makes anvils.\nhttps://acme.test"
    flows = saved["data"]["settings"]["flows"]
    call(runtime.use("PUT", body={"fromName": "Alice"}), "/api/crm/acme/settings")
    after = call(runtime.use("GET"), "/api/crm/acme")["data"]["settings"]
    assert after["messageBrief"] == "Acme makes anvils.\nhttps://acme.test"
    assert after["flows"] == flows
    refused = call(runtime.use("PUT", actor="charlie", body={"messageBrief": "hijacked"}),
                   "/api/crm/acme/settings")
    assert refused["status"] == 403
    assert call(runtime.use("GET"), "/api/crm/acme")["data"]["settings"]["messageBrief"] == (
        "Acme makes anvils.\nhttps://acme.test")
