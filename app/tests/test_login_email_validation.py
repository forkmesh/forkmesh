#!/usr/bin/env python3
"""Login refuses malformed addresses with signup's rules (issue #622).

The login page used to guard only on ``email.includes("@")``, so ``test@`` and
``test@.com`` were posted to /api/accounts/login and came back as a credential
error, while ``testgmail.com`` was refused in the browser -- one class of
malformed input behaving two different ways, plus avoidable traffic on the
auth endpoint.

These checks exercise the *shipped* validators rather than a copy of them: the
login and signup slices are evaluated under Node, and the Worker's
``valid_account_email`` is exec'd straight out of entry.py. Drift in any one of
the three surfaces fails here instead of in someone's login form. The wiring
(the guard runs before the fetch) is pinned in test_auth_pages_frontend.py.
"""

import ast
import functools
import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
LOGIN_JS = PUBLIC / "login.js"
SIGNUP_JS = PUBLIC / "signup.js"
ENTRY = ROOT / "src" / "entry.py"

needs_node = pytest.mark.skipif(shutil.which("node") is None,
                                reason="Node.js is unavailable")

# Everything a browser refuses before any request is made. The first three are
# the reported cases; the rest are the neighbours a "just check for @" guard
# also waves through.
MALFORMED = (
    "test@",
    "testgmail.com",
    "test@.com",
    "",
    "@example.com",
    "a@b",
    "test@@example.com",
    "test..user@example.com",
    ".test@example.com",
    "test.@example.com",
    "test@example..com",
    "test@-example.com",
    "test@example-.com",
    "test@example.c",
    "test@example.com.",
    "test@ex ample.com",
    "test@localhost",
    "test@127.0.0.1",
    # Non-ASCII is out of the practical mailbox form on every surface.
    "tëst@example.com",
    "test@exämple.com",
    # Over the 64-character mailbox cap.
    "a" * 65 + "@example.com",
)

VALID = (
    "test@example.com",
    "first.last@example.co.uk",
    "user+tag@sub.example.org",
    "user_name@example.io",
    "user@example.museum",
    "TEST@Example.COM",
    "x@y.zz",
    "a" * 64 + "@example.com",
    # The localhost-only demo credential login.js offers must still get in.
    "demo@forkmesh.local",
)

CORPUS = MALFORMED + VALID

# The Node side receives one JSON payload on stdin: the validator lifted out of
# a bundle, and the addresses to run through it.
HARNESS = (
    'const {source, cases} = JSON.parse(require("fs").readFileSync(0, "utf8"));'
    'const validEmail = new Function(source + "\\nreturn validEmail;")();'
    'process.stdout.write('
    'JSON.stringify(cases.map((value) => validEmail(value))));'
)


def _validator_source(bundle: Path) -> str:
    """The regex pair and ``validEmail()`` as the bundle actually ships them."""
    text = bundle.read_text(encoding="utf-8")
    patterns = [
        line.strip() for line in text.splitlines()
        if re.match(r"\s*const EMAIL_(?:LOCAL|DOMAIN)_RE = /", line)
    ]
    assert len(patterns) == 2, f"{bundle.name}: {patterns}"
    start = text.index("function validEmail(value) {")
    end = text.index("\n  }\n", start) + len("\n  }")
    return "\n".join(patterns) + "\n" + text[start:end]


@functools.lru_cache(maxsize=None)
def _verdicts(bundle: Path, cases: tuple) -> tuple:
    """Run ``cases`` through one bundle's own validator under Node."""
    payload = json.dumps(
        {"source": _validator_source(bundle), "cases": list(cases)})
    completed = subprocess.run(
        ["node", "-e", HARNESS],
        input=payload, text=True, capture_output=True, check=True)
    return tuple(json.loads(completed.stdout))


@functools.lru_cache(maxsize=None)
def _valid_account_email():
    """The Worker's own validator, lifted out of entry.py without importing it."""
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    wanted = {"ACCOUNT_EMAIL_LOCAL_RE", "DOMAIN_NAME_RE"}
    selected = [
        node for node in tree.body
        if (isinstance(node, ast.Assign)
            and any(getattr(target, "id", "") in wanted
                    for target in node.targets))
        or (isinstance(node, ast.FunctionDef)
            and node.name == "valid_account_email")
    ]
    assert len(selected) == 3, [ast.dump(node)[:40] for node in selected]
    namespace = {"re": re}
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[])), str(ENTRY), "exec"),
        namespace)
    return namespace["valid_account_email"]


@needs_node
def test_the_reported_addresses_never_reach_the_login_endpoint():
    login = dict(zip(CORPUS, _verdicts(LOGIN_JS, CORPUS)))

    for value in ("test@", "testgmail.com", "test@.com"):
        assert login[value] is False, value


@needs_node
def test_login_refuses_every_malformed_address():
    login = dict(zip(CORPUS, _verdicts(LOGIN_JS, CORPUS)))

    for value in MALFORMED:
        assert login[value] is False, value


@needs_node
def test_ordinary_addresses_and_the_demo_credential_still_log_in():
    login = dict(zip(CORPUS, _verdicts(LOGIN_JS, CORPUS)))

    for value in VALID:
        assert login[value] is True, value
    demo = re.search(r'const DEMO_EMAIL = "([^"]+)"',
                     LOGIN_JS.read_text(encoding="utf-8"))
    assert demo and login[demo.group(1)] is True


@needs_node
def test_login_and_signup_judge_every_address_identically():
    # Behavioural equality, so the two may be formatted or reordered freely but
    # cannot come to different answers about the same address.
    assert _verdicts(LOGIN_JS, CORPUS) == _verdicts(SIGNUP_JS, CORPUS)


@needs_node
def test_the_browser_agrees_with_the_worker_about_valid_addresses():
    # The client must not refuse an address the account system accepts (that
    # would lock a real account out of its own login form), nor pass one it
    # rejects (that is the avoidable request this issue is about).
    server = _valid_account_email()

    assert _verdicts(LOGIN_JS, CORPUS) == tuple(
        server(value) for value in CORPUS)


def test_the_three_surfaces_share_one_mailbox_shape():
    # Compared with whitespace collapsed: a reflow of either bundle is fine, a
    # different rule is not. This also covers the case where Node is missing.
    def shape(bundle):
        return re.sub(r"\s+", " ", _validator_source(bundle)).strip()

    login = shape(LOGIN_JS)
    entry = ENTRY.read_text(encoding="utf-8")

    assert login == shape(SIGNUP_JS)
    # Same caps and same mailbox punctuation as valid_account_email.
    for rule in ("254", "64", "253"):
        assert rule in login
    assert "[a-z0-9.!#$%&'*+/=?^_`{|}~-]" in login
    assert "[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]" in entry
