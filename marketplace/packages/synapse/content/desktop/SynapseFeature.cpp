#include "SynapseFeature.h"

#include "SynapseView.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QStringListModel>
#include <QThreadPool>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace forkmesh::synapse {

namespace {

QString plural(int count, const QString &one, const QString &many)
{
    return QStringLiteral("%1 %2").arg(QLocale().toString(count), count == 1 ? one : many);
}

}

SynapseFeature::SynapseFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent)
{
    setObjectName(QStringLiteral("synapsePage"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    m_bar = new forkmesh::insight::RepoInsightBar(host, QStringLiteral("Synapse"),
                                                  QStringLiteral("How files connect"), this);
    outer->addWidget(m_bar);

    m_colourBy = new QComboBox;
    m_colourBy->setAccessibleName(QStringLiteral("Colour by"));
    m_colourBy->setToolTip(QStringLiteral("Colour the files by module, language, code health or change frequency"));
    m_colourBy->addItems({QStringLiteral("Colour: Module"), QStringLiteral("Colour: Language"),
                          QStringLiteral("Colour: Health"), QStringLiteral("Colour: Hotspot")});
    m_show = new QComboBox;
    m_show->setAccessibleName(QStringLiteral("Show"));
    m_show->setToolTip(QStringLiteral("Which files to show"));
    m_show->addItems({QStringLiteral("Everything"), QStringLiteral("Circular dependencies only"),
                      QStringLiteral("Orphans"), QStringLiteral("Selected neighbourhood")});
    m_depth = new QComboBox;
    m_depth->setAccessibleName(QStringLiteral("Neighbourhood depth"));
    m_depth->setToolTip(QStringLiteral("How many import steps around the selected file light up"));
    m_depth->addItems({QStringLiteral("1 hop"), QStringLiteral("2 hops"), QStringLiteral("3 hops")});
    m_autoRotate = new QCheckBox(QStringLiteral("Auto-rotate"));
    m_autoRotate->setChecked(true);
    m_autoRotate->setToolTip(QStringLiteral("Turn the brain slowly while you are not touching it (Space)"));
    m_pulses = new QCheckBox(QStringLiteral("Pulses"));
    m_pulses->setChecked(true);
    m_pulses->setToolTip(QStringLiteral("Send signals along the imports, from the importing file to the imported one"));
    m_fit = new QPushButton(QStringLiteral("Fit"));
    m_fit->setObjectName(QStringLiteral("ghostButton"));
    m_fit->setCursor(Qt::PointingHandCursor);
    m_fit->setToolTip(QStringLiteral("Show everything that is visible (F)"));
    m_bar->actions()->addWidget(m_colourBy);
    m_bar->actions()->addWidget(m_show);
    m_bar->actions()->addWidget(m_depth);
    m_bar->actions()->addWidget(m_autoRotate);
    m_bar->actions()->addWidget(m_pulses);
    m_bar->actions()->addWidget(m_fit);

    auto *body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    m_view = new SynapseView(this);
    body->addWidget(m_view, 1);
    auto *line = new QFrame;
    line->setFrameShape(QFrame::VLine);
    line->setFrameShadow(QFrame::Plain);
    line->setFixedWidth(1);
    body->addWidget(line);

    auto *side = new QWidget;
    side->setFixedWidth(340);
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(16, 12, 8, 8);
    sideLayout->setSpacing(8);
    m_search = new QLineEdit;
    m_search->setPlaceholderText(QStringLiteral("Find a file…  ( / )"));
    m_search->setClearButtonEnabled(true);
    m_search->setAccessibleName(QStringLiteral("Find a file"));
    m_searchModel = new QStringListModel(this);
    auto *completer = new QCompleter(m_searchModel, this);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    completer->setMaxVisibleItems(14);
    m_search->setCompleter(completer);
    sideLayout->addWidget(m_search);
    m_scroll = new QScrollArea;
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_panel = new QWidget;
    m_panelLayout = new QVBoxLayout(m_panel);
    m_panelLayout->setContentsMargins(0, 4, 8, 12);
    m_panelLayout->setSpacing(6);
    m_scroll->setWidget(m_panel);
    sideLayout->addWidget(m_scroll, 1);
    body->addWidget(side);
    outer->addLayout(body, 1);

    connect(m_colourBy, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) { m_view->setColourBy(ColourBy(std::clamp(index, 0, 3))); });
    connect(m_show, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) { setShowIndex(index); });
    connect(m_depth, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        m_view->setDepth(index + 1);
        rebuildPanel();
    });
    connect(m_autoRotate, &QCheckBox::toggled, this, [this](bool on) { m_view->setAutoRotate(on); });
    connect(m_pulses, &QCheckBox::toggled, this, [this](bool on) { m_view->setPulses(on); });
    connect(m_fit, &QPushButton::clicked, this, [this] { m_view->fit(true); });
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_view->setSearchQuery(text.isEmpty() ? m_hostQuery : text);
    });
    connect(completer, QOverload<const QString &>::of(&QCompleter::activated), this,
            [this](const QString &path) {
                if (m_snapshot)
                    selectFile(m_snapshot->indexOf(path), true);
            });
    connect(m_search, &QLineEdit::returnPressed, this, [this] {
        if (!m_snapshot)
            return;
        const QString text = m_search->text().trimmed();
        int found = m_snapshot->indexOf(text);
        if (found < 0) {
            for (int i = 0; i < m_snapshot->files.size() && found < 0; ++i) {
                if (m_snapshot->files.at(i).path.contains(text, Qt::CaseInsensitive)
                    && (!m_graph || m_graph->nodeFor(i) >= 0))
                    found = i;
            }
        }
        if (found >= 0)
            selectFile(found, true);
    });
    for (const QKeySequence &key : {QKeySequence(QKeySequence::Find), QKeySequence(Qt::Key_Slash)}) {
        auto *shortcut = new QShortcut(key, this);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, this, [this] {
            m_search->setFocus(Qt::ShortcutFocusReason);
            m_search->selectAll();
        });
    }

    m_view->setOnSelectionChanged([this](int) { rebuildPanel(); });
    m_view->setOnAutoRotateChanged([this](bool on) {
        const QSignalBlocker blocker(m_autoRotate);
        m_autoRotate->setChecked(on);
    });
    m_bar->setOnSnapshot([this](forkmesh::insight::SnapshotPtr snapshot) { showSnapshot(snapshot); });
    m_bar->setOnRootChanged([this](const QString &) {
        if (m_cancel)
            m_cancel->store(true);
        m_snapshot.reset();
        m_graph.reset();
        m_cache.clear();
        m_running = false;
        m_view->clearGraph(QStringLiteral("Reading %1…").arg(m_bar->repositoryTitle()));
        clearPanel();
    });
    rebuildPanel();
}

