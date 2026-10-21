"""Custom comment reactions: the instance's and organizations' own images.

The built-in vocabulary (comment_reactions.CATALOG) lives in code. On top of
it an instance administrator can add reactions for the whole instance, and an
organization owner can add reactions for the repositories the organization
owns: a slug, a label, one small image (a sanitised SVG, or a PNG, GIF or
WebP within the size and dimension caps), an optional animated variant, an
order and an enabled switch. A comment reaction that uses one is stored as
``custom:<slug>``.

Images are served from this origin by content address
(``/api/reactions/custom/asset/<digest>.<ext>``) with a year-long immutable
cache: a new upload is a new address, so nothing is ever stale. The list the
picker merges (``GET /api/reactions/custom?owner=``) is small and cached for a
minute. Deleting a custom reaction keeps the reactions already made with it;
they render as a text fallback chip (comment_reactions attaches
``custom: null`` to their tallies).
"""

import base64
import hashlib
import json
import re
import xml.etree.ElementTree as ET
from urllib.parse import parse_qs, urlparse

PREFIX = "custom:"
SLUG_RE = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,30}[a-z0-9])?$")
EMOJI_RE = re.compile(r"^custom:[a-z0-9](?:[a-z0-9-]{0,30}[a-z0-9])?$")
ASSET_RE = re.compile(r"^[0-9a-f]{40}$")
ORG_RE = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,62})$")
MAX_LABEL = 40
MAX_PER_SCOPE = 50
MAX_SVG_BYTES = 32 * 1024
MAX_RASTER_BYTES = 64 * 1024
MAX_DIMENSION = 256
MAX_BODY_BYTES = 256 * 1024
MAX_POSITION = 10000
CONTENT_TYPES = {"svg": "image/svg+xml", "png": "image/png",
                 "gif": "image/gif", "webp": "image/webp"}
LIST_CACHE = "public, max-age=60, stale-while-revalidate=600"
ASSET_CACHE = "public, max-age=31536000, immutable"
PRIVATE_CACHE = "private, no-store"
ASSET_CSP = ("default-src 'none'; style-src 'unsafe-inline'; "
             "img-src 'none'; sandbox")
CATALOG_SECONDS = 30.0
RATE_WINDOW_MS = 60 * 1000
RATE_MAX_PER_WINDOW = 30
_catalogs = {}

SVG_NS = "http://www.w3.org/2000/svg"
XLINK_NS = "http://www.w3.org/1999/xlink"
# Drawing elements only. No script, style, foreignObject, image, a, set,
# iframe, use of external documents or anything that can load or run code.
SVG_ELEMENTS = frozenset({
    "svg", "g", "defs", "title", "desc", "symbol", "use", "path", "rect",
    "circle", "ellipse", "line", "polyline", "polygon", "text", "tspan",
    "linearGradient", "radialGradient", "stop", "clipPath", "mask",
    "pattern", "filter", "feGaussianBlur", "feOffset", "feMerge",
    "feMergeNode", "feFlood", "feComposite", "feColorMatrix", "feBlend",
    "feDropShadow", "animate", "animateTransform",
})
SVG_ATTRIBUTES = frozenset({
    "id", "class", "viewBox", "width", "height", "x", "y", "x1", "y1", "x2",
    "y2", "cx", "cy", "r", "rx", "ry", "d", "points", "fill", "fill-opacity",
    "fill-rule", "stroke", "stroke-width", "stroke-linecap",
    "stroke-linejoin", "stroke-miterlimit", "stroke-dasharray",
    "stroke-dashoffset", "stroke-opacity", "opacity", "transform",
    "gradientUnits", "gradientTransform", "spreadMethod", "offset",
    "stop-color", "stop-opacity", "clip-path", "clip-rule", "mask",
    "clipPathUnits", "maskUnits", "maskContentUnits", "patternUnits",
    "patternContentUnits", "patternTransform", "preserveAspectRatio",
    "font-family", "font-size", "font-weight", "font-style",
    "text-anchor", "dominant-baseline", "letter-spacing", "paint-order",
    "filter", "filterUnits", "primitiveUnits", "in", "in2", "result",
    "stdDeviation", "dx", "dy", "flood-color", "flood-opacity", "operator",
    "k1", "k2", "k3", "k4", "values", "mode", "type", "attributeName",
    "attributeType", "from", "to", "by", "dur", "begin", "end",
    "repeatCount", "repeatDur", "keyTimes", "keySplines", "calcMode",
    "additive", "accumulate", "fx", "fy", "fr", "style", "href",
    "version", "xml:space", "vector-effect", "shape-rendering",
    "color-interpolation-filters",
})
# animate may only move geometry and paint, never a link or a handler.
ANIMATABLE = frozenset({
    "opacity", "fill", "stroke", "transform", "x", "y", "cx", "cy", "r",
    "rx", "ry", "d", "width", "height", "fill-opacity", "stroke-opacity",
    "stroke-width", "stroke-dashoffset", "offset", "stop-color", "points",
})
URL_RE = re.compile(r"url\s*\(\s*(['\"]?)(.*?)\1\s*\)", re.I | re.S)
STYLE_BLOCKED = re.compile(
    r"@import|expression\s*\(|javascript:|behavior\s*:|-moz-binding|"
    r"data\s*:|\\", re.I)


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


