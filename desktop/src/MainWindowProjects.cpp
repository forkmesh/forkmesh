
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "KebabHeaderView.h"
#include "ProjectGantt.h"

#include <QDateEdit>

using namespace forkmesh::ui;

namespace {

QString projectDateText(qint64 ms)
{
    return ms > 0 ? QDateTime::fromMSecsSinceEpoch(ms)
                        .toString(QStringLiteral("yyyy-MM-dd"))
                  : QString();
}

qint64 dateEditorMs(QDateEdit *edit, QCheckBox *enable)
{
    if (!edit || !enable || !enable->isChecked())
        return 0;
    return edit->date().startOfDay().toMSecsSinceEpoch();
}

void seedDateEditor(QDateEdit *edit, QCheckBox *enable, qint64 ms)
{
    if (!edit || !enable)
        return;
    enable->setChecked(ms > 0);
    edit->setEnabled(ms > 0);
    edit->setDate(ms > 0 ? QDateTime::fromMSecsSinceEpoch(ms).date()
                         : QDate::currentDate());
}

QWidget *makeDateRow(QWidget *parent, QCheckBox *&enableOut, QDateEdit *&editOut)
{
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    enableOut = new QCheckBox(row);
    enableOut->setToolTip(QStringLiteral("Untick to leave this date unset"));
    editOut = new QDateEdit(QDate::currentDate(), row);
    editOut->setCalendarPopup(true);
    editOut->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    editOut->setEnabled(false);
    QObject::connect(enableOut, &QCheckBox::toggled, editOut,
                     &QWidget::setEnabled);
    layout->addWidget(enableOut);
    layout->addWidget(editOut, 1);
    return row;
}

QListWidget *makeIssuePicker(QWidget *parent, const QList<Issue> &issues,
                             const QList<int> &checked)
{
    auto *list = new QListWidget(parent);
    list->setSelectionMode(QAbstractItemView::NoSelection);
    list->setMinimumHeight(160);
    for (const Issue &issue : issues) {
        if (issue.isDeleted())
            continue; // a tombstoned issue can't be linked to a project
        auto *item = new QListWidgetItem(
            QStringLiteral("#%1  %2%3")
                .arg(issue.number)
                .arg(issue.title,
                     issue.status == QLatin1String("closed")
                         ? QStringLiteral("  (closed)")
                         : QString()));
        item->setData(Qt::UserRole, issue.number);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(checked.contains(issue.number) ? Qt::Checked
                                                           : Qt::Unchecked);
        list->addItem(item);
    }
    return list;
}

QList<int> pickedIssues(QListWidget *list)
{
    QList<int> numbers;
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->checkState() == Qt::Checked)
            numbers.append(list->item(i)->data(Qt::UserRole).toInt());
    return numbers;
}

} // namespace


