#include "EmberFeature.h"

#include "EmberWidgets.h"
#include "RepoInsightBar.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QScrollArea>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>

namespace forkmesh::ember {

using insight::FileInfo;
using insight::Snapshot;
using insight::SnapshotPtr;

namespace {

QLabel *sectionLabel(const QString &text)
{
    auto *label = new QLabel(text);
    label->setObjectName(QStringLiteral("mutedLabel"));
    QFont f = label->font();
    f.setWeight(QFont::DemiBold);
    label->setFont(f);
    return label;
}

QFrame *separator()
{
    auto *line = new QFrame;
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Plain);
    line->setStyleSheet(QStringLiteral("color: rgba(128,128,128,60);"));
    return line;
}

QString pathCrumb(const QString &folder)
{
    if (folder.isEmpty())
        return QStringLiteral("/");
    return folder.split(QLatin1Char('/')).join(QStringLiteral(" / "));
}

QString bandStyle(const QColor &color)
{
    return QStringLiteral("background:%1; color:#0d1117; border-radius:9px; padding:2px 10px; font-weight:600;")
        .arg(color.name());
}

int longestFunction(const FileInfo &file)
{
    int longest = 0;
    for (const insight::FunctionInfo &fn : file.functions)
        longest = std::max(longest, fn.length);
    return longest;
}

}

