"""Pure ActivityPub protocol helpers for the ForkMesh relay Worker.

Everything the fediverse layer needs that is NOT tied to WebCrypto/D1 lives
here: WebFinger/NodeInfo/actor/Note document builders, the draft-cavage HTTP
Signature signing-string construction + Signature header parsing, the Digest
header format, remote-document field extraction and HTML sanitization. The
wire shapes deliberately match what Mastodon (and Wildebeest) emit and accept:

  * Digest: "SHA-256=<standard base64 of sha256(raw body)>"
  * signing string: one "<name>: <value>" line per signed header, joined by
    "\n", with the pseudo-headers "(request-target)" (lowercased method +
    path?query) and "(created)"/"(expires)" (integer seconds)
  * Signature header: keyId="...",algorithm="rsa-sha256",
    headers="(request-target) host date digest",signature="<base64>"

Like ``releases.py``/``urls.py`` this module is stdlib-only — no ``js`` /
``pyodide`` imports — so the test suite imports it directly. All crypto
(RSA keygen/sign/verify via WebCrypto) stays in ``entry.py``.
"""

import base64
import hashlib
import html
import ipaddress
import re
import time
from urllib.parse import urlparse

# --- Constants ----------------------------------------------------------------

AS_CONTEXT = "https://www.w3.org/ns/activitystreams"
SECURITY_CONTEXT = "https://w3id.org/security/v1"
AS_PUBLIC = "https://www.w3.org/ns/activitystreams#Public"
ACTIVITY_CONTENT_TYPE = "application/activity+json; charset=utf-8"
JRD_CONTENT_TYPE = "application/jrd+json; charset=utf-8"

# Accept-header fragments that mean "give me the ActivityPub JSON, not HTML".
_ACTIVITY_ACCEPT_TOKENS = (
    "application/activity+json",
    "application/ld+json",
)

# Default signed headers for outbound requests (Mastodon-compatible).
SIGNED_HEADERS_POST = ["(request-target)", "host", "date", "digest",
                       "content-type"]
SIGNED_HEADERS_GET = ["(request-target)", "host", "date", "accept"]

# Local user/node names never contain a dot (see NODE_NAME_RE in entry.py), so
# "<owner>.<repo>" is an unambiguous repo-actor handle: split on the FIRST dot.
USER_HANDLE_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
REPO_SEGMENT_RE = re.compile(r"^[A-Za-z0-9._-]{1,100}$")

# Give up on a queued delivery after this many attempts (~2 days of backoff).
MAX_DELIVERY_ATTEMPTS = 8

# Actor documents are small JSON objects.  Bound hostile responses before
# parsing so an anonymous inbox delivery cannot make the Worker buffer an
# arbitrarily large remote body.
MAX_REMOTE_ACTOR_BYTES = 256 * 1024


def wants_activity_json(accept_header):
    accept = (accept_header or "").lower()
    return any(token in accept for token in _ACTIVITY_ACCEPT_TOKENS)


# --- Handles / identifiers -----------------------------------------------------

def parse_acct_resource(resource):
    """Parse a WebFinger ?resource= value into (handle, domain), lowercased.

    Accepts "acct:alice@example.com" and the bare "alice@example.com" form some
    clients send. Returns (None, None) when malformed.
    """
    value = (resource or "").strip()
    if value.lower().startswith("acct:"):
        value = value[5:]
    if "@" not in value:
        return None, None
    handle, _, domain = value.partition("@")
    handle = handle.strip().lower()
    domain = domain.strip().lower().rstrip("/")
    if not handle or not domain or "/" in handle or "/" in domain:
        return None, None
    return handle, domain


def repo_handle(owner, repo):
    return "%s.%s" % (owner, repo)


def split_handle(handle):
    """Classify a local acct handle: ("user", name) or ("repo", owner, repo).

    User names cannot contain a dot, so any dotted handle is a repo actor with
    the owner before the first dot.
    """
    handle = (handle or "").strip().lower()
    if "." not in handle:
        if not USER_HANDLE_RE.match(handle):
            return None
        return ("user", handle)
    owner, _, repo = handle.partition(".")
    if not USER_HANDLE_RE.match(owner) or not REPO_SEGMENT_RE.match(repo):
        return None
    return ("repo", owner, repo)


