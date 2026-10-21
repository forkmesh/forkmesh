#!/usr/bin/env python3
"""The unread-notifications digest email shows item number, submitter, and
time — not just a bare title/body (previously the only fields rendered) — and
names the public organization/account identity rather than the mirror node
that happens to store the row (issue #652)."""

import ast
import asyncio
import importlib.util
import re
import time
from pathlib import Path
from urllib.parse import quote


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
CATALOG = ENTRY.parent / "catalog.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
CATALOG_TEXT = CATALOG.read_text(encoding="utf-8")

FUNCS = {
    "clean_string",
    "_notification_display_repo",
    "_notification_digest_title",
    "_notification_item_record",
    "_notification_item_path",
    "_notification_stored_path",
    "_notification_stored_tail",
    "_notification_item_url",
    "_notification_record_label",
    "_notification_actor_html",
    "_html_escape",
    "_light_email_fragment",
    "_forkmesh_email_action_html",
    "_forkmesh_email_card_html",
    "_format_email_ts",
    "_notification_email_item",
    "_notification_digest_email",
    "_notification_instant_email",
    "_notification_email_footer_html",
    "_notification_email_subject",
}
# Module constants the rendering path reads. Taken from entry.py's own
# assignments rather than restated here, so a limit or route-segment change
# cannot drift away from what these tests assert.
CONSTANTS = {
    "MAX_NODE_NAME",
    "NODE_NAME_RE",
    "MAX_REPO_SEGMENT",
    "FORKMESH_SITE_URL",
    "AVATAR_B64_RE",
    "MAX_AVATAR_BYTES",
    "MAX_AVATAR_B64",
    "NOTIFICATION_HREF_RE",
    "NOTIFICATION_ITEM_URL_SEGMENTS",
    "NOTIFICATION_ITEM_NOUNS",
    "NOTIFICATION_EMAIL_AVATAR_MAX_B64",
    "NOTIFICATION_EMAIL_SNIPPET_MAX",
}


BOB_BYLINE = ('<a href="https://forkmesh.com/@bob" '
              'style="color:#0969da;text-decoration:none"><strong>bob</strong></a>')


def _chat_format_module():
    spec = importlib.util.spec_from_file_location(
        "chat_format", ENTRY.parent / "chat_format.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _load():
    tree = ast.parse(CATALOG_TEXT + "\n" + ENTRY_TEXT, filename=str(ENTRY))
    selected = []
    for node in tree.body:
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
                and node.name in FUNCS):
            selected.append(node)
        elif (isinstance(node, ast.Assign) and len(node.targets) == 1
                and isinstance(node.targets[0], ast.Name)
                and node.targets[0].id in CONSTANTS):
            selected.append(node)
    found = {
        node.name for node in selected
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
    }
    assert found == FUNCS, "missing functions: %s" % sorted(FUNCS - found)
    names = {
        node.targets[0].id for node in selected
        if isinstance(node, ast.Assign)
    }
    assert names == CONSTANTS, "missing constants: %s" % sorted(
        CONSTANTS - names)
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    ns = {"time": time, "re": re, "quote": quote,
          "_chat_format": _chat_format_module()}
    exec(compile(module, str(ENTRY), "exec"), ns)
    return ns


def test_format_email_ts_is_a_sortable_utc_string():
    ns = _load()
    fmt = ns["_format_email_ts"]
    # 2026-07-09T14:32:00Z in epoch ms.
    assert fmt(1783607520000) == "2026-07-09 14:32 UTC"
    # Missing/zero/garbage timestamps degrade to empty, not a crash or "1970".
    assert fmt(0) == ""
    assert fmt(None) == ""
    assert fmt("not-a-number") == ""