EmberFeature::EmberFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent)
    , m_host(host)
{
    setObjectName(QStringLiteral("emberPage"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_bar = new insight::RepoInsightBar(
        host, QStringLiteral("Ember"),
        QStringLiteral("File-level hotspots: which files are hard to maintain and change often"), this);
    outer->addWidget(m_bar);

    auto *tabsRow = new QHBoxLayout;
    tabsRow->setContentsMargins(20, 2, 20, 0);
    tabsRow->setSpacing(12);
    m_tabs = new QTabBar;
    m_tabs->setObjectName(QStringLiteral("emberTabs"));
    m_tabs->setDrawBase(false);
    m_tabs->setExpanding(false);
    m_tabs->setDocumentMode(true);
    for (Tab tab : allTabs())
        m_tabs->addTab(tabTitle(tab));
    tabsRow->addWidget(m_tabs);
    tabsRow->addStretch(1);
    outer->addLayout(tabsRow);

    m_crumbs = new QLabel;
    m_crumbs->setObjectName(QStringLiteral("emberBreadcrumb"));
    m_crumbs->setTextFormat(Qt::RichText);
    m_crumbs->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    m_crumbs->setContentsMargins(20, 6, 20, 6);
    outer->addWidget(m_crumbs);

    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(1);
    m_view = new EmberMapView;
    splitter->addWidget(m_view);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMinimumWidth(410);
    scroll->setMaximumWidth(480);
    auto *panel = new QWidget;
    panel->setObjectName(QStringLiteral("emberPanel"));
    buildPanel(panel);
    scroll->setWidget(panel);
    splitter->addWidget(scroll);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setSizes({1180, 420});
    outer->addWidget(splitter, 1);

    m_filterTimer = new QTimer(this);
    m_filterTimer->setSingleShot(true);
    m_filterTimer->setInterval(90);
    connect(m_filterTimer, &QTimer::timeout, this, [this] { applyFilters(); });

    connect(m_tabs, &QTabBar::currentChanged, this, [this](int index) {
        if (m_syncing || index < 0)
            return;
        m_tabChosen = true;
        setTab(allTabs().value(index, Tab::CodeHealth));
    });
    connect(m_crumbs, &QLabel::linkActivated, this, [this](const QString &link) {
        if (link.startsWith(QStringLiteral("node:")))
            m_view->focusNode(link.mid(5).toInt(), true);
    });

    m_view->setMessage(QStringLiteral("Open or clone a repository, or pick a folder to analyze."));
    m_view->setOnSelect([this](int file) {
        m_selected = file;
        updateCard();
        updateCrumbs();
    });
    m_view->setOnFocusChanged([this](int) { updateCrumbs(); });
    m_view->setOnOpenFile([this](int file) {
        if (m_snapshot && file >= 0 && file < m_snapshot->files.size())
            m_bar->openFile(m_snapshot->files[file].path);
    });

    m_bar->setOnSnapshot([this](SnapshotPtr snapshot) { showSnapshot(std::move(snapshot)); });
    m_bar->setOnRootChanged([this](const QString &) { resetData(); });

    auto *find = new QShortcut(QKeySequence::Find, this);
    find->setContext(Qt::WidgetWithChildrenShortcut);
    connect(find, &QShortcut::activated, this, [this] {
        m_search->setFocus(Qt::ShortcutFocusReason);
        m_search->selectAll();
    });
    auto *slash = new QShortcut(QKeySequence(QStringLiteral("/")), this);
    slash->setContext(Qt::WidgetWithChildrenShortcut);
    connect(slash, &QShortcut::activated, this, [this] {
        m_search->setFocus(Qt::ShortcutFocusReason);
        m_search->selectAll();
    });

    updateCard();
    updateCrumbs();
}

EmberFeature::~EmberFeature()
{
    if (m_prepareWatcher)
        m_prepareWatcher->disconnect(this);
}

QLabel *EmberFeature::addMetric(QGridLayout *grid, const QString &name)
{
    const int row = grid->rowCount();
    auto *label = new QLabel(name);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    auto *value = new QLabel;
    value->setWordWrap(true);
    value->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    value->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    grid->addWidget(label, row, 0);
    grid->addWidget(value, row, 1);
    m_metricRows.append(qMakePair(label, value));
    return value;
}

void EmberFeature::buildPanel(QWidget *panel)
{
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(16, 8, 18, 18);
    layout->setSpacing(8);

    layout->addWidget(sectionLabel(QStringLiteral("Search by filename")));
    m_search = new QLineEdit;
    m_search->setPlaceholderText(QStringLiteral("Search by filename  ( / )"));
    m_search->setClearButtonEnabled(true);
    m_search->setAccessibleName(QStringLiteral("Search by filename"));
    layout->addWidget(m_search);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_filters.search = text;
        m_filterTimer->start();
    });
    connect(m_search, &QLineEdit::returnPressed, this, [this] { selectBestMatch(); });

    layout->addWidget(sectionLabel(QStringLiteral("Filter by authors")));
    m_authors = new QComboBox;
    m_authors->setAccessibleName(QStringLiteral("Filter by authors"));
    m_authors->addItem(QStringLiteral("All authors"), QString());
    layout->addWidget(m_authors);
    connect(m_authors, &QComboBox::activated, this, [this](int index) {
        m_filters.author = m_authors->itemData(index).toString();
        applyFilters();
    });

    layout->addWidget(sectionLabel(QStringLiteral("Code health range")));
    m_health = new RangeSlider;
    m_health->setRange(1, 10, 0.5);
    m_health->setValues(1, 10);
    m_health->setAccessibleName(QStringLiteral("Code health range"));
    layout->addWidget(m_health);
    m_health->setOnChanged([this](double low, double high) {
        m_filters.minHealth = low;
        m_filters.maxHealth = high;
        m_filterTimer->start();
    });

    auto *commitsRow = new QHBoxLayout;
    commitsRow->addWidget(sectionLabel(QStringLiteral("Commit threshold")));
    commitsRow->addStretch(1);
    m_commitsLabel = new QLabel(QStringLiteral("any"));
    m_commitsLabel->setObjectName(QStringLiteral("mutedLabel"));
    commitsRow->addWidget(m_commitsLabel);
    layout->addLayout(commitsRow);
    m_commits = new RangeSlider;
    m_commits->setSingle(true);
    m_commits->setRange(0, 50, 1);
    m_commits->setValues(0, 50);
    m_commits->setMinimumHeight(24);
    m_commits->setAccessibleName(QStringLiteral("Minimum commits"));
    layout->addWidget(m_commits);
    m_commits->setOnChanged([this](double low, double) {
        const int value = int(std::lround(low));
        m_filters.minCommits = value;
        m_commitsLabel->setText(value == 0 ? QStringLiteral("any")
                                           : QStringLiteral("at least %1 commits").arg(value));
        m_filterTimer->start();
    });

    layout->addSpacing(4);
    layout->addWidget(separator());

    m_targetsBox = new QWidget;
    auto *targetsLayout = new QVBoxLayout(m_targetsBox);
    targetsLayout->setContentsMargins(0, 4, 0, 4);
    targetsLayout->setSpacing(6);
    auto *targetsTitle = new QLabel(QStringLiteral("Refactoring targets"));
    QFont tf = targetsTitle->font();
    tf.setPixelSize(15);
    tf.setBold(true);
    targetsTitle->setFont(tf);
    targetsLayout->addWidget(targetsTitle);
    m_targetsIntro = new QLabel(QStringLiteral(
        "Frequently changed files with low code health, ranked by hotspot × (10 − health). "
        "Improving these pays back on every future change."));
    m_targetsIntro->setObjectName(QStringLiteral("mutedLabel"));
    m_targetsIntro->setWordWrap(true);
    targetsLayout->addWidget(m_targetsIntro);
    m_targets = new RowList;
    m_targets->setEmptyText(QStringLiteral("No refactoring targets."));
    targetsLayout->addWidget(m_targets);
    targetsLayout->addWidget(separator());
    layout->addWidget(m_targetsBox);
    m_targets->setOnActivated([this](int file) { selectFile(file, true); });
    m_targets->setOnOpened([this](int file) {
        if (m_snapshot && file >= 0)
            m_bar->openFile(m_snapshot->files[file].path);
    });

    m_overview = new QWidget;
    auto *overviewLayout = new QVBoxLayout(m_overview);
    overviewLayout->setContentsMargins(0, 4, 0, 0);
    overviewLayout->setSpacing(6);
    auto *overviewTitle = new QLabel(QStringLiteral("Code health"));
    overviewTitle->setFont(tf);
    overviewLayout->addWidget(overviewTitle);
    m_bands = new RowList;
    overviewLayout->addWidget(m_bands);
    m_hotspotsTitle = sectionLabel(QStringLiteral("Top hotspots"));
    overviewLayout->addWidget(m_hotspotsTitle);
    m_hotspots = new RowList;
    overviewLayout->addWidget(m_hotspots);
    auto *hint = new QLabel(QStringLiteral(
        "Click a file on the map to see its metrics. Click a folder to zoom in, Esc to zoom out, "
        "scroll to zoom, drag to pan, double-click a file to open it."));
    hint->setObjectName(QStringLiteral("mutedLabel"));
    hint->setWordWrap(true);
    overviewLayout->addWidget(hint);
    layout->addWidget(m_overview);
    m_hotspots->setOnActivated([this](int file) { selectFile(file, true); });
    m_bands->setOnActivated([this](int band) {
        const double ranges[3][2] = {{8, 10}, {4, 7.5}, {1, 3.5}};
        if (band < 0 || band > 2)
            return;
        const bool same = m_filters.minHealth == ranges[band][0] && m_filters.maxHealth == ranges[band][1];
        m_health->setValues(same ? 1 : ranges[band][0], same ? 10 : ranges[band][1]);
        m_filters.minHealth = m_health->low();
        m_filters.maxHealth = m_health->high();
        applyFilters();
    });

    m_card = new QWidget;
    auto *card = new QVBoxLayout(m_card);
    card->setContentsMargins(0, 4, 0, 0);
    card->setSpacing(6);
    m_cardCrumb = new QLabel;
    m_cardCrumb->setObjectName(QStringLiteral("mutedLabel"));
    m_cardCrumb->setWordWrap(true);
    card->addWidget(m_cardCrumb);
    m_cardName = new QLabel;
    QFont nf = m_cardName->font();
    nf.setPixelSize(18);
    nf.setBold(true);
    m_cardName->setFont(nf);
    m_cardName->setWordWrap(true);
    m_cardName->setTextInteractionFlags(Qt::TextSelectableByMouse);
    card->addWidget(m_cardName);
    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(6);
    auto *open = new QPushButton(QStringLiteral("Open file"));
    auto *reveal = new QPushButton(QStringLiteral("Reveal in folder"));
    auto *copy = new QPushButton(QStringLiteral("Copy path"));
    for (QPushButton *button : {open, reveal, copy}) {
        button->setObjectName(QStringLiteral("ghostButton"));
        button->setCursor(Qt::PointingHandCursor);
        buttons->addWidget(button);
    }
    buttons->addStretch(1);
    card->addLayout(buttons);
    connect(open, &QPushButton::clicked, this, [this] {
        if (m_snapshot && m_selected >= 0)
            m_bar->openFile(m_snapshot->files[m_selected].path);
    });
    connect(reveal, &QPushButton::clicked, this, [this] {
        if (!m_snapshot || m_selected < 0)
            return;
        const QString absolute = QDir(m_bar->root()).filePath(m_snapshot->files[m_selected].path);
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(absolute).absolutePath()));
    });
    connect(copy, &QPushButton::clicked, this, [this] {
        if (m_snapshot && m_selected >= 0)
            QGuiApplication::clipboard()->setText(m_snapshot->files[m_selected].path);
    });

    m_cardTabs = new QTabBar;
    m_cardTabs->setDrawBase(false);
    m_cardTabs->setExpanding(false);
    m_cardTabs->setDocumentMode(true);
    m_cardTabs->addTab(QStringLiteral("Metrics"));
    m_cardTabs->addTab(QStringLiteral("Complexity trend"));
    m_cardTabs->addTab(QStringLiteral("Coupling"));
    m_cardTabs->setTabToolTip(2, QStringLiteral("Change coupling: files committed together with this one"));
    m_cardTabs->setUsesScrollButtons(false);
    card->addWidget(m_cardTabs);
    m_cardStack = new QStackedWidget;
    card->addWidget(m_cardStack);
    connect(m_cardTabs, &QTabBar::currentChanged, this, [this](int index) {
        m_cardStack->setCurrentIndex(index);
        if (index == 1)
            loadTrend();
    });

    auto *metrics = new QWidget;
    auto *metricsLayout = new QVBoxLayout(metrics);
    metricsLayout->setContentsMargins(0, 8, 0, 0);
    metricsLayout->setSpacing(10);
    auto *healthRow = new QHBoxLayout;
    healthRow->setSpacing(14);
    m_ring = new HealthRing;
    healthRow->addWidget(m_ring);
    auto *healthText = new QVBoxLayout;
    healthText->setSpacing(4);
    healthText->addStretch(1);
    auto *healthTitle = sectionLabel(QStringLiteral("Code health"));
    healthText->addWidget(healthTitle);
    auto *badgeRow = new QHBoxLayout;
    m_band = new QLabel;
    badgeRow->addWidget(m_band);
    badgeRow->addStretch(1);
    healthText->addLayout(badgeRow);
    m_bandNote = new QLabel;
    m_bandNote->setObjectName(QStringLiteral("mutedLabel"));
    m_bandNote->setWordWrap(true);
    healthText->addWidget(m_bandNote);
    healthText->addStretch(1);
    healthRow->addLayout(healthText, 1);
    metricsLayout->addLayout(healthRow);
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(7);
    grid->setColumnStretch(1, 1);
    m_commitsValue = addMetric(grid, QStringLiteral("Commits"));
    m_sizeValue = addMetric(grid, QStringLiteral("Size"));
    m_authorValue = addMetric(grid, QStringLiteral("Main author"));
    m_knowledgeValue = addMetric(grid, QStringLiteral("Knowledge"));
    m_defectsValue = addMetric(grid, QStringLiteral("Defects"));
    m_modifiedValue = addMetric(grid, QStringLiteral("Modified"));
    m_complexityValue = addMetric(grid, QStringLiteral("Complexity"));
    m_functionsValue = addMetric(grid, QStringLiteral("Functions"));
    m_duplicationValue = addMetric(grid, QStringLiteral("Duplication"));
    m_importsValue = addMetric(grid, QStringLiteral("Imports"));
    m_languageValue = addMetric(grid, QStringLiteral("Language"));
    metricsLayout->addLayout(grid);
    connect(m_complexityValue, &QLabel::linkActivated, this, [this](const QString &link) {
        if (m_snapshot && m_selected >= 0)
            m_bar->openFile(m_snapshot->files[m_selected].path, link.mid(5).toInt());
    });
    metricsLayout->addWidget(sectionLabel(QStringLiteral("Health issues")));
    m_issues = new QLabel;
    m_issues->setWordWrap(true);
    m_issues->setTextFormat(Qt::RichText);
    metricsLayout->addWidget(m_issues);
    metricsLayout->addStretch(1);
    m_cardStack->addWidget(metrics);

    m_trend = new TrendChart;
    m_cardStack->addWidget(m_trend);

    auto *couplingPage = new QWidget;
    auto *couplingLayout = new QVBoxLayout(couplingPage);
    couplingLayout->setContentsMargins(0, 8, 0, 0);
    auto *couplingIntro = new QLabel(QStringLiteral(
        "Files committed together with this one. Degree is the share of their commits made together."));
    couplingIntro->setObjectName(QStringLiteral("mutedLabel"));
    couplingIntro->setWordWrap(true);
    couplingLayout->addWidget(couplingIntro);
    m_coupled = new RowList;
    m_coupled->setEmptyText(QStringLiteral("This file does not change together with other files often enough to count as coupled."));
    couplingLayout->addWidget(m_coupled);
    couplingLayout->addStretch(1);
    m_cardStack->addWidget(couplingPage);
    m_coupled->setOnActivated([this](int file) { selectFile(file, true); });
    m_coupled->setOnOpened([this](int file) {
        if (m_snapshot && file >= 0)
            m_bar->openFile(m_snapshot->files[file].path);
    });

    layout->addWidget(m_card);
    layout->addStretch(1);
}

