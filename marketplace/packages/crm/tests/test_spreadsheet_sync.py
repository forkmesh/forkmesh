import importlib.util
from pathlib import Path


spec = importlib.util.spec_from_file_location(
    "crm_sync_harness", Path(__file__).with_name("test_crm.py"))
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)
crm = harness.crm
call = harness.call

SYNC = "/api/crm/acme/contacts/sync"


class CountingRuntime(harness.Runtime):
    def __init__(self):
        super().__init__()
        self.queries = 0

    async def d1_all(self, sql, *args):
        self.queries += 1
        return await super().d1_all(sql, *args)

    async def d1_first(self, sql, *args):
        self.queries += 1
        return await super().d1_first(sql, *args)

    async def d1_run(self, sql, *args):
        self.queries += 1
        return await super().d1_run(sql, *args)


def make_list(runtime, name="Leads", **extra):
    reply = call(runtime.use("POST", body={"name": name, **extra}), "/api/crm/acme/views")
    assert reply["status"] == 201, reply
    return reply["data"]["view"]


def add(runtime, view_id, **fields):
    reply = call(runtime.use("POST", body={**fields, "views": [view_id]}),
                 "/api/crm/acme/contacts")
    assert reply["status"] == 201, reply
    return reply["data"]["contact"]


def read(runtime, list_id, actor="alice"):
    return call(runtime.use("GET", actor, query={"list": list_id}), SYNC)


def write(runtime, list_id, actor="alice", **body):
    return call(runtime.use("POST", actor, body={"list": list_id, **body}), SYNC)


def stored(runtime, contact_id):
    return call(runtime.use("GET"), f"/api/crm/acme/contacts/{contact_id}")["data"]["contact"]


def test_reading_a_list_returns_only_its_contacts_and_every_editable_column():
    runtime = harness.Runtime()
    leads = make_list(runtime)
    other = make_list(runtime, "Other")
    ada = add(runtime, leads["id"], name="Ada", email="ada@example.com",
              fields={"region": "EU"})
    add(runtime, other["id"], name="Bob", email="bob@example.com")
    reply = read(runtime, leads["id"])
    assert reply["status"] == 200
    data = reply["data"]
    assert [contact["id"] for contact in data["contacts"]] == [ada["id"]]
    assert data["contacts"][0]["fields"] == {"region": "EU"}
    fields = [column["field"] for column in data["columns"]]
    assert fields[:2] == ["name", "email"]
    assert set(crm.SYNC_FIELDS) <= set(fields)
    assert "fields.region" in fields and "flow" not in fields
    labels = [column["label"].lower() for column in data["columns"]]
    assert len(labels) == len(set(labels)) and "crm id" not in labels
    assert data["list"] == {"id": leads["id"], "name": "Leads", "flow": "",
                            "stages": [], "canAdd": True}
    assert data["canWrite"] is True and "alice" in data["members"]


def test_a_private_list_and_an_unknown_list_are_not_found():
    runtime = harness.Runtime()
    private = call(runtime.use("POST", "charlie", {"name": "Mine", "visibility": "private"}),
                   "/api/crm/acme/views")["data"]["view"]
    runtime.db.execute(
        "INSERT INTO org_members VALUES('org:acme','bi:bob','member','bob',1)")
    assert read(runtime, private["id"], "bob")["status"] == 404
    assert read(runtime, "ab" * 16)["status"] == 404
    assert read(runtime, "pipeline:nope")["status"] == 404
    assert write(runtime, "ab" * 16, updates=[{"id": "cd" * 16, "set": {"name": "x"}}]
                 )["status"] == 404


