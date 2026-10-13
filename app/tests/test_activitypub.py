#!/usr/bin/env python3
"""ActivityPub protocol-spine tests (fediverse federation).

activitypub.py is a pure, js-free sibling module (like releases.py/urls.py),
so unlike the entry.py mirrors these import the real implementation directly.
They pin the wire-compatibility surface Mastodon peers depend on:

  * the Digest header format ("SHA-256=<standard b64 of sha256(body)>");
  * the draft-cavage signing string ((request-target)/(created) pseudo-headers,
    "name: value" lines joined by \n, no trailing newline);
  * Signature header parsing/building;
  * WebFinger / NodeInfo / actor / Note / Create / Accept document shapes;
  * acct-handle <-> actor-URL mapping (users vs "owner.repo" repo actors);
  * remote-HTML sanitization for ingested federated comments.

Run: python3 -m pytest cloudflare_worker/tests/test_activitypub.py
"""
import base64
import hashlib
import importlib.util
import sys
from pathlib import Path

MODULE_PATH = Path(__file__).resolve().parents[1] / "src" / "activitypub.py"
spec = importlib.util.spec_from_file_location("activitypub", MODULE_PATH)
ap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ap)


# --- Handles / identifiers ---------------------------------------------------

def test_parse_acct_resource_forms():
    assert ap.parse_acct_resource("acct:alice@forkmesh.com") == (
        "alice", "forkmesh.com")
    assert ap.parse_acct_resource("alice@forkmesh.com") == (
        "alice", "forkmesh.com")
    assert ap.parse_acct_resource("ACCT:Alice@ForkMesh.COM") == (
        "alice", "forkmesh.com")
    assert ap.parse_acct_resource("acct:owner.repo@forkmesh.com") == (
        "owner.repo", "forkmesh.com")


def test_parse_acct_resource_rejects_malformed():
    assert ap.parse_acct_resource("") == (None, None)
    assert ap.parse_acct_resource("alice") == (None, None)
    assert ap.parse_acct_resource("acct:@forkmesh.com") == (None, None)
    assert ap.parse_acct_resource("acct:a/b@forkmesh.com") == (None, None)
    assert ap.parse_acct_resource("acct:alice@") == (None, None)


def test_split_handle_user_vs_repo():
    assert ap.split_handle("alice") == ("user", "alice")
    assert ap.split_handle("owner.repo") == ("repo", "owner", "repo")
    # Owner names cannot contain dots, so everything after the FIRST dot is
    # the repo name — including repo names that themselves contain dots.
    assert ap.split_handle("owner.my.repo") == ("repo", "owner", "my.repo")
    assert ap.split_handle("Bad Handle") is None
    assert ap.split_handle("-leading") is None
    assert ap.split_handle("") is None


def test_parse_local_actor_url():
    origin = "https://forkmesh.com"
    assert ap.parse_local_actor_url(origin, origin + "/ap/users/alice") == (
        "user", "alice")
    assert ap.parse_local_actor_url(
        origin, origin + "/ap/repos/owner/repo") == ("repo", "owner", "repo")
    assert ap.parse_local_actor_url(origin, origin + "/ap/actor") == (
        "instance",)
    # Fragments/trailing slashes are tolerated; foreign origins are not.
    assert ap.parse_local_actor_url(
        origin, origin + "/ap/users/alice#main-key") == ("user", "alice")
    assert ap.parse_local_actor_url(
        "https://forkmesh.com", "https://evil.com/ap/users/alice") is None
    assert ap.parse_local_actor_url(origin, origin + "/@alice") is None


def test_parse_local_object_url():
    origin = "https://forkmesh.com"
    uuid = "ab" * 16
    assert ap.parse_local_object_url(origin, origin + "/ap/o/" + uuid) == uuid
    assert ap.parse_local_object_url(origin, origin + "/ap/o/xyz") is None
    assert ap.parse_local_object_url(
        origin, "https://other.tld/ap/o/" + uuid) is None


def test_context_key():
    assert ap.context_key("o", "r", "issue", 7) == "o/r#issue#7"
    assert ap.context_key("o", "r", "commit", "abc123") == "o/r#commit#abc123"


