from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WINDOW = ROOT / "desktop" / "src" / "MainWindow.cpp"
SETUP = ROOT / "desktop" / "src" / "MainWindowSetup.cpp"
MARKET = ROOT / "desktop" / "src" / "MainWindowMarket.cpp"


def _start_networking() -> str:
    source = WINDOW.read_text(encoding="utf-8")
    start = source.index("auto startNetworking = [&] {")
    return source[start:source.index("if (m_pendingRestoreRepoIndex >= 0 &&", start)]


def _authenticate_silently() -> str:
    source = SETUP.read_text(encoding="utf-8")
    start = source.index("bool MainWindow::authenticateSilently(")
    return source[start:source.index("\nnamespace {", start)]


def test_deferred_startup_prepares_package_pages_whatever_sign_in_decided():
    block = _start_networking()

    assert "preparePackagePages();" in block
    assert block.index("m_startupAuthResolved = true;") < block.index(
        "preparePackagePages();")
    assert "hasActiveAccountSession()" not in block


def test_a_restored_account_has_no_other_path_that_prepares_the_pages():
    assert "activateAccountSession(" not in _authenticate_silently()
    assert "preparePackagePages" not in _authenticate_silently()


def test_rebuilding_the_package_pages_cannot_be_relied_on_to_prepare_them():
    source = MARKET.read_text(encoding="utf-8")
    start = source.index("void MainWindow::syncNativePackagePages()")
    head = source[start:source.index("preparePackagePage(widget);", start)]

    assert "if (desired == m_nativePackagePagesWanted)\n        return;" in head