def test_edits_are_saved_per_row_and_a_crm_change_since_the_pull_is_a_conflict():
    runtime = harness.Runtime()
    leads = make_list(runtime)
    ada = add(runtime, leads["id"], name="Ada", email="ada@example.com", owner="alice")
    bea = add(runtime, leads["id"], name="Bea", email="bea@example.com", nextStep="Call")
    call(runtime.use("PATCH", body={"nextStep": "Email"}), f"/api/crm/acme/contacts/{bea['id']}")
    reply = write(runtime, leads["id"], updates=[
        {"id": ada["id"], "set": {"owner": "charlie", "fields.region": "EU",
                                  "tags": "vip, Beta"},
         "expect": {"owner": "alice", "fields.region": "", "tags": []}},
        {"id": bea["id"], "set": {"nextStep": "Demo"}, "expect": {"nextStep": "Call"}},
        {"id": "ef" * 16, "set": {"name": "Gone"}},
    ])
    assert reply["status"] == 200, reply
    first, second, third = reply["data"]["results"]
    assert first["status"] == "updated" and first["contact"]["owner"] == "charlie"
    assert first["contact"]["tags"] == ["vip", "beta"]
    assert first["contact"]["fields"] == {"region": "EU"}
    assert second["status"] == "conflict" and second["fields"] == ["nextStep"]
    assert second["contact"]["nextStep"] == "Email"
    assert third == {"id": "ef" * 16, "status": "missing"}
    assert stored(runtime, bea["id"])["nextStep"] == "Email"
    assert stored(runtime, ada["id"])["owner"] == "charlie"
    kept = write(runtime, leads["id"], updates=[
        {"id": bea["id"], "set": {"nextStep": "Demo"}, "expect": {"nextStep": "Email"}}])
    assert kept["data"]["results"][0]["status"] == "updated"
    assert stored(runtime, bea["id"])["nextStep"] == "Demo"


def test_bad_rows_fail_alone_with_a_reason():
    runtime = harness.Runtime()
    pipeline = make_list(runtime, "Deals", flow="customers")
    ada = add(runtime, pipeline["id"], name="Ada", email="ada@example.com", flow="customers")
    bea = add(runtime, pipeline["id"], name="Bea", email="bea@example.com", flow="customers")
    cy = add(runtime, pipeline["id"], name="Cy", email="cy@example.com", flow="customers")
    dee = add(runtime, pipeline["id"], name="Dee", flow="customers")
    reply = write(runtime, pipeline["id"], updates=[
        {"id": ada["id"], "set": {"stage": "nonsense"}},
        {"id": bea["id"], "set": {"email": "ada@example.com"}},
        {"id": cy["id"], "set": {"flow": "investors"}},
        {"id": cy["id"], "set": {"name": "Cy"}},
        {"id": dee["id"], "set": {"name": "Dee"}},
        {"id": "not-an-id", "set": {"name": "x"}},
    ])
    results = reply["data"]["results"]
    assert results[0]["error"] == "unknown_stage" and "lead" in results[0]["stages"]
    assert results[1]["error"] == "contact_email_exists"
    assert results[2] == {"id": cy["id"], "status": "error", "error": "invalid_field",
                          "field": "flow"}
    assert results[3]["error"] == "duplicate_contact_id"
    assert results[4]["status"] == "unchanged"
    assert results[5]["error"] == "invalid_contact_id"
    assert stored(runtime, bea["id"])["email"] == "bea@example.com"
    moved = write(runtime, pipeline["id"], updates=[
        {"id": bea["id"], "set": {"email": "bea@new.example", "stage": "customer"}}])
    assert moved["data"]["results"][0]["status"] == "updated"
    assert stored(runtime, bea["id"])["email"] == "bea@new.example"
    lookup = call(runtime.use("POST", body={"email": "bea@new.example"}),
                  "/api/crm/acme/contacts/lookup")
    assert lookup["data"]["contact"]["id"] == bea["id"]


def test_new_rows_become_contacts_on_the_list_once_even_when_sent_twice():
    runtime = harness.Runtime()
    leads = make_list(runtime)
    deals = make_list(runtime, "Deals", flow="partnerships")
    create = {"ref": "row-3", "requestId": "12" * 16,
              "set": {"name": "Neo", "email": "neo@example.com", "nextStep": "Intro"}}
    first = write(runtime, leads["id"], creates=[create])["data"]["results"][0]
    again = write(runtime, leads["id"], creates=[create])["data"]["results"][0]
    assert first["status"] == "created" and first["id"] == "12" * 16 and first["ref"] == "row-3"
    assert again["status"] == "created" and again["replayed"] is True
    assert [c["id"] for c in read(runtime, leads["id"])["data"]["contacts"]] == ["12" * 16]
    piped = write(runtime, deals["id"], creates=[
        {"ref": "a", "set": {"name": "Pia", "stage": "active"}},
        {"ref": "b", "set": {"name": "Quin", "stage": "nonsense"}},
        {"ref": "c", "set": {"email": "neo@example.com"}},
        {"ref": "d", "set": {"company": "No name"}},
    ])["data"]["results"]
    assert piped[0]["status"] == "created" and piped[0]["contact"]["stage"] == "active"
    assert piped[0]["contact"]["flow"] == "partnerships"
    assert piped[1]["error"] == "unknown_stage"
    assert piped[2]["error"] == "contact_email_exists"
    assert piped[3]["error"] == "name_or_email_required"
    assert [c["name"] for c in read(runtime, deals["id"])["data"]["contacts"]] == ["Pia"]