# --- Dates -------------------------------------------------------------------

def test_http_date_is_imf_fixdate():
    # 2026-07-10 00:00:00 UTC is a Friday.
    ms = 1783641600000
    value = ap.http_date(ms)
    assert value == "Fri, 10 Jul 2026 00:00:00 GMT"
    assert ap.parse_http_date_ms(value) == ms


def test_iso_utc():
    assert ap.iso_utc(1783641600000) == "2026-07-10T00:00:00Z"


def test_parse_http_date_ms_garbage():
    assert ap.parse_http_date_ms("") == 0
    assert ap.parse_http_date_ms("not a date") == 0


def test_public_federation_url_rejects_ssrf_and_credential_targets():
    assert ap.public_federation_url(
        "https://mastodon.social/users/alice")
    assert not ap.public_federation_url("https://127.0.0.1/actor")
    assert not ap.public_federation_url("https://169.254.169.254/actor")
    assert not ap.public_federation_url(
        "https://user:password@mastodon.social/actor")
    assert not ap.public_federation_url("https://service.internal/actor")
    assert ap.public_federation_url(
        "https://mastodon.social:8443/actor") == (
            "https://mastodon.social:8443/actor")


# --- Digest ------------------------------------------------------------------

def test_digest_header_value_matches_mastodon_format():
    body = b'{"a":1}'
    expected = "SHA-256=" + base64.b64encode(
        hashlib.sha256(body).digest()).decode()
    assert ap.digest_header_value(body) == expected
    assert ap.digest_matches(expected, body)


def test_digest_matches_case_and_multi():
    body = b"hello"
    b64 = base64.b64encode(hashlib.sha256(body).digest()).decode()
    assert ap.digest_matches("sha-256=" + b64, body)
    assert ap.digest_matches("SHA-512=xxx, SHA-256=" + b64, body)
    assert not ap.digest_matches("SHA-256=" + b64, b"tampered")
    assert not ap.digest_matches("", body)
    assert not ap.digest_matches("MD5=abc", body)


# --- Signing string / Signature header -----------------------------------------

def test_signing_string_request_target_and_headers():
    headers = {"host": "mastodon.social", "date": "Fri, 10 Jul 2026 00:00:00 GMT",
               "digest": "SHA-256=abc", "content-type": "application/activity+json"}
    base = ap.signing_string(
        "POST", "/inbox", headers,
        ["(request-target)", "host", "date", "digest", "content-type"])
    assert base == (
        "(request-target): post /inbox\n"
        "host: mastodon.social\n"
        "date: Fri, 10 Jul 2026 00:00:00 GMT\n"
        "digest: SHA-256=abc\n"
        "content-type: application/activity+json")


def test_signing_string_created_and_missing_header():
    base = ap.signing_string(
        "GET", "/ap/users/alice?page=1", {"host": "x"},
        ["(request-target)", "(created)", "host"], created=1700000000)
    assert base == ("(request-target): get /ap/users/alice?page=1\n"
                    "(created): 1700000000\n"
                    "host: x")
    # A signed header that is absent from the request must fail, not sign an
    # empty line.
    assert ap.signing_string("GET", "/", {}, ["host"]) is None
    assert ap.signing_string("GET", "/", {}, ["(created)"]) is None


def test_parse_signature_header_round_trip():
    header = ap.build_signature_header(
        "https://forkmesh.com/ap/users/alice#main-key",
        ["(request-target)", "host", "date", "digest"], "c2ln")
    parsed = ap.parse_signature_header(header)
    assert parsed["keyId"] == "https://forkmesh.com/ap/users/alice#main-key"
    assert parsed["algorithm"] == "rsa-sha256"
    assert parsed["headers"] == ["(request-target)", "host", "date", "digest"]
    assert parsed["signature"] == "c2ln"


def test_parse_signature_header_variants():
    parsed = ap.parse_signature_header(
        'Signature keyId="k",algorithm="hs2019",created=170,expires=200,'
        'headers="(request-target) (created) host",signature="s=="')
    assert parsed["keyId"] == "k"
    assert parsed["created"] == 170
    assert parsed["expires"] == 200
    assert parsed["headers"][1] == "(created)"
    # Missing headers param defaults to ["date"] per draft-cavage.
    parsed = ap.parse_signature_header('keyId="k",signature="s"')
    assert parsed["headers"] == ["date"]
    assert ap.parse_signature_header("") is None
    assert ap.parse_signature_header('algorithm="rsa-sha256"') is None


