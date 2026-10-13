from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
QT_REPO_DETAIL = ROOT / "desktop" / "src" / "MainWindowRepoDetail.cpp"


def test_action_strip_is_parented_to_repo_detail_page_not_tab_viewport():
    source = QT_REPO_DETAIL.read_text(encoding="utf-8")
    start = source.index("QWidget *MainWindow::buildRepoDetailSection()")
    end = source.index("void MainWindow::updateRepoActivityRail()", start)
    body = source[start:end]

    assert "auto *page = new QWidget;" in body
    assert "tabRow->addWidget(m_forkButton" in body
    assert "tabRow->addWidget(m_mirrorButton" in body
    assert "tabRow->addWidget(m_sourceButton" in body
    assert "tabRow->addWidget(m_repoOpenButton" in body
    assert "tabBarScroll->setWidget(tabBar);" in body
    assert "chromeLayout->addWidget(tabBarScroll);" in body
    assert "layout->addLayout(content, 1);" in body
