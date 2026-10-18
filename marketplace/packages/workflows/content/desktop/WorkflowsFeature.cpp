#include "WorkflowsFeature.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
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
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QSet>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QTableWidget>
#include <QTimer>
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

QTableWidget *makeTable(const QStringList &headers, const QString &name)
{
    auto *table = new QTableWidget(0, int(headers.size()));
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

} // namespace

WorkflowsFeature::WorkflowsFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent), m_request(host.request),
      m_searchIndexChanged(host.searchIndexChanged), m_navigationBadgeChanged(host.navigationBadgeChanged),
      m_repositories(host.repositories)
{
    setObjectName(QStringLiteral("workflowsSection"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Workflows"));
    title->setObjectName(QStringLiteral("pageTitle"));
    heading->addWidget(title);
    heading->addStretch();
    m_newButton = button(QStringLiteral("New workflow"), "primaryButton");
    m_newButton->setToolTip(QStringLiteral("Start blank, from an example, or import an n8n file or link"));
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
    layout->setSpacing(10);

    auto *kpis = new QGridLayout;
    kpis->setHorizontalSpacing(28);
    kpis->setVerticalSpacing(2);
    const QStringList names{QStringLiteral("Workflows"),    QStringLiteral("Active"),
                            QStringLiteral("Runs"),         QStringLiteral("Success rate"),
                            QStringLiteral("Avg. duration"), QStringLiteral("Last run"),
                            QStringLiteral("Next scheduled run")};
    for (int column = 0; column < names.size(); ++column) {
        kpis->addWidget(mutedLabel(names.at(column)), 0, column);
        auto *value = new QLabel(QStringLiteral("–"));
        value->setObjectName(QStringLiteral("workflowsKpi"));
        value->setAccessibleName(names.at(column));
        value->setStyleSheet(QStringLiteral("font-size: 22px; font-weight: 600;"));
        kpis->addWidget(value, 1, column);
        m_kpiValues.append(value);
        auto *note = mutedLabel(QString());
        note->setWordWrap(false);
        kpis->addWidget(note, 2, column);
        m_kpiNotes.append(note);
    }
    kpis->setColumnStretch(int(names.size()), 1);
    layout->addLayout(kpis);

    auto *splitter = new QSplitter(Qt::Vertical);
    splitter->setChildrenCollapsible(false);

    auto *top = new QWidget;
    auto *topLayout = new QVBoxLayout(top);
    topLayout->setContentsMargins(0, 6, 0, 0);
    topLayout->setSpacing(6);
    auto *listHeading = new QHBoxLayout;
    listHeading->addWidget(sectionLabel(QStringLiteral("Your workflows")));
    listHeading->addStretch();
    m_filter = new QLineEdit;
    m_filter->setPlaceholderText(QStringLiteral("Filter by name or trigger"));
    m_filter->setAccessibleName(QStringLiteral("Filter workflows"));
    m_filter->setClearButtonEnabled(true);
    m_filter->setMaximumWidth(260);
    listHeading->addWidget(m_filter);
    m_openButton = button(QStringLiteral("Edit"));
    m_runButton = button(QStringLiteral("Run"));
    m_toggleButton = button(QStringLiteral("Turn on"));
    m_duplicateButton = button(QStringLiteral("Duplicate"));
    m_deleteButton = button(QStringLiteral("Delete"), "dangerButton");
    for (QPushButton *item : {m_openButton, m_runButton, m_toggleButton, m_duplicateButton, m_deleteButton})
        listHeading->addWidget(item);
    topLayout->addLayout(listHeading);
    m_workflowTable = makeTable({QStringLiteral("Name"), QStringLiteral("Trigger"), QStringLiteral("Steps"),
                                 QStringLiteral("State"), QStringLiteral("Last run"), QStringLiteral("Last result"),
                                 QStringLiteral("Runs"), QStringLiteral("Next run"), QStringLiteral("Updated")},
                                QStringLiteral("workflowsTable"));
    topLayout->addWidget(m_workflowTable, 1);
    m_emptyNote = mutedLabel(QStringLiteral(
        "No workflows yet. Press New workflow to start blank, from an example, or from an n8n file."));
    m_emptyNote->setAlignment(Qt::AlignHCenter);
    m_emptyNote->setVisible(false);
    topLayout->addWidget(m_emptyNote);
    splitter->addWidget(top);

    auto *bottom = new QWidget;
    auto *bottomLayout = new QVBoxLayout(bottom);
    bottomLayout->setContentsMargins(0, 6, 0, 0);
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
    detailLayout->addWidget(m_runSteps, 1);
    runSplitter->addWidget(detail);
    runSplitter->setStretchFactor(0, 3);
    runSplitter->setStretchFactor(1, 2);
    bottomLayout->addWidget(runSplitter, 1);
    splitter->addWidget(bottom);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);

    connect(m_filter, &QLineEdit::textChanged, this, [this] { renderWorkflowTable(); });
    connect(m_workflowTable, &QTableWidget::itemSelectionChanged, this, [this] {
        const QJsonObject summary = selectedSummary();
        const bool chosen = !summary.isEmpty();
        for (QPushButton *item : {m_openButton, m_runButton, m_toggleButton, m_duplicateButton, m_deleteButton})
            item->setEnabled(chosen && !m_busy);
        m_toggleButton->setText(summary.value(QStringLiteral("active")).toBool() ? QStringLiteral("Turn off")
                                                                                 : QStringLiteral("Turn on"));
        m_toggleButton->setAccessibleName(m_toggleButton->text());
    });
    connect(m_workflowTable, &QTableWidget::cellDoubleClicked, this, [this] {
        openWorkflow(text(selectedSummary(), "id"));
    });
    connect(m_openButton, &QPushButton::clicked, this, [this] { openWorkflow(text(selectedSummary(), "id")); });
    connect(m_runButton, &QPushButton::clicked, this, [this] { runSummary(selectedSummary()); });
    connect(m_toggleButton, &QPushButton::clicked, this, [this] { toggleActive(selectedSummary()); });
    connect(m_duplicateButton, &QPushButton::clicked, this, [this] { duplicateWorkflow(selectedSummary()); });
    connect(m_deleteButton, &QPushButton::clicked, this, [this] {
        const QJsonObject summary = selectedSummary();
        deleteWorkflow(text(summary, "id"), text(summary, "name"));
    });
    connect(m_runWorkflowFilter, &QComboBox::currentIndexChanged, this, [this] { renderRunTable(); });
    connect(m_runStatusFilter, &QComboBox::currentIndexChanged, this, [this] { renderRunTable(); });
    connect(m_runTable, &QTableWidget::itemSelectionChanged, this, [this] {
        const QJsonObject run = selectedDashboardRun();
        if (run.isEmpty())
            return;
        showRunDetail(m_runSteps, m_runSummary, text(run, "workflowId"), text(run, "id"));
    });
    connect(m_runTable, &QTableWidget::cellDoubleClicked, this, [this] {
        openWorkflow(text(selectedDashboardRun(), "workflowId"));
    });
    for (QPushButton *item : {m_openButton, m_runButton, m_toggleButton, m_duplicateButton, m_deleteButton})
        item->setEnabled(false);
    return page;
}

QWidget *WorkflowsFeature::buildEditor()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto *bar = new QHBoxLayout;
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

    m_editorNote = mutedLabel(QString());
    m_editorNote->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    m_editorNote->setOpenExternalLinks(false);
    layout->addWidget(m_editorNote);

    auto *vertical = new QSplitter(Qt::Vertical);
    vertical->setChildrenCollapsible(false);
    auto *horizontal = new QSplitter(Qt::Horizontal);
    horizontal->setChildrenCollapsible(false);

    auto *left = new QWidget;
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(6);
    leftLayout->addWidget(sectionLabel(QStringLiteral("Steps")));
    m_steps = new QListWidget;
    m_steps->setObjectName(QStringLiteral("workflowsSteps"));
    m_steps->setAccessibleName(QStringLiteral("Steps in run order"));
    m_steps->setWordWrap(true);
    m_steps->setSpacing(2);
    m_steps->setMinimumWidth(240);
    leftLayout->addWidget(m_steps, 1);
    auto *stepButtons = new QHBoxLayout;
    m_addStepButton = button(QStringLiteral("Add step…"));
    m_addStepButton->setToolTip(QStringLiteral("Add a step after the selected one"));
    m_removeStepButton = button(QStringLiteral("Remove step"), "dangerButton");
    m_removeStepButton->setToolTip(QStringLiteral("Remove the selected step and join its inputs to its outputs"));
    stepButtons->addWidget(m_addStepButton);
    stepButtons->addWidget(m_removeStepButton);
    stepButtons->addStretch();
    leftLayout->addLayout(stepButtons);
    horizontal->addWidget(left);

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_formHost = new QWidget;
    new QVBoxLayout(m_formHost);
    scroll->setWidget(m_formHost);
    scroll->setMinimumWidth(380);
    horizontal->addWidget(scroll);
    horizontal->setStretchFactor(0, 2);
    horizontal->setStretchFactor(1, 3);
    horizontal->setSizes({400, 600});
    vertical->addWidget(horizontal);

    auto *runs = new QWidget;
    auto *runsLayout = new QVBoxLayout(runs);
    runsLayout->setContentsMargins(0, 6, 0, 0);
    runsLayout->setSpacing(6);
    runsLayout->addWidget(sectionLabel(QStringLiteral("Runs of this workflow")));
    auto *runSplitter = new QSplitter(Qt::Horizontal);
    runSplitter->setChildrenCollapsible(false);
    m_editorRunTable = makeTable({QStringLiteral("Started"), QStringLiteral("Trigger"), QStringLiteral("Result"),
                                  QStringLiteral("Duration"), QStringLiteral("Output")},
                                 QStringLiteral("workflowsEditorRuns"));
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
    detailLayout->addWidget(m_editorRunSteps, 1);
    runSplitter->addWidget(detail);
    runSplitter->setStretchFactor(0, 2);
    runSplitter->setStretchFactor(1, 3);
    runsLayout->addWidget(runSplitter, 1);
    vertical->addWidget(runs);
    vertical->setStretchFactor(0, 3);
    vertical->setStretchFactor(1, 2);
    vertical->setSizes({560, 280});
    layout->addWidget(vertical, 1);

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
        showRunDetail(m_editorRunSteps, m_editorRunSummary, m_draft.id,
                      text(m_editorRuns.at(row).toObject(), "id"));
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
            setStatus(m_workflows.isEmpty() ? QStringLiteral("No workflows yet.")
                                            : QStringLiteral("%1 workflows, updated %2.")
                                                  .arg(m_workflows.size())
                                                  .arg(QLocale().toString(QTime::currentTime(), QLocale::ShortFormat)));
        const QString chosen = m_runWorkflowFilter->currentData().toString();
        const QSignalBlocker block(m_runWorkflowFilter);
        m_runWorkflowFilter->clear();
        m_runWorkflowFilter->addItem(QStringLiteral("All workflows"), QString());
        for (const QJsonValue &value : std::as_const(m_workflows)) {
            const QJsonObject summary = value.toObject();
            m_runWorkflowFilter->addItem(text(summary, "name"), text(summary, "id"));
        }
        m_runWorkflowFilter->setCurrentIndex(std::max(0, m_runWorkflowFilter->findData(chosen)));
        renderDashboard();
        if (m_searchIndexChanged)
            m_searchIndexChanged();
        if (m_navigationBadgeChanged)
            m_navigationBadgeChanged();
        loadAllRuns();
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
    const Stats stats = computeStats(m_workflows, m_allRuns, now());
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
    m_newButton->setEnabled(m_workflows.size() < kMaxWorkflows);
    m_newButton->setToolTip(m_workflows.size() < kMaxWorkflows
                                ? QStringLiteral("Start blank, from an example, or import an n8n file or link")
                                : QStringLiteral("You have 50 workflows, the most an account keeps"));
    renderWorkflowTable();
    renderRunTable();
}