# --- PEM ---------------------------------------------------------------------

def test_pem_wrap_and_body_round_trip():
    der_b64 = base64.b64encode(b"\x01" * 100).decode()
    pem = ap.pem_wrap("PUBLIC KEY", der_b64)
    assert pem.startswith("-----BEGIN PUBLIC KEY-----\n")
    assert pem.rstrip().endswith("-----END PUBLIC KEY-----")
    assert max(len(line) for line in pem.splitlines()) <= 64
    assert ap.pem_body(pem) == der_b64


# --- Documents ------------------------------------------------------------------

def test_webfinger_doc_shape():
    doc = ap.webfinger_doc("alice@forkmesh.com",
                           "https://forkmesh.com/ap/users/alice")
    assert doc["subject"] == "acct:alice@forkmesh.com"
    link = doc["links"][0]
    assert link["rel"] == "self"
    assert link["type"] == "application/activity+json"
    assert link["href"] == "https://forkmesh.com/ap/users/alice"


def test_nodeinfo_docs():
    index = ap.nodeinfo_index("https://forkmesh.com")
    assert index["links"][0]["href"] == "https://forkmesh.com/nodeinfo/2.1"
    doc = ap.nodeinfo_doc("abc123", 5, 9)
    assert doc["version"] == "2.1"
    assert doc["software"]["name"] == "forkmesh"
    assert doc["protocols"] == ["activitypub"]
    assert doc["usage"]["users"]["total"] == 5
    assert doc["usage"]["localPosts"] == 9


def test_actor_doc_shape():
    doc = ap.actor_doc(
        "https://forkmesh.com/ap/users/alice", "Person", "alice", "alice",
        "", "https://forkmesh.com/@alice", "PEM",
        shared_inbox="https://forkmesh.com/ap/inbox", published_ms=1000)
    assert doc["@context"][0] == ap.AS_CONTEXT
    assert doc["@context"][1] == ap.SECURITY_CONTEXT
    assert doc["preferredUsername"] == "alice"
    assert doc["inbox"] == "https://forkmesh.com/ap/users/alice/inbox"
    assert doc["followers"] == "https://forkmesh.com/ap/users/alice/followers"
    assert doc["publicKey"]["id"] == (
        "https://forkmesh.com/ap/users/alice#main-key")
    assert doc["publicKey"]["owner"] == "https://forkmesh.com/ap/users/alice"
    assert doc["publicKey"]["publicKeyPem"] == "PEM"
    assert doc["endpoints"]["sharedInbox"] == "https://forkmesh.com/ap/inbox"
    assert doc["manuallyApprovesFollowers"] is False


def test_actor_doc_attachments_and_property_value():
    # Profile metadata rows: PropertyValue anchors carrying rel="me" so the
    # linked page can reciprocate and earn Mastodon's verified check.
    pv = ap.property_value("Repository", "https://f.c/o/r")
    assert pv["type"] == "PropertyValue" and pv["name"] == "Repository"
    assert 'href="https://f.c/o/r"' in pv["value"]
    assert 'rel="me' in pv["value"]
    assert ">f.c/o/r</a>" in pv["value"]  # scheme-stripped display label
    doc = ap.actor_doc(
        "https://f.c/ap/repos/o/r", "Group", "o.r", "o/r", "",
        "https://f.c/o/r", "PEM",
        attachments=[pv, ap.property_value("Relay", "https://f.c")])
    assert [a["name"] for a in doc["attachment"]] == ["Repository", "Relay"]
    # No attachments -> no empty attachment key.
    bare = ap.actor_doc("https://f.c/ap/users/a", "Person", "a", "a", "",
                        "https://f.c/@a", "PEM")
    assert "attachment" not in bare
    # The instance actor advertises its relay too.
    inst = ap.instance_actor_doc("https://f.c", "f.c", "PEM")
    assert inst["attachment"][0]["name"] == "Relay"