class ImageError(ValueError):
    pass


def clean_slug(value):
    slug = str(value or "").strip().lower() if isinstance(value, str) else ""
    return slug if SLUG_RE.fullmatch(slug) else ""


def clean_label(value):
    label = " ".join(str(value or "").split()) if isinstance(value, str) else ""
    label = "".join(ch for ch in label if ch.isprintable())
    return label[:MAX_LABEL]


def is_custom(emoji):
    return isinstance(emoji, str) and bool(EMOJI_RE.fullmatch(emoji))


def slug_of(emoji):
    return emoji[len(PREFIX):] if is_custom(emoji) else ""


def _local(name):
    return name.rsplit("}", 1)[-1]


def _attribute_name(name):
    if name.startswith("{%s}" % XLINK_NS):
        return "href" if _local(name) == "href" else ""
    if name == "{http://www.w3.org/XML/1998/namespace}space":
        return "xml:space"
    if name.startswith("{"):
        return ""
    return name


def _url_ok(value):
    for match in URL_RE.finditer(value):
        if not match.group(2).strip().startswith("#"):
            return False
    return True


def _clean_attribute(element_name, name, value):
    if name.lower().startswith("on"):
        return None
    if name not in SVG_ATTRIBUTES:
        return None
    value = str(value)
    lowered = re.sub(r"\s", "", value).lower()
    if "javascript:" in lowered or "data:" in lowered \
            or "vbscript:" in lowered:
        return None
    if name == "href":
        # Only a fragment inside this same image: no other document, no data:
        return value if re.fullmatch(r"#[A-Za-z_][\w.-]{0,63}", value) else None
    if name == "style" and STYLE_BLOCKED.search(value):
        return None
    if "url(" in lowered and not _url_ok(value):
        return None
    if element_name in ("animate", "animateTransform") \
            and name == "attributeName" and value not in ANIMATABLE:
        raise ImageError("animate_target")
    if name in ("values", "from", "to", "by") and "url(" in lowered:
        return None
    return value


def sanitize_svg(text):
    """A drawing-only copy of an SVG document, or ImageError.

    Parsing (not pattern matching) decides what is an element. Anything that
    can run code or load something else - scripts, event handlers,
    foreignObject, images, external or data: references, style blocks,
    entity declarations - is removed or refused."""
    if not isinstance(text, str):
        raise ImageError("invalid_image")
    raw = text.encode("utf-8")
    if len(raw) > MAX_SVG_BYTES:
        raise ImageError("image_too_large")
    if re.search(r"<!DOCTYPE|<!ENTITY|<\?xml-stylesheet", text, re.I):
        raise ImageError("svg_doctype")
    try:
        root = ET.fromstring(text)
    except ET.ParseError:
        raise ImageError("invalid_svg")
    if root.tag not in ("{%s}svg" % SVG_NS, "svg"):
        raise ImageError("invalid_svg")

    def clean(element):
        tag = _local(element.tag)
        element.tag = "{%s}%s" % (SVG_NS, tag)
        kept = {}
        for name, value in element.attrib.items():
            plain = _attribute_name(name)
            if not plain:
                continue
            cleaned = _clean_attribute(tag, plain, value)
            if cleaned is None:
                continue
            if plain == "href":
                kept["href"] = cleaned
            elif plain == "xml:space":
                kept["{http://www.w3.org/XML/1998/namespace}space"] = cleaned
            else:
                kept[plain] = cleaned
        element.attrib.clear()
        element.attrib.update(kept)
        for child in list(element):
            if not isinstance(child.tag, str) or _local(child.tag) not in SVG_ELEMENTS:
                element.remove(child)
                continue
            clean(child)

    clean(root)
    root.tag = "{%s}svg" % SVG_NS
    if "viewBox" not in root.attrib and not (
            root.attrib.get("width") and root.attrib.get("height")):
        raise ImageError("svg_size")
    ET.register_namespace("", SVG_NS)
    out = ET.tostring(root, encoding="unicode", short_empty_elements=True)
    if len(out.encode("utf-8")) > MAX_SVG_BYTES:
        raise ImageError("image_too_large")
    return out


