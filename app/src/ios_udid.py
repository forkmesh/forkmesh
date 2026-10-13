"""iOS UDID reader: an over-the-air profile service at /api/ios-udid.

iOS gives apps no way to read a device's UDID, but Safari can install a
"Profile Service" configuration profile. Installing one makes iOS POST the
requested device attributes (UDID, model, OS build) to the profile's URL in a
device-signed PKCS#7 envelope, then follow our 301 back into Safari. The
profile carries no settings and iOS does not keep it installed.

Nothing is stored: the attributes travel back to the page in the redirect's
query string and, when the ForkMesh app started the flow, on to the app through
a ``forkmesh://ios-udid`` link that echoes the app's one-time nonce.

Split out of entry.py so Cloudflare's startup-memory validation (error 10021)
does not pay for this domain; entry.py loads it through a _LazyModule proxy on
the first /api/ios-udid request.
"""

import re
import uuid
from urllib.parse import parse_qs, urlencode, urlsplit


def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by this domain."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


IOS_UDID_PATH = "/api/ios-udid"
IOS_UDID_APP_SCHEME = "forkmesh"
# A signed attribute envelope is a few KB; anything far larger is not iOS.
IOS_UDID_MAX_BODY = 64 * 1024
# Current devices: 8 hex, dash, 16 hex. Devices before iPhone XS: 40 hex.
IOS_UDID_RE = re.compile(r"^(?:[0-9A-Fa-f]{8}-[0-9A-Fa-f]{16}|[0-9A-Fa-f]{40})$")
IOS_PRODUCT_RE = re.compile(r"^[A-Za-z0-9,]{1,32}$")
IOS_VERSION_RE = re.compile(r"^[A-Za-z0-9.]{1,16}$")
IOS_NONCE_RE = re.compile(r"^[A-Za-z0-9_-]{16,64}$")
_PLIST_STRING_RE = re.compile(
    r"<key>\s*([A-Z_]+)\s*</key>\s*<string>([^<]{0,128})</string>")


# CSP, referrer policy and the other browser headers come from
# _apply_browser_security_headers, which covers every HTML response. Its
# strict-origin-when-cross-origin referrer never sends the result page's query
# string (the UDID) to another origin.
_PAGE_HEADERS = {
    "content-type": "text/html; charset=utf-8",
    "cache-control": "no-store, max-age=0",
    "x-robots-tag": "noindex",
}


def _esc(value):
    return (str(value or "").replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;").replace("'", "&#39;"))


def _origin(request):
    parts = urlsplit(str(request.url))
    return "%s://%s" % (parts.scheme, parts.netloc)


def _query(request):
    return parse_qs(urlsplit(str(request.url)).query)


def _param(query, name, pattern):
    value = (query.get(name) or [""])[0].strip()
    return value if pattern.match(value) else ""


def _nonce_query(nonce):
    return ("?" + urlencode({"n": nonce})) if nonce else ""


def ios_udid_attributes(body):
    """Pull UDID/PRODUCT/VERSION out of iOS's signed attribute envelope.

    The PKCS#7 SignedData wraps an XML plist verbatim, so the plist is read
    straight out of the DER bytes. The device signature is not checked: the
    values are only echoed back to the same visitor, never trusted or stored.
    Returns None when the body holds no valid UDID.
    """
    raw = bytes(body or b"")
    start = raw.find(b"<?xml")
    end = raw.find(b"</plist>", start)
    if start < 0 or end < 0:
        return None
    text = raw[start:end].decode("utf-8", "replace")
    values = {key: value.strip() for key, value in _PLIST_STRING_RE.findall(text)}
    udid = values.get("UDID", "")
    if not IOS_UDID_RE.match(udid):
        return None
    product = values.get("PRODUCT", "")
    version = values.get("VERSION", "")
    return {
        "udid": udid.upper(),
        "product": product if IOS_PRODUCT_RE.match(product) else "",
        "version": version if IOS_VERSION_RE.match(version) else "",
    }


def ios_udid_profile(origin, nonce):
    """The unsigned Profile Service payload that asks iOS for the UDID."""
    enroll_url = origin + IOS_UDID_PATH + "/enroll" + _nonce_query(nonce)
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
        '"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
        '<plist version="1.0">\n<dict>\n'
        "  <key>PayloadContent</key>\n  <dict>\n"
        "    <key>URL</key>\n    <string>" + _esc(enroll_url) + "</string>\n"
        "    <key>DeviceAttributes</key>\n    <array>\n"
        "      <string>UDID</string>\n"
        "      <string>PRODUCT</string>\n"
        "      <string>VERSION</string>\n"
        "    </array>\n  </dict>\n"
        "  <key>PayloadOrganization</key>\n  <string>ForkMesh</string>\n"
        "  <key>PayloadDisplayName</key>\n"
        "  <string>ForkMesh device UDID</string>\n"
        "  <key>PayloadDescription</key>\n"
        "  <string>Sends this device's UDID, model and iOS build to "
        "ForkMesh once so they can be shown to you. It installs no settings "
        "and is not kept.</string>\n"
        "  <key>PayloadIdentifier</key>\n"
        "  <string>com.forkmesh.forkmesh.ios-udid</string>\n"
        "  <key>PayloadUUID</key>\n  <string>"
        + str(uuid.uuid4()).upper() + "</string>\n"
        "  <key>PayloadVersion</key>\n  <integer>1</integer>\n"
        "  <key>PayloadType</key>\n  <string>Profile Service</string>\n"
        "</dict>\n</plist>\n"
    )