def test_actor_doc_icon_and_banner():
    doc = ap.actor_doc(
        "https://f.c/ap/repos/o/r", "Group", "o.r", "o/r", "", "https://f.c/o/r",
        "PEM", icon_url="https://f.c/api/repo/o/r/media/logo.png?v=5",
        image_url="https://f.c/assets/fediverse-banner.png")
    assert doc["icon"] == {"type": "Image", "mediaType": "image/png",
                           "url": "https://f.c/api/repo/o/r/media/logo.png?v=5"}
    assert doc["image"]["url"] == "https://f.c/assets/fediverse-banner.png"
    # Absent URLs must not emit empty icon/image blocks.
    bare = ap.actor_doc("https://f.c/ap/users/a", "Person", "a", "a", "",
                        "https://f.c/@a", "PEM")
    assert "icon" not in bare and "image" not in bare


def test_instance_actor_doc():
    doc = ap.instance_actor_doc("https://forkmesh.com", "forkmesh.com", "PEM")
    assert doc["id"] == "https://forkmesh.com/ap/actor"
    assert doc["type"] == "Application"
    assert doc["preferredUsername"] == "forkmesh.com"
    assert doc["inbox"] == "https://forkmesh.com/ap/inbox"


def test_note_and_create_activity():
    note = ap.note_doc(
        "https://forkmesh.com/ap/o/" + "ab" * 16,
        "https://forkmesh.com/ap/repos/o/r",
        "https://forkmesh.com/ap/repos/o/r/followers",
        "<p>hello</p>", 1783641600000,
        web_url="https://forkmesh.com/o/r")
    assert note["type"] == "Note"
    assert note["to"] == [ap.AS_PUBLIC]
    assert note["cc"] == ["https://forkmesh.com/ap/repos/o/r/followers"]
    assert note["published"] == "2026-07-10T00:00:00Z"
    activity = ap.create_activity(note)
    assert activity["type"] == "Create"
    assert activity["id"] == note["id"] + "/activity"
    assert activity["actor"] == note["attributedTo"]
    assert activity["object"] is note
    assert activity["to"] == note["to"]


def test_note_doc_attachments_default_empty_and_pass_through():
    bare = ap.note_doc(
        "https://forkmesh.com/ap/o/1", "https://forkmesh.com/ap/repos/o/r",
        "https://forkmesh.com/ap/repos/o/r/followers", "<p>hi</p>", 1000)
    assert bare["attachment"] == []
    note = ap.note_doc(
        "https://forkmesh.com/ap/o/1", "https://forkmesh.com/ap/repos/o/r",
        "https://forkmesh.com/ap/repos/o/r/followers", "<p>hi</p>", 1000,
        attachments=[ap.image_object("https://forkmesh.com/ap/o/1/media/0")])
    assert note["attachment"] == [
        {"type": "Image", "mediaType": "image/png",
         "url": "https://forkmesh.com/ap/o/1/media/0"}]


def test_delete_activity_is_tombstone():
    activity = ap.delete_activity(
        "https://forkmesh.com/ap/repos/o/r",
        "https://forkmesh.com/ap/o/" + "cd" * 16,
        "https://forkmesh.com/ap/repos/o/r/followers", 1783641600000)
    assert activity["type"] == "Delete"
    assert activity["actor"] == "https://forkmesh.com/ap/repos/o/r"
    assert activity["object"] == {
        "id": "https://forkmesh.com/ap/o/" + "cd" * 16, "type": "Tombstone"}
    # Same audience the original Create reached, so every timeline that showed
    # the Note gets the Tombstone.
    assert activity["to"] == [ap.AS_PUBLIC]
    assert activity["cc"] == ["https://forkmesh.com/ap/repos/o/r/followers"]
    assert activity["published"] == "2026-07-10T00:00:00Z"


def test_accept_activity_is_deterministic():
    follow = {"id": "https://mastodon.social/x/follow/1", "type": "Follow",
              "actor": "https://mastodon.social/users/bob",
              "object": "https://forkmesh.com/ap/users/alice"}
    a1 = ap.accept_activity("https://forkmesh.com/ap/users/alice", follow)
    a2 = ap.accept_activity("https://forkmesh.com/ap/users/alice", follow)
    assert a1["id"] == a2["id"]
    assert a1["type"] == "Accept"
    assert a1["object"] is follow


