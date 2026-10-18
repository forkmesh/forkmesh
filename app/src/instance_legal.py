import html
import json
import re
import time
from urllib.parse import urlsplit

import instance_pages

SETTING_KEY = "legal_pages"
CACHE_MS = 30 * 1000
MAX_MARKDOWN_BYTES = 32 * 1024
MAX_URL_CHARS = 2000
MAX_CONTACT_CHARS = 320
SECURITY_TXT_DAYS = 180
PAGES = (
    ("terms", "Terms", "/terms"),
    ("privacy", "Privacy", "/privacy"),
    ("security", "Security", "/security"),
)
IDS = tuple(page[0] for page in PAGES)
TITLES = {page[0]: page[1] for page in PAGES}
PATHS = {page[0]: page[2] for page in PAGES}
MODES = ("off", "markdown", "url")
CORE_LINKS = {"terms": "/terms", "privacy": "/privacy",
              "security": "/security-report"}
ROUTES = {"/terms": "terms", "/privacy": "privacy", "/security": "security",
          "/security-report": "security"}
SECURITY_TXT_PATH = "/.well-known/security.txt"
EMAIL_RE = re.compile(
    r"^[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]{1,64}@"
    r"(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\.)+[A-Za-z]{2,63}$")
UNSAFE_RE = re.compile(r"[\s<>\"'`\\\x00-\x1f\x7f]")

_cache = {"at": -CACHE_MS, "value": None}


class SettingError(ValueError):
    pass


def _blank(page):
    entry = {"mode": "off", "markdown": "", "url": ""}
    if page == "security":
        entry["contact"] = ""
    return entry


def default():
    return {page: _blank(page) for page in IDS}


def clean_url(value):
    url = str(value or "").strip()
    if not url:
        return ""
    if len(url) > MAX_URL_CHARS or UNSAFE_RE.search(url):
        raise SettingError("The link must be a full http or https address.")
    try:
        parts = urlsplit(url)
    except ValueError as error:
        raise SettingError("The link must be a full http or https "
                           "address.") from error
    if parts.scheme.lower() not in ("http", "https") or not parts.hostname:
        raise SettingError("The link must be a full http or https address.")
    return url


def clean_contact(value):
    contact = str(value or "").strip()
    if not contact:
        return ""
    if len(contact) > MAX_CONTACT_CHARS:
        raise SettingError("The security contact is too long.")
    address = contact[7:] if contact.lower().startswith("mailto:") else contact
    if EMAIL_RE.fullmatch(address):
        return "mailto:" + address
    if contact.lower().startswith("mailto:"):
        raise SettingError("The security contact must be an email address "
                           "or an https address.")
    url = clean_url(contact)
    if not url.lower().startswith("https://"):
        raise SettingError("The security contact must be an email address "
                           "or an https address.")
    return url


def clean_markdown(value):
    text = str(value or "").replace("\r\n", "\n").replace("\r", "\n")
    text = text.replace("\x00", "")
    if len(text.encode("utf-8")) > MAX_MARKDOWN_BYTES:
        raise SettingError("The page text is longer than 32 KB.")
    return text if text.strip() else ""


def clean(page, mode, markdown="", url="", contact=""):
    if page not in IDS:
        raise SettingError("Unknown page.")
    mode = str(mode or "off").strip().lower()
    if mode not in MODES:
        raise SettingError("Choose off, a page on this site or a link.")
    entry = _blank(page)
    entry["mode"] = mode
    entry["markdown"] = clean_markdown(markdown)
    entry["url"] = clean_url(url)
    if page == "security":
        entry["contact"] = clean_contact(contact)
    if mode == "markdown" and not entry["markdown"]:
        raise SettingError("Write the page text, or choose another option.")
    if mode == "url" and not entry["url"]:
        raise SettingError("Give the link, or choose another option.")
    return entry


def _entry(page, raw):
    if not isinstance(raw, dict):
        return _blank(page)
    try:
        return clean(page, raw.get("mode"), raw.get("markdown"),
                     raw.get("url"), raw.get("contact"))
    except SettingError:
        return _blank(page)


