#include "StrataFeature.h"

#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QSaveFile>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace forkmesh::strata {

using insight::FileInfo;
using insight::ModuleNode;

namespace {

QString chip(const QColor &color, const QString &text)
{
    const double l = 0.2126 * color.redF() + 0.7152 * color.greenF() + 0.0722 * color.blueF();
    return QStringLiteral("<span style=\"background-color:%1;color:%2;font-weight:700\">&nbsp;%3&nbsp;</span>")
        .arg(color.name(), l > 0.52 ? QStringLiteral("#0d1117") : QStringLiteral("#ffffff"), text.toHtmlEscaped());
}

QString dot(const QColor &color)
{
    return QStringLiteral("<span style=\"color:%1\">●</span>").arg(color.name());
}

QString row(const QString &label, const QString &value, const QString &muted)
{
    return QStringLiteral("<tr><td style=\"color:%1;padding-right:14px\">%2</td><td>%3</td></tr>").arg(muted, label, value);
}

QString two(double value)
{
    return QString::number(value, 'f', 2);
}

}

StrataFeature::StrataFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : NativePackageWidget(parent)
    , m_host(host)
{
    setObjectName(QStringLiteral("strataPage"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    m_bar = new insight::RepoInsightBar(host, QStringLiteral("Strata"), QStringLiteral("Architecture and complexity"));
    outer->addWidget(m_bar);

    m_depth = new QComboBox;
    m_depth->setAccessibleName(QStringLiteral("Module depth"));
    m_depth->setToolTip(QStringLiteral("How many folder levels make a module"));
    m_depth->addItem(QStringLiteral("Modules: Auto"), 0);
    for (int i = 1; i <= 4; ++i)
        m_depth->addItem(QStringLiteral("Modules: %1 %2").arg(i).arg(i == 1 ? QStringLiteral("level") : QStringLiteral("levels")), i);
    m_bar->actions()->addWidget(m_depth);
    m_export = new QPushButton(QStringLiteral("Export report…"));
    m_export->setObjectName(QStringLiteral("ghostButton"));
    m_export->setCursor(Qt::PointingHandCursor);
    m_export->setToolTip(QStringLiteral("Save the dashboard, quality gates, rules and modules as one HTML file"));
    m_export->setEnabled(false);
    m_bar->actions()->addWidget(m_export);

    m_tabRow = new QWidget;
    m_tabRow->setObjectName(QStringLiteral("strataTabs"));
    auto *tabs = new QHBoxLayout(m_tabRow);
    tabs->setContentsMargins(20, 2, 20, 8);
    tabs->setSpacing(4);
    const QStringList names = {QStringLiteral("Dashboard"), QStringLiteral("Dependency graph"), QStringLiteral("Matrix"),
                               QStringLiteral("Abstractness × Instability"), QStringLiteral("Metrics"), QStringLiteral("Rules")};
    for (int i = 0; i < names.size(); ++i) {
        auto *button = new QPushButton(names[i]);
        button->setObjectName(QStringLiteral("strataTab"));
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::TabFocus);
        connect(button, &QPushButton::clicked, this, [this, i] { showView(i); });
        tabs->addWidget(button);
        m_tabs.append(button);
    }
    tabs->addSpacing(12);
    m_tools = new QStackedWidget;
    m_tools->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *emptyTools = new QWidget;
    m_tools->addWidget(emptyTools);
    auto *graphTools = new QWidget;
    auto *graphRow = new QHBoxLayout(graphTools);
    graphRow->setContentsMargins(0, 0, 0, 0);
    graphRow->setSpacing(6);
    m_crumb = new QLabel;
    m_crumb->setObjectName(QStringLiteral("mutedLabel"));
    m_crumb->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    graphRow->addWidget(m_crumb, 1);
    m_colorBy = new QComboBox;
    m_colorBy->setAccessibleName(QStringLiteral("Colour modules by"));
    m_colorBy->addItem(QStringLiteral("Colour: Debt rating"));
    m_colorBy->addItem(QStringLiteral("Colour: Health"));
    m_colorBy->addItem(QStringLiteral("Colour: Instability"));
    m_colorBy->addItem(QStringLiteral("Colour: Complexity"));
    graphRow->addWidget(m_colorBy);
    m_fit = new QPushButton(QStringLiteral("Fit"));
    m_fit->setObjectName(QStringLiteral("ghostButton"));
    m_fit->setCursor(Qt::PointingHandCursor);
    m_fit->setToolTip(QStringLiteral("Show the whole graph"));
    graphRow->addWidget(m_fit);
    m_tools->addWidget(graphTools);
    auto *metricTools = new QWidget;
    auto *metricRow = new QHBoxLayout(metricTools);
    metricRow->setContentsMargins(0, 0, 0, 0);
    metricRow->setSpacing(6);
    metricRow->addStretch(1);
    m_sizeBy = new QComboBox;
    m_sizeBy->setAccessibleName(QStringLiteral("Size files by"));
    m_sizeBy->addItem(QStringLiteral("Size: Lines of code"));
    m_sizeBy->addItem(QStringLiteral("Size: Complexity"));
    m_sizeBy->addItem(QStringLiteral("Size: Functions"));
    metricRow->addWidget(m_sizeBy);
    m_metricColor = new QComboBox;
    m_metricColor->setAccessibleName(QStringLiteral("Colour files by"));
    m_metricColor->addItem(QStringLiteral("Colour: Health"));
    m_metricColor->addItem(QStringLiteral("Colour: Max complexity"));
    m_metricColor->addItem(QStringLiteral("Colour: Debt"));
    m_metricColor->addItem(QStringLiteral("Colour: Change frequency"));
    metricRow->addWidget(m_metricColor);
    m_tools->addWidget(metricTools);
    tabs->addWidget(m_tools, 1);
    m_detailsToggle = new QPushButton(QStringLiteral("Details"));
    m_detailsToggle->setObjectName(QStringLiteral("strataTab"));
    m_detailsToggle->setCheckable(true);
    m_detailsToggle->setChecked(true);
    m_detailsToggle->setCursor(Qt::PointingHandCursor);
    m_detailsToggle->setToolTip(QStringLiteral("Show or hide the details panel"));
    tabs->addWidget(m_detailsToggle);
    outer->addWidget(m_tabRow);

    m_split = new QSplitter(Qt::Horizontal);
    m_split->setChildrenCollapsible(true);
    m_split->setHandleWidth(1);
    m_views = new QStackedWidget;
    m_dashboard = new DashboardView;
    m_graph = new GraphView;
    m_matrix = new MatrixView;
    m_scatter = new ScatterView;
    m_metrics = new MetricsView;
    m_rules = new RulesView;
    m_views->addWidget(m_dashboard);
    m_views->addWidget(m_graph);
    m_views->addWidget(m_matrix);
    m_views->addWidget(m_scatter);
    m_views->addWidget(m_metrics);
    m_views->addWidget(m_rules);
    m_split->addWidget(m_views);
    m_details = new QTextBrowser;
    m_details->setObjectName(QStringLiteral("strataDetails"));
    m_details->setOpenLinks(false);
    m_details->setFrameShape(QFrame::NoFrame);
    m_details->setMinimumWidth(260);
    m_details->document()->setDocumentMargin(16);
    m_split->addWidget(m_details);
    m_split->setStretchFactor(0, 1);
    m_split->setStretchFactor(1, 0);
    m_split->setSizes({1200, 340});
    outer->addWidget(m_split, 1);

    ViewCallbacks callbacks;
    callbacks.select = [this](const Selection &selection) { select(selection); };
    callbacks.open = [this](int file, int line) { openFile(file, line); };
    callbacks.cell = [this](int from, int to) { showCell(from, to); };
    callbacks.showModule = [this](int module) { showModule(module); };
    m_dashboard->setCallbacks(callbacks);
    m_graph->setCallbacks(callbacks);
    m_matrix->setCallbacks(callbacks);
    m_scatter->setCallbacks(callbacks);
    m_metrics->setCallbacks(callbacks);
    m_rules->setCallbacks(callbacks);
    m_graph->setOnDrillChanged([this] { updateBreadcrumb(); });

    connect(m_depth, &QComboBox::currentIndexChanged, this, [this](int) { computeModel(); });
    connect(m_export, &QPushButton::clicked, this, [this] { exportReport(); });
    connect(m_colorBy, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_graph->setColorBy(GraphView::ColorBy(std::clamp(index, 0, 3)));
    });
    connect(m_fit, &QPushButton::clicked, this, [this] { m_graph->fit(true); });
    connect(m_crumb, &QLabel::linkActivated, this, [this](const QString &) { m_graph->drillOut(); });
    connect(m_sizeBy, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_metrics->setSizeBy(MetricsView::SizeBy(std::clamp(index, 0, 2)));
    });
    connect(m_metricColor, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_metrics->setColorBy(MetricsView::ColorBy(std::clamp(index, 0, 3)));
    });
    connect(m_detailsToggle, &QPushButton::toggled, this, [this](bool on) { setDetailsVisible(on); });
    connect(m_split, &QSplitter::splitterMoved, this, [this](int, int) {
        const bool open = m_split->sizes().value(1) > 0;
        if (m_detailsToggle->isChecked() != open) {
            const QSignalBlocker blocker(m_detailsToggle);
            m_detailsToggle->setChecked(open);
        }
    });
    connect(m_details, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) { anchorClicked(url); });

    auto *find = new QShortcut(QKeySequence::Find, this);
    find->setContext(Qt::WidgetWithChildrenShortcut);
    connect(find, &QShortcut::activated, this, [this] {
        showView(Rules);
        m_rules->focusFilter();
    });

    m_bar->setOnSnapshot([this](insight::SnapshotPtr snapshot) { applySnapshot(snapshot); });
    m_bar->setOnRootChanged([this](const QString &) {
        ++m_generation;
        m_snapshot.reset();
        applyModel(nullptr);
    });

    restyleTabs();
    showView(Graph);
    updateBreadcrumb();
    updateDetails();
}

