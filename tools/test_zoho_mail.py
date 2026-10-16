#!/usr/bin/env python3
import importlib.util
import json
import os
import smtplib
import stat
import sys
import tempfile
from pathlib import Path

import pytest

_SANDBOX = tempfile.mkdtemp(prefix="fm-zoho-mail-")
for _name in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR", "FORKMESH_PROJECT",
              "FORKMESH_PROJECT_DIR", "FORKMESH_DATA_DIR", "FORKMESH_CONFIG_DIR",
              "FORKMESH_MCP_TOKEN", "FORKMESH_MCP_CONNECTOR",
              "FORKMESH_AGENT_INTAKE", "FORKMESH_AGENT_INTAKE_TOKEN",
              "ZOHO_MAIL_CONFIG", "ZOHO_MAIL_ADDRESS", "ZOHO_MAIL_APP_PASSWORD",
              "ZOHO_MAIL_HOST", "ZOHO_MAIL_PORT", "ZOHO_MAIL_FROM_NAME",
              "ZOHO_MAIL_CRM_ORGANIZATION", "ZOHO_MAIL_CRM_LIST"):
    os.environ.pop(_name, None)
os.environ["HOME"] = _SANDBOX
os.environ["XDG_DATA_HOME"] = os.path.join(_SANDBOX, "data")
os.environ["XDG_CONFIG_HOME"] = os.path.join(_SANDBOX, "config")
os.environ["XDG_CACHE_HOME"] = os.path.join(_SANDBOX, "cache")

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import zoho_mail  # noqa: E402

_spec = importlib.util.spec_from_file_location(
    "fm_mcp_zoho", HERE / "forkmesh_mcp_server.py")
srv = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(srv)
sys.modules["forkmesh_mcp_server"] = srv


class FakeSMTP:
    instances = []
    reject_login = False
    refuse = {}

    def __init__(self, host, port, timeout=None, context=None):
        self.host, self.port, self.timeout = host, port, timeout
        self.logins, self.sent, self.closed = [], [], False
        FakeSMTP.instances.append(self)

    def login(self, user, password):
        if FakeSMTP.reject_login:
            raise smtplib.SMTPAuthenticationError(535, b"Authentication Failed")
        self.logins.append((user, password))

    def send_message(self, message, from_addr=None, to_addrs=None):
        self.sent.append((message, from_addr, list(to_addrs)))
        return dict(FakeSMTP.refuse)

    def quit(self):
        self.closed = True

    def close(self):
        self.closed = True


class FakeCRM:
    def __init__(self, organizations=("acme",)):
        self.organizations = list(organizations)
        self.views = [{"id": "a" * 32, "name": "Investors"}]
        self.contacts = {}
        self.requests = []
        self.fail = None

    def __call__(self, method, path, body=None):
        self.requests.append((method, path, body))
        if self.fail:
            raise ValueError(self.fail)
        parts = path.split("/")[3:]
        if not parts:
            return {"organizations": [{"name": name} for name in self.organizations]}
        if parts[1:] == ["views"] and method == "GET":
            return {"views": list(self.views)}
        if parts[1:] == ["views"] and method == "POST":
            view = {"id": f"{len(self.views):032x}", "name": body["name"]}
            self.views.append(view)
            return {"view": view}
        if parts[1:] == ["emails", "link"]:
            found = next((c for c in self.contacts.values() if c["email"] == body["contactEmail"]), None)
            created = found is None
            if created:
                found = {"id": f"{len(self.contacts) + 1:032x}", "email": body["contactEmail"],
                         "name": body["contactName"], "views": [], "emails": []}
                self.contacts[found["id"]] = found
            found["emails"].append(body)
            return {"contact": dict(found), "contactCreated": created}
        if parts[1] == "contacts" and method == "PATCH":
            contact = self.contacts[parts[2]]
            contact["views"] = contact["views"] + [v for v in body["views"] if v not in contact["views"]]
            return {"contact": dict(contact)}
        raise AssertionError((method, path))


@pytest.fixture
def crm(monkeypatch):
    fake = FakeCRM()
    monkeypatch.setattr(srv, "crm_request", fake)
    return fake


