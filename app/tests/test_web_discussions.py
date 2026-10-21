#!/usr/bin/env python3
"""Discussions from the web: start, reply, answer and moderate in a browser.

The browser signs every discussion event with its own key (the same key and
wire format as web issue events); the relay checks the signature and, for the
owner-only events, the account session; mirrors and the owner's desktop write
the result in the same .forkmesh/discussions format either way.
"""

import ast
import asyncio
import base64
import json
import re
import subprocess
from pathlib import Path
from urllib.parse import quote

from _dashboard_bundle import assembled_dashboard_js
from test_event_repo_binding import EV, _Signer
from worker_test_helpers import json_from_request_double


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
ENTRY_TEXT = (SRC / "entry.py").read_text(encoding="utf-8")
DASHBOARD_JS = assembled_dashboard_js()
REPO_KEY = "alice/garden"


def _run(coro):
    return asyncio.run(coro)


def _sign(signer, ev, number):
    import hashlib
    content = hashlib.sha256(
        EV["discussion_event_content"](ev).encode()).hexdigest()
    body = "%s\n%d\n%s\n%d\n%s" % (
        ev["type"], number, signer.pub, ev["ts"], content)
    ev = dict(ev, author=signer.pub)
    ev["sig"] = signer.sign(
        "forkmesh-discussion-event-v2\n" + REPO_KEY + "\n" + body)
    return ev


def _verify(number, ev):
    return _run(EV["verify_discussion_event"](number, ev, (REPO_KEY,), False))


# -- the relay's signature rules ---------------------------------------------

def test_a_new_discussion_is_signed_against_zero_with_title_and_category():
    signer = _Signer()
    ev = _sign(signer, {"type": "open", "ts": 1790000000000, "title": "Roadmap",
                        "body": "What next?", "category": "Q&A"}, 0)
    assert _verify(0, ev) is True
    assert _verify(3, ev) is False                      # numbered: refused
    assert _verify(0, dict(ev, body="tampered")) is False
    assert _verify(0, dict(ev, category="Off-topic")) is False
    untitled = _sign(signer, {"type": "open", "ts": 1790000000000, "title": " ",
                              "body": "x", "category": "Ideas"}, 0)
    assert _verify(0, untitled) is False


def test_replies_status_and_delete_need_a_real_number():
    signer = _Signer()
    reply = _sign(signer, {"type": "comment", "ts": 1790000000, "body": "+1"}, 0)
    assert _verify(0, reply) is False
    reply = _sign(signer, {"type": "comment", "ts": 1790000000, "body": "+1"}, 4)
    assert _verify(4, reply) is True
    for status in ("open", "closed", "archived"):
        ev = _sign(signer, {"type": "status", "ts": 1790000001, "status": status}, 4)
        assert EV["discussion_event_content"](ev) == status
        assert _verify(4, ev) is True
        assert _verify(5, ev) is False
    frozen = _sign(signer, {"type": "status", "ts": 1790000001, "status": "frozen"}, 4)
    assert _verify(4, frozen) is False
    delete = _sign(signer, {"type": "delete", "ts": 1790000002,
                            "target": "comment-web-1"}, 4)
    assert _verify(4, delete) is True
    assert _verify(4, dict(delete, target="comment-web-2")) is False
    empty = _sign(signer, {"type": "delete", "ts": 1790000002, "target": ""}, 4)
    assert _verify(4, empty) is False
    other = _sign(signer, {"type": "pin", "ts": 1790000002}, 4)
    assert _verify(4, other) is False


# -- the relay's discussions_handler -----------------------------------------

FUNCS = {
    "discussions_handler",
    "_queued_issue_inbox_event",
    "_bind_session_author",
    "_inbox_author_over_quota",
    "_best_effort_inbox_side_effect",
    "method_name",
    "clean_string",
    "repo_web_href",
}