StrataFeature::~StrataFeature()
{
    if (m_watcher)
        m_watcher->disconnect(this);
}

void StrataFeature::refresh()
{
    m_bar->refresh();
}

void StrataFeature::repositoryChanged()
{
    m_bar->repositoryChanged();
}

bool StrataFeature::computing() const
{
    return m_watcher && m_watcher->isRunning();
}

bool StrataFeature::settled() const
{
    return m_model && !computing() && !m_bar->busy() && m_graph->settled();
}

void StrataFeature::restyleTabs()
{
    const Colors colors = Colors::from(this);
    const auto rgba = [](const QColor &color, double alpha) {
        return QStringLiteral("rgba(%1,%2,%3,%4)").arg(color.red()).arg(color.green()).arg(color.blue()).arg(alpha);
    };
    m_tabRow->setStyleSheet(
        QStringLiteral("QPushButton#strataTab{background:transparent;border:none;border-radius:6px;padding:5px 12px;color:%1;}"
                       "QPushButton#strataTab:hover{background:%2;color:%3;}"
                       "QPushButton#strataTab:checked{background:%4;color:%5;font-weight:600;}"
                       "QPushButton#strataTab:focus{outline:none;border:1px solid %5;}")
            .arg(colors.muted.name(), colors.faint.name(), colors.text.name(), rgba(colors.accent, 0.16), colors.accent.name()));
}