@pytest.fixture(autouse=True)
def _fake_zoho(monkeypatch, tmp_path, crm):
    FakeSMTP.instances, FakeSMTP.reject_login, FakeSMTP.refuse = [], False, {}
    monkeypatch.setattr(zoho_mail, "SMTP", FakeSMTP)
    monkeypatch.setenv("ZOHO_MAIL_CONFIG", str(tmp_path / "zoho-mail.json"))
    monkeypatch.setattr(zoho_mail, "sent_log_path", lambda environ=None: tmp_path / "sent.jsonl")
    monkeypatch.setattr(srv, "connector_record", lambda: {})
    zoho_mail.save_config({"address": "sender@example.com", "appPassword": "app-secret",
                           "host": "smtppro.zoho.com", "port": 465, "fromName": "Example Team"})
    return tmp_path


def test_send_signs_in_and_delivers_to_every_recipient(_fake_zoho):
    result = zoho_mail.send("Ada <ada@example.org>, bob@example.org", "Hello",
                            "Line one", cc=["carol@example.org"], bcc="dan@example.org",
                            reply_to="replies@example.com")
    client = FakeSMTP.instances[-1]
    message, sender, recipients = client.sent[0]
    assert (client.host, client.port) == ("smtppro.zoho.com", 465)
    assert client.logins == [("sender@example.com", "app-secret")] and client.closed
    assert sender == "sender@example.com"
    assert recipients == ["ada@example.org", "bob@example.org", "carol@example.org", "dan@example.org"]
    assert message["From"] == "Example Team <sender@example.com>"
    assert message["To"] == "Ada <ada@example.org>, bob@example.org"
    assert message["Cc"] == "carol@example.org" and message["Bcc"] is None
    assert message["Reply-To"] == "replies@example.com"
    assert message.get_content() == "Line one\n"
    assert result["sent"] and result["messageId"] == message["Message-ID"]
    assert message["Message-ID"].endswith("@example.com>")
    logged = json.loads((_fake_zoho / "sent.jsonl").read_text().strip())
    assert logged["to"] == ["ada@example.org", "bob@example.org"] and "body" not in logged
    assert zoho_mail.summary(result).startswith("Sent to ada@example.org, bob@example.org")


def test_html_body_carries_a_plain_copy():
    zoho_mail.send("ada@example.org", "Hi", "<p>Hello &amp; welcome</p>", html=True)
    message = FakeSMTP.instances[-1].sent[0][0]
    assert message.get_body(("plain",)).get_content() == "Hello & welcome\n"
    assert "<p>Hello" in message.get_body(("html",)).get_content()


@pytest.mark.parametrize("kwargs, needle", [
    ({"to": "", "subject": "s", "body": "b"}, "recipient"),
    ({"to": "not-an-address", "subject": "s", "body": "b"}, "not an email address"),
    ({"to": "a@example.org\r\nBcc: x@evil.example", "subject": "s", "body": "b"}, "single line"),
    ({"to": "a@example.org", "subject": "s\nBcc: x@evil.example", "body": "b"}, "single line"),
    ({"to": "a@example.org", "subject": "", "body": "b"}, "subject"),
    ({"to": "a@example.org", "subject": "s", "body": "  "}, "body"),
    ({"to": [f"u{i}@example.org" for i in range(51)], "subject": "s", "body": "b"}, "at most"),
])
def test_bad_input_is_refused_before_connecting(kwargs, needle):
    with pytest.raises(zoho_mail.ZohoMailError, match=needle):
        zoho_mail.send(**kwargs)
    assert FakeSMTP.instances == []


def test_missing_setup_says_how_to_set_up(monkeypatch, tmp_path):
    monkeypatch.setenv("ZOHO_MAIL_CONFIG", str(tmp_path / "absent.json"))
    with pytest.raises(zoho_mail.ZohoMailError, match="zoho_mail.py setup"):
        zoho_mail.send("ada@example.org", "s", "b")


def test_environment_overrides_the_saved_file(monkeypatch):
    monkeypatch.setenv("ZOHO_MAIL_ADDRESS", "other@example.net")
    monkeypatch.setenv("ZOHO_MAIL_APP_PASSWORD", "env-secret")
    monkeypatch.setenv("ZOHO_MAIL_HOST", "smtp.zoho.eu")
    zoho_mail.send("ada@example.org", "s", "b")
    client = FakeSMTP.instances[-1]
    assert client.host == "smtp.zoho.eu" and client.logins == [("other@example.net", "env-secret")]