QWidget *MainWindow::buildProjectsSection()
{
    auto *page = new QWidget;

    auto *listPane = new QWidget;
    listPane->setMinimumWidth(260);

    auto *heading = new QLabel("Projects");
    heading->setObjectName("channelTitle");

    m_projectNewButton = new QPushButton("New project");
    m_projectNewButton->setObjectName("primaryButton");
    m_projectNewButton->setProperty("buttonSize", "sm");
    m_projectNewButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_projectNewButton, "plus", 16);
    connect(m_projectNewButton, &QPushButton::clicked, this,
            &MainWindow::promptNewProject);

    auto *viewGroup = new QButtonGroup(page);
    viewGroup->setExclusive(true);
    auto *listTab = new QPushButton("List");
    listTab->setToolTip("Projects as a sortable table");
    setOcticon(listTab, "list-unordered", 16);
    auto *ganttTab = new QPushButton("Gantt");
    ganttTab->setToolTip("Projects and their linked issues on a timeline");
    setOcticon(ganttTab, "graph", 16);
    for (QPushButton *b : {listTab, ganttTab}) {
        b->setObjectName("ghostButton");
        b->setProperty("buttonSize", "sm");
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
    }
    listTab->setChecked(true);
    viewGroup->addButton(listTab, 0);
    viewGroup->addButton(ganttTab, 1);
    m_projectViewTabs = viewGroup;

    m_projectStatusFilter = new QComboBox;
    m_projectStatusFilter->setObjectName("issueControlSm");
    m_projectStatusFilter->addItems({"Open", "Closed", "All"});
    auto *refreshButton = new QPushButton;
    refreshButton->setObjectName("ghostButton");
    refreshButton->setProperty("buttonSize", "sm");
    refreshButton->setCursor(Qt::PointingHandCursor);
    refreshButton->setToolTip("Reload projects");
    setOcticon(refreshButton, "sync", 16);
    connect(refreshButton, &QPushButton::clicked, this,
            &MainWindow::reloadProjects);

    auto *headingRow = new QHBoxLayout;
    headingRow->setContentsMargins(0, 0, 0, 0);
    headingRow->addWidget(heading);
    headingRow->addWidget(m_projectNewButton);
    headingRow->addStretch();
    headingRow->addWidget(m_projectStatusFilter);
    headingRow->addWidget(listTab);
    headingRow->addWidget(ganttTab);
    headingRow->addWidget(refreshButton);

    m_projectTable = new QTableWidget(0, 8);
    m_projectTable->setObjectName("issueTable");
    installColumnHeaderMenu(m_projectTable);
    enableHoverRowHighlight(m_projectTable);
    m_projectTable->setHorizontalHeaderLabels(
        {"#", "Title", "Status", "Start", "End", "Milestone", "Issues",
         "Progress"});
    m_projectTable->verticalHeader()->setVisible(false);
    m_projectTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_projectTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_projectTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_projectTable->setShowGrid(false);
    m_projectTable->setWordWrap(false);
    m_projectTable->setSortingEnabled(true);
    m_projectTable->sortByColumn(0, Qt::AscendingOrder);
    QHeaderView *header = m_projectTable->horizontalHeader();
    header->setHighlightSections(false);
    header->setSectionResizeMode(0, QHeaderView::ResizeToContents); // #
    header->setSectionResizeMode(1, QHeaderView::Interactive);      // Title
    m_projectTable->setColumnWidth(1, 320);
    for (int i = 2; i < 8; ++i)
        header->setSectionResizeMode(i, QHeaderView::ResizeToContents);
    makeColumnsResizable(m_projectTable);
    connect(m_projectTable, &QTableWidget::itemSelectionChanged, this, [this] {
        const QModelIndexList rows =
            m_projectTable->selectionModel()->selectedRows();
        if (rows.isEmpty())
            return;
        QTableWidgetItem *first = m_projectTable->item(rows.first().row(), 0);
        if (first)
            showProject(first->data(Qt::UserRole).toInt());
    });

    auto *gantt = new ProjectGantt;
    gantt->setDarkProbe([] { return currentThemeIsDark(); });
    m_projectGantt = gantt;
    auto *ganttScroll = new QScrollArea;
    ganttScroll->setObjectName("issuePageScroll");
    ganttScroll->setFrameShape(QFrame::NoFrame);
    ganttScroll->setWidgetResizable(true);
    ganttScroll->setWidget(gantt);

    m_projectViewStack = new QStackedWidget;
    m_projectViewStack->addWidget(m_projectTable); // 0 List
    m_projectViewStack->addWidget(ganttScroll);    // 1 Gantt
    connect(viewGroup, &QButtonGroup::idClicked, this, [this](int id) {
        if (m_projectViewStack)
            m_projectViewStack->setCurrentIndex(id);
        if (id == 1)
            refreshProjectGantt();
        scheduleNavRecord();
    });
    connect(m_projectStatusFilter, &QComboBox::currentIndexChanged, this,
            [this](int) {
                refreshProjectList();
                refreshProjectGantt();
            });

    m_projectInlineNotice = new QLabel;
    m_projectInlineNotice->setObjectName("issueInlineNotice");
    m_projectInlineNotice->setWordWrap(true);
    m_projectInlineNotice->hide();

    auto *listLayout = new QVBoxLayout(listPane);
    listLayout->setContentsMargins(18, 18, 12, 18);
    listLayout->setSpacing(8);
    listLayout->addLayout(headingRow);
    listLayout->addWidget(m_projectInlineNotice);
    listLayout->addWidget(m_projectViewStack, 1);

    auto *detail = new QWidget;
    detail->setObjectName("issueSidebar");
    m_projectDetailTitle = new QLabel("Select a project");
    m_projectDetailTitle->setObjectName("issuePageTitle");
    m_projectDetailTitle->setTextFormat(Qt::RichText);
    m_projectDetailTitle->setWordWrap(true);
    m_projectDetailStatus = new QLabel;
    m_projectDetailStatus->setObjectName("issueStatusPill");
    m_projectDetailStatus->setTextFormat(Qt::PlainText);
    m_projectDetailStatus->setAlignment(Qt::AlignCenter);
    m_projectDetailStatus->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_projectDetailBody = new QLabel;
    m_projectDetailBody->setObjectName("statusLine");
    m_projectDetailBody->setWordWrap(true);
    m_projectDetailBody->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_projectDetailProgress = new QLabel;
    m_projectDetailProgress->setObjectName("statusLine");
    m_projectDetailProgress->setTextFormat(Qt::RichText);

    auto *detailLayout = new QVBoxLayout(detail);
    detailLayout->setContentsMargins(14, 22, 12, 22);
    detailLayout->setSpacing(8);
    auto addHeading = [&](const QString &text) {
        auto *label = new QLabel(text, detail);
        label->setObjectName("issueSidebarHeading");
        detailLayout->addSpacing(8);
        detailLayout->addWidget(label);
    };
    detailLayout->addWidget(m_projectDetailTitle);
    detailLayout->addWidget(m_projectDetailStatus, 0, Qt::AlignLeft);
    detailLayout->addWidget(m_projectDetailBody);
    addHeading("Progress");
    detailLayout->addWidget(m_projectDetailProgress);

    addHeading("Dates");
    detailLayout->addWidget(new QLabel("Start", detail));
    detailLayout->addWidget(
        makeDateRow(detail, m_projectStartEnable, m_projectStartEdit));
    detailLayout->addWidget(new QLabel("End", detail));
    detailLayout->addWidget(
        makeDateRow(detail, m_projectEndEnable, m_projectEndEdit));
    auto *saveDates = new QPushButton("Save dates", detail);
    saveDates->setObjectName("ghostButton");
    saveDates->setProperty("buttonSize", "sm");
    saveDates->setCursor(Qt::PointingHandCursor);
    detailLayout->addWidget(saveDates, 0, Qt::AlignLeft);
    connect(saveDates, &QPushButton::clicked, this, [this] {
        if (m_currentProjectNumber < 0)
            return;
        ProjectStore store = projectStoreForCurrentRepo();
        QString error;
        if (!store.setDates(m_currentProjectNumber,
                            dateEditorMs(m_projectStartEdit, m_projectStartEnable),
                            dateEditorMs(m_projectEndEdit, m_projectEndEnable),
                            &error)) {
            setProjectInlineNotice(
                error.isEmpty() ? "Could not update dates." : error, true);
            return;
        }
        setProjectInlineNotice("Dates updated.");
        reloadProjects();
    });

    addHeading("Milestone");
    m_projectMilestoneCombo = new QComboBox(detail);
    m_projectMilestoneCombo->setToolTip(
        "Linking a milestone drives the project's progress when no issues are "
        "linked directly");
    detailLayout->addWidget(m_projectMilestoneCombo);
    auto *saveMilestone = new QPushButton("Save milestone", detail);
    saveMilestone->setObjectName("ghostButton");
    saveMilestone->setProperty("buttonSize", "sm");
    saveMilestone->setCursor(Qt::PointingHandCursor);
    detailLayout->addWidget(saveMilestone, 0, Qt::AlignLeft);
    connect(saveMilestone, &QPushButton::clicked, this, [this] {
        if (m_currentProjectNumber < 0 || !m_projectMilestoneCombo)
            return;
        ProjectStore store = projectStoreForCurrentRepo();
        QString error;
        if (!store.setMilestone(m_currentProjectNumber,
                                m_projectMilestoneCombo->currentData().toString(),
                                &error)) {
            setProjectInlineNotice(
                error.isEmpty() ? "Could not update milestone." : error, true);
            return;
        }
        setProjectInlineNotice("Milestone updated.");
        reloadProjects();
    });

    addHeading("Linked issues");
    m_projectIssuesList = new QListWidget(detail);
    m_projectIssuesList->setSelectionMode(QAbstractItemView::NoSelection);
    m_projectIssuesList->setMinimumHeight(110);
    detailLayout->addWidget(m_projectIssuesList, 1);
    auto *linkIssues = new QPushButton("Edit linked issues", detail);
    linkIssues->setObjectName("ghostButton");
    linkIssues->setProperty("buttonSize", "sm");
    linkIssues->setCursor(Qt::PointingHandCursor);
    setOcticon(linkIssues, "issue-opened", 15);
    detailLayout->addWidget(linkIssues, 0, Qt::AlignLeft);
    connect(linkIssues, &QPushButton::clicked, this,
            &MainWindow::editProjectLinkedIssues);

    m_projectCloseButton = new QPushButton("Close project", detail);
    m_projectCloseButton->setObjectName("ghostButton");
    m_projectCloseButton->setCursor(Qt::PointingHandCursor);
    connect(m_projectCloseButton, &QPushButton::clicked, this, [this] {
        if (m_currentProjectNumber < 0)
            return;
        QString status = QStringLiteral("closed");
        for (const Project &p : std::as_const(m_currentProjects))
            if (p.number == m_currentProjectNumber &&
                p.status == QLatin1String("closed"))
                status = QStringLiteral("open");
        ProjectStore store = projectStoreForCurrentRepo();
        QString error;
        if (!store.setStatus(m_currentProjectNumber, status, &error)) {
            setProjectInlineNotice(
                error.isEmpty() ? "Could not update the project." : error, true);
            return;
        }
        setProjectInlineNotice(status == QLatin1String("closed")
                                   ? "Project closed."
                                   : "Project reopened.");
        reloadProjects();
    });
    m_projectDeleteButton = new QPushButton("Delete project", detail);
    m_projectDeleteButton->setObjectName("issueDangerLink");
    m_projectDeleteButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_projectDeleteButton, "trash", 15);
    connect(m_projectDeleteButton, &QPushButton::clicked, this, [this] {
        if (m_currentProjectNumber < 0)
            return;
        const auto choice = QMessageBox::question(
            this, "Delete project",
            QStringLiteral("Delete project #%1? Its linked issues are kept.")
                .arg(m_currentProjectNumber));
        if (choice != QMessageBox::Yes)
            return;
        ProjectStore store = projectStoreForCurrentRepo();
        QString error;
        if (!store.tombstoneProject(m_currentProjectNumber, &error)) {
            setProjectInlineNotice(
                error.isEmpty() ? "Could not delete the project." : error, true);
            return;
        }
        m_currentProjectNumber = -1;
        setProjectInlineNotice("Project deleted.");
        reloadProjects();
    });
    detailLayout->addSpacing(10);
    detailLayout->addWidget(m_projectCloseButton, 0, Qt::AlignLeft);
    detailLayout->addWidget(m_projectDeleteButton, 0, Qt::AlignLeft);
    detailLayout->addStretch();

    auto *detailScroll = new QScrollArea;
    detailScroll->setObjectName("issueSidebarScroll");
    detailScroll->setFrameShape(QFrame::NoFrame);
    detailScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    detailScroll->setWidgetResizable(true);
    detailScroll->setMinimumWidth(230);
    detailScroll->setMaximumWidth(380);
    detailScroll->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
    detailScroll->setWidget(detail);
    m_projectDetailPane = detailScroll;

    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->setObjectName("issuesSplitter");
    splitter->setChildrenCollapsible(true);
    splitter->addWidget(listPane);
    splitter->addWidget(m_projectDetailPane);
    splitter->setCollapsible(0, false);
    splitter->setCollapsible(1, true);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setSizes({900, 320});
    m_projectDetailPane->hide();

    auto *layout = new QHBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(splitter);
    return page;
}