void WorkflowsFeature::renderWorkflowTable()
{
    const QString chosen = text(selectedSummary(), "id");
    const QString filter = m_filter->text().trimmed();
    const qint64 current = now();
    const QSignalBlocker block(m_workflowTable);
    m_workflowTable->setRowCount(0);
    for (const QJsonValue &value : std::as_const(m_workflows)) {
        const QJsonObject summary = value.toObject();
        const QString name = text(summary, "name");
        const QString trigger = triggerLabel(text(summary, "trigger"));
        if (!filter.isEmpty() && !name.contains(filter, Qt::CaseInsensitive) &&
            !trigger.contains(filter, Qt::CaseInsensitive))
            continue;
        const int row = m_workflowTable->rowCount();
        m_workflowTable->insertRow(row);
        auto *nameItem = cell(name);
        nameItem->setData(Qt::UserRole, text(summary, "id"));
        m_workflowTable->setItem(row, 0, nameItem);
        QString triggerText = trigger;
        if (text(summary, "trigger") == QLatin1String("core.schedule"))
            triggerText = everyText(int(integer(summary, "scheduleMinutes")));
        if (triggerText.isEmpty())
            triggerText = trigger;
        m_workflowTable->setItem(row, 1, cell(triggerText));
        m_workflowTable->setItem(row, 2, cell(QString::number(summary.value(QStringLiteral("nodes")).toInt())));
        const bool active = summary.value(QStringLiteral("active")).toBool();
        m_workflowTable->setItem(row, 3, cell(active ? QStringLiteral("On") : QStringLiteral("Off")));
        m_workflowTable->setItem(row, 4, cell(relativeTime(integer(summary, "lastRunAt"), current),
                                              timeText(integer(summary, "lastRunAt"))));
        m_workflowTable->setItem(row, 5, statusCell(text(summary, "lastStatus")));
        const QJsonArray runs = m_runsByWorkflow.value(text(summary, "id"));
        int failed = 0;
        for (const QJsonValue &run : runs)
            failed += text(run.toObject(), "status") == QLatin1String("error") ? 1 : 0;
        m_workflowTable->setItem(row, 6, cell(runs.isEmpty() ? QStringLiteral("–")
                                                             : failed ? QStringLiteral("%1 (%2 failed)").arg(runs.size()).arg(failed)
                                                                      : QString::number(runs.size())));
        const qint64 next = active ? integer(summary, "nextRunAt") : 0;
        m_workflowTable->setItem(row, 7, cell(next ? relativeTime(next, current) : QStringLiteral("–"), timeText(next)));
        m_workflowTable->setItem(row, 8, cell(relativeTime(integer(summary, "updatedAt"), current),
                                              timeText(integer(summary, "updatedAt"))));
        if (text(summary, "id") == chosen)
            m_workflowTable->selectRow(row);
    }
    m_emptyNote->setVisible(m_ready && m_workflows.isEmpty());
    const QJsonObject summary = selectedSummary();
    for (QPushButton *item : {m_openButton, m_runButton, m_toggleButton, m_duplicateButton, m_deleteButton})
        item->setEnabled(!summary.isEmpty() && !m_busy);
    m_toggleButton->setText(summary.value(QStringLiteral("active")).toBool() ? QStringLiteral("Turn off")
                                                                             : QStringLiteral("Turn on"));
    m_toggleButton->setAccessibleName(m_toggleButton->text());
}