def test_digest_item_shows_number_actor_and_time():
    ns = _load()
    subject, text, html = ns["_notification_digest_email"]("alice", [{
        "kind": "pending_inbox",
        "title": "Issue submitted for alice/forkmesh",
        "body": "Search box is broken on mobile",
        "repo": "alice/forkmesh",
        "actor": "bob",
        "ts": 1783607520000,
        "meta": {"number": 42, "source": "issue"},
    }], base_url="https://forkmesh.com")
    assert subject == (
        "bob: Search box is broken on mobile \xb7 [alice/forkmesh] #42")
    assert "new notification" not in subject + text + html
    assert "You have" not in text + html
    assert ('style="display:none;max-height:0;overflow:hidden;opacity:0;'
            'color:transparent">Search box is broken on mobile</div>') in html
    card = html[html.index('class="fm-item"'):]
    assert card.index(BOB_BYLINE) < card.index("Search box is broken")
    assert card.index("Search box is broken") < card.index("Issue submitted")
    # Text body: number folded into the header line, "by <actor>" + time on
    # their own indented line (matching the existing "    body" indent style).
    # The repository is named by the "[owner/repo]" prefix and taken back out
    # of the title, so an ordinary repo is printed exactly once per item.
    assert "- [alice/forkmesh] #42 Issue submitted\n" in text
    assert text.count("alice/forkmesh]") == 1
    assert "Issue submitted for alice/forkmesh" not in text
    assert "    by bob \xb7 2026-07-09 14:32 UTC" in text
    assert "    Search box is broken on mobile" in text
    # HTML body: same three facts, escaped, in a small muted meta line.
    assert "#42" in html
    assert BOB_BYLINE in html
    assert "2026-07-09 14:32 UTC" in html


def test_digest_links_every_item_and_the_button_to_the_exact_record():
    ns = _load()
    _subject, text, html = ns["_notification_digest_email"]("alice", [{
        "kind": "pending_inbox",
        "title": "Issue submitted for alice/forkmesh",
        "body": "",
        "repo": "alice/forkmesh",
        "actor": "bob",
        "ts": 1783607520000,
        "meta": {"number": 42, "source": "issue"},
    }], base_url="https://forkmesh.com")
    link = "https://forkmesh.com/alice/forkmesh/issues/42"
    # Text digest carries the link on its own indented line.
    assert "    " + link in text
    # The item title is the link, and so is the primary action - which names
    # the record it opens rather than saying "Open ForkMesh".
    assert '<a href="' + link + '"' in html
    assert "data-forkmesh-site-action" in html
    assert ">Open issue #42</a>" in html
    assert 'href="https://forkmesh.com/"' not in html


def test_digest_title_drops_the_repository_however_it_was_cased():
    # A ping title carries the repository name with the case it was created
    # with; the slug is stored folded. Matching only exactly left the mirror
    # name sitting in the title with nothing to strip it.
    ns = _load()
    title = ns["_notification_digest_title"]
    assert title("Issue submitted for Mirror23/ForkMesh",
                 "forkmesh/forkmesh", "mirror23/forkmesh") == "Issue submitted"
    assert title("acme/Widgets: build is red", "acme/widgets") == "build is red"
    assert title("New activity in acme/widgets", "acme/widgets") == "New activity"
    # Nothing to strip leaves the title exactly as written.
    assert title("Bounty funded", "acme/widgets") == "Bounty funded"
    assert title("", "acme/widgets") == ""


def test_digest_maps_each_source_to_its_public_route_segment():
    # Stored sources are singular ("issue"); the web route of an issue is
    # plural ("/issues/93") and of a pull request "/pull/93".
    ns = _load()
    for source, segment, noun in (
        ("issue", "issues", "issue"),
        ("pull", "pull", "pull request"),
        ("discussion", "discussions", "discussion"),
        ("issue_assigned", "issues", "issue"),
        ("bounty", "issues", "issue"),
    ):
        found, number = ns["_notification_item_record"](
            {"meta": {"number": 93, "source": source}}, "acme/widgets")
        assert found == segment
        assert number == 93
        assert ns["_notification_item_path"](
            "acme/widgets", found, number) == "/acme/widgets/%s/93" % segment
        assert ns["_notification_record_label"](
            "acme/widgets", found, number) == "%s #93" % noun


