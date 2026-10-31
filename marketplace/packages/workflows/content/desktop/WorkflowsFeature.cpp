#include "WorkflowsFeature.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMimeData>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace forkmesh::workflows {
namespace {

constexpr char kOrigin[] = "https://app.forkmesh.com";
constexpr char kPrefix[] = "/api/workflows";
constexpr char kCleanupProcess[] = "forkmeshFreeDiskSpaceProcess";
constexpr int kRunsShown = 200;
constexpr int kRunRequests = 4;

QString text(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString();
}

qint64 integer(const QJsonObject &object, const char *key)
{
    return qint64(object.value(QLatin1String(key)).toDouble());
}

QString path(const QString &suffix = QString())
{
    return QString::fromLatin1(kPrefix) + suffix;
}

qint64 now()
{
    return QDateTime::currentMSecsSinceEpoch();
}

QLabel *mutedLabel(const QString &value)
{
    auto *label = new QLabel(value);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    return label;
}

QLabel *sectionLabel(const QString &value)
{
    auto *label = new QLabel(value);
    label->setObjectName(QStringLiteral("sectionTitle"));
    label->setStyleSheet(QStringLiteral("font-weight: 600;"));
    return label;
}

QPushButton *button(const QString &label, const char *style = "ghostButton")
{
    auto *item = new QPushButton(label);
    item->setObjectName(QLatin1String(style));
    item->setAccessibleName(label);
    return item;
}

const QString kWorkflowDragMime = QStringLiteral("application/x-forkmesh-workflow");

class WorkflowDragTable final : public QTableWidget
{
public:
    using QTableWidget::QTableWidget;

protected:
    QStringList mimeTypes() const override { return {kWorkflowDragMime}; }

    QMimeData *mimeData(const QList<QTableWidgetItem *> &items) const override
    {
        for (QTableWidgetItem *item : items) {
            QTableWidgetItem *name = item ? this->item(item->row(), 0) : nullptr;
            if (!name || name->data(Qt::UserRole).toString().isEmpty())
                continue;
            auto *mime = new QMimeData;
            mime->setData(kWorkflowDragMime,
                          QJsonDocument(QJsonObject{{QStringLiteral("repo"), name->data(Qt::UserRole).toString()},
                                                    {QStringLiteral("id"), name->data(Qt::UserRole + 1).toString()}})
                              .toJson(QJsonDocument::Compact));
            mime->setText(name->text());
            return mime;
        }
        return nullptr;
    }
};

QTableWidget *makeTable(const QStringList &headers, const QString &name, QTableWidget *table = nullptr)
{
    if (!table)
        table = new QTableWidget(0, int(headers.size()));
    else
        table->setColumnCount(int(headers.size()));
    table->setObjectName(name);
    table->setAccessibleName(headers.first());
    table->setHorizontalHeaderLabels(headers);
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->setWordWrap(false);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setHighlightSections(false);
    return table;
}

QTableWidgetItem *cell(const QString &value, const QString &tooltip = QString())
{
    auto *item = new QTableWidgetItem(value);
    item->setToolTip(tooltip.isEmpty() ? value : tooltip);
    return item;
}

QTableWidgetItem *statusCell(const QString &status)
{
    auto *item = cell(statusLabel(status));
    if (status == QLatin1String("success"))
        item->setForeground(QColor(QStringLiteral("#16a34a")));
    else if (status == QLatin1String("error"))
        item->setForeground(QColor(QStringLiteral("#dc2626")));
    return item;
}

QString timeText(qint64 atMs)
{
    if (atMs <= 0)
        return QStringLiteral("–");
    return QLocale().toString(QDateTime::fromMSecsSinceEpoch(atMs), QLocale::ShortFormat);
}

QString itemPreview(const QJsonObject &entry)
{
    const QJsonObject main = entry.value(QStringLiteral("outputs")).toObject();
    for (auto it = main.begin(); it != main.end(); ++it) {
        const QJsonArray items = it.value().toObject().value(QStringLiteral("items")).toArray();
        if (!items.isEmpty())
            return QString::fromUtf8(QJsonDocument(items.first().toObject()).toJson(QJsonDocument::Compact)).left(240);
    }
    return {};
}

int itemCount(const QJsonObject &entry)
{
    int count = 0;
    const QJsonObject outputs = entry.value(QStringLiteral("outputs")).toObject();
    for (auto it = outputs.begin(); it != outputs.end(); ++it)
        count += it.value().toObject().value(QStringLiteral("count")).toInt();
    return count;
}

QString paramString(const QJsonValue &value)
{
    if (value.isString())
        return value.toString();
    if (value.isDouble())
        return QString::number(value.toDouble());
    if (value.isBool())
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (value.isObject())
        return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Indented)).trimmed();
    if (value.isArray())
        return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Indented)).trimmed();
    return {};
}

QJsonValue storedValue(const QString &value, const QJsonValue &previous)
{
    if (previous.isDouble()) {
        bool ok = false;
        const double number = value.trimmed().toDouble(&ok);
        if (ok)
            return number;
    }
    if (previous.isObject() || previous.isArray()) {
        const QJsonDocument parsed = QJsonDocument::fromJson(value.toUtf8());
        if (parsed.isObject())
            return parsed.object();
        if (parsed.isArray())
            return parsed.array();
    }
    return value;
}

bool liveStatus(const QString &status)
{
    return status == QLatin1String("running") || status == QLatin1String("queued");
}

QString runFilterKey(const QJsonObject &entry)
{
    return text(entry, "source") == QLatin1String("local") ? text(entry, "key") : text(entry, "id");
}

QString stateTone(const QJsonObject &entry)
{
    if (!text(entry, "error").isEmpty())
        return QStringLiteral("attention");
    if (entry.value(QStringLiteral("running")).toBool() || entry.value(QStringLiteral("agentWorking")).toBool())
        return QStringLiteral("accent");
    if (!entry.value(QStringLiteral("enabled")).toBool())
        return QString();
    if (text(entry, "lastStatus") == QLatin1String("error"))
        return QStringLiteral("danger");
    if (text(entry, "lastStatus") == QLatin1String("success"))
        return QStringLiteral("success");
    return QString();
}

void setLiveText(QPlainTextEdit *view, const QString &value, bool toEnd)
{
    if (!view || view->toPlainText() == value)
        return;
    QScrollBar *bar = view->verticalScrollBar();
    const bool atEnd = toEnd || bar->value() >= bar->maximum() - 4;
    const int kept = bar->value();
    view->setPlainText(value);
    bar->setValue(atEnd ? bar->maximum() : kept);
}

QString localRunLog(const QJsonObject &run)
{
    QStringList parts;
    for (const QJsonValue &value : run.value(QStringLiteral("steps")).toArray()) {
        const QJsonObject entry = value.toObject();
        const QString status = text(entry, "status");
        QString body = text(entry, "output").trimmed();
        if (body.isEmpty()) {
            QStringList logs;
            for (const QJsonValue &line : entry.value(QStringLiteral("logs")).toArray())
                logs << line.toString();
            body = logs.join(QLatin1Char('\n')).trimmed();
        }
        const QString error = text(entry, "error").trimmed();
        const bool running = status == QLatin1String("running");
        if (body.isEmpty() && error.isEmpty() && !running)
            continue;
        QStringList block{QStringLiteral("── %1 · %2").arg(text(entry, "name"), localStatusLabel(status))};
        if (!body.isEmpty())
            block << body;
        if (!error.isEmpty() && !body.contains(error))
            block << QStringLiteral("Error: %1").arg(error);
        if (running && body.isEmpty())
            block << QStringLiteral("Waiting for output…");
        parts << block.join(QLatin1Char('\n'));
    }
    if (parts.isEmpty() && !text(run, "error").trimmed().isEmpty())
        parts << text(run, "error").trimmed();
    if (parts.isEmpty())
        return liveStatus(text(run, "status")) ? QStringLiteral("Waiting for output…")
                                               : QStringLiteral("This run left no output.");
    return parts.join(QStringLiteral("\n\n"));
}

QList<FlowStep> flowSteps(const QJsonArray &steps)
{
    QList<FlowStep> out;
    const qint64 current = now();
    for (const QJsonValue &value : steps) {
        const QJsonObject entry = value.toObject();
        FlowStep step{text(entry, "name"), text(entry, "status"), text(entry, "note"), text(entry, "error")};
        const qint64 begun = integer(entry, "startedAtMs");
        const qint64 ended = integer(entry, "finishedAtMs");
        if (step.note.isEmpty() && begun > 0 && ended >= begun)
            step.note = durationText(ended - begun);
        else if (step.note.isEmpty() && begun > 0 && step.status == QLatin1String("running"))
            step.note = QStringLiteral("Running · %1").arg(durationText(std::max<qint64>(0, current - begun)));
        if (step.detail.isEmpty())
            step.detail = text(entry, "output").trimmed().section(QLatin1Char('\n'), -1);
        out.append(step);
    }
    return out;
}

QJsonArray pendingSteps(const Draft &draft)
{
    QJsonArray out;
    for (const QString &id : draft.ordered()) {
        const QJsonObject node = draft.node(id);
        if (node.value(QStringLiteral("disabled")).toBool())
            continue;
        const QString name = text(node, "name");
        out.append(QJsonObject{{QStringLiteral("name"), name.isEmpty() ? id : name},
                               {QStringLiteral("status"), QStringLiteral("queued")}});
    }
    return out;
}

} // namespace

WorkflowsFeature::WorkflowsFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent), m_request(host.request),
      m_searchIndexChanged(host.searchIndexChanged), m_navigationBadgeChanged(host.navigationBadgeChanged),
      m_repositories(host.repositories), m_repositoryWorkflows(host.repositoryWorkflows),
      m_repositoryWorkflowAction(host.repositoryWorkflowAction)
{
    setObjectName(QStringLiteral("workflowsSection"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *heading = new QHBoxLayout;
    auto *titles = new QVBoxLayout;
    titles->setSpacing(2);
    auto *title = new QLabel(QStringLiteral("Workflows"));
    title->setObjectName(QStringLiteral("pageTitle"));
    titles->addWidget(title);
    m_subtitle = new QLabel(QStringLiteral(
        "Special agents, workflows on this desktop and workflows on ForkMesh, in one list. Every one can run "
        "with an assignment, keep a schedule, sit on a rail and show its latest output."));
    m_subtitle->setObjectName(QStringLiteral("workflowsSubtitle"));
    m_subtitle->setWordWrap(true);
    titles->addWidget(m_subtitle);
    heading->addLayout(titles, 1);
    heading->addSpacing(12);
    m_newButton = button(QStringLiteral("New workflow"), "primaryButton");
    m_newButton->setToolTip(QStringLiteral("A special agent, a desktop workflow, or one that runs on ForkMesh"));
    auto *credentialsButton = button(QStringLiteral("Credentials"));
    credentialsButton->setToolTip(QStringLiteral("The integration accounts your steps sign in with"));
    auto *refreshButton = button(QStringLiteral("Refresh"));
    auto *webButton = button(QStringLiteral("Open in browser"));
    webButton->setToolTip(QStringLiteral("Open the visual canvas on the web dashboard"));
    for (QPushButton *item : {m_newButton, credentialsButton, refreshButton, webButton})
        heading->addWidget(item);
    outer->addLayout(heading);

    m_status = mutedLabel(QStringLiteral("Loading workflows…"));
    m_status->setObjectName(QStringLiteral("mutedLabel"));
    outer->addWidget(m_status);

    m_pages = new QStackedWidget;
    m_pages->addWidget(buildDashboard());
    m_pages->addWidget(buildEditor());
    outer->addWidget(m_pages, 1);
    applyTheme();

    connect(m_newButton, &QPushButton::clicked, this, [this] { newWorkflowDialog(); });
    connect(credentialsButton, &QPushButton::clicked, this, [this] { credentialsDialog(); });
    connect(refreshButton, &QPushButton::clicked, this, [this] {
        if (m_pages->currentIndex() == 1 && !m_draft.id.isEmpty())
            loadEditorRuns();
        refresh();
    });
    connect(webButton, &QPushButton::clicked, this, [this] {
        QString target = QString::fromLatin1(kOrigin) + QStringLiteral("/dashboard/workflows");
        if (m_pages->currentIndex() == 1 && !m_draft.id.isEmpty())
            target += QStringLiteral("?id=") + m_draft.id;
        QDesktopServices::openUrl(QUrl(target));
    });
}

QWidget *WorkflowsFeature::buildDashboard()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    auto *kpis = new QHBoxLayout;
    kpis->setSpacing(10);
    const QStringList names{QStringLiteral("Workflows"),    QStringLiteral("Active"),
                            QStringLiteral("Runs"),         QStringLiteral("Success rate"),
                            QStringLiteral("Avg. duration"), QStringLiteral("Last run"),
                            QStringLiteral("Next scheduled run")};
    for (const QString &name : names) {
        auto *tile = new QFrame;
        tile->setObjectName(QStringLiteral("workflowsKpiTile"));
        tile->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto *tileLayout = new QVBoxLayout(tile);
        tileLayout->setContentsMargins(14, 10, 14, 10);
        tileLayout->setSpacing(1);
        auto *label = new QLabel(name);
        label->setObjectName(QStringLiteral("workflowsKpiName"));
        tileLayout->addWidget(label);
        auto *value = new QLabel(QStringLiteral("–"));
        value->setObjectName(QStringLiteral("workflowsKpi"));
        value->setAccessibleName(name);
        tileLayout->addWidget(value);
        m_kpiValues.append(value);
        auto *note = new QLabel;
        note->setObjectName(QStringLiteral("workflowsKpiNote"));
        note->setMinimumWidth(0);
        note->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        tileLayout->addWidget(note);
        m_kpiNotes.append(note);
        kpis->addWidget(tile, 1);
    }
    layout->addLayout(kpis);

    auto *splitter = new QSplitter(Qt::Vertical);
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(buildCollection());
    m_dashboardSplitter = splitter;

    auto *bottom = new QWidget;
    auto *bottomLayout = new QVBoxLayout(bottom);
    bottomLayout->setContentsMargins(0, 8, 0, 0);
    bottomLayout->setSpacing(6);
    auto *runHeading = new QHBoxLayout;
    runHeading->addWidget(sectionLabel(QStringLiteral("Recent runs")));
    runHeading->addStretch();
    m_runWorkflowFilter = new QComboBox;
    m_runWorkflowFilter->setAccessibleName(QStringLiteral("Runs of workflow"));
    m_runWorkflowFilter->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_runStatusFilter = new QComboBox;
    m_runStatusFilter->setAccessibleName(QStringLiteral("Runs with result"));
    m_runStatusFilter->addItem(QStringLiteral("All results"), QString());
    m_runStatusFilter->addItem(QStringLiteral("Succeeded"), QStringLiteral("success"));
    m_runStatusFilter->addItem(QStringLiteral("Failed"), QStringLiteral("error"));
    runHeading->addWidget(m_runWorkflowFilter);
    runHeading->addWidget(m_runStatusFilter);
    bottomLayout->addLayout(runHeading);
    auto *runSplitter = new QSplitter(Qt::Horizontal);
    runSplitter->setChildrenCollapsible(false);
    m_runTable = makeTable({QStringLiteral("Workflow"), QStringLiteral("Started"), QStringLiteral("Trigger"),
                            QStringLiteral("Result"), QStringLiteral("Duration"), QStringLiteral("Output")},
                           QStringLiteral("workflowsRuns"));
    m_runTable->setIconSize(QSize(18, 18));
    runSplitter->addWidget(m_runTable);
    auto *detail = new QWidget;
    auto *detailLayout = new QVBoxLayout(detail);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    detailLayout->setSpacing(4);
    m_runSummary = mutedLabel(QStringLiteral("Select a run to see what each step did."));
    m_runSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detailLayout->addWidget(m_runSummary);
    m_runSteps = makeTable({QStringLiteral("Step"), QStringLiteral("Result"), QStringLiteral("Time"),
                            QStringLiteral("Items"), QStringLiteral("Details")},
                           QStringLiteral("workflowsRunSteps"));
    m_runSteps->setSelectionMode(QAbstractItemView::NoSelection);
    m_runFlow = new RunFlow;
    m_runFlowArea = new QScrollArea;
    m_runFlowArea->setObjectName(QStringLiteral("workflowsRunFlowArea"));
    m_runFlowArea->setFrameShape(QFrame::NoFrame);
    m_runFlowArea->setWidgetResizable(true);
    m_runFlowArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_runFlowArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_runFlowArea->setStyleSheet(QStringLiteral("QScrollArea#workflowsRunFlowArea { background: transparent; }"));
    m_runFlowArea->viewport()->setAutoFillBackground(false);
    m_runFlowArea->setWidget(m_runFlow);
    m_runFlowArea->setFixedHeight(m_runFlow->sizeHint().height() + 12);
    m_runFlowArea->hide();
    detailLayout->addWidget(m_runFlowArea);
    auto *detailSplitter = new QSplitter(Qt::Vertical);
    detailSplitter->setChildrenCollapsible(false);
    m_runOutput = new QPlainTextEdit;
    m_runOutput->setObjectName(QStringLiteral("workflowsRunOutput"));
    m_runOutput->setAccessibleName(QStringLiteral("Run output"));
    m_runOutput->setReadOnly(true);
    m_runOutput->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_runOutput->setPlaceholderText(QStringLiteral("The selected run's output shows here, live while it runs."));
    detailSplitter->addWidget(m_runOutput);
    detailSplitter->addWidget(m_runSteps);
    detailSplitter->setStretchFactor(0, 2);
    detailSplitter->setStretchFactor(1, 1);
    detailLayout->addWidget(detailSplitter, 1);
    runSplitter->addWidget(detail);
    runSplitter->setStretchFactor(0, 1);
    runSplitter->setStretchFactor(1, 1);
    bottomLayout->addWidget(runSplitter, 1);
    splitter->addWidget(bottom);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);

    connect(m_runWorkflowFilter, &QComboBox::currentIndexChanged, this, [this] { renderRunTable(); });
    connect(m_runStatusFilter, &QComboBox::currentIndexChanged, this, [this] { renderRunTable(); });
    m_liveTimer = new QTimer(this);
    m_liveTimer->setInterval(1000);
    connect(m_liveTimer, &QTimer::timeout, this, [this] { pollLiveRun(); });
    connect(m_runTable, &QTableWidget::itemSelectionChanged, this, [this] { showSelectedRun(); });
    connect(m_runTable, &QTableWidget::cellDoubleClicked, this, [this] {
        const QJsonObject run = selectedDashboardRun();
        if (run.value(QStringLiteral("local")).toBool()) {
            openLocalWorkflow(text(run, "repo"), text(run, "localWorkflowId"));
            return;
        }
        openWorkflow(text(run, "workflowId"));
    });
    return page;
}