void EmberFeature::refresh()
{
    m_bar->refresh();
}

void EmberFeature::repositoryChanged()
{
    m_bar->repositoryChanged();
}

void EmberFeature::resetData()
{
    if (m_prepareWatcher) {
        m_prepareWatcher->disconnect(this);
        m_prepareWatcher.clear();
    }
    m_snapshot.reset();
    m_layout.reset();
    m_coupling.reset();
    m_selected = -1;
    m_trends.clear();
    m_trendLoading.clear();
    m_coloring = TabColoring();
    m_view->clearData();
    m_view->setMessage(QStringLiteral("Reading the repository…"));
    updateCard();
    updateCrumbs();
}

void EmberFeature::showSnapshot(SnapshotPtr snapshot)
{
    if (!snapshot)
        return;
    if (!snapshot->error.isEmpty() || snapshot->files.isEmpty()) {
        resetData();
        m_snapshot = snapshot;
        m_view->setMessage(!snapshot->error.isEmpty()
                               ? snapshot->error
                               : QStringLiteral("No source files found here. Pick another repository or folder."));
        return;
    }
    if (m_prepareWatcher)
        m_prepareWatcher->disconnect(this);
    auto *watcher = new QFutureWatcher<std::shared_ptr<Prepared>>(this);
    m_prepareWatcher = watcher;
    connect(watcher, &QFutureWatcher<std::shared_ptr<Prepared>>::finished, this, [this, watcher] {
        watcher->deleteLater();
        if (watcher != m_prepareWatcher)
            return;
        m_prepareWatcher.clear();
        if (watcher->future().resultCount() > 0)
            prepared(watcher->result());
    });
    watcher->setFuture(QtConcurrent::run([snapshot] {
        auto result = std::make_shared<Prepared>();
        result->snapshot = snapshot;
        result->layout = std::make_shared<const MapLayout>(buildLayout(*snapshot));
        result->coupling = std::make_shared<const CouplingIndex>(buildCouplingIndex(*snapshot));
        return result;
    }));
}