def _png_size(data):
    if len(data) < 24 or data[12:16] != b"IHDR":
        raise ImageError("invalid_image")
    return int.from_bytes(data[16:20], "big"), int.from_bytes(data[20:24], "big")


def _gif_size(data):
    if len(data) < 10:
        raise ImageError("invalid_image")
    return int.from_bytes(data[6:8], "little"), int.from_bytes(data[8:10], "little")


def _webp_size(data):
    if len(data) < 30:
        raise ImageError("invalid_image")
    chunk = data[12:16]
    if chunk == b"VP8X":
        return (int.from_bytes(data[24:27], "little") + 1,
                int.from_bytes(data[27:30], "little") + 1)
    if chunk == b"VP8L":
        bits = int.from_bytes(data[21:25], "little")
        return (bits & 0x3FFF) + 1, ((bits >> 14) & 0x3FFF) + 1
    if chunk == b"VP8 ":
        return (int.from_bytes(data[26:28], "little") & 0x3FFF,
                int.from_bytes(data[28:30], "little") & 0x3FFF)
    raise ImageError("invalid_image")


def sniff_raster(data):
    """(kind, width, height) of a PNG, GIF or WebP, or ImageError."""
    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        kind, size = "png", _png_size(data)
    elif data[:6] in (b"GIF87a", b"GIF89a"):
        kind, size = "gif", _gif_size(data)
    elif data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        kind, size = "webp", _webp_size(data)
    else:
        raise ImageError("unsupported_image")
    width, height = size
    if not (0 < width <= MAX_DIMENSION and 0 < height <= MAX_DIMENSION):
        raise ImageError("image_dimensions")
    return kind, width, height


def clean_image(payload):
    """(kind, bytes) of an uploaded image, or ImageError.

    ``payload`` is ``{"svg": "<svg ...>"}`` or ``{"data": "<base64>"}``
    (a data: URL prefix is accepted and dropped)."""
    if not isinstance(payload, dict):
        raise ImageError("invalid_image")
    if isinstance(payload.get("svg"), str):
        return "svg", sanitize_svg(payload["svg"]).encode("utf-8")
    data = payload.get("data")
    if not isinstance(data, str) or not data:
        raise ImageError("invalid_image")
    data = re.sub(r"^data:[\w/+.-]+;base64,", "", data.strip())
    if len(data) > (MAX_RASTER_BYTES * 4) // 3 + 8:
        raise ImageError("image_too_large")
    try:
        raw = base64.b64decode(data, validate=True)
    except Exception:
        raise ImageError("invalid_image")
    if raw.lstrip()[:1] == b"<":
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            raise ImageError("invalid_svg")
        return "svg", sanitize_svg(text).encode("utf-8")
    if len(raw) > MAX_RASTER_BYTES:
        raise ImageError("image_too_large")
    kind, _w, _h = sniff_raster(raw)
    return kind, raw


def digest_of(data):
    return hashlib.sha256(data).hexdigest()[:40]


def asset_url(digest, kind):
    return "/api/reactions/custom/asset/%s.%s" % (digest, kind)


def _item(row, with_admin=False):
    item = {
        "slug": str(row.get("slug") or ""),
        "emoji": PREFIX + str(row.get("slug") or ""),
        "label": str(row.get("label") or ""),
        "src": asset_url(row.get("digest"), row.get("kind")),
        "animatedSrc": asset_url(row.get("anim_digest"), row.get("anim_kind"))
        if row.get("anim_digest") else "",
        "scope": "instance" if row.get("scope") == "instance" else "org",
    }
    if with_admin:
        item["position"] = int(row.get("position") or 0)
        item["enabled"] = bool(int(row.get("enabled") or 0))
        item["updatedAt"] = int(row.get("updated_at") or 0)
    return item