void StrataFeature::changeEvent(QEvent *event)
{
    NativePackageWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange && m_tabRow) {
        restyleTabs();
        updateDetails();
    }
}

void StrataFeature::showView(int view)
{
    view = std::clamp(view, 0, int(ViewCount) - 1);
    m_view = view;
    m_views->setCurrentIndex(view);
    for (int i = 0; i < m_tabs.size(); ++i)
        m_tabs[i]->setChecked(i == view);
    m_tools->setCurrentIndex(view == Graph ? 1 : view == Metrics ? 2 : 0);
}

void StrataFeature::setDetailsVisible(bool visible)
{
    if (m_detailsToggle->isChecked() != visible) {
        const QSignalBlocker blocker(m_detailsToggle);
        m_detailsToggle->setChecked(visible);
    }
    QList<int> sizes = m_split->sizes();
    if (sizes.size() < 2)
        return;
    const int total = sizes[0] + sizes[1];
    const int panel = visible ? std::min(360, std::max(280, total / 4)) : 0;
    m_split->setSizes({total - panel, panel});
}

void StrataFeature::setModuleDepth(int depth)
{
    const int index = m_depth->findData(depth);
    if (index >= 0)
        m_depth->setCurrentIndex(index);
}

void StrataFeature::applySnapshot(const insight::SnapshotPtr &snapshot)
{
    m_snapshot = snapshot;
    if (!snapshot || !snapshot->error.isEmpty()) {
        ++m_generation;
        applyModel(nullptr);
        return;
    }
    computeModel();
}

void StrataFeature::computeModel()
{
    const int generation = ++m_generation;
    if (!m_snapshot) {
        applyModel(nullptr);
        return;
    }
    if (m_watcher) {
        m_watcher->disconnect(this);
        m_watcher->deleteLater();
        m_watcher = nullptr;
    }
    m_bar->setSummary(QStringLiteral("Building the module graph…"));
    const insight::SnapshotPtr snapshot = m_snapshot;
    const int depth = m_depth->currentData().toInt();
    m_watcher = new QFutureWatcher<ModelPtr>(this);
    QPointer<StrataFeature> self(this);
    connect(m_watcher, &QFutureWatcher<ModelPtr>::finished, this, [self, generation] {
        if (!self || !self->m_watcher)
            return;
        QFutureWatcher<ModelPtr> *watcher = self->m_watcher;
        self->m_watcher = nullptr;
        watcher->deleteLater();
        if (generation != self->m_generation || watcher->future().resultCount() == 0)
            return;
        self->applyModel(watcher->result());
    });
    m_watcher->setFuture(QtConcurrent::run([snapshot, depth] { return buildModel(snapshot, depth); }));
}

void StrataFeature::applyModel(const ModelPtr &model)
{
    m_model = model;
    m_selection = Selection();
    m_cellFrom = m_cellTo = -1;
    m_dashboard->setModel(model);
    m_graph->setModel(model);
    m_matrix->setModel(model);
    m_scatter->setModel(model);
    m_metrics->setModel(model);
    m_rules->setModel(model);
    m_matrix->setSelection({});
    m_scatter->setSelection({});
    m_metrics->setSelection({});
    m_export->setEnabled(model && model->snapshot && !model->snapshot->files.isEmpty());
    if (model && model->snapshot && !model->snapshot->files.isEmpty())
        m_bar->setSummary(summaryLine(model->report));
    else
        m_bar->setSummary(QString());
    if (!m_query.isEmpty()) {
        m_graph->setFilter(m_query);
        m_scatter->setFilter(m_query);
        m_metrics->setFilter(m_query);
    }
    updateBreadcrumb();
    updateDetails();
    if (m_host.searchIndexChanged)
        m_host.searchIndexChanged();
}