def _load(extra):
    tree = ast.parse(ENTRY_TEXT + "\n" + (SRC / "catalog.py").read_text(
        encoding="utf-8"))
    selected = [node for node in tree.body
                if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
                and node.name in FUNCS]
    assert {node.name for node in selected} == FUNCS
    namespace = dict(extra)
    namespace.setdefault("bounded_json_request", json_from_request_double)
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[])), "entry.py", "exec"),
        namespace)
    return namespace


class _Request:
    method = "POST"
    url = "https://forge.test/api/repo/alice/garden/discussions"

    def __init__(self, body):
        self._body = body

    async def json(self):
        return self._body


class _Relay:
    """The handler with an in-memory inbox and recorded side effects."""

    def __init__(self, owner=False, session="bob", queued=(), pending=0,
                 author_rows=0, fanout_fails=False):
        self.inserted = []
        self.calls = []
        self.queued = list(queued)
        relay = self

        class _Date:
            @staticmethod
            def now():
                return 1_790_000_000_000

        def json_response(data, status=200, **_kw):
            return {"status": status, "data": data}

        async def none(*_a, **_kw):
            return None

        def recorder(name):
            async def record(*args, **kwargs):
                relay.calls.append((name, args, kwargs))
                if fanout_fails:
                    raise RuntimeError("fan-out down")
            return record

        async def blind_index(_env, value):
            return "bi:" + str(value)

        async def d1_first(_env, sql, *_args):
            if "COUNT(*) AS c FROM discussion_inbox WHERE repo_bi=?" in sql \
                    and "submitter_bi" not in sql:
                return {"c": pending}
            if "submitter_bi=?" in sql:
                return {"c": author_rows}
            raise AssertionError(sql)

        async def d1_all(_env, sql, *_args):
            assert "FROM discussion_inbox WHERE repo_bi=? AND submitter_bi=?" in sql
            return [{"data": row} for row in relay.queued]

        async def d1_run(_env, sql, *args):
            assert sql.startswith("INSERT INTO discussion_inbox")
            relay.inserted.append(args[1])

        async def identity(_env, data):
            return data

        async def owner_check(_env, _request, _data, _owner, _repo):
            return owner

        async def session_record(_env, _request, _data=None, allow_unverified=False):
            return ("bi:" + session, {"name": session}) if session else ("", None)

        async def verify(number, ev, repo_keys=()):
            return await EV["verify_discussion_event"](number, ev, repo_keys, False)

        self.ns = _load({
            "Date": _Date, "json_response": json_response,
            "ensure_schema": none, "blind_index": blind_index,
            "_inbox_repo_key": lambda *_a: _coro("bi:alice/garden"),
            "_OrgAliasUnresolved": type("Unresolved", (Exception,), {}),
            "_repo_feature_off": none,
            "discussion_event_content": EV["discussion_event_content"],
            "verify_discussion_event": verify,
            "_event_signing_repo_keys": lambda _r, o, n: ((o + "/" + n).lower(),),
            "MAX_DISCUSSION_BYTES": 64 * 1024,
            "MAX_PENDING_DISCUSSIONS": 500,
            "MAX_PENDING_PER_AUTHOR": 50,
            "MAX_NODE_NAME": 63,
            "DISCUSSION_OWNER_EVENT_TYPES": frozenset({"status", "delete"}),
            "_discussion_answer_by_owner": owner_check,
            "_account_session_record": session_record,
            "valid_node_name": lambda v: bool(re.fullmatch(r"[a-z][a-z0-9-]*", v or "")),
            "valid_node_pubkey": lambda _v: False,
            "_git_push_account_for_pubkey": none,
            "d1_first": d1_first, "d1_all": d1_all, "d1_run": d1_run,
            "encrypt_row": identity, "decrypt_row": identity,
            "notify_pending_inbox": recorder("pending"),
            "notify_mentions": recorder("mentions"),
            "notify_subscribers": recorder("subscribers"),
            "subscribe_thread": recorder("subscribe"),
            "notify_repo_host": recorder("host"),
            "notify_repo_mirrors": recorder("mirrors"),
            "provider_mirror_note_event": recorder("provider"),
            "_ap_publish_repo_event": recorder("fediverse"),
            "_record_contributor": recorder("contributor"),
            "quote": quote, "base64": base64,
        })

    def post(self, body):
        return _run(self.ns["discussions_handler"](
            None, _Request(body), "alice", "garden"))

    def called(self, name):
        return [call for call in self.calls if call[0] == name]