ProjectStore MainWindow::projectStoreForCurrentRepo() const
{
    const int idx = issuesRepoIndex();
    if (idx < 0)
        return ProjectStore(QString(), QString(), &m_profileIdentity,
                            chatDisplayName());
    const RepositoryRecord &repo = writableRecordFor(m_repositories.at(idx));
    return ProjectStore(repo.localPath, repo.mirrorPath, &m_profileIdentity,
                        chatDisplayName());
}

void MainWindow::setProjectInlineNotice(const QString &message, bool error)
{
    if (!m_projectInlineNotice)
        return;
    m_projectInlineNotice->setText(message);
    m_projectInlineNotice->setProperty("noticeError", error);
    m_projectInlineNotice->style()->unpolish(m_projectInlineNotice);
    m_projectInlineNotice->style()->polish(m_projectInlineNotice);
    m_projectInlineNotice->setVisible(!message.isEmpty());
}

int MainWindow::projectProgressPercent(const Project &project) const
{
    int total = 0;
    int closed = 0;
    if (!project.issues.isEmpty()) {
        for (const Issue &issue : m_currentIssues) {
            if (!project.issues.contains(issue.number))
                continue;
            ++total;
            if (issue.status == QLatin1String("closed"))
                ++closed;
        }
    } else if (!project.milestone.isEmpty()) {
        for (const Issue &issue : m_currentIssues) {
            if (issue.milestone != project.milestone)
                continue;
            ++total;
            if (issue.status == QLatin1String("closed"))
                ++closed;
        }
    }
    if (total <= 0)
        return 0;
    return qRound(100.0 * closed / total);
}

