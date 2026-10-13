"""The extension's Password Vault half: behaviour under a fake browser, and
the properties of its source that keep a password where it belongs."""

import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

import pytest


PACKAGE = Path(__file__).resolve().parents[1]
EXTENSION = PACKAGE / "content" / "extension"
VAULT_PACKAGE = PACKAGE.parent / "vault"
NODE = shutil.which("node")
BACKGROUND = (EXTENSION / "background.js").read_text(encoding="utf-8")
VAULT_BACKGROUND = (EXTENSION / "vault.js").read_text(encoding="utf-8")
POPUP_VAULT = (EXTENSION / "popup-vault.js").read_text(encoding="utf-8")
POPUP_HTML = (EXTENSION / "popup.html").read_text(encoding="utf-8")
MANIFEST = json.loads((EXTENSION / "manifest.json").read_text(encoding="utf-8"))
CONTENT_SCRIPTS = [
    (EXTENSION / name).read_text(encoding="utf-8")
    for script in MANIFEST["content_scripts"] for name in script["js"]]


@pytest.mark.skipif(NODE is None, reason="node is not installed")
def test_background_vault_unlocks_fills_only_its_own_site_and_locks():
    result = subprocess.run(
        [NODE, "--no-warnings", str(PACKAGE / "tests" / "background_vault_harness.js")],
        capture_output=True, text=True, timeout=600,
        env={"PATH": "/usr/local/bin:/usr/bin:/bin", "PYTHON": sys.executable})
    assert result.returncode == 0, result.stdout[-3000:] + result.stderr[-2000:]
    assert "checks passed" in result.stdout


def test_the_extension_runs_the_vault_packages_own_cryptography():
    for name in ("vault-crypto.js", "vault-client.js"):
        assert (EXTENSION / name).read_bytes() == (
            VAULT_PACKAGE / "content" / "crypto" / name).read_bytes(), name
    assert 'importScripts("vault-crypto.js", "vault-client.js", "vault.js");' in BACKGROUND
    # Packaged files only: no remote code, no eval, and the default MV3 policy.
    assert "content_security_policy" not in MANIFEST
    for source in (VAULT_BACKGROUND, POPUP_VAULT):
        assert "eval(" not in source and "new Function" not in source


def test_keys_stay_in_memory_and_out_of_reach_of_content_scripts():
    # The keyset goes to session storage (memory only), never to disk.
    assert "chrome.storage.session.set({ vaultKeyset:" in VAULT_BACKGROUND
    assert not re.search(r"storage\.local\.set\([^)]*vault", VAULT_BACKGROUND + BACKGROUND, re.S)
    assert "localStorage" not in VAULT_BACKGROUND + POPUP_VAULT
    assert 'setAccessLevel?.({ accessLevel: "TRUSTED_CONTEXTS" })' in VAULT_BACKGROUND
    assert "const IDLE_MINUTES = 15;" in VAULT_BACKGROUND
    # Signing out of ForkMesh locks the vault.
    sign_out = BACKGROUND.split("async function signOut()", 1)[1].split("\n}\n", 1)[0]
    assert "await vaultBackground.forget();" in sign_out
    # The background worker refuses vault messages from anything with a tab
    # behind it, before any handler runs.
    listener = BACKGROUND.split("chrome.runtime.onMessage.addListener(", 1)[1]
    assert listener.index("!mayUseVault(sender)") < listener.index("handler(request, sender)")
    assert "sender?.id === chrome.runtime.id && !sender.tab" in BACKGROUND
    assert all("vault" in name for name in re.findall(r'"(fm-[a-z-]*vault[a-z-]*)"', VAULT_BACKGROUND))
    assert set(re.findall(r'"(fm-[a-z-]+)":', VAULT_BACKGROUND)) == {
        "fm-vault-status", "fm-vault-unlock", "fm-vault-lock", "fm-vault-items", "fm-vault-fill",
        "fm-vault-copy", "fm-vault-capture", "fm-vault-save", "fm-vault-generate", "fm-open-vault"}
    # Nothing that runs inside a web page knows the vault exists.
    for source in CONTENT_SCRIPTS:
        assert "fm-vault" not in source and "vaultKeyset" not in source


def test_a_password_reaches_a_page_only_by_a_checked_click():
    fill = VAULT_BACKGROUND.split("async function vaultFill(", 1)[1].split("\n    }\n", 1)[0]
    # The host check uses the tab Chrome reports and comes before injection.
    assert fill.index("Vault.matchesPage(item, tab.url)") < fill.index("chrome.scripting.executeScript")
    assert "allFrames" not in VAULT_BACKGROUND and "frameIds" not in VAULT_BACKGROUND
    # The list the popup renders is built from a summary with no secret in it.
    summary = VAULT_BACKGROUND.split("function summary(item)", 1)[1].split("\n    }\n", 1)[0]
    assert "fields.password" in summary and "hasPassword: Boolean(fields.password)" in summary
    assert not re.search(r"password:\s*(String\()?fields\.password", summary)
    # The popup asks for a secret one field at a time, on a button press.
    assert 'send({ type: "fm-vault-copy", id, field: what })' in POPUP_VAULT
    assert "innerHTML" in POPUP_VAULT and "escapeHtml(item.title)" in POPUP_VAULT


def test_popup_password_fields_sit_in_forms_and_search_does_not_autocomplete():
    for field in re.findall(r"<input[^>]*type=\"password\"[^>]*>", POPUP_HTML):
        before = POPUP_HTML[:POPUP_HTML.index(field)]
        assert before.count("<form") > before.count("</form>"), field
    assert 'data-vault-search type="search" autocomplete="off"' in POPUP_HTML
    assert '<script src="popup-vault.js"></script>' in POPUP_HTML
    assert "<h2>Password Vault</h2>" in POPUP_HTML and "<h2>CRM</h2>" in POPUP_HTML