def test_pipeline_tabs_without_a_list_and_the_no_pipeline_tab_sync_too():
    runtime = harness.Runtime()
    made = write(runtime, "pipeline:investment", creates=[
        {"ref": "a", "set": {"name": "Ivy"}}])["data"]["results"][0]
    assert made["status"] == "created" and made["contact"]["flow"] == "investment"
    listed = read(runtime, "pipeline:investment")["data"]
    assert [c["name"] for c in listed["contacts"]] == ["Ivy"]
    assert listed["list"]["stages"] and listed["list"]["flow"] == "investment"
    orphan = call(runtime.use("POST", body={"name": "Orphan"}), "/api/crm/acme/contacts")
    contact_id = orphan["data"]["contact"]["id"]
    runtime.db.execute("UPDATE crm_contacts SET data=? WHERE contact_id=?", (
        "sealed:" + harness.json.dumps(dict(
            harness.json.loads(runtime.db.execute(
                "SELECT data FROM crm_contacts WHERE contact_id=?",
                (contact_id,)).fetchone()[0].removeprefix("sealed:")),
            flow="retired"), sort_keys=True), contact_id))
    assert [c["name"] for c in read(runtime, "no-pipeline")["data"]["contacts"]] == ["Orphan"]
    refused = write(runtime, "no-pipeline", creates=[{"set": {"name": "x"}}])
    assert refused["data"]["results"][0]["error"] == "list_cannot_add"


def test_read_only_members_can_pull_but_not_push_and_limits_hold():
    runtime = harness.Runtime()
    runtime.db.execute(
        "INSERT INTO org_members VALUES('org:acme','bi:bob','auditor','bob',1)")
    leads = make_list(runtime)
    assert read(runtime, leads["id"], "bob")["data"]["canWrite"] is False
    assert write(runtime, leads["id"], "bob", creates=[{"set": {"name": "x"}}]
                 )["status"] == 403
    assert write(runtime, leads["id"])["status"] == 400
    too_many = [{"set": {"name": str(i)}} for i in range(crm.SYNC_MAX_ROWS + 1)]
    assert write(runtime, leads["id"], creates=too_many)["status"] == 413
    assert call(runtime.use("DELETE"), SYNC)["status"] == 405


def test_five_hundred_edits_are_one_request_with_a_handful_of_queries():
    runtime = CountingRuntime()
    leads = make_list(runtime)
    ids = []
    for index in range(crm.SYNC_MAX_ROWS):
        ids.append(add(runtime, leads["id"], name=f"Person {index}",
                       email=f"p{index}@example.com")["id"])
    runtime.queries = 0
    reply = write(runtime, leads["id"], updates=[
        {"id": contact_id, "set": {"nextStep": "Follow up"}, "expect": {"nextStep": ""}}
        for contact_id in ids])
    assert {result["status"] for result in reply["data"]["results"]} == {"updated"}
    assert runtime.queries < 40
    assert stored(runtime, ids[-1])["nextStep"] == "Follow up"


def test_a_sync_pings_the_team_once():
    runtime = harness.Runtime()
    leads = make_list(runtime)
    ada = add(runtime, leads["id"], name="Ada")
    runtime.pings.clear()
    crm._ACTIVITY_SEEN.clear()
    write(runtime, leads["id"], updates=[{"id": ada["id"], "set": {"title": "CTO"}}])
    assert [ping["title"] for ping in runtime.pings] == [
        "CRM contacts updated from a spreadsheet"]
