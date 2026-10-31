"""Browser password managers must not treat dashboard search boxes as logins.

Chrome pools every form-less input on a page into one synthetic login form. A
bare ``type="password"`` field anywhere in it (the payout modal ships on every
dashboard page) made the visible search inputs the "username" and auto-filled
the user's saved email into them (#671). Every password field therefore lives
inside its own ``<form>``, and search inputs opt out of autocomplete.
"""

import re
import subprocess
from html.parser import HTMLParser
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PUBLIC = ROOT / "app" / "public"


def _served_dashboard_pages():
    listed = subprocess.check_output(
        ["git", "ls-files", "app/public/dashboard", "app/public/hackathon",
         "app/public/microjobs", "app/public/referral"],
        cwd=ROOT, text=True,
    ).split()
    pages = [ROOT / name for name in listed
             if name.endswith(".html") and "/partials/" not in name]
    assert pages
    return pages


class _InputAudit(HTMLParser):
    def __init__(self):
        super().__init__()
        self.form_depth = 0
        self.formless_passwords = []
        self.autocompleting_searches = []

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == "form":
            self.form_depth += 1
        elif tag == "input" and attrs.get("type") == "password" and not self.form_depth:
            self.formless_passwords.append(self.getpos()[0])
        elif tag == "input" and attrs.get("type") == "search" and attrs.get("autocomplete") != "off":
            self.autocompleting_searches.append(self.getpos()[0])

    def handle_endtag(self, tag):
        if tag == "form":
            self.form_depth -= 1


def test_dashboard_pages_keep_password_fields_out_of_the_formless_pool():
    for page in _served_dashboard_pages():
        audit = _InputAudit()
        audit.feed(page.read_text(encoding="utf-8"))
        rel = page.relative_to(ROOT)
        assert not audit.formless_passwords, f"{rel}: password input outside a <form> at lines {audit.formless_passwords}"
        assert not audit.autocompleting_searches, f"{rel}: search input without autocomplete=off at lines {audit.autocompleting_searches}"


def test_dashboard_js_search_inputs_opt_out_of_autocomplete():
    dashboard_js = (PUBLIC / "dashboard.js").read_text(encoding="utf-8")
    inputs = re.findall(r"<input\b[^>]*\btype=\"search\"[^>]*>", dashboard_js)
    assert inputs
    missing = [tag[:120] for tag in inputs if 'autocomplete="off"' not in tag]
    assert not missing, missing
    for match in re.finditer(r"<input\b[^>]*\btype=\"password\"", dashboard_js):
        before = dashboard_js[:match.start()]
        opened, closed = before.rfind("<form"), before.rfind("</form>")
        assert opened > closed and match.start() - opened < 4000, (
            "password field outside its own <form>: " + match.group(0)[:120])


def test_credential_scope_forms_never_submit():
    dashboard_js = (PUBLIC / "dashboard.js").read_text(encoding="utf-8")
    guard = dashboard_js.index('document.addEventListener("submit", (event) => {\n    if (event.target instanceof Element && event.target.matches("[data-credential-scope]"))')
    assert "event.preventDefault();" in dashboard_js[guard:guard + 300]