void MainWindow::reloadProjects()
{
    if (!m_projectTable)
        return;
    // Linked-issue titles, milestone choices and progress all read the issue
    // data, so make sure it's loaded (cheap: reloadIssues() skips on an
    // unchanged content signature).
    reloadIssues();
    if (issuesRepoIndex() < 0) {
        m_currentProjects.clear();
        m_currentProjectNumber = -1;
        m_projectTable->setRowCount(0);
        refreshProjectGantt();
        if (m_projectDetailPane)
            m_projectDetailPane->hide();
        return;
    }
    const ProjectStore store = projectStoreForCurrentRepo();
    m_currentProjects = store.loadAll();
    const bool writable = store.canWrite();
    if (m_projectNewButton)
        m_projectNewButton->setEnabled(writable);
    if (auto *b = dynamic_cast<VerticalIconButton *>(m_repoProjectsTab))
        b->setBadgeCount(m_currentProjects.size());

    if (m_projectMilestoneCombo) {
        QSignalBlocker blocker(m_projectMilestoneCombo);
        m_projectMilestoneCombo->clear();
        m_projectMilestoneCombo->addItem("No milestone", QString());
        for (const IssueMilestone &ms : std::as_const(m_currentMilestones))
            m_projectMilestoneCombo->addItem(ms.title, ms.title);
    }

    refreshProjectList();
    refreshProjectGantt();

    bool stillThere = false;
    for (const Project &project : std::as_const(m_currentProjects))
        if (project.number == m_currentProjectNumber)
            stillThere = true;
    if (stillThere)
        showProject(m_currentProjectNumber);
    else if (m_projectDetailPane) {
        m_currentProjectNumber = -1;
        m_projectDetailPane->hide();
    }
}