bool EmberFeature::ready() const
{
    return m_layout && !m_prepareWatcher && m_snapshot;
}

void EmberFeature::prepared(std::shared_ptr<Prepared> result)
{
    if (!result || !result->snapshot)
        return;
    const QString previousPath = m_snapshot && m_selected >= 0 && m_selected < m_snapshot->files.size()
                                     ? m_snapshot->files[m_selected].path
                                     : QString();
    if (m_snapshot != result->snapshot)
        m_trends.clear();
    m_snapshot = result->snapshot;
    m_layout = result->layout;
    m_coupling = result->coupling;
    m_selected = -1;
    const Snapshot &s = *m_snapshot;
    const bool history = hasHistory(s);

    m_authors->clear();
    m_authors->addItem(QStringLiteral("All authors"), QString());
    const QStringList authors = topAuthors(s, 0);
    QStringList all = s.authors;
    for (const QString &a : authors) {
        if (!all.contains(a))
            all.append(a);
    }
    const QSet<QString> former(s.formerAuthors.cbegin(), s.formerAuthors.cend());
    for (const QString &a : authors)
        m_authors->addItem(former.contains(a) ? a + QStringLiteral(" (former)") : a, a);
    for (const QString &a : all) {
        if (!authors.contains(a))
            m_authors->addItem(former.contains(a) ? a + QStringLiteral(" (former)") : a, a);
    }
    const int authorIndex = m_authors->findData(m_filters.author);
    if (authorIndex > 0)
        m_authors->setCurrentIndex(authorIndex);
    else {
        m_filters.author.clear();
        m_authors->setCurrentIndex(0);
    }
    m_authors->setEnabled(history && m_authors->count() > 1);
    int maxCommits = 0;
    for (const FileInfo &file : s.files)
        maxCommits = std::max(maxCommits, file.commits);
    const int commitMax = std::max(1, std::min(maxCommits, 200));
    m_commits->setRange(0, commitMax, 1);
    m_commits->setValues(std::min(m_filters.minCommits, commitMax), commitMax);
    m_filters.minCommits = int(m_commits->low());
    m_commits->setEnabled(history);

    if (!m_tabChosen)
        m_tab = history ? Tab::Hotspots : Tab::CodeHealth;
    else if (!history && tabNeedsHistory(m_tab))
        m_tab = Tab::CodeHealth;
    m_syncing = true;
    m_tabs->setCurrentIndex(allTabs().indexOf(m_tab));
    for (int i = 0; i < allTabs().size(); ++i)
        m_tabs->setTabToolTip(i, tabNeedsHistory(allTabs()[i]) && !history
                                     ? QStringLiteral("Needs git history")
                                     : QString());
    m_syncing = false;

    m_view->setData(m_snapshot, m_layout, m_coupling);
    recolor();
    m_bar->setSummary(summarize(s).text);
    updateOverview();
    int restore = -1;
    if (!m_pendingPath.isEmpty())
        restore = s.indexOf(m_pendingPath);
    if (restore < 0 && !previousPath.isEmpty())
        restore = s.indexOf(previousPath);
    if (restore < 0 && !m_pendingPath.isEmpty()) {
        for (int i = 0; i < s.files.size(); ++i) {
            if (s.files[i].path == m_pendingPath) {
                restore = i;
                break;
            }
        }
    }
    m_pendingPath.clear();
    if (restore >= 0)
        selectFile(restore, true);
    else
        updateCard();
    updateCrumbs();
    if (m_host.searchIndexChanged)
        m_host.searchIndexChanged();
}