void WorkflowsFeature::renderRunTable()
{
    const QJsonObject chosen = selectedDashboardRun();
    const QString workflow = m_runWorkflowFilter->currentData().toString();
    const QString status = m_runStatusFilter->currentData().toString();
    const qint64 current = now();
    const QSignalBlocker block(m_runTable);
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
    }
}

QJsonObject WorkflowsFeature::selectedSummary() const
{
    const int row = m_workflowTable->currentRow();
    if (row < 0 || !m_workflowTable->item(row, 0) || m_workflowTable->selectedItems().isEmpty())
        return {};
    const QString id = m_workflowTable->item(row, 0)->data(Qt::UserRole).toString();
    for (const QJsonValue &value : m_workflows)
        if (text(value.toObject(), "id") == id)
            return value.toObject();
    return {};
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
                                     const QString &runId)
{
    if (workflowId.isEmpty() || runId.isEmpty())
        return;
    summary->setText(QStringLiteral("Loading the run…"));
    steps->setRowCount(0);
    const QPointer<QTableWidget> table(steps);
    const QPointer<QLabel> label(summary);
    auto render = [this, table, label, workflowId, runId](const Draft &draft) {
        request(QByteArrayLiteral("GET"), path(QStringLiteral("/%1/runs/%2").arg(workflowId, runId)), {},
                [this, table, label, draft](bool ok, const QJsonObject &payload, const QString &error) {
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
            }
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

void WorkflowsFeature::runSummary(const QJsonObject &summary)
{
    const QString id = text(summary, "id");
    if (id.isEmpty() || m_busy)
        return;
    loadCatalog([this, id] {
        fetchWorkflow(id, [this, id](const QJsonObject &workflow) {
            const Draft draft = Draft::fromJson(workflow);
            if (draft.hasDesktopSteps(m_catalog)) {
                runOnDesktop(draft);
                return;
            }
            runWorkflow(id);
        });
    });
}

void WorkflowsFeature::runWorkflow(const QString &id)
{
    m_busy = true;
    m_runDraftButton->setEnabled(false);
    setStatus(QStringLiteral("Running on ForkMesh…"));
    request(QByteArrayLiteral("POST"), path(QStringLiteral("/%1/run").arg(id)), {},
            [this, id](bool ok, const QJsonObject &payload, const QString &error) {
        m_busy = false;
        m_runDraftButton->setEnabled(true);
        if (!ok) {
            setStatus(QStringLiteral("The run did not start: %1").arg(failureText(payload, error)), true);
            return;
        }
        const QJsonObject run = payload.value(QStringLiteral("run")).toObject();
        const qint64 ms = integer(run, "finishedAt") - integer(run, "startedAt");
        if (text(run, "status") == QLatin1String("success"))
            setStatus(QStringLiteral("Run succeeded in %1. %2").arg(durationText(ms), runOutputText(run)).trimmed());
        else
            setStatus(QStringLiteral("Run failed: %1").arg(text(run, "error")), true);
        if (m_pages->currentIndex() == 1 && m_draft.id == id)
            loadEditorRuns();
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
    openWorkflow(id);
}

int WorkflowsFeature::navigationBadge() const
{
    int failed = 0;
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
    const QJsonObject trigger = m_draft.trigger(m_catalog);
    const QString type = text(trigger, "type");
    QString note;
    if (m_draft.hasDesktopSteps(m_catalog)) {
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
    m_editorNote->setTextFormat(type == QLatin1String("core.webhook") && !m_draft.hasDesktopSteps(m_catalog)
                                    ? Qt::RichText : Qt::PlainText);
    m_editorNote->setText(note);
    renderStepList();
    renderStepForm();
}

void WorkflowsFeature::renderStepList()
{
    const QSignalBlocker block(m_steps);
    m_steps->clear();
    int selectedRow = -1;
    for (const QString &id : m_draft.ordered()) {
        const QJsonObject node = m_draft.node(id);
        const QString type = text(node, "type");
        QString line = QStringLiteral("%1\n%2").arg(text(node, "name"), m_catalog.name(type));
        const QString summary = m_catalog.summary(node);
        if (!summary.isEmpty() && summary != m_catalog.description(type))
            line += QStringLiteral(" · ") + summary;
        const QString inputs = m_draft.inputsText(id);
        if (!inputs.isEmpty())
            line += QStringLiteral("\n") + inputs;
        else if (!m_catalog.isTrigger(type))
            line += QStringLiteral("\nNot connected: nothing runs it");
        auto *item = new QListWidgetItem(line);
        item->setData(Qt::UserRole, id);
        item->setToolTip(m_catalog.description(type));
        if (node.value(QStringLiteral("disabled")).toBool())
            item->setForeground(palette().color(QPalette::PlaceholderText));
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
    const QString type = text(node, "type");
    QString line = QStringLiteral("%1\n%2").arg(text(node, "name"), m_catalog.name(type));
    const QString summary = m_catalog.summary(node);
    if (!summary.isEmpty() && summary != m_catalog.description(type))
        line += QStringLiteral(" · ") + summary;
    const QString inputs = m_draft.inputsText(m_selectedStep);
    if (!inputs.isEmpty())
        line += QStringLiteral("\n") + inputs;
    item->setText(line);
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
    const bool trigger = m_catalog.isTrigger(type);
    layout->addWidget(sectionLabel(m_catalog.name(type)));
    if (!m_catalog.description(type).isEmpty())
        layout->addWidget(mutedLabel(m_catalog.description(type)));
    if (m_catalog.spec(type).isEmpty())
        layout->addWidget(mutedLabel(QStringLiteral("This step type (%1) is not in the catalog; it is kept as it is.").arg(type)));

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
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
        for (const QString &option : m_catalog.triggerTypes())
            kind->addItem(m_catalog.name(option), option);
        kind->setCurrentIndex(std::max(0, kind->findData(type)));
        form->addRow(QStringLiteral("Starts on"), kind);
        connect(kind, &QComboBox::currentIndexChanged, this, [this, kind] {
            m_draft.setTrigger(m_catalog, kind->currentData().toString());
            setDirty(true);
            QTimer::singleShot(0, this, [this] { renderEditor(); });
        });
    }

    QString operation;
    if (m_catalog.isApp(type)) {
        operation = node.value(QStringLiteral("params")).toObject().value(QStringLiteral("operation")).toString();
        auto *operations = new QComboBox;
        operations->setAccessibleName(QStringLiteral("Operation"));
        for (const QJsonValue &value : m_catalog.operations(type)) {
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
                for (const QJsonValue &value : m_catalog.fields(text(item, "type"), chosen)) {
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
        if (!m_catalog.credentialFields(type.mid(4)).isEmpty()) {
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

    for (const QJsonValue &value : m_catalog.fields(type, operation)) {
        const QJsonObject field = value.toObject();
        QString label = text(field, "label");
        if (field.value(QStringLiteral("required")).toBool())
            label += QStringLiteral(" *");
        QWidget *editor = fieldEditor(field, node);
        editor->setAccessibleName(text(field, "label"));
        form->addRow(label, editor);
    }

    if (m_catalog.isDesktop(type)) {
        const QJsonObject script = m_catalog.spec(type).value(QStringLiteral("script")).toObject();
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
        for (const QString &output : m_catalog.outputs(text(source, "type"))) {
            if (!m_draft.canConnect(m_catalog, candidate, output, id))
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
        if (!m_draft.connectSteps(m_catalog, chosen.at(0), chosen.at(1), id))
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
}

void WorkflowsFeature::saveDraft(std::function<void(bool)> done)
{
    if (m_busy && !done)
        return;
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
    if (m_draft.hasDesktopSteps(m_catalog)) {
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
        m_editorRunTable->setItem(row, 1, cell(text(run, "trigger")));
        m_editorRunTable->setItem(row, 2, statusCell(text(run, "status")));
        m_editorRunTable->setItem(row, 3, cell(finished >= started && started > 0 ? durationText(finished - started)
                                                                                  : QStringLiteral("–")));
        m_editorRunTable->setItem(row, 4, cell(runOutputText(run)));
    }
    if (m_editorRuns.isEmpty()) {
        m_editorRunSummary->setText(m_draft.id.isEmpty() ? QStringLiteral("Save the workflow, then run it.")
                                                         : QStringLiteral("No runs yet."));
        m_editorRunSteps->setRowCount(0);
    }
}

void WorkflowsFeature::loadEditorRuns()
{
    m_editorRuns = QJsonArray();
    renderEditorRuns();
    if (m_draft.id.isEmpty())
        return;
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
    renderWorkflowTable();
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
    request(QByteArrayLiteral("POST"), path(QStringLiteral("/%1/record").arg(id)),
            QJsonObject{{QStringLiteral("startedAt"), double(run.startedAt)},
                        {QStringLiteral("finishedAt"), double(finished)},
                        {QStringLiteral("nodes"), run.report},
                        {QStringLiteral("output"), output}},
            [this, id, run, finished](bool ok, const QJsonObject &payload, const QString &error) {
        m_busy = false;
        setDirty(m_dirty);
        if (!ok) {
            setStatus(QStringLiteral("The run finished but was not saved: %1").arg(failureText(payload, error)), true);
        } else {
            const QJsonObject saved = payload.value(QStringLiteral("run")).toObject();
            if (text(saved, "status") == QLatin1String("success"))
                setStatus(QStringLiteral("Done in %1. %2")
                              .arg(durationText(finished - run.startedAt), runOutputText(saved))
                              .trimmed());
            else
                setStatus(QStringLiteral("Workflow failed. %1").arg(text(saved, "error")).trimmed(), true);
        }
        if (m_pages->currentIndex() == 1 && m_draft.id == id)
            loadEditorRuns();
        refresh();
    });
}

void WorkflowsFeature::exportDraft()
{
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