QWidget *WorkflowsFeature::buildCollection()
{
    auto *section = new QWidget;
    section->setObjectName(QStringLiteral("workflowsCollectionSection"));
    auto *layout = new QVBoxLayout(section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto *bar = new QHBoxLayout;
    bar->setSpacing(6);
    const QList<QPair<QString, QString>> scopes{{QString(), QStringLiteral("All")},
                                                {QStringLiteral("agent"), QStringLiteral("Special agents")},
                                                {QStringLiteral("desktop"), QStringLiteral("On this desktop")},
                                                {QStringLiteral("account"), QStringLiteral("On ForkMesh")},
                                                {QStringLiteral("pinned"), QStringLiteral("Pinned")},
                                                {QStringLiteral("failing"), QStringLiteral("Needs attention")}};
    for (const auto &scope : scopes) {
        auto *chip = new QToolButton;
        chip->setObjectName(QStringLiteral("workflowsScope"));
        chip->setCheckable(true);
        chip->setChecked(scope.first == m_scope);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setText(scope.second);
        chip->setAccessibleName(QStringLiteral("Show %1").arg(scope.second.toLower()));
        chip->setProperty("scope", scope.first);
        connect(chip, &QToolButton::clicked, this, [this, value = scope.first] {
            m_scope = value;
            for (QToolButton *other : std::as_const(m_scopeButtons))
                other->setChecked(other->property("scope").toString() == m_scope);
            renderCollection();
        });
        m_scopeButtons.append(chip);
        bar->addWidget(chip);
    }
    bar->addStretch(1);
    m_collectionCount = new QLabel;
    m_collectionCount->setObjectName(QStringLiteral("workflowsSubtitle"));
    bar->addWidget(m_collectionCount);
    bar->addSpacing(6);
    m_filter = new QLineEdit;
    m_filter->setPlaceholderText(QStringLiteral("Filter by name, trigger, repository or output"));
    m_filter->setAccessibleName(QStringLiteral("Filter workflows"));
    m_filter->setClearButtonEnabled(true);
    m_filter->setMinimumWidth(180);
    m_filter->setMaximumWidth(280);
    bar->addWidget(m_filter);
    m_sortCombo = new QComboBox;
    m_sortCombo->setAccessibleName(QStringLiteral("Sort workflows"));
    m_sortCombo->addItem(QStringLiteral("Name"), QStringLiteral("name"));
    m_sortCombo->addItem(QStringLiteral("Last run"), QStringLiteral("recent"));
    m_sortCombo->addItem(QStringLiteral("Next run"), QStringLiteral("next"));
    m_sortCombo->addItem(QStringLiteral("Kind"), QStringLiteral("kind"));
    bar->addWidget(m_sortCombo);
    auto *views = new QWidget;
    views->setObjectName(QStringLiteral("workflowsViewSwitch"));
    views->setAttribute(Qt::WA_StyledBackground);
    auto *viewsLayout = new QHBoxLayout(views);
    viewsLayout->setContentsMargins(2, 2, 2, 2);
    viewsLayout->setSpacing(2);
    const QList<QPair<ViewMode, QString>> modes{{ViewMode::List, QStringLiteral("List view")},
                                                {ViewMode::Thumbnails, QStringLiteral("Thumbnails view")},
                                                {ViewMode::Compact, QStringLiteral("Compact grid view")}};
    for (const auto &mode : modes) {
        auto *button = new QToolButton;
        button->setObjectName(QStringLiteral("workflowsViewButton"));
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setAccessibleName(mode.second);
        button->setToolTip(mode.second);
        button->setIconSize(QSize(16, 16));
        button->setProperty("viewMode", viewModeKey(mode.first));
        connect(button, &QToolButton::clicked, this, [this, value = mode.first] { setViewMode(value); });
        m_viewButtons.append(button);
        viewsLayout->addWidget(button);
    }
    bar->addWidget(views);
    layout->addLayout(bar);

    m_collection = new QScrollArea;
    m_collection->setObjectName(QStringLiteral("workflowsCollection"));
    m_collection->setAccessibleName(QStringLiteral("Workflows"));
    m_collection->setWidgetResizable(true);
    m_collection->setFrameShape(QFrame::NoFrame);
    m_collection->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *body = new QWidget;
    body->setObjectName(QStringLiteral("workflowsCollectionBody"));
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    m_grid = new CardGrid;
    bodyLayout->addWidget(m_grid);
    m_collectionEmpty = new QLabel;
    m_collectionEmpty->setObjectName(QStringLiteral("workflowsCollectionEmpty"));
    m_collectionEmpty->setAlignment(Qt::AlignCenter);
    m_collectionEmpty->setWordWrap(true);
    m_collectionEmpty->setVisible(false);
    bodyLayout->addWidget(m_collectionEmpty);
    bodyLayout->addStretch(1);
    m_collection->setWidget(body);
    m_collectionSplitter = new QSplitter(Qt::Horizontal);
    m_collectionSplitter->setChildrenCollapsible(false);
    m_collectionSplitter->addWidget(m_collection);
    m_collectionSplitter->addWidget(buildDetails());
    m_collectionSplitter->setStretchFactor(0, 1);
    m_collectionSplitter->setStretchFactor(1, 0);
    connect(m_collectionSplitter, &QSplitter::splitterMoved, this, [this] {
        if (m_details && !m_details->isHidden())
            m_detailsWidth = m_collectionSplitter->sizes().value(1);
    });
    layout->addWidget(m_collectionSplitter, 1);

    m_viewMode = viewModeFromKey(QSettings().value(QStringLiteral("workflows/viewMode")).toString());
    m_sort = QSettings().value(QStringLiteral("workflows/sort"), QStringLiteral("name")).toString();
    m_sortCombo->setCurrentIndex(std::max(0, m_sortCombo->findData(m_sort)));
    connect(m_sortCombo, &QComboBox::currentIndexChanged, this, [this] {
        m_sort = m_sortCombo->currentData().toString();
        QSettings().setValue(QStringLiteral("workflows/sort"), m_sort);
        renderCollection();
    });
    connect(m_filter, &QLineEdit::textChanged, this, [this] { renderCollection(); });
    return section;
}

void WorkflowsFeature::changeEvent(QEvent *event)
{
    forkmesh::market::NativePackageWidget::changeEvent(event);
    if (!m_theming && (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange))
        applyTheme();
}

void WorkflowsFeature::applyTheme()
{
    m_theming = true;
    m_tones = Tones::from(palette());
    const QString sheet = m_tones.sheet();
    if (sheet != m_appliedSheet) {
        m_appliedSheet = sheet;
        setStyleSheet(sheet);
        m_detailsShown = QJsonObject();
    }
    for (QToolButton *button : std::as_const(m_viewButtons)) {
        const ViewMode mode = viewModeFromKey(button->property("viewMode").toString());
        button->setChecked(mode == m_viewMode);
        button->setIcon(glyphIcon(viewModeKey(mode), mode == m_viewMode ? m_tones.accent : m_tones.muted, 16));
    }
    if (m_runFlow)
        m_runFlow->setTones(m_tones);
    m_theming = false;
    if (m_grid)
        renderCollection();
}

void WorkflowsFeature::setViewMode(ViewMode mode)
{
    m_viewMode = mode;
    QSettings().setValue(QStringLiteral("workflows/viewMode"), viewModeKey(mode));
    applyTheme();
}

void WorkflowsFeature::rebuildEntries()
{
    m_entries = unifiedWorkflows(m_workflows, m_runsByWorkflow, m_localWorkflows,
                                 m_local.value(QStringLiteral("pins")).toArray());
}

QJsonObject WorkflowsFeature::entryFor(const QString &key) const
{
    for (const QJsonValue &value : m_entries)
        if (text(value.toObject(), "key") == key)
            return value.toObject();
    return {};
}

QPixmap WorkflowsFeature::entryFace(const QJsonObject &entry) const
{
    if (text(entry, "source") != QLatin1String("local") || text(entry, "face").isEmpty())
        return {};
    QPixmap pixmap;
    pixmap.loadFromData(QByteArray::fromBase64(text(entry, "face").toLatin1()), "PNG");
    return pixmap;
}

void WorkflowsFeature::renderCollection()
{
    if (!m_grid)
        return;
    if (m_tones.window == QColor())
        m_tones = Tones::from(palette());
    QJsonArray shown;
    for (const QJsonValue &value : std::as_const(m_entries))
        if (workflowMatches(value.toObject(), m_filter->text(), m_scope))
            shown.append(value);
    sortWorkflows(shown, m_sort);
    m_collection->setUpdatesEnabled(false);
    QList<QWidget *> cards;
    WorkflowCard::Options options;
    options.mode = m_viewMode;
    options.busy = m_busy;
    options.canPin = localAvailable();
    for (const QJsonValue &value : std::as_const(shown)) {
        const QJsonObject entry = value.toObject();
        options.selected = text(entry, "key") == m_selectedKey;
        const QString key = text(entry, "key");
        cards.append(new WorkflowCard(entry, entryFace(entry), m_tones, options,
                                      [this, key](const QString &action, QWidget *anchor) {
            entryAction(key, action, anchor);
        }));
    }
    QWidget *header = nullptr;
    if (m_viewMode == ViewMode::List && !cards.isEmpty()) {
        header = new QWidget;
        auto *row = new QHBoxLayout(header);
        row->setContentsMargins(60, 0, 10, 0);
        row->setSpacing(12);
        const auto heading = [](const QString &value, int width) {
            auto *label = new QLabel(value);
            label->setObjectName(QStringLiteral("workflowListHeading"));
            if (width > 0)
                label->setFixedWidth(width);
            return label;
        };
        row->addWidget(heading(QStringLiteral("Workflow and latest output"), 0), 1);
        row->addWidget(heading(QStringLiteral("Trigger"), 150));
        row->addWidget(heading(QStringLiteral("State"), 130));
        row->addWidget(heading(QStringLiteral("Next or last"), 96));
        row->addSpacing(274);
    }
    m_grid->setCards(cards, m_viewMode, header);
    m_collection->setUpdatesEnabled(true);
    const bool none = m_entries.isEmpty();
    m_collectionEmpty->setVisible(cards.isEmpty() && (m_ready || !m_localWorkflows.isEmpty()));
    m_collectionEmpty->setText(none ? QStringLiteral("No workflows yet. Press New workflow to make a special agent, run a "
                                                     "shortcut on this desktop, or build one that runs on ForkMesh.")
                                    : QStringLiteral("No workflows match. Clear the filter or pick All."));
    int agents = 0;
    for (const QJsonValue &value : std::as_const(m_entries))
        agents += text(value.toObject(), "kind") == QLatin1String("agent") ? 1 : 0;
    m_collectionCount->setText(cards.size() == m_entries.size()
                                   ? QStringLiteral("%1 workflows · %2 special agents").arg(m_entries.size()).arg(agents)
                                   : QStringLiteral("%1 of %2 shown").arg(cards.size()).arg(m_entries.size()));
    renderDetails();
}

void WorkflowsFeature::entryAction(const QString &key, const QString &action, QWidget *anchor)
{
    const QJsonObject entry = entryFor(key);
    if (entry.isEmpty())
        return;
    const bool local = text(entry, "source") == QLatin1String("local");
    if (action == QLatin1String("select")) {
        selectEntry(key);
        return;
    }
    if (action == QLatin1String("edit")) {
        if (local)
            openLocalWorkflow(text(entry, "repo"), text(entry, "id"));
        else
            openWorkflow(text(entry, "id"));
        return;
    }
    if (action == QLatin1String("run")) {
        runEntry(entry);
        return;
    }
    if (action == QLatin1String("toggle")) {
        toggleEntry(entry);
        return;
    }
    if (action == QLatin1String("schedule")) {
        localAction(text(entry, "repo"), text(entry, "id"), QStringLiteral("setSchedule"),
                    {{QStringLiteral("active"), !entry.value(QStringLiteral("scheduleActive")).toBool()}},
                    [this](bool ok, const QJsonObject &, const QString &message) { setStatus(message, !ok); });
        return;
    }
    if (action == QLatin1String("pin")) {
        showPinMenu(entry, anchor ? anchor->mapToGlobal(QPoint(0, anchor->height())) : QCursor::pos());
        return;
    }
    if (action == QLatin1String("output")) {
        showEntryOutput(entry);
        return;
    }
    if (action == QLatin1String("menu")) {
        showEntryMenu(entry, anchor ? anchor->mapToGlobal(QPoint(0, anchor->height())) : QCursor::pos());
        return;
    }
    if (action == QLatin1String("menu-at"))
        showEntryMenu(entry, QCursor::pos());
}

void WorkflowsFeature::runEntry(const QJsonObject &entry, const QString &assignment)
{
    const bool running = entry.value(QStringLiteral("running")).toBool() ||
                         entry.value(QStringLiteral("agentWorking")).toBool();
    if (text(entry, "source") == QLatin1String("local")) {
        if (running) {
            localAction(text(entry, "repo"), text(entry, "id"), QStringLiteral("stop"), {},
                        [this](bool ok, const QJsonObject &, const QString &message) { setStatus(message, !ok); });
            return;
        }
        runLocal(entry, assignment);
        return;
    }
    runSummary(entry, assignment);
}

void WorkflowsFeature::askAssignment(const QJsonObject &entry)
{
    bool ok = false;
    const QString assignment = QInputDialog::getMultiLineText(
        this, QStringLiteral("Run %1").arg(text(entry, "name")),
        text(entry, "source") == QLatin1String("local")
            ? QStringLiteral("What should this run do?")
            : QStringLiteral("What should this run do? Its steps get it as {{ $trigger.assignment }}."),
        QString(), &ok);
    if (ok && !assignment.trimmed().isEmpty())
        runEntry(entry, assignment.trimmed());
}

void WorkflowsFeature::toggleEntry(const QJsonObject &entry)
{
    if (text(entry, "source") == QLatin1String("local")) {
        localAction(text(entry, "repo"), text(entry, "id"), QStringLiteral("setEnabled"),
                    {{QStringLiteral("enabled"), !entry.value(QStringLiteral("enabled")).toBool()}},
                    [this](bool ok, const QJsonObject &, const QString &message) { setStatus(message, !ok); });
        return;
    }
    toggleActive(QJsonObject{{QStringLiteral("id"), text(entry, "id")},
                             {QStringLiteral("active"), entry.value(QStringLiteral("enabled")).toBool()}});
}

void WorkflowsFeature::showEntryOutput(const QJsonObject &entry)
{
    if (text(entry, "source") == QLatin1String("local") && !text(entry, "lastOutputFile").isEmpty()) {
        localAction(text(entry, "repo"), text(entry, "id"), QStringLiteral("openOutput"),
                    {{QStringLiteral("path"), text(entry, "lastOutputFile")}}, {});
        return;
    }
    const QString runId = text(entry, "lastRunId");
    if (runId.isEmpty())
        return setStatus(QStringLiteral("%1 has not run yet.").arg(text(entry, "name")));
    const QString filtered = m_runWorkflowFilter->currentData().toString();
    if (!filtered.isEmpty() && filtered != runFilterKey(entry))
        m_runWorkflowFilter->setCurrentIndex(0);
    m_runStatusFilter->setCurrentIndex(0);
    for (int row = 0; row < m_runTable->rowCount(); ++row) {
        QTableWidgetItem *item = m_runTable->item(row, 0);
        if (item && item->data(Qt::UserRole + 1).toString() == runId) {
            m_runTable->selectRow(row);
            m_runTable->scrollToItem(item);
            return;
        }
    }
    setStatus(text(entry, "lastOutput").isEmpty() ? QStringLiteral("That run has no output.") : text(entry, "lastOutput"));
}

void WorkflowsFeature::deleteEntry(const QJsonObject &entry)
{
    if (text(entry, "source") != QLatin1String("local")) {
        if (!text(entry, "pinned").isEmpty())
            localAction(QString::fromLatin1(kAccountRepo), text(entry, "id"), QStringLiteral("unpin"), {},
                        [](bool, const QJsonObject &, const QString &) {});
        deleteWorkflow(text(entry, "id"), text(entry, "name"));
        return;
    }
    if (QMessageBox::warning(this, QStringLiteral("Delete workflow"),
                             QStringLiteral("Delete \"%1\"? This removes its file from %2%3.")
                                 .arg(text(entry, "name"), text(entry, "repo"),
                                      entry.value(QStringLiteral("specialAgent")).toBool()
                                          ? QStringLiteral(", and with it the special agent")
                                          : QString()),
                             QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    localAction(text(entry, "repo"), text(entry, "id"), QStringLiteral("delete"), {},
                [this](bool ok, const QJsonObject &, const QString &message) { setStatus(message, !ok); });
}

void WorkflowsFeature::addAgentStep(const QJsonObject &entry)
{
    m_pendingAgentStep = text(entry, "key");
    openLocalWorkflow(text(entry, "repo"), text(entry, "id"));
}

void WorkflowsFeature::showEntryMenu(const QJsonObject &entry, const QPoint &globalPos)
{
    const bool local = text(entry, "source") == QLatin1String("local");
    const bool running = entry.value(QStringLiteral("running")).toBool() ||
                         entry.value(QStringLiteral("agentWorking")).toBool();
    const bool enabled = entry.value(QStringLiteral("enabled")).toBool();
    QMenu menu(this);
    if (running) {
        connect(menu.addAction(QStringLiteral("Stop")), &QAction::triggered, this, [this, entry] { runEntry(entry); });
    } else {
        QAction *run = menu.addAction(QStringLiteral("Run now"));
        run->setEnabled(enabled && !(m_busy && !local));
        connect(run, &QAction::triggered, this, [this, entry] { runEntry(entry); });
        QAction *assigned = menu.addAction(QStringLiteral("Run with an assignment…"));
        assigned->setEnabled(enabled && !(m_busy && !local));
        connect(assigned, &QAction::triggered, this, [this, entry] { askAssignment(entry); });
    }
    connect(menu.addAction(QStringLiteral("Edit")), &QAction::triggered, this,
            [this, key = text(entry, "key")] { entryAction(key, QStringLiteral("edit"), nullptr); });
    QAction *output = menu.addAction(QStringLiteral("Show the last output"));
    output->setEnabled(!text(entry, "lastRunId").isEmpty() || !text(entry, "lastOutputFile").isEmpty());
    connect(output, &QAction::triggered, this, [this, entry] { showEntryOutput(entry); });
    if (local) {
        if (entry.value(QStringLiteral("specialAgent")).toBool())
            connect(menu.addAction(QStringLiteral("Agent home")), &QAction::triggered, this, [this, entry] {
                localAction(text(entry, "repo"), text(entry, "id"), QStringLiteral("openAgent"), {}, {});
            });
        else
            connect(menu.addAction(QStringLiteral("Add a special agent step…")), &QAction::triggered, this,
                    [this, entry] { addAgentStep(entry); });
        if (entry.value(QStringLiteral("hasSchedule")).toBool())
            connect(menu.addAction(entry.value(QStringLiteral("scheduleActive")).toBool()
                                       ? QStringLiteral("Pause schedule")
                                       : QStringLiteral("Start schedule")),
                    &QAction::triggered, this,
                    [this, key = text(entry, "key")] { entryAction(key, QStringLiteral("schedule"), nullptr); });
        connect(menu.addAction(QStringLiteral("Show file")), &QAction::triggered, this, [this, entry] {
            localAction(text(entry, "repo"), text(entry, "id"), QStringLiteral("openFile"), {}, {});
        });
    } else {
        QAction *copy = menu.addAction(QStringLiteral("Duplicate"));
        copy->setEnabled(!m_busy);
        connect(copy, &QAction::triggered, this, [this, entry] {
            duplicateWorkflow(QJsonObject{{QStringLiteral("id"), text(entry, "id")}});
        });
    }
    if (localAvailable()) {
        QMenu *pin = menu.addMenu(text(entry, "pinned").isEmpty()
                                      ? QStringLiteral("Pin to rail")
                                      : QStringLiteral("Pinned to the %1 rail").arg(text(entry, "pinned")));
        const QString current = text(entry, "pinned");
        for (const auto &rail : {QPair<QString, QString>{QStringLiteral("top"), QStringLiteral("Top rail")},
                                 {QStringLiteral("left"), QStringLiteral("Left rail")},
                                 {QStringLiteral("right"), QStringLiteral("Right rail")},
                                 {QStringLiteral("bottom"), QStringLiteral("Bottom rail")}}) {
            QAction *action = pin->addAction(rail.second);
            action->setCheckable(true);
            action->setChecked(current == rail.first);
            connect(action, &QAction::triggered, this, [this, entry, value = rail.first] {
                QJsonObject payload{{QStringLiteral("rail"), value}};
                if (text(entry, "source") != QLatin1String("local")) {
                    payload.insert(QStringLiteral("name"), text(entry, "name"));
                    payload.insert(QStringLiteral("lastStatus"), text(entry, "lastStatus"));
                }
                localAction(text(entry, "repo"), text(entry, "id"), QStringLiteral("pin"), payload,
                            [this](bool ok, const QJsonObject &, const QString &message) {
                    setStatus(message, !ok);
                    loadLocal();
                });
            });
        }
        if (!current.isEmpty()) {
            pin->addSeparator();
            connect(pin->addAction(QStringLiteral("Unpin")), &QAction::triggered, this, [this, entry] {
                localAction(text(entry, "repo"), text(entry, "id"), QStringLiteral("unpin"), {},
                            [this](bool ok, const QJsonObject &, const QString &message) {
                    setStatus(message, !ok);
                    loadLocal();
                });
            });
        }
    }
    menu.addSeparator();
    connect(menu.addAction(enabled ? QStringLiteral("Turn off") : QStringLiteral("Turn on")), &QAction::triggered, this,
            [this, entry] { toggleEntry(entry); });
    QAction *remove = menu.addAction(QStringLiteral("Delete…"));
    remove->setEnabled(!running && !(m_busy && !local));
    connect(remove, &QAction::triggered, this, [this, entry] { deleteEntry(entry); });
    menu.exec(globalPos);
}

QWidget *WorkflowsFeature::buildDetails()
{
    m_details = new QFrame;
    m_details->setObjectName(QStringLiteral("workflowsDetails"));
    m_details->setAccessibleName(QStringLiteral("Workflow details"));
    m_details->setMinimumWidth(340);
    auto *layout = new QVBoxLayout(m_details);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);
    m_detailsScroll = new QScrollArea;
    m_detailsScroll->setObjectName(QStringLiteral("workflowsDetailsScroll"));
    m_detailsScroll->setWidgetResizable(true);
    m_detailsScroll->setFrameShape(QFrame::NoFrame);
    m_detailsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(m_detailsScroll);
    m_details->setVisible(false);
    return m_details;
}

void WorkflowsFeature::selectEntry(const QString &key)
{
    const QJsonObject entry = entryFor(key);
    if (entry.isEmpty())
        return;
    const bool changed = key != m_selectedKey;
    m_selectedKey = key;
    for (WorkflowCard *card : m_grid->cards())
        card->setSelected(card->key() == key);
    renderDetails();
    const int index = m_runWorkflowFilter->findData(runFilterKey(entry));
    if (index < 0 || (!changed && m_runWorkflowFilter->currentIndex() == index))
        return;
    {
        const QSignalBlocker workflowBlock(m_runWorkflowFilter);
        const QSignalBlocker statusBlock(m_runStatusFilter);
        m_runWorkflowFilter->setCurrentIndex(index);
        m_runStatusFilter->setCurrentIndex(0);
    }
    renderRunTable();
    if (m_runTable->rowCount() == 0) {
        showSelectedRun();
    } else if (selectedDashboardRun().isEmpty()) {
        m_runTable->selectRow(0);
        m_runTable->scrollToItem(m_runTable->item(0, 0));
    }
}

void WorkflowsFeature::closeDetails()
{
    const QJsonObject entry = entryFor(m_selectedKey);
    m_selectedKey.clear();
    m_detailsShown = QJsonObject();
    for (WorkflowCard *card : m_grid->cards())
        card->setSelected(false);
    m_details->setVisible(false);
    if (!entry.isEmpty() && m_runWorkflowFilter->currentData().toString() == runFilterKey(entry))
        m_runWorkflowFilter->setCurrentIndex(0);
}

void WorkflowsFeature::renderDetails()
{
    if (!m_details)
        return;
    const QJsonObject entry = m_selectedKey.isEmpty() ? QJsonObject() : entryFor(m_selectedKey);
    if (entry.isEmpty()) {
        m_selectedKey.clear();
        m_detailsShown = QJsonObject();
        m_details->setVisible(false);
        return;
    }
    QJsonObject shown = entry;
    shown.insert(QStringLiteral("detailsBusy"), m_busy);
    shown.insert(QStringLiteral("detailsCanPin"), localAvailable());
    if (shown == m_detailsShown && !m_details->isHidden())
        return;
    const bool sameEntry = text(m_detailsShown, "key") == text(entry, "key");
    m_detailsShown = shown;

    const QString key = text(entry, "key");
    const bool local = text(entry, "source") == QLatin1String("local");
    const bool running = entry.value(QStringLiteral("running")).toBool() ||
                         entry.value(QStringLiteral("agentWorking")).toBool();
    const bool enabled = entry.value(QStringLiteral("enabled")).toBool();
    const bool locked = m_busy && !local;
    const qint64 current = now();

    auto *body = new QWidget;
    body->setObjectName(QStringLiteral("workflowsDetailsBody"));
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(16, 14, 12, 16);
    layout->setSpacing(10);
    const auto wrapped = [](const QString &value, const char *name) {
        auto *label = new QLabel(value);
        label->setObjectName(QLatin1String(name));
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setMinimumWidth(0);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        return label;
    };

    auto *head = new QHBoxLayout;
    head->setSpacing(10);
    auto *face = new QLabel;
    face->setPixmap(roundFace(entryFace(entry), text(entry, "kind"), m_tones, 40, devicePixelRatioF()));
    face->setFixedSize(40, 40);
    head->addWidget(face, 0, Qt::AlignTop);
    auto *names = new QVBoxLayout;
    names->setSpacing(2);
    names->addWidget(wrapped(text(entry, "name").isEmpty() ? QStringLiteral("Untitled workflow") : text(entry, "name"),
                             "workflowsDetailsName"));
    names->addWidget(wrapped(QStringLiteral("%1 · %2").arg(text(entry, "kindLabel"), text(entry, "place")),
                             "workflowsDetailsMeta"));
    head->addLayout(names, 1);
    auto *close = new QToolButton;
    close->setObjectName(QStringLiteral("workflowCardButton"));
    close->setIcon(glyphIcon(QStringLiteral("close"), m_tones.muted, 16));
    close->setIconSize(QSize(16, 16));
    close->setAutoRaise(true);
    close->setCursor(Qt::PointingHandCursor);
    close->setAccessibleName(QStringLiteral("Close details"));
    close->setToolTip(QStringLiteral("Close the details and show every workflow's runs"));
    connect(close, &QToolButton::clicked, this, [this] { closeDetails(); });
    head->addWidget(close, 0, Qt::AlignTop);
    layout->addLayout(head);

    auto *chips = new QHBoxLayout;
    chips->setSpacing(6);
    const auto chip = [chips](const QString &value, const QString &tone) {
        auto *label = new QLabel(value);
        label->setObjectName(QStringLiteral("workflowsFact"));
        label->setTextFormat(Qt::PlainText);
        label->setAccessibleName(value);
        if (!tone.isEmpty())
            label->setProperty("tone", tone);
        chips->addWidget(label);
    };
    chip(workflowStateText(entry), stateTone(entry));
    if (!text(entry, "pinned").isEmpty())
        chip(QStringLiteral("On the %1 rail").arg(text(entry, "pinned")), QString());
    if (entry.value(QStringLiteral("legacy")).toBool())
        chip(QStringLiteral("Old teammate file"), QStringLiteral("attention"));
    chips->addStretch(1);
    layout->addLayout(chips);

    if (!text(entry, "description").isEmpty())
        layout->addWidget(wrapped(text(entry, "description"), "workflowsDetailsText"));
    if (!text(entry, "error").isEmpty())
        layout->addWidget(wrapped(text(entry, "error"), "workflowsDetailsError"));

    const auto withEntry = [this, key](const std::function<void(const QJsonObject &)> &act) {
        return [this, key, act] {
            const QJsonObject found = entryFor(key);
            if (!found.isEmpty())
                act(found);
        };
    };

    auto *runRow = new QHBoxLayout;
    runRow->setSpacing(6);
    auto *run = new QToolButton;
    run->setObjectName(QStringLiteral("workflowCardRun"));
    run->setCursor(Qt::PointingHandCursor);
    run->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    run->setText(running ? QStringLiteral("Stop") : QStringLiteral("Run now"));
    run->setAccessibleName(run->text());
    run->setProperty("stopping", running);
    run->setIcon(glyphIcon(running ? QStringLiteral("stop") : QStringLiteral("play"),
                           running ? m_tones.danger : m_tones.accent, 14));
    run->setIconSize(QSize(14, 14));
    run->setToolTip(running ? QStringLiteral("Stop this run")
                            : enabled ? QStringLiteral("Run it now and follow its log below")
                                      : QStringLiteral("Turn it on to run it"));
    run->setEnabled(running || (enabled && !locked));
    connect(run, &QToolButton::clicked, this, withEntry([this](const QJsonObject &found) { runEntry(found); }));
    runRow->addWidget(run);
    auto *assigned = button(QStringLiteral("Run with an assignment…"));
    assigned->setEnabled(!running && enabled && !locked);
    connect(assigned, &QPushButton::clicked, this,
            withEntry([this](const QJsonObject &found) { askAssignment(found); }));
    runRow->addWidget(assigned);
    runRow->addStretch(1);
    layout->addLayout(runRow);

    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(6);
    const auto act = [this, grid](const QString &label, const QString &tip, bool allowed, const char *style = "ghostButton") {
        auto *item = button(label, style);
        item->setToolTip(tip);
        item->setEnabled(allowed);
        const int slot = grid->count();
        grid->addWidget(item, slot / 2, slot % 2);
        return item;
    };
    connect(act(QStringLiteral("Edit steps"), QStringLiteral("Open it in the editor"), true), &QPushButton::clicked,
            this, [this, key] { entryAction(key, QStringLiteral("edit"), nullptr); });
    connect(act(enabled ? QStringLiteral("Turn off") : QStringLiteral("Turn on"),
                enabled ? QStringLiteral("Stop it running by hand or on its schedule")
                        : QStringLiteral("Let it run by hand and on its schedule"),
                !locked),
            &QPushButton::clicked, this, withEntry([this](const QJsonObject &found) { toggleEntry(found); }));
    if (local && entry.value(QStringLiteral("hasSchedule")).toBool()) {
        const bool active = entry.value(QStringLiteral("scheduleActive")).toBool();
        connect(act(active ? QStringLiteral("Pause schedule") : QStringLiteral("Start schedule"), text(entry, "trigger"),
                    true),
                &QPushButton::clicked, this, [this, key] { entryAction(key, QStringLiteral("schedule"), nullptr); });
    }
    if (localAvailable()) {
        QPushButton *pin = act(text(entry, "pinned").isEmpty() ? QStringLiteral("Pin to rail…")
                                                                : QStringLiteral("Move or unpin…"),
                               QStringLiteral("Put a run button on a rail"), true);
        connect(pin, &QPushButton::clicked, this, [this, key, pin] { entryAction(key, QStringLiteral("pin"), pin); });
    }
    if (local && !text(entry, "lastOutputFile").isEmpty())
        connect(act(QStringLiteral("Open output file"), text(entry, "lastOutputFile"), true), &QPushButton::clicked,
                this, withEntry([this](const QJsonObject &found) { showEntryOutput(found); }));
    if (local) {
        if (entry.value(QStringLiteral("specialAgent")).toBool())
            connect(act(QStringLiteral("Agent home"), QStringLiteral("Open the special agent"), true),
                    &QPushButton::clicked, this, withEntry([this](const QJsonObject &found) {
                localAction(text(found, "repo"), text(found, "id"), QStringLiteral("openAgent"), {}, {});
            }));
        else
            connect(act(QStringLiteral("Add an agent step…"), QStringLiteral("Add a special agent step to it"), true),
                    &QPushButton::clicked, this, withEntry([this](const QJsonObject &found) { addAgentStep(found); }));
        connect(act(QStringLiteral("Show file"), text(entry, "path"), true), &QPushButton::clicked, this,
                withEntry([this](const QJsonObject &found) {
            localAction(text(found, "repo"), text(found, "id"), QStringLiteral("openFile"), {}, {});
        }));
    } else {
        connect(act(QStringLiteral("Duplicate"), QStringLiteral("Make a copy that starts turned off"), !m_busy),
                &QPushButton::clicked, this, withEntry([this](const QJsonObject &found) {
            duplicateWorkflow(QJsonObject{{QStringLiteral("id"), text(found, "id")}});
        }));
        connect(act(QStringLiteral("Open in browser"), QStringLiteral("Open it on the web dashboard's canvas"), true),
                &QPushButton::clicked, this, [id = text(entry, "id")] {
            QDesktopServices::openUrl(
                QUrl(QString::fromLatin1(kOrigin) + QStringLiteral("/dashboard/workflows?id=") + id));
        });
    }
    connect(act(QStringLiteral("Delete…"), QStringLiteral("Delete it"), !running && !locked, "dangerButton"),
            &QPushButton::clicked, this, withEntry([this](const QJsonObject &found) { deleteEntry(found); }));
    layout->addLayout(grid);

    auto *factsTitle = new QLabel(QStringLiteral("Details"));
    factsTitle->setObjectName(QStringLiteral("workflowsPaneTitle"));
    layout->addSpacing(4);
    layout->addWidget(factsTitle);
    auto *form = new QFormLayout;
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(6);
    form->setLabelAlignment(Qt::AlignLeft);
    form->setRowWrapPolicy(QFormLayout::DontWrapRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const auto fact = [&form, &wrapped](const QString &name, const QString &value, const QString &tip = QString()) {
        auto *label = new QLabel(name);
        label->setObjectName(QStringLiteral("workflowsDetailsKey"));
        QLabel *field = wrapped(value, "workflowsDetailsValue");
        field->setToolTip(tip);
        form->addRow(label, field);
    };
    fact(QStringLiteral("Trigger"), text(entry, "trigger").isEmpty() ? QStringLiteral("By hand") : text(entry, "trigger"));
    if (entry.value(QStringLiteral("hasSchedule")).toBool())
        fact(QStringLiteral("Schedule"),
             entry.value(QStringLiteral("scheduleActive")).toBool() ? QStringLiteral("On") : QStringLiteral("Paused"));
    const qint64 next = integer(entry, "nextRunAt");
    fact(QStringLiteral("Next run"), next > 0 ? relativeTime(next, current) : QStringLiteral("None"),
         next > 0 ? timeText(next) : QString());
    const qint64 last = integer(entry, "lastRunAt");
    const QString lastStatus = text(entry, "lastStatus");
    QString lastText = last > 0 ? relativeTime(last, current) : QStringLiteral("Never run");
    if (last > 0 && !lastStatus.isEmpty())
        lastText += QStringLiteral(" · ") + (local ? localStatusLabel(lastStatus) : statusLabel(lastStatus));
    fact(QStringLiteral("Last run"), lastText, last > 0 ? timeText(last) : QString());
    if (running && !text(entry, "runningStep").isEmpty())
        fact(QStringLiteral("Running"), text(entry, "runningStep"),
             integer(entry, "runningSince") > 0 ? QStringLiteral("Since %1").arg(timeText(integer(entry, "runningSince")))
                                                : QString());
    const int runs = entry.value(QStringLiteral("runs")).toInt();
    if (runs > 0) {
        const int failed = entry.value(QStringLiteral("failedRuns")).toInt();
        fact(QStringLiteral("Runs"), failed ? QStringLiteral("%1, %2 failed").arg(runs).arg(failed)
                                            : QStringLiteral("%1, none failed").arg(runs));
    }
    const int steps = entry.value(QStringLiteral("steps")).toInt();
    fact(QStringLiteral("Steps"), QString::number(steps));
    fact(QStringLiteral("Where"), text(entry, "place"));
    if (localAvailable())
        fact(QStringLiteral("Rail"), text(entry, "pinned").isEmpty() ? QStringLiteral("Not pinned")
                                                                     : QStringLiteral("%1 rail").arg(text(entry, "pinned")));
    if (local && !text(entry, "path").isEmpty())
        fact(QStringLiteral("File"), QFileInfo(text(entry, "path")).fileName(), text(entry, "path"));
    layout->addLayout(form);

    auto *outputTitle = new QLabel(QStringLiteral("Latest output"));
    outputTitle->setObjectName(QStringLiteral("workflowsPaneTitle"));
    layout->addSpacing(4);
    layout->addWidget(outputTitle);
    QLabel *output = wrapped(text(entry, "lastOutput").isEmpty() ? QStringLiteral("No output yet. Run it to see what it makes.")
                                                                 : text(entry, "lastOutput"),
                             "workflowsDetailsOutput");
    output->setAccessibleName(QStringLiteral("Latest output"));
    layout->addWidget(output);
    QLabel *note = mutedLabel(QStringLiteral("Its runs are listed below. The selected run's full log shows at the "
                                             "bottom right."));
    note->setObjectName(QStringLiteral("workflowsDetailsMeta"));
    layout->addWidget(note);
    layout->addStretch(1);

    QScrollBar *bar = m_detailsScroll->verticalScrollBar();
    const int scrolled = sameEntry ? bar->value() : 0;
    if (QWidget *old = m_detailsScroll->takeWidget())
        old->deleteLater();
    m_detailsScroll->setWidget(body);
    if (scrolled > 0)
        QTimer::singleShot(0, m_detailsScroll, [bar = QPointer<QScrollBar>(bar), scrolled] {
            if (bar)
                bar->setValue(scrolled);
        });
    if (m_details->isHidden()) {
        m_details->setVisible(true);
        const QList<int> sizes = m_collectionSplitter->sizes();
        const int total = sizes.value(0) + sizes.value(1);
        const int width = m_detailsWidth > 0 ? m_detailsWidth : std::clamp(total / 3, 360, 440);
        if (total > width + 300)
            m_collectionSplitter->setSizes({total - width, width});
    }
}

void WorkflowsFeature::syncAccountPins()
{
    if (!localAvailable() || !m_ready)
        return;
    QJsonArray rows;
    for (const QJsonValue &value : std::as_const(m_workflows)) {
        const QJsonObject summary = value.toObject();
        rows.append(QJsonObject{{QStringLiteral("id"), text(summary, "id")},
                                {QStringLiteral("name"), text(summary, "name")},
                                {QStringLiteral("lastStatus"), text(summary, "lastStatus")}});
    }
    localAction(QString::fromLatin1(kAccountRepo), QString(), QStringLiteral("sync"),
                {{QStringLiteral("workflows"), rows}}, [](bool, const QJsonObject &, const QString &) {});
}

QWidget *WorkflowsFeature::buildEditor()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto *bar = new QHBoxLayout;
    bar->setSpacing(6);
    auto *back = button(QStringLiteral("‹ All workflows"));
    bar->addWidget(back);
    m_name = new QLineEdit;
    m_name->setAccessibleName(QStringLiteral("Workflow name"));
    m_name->setPlaceholderText(QStringLiteral("Workflow name"));
    m_name->setMaxLength(80);
    m_name->setMinimumWidth(220);
    bar->addWidget(m_name, 1);
    m_active = new QCheckBox(QStringLiteral("Active"));
    m_active->setToolTip(QStringLiteral("Active workflows answer their webhook and run on their schedule"));
    bar->addWidget(m_active);
    m_saveButton = button(QStringLiteral("Save"), "primaryButton");
    m_runDraftButton = button(QStringLiteral("Run workflow"));
    m_exportButton = button(QStringLiteral("Download"));
    m_exportButton->setToolTip(QStringLiteral("Save as an n8n-compatible JSON file"));
    m_deleteDraftButton = button(QStringLiteral("Delete"), "dangerButton");
    for (QPushButton *item : {m_saveButton, m_runDraftButton, m_exportButton, m_deleteDraftButton})
        bar->addWidget(item);
    layout->addLayout(bar);

    auto *localBar = new QWidget;
    localBar->setObjectName(QStringLiteral("workflowsLocalBar"));
    auto *localRow = new QHBoxLayout(localBar);
    localRow->setContentsMargins(0, 0, 0, 0);
    localRow->setSpacing(6);
    m_iconLabel = new QLabel(QStringLiteral("Icon"));
    localRow->addWidget(m_iconLabel);
    m_iconCombo = new QComboBox;
    m_iconCombo->setAccessibleName(QStringLiteral("Workflow icon"));
    m_iconCombo->setIconSize(QSize(20, 20));
    localRow->addWidget(m_iconCombo);
    m_description = new QLineEdit;
    m_description->setAccessibleName(QStringLiteral("Workflow description"));
    m_description->setPlaceholderText(QStringLiteral("What this workflow does"));
    m_description->setMaxLength(1000);
    localRow->addWidget(m_description, 1);
    m_editorAgentButton = button(QStringLiteral("Agent home"));
    m_editorAgentButton->setToolTip(QStringLiteral("Open the special agent's page: its conversation, notes and widgets"));
    m_editorPinButton = button(QStringLiteral("Pin to rail"));
    m_editorPinButton->setToolTip(QStringLiteral("Put a button for this workflow on the top, left, right or bottom rail"));
    localRow->addWidget(m_editorAgentButton);
    localRow->addWidget(m_editorPinButton);
    localBar->setVisible(false);
    layout->addWidget(localBar);

    m_editorFacts = new QWidget;
    m_editorFacts->setObjectName(QStringLiteral("workflowsEditorFacts"));
    auto *facts = new QHBoxLayout(m_editorFacts);
    facts->setContentsMargins(0, 0, 0, 0);
    facts->setSpacing(6);
    layout->addWidget(m_editorFacts);

    m_editorNote = mutedLabel(QString());
    m_editorNote->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    m_editorNote->setOpenExternalLinks(false);
    layout->addWidget(m_editorNote);

    auto *horizontal = new QSplitter(Qt::Horizontal);
    horizontal->setChildrenCollapsible(false);

    auto *left = new QWidget;
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(4);
    auto *stepsTitle = new QLabel(QStringLiteral("STEPS"));
    stepsTitle->setObjectName(QStringLiteral("workflowsPaneTitle"));
    leftLayout->addWidget(stepsTitle);
    m_steps = new QListWidget;
    m_steps->setObjectName(QStringLiteral("workflowsSteps"));
    m_steps->setAccessibleName(QStringLiteral("Steps in run order"));
    m_steps->setItemDelegate(new StepDelegate(&m_tones, m_steps));
    m_steps->setMouseTracking(true);
    m_steps->viewport()->setAttribute(Qt::WA_Hover);
    m_steps->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_steps->setUniformItemSizes(false);
    m_steps->setMinimumWidth(220);
    leftLayout->addWidget(m_steps, 1);
    auto *stepButtons = new QHBoxLayout;
    stepButtons->setSpacing(4);
    m_addStepButton = button(QStringLiteral("Add step…"));
    m_addStepButton->setToolTip(QStringLiteral("Add a step after the selected one"));
    m_removeStepButton = button(QStringLiteral("Remove step"), "dangerButton");
    m_removeStepButton->setToolTip(QStringLiteral("Remove the selected step and join its inputs to its outputs"));
    stepButtons->addWidget(m_addStepButton);
    stepButtons->addStretch();
    stepButtons->addWidget(m_removeStepButton);
    leftLayout->addLayout(stepButtons);
    horizontal->addWidget(left);

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_formHost = new QWidget;
    new QVBoxLayout(m_formHost);
    scroll->setWidget(m_formHost);
    scroll->setMinimumWidth(340);
    horizontal->addWidget(scroll);

    auto *runs = new QWidget;
    auto *runsLayout = new QVBoxLayout(runs);
    runsLayout->setContentsMargins(0, 0, 0, 0);
    runsLayout->setSpacing(4);
    auto *runsTitle = new QLabel(QStringLiteral("RUNS OF THIS WORKFLOW"));
    runsTitle->setObjectName(QStringLiteral("workflowsPaneTitle"));
    runsLayout->addWidget(runsTitle);
    auto *runSplitter = new QSplitter(Qt::Vertical);
    runSplitter->setChildrenCollapsible(false);
    m_editorRunTable = makeTable({QStringLiteral("Started"), QStringLiteral("Result"), QStringLiteral("Duration"),
                                  QStringLiteral("Trigger"), QStringLiteral("Output")},
                                 QStringLiteral("workflowsEditorRuns"));
    m_editorRunTable->setAccessibleName(QStringLiteral("Runs of this workflow"));
    m_editorRunTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_editorRunTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Interactive);
    m_editorRunTable->horizontalHeader()->setStretchLastSection(true);
    m_editorRunTable->setColumnWidth(3, 160);
    m_editorRunTable->verticalHeader()->setDefaultSectionSize(24);
    runSplitter->addWidget(m_editorRunTable);
    auto *detail = new QWidget;
    auto *detailLayout = new QVBoxLayout(detail);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    detailLayout->setSpacing(4);
    m_editorRunSummary = mutedLabel(QStringLiteral("Select a run to see what each step did."));
    m_editorRunSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detailLayout->addWidget(m_editorRunSummary);
    m_editorRunSteps = makeTable({QStringLiteral("Step"), QStringLiteral("Result"), QStringLiteral("Time"),
                                  QStringLiteral("Items"), QStringLiteral("Details")},
                                 QStringLiteral("workflowsEditorRunSteps"));
    m_editorRunSteps->setSelectionMode(QAbstractItemView::NoSelection);
    m_editorRunSteps->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_editorRunSteps->horizontalHeader()->setStretchLastSection(true);
    m_editorRunSteps->verticalHeader()->setDefaultSectionSize(24);
    detailLayout->addWidget(m_editorRunSteps, 1);
    runSplitter->addWidget(detail);
    runSplitter->setStretchFactor(0, 2);
    runSplitter->setStretchFactor(1, 3);
    runsLayout->addWidget(runSplitter, 1);
    runs->setMinimumWidth(320);
    horizontal->addWidget(runs);
    horizontal->setStretchFactor(0, 0);
    horizontal->setStretchFactor(1, 1);
    horizontal->setStretchFactor(2, 1);
    horizontal->setSizes({260, 560, 520});
    layout->addWidget(horizontal, 1);

    connect(back, &QPushButton::clicked, this, [this] {
        if (leaveEditor())
            showDashboard();
    });
    connect(m_name, &QLineEdit::textEdited, this, [this](const QString &value) {
        m_draft.name = value.trimmed().isEmpty() ? QStringLiteral("Untitled workflow") : value.trimmed();
        setDirty(true);
    });
    connect(m_active, &QCheckBox::toggled, this, [this](bool on) {
        if (m_draft.active == on)
            return;
        m_draft.active = on;
        setDirty(true);
    });
    connect(m_iconCombo, &QComboBox::currentIndexChanged, this, [this] {
        const QString chosen = m_iconCombo->currentData().toString();
        if (!m_draft.isLocal() || chosen == m_draft.icon)
            return;
        m_draft.icon = chosen;
        setDirty(true);
    });
    connect(m_description, &QLineEdit::textEdited, this, [this](const QString &value) {
        m_draft.description = value.trimmed();
        setDirty(true);
    });
    connect(m_editorAgentButton, &QPushButton::clicked, this, [this] {
        if (m_draft.isLocal() && !m_draft.id.isEmpty())
            localAction(m_draft.repo, m_draft.id, QStringLiteral("openAgent"), {}, {});
    });
    connect(m_editorPinButton, &QPushButton::clicked, this, [this] {
        if (m_draft.isLocal() && !m_draft.id.isEmpty())
            showPinMenu(localSummary(m_draft.repo, m_draft.id),
                        m_editorPinButton->mapToGlobal(QPoint(0, m_editorPinButton->height())));
    });
    connect(m_saveButton, &QPushButton::clicked, this, [this] { saveDraft(); });
    connect(m_runDraftButton, &QPushButton::clicked, this, [this] { runDraft(); });
    connect(m_exportButton, &QPushButton::clicked, this, [this] { exportDraft(); });
    connect(m_deleteDraftButton, &QPushButton::clicked, this, [this] { deleteDraft(); });
    connect(m_editorNote, &QLabel::linkActivated, this, [this](const QString &link) {
        if (link == QLatin1String("copy-hook")) {
            QApplication::clipboard()->setText(QString::fromLatin1(kOrigin) + m_draft.hookPath);
            setStatus(QStringLiteral("Webhook URL copied."));
        } else if (link == QLatin1String("rotate-hook")) {
            rotateHook();
        }
    });
    connect(m_steps, &QListWidget::currentRowChanged, this, [this] {
        const QString id = selectedStepId();
        if (id == m_selectedStep)
            return;
        m_selectedStep = id;
        m_removeStepButton->setEnabled(!id.isEmpty());
        renderStepForm();
    });
    connect(m_addStepButton, &QPushButton::clicked, this, [this] { addStepDialog(); });
    connect(m_removeStepButton, &QPushButton::clicked, this, [this] {
        const QString id = selectedStepId();
        if (id.isEmpty())
            return;
        const QString name = text(m_draft.node(id), "name");
        if (QMessageBox::question(this, QStringLiteral("Remove step"),
                                  QStringLiteral("Remove \"%1\"? Its inputs are joined to the steps after it.").arg(name))
            != QMessageBox::Yes)
            return;
        const QStringList order = m_draft.ordered();
        const int position = int(order.indexOf(id));
        m_draft.remove(id);
        const QStringList left = m_draft.ordered();
        m_selectedStep = left.isEmpty() ? QString() : left.at(std::clamp(position - 1, 0, int(left.size()) - 1));
        setDirty(true);
        renderStepList();
        renderStepForm();
    });
    connect(m_editorRunTable, &QTableWidget::itemSelectionChanged, this, [this] {
        const int row = m_editorRunTable->currentRow();
        if (row < 0 || row >= m_editorRuns.size())
            return;
        const QJsonObject run = m_editorRuns.at(row).toObject();
        if (run.value(QStringLiteral("local")).toBool()) {
            showLocalRunDetail(m_editorRunSteps, m_editorRunSummary, run);
            return;
        }
        showRunDetail(m_editorRunSteps, m_editorRunSummary, m_draft.id, text(run, "id"));
    });
    return page;
}

void WorkflowsFeature::request(const QByteArray &method, const QString &target, const QJsonObject &body,
                               ReplyHandler handler)
{
    if (!m_request) {
        handler(false, {}, QStringLiteral("Workflows network access is unavailable."));
        return;
    }
    const QPointer<WorkflowsFeature> guard(this);
    m_request(method, target, body,
              [guard, handler = std::move(handler)](bool ok, const QJsonObject &payload, const QString &error) mutable {
        if (guard)
            handler(ok, payload, error);
    });
}

void WorkflowsFeature::setStatus(const QString &value, bool error)
{
    m_status->setText(value);
    m_status->setObjectName(error ? QStringLiteral("errorLabel") : QStringLiteral("mutedLabel"));
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
}

QString WorkflowsFeature::failureText(const QJsonObject &payload, const QString &error) const
{
    return errorText(text(payload, "error"), error);
}

void WorkflowsFeature::refresh()
{
    loadLocal();
    if (m_loading)
        return;
    m_loading = true;
    if (!m_ready)
        setStatus(QStringLiteral("Loading workflows…"));
    loadCatalog([this] { loadWorkflows(); });
}

void WorkflowsFeature::loadCatalog(std::function<void()> done)
{
    if (m_catalogLoaded) {
        done();
        return;
    }
    request(QByteArrayLiteral("GET"), path(QStringLiteral("/catalog")), {},
            [this, done = std::move(done)](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok) {
            m_loading = false;
            setStatus(QStringLiteral("The step catalog did not load: %1").arg(failureText(payload, error)), true);
            return;
        }
        m_catalog.load(payload);
        m_catalogLoaded = true;
        done();
    });
}