SynapseFeature::~SynapseFeature()
{
    if (m_cancel)
        m_cancel->store(true);
}

void SynapseFeature::refresh()
{
    m_bar->refresh();
}

void SynapseFeature::repositoryChanged()
{
    m_bar->repositoryChanged();
}

void SynapseFeature::setHostSearchQuery(const QString &text)
{
    m_hostQuery = text.trimmed();
    if (m_search->text().isEmpty())
        m_view->setSearchQuery(m_hostQuery);
}

QList<forkmesh::market::NativePackageSearchHit> SynapseFeature::searchHits(const QString &query,
                                                                           int limit) const
{
    QList<forkmesh::market::NativePackageSearchHit> hits;
    const QString needle = query.trimmed();
    if (!m_snapshot || needle.isEmpty() || limit <= 0)
        return hits;
    for (int i = 0; i < m_snapshot->files.size() && hits.size() < limit; ++i) {
        const forkmesh::insight::FileInfo &file = m_snapshot->files.at(i);
        if (!file.path.contains(needle, Qt::CaseInsensitive))
            continue;
        forkmesh::market::NativePackageSearchHit hit;
        hit.id = file.path;
        hit.title = fileName(file.path);
        hit.detail = QStringLiteral("%1 · imports %2 · imported by %3")
                         .arg(file.path)
                         .arg(file.dependsOn.size())
                         .arg(file.dependedOnBy.size());
        hits.append(hit);
    }
    return hits;
}