def ios_udid_app_link(attributes, nonce):
    """The forkmesh:// link that hands the result back to the app."""
    params = {"udid": attributes["udid"]}
    if attributes.get("product"):
        params["product"] = attributes["product"]
    if attributes.get("version"):
        params["version"] = attributes["version"]
    params["n"] = nonce
    return IOS_UDID_APP_SCHEME + "://ios-udid?" + urlencode(params)


_PAGE_STYLE = (
    ":root{color-scheme:dark}"
    "body{margin:0;font:16px/1.5 -apple-system,BlinkMacSystemFont,"
    "'Segoe UI',sans-serif;background:#050505;color:#f5f5f5}"
    "main{max-width:34rem;margin:0 auto;padding:32px 20px 48px}"
    "h1{font-size:1.5rem;margin:0 0 8px}"
    "p,li{color:#c8c8c8}ol{padding-left:1.25rem}li{margin:6px 0}"
    ".card{background:#111;border:1px solid #262626;border-radius:14px;"
    "padding:18px;margin:20px 0}"
    ".udid{font:600 1.15rem/1.4 ui-monospace,SFMono-Regular,Menlo,monospace;"
    "word-break:break-all;-webkit-user-select:all;user-select:all;"
    "color:#fff;margin:6px 0 0}"
    ".label{font-size:.8rem;text-transform:uppercase;letter-spacing:.06em;"
    "color:#8a8a8a;margin:0}"
    ".row{display:flex;gap:12px;flex-wrap:wrap;margin-top:16px}"
    ".btn{display:inline-flex;align-items:center;justify-content:center;"
    "min-height:46px;padding:0 18px;border-radius:10px;font-weight:600;"
    "font-size:1rem;text-decoration:none;border:1px solid #3a3a3a;"
    "background:transparent;color:#f5f5f5;cursor:pointer}"
    ".btn.primary{background:#f5f5f5;color:#050505;border-color:#f5f5f5}"
    ".muted{font-size:.9rem;color:#8a8a8a}"
    "dl{margin:14px 0 0;display:grid;grid-template-columns:auto 1fr;"
    "gap:4px 14px}dt{color:#8a8a8a}dd{margin:0}"
)


def _page(title, body_html, script=""):
    return (
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, "
        "initial-scale=1\"><title>" + _esc(title) + " · ForkMesh</title>"
        "<meta name=\"robots\" content=\"noindex\">"
        "<style>" + _PAGE_STYLE + "</style></head><body><main>"
        + body_html + "</main>"
        + ("<script>" + script + "</script>" if script else "")
        + "</body></html>"
    )


