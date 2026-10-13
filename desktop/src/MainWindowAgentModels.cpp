#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "AgentModelStats.h"
#include "AgentModelTable.h"
#include "AgentToolEfficiencyLedger.h"

#include <QCheckBox>
#include <QDateTime>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QSet>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

using namespace forkmesh::ui;

namespace {

constexpr qint64 kAgentModelGitRescanMs = 10 * 60 * 1000;
constexpr int kAgentModelGitMaxRepos = 40;

QByteArray agentModelGitRoot(const QByteArray &log)
{
    const int start = log.lastIndexOf('\x1e');
    if (start < 0)
        return QByteArray();
    const int end = log.indexOf('\x1f', start);
    return log.mid(start + 1, end < 0 ? -1 : end - start - 1).trimmed();
}

AgentModelGitHistory scanAgentModelGitHistory(const QStringList &paths)
{
    QHash<QByteArray, QByteArray> logs;
    for (const QString &path : paths) {
        QProcess git;
        git.setProgram(QStringLiteral("git"));
        git.setArguments(QStringList{QStringLiteral("-C"), path} +
                         AgentModelStats::gitLogArguments(QStringLiteral("HEAD")));
        git.start();
        if (!git.waitForFinished(120000) || git.exitStatus() != QProcess::NormalExit ||
            git.exitCode() != 0)
            continue;
        const QByteArray log = git.readAllStandardOutput();
        const QByteArray root = agentModelGitRoot(log);
        if (root.isEmpty())
            continue;
        if (log.size() > logs.value(root).size())
            logs.insert(root, log);
    }
    AgentModelGitHistory history;
    for (const QByteArray &log : std::as_const(logs))
        history.add(AgentModelStats::parseGitHistory(log));
    return history;
}

} // namespace