void SynapseFeature::activateSearchHit(const QString &id)
{
    if (!m_snapshot)
        return;
    const int file = m_snapshot->indexOf(id);
    if (file >= 0)
        selectFile(file, true);
}

void SynapseFeature::showSnapshot(const forkmesh::insight::SnapshotPtr &snapshot)
{
    if (m_cancel)
        m_cancel->store(true);
    m_snapshot = snapshot;
    m_graph.reset();
    m_cache.clear();
    m_running = false;
    m_orphans.clear();
    m_leaves.clear();
    if (!snapshot) {
        m_view->clearGraph(QStringLiteral("Open or clone a repository to see how its files connect."));
        clearPanel();
        return;
    }
    if (!snapshot->error.isEmpty()) {
        m_view->clearGraph(snapshot->error);
        clearPanel();
        return;
    }
    if (snapshot->files.isEmpty()) {
        m_view->clearGraph(QStringLiteral("This folder has no source files Synapse can read. Pick another repository or folder."));
        clearPanel();
        return;
    }
    m_orphans = orphans(*snapshot);
    m_leaves = leaves(*snapshot);
    rebuildSearchModel();
    updateSummary();
    m_view->setMessage(QStringLiteral("Wiring the brain…"));
    startLayout();
    if (m_hostQuery.isEmpty() && m_search->text().isEmpty())
        m_view->setSearchQuery(QString());
}

void SynapseFeature::startLayout()
{
    if (!m_snapshot)
        return;
    if (m_cancel)
        m_cancel->store(true);
    const bool isolated = m_includeIsolated;
    const auto cached = m_cache.constFind(isolated);
    if (cached != m_cache.constEnd()) {
        m_running = false;
        m_graph = cached->graph;
        m_view->setGraph(cached->graph, cached->positions);
        m_view->setPositions(cached->positions, 1.0);
        rebuildPanel();
        return;
    }
    auto cancel = std::make_shared<std::atomic_bool>(false);
    auto inFlight = std::make_shared<std::atomic_bool>(false);
    m_cancel = cancel;
    m_inFlight = inFlight;
    const quint64 generation = ++m_generation;
    const forkmesh::insight::SnapshotPtr snapshot = m_snapshot;
    QPointer<SynapseFeature> self(this);
    m_running = true;
    QThreadPool::globalInstance()->start([snapshot, isolated, cancel, inFlight, generation, self] {
        auto built = std::make_shared<Graph>(buildGraph(snapshot, isolated));
        const GraphPtr graph = built;
        if (cancel->load())
            return;
        const QVector<QVector3D> seeds = seedPositions(*graph);
        QMetaObject::invokeMethod(qApp, [self, generation, graph, seeds] {
            if (self)
                self->graphReady(generation, graph, seeds);
        }, Qt::QueuedConnection);
        const PublishFn publish = [self, generation, inFlight](const QVector<QVector3D> &positions, double progress) {
            if (progress < 1.0 && inFlight->exchange(true))
                return;
            inFlight->store(true);
            QMetaObject::invokeMethod(qApp, [self, generation, positions, progress, inFlight] {
                inFlight->store(false);
                if (self)
                    self->layoutStep(generation, positions, progress);
            }, Qt::QueuedConnection);
        };
        runLayout(*graph, LayoutOptions(), publish, [cancel] { return cancel->load(); });
    });
}

void SynapseFeature::graphReady(quint64 generation, const GraphPtr &graph,
                                const QVector<QVector3D> &positions)
{
    if (generation != m_generation)
        return;
    m_graph = graph;
    m_view->setGraph(graph, positions);
    rebuildPanel();
}