async def _coro(value):
    return value


def _web_open(signer):
    return {"number": 0, "titleIfNew": "Roadmap", "event": _sign(signer, {
        "type": "open", "id": "open-web-1", "ts": 1790000000000,
        "title": "Roadmap", "body": "What next, @carol?", "category": "Ideas",
        "authorName": "bob"}, 0)}


def test_a_web_discussion_is_queued_numberless_and_announced():
    relay = _Relay()
    result = relay.post(_web_open(_Signer()))
    assert result == {"status": 201, "data": {"ok": True, "number": 0}}
    [item] = relay.inserted
    assert item["number"] == 0 and item["titleIfNew"] == "Roadmap"
    assert item["event"]["authorName"] == "bob" and "meta" not in item
    assert relay.called("pending")[0][2]["opened"] is True
    assert relay.called("mentions") and relay.called("fediverse")
    assert relay.called("provider") and relay.called("contributor")
    assert not relay.called("subscribers")


def test_a_reply_notifies_followers_and_follows_the_thread():
    signer = _Signer()
    relay = _Relay()
    reply = _sign(signer, {"type": "comment", "id": "comment-web-1-a",
                           "ts": 1790000000, "body": "tests first"}, 4)
    assert relay.post({"number": 4, "event": reply})["status"] == 201
    assert relay.called("subscribers")[0][1][3:5] == ("discussion", 4)
    assert relay.called("subscribe")[0][1][3:6] == ("discussion", 4, "bob")


def test_status_and_delete_need_the_owners_session():
    signer = _Signer()
    close = _sign(signer, {"type": "status", "id": "status-web-1",
                           "ts": 1790000000, "status": "closed"}, 4)
    delete = _sign(signer, {"type": "delete", "id": "delete-web-1",
                            "ts": 1790000000, "target": "comment-web-9"}, 4)
    stranger = _Relay(owner=False)
    for event in (close, delete):
        assert stranger.post({"number": 4, "event": event}) == {
            "status": 403, "data": {"error": "not_authorized"}}
    assert stranger.inserted == []
    owner = _Relay(owner=True, session="alice")
    for event in (close, delete):
        assert owner.post({"number": 4, "event": event})["status"] == 201
    assert [item["meta"] for item in owner.inserted] == [
        {"ownerApproved": True}, {"ownerApproved": True}]
    # Moderation is not content: no mentions, followers or contribution.
    assert not owner.called("mentions") and not owner.called("subscribers")
    assert not owner.called("contributor") and not owner.called("fediverse")


def test_an_owners_answer_is_approved_and_anyone_elses_is_not():
    signer = _Signer()
    answer = _sign(signer, {"type": "answer", "id": "answer-web-1",
                            "ts": 1790000000, "target": "comment-web-1"}, 4)
    owner = _Relay(owner=True, session="alice")
    owner.post({"number": 4, "event": answer})
    assert owner.inserted[0]["meta"] == {"ownerApproved": True}
    opener = _Relay(owner=False)
    assert opener.post({"number": 4, "event": answer})["status"] == 201
    assert "meta" not in opener.inserted[0]


def test_a_replayed_signature_is_answered_without_queuing_twice():
    body = _web_open(_Signer())
    relay = _Relay(queued=[{"number": 0, "event": body["event"]}])
    assert relay.post(body) == {"status": 201, "data": {"ok": True, "number": 0}}
    assert relay.inserted == [] and relay.calls == []


