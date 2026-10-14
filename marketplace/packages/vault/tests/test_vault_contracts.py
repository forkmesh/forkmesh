"""Password Vault package wiring, and the properties of its page that keep a
master password and a keyset from leaking out of the tab."""

from html.parser import HTMLParser
import json
from pathlib import Path
import re
import sys


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
sys.path.insert(0, str(REPO / "app" / "tools"))
sys.path.insert(0, str(REPO / "app" / "src"))
import build_app_plugins  # noqa: E402

MANIFEST = json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))
VIEW = (PACKAGE / "content" / "web" / "view.html").read_text(encoding="utf-8")
PAGE_UI = (PACKAGE / "content" / "web" / "page.ui.js").read_text(encoding="utf-8")
PAGE = (PACKAGE / "content" / "web" / "page.js").read_text(encoding="utf-8")
CLIENT = (PACKAGE / "content" / "crypto" / "vault-client.js").read_text(encoding="utf-8")
CRYPTO = (PACKAGE / "content" / "crypto" / "vault-crypto.js").read_text(encoding="utf-8")
WORKER = (PACKAGE / "content" / "worker" / "vault.py").read_text(encoding="utf-8")
TAILWIND = (REPO / "app" / "public" / "dashboard" / "tailwind.css").read_text(encoding="utf-8")
SHELL = (REPO / "app" / "public" / "dashboard" / "shell.html").read_text(encoding="utf-8")


def test_the_package_is_account_scoped_and_the_host_stages_it():
    assert MANIFEST["id"] == "vault" and MANIFEST["dataLifecycle"] == {"scope": "account"}
    assert MANIFEST["worker"] == {"module": "vault", "api": "/api/vault", "outboundOrigins": []}
    assert len(MANIFEST["summary"]) <= 240
    catalog = json.loads((REPO / "marketplace" / "catalog.json").read_text(encoding="utf-8"))
    row = next(item for item in catalog["packages"] if item["id"] == "vault")
    assert row["version"] == MANIFEST["version"]
    plugin = json.loads((PACKAGE / ".codex-plugin" / "plugin.json").read_text(encoding="utf-8"))
    assert plugin["version"] == MANIFEST["version"]
    registry = {}
    exec(build_app_plugins.registry_source(), registry)
    entry = registry["PLUGINS"]["vault"]
    assert {key: entry[key] for key in (
        "module", "api", "outboundOrigins", "organizationData", "accountData", "default",
        "accountSync", "installedBefore", "dependencies", "settings")} == {
        "module": "vault", "api": "/api/vault", "outboundOrigins": (),
        "organizationData": False, "accountData": True, "default": False,
        "accountSync": True, "installedBefore": "", "dependencies": (), "settings": ()}
    assert registry["PLUGIN_DASHBOARD_PAGE_ASSETS"]["/dashboard/vault"] == (
        "dashboard-plugins/vault/default/index.html")
    assert "vault.py" in {str(path) for path, _size, _raw in build_app_plugins.worker_payloads()}
    # The account lifecycle hooks plugin_data calls by name.
    for hook in ("async def purge_account_data(runtime, account_bi, account_name=\"\")",
                 "async def account_installed(runtime, account_bi)",
                 "async def handle(runtime, path)", "DATA_SCHEMA_UP", "DATA_SCHEMA_DOWN",
                 'REQUEST_PROOF = "forkmesh-vault-request-v1"'):
        assert hook in WORKER, hook


def test_the_worker_has_no_plaintext_column_and_no_cryptography():
    columns = set(re.findall(r"^\s{8}([a-z_]+) (?:TEXT|INTEGER)", WORKER, re.M))
    assert columns == {
        "account_bi", "account_name", "revision", "created_at", "updated_at", "data",
        "item_id", "owner_bi", "owner_name", "updated_by", "target_kind", "target_bi",
        "target_name", "permission", "recipient_bi", "recipient_name", "via_kind", "via_bi"}
    # It files blobs; it never opens one. No key material, no cipher, no hash.
    for forbidden in ("import hashlib", "import hmac", "import secrets", "AES", "decrypt(",
                      "new_secret", "seal_with", "open_with", "password"):
        assert forbidden not in WORKER.replace("master password", "").replace(
            "guess a master", ""), forbidden
    assert WORKER.count("await runtime.seal(") == 6


class _Audit(HTMLParser):
    def __init__(self):
        super().__init__()
        self.forms = 0
        self.formless_passwords, self.loose_searches, self.classes = [], [], set()
        self.password_autocomplete = []

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        self.classes.update((attrs.get("class") or "").split())
        if tag == "form":
            self.forms += 1
        if tag == "input" and attrs.get("type") == "password":
            self.password_autocomplete.append(attrs.get("autocomplete"))
            if not self.forms:
                self.formless_passwords.append(self.getpos())
        if tag == "input" and attrs.get("type") == "search" and attrs.get("autocomplete") != "off":
            self.loose_searches.append(self.getpos())

    def handle_endtag(self, tag):
        if tag == "form":
            self.forms -= 1


