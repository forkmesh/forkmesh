"""The iOS UDID profile service at /api/ios-udid (issue #721).

iOS never lets an app read the UDID, so Safari installs a one-time Profile
Service profile, iOS posts the device attributes to /api/ios-udid/enroll, and
the 301 carries them back to the page and, via forkmesh://, into the app.
"""

import asyncio
import importlib
import sys
from pathlib import Path
from urllib.parse import parse_qs, urlsplit


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
REPO = ROOT.parent
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

NONCE = "abcdefghijklmnop0123456789_-XYZ"
UDID = "00008030-001A2D3C3E3A802E"


class _Response:
    def __init__(self, body="", status=200, headers=None):
        self.body = body
        self.status = status
        self.headers = dict(headers or {})


class _Request:
    def __init__(self, url, method="GET", body=b""):
        self.url = url
        self.method = method
        self._body = body

    async def bytes(self):
        return self._body


def _module():
    module = importlib.import_module("ios_udid")
    module._bind_runtime({
        "Response": _Response,
        "json_response": lambda data, status=200, **_: _Response(
            data, status=status),
        "method_name": lambda request: request.method,
    })
    return module


def _call(url, method="GET", body=b""):
    module = _module()
    path = urlsplit(url).path
    return asyncio.run(
        module.ios_udid_handler(None, _Request(url, method, body), path))


def _envelope(udid=UDID, product="iPhone15,2", version="22A3354"):
    """What iOS posts: a DER PKCS#7 blob with the XML plist inside."""
    plist = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
        '"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
        '<plist version="1.0"><dict>'
        "<key>PRODUCT</key><string>%s</string>"
        "<key>UDID</key><string>%s</string>"
        "<key>VERSION</key><string>%s</string>"
        "</dict></plist>" % (product, udid, version)
    ).encode()
    return b"\x30\x80\x06\x09\x2a\x86\x48\x86\xf7\x0d\x01\x07\x02" + plist + (
        b"\xa0\x82\x03\x00" + bytes(range(256)))


def test_profile_asks_ios_for_udid_and_posts_back_with_the_nonce():
    response = _call("https://app.forkmesh.com/api/ios-udid/profile?n=" + NONCE)
    assert response.status == 200
    assert response.headers["content-type"] == "application/x-apple-aspen-config"
    assert "attachment" in response.headers["content-disposition"]
    body = response.body
    assert "<string>Profile Service</string>" in body
    assert ("<string>https://app.forkmesh.com/api/ios-udid/enroll?n="
            + NONCE + "</string>") in body
    for attribute in ("UDID", "PRODUCT", "VERSION"):
        assert "<string>%s</string>" % attribute in body
    # Only what the page shows is requested: no serial, IMEI or MEID.
    for attribute in ("SERIAL", "IMEI", "MEID", "ICCID"):
        assert attribute not in body


def test_enroll_redirects_attributes_back_to_the_page_with_a_301():
    response = _call(
        "https://app.forkmesh.com/api/ios-udid/enroll?n=" + NONCE,
        method="POST", body=_envelope(udid=UDID.lower()))
    assert response.status == 301
    location = urlsplit(response.headers["location"])
    assert (location.scheme, location.netloc, location.path) == (
        "https", "app.forkmesh.com", "/api/ios-udid")
    assert parse_qs(location.query) == {
        "udid": [UDID], "product": ["iPhone15,2"], "version": ["22A3354"],
        "n": [NONCE],
    }


def test_enroll_accepts_legacy_forty_hex_udids():
    legacy = "a" * 40
    response = _call("https://app.forkmesh.com/api/ios-udid/enroll",
                     method="POST", body=_envelope(udid=legacy))
    assert response.status == 301
    assert parse_qs(urlsplit(response.headers["location"]).query)["udid"] == [
        legacy.upper()]


def test_enroll_rejects_bodies_without_a_valid_udid():
    for body in (b"", b"not a plist", _envelope(udid="<script>"),
                 _envelope(udid="1234")):
        response = _call("https://app.forkmesh.com/api/ios-udid/enroll",
                         method="POST", body=body)
        assert response.status == 400, body[:40]
    oversized = _envelope() + b"\x00" * (70 * 1024)
    assert _call("https://app.forkmesh.com/api/ios-udid/enroll",
                 method="POST", body=oversized).status == 400
    assert _call("https://app.forkmesh.com/api/ios-udid/enroll").status == 405


def test_enroll_drops_malformed_product_and_version():
    response = _call("https://app.forkmesh.com/api/ios-udid/enroll",
                     method="POST",
                     body=_envelope(product="iPhone 15\"", version="x" * 40))
    query = parse_qs(urlsplit(response.headers["location"]).query)
    assert query == {"udid": [UDID]}


def test_landing_page_links_the_profile_and_keeps_the_nonce():
    response = _call("https://app.forkmesh.com/api/ios-udid?n=" + NONCE)
    assert response.status == 200
    assert response.headers["content-type"].startswith("text/html")
    assert response.headers["cache-control"].startswith("no-store")
    assert 'href="/api/ios-udid/profile?n=' + NONCE + '"' in response.body
    assert "Download profile" in response.body


def test_landing_page_ignores_a_malformed_nonce():
    response = _call("https://app.forkmesh.com/api/ios-udid?n=%3Cscript%3E")
    assert 'href="/api/ios-udid/profile"' in response.body
    assert "<script>" not in response.body.split("</main>")[0]


def test_result_page_shows_udid_and_hands_it_to_the_app():
    response = _call(
        "https://app.forkmesh.com/api/ios-udid?udid=" + UDID
        + "&product=iPhone15,2&version=22A3354&n=" + NONCE)
    body = response.body
    assert 'id="udid">' + UDID + "<" in body
    assert "iPhone15,2" in body and "22A3354" in body
    assert "Copy UDID" in body
    assert ('href="forkmesh://ios-udid?udid=' + UDID
            + "&amp;product=iPhone15%2C2&amp;version=22A3354&amp;n="
            + NONCE + '"') in body


def test_result_page_without_nonce_has_no_app_button():
    body = _call("https://app.forkmesh.com/api/ios-udid?udid=" + UDID).body
    assert UDID in body
    assert "forkmesh://" not in body


def test_result_page_never_echoes_an_invalid_udid():
    body = _call(
        "https://app.forkmesh.com/api/ios-udid?udid=%3Cimg%20src%3Dx%3E").body
    assert "<img" not in body
    assert "Download profile" in body


def test_route_is_wired_lazily():
    entry = (SRC / "entry.py").read_text(encoding="utf-8")
    runtime = (SRC / "app_runtime.py").read_text(encoding="utf-8")
    footprint = (REPO / "world" / "tools" / "build_worker_footprint.py"
                 ).read_text(encoding="utf-8")
    assert '_ios_udid = _LazyModule("ios_udid")' in entry
    assert 'ios_udid_handler = _ios_udid.export("ios_udid_handler")' in entry
    assert "import ios_udid" not in entry
    assert "return await ios_udid_handler(self.env, request, url.path)" in runtime
    assert '"ios_udid.py",' in footprint