def test_digest_item_without_number_or_actor_degrades_gracefully():
    # A brand-new PR/comment has no durable number yet, and an older stored
    # notification predating actor tracking has none either — the meta line
    # must simply omit whatever piece is missing, not print "None" or "#0".
    ns = _load()
    _subject, text, html = ns["_notification_digest_email"]("alice", [{
        "kind": "pull_submitted",
        "title": "Pull request submitted for alice/forkmesh",
        "body": "",
        "repo": "alice/forkmesh",
        "actor": "",
        "ts": 0,
        "meta": {},
    }], base_url="https://forkmesh.com")
    # The header line has no "#0" prefix and no dangling "by " with nothing
    # after it.
    assert "- [alice/forkmesh] Pull request submitted" in text
    assert "#0" not in text
    assert "None" not in text
    # No "by "/time meta line at all follows the header. Without a number
    # there is no record to address, so the item and the button fall back to
    # the repository — the closest exact address there is.
    lines = text.split("\n")
    header_idx = lines.index("- [alice/forkmesh] Pull request submitted")
    assert lines[header_idx + 1] == "    https://forkmesh.com/alice/forkmesh"
    assert ">Open alice/forkmesh</a>" in html
    card = html.split('class="fm-item"')[1].split("</div>")[0]
    assert 'line-height:20px">alice/forkmesh</p>' in card
    assert "Pull request submitted</a></p>" in card


def test_legacy_notification_without_repo_or_meta_still_renders_and_links():
    # Kinds that predate repo/meta tracking - and kinds that were never about
    # a repository record at all - keep the exact address they recorded.
    ns = _load()
    _subject, text, html = ns["_notification_digest_email"]("alice", [{
        "kind": "calendar_invitation",
        "title": "Invitation: release sync",
        "body": "",
        "repo": "",
        "href": "/dashboard/calendar?event=abc123",
        "actor": "",
        "ts": 0,
    }], base_url="https://forkmesh.com")
    assert text.startswith("- Invitation: release sync\n")
    assert "[" not in text.split("\n")[0]
    link = "https://forkmesh.com/dashboard/calendar?event=abc123"
    assert "    " + link in text
    assert '<a href="' + link + '"' in html
    # No record noun to name, so the button keeps the generic label while
    # still opening the exact address.
    assert ">Open ForkMesh</a>" in html


def test_stored_href_is_only_followed_as_a_same_site_path():
    # The href is stored data. Anything that could leave forkmesh.com is
    # dropped, so a poisoned row cannot turn the digest into an open redirect.
    ns = _load()
    safe = ns["_notification_stored_path"]
    assert safe("/dashboard/settings") == "/dashboard/settings"
    assert safe("/acme/widgets/pulls/93") == "/acme/widgets/pulls/93"
    assert safe("//evil.example/pwn") == ""
    assert safe("https://evil.example/pwn") == ""
    assert safe("javascript:alert(1)") == ""
    assert safe('/x" onmouseover="alert(1)') == ""
    assert safe("") == ""


def test_digest_html_escapes_the_actor_name():
    ns = _load()
    _subject, _text, html = ns["_notification_digest_email"]("alice", [{
        "kind": "mention",
        "title": "You were mentioned",
        "body": "",
        "repo": "alice/forkmesh",
        "actor": "<script>alert(1)</script>",
        "ts": 1783607520000,
        "meta": {"number": 7},
    }])
    assert "<script>alert(1)</script>" not in html
    assert "&lt;script&gt;" in html


def test_actor_avatar_renders_with_the_name_and_falls_back_to_text():
    ns = _load()
    actor_html = ns["_notification_actor_html"]
    png = "iVBORw0KGgoAAAANSUhEUg=="
    # With a portrait: image plus the name, never the image alone, and the
    # alt text repeats the name for a client that blocks images.
    rendered = actor_html("bob", png)
    assert 'src="data:image/png;base64,' + png + '"' in rendered
    assert 'alt="bob"' in rendered
    assert BOB_BYLINE in rendered
    # No avatar stored, an oversized one, and a malformed one all degrade to
    # exactly the text byline the digest showed before avatars existed.
    text_only = BOB_BYLINE
    assert actor_html("bob", "") == text_only
    assert actor_html("bob", None) == text_only
    assert actor_html(
        "bob", "A" * (ns["NOTIFICATION_EMAIL_AVATAR_MAX_B64"] + 8)) == text_only
    assert actor_html("bob", '"><script>alert(1)</script>') == text_only
    # No actor at all (a host/release ping) contributes nothing to the meta
    # line rather than a dangling "by".
    assert actor_html("", png) == ""
    assert actor_html("bob", "", "https://next.example/") == (
        '<a href="https://next.example/@bob" '
        'style="color:#0969da;text-decoration:none"><strong>bob</strong></a>')
    assert actor_html("Bob Smith", "") == "<strong>Bob Smith</strong>"