void StrataFeature::updateBreadcrumb()
{
    if (!m_model || m_model->report.graph.nodes.isEmpty()) {
        m_crumb->clear();
        return;
    }
    const int module = m_graph->drilledModule();
    const QString link = Colors::from(this).accent.name();
    if (module < 0) {
        m_crumb->setText(QStringLiteral("%1 modules · %2 dependencies · depth %3")
                             .arg(m_model->report.graph.nodes.size())
                             .arg(m_model->report.graph.edges.size())
                             .arg(m_model->report.graph.depth));
    } else {
        m_crumb->setText(QStringLiteral("<a href=\"back\" style=\"color:%1;text-decoration:none\">All modules</a>  ›  <b>%2</b>")
                             .arg(link, m_model->report.graph.nodes[module].path.toHtmlEscaped()));
    }
}

void StrataFeature::select(const Selection &selection)
{
    m_selection = selection;
    m_cellFrom = m_cellTo = -1;
    m_graph->setSelection(selection);
    m_matrix->setSelection(selection);
    m_matrix->setCell(-1, -1);
    m_scatter->setSelection(selection);
    m_metrics->setSelection(selection);
    updateDetails();
}

void StrataFeature::showCell(int from, int to)
{
    m_cellFrom = from;
    m_cellTo = to;
    updateDetails();
    if (!m_detailsToggle->isChecked())
        setDetailsVisible(true);
}

void StrataFeature::showModule(int module)
{
    if (!m_model || module < 0 || module >= m_model->report.graph.nodes.size())
        return;
    showView(Graph);
    if (m_graph->drilledModule() >= 0)
        m_graph->drillOut();
    select({SelectionKind::Module, module});
}

void StrataFeature::openFile(int file, int line) const
{
    if (!m_model || !m_model->snapshot || file < 0 || file >= m_model->snapshot->files.size())
        return;
    m_bar->openFile(m_model->snapshot->files[file].path, line);
}

void StrataFeature::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        if (m_selection.kind != SelectionKind::None || m_cellFrom >= 0)
            select({});
        else if (m_view == Graph && m_graph->drilledModule() >= 0)
            m_graph->drillOut();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Slash) {
        showView(Rules);
        m_rules->focusFilter();
        event->accept();
        return;
    }
    NativePackageWidget::keyPressEvent(event);
}

void StrataFeature::setHostSearchQuery(const QString &text)
{
    m_query = text.trimmed();
    m_graph->setFilter(m_query);
    m_scatter->setFilter(m_query);
    m_metrics->setFilter(m_query);
    m_rules->setFilter(m_query);
}

QList<forkmesh::market::NativePackageSearchHit> StrataFeature::searchHits(const QString &query, int limit) const
{
    QList<forkmesh::market::NativePackageSearchHit> hits;
    const QString needle = query.trimmed();
    if (!m_model || !m_model->snapshot || needle.isEmpty() || limit <= 0)
        return hits;
    const Report &report = m_model->report;
    const QLocale locale;
    for (int m = 0; m < report.graph.nodes.size() && hits.size() < limit; ++m) {
        const ModuleNode &node = report.graph.nodes[m];
        if (!node.path.contains(needle, Qt::CaseInsensitive) && !node.name.contains(needle, Qt::CaseInsensitive))
            continue;
        hits.append({QStringLiteral("module:%1").arg(m), node.path.isEmpty() ? node.name : node.path,
                     QStringLiteral("Strata module · %1 lines · rating %2 · %3 debt")
                         .arg(locale.toString(node.codeLines))
                         .arg(ratingLetter(report.modules.value(m).rating))
                         .arg(formatDebt(report.modules.value(m).debt))});
    }
    const auto &files = m_model->snapshot->files;
    for (int f = 0; f < files.size() && hits.size() < limit; ++f) {
        if (!files[f].path.contains(needle, Qt::CaseInsensitive))
            continue;
        hits.append({QStringLiteral("file:%1").arg(f), files[f].path,
                     QStringLiteral("Strata file · rating %1 · %2 debt")
                         .arg(ratingLetter(report.files.value(f).rating))
                         .arg(formatDebt(report.files.value(f).debt))});
    }
    return hits;
}

void StrataFeature::activateSearchHit(const QString &id)
{
    if (!m_model)
        return;
    const int colon = id.indexOf(QLatin1Char(':'));
    const QString kind = id.left(colon);
    const int index = id.mid(colon + 1).toInt();
    if (kind == QLatin1String("module")) {
        if (index < 0 || index >= m_model->report.graph.nodes.size())
            return;
        if (m_view != Graph && m_view != Matrix && m_view != Scatter && m_view != Metrics)
            showView(Graph);
        if (m_view == Graph && m_graph->drilledModule() >= 0)
            m_graph->drillOut();
        select({SelectionKind::Module, index});
    } else if (kind == QLatin1String("file")) {
        if (index < 0 || index >= m_model->snapshot->files.size())
            return;
        const int module = m_model->report.graph.moduleOfFile.value(index, -1);
        if (m_view == Graph && module >= 0) {
            if (m_graph->drilledModule() != module)
                m_graph->drillInto(module);
        } else if (m_view != Metrics) {
            showView(Metrics);
        }
        select({SelectionKind::File, index});
    }
    if (!m_detailsToggle->isChecked())
        setDetailsVisible(true);
}