void WorkflowsFeature::loadWorkflows()
{
    request(QByteArrayLiteral("GET"), path(), {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        m_loading = false;
        if (!ok) {
            setStatus(failureText(payload, error), true);
            return;
        }
        m_workflows = payload.value(QStringLiteral("workflows")).toArray();
        m_ready = true;
        if (!m_busy)
            setStatus(m_workflows.isEmpty() && m_localWorkflows.isEmpty()
                          ? QStringLiteral("No workflows yet.")
                          : QStringLiteral("%1 workflows, updated %2.")
                                .arg(m_workflows.size() + m_localWorkflows.size())
                                .arg(QLocale().toString(QTime::currentTime(), QLocale::ShortFormat)));
        rebuildRunFilter();
        renderDashboard();
        if (m_searchIndexChanged)
            m_searchIndexChanged();
        if (m_navigationBadgeChanged)
            m_navigationBadgeChanged();
        loadAllRuns();
        syncAccountPins();
    });
}

void WorkflowsFeature::loadAllRuns()
{
    ++m_serial;
    m_runsInFlight = 0;
    m_runQueue.clear();
    QSet<QString> present;
    for (const QJsonValue &value : std::as_const(m_workflows)) {
        const QJsonObject summary = value.toObject();
        present.insert(text(summary, "id"));
        if (integer(summary, "lastRunAt") > 0)
            m_runQueue << text(summary, "id");
    }
    for (auto it = m_runsByWorkflow.begin(); it != m_runsByWorkflow.end();) {
        if (!present.contains(it.key()))
            it = m_runsByWorkflow.erase(it);
        else
            ++it;
    }
    if (m_runQueue.isEmpty()) {
        renderDashboard();
        return;
    }
    pumpRunQueue();
}

