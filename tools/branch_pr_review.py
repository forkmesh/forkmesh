#!/usr/bin/env python3
"""Local diff viewer for the port/* branches with one-click ForkMesh PR creation.

Run from the repo root:

    python3 tools/branch_pr_review.py        # serves http://127.0.0.1:8799

For every local `port/*` branch it shows the diff vs `main` and a "Create PR"
button. Clicking it writes a real, signed ForkMesh native pull under `.forkmesh/pulls/N/`.
Because every pull originates from a real branch, pull.md stores immutable base
and head commit pointers; the diff and commit series are derived from Git rather
than duplicated into large changes.patch and commits.mbox files.

Signing matches desktop/src/PullStore.cpp::canonicalString:
    content      = title \0 base \0 head \0 patch \0 commits      (utf-8 bytes)
    contentHash  = sha256(content).hexdigest()
    canonical    = "forkmesh-pull-event-v1\n{author}\n{ts}\n{contentHash}"
    sig          = base64url(ed25519_sign(canonical)).rstrip("=")
using the node identity key at
    $XDG_DATA_HOME/ForkMesh/ForkMesh/identity/ed25519.pem
"""

import base64
import hashlib
import html
import json
import os
import re
import subprocess
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519

REPO = Path(__file__).resolve().parent.parent
BASE = "main"
HOST, PORT = "127.0.0.1", 8799

sys.path.insert(0, str(Path(__file__).resolve().parent))
import forkmesh_paths  # noqa: E402  (sibling module; tools run as scripts)

KEY_PATH = forkmesh_paths.resolve().identity_key


def git(*args, text=True):
    return subprocess.run(
        ["git", *args], cwd=REPO, capture_output=True,
        text=text, errors="replace" if text else None,
    )


def git_out(*args):
    r = git(*args)
    return r.stdout if r.returncode == 0 else ""


def git_bytes(*args):
    return git(*args, text=False).stdout


def port_branches():
    out = git_out("for-each-ref", "--format=%(refname:short)", "refs/heads/port/")
    return sorted(b for b in out.splitlines() if b.strip())


def branch_info(branch):
    shortstat = git_out("diff", "--shortstat", f"{BASE}..{branch}").strip()
    commits = [
        line for line in git_out(
            "log", "--reverse", "--format=%h\x1f%s", f"{BASE}..{branch}"
        ).splitlines() if line
    ]
    default_title = commits[0].split("\x1f", 1)[1] if commits else branch
    return {
        "branch": branch,
        "shortstat": shortstat or "no changes",
        "commits": [c.split("\x1f", 1) for c in commits],
        "default_title": default_title,
        "existing_pr": existing_pr_for_head(branch),
    }


def pulls_dir():
    return REPO / ".forkmesh" / "pulls"


def next_pull_number():
    mx = 0
    if pulls_dir().exists():
        for d in pulls_dir().iterdir():
            if d.is_dir() and d.name.isdigit():
                mx = max(mx, int(d.name))
    return mx + 1


def existing_pr_for_head(head):
    """Return an existing open PR number for this head branch, else None."""
    if not pulls_dir().exists():
        return None
    for d in sorted(pulls_dir().iterdir()):
        md = d / "pull.md"
        if not (d.is_dir() and d.name.isdigit() and md.exists()):
            continue
        fm = md.read_text(errors="replace")
        h = re.search(r"^head:\s*(.*)$", fm, re.M)
        s = re.search(r"^status:\s*(.*)$", fm, re.M)
        if h and h.group(1).strip() == head and (not s or s.group(1).strip() == "open"):
            return int(d.name)
    return None


_key = None
_pub = None


def ensure_identity():
    """Create the desktop-compatible identity key when none exists."""
    if KEY_PATH.exists():
        return

    KEY_PATH.parent.mkdir(parents=True, exist_ok=True)
    key = ed25519.Ed25519PrivateKey.generate()
    pem = key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    )

    # O_EXCL preserves an identity created by the desktop app (or another
    # instance of this tool) after the existence check above.  Passing 0600 to
    # os.open also prevents a permissive umask from exposing the private key.
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    flags |= getattr(os, "O_BINARY", 0)
    try:
        descriptor = os.open(KEY_PATH, flags, 0o600)
    except FileExistsError:
        return

    try:
        with os.fdopen(descriptor, "wb") as key_file:
            key_file.write(pem)
    except BaseException:
        KEY_PATH.unlink(missing_ok=True)
        raise

    try:
        KEY_PATH.chmod(0o600)
    except OSError as exc:
        # Match the desktop app's best-effort permission hardening on filesystems
        # that do not expose POSIX-style permissions (notably some Windows and
        # network-mounted data directories).
        print(
            f"Warning: could not restrict permissions on identity key "
            f"{KEY_PATH}: {exc}",
            file=sys.stderr,
        )
    print(f"Identity key not found. Generated a new one at {KEY_PATH}.")