def test_digest_shows_the_actor_avatar_next_to_the_public_username():
    ns = _load()
    png = "iVBORw0KGgoAAAANSUhEUg=="
    _subject, text, html = ns["_notification_digest_email"](
        "alice",
        [{
            "kind": "pending_inbox",
            "title": "Issue submitted for acme/widgets",
            "body": "",
            "repo": "acme/widgets",
            "actor": "bob",
            "ts": 1783607520000,
            "meta": {"number": 42, "source": "issue"},
        }],
        identity={"repos": {}, "avatars": {"bob": png}},
        base_url="https://forkmesh.com")
    assert 'src="data:image/png;base64,' + png + '"' in html
    assert BOB_BYLINE in html
    # The plain-text alternative still names the submitter.
    assert "    by bob \xb7 " in text


def test_digest_html_uses_one_high_contrast_light_mode():
    ns = _load()
    _subject, _text, html = ns["_notification_digest_email"]("alice", [{
        "kind": "mention",
        "title": "You were mentioned",
        "body": "",
        "repo": "alice/forkmesh",
        "actor": "bob",
        "ts": 1783607520000,
        "meta": {"number": 7},
    }])
    assert 'name="color-scheme" content="light"' in html
    assert 'name="supported-color-schemes" content="light"' in html
    assert "prefers-color-scheme:dark" not in html
    assert "#ffffff" in html
    assert 'class="fm-card"' in html
    assert "background:#090909" not in html
    assert "background:#0f0f11" not in html
    assert "data-forkmesh-site-action" in html


def test_digest_item_uses_the_resolved_public_alias_and_does_not_duplicate():
    # The flagship repository is served by a mirror node and addressed as
    # forkmesh/forkmesh. The mapping is resolved per digest from the
    # organization alias table (no name is hard-coded), and once resolved the
    # backing node must appear nowhere - not in the prefix, not in the title
    # the ping was written with, and not in the link.
    ns = _load()
    _subject, text, html = ns["_notification_digest_email"](
        "alice",
        [{
            "kind": "pending_inbox",
            "title": "Issue submitted for mirror23/forkmesh",
            "body": "",
            "repo": "mirror23/forkmesh",
            "actor": "bob",
            "ts": 1783607520000,
            "meta": {"number": 42, "source": "issue"},
        }],
        identity={"repos": {"mirror23/forkmesh": "forkmesh/forkmesh"}},
        base_url="https://forkmesh.com")
    assert _subject == "bob: Issue submitted \xb7 [forkmesh/forkmesh] #42"
    assert "mirror23" not in text
    assert "mirror23" not in html
    assert text.count("forkmesh/forkmesh") == 2  # item prefix + item link
    assert "- [forkmesh/forkmesh] #42 Issue submitted\n" in text
    assert "https://forkmesh.com/forkmesh/forkmesh/issues/42" in html


def test_unaliased_node_repo_keeps_its_own_route():
    # A repo no organization fronts is addressed at /<node>/<repo>: that is
    # its only real URL, so the digest must not swap in the owning user's
    # account name and produce a link that resolves nowhere.
    ns = _load()
    _subject, text, html = ns["_notification_digest_email"](
        "alice",
        [{
            "kind": "pull_submitted",
            "title": "Pull request submitted for alices-box/notes",
            "body": "",
            "repo": "alices-box/notes",
            "actor": "bob",
            "ts": 1783607520000,
            "meta": {"number": 3, "source": "pull"},
        }],
        identity={"repos": {"alices-box/notes": ""}},
        base_url="https://forkmesh.com")
    assert "Sent to alice." in text
    assert "- [alices-box/notes] #3 Pull request submitted\n" in text
    assert "https://forkmesh.com/alices-box/notes/pull/3" in html


def test_digest_greets_the_account_that_owns_the_mailbox():
    # A node-owned notification is emailed to the node owner's address, so
    # send_notification_digests greets that account rather than the node
    # alias the rows are indexed against.
    digest = ENTRY_TEXT[
        ENTRY_TEXT.index("async def send_notification_digests"):
        ENTRY_TEXT.index("async def _general_chat_encrypted_message_count")
    ]
    assert 'email_rec.get("name", "")' in digest
    assert "identity = await _notification_digest_identity(env, items)" in digest
    assert "greeting, items, identity=identity," in digest
    # The node the rows are indexed against is no longer what gets greeted.
    assert 'clean_string(rec.get("name", "")' not in digest