void MainWindow::refreshProjectList()
{
    if (!m_projectTable)
        return;
    const QString statusFilter =
        m_projectStatusFilter ? m_projectStatusFilter->currentText()
                              : QStringLiteral("Open");
    m_projectTable->setSortingEnabled(false);
    m_projectTable->setRowCount(0);
    for (const Project &project : std::as_const(m_currentProjects)) {
        const bool closed = project.status == QLatin1String("closed");
        if (statusFilter == QLatin1String("Open") && closed)
            continue;
        if (statusFilter == QLatin1String("Closed") && !closed)
            continue;
        const int row = m_projectTable->rowCount();
        m_projectTable->insertRow(row);
        auto setCell = [&](int col, const QString &text) {
            auto *item = new QTableWidgetItem(text);
            if (col == 0)
                item->setData(Qt::UserRole, project.number);
            m_projectTable->setItem(row, col, item);
        };
        setCell(0, QString::number(project.number));
        setCell(1, project.title);
        setCell(2, closed ? QStringLiteral("Closed") : QStringLiteral("Open"));
        setCell(3, projectDateText(project.startDate));
        setCell(4, projectDateText(project.endDate));
        setCell(5, project.milestone);
        setCell(6, project.issues.isEmpty()
                       ? QString()
                       : QString::number(project.issues.size()));
        setCell(7, QStringLiteral("%1%").arg(projectProgressPercent(project)));
    }
    m_projectTable->setSortingEnabled(true);
}