def test_rejected_login_names_the_account():
    FakeSMTP.reject_login = True
    with pytest.raises(zoho_mail.ZohoMailError, match="rejected the login for sender@example.com"):
        zoho_mail.send("ada@example.org", "s", "b")
    assert FakeSMTP.instances[-1].sent == []


def test_saved_config_is_private(_fake_zoho):
    mode = stat.S_IMODE(os.stat(_fake_zoho / "zoho-mail.json").st_mode)
    assert mode == 0o600


def test_hosts_follow_region_and_account_kind():
    assert zoho_mail.smtp_host("eu") == "smtppro.zoho.eu"
    assert zoho_mail.smtp_host("com", organization=False) == "smtp.zoho.com"
    assert zoho_mail.smtp_host("ca") == "smtppro.zohocloud.ca"
    with pytest.raises(zoho_mail.ZohoMailError):
        zoho_mail.smtp_host("mars")


def test_cli_send_prints_the_confirmation(capsys):
    code = zoho_mail.main(["send", "--to", "ada@example.org,bob@example.org", "--to", "carol@example.org",
                           "--subject", "Status", "--body", "All good"])
    assert code == 0
    assert FakeSMTP.instances[-1].sent[0][2] == ["ada@example.org", "bob@example.org", "carol@example.org"]
    assert capsys.readouterr().out.startswith("Sent to ada@example.org, bob@example.org, carol@example.org: Status (<")


def test_cli_setup_saves_without_prompting(monkeypatch, tmp_path, capsys):
    target = tmp_path / "fresh.json"
    monkeypatch.setenv("ZOHO_MAIL_CONFIG", str(target))
    monkeypatch.setattr(sys, "stdin", __import__("io").StringIO("piped-secret\n"))
    code = zoho_mail.main(["setup", "--address", "me@example.com", "--region", "eu",
                           "--password-stdin", "--from-name", "Me"])
    saved = json.loads(target.read_text())
    assert code == 0 and saved == {"address": "me@example.com", "appPassword": "piped-secret",
                                   "host": "smtppro.zoho.eu", "port": 465, "fromName": "Me"}
    assert FakeSMTP.instances[-1].logins == [("me@example.com", "piped-secret")]
    assert "piped-secret" not in capsys.readouterr().out


def call_tool(name, arguments):
    reply = srv.handle_request({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                                "params": {"name": name, "arguments": arguments}})
    content = reply["result"]
    return content.get("isError", False), content["content"][0]["text"]


def test_mcp_send_email_sends_and_reports_sent():
    failed, text = call_tool("send_email", {"to": ["ada@example.org"], "subject": "From an agent",
                                            "body": "Done.", "cc": "bob@example.org"})
    assert not failed and text.startswith("Sent to ada@example.org, bob@example.org: From an agent")
    assert FakeSMTP.instances[-1].sent[0][2] == ["ada@example.org", "bob@example.org"]


def test_mcp_send_email_reports_errors_without_sending():
    failed, text = call_tool("send_email", {"to": "ada@example.org", "subject": "x", "body": ""})
    assert failed and "body is required" in text and FakeSMTP.instances == []


def test_mcp_send_email_is_a_write_tool(monkeypatch):
    listed = {tool["name"]: tool for tool in srv.tools_list_payload()["tools"]}
    assert listed["send_email"]["annotations"]["readOnlyHint"] is False
    assert listed["send_email"]["annotations"]["openWorldHint"] is True
    monkeypatch.setattr(srv, "connector_record", lambda: {"token": "expected"})
    monkeypatch.setenv("FORKMESH_MCP_TOKEN", "wrong")
    failed, _ = call_tool("send_email", {"to": "ada@example.org", "subject": "x", "body": "y"})
    assert failed and FakeSMTP.instances == []