def test_collection_doc():
    doc = ap.collection_doc("https://x/ap/users/a/followers", 3)
    assert doc["type"] == "OrderedCollection"
    assert doc["totalItems"] == 3
    assert "first" not in doc  # no items given: opaque to remote enumerators


def test_collection_doc_with_items_embeds_first_page():
    doc = ap.collection_doc(
        "https://x/ap/users/a/followers", 2,
        items=["https://remote/users/bob", "https://remote/users/carol"])
    assert doc["first"]["type"] == "OrderedCollectionPage"
    assert doc["first"]["partOf"] == "https://x/ap/users/a/followers"
    assert doc["first"]["orderedItems"] == [
        "https://remote/users/bob", "https://remote/users/carol"]


# --- Remote extraction -----------------------------------------------------------

def test_actor_essentials():
    doc = {
        "id": "https://mastodon.social/users/bob",
        "type": "Person",
        "inbox": "https://mastodon.social/users/bob/inbox",
        "endpoints": {"sharedInbox": "https://mastodon.social/inbox"},
        "preferredUsername": "bob",
        "name": "Bob",
        "url": "https://mastodon.social/@bob",
        "publicKey": {"id": "https://mastodon.social/users/bob#main-key",
                      "owner": "https://mastodon.social/users/bob",
                      "publicKeyPem": "PEM"},
    }
    ess = ap.actor_essentials(doc)
    assert ess["inbox"].endswith("/inbox")
    assert ess["sharedInbox"] == "https://mastodon.social/inbox"
    assert ess["pubkeyPem"] == "PEM"
    assert ess["preferredUsername"] == "bob"
    assert ap.actor_essentials({"id": "x"}) is None
    assert ap.actor_essentials("nope") is None


def test_actor_essentials_carries_public_profile_presentation():
    # The avatar/bio a follower already publishes: cached so "who follows this
    # repository" renders without re-fetching every remote actor.
    doc = {
        "id": "https://mastodon.social/users/bob",
        "inbox": "https://mastodon.social/users/bob/inbox",
        "preferredUsername": "bob",
        "summary": "<p>Kernel hacker</p>",
        "icon": {"type": "Image", "url": "https://files.m.s/bob.png"},
        "image": "https://files.m.s/header.png",
    }
    ess = ap.actor_essentials(doc)
    assert ess["icon"] == "https://files.m.s/bob.png"
    assert ess["image"] == "https://files.m.s/header.png"
    assert ess["summary"] == "<p>Kernel hacker</p>"
    # An actor with no avatar/bio yields empty strings, never None.
    bare = ap.actor_essentials(
        {"id": "https://m.s/users/x", "inbox": "https://m.s/users/x/inbox"})
    assert (bare["icon"], bare["image"], bare["summary"]) == ("", "", "")


def test_image_url_of_accepts_every_published_shape():
    assert ap.image_url_of("https://files.m.s/a.png") == \
        "https://files.m.s/a.png"
    assert ap.image_url_of({"url": "https://files.m.s/b.png"}) == \
        "https://files.m.s/b.png"
    assert ap.image_url_of({"url": {"href": "https://files.m.s/c.png"}}) == \
        "https://files.m.s/c.png"
    assert ap.image_url_of([{}, {"url": "https://files.m.s/d.png"}]) == \
        "https://files.m.s/d.png"
    assert ap.image_url_of(None) == ""
    assert ap.image_url_of(7) == ""


def test_public_media_url_rejects_unroutable_and_hostile_urls():
    assert ap.public_media_url("https://files.m.s/a.png?v=2") == \
        "https://files.m.s/a.png?v=2"
    # Fragments are dropped; the scheme/host are normalized.
    assert ap.public_media_url("https://Files.M.S/a.png#x") == \
        "https://files.m.s/a.png"
    for hostile in (
        "javascript:alert(1)",
        "data:image/png;base64,AAAA",
        "http://files.m.s/a.png",
        "https://user:pass@files.m.s/a.png",
        "https://localhost/a.png",
        "https://192.168.1.10/a.png",
        "https://[::1]/a.png",
        "https://box.local/a.png",
        "https://relay.onion/a.png",
        "https://files.m.s:8443/a.png",
        "https://files.m.s/" + "a" * 900,
        "",
        None,
    ):
        assert ap.public_media_url(hostile) == "", hostile