def load_identity():
    global _key, _pub
    if _key is None:
        _key = serialization.load_pem_private_key(KEY_PATH.read_bytes(), password=None)
        raw = _key.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw
        )
        _pub = base64.urlsafe_b64encode(raw).decode().rstrip("=")
    return _key, _pub


def sign_pull(title, base, head, patch_bytes, commits_bytes, ts, author):
    content = (
        title.encode() + b"\x00" + base.encode() + b"\x00" + head.encode()
        + b"\x00" + patch_bytes + b"\x00" + commits_bytes
    )
    content_hash = hashlib.sha256(content).hexdigest().encode()
    canonical = (
        b"forkmesh-pull-event-v1\n" + author.encode() + b"\n"
        + str(ts).encode() + b"\n" + content_hash
    )
    sig = load_identity()[0].sign(canonical)
    return base64.urlsafe_b64encode(sig).decode().rstrip("=")


def create_pull(branch, title, description):
    existing = existing_pr_for_head(branch)
    if existing:
        return {"ok": True, "number": existing, "existing": True}

    base, head = BASE, branch
    patch_bytes = git_bytes("diff", f"{base}..{head}")
    commits_bytes = git_bytes("format-patch", "--stdout", f"{base}..{head}")
    if not patch_bytes.strip():
        return {"ok": False, "error": "Branch has no diff against main."}

    ts = int(time.time() * 1000)
    _, author = load_identity()
    author_name = git_out("config", "user.name").strip()
    sig = sign_pull(title, base, head, patch_bytes, commits_bytes, ts, author)
    base_oid = git_out("rev-parse", "--verify", f"{base}^{{commit}}").strip()
    head_oid = git_out("rev-parse", "--verify", f"{head}^{{commit}}").strip()
    if not base_oid or not head_oid:
        return {"ok": False, "error": "Could not resolve the PR branch commits."}
    number = next_pull_number()

    pdir = pulls_dir() / str(number)
    pdir.mkdir(parents=True, exist_ok=True)
    front = "\n".join([
        "---",
        "schema: forkmesh-pull-v1",
        f"number: {number}",
        f"title: {title}",
        f"base: {base}",
        f"head: {head}",
        "status: open",
        "derive: branch",
        f"creationBaseOid: {base_oid}",
        f"creationHeadOid: {head_oid}",
        f"ts: {ts}",
        f"author: {author}",
        f"authorName: {author_name}",
        f"sig: {sig}",
        "---",
        "",
    ])
    (pdir / "pull.md").write_text(front + "\n" + description + "\n")

    add = git("add", f".forkmesh/pulls/{number}")
    if add.returncode != 0:
        return {"ok": False, "error": f"git add failed: {add.stderr}"}
    commit = git(
        "commit", "-m", f"pull #{number}: open", "--",
        f".forkmesh/pulls/{number}")
    if commit.returncode != 0:
        return {"ok": False, "error": f"git commit failed: {commit.stderr or commit.stdout}"}
    return {"ok": True, "number": number, "existing": False}


def diff_html(branch):
    raw = git_out("diff", f"{BASE}..{branch}")
    rows = []
    for line in raw.split("\n"):
        cls = "ctx"
        if line.startswith("diff --git") or line.startswith("index ") \
                or line.startswith("--- ") or line.startswith("+++ ") \
                or line.startswith("new file") or line.startswith("deleted file") \
                or line.startswith("rename "):
            cls = "meta"
        elif line.startswith("@@"):
            cls = "hunk"
        elif line.startswith("+"):
            cls = "add"
        elif line.startswith("-"):
            cls = "del"
        rows.append(f'<div class="ln {cls}">{html.escape(line) or "&nbsp;"}</div>')
    return "\n".join(rows)