void MainWindow::refreshProjectGantt()
{
    auto *gantt = static_cast<ProjectGantt *>(m_projectGantt);
    if (!gantt)
        return;
    const QString statusFilter =
        m_projectStatusFilter ? m_projectStatusFilter->currentText()
                              : QStringLiteral("Open");
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 day = 24 * 60 * 60 * 1000LL;

    // A row's plotted range: its signed dates when both are set; otherwise an
    // inferred createdAt -> today span drawn as a dashed placeholder.
    auto rowFor = [&](const QString &label, qint64 start, qint64 end,
                      qint64 createdAt, bool isProject, const QString &status,
                      int progress, int indent) {
        ProjectGanttRow row;
        row.label = label;
        row.inferred = start <= 0 || end <= 0;
        row.start = start > 0 ? start : (createdAt > 0 ? createdAt : now);
        row.end = end > 0 ? end : qMax(now, row.start + day);
        if (row.end <= row.start)
            row.end = row.start + day;
        row.isProject = isProject;
        row.status = status;
        row.progress = progress;
        row.indent = indent;
        return row;
    };

    // Linked issues are looked up by number once per project row; index the
    // issue list once instead of scanning it for every link. The first issue
    // with a number wins, as the scan did.
    QHash<int, const Issue *> issuesByNumber;
    issuesByNumber.reserve(m_currentIssues.size());
    for (const Issue &issue : std::as_const(m_currentIssues))
        if (!issuesByNumber.contains(issue.number))
            issuesByNumber.insert(issue.number, &issue);

    QList<ProjectGanttRow> rows;
    for (const Project &project : std::as_const(m_currentProjects)) {
        const bool closed = project.status == QLatin1String("closed");
        if (statusFilter == QLatin1String("Open") && closed)
            continue;
        if (statusFilter == QLatin1String("Closed") && !closed)
            continue;
        rows.append(rowFor(QStringLiteral("#%1 %2")
                               .arg(project.number)
                               .arg(project.title),
                           project.startDate, project.endDate, project.createdAt,
                           true, project.status, projectProgressPercent(project),
                           0));
        for (int number : project.issues) {
            const Issue *linked = issuesByNumber.value(number, nullptr);
            if (!linked)
                continue;
            const Issue &issue = *linked;
            rows.append(rowFor(
                QStringLiteral("#%1 %2").arg(issue.number).arg(issue.title),
                issue.startDate, issue.endDate, issue.createdAt, false,
                issue.status,
                issue.status == QLatin1String("closed") ? 100
                                                        : issue.progress,
                1));
        }
    }
    gantt->setRows(rows);
}