void WorkflowsFeature::pumpRunQueue()
{
    const int serial = m_serial;
    while (m_runsInFlight < kRunRequests && !m_runQueue.isEmpty()) {
        const QString id = m_runQueue.takeFirst();
        ++m_runsInFlight;
        request(QByteArrayLiteral("GET"), path(QStringLiteral("/%1/runs").arg(id)), {},
                [this, serial, id](bool ok, const QJsonObject &payload, const QString &) {
            if (serial != m_serial)
                return;
            --m_runsInFlight;
            if (ok)
                m_runsByWorkflow.insert(id, payload.value(QStringLiteral("runs")).toArray());
            if (m_runQueue.isEmpty() && m_runsInFlight == 0)
                renderDashboard();
            else
                pumpRunQueue();
        });
    }
}

void WorkflowsFeature::renderDashboard()
{
    QHash<QString, QString> names;
    for (const QJsonValue &value : std::as_const(m_workflows))
        names.insert(text(value.toObject(), "id"), text(value.toObject(), "name"));
    m_allRuns = mergeRuns(m_runsByWorkflow, names, 0);
    if (!m_localRuns.isEmpty()) {
        QList<QJsonObject> all;
        for (const QJsonValue &value : std::as_const(m_allRuns))
            all.append(value.toObject());
        for (const QJsonValue &value : std::as_const(m_localRuns))
            all.append(value.toObject());
        std::stable_sort(all.begin(), all.end(), [](const QJsonObject &a, const QJsonObject &b) {
            return integer(a, "startedAt") > integer(b, "startedAt");
        });
        m_allRuns = QJsonArray();
        for (const QJsonObject &run : all)
            m_allRuns.append(run);
    }
    rebuildEntries();
    const Stats stats = computeStats(statsWorkflows(m_entries), m_allRuns, now());
    const qint64 current = now();
    const QStringList values{
        QString::number(stats.workflows),
        QString::number(stats.active),
        QString::number(stats.runs),
        successRateText(stats),
        stats.runs ? durationText(stats.averageMs) : QStringLiteral("–"),
        relativeTime(stats.lastRunAt, current),
        stats.nextRunAt ? relativeTime(stats.nextRunAt, current) : QStringLiteral("None"),
    };
    const QStringList notes{
        QStringLiteral("%1 steps in all").arg(stats.steps),
        QStringLiteral("%1 scheduled · %2 webhook · %3 manual").arg(stats.scheduled).arg(stats.webhooks).arg(stats.manual),
        QStringLiteral("%1 today · last 30 kept each").arg(stats.runsToday),
        QStringLiteral("%1 succeeded · %2 failed").arg(stats.succeeded).arg(stats.failed),
        QStringLiteral("per run"),
        stats.lastFailedWorkflow.isEmpty() ? QString()
                                           : QStringLiteral("Last failure: %1").arg(stats.lastFailedWorkflow),
        stats.nextRunAt ? timeText(stats.nextRunAt) : QStringLiteral("No active schedule"),
    };
    for (int index = 0; index < m_kpiValues.size(); ++index) {
        m_kpiValues.at(index)->setText(values.value(index));
        m_kpiNotes.at(index)->setText(notes.value(index));
    }
    if (!m_pendingRun.isEmpty()) {
        const QString landed = text(m_pendingRun, "runId");
        bool replaced = m_pendingRun.value(QStringLiteral("pending")).toBool() && !landed.isEmpty() &&
                        current - integer(m_pendingRun, "startedAt") > 120000;
        for (const QJsonValue &value : std::as_const(m_allRuns))
            replaced = replaced || (!landed.isEmpty() && text(value.toObject(), "id") == landed);
        if (replaced)
            m_pendingRun = QJsonObject();
        else
            m_allRuns.prepend(m_pendingRun);
    }
    m_newButton->setEnabled(m_workflows.size() < kMaxWorkflows);
    m_newButton->setToolTip(m_workflows.size() < kMaxWorkflows
                                ? QStringLiteral("A special agent, a desktop workflow, or one that runs on ForkMesh")
                                : QStringLiteral("You have 50 workflows on ForkMesh, the most an account keeps"));
    renderCollection();
    renderRunTable();
}

void WorkflowsFeature::renderRunTable()
{
    const QJsonObject chosen = selectedDashboardRun();
    const QString workflow = m_runWorkflowFilter->currentData().toString();
    const QString status = m_runStatusFilter->currentData().toString();
    const qint64 current = now();
    int followRow = -1;
    bool followed = false;
    QSignalBlocker block(m_runTable);
    m_runTable->setRowCount(0);
    for (const QJsonValue &value : std::as_const(m_allRuns)) {
        const QJsonObject run = value.toObject();
        if (!workflow.isEmpty() && text(run, "workflowId") != workflow)
            continue;
        if (!status.isEmpty() && text(run, "status") != status)
            continue;
        if (!m_hostQuery.isEmpty() && !text(run, "workflowName").contains(m_hostQuery, Qt::CaseInsensitive))
            continue;
        if (m_runTable->rowCount() >= kRunsShown)
            break;
        const int row = m_runTable->rowCount();
        m_runTable->insertRow(row);
        auto *nameItem = cell(text(run, "workflowName"));
        nameItem->setData(Qt::UserRole, text(run, "workflowId"));
        nameItem->setData(Qt::UserRole + 1, text(run, "id"));
        m_runTable->setItem(row, 0, nameItem);
        const qint64 started = integer(run, "startedAt");
        m_runTable->setItem(row, 1, cell(relativeTime(started, current), timeText(started)));
        m_runTable->setItem(row, 2, cell(text(run, "trigger")));
        m_runTable->setItem(row, 3, statusCell(text(run, "status")));
        const qint64 finished = integer(run, "finishedAt");
        m_runTable->setItem(row, 4, cell(finished >= started && started > 0 ? durationText(finished - started)
                                                                            : QStringLiteral("–")));
        m_runTable->setItem(row, 5, cell(runOutputText(run)));
        if (text(run, "id") == text(chosen, "id"))
            m_runTable->selectRow(row);
        if (followRow < 0 && !m_followWorkflow.isEmpty() && text(run, "workflowId") == m_followWorkflow &&
            (m_followRun.isEmpty() ? started >= m_followSince && started <= m_followSince + 120000
                                  : text(run, "id") == m_followRun)) {
            followRow = row;
            followed = !run.value(QStringLiteral("pending")).toBool();
        }
    }
    block.unblock();
    if (followed)
        stopFollowing();
    if (followRow >= 0) {
        m_runTable->selectRow(followRow);
        m_runTable->scrollToItem(m_runTable->item(followRow, 0));
        if (!m_sparkled) {
            m_sparkled = true;
            RunSparkle::burst(m_runTable, m_runTable->item(followRow, 0)->data(Qt::UserRole + 1).toString(), m_tones);
        }
    }
    const QJsonObject shown = selectedDashboardRun();
    if (text(shown, "id") != m_shownRunId || text(shown, "status") != m_shownStatus)
        showSelectedRun();
}

void WorkflowsFeature::showSelectedRun()
{
    const QJsonObject run = selectedDashboardRun();
    const bool fresh = text(run, "id") != m_shownRunId;
    m_shownRunId = text(run, "id");
    m_shownStatus = text(run, "status");
    if (run.isEmpty() || !run.value(QStringLiteral("local")).toBool() || !liveStatus(m_shownStatus))
        m_liveTimer->stop();
    if (run.isEmpty()) {
        m_runFlow->clear();
        m_runFlowArea->hide();
        const QJsonObject entry = entryFor(m_selectedKey);
        m_runSteps->setRowCount(0);
        m_runOutput->clear();
        m_runSummary->setTextFormat(Qt::PlainText);
        m_runSummary->setText(!entry.isEmpty() && m_runTable->rowCount() == 0 &&
                                      m_runWorkflowFilter->currentData().toString() == runFilterKey(entry)
                                  ? QStringLiteral("%1 has not run yet. Run it to see its log here.")
                                        .arg(text(entry, "name"))
                                  : QStringLiteral("Select a run to see what each step did."));
        return;
    }
    if (run.value(QStringLiteral("pending")).toBool()) {
        showPendingRun(text(run, "id") == text(m_pendingRun, "id") ? m_pendingRun : run);
        return;
    }
    if (run.value(QStringLiteral("local")).toBool()) {
        showLocalRunDetail(m_runSteps, m_runSummary, run);
        setLiveText(m_runOutput, localRunLog(run), fresh);
        if (liveStatus(m_shownStatus) && !m_liveTimer->isActive())
            m_liveTimer->start();
        pollLiveRun();
        return;
    }
    if (fresh && text(run, "id") != m_flowAliasTo)
        m_runOutput->clear();
    showRunDetail(m_runSteps, m_runSummary, text(run, "workflowId"), text(run, "id"), m_runOutput);
}

void WorkflowsFeature::showPendingRun(const QJsonObject &run)
{
    const QJsonArray steps = run.value(QStringLiteral("steps")).toArray();
    m_runSteps->setRowCount(0);
    if (QTableWidgetItem *header = m_runSteps->horizontalHeaderItem(3))
        header->setText(QStringLiteral("Items"));
    for (const FlowStep &step : flowSteps(steps)) {
        const int row = m_runSteps->rowCount();
        m_runSteps->insertRow(row);
        m_runSteps->setItem(row, 0, cell(step.name));
        m_runSteps->setItem(row, 1, statusCell(step.status));
        m_runSteps->item(row, 1)->setText(localStatusLabel(step.status));
        m_runSteps->setItem(row, 2, cell(step.note.isEmpty() ? QStringLiteral("–") : step.note));
        m_runSteps->setItem(row, 3, cell(QStringLiteral("–")));
        m_runSteps->setItem(row, 4, cell(step.detail.left(240), step.detail));
    }
    const QString name = text(run, "workflowName");
    const QString where = text(run, "where");
    const qint64 started = integer(run, "startedAt");
    const bool live = liveStatus(text(run, "status"));
    m_runSummary->setTextFormat(Qt::PlainText);
    m_runSummary->setText(
        live ? QStringLiteral("Running %1 %2…\nStarted %3 · trigger: %4")
                   .arg(name, where, timeText(started), text(run, "trigger"))
             : QStringLiteral("%1 did not start %2.\n%3").arg(name, where, text(run, "error")));
    QString log = text(run, "log");
    if (log.isEmpty())
        log = QStringLiteral("▶ %1 started %2 at %3\n%4")
                  .arg(name, where, timeText(started),
                       where == QLatin1String("on ForkMesh") ? QStringLiteral("Waiting for ForkMesh to finish the run…")
                                                             : QStringLiteral("Waiting for the first step's output…"));
    setLiveText(m_runOutput, log, true);
    showFlow(text(run, "id"), flowSteps(steps), live);
}

void WorkflowsFeature::showFlow(const QString &key, const QList<FlowStep> &steps, bool live)
{
    m_runFlow->setSteps(key, steps, live, key == m_flowAliasTo ? m_flowAliasFrom : QString());
    m_runFlowArea->setVisible(live || !steps.isEmpty());
    m_runFlowArea->ensureVisible(m_runFlow->focusX(), m_runFlow->height() / 2, 80, 0);
}

void WorkflowsFeature::startPendingRun(const QString &workflowKey, const QString &name, const QString &where,
                                       const QJsonArray &steps)
{
    m_pendingRun = QJsonObject{{QStringLiteral("id"), QStringLiteral("pending:") + workflowKey},
                               {QStringLiteral("pending"), true},
                               {QStringLiteral("workflowId"), workflowKey},
                               {QStringLiteral("workflowName"), name},
                               {QStringLiteral("trigger"), QStringLiteral("By hand")},
                               {QStringLiteral("where"), where},
                               {QStringLiteral("status"), QStringLiteral("running")},
                               {QStringLiteral("startedAt"), double(now())},
                               {QStringLiteral("steps"), steps}};
    renderDashboard();
    followRun(workflowKey);
}

void WorkflowsFeature::landPendingRun(const QJsonObject &run, const QString &workflowKey)
{
    const QString id = text(run, "id");
    if (m_followWorkflow == workflowKey)
        m_followRun = id;
    if (id.isEmpty() || text(m_pendingRun, "workflowId") != workflowKey) {
        m_pendingRun = QJsonObject();
        return;
    }
    m_flowAliasFrom = text(m_pendingRun, "id");
    m_flowAliasTo = id;
    QJsonObject fresh = run;
    fresh.insert(QStringLiteral("workflowId"), workflowKey);
    fresh.insert(QStringLiteral("workflowName"), text(m_pendingRun, "workflowName"));
    fresh.insert(QStringLiteral("runId"), id);
    if (text(fresh, "trigger").isEmpty())
        fresh.insert(QStringLiteral("trigger"), text(m_pendingRun, "trigger"));
    m_pendingRun = fresh;
}

void WorkflowsFeature::failPendingRun(const QString &workflowKey, const QString &message)
{
    if (text(m_pendingRun, "workflowId") != workflowKey || !m_pendingRun.value(QStringLiteral("pending")).toBool() ||
        !text(m_pendingRun, "runId").isEmpty())
        return;
    m_pendingRun.insert(QStringLiteral("status"), QStringLiteral("error"));
    m_pendingRun.insert(QStringLiteral("finishedAt"), double(now()));
    m_pendingRun.insert(QStringLiteral("error"), message);
    m_pendingRun.insert(QStringLiteral("log"), QStringLiteral("✕ %1 did not start.\n%2")
                                                   .arg(text(m_pendingRun, "workflowName"), message));
}

void WorkflowsFeature::updateDesktopPending(int running)
{
    const DesktopRun &run = m_desktopRun;
    if (!m_pendingRun.value(QStringLiteral("pending")).toBool() || text(m_pendingRun, "workflowId") != run.draft.id)
        return;
    QJsonArray steps;
    QStringList log;
    for (int index = 0; index < run.order.size(); ++index) {
        const QString id = run.order.at(index);
        const QJsonObject node = run.draft.node(id);
        if (node.value(QStringLiteral("disabled")).toBool())
            continue;
        const QJsonObject report = run.report.value(id).toObject();
        QString status = text(report, "status");
        if (status.isEmpty())
            status = index == running ? QStringLiteral("running") : QStringLiteral("queued");
        const QString name = text(node, "name").isEmpty() ? id : text(node, "name");
        QJsonObject step{{QStringLiteral("name"), name}, {QStringLiteral("status"), status}};
        if (report.contains(QStringLiteral("ms")))
            step.insert(QStringLiteral("note"), durationText(integer(report, "ms")));
        if (!text(report, "error").isEmpty())
            step.insert(QStringLiteral("error"), text(report, "error"));
        steps.append(step);
        QStringList lines;
        for (const QJsonValue &line : report.value(QStringLiteral("logs")).toArray())
            lines << line.toString();
        if (!text(report, "error").isEmpty())
            lines << QStringLiteral("Error: %1").arg(text(report, "error"));
        if (status == QLatin1String("running"))
            lines << QStringLiteral("Working…");
        if (!lines.isEmpty())
            log << QStringLiteral("── %1 · %2\n%3").arg(name, localStatusLabel(status), lines.join(QLatin1Char('\n')));
    }
    m_pendingRun.insert(QStringLiteral("steps"), steps);
    m_pendingRun.insert(QStringLiteral("log"), log.join(QStringLiteral("\n\n")));
    if (text(selectedDashboardRun(), "id") == text(m_pendingRun, "id"))
        showPendingRun(m_pendingRun);
}

void WorkflowsFeature::pollLiveRun()
{
    const QJsonObject shown = selectedDashboardRun();
    if (!shown.value(QStringLiteral("local")).toBool()) {
        m_liveTimer->stop();
        return;
    }
    if (m_liveTimer->isActive() && (!isVisible() || m_pages->currentIndex() != 0))
        return;
    const QString repo = text(shown, "repo");
    const QString runId = text(shown, "id");
    const QString name = text(shown, "workflowName");
    localAction(repo, text(shown, "localWorkflowId"), QStringLiteral("get"), {},
                [this, repo, runId, name](bool ok, const QJsonObject &payload, const QString &) {
        if (!ok || text(selectedDashboardRun(), "id") != runId)
            return;
        QJsonObject found = payload.value(QStringLiteral("active")).toObject();
        if (text(found, "id") != runId) {
            found = QJsonObject();
            for (const QJsonValue &value : payload.value(QStringLiteral("runs")).toArray())
                if (text(value.toObject(), "id") == runId)
                    found = value.toObject();
        }
        if (found.isEmpty())
            return;
        const QJsonObject row = localRunRow(found, repo, name);
        showLocalRunDetail(m_runSteps, m_runSummary, row);
        setLiveText(m_runOutput, localRunLog(row), false);
        if (!liveStatus(text(row, "status")))
            m_liveTimer->stop();
    });
}

void WorkflowsFeature::followRun(const QString &workflowKey, const QString &runId)
{
    m_followWorkflow = workflowKey;
    m_followRun = runId;
    m_followSince = now() - 2000;
    m_sparkled = false;
    const QString filtered = m_runWorkflowFilter->currentData().toString();
    if ((!filtered.isEmpty() && filtered != workflowKey) || m_runStatusFilter->currentIndex() != 0) {
        const QSignalBlocker workflowBlock(m_runWorkflowFilter);
        const QSignalBlocker statusBlock(m_runStatusFilter);
        if (filtered != workflowKey)
            m_runWorkflowFilter->setCurrentIndex(0);
        m_runStatusFilter->setCurrentIndex(0);
    }
    if (m_pages->currentIndex() == 0) {
        const QList<int> sizes = m_dashboardSplitter->sizes();
        const int total = sizes.value(0) + sizes.value(1);
        if (total > 0 && sizes.value(1) < total * 2 / 5)
            m_dashboardSplitter->setSizes({total - total / 2, total / 2});
    }
    renderRunTable();
}

