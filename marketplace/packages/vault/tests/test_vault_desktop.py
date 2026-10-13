"""The desktop Vault page: package wiring, what it may and may not do with
secrets, and its C++ suite checked against the browser's."""

import json
from pathlib import Path
import re
import shutil
import subprocess

import pytest


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
DESKTOP = PACKAGE / "content" / "desktop"
FEATURE = (DESKTOP / "VaultFeature.cpp").read_text(encoding="utf-8")
FEATURE_HEADER = (DESKTOP / "VaultFeature.h").read_text(encoding="utf-8")
CLIENT = (DESKTOP / "VaultClient.cpp").read_text(encoding="utf-8")
CRYPTO = (DESKTOP / "VaultCrypto.cpp").read_text(encoding="utf-8")
WORKER = (PACKAGE / "content" / "worker" / "vault.py").read_text(encoding="utf-8")
MANIFEST = json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))
HOST = "".join(
    path.read_text(encoding="utf-8", errors="ignore")
    for path in sorted((REPO / "desktop" / "src").glob("*.cpp")))


def test_the_page_registers_through_the_neutral_host_socket():
    assert MANIFEST["entrypoints"]["desktop"] == "builtin:vault"
    assert "const forkmesh::market::NativePackageRegistration vaultRegistration({" in FEATURE
    for literal in ('QStringLiteral("builtin:vault")', 'QStringLiteral("/api/vault")',
                    'QStringLiteral("Vault")', 'QStringLiteral("key")'):
        assert literal in FEATURE, literal
    # The signing proof is the Worker module's, to the letter.
    proof = re.search(r'REQUEST_PROOF = "([^"]+)"', WORKER).group(1)
    assert f'QStringLiteral("{proof}")' in FEATURE
    assert 'alias="key.svg"' in (REPO / "desktop" / "resources" / "icons.qrc").read_text(encoding="utf-8")
    assert json.loads((DESKTOP / "build.json").read_text(encoding="utf-8")) == {
        "schemaVersion": 1,
        "sources": ["VaultCrypto.h", "VaultCrypto.cpp", "VaultClient.h", "VaultClient.cpp",
                    "VaultFeature.h", "VaultFeature.cpp"],
        "mocHeaders": [],
        "testSources": ["tests/test_vault_crypto.cpp", "tests/test_vault_client_flow.cpp",
                        "VaultCrypto.h", "VaultCrypto.cpp", "VaultClient.h", "VaultClient.cpp"],
        "testLibraries": ["Qt6::Core", "OpenSSL::Crypto"],
    }
    # The host stays package-neutral: it learns of the vault from the catalog.
    for mention in ("api/vault", "builtin:vault", "VaultFeature", "forkmesh-vault-request"):
        assert mention not in HOST, mention
    # The top search box socket every page package answers.
    assert "searchHits(const QString &query, int limit) const override;" in FEATURE_HEADER
    assert "void activateSearchHit(const QString &id) override;" in FEATURE_HEADER
    assert "m_searchIndexChanged(host.searchIndexChanged)" in FEATURE
    # Searching must never be what asks for a master password.
    assert "void prepareSearch() override {}" in FEATURE_HEADER


def test_every_desktop_call_is_in_the_network_catalog():
    catalog = json.loads((REPO / "desktop" / "resources" / "network" / "endpoint-catalog.json")
                         .read_text(encoding="utf-8"))["endpoints"]
    rows = {(row["pattern"], row["method"]): row for row in catalog if "api/vault" in row["pattern"]}
    assert set(rows) == {("app.forkmesh.com/api/vault", "GET")} | {
        ("app.forkmesh.com/api/vault/*", method) for method in ("GET", "POST", "PUT", "DELETE")}
    used = set(re.findall(r'call\("([A-Z]+)"', CLIENT))
    assert used == {"GET", "POST", "PUT", "DELETE"}
    for row in rows.values():
        for source in row["source"]:
            path, function = source.split(": ")
            assert (REPO / path).is_file(), source
            assert f"void {function}(" in CLIENT or f"{function}(" in CLIENT, source


def test_secrets_stay_in_memory_and_leave_only_by_a_click():
    for source in (FEATURE, CLIENT, CRYPTO):
        # No settings file, no log line, no temp file ever holds a key or a login.
        for forbidden in ("QSettings", "qDebug", "qInfo", "qWarning", "QTemporaryFile", "std::cout"):
            assert forbidden not in source, forbidden
    # The one file the page writes is the encrypted export the person asked for.
    assert FEATURE.count("QSaveFile file(") == 1
    assert "setEchoMode(QLineEdit::Password)" in FEATURE
    assert "Qt::ImhHiddenText | Qt::ImhSensitiveData" in FEATURE
    # Keys are zeroed, not just dropped.
    assert "OPENSSL_cleanse" in CRYPTO and "Keyset::~Keyset() { clear(); }" in CRYPTO
    assert "void VaultClient::lock()" in CLIENT and "forgetItemKeys();" in CLIENT
    # Locking: the idle timer, the Lock button, and the page going away.
    assert "m_idleTimer->start(m_autoLockMinutes * 60 * 1000);" in FEATURE
    destructor = FEATURE.split("VaultFeature::~VaultFeature()", 1)[1].split("\n}\n", 1)[0]
    assert "m_client->lock();" in destructor
    lock = FEATURE.split("void VaultFeature::lockVault(", 1)[1].split("\n}\n", 1)[0]
    assert "dialog->reject();" in lock and "m_searchIndexChanged();" in lock
    # The clipboard is cleared after 30 s, and only if it still holds our text.
    assert "constexpr int kClipboardClearMs = 30000;" in FEATURE
    assert "clipboard->text() == m_clipboardValue" in FEATURE
    # Only an https address is ever handed to the system browser.
    open_site = FEATURE.split("void VaultFeature::openSite(", 1)[1].split("\n}\n", 1)[0]
    assert "loginHref(item->field(\"url\"))" in open_site and "QDesktopServices::openUrl(QUrl(href))" in open_site
    assert FEATURE.count("QDesktopServices::openUrl") == 1
    # The window's search box gets titles and usernames, never a password.
    hits = FEATURE.split("VaultFeature::searchHits(", 1)[1].split("\n}\n", 1)[0]
    assert "password" not in hits and "!m_client->unlocked()" in hits