async def _org_scope(env, owner):
    owner = str(owner or "").strip().lower()
    if not owner or not ORG_RE.fullmatch(owner):
        return ""
    try:
        org_bi, row = await _org_row(env, owner)  # noqa: F821
    except Exception:
        return ""
    return "org:" + org_bi if row else ""


def forget():
    _catalogs.clear()


async def catalog(env, owner=""):
    """slug -> item for every enabled custom reaction usable on ``owner``'s
    repositories: the instance's, then the owner organization's (which win
    on a shared slug). Remembered for 30 seconds per owner."""
    key = str(owner or "").strip().lower()
    now = int(Date.now())  # noqa: F821
    kept = _catalogs.get(key)
    if kept and now - kept[0] < CATALOG_SECONDS * 1000:
        return kept[1]
    scopes = ["instance"]
    org_scope = await _org_scope(env, key) if key else ""
    if org_scope:
        scopes.append(org_scope)
    rows = await d1_all(  # noqa: F821
        env,
        "SELECT scope, slug, label, kind, digest, anim_kind, anim_digest, "
        "position, enabled, updated_at FROM custom_reactions WHERE enabled=1 "
        "AND scope IN (%s) ORDER BY position, slug" % ",".join(
            "?" for _ in scopes), *scopes)
    found = {}
    for row in sorted(rows or [], key=lambda r: r.get("scope") != "instance"):
        found[str(row.get("slug"))] = (int(row.get("position") or 0), _item(row))
    ordered = {slug: entry[1] for slug, entry in sorted(
        found.items(), key=lambda pair: (pair[1][0], pair[0]))}
    if len(_catalogs) > 256:
        _catalogs.clear()
    _catalogs[key] = (now, ordered)
    return ordered


async def resolve(env, owner, emoji):
    if not is_custom(emoji):
        return None
    return (await catalog(env, owner)).get(slug_of(emoji))


def public_meta(item):
    if not item:
        return None
    return {"label": item["label"], "src": item["src"],
            "animatedSrc": item.get("animatedSrc") or ""}


async def _public_list(env, request):
    query = parse_qs(urlparse(str(request.url)).query)
    owner = str((query.get("owner") or [""])[0] or "").strip().lower()
    if owner and not ORG_RE.fullmatch(owner):
        owner = ""
    items = list((await catalog(env, owner)).values())
    return json_response(  # noqa: F821
        {"ok": True, "reactions": items}, cache_control=LIST_CACHE)


def _asset_response(data, content_type):
    headers = {"content-type": content_type, "cache-control": ASSET_CACHE,
               "x-content-type-options": "nosniff",
               "content-security-policy": ASSET_CSP,
               "cross-origin-resource-policy": "same-origin"}
    return JsResponse.new(  # noqa: F821
        Uint8Array.new(_to_js(bytes(data))),  # noqa: F821
        to_js({"status": 200, "headers": headers}))  # noqa: F821


async def asset_handler(env, request, digest, ext):
    if method_name(request) not in ("GET", "HEAD"):  # noqa: F821
        return json_response({"error": "method_not_allowed"}, status=405,  # noqa: F821
                             cache_control=PRIVATE_CACHE)
    if not ASSET_RE.fullmatch(digest or "") or ext not in CONTENT_TYPES:
        return json_response({"error": "not_found"}, status=404,  # noqa: F821
                             cache_control=PRIVATE_CACHE)
    await ensure_schema(env)  # noqa: F821
    row = await d1_first(  # noqa: F821
        env, "SELECT kind, asset FROM custom_reactions WHERE digest=? "
        "UNION ALL SELECT anim_kind AS kind, anim_asset AS asset "
        "FROM custom_reactions WHERE anim_digest=? LIMIT 1", digest, digest)
    if not row or row.get("kind") != ext:
        return json_response({"error": "not_found"}, status=404,  # noqa: F821
                             cache_control="public, max-age=60")
    return _asset_response(base64.b64decode(row.get("asset") or ""),
                           CONTENT_TYPES[ext])