void EmberFeature::setTab(Tab tab)
{
    m_tab = tab;
    m_syncing = true;
    m_tabs->setCurrentIndex(allTabs().indexOf(tab));
    m_syncing = false;
    recolor();
}

void EmberFeature::setFilters(const Filters &filters)
{
    m_filters = filters;
    m_syncing = true;
    m_health->setValues(filters.minHealth, filters.maxHealth);
    m_commits->setValues(filters.minCommits, 1e9);
    if (m_search->text() != filters.search)
        m_search->setText(filters.search);
    m_syncing = false;
    m_filters = filters;
    applyFilters();
}

void EmberFeature::applyFilters()
{
    m_filterTimer->stop();
    recolor();
}

void EmberFeature::recolor()
{
    if (!m_snapshot || !m_layout || !m_coupling) {
        m_targetsBox->setVisible(false);
        return;
    }
    m_coloring = colorize(*m_snapshot, m_tab, m_filters, *m_coupling);
    m_view->setColoring(m_coloring);
    updateTargets();
}

void EmberFeature::updateTargets()
{
    const bool show = m_tab == Tab::RefactoringTargets && m_snapshot && hasHistory(*m_snapshot);
    m_targetsBox->setVisible(show);
    if (!show)
        return;
    QList<RowItem> rows;
    const double top = m_coloring.targets.isEmpty() ? 1 : m_coloring.targets.first().score;
    const QLocale locale;
    for (const Target &t : m_coloring.targets) {
        const FileInfo &f = m_snapshot->files[t.file];
        RowItem row;
        row.id = t.file;
        row.rank = t.rank;
        row.title = fileNameOf(f.path);
        row.detail = folderOf(f.path);
        row.value = QStringLiteral("health %1").arg(locale.toString(f.health, 'f', 1));
        row.fraction = t.score / std::max(1e-9, top);
        row.color = healthColor(f.health);
        row.notes = t.reasons.mid(0, 1) + t.reasons.mid(2, 2);
        rows.append(row);
    }
    m_targets->setRows(rows);
    m_targets->setSelectedId(m_selected);
}

void EmberFeature::updateOverview()
{
    if (!m_snapshot)
        return;
    const Summary s = summarize(*m_snapshot);
    const qint64 total = std::max<qint64>(1, s.healthyLines + s.problematicLines + s.unhealthyLines);
    const QLocale locale;
    QList<RowItem> bands;
    const auto band = [&](int id, const QString &name, int files, qint64 lines, insight::HealthBand b) {
        RowItem row;
        row.id = id;
        row.title = name;
        row.detail = QStringLiteral("%1 files · %2 lines of code").arg(locale.toString(files), locale.toString(lines));
        row.value = percentText(double(lines) / total) + QStringLiteral(" of code");
        row.fraction = double(lines) / total;
        row.color = bandColor(b);
        bands.append(row);
    };
    band(0, QStringLiteral("Healthy"), s.healthy, s.healthyLines, insight::HealthBand::Healthy);
    band(1, QStringLiteral("Problematic"), s.problematic, s.problematicLines, insight::HealthBand::Problematic);
    band(2, QStringLiteral("Unhealthy"), s.unhealthy, s.unhealthyLines, insight::HealthBand::Unhealthy);
    m_bands->setRows(bands);

    const bool history = hasHistory(*m_snapshot);
    QVector<int> order;
    for (int i = 0; i < m_snapshot->files.size(); ++i) {
        if (m_snapshot->files[i].hotspot > 0)
            order.append(i);
    }
    std::sort(order.begin(), order.end(), [this](int a, int b) {
        return m_snapshot->files[a].hotspot > m_snapshot->files[b].hotspot;
    });
    QList<RowItem> hot;
    for (int i = 0; i < std::min<int>(8, order.size()); ++i) {
        const FileInfo &f = m_snapshot->files[order[i]];
        RowItem row;
        row.id = order[i];
        row.title = fileNameOf(f.path);
        row.detail = folderOf(f.path);
        row.value = QStringLiteral("%1 commits").arg(locale.toString(f.commits));
        row.fraction = f.hotspot;
        row.color = healthColor(f.health);
        hot.append(row);
    }
    m_hotspots->setRows(hot);
    m_hotspots->setEmptyText(history ? QStringLiteral("No file changed in the history window.")
                                     : QStringLiteral("Hotspots need git history; this folder has none."));
    m_hotspotsTitle->setVisible(true);
}