QWidget *MainWindow::buildAgentModelsPage()
{
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("agentModelsPage"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *header = new QWidget;
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(16, 10, 16, 4);
    headerLayout->setSpacing(10);
    auto *back = new QPushButton(QStringLiteral("← Sessions"));
    back->setObjectName(QStringLiteral("agentModelsBack"));
    back->setFlat(true);
    back->setCursor(Qt::PointingHandCursor);
    back->setToolTip(QStringLiteral("Back to the agent session list"));
    connect(back, &QPushButton::clicked, this, [this] { setAgentPageTab(0); });
    auto *title = new QLabel(QStringLiteral("Models"));
    QFont titleFont = title->font();
    titleFont.setWeight(QFont::DemiBold);
    title->setFont(titleFont);
    auto *subtitle = new QLabel(
        QStringLiteral("Every model's runs, outcomes, approximate cost and tool "
                       "efficiency on this device, with merges backfilled from "
                       "commit trailers. Click a column to sort."));
    subtitle->setObjectName(QStringLiteral("agentModelsSubtitle"));
    subtitle->setWordWrap(false);
    subtitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    QPalette mutedPalette = subtitle->palette();
    mutedPalette.setColor(QPalette::WindowText,
                          mutedPalette.color(QPalette::PlaceholderText));
    subtitle->setPalette(mutedPalette);
    m_agentModelLive = new QLabel;
    m_agentModelLive->setObjectName(QStringLiteral("agentModelsLive"));
    m_agentModelLive->setPalette(mutedPalette);
    auto *hideUnused = new QCheckBox(QStringLiteral("Hide unused"));
    hideUnused->setObjectName(QStringLiteral("agentModelsHideUnused"));
    hideUnused->setToolTip(
        QStringLiteral("Hide models with no runs, merges, commits or tool calls"));
    auto *rescan = new QPushButton(QStringLiteral("Rescan git"));
    rescan->setObjectName(QStringLiteral("agentModelsRescan"));
    rescan->setFlat(true);
    rescan->setCursor(Qt::PointingHandCursor);
    rescan->setToolTip(QStringLiteral(
        "Read every local repository's history again and credit each merge to "
        "the model its commits name in Co-Authored-By or ForkMesh-Agent "
        "trailers"));
    connect(rescan, &QPushButton::clicked, this,
            [this] { startAgentModelGitScan(true); });
    headerLayout->addWidget(back);
    headerLayout->addWidget(title);
    headerLayout->addWidget(subtitle, 1);
    headerLayout->addWidget(m_agentModelLive);
    headerLayout->addWidget(hideUnused);
    headerLayout->addWidget(rescan);
    layout->addWidget(header);

    m_agentModelTable = new AgentModelTable;
    connect(hideUnused, &QCheckBox::toggled, this,
            [this](bool on) { m_agentModelTable->setHideUnused(on); });
    layout->addWidget(m_agentModelTable, 1);

    auto *timer = new QTimer(page);
    timer->setInterval(5000);
    connect(timer, &QTimer::timeout, this, [this, page] {
        if (m_agentPageStack && m_agentPageStack->currentWidget() == page &&
            page->isVisible())
            refreshAgentModelBoard();
    });
    timer->start();
    return page;
}

void MainWindow::refreshAgentModelBoard()
{
    if (!m_agentModelTable)
        return;
    const QList<ComposerModelChoice> choices = composerModelCatalog();
    QList<AgentModelCatalogEntry> catalog;
    catalog.reserve(choices.size());
    for (const ComposerModelChoice &choice : choices)
        catalog.append(AgentModelCatalogEntry{choice.provider, choice.model,
                                              choice.label, choice.agentName});
    const QHash<QString, AgentModelOutcome> retired =
        m_agentStore ? m_agentStore->retiredModelOutcomes()
                     : QHash<QString, AgentModelOutcome>();
    const QVector<AgentToolModelStats> tools =
        m_agentToolLedger ? m_agentToolLedger->modelUsage()
                          : QVector<AgentToolModelStats>();
    const AgentModelGitHistory git =
        m_agentModelGitHistory ? *m_agentModelGitHistory : AgentModelGitHistory();
    const QVector<AgentModelStatsRow> rows = AgentModelStats::build(
        catalog, m_agentSessions, retired, git, tools,
        QDateTime::currentMSecsSinceEpoch());

    m_agentModelTable->iconFor = [choices](const AgentModelStatsRow &row) {
        if (row.catalogIndex >= 0 && row.catalogIndex < choices.size())
            return choices.at(row.catalogIndex).icon;
        return agentModelIcon(row.provider, row.model);
    };
    m_agentModelTable->refresh(rows);

    if (!m_agentModelLive)
        return;
    int used = 0;
    for (const AgentModelStatsRow &row : rows)
        if (row.hasActivity())
            ++used;
    QString status = QStringLiteral("%1 models · %2 used").arg(rows.size()).arg(used);
    if (m_agentModelGitScanning)
        status += QStringLiteral(" · reading git history…");
    else if (m_agentModelGitHistory)
        status += QStringLiteral(" · %1 of %2 merges credited in %3 repositories")
                      .arg(m_agentModelGitHistory->mergesCredited)
                      .arg(m_agentModelGitHistory->mergesScanned)
                      .arg(m_agentModelGitRepos.size());
    m_agentModelLive->setText(status);
}

void MainWindow::startAgentModelGitScan(bool force)
{
    if (m_agentModelGitScanning)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!force && m_agentModelGitHistory &&
        now - m_agentModelGitScannedMs < kAgentModelGitRescanMs)
        return;
    QStringList paths;
    QSet<QString> seen;
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        const QString path = repo.localPath.trimmed().isEmpty()
                                 ? repo.mirrorPath.trimmed()
                                 : repo.localPath.trimmed();
        if (path.isEmpty() || seen.contains(path))
            continue;
        seen.insert(path);
        paths.append(path);
        if (paths.size() >= kAgentModelGitMaxRepos)
            break;
    }
    m_agentModelGitScanning = true;
    refreshAgentModelBoard();
    auto *watcher = new QFutureWatcher<AgentModelGitHistory>(this);
    connect(watcher, &QFutureWatcher<AgentModelGitHistory>::finished, this,
            [this, watcher, paths] {
                watcher->deleteLater();
                m_agentModelGitHistory =
                    std::make_shared<AgentModelGitHistory>(watcher->result());
                m_agentModelGitRepos = paths;
                m_agentModelGitScannedMs = QDateTime::currentMSecsSinceEpoch();
                m_agentModelGitScanning = false;
                refreshAgentModelBoard();
            });
    watcher->setFuture(
        QtConcurrent::run([paths] { return scanAgentModelGitHistory(paths); }));
}