void MainWindow::showProject(int number)
{
    const Project *found = nullptr;
    for (const Project &project : std::as_const(m_currentProjects))
        if (project.number == number)
            found = &project;
    if (!found)
        return;
    m_currentProjectNumber = number;
    setProjectInlineNotice(QString());

    m_projectDetailTitle->setText(
        QStringLiteral("%1 <span style='color:#656d76;font-weight:400'>#%2</span>")
            .arg(found->title.toHtmlEscaped())
            .arg(found->number));
    const bool closed = found->status == QLatin1String("closed");
    m_projectDetailStatus->setText(closed ? "Closed" : "Open");
    m_projectDetailStatus->setProperty("status", closed ? "closed" : "open");
    m_projectDetailStatus->style()->unpolish(m_projectDetailStatus);
    m_projectDetailStatus->style()->polish(m_projectDetailStatus);
    m_projectDetailBody->setText(found->body.trimmed().isEmpty()
                                     ? QStringLiteral("No description.")
                                     : found->body);
    const int pct = projectProgressPercent(*found);
    QString progressText = QStringLiteral("<b>%1%</b>").arg(pct);
    if (found->issues.isEmpty() && !found->milestone.isEmpty())
        progressText +=
            QStringLiteral(" <span style='color:#8b949e'>(milestone %1)</span>")
                .arg(found->milestone.toHtmlEscaped());
    else
        progressText += QStringLiteral(
                            " <span style='color:#8b949e'>(%1 linked issue%2)</span>")
                            .arg(found->issues.size())
                            .arg(found->issues.size() == 1 ? "" : "s");
    m_projectDetailProgress->setText(progressText);

    seedDateEditor(m_projectStartEdit, m_projectStartEnable, found->startDate);
    seedDateEditor(m_projectEndEdit, m_projectEndEnable, found->endDate);
    if (m_projectMilestoneCombo) {
        const int selected = m_projectMilestoneCombo->findData(found->milestone);
        QSignalBlocker blocker(m_projectMilestoneCombo);
        m_projectMilestoneCombo->setCurrentIndex(selected >= 0 ? selected : 0);
    }

    if (m_projectIssuesList) {
        m_projectIssuesList->clear();
        for (int issueNumber : found->issues) {
            QString title;
            QString status;
            for (const Issue &issue : std::as_const(m_currentIssues))
                if (issue.number == issueNumber) {
                    title = issue.title;
                    status = issue.status;
                    break;
                }
            auto *item = new QListWidgetItem(
                QStringLiteral("#%1  %2%3")
                    .arg(issueNumber)
                    .arg(title,
                         status == QLatin1String("closed")
                             ? QStringLiteral("  (closed)")
                             : QString()));
            item->setData(Qt::UserRole, issueNumber);
            m_projectIssuesList->addItem(item);
        }
        if (found->issues.isEmpty())
            m_projectIssuesList->addItem(QStringLiteral("No linked issues"));
    }

    m_projectCloseButton->setText(closed ? "Reopen project" : "Close project");
    const bool writable = projectStoreForCurrentRepo().canWrite();
    for (QWidget *w : std::initializer_list<QWidget *>{
             m_projectCloseButton, m_projectDeleteButton, m_projectStartEdit,
             m_projectEndEdit, m_projectStartEnable, m_projectEndEnable,
             m_projectMilestoneCombo})
        if (w)
            w->setEnabled(writable);
    if (writable) {
        if (m_projectStartEdit && m_projectStartEnable)
            m_projectStartEdit->setEnabled(m_projectStartEnable->isChecked());
        if (m_projectEndEdit && m_projectEndEnable)
            m_projectEndEdit->setEnabled(m_projectEndEnable->isChecked());
    }

    if (m_projectDetailPane)
        m_projectDetailPane->show();
}