def test_notify_mentions_accepts_an_optional_number_for_the_digest():
    # notify_mentions had no way to attach the item's number to the stored
    # notification at all — mentions always rendered numberless in the digest
    # even when the mentioning comment was on a real, numbered issue/PR.
    body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def notify_mentions"):
        ENTRY_TEXT.index("def _thread_key")
    ]
    assert "number=0" in body
    assert 'meta={"number": number} if number else {}' in body
    # Call sites that know the number now pass it through.
    for marker in (
        'event.get("body", ""), repo_web_href(owner, repo), "issue",\n                number=number))',
        '"issue",\n                number=number))',
        'repo_web_href(owner, repo), "pull", number=number)',
        'repo_web_href(owner, repo), "discussion", number=number)',
    ):
        assert marker in ENTRY_TEXT


def _load_identity_resolvers(org_repos, accounts):
    """The two database-backed digest resolvers with D1 stubbed.

    org_repos is a list of (organization, repo, node_owner); accounts maps an
    account name to its stored record. Everything else in the resolution path
    is real code read out of entry.py.
    """
    want_funcs = {
        "_notification_public_repo",
        "_notification_digest_identity",
        "_notification_actor_avatar",
        "clean_string",
        "valid_node_name",
    }
    want_assigns = {
        "NODE_NAME_RE",
        "MAX_NODE_NAME",
        "MAX_REPO_SEGMENT",
        "MAX_AVATAR_BYTES",
        "MAX_AVATAR_B64",
        "AVATAR_B64_RE",
        "NOTIFICATION_EMAIL_AVATAR_MAX_B64",
        "STATUS_RETIRED_MIRRORS",
        "NOTIFICATION_DIGEST_MAX_LOOKUPS",
    }
    tree = ast.parse(CATALOG_TEXT + "\n" + ENTRY_TEXT, filename=str(ENTRY))
    body = []
    for node in tree.body:
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
                and node.name in want_funcs):
            body.append(node)
        elif isinstance(node, ast.Assign) and {
                t.id for t in node.targets
                if isinstance(t, ast.Name)} & want_assigns:
            body.append(node)
    found = {
        node.name for node in body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
    }
    assert found == want_funcs, "missing: %s" % sorted(want_funcs - found)

    def _rows(sql, args):
        if "WHERE r.node_owner=? AND r.repo=?" in sql:
            owner, repo = args
            return [{"name": org} for org, name, node in org_repos
                    if node == owner and name == repo]
        if "WHERE r.repo=?" in sql:
            (repo,) = args
            return [{"name": org} for org, name, _node in org_repos
                    if name == repo]
        raise AssertionError("unexpected SQL: %s" % sql)

    async def d1_first(_env, sql, *args):
        rows = _rows(sql, args)
        return rows[0] if rows else None

    async def d1_all(_env, sql, *args):
        return _rows(sql, args)

    async def _account_row(_env, name):
        return name, accounts.get(name)

    ns = {
        "re": re,
        "d1_first": d1_first,
        "d1_all": d1_all,
        "_account_row": _account_row,
    }
    mod = ast.fix_missing_locations(ast.Module(body=body, type_ignores=[]))
    exec(compile(mod, str(ENTRY), "exec"), ns)
    return ns


ALIAS_ROWS = [("forkmesh", "forkmesh", "mirror23"), ("acme", "widgets", "box1")]


def test_alias_fronted_repo_resolves_to_the_organization():
    ns = _load_identity_resolvers(ALIAS_ROWS, {})
    resolve = ns["_notification_public_repo"]
    assert asyncio.run(
        resolve(object(), "mirror23/forkmesh")) == "forkmesh/forkmesh"
    assert asyncio.run(resolve(object(), "box1/widgets")) == "acme/widgets"


def test_unaliased_repo_resolves_to_nothing_and_keeps_its_stored_slug():
    # "" means "no public identity other than the one already stored", which
    # _notification_display_repo renders unchanged.
    ns = _load_identity_resolvers(ALIAS_ROWS, {
        "jetts-box": {"status": "active", "owner": "jett"},
    })
    resolve = ns["_notification_public_repo"]
    assert asyncio.run(resolve(object(), "jetts-box/notes")) == ""
    assert asyncio.run(resolve(object(), "alice/forkmesh")) == ""
    # Malformed slugs never reach a query.
    assert asyncio.run(resolve(object(), "")) == ""
    assert asyncio.run(resolve(object(), "no-slash")) == ""