def _landing_page(nonce):
    profile_href = IOS_UDID_PATH + "/profile" + _nonce_query(nonce)
    body = (
        "<h1>Find this iPhone's UDID</h1>"
        "<p>Apple does not let apps read the UDID, but iOS will send it to a "
        "page you trust when you install a one-time profile. Do this in "
        "Safari on the iPhone or iPad whose UDID you need.</p>"
        "<div class=\"card\"><ol>"
        "<li>Tap <strong>Download profile</strong> and choose "
        "<strong>Allow</strong>.</li>"
        "<li>Open <strong>Settings</strong>, tap <strong>Profile "
        "Downloaded</strong>, then <strong>Install</strong>. iOS labels it "
        "“Not Verified” because it is not signed.</li>"
        "<li>iOS returns you here with the UDID"
        + (", and a button to send it to the ForkMesh app" if nonce else "")
        + ".</li></ol>"
        "<div class=\"row\"><a class=\"btn primary\" href=\""
        + _esc(profile_href) + "\">Download profile</a></div></div>"
        "<p class=\"muted\">The profile asks for the UDID, model and iOS "
        "build only. It installs no settings, iOS does not keep it, and "
        "ForkMesh does not store what the device sends.</p>"
    )
    return _page("Find your UDID", body)


def _result_page(attributes, nonce):
    details = ""
    if attributes["product"]:
        details += "<dt>Model</dt><dd>" + _esc(attributes["product"]) + "</dd>"
    if attributes["version"]:
        details += ("<dt>iOS build</dt><dd>" + _esc(attributes["version"])
                    + "</dd>")
    app_button = ""
    if nonce:
        app_button = (
            "<a class=\"btn primary\" href=\""
            + _esc(ios_udid_app_link(attributes, nonce))
            + "\">Open in ForkMesh</a>")
    body = (
        "<h1>Your device UDID</h1>"
        "<div class=\"card\"><p class=\"label\">UDID</p>"
        "<p class=\"udid\" id=\"udid\">" + _esc(attributes["udid"]) + "</p>"
        + ("<dl>" + details + "</dl>" if details else "")
        + "<div class=\"row\">" + app_button
        + "<button type=\"button\" class=\"btn\" id=\"copy\">Copy UDID"
        "</button></div></div>"
        "<p class=\"muted\">ForkMesh did not store this. The profile that "
        "sent it is not kept on the device.</p>"
    )
    script = (
        "document.getElementById('copy').addEventListener('click',function(){"
        "var b=this,t=document.getElementById('udid').textContent;"
        "navigator.clipboard.writeText(t).then(function(){"
        "b.textContent='Copied';},function(){b.textContent='Select and copy';"
        "});});"
    )
    return _page("Your UDID", body, script)


async def ios_udid_handler(env, request, path):
    """GET page / GET profile / POST enroll for the iOS UDID profile service."""
    method = method_name(request)
    tail = path[len(IOS_UDID_PATH):].strip("/")
    query = _query(request)
    nonce = _param(query, "n", IOS_NONCE_RE)

    if tail == "enroll":
        if method != "POST":
            return json_response({"error": "method_not_allowed"}, status=405)
        body = bytes(await request.bytes())
        attributes = (ios_udid_attributes(body)
                      if len(body) <= IOS_UDID_MAX_BODY else None)
        if attributes is None:
            return json_response({"error": "invalid_device_attributes"},
                                 status=400)
        params = dict(attributes)
        if nonce:
            params["n"] = nonce
        location = (_origin(request) + IOS_UDID_PATH + "?"
                    + urlencode({k: v for k, v in params.items() if v}))
        # iOS only follows a 301 from a profile service back into Safari.
        return Response("", status=301, headers={
            "location": location,
            "cache-control": "no-store",
        })

    if method not in ("GET", "HEAD"):
        return json_response({"error": "method_not_allowed"}, status=405)

    if tail == "profile":
        return Response(ios_udid_profile(_origin(request), nonce), headers={
            "content-type": "application/x-apple-aspen-config",
            "content-disposition":
                "attachment; filename=\"forkmesh-udid.mobileconfig\"",
            "cache-control": "no-store",
            "x-content-type-options": "nosniff",
        })

    if tail:
        return json_response({"error": "not_found"}, status=404)

    udid = _param(query, "udid", IOS_UDID_RE)
    if udid:
        attributes = {
            "udid": udid.upper(),
            "product": _param(query, "product", IOS_PRODUCT_RE),
            "version": _param(query, "version", IOS_VERSION_RE),
        }
        html = _result_page(attributes, nonce)
    else:
        html = _landing_page(nonce)
    return Response(html, headers=dict(_PAGE_HEADERS))
