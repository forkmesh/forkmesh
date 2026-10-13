#!/usr/bin/env python3
"""The badge's last-email row reads on everybody, not just on yourself.

adhoc #21: the chest badge showed "LAST EMAIL 2D AGO · DELIVERED" for the
signed-in player and "LAST EMAIL · NOT SHARED" on every other member, because
the stamp only rode the owner's authenticated /api/accounts/sessions read.
Contracts pinned here:

  * The public directory (/api/accounts/users) carries an hour-bucketed send
    time plus the delivery outcome. The address, the email kind and the send
    count stay behind the authenticated read.
  * The World app forwards both fields, and the scene files them as directory
    facts so a member's walking avatar and their campfire bench figure paint
    the same row.
  * A directory member is never "NOT SHARED": no stamp means never emailed.
    Guests — who own no account record — still are.
"""

import ast
from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
ACCOUNT_USERS = (ROOT / "src" / "account_users.py").read_text(
    encoding="utf-8")
APP = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()


def _top_level_def(tree, name):
    for node in tree.body:
        if isinstance(node, ast.FunctionDef) and node.name == name:
            return node
    raise AssertionError("entry.py must define %s" % name)


def _load(name):
    for node in ast.parse(ENTRY).body:
        if isinstance(node, ast.FunctionDef) and node.name == name:
            namespace = {
                "ACCOUNT_EMAIL_STATUS_DELIVERED": "delivered",
                "ACCOUNT_EMAIL_STATUS_FAILED": "failed",
                "WORLD_PUBLIC_LAST_EMAIL_BUCKET_MS": 60 * 60 * 1000,
            }
            exec(ast.get_source_segment(ENTRY, node), namespace)
            return namespace[name]
    raise AssertionError("entry.py must define %s" % name)


def test_public_stamp_is_hour_bucketed_with_the_delivery_outcome():
    public_last_email = _load("_account_public_last_email")
    hour = 60 * 60 * 1000
    assert public_last_email(
        {"last_email_ts": 5 * hour + 1234, "last_email_ok": True}
    ) == {"lastEmailAt": 5 * hour, "lastEmailStatus": "delivered"}
    assert public_last_email(
        {"last_email_ts": 9 * hour, "last_email_ok": False}
    ) == {"lastEmailAt": 9 * hour, "lastEmailStatus": "failed"}


def test_never_emailed_and_broken_records_publish_no_stamp():
    public_last_email = _load("_account_public_last_email")
    empty = {"lastEmailAt": 0, "lastEmailStatus": ""}
    assert public_last_email({}) == empty
    assert public_last_email({"last_email_ts": 0}) == empty
    assert public_last_email({"last_email_ts": "not-a-number"}) == empty
    assert public_last_email(None) == empty


def test_directory_payload_carries_the_stamp_but_not_the_address():
    stamp = _load("_account_public_last_email")(
        {"last_email_ts": 60 * 60 * 1000, "last_email_ok": True})
    assert set(stamp) == {"lastEmailAt", "lastEmailStatus"}
    assert "**_account_public_last_email(rec)," in ENTRY
    # The kind and the running counter stay on the authenticated read.
    helper = ENTRY.split("def _account_public_last_email", 1)[1].split(
        "\ndef ", 1)[0]
    assert '"lastEmailKind"' not in helper
    assert '"emailSendCount"' not in helper


def test_the_stamp_is_the_directorys_only_last_email_source():
    """adhoc #220: a merge left a second, raw last-email source behind.

    An earlier branch fed the directory a per-account row read straight out of
    mailtrap_email_sends, as a fourth ``email_activity`` argument. This branch
    replaced it: the stamp now rides the account record, hour-bucketed. The
    merge kept both halves of the payload body but only one signature, so
    every directory read raised TypeError and /api/accounts/users and
    /api/leaderboards (which reads the directory) answered 500.

    Both halves are pinned here because either one alone reintroduces the
    outage: a caller arity the signature cannot take, or a raw provider
    timestamp and status where the bucketed contract is documented.
    """
    tree = ast.parse(ENTRY)
    payload = _top_level_def(tree, "_account_chat_user_payload")
    accepted = len(payload.args.args)
    required = accepted - len(payload.args.defaults)
    assert payload.args.vararg is None and payload.args.kwarg is None

    calls = [node for node in ast.walk(ast.parse(ACCOUNT_USERS))
             if isinstance(node, ast.Call)
             and isinstance(node.func, ast.Name)
             and node.func.id == "_account_chat_user_payload"]
    assert calls, "the public directory must build its rows through the payload"
    accepted_names = {argument.arg for argument in payload.args.args}
    for call in calls:
        assert all(
            keyword.arg in accepted_names for keyword in call.keywords
        )
        assert required <= len(call.args) <= accepted, (
            "line %d passes %d arguments to a function taking %d"
            % (call.lineno, len(call.args), accepted))

    # The raw provider row carries an unbucketed send time and the provider's
    # own status vocabulary ("accepted", webhook events) rather than the
    # delivered/failed pair the badge paints, so the public directory must not
    # read it at all. It stays behind the admin email reporting endpoint.
    directory = ACCOUNT_USERS.split(
        "async def _account_users_directory", 1)[1].split(
        "\ndef ", 1)[0]
    assert "mailtrap_email_sends" not in directory
    assert "email_activity" not in directory


def test_world_app_forwards_both_fields_from_the_directory():
    directory = APP.split("function normalizeMemberDirectory(value)", 1)[1] \
        .split("\nfunction ", 1)[0]
    assert "lastEmailAt: Math.max(0, Number(user?.lastEmailAt) || 0)" in \
        directory
    assert 'String(user?.lastEmailStatus || "")' in directory


def test_scene_paints_the_row_for_members_and_hides_it_for_guests():
    # Directory facts: walking avatars and open-circle member figures read them.
    assert "lastEmailAt: Math.max(0, Number(member?.lastEmailAt) || 0)" in SCENE
    assert "lastEmailAt: Math.max(0, Number(member.lastEmailAt) || 0)" in SCENE
    assert SCENE.count("lastEmailPrivate: false") == 3
    # withMemberFacts returns early for a guest, so the guest badge keeps the
    # "NOT SHARED" line.
    assert '"LAST EMAIL · NOT SHARED"' in SCENE
    assert '"LAST EMAIL NEVER · NO DELIVERY"' in SCENE
