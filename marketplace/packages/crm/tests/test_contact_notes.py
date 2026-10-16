import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from test_crm import Runtime, call, crm


def _runtime():
    runtime = Runtime()
    runtime.accounts["dana"] = "bi:dana"
    runtime.db.execute(
        "INSERT INTO org_members VALUES('org:acme','bi:dana','admin','dana',2)")
    runtime.db.commit()
    return runtime


def _contact(runtime, **fields):
    created = call(runtime.use("POST", body=dict(
        {"name": "Grace Example", "email": "grace@example.com"}, **fields)),
        "/api/crm/acme/contacts")
    assert created["status"] == 201
    return created["data"]["contact"]


def _note(runtime, contact_id, text, actor="alice"):
    return call(runtime.use("POST", actor, {"contact": contact_id, "text": text}),
                "/api/crm/acme/contacts/note")


def test_a_note_is_stored_on_the_contact_and_logged_in_list_activity():
    runtime = _runtime()
    view = call(runtime.use("POST", body={"name": "Partners"}),
                "/api/crm/acme/views")["data"]["view"]
    contact = _contact(runtime, views=[view["id"]])

    saved = _note(runtime, contact["id"], "  Called them, wants a demo next week.  ")
    assert saved["status"] == 201
    note = saved["data"]["note"]
    assert (note["by"], note["text"]) == ("alice", "Called them, wants a demo next week.")
    assert saved["data"]["contact"]["noteLog"] == [note]

    stored = call(runtime.use("GET", "charlie"),
                  f"/api/crm/acme/contacts/{contact['id']}")["data"]["contact"]
    assert stored["noteLog"] == [note]
    assert ("alice", "crm.contact.note", "crm_contact", contact["id"]) in [
        args for args, _ in runtime.audits]

    history = call(runtime.use("GET", query={"view": view["id"]}),
                   "/api/crm/acme/views/activity")["data"]["entries"]
    assert history[0]["action"] == "contact.noted"
    assert history[0]["actor"] == "alice"
    assert history[0]["text"] == (
        "added a note on Grace Example: “Called them, wants a demo next week.”")
    row = runtime.db.execute(
        "SELECT data FROM crm_list_activity WHERE action='contact.noted'").fetchone()
    assert row["data"].startswith("sealed:")

    edited = call(runtime.use("PATCH", body={"stage": "qualified"}),
                  f"/api/crm/acme/contacts/{contact['id']}")
    assert edited["data"]["contact"]["noteLog"] == [note]


def test_a_note_pings_everyone_on_an_organization_list_but_the_author():
    runtime = _runtime()
    view = call(runtime.use("POST", body={"name": "Partners"}),
                "/api/crm/acme/views")["data"]["view"]
    contact = _contact(runtime, views=[view["id"]])
    runtime.pings.clear()

    saved = _note(runtime, contact["id"], "Sent the deck.", actor="charlie")
    assert saved["data"]["pinged"] == 2
    pings = {ping["recipient"]: ping for ping in runtime.pings}
    assert set(pings) == {"alice", "dana"}
    ping = pings["alice"]
    assert ping["kind"] == "plugin_activity"
    assert ping["title"] == "Note on Grace Example"
    assert ping["body"] == "By charlie in acme · Partners: Sent the deck."
    assert ping["href"] == f"/dashboard/crm?contact={contact['id']}"
    assert ping["dedupe"] == "crm-note:" + saved["data"]["note"]["id"]