def test_rate_limits_and_bad_signatures_refuse_before_queuing():
    signer = _Signer()
    assert _Relay(author_rows=50).post(_web_open(signer))["data"] == {
        "error": "author_quota"}
    assert _Relay(pending=500).post(_web_open(signer))["data"] == {
        "error": "inbox_full"}
    forged = _web_open(signer)
    forged["event"]["body"] = "changed after signing"
    relay = _Relay()
    assert relay.post(forged) == {"status": 401, "data": {"error": "bad_signature"}}
    assert relay.inserted == []
    big = _web_open(signer)
    big["event"]["body"] = "x" * (64 * 1024 + 1)
    assert _Relay().post(big)["status"] == 413


def test_a_failing_fan_out_never_fails_an_accepted_discussion():
    relay = _Relay(fanout_fails=True)

    async def quiet(*_a, **_kw):
        return None
    relay.ns["notify_repo_host"] = quiet
    relay.ns["notify_repo_mirrors"] = quiet
    assert relay.post(_web_open(_Signer()))["status"] == 201
    assert len(relay.inserted) == 1


def test_private_repositories_refuse_every_discussion_post():
    runtime = (SRC / "app_runtime.py").read_text(encoding="utf-8")
    block = runtime[runtime.index("discussions_match = REPO_DISCUSSIONS_RE.match"):]
    block = block[:block.index("return await discussions_handler")]
    # POST is never in the allowed methods for a private repository: a
    # guessed private name answers exactly like an unknown one.
    assert "await _repo_is_private(self.env, owner, repo)" in block
    assert 'method_name(request) in ("GET", "DELETE")' in block
    assert '{"error": "not_found"}, status=404' in block


# -- the browser --------------------------------------------------------------

def _js_function(name):
    match = re.search(r"\n  (?:async )?function %s\(" % re.escape(name), DASHBOARD_JS)
    assert match, name
    end = DASHBOARD_JS.index("\n  }\n", match.start()) + 4
    return DASHBOARD_JS[match.start():end]


def _js_const(name):
    match = re.search(r"\n  const %s = [^\n]*;" % re.escape(name), DASHBOARD_JS)
    assert match, name
    return match.group(0)


SIGNING = [
    "bytesToB64url", "sha256HexLower", "getWebIssueKey", "webEventSigningPrefix",
    "webDiscussionEventId", "signWebDiscussionOpenEvent", "submitWebDiscussion",
    "submitWebDiscussionModeration", "submitWebDiscussionComment",
    "submitWebDiscussionAnswer",
]


def _browser(script):
    prelude = "\n".join([
        "const store = {};",
        "globalThis.localStorage = {getItem: (k) => store[k] ?? null,"
        " setItem: (k, v) => { store[k] = String(v); }};",
        "const state = {session: {nodeName: 'bob', sessionToken: 'tok'}};",
        "const posted = [];",
        "async function fetchRepoMutation(repo, op, payload, options) {"
        " posted.push({op, payload, options: Object.keys(options || {})});"
        " return {ok: true, number: payload.event.type === 'open' ? 9 : 0}; }",
        _js_const("ISSUE_TEXT_ENCODER"), _js_const("WEB_ISSUE_KEY_STORAGE"),
        _js_const("WEB_DISCUSSION_CATEGORIES"),
    ] + [_js_function(name) for name in SIGNING])
    out = subprocess.run(["node", "-e", prelude + "\n(async () => {\n" + script
                          + "\n})().catch((e) => { console.error(e); process.exit(1); });"],
                         check=True, capture_output=True, text=True)
    return json.loads(out.stdout)