def test_desktop_search_distinguishes_an_empty_vault_from_no_match_and_describes_results():
    render = FEATURE.split("void VaultFeature::renderItems()", 1)[1].split(
        "QString VaultFeature::selectedId()", 1)[0]
    assert ('QStringLiteral("No saved login matches \\"%1\\". Try a different search.")'
            in render)
    assert "m_empty->setAccessibleName(emptyText);" in render
    assert 'QStringLiteral("Showing %1 matching \\"%2\\".").arg(countText, query)' in render


def test_delete_offers_the_export_first_and_a_declined_save_stops_it():
    destroy = FEATURE.split("connect(destroy, &QPushButton::clicked", 1)[1].split("dialog.exec();", 1)[0]
    assert destroy.index("QMessageBox::warning(") < destroy.index("m_client->exportData(") \
        < destroy.index("saveExport(payload)") < destroy.index("m_client->destroy(")
    assert "Nothing was deleted: the export was not saved." in destroy


def test_the_client_enforces_what_the_browser_client_enforces():
    assert 'item.error = QStringLiteral("sender_keys_changed");' in CLIENT
    assert 'failed(QStringLiteral("recipient_keys_changed"));' in CLIENT
    assert 'return QStringLiteral("keyset_mismatch");' in CLIENT
    # A sender is pinned only after their signature held under those keys.
    load = CLIENT.split("void VaultClient::load(", 1)[1].split("void VaultClient::save(", 1)[0]
    assert load.index("openGrant(m_keyset") < load.index("m_pins.insert(sender.account, print);")
    # The same floor and the same defaults as the browser suite.
    assert "constexpr int kFloorMemoryKiB = 19456;" in CRYPTO and "constexpr int kFloorPasses = 2;" in CRYPTO
    assert "int memoryKiB = 65536;" in (DESKTOP / "VaultCrypto.h").read_text(encoding="utf-8")
    assert 'QByteArray::fromHex("5c2e2f2f5e5c")' in CRYPTO
    # Every primitive is OpenSSL's: nothing cryptographic is hand-rolled in C++.
    for primitive in ('"ARGON2ID"', '"HKDF"', "EVP_aes_256_gcm()", '"ML-KEM-768"', "EVP_sha3_256()",
                      "EVP_PKEY_ED25519", "EVP_PKEY_X25519", "OSSL_PKEY_PARAM_ML_KEM_SEED"):
        assert primitive in CRYPTO, primitive


def _toolchain():
    if not (shutil.which("g++") and shutil.which("pkg-config") and shutil.which("python3")):
        return ""
    probe = subprocess.run(["pkg-config", "--atleast-version=3.5", "openssl"], capture_output=True)
    flags = subprocess.run(["pkg-config", "--cflags", "--libs", "Qt6Core", "openssl"],
                           capture_output=True, text=True)
    return flags.stdout.strip() if probe.returncode == 0 and flags.returncode == 0 else ""


@pytest.mark.skipif(not _toolchain(), reason="needs g++, Qt6Core and OpenSSL 3.5 development files")
def test_cpp_suite_opens_what_the_browser_writes_and_drives_the_real_worker(tmp_path):
    binary = tmp_path / "forkmesh-vault-tests"
    sources = [str(DESKTOP / name) for name in (
        "VaultCrypto.cpp", "VaultClient.cpp", "tests/test_vault_crypto.cpp", "tests/test_vault_client_flow.cpp")]
    build = subprocess.run(
        ["g++", "-std=c++20", "-O1", "-fPIC", "-Wall", "-Wextra", "-Werror", f"-I{DESKTOP}", *sources,
         "-o", str(binary), *_toolchain().split()], capture_output=True, text=True, timeout=900)
    assert build.returncode == 0, build.stderr[-3000:]
    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=600,
                         env={"PATH": "/usr/local/bin:/usr/bin:/bin",
                              "FORKMESH_VAULT_FIXTURES": str(PACKAGE / "tests" / "fixtures")})
    assert run.returncode == 0, run.stdout[-3000:] + run.stderr[-3000:]
    assert "0 failures" in run.stdout and "skipped" not in run.stdout