void SynapseFeature::layoutStep(quint64 generation, const QVector<QVector3D> &positions, double progress)
{
    if (generation != m_generation || !m_graph)
        return;
    m_view->setPositions(positions, progress);
    if (progress >= 1.0) {
        m_running = false;
        m_cache.insert(m_graph->includesIsolated, Cached{m_graph, positions});
        if (m_view->selectedFile() < 0 && m_view->isolatedCycle() < 0)
            m_view->fit(true);
    }
}

void SynapseFeature::setShowIndex(int index)
{
    index = std::clamp(index, 0, 3);
    if (m_show->currentIndex() != index) {
        const QSignalBlocker blocker(m_show);
        m_show->setCurrentIndex(index);
    }
    const bool isolated = index == 2;
    m_view->setShowMode(ShowMode(index));
    if (isolated != m_includeIsolated) {
        m_includeIsolated = isolated;
        startLayout();
    } else {
        m_view->fit(true);
    }
    rebuildPanel();
}

void SynapseFeature::selectFile(int file, bool fly)
{
    if (!m_snapshot || file < 0 || file >= m_snapshot->files.size()) {
        m_view->clearSelection();
        rebuildPanel();
        return;
    }
    if (m_graph && m_graph->nodeFor(file) < 0 && !m_includeIsolated) {
        m_view->selectFile(file, false);
        rebuildPanel();
        return;
    }
    if (m_view->isolatedCycle() >= 0 && m_snapshot->files.at(file).cycle != m_graph->cycles.value(m_view->isolatedCycle()).index)
        m_view->setShowMode(ShowMode(m_show->currentIndex()));
    m_view->selectFile(file, fly);
    rebuildPanel();
}

void SynapseFeature::isolateCycle(int cycle)
{
    if (!m_graph || cycle < 0 || cycle >= m_graph->cycles.size())
        return;
    {
        const QSignalBlocker blocker(m_show);
        m_show->setCurrentIndex(1);
    }
    if (m_includeIsolated) {
        m_includeIsolated = false;
        startLayout();
    }
    m_view->clearSelection();
    m_view->setIsolatedCycle(cycle);
    m_view->flyToCycle(cycle);
    rebuildPanel();
}

void SynapseFeature::showcase()
{
    if (!m_snapshot)
        return;
    m_autoRotate->setChecked(false);
    Camera camera = m_view->camera();
    camera.yaw = 0.5f;
    camera.pitch = 0.58f;
    m_view->setCamera(camera);
    m_view->fit(false);
    int file = mostConnected(*m_snapshot);
    if (m_graph && m_view->positions().size() == m_graph->nodes.size()) {
        const float cp = std::cos(camera.pitch);
        const QVector3D back(cp * std::sin(camera.yaw), std::sin(camera.pitch), cp * std::cos(camera.yaw));
        double best = 0;
        for (int n = 0; n < m_graph->nodes.size(); ++n) {
            const forkmesh::insight::FileInfo &info = m_snapshot->files.at(m_graph->nodes.at(n).file);
            const double connected = std::log1p(double(info.dependedOnBy.size()))
                                     * std::log1p(double(info.dependsOn.size()));
            const QVector3D p = m_view->positions().at(n);
            const double facing = p.length() > 0 ? QVector3D::dotProduct(p.normalized(), back) : 0.0;
            const double score = connected * (0.15 + std::max(0.0, facing));
            if (score > best) {
                best = score;
                file = m_graph->nodes.at(n).file;
            }
        }
    }
    if (file >= 0)
        selectFile(file, false);
    m_view->setClock(1.35);
}

void SynapseFeature::updateSummary()
{
    if (!m_snapshot) {
        m_summary.clear();
        m_bar->setSummary(QString());
        return;
    }
    m_summary = QStringLiteral("%1 · %2 · %3")
                    .arg(plural(importCount(*m_snapshot), QStringLiteral("import"), QStringLiteral("imports")),
                         plural(m_snapshot->cycles.size(), QStringLiteral("circular dependency"),
                                QStringLiteral("circular dependencies")),
                         plural(m_orphans.size(), QStringLiteral("orphan"), QStringLiteral("orphans")));
    m_bar->setSummary(m_summary);
}