PAGE = """<!doctype html><html><head><meta charset="utf-8">
<title>ForkMesh — port/* branch review</title>
<style>
:root{{color-scheme:dark}}
body{{margin:0;font:14px/1.5 -apple-system,Segoe UI,Roboto,sans-serif;background:#0d1117;color:#c9d1d9}}
header{{padding:18px 24px;border-bottom:1px solid #21262d;position:sticky;top:0;background:#0d1117;z-index:5}}
header h1{{margin:0;font-size:18px}} header p{{margin:4px 0 0;color:#8b949e;font-size:13px}}
.wrap{{max-width:1100px;margin:0 auto;padding:18px 24px}}
.card{{border:1px solid #21262d;border-radius:10px;margin:14px 0;overflow:hidden;background:#0d1117}}
.summary{{cursor:pointer;padding:14px 16px;display:flex;align-items:center;gap:12px;background:#161b22;user-select:none}}
.summary:hover{{background:#1b222b}}
.caret{{display:inline-block;transition:transform .12s;color:#8b949e}}
.card.open .caret{{transform:rotate(90deg)}}
.body{{display:none}}
.card.open .body{{display:block}}
.bname{{font-weight:600;font-family:ui-monospace,SFMono-Regular,Menlo,monospace}}
.stat{{color:#8b949e;font-size:12px}}
.spacer{{flex:1}}
.commits{{padding:6px 16px;border-top:1px solid #21262d;color:#8b949e;font-size:12px}}
.commits code{{color:#58a6ff}}
.diff{{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12px;overflow:auto;max-height:75vh;border-top:1px solid #21262d}}
.empty{{padding:14px 16px;color:#8b949e}}
.ln{{white-space:pre;padding:0 14px}}
.ln.add{{background:#0f2a17;color:#aff5b4}} .ln.del{{background:#3a1115;color:#ffc1c1}}
.ln.hunk{{background:#161b22;color:#a371f7}} .ln.meta{{color:#8b949e}} .ln.ctx{{color:#c9d1d9}}
.act{{display:flex;align-items:center;gap:10px;padding:12px 16px;border-top:1px solid #21262d;background:#161b22}}
.act input{{flex:1;background:#0d1117;border:1px solid #30363d;border-radius:6px;color:#c9d1d9;padding:7px 10px;font-size:13px}}
button{{background:#238636;border:1px solid #2ea043;color:#fff;border-radius:6px;padding:7px 14px;font-size:13px;font-weight:600;cursor:pointer}}
button:hover{{background:#2ea043}} button:disabled{{opacity:.5;cursor:default}}
.pill{{font-size:12px;padding:3px 9px;border-radius:20px;border:1px solid #30363d;color:#8b949e}}
.pill.done{{color:#3fb950;border-color:#238636}}
.msg{{font-size:12px;margin-left:4px}} .msg.ok{{color:#3fb950}} .msg.err{{color:#f85149}}
</style></head><body>
<header><h1>ForkMesh · port/* branch review</h1>
<p>Identity <code>{pub}</code> · base <code>{base}</code> · {n} branch(es). Create PR writes a signed pull under <code>.forkmesh/pulls/</code>.
<a href="#" onclick="setAll(true);return false" style="color:#58a6ff;margin-left:8px">expand all</a> ·
<a href="#" onclick="setAll(false);return false" style="color:#58a6ff">collapse all</a></p></header>
<div class="wrap">{cards}</div>
<script>
function toggleCard(el){{ el.closest('.card').classList.toggle('open'); }}
function setAll(open){{ document.querySelectorAll('.card').forEach(c=>c.classList.toggle('open',open)); }}
async function createPR(branch, btn){{
  const card=btn.closest('.card');
  const title=card.querySelector('.title').value.trim();
  const msg=card.querySelector('.msg');
  btn.disabled=true; msg.className='msg'; msg.textContent='Creating…';
  try{{
    const r=await fetch('/api/create-pr',{{method:'POST',headers:{{'Content-Type':'application/json'}},
      body:JSON.stringify({{branch,title}})}});
    const d=await r.json();
    if(d.ok){{msg.className='msg ok';msg.textContent=(d.existing?'Already open as PR #':'Created PR #')+d.number;
      btn.textContent='PR #'+d.number;}}
    else{{msg.className='msg err';msg.textContent=d.error||'failed';btn.disabled=false;}}
  }}catch(e){{msg.className='msg err';msg.textContent=String(e);btn.disabled=false;}}
}}
</script></body></html>"""