async def _rate_limited(env, account_bi, now):
    key = "custom-reactions:" + account_bi
    row = await d1_first(  # noqa: F821
        env,
        "INSERT INTO comment_extras_rate (k, window_start, count) "
        "VALUES (?,?,1) ON CONFLICT(k) DO UPDATE SET "
        "count=CASE WHEN ?-comment_extras_rate.window_start>=? "
        "THEN 1 ELSE comment_extras_rate.count+1 END, "
        "window_start=CASE WHEN ?-comment_extras_rate.window_start>=? "
        "THEN ? ELSE comment_extras_rate.window_start END "
        "RETURNING count", key, now, now, RATE_WINDOW_MS, now,
        RATE_WINDOW_MS, now)
    return int((row or {}).get("count") or 1) > RATE_MAX_PER_WINDOW


async def manage_scope(env, account, value):
    """The stored scope an account may manage for ``value`` ("instance" or
    "org:<name>"), or "" when it may not."""
    value = str(value or "").strip().lower()
    if value == "instance":
        return "instance" if await _is_admin(env, account) else ""  # noqa: F821
    if not value.startswith("org:"):
        return ""
    org = value[4:]
    if not ORG_RE.fullmatch(org):
        return ""
    org_bi, row = await _org_row(env, org)  # noqa: F821
    if not row:
        return ""
    role = await _org_role(env, org_bi, account)  # noqa: F821
    return "org:" + org_bi if role == "owner" else ""


async def _scopes_for(env, account):
    """Every scope this account manages, for the settings page."""
    scopes = []
    if await _is_admin(env, account):  # noqa: F821
        scopes.append({"scope": "instance", "label": "This instance"})
    try:
        member_bi = await blind_index(env, account)  # noqa: F821
        rows = await d1_all(  # noqa: F821
            env, "SELECT o.name AS name FROM org_members m JOIN orgs o "
            "ON o.org_bi=m.org_bi WHERE m.member_bi=? AND m.role='owner' "
            "ORDER BY o.name LIMIT 50", member_bi)
    except Exception:
        rows = []
    for row in rows or []:
        name = str(row.get("name") or "").strip().lower()
        if ORG_RE.fullmatch(name):
            scopes.append({"scope": "org:" + name, "label": name})
    return scopes


async def _manage_list(env, request, account):
    query = parse_qs(urlparse(str(request.url)).query)
    wanted = str((query.get("scope") or [""])[0] or "")
    scopes = await _scopes_for(env, account)
    if not wanted:
        return json_response({"ok": True, "scopes": scopes, "reactions": []},  # noqa: F821
                             cache_control=PRIVATE_CACHE)
    scope = await manage_scope(env, account, wanted)
    if not scope:
        return json_response({"error": "forbidden"}, status=403,  # noqa: F821
                             cache_control=PRIVATE_CACHE)
    rows = await d1_all(  # noqa: F821
        env, "SELECT scope, slug, label, kind, digest, anim_kind, anim_digest, "
        "position, enabled, updated_at FROM custom_reactions WHERE scope=? "
        "ORDER BY position, slug", scope)
    return json_response(  # noqa: F821
        {"ok": True, "scope": wanted.lower(), "scopes": scopes,
         "limits": {"perScope": MAX_PER_SCOPE, "svgBytes": MAX_SVG_BYTES,
                    "rasterBytes": MAX_RASTER_BYTES,
                    "dimension": MAX_DIMENSION},
         "reactions": [_item(row, True) for row in rows or []]},
        cache_control=PRIVATE_CACHE)


def _error(code, status=400):
    return json_response({"error": code}, status=status,  # noqa: F821
                         cache_control=PRIVATE_CACHE)