void StrataFeature::anchorClicked(const QUrl &url)
{
    const QString text = url.toString();
    const QStringList parts = text.split(QLatin1Char(':'));
    if (parts.size() < 2 || !m_model)
        return;
    const int index = parts[1].toInt();
    if (parts[0] == QLatin1String("module")) {
        if (m_view == Graph && m_graph->drilledModule() >= 0 && m_graph->drilledModule() != index)
            m_graph->drillOut();
        select({SelectionKind::Module, index});
    } else if (parts[0] == QLatin1String("file")) {
        select({SelectionKind::File, index});
    } else if (parts[0] == QLatin1String("open")) {
        openFile(index, parts.size() > 2 ? parts[2].toInt() : 0);
    } else if (parts[0] == QLatin1String("drill")) {
        showView(Graph);
        m_graph->drillInto(index);
        select({SelectionKind::Module, index});
    }
}

void StrataFeature::updateDetails()
{
    m_details->setHtml(detailsHtml());
}

QString StrataFeature::detailsHtml() const
{
    if (!m_model || !m_model->snapshot || m_model->snapshot->files.isEmpty()) {
        const Colors colors = Colors::from(this);
        return QStringLiteral("<p style=\"color:%1\">%2</p>")
            .arg(colors.muted.name(),
                 m_bar->busy() || computing() ? QStringLiteral("Analyzing…")
                                              : QStringLiteral("Open or clone a repository, or pick a folder to analyze."));
    }
    if (m_cellFrom >= 0 && m_cellTo >= 0)
        return cellDetails(m_cellFrom, m_cellTo);
    if (m_selection.kind == SelectionKind::Module)
        return moduleDetails(m_selection.index);
    if (m_selection.kind == SelectionKind::File)
        return fileDetails(m_selection.index);
    return overviewDetails();
}

QString StrataFeature::overviewDetails() const
{
    const Colors colors = Colors::from(this);
    const Report &report = m_model->report;
    const QString muted = colors.muted.name();
    QString html = QStringLiteral("<div style=\"font-size:15px;font-weight:600\">%1</div>").arg(m_bar->repositoryTitle().toHtmlEscaped());
    html += QStringLiteral("<p>%1 %2 debt ratio · %3 debt</p>")
                .arg(chip(ratingColor(report.total.rating), QString(ratingLetter(report.total.rating))), formatPercent(report.total.ratio),
                     formatDebt(report.total.debt));
    html += QStringLiteral("<p style=\"color:%1\">Click a module or file to see its numbers, rules and dependencies. "
                           "Double-click a module in the graph to see its files.</p>").arg(muted);
    if (!report.graph.cycles.isEmpty()) {
        html += QStringLiteral("<p><b style=\"color:%1\">Module cycles</b></p>").arg(colors.unhealthy.name());
        for (const QVector<int> &cycle : report.graph.cycles) {
            QStringList names;
            for (int m : cycle)
                names << QStringLiteral("<a href=\"module:%1\" style=\"color:%2;text-decoration:none\">%3</a>")
                             .arg(m)
                             .arg(colors.accent.name(), report.graph.nodes[m].name.toHtmlEscaped());
            html += QStringLiteral("<p>%1</p>").arg(names.join(QStringLiteral(" ⇄ ")));
        }
    }
    return html;
}