def test_note_essentials():
    obj = {"id": "https://m.s/notes/1", "type": "Note",
           "inReplyTo": {"id": "https://f.c/ap/o/" + "cd" * 16},
           "content": "<p>hi</p>",
           "attributedTo": "https://m.s/users/bob"}
    ess = ap.note_essentials(obj)
    assert ess["inReplyTo"] == "https://f.c/ap/o/" + "cd" * 16
    assert ess["attributedTo"] == "https://m.s/users/bob"
    assert ap.note_essentials({"id": "x", "type": "Video"}) is None
    assert ap.note_essentials(None) is None


def test_note_essentials_mentions_and_images():
    obj = {
        "id": "https://m.s/notes/2", "type": "Note",
        "content": "<p>@forkmesh.forkmesh broken button</p>",
        "attributedTo": "https://m.s/users/bob",
        "tag": [
            {"type": "Mention",
             "href": "https://f.c/ap/repos/forkmesh/forkmesh",
             "name": "@forkmesh.forkmesh@f.c"},
            {"type": "Hashtag", "href": "https://m.s/tags/bug"},
            "not-a-dict",
        ],
        "attachment": [
            {"type": "Document", "mediaType": "image/png",
             "url": "https://files.m.s/1.png", "name": "screenshot"},
            {"type": "Document", "mediaType": "video/mp4",
             "url": "https://files.m.s/clip.mp4"},
            {"type": "Image", "mediaType": "image/jpeg; charset=binary",
             "url": {"id": "https://files.m.s/2.jpg"}},
            {"type": "Link", "href": "https://elsewhere"},
        ],
    }
    ess = ap.note_essentials(obj)
    assert ess["mentions"] == ["https://f.c/ap/repos/forkmesh/forkmesh"]
    assert ess["images"] == [
        {"url": "https://files.m.s/1.png", "mediaType": "image/png",
         "name": "screenshot"},
        {"url": "https://files.m.s/2.jpg", "mediaType": "image/jpeg",
         "name": ""},
    ]
    # Absent/malformed tag+attachment stay empty lists, never crash.
    bare = ap.note_essentials({"id": "x", "type": "Note", "tag": "nope",
                               "attachment": 7})
    assert bare["mentions"] == [] and bare["images"] == []


def test_activity_object_id():
    assert ap.activity_object_id("x") == "x"
    assert ap.activity_object_id({"id": "y"}) == "y"
    assert ap.activity_object_id(None) == ""
    assert ap.activity_object_id(7) == ""


# --- Sanitization / rendering -------------------------------------------------

def test_sanitize_remote_html():
    dirty = ('<script>alert(1)</script><p>Hello <b>world</b></p>'
             '<p>&amp; more<br>lines</p><style>x{}</style>')
    assert ap.sanitize_remote_html(dirty) == "Hello world\n& more\nlines"


def test_sanitize_remote_html_caps_length():
    text = ap.sanitize_remote_html("x" * 5000, max_len=100)
    assert len(text) <= 101
    assert text.endswith("…")


def test_note_html_from_text_escapes_and_linkifies():
    rendered = ap.note_html_from_text("a <b> & c\nhttps://forkmesh.com/x")
    assert rendered.startswith("<p>")
    assert "&lt;b&gt;" in rendered
    assert "<br>" in rendered
    assert '<a href="https://forkmesh.com/x"' in rendered
    assert 'rel="nofollow noopener noreferrer"' in rendered


def test_event_note_text():
    text = ap.event_note_text(
        "issue", "open", "alice", "widget", 7, "Crash on start",
        "It crashes.", "bob", "https://forkmesh.com/alice/widget")
    assert text.startswith("New issue #7 in alice/widget: Crash on start")
    assert "by @bob" in text
    assert "It crashes." in text
    assert text.endswith("https://forkmesh.com/alice/widget")