async def _save(env, scope, data, account, now):
    slug = clean_slug(data.get("slug"))
    if not slug:
        return _error("invalid_slug")
    existing = await d1_first(  # noqa: F821
        env, "SELECT kind, digest, anim_kind, anim_digest, anim_asset, label, "
        "position, enabled FROM custom_reactions WHERE scope=? AND slug=?",
        scope, slug)
    label = clean_label(data.get("label")) if "label" in data else (
        str((existing or {}).get("label") or ""))
    if not label:
        return _error("invalid_label")
    try:
        position = int(data.get("position", (existing or {}).get("position") or 0))
    except (TypeError, ValueError):
        return _error("invalid_position")
    position = max(0, min(MAX_POSITION, position))
    enabled = data.get("enabled", bool(int((existing or {}).get("enabled", 1))))
    if not isinstance(enabled, bool):
        return _error("invalid_state")
    image = data.get("image")
    if existing is None and image is None:
        return _error("image_required")
    if existing is None:
        count = await d1_first(  # noqa: F821
            env, "SELECT COUNT(*) AS n FROM custom_reactions WHERE scope=?",
            scope)
        if int((count or {}).get("n") or 0) >= MAX_PER_SCOPE:
            return _error("scope_full", 409)
    try:
        if image is not None:
            kind, raw = clean_image(image)
        else:
            kind, raw = None, None
        animated = data.get("animated", "keep")
        if animated is None or animated is False:
            anim = ("", b"")
        elif animated == "keep":
            anim = None
        else:
            anim = clean_image(animated)
    except ImageError as error:
        return _error(str(error))
    sealed = await encrypt_row(env, {"by": account})  # noqa: F821
    if existing is None:
        anim = anim or ("", b"")
        await d1_run(  # noqa: F821
            env,
            "INSERT INTO custom_reactions (scope, slug, label, kind, asset, "
            "digest, anim_kind, anim_asset, anim_digest, position, enabled, "
            "created_at, updated_at, data) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            scope, slug, label, kind, base64.b64encode(raw).decode("ascii"),
            digest_of(raw), anim[0],
            base64.b64encode(anim[1]).decode("ascii") if anim[1] else "",
            digest_of(anim[1]) if anim[1] else "", position,
            1 if enabled else 0, now, now, sealed)
    else:
        sets = ["label=?", "position=?", "enabled=?", "updated_at=?", "data=?"]
        args = [label, position, 1 if enabled else 0, now, sealed]
        if raw is not None:
            sets += ["kind=?", "asset=?", "digest=?"]
            args += [kind, base64.b64encode(raw).decode("ascii"),
                     digest_of(raw)]
        if anim is not None:
            sets += ["anim_kind=?", "anim_asset=?", "anim_digest=?"]
            args += [anim[0],
                     base64.b64encode(anim[1]).decode("ascii") if anim[1] else "",
                     digest_of(anim[1]) if anim[1] else ""]
        await d1_run(  # noqa: F821
            env, "UPDATE custom_reactions SET %s WHERE scope=? AND slug=?"
            % ", ".join(sets), *(args + [scope, slug]))
    forget()
    row = await d1_first(  # noqa: F821
        env, "SELECT scope, slug, label, kind, digest, anim_kind, anim_digest, "
        "position, enabled, updated_at FROM custom_reactions WHERE scope=? "
        "AND slug=?", scope, slug)
    return json_response({"ok": True, "reaction": _item(row, True)},  # noqa: F821
                         cache_control=PRIVATE_CACHE)


async def _manage_write(env, request):
    try:
        data = await bounded_json_request(request, MAX_BODY_BYTES)  # noqa: F821
    except Exception:
        return _error("invalid_json")
    if not isinstance(data, dict):
        return _error("invalid_json")
    account_bi, rec = await _account_session_record(env, request, data)  # noqa: F821
    account = clean_string((rec or {}).get("name"), MAX_NODE_NAME).lower()  # noqa: F821
    if not rec or not account_bi or not account:
        return _error("unauthorized", 401)
    scope = await manage_scope(env, account, data.get("scope"))
    if not scope:
        return _error("forbidden", 403)
    now = int(Date.now())  # noqa: F821
    if await _rate_limited(env, account_bi, now):
        return _error("rate_limited", 429)
    action = str(data.get("action") or "save")
    if action == "delete":
        slug = clean_slug(data.get("slug"))
        if not slug:
            return _error("invalid_slug")
        await d1_run(  # noqa: F821
            env, "DELETE FROM custom_reactions WHERE scope=? AND slug=?",
            scope, slug)
        forget()
        return json_response({"ok": True, "deleted": slug},  # noqa: F821
                             cache_control=PRIVATE_CACHE)
    if action != "save":
        return _error("invalid_action")
    return await _save(env, scope, data, account, now)


async def handler(env, request, rest=""):
    await ensure_schema(env)  # noqa: F821
    method = method_name(request)  # noqa: F821
    if rest == "":
        if method != "GET":
            return _error("method_not_allowed", 405)
        return await _public_list(env, request)
    if rest == "manage":
        if method == "POST":
            return await _manage_write(env, request)
        if method != "GET":
            return _error("method_not_allowed", 405)
        account_bi, rec = await _account_session_record(env, request, None)  # noqa: F821
        account = clean_string((rec or {}).get("name"), MAX_NODE_NAME).lower()  # noqa: F821
        if not rec or not account:
            return _error("unauthorized", 401)
        return await _manage_list(env, request, account)
    return _error("not_found", 404)
