#!/usr/bin/env python3
"""Resolving Discord reporters to the ForkMesh accounts linked in D1."""

import hashlib
import hmac
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GENERATOR = ROOT / "app" / "tools" / "sync_discord_accounts.py"

_spec = importlib.util.spec_from_file_location("sync_discord_accounts", GENERATOR)
mapper = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mapper)


def human(discord_id, username, content, message_id="1"):
    return {"id": message_id, "content": content,
            "author": {"id": discord_id, "username": username,
                       "global_name": username.title()}}


def bot(content, message_id="1"):
    return {"id": message_id, "content": content,
            "author": {"id": "999", "username": "Forkmesh Connector",
                       "bot": True}}


def mapping(messages):
    entries = mapper.merge_claims(mapper.extract_claims(messages), messages)
    return {entry["discordUsername"]: entry for entry in entries}


def test_a_reporter_naming_themselves_outranks_a_connector_claim():
    # The luistellom/spidertest case: the connector first guessed, the reporter
    # then stated their own username. Self-declaration has to win.
    result = mapping([
        human("751", "luistellom", "My ForkMesh username is: spidertest"),
        bot("<@751> `someoneelse`: your estimates are ready", "2"),
    ])
    assert result["luistellom"]["forkmeshUser"] == "spidertest"
    assert result["luistellom"]["confidence"] == "self-declared"
    assert [c["forkmeshUser"] for c in result["luistellom"]["conflicts"]] \
        == ["someoneelse"]


def test_a_multi_user_summary_attributes_each_line_separately():
    # One connector message can settle several people at once; a username must
    # not smear across everyone the message mentions.
    result = mapping([
        human("1", "alice", "hi"), human("2", "bob", "hi"),
        bot("Triage updates:\n"
            "• <@1> filed as `alice-fm`\n"
            "• <@2> filed as `bob-fm`\n"
            "• <@1> and <@2> both hit `shared-account`", "9"),
    ])
    assert result["alice"]["forkmeshUser"] == "alice-fm"
    assert result["bob"]["forkmeshUser"] == "bob-fm"
    assert "conflicts" not in result["alice"]
    assert "conflicts" not in result["bob"]


def test_a_connector_guess_is_recorded_as_a_guess():
    result = mapping([
        human("3", "carol", "hi"),
        bot("<@3> I matched your ForkMesh account to `carol-guess` as the "
            "best available guess and linked Discord", "4"),
    ])
    assert result["carol"]["confidence"] == "connector-guess"


def test_a_later_confirmed_link_supersedes_an_earlier_guess():
    result = mapping([
        human("3", "carol", "hi"),
        bot("<@3> I matched your ForkMesh account to `carol-guess` as the "
            "best available guess", "4"),
        bot("<@3> `carol-real`: your estimates are ready", "5"),
    ])
    assert result["carol"]["forkmeshUser"] == "carol-real"
    assert result["carol"]["confidence"] == "connector-linked"
    assert [c["forkmeshUser"] for c in result["carol"]["conflicts"]] \
        == ["carol-guess"]


def test_a_prose_link_resolves_the_handle_to_its_discord_id():
    result = mapping([
        human("7", "yagyashen", "bug report"),
        bot("I linked Discord `@yagyashen` to ForkMesh `yagya-sen`.", "8"),
    ])
    assert result["yagyashen"]["forkmeshUser"] == "yagya-sen"


def test_a_reporter_with_no_claim_is_surfaced_not_guessed():
    messages = [human("5", "dave", "found a bug"),
                human("6", "erin", "another"),
                bot("<@5> `dave-fm`: filed", "7")]
    entries = mapper.merge_claims(mapper.extract_claims(messages), messages)
    unmapped = [u["discordUsername"]
                for u in mapper.reporters_without_account(messages, entries)]
    assert unmapped == ["erin"]


def test_blind_index_matches_the_worker_derivation():
    # entry.py _hmac_key: SHA-256 over DATA_KEY + ":blind-index", used as an
    # HMAC-SHA256 key. Getting this wrong writes rows the Worker can never
    # find, so pin it against an independent recomputation.
    key = "test-data-key"
    expected = hmac.new(
        hashlib.sha256((key + ":blind-index").encode()).digest(),
        b"751648360179433622", hashlib.sha256).hexdigest()
    assert mapper.blind_index(key, "751648360179433622") == expected
    # The Worker lowercases and strips before signing.
    assert mapper.blind_index(key, "  ABC  ") == mapper.blind_index(key, "abc")


def test_sql_links_existing_accounts_and_never_creates_them():
    links = [{"discordId": "751648360179433622", "forkmeshUser": "spidertest"}]
    sql = mapper.render_sql(links, "test-data-key")
    assert "UPDATE users SET discord_bi=" in sql
    assert "INSERT" not in sql.upper().replace("-- GENERATED", "")
    assert "WHERE username='spidertest'" in sql
    # Re-running must not steal a link that already points somewhere else.
    assert "discord_bi IS NULL OR discord_bi=" in sql
    assert mapper.blind_index("test-data-key", "751648360179433622") in sql


def test_sql_escapes_quotes_in_names():
    links = [{"discordId": "751648360179433622", "forkmeshUser": "o'brien"}]
    sql = mapper.render_sql(links, "k")
    assert "'o''brien'" in sql


def test_apply_projection_encrypts_readable_discord_identity_in_account_row():
    data_key = "test-discord-account-data-key"
    original = mapper.encrypt_d1_record(
        {"name": "spidertest", "kind": "user", "status": "active"},
        data_key,
    )
    links = [{
        "discordId": "751648360179433622",
        "discordUsername": "luistellom",
        "discordDisplayName": "Luis Tellom",
        "forkmeshUser": "spidertest",
    }]

    updates = mapper.encrypted_identity_updates(
        links, [{"username": "spidertest", "data": original}], data_key)
    updated = mapper.decrypt_d1_record(
        updates["spidertest"]["updated"], data_key)
    sql = mapper.render_sql(links, data_key, updates)

    assert updated["discordId"] == "751648360179433622"
    assert updated["discordUsername"] == "luistellom"
    assert updated["discordDisplayName"] == "Luis Tellom"
    assert updates["spidertest"]["original"] == original
    assert "data=" in sql
    assert " AND data=" in sql
    assert "751648360179433622" not in sql
    assert "luistellom" not in sql
    assert "Luis Tellom" not in sql


def test_a_guess_is_withheld_from_the_default_write_set():
    messages = [
        human("3", "carol", "hi"),
        bot("<@3> I matched your ForkMesh account to `carol-guess` as the "
            "best available guess", "4"),
    ]
    entries, _ = mapper.resolve(messages)
    writable = [e for e in entries if e["confidence"] != "connector-guess"]
    assert entries and not writable