def parse(doc):
    try:
        value = json.loads(str(doc or "") or "{}")
    except ValueError:
        value = {}
    if not isinstance(value, dict):
        value = {}
    return {page: _entry(page, value.get(page)) for page in IDS}


def with_page(setting, page, entry):
    merged = {key: dict(value) for key, value in (setting or default()).items()
              if key in IDS}
    for key in IDS:
        merged.setdefault(key, _blank(key))
    merged[page] = entry
    return merged


def dump(setting):
    return json.dumps({page: (setting or {}).get(page) or _blank(page)
                       for page in IDS}, sort_keys=True,
                      separators=(",", ":"))


def from_install(value):
    if value in (None, "", {}):
        return None
    if not isinstance(value, dict) or not set(value) <= set(IDS):
        raise SettingError("legalPages may only name terms, privacy and "
                           "security.")
    setting = default()
    for page, raw in value.items():
        if raw in (None, False):
            continue
        if isinstance(raw, str):
            raw = {"url": raw}
        if not isinstance(raw, dict) or not set(raw) <= {
                "markdown", "url", "contact"}:
            raise SettingError("legalPages.%s must give markdown or url."
                               % page)
        if bool(raw.get("markdown")) == bool(raw.get("url")):
            raise SettingError("legalPages.%s must give markdown or url, "
                               "not both." % page)
        if raw.get("contact") and page != "security":
            raise SettingError("Only legalPages.security takes a contact.")
        mode = "markdown" if raw.get("markdown") else "url"
        setting[page] = clean(page, mode, raw.get("markdown"),
                              raw.get("url"), raw.get("contact"))
    return setting


def enabled(setting, page):
    return ((setting or {}).get(page) or {}).get("mode") in ("markdown", "url")


def href(setting, page):
    entry = (setting or {}).get(page) or {}
    if entry.get("mode") == "url":
        return entry.get("url") or ""
    if entry.get("mode") == "markdown":
        return PATHS[page]
    return ""


def links(setting, branded):
    if not branded:
        return {page: {"enabled": True, "href": CORE_LINKS[page],
                       "external": False} for page in IDS}
    out = {}
    for page in IDS:
        on = enabled(setting, page)
        out[page] = {"enabled": on, "href": href(setting, page) if on else "",
                     "external": on and setting[page]["mode"] == "url"}
    return out


def route_page(path):
    path = str(path or "")
    if path != "/" and path.endswith("/"):
        path = path[:-1]
    return ROUTES.get(path, "")


def markdown_html(source):
    try:
        from forkmesh_host.repo_ssr import markdown_html as render
    except ImportError:
        return "".join("<p>%s</p>" % html.escape(block.strip())
                       for block in str(source or "").split("\n\n")
                       if block.strip())
    return render(str(source or ""), "")


def page_html(name, page, markdown):
    body = markdown_html(markdown)
    if "<h1>" not in body:
        body = "<h1>%s</h1>%s" % (html.escape(TITLES[page]), body)
    return instance_pages.page(name, TITLES[page], PATHS[page], body)


def _expires(now_ms):
    seconds = int(now_ms // 1000) + SECURITY_TXT_DAYS * 86400
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(seconds))


def security_txt(setting, origin, now_ms):
    if not enabled(setting, "security"):
        return ""
    entry = setting["security"]
    policy = entry["url"] if entry["mode"] == "url" else (
        origin.rstrip("/") + PATHS["security"])
    lines = ["Contact: " + (entry.get("contact") or policy),
             "Expires: " + _expires(now_ms),
             "Policy: " + policy,
             "Canonical: " + origin.rstrip("/") + SECURITY_TXT_PATH]
    return "\n".join(lines) + "\n"


def cached(now_ms):
    if _cache["value"] is not None and now_ms - _cache["at"] < CACHE_MS:
        return _cache["value"]
    return None


def remember(value, now_ms):
    _cache["value"] = value
    _cache["at"] = now_ms
    return value


def forget():
    _cache["value"] = None
    _cache["at"] = -CACHE_MS