QString StrataFeature::moduleDetails(int module) const
{
    const Report &report = m_model->report;
    if (module < 0 || module >= report.graph.nodes.size())
        return overviewDetails();
    const Colors colors = Colors::from(this);
    const QString muted = colors.muted.name();
    const QString link = colors.accent.name();
    const QLocale locale;
    const ModuleNode &node = report.graph.nodes[module];
    const Score &score = report.modules[module];
    const auto &files = m_model->snapshot->files;
    QString html = QStringLiteral("<div style=\"font-size:16px;font-weight:600\">%1</div>").arg(node.name.toHtmlEscaped());
    if (node.path != node.name)
        html += QStringLiteral("<div style=\"color:%1\">%2</div>").arg(muted, (node.path.isEmpty() ? QStringLiteral("(root)") : node.path).toHtmlEscaped());
    html += QStringLiteral("<p>%1 &nbsp;%2 debt ratio · %3 debt</p>")
                .arg(chip(ratingColor(score.rating), QString(ratingLetter(score.rating))), formatPercent(score.ratio), formatDebt(score.debt));
    if (node.cycle >= 0)
        html += QStringLiteral("<p style=\"color:%1\">In a dependency cycle with %2 other modules.</p>")
                    .arg(colors.unhealthy.name())
                    .arg(report.graph.cycles.value(node.cycle).size() - 1);
    html += QStringLiteral("<table cellspacing=\"0\" cellpadding=\"2\">");
    html += row(QStringLiteral("Lines of code"), locale.toString(node.codeLines), muted);
    html += row(QStringLiteral("Files"), locale.toString(node.files.size()), muted);
    html += row(QStringLiteral("Functions"), locale.toString(node.functions), muted);
    html += row(QStringLiteral("Complexity"), locale.toString(node.complexity), muted);
    html += row(QStringLiteral("Types"), QStringLiteral("%1 (%2 abstract)").arg(node.types).arg(node.abstractTypes), muted);
    html += row(QStringLiteral("Health"), QStringLiteral("%1 %2").arg(dot(healthColor(node.health)), QString::number(node.health, 'f', 1)), muted);
    html += row(QStringLiteral("Afferent Ca"), QString::number(node.afferent), muted);
    html += row(QStringLiteral("Efferent Ce"), QString::number(node.efferent), muted);
    html += row(QStringLiteral("Instability I"), two(node.instability), muted);
    html += row(QStringLiteral("Abstractness A"), two(node.abstractness), muted);
    html += row(QStringLiteral("Distance D"), QStringLiteral("%1 %2").arg(dot(scaleColor(node.distance)), two(node.distance)), muted);
    html += row(QStringLiteral("Cohesion"), two(node.cohesion), muted);
    if (node.commits > 0)
        html += row(QStringLiteral("Commits"), locale.toString(node.commits), muted);
    if (report.history)
        html += row(QStringLiteral("Interest"), formatDebt(score.interest) + QStringLiteral(" a year"), muted);
    html += QStringLiteral("</table>");
    html += QStringLiteral("<p><a href=\"drill:%1\" style=\"color:%2;text-decoration:none\">Show its files in the graph ›</a></p>").arg(module).arg(link);

    const QVector<ViolationRef> worst = report.worstForModule(module, 6);
    if (!worst.isEmpty()) {
        html += QStringLiteral("<p><b>Worst rules</b></p>");
        for (const ViolationRef &ref : worst) {
            const Rule &rule = report.rules[ref.first];
            const Violation &v = report.violation(ref);
            QString where;
            if (!rule.moduleScope && v.file >= 0)
                where = QStringLiteral("<a href=\"open:%1:%2\" style=\"color:%3;text-decoration:none\">%4%5</a> · ")
                            .arg(v.file)
                            .arg(v.line)
                            .arg(link, QFileInfo(files[v.file].path).fileName().toHtmlEscaped(),
                                 v.line > 0 ? QStringLiteral(":%1").arg(v.line) : QString());
            html += QStringLiteral("<p style=\"margin:2px 0\">%1 %2<br><span style=\"color:%3\">%4%5 · %6</span></p>")
                        .arg(dot(severityColor(v.severity)), rule.title.toHtmlEscaped(), muted, where, v.message.toHtmlEscaped(),
                             formatDebt(v.debt));
        }
    }

    QVector<QPair<int, int>> uses;
    QVector<QPair<int, int>> usedBy;
    for (const insight::ModuleEdge &edge : report.graph.edges) {
        if (edge.from == module && edge.to != module)
            uses.append({edge.weight, edge.to});
        else if (edge.to == module && edge.from != module)
            usedBy.append({edge.weight, edge.from});
    }
    const auto byWeight = [](const QPair<int, int> &a, const QPair<int, int> &b) { return a.first > b.first; };
    std::stable_sort(uses.begin(), uses.end(), byWeight);
    std::stable_sort(usedBy.begin(), usedBy.end(), byWeight);
    const auto list = [&](const QString &title, const QVector<QPair<int, int>> &items, const QColor &color) {
        QString out = QStringLiteral("<p><b style=\"color:%1\">%2</b> <span style=\"color:%3\">%4</span></p>")
                          .arg(color.name(), title, muted)
                          .arg(items.size());
        if (items.isEmpty())
            out += QStringLiteral("<p style=\"color:%1;margin:2px 0\">None</p>").arg(muted);
        for (int i = 0; i < items.size() && i < 25; ++i) {
            const ModuleNode &other = report.graph.nodes[items[i].second];
            const bool cycle = other.cycle >= 0 && other.cycle == node.cycle;
            out += QStringLiteral("<p style=\"margin:2px 0\"><a href=\"module:%1\" style=\"color:%2;text-decoration:none\">%3</a> "
                                  "<span style=\"color:%4\">%5 imports%6</span></p>")
                       .arg(items[i].second)
                       .arg(link, other.name.toHtmlEscaped(), muted)
                       .arg(items[i].first)
                       .arg(cycle ? QStringLiteral(" · cycle") : QString());
        }
        if (items.size() > 25)
            out += QStringLiteral("<p style=\"color:%1\">and %2 more</p>").arg(muted).arg(items.size() - 25);
        return out;
    };
    html += list(QStringLiteral("Uses"), uses, colors.accent);
    html += list(QStringLiteral("Used by"), usedBy, colors.dependent);

    QVector<int> ranked = node.files;
    std::stable_sort(ranked.begin(), ranked.end(), [&report](int a, int b) { return report.files[a].debt > report.files[b].debt; });
    if (!ranked.isEmpty() && report.files[ranked.first()].debt > 0) {
        html += QStringLiteral("<p><b>Files with the most debt</b></p>");
        for (int i = 0; i < ranked.size() && i < 8; ++i) {
            const int f = ranked[i];
            if (report.files[f].debt <= 0)
                break;
            html += QStringLiteral("<p style=\"margin:2px 0\">%1 <a href=\"file:%2\" style=\"color:%3;text-decoration:none\">%4</a> "
                                   "<span style=\"color:%5\">%6</span></p>")
                        .arg(chip(ratingColor(report.files[f].rating), QString(ratingLetter(report.files[f].rating))))
                        .arg(f)
                        .arg(link, QFileInfo(files[f].path).fileName().toHtmlEscaped(), muted, formatDebt(report.files[f].debt));
        }
    }
    return html;
}