def test_a_note_on_a_private_shared_list_pings_its_owner_admins_and_share_list():
    runtime = _runtime()
    sheet = call(runtime.use("POST", body={
        "name": "Bob's users", "visibility": "private",
        "sharedWith": ["bob"], "shareAccess": "edit"}),
        "/api/crm/acme/views")["data"]["view"]
    contact = _contact(runtime, views=[sheet["id"]])
    runtime.pings.clear()

    _note(runtime, contact["id"], "Partner says they signed up.", actor="dana")
    pings = {ping["recipient"]: ping for ping in runtime.pings}
    assert set(pings) == {"alice", "bob"}
    assert pings["alice"]["href"] == f"/dashboard/crm?contact={contact['id']}"
    assert pings["bob"]["href"] == f"/dashboard/crm?list={sheet['id']}"
    shows_notes = any(column["field"] == "notes" and not column["hidden"]
                      for column in sheet["columns"])
    assert pings["bob"]["body"] == "By dana in acme · Bob's users" + (
        ": Partner says they signed up." if shows_notes else "")

    columns = [dict(column, hidden=column["field"] == "notes")
               for column in sheet["columns"]]
    call(runtime.use("PATCH", body={"columns": columns}),
         f"/api/crm/acme/views/{sheet['id']}")
    runtime.pings.clear()
    _note(runtime, contact["id"], "Private detail.", actor="dana")
    bob = next(ping for ping in runtime.pings if ping["recipient"] == "bob")
    assert bob["body"] == "By dana in acme · Bob's users"

    shared = call(runtime.use("GET", "bob"), f"/api/crm/-/shared/{sheet['id']}")
    assert all("noteLog" not in item for item in shared["data"]["contacts"])


def test_a_contact_on_no_list_pings_every_member():
    runtime = _runtime()
    contact = _contact(runtime)
    runtime.pings.clear()
    _note(runtime, contact["id"], "Met at the conference.")
    assert sorted(ping["recipient"] for ping in runtime.pings) == ["charlie", "dana"]
    assert runtime.pings[0]["body"] == "By alice in acme: Met at the conference."


def test_note_requests_are_checked():
    runtime = _runtime()
    contact = _contact(runtime)
    assert _note(runtime, contact["id"], "   ")["status"] == 400
    assert _note(runtime, contact["id"], "x" * (crm.CONTACT_NOTE_LIMIT + 1))["status"] == 413
    assert _note(runtime, "0" * 32, "Hello")["status"] == 404
    assert _note(runtime, contact["id"], "Hello", actor="bob")["status"] == 404
    assert call(runtime.use("GET"), "/api/crm/acme/contacts/note")["status"] == 405
    assert not [ping for ping in runtime.pings if ping["title"].startswith("Note on")]


def test_the_note_log_keeps_the_newest_notes_and_merges_with_duplicates():
    entries = [{"id": f"{index:032x}", "at": index, "by": "alice", "text": str(index)}
               for index in range(crm.CONTACT_NOTE_KEEP + 5)]
    kept = crm._note_log(entries + [{"id": "bad", "text": "x"}, "junk"])
    assert len(kept) == crm.CONTACT_NOTE_KEEP
    assert kept[0]["text"] == "5" and kept[-1]["text"] == str(crm.CONTACT_NOTE_KEEP + 4)

    first = {"id": "a" * 32, "at": 2, "by": "alice", "text": "Kept note"}
    second = {"id": "b" * 32, "at": 1, "by": "dana", "text": "Duplicate note"}
    merged = crm._merged_contact_data(
        {"name": "Grace", "noteLog": [first]},
        {"name": "Grace", "noteLog": [second, first]})
    assert merged["noteLog"] == [second, first]


def test_the_detail_card_saves_notes_and_lists_them_in_its_activity():
    script = (Path(__file__).resolve().parents[1] / "content" / "web" / "page.js").read_text(
        encoding="utf-8")
    detail = script.split("function renderCrmContactDetail()", 1)[1].split("\n  }\n", 1)[0]
    assert "data-crm-note-text" in detail and 'data-crm-note-save="${id}"' in detail
    assert detail.index("Add a note") < detail.index(">Activity<")
    activity = script.split("function crmContactActivity(contact)", 1)[1].split("\n  }\n", 1)[0]
    assert "contact.noteLog" in activity
    save = script.split("async function saveCrmContactNote(", 1)[1].split("\n  }\n", 1)[0]
    assert 'crmOrganizationPath("/contacts/note"), "POST", { contact: contactId, text }' in save
    assert "saveCrmContactNote(note.dataset.crmNoteSave, note)" in script