void SynapseFeature::rebuildSearchModel()
{
    QStringList paths;
    if (m_snapshot) {
        paths.reserve(m_snapshot->files.size());
        for (const forkmesh::insight::FileInfo &file : m_snapshot->files)
            paths.append(file.path);
    }
    m_searchModel->setStringList(paths);
}

QString SynapseFeature::linkColour() const
{
    return palette().color(QPalette::Window).lightness() < 128 ? QStringLiteral("#58a6ff")
                                                               : QStringLiteral("#0969da");
}

QString SynapseFeature::fileLink(int file) const
{
    if (!m_snapshot || file < 0 || file >= m_snapshot->files.size())
        return QString();
    const QString path = m_snapshot->files.at(file).path;
    return QStringLiteral("<a href=\"file:%1\" style=\"color:%2;text-decoration:none\" title=\"%3\">%4</a>")
        .arg(file)
        .arg(linkColour(), path.toHtmlEscaped(), fileName(path).toHtmlEscaped());
}

void SynapseFeature::clearPanel()
{
    while (QLayoutItem *item = m_panelLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        } else if (QLayout *layout = item->layout()) {
            while (QLayoutItem *child = layout->takeAt(0)) {
                if (QWidget *widget = child->widget()) {
                    widget->hide();
                    widget->deleteLater();
                }
                delete child;
            }
        }
        delete item;
    }
}

QLabel *SynapseFeature::addSection(const QString &title)
{
    auto *label = new QLabel(title.toUpper());
    label->setObjectName(QStringLiteral("mutedLabel"));
    QFont f = label->font();
    f.setPointSizeF(std::max(7.0, f.pointSizeF() * 0.85));
    f.setWeight(QFont::DemiBold);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
    label->setFont(f);
    label->setContentsMargins(0, 10, 0, 0);
    m_panelLayout->addWidget(label);
    return label;
}

QLabel *SynapseFeature::addHtml(const QString &html)
{
    auto *label = new QLabel(html);
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextBrowserInteraction);
    label->setOpenExternalLinks(false);
    label->setMinimumWidth(0);
    connect(label, &QLabel::linkActivated, this, [this](const QString &link) { handleLink(link); });
    m_panelLayout->addWidget(label);
    return label;
}

void SynapseFeature::handleLink(const QString &link)
{
    const int colon = link.indexOf(QLatin1Char(':'));
    const QString kind = colon < 0 ? link : link.left(colon);
    const int value = colon < 0 ? -1 : link.mid(colon + 1).toInt();
    if (kind == QLatin1String("file")) {
        selectFile(value, true);
    } else if (kind == QLatin1String("cycle")) {
        isolateCycle(value);
    } else if (kind == QLatin1String("clear")) {
        m_view->clearSelection();
        rebuildPanel();
    } else if (kind == QLatin1String("all")) {
        setShowIndex(0);
    }
}