def card_html(info):
    b = info["branch"]
    commits = "".join(
        f'<div><code>{html.escape(h)}</code> {html.escape(s)}</div>'
        for h, s in info["commits"]
    )
    if info["existing_pr"]:
        action = (
            f'<span class="pill done">PR #{info["existing_pr"]} open</span>'
            f'<span class="spacer"></span>'
            f'<input class="title" value="{html.escape(info["default_title"])}" disabled>'
            f'<button disabled>PR #{info["existing_pr"]}</button>'
        )
    else:
        action = (
            f'<input class="title" value="{html.escape(info["default_title"])}">'
            f'<button data-branch="{html.escape(b)}" onclick="createPR(this.dataset.branch,this)">Create PR</button>'
            f'<span class="msg"></span>'
        )
    diff = diff_html(b)
    diff_block = f'<div class="diff">{diff}</div>' if diff.strip() \
        else '<div class="empty">No textual diff against main.</div>'
    return f"""<div class="card">
<div class="summary" onclick="toggleCard(this)"><span class="caret">&#9654;</span>
<span class="bname">{html.escape(b)}</span>
<span class="stat">{html.escape(info['shortstat'])}</span><span class="spacer"></span>
<span class="stat">{len(info['commits'])} commit(s) · click to view diff</span></div>
<div class="body">
<div class="commits">{commits}</div>
{diff_block}
<div class="act">{action}</div>
</div></div>"""


_ALLOWED_HOSTS = frozenset({f"127.0.0.1:{PORT}", f"localhost:{PORT}"})
_ALLOWED_ORIGINS = frozenset({f"http://127.0.0.1:{PORT}", f"http://localhost:{PORT}"})


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="text/html; charset=utf-8"):
        data = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _local_guard(self, require_origin=False):
        # This server signs and creates real PRs and exposes local diffs, so it
        # must only ever answer the local operator — never a web page in the
        # operator's browser. Pin the Host header to a loopback name (defeats
        # DNS-rebinding, where an attacker domain resolves to 127.0.0.1 but the
        # Host header carries the attacker's domain). For state-changing POSTs,
        # also require the Origin (which browsers always send cross-origin) to be
        # absent or loopback, defeating cross-site form/fetch CSRF.
        if (self.headers.get("Host") or "") not in _ALLOWED_HOSTS:
            self._send(403, "{}", "application/json")
            return False
        if require_origin:
            origin = self.headers.get("Origin")
            if origin and origin not in _ALLOWED_ORIGINS:
                self._send(403, "{}", "application/json")
                return False
        return True

    def do_GET(self):
        if not self._local_guard():
            return
        if self.path not in ("/", "/index.html"):
            return self._send(404, "not found", "text/plain")
        branches = port_branches()
        cards = "".join(card_html(branch_info(b)) for b in branches) \
            or "<p>No <code>port/*</code> branches found.</p>"
        _, pub = load_identity()
        self._send(200, PAGE.format(pub=pub, base=BASE, n=len(branches), cards=cards))

    def do_POST(self):
        if not self._local_guard(require_origin=True):
            return
        if self.path != "/api/create-pr":
            return self._send(404, "{}", "application/json")
        length = int(self.headers.get("Content-Length", 0))
        try:
            payload = json.loads(self.rfile.read(length) or b"{}")
            branch = payload["branch"]
            if branch not in port_branches():
                raise ValueError("unknown branch")
            title = (payload.get("title") or branch).strip()
            desc_lines = git_out("log", "--reverse", "--format=- %s",
                                 f"{BASE}..{branch}").strip()
            description = (
                f"Ported from newnewnode-forkmesh.\n\n{desc_lines}\n"
            )
            result = create_pull(branch, title, description)
        except Exception as e:  # noqa: BLE001
            result = {"ok": False, "error": str(e)}
        self._send(200, json.dumps(result), "application/json")


def main():
    ensure_identity()
    load_identity()
    srv = ThreadingHTTPServer((HOST, PORT), Handler)
    print(f"Diff viewer + PR creator → http://{HOST}:{PORT}  (Ctrl-C to stop)")
    print(f"Identity: {_pub}")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")


if __name__ == "__main__":
    main()