def test_the_view_follows_the_dashboard_credential_rules_and_uses_built_classes():
    audit = _Audit()
    audit.feed(VIEW)
    # Chrome pools form-less password inputs with the nearest search box.
    assert not audit.formless_passwords and not audit.loose_searches
    assert set(audit.password_autocomplete) == {"new-password", "current-password"}
    assert VIEW.count("data-credential-scope") == 5
    # tailwind.css is a committed build that never scans package views: a
    # class it lacks would silently style nothing.
    missing = [name for name in sorted(audit.classes)
               if not name.startswith(("vault-", "fm-")) and name not in {"view", "active"}
               and "." + re.sub(r"([^A-Za-z0-9_-])", r"\\\1", name) not in TAILWIND]
    assert missing == []
    shared = [name for name in sorted(audit.classes)
              if name.startswith("fm-") and "." + name + " " not in SHELL and "." + name + "," not in SHELL
              and "." + name + ":" not in SHELL and "." + name + "." not in SHELL and "." + name + " >" not in SHELL]
    assert shared == []
    # `hidden` loses to a display utility declared later in the stylesheet.
    for group in re.findall(r'class="([^"]*)"', VIEW):
        names = group.split()
        assert not ("hidden" in names and {"flex", "grid", "inline-block"} & set(names)), group
    assert VIEW.lstrip().startswith('<section data-view="vault"')


def test_the_page_keeps_secrets_in_memory_only():
    assert 'window.ForkMeshDashboard.registerPage("vault"' in PAGE_UI
    # The only thing the page persists is the idle-lock preference.
    assert re.findall(r"localStorage\.(?:set|get)Item\(([A-Z_]+)", PAGE_UI) == ["AUTOLOCK_KEY", "AUTOLOCK_KEY"]
    for source in (PAGE_UI, CLIENT, CRYPTO):
        for forbidden in ("sessionStorage", "indexedDB", "document.cookie", "console.log",
                          "console.debug", "eval(", "new Function"):
            assert forbidden not in source, forbidden
    assert "localStorage" not in CLIENT + CRYPTO
    # The website session is a cookie; "cookie" is never sent as a bearer.
    assert 'if (token && token !== "cookie") headers.authorization' in PAGE_UI
    assert 'credentials: "same-origin"' in PAGE_UI
    # Locking: idle timer, leaving the page, and the Lock button.
    assert 'window.addEventListener("pagehide", () => state.client?.lock());' in PAGE_UI
    assert "autolockMinutes() * 60000" in PAGE_UI
    # Everything a person typed is escaped on its way into markup.
    markup = PAGE_UI.split("function renderList()", 1)[1].split("async function reload()", 1)[0]
    templates = re.findall(r"\$\{([^}]*fields\.[a-z]+[^}]*)\}", markup)
    assert len(templates) >= 2
    for template in templates:
        assert template.startswith("escapeHtml(") or "?" in template, template
    # The Open button only ever visits an https address, in a detached tab.
    assert 'const href = Vault.loginHref(item.fields.url);' in PAGE_UI
    assert 'window.open(href, "_blank", "noopener,noreferrer")' in PAGE_UI
    assert "if (parsed.protocol !== \"https:\" &&" in CLIENT


def test_the_client_refuses_changed_keys_and_matches_sites_exactly():
    assert 'if (seen.state === "changed") throw new VaultError("sender_keys_changed");' in CLIENT
    assert 'throw new VaultError("recipient_keys_changed"' in CLIENT
    # A sender is pinned only after their signature held under those keys.
    load = CLIENT.split("async function load()", 1)[1].split("async function save(", 1)[0]
    assert load.index("await C.openGrant({") < load.index("trust.pins[seen.name] = seen.fingerprint")
    assert "return Boolean(saved) && saved === loginHost(pageUrl);" in CLIENT
    assert "endsWith" not in CLIENT
    # The published keys must be the ones this keyset can open.
    assert 'throw new VaultError("keyset_mismatch");' in CLIENT


def test_the_suite_is_what_the_documentation_says():
    assert 'const SUITE = "fmv1";' in CRYPTO and 'SUITE = "fmv1"' in WORKER
    assert "memoryKiB: 65536, passes: 3, parallelism: 1" in CRYPTO
    assert "memoryKiB: 19456, passes: 2" in CRYPTO
    assert 'fromHex("5c2e2f2f5e5c")' in CRYPTO          # XWingLabel
    assert "sha3_256(concat(kem.sharedSecret, ssX, ctX, recipient.x, XWING_LABEL))" in CRYPTO
    assert "KDF_MIN_MEMORY_KIB = 19456" in WORKER and "KDF_MIN_PASSES = 2" in WORKER
    readme = (PACKAGE / "README.md").read_text(encoding="utf-8")
    for claim in ("Argon2id", "64 MiB", "AES-256-GCM", "ML-KEM-768", "X25519", "Ed25519",
                  "X-Wing", "OpenSSL 3.5", "not been audited", "constant-time"):
        assert claim in readme, claim