def test_browser_signed_events_verify_at_the_relay():
    out = _browser("""
      const repo = {owner: 'Alice', name: 'Garden'};
      const opened = await submitWebDiscussion(repo,
        {title: '  Roadmap  ', body: '\\nWhat next?\\n', category: 'q&a'});
      await submitWebDiscussionComment(repo, 9, 'tests first');
      await submitWebDiscussionAnswer(repo, 9, 'comment-web-1');
      await submitWebDiscussionModeration(repo, 9, 'status', 'closed');
      await submitWebDiscussionModeration(repo, 9, 'delete', 'comment-web-1');
      let refused = '';
      try { await submitWebDiscussionModeration(repo, 9, 'status', 'frozen'); }
      catch (e) { refused = e.message; }
      console.log(JSON.stringify({number: opened.number, posted, refused}));
    """)
    assert out["number"] == 9 and out["refused"] == "unsupported_discussion_action"
    opened, reply, answer, close, delete = out["posted"]
    assert opened["payload"]["number"] == 0 and opened["payload"]["titleIfNew"] == "Roadmap"
    event = opened["payload"]["event"]
    assert (event["title"], event["body"], event["category"]) == (
        "Roadmap", "What next?", "Q&A")
    assert re.fullmatch(r"open-web-\d{13}-[0-9a-z]+", event["id"])
    assert "retryTransient" in opened["options"]
    assert _verify(0, event) is True
    for post in (reply, answer, close, delete):
        assert post["op"] == "discussions" and post["payload"]["number"] == 9
        assert _verify(9, post["payload"]["event"]) is True, post
    assert reply["payload"]["event"]["id"] != answer["payload"]["event"]["id"]
    assert close["payload"]["event"]["status"] == "closed"
    assert delete["payload"]["event"]["target"] == "comment-web-1"
    for post in (close, delete):
        assert post["payload"]["sessionToken"] == "tok"


def test_two_replies_in_one_second_get_distinct_ids():
    out = _browser("""
      const repo = {owner: 'alice', name: 'garden'};
      await submitWebDiscussionComment(repo, 2, 'one');
      await submitWebDiscussionComment(repo, 2, 'two');
      console.log(JSON.stringify(posted.map((p) => p.payload.event.id)));
    """)
    assert len(set(out)) == 2


def _render(script, functions, extra=""):
    prelude = "\n".join([
        "const escapeHtml = (v) => String(v ?? '').replace(/[&<>\"]/g, (c) =>"
        " ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '\"': '&quot;'})[c]);",
        "const state = {session: {nodeName: 'bob'}, discussionsView: {category: 'Q&A'}};",
        "const MD_BODY_CLASS = 'md';",
        "const composeIdentityHtml = () => '<identity>';",
        "const renderPullConversationEvent = (ev) => `<row ${ev.id}>`;",
        "const discussionView = () => state.discussionsView;",
        "const discussionCategoryEmoji = () => '*';",
        _js_const("WEB_DISCUSSION_CATEGORIES"),
        extra,
    ] + [_js_function(name) for name in functions])
    return subprocess.run(["node", "-e", prelude + "\n" + script], check=True,
                          capture_output=True, text=True).stdout


def test_the_composer_has_category_title_body_and_preview():
    html = _render("console.log(renderDiscussionComposeForm())",
                   ["renderDiscussionComposeForm"])
    assert "data-repo-discussion-form" in html
    assert html.count("data-repo-discussion-category-input") == 5
    assert re.search(r'value="Q&amp;A" checked', html)    # the open category
    assert "data-repo-discussion-title" in html and "required" in html
    assert 'data-repo-discussion-body data-mention-input="repo"' in html
    assert 'data-repo-discussion-compose-tab="preview"' in html
    assert "data-repo-discussion-preview hidden" in html
    assert "data-repo-discussion-submit" in html and "desktop" not in html.lower()


def test_empty_list_invites_the_first_discussion():
    html = _render("console.log(renderDiscussionsEmptyState())",
                   ["renderDiscussionsEmptyState"])
    assert "data-repo-discussion-new" in html and "Start a discussion" in html


