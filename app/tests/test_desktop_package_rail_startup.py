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


def _market_section(start: str, end: str) -> str:
    source = MARKET.read_text(encoding="utf-8")
    first = source.index(start)
    return source[first:source.index(end, first)]


def test_installing_one_package_keeps_the_other_package_pages():
    sync = _market_section("void MainWindow::syncNativePackagePages()",
                           "void MainWindow::buildDeferredNativePackagePages()")

    assert "m_nativePackagePages.clear();" not in sync
    assert "m_nativePackagePages.takeAt(i)" in sync
    assert "for (const NativePackagePage &page : std::as_const(removed))" in sync
    assert "missing.size() > 1" in sync


def test_market_previews_reach_the_gui_thread_in_the_pixmap_format():
    decode = _market_section("QPixmap MainWindow::marketPreviewPixmap(",
                             "void MainWindow::flushMarketPreviews()")

    assert decode.index("image.convertTo(") < decode.index(
        "void MainWindow::applyMarketPreview(")
    assert "QPixmap::fromImage(image, Qt::NoOpaqueDetection)" in decode


def test_crm_column_moves_do_not_refit_every_row_per_move():
    crm = (ROOT / "marketplace" / "packages" / "crm" / "content" / "desktop" /
           "CrmFeature.cpp").read_text(encoding="utf-8")
    first = crm.index("void CrmFeature::applyColumnOrder()")
    order = crm[first:crm.index("QJsonArray CrmFeature::tableViews() const", first)]

    assert order.index("QHeaderView::Fixed") < order.index("header->moveSection(from, to);")
    assert "header->moveSection(header->visualIndex" not in order
    assert order.rindex("QHeaderView::ResizeToContents);") > order.index(
        "header->moveSection(from, to);")