void MainWindow::promptNewProject()
{
    ProjectStore store = projectStoreForCurrentRepo();
    if (!store.canWrite()) {
        setProjectInlineNotice(
            "This repository is read-only on this node, so projects can't be "
            "created here.",
            true);
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle("New project");
    dialog.setMinimumWidth(460);
    auto *form = new QFormLayout(&dialog);
    auto *titleEdit = new QLineEdit(&dialog);
    titleEdit->setPlaceholderText("Project title");
    auto *bodyEdit = new QPlainTextEdit(&dialog);
    bodyEdit->setPlaceholderText("Describe the goal of this project\xE2\x80\xA6");
    bodyEdit->setFixedHeight(90);
    QCheckBox *startEnable = nullptr;
    QDateEdit *startEdit = nullptr;
    QCheckBox *endEnable = nullptr;
    QDateEdit *endEdit = nullptr;
    QWidget *startRow = makeDateRow(&dialog, startEnable, startEdit);
    QWidget *endRow = makeDateRow(&dialog, endEnable, endEdit);
    auto *milestoneCombo = new QComboBox(&dialog);
    milestoneCombo->addItem("No milestone", QString());
    const IssueStore issueStore = issueStoreForCurrentRepo();
    for (const IssueMilestone &ms : issueStore.loadMilestones())
        milestoneCombo->addItem(ms.title, ms.title);
    QListWidget *issuePicker = makeIssuePicker(&dialog, m_currentIssues, {});

    form->addRow("Title", titleEdit);
    form->addRow("Description", bodyEdit);
    form->addRow("Start date", startRow);
    form->addRow("End date", endRow);
    form->addRow("Milestone", milestoneCombo);
    form->addRow("Link issues", issuePicker);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText("Create project");
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&dialog, titleEdit] {
        if (!titleEdit->text().trimmed().isEmpty())
            dialog.accept();
        else
            titleEdit->setFocus();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;

    QString error;
    const int number = store.createProject(
        titleEdit->text().trimmed(), bodyEdit->toPlainText().trimmed(),
        dateEditorMs(startEdit, startEnable), dateEditorMs(endEdit, endEnable),
        milestoneCombo->currentData().toString(), pickedIssues(issuePicker),
        &error);
    if (number < 0) {
        setProjectInlineNotice(
            error.isEmpty() ? "Could not create the project." : error, true);
        return;
    }
    m_currentProjectNumber = number;
    setProjectInlineNotice(QStringLiteral("Project #%1 created.").arg(number));
    reloadProjects();
}

void MainWindow::editProjectLinkedIssues()
{
    if (m_currentProjectNumber < 0)
        return;
    ProjectStore store = projectStoreForCurrentRepo();
    if (!store.canWrite()) {
        setProjectInlineNotice("This repository is read-only on this node.", true);
        return;
    }
    QList<int> linked;
    for (const Project &project : std::as_const(m_currentProjects))
        if (project.number == m_currentProjectNumber)
            linked = project.issues;

    QDialog dialog(this);
    dialog.setWindowTitle(
        QStringLiteral("Link issues to project #%1").arg(m_currentProjectNumber));
    dialog.setMinimumWidth(420);
    auto *layout = new QVBoxLayout(&dialog);
    auto *hint = new QLabel("Tick the issues that belong to this project.",
                            &dialog);
    hint->setObjectName("statusLine");
    QListWidget *issuePicker = makeIssuePicker(&dialog, m_currentIssues, linked);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(hint);
    layout->addWidget(issuePicker, 1);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;

    QString error;
    if (!store.setIssues(m_currentProjectNumber, pickedIssues(issuePicker),
                         &error)) {
        setProjectInlineNotice(
            error.isEmpty() ? "Could not update the linked issues." : error, true);
        return;
    }
    setProjectInlineNotice("Linked issues updated.");
    reloadProjects();
}