void WorkflowsFeature::stopFollowing()
{
    m_followWorkflow.clear();
    m_followRun.clear();
    m_followSince = 0;
}

QJsonObject WorkflowsFeature::selectedDashboardRun() const
{
    const int row = m_runTable->currentRow();
    if (row < 0 || !m_runTable->item(row, 0) || m_runTable->selectedItems().isEmpty())
        return {};
    const QString id = m_runTable->item(row, 0)->data(Qt::UserRole + 1).toString();
    for (const QJsonValue &value : m_allRuns)
        if (text(value.toObject(), "id") == id)
            return value.toObject();
    return {};
}

void WorkflowsFeature::showRunDetail(QTableWidget *steps, QLabel *summary, const QString &workflowId,
                                     const QString &runId, QPlainTextEdit *outputView)
{
    if (workflowId.isEmpty() || runId.isEmpty())
        return;
    summary->setTextFormat(Qt::PlainText);
    summary->setText(QStringLiteral("Loading the run…"));
    steps->setRowCount(0);
    if (QTableWidgetItem *header = steps->horizontalHeaderItem(3))
        header->setText(QStringLiteral("Items"));
    const QPointer<QTableWidget> table(steps);
    const QPointer<QLabel> label(summary);
    const QPointer<QPlainTextEdit> log(outputView);
    auto render = [this, table, label, log, workflowId, runId](const Draft &draft) {
        request(QByteArrayLiteral("GET"), path(QStringLiteral("/%1/runs/%2").arg(workflowId, runId)), {},
                [this, table, label, log, runId, draft](bool ok, const QJsonObject &payload, const QString &error) {
            if (!table || !label)
                return;
            if (!ok) {
                label->setText(QStringLiteral("The run did not load: %1").arg(failureText(payload, error)));
                return;
            }
            const QJsonObject run = payload.value(QStringLiteral("run")).toObject();
            const qint64 started = integer(run, "startedAt");
            const qint64 finished = integer(run, "finishedAt");
            QStringList parts{statusLabel(text(run, "status"))};
            if (started > 0)
                parts << timeText(started);
            if (finished >= started && started > 0)
                parts << durationText(finished - started);
            if (!text(run, "trigger").isEmpty())
                parts << QStringLiteral("trigger: %1").arg(text(run, "trigger"));
            if (run.value(QStringLiteral("requests")).toInt() > 0)
                parts << QStringLiteral("%1 requests").arg(run.value(QStringLiteral("requests")).toInt());
            QString headline = parts.join(QStringLiteral(" · "));
            const QString output = runOutputText(run);
            if (!output.isEmpty())
                headline += QStringLiteral("\n") + output;
            if (!text(run, "stopped").isEmpty())
                headline += QStringLiteral("\nStopped at %1").arg(text(run, "stopped"));
            label->setText(headline);
            const QJsonObject nodes = run.value(QStringLiteral("nodes")).toObject();
            QStringList order = draft.ordered();
            for (auto it = nodes.begin(); it != nodes.end(); ++it)
                if (!order.contains(it.key()))
                    order << it.key();
            table->setRowCount(0);
            QStringList lines;
            QList<FlowStep> flow;
            for (const QString &id : std::as_const(order)) {
                if (!nodes.contains(id))
                    continue;
                const QJsonObject entry = nodes.value(id).toObject();
                const QString name = text(draft.node(id), "name");
                const int row = table->rowCount();
                table->insertRow(row);
                table->setItem(row, 0, cell(name.isEmpty() ? id : name));
                table->setItem(row, 1, statusCell(text(entry, "status")));
                table->setItem(row, 2, cell(entry.contains(QStringLiteral("ms")) ? durationText(integer(entry, "ms"))
                                                                                  : QStringLiteral("–")));
                table->setItem(row, 3, cell(text(entry, "status") == QLatin1String("success")
                                                ? QString::number(itemCount(entry)) : QStringLiteral("–")));
                QStringList logs;
                for (const QJsonValue &line : entry.value(QStringLiteral("logs")).toArray())
                    logs << line.toString();
                QString details = text(entry, "error");
                if (details.isEmpty())
                    details = itemPreview(entry);
                if (details.isEmpty() && !logs.isEmpty())
                    details = logs.last();
                QString tooltip = details;
                if (!logs.isEmpty())
                    tooltip += QStringLiteral("\n\n") + logs.mid(std::max(0, int(logs.size()) - 40)).join(QLatin1Char('\n'));
                table->setItem(row, 4, cell(details, tooltip.trimmed()));
                flow.append({name.isEmpty() ? id : name, text(entry, "status"),
                             entry.contains(QStringLiteral("ms")) ? durationText(integer(entry, "ms")) : QString(),
                             details});
                QStringList block{QStringLiteral("── %1 · %2").arg(name.isEmpty() ? id : name,
                                                                  statusLabel(text(entry, "status")))};
                if (!logs.isEmpty())
                    block << logs.join(QLatin1Char('\n'));
                if (!text(entry, "error").isEmpty())
                    block << QStringLiteral("Error: %1").arg(text(entry, "error"));
                else if (!itemPreview(entry).isEmpty())
                    block << itemPreview(entry);
                lines << block.join(QLatin1Char('\n'));
            }
            if (!output.isEmpty())
                lines << output;
            if (log && (log != m_runOutput || m_shownRunId == runId))
                setLiveText(log, lines.join(QStringLiteral("\n\n")), true);
            if (table == m_runSteps && m_shownRunId == runId)
                showFlow(runId, flow, false);
        });
    };
    if (workflowId == m_draft.id && m_pages->currentIndex() == 1) {
        render(m_draft);
        return;
    }
    fetchWorkflow(workflowId, [render](const QJsonObject &workflow) { render(Draft::fromJson(workflow)); });
}

void WorkflowsFeature::fetchWorkflow(const QString &id, std::function<void(const QJsonObject &)> done)
{
    if (id.isEmpty())
        return;
    request(QByteArrayLiteral("GET"), path(QStringLiteral("/") + id), {},
            [this, done = std::move(done)](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok) {
            setStatus(QStringLiteral("The workflow did not load: %1").arg(failureText(payload, error)), true);
            return;
        }
        done(payload.value(QStringLiteral("workflow")).toObject());
    });
}

void WorkflowsFeature::openWorkflow(const QString &id)
{
    if (id.isEmpty())
        return;
    if (m_pages->currentIndex() == 1 && m_draft.id == id)
        return;
    if (m_pages->currentIndex() == 1 && !leaveEditor())
        return;
    setStatus(QStringLiteral("Opening the workflow…"));
    loadCatalog([this, id] {
        fetchWorkflow(id, [this](const QJsonObject &workflow) {
            openDraft(Draft::fromJson(workflow), false);
            setStatus(QStringLiteral("Editing %1.").arg(m_draft.name));
        });
    });
}

void WorkflowsFeature::openDraft(const Draft &draft, bool dirty)
{
    m_draft = draft;
    m_editorRuns = QJsonArray();
    const QStringList order = m_draft.ordered();
    m_selectedStep = order.isEmpty() ? QString() : order.first();
    m_pages->setCurrentIndex(1);
    m_subtitle->setVisible(false);
    renderEditor();
    setDirty(dirty);
    loadEditorRuns();
    if (!m_credentialsLoaded)
        loadCredentials([this] {
            if (m_pages->currentIndex() == 1)
                renderStepForm();
        });
}

bool WorkflowsFeature::leaveEditor()
{
    if (!m_dirty)
        return true;
    if (m_busy) {
        setStatus(QStringLiteral("Wait for the run to finish first."), true);
        return false;
    }
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Unsaved changes"),
        QStringLiteral("Save the changes to \"%1\" before leaving?").arg(m_draft.name),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Cancel)
        return false;
    if (answer == QMessageBox::Save) {
        saveDraft([this](bool ok) {
            if (ok)
                showDashboard();
        });
        return false;
    }
    setDirty(false);
    return true;
}

void WorkflowsFeature::showDashboard()
{
    m_pages->setCurrentIndex(0);
    m_subtitle->setVisible(true);
    m_draft = Draft();
    m_selectedStep.clear();
    setDirty(false);
    refresh();
}

void WorkflowsFeature::toggleActive(const QJsonObject &summary)
{
    const QString id = text(summary, "id");
    if (id.isEmpty())
        return;
    const bool active = !summary.value(QStringLiteral("active")).toBool();
    fetchWorkflow(id, [this, id, active](const QJsonObject &workflow) {
        QJsonObject body = Draft::fromJson(workflow).body();
        body.insert(QStringLiteral("active"), active);
        request(QByteArrayLiteral("POST"), path(QStringLiteral("/") + id), body,
                [this, active](bool ok, const QJsonObject &payload, const QString &error) {
            if (!ok)
                return setStatus(QStringLiteral("The workflow was not changed: %1").arg(failureText(payload, error)), true);
            setStatus(active ? QStringLiteral("Workflow turned on.") : QStringLiteral("Workflow turned off."));
            refresh();
        });
    });
}

void WorkflowsFeature::duplicateWorkflow(const QJsonObject &summary)
{
    const QString id = text(summary, "id");
    if (id.isEmpty())
        return;
    if (m_workflows.size() >= kMaxWorkflows)
        return setStatus(errorText(QStringLiteral("too_many_workflows"), QString()), true);
    fetchWorkflow(id, [this](const QJsonObject &workflow) {
        QJsonObject body = Draft::fromJson(workflow).body();
        body.insert(QStringLiteral("name"), QStringLiteral("Copy of %1").arg(text(workflow, "name")).left(80));
        body.insert(QStringLiteral("active"), false);
        request(QByteArrayLiteral("POST"), path(), body,
                [this](bool ok, const QJsonObject &payload, const QString &error) {
            if (!ok)
                return setStatus(QStringLiteral("The copy was not made: %1").arg(failureText(payload, error)), true);
            setStatus(QStringLiteral("Copied as %1 (off until you turn it on).")
                          .arg(text(payload.value(QStringLiteral("workflow")).toObject(), "name")));
            refresh();
        });
    });
}