QString StrataFeature::fileDetails(int file) const
{
    const Report &report = m_model->report;
    const auto &files = m_model->snapshot->files;
    if (file < 0 || file >= files.size())
        return overviewDetails();
    const Colors colors = Colors::from(this);
    const QString muted = colors.muted.name();
    const QString link = colors.accent.name();
    const QLocale locale;
    const FileInfo &info = files[file];
    const Score &score = report.files[file];
    QString html = QStringLiteral("<div style=\"font-size:16px;font-weight:600\">%1</div>").arg(QFileInfo(info.path).fileName().toHtmlEscaped());
    html += QStringLiteral("<div style=\"color:%1\">%2</div>").arg(muted, info.path.toHtmlEscaped());
    html += QStringLiteral("<p>%1 &nbsp;%2 debt ratio · %3 debt</p>")
                .arg(chip(ratingColor(score.rating), QString(ratingLetter(score.rating))), formatPercent(score.ratio), formatDebt(score.debt));
    html += QStringLiteral("<p><a href=\"open:%1:0\" style=\"color:%2;text-decoration:none\">Open the file ›</a></p>").arg(file).arg(link);
    const int module = report.graph.moduleOfFile.value(file, -1);
    html += QStringLiteral("<table cellspacing=\"0\" cellpadding=\"2\">");
    if (module >= 0)
        html += row(QStringLiteral("Module"), QStringLiteral("<a href=\"module:%1\" style=\"color:%2;text-decoration:none\">%3</a>")
                                                  .arg(module)
                                                  .arg(link, report.graph.nodes[module].name.toHtmlEscaped()),
                    muted);
    html += row(QStringLiteral("Language"), info.language.toHtmlEscaped(), muted);
    html += row(QStringLiteral("Lines of code"), QStringLiteral("%1 (%2 comment lines)").arg(locale.toString(info.codeLines), locale.toString(info.commentLines)), muted);
    html += row(QStringLiteral("Functions"), locale.toString(info.functions.size()), muted);
    html += row(QStringLiteral("Complexity"), QStringLiteral("%1 · max %2").arg(info.complexity).arg(info.maxComplexity), muted);
    html += row(QStringLiteral("Max nesting"), QString::number(info.maxNesting), muted);
    html += row(QStringLiteral("Health"), QStringLiteral("%1 %2").arg(dot(healthColor(info.health)), QString::number(info.health, 'f', 1)), muted);
    html += row(QStringLiteral("Imports"), QStringLiteral("%1 files · %2 packages").arg(info.dependsOn.size()).arg(info.externalImports.size()), muted);
    html += row(QStringLiteral("Imported by"), QStringLiteral("%1 files").arg(info.dependedOnBy.size()), muted);
    if (info.duplicatedLines > 0)
        html += row(QStringLiteral("Duplicated"), QStringLiteral("%1 lines").arg(info.duplicatedLines), muted);
    if (info.commits > 0) {
        html += row(QStringLiteral("Commits"), QStringLiteral("%1 (%2 recent, %3 fixes)").arg(info.commits).arg(info.recentCommits).arg(info.fixCommits), muted);
        html += row(QStringLiteral("Authors"), QStringLiteral("%1 · main %2 (%3)").arg(info.authors).arg(info.mainAuthor.toHtmlEscaped(), formatPercent(info.mainAuthorShare)), muted);
        html += row(QStringLiteral("Interest"), formatDebt(score.interest) + QStringLiteral(" a year"), muted);
    }
    html += QStringLiteral("</table>");
    if (!info.healthIssues.isEmpty()) {
        html += QStringLiteral("<p><b>Health</b></p>");
        for (const QString &issue : info.healthIssues)
            html += QStringLiteral("<p style=\"margin:2px 0;color:%1\">• %2</p>").arg(muted, issue.toHtmlEscaped());
    }
    QVector<ViolationRef> refs = report.fileViolations.value(file);
    std::stable_sort(refs.begin(), refs.end(), [&report](const ViolationRef &a, const ViolationRef &b) {
        const Violation &va = report.violation(a);
        const Violation &vb = report.violation(b);
        if (va.severity != vb.severity)
            return va.severity < vb.severity;
        return va.debt > vb.debt;
    });
    if (!refs.isEmpty()) {
        html += QStringLiteral("<p><b>Rule violations</b> <span style=\"color:%1\">%2</span></p>").arg(muted).arg(refs.size());
        for (int i = 0; i < refs.size() && i < 30; ++i) {
            const Rule &rule = report.rules[refs[i].first];
            const Violation &v = report.violation(refs[i]);
            html += QStringLiteral("<p style=\"margin:2px 0\">%1 <a href=\"open:%2:%3\" style=\"color:%4;text-decoration:none\">%5</a><br>"
                                   "<span style=\"color:%6\">%7 · %8</span></p>")
                        .arg(dot(severityColor(v.severity)))
                        .arg(file)
                        .arg(v.line)
                        .arg(link, v.message.toHtmlEscaped(), muted, rule.title.toHtmlEscaped(), formatDebt(v.debt));
        }
    }
    const auto fileList = [&](const QString &title, const QVector<int> &items, const QColor &color) {
        if (items.isEmpty())
            return QString();
        QString out = QStringLiteral("<p><b style=\"color:%1\">%2</b> <span style=\"color:%3\">%4</span></p>").arg(color.name(), title, muted).arg(items.size());
        for (int i = 0; i < items.size() && i < 15; ++i)
            out += QStringLiteral("<p style=\"margin:2px 0\"><a href=\"file:%1\" style=\"color:%2;text-decoration:none\">%3</a></p>")
                       .arg(items[i])
                       .arg(link, files[items[i]].path.toHtmlEscaped());
        if (items.size() > 15)
            out += QStringLiteral("<p style=\"color:%1\">and %2 more</p>").arg(muted).arg(items.size() - 15);
        return out;
    };
    html += fileList(QStringLiteral("Uses"), info.dependsOn, colors.accent);
    html += fileList(QStringLiteral("Used by"), info.dependedOnBy, colors.dependent);
    return html;
}