void EmberFeature::selectFile(int file, bool zoomTo)
{
    if (!m_snapshot || file < 0 || file >= m_snapshot->files.size())
        file = -1;
    m_selected = file;
    m_view->setSelectedFile(file, zoomTo);
    updateCard();
    updateCrumbs();
}

void EmberFeature::updateCard()
{
    const bool has = m_snapshot && m_selected >= 0 && m_selected < m_snapshot->files.size();
    m_card->setVisible(has);
    m_overview->setVisible(!has && m_snapshot && !m_snapshot->files.isEmpty());
    m_targets->setSelectedId(m_selected);
    m_hotspots->setSelectedId(m_selected);
    if (!has)
        return;
    const Snapshot &s = *m_snapshot;
    const FileInfo &f = s.files[m_selected];
    const QLocale locale;
    const bool history = hasHistory(s);
    const QString span = spanText(s.historyDays > 0 ? s.historyDays : 365);
    m_cardCrumb->setText(pathCrumb(folderOf(f.path)));
    m_cardName->setText(fileNameOf(f.path));
    m_ring->setHealth(f.health);
    const insight::HealthBand band = insight::healthBand(f.health);
    QString bandName = insight::healthBandName(band);
    if (bandName.isEmpty())
        bandName = band == insight::HealthBand::Healthy ? QStringLiteral("Healthy")
                   : band == insight::HealthBand::Problematic ? QStringLiteral("Problematic")
                                                               : QStringLiteral("Unhealthy");
    m_band->setText(bandName);
    m_band->setStyleSheet(bandStyle(bandColor(band)));
    QString note;
    if (m_selected < m_coloring.files.size() && m_coloring.files[m_selected].rank > 0)
        note = QStringLiteral("Refactoring target #%1").arg(m_coloring.files[m_selected].rank);
    else if (f.hotspot >= 0.5)
        note = QStringLiteral("Hotspot: changes often and is large");
    else if (f.test)
        note = QStringLiteral("Test file");
    m_bandNote->setText(note);
    m_bandNote->setVisible(!note.isEmpty());

    const QString noHistory = QStringLiteral("No git history");
    m_commitsValue->setText(history ? QStringLiteral("%1 commits / %2 · %3 in the last 90 days")
                                          .arg(locale.toString(f.commits), span, locale.toString(f.recentCommits))
                                    : noHistory);
    m_sizeValue->setText(QStringLiteral("%1 lines of code · %2 lines in total")
                             .arg(locale.toString(f.codeLines), locale.toString(f.lines)));
    m_authorValue->setText(history && !f.mainAuthor.isEmpty()
                               ? QStringLiteral("%1 (%2) · %3 authors")
                                     .arg(f.mainAuthor, percentText(f.mainAuthorShare), locale.toString(f.authors))
                               : noHistory);
    m_knowledgeValue->setText(history ? QStringLiteral("%1 code by former contributors").arg(percentText(f.formerAuthorShare))
                                      : noHistory);
    m_defectsValue->setText(history ? QStringLiteral("%1 fix commits / %2").arg(locale.toString(f.fixCommits), span)
                                    : noHistory);
    m_modifiedValue->setText(history && f.lastChanged > 0
                                 ? QStringLiteral("%1 · %2")
                                       .arg(agoText(f.lastChanged, QDateTime::currentSecsSinceEpoch()),
                                            QDateTime::fromSecsSinceEpoch(f.lastChanged).toString(QStringLiteral("d MMM yyyy")))
                                 : noHistory);
    const insight::FunctionInfo *worst = nullptr;
    for (const insight::FunctionInfo &fn : f.functions) {
        if (!worst || fn.complexity > worst->complexity)
            worst = &fn;
    }
    QString complexity = QStringLiteral("%1 in total").arg(locale.toString(f.complexity));
    if (worst)
        complexity += QStringLiteral(" · worst %1() %2 at <a href=\"line:%3\">line %3</a>")
                          .arg(worst->name.toHtmlEscaped(), locale.toString(worst->complexity))
                          .arg(worst->line);
    m_complexityValue->setText(complexity);
    m_functionsValue->setText(QStringLiteral("%1 functions · longest %2 lines · deepest nesting %3")
                                  .arg(locale.toString(f.functions.size()), locale.toString(longestFunction(f)),
                                       locale.toString(f.maxNesting)));
    m_duplicationValue->setText(f.duplicatedLines > 0
                                    ? QStringLiteral("%1 lines in repeated blocks").arg(locale.toString(f.duplicatedLines))
                                    : QStringLiteral("No repeated blocks"));
    QString imports = QStringLiteral("imports %1 files · used by %2 files")
                          .arg(locale.toString(f.dependsOn.size()), locale.toString(f.dependedOnBy.size()));
    if (!f.externalImports.isEmpty())
        imports += QStringLiteral(" · %1 packages").arg(locale.toString(f.externalImports.size()));
    if (f.cycle >= 0 && f.cycle < s.cycles.size())
        imports += QStringLiteral(" · in an import cycle of %1 files").arg(s.cycles[f.cycle].size());
    m_importsValue->setText(imports);
    m_languageValue->setText(f.test ? f.language + QStringLiteral(" · test file") : f.language);

    if (f.healthIssues.isEmpty()) {
        m_issues->setText(QStringLiteral("<span>No issues found.</span>"));
    } else {
        QStringList items;
        for (const QString &issue : f.healthIssues)
            items << QStringLiteral("• %1").arg(issue.toHtmlEscaped());
        m_issues->setText(items.join(QStringLiteral("<br>")));
    }
    updateCoupling();
    m_trend->setPoints({});
    if (m_cardTabs->currentIndex() == 1)
        loadTrend();
}