void WorkflowsFeature::deleteWorkflow(const QString &id, const QString &name)
{
    if (id.isEmpty())
        return;
    if (QMessageBox::warning(this, QStringLiteral("Delete workflow"),
                             QStringLiteral("Delete \"%1\" and its run history? This cannot be undone.").arg(name),
                             QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    request(QByteArrayLiteral("POST"), path(QStringLiteral("/%1/delete").arg(id)), {},
            [this, id, name](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The workflow was not deleted: %1").arg(failureText(payload, error)), true);
        m_runsByWorkflow.remove(id);
        setStatus(QStringLiteral("Deleted %1.").arg(name));
        if (m_pages->currentIndex() == 1 && m_draft.id == id) {
            setDirty(false);
            showDashboard();
            return;
        }
        refresh();
    });
}

void WorkflowsFeature::runSummary(const QJsonObject &summary, const QString &assignment)
{
    const QString id = text(summary, "id");
    if (id.isEmpty() || m_busy)
        return;
    loadCatalog([this, id, assignment] {
        fetchWorkflow(id, [this, id, assignment](const QJsonObject &workflow) {
            const Draft draft = Draft::fromJson(workflow);
            if (draft.hasDesktopSteps(m_catalog)) {
                runOnDesktop(draft);
                return;
            }
            QJsonObject body;
            if (!assignment.isEmpty())
                body.insert(QStringLiteral("payload"), QJsonObject{{QStringLiteral("assignment"), assignment}});
            runWorkflow(id, body, pendingSteps(draft));
        });
    });
}

void WorkflowsFeature::runWorkflow(const QString &id, const QJsonObject &body, const QJsonArray &steps)
{
    m_busy = true;
    m_runDraftButton->setEnabled(false);
    setStatus(QStringLiteral("Running on ForkMesh…"));
    QString name = id == m_draft.id ? m_draft.name : QString();
    for (const QJsonValue &value : std::as_const(m_workflows))
        if (text(value.toObject(), "id") == id)
            name = text(value.toObject(), "name");
    startPendingRun(id, name, QStringLiteral("on ForkMesh"),
                    steps.isEmpty() && id == m_draft.id ? pendingSteps(m_draft) : steps);
    request(QByteArrayLiteral("POST"), path(QStringLiteral("/%1/run").arg(id)), body,
            [this, id](bool ok, const QJsonObject &payload, const QString &error) {
        m_busy = false;
        m_runDraftButton->setEnabled(true);
        if (!ok) {
            failPendingRun(id, failureText(payload, error));
            renderDashboard();
            stopFollowing();
            setStatus(QStringLiteral("The run did not start: %1").arg(failureText(payload, error)), true);
            return;
        }
        const QJsonObject run = payload.value(QStringLiteral("run")).toObject();
        landPendingRun(run, id);
        const qint64 ms = integer(run, "finishedAt") - integer(run, "startedAt");
        if (text(run, "status") == QLatin1String("success"))
            setStatus(QStringLiteral("Run succeeded in %1. %2").arg(durationText(ms), runOutputText(run)).trimmed());
        else
            setStatus(QStringLiteral("Run failed: %1").arg(text(run, "error")), true);
        if (m_pages->currentIndex() == 1 && m_draft.id == id)
            loadEditorRuns();
        renderDashboard();
        refresh();
    });
}

void WorkflowsFeature::setHostSearchQuery(const QString &query)
{
    m_hostQuery = query.trimmed();
    if (m_filter->text() != m_hostQuery)
        m_filter->setText(m_hostQuery);
    renderRunTable();
}

QList<forkmesh::market::NativePackageSearchHit> WorkflowsFeature::searchHits(const QString &query, int limit) const
{
    QList<forkmesh::market::NativePackageSearchHit> hits;
    const QString needle = query.trimmed();
    if (needle.isEmpty())
        return hits;
    for (const QJsonValue &value : m_localWorkflows) {
        if (limit > 0 && hits.size() >= limit)
            break;
        const QJsonObject summary = value.toObject();
        if (!text(summary, "name").contains(needle, Qt::CaseInsensitive) &&
            !text(summary, "description").contains(needle, Qt::CaseInsensitive))
            continue;
        hits.append({QStringLiteral("repo:%1|%2").arg(text(summary, "repo"), text(summary, "id")),
                     text(summary, "name"),
                     QStringLiteral("%1 · %2 · %3")
                         .arg(summary.value(QStringLiteral("specialAgent")).toBool() ? QStringLiteral("Special agent")
                                                                                     : QStringLiteral("Workflow"),
                              text(summary, "repo"), text(summary, "trigger"))});
    }
    for (const QJsonValue &value : m_workflows) {
        if (limit > 0 && hits.size() >= limit)
            break;
        const QJsonObject summary = value.toObject();
        if (!text(summary, "name").contains(needle, Qt::CaseInsensitive))
            continue;
        hits.append({text(summary, "id"), text(summary, "name"),
                     QStringLiteral("Workflow · %1 · %2")
                         .arg(triggerLabel(text(summary, "trigger")),
                              summary.value(QStringLiteral("active")).toBool() ? QStringLiteral("on")
                                                                               : QStringLiteral("off"))});
    }
    return hits;
}

void WorkflowsFeature::activateSearchHit(const QString &id)
{
    if (id.startsWith(QLatin1String("run:"))) {
        runSummary(QJsonObject{{QStringLiteral("id"), id.mid(4)}});
        return;
    }
    if (id.startsWith(QLatin1String("repo:"))) {
        const QString rest = id.mid(5);
        openLocalWorkflow(rest.section(QLatin1Char('|'), 0, 0), rest.section(QLatin1Char('|'), 1));
        return;
    }
    openWorkflow(id);
}

int WorkflowsFeature::navigationBadge() const
{
    int failed = 0;
    for (const QJsonValue &value : m_localWorkflows)
        failed += text(value.toObject(), "lastStatus") == QLatin1String("error") ? 1 : 0;
    for (const QJsonValue &value : m_workflows)
        failed += text(value.toObject(), "lastStatus") == QLatin1String("error") ? 1 : 0;
    return failed;
}

void WorkflowsFeature::renderEditor()
{
    m_name->setText(m_draft.name);
    {
        const QSignalBlocker block(m_active);
        m_active->setChecked(m_draft.active);
    }
    const bool saved = !m_draft.id.isEmpty();
    m_deleteDraftButton->setText(saved ? QStringLiteral("Delete") : QStringLiteral("Discard"));
    renderLocalEditorBar();
    if (m_draft.isLocal()) {
        const QJsonObject summary = localSummary(m_draft.repo, m_draft.id);
        QStringList parts{saved ? QStringLiteral("Runs on this desktop from %1, file .forkmesh/workflows/%2.json.")
                                      .arg(m_draft.repo, m_draft.id)
                                : QStringLiteral("Runs on this desktop from %1. Save writes "
                                                 ".forkmesh/workflows/<id>.json in its checkout.")
                                      .arg(m_draft.repo)};
        if (m_draft.hasAgentStep())
            parts << QStringLiteral("Special agent steps run an agent with only the permissions you tick; the "
                                    "agent's reply is the step's output.");
        if (summary.value(QStringLiteral("running")).toBool())
            parts << QStringLiteral("Running now: %1.").arg(text(summary, "runningStep"));
        if (!text(summary, "error").isEmpty())
            parts << text(summary, "error");
        parts << QStringLiteral("Every run keeps a Markdown output file.");
        m_editorNote->setTextFormat(Qt::PlainText);
        m_editorNote->setToolTip(parts.join(QLatin1Char(' ')));
        m_editorNote->clear();
        m_editorNote->setVisible(false);
        renderStepList();
        renderStepForm();
        renderEditorFacts();
        return;
    }
    const QJsonObject trigger = m_draft.trigger(catalog());
    const QString type = text(trigger, "type");
    QString note;
    if (m_draft.hasDesktopSteps(catalog())) {
        note = QStringLiteral("Runs on this desktop: Run workflow starts each step's tools/free_disk_space.py "
                              "in your forkmesh checkout and saves the report to your account.");
    } else if (type == QLatin1String("core.webhook")) {
        note = saved && !m_draft.hookPath.isEmpty()
                   ? QStringLiteral("Webhook URL: %1%2 · <a href=\"copy-hook\">Copy</a> · "
                                    "<a href=\"rotate-hook\">New URL</a>%3")
                         .arg(QString::fromLatin1(kOrigin), m_draft.hookPath.toHtmlEscaped(),
                              m_draft.active ? QString() : QStringLiteral(" · answers only while Active"))
                   : QStringLiteral("Save the workflow to get its secret webhook URL.");
        m_editorNote->setTextFormat(Qt::RichText);
    } else if (type == QLatin1String("core.schedule")) {
        const QString every = everyText(
            m_draft.node(text(trigger, "id")).value(QStringLiteral("params")).toObject()
                .value(QStringLiteral("every")).toVariant().toInt());
        note = m_draft.active ? QStringLiteral("%1 while Active (5 to 10,080 minutes).").arg(every)
                              : QStringLiteral("%1 once you turn on Active.").arg(every);
    } else if (type.isEmpty()) {
        note = QStringLiteral("Add a trigger: choose Add step and pick Manual trigger, Webhook or Schedule.");
    } else {
        note = QStringLiteral("Runs when you press Run workflow, on ForkMesh's servers with your saved credentials.");
    }
    m_editorNote->setTextFormat(type == QLatin1String("core.webhook") && !m_draft.hasDesktopSteps(catalog())
                                    ? Qt::RichText : Qt::PlainText);
    m_editorNote->setText(note);
    m_editorNote->setToolTip(QString());
    m_editorNote->setVisible(!note.isEmpty());
    renderStepList();
    renderStepForm();
    renderEditorFacts();
}

void WorkflowsFeature::renderStepList()
{
    const QSignalBlocker block(m_steps);
    m_steps->clear();
    int selectedRow = -1;
    const QHash<QString, QString> statuses = latestStepStatuses();
    for (const QString &id : m_draft.ordered()) {
        auto *item = new QListWidgetItem;
        fillStepItem(item, id, statuses);
        m_steps->addItem(item);
        if (id == m_selectedStep)
            selectedRow = m_steps->count() - 1;
    }
    if (selectedRow < 0 && m_steps->count() > 0) {
        selectedRow = 0;
        m_selectedStep = m_steps->item(0)->data(Qt::UserRole).toString();
    }
    m_steps->setCurrentRow(selectedRow);
    m_removeStepButton->setEnabled(selectedRow >= 0);
    m_addStepButton->setEnabled(m_draft.nodes.size() < kMaxNodes);
}

QString WorkflowsFeature::selectedStepId() const
{
    QListWidgetItem *item = m_steps->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void WorkflowsFeature::selectStep(const QString &id)
{
    m_selectedStep = id;
    renderStepList();
    renderStepForm();
}

void WorkflowsFeature::updateSelectedNode(const std::function<void(QJsonObject &)> &change)
{
    QJsonObject node = m_draft.node(m_selectedStep);
    if (node.isEmpty())
        return;
    change(node);
    m_draft.updateNode(node);
    setDirty(true);
    QListWidgetItem *item = m_steps->currentItem();
    if (!item || item->data(Qt::UserRole).toString() != m_selectedStep)
        return;
    fillStepItem(item, m_selectedStep, latestStepStatuses());
}

void WorkflowsFeature::fillStepItem(QListWidgetItem *item, const QString &id,
                                    const QHash<QString, QString> &statuses)
{
    const QJsonObject node = m_draft.node(id);
    const QString type = text(node, "type");
    const QString name = text(node, "name");
    QString kind = QStringLiteral("step");
    if (catalog().isTrigger(type))
        kind = QStringLiteral("trigger");
    else if (type == QLatin1String("agent.run"))
        kind = QStringLiteral("agent");
    else if (catalog().isDesktop(type))
        kind = QStringLiteral("desktop");
    QString kindLine = catalog().name(type);
    const QString summary = catalog().summary(node);
    if (!summary.isEmpty() && summary != catalog().description(type))
        kindLine += QStringLiteral(" · ") + summary;
    QString detail = m_draft.inputsText(id);
    const bool warn = detail.isEmpty() && kind != QLatin1String("trigger");
    if (warn)
        detail = QStringLiteral("Not connected: nothing runs it");
    const QString status = statuses.value(name);
    item->setText(QStringList{name, kindLine, detail}.join(QLatin1Char('\n')).trimmed());
    item->setData(Qt::UserRole, id);
    item->setData(StepNameRole, name);
    item->setData(StepTypeRole, kindLine);
    item->setData(StepDetailRole, detail);
    item->setData(StepKindRole, kind);
    item->setData(StepWarnRole, warn);
    item->setData(StepOffRole, node.value(QStringLiteral("disabled")).toBool());
    item->setData(StepStatusRole, status);
    QString tooltip = catalog().description(type);
    if (!status.isEmpty())
        tooltip += QStringLiteral("\nLast run: %1").arg(localStatusLabel(status));
    item->setToolTip(tooltip.trimmed());
}

QHash<QString, QString> WorkflowsFeature::latestStepStatuses() const
{
    QHash<QString, QString> statuses;
    if (m_editorRuns.isEmpty())
        return statuses;
    const QJsonObject run = m_editorRuns.first().toObject();
    for (const QJsonValue &value : run.value(QStringLiteral("steps")).toArray()) {
        const QJsonObject entry = value.toObject();
        statuses.insert(text(entry, "name"), text(entry, "status"));
    }
    const QJsonObject nodes = run.value(QStringLiteral("nodes")).toObject();
    for (auto it = nodes.begin(); it != nodes.end(); ++it)
        statuses.insert(text(m_draft.node(it.key()), "name"), text(it.value().toObject(), "status"));
    return statuses;
}

void WorkflowsFeature::renderEditorFacts()
{
    QLayout *row = m_editorFacts->layout();
    while (QLayoutItem *item = row->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    const auto add = [row](const QString &value, const QString &tone = QString(),
                           const QString &tooltip = QString()) {
        auto *chip = new QLabel(value);
        chip->setObjectName(QStringLiteral("workflowsFact"));
        chip->setAccessibleName(value);
        if (!tone.isEmpty())
            chip->setProperty("tone", tone);
        chip->setToolTip(tooltip.isEmpty() ? value : tooltip);
        chip->setTextInteractionFlags(Qt::TextSelectableByMouse);
        row->addWidget(chip);
    };
    const qint64 current = now();
    const bool local = m_draft.isLocal();
    const QJsonObject summary = local ? localSummary(m_draft.repo, m_draft.id) : QJsonObject();
    QString lastStatus;
    qint64 lastRun = 0;
    int runs = int(m_editorRuns.size());
    int failed = 0;
    for (const QJsonValue &value : std::as_const(m_editorRuns))
        failed += text(value.toObject(), "status") == QLatin1String("error") ? 1 : 0;
    if (!m_editorRuns.isEmpty()) {
        lastStatus = text(m_editorRuns.first().toObject(), "status");
        lastRun = integer(m_editorRuns.first().toObject(), "startedAt");
    }
    if (local && !summary.isEmpty()) {
        runs = std::max(runs, summary.value(QStringLiteral("runs")).toInt());
        failed = std::max(failed, summary.value(QStringLiteral("failedRuns")).toInt());
        lastRun = std::max(lastRun, integer(summary, "lastRunAt"));
        if (lastStatus.isEmpty())
            lastStatus = text(summary, "lastStatus");
    }

    const bool running = summary.value(QStringLiteral("running")).toBool() || liveStatus(lastStatus);
    if (!m_draft.active)
        add(local ? QStringLiteral("Turned off") : QStringLiteral("Not active"), QString(),
            QStringLiteral("It never runs by itself until you turn it on"));
    if (running) {
        const QString step = text(summary, "runningStep");
        add(step.isEmpty() ? QStringLiteral("Running") : QStringLiteral("Running: %1").arg(step),
            QStringLiteral("accent"));
    } else if (lastStatus == QLatin1String("success")) {
        add(QStringLiteral("Last run succeeded"), QStringLiteral("success"));
    } else if (lastStatus == QLatin1String("error")) {
        add(QStringLiteral("Last run failed"), QStringLiteral("danger"));
    }
    if (!text(summary, "error").isEmpty())
        add(QStringLiteral("Problem in the file"), QStringLiteral("attention"), text(summary, "error"));
    if (!text(summary, "trigger").isEmpty())
        add(text(summary, "trigger"));
    const qint64 next = integer(summary, "nextRunAt");
    if (m_draft.active && next > current)
        add(QStringLiteral("Next run %1").arg(relativeTime(next, current).toLower()), QString(), timeText(next));
    add(lastRun > 0 ? QStringLiteral("Last run %1").arg(relativeTime(lastRun, current).toLower())
                    : QStringLiteral("Never run"),
        QString(), lastRun > 0 ? timeText(lastRun) : QString());
    if (runs > 0) {
        const int rate = int(std::lround(100.0 * (runs - failed) / runs));
        add(QStringLiteral("%1 %2 · %3% ok").arg(runs).arg(runs == 1 ? QStringLiteral("run") : QStringLiteral("runs"))
                .arg(rate),
            failed > 0 ? QStringLiteral("attention") : QString(),
            QStringLiteral("%1 of the last %2 runs failed").arg(failed).arg(runs));
    }
    const int steps = int(m_draft.ordered().size());
    add(QStringLiteral("%1 %2").arg(steps).arg(steps == 1 ? QStringLiteral("step") : QStringLiteral("steps")));
    if (local) {
        const QString file = m_draft.id.isEmpty() ? QStringLiteral("not saved yet")
                                                   : QStringLiteral(".forkmesh/workflows/%1.json").arg(m_draft.id);
        add(QStringLiteral("%1 · %2").arg(m_draft.repo, file), QString(), m_editorNote->toolTip());
        if (!text(summary, "pinned").isEmpty())
            add(QStringLiteral("On the %1 rail").arg(text(summary, "pinned")));
    } else {
        add(m_draft.hasDesktopSteps(catalog()) ? QStringLiteral("Runs on this desktop")
                                               : QStringLiteral("Runs on ForkMesh"));
    }
    qobject_cast<QHBoxLayout *>(row)->addStretch();
}

void WorkflowsFeature::renderStepForm()
{
    QLayout *hostLayout = m_formHost->layout();
    while (QLayoutItem *item = hostLayout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(4, 0, 4, 0);
    layout->setSpacing(8);
    hostLayout->addWidget(content);
    const QJsonObject node = m_draft.node(m_selectedStep);
    if (node.isEmpty()) {
        layout->addWidget(mutedLabel(QStringLiteral("Select a step to edit it, or press Add step.")));
        layout->addStretch();
        return;
    }
    const QString type = text(node, "type");
    const bool trigger = catalog().isTrigger(type);
    layout->addWidget(sectionLabel(catalog().name(type)));
    if (!catalog().description(type).isEmpty())
        layout->addWidget(mutedLabel(catalog().description(type)));
    if (catalog().spec(type).isEmpty())
        layout->addWidget(mutedLabel(QStringLiteral("This step type (%1) is not in the catalog; it is kept as it is.").arg(type)));

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(6);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
    auto *name = new QLineEdit(text(node, "name"));
    name->setAccessibleName(QStringLiteral("Step name"));
    name->setMaxLength(60);
    form->addRow(QStringLiteral("Name"), name);
    connect(name, &QLineEdit::editingFinished, this, [this, name] {
        const QString value = name->text().trimmed();
        const QString current = text(m_draft.node(m_selectedStep), "name");
        if (value == current)
            return;
        if (value.isEmpty() || m_draft.nameTaken(value, m_selectedStep)) {
            setStatus(value.isEmpty() ? QStringLiteral("A step needs a name.")
                                      : QStringLiteral("Another step is already called %1.").arg(value),
                      true);
            name->setText(current);
            return;
        }
        updateSelectedNode([value](QJsonObject &item) { item.insert(QStringLiteral("name"), value); });
        renderStepList();
    });

    if (trigger) {
        auto *kind = new QComboBox;
        kind->setAccessibleName(QStringLiteral("Trigger"));
        for (const QString &option : catalog().triggerTypes())
            kind->addItem(catalog().name(option), option);
        kind->setCurrentIndex(std::max(0, kind->findData(type)));
        form->addRow(QStringLiteral("Starts on"), kind);
        connect(kind, &QComboBox::currentIndexChanged, this, [this, kind] {
            if (m_draft.isLocal())
                m_draft.retype(catalog(), m_selectedStep, kind->currentData().toString());
            else
                m_draft.setTrigger(catalog(), kind->currentData().toString());
            setDirty(true);
            QTimer::singleShot(0, this, [this] { renderEditor(); });
        });
    }

    QString operation;
    if (catalog().isApp(type)) {
        operation = node.value(QStringLiteral("params")).toObject().value(QStringLiteral("operation")).toString();
        auto *operations = new QComboBox;
        operations->setAccessibleName(QStringLiteral("Operation"));
        for (const QJsonValue &value : catalog().operations(type)) {
            const QJsonObject op = value.toObject();
            operations->addItem(QStringLiteral("%1 (%2)").arg(text(op, "name"), text(op, "method")), text(op, "key"));
        }
        operations->setCurrentIndex(std::max(0, operations->findData(operation)));
        if (operation.isEmpty())
            operation = operations->currentData().toString();
        form->addRow(QStringLiteral("Operation"), operations);
        connect(operations, &QComboBox::currentIndexChanged, this, [this, operations] {
            const QString chosen = operations->currentData().toString();
            updateSelectedNode([this, chosen](QJsonObject &item) {
                QJsonObject params{{QStringLiteral("operation"), chosen}};
                const QJsonObject old = item.value(QStringLiteral("params")).toObject();
                for (const QJsonValue &value : catalog().fields(text(item, "type"), chosen)) {
                    const QJsonObject field = value.toObject();
                    const QString key = text(field, "key");
                    if (old.contains(key))
                        params.insert(key, old.value(key));
                    else if (!field.value(QStringLiteral("default")).toVariant().toString().isEmpty())
                        params.insert(key, field.value(QStringLiteral("default")).toVariant().toString());
                }
                item.insert(QStringLiteral("params"), params);
            });
            QTimer::singleShot(0, this, [this] { renderStepForm(); });
        });
        if (!catalog().credentialFields(type.mid(4)).isEmpty()) {
            auto *row = new QHBoxLayout;
            auto *credential = new QComboBox;
            credential->setAccessibleName(QStringLiteral("Credential"));
            credential->addItem(QStringLiteral("None"), QString());
            for (const QJsonValue &value : std::as_const(m_credentials)) {
                const QJsonObject item = value.toObject();
                if (text(item, "integration") == type.mid(4))
                    credential->addItem(text(item, "name"), text(item, "id"));
            }
            const QString chosen = text(node, "credential");
            int index = credential->findData(chosen);
            if (index < 0 && !chosen.isEmpty()) {
                credential->addItem(QStringLiteral("Saved credential"), chosen);
                index = credential->count() - 1;
            }
            credential->setCurrentIndex(std::max(0, index));
            row->addWidget(credential, 1);
            auto *manage = button(QStringLiteral("Manage…"));
            row->addWidget(manage);
            form->addRow(QStringLiteral("Credential"), row);
            connect(credential, &QComboBox::currentIndexChanged, this, [this, credential] {
                const QString id = credential->currentData().toString();
                updateSelectedNode([id](QJsonObject &item) { item.insert(QStringLiteral("credential"), id); });
            });
            connect(manage, &QPushButton::clicked, this, [this, type] { credentialsDialog(type.mid(4)); });
        }
    }

    for (const QJsonValue &value : catalog().fields(type, operation)) {
        const QJsonObject field = value.toObject();
        QString label = text(field, "label");
        if (field.value(QStringLiteral("required")).toBool())
            label += QStringLiteral(" *");
        QWidget *editor = fieldEditor(field, node);
        editor->setAccessibleName(text(field, "label"));
        form->addRow(label, editor);
    }

    if (catalog().isDesktop(type) && catalog().spec(type).value(QStringLiteral("script")).isObject()) {
        const QJsonObject script = catalog().spec(type).value(QStringLiteral("script")).toObject();
        auto *info = mutedLabel(QStringLiteral("Runs on this desktop:\n%1\nTry it without deleting anything: %2")
                                    .arg(text(script, "command"), text(script, "dryRun")));
        info->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(QStringLiteral("Script"), info);
    }

    if (!trigger)
        addInputRow(form, node);

    if (!trigger) {
        auto *disabled = new QCheckBox(QStringLiteral("Turned off: pass items through without running this step"));
        disabled->setChecked(node.value(QStringLiteral("disabled")).toBool());
        auto *keepGoing = new QCheckBox(QStringLiteral("Keep going if this step fails"));
        keepGoing->setChecked(node.value(QStringLiteral("continueOnFail")).toBool());
        form->addRow(QString(), disabled);
        form->addRow(QString(), keepGoing);
        connect(disabled, &QCheckBox::toggled, this, [this](bool on) {
            updateSelectedNode([on](QJsonObject &item) { item.insert(QStringLiteral("disabled"), on); });
        });
        connect(keepGoing, &QCheckBox::toggled, this, [this](bool on) {
            updateSelectedNode([on](QJsonObject &item) { item.insert(QStringLiteral("continueOnFail"), on); });
        });
    }
    layout->addLayout(form);
    if (m_draft.isLocal())
        layout->addWidget(mutedLabel(QStringLiteral(
            "Each step gets the output of the steps it runs after: a special agent reads it under its assignment, "
            "a script shortcut in FORKMESH_WORKFLOW_INPUT.")));
    else
        layout->addWidget(mutedLabel(QStringLiteral(
            "Use {{ $json.field }} for the item's values, {{ $node[\"Step\"].json.field }} for another step's "
            "and {{ $trigger.field }} for the trigger's.")));
    layout->addStretch();
}

QWidget *WorkflowsFeature::fieldEditor(const QJsonObject &field, const QJsonObject &node)
{
    const QString key = text(field, "key");
    const QString kind = text(field, "type");
    const QJsonValue current = node.value(QStringLiteral("params")).toObject().value(key);
    const QString value = current.isUndefined() ? QString() : paramString(current);
    const QString placeholder = field.value(QStringLiteral("placeholder")).toVariant().toString();
    auto store = [this, key](const QString &entered) {
        updateSelectedNode([key, entered](QJsonObject &item) {
            QJsonObject params = item.value(QStringLiteral("params")).toObject();
            params.insert(key, storedValue(entered, params.value(key)));
            item.insert(QStringLiteral("params"), params);
        });
    };
    auto storeValue = [this, key](const QJsonValue &entered) {
        updateSelectedNode([key, entered](QJsonObject &item) {
            QJsonObject params = item.value(QStringLiteral("params")).toObject();
            params.insert(key, entered);
            item.insert(QStringLiteral("params"), params);
        });
    };
    if (kind == QLatin1String("checks")) {
        auto *box = new QWidget;
        auto *grid = new QGridLayout(box);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(12);
        grid->setVerticalSpacing(2);
        QStringList chosen;
        if (current.isArray()) {
            for (const QJsonValue &entry : current.toArray())
                chosen << entry.toVariant().toString();
        } else {
            for (const QString &entry : value.split(QLatin1Char(','), Qt::SkipEmptyParts))
                chosen << entry.trimmed();
        }
        const QJsonArray options = field.value(QStringLiteral("options")).toArray();
        QList<QCheckBox *> boxes;
        for (int index = 0; index < options.size(); ++index) {
            const QJsonObject entry = options.at(index).toObject();
            auto *check = new QCheckBox(entry.value(QStringLiteral("label")).toVariant().toString());
            check->setProperty("optionValue", entry.value(QStringLiteral("value")).toVariant().toString());
            check->setChecked(chosen.contains(check->property("optionValue").toString()));
            const int columns = options.size() > 4 ? 4 : 2;
            grid->addWidget(check, index / columns, index % columns);
            boxes.append(check);
        }
        for (QCheckBox *check : std::as_const(boxes)) {
            connect(check, &QCheckBox::toggled, this, [boxes, storeValue] {
                QJsonArray picked;
                for (QCheckBox *item : boxes)
                    if (item->isChecked())
                        picked.append(item->property("optionValue").toString());
                storeValue(picked);
            });
        }
        return box;
    }
    if (kind == QLatin1String("list")) {
        QStringList lines;
        if (current.isArray()) {
            for (const QJsonValue &entry : current.toArray())
                lines << entry.toVariant().toString();
        } else {
            lines = value.split(QLatin1Char('\n'));
        }
        auto *edit = new QPlainTextEdit(lines.join(QLatin1Char('\n')));
        edit->setPlaceholderText(placeholder.isEmpty() ? QStringLiteral("One per line") : placeholder);
        edit->setTabChangesFocus(true);
        edit->setMinimumHeight(70);
        edit->setMaximumHeight(200);
        connect(edit, &QPlainTextEdit::textChanged, this, [edit, storeValue] {
            QJsonArray entries;
            for (const QString &line : edit->toPlainText().split(QLatin1Char('\n'))) {
                const QString clean = line.trimmed();
                if (!clean.isEmpty())
                    entries.append(clean);
            }
            storeValue(entries);
        });
        return edit;
    }
    if (kind == QLatin1String("avatar")) {
        auto *combo = new QComboBox;
        combo->setIconSize(QSize(24, 24));
        for (const QJsonValue &option : m_local.value(QStringLiteral("avatars")).toArray()) {
            const QJsonObject entry = option.toObject();
            combo->addItem(faceIcon(text(entry, "face")), text(entry, "label"), text(entry, "value"));
        }
        const QString chosen = value.isEmpty() ? field.value(QStringLiteral("default")).toVariant().toString() : value;
        int index = combo->findData(chosen);
        if (index < 0 && !chosen.isEmpty()) {
            combo->addItem(chosen, chosen);
            index = combo->count() - 1;
        }
        combo->setCurrentIndex(std::max(0, index));
        connect(combo, &QComboBox::currentIndexChanged, this, [combo, storeValue] {
            storeValue(combo->currentData().toString());
        });
        return combo;
    }
    if (kind == QLatin1String("shortcut")) {
        auto *combo = new QComboBox;
        combo->setEditable(true);
        combo->setInsertPolicy(QComboBox::NoInsert);
        combo->addItem(QStringLiteral("None"), QString());
        for (const QJsonValue &option : repositoryShortcuts(m_draft.repo)) {
            const QJsonObject entry = option.toObject();
            const QString label = text(entry, "name") == text(entry, "file")
                                      ? text(entry, "file")
                                      : QStringLiteral("%1 (%2)").arg(text(entry, "name"), text(entry, "file"));
            combo->addItem(label, text(entry, "file"));
            combo->setItemData(combo->count() - 1, text(entry, "description"), Qt::ToolTipRole);
        }
        int index = combo->findData(value);
        if (index < 0 && !value.isEmpty()) {
            combo->addItem(value, value);
            index = combo->count() - 1;
        }
        combo->setCurrentIndex(std::max(0, index));
        connect(combo, &QComboBox::activated, this, [combo, storeValue] {
            storeValue(combo->currentData().toString());
        });
        connect(combo->lineEdit(), &QLineEdit::editingFinished, this, [combo, storeValue] {
            const int found = combo->findText(combo->currentText());
            storeValue(found >= 0 ? combo->itemData(found).toString() : combo->currentText().trimmed());
        });
        return combo;
    }
    if (kind == QLatin1String("select")) {
        auto *combo = new QComboBox;
        for (const QJsonValue &option : field.value(QStringLiteral("options")).toArray()) {
            const QJsonObject entry = option.toObject();
            combo->addItem(entry.value(QStringLiteral("label")).toVariant().toString(),
                           entry.value(QStringLiteral("value")).toVariant().toString());
        }
        const QString chosen = value.isEmpty() ? field.value(QStringLiteral("default")).toVariant().toString() : value;
        int index = combo->findData(chosen);
        if (index < 0 && !chosen.isEmpty()) {
            combo->addItem(chosen, chosen);
            index = combo->count() - 1;
        }
        combo->setCurrentIndex(std::max(0, index));
        connect(combo, &QComboBox::currentIndexChanged, this, [combo, store] { store(combo->currentData().toString()); });
        return combo;
    }
    if (kind == QLatin1String("boolean")) {
        auto *check = new QCheckBox;
        const QString chosen = value.isEmpty() ? field.value(QStringLiteral("default")).toVariant().toString() : value;
        check->setChecked(chosen == QLatin1String("true"));
        connect(check, &QCheckBox::toggled, this, [this, key](bool on) {
            updateSelectedNode([key, on](QJsonObject &item) {
                QJsonObject params = item.value(QStringLiteral("params")).toObject();
                if (params.value(key).isBool())
                    params.insert(key, on);
                else
                    params.insert(key, on ? QStringLiteral("true") : QStringLiteral("false"));
                item.insert(QStringLiteral("params"), params);
            });
        });
        return check;
    }
    if (kind == QLatin1String("text") || kind == QLatin1String("json") || kind == QLatin1String("code")) {
        auto *edit = new QPlainTextEdit(value);
        edit->setPlaceholderText(placeholder);
        edit->setTabChangesFocus(kind != QLatin1String("code"));
        edit->setMinimumHeight(kind == QLatin1String("code") ? 220 : 80);
        edit->setMaximumHeight(kind == QLatin1String("code") ? 480 : 180);
        if (kind != QLatin1String("text"))
            edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        connect(edit, &QPlainTextEdit::textChanged, this, [edit, store] { store(edit->toPlainText()); });
        return edit;
    }
    auto *line = new QLineEdit(value);
    line->setPlaceholderText(placeholder);
    if (kind == QLatin1String("number"))
        line->setInputMethodHints(Qt::ImhFormattedNumbersOnly);
    connect(line, &QLineEdit::textEdited, this, [store](const QString &entered) { store(entered); });
    return line;
}

void WorkflowsFeature::addInputRow(QFormLayout *form, const QJsonObject &node)
{
    const QString id = text(node, "id");
    auto *box = new QWidget;
    auto *rows = new QVBoxLayout(box);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(4);
    const QJsonArray inputs = m_draft.inputs(id);
    if (inputs.isEmpty())
        rows->addWidget(mutedLabel(QStringLiteral("Nothing runs this step yet; connect it below.")));
    for (const QJsonValue &value : inputs) {
        const QJsonObject link = value.toObject();
        auto *row = new QHBoxLayout;
        row->addWidget(new QLabel(m_draft.inputText(link)), 1);
        auto *remove = button(QStringLiteral("Disconnect"));
        row->addWidget(remove);
        rows->addLayout(row);
        connect(remove, &QPushButton::clicked, this, [this, link, id] {
            m_draft.disconnectSteps(text(link, "from"), text(link, "output"), id);
            setDirty(true);
            QTimer::singleShot(0, this, [this] {
                renderStepList();
                renderStepForm();
            });
        });
    }
    auto *addRow = new QHBoxLayout;
    auto *choice = new QComboBox;
    choice->setAccessibleName(QStringLiteral("Connect after"));
    for (const QString &candidate : m_draft.ordered()) {
        const QJsonObject source = m_draft.node(candidate);
        for (const QString &output : catalog().outputs(text(source, "type"))) {
            if (!m_draft.canConnect(catalog(), candidate, output, id))
                continue;
            bool exists = false;
            for (const QJsonValue &value : inputs)
                exists = exists || (text(value.toObject(), "from") == candidate &&
                                    text(value.toObject(), "output") == output);
            if (exists)
                continue;
            const QString label = output == QLatin1String("main")
                                      ? text(source, "name")
                                      : QStringLiteral("%1: %2").arg(text(source, "name"), output);
            choice->addItem(label, QStringList{candidate, output});
        }
    }
    if (choice->count() == 0)
        choice->setPlaceholderText(QStringLiteral("No other step can feed this one"));
    addRow->addWidget(choice, 1);
    auto *add = button(QStringLiteral("Connect"));
    add->setEnabled(choice->count() > 0);
    addRow->addWidget(add);
    rows->addLayout(addRow);
    form->addRow(QStringLiteral("Runs after"), box);
    connect(add, &QPushButton::clicked, this, [this, choice, id] {
        const QStringList chosen = choice->currentData().toStringList();
        if (chosen.size() != 2)
            return;
        if (!m_draft.connectSteps(catalog(), chosen.at(0), chosen.at(1), id))
            return setStatus(QStringLiteral("That connection would make a loop."), true);
        setDirty(true);
        QTimer::singleShot(0, this, [this] {
            renderStepList();
            renderStepForm();
        });
    });
}

void WorkflowsFeature::setDirty(bool dirty)
{
    m_dirty = dirty;
    const bool editing = m_pages->currentIndex() == 1;
    m_saveButton->setEnabled(editing && (dirty || m_draft.id.isEmpty()) && !m_busy);
    m_runDraftButton->setEnabled(editing && !m_busy);
    m_exportButton->setEnabled(editing);
    m_deleteDraftButton->setEnabled(editing && !m_busy);
    m_saveButton->setText(dirty ? QStringLiteral("Save changes") : QStringLiteral("Saved"));
    if (m_draft.id.isEmpty() && editing)
        m_saveButton->setText(QStringLiteral("Save"));
    m_exportButton->setText(m_draft.isLocal() ? QStringLiteral("Show file") : QStringLiteral("Download"));
    m_exportButton->setEnabled(editing && (!m_draft.isLocal() || !m_draft.id.isEmpty()));
    if (m_editorPinButton)
        m_editorPinButton->setEnabled(editing && m_draft.isLocal() && !m_draft.id.isEmpty());
}

void WorkflowsFeature::saveDraft(std::function<void(bool)> done)
{
    if (m_busy && !done)
        return;
    if (m_draft.isLocal()) {
        const bool creating = m_draft.id.isEmpty();
        Draft draft = m_draft;
        if (creating)
            draft.id = uniqueLocalId(draft.repo, draft.name);
        m_saveButton->setEnabled(false);
        setStatus(QStringLiteral("Saving…"));
        const QString selected = m_selectedStep;
        localAction(draft.repo, creating ? QString() : draft.id, QStringLiteral("save"),
                    {{QStringLiteral("workflow"), draft.localJson()}},
                    [this, selected, repo = draft.repo, done = std::move(done)](bool ok, const QJsonObject &payload,
                                                                                 const QString &message) {
            if (!ok) {
                setDirty(m_dirty);
                setStatus(QStringLiteral("Not saved: %1").arg(message), true);
                if (done)
                    done(false);
                return;
            }
            m_draft = Draft::fromLocal(payload.value(QStringLiteral("workflow")).toObject(), repo);
            m_selectedStep = m_draft.indexOf(selected) >= 0 ? selected : QString();
            setDirty(false);
            loadLocal();
            renderEditor();
            setStatus(message);
            if (done)
                done(true);
        });
        return;
    }
    const QString target = m_draft.id.isEmpty() ? path() : path(QStringLiteral("/") + m_draft.id);
    m_saveButton->setEnabled(false);
    setStatus(QStringLiteral("Saving…"));
    const QString selected = m_selectedStep;
    request(QByteArrayLiteral("POST"), target, m_draft.body(),
            [this, selected, done = std::move(done)](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok) {
            setDirty(m_dirty);
            setStatus(QStringLiteral("Not saved: %1").arg(failureText(payload, error)), true);
            if (done)
                done(false);
            return;
        }
        m_draft = Draft::fromJson(payload.value(QStringLiteral("workflow")).toObject());
        m_selectedStep = m_draft.indexOf(selected) >= 0 ? selected : QString();
        setDirty(false);
        renderEditor();
        setStatus(QStringLiteral("Saved %1.").arg(m_draft.name));
        refresh();
        if (done)
            done(true);
    });
}

void WorkflowsFeature::runDraft()
{
    if (m_busy)
        return;
    if (m_dirty || m_draft.id.isEmpty()) {
        saveDraft([this](bool ok) {
            if (ok)
                runDraft();
        });
        return;
    }
    if (m_draft.isLocal()) {
        QJsonObject summary = localSummary(m_draft.repo, m_draft.id);
        if (summary.isEmpty())
            summary = QJsonObject{{QStringLiteral("repo"), m_draft.repo}, {QStringLiteral("id"), m_draft.id},
                                  {QStringLiteral("name"), m_draft.name}};
        runLocal(summary);
        return;
    }
    if (m_draft.hasDesktopSteps(catalog())) {
        runOnDesktop(m_draft);
        return;
    }
    runWorkflow(m_draft.id);
}

void WorkflowsFeature::renderEditorRuns()
{
    const QSignalBlocker block(m_editorRunTable);
    m_editorRunTable->setRowCount(0);
    const qint64 current = now();
    for (const QJsonValue &value : std::as_const(m_editorRuns)) {
        const QJsonObject run = value.toObject();
        const int row = m_editorRunTable->rowCount();
        m_editorRunTable->insertRow(row);
        const qint64 started = integer(run, "startedAt");
        const qint64 finished = integer(run, "finishedAt");
        m_editorRunTable->setItem(row, 0, cell(relativeTime(started, current), timeText(started)));
        m_editorRunTable->setItem(row, 1, statusCell(text(run, "status")));
        if (run.value(QStringLiteral("local")).toBool())
            m_editorRunTable->item(row, 1)->setText(localStatusLabel(text(run, "status")));
        m_editorRunTable->setItem(row, 2, cell(finished >= started && started > 0 ? durationText(finished - started)
                                                                                  : QStringLiteral("–")));
        m_editorRunTable->setItem(row, 3, cell(text(run, "trigger")));
        m_editorRunTable->setItem(row, 4, cell(runOutputText(run)));
    }
    if (m_editorRuns.isEmpty()) {
        m_editorRunSummary->setText(m_draft.id.isEmpty() ? QStringLiteral("Save the workflow, then run it.")
                                                         : QStringLiteral("No runs yet."));
        m_editorRunSteps->setRowCount(0);
    }
    renderStepList();
    renderEditorFacts();
}

void WorkflowsFeature::loadEditorRuns()
{
    m_editorRuns = QJsonArray();
    renderEditorRuns();
    if (m_draft.id.isEmpty())
        return;
    if (m_draft.isLocal()) {
        loadLocalRuns();
        return;
    }
    const QString id = m_draft.id;
    request(QByteArrayLiteral("GET"), path(QStringLiteral("/%1/runs").arg(id)), {},
            [this, id](bool ok, const QJsonObject &payload, const QString &error) {
        if (m_draft.id != id)
            return;
        if (!ok) {
            m_editorRunSummary->setText(QStringLiteral("Runs did not load: %1").arg(failureText(payload, error)));
            return;
        }
        m_editorRuns = payload.value(QStringLiteral("runs")).toArray();
        m_runsByWorkflow.insert(id, m_editorRuns);
        renderEditorRuns();
        if (!m_editorRuns.isEmpty())
            m_editorRunTable->selectRow(0);
    });
}

QString WorkflowsFeature::desktopCheckout() const
{
    if (!m_repositories)
        return {};
    QString fallback;
    for (const forkmesh::market::NativePackageRepository &repository : m_repositories()) {
        if (repository.path.isEmpty() ||
            !QFileInfo(QDir(repository.path).filePath(QStringLiteral("tools/free_disk_space.py"))).isFile())
            continue;
        if (repository.key == QLatin1String("forkmesh/forkmesh"))
            return repository.path;
        if (fallback.isEmpty())
            fallback = repository.path;
    }
    return fallback;
}

void WorkflowsFeature::runOnDesktop(const Draft &draft)
{
    if (m_busy)
        return;
    const QString checkout = desktopCheckout();
    if (checkout.isEmpty())
        return setStatus(QStringLiteral("Add the forkmesh/forkmesh repository with a local checkout on this desktop: "
                                        "the cleanup runs its tools/free_disk_space.py."),
                         true);
    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (python.isEmpty())
        return setStatus(QStringLiteral("python3 is not installed on this desktop."), true);
    if (window()->findChild<QProcess *>(QString::fromLatin1(kCleanupProcess), Qt::FindDirectChildrenOnly))
        return setStatus(QStringLiteral("Another disk cleanup step is still running on this desktop."), true);
    QStringList steps;
    for (const QString &id : draft.ordered()) {
        const QJsonObject node = draft.node(id);
        if (m_catalog.isDesktop(text(node, "type")) && !node.value(QStringLiteral("disabled")).toBool())
            steps << QStringLiteral("• ") + text(node, "name");
    }
    if (QMessageBox::question(this, QStringLiteral("Run on this desktop"),
                              QStringLiteral("Run \"%1\" on this computer now? It runs tools/free_disk_space.py from "
                                             "%2 with --execute, one step at a time:\n\n%3")
                                  .arg(draft.name, checkout, steps.join(QLatin1Char('\n'))))
        != QMessageBox::Yes)
        return;
    m_desktopRun = DesktopRun();
    m_desktopRun.draft = draft;
    m_desktopRun.order = draft.ordered();
    m_desktopRun.checkout = checkout;
    m_desktopRun.python = python;
    m_desktopRun.startedAt = now();
    m_busy = true;
    setDirty(m_dirty);
    startPendingRun(draft.id, draft.name, QStringLiteral("on this desktop"), pendingSteps(draft));
    runDesktopStep(0);
}

void WorkflowsFeature::runDesktopStep(int index)
{
    DesktopRun &run = m_desktopRun;
    if (index >= run.order.size()) {
        finishDesktopRun();
        return;
    }
    const QString id = run.order.at(index);
    const QJsonObject node = run.draft.node(id);
    const QString type = text(node, "type");
    const QJsonObject passed{{QStringLiteral("status"), QStringLiteral("success")},
                             {QStringLiteral("ms"), 0},
                             {QStringLiteral("outputs"),
                              QJsonObject{{QStringLiteral("main"),
                                           QJsonObject{{QStringLiteral("count"), 1},
                                                       {QStringLiteral("items"), QJsonArray{QJsonObject()}}}}}}};
    if (m_catalog.isTrigger(type)) {
        run.report.insert(id, passed);
        runDesktopStep(index + 1);
        return;
    }
    bool fed = false;
    for (const QJsonValue &value : run.draft.inputs(id))
        fed = fed || text(run.report.value(text(value.toObject(), "from")).toObject(), "status") ==
                         QLatin1String("success");
    if (!run.failed.isEmpty() || !fed) {
        run.report.insert(id, QJsonObject{{QStringLiteral("status"), QStringLiteral("skipped")}});
        runDesktopStep(index + 1);
        return;
    }
    if (node.value(QStringLiteral("disabled")).toBool() || !m_catalog.isDesktop(type)) {
        run.report.insert(id, passed);
        runDesktopStep(index + 1);
        return;
    }
    const QString step = m_catalog.desktopStep(type);
    setStatus(QStringLiteral("%1… (step %2 of %3)").arg(text(node, "name")).arg(index).arg(run.order.size() - 1));
    auto *process = new QProcess(window());
    process->setObjectName(QString::fromLatin1(kCleanupProcess));
    process->setWorkingDirectory(run.checkout);
    process->setProcessChannelMode(QProcess::MergedChannels);
    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, process, [process] { process->kill(); });
    connect(this, &QObject::destroyed, process, [process] {
        process->kill();
        process->deleteLater();
    });
    const qint64 started = now();
    auto settle = [this, index, id, step, started, process](bool finishedOk, bool startFailed) {
        process->setObjectName(QString());
        process->deleteLater();
        DesktopRun &current = m_desktopRun;
        const QByteArray output = process->readAll();
        const QJsonObject reply = finishedOk ? freeDiskSpaceReply(step, output, now() - started) : QJsonObject();
        const QString name = text(current.draft.node(id), "name");
        if (reply.isEmpty()) {
            const QString message = startFailed ? QStringLiteral("python3 could not start.")
                                                : freeDiskSpaceFailure(output);
            current.failed = QStringLiteral("%1: %2").arg(name, message);
            current.report.insert(id, QJsonObject{{QStringLiteral("status"), QStringLiteral("error")},
                                                  {QStringLiteral("ms"), double(now() - started)},
                                                  {QStringLiteral("error"), message}});
            runDesktopStep(index + 1);
            return;
        }
        const double freed = reply.value(QStringLiteral("freedBytes")).toDouble();
        current.freed += freed;
        if (reply.value(QStringLiteral("totalBytes")).toDouble() > 0) {
            current.free = reply.value(QStringLiteral("freeBytes")).toDouble();
            current.total = reply.value(QStringLiteral("totalBytes")).toDouble();
        }
        const QJsonObject item{{QStringLiteral("step"), step},
                               {QStringLiteral("freedBytes"), freed},
                               {QStringLiteral("freed"), humanBytes(freed)},
                               {QStringLiteral("items"), reply.value(QStringLiteral("items")).toDouble()},
                               {QStringLiteral("freeBytes"), reply.value(QStringLiteral("freeBytes")).toDouble()},
                               {QStringLiteral("free"), humanBytes(reply.value(QStringLiteral("freeBytes")).toDouble())}};
        current.report.insert(id, QJsonObject{
                                      {QStringLiteral("status"), QStringLiteral("success")},
                                      {QStringLiteral("ms"), double(now() - started)},
                                      {QStringLiteral("outputs"),
                                       QJsonObject{{QStringLiteral("main"),
                                                    QJsonObject{{QStringLiteral("count"), 1},
                                                                {QStringLiteral("items"), QJsonArray{item}}}}}},
                                      {QStringLiteral("logs"), reply.value(QStringLiteral("lines"))}});
        runDesktopStep(index + 1);
    };
    connect(process, &QProcess::finished, this, [settle](int exitCode, QProcess::ExitStatus status) {
        settle(status == QProcess::NormalExit && exitCode == 0, false);
    });
    connect(process, &QProcess::errorOccurred, this, [settle](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            settle(false, true);
    });
    timer->start(int(kDesktopStepTimeoutMs));
    updateDesktopPending(index);
    process->start(run.python, freeDiskSpaceArguments(run.checkout, step));
}

void WorkflowsFeature::finishDesktopRun()
{
    const DesktopRun run = m_desktopRun;
    QJsonObject output{{QStringLiteral("freedBytes"), run.freed}};
    if (run.total > 0) {
        output.insert(QStringLiteral("freeBytes"), run.free);
        output.insert(QStringLiteral("totalBytes"), run.total);
    }
    const qint64 finished = now();
    const QString id = run.draft.id;
    updateDesktopPending(-1);
    request(QByteArrayLiteral("POST"), path(QStringLiteral("/%1/record").arg(id)),
            QJsonObject{{QStringLiteral("startedAt"), double(run.startedAt)},
                        {QStringLiteral("finishedAt"), double(finished)},
                        {QStringLiteral("nodes"), run.report},
                        {QStringLiteral("output"), output}},
            [this, id, run, finished](bool ok, const QJsonObject &payload, const QString &error) {
        m_busy = false;
        setDirty(m_dirty);
        if (!ok) {
            m_pendingRun = QJsonObject();
            stopFollowing();
            setStatus(QStringLiteral("The run finished but was not saved: %1").arg(failureText(payload, error)), true);
        } else {
            const QJsonObject saved = payload.value(QStringLiteral("run")).toObject();
            landPendingRun(saved, id);
            if (text(saved, "status") == QLatin1String("success"))
                setStatus(QStringLiteral("Done in %1. %2")
                              .arg(durationText(finished - run.startedAt), runOutputText(saved))
                              .trimmed());
            else
                setStatus(QStringLiteral("Workflow failed. %1").arg(text(saved, "error")).trimmed(), true);
        }
        if (m_pages->currentIndex() == 1 && m_draft.id == id)
            loadEditorRuns();
        renderDashboard();
        refresh();
    });
}

void WorkflowsFeature::exportDraft()
{
    if (m_draft.isLocal()) {
        localAction(m_draft.repo, m_draft.id, QStringLiteral("openFile"), {}, {});
        return;
    }
    request(QByteArrayLiteral("POST"), path(QStringLiteral("/export")), m_draft.graph(),
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The file was not made: %1").arg(failureText(payload, error)), true);
        const QString suggested = QDir(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation))
                                      .filePath(text(payload, "filename").isEmpty() ? QStringLiteral("workflow.json")
                                                                                    : text(payload, "filename"));
        const QString target = QFileDialog::getSaveFileName(this, QStringLiteral("Download workflow"), suggested,
                                                            QStringLiteral("Workflow JSON (*.json)"));
        if (target.isEmpty())
            return;
        const QJsonValue document = payload.value(QStringLiteral("document"));
        const QByteArray bytes = document.isObject()
                                     ? QJsonDocument(document.toObject()).toJson(QJsonDocument::Indented)
                                     : document.toString().toUtf8();
        QSaveFile file(target);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            return setStatus(QStringLiteral("Could not write %1.").arg(target), true);
        setStatus(QStringLiteral("Saved %1.").arg(QDir::toNativeSeparators(target)));
    });
}

void WorkflowsFeature::deleteDraft()
{
    if (m_draft.id.isEmpty()) {
        setDirty(false);
        showDashboard();
        return;
    }
    if (m_draft.isLocal()) {
        if (QMessageBox::warning(this, QStringLiteral("Delete workflow"),
                                 QStringLiteral("Delete \"%1\"? This removes .forkmesh/workflows/%2.json from %3%4.")
                                     .arg(m_draft.name, m_draft.id, m_draft.repo,
                                          m_draft.hasAgentStep() ? QStringLiteral(", and with it the special agent")
                                                                 : QString()),
                                 QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        localAction(m_draft.repo, m_draft.id, QStringLiteral("delete"), {},
                    [this](bool ok, const QJsonObject &, const QString &message) {
            setStatus(message, !ok);
            if (!ok)
                return;
            setDirty(false);
            showDashboard();
        });
        return;
    }
    deleteWorkflow(m_draft.id, m_draft.name);
}

void WorkflowsFeature::rotateHook()
{
    if (m_draft.id.isEmpty() || m_busy)
        return;
    if (QMessageBox::question(this, QStringLiteral("New webhook URL"),
                              QStringLiteral("Make a new secret URL? The old one stops working at once."))
        != QMessageBox::Yes)
        return;
    QJsonObject body = m_draft.body();
    body.insert(QStringLiteral("rotateHook"), true);
    request(QByteArrayLiteral("POST"), path(QStringLiteral("/") + m_draft.id), body,
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The URL was not changed: %1").arg(failureText(payload, error)), true);
        const QString selected = m_selectedStep;
        m_draft = Draft::fromJson(payload.value(QStringLiteral("workflow")).toObject());
        m_selectedStep = selected;
        setDirty(false);
        renderEditor();
        setStatus(QStringLiteral("New webhook URL made; the old one no longer works."));
    });
}

Catalog &WorkflowsFeature::catalog()
{
    return m_draft.isLocal() ? m_localCatalog : m_catalog;
}

bool WorkflowsFeature::localAvailable() const
{
    return bool(m_repositoryWorkflows) && bool(m_repositoryWorkflowAction);
}

QIcon WorkflowsFeature::faceIcon(const QString &base64) const
{
    if (base64.isEmpty())
        return {};
    QPixmap pixmap;
    pixmap.loadFromData(QByteArray::fromBase64(base64.toLatin1()), "PNG");
    return QIcon(pixmap);
}

void WorkflowsFeature::loadLocal()
{
    if (!localAvailable())
        return;
    m_local = m_repositoryWorkflows();
    if (m_localCatalog.isEmpty())
        m_localCatalog.load(m_local.value(QStringLiteral("catalog")).toObject());
    m_localWorkflows = m_local.value(QStringLiteral("workflows")).toArray();
    m_localRuns = QJsonArray();
    for (const QJsonValue &value : std::as_const(m_localWorkflows)) {
        const QJsonObject summary = value.toObject();
        for (const QJsonValue &run : summary.value(QStringLiteral("recentRuns")).toArray())
            m_localRuns.append(localRunRow(run.toObject(), text(summary, "repo"), text(summary, "name")));
    }
    rebuildRunFilter();
    renderDashboard();
    if (m_searchIndexChanged)
        m_searchIndexChanged();
    if (m_navigationBadgeChanged)
        m_navigationBadgeChanged();
}

void WorkflowsFeature::repositoryWorkflowsChanged()
{
    loadLocal();
    if (m_pages->currentIndex() == 1 && m_draft.isLocal()) {
        if (!m_dirty)
            renderLocalEditorBar();
        if (!m_draft.id.isEmpty())
            loadLocalRuns();
    }
}

void WorkflowsFeature::rebuildRunFilter()
{
    const QString chosen = m_runWorkflowFilter->currentData().toString();
    const QSignalBlocker block(m_runWorkflowFilter);
    m_runWorkflowFilter->clear();
    m_runWorkflowFilter->addItem(QStringLiteral("All workflows"), QString());
    for (const QJsonValue &value : std::as_const(m_localWorkflows)) {
        const QJsonObject summary = value.toObject();
        m_runWorkflowFilter->addItem(faceIcon(text(summary, "face")), text(summary, "name"),
                                     QStringLiteral("local:%1|%2").arg(text(summary, "repo"), text(summary, "id")));
    }
    for (const QJsonValue &value : std::as_const(m_workflows)) {
        const QJsonObject summary = value.toObject();
        m_runWorkflowFilter->addItem(text(summary, "name"), text(summary, "id"));
    }
    m_runWorkflowFilter->setCurrentIndex(std::max(0, m_runWorkflowFilter->findData(chosen)));
}

QJsonObject WorkflowsFeature::localSummary(const QString &repo, const QString &id) const
{
    for (const QJsonValue &value : m_localWorkflows) {
        const QJsonObject summary = value.toObject();
        if (text(summary, "repo").compare(repo, Qt::CaseInsensitive) == 0 && text(summary, "id") == id)
            return summary;
    }
    return {};
}

QJsonArray WorkflowsFeature::repositoryShortcuts(const QString &repo) const
{
    for (const QJsonValue &value : m_local.value(QStringLiteral("repositories")).toArray())
        if (text(value.toObject(), "key").compare(repo, Qt::CaseInsensitive) == 0)
            return value.toObject().value(QStringLiteral("shortcuts")).toArray();
    return {};
}

QString WorkflowsFeature::uniqueLocalId(const QString &repo, const QString &name) const
{
    const QString base = localWorkflowId(name);
    QString candidate = base;
    for (int suffix = 2; !localSummary(repo, candidate).isEmpty(); ++suffix)
        candidate = QStringLiteral("%1-%2").arg(base.left(58)).arg(suffix);
    return candidate;
}

void WorkflowsFeature::localAction(const QString &repo, const QString &id, const QString &action,
                                   const QJsonObject &payload, ReplyHandler done)
{
    if (!localAvailable()) {
        if (done)
            done(false, {}, QStringLiteral("This desktop cannot run repository workflows."));
        return;
    }
    const QPointer<WorkflowsFeature> guard(this);
    m_repositoryWorkflowAction(repo, id, action, payload,
                               [guard, done = std::move(done)](bool ok, const QJsonObject &data, const QString &message) {
        if (!guard)
            return;
        if (done)
            done(ok, data, message);
        else if (!ok && !message.isEmpty())
            guard->setStatus(message, true);
    });
}

void WorkflowsFeature::runLocal(const QJsonObject &summary, const QString &assignment)
{
    const QString repo = text(summary, "repo");
    const QString id = text(summary, "id");
    if (repo.isEmpty() || id.isEmpty())
        return;
    setStatus(QStringLiteral("Starting %1…").arg(text(summary, "name")));
    QJsonObject payload{{QStringLiteral("trigger"), QStringLiteral("By hand · Workflows page")}};
    if (!assignment.isEmpty())
        payload.insert(QStringLiteral("assignment"), assignment);
    const QString key = QStringLiteral("local:%1|%2").arg(repo, id);
    startPendingRun(key, text(summary, "name"), QStringLiteral("on this desktop"),
                    m_draft.isLocal() && m_draft.repo == repo && m_draft.id == id ? pendingSteps(m_draft)
                                                                                  : QJsonArray());
    localAction(repo, id, QStringLiteral("run"), payload,
                [this, key, repo, id, summary, assignment](bool ok, const QJsonObject &data, const QString &message) {
        const QString runId = text(data, "runId");
        const bool pending =
            text(m_pendingRun, "workflowId") == key && m_pendingRun.value(QStringLiteral("pending")).toBool();
        if (!ok && runId.isEmpty() && text(data, "code") == QLatin1String("approval_required")) {
            setStatus(QStringLiteral("%1 needs your approval on this desktop before it runs.").arg(text(summary, "name")));
            if (pending) {
                m_pendingRun.insert(QStringLiteral("log"),
                                    QStringLiteral("▶ %1 is waiting for your approval.\n%2")
                                        .arg(text(summary, "name"), message));
                if (text(selectedDashboardRun(), "id") == text(m_pendingRun, "id"))
                    showPendingRun(m_pendingRun);
            }
            localAction(repo, id, QStringLiteral("review"), {},
                        [this, key, summary, assignment](bool approved, const QJsonObject &, const QString &reply) {
                if (approved) {
                    runLocal(summary, assignment);
                    return;
                }
                setStatus(reply, true);
                failPendingRun(key, reply);
                loadLocal();
                if (m_followWorkflow == key)
                    stopFollowing();
            });
            return;
        }
        setStatus(message, !ok);
        if (m_followWorkflow == key)
            m_followRun = runId;
        if (pending) {
            if (!runId.isEmpty()) {
                m_flowAliasFrom = text(m_pendingRun, "id");
                m_flowAliasTo = runId;
                m_pendingRun.insert(QStringLiteral("runId"), runId);
            } else {
                failPendingRun(key, message);
            }
        }
        loadLocal();
        if (runId.isEmpty() && m_followWorkflow == key)
            stopFollowing();
    });
}

void WorkflowsFeature::showPinMenu(const QJsonObject &entry, const QPoint &globalPos)
{
    const QString repo = text(entry, "repo");
    const QString id = text(entry, "id");
    if (repo.isEmpty() || id.isEmpty())
        return;
    const QString current = text(entry, "pinned");
    const auto done = [this](bool ok, const QJsonObject &, const QString &message) {
        setStatus(message, !ok);
        loadLocal();
        if (m_pages->currentIndex() == 1)
            renderLocalEditorBar();
    };
    QMenu menu(this);
    for (const auto &rail : {QPair<QString, QString>{QStringLiteral("top"), QStringLiteral("Top rail")},
                             {QStringLiteral("left"), QStringLiteral("Left rail")},
                             {QStringLiteral("right"), QStringLiteral("Right rail")},
                             {QStringLiteral("bottom"), QStringLiteral("Bottom rail")}}) {
        QAction *action = menu.addAction(rail.second);
        action->setCheckable(true);
        action->setChecked(current == rail.first);
        connect(action, &QAction::triggered, this, [this, entry, repo, id, done, value = rail.first] {
            QJsonObject payload{{QStringLiteral("rail"), value}};
            if (repo == QLatin1String(kAccountRepo)) {
                payload.insert(QStringLiteral("name"), text(entry, "name"));
                payload.insert(QStringLiteral("lastStatus"), text(entry, "lastStatus"));
            }
            localAction(repo, id, QStringLiteral("pin"), payload, done);
        });
    }
    if (!current.isEmpty()) {
        menu.addSeparator();
        connect(menu.addAction(QStringLiteral("Unpin")), &QAction::triggered, this,
                [this, repo, id, done] { localAction(repo, id, QStringLiteral("unpin"), {}, done); });
    }
    menu.exec(globalPos);
}

void WorkflowsFeature::openLocalWorkflow(const QString &repo, const QString &id)
{
    if (repo.isEmpty() || id.isEmpty())
        return;
    if (m_pages->currentIndex() == 1 && m_draft.isLocal() && m_draft.repo == repo && m_draft.id == id)
        return;
    if (m_pages->currentIndex() == 1 && !leaveEditor())
        return;
    loadLocal();
    setStatus(QStringLiteral("Opening the workflow…"));
    localAction(repo, id, QStringLiteral("get"), {},
                [this, repo](bool ok, const QJsonObject &payload, const QString &message) {
        if (!ok)
            return setStatus(message, true);
        openDraft(Draft::fromLocal(payload.value(QStringLiteral("workflow")).toObject(), repo), false);
        if (m_pendingAgentStep == QStringLiteral("local:%1|%2").arg(repo, m_draft.id)) {
            m_pendingAgentStep.clear();
            const QStringList order = m_draft.ordered();
            const QString after = order.isEmpty() ? QString() : order.last();
            const QString added = m_draft.addStep(m_localCatalog, QStringLiteral("agent.run"), after);
            if (!added.isEmpty()) {
                QJsonObject node = m_draft.node(added);
                QJsonObject params = node.value(QStringLiteral("params")).toObject();
                params.insert(QStringLiteral("specialty"), QStringLiteral("Special agent"));
                params.insert(QStringLiteral("permissions"), QJsonArray{QStringLiteral("read-repositories")});
                node.insert(QStringLiteral("params"), params);
                m_draft.updateNode(node);
                setDirty(true);
                renderEditor();
                selectStep(added);
                setStatus(QStringLiteral("Added a special agent step to %1. Give it a role and instructions, then Save.")
                              .arg(m_draft.name));
                return;
            }
        }
        if (payload.value(QStringLiteral("legacy")).toBool())
            setStatus(QStringLiteral("%1 is still a .forkmesh/teammates file. Save moves it to .forkmesh/workflows.")
                          .arg(m_draft.name));
        else if (!text(payload, "error").isEmpty())
            setStatus(text(payload, "error"), true);
        else
            setStatus(QStringLiteral("Editing %1.").arg(m_draft.name));
    });
}

void WorkflowsFeature::loadLocalRuns()
{
    const QString repo = m_draft.repo;
    const QString id = m_draft.id;
    localAction(repo, id, QStringLiteral("get"), {},
                [this, repo, id](bool ok, const QJsonObject &payload, const QString &message) {
        if (!m_draft.isLocal() || m_draft.repo != repo || m_draft.id != id)
            return;
        if (!ok) {
            m_editorRunSummary->setText(QStringLiteral("Runs did not load: %1").arg(message));
            return;
        }
        const int previous = m_editorRunTable->currentRow();
        m_editorRuns = QJsonArray();
        if (payload.contains(QStringLiteral("active")))
            m_editorRuns.append(localRunRow(payload.value(QStringLiteral("active")).toObject(), repo, m_draft.name));
        for (const QJsonValue &value : payload.value(QStringLiteral("runs")).toArray()) {
            const QJsonObject run = value.toObject();
            if (payload.contains(QStringLiteral("active")) &&
                text(run, "id") == text(payload.value(QStringLiteral("active")).toObject(), "id"))
                continue;
            m_editorRuns.append(localRunRow(run, repo, m_draft.name));
        }
        renderEditorRuns();
        if (!m_editorRuns.isEmpty())
            m_editorRunTable->selectRow(std::clamp(previous, 0, int(m_editorRuns.size()) - 1));
    });
}

QJsonObject WorkflowsFeature::localRunRow(const QJsonObject &run, const QString &repo,
                                          const QString &workflowName) const
{
    QString summary = text(run, "caption").trimmed();
    if (summary.isEmpty() && text(run, "status") == QLatin1String("error"))
        summary = text(run, "error").section(QLatin1Char('\n'), 0, 0);
    if (summary.isEmpty()) {
        for (const QString &line : text(run, "output").split(QLatin1Char('\n'))) {
            QString clean = line.trimmed();
            while (clean.startsWith(QLatin1Char('#')))
                clean.remove(0, 1);
            if (!clean.trimmed().isEmpty()) {
                summary = clean.trimmed().left(200);
                break;
            }
        }
    }
    return {{QStringLiteral("id"), text(run, "id")},
            {QStringLiteral("local"), true},
            {QStringLiteral("repo"), repo},
            {QStringLiteral("localWorkflowId"), text(run, "workflowId")},
            {QStringLiteral("workflowId"), QStringLiteral("local:%1|%2").arg(repo, text(run, "workflowId"))},
            {QStringLiteral("workflowName"), text(run, "workflowName").isEmpty() ? workflowName
                                                                                 : text(run, "workflowName")},
            {QStringLiteral("startedAt"), run.value(QStringLiteral("startedAtMs"))},
            {QStringLiteral("finishedAt"), run.value(QStringLiteral("finishedAtMs"))},
            {QStringLiteral("trigger"), text(run, "trigger")},
            {QStringLiteral("status"), text(run, "status")},
            {QStringLiteral("error"), text(run, "error")},
            {QStringLiteral("caption"), text(run, "caption")},
            {QStringLiteral("assignment"), text(run, "assignment")},
            {QStringLiteral("outputFile"), text(run, "outputFile")},
            {QStringLiteral("output"), QJsonObject{{QStringLiteral("text"), summary}}},
            {QStringLiteral("steps"), run.value(QStringLiteral("steps"))}};
}

void WorkflowsFeature::showLocalRunDetail(QTableWidget *steps, QLabel *summary, const QJsonObject &run)
{
    const qint64 started = integer(run, "startedAt");
    const qint64 finished = integer(run, "finishedAt");
    QStringList parts{localStatusLabel(text(run, "status"))};
    if (started > 0)
        parts << timeText(started);
    if (finished >= started && started > 0)
        parts << durationText(finished - started);
    if (!text(run, "trigger").isEmpty())
        parts << QStringLiteral("trigger: %1").arg(text(run, "trigger"));
    QString headline = parts.join(QStringLiteral(" · ")).toHtmlEscaped();
    const QString output = runOutputText(run);
    if (!output.isEmpty())
        headline += QStringLiteral("<br>") + output.toHtmlEscaped();
    if (!text(run, "assignment").isEmpty())
        headline += QStringLiteral("<br>Assignment: ") + text(run, "assignment").left(240).toHtmlEscaped();
    QStringList links;
    if (!text(run, "outputFile").isEmpty())
        links << QStringLiteral("<a href=\"output\">Open the output file</a>");
    int session = 0;
    for (const QJsonValue &value : run.value(QStringLiteral("steps")).toArray())
        session = value.toObject().value(QStringLiteral("sessionId")).toInt() > 0
                      ? value.toObject().value(QStringLiteral("sessionId")).toInt()
                      : session;
    if (session > 0)
        links << QStringLiteral("<a href=\"session\">Show agent session #%1</a>").arg(session);
    if (!links.isEmpty())
        headline += QStringLiteral("<br>") + links.join(QStringLiteral(" · "));
    summary->setTextFormat(Qt::RichText);
    summary->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    summary->setText(headline);
    summary->disconnect(this);
    const QString repo = text(run, "repo");
    const QString id = text(run, "localWorkflowId");
    const QString file = text(run, "outputFile");
    connect(summary, &QLabel::linkActivated, this, [this, repo, id, file, session](const QString &link) {
        if (link == QLatin1String("output"))
            localAction(repo, id, QStringLiteral("openOutput"), {{QStringLiteral("path"), file}}, {});
        else if (link == QLatin1String("session"))
            localAction(repo, id, QStringLiteral("openSession"), {{QStringLiteral("sessionId"), session}}, {});
    });
    steps->setRowCount(0);
    if (QTableWidgetItem *header = steps->horizontalHeaderItem(3))
        header->setText(QStringLiteral("Session"));
    for (const QJsonValue &value : run.value(QStringLiteral("steps")).toArray()) {
        const QJsonObject entry = value.toObject();
        const int row = steps->rowCount();
        steps->insertRow(row);
        steps->setItem(row, 0, cell(text(entry, "name")));
        steps->setItem(row, 1, statusCell(text(entry, "status")));
        steps->item(row, 1)->setText(localStatusLabel(text(entry, "status")));
        const qint64 begun = integer(entry, "startedAtMs");
        const qint64 ended = integer(entry, "finishedAtMs");
        steps->setItem(row, 2, cell(begun > 0 && ended >= begun ? durationText(ended - begun) : QStringLiteral("–")));
        const int stepSession = entry.value(QStringLiteral("sessionId")).toInt();
        steps->setItem(row, 3, cell(stepSession > 0 ? QStringLiteral("#%1").arg(stepSession) : QStringLiteral("–"),
                                    stepSession > 0 ? QStringLiteral("Agent session") : QString()));
        QString details = text(entry, "error");
        if (details.isEmpty())
            details = text(entry, "output").trimmed().section(QLatin1Char('\n'), -1);
        QStringList logs;
        for (const QJsonValue &line : entry.value(QStringLiteral("logs")).toArray())
            logs << line.toString();
        QString tooltip = text(entry, "output").trimmed().left(1500);
        if (!text(entry, "error").isEmpty())
            tooltip = text(entry, "error") + QStringLiteral("\n\n") + tooltip;
        if (!logs.isEmpty())
            tooltip += QStringLiteral("\n\n") + logs.join(QLatin1Char('\n'));
        steps->setItem(row, 4, cell(details.left(240), tooltip.trimmed()));
    }
    if (steps == m_runSteps)
        showFlow(text(run, "id"), flowSteps(run.value(QStringLiteral("steps")).toArray()),
                 liveStatus(text(run, "status")));
}

void WorkflowsFeature::renderLocalEditorBar()
{
    QWidget *bar = m_description ? m_description->parentWidget() : nullptr;
    const bool local = m_draft.isLocal();
    if (bar)
        bar->setVisible(local);
    m_active->setText(local ? QStringLiteral("Turned on") : QStringLiteral("Active"));
    m_active->setToolTip(local ? QStringLiteral("A workflow that is turned off never runs, by hand or on its schedule")
                               : QStringLiteral("Active workflows answer their webhook and run on their schedule"));
    if (!local)
        return;
    const bool agent = m_draft.hasAgentStep();
    {
        const QSignalBlocker block(m_iconCombo);
        m_iconCombo->clear();
        for (const char *group : {"icons", "avatars"}) {
            for (const QJsonValue &value : m_local.value(QLatin1String(group)).toArray()) {
                const QJsonObject entry = value.toObject();
                m_iconCombo->addItem(faceIcon(text(entry, "face")), text(entry, "label"), text(entry, "value"));
            }
        }
        int index = m_iconCombo->findData(m_draft.icon);
        if (index < 0 && !m_draft.icon.isEmpty()) {
            m_iconCombo->addItem(m_draft.icon, m_draft.icon);
            index = m_iconCombo->count() - 1;
        }
        m_iconCombo->setCurrentIndex(std::max(0, index));
    }
    m_iconCombo->setVisible(!agent);
    const QJsonObject listed = localSummary(m_draft.repo, m_draft.id);
    const QIcon portrait = agent ? faceIcon(text(listed, "face")) : QIcon();
    if (!portrait.isNull()) {
        m_iconLabel->setPixmap(portrait.pixmap(QSize(26, 26)));
    } else {
        m_iconLabel->setPixmap(QPixmap());
        m_iconLabel->setText(agent ? QStringLiteral("Portrait") : QStringLiteral("Icon"));
    }
    m_iconLabel->setToolTip(agent ? QStringLiteral("Its icon is the special agent's portrait") : QString());
    if (m_description->text() != m_draft.description)
        m_description->setText(m_draft.description);
    const bool saved = !m_draft.id.isEmpty();
    m_editorAgentButton->setVisible(agent);
    m_editorAgentButton->setEnabled(saved);
    const QString pinned = text(localSummary(m_draft.repo, m_draft.id), "pinned");
    m_editorPinButton->setText(pinned.isEmpty() ? QStringLiteral("Pin to rail")
                                                : QStringLiteral("Pinned: %1").arg(pinned));
    m_editorPinButton->setEnabled(saved);
}

void WorkflowsFeature::startLocalDraft(const QString &kind, const QString &repo)
{
    if (repo.isEmpty())
        return setStatus(QStringLiteral("Add a repository with a checkout on this desktop first."), true);
    if (m_localCatalog.isEmpty())
        loadLocal();
    Draft draft;
    draft.repo = repo;
    draft.active = true;
    draft.icon = QStringLiteral("workflow");
    const auto add = [&draft, this](const QString &type, const QString &after) {
        return draft.addStep(m_localCatalog, type, after);
    };
    if (kind == QLatin1String("agent")) {
        draft.name = QStringLiteral("New special agent");
        draft.addTrigger(m_localCatalog, QStringLiteral("workflow.schedule"));
        const QString schedule = text(draft.nodes.first().toObject(), "id");
        const QString agent = add(QStringLiteral("agent.run"), schedule);
        QJsonObject node = draft.node(agent);
        QJsonObject params = node.value(QStringLiteral("params")).toObject();
        params.insert(QStringLiteral("specialty"), QStringLiteral("Special agent"));
        params.insert(QStringLiteral("permissions"), QJsonArray{QStringLiteral("read-repositories")});
        params.insert(QStringLiteral("selfImproving"), true);
        node.insert(QStringLiteral("params"), params);
        node.insert(QStringLiteral("name"), QStringLiteral("New special agent"));
        draft.updateNode(node);
        add(QStringLiteral("workflow.output"), agent);
        draft.icon = QStringLiteral("sol");
    } else if (kind == QLatin1String("shortcut")) {
        const QJsonArray shortcuts = repositoryShortcuts(repo);
        QStringList labels;
        for (const QJsonValue &value : shortcuts)
            labels << QStringLiteral("%1 (%2)").arg(text(value.toObject(), "name"), text(value.toObject(), "file"));
        if (labels.isEmpty())
            return setStatus(QStringLiteral("%1 has no shortcuts in .forkmesh/shortcuts yet.").arg(repo), true);
        bool ok = false;
        const QString picked = QInputDialog::getItem(this, QStringLiteral("Run a shortcut"),
                                                     QStringLiteral("Shortcut to run:"), labels, 0, false, &ok);
        if (!ok)
            return;
        const QJsonObject shortcut = shortcuts.at(int(labels.indexOf(picked))).toObject();
        draft.name = text(shortcut, "name").left(80);
        draft.icon = text(shortcut, "kind") == QLatin1String("script") ? QStringLiteral("terminal")
                                                                        : QStringLiteral("sparkle");
        draft.description = text(shortcut, "description");
        draft.addTrigger(m_localCatalog, QStringLiteral("workflow.manual"));
        const QString step = add(QStringLiteral("workflow.shortcut"), text(draft.nodes.first().toObject(), "id"));
        QJsonObject node = draft.node(step);
        QJsonObject params = node.value(QStringLiteral("params")).toObject();
        params.insert(QStringLiteral("file"), text(shortcut, "file"));
        node.insert(QStringLiteral("params"), params);
        node.insert(QStringLiteral("name"), draft.uniqueName(text(shortcut, "name").left(56), step));
        draft.updateNode(node);
        add(QStringLiteral("workflow.output"), step);
    } else if (kind == QLatin1String("cleanup")) {
        draft.name = QStringLiteral("Free disk space");
        draft.icon = QStringLiteral("server");
        draft.addTrigger(m_localCatalog, QStringLiteral("workflow.manual"));
        QString previous = text(draft.nodes.first().toObject(), "id");
        for (const QString &type : {QStringLiteral("desktop.diskUsage"), QStringLiteral("desktop.cleanAgents"),
                                    QStringLiteral("desktop.cleanTests"), QStringLiteral("desktop.cleanVersions"),
                                    QStringLiteral("desktop.cleanCaches"), QStringLiteral("desktop.diskReport")})
            previous = add(type, previous);
    } else {
        draft.name = QStringLiteral("New desktop workflow");
        draft.addTrigger(m_localCatalog, QStringLiteral("workflow.manual"));
    }
    openDraft(draft, true);
    setStatus(QStringLiteral("New workflow in %1. Add steps, then Save.").arg(repo));
}

} // namespace forkmesh::workflows

namespace {
const forkmesh::market::NativePackageRegistration workflowsRegistration({
    QStringLiteral("builtin:workflows"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Workflows"),
    QStringLiteral("workflow"),
    QStringLiteral("/api/workflows"),
    QStringLiteral("forkmesh-workflows-request-v1"),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::workflows::WorkflowsFeature(host, parent);
    },
});
} // namespace
