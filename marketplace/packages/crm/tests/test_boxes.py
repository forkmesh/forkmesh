import asyncio
import importlib.util
from pathlib import Path


spec = importlib.util.spec_from_file_location(
    "crm_test_harness", Path(__file__).with_name("test_crm.py"))
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)
call = harness.call
crm = harness.crm


def _contact(runtime, **body):
    return call(runtime.use("POST", body=body),
                "/api/crm/acme/contacts")["data"]["contact"]["id"]


def _boxes(runtime, actor="alice"):
    result = call(runtime.use("GET", actor), "/api/crm/acme/boxes")
    assert result["status"] == 200, result
    return result["data"]["boxes"]


def _box_of(boxes, contact_id):
    return next(box for box in boxes if contact_id in box["contacts"])


def test_company_keys_read_work_domains_profiles_and_the_company():
    keys = crm._company_keys({
        "email": "Ada@Mail.Example.co.uk", "company": "Analytical Engines",
        "website": "https://gitlab.example.co.uk/tpo/ux", "github": "ada-org",
        "twitter": "@ada", "fields": {"other_emails": "ada@gmail.com"}})
    assert keys == {
        "email:ada@mail.example.co.uk", "email:ada@gmail.com", "x:ada",
        "domain:example.co.uk", "github:ada-org", "company:analyticalengines"}
    assert crm._company_keys({"email": "x@users.noreply.github.com",
                              "website": "https://github.com/x", "company": "Freelance"}) == {
        "email:x@users.noreply.github.com", "github:x"}
    assert crm._company_keys({"website": "https://fosstodon.org/@someone"}) == set()
    assert crm._registrable_domain("mail.tsinghua.edu.cn") == "tsinghua.edu.cn"
    assert crm._registrable_domain("gitlab.torproject.org") == "torproject.org"


def test_contacts_from_one_company_land_in_one_named_box():
    runtime = harness.Runtime()
    ada = _contact(runtime, name="Ada", email="ada@sagernet.example", company="SagerNet")
    bob = _contact(runtime, name="Bob", email="bob@sagernet.example")
    repo = _contact(runtime, name="sing-box", company="SagerNet", github="SagerNet")
    fork = _contact(runtime, name="cronet-go", github="sagernet")
    gmail = _contact(runtime, name="Gil", email="gil@gmail.com")
    other = _contact(runtime, name="Olga", email="olga@gmail.com")
    boxes = _boxes(runtime)
    sager = _box_of(boxes, ada)
    assert sorted(sager["contacts"]) == sorted([ada, bob, repo, fork])
    assert sager["name"] == "SagerNet" and sager["named"] is False
    assert "domain:sagernet.example" in sager["keys"]
    assert _box_of(boxes, gmail)["id"] != _box_of(boxes, other)["id"]
    assert _box_of(boxes, gmail)["name"] == "Gil"
    assert len(boxes) == 3

    late = _contact(runtime, name="Cy", email="cy@mail.sagernet.example")
    boxes = _boxes(runtime)
    assert late in _box_of(boxes, ada)["contacts"]
    assert len(boxes) == 3
    assert _boxes(runtime, "charlie") == boxes


def test_a_box_can_be_renamed_noted_moved_into_and_merged():
    runtime = harness.Runtime()
    ada = _contact(runtime, name="Ada", email="ada@engines.example")
    bob = _contact(runtime, name="Bob", email="bob@looms.example")
    boxes = _boxes(runtime)
    engines, looms = _box_of(boxes, ada)["id"], _box_of(boxes, bob)["id"]

    renamed = call(runtime.use("PATCH", body={"name": "Analytical Engines", "notes": "Met at the fair"}),
                   f"/api/crm/acme/boxes/{engines}")
    assert renamed["status"] == 200
    box = _box_of(renamed["data"]["boxes"], ada)
    assert (box["name"], box["named"], box["notes"]) == ("Analytical Engines", True, "Met at the fair")
    assert call(runtime.use("PATCH", body={"name": " "}),
                f"/api/crm/acme/boxes/{engines}")["status"] == 400

    moved = call(runtime.use("POST", body={"contacts": [bob], "box": engines}),
                 "/api/crm/acme/boxes/move")
    assert moved["status"] == 200
    boxes = moved["data"]["boxes"]
    assert sorted(_box_of(boxes, ada)["contacts"]) == sorted([ada, bob])
    assert all(box["id"] != looms for box in boxes)
    assert "domain:looms.example" in _box_of(boxes, ada)["keys"]

    fresh = call(runtime.use("POST", body={"contacts": [bob], "name": "Looms Ltd"}),
                 "/api/crm/acme/boxes")
    assert fresh["status"] == 200
    looms = fresh["data"]["box"]
    assert _box_of(fresh["data"]["boxes"], bob)["name"] == "Looms Ltd"
    assert "domain:looms.example" not in _box_of(fresh["data"]["boxes"], ada)["keys"]
    assert _box_of(_boxes(runtime), bob)["id"] == looms

    merged = call(runtime.use("POST", body={"from": looms, "into": engines}),
                  "/api/crm/acme/boxes/merge")
    assert merged["status"] == 200
    assert len(merged["data"]["boxes"]) == 1
    assert sorted(merged["data"]["boxes"][0]["contacts"]) == sorted([ada, bob])
    assert call(runtime.use("POST", body={"from": engines, "into": engines}),
                "/api/crm/acme/boxes/merge")["status"] == 400
    assert call(runtime.use("POST", body={"contacts": [ada]}),
                "/api/crm/acme/boxes/move")["status"] == 400
    assert call(runtime.use("POST", body={"contacts": [ada], "box": "f" * 32}),
                "/api/crm/acme/boxes/move")["status"] == 404
    assert call(runtime.use("DELETE"), f"/api/crm/acme/boxes/{engines}")["status"] == 405


def test_a_moved_contact_stays_put_and_a_deleted_one_leaves_its_box():
    runtime = harness.Runtime()
    ada = _contact(runtime, name="Ada", email="ada@engines.example")
    bob = _contact(runtime, name="Bob", email="bob@engines.example")
    boxes = _boxes(runtime)
    assert _box_of(boxes, ada)["id"] == _box_of(boxes, bob)["id"]
    call(runtime.use("POST", body={"contacts": [bob], "name": "Bob on his own"}),
         "/api/crm/acme/boxes")
    boxes = _boxes(runtime)
    assert _box_of(boxes, bob)["name"] == "Bob on his own"
    assert call(runtime.use("DELETE"), f"/api/crm/acme/contacts/{bob}")["status"] == 200
    boxes = _boxes(runtime)
    assert [box["contacts"] for box in boxes] == [[ada]]
    assert runtime.db.execute("SELECT COUNT(*) FROM crm_box_members").fetchone()[0] == 1


def test_boxes_are_sealed_exported_and_purged_with_the_organization():
    runtime = harness.Runtime()
    ada = _contact(runtime, name="Ada", email="ada@engines.example", company="Engines")
    _boxes(runtime)
    stored = runtime.db.execute("SELECT data FROM crm_boxes").fetchone()["data"]
    assert stored.startswith("sealed:")
    exported = asyncio.run(crm.export_org_data(runtime, "org:acme", "acme"))
    assert exported["boxes"][0]["contacts"] == [ada]
    asyncio.run(crm.purge_org_data(runtime, "org:acme"))
    assert runtime.db.execute("SELECT COUNT(*) FROM crm_boxes").fetchone()[0] == 0
    assert runtime.db.execute("SELECT COUNT(*) FROM crm_box_members").fetchone()[0] == 0