def user_actor_path(name):
    return "/ap/users/%s" % name


def repo_actor_path(owner, repo):
    return "/ap/repos/%s/%s" % (owner, repo)


INSTANCE_ACTOR_PATH = "/ap/actor"

_LOCAL_USER_URL_RE = re.compile(r"^/ap/users/([^/]+)$")
_LOCAL_REPO_URL_RE = re.compile(r"^/ap/repos/([^/]+)/([^/]+)$")
_LOCAL_OBJECT_URL_RE = re.compile(r"^/ap/o/([0-9a-f]{32})$")


def parse_local_actor_url(origin, url):
    """Map one of OUR actor URLs back to ("user", name) / ("repo", owner, repo)
    / ("instance",). Returns None for foreign or unrecognized URLs."""
    url = (url or "").split("#")[0].split("?")[0].rstrip("/")
    if not origin or not url.startswith(origin + "/"):
        return None
    path = url[len(origin):]
    if path == INSTANCE_ACTOR_PATH:
        return ("instance",)
    match = _LOCAL_USER_URL_RE.match(path)
    if match:
        parsed = split_handle(match.group(1))
        if parsed and parsed[0] == "user":
            return parsed
    match = _LOCAL_REPO_URL_RE.match(path)
    if match:
        # Lowercase like split_handle does: AP actor ids must be one stable
        # string, and every forkmesh lookup is case-insensitive anyway.
        owner, repo = match.group(1).lower(), match.group(2).lower()
        if USER_HANDLE_RE.match(owner) and REPO_SEGMENT_RE.match(repo):
            return ("repo", owner, repo)
    return None


def parse_local_object_url(origin, url):
    """Extract the 32-hex object uuid from one of OUR /ap/o/<uuid> URLs."""
    url = (url or "").split("#")[0].split("?")[0].rstrip("/")
    if not origin or not url.startswith(origin + "/"):
        return None
    match = _LOCAL_OBJECT_URL_RE.match(url[len(origin):])
    return match.group(1) if match else None


def context_key(owner, repo, kind, ref):
    """Stable thread key an object and its remote replies share.

    kind is the content family (issue/pull/discussion/commit/release), ref the
    thread identifier within it (issue number, commit sha, release tag...).
    """
    return "%s/%s#%s#%s" % (owner, repo, kind, ref)


# --- Time / date formatting ------------------------------------------------

