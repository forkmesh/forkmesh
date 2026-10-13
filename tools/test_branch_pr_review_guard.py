#!/usr/bin/env python3
"""Loopback guard for the local diff/PR-creation server (branch_pr_review.py).

The server signs and creates real PRs, so it must answer only the local
operator, never a page in the operator's browser. Verifies the Host pin (anti
DNS-rebind) on GET and the Host+Origin check (anti-CSRF) on POST.

Run:  python3 tools/test_branch_pr_review_guard.py
"""

import importlib.util
from pathlib import Path

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("bpr", HERE / "branch_pr_review.py")
bpr = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(bpr)

PASS = FAIL = 0


def check(name, cond):
    global PASS, FAIL
    if cond:
        PASS += 1
        print("ok  ", name)
    else:
        FAIL += 1
        print("FAIL", name)


def _handler(headers):
    h = bpr.Handler.__new__(bpr.Handler)
    h.headers = headers
    sent = {}
    h._send = lambda code, body, ctype="": sent.setdefault("code", code)
    return h, sent


HOST = f"127.0.0.1:{bpr.PORT}"
ORIGIN = f"http://127.0.0.1:{bpr.PORT}"

h, sent = _handler({"Host": HOST})
check("GET loopback host allowed", h._local_guard() is True and not sent)

h, sent = _handler({"Host": "evil.example.com"})
check("GET foreign host blocked (403)",
      h._local_guard() is False and sent.get("code") == 403)

h, sent = _handler({"Host": f"evil.example.com:{bpr.PORT}"})
check("GET rebind host:port blocked", h._local_guard() is False)

h, sent = _handler({"Host": HOST})
check("POST no Origin allowed (curl/CLI)",
      h._local_guard(require_origin=True) is True and not sent)

h, sent = _handler({"Host": HOST, "Origin": ORIGIN})
check("POST loopback Origin allowed",
      h._local_guard(require_origin=True) is True)

h, sent = _handler({"Host": HOST, "Origin": "https://evil.example.com"})
check("POST cross-site Origin blocked (CSRF)",
      h._local_guard(require_origin=True) is False and sent.get("code") == 403)

h, sent = _handler({"Host": "evil.example.com", "Origin": ORIGIN})
check("POST foreign Host blocked even with good Origin",
      h._local_guard(require_origin=True) is False)

import html as _html
import re as _re

bpr.diff_html = lambda branch: ""
hostile = "port/x',alert(document.domain),'"
card = bpr.card_html({"branch": hostile, "shortstat": "", "commits": [],
                      "default_title": "t", "existing_pr": None})
handlers = [_html.unescape(v) for v in _re.findall(r'onclick="([^"]*)"', card)]
check("branch name never reaches an inline handler",
      handlers and not any("alert" in v for v in handlers))

print(f"\n{PASS} passed, {FAIL} failed")
raise SystemExit(1 if FAIL else 0)