void SynapseFeature::rebuildPanel()
{
    clearPanel();
    if (!m_snapshot || !m_snapshot->error.isEmpty()) {
        auto *label = new QLabel(QStringLiteral("Pick a repository above. Synapse reads its files and draws every import as a fibre between two files."));
        label->setObjectName(QStringLiteral("mutedLabel"));
        label->setWordWrap(true);
        m_panelLayout->addWidget(label);
        m_panelLayout->addStretch(1);
        return;
    }
    const forkmesh::insight::Snapshot &snapshot = *m_snapshot;
    const QLocale locale;
    const bool dark = palette().color(QPalette::Window).lightness() < 128;
    const QString muted = dark ? QStringLiteral("#8b949e") : QStringLiteral("#57606a");
    const QString red = QStringLiteral("#e5534b");
    const int selected = m_view->selectedFile();
    const auto list = [&](const QVector<int> &files, int cap, const std::function<QString(int)> &extra) {
        QStringList rows;
        for (int i = 0; i < files.size() && i < cap; ++i) {
            QString row = fileLink(files.at(i));
            const QString tail = extra ? extra(files.at(i)) : QString();
            if (!tail.isEmpty())
                row += QStringLiteral(" <span style=\"color:%1\">%2</span>").arg(muted, tail);
            rows.append(row);
        }
        if (files.size() > cap)
            rows.append(QStringLiteral("<span style=\"color:%1\">and %2 more</span>")
                            .arg(muted, locale.toString(files.size() - cap)));
        return rows.join(QStringLiteral("<br>"));
    };
    const auto sortedByPath = [&](QVector<int> files) {
        std::sort(files.begin(), files.end(), [&](int a, int b) {
            return snapshot.files.at(a).path < snapshot.files.at(b).path;
        });
        files.erase(std::unique(files.begin(), files.end()), files.end());
        return files;
    };

    if (selected >= 0 && selected < snapshot.files.size()) {
        const forkmesh::insight::FileInfo &file = snapshot.files.at(selected);
        auto *title = new QLabel(fileName(file.path));
        title->setObjectName(QStringLiteral("pageTitle"));
        title->setWordWrap(true);
        m_panelLayout->addWidget(title);
        auto *path = new QLabel(file.path);
        path->setObjectName(QStringLiteral("mutedLabel"));
        path->setWordWrap(true);
        path->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_panelLayout->addWidget(path);
        const QString health = QStringLiteral("<span style=\"color:%1\">health %2/10</span>")
                                   .arg(healthColour(file.health).name(), locale.toString(file.health, 'f', 1));
        QStringList facts;
        if (!file.language.isEmpty())
            facts << file.language.toHtmlEscaped();
        facts << QStringLiteral("%1 lines").arg(locale.toString(file.codeLines));
        facts << health;
        if (file.test)
            facts << QStringLiteral("test");
        addHtml(facts.join(QStringLiteral(" · ")));
        if (m_graph && m_graph->nodeFor(selected) >= 0) {
            const Node &node = m_graph->nodes.at(m_graph->nodeFor(selected));
            addHtml(QStringLiteral("<span style=\"color:%1\">Module</span> %2")
                        .arg(muted, m_graph->lobes.at(node.lobe).name.toHtmlEscaped()));
        }
        auto *buttons = new QHBoxLayout;
        buttons->setSpacing(6);
        auto *open = new QPushButton(QStringLiteral("Open file"));
        open->setObjectName(QStringLiteral("ghostButton"));
        open->setCursor(Qt::PointingHandCursor);
        connect(open, &QPushButton::clicked, this, [this, selected] {
            if (m_snapshot && selected < m_snapshot->files.size())
                m_bar->openFile(m_snapshot->files.at(selected).path);
        });
        auto *fly = new QPushButton(QStringLiteral("Fly to"));
        fly->setObjectName(QStringLiteral("ghostButton"));
        fly->setCursor(Qt::PointingHandCursor);
        connect(fly, &QPushButton::clicked, this, [this, selected] { m_view->flyToFile(selected); });
        auto *clear = new QPushButton(QStringLiteral("Clear"));
        clear->setObjectName(QStringLiteral("ghostButton"));
        clear->setCursor(Qt::PointingHandCursor);
        clear->setToolTip(QStringLiteral("Clear the selection (Esc)"));
        connect(clear, &QPushButton::clicked, this, [this] { handleLink(QStringLiteral("clear")); });
        buttons->addWidget(open);
        buttons->addWidget(fly);
        buttons->addWidget(clear);
        buttons->addStretch(1);
        m_panelLayout->addLayout(buttons);
        if (file.cycle >= 0 && file.cycle < snapshot.cycles.size()) {
            addSection(QStringLiteral("Circular dependency"));
            const QVector<int> loop = cyclePath(snapshot, snapshot.cycles.at(file.cycle));
            QStringList steps;
            for (int step : loop)
                steps << fileLink(step);
            QString html = QStringLiteral("<span style=\"color:%1\">●</span> ").arg(red)
                           + steps.join(QStringLiteral(" <span style=\"color:%1\">→</span> ").arg(red));
            const int size = snapshot.cycles.at(file.cycle).size();
            if (size > std::max(0, int(loop.size()) - 1))
                html += QStringLiteral("<br><span style=\"color:%1\">%2 files take part in this loop</span>")
                            .arg(muted, locale.toString(size));
            int graphCycle = -1;
            if (m_graph) {
                for (int c = 0; c < m_graph->cycles.size(); ++c) {
                    if (m_graph->cycles.at(c).index == file.cycle)
                        graphCycle = c;
                }
            }
            if (graphCycle >= 0)
                html += QStringLiteral("<br><a href=\"cycle:%1\" style=\"color:%2;text-decoration:none\">Show only this loop</a>")
                            .arg(graphCycle)
                            .arg(linkColour());
            addHtml(html);
        }
        const QVector<int> imports = sortedByPath(file.dependsOn);
        const QVector<int> importers = sortedByPath(file.dependedOnBy);
        addSection(QStringLiteral("Imports · %1").arg(locale.toString(imports.size())));
        addHtml(imports.isEmpty() ? QStringLiteral("<span style=\"color:%1\">Imports no file of this repository.</span>").arg(muted)
                                  : list(imports, 60, {}));
        addSection(QStringLiteral("Imported by · %1").arg(locale.toString(importers.size())));
        addHtml(importers.isEmpty() ? QStringLiteral("<span style=\"color:%1\">No file imports it.</span>").arg(muted)
                                    : list(importers, 60, {}));
        if (!file.externalImports.isEmpty()) {
            QStringList packages = file.externalImports;
            packages.removeDuplicates();
            packages.sort(Qt::CaseInsensitive);
            addSection(QStringLiteral("External packages · %1").arg(locale.toString(packages.size())));
            QStringList rows;
            for (int i = 0; i < packages.size() && i < 40; ++i)
                rows << packages.at(i).toHtmlEscaped();
            if (packages.size() > 40)
                rows << QStringLiteral("and %1 more").arg(packages.size() - 40);
            addHtml(QStringLiteral("<span style=\"color:%1\">%2</span>").arg(muted, rows.join(QStringLiteral("<br>"))));
        }
        m_panelLayout->addStretch(1);
        return;
    }

    auto *title = new QLabel(QStringLiteral("Overview"));
    title->setObjectName(QStringLiteral("pageTitle"));
    m_panelLayout->addWidget(title);
    const int inGraph = m_graph ? int(std::count_if(m_graph->nodes.begin(), m_graph->nodes.end(),
                                                    [](const Node &node) { return node.fanIn + node.fanOut > 0; }))
                                : 0;
    const QString cell = QStringLiteral("<td style=\"padding:2px 14px 2px 0\"><span style=\"font-size:15pt;font-weight:600\">%1</span><br><span style=\"color:%2\">%3</span></td>");
    const auto value = [&](int n, const QString &label, const QString &colour = QString()) {
        QString number = locale.toString(n);
        if (!colour.isEmpty())
            number = QStringLiteral("<span style=\"color:%1\">%2</span>").arg(colour, number);
        return cell.arg(number, muted, label);
    };
    addHtml(QStringLiteral("<table cellspacing=0><tr>%1%2</tr><tr>%3%4</tr><tr>%5%6</tr></table>")
                .arg(value(inGraph, QStringLiteral("connected files")),
                     value(importCount(snapshot), QStringLiteral("imports")),
                     value(snapshot.cycles.size(), QStringLiteral("circular dependencies"),
                           snapshot.cycles.isEmpty() ? QString() : red),
                     value(m_orphans.size(), QStringLiteral("orphans")),
                     value(m_leaves.size(), QStringLiteral("leaves")),
                     value(m_graph ? m_graph->lobes.size() : 0, QStringLiteral("modules"))));
    if (m_view->isolatedCycle() >= 0 || m_show->currentIndex() != 0)
        addHtml(QStringLiteral("<a href=\"all\" style=\"color:%1;text-decoration:none\">Show everything again</a>")
                    .arg(linkColour()));
    if (m_graph && !m_graph->cycles.isEmpty()) {
        addSection(QStringLiteral("Circular dependencies · %1").arg(locale.toString(m_graph->cycles.size())));
        QVector<int> order(m_graph->cycles.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            const int sa = m_graph->cycles.at(a).files.size();
            const int sb = m_graph->cycles.at(b).files.size();
            return sa != sb ? sa > sb : a < b;
        });
        QStringList rows;
        for (int i = 0; i < order.size() && i < 40; ++i) {
            const CycleInfo &info = m_graph->cycles.at(order.at(i));
            QString text = cycleText(snapshot, info.path);
            if (text.isEmpty())
                text = QStringLiteral("%1 files").arg(info.files.size());
            const bool active = m_view->isolatedCycle() == order.at(i);
            rows << QStringLiteral("<span style=\"color:%1\">●</span> <a href=\"cycle:%2\" style=\"color:%3;text-decoration:none\">%4%5%6</a>%7")
                        .arg(red)
                        .arg(order.at(i))
                        .arg(linkColour(), active ? QStringLiteral("<b>") : QString(), text.toHtmlEscaped(),
                             active ? QStringLiteral("</b>") : QString(),
                             info.files.size() > std::max(0, int(info.path.size()) - 1)
                                 ? QStringLiteral(" <span style=\"color:%1\">(%2 files)</span>").arg(muted).arg(info.files.size())
                                 : QString());
        }
        if (order.size() > 40)
            rows << QStringLiteral("<span style=\"color:%1\">and %2 more</span>").arg(muted).arg(order.size() - 40);
        addHtml(rows.join(QStringLiteral("<br>")));
    }
    const QVector<int> imported = mostImported(snapshot, 10);
    if (!imported.isEmpty()) {
        addSection(QStringLiteral("Most imported"));
        addHtml(list(imported, 10, [&](int f) {
            return QStringLiteral("· %1").arg(locale.toString(snapshot.files.at(f).dependedOnBy.size()));
        }));
    }
    const QVector<int> importing = mostImporting(snapshot, 10);
    if (!importing.isEmpty()) {
        addSection(QStringLiteral("Most importing"));
        addHtml(list(importing, 10, [&](int f) {
            return QStringLiteral("· %1").arg(locale.toString(snapshot.files.at(f).dependsOn.size()));
        }));
    }
    if (!m_orphans.isEmpty()) {
        addSection(QStringLiteral("Orphans · %1").arg(locale.toString(m_orphans.size())));
        auto *hint = new QLabel(QStringLiteral("Files no other file imports and that import nothing here. Show → Orphans places them in the brain."));
        hint->setObjectName(QStringLiteral("mutedLabel"));
        hint->setWordWrap(true);
        m_panelLayout->addWidget(hint);
        addHtml(list(m_orphans, 30, {}));
    }
    if (snapshot.cycles.isEmpty() && importCount(snapshot) > 0) {
        addSection(QStringLiteral("Circular dependencies"));
        addHtml(QStringLiteral("<span style=\"color:%1\">None. Every import points one way.</span>")
                    .arg(healthColour(10).name()));
    }
    m_panelLayout->addStretch(1);
}

}

namespace {
const forkmesh::market::NativePackageRegistration synapseRegistration([] {
    forkmesh::market::NativePackageContribution contribution{
        QStringLiteral("builtin:synapse"),
        {QStringLiteral("extension.native")},
        QStringLiteral("Synapse"),
        QStringLiteral("brain"),
        QString(),
        QString(),
        [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
            return new forkmesh::synapse::SynapseFeature(host, parent);
        },
    };
    contribution.gpuSurface = true;
    return contribution;
}());
}