def test_retired_mirror_resolves_through_the_repo_it_used_to_serve():
    # Notifications are retained for ninety days, so a digest can still carry
    # a slug written under a mirror that has since been retired and unlinked.
    ns = _load_identity_resolvers(ALIAS_ROWS, {})
    resolve = ns["_notification_public_repo"]
    assert "mirror2" in ns["STATUS_RETIRED_MIRRORS"]
    assert asyncio.run(
        resolve(object(), "mirror2/forkmesh")) == "forkmesh/forkmesh"
    # Ambiguous (two organizations publish a "widgets") stays unresolved
    # rather than guessing at someone else's repository.
    ambiguous = ALIAS_ROWS + [("globex", "widgets", "box2")]
    ns = _load_identity_resolvers(ambiguous, {})
    assert asyncio.run(ns["_notification_public_repo"](
        object(), "mirror2/widgets")) == ""


def test_digest_identity_resolves_each_repo_and_actor_once():
    reads = []
    ns = _load_identity_resolvers(ALIAS_ROWS, {
        "bob": {"status": "active", "avatar_png": "iVBORw0KGgo="},
        "carol": {"status": "active"},
        "dave": {"status": "suspended", "avatar_png": "iVBORw0KGgo="},
    })
    real_resolve = ns["_notification_public_repo"]

    async def counting_resolve(env, slug):
        reads.append(slug)
        return await real_resolve(env, slug)

    ns["_notification_public_repo"] = counting_resolve
    identity = asyncio.run(ns["_notification_digest_identity"](object(), [
        {"repo": "mirror23/forkmesh", "actor": "bob"},
        {"repo": "mirror23/forkmesh", "actor": "bob"},
        {"repo": "jetts-box/notes", "actor": "carol"},
        {"repo": "", "actor": "dave"},
    ]))
    assert reads == ["mirror23/forkmesh", "jetts-box/notes"]
    assert identity["repos"] == {
        "mirror23/forkmesh": "forkmesh/forkmesh",
        "jetts-box/notes": "",
    }
    # An account with no portrait, and one that is not active, both resolve to
    # the empty string the renderer treats as "text byline only".
    assert identity["avatars"] == {
        "bob": "iVBORw0KGgo=", "carol": "", "dave": ""}


def test_identity_lookups_are_bounded_per_digest():
    # The digest runs on a shared cron slice, so a backlog spanning many
    # repositories must not turn into one D1 read per item. Past the cap the
    # remaining slugs simply render as stored, which is what they did before
    # any resolution existed.
    ns = _load_identity_resolvers(ALIAS_ROWS, {})
    cap = ns["NOTIFICATION_DIGEST_MAX_LOOKUPS"]
    items = [{"repo": "node%d/app" % i, "actor": "user%d" % i}
             for i in range(cap + 5)]
    identity = asyncio.run(ns["_notification_digest_identity"](object(), items))
    assert len(identity["repos"]) == cap
    assert len(identity["avatars"]) == cap
    # An unresolved slug is absent from the map, and the renderer leaves it
    # exactly as the notification stored it.
    unresolved = "node%d/app" % (cap + 1)
    assert unresolved not in identity["repos"]
    assert _load()["_notification_display_repo"](
        unresolved, identity["repos"]) == unresolved


def test_oversized_or_malformed_avatars_are_dropped_before_rendering():
    limit = _load()["NOTIFICATION_EMAIL_AVATAR_MAX_B64"]
    ns = _load_identity_resolvers(ALIAS_ROWS, {
        "big": {"status": "active", "avatar_png": "A" * (limit + 4)},
        "small": {"status": "active", "avatar_png": "A" * (limit - 4)},
        "bad": {"status": "active", "avatar_png": '"><script>'},
        "gone": None,
    })
    avatar = ns["_notification_actor_avatar"]
    # A portrait too large for one email is dropped here rather than clipping
    # the whole digest in the recipient's client.
    assert asyncio.run(avatar(object(), "big")) == ""
    assert asyncio.run(avatar(object(), "small")) == "A" * (limit - 4)
    assert asyncio.run(avatar(object(), "bad")) == ""
    assert asyncio.run(avatar(object(), "gone")) == ""
    assert asyncio.run(avatar(object(), "not a node name")) == ""