QString StrataFeature::cellDetails(int from, int to) const
{
    const Report &report = m_model->report;
    const auto &files = m_model->snapshot->files;
    if (from < 0 || to < 0 || from >= report.graph.nodes.size() || to >= report.graph.nodes.size())
        return overviewDetails();
    const Colors colors = Colors::from(this);
    const QString muted = colors.muted.name();
    const QString link = colors.accent.name();
    QString html;
    const auto direction = [&](int a, int b, const QColor &color) {
        const int index = report.graph.edgeIndex(a, b);
        const insight::ModuleEdge *edge = nullptr;
        if (index >= 0 && index < report.graph.edges.size())
            edge = &report.graph.edges[index];
        if (!edge) {
            for (const insight::ModuleEdge &candidate : report.graph.edges) {
                if (candidate.from == a && candidate.to == b) {
                    edge = &candidate;
                    break;
                }
            }
        }
        QString out = QStringLiteral("<p><b style=\"color:%1\">%2 → %3</b> <span style=\"color:%4\">%5 imports</span></p>")
                          .arg(color.name(), report.graph.nodes[a].name.toHtmlEscaped(), report.graph.nodes[b].name.toHtmlEscaped(), muted)
                          .arg(edge ? edge->weight : 0);
        if (!edge)
            return out + QStringLiteral("<p style=\"color:%1\">No imports in this direction.</p>").arg(muted);
        for (int i = 0; i < edge->fileEdges.size() && i < 60; ++i) {
            const int fa = edge->fileEdges[i].first;
            const int fb = edge->fileEdges[i].second;
            if (fa < 0 || fb < 0 || fa >= files.size() || fb >= files.size())
                continue;
            out += QStringLiteral("<p style=\"margin:2px 0\"><a href=\"open:%1:0\" style=\"color:%2;text-decoration:none\">%3</a> "
                                  "<span style=\"color:%4\">imports</span> <a href=\"open:%5:0\" style=\"color:%2;text-decoration:none\">%6</a></p>")
                       .arg(fa)
                       .arg(link, files[fa].path.toHtmlEscaped(), muted)
                       .arg(fb)
                       .arg(files[fb].path.toHtmlEscaped());
        }
        if (edge->fileEdges.size() > 60)
            out += QStringLiteral("<p style=\"color:%1\">and %2 more</p>").arg(muted).arg(edge->fileEdges.size() - 60);
        return out;
    };
    html += QStringLiteral("<div style=\"font-size:15px;font-weight:600\">Dependencies</div>");
    if (from == to) {
        html += QStringLiteral("<p>%1 has %2 imports between its own files.</p>")
                    .arg(report.graph.nodes[from].name.toHtmlEscaped())
                    .arg(report.graph.nodes[from].internalEdges);
        return html;
    }
    html += direction(from, to, colors.accent);
    html += direction(to, from, colors.dependent);
    return html;
}

bool StrataFeature::writeReport(const QString &path) const
{
    if (!m_model || !m_model->snapshot)
        return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const QString title = m_bar->repositoryTitle().isEmpty() ? m_model->snapshot->name : m_bar->repositoryTitle();
    file.write(reportHtml(*m_model->snapshot, m_model->report, title).toUtf8());
    return file.commit();
}

void StrataFeature::exportReport()
{
    if (!m_model)
        return;
    const QString title = m_bar->repositoryTitle().isEmpty() ? QStringLiteral("repository") : m_bar->repositoryTitle();
    const QString suggested = QDir(QDir::homePath()).filePath(QStringLiteral("%1-strata-report.html").arg(title));
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export Strata report"), suggested,
                                                      QStringLiteral("HTML report (*.html)"));
    if (path.isEmpty())
        return;
    if (writeReport(path))
        m_bar->setSummary(summaryLine(m_model->report) + QStringLiteral(" · report saved"));
    else
        m_bar->setSummary(summaryLine(m_model->report) + QStringLiteral(" · the report could not be saved"));
}

}

namespace {
const forkmesh::market::NativePackageRegistration strataRegistration({
    QStringLiteral("builtin:strata"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Strata"),
    QStringLiteral("layers"),
    QString(),
    QString(),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::strata::StrataFeature(host, parent);
    },
});
}