_WEEKDAYS = ("Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun")
_MONTHS = ("Jan", "Feb", "Mar", "Apr", "May", "Jun",
           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")


def http_date(epoch_ms):
    """IMF-fixdate (RFC 7231) for the HTTP Date header, locale-independent."""
    t = time.gmtime(int(epoch_ms) // 1000)
    return "%s, %02d %s %04d %02d:%02d:%02d GMT" % (
        _WEEKDAYS[t.tm_wday], t.tm_mday, _MONTHS[t.tm_mon - 1], t.tm_year,
        t.tm_hour, t.tm_min, t.tm_sec)


def iso_utc(epoch_ms):
    t = time.gmtime(int(epoch_ms) // 1000)
    return "%04d-%02d-%02dT%02d:%02d:%02dZ" % (
        t.tm_year, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec)


def parse_http_date_ms(value):
    """Best-effort parse of an HTTP Date header back to epoch-ms (0 = unknown)."""
    value = (value or "").strip()
    for fmt in ("%a, %d %b %Y %H:%M:%S GMT", "%a, %d %b %Y %H:%M:%S %Z"):
        try:
            import calendar
            return calendar.timegm(time.strptime(value, fmt)) * 1000
        except (ValueError, OverflowError):
            continue
    return 0


# --- Digest header ----------------------------------------------------------

def digest_header_value(body_bytes):
    return "SHA-256=" + base64.b64encode(
        hashlib.sha256(body_bytes).digest()).decode()


def digest_matches(header_value, body_bytes):
    """True when a Digest header contains a matching sha-256 entry. Multiple
    comma-separated digests are allowed; the algorithm token is case-insensitive
    (Mastodon sends SHA-256=, some servers sha-256=)."""
    expected = base64.b64encode(hashlib.sha256(body_bytes).digest()).decode()
    for part in (header_value or "").split(","):
        alg, _, value = part.strip().partition("=")
        if alg.strip().lower() != "sha-256":
            continue
        if value.strip() == expected:
            return True
    return False


# --- HTTP Signatures (draft-cavage) ------------------------------------------

def signing_string(method, path_with_query, headers, header_names,
                   created=None, expires=None):
    """Build the string that gets RSA-signed / verified.

    headers: dict of lowercased header name -> value as sent/received.
    header_names: the ordered list from the Signature header's headers param.
    Returns None when a required header is missing (a verification MUST fail
    rather than sign an empty line into the string).
    """
    lines = []
    for name in header_names:
        name = name.lower()
        if name == "(request-target)":
            lines.append("(request-target): %s %s"
                         % (method.lower(), path_with_query))
        elif name == "(created)":
            if created is None:
                return None
            lines.append("(created): %d" % int(created))
        elif name == "(expires)":
            if expires is None:
                return None
            lines.append("(expires): %d" % int(expires))
        else:
            if name not in headers:
                return None
            lines.append("%s: %s" % (name, str(headers[name]).strip()))
    return "\n".join(lines)


_SIG_PARAM_RE = re.compile(r'([A-Za-z]+)\s*=\s*(?:"([^"]*)"|([^\s,]+))')


def parse_signature_header(value):
    """Parse a Signature header into {keyId, algorithm, headers, signature,
    created, expires}. Returns None when the essentials are missing."""
    if not value:
        return None
    value = value.strip()
    # Some implementations send "Signature keyId=..." inside an Authorization
    # header; strip the scheme token if present.
    if value.lower().startswith("signature "):
        value = value[len("signature "):]
    params = {}
    for match in _SIG_PARAM_RE.finditer(value):
        params[match.group(1).lower()] = (
            match.group(2) if match.group(2) is not None else match.group(3))
    key_id = params.get("keyid", "")
    signature = params.get("signature", "")
    if not key_id or not signature:
        return None
    headers = params.get("headers", "date").strip().lower().split()
    created = None
    expires = None
    try:
        if params.get("created"):
            created = int(params["created"])
        if params.get("expires"):
            expires = int(params["expires"])
    except (TypeError, ValueError):
        return None
    return {
        "keyId": key_id,
        "algorithm": params.get("algorithm", "").lower(),
        "headers": headers,
        "signature": signature,
        "created": created,
        "expires": expires,
    }


def build_signature_header(key_id, header_names, signature_b64,
                           algorithm="rsa-sha256"):
    return ('keyId="%s",algorithm="%s",headers="%s",signature="%s"'
            % (key_id, algorithm, " ".join(header_names), signature_b64))


# --- PEM helpers -------------------------------------------------------------

def pem_wrap(label, der_b64):
    body = "\n".join(der_b64[i:i + 64] for i in range(0, len(der_b64), 64))
    return "-----BEGIN %s-----\n%s\n-----END %s-----\n" % (label, body, label)


def pem_body(pem):
    """Strip PEM armor + whitespace, returning the base64 DER payload."""
    lines = [line.strip() for line in (pem or "").splitlines()
             if line.strip() and not line.startswith("-----")]
    return "".join(lines)


# --- Document builders --------------------------------------------------------

def webfinger_doc(acct, actor_url):
    return {
        "subject": "acct:" + acct,
        "aliases": [actor_url],
        "links": [
            {"rel": "self", "type": "application/activity+json",
             "href": actor_url},
        ],
    }


def nodeinfo_index(origin):
    return {"links": [{
        "rel": "http://nodeinfo.diaspora.software/ns/schema/2.1",
        "href": origin + "/nodeinfo/2.1",
    }]}


def nodeinfo_doc(version, user_count, post_count):
    return {
        "version": "2.1",
        "software": {
            "name": "forkmesh",
            "version": version or "unknown",
            "repository": "https://forkmesh.com/forkmesh/forkmesh",
        },
        "protocols": ["activitypub"],
        "services": {"outbound": [], "inbound": []},
        "usage": {
            "users": {"total": int(user_count)},
            "localPosts": int(post_count),
        },
        "openRegistrations": True,
        "metadata": {"nodeName": "ForkMesh",
                     "nodeDescription":
                         "Peer-to-peer code hosting mesh — repos, issues and "
                         "releases served from their owners' machines."},
    }


def image_object(url, media_type="image/png"):
    return {"type": "Image", "mediaType": media_type, "url": url}


def property_value(name, url, label=None):
    """One Mastodon profile-metadata row (PropertyValue attachment). The
    value is an anchor carrying rel=\"me\" so the target page can reciprocate
    with its own rel=\"me\" link and earn Mastodon's green verified check."""
    text = label or url.split("://", 1)[-1]
    return {
        "type": "PropertyValue",
        "name": name,
        "value": ("<a href=\"%s\" rel=\"me nofollow noopener noreferrer\" "
                  "target=\"_blank\">%s</a>" % (url, text)),
    }


def property_text(name, text):
    """A plain-text profile-metadata row. Mastodon's link verifier fetches
    every anchor-valued row on each verification pass, so rows that don't
    need the green check stay text and cost the origin nothing."""
    return {"type": "PropertyValue", "name": name, "value": text}


def actor_doc(actor_url, actor_type, preferred_username, display_name,
              summary, profile_url, pubkey_pem, shared_inbox=None,
              published_ms=None, icon_url=None, image_url=None,
              attachments=None):
    doc = {
        "@context": [AS_CONTEXT, SECURITY_CONTEXT],
        "id": actor_url,
        "type": actor_type,
        "preferredUsername": preferred_username,
        "name": display_name,
        "summary": summary or "",
        "url": profile_url,
        "inbox": actor_url + "/inbox",
        "outbox": actor_url + "/outbox",
        "followers": actor_url + "/followers",
        "following": actor_url + "/following",
        "manuallyApprovesFollowers": False,
        "discoverable": True,
        "publicKey": {
            "id": actor_url + "#main-key",
            "owner": actor_url,
            "publicKeyPem": pubkey_pem,
        },
    }
    if shared_inbox:
        doc["endpoints"] = {"sharedInbox": shared_inbox}
    if published_ms:
        doc["published"] = iso_utc(published_ms)
    if icon_url:
        doc["icon"] = image_object(icon_url)     # avatar
    if image_url:
        doc["image"] = image_object(image_url)   # profile header/banner
    if attachments:
        doc["attachment"] = list(attachments)    # profile metadata rows
    return doc


def instance_actor_doc(origin, domain, pubkey_pem, icon_url=None,
                       image_url=None):
    # The service-level actor used to sign outbound GETs (Mastodon "secure mode"
    # requires signed fetches). preferredUsername is the domain itself, which a
    # local user name can never collide with (names cannot contain dots).
    actor_url = origin + INSTANCE_ACTOR_PATH
    doc = actor_doc(
        actor_url, "Application", domain, "ForkMesh relay",
        "Service actor for %s. Follow individual users or repositories "
        "instead." % domain,
        origin, pubkey_pem, shared_inbox=origin + "/ap/inbox",
        icon_url=icon_url, image_url=image_url,
        attachments=[property_value("Relay", origin)])
    doc["inbox"] = origin + "/ap/inbox"
    return doc


def collection_doc(collection_url, total_items, items=None):
    doc = {
        "@context": AS_CONTEXT,
        "id": collection_url,
        "type": "OrderedCollection",
        "totalItems": int(total_items),
    }
    if items is not None:
        # A self-contained first page (no next/prev): small enough lists that
        # remote servers don't need real pagination. Without a `first` page at
        # all, Mastodon's UI treats the collection as hidden ("this user has
        # chosen to not make their followers/following visible") even though
        # totalItems is populated — so omitting this for opted-out accounts is
        # what actually keeps their list private, not just leaving it out.
        doc["first"] = {
            "type": "OrderedCollectionPage",
            "partOf": collection_url,
            "orderedItems": list(items),
        }
    return doc


def note_doc(object_url, actor_url, followers_url, content_html,
             published_ms, web_url=None, in_reply_to=None, summary=None,
             attachments=None):
    doc = {
        "id": object_url,
        "type": "Note",
        "attributedTo": actor_url,
        "content": content_html,
        "published": iso_utc(published_ms),
        "to": [AS_PUBLIC],
        "cc": [followers_url],
        "sensitive": False,
        "attachment": list(attachments) if attachments else [],
        "tag": [],
    }
    if web_url:
        doc["url"] = web_url
    if in_reply_to:
        doc["inReplyTo"] = in_reply_to
    if summary:
        doc["summary"] = summary
    return doc


def create_activity(note):
    return {
        "@context": AS_CONTEXT,
        "id": note["id"] + "/activity",
        "type": "Create",
        "actor": note["attributedTo"],
        "published": note.get("published"),
        "to": note.get("to", []),
        "cc": note.get("cc", []),
        "object": note,
    }


def delete_activity(actor_url, object_url, followers_url, published_ms):
    """Delete(Tombstone) broadcast when the repo owner removes a federated
    post: remote servers replace their cached Note with a Tombstone (Mastodon
    drops the toot from every timeline that showed it). Addressed to Public +
    the actor's followers, the same audience the original Create reached."""
    return {
        "@context": AS_CONTEXT,
        "id": object_url + "#delete/" + str(int(published_ms)),
        "type": "Delete",
        "actor": actor_url,
        "published": iso_utc(published_ms),
        "to": [AS_PUBLIC],
        "cc": [followers_url],
        "object": {
            "id": object_url,
            "type": "Tombstone",
        },
    }


def update_activity(actor_url, actor_doc_obj, published_ms):
    """Update(actor) broadcast after a profile/branding change: remote servers
    replace their cached copy (avatar, header, bio) on receipt instead of
    waiting out their refresh interval."""
    return {
        "@context": [AS_CONTEXT, SECURITY_CONTEXT],
        "id": actor_url + "#updates/" + str(int(published_ms)),
        "type": "Update",
        "actor": actor_url,
        "published": iso_utc(published_ms),
        "to": [AS_PUBLIC],
        "object": actor_doc_obj,
    }


def accept_activity(actor_url, follow_activity):
    follow_id = follow_activity.get("id") or ""
    suffix = hashlib.sha256(follow_id.encode()).hexdigest()[:16]
    return {
        "@context": AS_CONTEXT,
        "id": actor_url + "#accepts/" + suffix,
        "type": "Accept",
        "actor": actor_url,
        "object": follow_activity,
    }


# --- Remote document extraction ------------------------------------------------

def activity_object_id(value):
    """An activity's object/actor may be a bare id string or an embedded
    document; normalize to the id string ("" when absent)."""
    if isinstance(value, str):
        return value
    if isinstance(value, dict):
        return str(value.get("id", "") or "")
    return ""


def image_url_of(value):
    """The URL of an ActivityStreams image property (icon/image).

    Servers publish these as a bare string, an Image object, a nested
    {"url": {"href": ...}} link, or a list of any of those (Mastodon sends an
    Image object; Lemmy and some relays send a plain string)."""
    if isinstance(value, list):
        for item in value[:4]:
            found = image_url_of(item)
            if found:
                return found
        return ""
    if isinstance(value, str):
        return value.strip()
    if isinstance(value, dict):
        return image_url_of(value.get("url") or value.get("href"))
    return ""


_NONPUBLIC_MEDIA_SUFFIXES = (
    ".internal", ".invalid", ".local", ".localhost", ".test", ".example",
    ".onion",
)


def public_federation_url(value, limit=2048):
    """Return a bounded public HTTPS federation endpoint, or ``""``.

    Unlike browser-facing media URLs, public IP literals are allowed because
    ActivityPub actors may legitimately be hosted there; non-global addresses,
    credentials, fragments, and internal/test names are rejected.
    """
    raw = str(value or "").strip()
    if not raw or len(raw) > limit or "\\" in raw:
        return ""
    try:
        parsed = urlparse(raw)
        host = str(parsed.hostname or "").lower().rstrip(".")
        port = parsed.port
    except (TypeError, ValueError):
        return ""
    if (parsed.scheme.lower() != "https"
            or not host
            or parsed.username is not None
            or parsed.password is not None
            or parsed.fragment
            or any(host.endswith(suffix)
                   for suffix in _NONPUBLIC_MEDIA_SUFFIXES)):
        return ""
    try:
        if not ipaddress.ip_address(host).is_global:
            return ""
    except ValueError:
        if "." not in host:
            return ""
    netloc = "[" + host + "]" if ":" in host else host
    if port not in (None, 443):
        netloc += ":" + str(port)
    return parsed._replace(scheme="https", netloc=netloc).geturl()


def public_media_url(value, limit=800):
    """A remote media URL safe to hand a browser: bounded https, public host.

    A remote actor's icon is attacker-controlled text. Only an https URL on a
    routable, non-reserved host is kept — never data:/javascript:, credentials,
    a raw IP, or an internal name a viewer's browser would resolve on its own
    network."""
    raw = str(value or "").strip()
    if not raw or len(raw) > limit or "\\" in raw:
        return ""
    try:
        parsed = urlparse(raw)
        host = str(parsed.hostname or "").lower().rstrip(".")
        port = parsed.port
    except (TypeError, ValueError):
        return ""
    if (parsed.scheme.lower() != "https"
            or not host
            or parsed.username is not None
            or parsed.password is not None
            or port not in (None, 443)
            or "." not in host
            or any(host.endswith(suffix)
                   for suffix in _NONPUBLIC_MEDIA_SUFFIXES)):
        return ""
    try:
        ipaddress.ip_address(host)
        return ""
    except ValueError:
        pass
    return parsed._replace(scheme="https", netloc=host, fragment="").geturl()


def actor_essentials(doc):
    """Pull the fields we persist from a remote actor document."""
    if not isinstance(doc, dict) or not doc.get("id") or not doc.get("inbox"):
        return None
    endpoints = doc.get("endpoints")
    shared_inbox = ""
    if isinstance(endpoints, dict):
        shared_inbox = str(endpoints.get("sharedInbox", "") or "")
    public_key = doc.get("publicKey")
    pem = ""
    key_id = ""
    if isinstance(public_key, dict):
        pem = str(public_key.get("publicKeyPem", "") or "")
        key_id = str(public_key.get("id", "") or "")
    elif isinstance(public_key, list) and public_key:
        first = public_key[0]
        if isinstance(first, dict):
            pem = str(first.get("publicKeyPem", "") or "")
            key_id = str(first.get("id", "") or "")
    if len(pem) > 16 * 1024:
        pem = ""
    return {
        "id": str(doc["id"]),
        "inbox": str(doc["inbox"]),
        "sharedInbox": shared_inbox,
        "pubkeyPem": pem,
        "keyId": key_id,
        "preferredUsername": str(doc.get("preferredUsername", "") or ""),
        "name": str(doc.get("name", "") or ""),
        "url": activity_object_id(doc.get("url")) or str(doc["id"]),
        "type": str(doc.get("type", "") or ""),
        # Public profile presentation: the avatar and the bio the remote server
        # already publishes to anyone who fetches this actor. Callers sanitize
        # (the summary is untrusted remote HTML) before storing or serving.
        "icon": image_url_of(doc.get("icon")),
        "image": image_url_of(doc.get("image")),
        "summary": str(doc.get("summary", "") or ""),
    }


def note_mentions(obj):
    """Actor URLs the note's Mention tags point at (Mastodon puts one tag per
    @-mention). Bounded: a hostile note can't make us walk thousands of tags."""
    tags = obj.get("tag")
    mentions = []
    if isinstance(tags, list):
        for tag in tags[:32]:
            if not isinstance(tag, dict):
                continue
            if str(tag.get("type", "")) != "Mention":
                continue
            href = activity_object_id(tag.get("href"))
            if href:
                mentions.append(href)
    return mentions


def note_image_attachments(obj):
    """Image attachments as [{url, mediaType, name}]. Mastodon attaches media
    as type "Document" with an image/* mediaType; some servers use "Image"."""
    atts = obj.get("attachment")
    if isinstance(atts, dict):
        atts = [atts]
    images = []
    if isinstance(atts, list):
        for att in atts[:16]:
            if not isinstance(att, dict):
                continue
            if str(att.get("type", "")) not in ("Document", "Image"):
                continue
            media_type = str(att.get("mediaType", "") or "")
            media_type = media_type.split(";")[0].strip().lower()
            if not media_type.startswith("image/"):
                continue
            url = activity_object_id(att.get("url"))
            if not url:
                continue
            images.append({
                "url": url,
                "mediaType": media_type,
                "name": str(att.get("name", "") or ""),
            })
    return images


def note_essentials(obj):
    """Pull the fields we keep from an inbound Note (a remote reply or a
    repo-actor mention)."""
    if not isinstance(obj, dict) or not obj.get("id"):
        return None
    if str(obj.get("type", "")) not in ("Note", "Article", "Question",
                                        "Page"):
        return None
    return {
        "id": str(obj["id"]),
        "inReplyTo": activity_object_id(obj.get("inReplyTo")),
        "content": str(obj.get("content", "") or ""),
        "attributedTo": activity_object_id(obj.get("attributedTo")),
        "url": activity_object_id(obj.get("url")) or str(obj["id"]),
        "published": str(obj.get("published", "") or ""),
        "mentions": note_mentions(obj),
        "images": note_image_attachments(obj),
    }


# --- Content rendering / sanitization ------------------------------------------

_TAG_RE = re.compile(r"<[^>]{0,500}>")
_BREAK_RE = re.compile(r"<\s*(?:br\s*/?|/p|/div|/li)\s*>", re.IGNORECASE)
_DROP_BLOCK_RE = re.compile(
    r"<\s*(script|style)[^>]*>.*?<\s*/\s*\1\s*>",
    re.IGNORECASE | re.DOTALL)
_WS_RE = re.compile(r"[ \t\r\f\v]+")
_URL_RE = re.compile(r"(https?://[^\s<>\"']+)")


def sanitize_remote_html(value, max_len=4000):
    """Reduce untrusted remote HTML to plain text: scripts/styles dropped,
    block/br boundaries become newlines, every tag stripped, entities decoded,
    whitespace collapsed, length capped. The clients render the result as text,
    never as markup."""
    text = _DROP_BLOCK_RE.sub(" ", value or "")
    text = _BREAK_RE.sub("\n", text)
    text = _TAG_RE.sub("", text)
    text = html.unescape(text)
    text = _WS_RE.sub(" ", text)
    lines = [line.strip() for line in text.split("\n")]
    text = "\n".join(line for line in lines if line != "").strip()
    if len(text) > max_len:
        text = text[:max_len].rstrip() + "…"
    return text


def note_html_from_text(text):
    """Render our plain-text note content as the minimal HTML fediverse
    clients expect: escaped text in one <p>, newlines as <br>, bare URLs
    wrapped in <a>."""
    escaped = html.escape(text or "", quote=False)

    def linkify(match):
        url = match.group(1)
        return ('<a href="%s" rel="nofollow noopener noreferrer" '
                'target="_blank">%s</a>') % (html.escape(url, quote=True), url)

    escaped = _URL_RE.sub(linkify, escaped)
    return "<p>" + escaped.replace("\n", "<br>") + "</p>"


_EVENT_LABELS = {
    ("issue", "open"): "New issue",
    ("issue", "comment"): "Comment on issue",
    ("pull", "open"): "New pull request",
    ("pull", "comment"): "Comment on pull request",
    ("pull", "review"): "Review on pull request",
    ("pull", "merge"): "Merged pull request",
    ("discussion", "open"): "New discussion",
    ("discussion", "comment"): "Reply in discussion",
    ("commit", "comment"): "Comment on commit",
    ("release", "publish"): "New release",
}


_BODY_IMG_RE = re.compile(r"!\[[^\]]*\]\((data:[^)\s]+)\)")
_DATA_IMAGE_URL_RE = re.compile(
    r"^data:(image/(?:png|jpe?g|gif|webp));base64,([A-Za-z0-9+/]+=*)$",
    re.IGNORECASE)

MAX_NOTE_IMAGES = 4
MAX_NOTE_IMAGE_BYTES = 64 * 1024


def extract_body_images(body, max_images=MAX_NOTE_IMAGES):
    """Web/desktop issue composers embed attached images as markdown
    ``![name](data:...)`` inline in the body (there is no separate upload
    channel). Remote fediverse servers can't fetch a data: URL, and dumping
    raw base64 into the note text is useless (and gets mangled by truncation),
    so pull them out here: return the body with that markdown removed, plus
    the decoded images (still base64) to be attached separately, each keyed
    by its position so the caller can build a stable media URL per image."""
    images = []

    def strip(match):
        if len(images) >= max_images:
            return ""
        parsed = _DATA_IMAGE_URL_RE.match(match.group(1))
        if not parsed:
            return ""
        media_type, data_b64 = parsed.group(1).lower(), parsed.group(2)
        try:
            raw_len = len(base64.b64decode(data_b64, validate=True))
        except Exception:
            return ""
        if not raw_len or raw_len > MAX_NOTE_IMAGE_BYTES:
            return ""
        images.append({"mediaType": media_type, "data": data_b64})
        return ""

    text = _BODY_IMG_RE.sub(strip, body or "")
    text = re.sub(r"\n{3,}", "\n\n", text).strip()
    return text, images


def event_note_text(kind, event_type, owner, repo, ref, title, body,
                    author_name, web_url, max_body=600):
    """Compose the plain-text content for a federated repo event."""
    if kind == "announcement":
        body = (body or "").strip()
        if len(body) > max_body:
            body = body[:max_body].rstrip() + "…"
        return "\n\n".join(part for part in (title, body, web_url) if part)
    label = _EVENT_LABELS.get((kind, event_type), "Update")
    where = "%s/%s" % (owner, repo)
    head = label
    if kind in ("issue", "pull", "discussion") and str(ref) not in ("", "0"):
        head += " #%s" % ref
    if kind == "commit" and ref:
        head += " %s" % str(ref)[:12]
    if kind == "release" and ref:
        head += " %s" % ref
    head += " in %s" % where
    if title:
        head += ": " + title
    if author_name:
        head += "\nby @" + author_name
    body = (body or "").strip()
    if len(body) > max_body:
        body = body[:max_body].rstrip() + "…"
    parts = [head]
    if body:
        parts.append(body)
    if web_url:
        parts.append(web_url)
    return "\n\n".join(parts)


# --- Domain blocklist (defederation) -----------------------------------------

_DOMAIN_RE = re.compile(
    r"^[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?"
    r"(?:\.[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?)+$")


def valid_domain(value):
    """A bare lowercase hostname suitable for the blocklist: labels joined by
    dots, no scheme/path/port, at least one dot. The character set (letters,
    digits, dots, hyphens only) also keeps it safe inside SQL LIKE patterns."""
    value = (value or "").strip().lower()
    return bool(value) and len(value) <= 253 and bool(_DOMAIN_RE.match(value))


def domain_blocked_by(host, blocked_domains):
    """True when host is one of the blocked domains or a subdomain of one."""
    host = (host or "").strip().lower().rstrip(".")
    for domain in blocked_domains:
        if host == domain or host.endswith("." + domain):
            return True
    return False


# --- Delivery retry policy -------------------------------------------------

def retry_backoff_ms(attempts):
    """Delay before retry number `attempts` (1-based): 5m, 20m, 80m ... capped
    at 24h."""
    base = 5 * 60 * 1000
    delay = base * (4 ** max(0, int(attempts) - 1))
    return min(delay, 24 * 60 * 60 * 1000)