def test_no_repository_name_is_hard_coded_in_the_rendering_path():
    # The flagship alias used to be a literal branch on "mirror2/forkmesh",
    # which silently stopped covering the repository the moment the fleet
    # moved it to another mirror (issue #652).
    render = ENTRY_TEXT[
        ENTRY_TEXT.index("def _notification_display_repo"):
        ENTRY_TEXT.index("def _general_chat_digest_email")
    ]
    assert "mirror2" not in render
    assert "forkmesh/forkmesh" not in render


def test_instant_email_puts_the_event_in_the_subject_and_reuses_the_digest_card():
    """Instant mail is the digest's item card with a per-event subject.

    Sharing _notification_email_item is what keeps the two modes from drifting,
    so this asserts instant inherits issue #652's rendering too: the repository
    named once, the actor rendered as the digest renders it, and a deep link to
    the exact record rather than the site root.
    """
    ns = _load()
    subject, text, html = ns["_notification_instant_email"]("alice", {
        "kind": "pending_inbox",
        "title": "Issue submitted for alice/forkmesh",
        "body": "Search box is broken on mobile",
        "repo": "alice/forkmesh",
        "actor": "bob",
        "ts": 1783607520000,
        "meta": {"number": 42, "source": "issue"},
    }, base_url="https://forkmesh.com")
    assert subject == (
        "bob: Search box is broken on mobile \xb7 [alice/forkmesh] #42")
    assert "New notification" not in html
    assert "new ForkMesh notification" not in html
    assert '<h1 class="fm-h1"' not in html
    assert "- [alice/forkmesh] #42 Issue submitted" in text
    assert "    by bob \xb7 2026-07-09 14:32 UTC" in text
    assert "    Search box is broken on mobile" in text
    # Deep link to the record, in both bodies.
    assert "    https://forkmesh.com/alice/forkmesh/issues/42" in text
    assert "https://forkmesh.com/alice/forkmesh/issues/42" in html
    assert "bob" in html
    # And it names the way back to the digest, not just the way out of email.
    assert "switch back to a periodic digest" in text
    assert "switch back to a periodic digest" in html


def test_instant_email_primary_action_opens_the_record_not_the_site_root():
    # A single-item mail has exactly one record worth opening, so the button
    # must be that record rather than the generic site link.
    ns = _load()
    _subject, _text, html = ns["_notification_instant_email"]("alice", {
        "kind": "pending_inbox", "title": "Pull submitted",
        "repo": "alice/forkmesh", "ts": 1783607520000,
        "meta": {"number": 93, "source": "pull"},
    }, base_url="https://forkmesh.com")
    assert "Open pull request" in html
    assert "https://forkmesh.com/alice/forkmesh/pull/93" in html


def test_instant_email_subject_degrades_when_the_item_has_no_title():
    ns = _load()
    subject, _text, _html = ns["_notification_instant_email"]("alice", {
        "kind": "pending_inbox", "title": "", "ts": 1783607520000})
    assert subject == "ForkMesh: Notification"


def test_item_body_keeps_chat_formatting():
    ns = _load()
    _subject, text, html = ns["_notification_digest_email"]("alice", [{
        "kind": "mention",
        "title": "bob mentioned you",
        "body": "*Deploy* finished\n- `api` is up\n<https://forkmesh.com/s|status>",
        "actor": "bob",
        "ts": 1783607520000,
    }])
    assert "    Deploy finished\n    • api is up\n" in text
    assert "    status (https://forkmesh.com/s)" in text
    assert "<strong>Deploy</strong> finished" in html
    assert "<li><code style=" in html
    assert ">status</a>" in html
    assert "*Deploy*" not in html


def test_item_body_mentions_link_to_the_absolute_profile():
    ns = _load()
    _subject, _text, html = ns["_notification_digest_email"]("alice", [{
        "kind": "mention", "title": "bob mentioned you",
        "body": "thanks @Alice see [@carol](https://x.test/c)",
        "actor": "bob", "ts": 1783607520000,
    }], base_url="https://next.example")
    assert ('<a href="https://next.example/@alice" style="color:#0969da;'
            'text-decoration:underline">@Alice</a>') in html
    assert "<strong>@carol</strong></a>" in html
    assert 'href="https://next.example/@bob"' in html