def test_announcement_note_text_is_title_body_and_link():
    text = ap.event_note_text(
        "announcement", "publish", "forkmesh", "forkmesh", "",
        "ForkMesh is on Open Collective", "Details on the blog.", "jett",
        "https://forkmesh.com/blog/open-collective/")
    assert text == (
        "ForkMesh is on Open Collective\n\nDetails on the blog.\n\n"
        "https://forkmesh.com/blog/open-collective/")


def test_event_note_text_truncates_body():
    text = ap.event_note_text(
        "release", "publish", "o", "r", "v1.0.0", "v1.0.0", "y" * 2000,
        "", "https://x", max_body=100)
    assert "…" in text
    assert len(text) < 400


def test_extract_body_images_pulls_data_url_and_strips_markdown():
    body = "See this:\n![shot](data:image/png;base64,%s)\nthanks" % ("A" * 40)
    text, images = ap.extract_body_images(body)
    assert "data:" not in text
    assert "![shot]" not in text
    assert text == "See this:\n\nthanks"
    assert images == [{"mediaType": "image/png", "data": "A" * 40}]


def test_extract_body_images_leaves_non_data_images_and_plain_text():
    body = "before ![ext](https://example.com/x.png) after, no images here"
    text, images = ap.extract_body_images(body)
    assert text == body
    assert images == []


def test_extract_body_images_caps_count_and_rejects_bad_payloads():
    good = "data:image/png;base64,%s" % ("A" * 40)
    body = "\n".join("![n](%s)" % good for _ in range(6))
    text, images = ap.extract_body_images(body, max_images=4)
    assert len(images) == 4
    assert text.strip() == ""
    # Malformed base64 and non-image mediaType are both dropped, not stored.
    bad_body = "![a](data:image/png;base64,not-base64!!) ![b](data:text/plain;base64,QQ==)"
    text, images = ap.extract_body_images(bad_body)
    assert images == []
    assert "data:" not in text


# --- Domain blocklist ----------------------------------------------------------

def test_valid_domain():
    assert ap.valid_domain("mastodon.social")
    assert ap.valid_domain("sub.example.co.uk")
    assert not ap.valid_domain("")
    assert not ap.valid_domain("nodot")
    assert not ap.valid_domain("https://mastodon.social")
    assert not ap.valid_domain("mastodon.social/path")
    assert not ap.valid_domain("bad_domain.com")  # _ is a SQL LIKE wildcard
    assert not ap.valid_domain("%.com")
    assert not ap.valid_domain("-lead.com")
    assert not ap.valid_domain("a." + "b" * 300)


def test_domain_blocked_by_includes_subdomains():
    blocked = {"spam.example"}
    assert ap.domain_blocked_by("spam.example", blocked)
    assert ap.domain_blocked_by("sub.spam.example", blocked)
    assert ap.domain_blocked_by("SPAM.example", blocked)
    assert not ap.domain_blocked_by("notspam.example", blocked)
    assert not ap.domain_blocked_by("spam.example.evil", blocked)
    assert not ap.domain_blocked_by("", blocked)


# --- Retry policy -------------------------------------------------------------

def test_retry_backoff_grows_and_caps():
    assert ap.retry_backoff_ms(1) == 5 * 60 * 1000
    assert ap.retry_backoff_ms(2) == 20 * 60 * 1000
    assert ap.retry_backoff_ms(3) == 80 * 60 * 1000
    day = 24 * 60 * 60 * 1000
    assert ap.retry_backoff_ms(7) == day
    assert ap.retry_backoff_ms(100) == day
    assert ap.MAX_DELIVERY_ATTEMPTS >= 5


def test_wants_activity_json():
    assert ap.wants_activity_json("application/activity+json")
    assert ap.wants_activity_json(
        'application/ld+json; profile="https://www.w3.org/ns/activitystreams"')
    assert not ap.wants_activity_json("text/html,application/xhtml+xml")
    assert not ap.wants_activity_json("")


if __name__ == "__main__":
    failures = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print("PASS", name)
            except AssertionError:
                failures += 1
                print("FAIL", name)
    sys.exit(1 if failures else 0)
