import asyncio
import importlib.util
import shutil
import subprocess
from pathlib import Path

import pytest


spec = importlib.util.spec_from_file_location(
    "crm_test_harness", Path(__file__).with_name("test_crm.py"))
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)
crm = harness.crm
call = harness.call

CONTACTS = "/api/crm/acme/contacts"
REQUEST = "ab" * 16


class RacingRuntime(harness.Runtime):
    def __init__(self, insert_before):
        super().__init__()
        self.insert_before = insert_before

    async def d1_run(self, sql, *args):
        if sql.startswith("INSERT INTO crm_contacts(") and self.insert_before:
            row = self.insert_before.pop(0)
            await super().d1_run(sql, row["id"], *args[1:3], row["email_bi"], *args[4:])
        return await super().d1_run(sql, *args)


def contact_count(runtime):
    return runtime.db.execute("SELECT COUNT(*) FROM crm_contacts").fetchone()[0]


def test_a_repeated_add_with_the_same_request_id_returns_the_first_contact():
    runtime = harness.Runtime()
    body = {"name": "Ada", "email": "ada@example.com", "requestId": REQUEST}
    first = call(runtime.use("POST", body=dict(body)), CONTACTS)
    again = call(runtime.use("POST", body=dict(body)), CONTACTS)
    assert first["status"] == 201 and first["data"]["contact"]["id"] == REQUEST
    assert again["status"] == 200 and again["data"]["replayed"] is True
    assert again["data"]["contact"]["id"] == REQUEST
    assert contact_count(runtime) == 1


def test_a_repeated_add_without_an_email_does_not_make_a_twin():
    runtime = harness.Runtime()
    body = {"name": "Company row", "requestId": REQUEST}
    call(runtime.use("POST", body=dict(body)), CONTACTS)
    call(runtime.use("POST", body=dict(body)), CONTACTS)
    assert contact_count(runtime) == 1


def test_an_email_already_in_the_crm_answers_with_that_contact():
    runtime = harness.Runtime()
    kept = call(runtime.use("POST", body={"name": "Ada", "email": "ada@example.com"}),
                CONTACTS)["data"]["contact"]
    clash = call(runtime.use("POST", body={
        "name": "Ada L", "email": "ADA@example.com", "requestId": REQUEST}), CONTACTS)
    assert clash["status"] == 409
    assert clash["data"]["error"] == "contact_email_exists"
    assert clash["data"]["contact"]["id"] == kept["id"]
    assert clash["data"]["contact"]["name"] == "Ada"
    assert contact_count(runtime) == 1


def test_a_racing_add_of_the_same_email_answers_409_not_500():
    racing = RacingRuntime([])
    email_bi = asyncio.run(crm._unique_email_bi(racing, "org:acme", "ada@example.com"))
    racing.insert_before.append({"id": "cd" * 16, "email_bi": email_bi})
    reply = call(racing.use("POST", body={"name": "Ada", "email": "ada@example.com"}),
                 CONTACTS)
    assert reply["status"] == 409
    assert reply["data"]["contact"]["id"] == "cd" * 16
    assert contact_count(racing) == 1


def test_a_racing_replay_of_the_same_request_returns_the_contact():
    racing = RacingRuntime([{"id": REQUEST, "email_bi": ""}])
    reply = call(racing.use("POST", body={"name": "Ada", "requestId": REQUEST}),
                 CONTACTS)
    assert reply["status"] == 200 and reply["data"]["replayed"] is True
    assert contact_count(racing) == 1


def test_a_malformed_request_id_is_ignored():
    runtime = harness.Runtime()
    reply = call(runtime.use("POST", body={"name": "Ada", "requestId": "../x"}), CONTACTS)
    assert reply["status"] == 201
    assert crm.CONTACT_ID_RE.fullmatch(reply["data"]["contact"]["id"])


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_the_web_add_form_retries_once_per_form_and_explains_a_duplicate():
    result = subprocess.run(
        [shutil.which("node"), "--no-warnings",
         str(Path(__file__).with_name("contact_save_checks.js"))],
        capture_output=True, text=True, timeout=120,
        env={"PATH": "/usr/local/bin:/usr/bin:/bin"})
    assert result.returncode == 0, result.stdout[-4000:] + result.stderr[-4000:]
    assert "contact save checks passed" in result.stdout