def test_closed_threads_are_read_only_and_owners_get_moderation():
    out = _render("""
      console.log(JSON.stringify({
        statuses: ['open', 'closed', 'archived', 'weird', ''].map(discussionStatus),
        closed: renderDiscussionLockedNotice('closed'),
        archived: renderDiscussionLockedNotice('archived'),
        openBar: renderDiscussionModerationBar(4, 'open'),
        closedBar: renderDiscussionModerationBar(4, 'closed'),
        archivedBar: renderDiscussionModerationBar(4, 'archived'),
        owner: renderRepoDiscussionConversation([{id: 'c1'}], {canModerate: true, number: 4}),
        visitor: renderRepoDiscussionConversation([{id: 'c1'}], {number: 4}),
      }));
    """, ["discussionStatus", "renderDiscussionLockedNotice",
          "renderDiscussionModerationBar", "renderRepoDiscussionConversation"])
    out = json.loads(out)
    assert out["statuses"] == ["open", "closed", "archived", "open", "open"]
    assert "closed" in out["closed"] and "archived" in out["archived"]
    assert 'data-repo-discussion-status="closed"' in out["openBar"]
    assert 'data-repo-discussion-status="archived"' in out["openBar"]
    assert 'data-repo-discussion-status="open"' in out["closedBar"]
    assert out["archivedBar"].count("data-repo-discussion-status=") == 1
    assert 'data-repo-discussion-delete="c1"' in out["owner"]
    assert "data-repo-discussion-delete" not in out["visitor"]


def test_the_thread_mounts_reply_box_only_while_open():
    detail = DASHBOARD_JS[DASHBOARD_JS.index(
        "    const discussionConversationSection = isDiscussions ? `"):]
    detail = detail[:detail.index('</section>` : "";')]
    assert ('${discussionState === "open" ? renderDiscussionReplyForm(number)'
            ' : renderDiscussionLockedNotice(discussionState)}') in detail
    assert "parsed.discussionModerationAuthorized ? renderDiscussionModerationBar(" in detail
    assert "loadRepoDiscussionModeration(repo)" in DASHBOARD_JS


def test_the_page_wires_every_discussion_control():
    for selector, handler in (
            ("[data-repo-discussion-new]", "openDiscussionCompose(state.selectedRepo)"),
            ("[data-repo-discussion-form]", "handleDiscussionComposeSubmit(state.selectedRepo, discussionForm)"),
            ("[data-repo-discussion-status]", "handleDiscussionStatusClick(state.selectedRepo, discussionStatusButton)"),
            ("[data-repo-discussion-delete]", "handleDiscussionDeleteClick(state.selectedRepo, discussionDeleteButton)"),
            ("[data-repo-discussion-compose-tab]", "setDiscussionComposeTab(")):
        assert 'event.target.closest("%s")' % selector in DASHBOARD_JS
        assert handler in DASHBOARD_JS
    new_button = DASHBOARD_JS[DASHBOARD_JS.index(
        'const discussionNewButton = event.target.closest("[data-repo-discussion-new]");'):]
    new_button = new_button[:new_button.index("openDiscussionCompose(")]
    assert 'location.href = "/login?next="' in new_button   # signed-in only


def test_no_discussion_copy_sends_web_users_to_the_desktop():
    panel = _js_function("renderRepoDiscussionsPanel")
    assert "desktop" not in panel.lower() and "<details data-repo-discussion-new" not in panel
    assert '<button type="button" data-repo-discussion-new' in panel
    docs = (ROOT.parent / "docs" / "app" / "repo-views.md").read_text(encoding="utf-8")
    assert "created from\nthe desktop app" not in docs and "desktop app, since they are signed" not in docs


def test_compose_handler_links_a_numbered_discussion_and_explains_a_queued_one():
    handler = _js_function("handleDiscussionComposeSubmit")
    assert "await submitWebDiscussion(repo, { title, body, category }, progress)" in handler
    assert 'rememberOptimisticRepoRecord(repo, "discussions", number,' in handler
    assert 'navigateHistory(repoRecordUrl(repo, "discussions", number, "conversation"))' in handler
    assert "data-repo-discussion-pending" in handler
    assert "Your draft is still here." in handler