void EmberFeature::updateCoupling()
{
    QList<RowItem> rows;
    if (m_snapshot && m_coupling && m_selected >= 0 && m_selected < m_coupling->size()) {
        const QLocale locale;
        for (const CouplingRef &ref : (*m_coupling)[m_selected]) {
            const FileInfo &f = m_snapshot->files[ref.file];
            RowItem row;
            row.id = ref.file;
            row.title = fileNameOf(f.path);
            row.detail = QStringLiteral("%1 · %2 shared commits")
                             .arg(folderOf(f.path).isEmpty() ? QStringLiteral("/") : folderOf(f.path),
                                  locale.toString(ref.shared));
            row.value = percentText(ref.degree);
            row.fraction = ref.degree;
            row.color = couplingLinkColor();
            rows.append(row);
        }
    }
    m_coupled->setRows(rows);
    if (m_snapshot && !hasHistory(*m_snapshot))
        m_coupled->setEmptyText(QStringLiteral("Change coupling needs git history; this folder has none."));
    m_cardTabs->setTabText(2, rows.isEmpty() ? QStringLiteral("Coupling")
                                             : QStringLiteral("Coupling (%1)").arg(rows.size()));
}

void EmberFeature::loadTrend()
{
    if (!m_snapshot || m_selected < 0)
        return;
    const QString path = m_snapshot->files[m_selected].path;
    if (!hasHistory(*m_snapshot)) {
        m_trend->setMessage(QStringLiteral("Complexity trends need git history; this folder has none."));
        return;
    }
    if (m_trends.contains(path)) {
        const QList<insight::TrendPoint> points = m_trends.value(path);
        if (points.isEmpty())
            m_trend->setMessage(QStringLiteral("No earlier versions of this file in the history window."));
        else
            m_trend->setPoints(points);
        return;
    }
    m_trend->setMessage(QStringLiteral("Reading the history of %1…").arg(fileNameOf(path)));
    if (m_trendLoading == path)
        return;
    m_trendLoading = path;
    const QString root = m_bar->root().isEmpty() ? m_snapshot->root : m_bar->root();
    auto *watcher = new QFutureWatcher<QList<insight::TrendPoint>>(this);
    QPointer<EmberFeature> guard(this);
    connect(watcher, &QFutureWatcher<QList<insight::TrendPoint>>::finished, this, [guard, watcher, path] {
        watcher->deleteLater();
        if (!guard)
            return;
        guard->m_trendLoading.clear();
        guard->m_trends.insert(path, watcher->future().resultCount() > 0 ? watcher->result()
                                                                          : QList<insight::TrendPoint>());
        if (guard->m_snapshot && guard->m_selected >= 0 && guard->m_snapshot->files[guard->m_selected].path == path
            && guard->m_cardTabs->currentIndex() == 1)
            guard->loadTrend();
    });
    watcher->setFuture(QtConcurrent::run([root, path] { return insight::complexityTrend(root, path, 16); }));
}

void EmberFeature::updateCrumbs()
{
    if (!m_layout || m_layout->isEmpty()) {
        m_crumbs->setText(QString());
        return;
    }
    const QVector<int> chain = m_view->focusChain();
    QStringList parts;
    const auto link = [this](int node) {
        return QStringLiteral("<a href=\"node:%1\" style=\"text-decoration:none\">%2</a>")
            .arg(node)
            .arg(m_layout->nodes[node].name.toHtmlEscaped());
    };
    if (chain.size() > 6) {
        parts << link(chain.first()) << QStringLiteral("…");
        for (int i = chain.size() - 4; i < chain.size(); ++i)
            parts << link(chain[i]);
    } else {
        for (int node : chain)
            parts << link(node);
    }
    if (m_snapshot && m_selected >= 0) {
        const int node = m_layout->fileNode(m_selected);
        if (node >= 0) {
            const QVector<int> fileChain = m_layout->chain(node);
            if (!chain.isEmpty() && fileChain.contains(chain.last())) {
                for (int i = fileChain.indexOf(chain.last()) + 1; i + 1 < fileChain.size(); ++i)
                    parts << link(fileChain[i]);
            } else {
                parts << QStringLiteral("…");
            }
            parts << QStringLiteral("<b>%1</b>").arg(m_layout->nodes[node].name.toHtmlEscaped());
        }
    }
    m_crumbs->setText(parts.join(QStringLiteral("&nbsp;&nbsp;/&nbsp;&nbsp;")));
}

