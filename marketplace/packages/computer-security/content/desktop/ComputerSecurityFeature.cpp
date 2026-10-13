#include "ComputerSecurityFeature.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QStandardPaths>
#include <QThread>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

namespace forkmesh::security {
namespace {

QColor stateColor(FindingState state)
{
    switch (state) {
    case FindingState::Pass: return QColor(QStringLiteral("#27834a"));
    case FindingState::Review: return QColor(QStringLiteral("#9a6200"));
    case FindingState::Warning: return QColor(QStringLiteral("#b34a00"));
    case FindingState::Critical: return QColor(QStringLiteral("#b42318"));
    case FindingState::Unavailable: return QColor(QStringLiteral("#6b7280"));
    }
    return {};
}

QLabel *mutedLabel(const QString &text)
{
    auto *label = new QLabel(text);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    return label;
}

QLabel *bodyLabel()
{
    auto *label = new QLabel;
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

QLabel *heading(const QString &text)
{
    auto *label = new QLabel(text);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    return label;
}

}

ComputerSecurityFeature::ComputerSecurityFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent)
    , m_searchIndexChanged(host.searchIndexChanged)
{
    setObjectName(QStringLiteral("computerSecurityPage"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *headingRow = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Computer Security Checkup"));
    title->setObjectName(QStringLiteral("pageTitle"));
    headingRow->addWidget(title);
    headingRow->addStretch();
    m_refresh = new QPushButton(QStringLiteral("Run checkup"));
    m_refresh->setObjectName(QStringLiteral("primaryButton"));
    headingRow->addWidget(m_refresh);
    outer->addLayout(headingRow);

    m_summary = new QLabel;
    m_summary->setObjectName(QStringLiteral("securitySummary"));
    m_summary->setWordWrap(true);
    outer->addWidget(m_summary);
    m_note = mutedLabel(QStringLiteral("The checkup only reads this computer's settings and never sends results anywhere. Nothing changes unless you press a fix button and confirm it; "
                                       "fixes that need administrator rights ask for your password first. Select a check to see what it means and whether you need it."));
    outer->addWidget(m_note);

    auto *filterRow = new QHBoxLayout;
    filterRow->setSpacing(6);
    m_filterGroup = new QButtonGroup(this);
    m_filterGroup->setExclusive(true);
    for (FindingFilter filter : filters()) {
        auto *button = new QPushButton(filterName(filter));
        button->setObjectName(QStringLiteral("logRangeButton"));
        button->setCheckable(true);
        button->setChecked(filter == m_filter);
        button->setCursor(Qt::PointingHandCursor);
        button->setAccessibleName(QStringLiteral("Show %1 checks").arg(filterName(filter)));
        m_filterGroup->addButton(button, int(filter));
        filterRow->addWidget(button);
    }
    filterRow->addStretch();
    m_categories = new QComboBox;
    m_categories->setObjectName(QStringLiteral("securityCategory"));
    m_categories->setAccessibleName(QStringLiteral("Filter checks by category"));
    m_categories->addItem(QStringLiteral("All categories"), QString());
    QStringList categories;
    for (const CheckDefinition &definition : checkCatalog()) {
        if (!categories.contains(definition.category))
            categories << definition.category;
    }
    for (const QString &category : categories)
        m_categories->addItem(category, category);
    filterRow->addWidget(m_categories);
    outer->addLayout(filterRow);

    m_status = mutedLabel({});
    m_status->setObjectName(QStringLiteral("securityFixStatus"));
    m_status->hide();
    outer->addWidget(m_status);

    m_empty = mutedLabel({});
    m_empty->setObjectName(QStringLiteral("securityNoMatches"));
    m_empty->hide();
    outer->addWidget(m_empty);

    auto *splitter = new QSplitter(Qt::Vertical);
    splitter->setChildrenCollapsible(false);
    m_tree = new QTreeWidget;
    m_tree->setObjectName(QStringLiteral("securityFindings"));
    m_tree->setAccessibleName(QStringLiteral("Security check findings"));
    m_tree->setColumnCount(5);
    m_tree->setHeaderLabels({QStringLiteral("Status"), QStringLiteral("Category"), QStringLiteral("Check"), QStringLiteral("Result"), QStringLiteral("Action")});
    m_tree->setRootIsDecorated(false);
    m_tree->setAlternatingRowColors(true);
    m_tree->setUniformRowHeights(false);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    splitter->addWidget(m_tree);

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_details = new QWidget;
    m_details->setObjectName(QStringLiteral("securityDetails"));
    auto *details = new QVBoxLayout(m_details);
    details->setContentsMargins(4, 8, 4, 4);
    details->setSpacing(6);
    m_detailTitle = heading({});
    m_detailTitle->setWordWrap(true);
    details->addWidget(m_detailTitle);
    m_detailState = mutedLabel({});
    details->addWidget(m_detailState);
    m_detailResult = bodyLabel();
    details->addWidget(m_detailResult);
    details->addWidget(heading(QStringLiteral("What is this?")));
    m_detailExplanation = bodyLabel();
    details->addWidget(m_detailExplanation);
    m_usageHeading = heading(QStringLiteral("Do you need it?"));
    details->addWidget(m_usageHeading);
    m_detailUsage = bodyLabel();
    details->addWidget(m_detailUsage);
    details->addWidget(heading(QStringLiteral("Fix")));
    m_detailFix = bodyLabel();
    details->addWidget(m_detailFix);
    m_detailCommand = mutedLabel({});
    m_detailCommand->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_detailCommand->setFont(mono);
    details->addWidget(m_detailCommand);
    auto *actions = new QHBoxLayout;
    m_fixButton = new QPushButton;
    m_fixButton->setObjectName(QStringLiteral("primaryButton"));
    m_copyButton = new QPushButton(QStringLiteral("Copy command"));
    m_copyButton->setObjectName(QStringLiteral("ghostButton"));
    m_copyButton->setToolTip(QStringLiteral("Copy the fix as a terminal command to run yourself"));
    actions->addWidget(m_fixButton);
    actions->addWidget(m_copyButton);
    actions->addStretch();
    details->addLayout(actions);
    details->addStretch();
    scroll->setWidget(m_details);
    splitter->addWidget(scroll);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    outer->addWidget(splitter, 1);

    connect(m_refresh, &QPushButton::clicked, this, [this] { refresh(); });
    connect(m_filterGroup, &QButtonGroup::idClicked, this, [this](int id) {
        m_filter = FindingFilter(id);
        render();
    });
    connect(m_categories, &QComboBox::currentIndexChanged, this, [this] {
        m_category = m_categories->currentData().toString();
        render();
    });
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (item)
            showDetails(item->data(0, Qt::UserRole).toString());
    });
    connect(m_fixButton, &QPushButton::clicked, this, [this] { applyFix(m_selectedId); });
    connect(m_copyButton, &QPushButton::clicked, this, [this] { copyCommand(m_selectedId); });
    render();
    refresh();
}

ComputerSecurityFeature::~ComputerSecurityFeature()
{
    if (m_scanThread) {
        m_scanThread->disconnect(this);
        m_scanThread->wait();
        delete m_scanThread;
    }
}

void ComputerSecurityFeature::refresh()
{
    if (m_scanThread)
        return;
    m_refresh->setEnabled(false);
    m_refresh->setText(QStringLiteral("Checking…"));
    auto results = std::make_shared<QList<Finding>>();
    m_scanThread = QThread::create([results] { *results = scan(); });
    connect(m_scanThread, &QThread::finished, this, [this, results] {
        m_scanThread->deleteLater();
        m_scanThread = nullptr;
        m_findings = *results;
        std::stable_sort(m_findings.begin(), m_findings.end(), [](const Finding &left, const Finding &right) {
            return severityRank(left.state) < severityRank(right.state);
        });
        m_refresh->setText(QStringLiteral("Run checkup"));
        m_refresh->setEnabled(true);
        render();
        if (m_searchIndexChanged)
            m_searchIndexChanged();
    });
    m_scanThread->start(QThread::LowPriority);
}

void ComputerSecurityFeature::setHostSearchQuery(const QString &query)
{
    m_query = query.trimmed();
    render();
}

QList<forkmesh::market::NativePackageSearchHit>
ComputerSecurityFeature::searchHits(const QString &query, int limit) const
{
    QList<forkmesh::market::NativePackageSearchHit> hits;
    const QString needle = query.trimmed();
    if (needle.isEmpty() || limit <= 0)
        return hits;
    for (const Finding &finding : m_findings) {
        if (!matchesQuery(finding, needle))
            continue;
        hits << forkmesh::market::NativePackageSearchHit{finding.check.id, finding.check.title,
                                                          stateName(finding.state) + QStringLiteral(" · ") + finding.detail};
        if (hits.size() == limit)
            break;
    }
    return hits;
}

void ComputerSecurityFeature::activateSearchHit(const QString &id)
{
    m_filter = FindingFilter::All;
    if (QAbstractButton *all = m_filterGroup->button(int(FindingFilter::All)))
        all->setChecked(true);
    m_categories->setCurrentIndex(0);
    m_selectedId = id;
    render();
}

const Finding *ComputerSecurityFeature::findingById(const QString &id) const
{
    for (const Finding &finding : m_findings) {
        if (finding.check.id == id)
            return &finding;
    }
    return nullptr;
}

bool ComputerSecurityFeature::matches(const Finding &finding, bool useFilter) const
{
    if (useFilter && !matchesFilter(finding, m_filter))
        return false;
    if (!m_category.isEmpty() && finding.check.category != m_category)
        return false;
    return matchesQuery(finding, m_query);
}

void ComputerSecurityFeature::render()
{
    for (FindingFilter filter : filters()) {
        int count = 0;
        for (const Finding &finding : m_findings) {
            if (matches(finding, false) && matchesFilter(finding, filter))
                ++count;
        }
        if (QAbstractButton *button = m_filterGroup->button(int(filter)))
            button->setText(m_findings.isEmpty() ? filterName(filter) : QStringLiteral("%1 %2").arg(filterName(filter), QString::number(count)));
    }
    int matched = 0;
    for (const Finding &finding : m_findings) {
        if (matches(finding))
            ++matched;
    }
    const bool scanned = !m_findings.isEmpty();
    const bool noMatches = scanned && matched == 0;
    m_summary->setText(scanned ? filteredSummary(m_findings, matched, m_query, m_filter, m_category) : QStringLiteral("Running the checkup…"));
    m_empty->setText(filteredSummary(m_findings, 0, m_query, m_filter, m_category));
    m_empty->setVisible(noMatches);
    m_tree->setVisible(!noMatches);
    m_tree->setAccessibleDescription(noMatches ? QStringLiteral("No matching security checks.")
                                               : QStringLiteral("Showing %1 security checks.").arg(matched));
    const QString keep = m_selectedId;
    m_tree->blockSignals(true);
    m_tree->clear();
    QTreeWidgetItem *selected = nullptr;
    for (const Finding &finding : m_findings) {
        if (!matches(finding))
            continue;
        auto *item = new QTreeWidgetItem(m_tree, {stateName(finding.state), finding.check.category, finding.check.title, finding.detail});
        item->setData(0, Qt::UserRole, finding.check.id);
        item->setForeground(0, stateColor(finding.state));
        item->setToolTip(3, finding.detail);
        item->setToolTip(2, finding.check.explanation);
        if (finding.check.id == keep)
            selected = item;
        if (finding.fix.script.isEmpty())
            continue;
        auto *button = new QPushButton(finding.fix.label);
        button->setObjectName(QStringLiteral("ghostButton"));
        button->setCursor(Qt::PointingHandCursor);
        button->setEnabled(!m_fixRunning);
        button->setAccessibleName(QStringLiteral("%1: %2").arg(finding.fix.label, finding.check.title));
        button->setToolTip(finding.fix.description);
        const QString id = finding.check.id;
        connect(button, &QPushButton::clicked, this, [this, id] {
            QMetaObject::invokeMethod(this, [this, id] { applyFix(id); }, Qt::QueuedConnection);
        });
        m_tree->setItemWidget(item, 4, button);
    }
    if (!selected)
        selected = m_tree->topLevelItem(0);
    if (selected) {
        m_tree->setCurrentItem(selected);
        m_tree->scrollToItem(selected);
    }
    m_tree->blockSignals(false);
    showDetails(selected ? selected->data(0, Qt::UserRole).toString() : QString());
}

void ComputerSecurityFeature::showDetails(const QString &id)
{
    m_selectedId = id;
    const Finding *finding = findingById(id);
    m_details->setVisible(finding != nullptr);
    if (!finding)
        return;
    m_detailTitle->setText(finding->check.title);
    m_detailState->setText(QStringLiteral("%1 · %2").arg(stateName(finding->state), finding->check.category));
    m_detailResult->setText(finding->detail);
    m_detailExplanation->setText(finding->check.explanation);
    m_usageHeading->setVisible(!finding->usage.isEmpty());
    m_detailUsage->setVisible(!finding->usage.isEmpty());
    m_detailUsage->setText(finding->usage);
    const FixAction &fix = finding->fix;
    const bool fixable = !fix.script.isEmpty();
    if (fixable) {
        QString text = fix.description;
        if (!fix.caution.isEmpty())
            text += QStringLiteral("\n\nBefore you fix it: ") + fix.caution;
        if (fix.elevated)
            text += QStringLiteral("\n\nYour computer asks for your administrator password before anything changes.");
        m_detailFix->setText(text);
    } else if (finding->state == FindingState::Pass) {
        m_detailFix->setText(QStringLiteral("Nothing to fix."));
    } else if (finding->state == FindingState::Unavailable) {
        m_detailFix->setText(QStringLiteral("This could not be checked, so there is nothing to fix from here."));
    } else {
        m_detailFix->setText(QStringLiteral("There is no safe one-click fix for this; it needs a decision only you can make. The explanation above says what to look at."));
    }
    m_detailCommand->setVisible(fixable);
    m_detailCommand->setText(manualCommand(fix));
    m_fixButton->setVisible(fixable);
    m_fixButton->setText(fix.label);
    m_fixButton->setEnabled(fixable && !m_fixRunning);
    m_copyButton->setVisible(fixable);
}

void ComputerSecurityFeature::copyCommand(const QString &id)
{
    const Finding *finding = findingById(id);
    if (!finding || finding->fix.script.isEmpty())
        return;
    QGuiApplication::clipboard()->setText(manualCommand(finding->fix));
    m_status->setText(QStringLiteral("Copied the command for \"%1\". Paste it into a terminal to run it yourself.").arg(finding->check.title));
    m_status->show();
}

void ComputerSecurityFeature::applyFix(const QString &id)
{
    const Finding *finding = findingById(id);
    if (!finding || finding->fix.script.isEmpty() || m_fixRunning)
        return;
    const FixAction fix = finding->fix;
    const QString title = finding->check.title;
    QMessageBox box(this);
    box.setIcon(finding->state == FindingState::Critical ? QMessageBox::Warning : QMessageBox::Question);
    box.setWindowTitle(QStringLiteral("%1: %2").arg(fix.label, title));
    box.setText(fix.description);
    QString informative;
    if (!fix.caution.isEmpty())
        informative = QStringLiteral("Before you continue: %1\n\n").arg(fix.caution);
    informative += fix.elevated ? QStringLiteral("Your computer will ask for your administrator password.")
                                : QStringLiteral("This runs as your own account.");
    box.setInformativeText(informative);
    box.setDetailedText(QStringLiteral("Command that will run:\n\n") + manualCommand(fix));
    QPushButton *apply = box.addButton(fix.label, QMessageBox::AcceptRole);
    QPushButton *cancel = box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(cancel);
    box.exec();
    if (box.clickedButton() != apply)
        return;
#if !defined(Q_OS_LINUX)
    m_status->setText(QStringLiteral("Fixes run on Linux only. Use \"Copy command\" and run it in a terminal."));
    m_status->show();
    return;
#else
    QString program = QStringLiteral("sh");
    QStringList arguments = {QStringLiteral("-c"), fix.script};
    if (fix.elevated) {
        program = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
        if (program.isEmpty()) {
            m_status->setText(QStringLiteral("The administrator prompt (pkexec) is not installed. Use \"Copy command\" and run it in a terminal."));
            m_status->show();
            return;
        }
        arguments.prepend(QStringLiteral("sh"));
    }
    m_fixRunning = true;
    m_status->setText(QStringLiteral("Applying \"%1\"…").arg(fix.label));
    m_status->show();
    render();
    auto *process = new QProcess(this);
    const bool elevated = fix.elevated;
    connect(process, &QProcess::finished, this, [this, process, title, elevated](int exitCode, QProcess::ExitStatus status) {
        process->deleteLater();
        m_fixRunning = false;
        const QString errors = QString::fromLocal8Bit(process->readAllStandardError()).trimmed();
        if (status == QProcess::NormalExit && exitCode == 0)
            m_status->setText(QStringLiteral("Fixed \"%1\". Running the checkup again to confirm.").arg(title));
        else if (elevated && exitCode == 126)
            m_status->setText(QStringLiteral("The administrator prompt was cancelled, so nothing was changed."));
        else
            m_status->setText(QStringLiteral("The fix for \"%1\" did not finish (exit code %2)%3")
                                  .arg(title, QString::number(exitCode), errors.isEmpty() ? QStringLiteral(".") : QStringLiteral(": ") + errors.right(300)));
        refresh();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        process->deleteLater();
        m_fixRunning = false;
        m_status->setText(QStringLiteral("The fix could not be started. Use \"Copy command\" and run it in a terminal."));
        render();
    });
    process->start(program, arguments);
#endif
}

}

namespace {
const forkmesh::market::NativePackageRegistration computerSecurityRegistration({
    QStringLiteral("builtin:computer-security"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Computer Security Checkup"),
    QStringLiteral("shield"),
    QString(),
    QString(),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::security::ComputerSecurityFeature(host, parent);
    },
});
}