def test_mcp_send_email_logs_outside_recipients_in_the_initiatives_list(crm):
    failed, text = call_tool("send_email", {
        "to": "Ada Example <ada@example.org>", "cc": "colleague@example.com",
        "subject": "Collaboration", "body": "Shall we work together?"})
    assert not failed
    assert "CRM: logged for Ada Example (new contact) in the Initiatives list (acme)" in text
    initiatives = next(v for v in crm.views if v["name"] == "Initiatives")
    contact = next(iter(crm.contacts.values()))
    assert len(crm.contacts) == 1 and contact["email"] == "ada@example.org"
    assert contact["views"] == [initiatives["id"]]
    link = contact["emails"][0]
    message_id = FakeSMTP.instances[-1].sent[0][0]["Message-ID"]
    assert link["provider"] == "zoho" and link["direction"] == "outbound"
    assert link["mailbox"] == "sender@example.com" and link["fromName"] == "Example Team"
    assert link["messageId"] == f"{message_id} ada@example.org"
    assert link["subject"] == "Collaboration" and link["text"] == "Shall we work together?"
    record = json.loads(text.split("\n", 2)[2])["crm"]
    assert record["listCreated"] is True and record["organization"] == "acme"


def test_a_second_email_reuses_the_list_and_the_contact(crm):
    call_tool("send_email", {"to": "ada@example.org", "subject": "One", "body": "First"})
    lists = len(crm.views)
    failed, text = call_tool("send_email", {"to": "ada@example.org", "subject": "Two", "body": "Second"})
    assert not failed and "CRM: logged for ada@example.org in the Initiatives list" in text
    assert "new contact" not in text and len(crm.views) == lists and len(crm.contacts) == 1
    assert not any(m == "PATCH" for m, _, _ in crm.requests[-3:])


def test_an_existing_list_is_matched_by_name(crm):
    crm.views.append({"id": "b" * 32, "name": "initiatives"})
    call_tool("send_email", {"to": "ada@example.org", "subject": "s", "body": "b"})
    assert next(iter(crm.contacts.values()))["views"] == ["b" * 32]
    assert not any(m == "POST" and p.endswith("/views") for m, p, _ in crm.requests)


def test_internal_only_email_touches_no_crm(crm):
    failed, text = call_tool("send_email", {"to": "colleague@example.com", "subject": "s", "body": "b"})
    assert not failed and "nothing to record" in text and crm.requests == []


def test_crm_failure_still_reports_the_email_as_sent(crm):
    crm.fail = "CRM API 503: unavailable"
    failed, text = call_tool("send_email", {"to": "ada@example.org", "subject": "s", "body": "b"})
    assert not failed and text.startswith("Sent to ada@example.org")
    assert "CRM not updated: CRM API 503: unavailable" in text


def test_crm_can_be_turned_off_per_email(crm):
    failed, text = call_tool("send_email", {"to": "ada@example.org", "subject": "s", "body": "b", "crm": False})
    assert not failed and "CRM" not in text.split("\n")[0] and crm.requests == []


def test_several_organizations_need_one_chosen(crm, monkeypatch, _fake_zoho):
    crm.organizations = ["acme", "globex"]
    _, text = call_tool("send_email", {"to": "ada@example.org", "subject": "s", "body": "b"})
    assert "several CRM organizations (acme, globex)" in text and crm.contacts == {}
    config = json.loads((_fake_zoho / "zoho-mail.json").read_text())
    zoho_mail.save_config({**config, "crmOrganization": "globex", "crmList": "Partners"})
    _, text = call_tool("send_email", {"to": "ada@example.org", "subject": "s", "body": "b"})
    assert "in the Partners list (globex)" in text
    assert all("/globex/" in p for _, p, _ in crm.requests[-4:])


def test_html_email_is_logged_as_text(crm):
    call_tool("send_email", {"to": "ada@example.org", "subject": "s", "body": "<p>Hi &amp; bye</p>", "html": True})
    assert next(iter(crm.contacts.values()))["emails"][0]["text"] == "Hi & bye\n"


def test_cli_logs_in_the_crm_unless_told_not_to(crm, capsys):
    assert zoho_mail.main(["send", "--to", "ada@example.org", "--subject", "s", "--body", "b"]) == 0
    assert "CRM: logged for ada@example.org (new contact) in the Initiatives list (acme)" in capsys.readouterr().out
    before = len(crm.requests)
    assert zoho_mail.main(["send", "--no-crm", "--to", "ada@example.org", "--subject", "s", "--body", "b"]) == 0
    assert len(crm.requests) == before and "CRM" not in capsys.readouterr().out