QString EmberFeature::crumbText() const
{
    QString text = m_crumbs->text();
    text.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
    text.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    return text.simplified();
}

int EmberFeature::cardTab() const
{
    return m_cardTabs->currentIndex();
}

void EmberFeature::setCardTab(int index)
{
    m_cardTabs->setCurrentIndex(index);
}

void EmberFeature::selectBestMatch()
{
    if (!m_snapshot)
        return;
    const QString query = m_search->text().trimmed();
    if (query.isEmpty())
        return;
    int best = -1;
    int bestRank = 1 << 30;
    for (int i = 0; i < m_snapshot->files.size(); ++i) {
        const QString &path = m_snapshot->files[i].path;
        const QString name = fileNameOf(path);
        int rank = -1;
        if (name.compare(query, Qt::CaseInsensitive) == 0)
            rank = 0;
        else if (name.startsWith(query, Qt::CaseInsensitive))
            rank = 1;
        else if (name.contains(query, Qt::CaseInsensitive))
            rank = 2;
        else if (path.contains(query, Qt::CaseInsensitive))
            rank = 3;
        if (rank < 0)
            continue;
        rank = rank * 100000 + int(path.size());
        if (rank < bestRank) {
            bestRank = rank;
            best = i;
        }
    }
    if (best >= 0)
        selectFile(best, true);
}

void EmberFeature::setHostSearchQuery(const QString &query)
{
    if (m_search->text() != query)
        m_search->setText(query);
}

QList<forkmesh::market::NativePackageSearchHit> EmberFeature::searchHits(const QString &query, int limit) const
{
    QList<forkmesh::market::NativePackageSearchHit> hits;
    const QString q = query.trimmed();
    if (!m_snapshot || q.isEmpty() || limit <= 0)
        return hits;
    const QLocale locale;
    QList<QPair<int, int>> ranked;
    for (int i = 0; i < m_snapshot->files.size(); ++i) {
        const QString &path = m_snapshot->files[i].path;
        if (!path.contains(q, Qt::CaseInsensitive))
            continue;
        const int rank = fileNameOf(path).contains(q, Qt::CaseInsensitive) ? 0 : 1;
        ranked.append({rank * 100000 + int(path.size()), i});
    }
    std::sort(ranked.begin(), ranked.end());
    for (const auto &entry : ranked) {
        if (hits.size() >= limit)
            break;
        const FileInfo &f = m_snapshot->files[entry.second];
        QString detail = QStringLiteral("%1 · health %2").arg(f.path, locale.toString(f.health, 'f', 1));
        if (f.commits > 0)
            detail += QStringLiteral(" · %1 commits").arg(locale.toString(f.commits));
        hits.append({QStringLiteral("file:") + f.path, fileNameOf(f.path), detail});
    }
    return hits;
}

void EmberFeature::activateSearchHit(const QString &id)
{
    if (!id.startsWith(QStringLiteral("file:")))
        return;
    const QString path = id.mid(5);
    if (!ready()) {
        m_pendingPath = path;
        return;
    }
    int index = m_snapshot->indexOf(path);
    if (index < 0) {
        for (int i = 0; i < m_snapshot->files.size(); ++i) {
            if (m_snapshot->files[i].path == path) {
                index = i;
                break;
            }
        }
    }
    if (index >= 0) {
        selectFile(index, true);
        m_view->setFocus(Qt::OtherFocusReason);
    }
}

void EmberFeature::preparePreview(const QString &viewName)
{
    Tab tab = Tab::CodeHealth;
    for (Tab candidate : allTabs()) {
        if (!viewName.isEmpty() && tabTitle(candidate).compare(viewName, Qt::CaseInsensitive) == 0)
            tab = candidate;
    }
    m_tabChosen = true;
    setTab(tab);
    if (!m_snapshot || m_snapshot->files.isEmpty())
        return;
    int pick = -1;
    const QList<Target> targets = refactoringTargets(*m_snapshot, *m_coupling, 15);
    for (const Target &t : targets) {
        if (!m_snapshot->files[t.file].test) {
            pick = t.file;
            break;
        }
    }
    if (pick < 0 && !targets.isEmpty())
        pick = targets.first().file;
    if (pick < 0) {
        double worst = 1e9;
        for (int i = 0; i < m_snapshot->files.size(); ++i) {
            const FileInfo &f = m_snapshot->files[i];
            const double score = f.health - std::log1p(f.codeLines) * 0.2;
            if (score < worst) {
                worst = score;
                pick = i;
            }
        }
    }
    selectFile(pick, true);
    m_view->finishAnimation();
    updateCrumbs();
}

} // namespace forkmesh::ember

namespace {
const forkmesh::market::NativePackageRegistration emberRegistration({
    QStringLiteral("builtin:ember"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Ember"),
    QStringLiteral("flame"),
    QString(),
    QString(),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::ember::EmberFeature(host, parent);
    },
});
} // namespace
