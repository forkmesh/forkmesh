
#include "ForkMeshVersion.h"
#include "Tooltips.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "SystemPressure.h"
#include "AgentCliLocator.h"
#include "AgentLoginPty.h"
#include "AgentWorktree.h"
#include "RepoStatsStore.h"
#include "ActionStore.h"
#include "ControlNode.h"
#include "BackgroundFileWriter.h"
#include "DiagnosticsReport.h"
#include "DesktopProcessView.h"
#include "DesktopProcesses.h"
#include "CurrentPageStack.h"
#include "MirrorSyncVerdict.h"
#include "D1QuerySource.h"
#include "DatabaseManagerWidget.h"
#include "KebabHeaderView.h"
#include "NetworkReplyError.h"
#include "NodeEventSocket.h"
#include "OfficeChannelMirror.h"
#include "PromptDraftStore.h"
#include "PromptSpellCheckHighlighter.h"
#include "PublicMirrorRuntime.h"
#include "QtRuntimeDiagnostics.h"
#include "RepoSecurity.h"
#include "RewardPoolSigner.h"
#include "SourceCheckout.h"
#include "SingleInstance.h"
#include "StallRepairRouting.h"
#include "TerminalWidget.h"
#include "WorldSpeechBridge.h"

#include <QDialog>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLineEdit>
#include <QSysInfo>
#include <QGuiApplication>
#include <QPlainTextEdit>
#include <QProcess>
#include <QScreen>
#include <QRegularExpression>
#include <QStandardPaths>

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <cstring>
#include <signal.h>
#endif

using namespace forkmesh::ui;

void MainWindow::updateFooterGitIdentity()
{
    if (!m_footerGitIdentity)
        return;
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size()) {
        m_footerGitIdentity->clear();
        return;
    }
    const QString dir = m_repositories.at(m_repoDetailIndex).localPath;
    if (dir.isEmpty()) {
        m_footerGitIdentity->clear();
        return;
    }
    runGitDetached(
        dir, {QStringLiteral("config"), QStringLiteral("--local"),
              QStringLiteral("--get-regexp"),
              QStringLiteral("^user\\.(name|email)$")},
        [this, dir](bool, const QByteArray &out) {
            if (!m_footerGitIdentity)
                return;
            if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size() ||
                m_repositories.at(m_repoDetailIndex).localPath != dir)
                return;
            QString name, email;
            for (const QString &line :
                 QString::fromUtf8(out).split(QLatin1Char('\n'))) {
                const int sp = line.indexOf(QLatin1Char(' '));
                if (sp <= 0)
                    continue;
                const QString key = line.left(sp);
                const QString value = line.mid(sp + 1).trimmed();
                if (key == QLatin1String("user.name"))
                    name = value;
                else if (key == QLatin1String("user.email"))
                    email = value;
            }
            QString text;
            const bool unset = name.isEmpty() && email.isEmpty();
            m_footerGitIdentity->setTextFormat(unset ? Qt::RichText
                                                      : Qt::PlainText);
            if (!name.isEmpty() && !email.isEmpty())
                text = QStringLiteral("· Local Git: %1 <%2>").arg(name, email);
            else if (!name.isEmpty())
                text = QStringLiteral("· Local Git: %1 (email not set)").arg(name);
            else if (!email.isEmpty())
                text = QStringLiteral("· Local Git: %1 (name not set)").arg(email);
            else
                text = QStringLiteral(
                    "· <a href='set-git-identity'>Local Git identity not set</a>");
            m_footerGitIdentity->setText(text);
        });
}

void MainWindow::updateFooterCommitInfo()
{
    if (!m_footerCommitInfo)
        return;
    const QString dir = repoGitDir();
    if (m_footerWorktreeInfo) {
        if (dir.isEmpty() || !repoHasWorkingTree()) {
            m_footerWorktreeInfo->clear();
            m_footerWorktreeInfo->hide();
        } else {
            QString name = QFileInfo(QDir(dir).absolutePath()).fileName();
            if (name.isEmpty())
                name = QDir::toNativeSeparators(dir);
            m_footerWorktreeInfo->setText(QStringLiteral("WT %1").arg(name));
            m_footerWorktreeInfo->setToolTip(
                QStringLiteral("Current worktree: %1")
                    .arg(QDir::toNativeSeparators(dir)));
            m_footerWorktreeInfo->show();
        }
    }
    if (dir.isEmpty()) {
        m_footerCommitInfo->clear();
        return;
    }
    const QString ref = currentRef();
    runGitDetached(
        dir,
        {QStringLiteral("log"), QStringLiteral("-1"), QStringLiteral("--root"),
         QStringLiteral("--shortstat"),
         QStringLiteral("--date=format:%Y-%m-%d %H:%M"),
         QStringLiteral("--pretty=%H%x1f%h%x1f%ad%x1f%s%x1f%an%x1f%ct%x1f%ae"
                        "%x1f%cn%x1f%cd%x1f%D%x1f%b%x1e"),
         ref, QStringLiteral("--")},
        [this, dir, ref](bool ok, const QByteArray &out) {
            if (!m_footerCommitInfo)
                return;
            if (repoGitDir() != dir || currentRef() != ref)
                return;
            const QString raw = QString::fromUtf8(out);
            const QStringList f =
                raw.section(QLatin1Char('\x1e'), 0, 0).split(QLatin1Char('\x1f'));
            if (!ok || f.size() < 5) {
                m_footerCommitInfo->clear();
                m_footerCommitInfo->setToolTip(
                    QStringLiteral("Commit the browsed branch points at"));
                return;
            }
            const QString date = f.at(2), subject = f.at(3), author = f.at(4);
            const QString rel =
                formatShortRelativeTime(f.value(5).toLongLong());
            const QString shortSubject = m_footerCommitInfo->fontMetrics().elidedText(
                subject, Qt::ElideRight, 360);
            m_footerCommitInfo->setText(
                QString::fromUtf8("\xC2\xB7 Last: %1 \xC2\xB7 %2 \xC2\xB7 %3 \xC2\xB7 %4")
                    .arg(f.at(1), date, shortSubject, author));

            const QString email = f.value(6).trimmed();
            const QString committer = f.value(7).trimmed();
            const QString commitDate = f.value(8).trimmed();
            const QString refs = f.value(9).trimmed();
            QString body = f.mid(10).join(QLatin1Char('\x1f')).trimmed();
            const QString stat = raw.section(QLatin1Char('\x1e'), 1)
                                     .trimmed()
                                     .section(QLatin1Char('\n'), 0, 0);

            const QString muted = QStringLiteral("#8b949e");
            QStringList lines;
            lines << QStringLiteral("<b>%1</b>").arg(subject.toHtmlEscaped());
            if (!body.isEmpty()) {
                if (body.size() > 800)
                    body = body.left(800) + QString::fromUtf8("\xE2\x80\xA6");
                lines << QStringLiteral("<span style='color:%1'>%2</span>")
                             .arg(muted, body.toHtmlEscaped().replace(
                                             QLatin1Char('\n'),
                                             QStringLiteral("<br>")));
            }
            QStringList meta;
            meta << QStringLiteral("Commit: %1").arg(f.at(0).toHtmlEscaped());
            if (!refs.isEmpty())
                meta << QStringLiteral("Refs: %1").arg(refs.toHtmlEscaped());
            meta << QStringLiteral("Author: %1")
                        .arg((email.isEmpty()
                                  ? author
                                  : QStringLiteral("%1 <%2>").arg(author, email))
                                 .toHtmlEscaped());
            if (!committer.isEmpty() && committer != author)
                meta << QStringLiteral("Committer: %1").arg(committer.toHtmlEscaped());
            meta << QStringLiteral("Authored: %1%2")
                        .arg(date.toHtmlEscaped(),
                             rel.isEmpty()
                                 ? QString()
                                 : QString::fromUtf8(" (%1 ago)").arg(rel));
            if (!commitDate.isEmpty() && commitDate != date)
                meta << QStringLiteral("Committed: %1").arg(commitDate.toHtmlEscaped());
            if (!stat.isEmpty())
                meta << QStringLiteral("Changes: %1").arg(stat.toHtmlEscaped());
            lines << QStringLiteral("<span style='color:%1'>%2</span>")
                         .arg(muted, meta.join(QStringLiteral("<br>")));

            m_footerCommitInfo->setToolTip(
                QStringLiteral("<table cellspacing='0' cellpadding='0'><tr>"
                               "<td width='460'>%1</td></tr></table>")
                    .arg(lines.join(QStringLiteral("<br><br>"))));
        });
}

QString MainWindow::stallLogPath()
{
#ifdef FORKMESH_WINDOW_TESTS
    return QString();
#else
    return forkmesh::paths::diagnosticsDir() + QStringLiteral("/stalls.log");
#endif
}

void MainWindow::startDiagnostics()
{
    if (!m_qtDiagnostics) {
        m_qtDiagnostics = new QtRuntimeDiagnostics(this);
        m_qtDiagnostics->onSnapshot = [this](const auto &) {
            updateQtDiagnosticTiles();
        };
        m_qtDiagnostics->setDetailedEnabled(
            m_statusVersionButton && m_statusVersionButton->isChecked());
        updateQtDiagnosticTiles();
    }
    if (!m_stallWatchdog) {
        m_stallWatchdog = new StallWatchdog(this);
        connect(m_stallWatchdog, &StallWatchdog::stalled, this, &MainWindow::onUiStall);
        const QString logPath = stallLogPath();
        m_stallLogPath = logPath;
        if (QFileInfo(logPath).size() > 4 * 1024 * 1024) {
            const QString prev = logPath + QStringLiteral(".1");
            QFile::remove(prev);
            QFile::rename(logPath, prev);
        }
        const QString buildInfo =
            QStringLiteral("ForkMesh v" FORKMESH_VERSION " (src ") + forkmesh::sourceDir() +
            QStringLiteral(")");
        m_stallWatchdog->start(/*stallThresholdMs=*/500, logPath, buildInfo);
        if (!logPath.isEmpty())
            logSystem(QStringLiteral("UI-stall watchdog armed; reports append to %1")
                          .arg(QDir::toNativeSeparators(logPath)));
    }
    if (!m_diagTimer) {
        m_diagTimer = new QTimer(this);
        m_diagTimer->setInterval(1000); // one sample a second into the charts
        connect(m_diagTimer, &QTimer::timeout, this,
                &MainWindow::updateFooterDiagnostics);
        connect(m_diagTimer, &QTimer::timeout, this, &MainWindow::sampleHostVitals);
        m_diagTimer->start();
    }
    resumeErrorLogMonitoringTeammates();
    updateFooterDiagnostics();
}

void MainWindow::updateQtDiagnosticTiles()
{
    if (!m_qtDiagnostics || m_qtDiagnosticTiles.size() < 5)
        return;
    const QtRuntimeDiagnostics::Snapshot s = m_qtDiagnostics->snapshot();
    auto *loop = static_cast<ResourceSparkline *>(m_qtDiagnosticTiles.at(0));
    auto *events = static_cast<ResourceSparkline *>(m_qtDiagnosticTiles.at(1));
    auto *paint = static_cast<ResourceSparkline *>(m_qtDiagnosticTiles.at(2));
    auto *ui = static_cast<ResourceSparkline *>(m_qtDiagnosticTiles.at(3));
    auto *pool = static_cast<ResourceSparkline *>(m_qtDiagnosticTiles.at(4));

    loop->addSample(s.loopLagMs, 200.0,
                    QStringLiteral("%1ms").arg(s.loopLagMs));
    loop->setToolTip(
        QStringLiteral("Qt event-loop lag · current %1 ms · average %2 ms · "
                       "p95 %3 ms · session peak %4 ms\nClick for the full "
                       "Qt runtime report")
            .arg(s.loopLagMs)
            .arg(s.averageLoopLagMs)
            .arg(s.p95LoopLagMs)
            .arg(s.peakLoopLagMs));

    events->addSample(s.eventsPerSecond, qMax(100, s.eventsPerSecond * 2),
                      QStringLiteral("%1/s").arg(s.eventsPerSecond));
    events->setToolTip(
        QStringLiteral("Qt events delivered: %1/s · dispatcher wakeups %2/s · "
                       "active %3%\nDetailed accounting is %4. Click for the "
                       "event-group breakdown.")
            .arg(s.eventsPerSecond)
            .arg(s.wakeupsPerSecond)
            .arg(s.dispatcherBusyPercent, 0, 'f', 1)
            .arg(s.detailed ? QStringLiteral("on") : QStringLiteral("off")));

    const int visualRate = s.paintsPerSecond + s.layoutsPerSecond;
    paint->addSample(visualRate, qMax(100, visualRate * 2),
                     QStringLiteral("%1/s").arg(visualRate));
    paint->setToolTip(
        QStringLiteral("Qt visual churn · paint/update %1/s · layout/geometry "
                       "%2/s\nClick for the full Qt runtime report")
            .arg(s.paintsPerSecond)
            .arg(s.layoutsPerSecond));

    ui->addSample(s.visibleWidgetCount, qMax(1, s.widgetCount),
                  QStringLiteral("%1").arg(s.visibleWidgetCount));
    ui->setToolTip(
        QStringLiteral("Qt object tree · widgets %1 visible / %2 total · windows "
                       "%3/%4 · application QObjects %5\nClick for the full "
                       "Qt runtime report")
            .arg(s.visibleWidgetCount)
            .arg(s.widgetCount)
            .arg(s.visibleWindowCount)
            .arg(s.windowCount)
            .arg(s.objectCount));

    pool->addSample(s.threadPoolActive, qMax(1, s.threadPoolMax),
                    QStringLiteral("%1/%2")
                        .arg(s.threadPoolActive)
                        .arg(s.threadPoolMax));
    pool->setToolTip(
        QStringLiteral("Qt workers · global thread pool %1 active / %2 max · "
                       "QTimers %3 active / %4 total\nClick for the full Qt "
                       "runtime report")
            .arg(s.threadPoolActive)
            .arg(s.threadPoolMax)
            .arg(s.activeTimerCount)
            .arg(s.timerCount));

    if (m_qtDiagnosticsReport && m_qtDiagnosticsDialog &&
        m_qtDiagnosticsDialog->isVisible() &&
        (m_qtDiagnosticsReportLastRefreshMs <= 0 ||
         s.sampledAtMs - m_qtDiagnosticsReportLastRefreshMs >= 5000)) {
        m_qtDiagnosticsReportLastRefreshMs = s.sampledAtMs;
        QScrollBar *bar = m_qtDiagnosticsReport->verticalScrollBar();
        const int position = bar ? bar->value() : 0;
        m_qtDiagnosticsReport->setPlainText(m_qtDiagnostics->report());
        if (bar)
            bar->setValue(position);
    }
}

void MainWindow::updateFooterDiagnostics()
{
    auto *uptime = static_cast<ResourceSparkline *>(m_debugUptimeTile);
    if (uptime && m_debugSessionUptime.isValid()) {
        const qint64 elapsed = m_debugSessionUptime.elapsed();
        uptime->setSamples({}, 1.0, formatDuration(elapsed));
        uptime->setToolTip(
            QStringLiteral("ForkMesh has been running for %1 this session")
                .arg(formatDuration(elapsed)));
    }

    auto updateActivityTile = [](ResourceSparkline *tile, qint64 total,
                                 qint64 *previous, const QString &kind) {
        if (!tile)
            return;
        const qint64 rate = *previous < 0 ? 0 : qMax<qint64>(0, total - *previous);
        *previous = total;
        tile->addSample(rate, qMax<qint64>(5, rate * 2),
                        QStringLiteral("%1/s").arg(rate));
        tile->setToolTip(
            QStringLiteral("%1 activity: %2 event%3 this second, %4 this session")
                .arg(kind)
                .arg(rate)
                .arg(rate == 1 ? QString() : QStringLiteral("s"))
                .arg(total));
    };
    updateActivityTile(static_cast<ResourceSparkline *>(m_debugNetworkActivityTile),
                       m_debugNetworkRequestTotal,
                       &m_debugLastNetworkRequestTotal,
                       QStringLiteral("Network request"));
    updateActivityTile(static_cast<ResourceSparkline *>(m_debugLogActivityTile),
                       m_debugAllLogCount, &m_debugLastLogActivityTotal,
                       QStringLiteral("Log"));
    auto updateTokenTile = [](ResourceSparkline *tile, qint64 total,
                              qint64 *previous, const QString &kind) {
        if (!tile)
            return;
        const qint64 rate = *previous < 0 ? 0 : qMax<qint64>(0, total - *previous);
        *previous = total;
        const QString rateText =
            rate >= 1000 ? QStringLiteral("%1k").arg(rate / 1000.0, 0, 'f', 1)
                         : QString::number(rate);
        tile->addSample(rate, qMax<qint64>(1000, rate * 2), rateText);
        tile->setToolTip(
            QStringLiteral("%1: %2 token%3 this second, %4 this session")
                .arg(kind, formatCount(rate),
                     rate == 1 ? QString() : QStringLiteral("s"),
                     formatCount(total)));
    };
    updateTokenTile(static_cast<ResourceSparkline *>(m_debugTokensInTile),
                    m_debugTokensInTotal, &m_debugLastTokensInTotal,
                    QStringLiteral("Input tokens sent to agent models"));
    updateTokenTile(static_cast<ResourceSparkline *>(m_debugTokensOutTile),
                    m_debugTokensOutTotal, &m_debugLastTokensOutTotal,
                    QStringLiteral("Output tokens received from agent models"));
    if (auto *stall = static_cast<ResourceSparkline *>(m_debugStallTile)) {
        const qint64 peak = m_debugStallPeakSinceSampleMs;
        m_debugStallPeakSinceSampleMs = 0;
        stall->addSample(qMin<qint64>(peak, kDebugStallChartMaxMs),
                         kDebugStallChartMaxMs, QStringLiteral("%1ms").arg(peak));
        QString tip = QStringLiteral("UI stalls: %1 this session").arg(m_stallCount);
        if (m_stallCount > 0)
            tip += QStringLiteral(" · longest %1 ms · last %2 ago")
                       .arg(m_debugStallLongestMs)
                       .arg(formatDuration(QDateTime::currentMSecsSinceEpoch() -
                                           m_debugStallLastAtMs));
        tip += QStringLiteral("\nThe longest time the window froze each second. "
                              "Click to see the stalls in the Log");
        stall->setToolTip(tip);
    }

    const qint64 fdNow = QDateTime::currentMSecsSinceEpoch();
    if (fdNow - m_fdPressureLastCheckMs >= 15000) {
        m_fdPressureLastCheckMs = fdNow;
        checkFileDescriptorPressure();
    }
    const qint64 statsNow = QDateTime::currentMSecsSinceEpoch();
    if (statsNow - m_repoStatsLastRefreshMs >= 60000) {
        m_repoStatsLastRefreshMs = statsNow;
        refreshRepositoryStats();
    }
    double cpuPct = -1.0;
    long rssMb = -1;
#if defined(__linux__)
    QFile stat(QStringLiteral("/proc/self/stat"));
    if (stat.open(QIODevice::ReadOnly)) {
        const QByteArray s = stat.readAll();
        const int rp = s.lastIndexOf(')'); // comm field may hold spaces/parens
        const QList<QByteArray> f = s.mid(rp + 2).split(' ');
        if (f.size() > 12) { // utime=14th, stime=15th field overall
            const qulonglong ticks = f.at(11).toULongLong() + f.at(12).toULongLong();
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            if (m_diagLastCpuTicks > 0 && nowMs > m_diagLastCpuMs) {
                const double dTicks = double(ticks) - double(m_diagLastCpuTicks);
                const double dSec = (nowMs - m_diagLastCpuMs) / 1000.0;
                const long hz = sysconf(_SC_CLK_TCK);
                if (hz > 0 && dSec > 0)
                    cpuPct = qMax(0.0, (dTicks / hz) / dSec * 100.0);
            }
            m_diagLastCpuTicks = ticks;
            m_diagLastCpuMs = nowMs;
        }
    }
    QFile statm(QStringLiteral("/proc/self/statm"));
    if (statm.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> p = statm.readAll().split(' ');
        if (p.size() > 1)
            rssMb = static_cast<long>((p.at(1).toULongLong() * sysconf(_SC_PAGESIZE)) /
                                      (1024 * 1024));
    }
#endif
    m_debugLastCpuPercent = cpuPct;
    if (cpuPct >= 0.0)
        m_diagnosticsCpuAverage = m_diagnosticsCpuAverage < 0.0
                                      ? cpuPct
                                      : 0.8 * m_diagnosticsCpuAverage + 0.2 * cpuPct;
    auto *resources = static_cast<ResourceQuadrantSparkline *>(m_resourceChart);
    if (resources) {
        resources->addSample(ResourceQuadrantSparkline::Cpu,
                             cpuPct >= 0 ? cpuPct : 0.0, 100.0);
        QString tip = QStringLiteral("CPU used by this app");
        if (cpuPct >= 0)
            tip += QStringLiteral(": %1%").arg(cpuPct, 0, 'f', 0);
        if (rssMb >= 0)
            tip += QString::fromUtf8(" \xC2\xB7 %1\xE2\x80\xAFMB resident").arg(rssMb);
        resources->setResourceToolTip(ResourceQuadrantSparkline::Cpu, tip);
    }
    double hostMemoryPct = -1.0;
    if (resources) {
        const qint64 total = SystemStats::totalMemoryBytes();
        const qint64 avail = SystemStats::availableMemoryBytes();
        double pct = -1.0;
        if (total > 0 && avail >= 0 && avail <= total)
            pct = 100.0 * double(total - avail) / double(total);
        hostMemoryPct = pct;
        if (hostMemoryPct >= 0.0) {
            if (m_highMemoryHostPct >= 0.0) {
                const double delta = hostMemoryPct - m_highMemoryHostPct;
                if (qAbs(delta) < 0.05)
                    m_highMemoryHostTrendArrow = QStringLiteral("→");
                else if (delta > 0.0)
                    m_highMemoryHostTrendArrow = QStringLiteral("↑");
                else
                    m_highMemoryHostTrendArrow = QStringLiteral("↓");
            }
            m_highMemoryHostPct = hostMemoryPct;
            updateHighMemoryHeading();
        }
        resources->addSample(ResourceQuadrantSparkline::Memory,
                             pct >= 0 ? pct : 0.0, 100.0);
        resources->setResourceToolTip(
            ResourceQuadrantSparkline::Memory,
            total > 0
                ? QStringLiteral("Host memory in use: %1 of %2")
                      .arg(SystemStats::formatBytes(total - avail),
                           SystemStats::formatBytes(total))
                : QStringLiteral("Host memory in use"));
    }
    if (resources) {
        const qint64 total = SystemStats::totalSwapBytes();
        const qint64 free = SystemStats::freeSwapBytes();
        double pct = -1.0;
        if (total > 0 && free >= 0 && free <= total)
            pct = 100.0 * double(total - free) / double(total);
        resources->addSample(ResourceQuadrantSparkline::Swap,
                             pct >= 0 ? pct : 0.0, 100.0);
        resources->setResourceToolTip(
            ResourceQuadrantSparkline::Swap,
            total > 0
                ? QStringLiteral("Swap in use: %1 of %2 (%3 free)")
                      .arg(SystemStats::formatBytes(total - free),
                           SystemStats::formatBytes(total),
                           SystemStats::formatBytes(free))
                : QStringLiteral("Swap is disabled or unavailable"));
    }
#ifndef FORKMESH_WINDOW_TESTS
    if (hostMemoryPct >= 85.0 && m_highMemoryAlertArmed) {
        m_highMemoryAlertArmed = false;
        endHighMemoryAutoKill();
        m_highMemoryAutoKillPending =
            QSettings().value(kHighMemoryAutoKillSetting, false).toBool();
        QTimer::singleShot(0, this, [this] { showHighMemoryProcessPanel(false); });
    } else if (hostMemoryPct >= 0.0 && hostMemoryPct < 80.0) {
        m_highMemoryAlertArmed = true;
        if (m_highMemoryDialog && m_highMemoryDialog->isVisible())
            m_highMemoryDialog->hide();
    }
#endif
    if (resources) {
        const QString path = QDir::homePath();
        const qint64 total = SystemStats::diskTotalBytes(path);
        const qint64 free = SystemStats::diskFreeBytes(path);
        double pct = -1.0;
        if (total > 0 && free >= 0 && free <= total)
            pct = 100.0 * double(total - free) / double(total);
        resources->addSample(ResourceQuadrantSparkline::Disk,
                             pct >= 0 ? pct : 0.0, 100.0);
        resources->setResourceToolTip(
            ResourceQuadrantSparkline::Disk,
            total > 0
                ? QStringLiteral("Drive space in use: %1 of %2 (%3 free)")
                      .arg(SystemStats::formatBytes(total - free),
                           SystemStats::formatBytes(total),
                           SystemStats::formatBytes(free))
                : QStringLiteral("Drive space in use"));
    }

    const QString stallTip =
        m_stallCount > 0
            ? QStringLiteral("%1 UI stall%2 recorded this session. Click to draft "
                             "a fix-it prompt; right-click for captured backtraces.")
                  .arg(m_stallCount)
                  .arg(m_stallCount == 1 ? QString() : QStringLiteral("s"))
            : QStringLiteral("No UI stalls recorded this session. Click for UI-stall "
                             "diagnostics; right-click for captured backtraces.");
    if (m_logActivityLights)
        m_logActivityLights->setStallToolTip(stallTip);
    if (m_logActivityHeader)
        m_logActivityHeader->setStallToolTip(stallTip);
    updateDiagnosticsAlert();
}

int MainWindow::crashBannerLineCount() const
{
    int count = 0;
    for (const QString &line : std::as_const(m_networkLog))
        if (line.contains(forkmesh::crashlog::kCrashBanner))
            ++count;
    return count;
}

void MainWindow::updateDiagnosticsAlert()
{
    auto *button = dynamic_cast<ActivityRailButton *>(m_debugDiagnosticsButton);
    if (!button)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_diagnosticsAlertLastCheckMs > 0 && now - m_diagnosticsAlertLastCheckMs < 5000)
        return;
    m_diagnosticsAlertLastCheckMs = now;

    const int crashes = crashBannerLineCount();
    m_diagnosticsAckCrashes = qMin(m_diagnosticsAckCrashes, crashes);
    m_diagnosticsAckStalls = qMin(m_diagnosticsAckStalls, m_stallCount);
    m_diagnosticsAckSecurity = qMin(m_diagnosticsAckSecurity, m_securityIncidentCount);

    forkmesh::diagreport::AlertInputs in;
    in.crashes = crashes - m_diagnosticsAckCrashes;
    in.stalls = m_stallCount - m_diagnosticsAckStalls;
    in.securityIncidents = m_securityIncidentCount - m_diagnosticsAckSecurity;
    if (in.securityIncidents > 0 && !m_securityIncidentLines.isEmpty())
        in.securitySample = m_securityIncidentLines.last();
    if (m_qtDiagnostics)
        in.loopLagP95Ms = m_qtDiagnostics->snapshot().p95LoopLagMs;
    in.cpuPercent = m_diagnosticsCpuAverage;
    in.residentBytes = SystemStats::residentBytes();
    in.hostMemoryTotal = SystemStats::totalMemoryBytes();
    in.hostMemoryAvailable = SystemStats::availableMemoryBytes();
    in.diskTotal = SystemStats::diskTotalBytes(QDir::homePath());
    in.diskFree = SystemStats::diskFreeBytes(QDir::homePath());
    in.openFiles = SystemStats::openFileCount();
    in.openFileLimit = SystemStats::openFileSoftLimit();
    const forkmesh::diagreport::Alert alert = forkmesh::diagreport::alert(in);

    m_diagnosticsAlertDetected = alert.detected();
    button->setDangerTint(m_diagnosticsAlertDetected);
    const forkmesh::teammates::AgentChoice choice = promptAreaAgentChoice();
    QString agent = agentProviderName(choice.provider);
    if (const QString model = agentModelLabel(choice.model); !model.isEmpty())
        agent += QStringLiteral(" (%1)").arg(model);
    if (agent.trimmed().isEmpty())
        agent = QStringLiteral("the selected agent");
    const QString action =
        QStringLiteral("Click to start %1 with a diagnostic snapshot to fix them. "
                       "Right-click to copy the report instead.")
            .arg(agent);
    button->setToolTip(
        m_diagnosticsAlertDetected
            ? QStringLiteral("Diagnostics found issues:\n%1\n\n%2")
                  .arg(alert.summary(), action)
            : QStringLiteral("No performance, security or UI issues detected. "
                             "Click to start %1 with a diagnostic snapshot to "
                             "fix errors and optimize resource usage. "
                             "Right-click to copy the report instead.")
                  .arg(agent));
    button->setAccessibleDescription(m_diagnosticsAlertDetected
                                         ? alert.summary()
                                         : QStringLiteral("No issues detected"));
}

void MainWindow::acknowledgeDiagnosticsAlert()
{
    m_diagnosticsAckCrashes = crashBannerLineCount();
    m_diagnosticsAckStalls = m_stallCount;
    m_diagnosticsAckSecurity = m_securityIncidentCount;
    m_diagnosticsAlertLastCheckMs = 0;
    updateDiagnosticsAlert();
}

void MainWindow::checkFileDescriptorPressure()
{
    const int limit = SystemStats::openFileSoftLimit();
    const int open = SystemStats::openFileCount();
    if (limit <= 0 || open <= 0)
        return; // platform doesn't expose them; nothing to police
    const double used = 100.0 * double(open) / double(limit);
    if (used >= 70.0 && m_fdPressureAlertArmed) {
        m_fdPressureAlertArmed = false;
        logSystem(QStringLiteral(
                      "Warning: %1 of %2 file descriptors in use (%3%), %4 "
                      "threads. Descriptor exhaustion aborts the app, so this "
                      "is worth reporting with what was running.")
                      .arg(open)
                      .arg(limit)
                      .arg(used, 0, 'f', 0)
                      .arg(SystemStats::threadCount()));
    } else if (used < 55.0) {
        m_fdPressureAlertArmed = true;
    }
}

namespace {
struct RepoTrendCacheEntry {
    QString day;
    QVector<RepoStatsSample> days;
    bool ratchet = false;
    QString error;
};
QHash<QString, RepoTrendCacheEntry> g_repoTrendCache;
QSet<QString> g_repoTrendCapturing; // repos with a capture pass in flight
} // namespace

void MainWindow::refreshRepositoryStats()
{
    if (!m_repoSizeChart || !m_repoLinesChart || !m_repoFilesChart)
        return;
    const QString dir = repoGitDir();
    const bool available = !dir.isEmpty() && repoHasWorkingTree();
    const QList<QWidget *> statsWidgets{m_repoSizeChart, m_repoLinesChart,
                                        m_repoFilesChart, m_repoRatchetButton};
    for (QWidget *widget : statsWidgets)
        if (widget) widget->setVisible(available);
    if (!available) return;

    const QString today = QDate::currentDate().toString(Qt::ISODate);
    RepoTrendCacheEntry &cached = g_repoTrendCache[dir];
    if (cached.day != today || cached.days.isEmpty()) {
        if (g_repoTrendCapturing.contains(dir))
            return;
        g_repoTrendCapturing.insert(dir);
        runOffThread<RepoTrendCacheEntry>(
            [dir, today] {
                RepoTrendCacheEntry fresh;
                fresh.day = today;
                fresh.days = RepoStatsStore::captureDaily(dir, &fresh.error);
                fresh.ratchet = RepoStatsStore::ratchetEnabled(dir);
                return fresh;
            },
            [this, dir](RepoTrendCacheEntry fresh) {
                g_repoTrendCapturing.remove(dir);
                if (fresh.days.isEmpty()) {
                    if (!fresh.error.isEmpty())
                        logSystem(
                            QStringLiteral("Repository stats: %1").arg(fresh.error));
                    return;
                }
                g_repoTrendCache[dir] = fresh;
                refreshRepositoryStats(); // now a cache hit: paints, runs no git
            });
        return;
    }
    const QVector<RepoStatsSample> &days = cached.days;
    if (days.isEmpty())
        return;
    QVector<double> sizes, lines, files;
    double maxSize = 1, maxLines = 1, maxFiles = 1;
    for (const RepoStatsSample &day : days) {
        sizes << double(day.bytes);
        lines << double(day.lines);
        files << double(day.files);
        maxSize = qMax(maxSize, double(day.bytes));
        maxLines = qMax(maxLines, double(day.lines));
        maxFiles = qMax(maxFiles, double(day.files));
    }
    const RepoStatsSample &latest = days.last();
    static_cast<ResourceSparkline *>(m_repoSizeChart)->setSamples(
        sizes, maxSize, SystemStats::formatBytes(latest.bytes));
    static_cast<ResourceSparkline *>(m_repoLinesChart)->setSamples(
        lines, maxLines, QString::number(latest.lines));
    static_cast<ResourceSparkline *>(m_repoFilesChart)->setSamples(
        files, maxFiles, QString::number(latest.files));
    const QString span = days.size() == 1
                             ? QStringLiteral("today")
                             : QStringLiteral("%1 to %2").arg(days.first().day,
                                                               days.last().day);
    m_repoSizeChart->setToolTip(QStringLiteral("Tracked repository size, %1").arg(span));
    m_repoLinesChart->setToolTip(QStringLiteral("Tracked lines of code, %1").arg(span));
    m_repoFilesChart->setToolTip(QStringLiteral("Tracked files, %1").arg(span));
    if (m_repoRatchetButton) {
        QSignalBlocker blocker(m_repoRatchetButton);
        m_repoRatchetButton->setChecked(cached.ratchet);
    }
}

void MainWindow::toggleRepositoryRatchet(bool enabled)
{
    const QString dir = repoGitDir();
    QString error;
    if (dir.isEmpty() || !RepoStatsStore::setRatchetEnabled(dir, enabled, &error)) {
        if (m_repoRatchetButton) {
            QSignalBlocker blocker(m_repoRatchetButton);
            m_repoRatchetButton->setChecked(!enabled);
        }
        flashMessage(QStringLiteral("Could not update Ratchet Mode: %1").arg(error), true);
        return;
    }
    if (auto entry = g_repoTrendCache.find(dir); entry != g_repoTrendCache.end())
        entry->ratchet = enabled;
    flashMessage(enabled ? QStringLiteral("Ratchet Mode enabled: the repository may not grow "
                                          "past the size it is now until tomorrow.")
                         : QStringLiteral("Ratchet Mode disabled: commits are no longer checked."));
}

namespace {
bool stallSessionIsActive(const AgentSession *session)
{
    return session &&
           (session->status == AgentStatus::Running ||
            session->status == AgentStatus::Queued ||
            session->status == AgentStatus::Waiting);
}

QString normalizedStallSignature(const QString &blockingCall,
                                 const QString &likelyOrigin,
                                 const QString &backtrace)
{
    const QString lowerCall = blockingCall.trimmed().toLower();
    const QString evidence = likelyOrigin + QLatin1Char('\n') + backtrace;
    // A watchdog sample can land in an allocator, a regex, or a string join at
    // several different offsets during one logical render.  Group by the
    // owning feature before considering those sampled frames.  Otherwise one
    // pull refresh produced five different auto-agent tasks, whose builds then
    // competed with the GUI and made the original burst worse.
    if (evidence.contains(QLatin1String("showPull")) ||
        evidence.contains(QLatin1String("refreshPullList")) ||
        evidence.contains(QLatin1String("applyLoadedPulls")) ||
        evidence.contains(QLatin1String("pullFileAuthorship"))) {
        return QStringLiteral("pull-detail-refresh");
    }
    if (evidence.contains(QLatin1String("renderDiff")) ||
        evidence.contains(QLatin1String("setDiffHtml")) ||
        evidence.contains(QLatin1String("splitDiffFile"))) {
        return QStringLiteral("diff-render");
    }
    if (evidence.contains(QLatin1String("MessageRow")) ||
        evidence.contains(QLatin1String("renderConversationRows")) ||
        evidence.contains(QLatin1String("refreshChatMembers"))) {
        return QStringLiteral("chat-render");
    }
    if (evidence.contains(QLatin1String("renderCalendar")))
        return QStringLiteral("calendar-render");
    if (evidence.contains(QLatin1String("appendNetworkLogLine")) ||
        evidence.contains(QLatin1String("rebuildNetworkLogView")) ||
        evidence.contains(QLatin1String("LogTimeline"))) {
        return QStringLiteral("log-view-render");
    }
    // Initial setHtml, streamed insertHtml, and Qt's deferred layout timers are
    // three samples of the same rich-document render, not three repair jobs.
    // Check the owning feature first so one pull refresh cannot file both a
    // pull-detail task and a generic rich-text task.
    if (lowerCall.startsWith(QLatin1String("diff html ")) ||
        evidence.contains(QLatin1String("QTextDocumentLayout")) ||
        evidence.contains(QLatin1String("QTextCursor10insertHtml"))) {
        return QStringLiteral("rich-text-layout");
    }
    if (!lowerCall.isEmpty()) {
        QString stable = lowerCall;
        stable.replace(QRegularExpression(QStringLiteral("[0-9]+")),
                       QStringLiteral("#"));
        return stable.simplified();
    }

    if (!likelyOrigin.trimmed().isEmpty()) {
        QString stable = likelyOrigin.trimmed().toLower();
        stable.replace(QRegularExpression(QStringLiteral("0x[0-9a-f]+")),
                       QStringLiteral("0x#"));
        stable.replace(QRegularExpression(QStringLiteral("[0-9]+")),
                       QStringLiteral("#"));
        return stable.simplified();
    }

    QStringList appFrames;
    for (const QString &line : backtrace.split(QLatin1Char('\n'))) {
        if (!line.contains(QLatin1String("forkmesh(")))
            continue;
        const int open = line.indexOf(QLatin1Char('('));
        int end = line.indexOf(QLatin1String("+0x"), open + 1);
        if (end < 0)
            end = line.indexOf(QLatin1Char(')'), open + 1);
        if (open < 0 || end <= open + 1)
            continue;
        const QString frame = line.mid(open + 1, end - open - 1);
        if (frame == QLatin1String("main") || frame == QLatin1String("_start") ||
            frame.contains(QLatin1String("ForkMeshApplication6notify")))
            continue;
        appFrames.append(frame);
        if (appFrames.size() == 2)
            break;
    }
    if (!appFrames.isEmpty())
        return appFrames.join(QLatin1Char('|'));
    return QStringLiteral("unattributed-ui-stall");
}
} // namespace

void MainWindow::onUiStall(qint64 peakMs, const QString &stallId,
                           const QString &likelyOrigin,
                           const QString &blockingCall,
                           const QString &report, qint64 startedAtMs)
{
    ++m_stallCount;
    m_debugStallPeakSinceSampleMs = qMax(m_debugStallPeakSinceSampleMs, peakMs);
    m_debugStallLongestMs = qMax(m_debugStallLongestMs, peakMs);
    m_debugStallLastAtMs = QDateTime::currentMSecsSinceEpoch();
    const QString when = QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss"));
    const QString marker =
        stallId.isEmpty()
            ? QStringLiteral("STALL-SESSION-p%1-%2")
                  .arg(QCoreApplication::applicationPid())
                  .arg(m_stallCount, 4, 10, QLatin1Char('0'))
            : stallId;
    const QString origin =
        likelyOrigin.isEmpty()
            ? stallwatch::likelyOrigin(
                  report, QCoreApplication::applicationFilePath())
            : likelyOrigin;
    QString head = QStringLiteral("UI stalled ~%1 ms (event loop blocked)").arg(peakMs);
    if (!blockingCall.isEmpty())
        head += QStringLiteral(" while %1").arg(blockingCall);
    if (!origin.isEmpty())
        head += QStringLiteral(" - likely origin: %1").arg(origin);
    head += QStringLiteral(" [%1]").arg(marker);
    QString logLine = head;
    if (!m_stallLogPath.isEmpty())
        logLine += QStringLiteral(" - details: search %1 in %2")
                       .arg(marker,
                            QDir::toNativeSeparators(m_stallLogPath));
    const QString signature =
        normalizedStallSignature(blockingCall, origin, report);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    // Anything reached from this watchdog callback (including auto-agent
    // startup logs) must stay away from the QTextDocument that may have caused
    // the watchdog sample in the first place.
    const QScopedValueRollback<bool> deferRichText(m_deferRichLogRendering,
                                                   true);
    constexpr qint64 kStallReportCoalesceMs = 10 * 1000;
    const bool duplicateReport =
        signature == m_lastStallReportSignature &&
        nowMs - m_lastStallReportMs < kStallReportCoalesceMs;
    const bool precedesClear =
        (startedAtMs < 0 ? nowMs : startedAtMs) < m_networkLogClearedAtMs;
    if (!precedesClear && !duplicateReport) {
        m_lastStallReportSignature = signature;
        m_lastStallReportMs = nowMs;
        logSystem(logLine); // counts itself through the STALL activity light
    } else if (!precedesClear) {
        // The log line is coalesced so a repeating freeze does not flood the
        // log, but the freeze still happened: count it, or the STALL tally
        // disagrees with the stall log the tooltip quotes.
        pulseLogActivityLights(QStringLiteral("STALL"));
    }
    // Past five seconds the compositor stops trusting the window (GNOME's
    // check-alive-timeout) and offers Force Quit, so the person saw this one.
    // Sub-second stalls are tuning material for the stall log; a freeze the
    // operator had to sit through is an ERROR that joins the error-log
    // batch the Fixer teammate works from, with the machine's state and the
    // full watchdog report. It stays in the log: no ping, no relay report.
    constexpr qint64 kUserVisibleStallMs = 5000;
    if (peakMs >= kUserVisibleStallMs) {
        const forkmesh::SystemPressureSnapshot pressure =
            forkmesh::cachedSystemPressure();
        const bool starved = forkmesh::systemUnderPressure(pressure) ||
                             report.contains(QLatin1String(
                                 "verdict: machine-wide memory/io starvation"));
        const QString seconds =
            QString::number(double(peakMs) / 1000.0, 'f', peakMs >= 10000 ? 0 : 1);
        QString froze =
            QStringLiteral("UI froze for %1 s (the window manager may have "
                           "offered Force Quit)")
                .arg(seconds);
        if (!origin.isEmpty())
            froze += QStringLiteral(" - likely origin: %1").arg(origin);
        if (!blockingCall.isEmpty())
            froze += QStringLiteral(" - while %1").arg(blockingCall);
        const QString system = forkmesh::describeSystemPressure(pressure);
        if (starved) {
            froze += QStringLiteral(
                " - the machine was under memory/io pressure, so this is the "
                "GUI thread waiting on paging rather than slow code");
        }
        if (!system.isEmpty())
            froze += QStringLiteral(" - system: %1").arg(system);
        froze += QStringLiteral(" [%1]").arg(marker);
        QStringList details;
        for (const QString &line : report.split(QLatin1Char('\n'))) {
            const QString trimmed = line.simplified();
            if (!trimmed.isEmpty())
                details.append(trimmed);
        }
        if (!details.isEmpty())
            froze += QStringLiteral(" - details: ") +
                     details.join(QStringLiteral(" | "));
        const QScopedValueRollback<bool> quiet(m_muteLoggedErrorAlert, true);
        if (!precedesClear)
            logSystemFrom(froze, forkmesh::logSourceRelativePath(__FILE__),
                          __LINE__, forkmesh::LogSeverity::Error);
    }
    QString actionDetail = marker + QStringLiteral(" | ") + origin;
    if (!blockingCall.isEmpty())
        actionDetail += QStringLiteral(" | ") + blockingCall;
    forkmesh::ActionTelemetry::instant(
        QStringLiteral("stall"), actionDetail,
        QStringLiteral("event-loop-blocked"));
    QString entry = QStringLiteral("[%1] %2").arg(when, head);
    if (!report.isEmpty())
        entry += QLatin1Char('\n') + report;
    m_stallLog.append(entry);
    while (m_stallLog.size() > 100)
        m_stallLog.removeFirst();
    updateFooterDiagnostics();
    maybeTriggerStallMonitoringTeammates();
}

void MainWindow::maybeTriggerStallMonitoringTeammates()
{
#ifdef FORKMESH_WINDOW_TESTS
    return;
#endif
    // A headless mirror has no interactive GUI to repair, and its expected
    // low-memory Git/encryption work can delay the offscreen event loop. Never
    // turn those service-side diagnostics into coding-agent processes: doing
    // so competes with the mirror seal for RAM/disk and can create a feedback
    // loop where each stall launches more work and makes the next stall worse.
    if (forkmesh::isBackgroundInstance() ||
        QGuiApplication::platformName().contains(QStringLiteral("offscreen"),
                                                 Qt::CaseInsensitive))
        return;
    if (!QSettings().value(kAutoAgentOnStallSetting, true).toBool())
        return;

    const int repoIndex = stallReportRepoIndex();
    if (repoIndex < 0)
        return; // no local checkout to run an agent in

    const forkmesh::teammates::LoadResult loaded =
        forkmesh::teammates::Store(teammatesDirPath(repoIndex)).load();

    if (const int openSessionId = openStallRepairSessionId(repoIndex);
        openSessionId > 0) {
        const AgentSession *open = findAgentSession(openSessionId);
        const bool working = open && stallSessionIsActive(open);
        const int waiting = int(stallEntriesNotHandedOff().size());
        bool due = working && waiting > 0;
        for (const forkmesh::teammates::Config &teammate : loaded.teammates) {
            if (due || !teammate.enabled)
                break;
            for (const forkmesh::teammates::LogMonitor &monitor :
                 teammate.logMonitors) {
                if (forkmesh::teammates::normalizedTaskType(monitor.taskType) ==
                        forkmesh::teammates::kTaskTypeUiStalls &&
                    forkmesh::teammates::logMonitorMatches(
                        monitor, QStringLiteral("STALL"), waiting)) {
                    due = true;
                    break;
                }
            }
        }
        if (due)
            addStallsToOpenRepairSession(/*immediate=*/false);
        return;
    }

    QString assignment;
    for (const forkmesh::teammates::Config &teammate : loaded.teammates) {
        if (!teammate.enabled)
            continue;
        for (const forkmesh::teammates::LogMonitor &monitor :
             teammate.logMonitors) {
            if (!forkmesh::teammates::logMonitorMatches(
                    monitor, QStringLiteral("STALL"), m_stallCount)) {
                continue;
            }
            const QString monitorKey =
                QStringLiteral("%1:%2:%3:%4")
                    .arg(repoIndex)
                    .arg(teammate.id, monitor.badge, monitor.taskType);
            if (m_triggeredTeammateLogMonitors.contains(monitorKey))
                continue;

            const QString trigger =
                QStringLiteral("Log monitor · %1 count %2 exceeded threshold %3")
                    .arg(monitor.badge)
                    .arg(m_stallCount)
                    .arg(monitor.threshold);
            // Reserve before launch: agent startup can spin a nested event loop,
            // and a queued watchdog callback must not pass this guard again.
            m_triggeredTeammateLogMonitors.insert(monitorKey);
            if (assignment.isEmpty())
                assignment = stallFixPrompt();
            const int handedOffThrough = m_stallCount;
            const int sessionId = runTeammate(
                teammate.id, trigger, monitor.taskType, repoIndex, assignment,
                /*switchToTab=*/false);
            if (sessionId > 0) {
                rememberStallRepairSession(sessionId);
                m_stallHandedOffThrough =
                    qMax(m_stallHandedOffThrough, handedOffThrough);
                m_stallFollowUpLastMs = QDateTime::currentMSecsSinceEpoch();
                const forkmesh::teammates::AgentChoice taskAgent =
                    forkmesh::teammates::recentAgentChoice();
                logSystem(QStringLiteral(
                              "%1 observed %2 UI stalls (threshold %3) and "
                              "handed them to %4 session #%5; later stalls "
                              "are added to that session.")
                              .arg(teammate.name)
                              .arg(m_stallCount)
                              .arg(monitor.threshold)
                              .arg(agentProviderName(taskAgent.provider))
                              .arg(sessionId));
                return;
            }
            // The teammate may already be busy or the provider may be
            // unavailable. Retry on the next observed stall.
            m_triggeredTeammateLogMonitors.remove(monitorKey);
        }
    }
}

int MainWindow::openStallRepairSessionId(int repoIndex)
{
    if (repoIndex < 0 || repoIndex >= m_repositories.size())
        return 0;
    const RepositoryRecord &repo = m_repositories.at(repoIndex);
    QList<forkmesh::stallrepair::Candidate> candidates;
    candidates.reserve(m_agentSessions.size());
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        forkmesh::stallrepair::Candidate candidate;
        candidate.id = session.id;
        candidate.active = stallSessionIsActive(&session);
        candidate.succeeded = session.status == AgentStatus::Success;
        candidate.merged = session.merged;
        candidate.foreign =
            isCloudSession(session.id) || isExternalSession(session.id);
        candidate.associationOnly = session.associationOnly;
        candidate.sameRepository =
            session.owner == repo.owner && session.name == repo.name;
        candidate.title = session.issueTitle;
        candidate.prompt = session.prompt;
        candidate.createdAtMs = session.createdAtMs;
        candidates.append(candidate);
    }
    return forkmesh::stallrepair::pickSession(
        candidates, QSettings().value(kStallRepairSessionSetting, 0).toInt());
}

void MainWindow::rememberStallRepairSession(int sessionId)
{
    if (sessionId > 0)
        QSettings().setValue(kStallRepairSessionSetting, sessionId);
}

QStringList MainWindow::stallEntriesNotHandedOff() const
{
    const int firstSerial = m_stallCount - int(m_stallLog.size()) + 1;
    QStringList entries;
    for (int i = 0; i < m_stallLog.size(); ++i) {
        if (firstSerial + i > m_stallHandedOffThrough)
            entries.append(m_stallLog.at(i));
    }
    return entries;
}

bool MainWindow::addStallsToOpenRepairSession(bool immediate)
{
    if (openStallRepairSessionId(stallReportRepoIndex()) <= 0)
        return false;
    if (immediate) {
        if (m_stallFollowUpTimer)
            m_stallFollowUpTimer->stop();
        deliverStallFollowUp();
        return true;
    }
    if (!m_stallFollowUpTimer) {
        m_stallFollowUpTimer = new QTimer(this);
        m_stallFollowUpTimer->setSingleShot(true);
        connect(m_stallFollowUpTimer, &QTimer::timeout, this, [this] {
            if (forkmesh::isBackgroundInstance() ||
                !QSettings().value(kAutoAgentOnStallSetting, true).toBool())
                return;
            deliverStallFollowUp();
        });
    }
    if (!m_stallFollowUpTimer->isActive()) {
        const qint64 delayMs = forkmesh::stallrepair::followUpDelayMs(
            QDateTime::currentMSecsSinceEpoch(), m_stallFollowUpLastMs);
        m_stallFollowUpTimer->start(int(qBound<qint64>(1000, delayMs,
                                                       24LL * 60 * 60 * 1000)));
    }
    return true;
}

void MainWindow::deliverStallFollowUp()
{
    const QStringList entries = stallEntriesNotHandedOff();
    if (entries.isEmpty())
        return;
    const int sessionId = openStallRepairSessionId(stallReportRepoIndex());
    if (sessionId <= 0)
        return;
    const QString prompt = forkmesh::stallrepair::followUpPrompt(
        entries, m_stallCount, stallLogLocationsBlock());
    m_stallHandedOffThrough = m_stallCount;
    m_stallFollowUpLastMs = QDateTime::currentMSecsSinceEpoch();
    rememberStallRepairSession(sessionId);
    sendPromptToAgentSession(sessionId, prompt);
    logSystem(QStringLiteral("Added %1 new UI stall report(s) to agent session "
                             "#%2 instead of starting another agent.")
                  .arg(entries.size())
                  .arg(sessionId));
}

// The repository this build came from. Crash and error-log repairs keep their
// checkpoint under it, so it must not move with the Issues tab: a repair
// dispatched against one repository and completed while the tab showed
// another was never credited, and the crash was dispatched again.
int MainWindow::stallReportRepoIndex() const
{
    QStringList checkouts;
    checkouts.reserve(m_repositories.size());
    for (const RepositoryRecord &repo : m_repositories)
        checkouts.append(repo.localPath);
    const int own = forkmesh::paths::checkoutContaining(
        checkouts, forkmesh::sourceDir());
    return own >= 0 ? own : issuesRepoIndex();
}

void MainWindow::clearStallLog()
{
    m_stallCount = 0;
    m_stallLog.clear();
    m_triggeredTeammateLogMonitors.clear();
    m_stallHandedOffThrough = 0;
    if (m_stallFollowUpTimer)
        m_stallFollowUpTimer->stop();
    m_lastStallReportSignature.clear();
    m_lastStallReportMs = 0;
    if (!m_stallLogPath.isEmpty())
        QFile::remove(m_stallLogPath);
    if (m_logActivityLights)
        m_logActivityLights->resetCategory(QStringLiteral("STALL"));
    if (m_logActivityHeader)
        m_logActivityHeader->resetCategory(QStringLiteral("STALL"));
    updateFooterDiagnostics();
}

bool MainWindow::sendStallLogToAgent()
{
    if (m_stallLog.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("UI stall diagnostics"),
                                 QStringLiteral("There are no recorded UI stalls to send."));
        return false;
    }
    const int repoIndex = stallReportRepoIndex();
    if (repoIndex < 0) {
        QMessageBox::warning(
            this, QStringLiteral("UI stall diagnostics"),
            QStringLiteral("No local checkout is available to run an agent in. Clone "
                           "ForkMesh (or add the repo on the Issues tab) and try again."));
        return false;
    }
    if (const int openSessionId = openStallRepairSessionId(repoIndex);
        openSessionId > 0) {
        if (stallEntriesNotHandedOff().isEmpty()) {
            flashMessage(QStringLiteral("Agent session #%1 already has every "
                                        "recorded UI stall.")
                             .arg(openSessionId));
            return true;
        }
        addStallsToOpenRepairSession(/*immediate=*/true);
        return true;
    }
    const QString prompt =
        QStringLiteral(
            "ForkMesh's GUI thread stalled %1 time(s) this session — the event loop "
            "was blocked, which makes the window freeze. For each report below, find "
            "the blocking call in the backtrace and fix it so the UI stays responsive "
            "(move the slow work off the main thread, or skip it when nothing "
            "changed).\n\n%2\nRecorded stalls:\n\n%3")
            .arg(m_stallLog.size())
            .arg(stallLogLocationsBlock())
            .arg(m_stallLog.join(QStringLiteral("\n\n---\n\n")));
    const int handedOffThrough = m_stallCount;
    const int sessionId = startAdHocAgentForRepo(
        repoIndex, prompt, defaultAgentProvider(), /*createPr=*/false);
    if (sessionId <= 0) {
        QMessageBox::warning(this, QStringLiteral("UI stall diagnostics"),
                             QStringLiteral("Could not start an agent for the recorded stalls."));
        return false;
    }
    rememberStallRepairSession(sessionId);
    m_stallHandedOffThrough = qMax(m_stallHandedOffThrough, handedOffThrough);
    m_stallFollowUpLastMs = QDateTime::currentMSecsSinceEpoch();
    logSystem(QStringLiteral("Started an agent to fix the %1 recorded UI stall(s).")
                  .arg(m_stallLog.size()));
    return true;
}

static constexpr int kStallPromptMaxChars = 15500;

QString MainWindow::stallLogLocationsBlock() const
{
    QString block;
    const QString stallLog =
        m_stallLogPath.isEmpty() ? stallLogPath() : m_stallLogPath;
    if (!stallLog.isEmpty())
        block += QStringLiteral("Stall log: %1\n")
                     .arg(QDir::toNativeSeparators(stallLog));
    block += QStringLiteral("App log: %1\n")
                 .arg(QDir::toNativeSeparators(networkLogPath()));
    block += QStringLiteral("Action timeline: %1\n")
                 .arg(QDir::toNativeSeparators(
                     forkmesh::paths::diagnosticsDir() +
                     QStringLiteral("/actions.jsonl")));
    block += QStringLiteral(
        "\nEvery new stall has a STALL-... marker. Search that exact marker in "
        "all three logs to jump from the one-line app entry to its BEGIN/END "
        "backtrace record and action-timeline event. Also read the surrounding "
        "records for any other work that never got backgrounded — "
        "slow git/network/disk operations still running on the GUI thread — and "
        "move those off the main thread too, not just the sampled frames below.\n");
    return block;
}

QString MainWindow::stallFixPrompt() const
{
    QString head =
        QStringLiteral(
            "Please fix these UI stalls. ForkMesh's GUI thread was blocked %1 "
            "time(s) this session, which freezes the window. For each report "
            "below, find the blocking call in the backtrace and fix it so the UI "
            "stays responsive (move the slow work off the main thread, or skip it "
            "when nothing changed).\n\n")
            .arg(m_stallCount);
    head += stallLogLocationsBlock();
    head += QStringLiteral("\nRecorded stalls (newest first):\n\n");

    QString body;
    const QString separator = QStringLiteral("\n\n---\n\n");
    for (int i = m_stallLog.size() - 1; i >= 0; --i) {
        const QString entry = m_stallLog.at(i);
        if (!body.isEmpty() &&
            head.size() + body.size() + separator.size() + entry.size() >
                kStallPromptMaxChars) {
            body += QStringLiteral(
                "\n\n(older reports omitted - the full history is in the stall log above)");
            break;
        }
        if (!body.isEmpty())
            body += separator;
        body += entry;
    }
    if (body.isEmpty())
        body = QStringLiteral("(nothing recorded yet)");
    return (head + body).left(kStallPromptMaxChars);
}

#ifdef FORKMESH_WINDOW_TESTS
QString MainWindow::testQuickAddText() const
{
    return m_issueQuickAdd ? m_issueQuickAdd->toPlainText() : QString();
}

QString MainWindow::testQtDiagnosticsReport() const
{
    return m_qtDiagnostics ? m_qtDiagnostics->report() : QString();
}

bool MainWindow::testQtDiagnosticsDetailed() const
{
    return m_qtDiagnostics && m_qtDiagnostics->detailedEnabled();
}

void MainWindow::testSampleQtDiagnostics()
{
    if (m_qtDiagnostics)
        m_qtDiagnostics->sampleNow();
}

bool MainWindow::testDraftStallPromptInComposer()
{
    if (!m_issueQuickAdd)
        return false;
    sendStallReportToComposer();
    return true;
}
#endif

void MainWindow::sendStallReportToComposer()
{
    if (!m_issueQuickAdd || m_stallLog.isEmpty()) {
        showDiagnosticsDialog();
        return;
    }
    recordQuickAddHistory(m_issueQuickAdd->toPlainText());
    m_issueQuickAdd->setPlainText(stallFixPrompt());
    m_issueQuickAdd->moveCursor(QTextCursor::End);
    m_issueQuickAdd->setFocus();
    logSystem(QStringLiteral("Drafted a fix-it prompt for the %1 recorded UI stall(s); "
                             "details in %2")
                  .arg(m_stallLog.size())
                  .arg(m_stallLogPath.isEmpty()
                           ? QStringLiteral("this session's diagnostics")
                           : QDir::toNativeSeparators(m_stallLogPath)));
}

QString MainWindow::diagnosticsReport(bool advanceBaseline)
{
    namespace dr = forkmesh::diagreport;
    dr::Snapshot s;
    s.takenAtMs = QDateTime::currentMSecsSinceEpoch();
    s.version = QStringLiteral(FORKMESH_VERSION);
    s.sourceDir = forkmesh::sourceDir();
    s.platform = QStringLiteral("%1 (%2, kernel %3), %4")
                     .arg(QSysInfo::prettyProductName(),
                          QSysInfo::currentCpuArchitecture(),
                          QSysInfo::kernelVersion(),
                          QGuiApplication::platformName());
    s.qtVersion = QString::fromLatin1(qVersion());
    s.uptimeMs = m_debugSessionUptime.isValid() ? m_debugSessionUptime.elapsed() : -1;
    s.offline = m_offlineMode;
    s.cpuPercent = m_debugLastCpuPercent;
    s.residentBytes = SystemStats::residentBytes();
    s.hostMemoryTotal = SystemStats::totalMemoryBytes();
    s.hostMemoryAvailable = SystemStats::availableMemoryBytes();
    s.swapTotal = SystemStats::totalSwapBytes();
    s.swapFree = SystemStats::freeSwapBytes();
    s.diskTotal = SystemStats::diskTotalBytes(QDir::homePath());
    s.diskFree = SystemStats::diskFreeBytes(QDir::homePath());
    s.openFiles = SystemStats::openFileCount();
    s.openFileLimit = SystemStats::openFileSoftLimit();
    s.threads = SystemStats::threadCount();
    s.networkRequests = m_debugNetworkRequestTotal;
    s.logLinesThisSession = m_debugAllLogCount;
    s.runningAgents = runningAgentCount();
    if (m_qtDiagnostics) {
        const QtRuntimeDiagnostics::Snapshot qt = m_qtDiagnostics->snapshot();
        s.loopLagP95Ms = qt.p95LoopLagMs;
        s.loopLagPeakMs = qt.peakLoopLagMs;
        s.widgets = qt.widgetCount;
        s.timers = qt.timerCount;
        s.qtReport = m_qtDiagnostics->report();
    }
    s.log.reserve(m_networkLog.size());
    for (const QString &line : std::as_const(m_networkLog)) {
        const LogLineStyle &style = logStyleFor(line);
        ++s.categoryCounts[style.badge];
        s.log.append({style.badge, style.message, line});
    }
    s.stallCount = m_stallCount;
    s.stalls = m_stallLog;
    const QList<forkmesh::crashlog::Record> crashes =
        forkmesh::crashlog::parseCrashRecords(m_networkLog);
    for (int i = qMax(0, int(crashes.size()) - 3); i < crashes.size(); ++i)
        s.crashes.append(forkmesh::crashlog::describe(crashes.at(i)));
    s.securityIncidents = m_securityIncidentLines;
    s.healthFindings =
        NodeDiagnostics::detailText(NodeDiagnostics::hostCollector().findings());
    s.logLocations = stallLogLocationsBlock();

    QSettings settings;
    const dr::Baseline previous = dr::baselineFromJson(
        QJsonDocument::fromJson(
            settings.value(kDiagnosticsReportBaselineSetting).toByteArray())
            .object());
    const QString report = dr::build(s, previous);
    if (advanceBaseline)
        settings.setValue(
            kDiagnosticsReportBaselineSetting,
            QJsonDocument(dr::toJson(dr::baselineFrom(s, previous.sequence + 1)))
                .toJson(QJsonDocument::Compact));
    return report;
}

static QString diagnosticsReportPath()
{
#ifdef FORKMESH_WINDOW_TESTS
    return QString();
#else
    return forkmesh::paths::diagnosticsDir() + QStringLiteral("/self-fix-report.md");
#endif
}

void MainWindow::copyDiagnosticsReport()
{
    const QString report = diagnosticsReport(true);
    QApplication::clipboard()->setText(report);
    const QString path = diagnosticsReportPath();
    if (!path.isEmpty())
        forkmesh::diskwrite::writeLater(path, report.toUtf8());
    const QString title = report.section(QLatin1Char('\n'), 0, 0).mid(2);
    flashMessage(QStringLiteral("Copied %1 (%2 KB). Paste it into an agent to fix "
                                "errors and speed things up.")
                     .arg(title)
                     .arg(qMax<qsizetype>(1, report.size() / 1024)));
}

void MainWindow::sendDiagnosticsReportToAgent()
{
    const int repoIndex = stallReportRepoIndex();
    if (repoIndex < 0) {
        flashMessage(
            QStringLiteral("No local checkout is available to run an agent in. Clone "
                           "ForkMesh (or add the repo on the Issues tab) and try again."),
            true, QString(), forkmesh::kLocalOperationNoticeKind);
        return;
    }
    const QString report = diagnosticsReport(true);
    const QString path = diagnosticsReportPath();
    QString error;
    if (path.isEmpty() || !forkmesh::diskwrite::writeNow(path, report.toUtf8(), &error)) {
        flashMessage(QStringLiteral("Could not save the diagnostic report: %1")
                         .arg(error.isEmpty() ? QStringLiteral("no path") : error),
                     true, QString(), forkmesh::kLocalOperationNoticeKind);
        return;
    }
    const QString prompt =
        QStringLiteral("Use the ForkMesh desktop diagnostic snapshot below and "
                       "follow its \"What to do with this report\" section: fix the "
                       "crashes, stalls and logged problems it lists, optimize what "
                       "its findings point at, verify the changes and commit. "
                       "Do not push. Preserve user data. The latest report is also "
                       "saved at %1; use this snapshot if that file changes.\n\n%2")
            .arg(QDir::toNativeSeparators(path), report);
    auto choice = promptAreaAgentChoice();
    if (!agentProviderIsKnown(choice.provider) || agentIsCloudflareAiProvider(choice.provider))
        choice = {defaultAgentProvider(), QString()};
    if (agentIsCloudflareAiProvider(choice.provider))
        choice = {kFallbackAgentProvider, QString()};
    flashMessage(QStringLiteral("Starting a desktop agent with the diagnostic report..."));
    const int sessionId = startAdHocAgentForRepo(
        repoIndex, prompt, choice.provider, false, choice.model,
        QStringLiteral("Diagnose and optimize ForkMesh desktop"));
    if (sessionId <= 0) {
        flashMessage(QStringLiteral("Could not start an agent for the diagnostic report. Check the Log for details."),
                     true, QString(), forkmesh::kLocalOperationNoticeKind);
        return;
    }
    const AgentSession *session = findAgentSession(sessionId);
    if (session && session->status == AgentStatus::Failed) {
        flashMessage(QStringLiteral("Diagnostic agent #%1 could not start: %2")
                         .arg(sessionId).arg(session->lastError),
                     true, QString(), forkmesh::kLocalOperationNoticeKind);
        return;
    }
    acknowledgeDiagnosticsAlert();
    flashMessage(QStringLiteral("Diagnostic report sent to desktop agent #%1. Follow its progress in Agents.")
                     .arg(sessionId));
}

void MainWindow::resetDiagnosticsReportBaseline()
{
    QSettings().remove(kDiagnosticsReportBaselineSetting);
    flashMessage(QStringLiteral("The next diagnostic report starts again at #1."));
}

void MainWindow::showDiagnosticsDialog()
{
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("UI stall diagnostics"));
    dlg.resize(720, 480);
    auto *v = new QVBoxLayout(&dlg);
    auto *summary = new QLabel(
        m_stallCount == 0
            ? QStringLiteral("No UI stalls detected this session. The app watches the "
                             "GUI thread and records any freeze longer than 1.5s here.")
            : QStringLiteral("%1 UI stall(s) detected this session. Each entry below "
                             "is where the GUI thread was blocked.")
                  .arg(m_stallCount));
    summary->setWordWrap(true);
    v->addWidget(summary);
    auto *view = new QPlainTextEdit;
    view->setReadOnly(true);
    applyLogFont(view);
    view->setPlainText(m_stallLog.isEmpty() ? QStringLiteral("(nothing recorded yet)")
                                            : m_stallLog.join(QStringLiteral("\n\n")));
    v->addWidget(view, 1);
    if (!m_stallLogPath.isEmpty()) {
        auto *path = new QLabel(QStringLiteral("Durable log: %1").arg(m_stallLogPath));
        path->setTextInteractionFlags(Qt::TextSelectableByMouse);
        path->setStyleSheet(Theme::remapSheet(QStringLiteral("color:#8b949e;font-size:11px;")));
        v->addWidget(path);
    }
    const bool haveStalls = !m_stallLog.isEmpty();
    auto *clearBtn = new QPushButton(QStringLiteral("Clear"));
    clearBtn->setToolTip(QStringLiteral("Forget every recorded stall (and its durable log)"));
    clearBtn->setEnabled(haveStalls);
    auto *draftBtn = new QPushButton(QStringLiteral("Draft in composer"));
    draftBtn->setToolTip(
        QStringLiteral("Fill the footer composer with a \"fix these stalls\" prompt "
                       "(with the log locations) so it can be reviewed before sending"));
    draftBtn->setEnabled(haveStalls);
    auto *sendBtn = new QPushButton(QStringLiteral("Send to a new agent"));
    sendBtn->setToolTip(
        QStringLiteral("Hand all recorded stalls to a coding agent to investigate and fix"));
    sendBtn->setEnabled(haveStalls);
    auto *close = new QPushButton(QStringLiteral("Close"));

    connect(clearBtn, &QPushButton::clicked, &dlg,
            [this, summary, view, clearBtn, sendBtn, draftBtn] {
                clearStallLog();
                summary->setText(QStringLiteral(
                    "No UI stalls detected this session. The app watches the "
                    "GUI thread and records any freeze longer than 1.5s here."));
                view->setPlainText(QStringLiteral("(nothing recorded yet)"));
                clearBtn->setEnabled(false);
                sendBtn->setEnabled(false);
                draftBtn->setEnabled(false);
            });
    connect(draftBtn, &QPushButton::clicked, &dlg, [this, &dlg] {
        sendStallReportToComposer();
        dlg.accept();
    });
    connect(sendBtn, &QPushButton::clicked, &dlg, [this, &dlg] {
        if (sendStallLogToAgent())
            dlg.accept();
    });
    connect(close, &QPushButton::clicked, &dlg, &QDialog::accept);

    auto *row = new QHBoxLayout;
    row->addWidget(clearBtn);
    row->addStretch(1);
    row->addWidget(draftBtn);
    row->addWidget(sendBtn);
    row->addWidget(close);
    v->addLayout(row);
    dlg.exec();
}

void MainWindow::showDesktopResourceDiagnostics()
{
    showSection(4);
    if (m_logViewTabs && m_logDesktopResourceTabIndex >= 0)
        m_logViewTabs->setCurrentIndex(m_logDesktopResourceTabIndex);
    if (m_desktopProcessView) {
        m_desktopProcessView->sampleNow();
        m_desktopProcessView->setFocus();
    }
}

void MainWindow::showDesktopProcessDetails(
    const forkmesh::desktopprocs::Process &process)
{
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Process diagnostics"));
    dialog->resize(680, 480);
    auto *layout = new QVBoxLayout(dialog);

    auto *title = new QLabel(
        QStringLiteral("<b>%1</b> <span style='color:#8b949e'>(PID %2)</span>")
            .arg(process.name.toHtmlEscaped())
            .arg(process.pid));
    title->setTextFormat(Qt::RichText);
    layout->addWidget(title);

    const QString state = forkmesh::desktopprocs::stateWord(process.state);
    QStringList details;
    details.append(QStringLiteral("State: %1").arg(state));
    details.append(QStringLiteral("Owner: %1").arg(process.user));
    details.append(QStringLiteral("Parent PID: %1").arg(process.parentPid));
    details.append(QStringLiteral("CPU: %1%")
                       .arg(process.cpuPercent < 0.0
                                ? QStringLiteral("-")
                                : QString::number(process.cpuPercent, 'f', 1)));
    details.append(QStringLiteral("Resident memory: %1")
                       .arg(forkmesh::corelogs::compactByteSize(process.rssBytes)));
    details.append(QStringLiteral("Threads: %1").arg(process.threads));
    if (process.ageSeconds >= 0)
        details.append(QStringLiteral("Age: %1 s").arg(process.ageSeconds));
    if (process.self)
        details.append(QStringLiteral("This is ForkMesh itself."));
    else if (process.forkmesh)
        details.append(QStringLiteral("Started by this ForkMesh desktop."));
    auto *summary = new QLabel(details.join(QStringLiteral("\n")));
    summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    summary->setWordWrap(true);
    layout->addWidget(summary);

    auto *commandLabel = new QLabel(QStringLiteral("What it is doing"));
    layout->addWidget(commandLabel);
    auto *command = new QPlainTextEdit(dialog);
    command->setReadOnly(true);
    command->setLineWrapMode(QPlainTextEdit::NoWrap);
    command->setPlainText(process.command.isEmpty()
                              ? QStringLiteral("Command line is unavailable.")
                              : process.command);
    layout->addWidget(command, 1);

    auto *fullDetailLabel = new QLabel(QStringLiteral("Full process details"));
    layout->addWidget(fullDetailLabel);
    auto *fullDetail = new QPlainTextEdit(dialog);
    fullDetail->setReadOnly(true);
    fullDetail->setLineWrapMode(QPlainTextEdit::NoWrap);
    fullDetail->setPlainText(forkmesh::desktopprocs::detailTextFor(process));
    fullDetail->setMaximumHeight(150);
    layout->addWidget(fullDetail);

    auto *status = new QLabel;
    status->setWordWrap(true);
    layout->addWidget(status);
    auto *copy = new QPushButton(QStringLiteral("Copy details"), dialog);
    auto *stop = new QPushButton(QStringLiteral("Request stop"), dialog);
    const bool protectedProcess =
        process.pid <= 1 || process.pid == QCoreApplication::applicationPid();
    stop->setEnabled(!protectedProcess);
    stop->setToolTip(protectedProcess
                         ? QStringLiteral("ForkMesh and PID 1 cannot be stopped here.")
                         : QStringLiteral("Request a normal termination of this process."));
    auto *close = new QPushButton(QStringLiteral("Close"), dialog);
    connect(copy, &QPushButton::clicked, dialog, [process] {
        QString text = QStringLiteral("%1 (PID %2)\n").arg(process.name).arg(process.pid);
        text += process.command;
        QApplication::clipboard()->setText(text);
    });
    connect(stop, &QPushButton::clicked, dialog, [this, dialog, process, status, stop] {
        if (QMessageBox::warning(
                dialog, QStringLiteral("Request process stop"),
                QStringLiteral("Request that “%1” (PID %2) terminate?\n\n"
                               "Unsaved work in that process may be lost.")
                    .arg(process.name)
                    .arg(process.pid),
                QMessageBox::Cancel | QMessageBox::Yes,
                QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        QString failure;
        const SystemStats::Termination outcome =
            SystemStats::requestTermination(process.pid, &failure);
        if (outcome == SystemStats::Termination::Requested) {
            status->setText(QStringLiteral("Termination requested for %1 (PID %2).")
                                .arg(process.name)
                                .arg(process.pid));
            stop->setEnabled(false);
            if (m_desktopProcessView)
                QTimer::singleShot(750, m_desktopProcessView,
                                   &forkmesh::DesktopProcessView::sampleNow);
        } else if (outcome == SystemStats::Termination::AlreadyExited) {
            status->setText(QStringLiteral("%1 (PID %2) had already exited.")
                                .arg(process.name)
                                .arg(process.pid));
            stop->setEnabled(false);
        } else if (outcome == SystemStats::Termination::PermissionDenied) {
            const QString pkexec = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
            const QString kill = QStandardPaths::findExecutable(QStringLiteral("kill"));
            QString program;
            QStringList arguments;
            QByteArray password;
            if (!pkexec.isEmpty()) {
                program = pkexec;
                arguments = {kill.isEmpty() ? QStringLiteral("/bin/kill") : kill,
                             QStringLiteral("-TERM"), QString::number(process.pid)};
            } else {
                bool accepted = false;
                QString entered = QInputDialog::getText(
                    dialog, QStringLiteral("Administrator password"),
                    QStringLiteral("%1 (PID %2) needs administrator permission to stop. "
                                   "Enter the sudo password:")
                        .arg(process.name)
                        .arg(process.pid),
                    QLineEdit::Password, QString(), &accepted);
                if (!accepted)
                    return;
                program = QStringLiteral("sudo");
                arguments = {QStringLiteral("-S"), QStringLiteral("-p"), QString(),
                             QStringLiteral("--"),
                             kill.isEmpty() ? QStringLiteral("/bin/kill") : kill,
                             QStringLiteral("-TERM"), QString::number(process.pid)};
                password = entered.toUtf8();
                password.append('\n');
                entered.fill(QChar::Null);
            }
            stop->setEnabled(false);
            status->setText(QStringLiteral("Requesting administrator permission to stop %1 …")
                                .arg(process.name));
            auto *elevated = new QProcess(dialog);
            auto finished = QSharedPointer<bool>::create(false);
            auto complete = [this, elevated, finished, process, status](bool requested) {
                if (*finished)
                    return;
                *finished = true;
                elevated->deleteLater();
                if (requested) {
                    status->setText(QStringLiteral("Termination requested for %1 (PID %2).")
                                        .arg(process.name)
                                        .arg(process.pid));
                    if (m_desktopProcessView)
                        QTimer::singleShot(750, m_desktopProcessView,
                                           &forkmesh::DesktopProcessView::sampleNow);
                } else {
                    status->setText(QStringLiteral("Administrator permission was declined or "
                                                   "could not stop %1 (PID %2).")
                                        .arg(process.name)
                                        .arg(process.pid));
                }
            };
            connect(elevated, &QProcess::finished, dialog,
                    [complete](int exitCode, QProcess::ExitStatus exitStatus) {
                        complete(exitStatus == QProcess::NormalExit && exitCode == 0);
                    });
            connect(elevated, &QProcess::errorOccurred, dialog,
                    [complete](QProcess::ProcessError error) {
                        if (error == QProcess::FailedToStart)
                            complete(false);
                    });
            elevated->start(program, arguments);
            if (!password.isEmpty()) {
                elevated->write(password);
                password.fill('\0');
            }
            elevated->closeWriteChannel();
        } else {
            status->setText(QStringLiteral("Could not stop %1 (PID %2): %3")
                                .arg(process.name)
                                .arg(process.pid)
                                .arg(failure.isEmpty()
                                         ? QStringLiteral("not supported on this platform")
                                         : failure));
        }
    });
    connect(close, &QPushButton::clicked, dialog, &QDialog::close);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(copy);
    buttons->addStretch(1);
    buttons->addWidget(stop);
    buttons->addWidget(close);
    layout->addLayout(buttons);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void MainWindow::showQtDiagnosticsDialog()
{
    if (!m_qtDiagnostics)
        return;
    if (m_qtDiagnosticsDialog) {
        m_qtDiagnosticsDialog->show();
        m_qtDiagnosticsDialog->raise();
        m_qtDiagnosticsDialog->activateWindow();
        return;
    }

    auto *dialog = new QDialog(this);
    m_qtDiagnosticsDialog = dialog;
    dialog->setObjectName(QStringLiteral("qtRuntimeDiagnosticsDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Qt runtime diagnostics"));
    dialog->resize(760, 620);

    auto *layout = new QVBoxLayout(dialog);
    auto *summary = new QLabel(QStringLiteral(
        "Live, payload-free diagnostics for Qt's event loop, event delivery, "
        "widgets, timers, worker pool, platform plugin, and screens."));
    summary->setWordWrap(true);
    layout->addWidget(summary);

    auto *report = new QPlainTextEdit(dialog);
    m_qtDiagnosticsReport = report;
    report->setObjectName(QStringLiteral("qtRuntimeDiagnosticsReport"));
    report->setReadOnly(true);
    report->setLineWrapMode(QPlainTextEdit::NoWrap);
    applyLogFont(report);
    report->setPlainText(m_qtDiagnostics->report());
    m_qtDiagnosticsReportLastRefreshMs =
        m_qtDiagnostics->snapshot().sampledAtMs;
    layout->addWidget(report, 1);

    auto *reset = new QPushButton(QStringLiteral("Reset session"), dialog);
    reset->setObjectName(QStringLiteral("qtDiagnosticsResetButton"));
    reset->setToolTip(
        QStringLiteral("Clear the in-memory Qt diagnostics history and peaks"));
    auto *copy = new QPushButton(QStringLiteral("Copy report"), dialog);
    copy->setObjectName(QStringLiteral("qtDiagnosticsCopyButton"));
    copy->setToolTip(QStringLiteral(
        "Copy this payload-free Qt runtime snapshot to the clipboard"));
    auto *close = new QPushButton(QStringLiteral("Close"), dialog);

    QObject::connect(reset, &QPushButton::clicked, dialog, [this] {
        if (!m_qtDiagnostics)
            return;
        m_qtDiagnostics->resetSession();
        m_qtDiagnosticsReportLastRefreshMs = 0;
        m_qtDiagnostics->sampleNow();
        updateQtDiagnosticTiles();
    });
    QObject::connect(copy, &QPushButton::clicked, dialog, [this] {
        if (m_qtDiagnostics)
            QApplication::clipboard()->setText(m_qtDiagnostics->report());
    });
    QObject::connect(close, &QPushButton::clicked, dialog, &QDialog::close);
    QObject::connect(dialog, &QObject::destroyed, this, [this] {
        m_qtDiagnosticsDialog = nullptr;
        m_qtDiagnosticsReport = nullptr;
        m_qtDiagnosticsReportLastRefreshMs = 0;
    });

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(reset);
    buttons->addStretch(1);
    buttons->addWidget(copy);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

static constexpr int kHighMemoryTrendColumn = 1;
static constexpr int kHighMemoryParentColumn = 3;
static constexpr int kHighMemoryAgentColumn = 7;
static constexpr int kHighMemoryCommandColumn = 8;
static constexpr int kHighMemoryActionColumn = 9;
static constexpr int kHighMemoryColumnCount = 10;
static constexpr int kHighMemoryTrendRows = 10;
static constexpr double kHighMemoryAutoKillTargetPct = 85.0;
static constexpr int kHighMemoryAutoKillWaitSeconds = 5;

void MainWindow::updateHighMemoryHeading()
{
    if (!m_highMemoryHeading)
        return;
    const QString percent =
        m_highMemoryHostPct >= 0.0
            ? QStringLiteral("%1%").arg(m_highMemoryHostPct, 0, 'f', 1)
            : QStringLiteral("—");
    m_highMemoryHeading->setText(
        QStringLiteral("<b>Host memory is above 85% · %1 %2</b>")
            .arg(percent, m_highMemoryHostTrendArrow));
}

namespace {

void setShowWithoutActivating(QWidget *window, bool withoutActivating)
{
    window->setAttribute(Qt::WA_ShowWithoutActivating, withoutActivating);
    if (QWindow *handle = window->windowHandle())
        handle->setProperty("_q_showWithoutActivating", withoutActivating);
}

} // namespace

void MainWindow::showHighMemoryProcessPanel(bool activate)
{
    if (m_highMemoryDialog) {
        setShowWithoutActivating(m_highMemoryDialog, !activate);
        m_highMemoryDialog->show();
        m_highMemoryDialog->raise();
        if (activate)
            m_highMemoryDialog->activateWindow();
        refreshHighMemoryProcessTable();
        return;
    }

    auto *dialog = new QDialog(this);
    m_highMemoryDialog = dialog;
    setShowWithoutActivating(dialog, !activate);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::NonModal);
    dialog->setModal(false);
    dialog->setWindowTitle(QStringLiteral("High memory usage"));
    QSize preferred(1680, 620);
    if (QScreen *screen = QGuiApplication::primaryScreen())
        preferred =
            preferred.boundedTo(screen->availableGeometry().size() * 0.92);
    dialog->resize(preferred);
    auto *layout = new QVBoxLayout(dialog);

    m_highMemoryHeading = new QLabel;
    m_highMemoryHeading->setTextFormat(Qt::RichText);
    updateHighMemoryHeading();
    layout->addWidget(m_highMemoryHeading);

    auto *description = new QLabel(QStringLiteral(
        "Processes are sorted by resident memory, and the top ten carry a "
        "trend square showing how their memory has moved since this panel "
        "opened. The Parent column names the process that started each row — "
        "hover it for the whole ancestry chain. Anything an agent run started "
        "— the agent itself and every build, test or tool below it — names "
        "that run in the Agent column, and “Stop agent” ends that run "
        "cleanly instead of killing one process out from under it. "
        "“Kill” requests a normal termination and “Kill all” does the "
        "same for every listed process sharing that name. ForkMesh, its "
        "helpers, whatever started it, the display server and PID 1 are "
        "protected, since ending them would close ForkMesh too."));
    description->setWordWrap(true);
    layout->addWidget(description);

    m_highMemoryProcessStatus =
        new QLabel(QStringLiteral("Scanning for the largest memory users…"));
    m_highMemoryProcessStatus->setObjectName(QStringLiteral("statusLine"));
    layout->addWidget(m_highMemoryProcessStatus);

    m_highMemoryProcessTable = new QTableWidget(0, kHighMemoryColumnCount);
    m_highMemoryProcessTable->setHorizontalHeaderLabels(
        {QStringLiteral("Process"), QStringLiteral("Trend"),
         QStringLiteral("PID"), QStringLiteral("Parent"),
         QStringLiteral("Owner"), QStringLiteral("Memory"),
         QStringLiteral("Host %"), QStringLiteral("Agent"),
         QStringLiteral("Command line"), QStringLiteral("Action")});
    m_highMemoryProcessTable->verticalHeader()->setVisible(false);
    m_highMemoryProcessTable->setSelectionBehavior(
        QAbstractItemView::SelectRows);
    m_highMemoryProcessTable->setEditTriggers(
        QAbstractItemView::NoEditTriggers);
    m_highMemoryProcessTable->setSortingEnabled(false);
    m_highMemoryProcessTable->setAlternatingRowColors(true);
    m_highMemoryProcessTable->setWordWrap(false);
    m_highMemoryProcessTable->setTextElideMode(Qt::ElideRight);
    for (int column = 0; column < kHighMemoryColumnCount; ++column)
        m_highMemoryProcessTable->horizontalHeader()->setSectionResizeMode(
            column, column == kHighMemoryCommandColumn ? QHeaderView::Stretch
                                                       : QHeaderView::Fixed);
    m_highMemoryProcessTable->setColumnWidth(0, 180);
    m_highMemoryProcessTable->setColumnWidth(kHighMemoryTrendColumn, 46);
    m_highMemoryProcessTable->setColumnWidth(2, 72);
    m_highMemoryProcessTable->setColumnWidth(kHighMemoryParentColumn, 170);
    m_highMemoryProcessTable->setColumnWidth(4, 110);
    m_highMemoryProcessTable->setColumnWidth(5, 105);
    m_highMemoryProcessTable->setColumnWidth(6, 72);
    m_highMemoryProcessTable->setColumnWidth(kHighMemoryAgentColumn, 190);
    m_highMemoryProcessTable->setColumnWidth(kHighMemoryActionColumn,
                                            250); // Kill + Kill all + Stop agent
    m_highMemoryProcessTable->verticalHeader()->setDefaultSectionSize(38);
    layout->addWidget(m_highMemoryProcessTable, 1);

    auto *autoKill = new QCheckBox(QStringLiteral(
        "Kill the largest processes, one at a time, until memory drops below "
        "85% when this window pops up"));
    autoKill->setChecked(
        QSettings().value(kHighMemoryAutoKillSetting, false).toBool());
    autoKill->setToolTip(QStringLiteral(
        "When host memory crosses 85% and this window opens on its own, "
        "ForkMesh requests a normal termination of the largest process it is "
        "allowed to end. If host memory is still at 85% or more once that "
        "process exits (or after five seconds), it ends the next largest, and "
        "so on until memory drops below 85% or no listed process is left. "
        "ForkMesh, its helpers, whatever started it, the display server and "
        "PID 1 are never chosen. Opening the window yourself "
        "never kills anything."));
    connect(autoKill, &QCheckBox::toggled, this, [this](bool enabled) {
        QSettings().setValue(kHighMemoryAutoKillSetting, enabled);
        if (!enabled)
            endHighMemoryAutoKill();
    });

    auto *refresh = new QPushButton(QStringLiteral("Refresh"));
    setOcticon(refresh, QStringLiteral("sync"), 14);
    connect(refresh, &QPushButton::clicked, this,
            &MainWindow::refreshHighMemoryProcessTable);
    auto *close = new QPushButton(QStringLiteral("Close"));
    connect(close, &QPushButton::clicked, dialog, &QDialog::close);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(refresh);
    buttons->addWidget(autoKill);
    buttons->addStretch(1);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(dialog, &QObject::destroyed, this, [this] {
        endHighMemoryAutoKill();
        m_highMemoryDialog = nullptr;
        m_highMemoryHeading = nullptr;
        m_highMemoryProcessTable = nullptr;
        m_highMemoryProcessStatus = nullptr;
        m_highMemoryProcessQuery = nullptr;
        m_highMemoryRssHistory.clear();
        m_highMemoryProtectedPids.clear();
    });
    auto *autoRefresh = new QTimer(dialog);
    autoRefresh->setInterval(5000);
    connect(autoRefresh, &QTimer::timeout, this,
            &MainWindow::refreshHighMemoryProcessTable);
    autoRefresh->start();
    dialog->show();
    dialog->raise();
    QTimer::singleShot(0, this, &MainWindow::refreshHighMemoryProcessTable);
}

#ifdef FORKMESH_WINDOW_TESTS
QString MainWindow::testHighMemoryPanelActivation(bool activate)
{
    showHighMemoryProcessPanel(activate);
    QCoreApplication::processEvents();
    if (!m_highMemoryDialog)
        return QString();
    const QWindow *window = m_highMemoryDialog->windowHandle();
    return QStringLiteral("%1|%2")
        .arg(m_highMemoryDialog->testAttribute(Qt::WA_ShowWithoutActivating))
        .arg(window &&
             window->property("_q_showWithoutActivating").toBool());
}
#endif

void MainWindow::refreshHighMemoryProcessTable()
{
    if (!m_highMemoryDialog || !m_highMemoryProcessTable ||
        m_highMemoryProcessQuery)
        return;
    m_highMemoryProcessStatus->setText(
        m_highMemoryProcessTable->rowCount() == 0
            ? QStringLiteral("Scanning for the largest memory users…")
            : QStringLiteral("Refreshing memory culprits in the background…"));
    auto *query = new QProcess(this);
    m_highMemoryProcessQuery = query;
    connect(m_highMemoryDialog.data(), &QObject::destroyed, query, [query] {
        query->disconnect();
        if (query->state() != QProcess::NotRunning)
            query->kill();
        query->deleteLater();
    });
    connect(query, &QProcess::finished, this,
            [this, query](int exitCode, QProcess::ExitStatus status) {
                const QByteArray output = query->readAllStandardOutput();
                const QByteArray error = query->readAllStandardError();
                query->deleteLater();
                m_highMemoryProcessQuery = nullptr;
                if (!m_highMemoryDialog || !m_highMemoryProcessTable)
                    return;
                if (status != QProcess::NormalExit || exitCode != 0) {
                    const QString detail =
                        QString::fromLocal8Bit(error).trimmed();
                    m_highMemoryProcessStatus->setText(
                        QStringLiteral("Could not list processes: %1")
                            .arg(detail.isEmpty()
                                     ? QStringLiteral("the background query timed out")
                                     : detail));
                    return;
                }

                struct ProcessRow {
                    qint64 pid = 0;
                    QString owner;
                    qint64 rssKb = 0;
                    double percent = 0.0;
                    QString name;
                    QString commandLine;
                };
                QVector<ProcessRow> rows;
                rows.reserve(30);
                QHash<qint64, qint64> parentOf;
                QHash<qint64, QString> nameOf;
                const QList<QByteArray> lines = output.split('\n');
                int validProcesses = 0;
                for (const QByteArray &raw : lines) {
                    const QList<QByteArray> fields =
                        raw.simplified().split(' ');
                    if (fields.size() < 6)
                        continue;
                    bool pidOk = false;
                    bool rssOk = false;
                    const qint64 pid = fields.at(0).toLongLong(&pidOk);
                    const qint64 ppid = fields.at(1).toLongLong();
                    const QString owner = QString::fromLocal8Bit(fields.at(2));
                    const qint64 rssKb = fields.at(3).toLongLong(&rssOk);
                    const QString percent =
                        QString::fromLocal8Bit(fields.at(4));
                    const QString commandLine =
                        QString::fromLocal8Bit(
                            QByteArrayList(fields.mid(5)).join(' '));
                    if (!pidOk || !rssOk || pid <= 0 || commandLine.isEmpty())
                        continue;
                    if (ppid > 0)
                        parentOf.insert(pid, ppid);
                    QString name = commandLine.section(QLatin1Char(' '), 0, 0);
                    const int slash = name.lastIndexOf(QLatin1Char('/'));
                    if (slash >= 0)
                        name = name.mid(slash + 1);
                    if (name.isEmpty())
                        name = commandLine;
                    nameOf.insert(pid, name);
                    ++validProcesses;
                    if (rows.size() < 30)
                        rows.append({pid, owner, rssKb, percent.toDouble(),
                                     name, commandLine});
                }

                QSet<QString> selfExecutables;
                for (const QString &path :
                     {SystemStats::executablePath(
                          QCoreApplication::applicationPid()),
                      QCoreApplication::applicationFilePath(),
                      QFileInfo(QCoreApplication::applicationFilePath())
                          .canonicalFilePath(),
                      qEnvironmentVariable("APPIMAGE")}) {
                    if (!path.isEmpty())
                        selfExecutables.insert(path);
                }
                QHash<qint64, QString> executableOf;
                for (const ProcessRow &process : rows) {
                    QString executable = SystemStats::executablePath(process.pid);
                    if (executable.isEmpty())
                        executable = process.commandLine.section(QLatin1Char(' '), 0, 0);
                    executableOf.insert(process.pid, executable);
                }
                m_highMemoryProtectedPids = SystemStats::killProtectedPids(
                    QCoreApplication::applicationPid(), selfExecutables,
                    parentOf, nameOf, executableOf);

                QHash<qint64, QVector<double>> history;
                history.reserve(rows.size());
                double historyMaxKb = 1.0;
                for (const ProcessRow &process : rows) {
                    QVector<double> samples =
                        m_highMemoryRssHistory.value(process.pid);
                    samples.append(double(process.rssKb));
                    while (samples.size() > ProcessMemorySparkline::kMaxPoints)
                        samples.removeFirst();
                    for (double sample : samples)
                        historyMaxKb = qMax(historyMaxKb, sample);
                    history.insert(process.pid, samples);
                }
                m_highMemoryRssHistory = history;

                QHash<QString, QList<qint64>> pidsByName;
                for (const ProcessRow &process : rows)
                    pidsByName[process.name].append(process.pid);

                struct AgentOwner {
                    qint64 rootPid = 0;
                    int sessionId = 0;
                    bool stoppable = false;
                    QString label;
                    QString detail;
                };
                QHash<qint64, AgentOwner> agentRoots;
                for (const AgentSession &session : std::as_const(m_agentSessions)) {
                    const qint64 rootPid = agentSessionProcessId(session.id);
                    if (rootPid <= 0)
                        continue;
                    QString title = session.issueTitle.trimmed();
                    if (title.isEmpty())
                        title = session.prompt.section(QLatin1Char('\n'), 0, 0)
                                    .trimmed();
                    if (title.isEmpty())
                        title = QStringLiteral("Agent run #%1").arg(session.id);
                    AgentOwner owner;
                    owner.rootPid = rootPid;
                    owner.sessionId = session.id;
                    owner.stoppable = isStoppableAgentSession(session.id);
                    owner.label =
                        session.issueNumber > 0
                            ? QStringLiteral("#%1 %2").arg(session.issueNumber).arg(title)
                            : title;
                    QStringList detail{owner.label};
                    if (!session.owner.isEmpty() && !session.name.isEmpty())
                        detail << QStringLiteral("Repository: %1/%2")
                                      .arg(session.owner, session.name);
                    if (!session.branchName.isEmpty())
                        detail << QStringLiteral("Branch: %1").arg(session.branchName);
                    QString provider = agentProviderName(session.provider);
                    if (!session.model.isEmpty())
                        provider += QStringLiteral(" · %1").arg(session.model);
                    detail << QStringLiteral("Agent: %1").arg(provider)
                           << QStringLiteral("Status: %1").arg(session.status);
                    owner.detail = detail.join(QLatin1Char('\n'));
                    agentRoots.insert(rootPid, owner);
                }
                auto agentOwnerFor =
                    [&agentRoots, &parentOf](qint64 pid) -> const AgentOwner * {
                    for (int hops = 0; pid > 1 && hops < 64; ++hops) {
                        const auto found = agentRoots.constFind(pid);
                        if (found != agentRoots.constEnd())
                            return &found.value();
                        const qint64 parent = parentOf.value(pid, 0);
                        if (parent <= 0 || parent == pid)
                            break;
                        pid = parent;
                    }
                    return nullptr;
                };

                auto ancestryOf = [&parentOf, &nameOf](qint64 pid) {
                    QStringList chain;
                    qint64 current = parentOf.value(pid, 0);
                    for (int hops = 0; current > 0 && hops < 64; ++hops) {
                        chain << QStringLiteral("%1 (%2)")
                                     .arg(nameOf.value(
                                         current, QStringLiteral("?")))
                                     .arg(current);
                        const qint64 parent = parentOf.value(current, 0);
                        if (parent <= 0 || parent == current)
                            break;
                        current = parent;
                    }
                    return chain.join(QStringLiteral("\n  ↑ "));
                };

                m_highMemoryProcessTable->setUpdatesEnabled(false);
                m_highMemoryProcessTable->clearContents();
                m_highMemoryProcessTable->setRowCount(rows.size());
                qint64 shownRssKb = 0;
                int agentRows = 0;
                for (int row = 0; row < rows.size(); ++row) {
                    const ProcessRow &process = rows.at(row);
                    shownRssKb += process.rssKb;
                    auto put = [this, row](int column, const QString &text,
                                           const QVariant &sortValue = {}) {
                        auto *item = new QTableWidgetItem(text);
                        if (sortValue.isValid())
                            item->setData(Qt::UserRole, sortValue);
                        m_highMemoryProcessTable->setItem(row, column, item);
                    };
                    put(0, process.name);
                    put(2, QString::number(process.pid), process.pid);
                    const qint64 parentPid = parentOf.value(process.pid, 0);
                    const QString parentName =
                        nameOf.value(parentPid, QStringLiteral("?"));
                    put(kHighMemoryParentColumn,
                        parentPid > 0 ? QStringLiteral("%1 (%2)")
                                            .arg(parentName)
                                            .arg(parentPid)
                                      : QStringLiteral("—"),
                        parentPid);
                    if (QTableWidgetItem *parentCell =
                            m_highMemoryProcessTable->item(
                                row, kHighMemoryParentColumn))
                        parentCell->setToolTip(
                            parentPid > 0
                                ? QStringLiteral("Ancestry:\n%1")
                                      .arg(ancestryOf(process.pid))
                                : QStringLiteral(
                                      "No parent reported — the process "
                                      "exited during the scan, or it is PID 1."));
                    put(4, process.owner);
                    put(5, SystemStats::formatBytes(process.rssKb * 1024),
                        process.rssKb);
                    put(6, QStringLiteral("%1%").arg(process.percent, 0, 'f', 1),
                        process.percent);
                    const AgentOwner *agent = agentOwnerFor(process.pid);
                    if (agent) {
                        ++agentRows;
                        const bool isAgentItself = process.pid == agent->rootPid;
                        put(kHighMemoryAgentColumn,
                            isAgentItself
                                ? agent->label
                                : QStringLiteral("↳ %1").arg(agent->label));
                        if (QTableWidgetItem *cell =
                                m_highMemoryProcessTable->item(
                                    row, kHighMemoryAgentColumn))
                            cell->setToolTip(
                                isAgentItself
                                    ? QStringLiteral("%1\n\nThis is the agent's "
                                                     "own process.")
                                          .arg(agent->detail)
                                    : QStringLiteral("%1\n\nStarted by that "
                                                     "agent run (PID %2).")
                                          .arg(agent->detail)
                                          .arg(agent->rootPid));
                    }
                    put(kHighMemoryCommandColumn, process.commandLine);
                    if (QTableWidgetItem *command =
                            m_highMemoryProcessTable->item(
                                row, kHighMemoryCommandColumn))
                        command->setToolTip(process.commandLine);
                    if (row < 5) {
                        for (int column = 0; column <= kHighMemoryCommandColumn;
                             ++column) {
                            QTableWidgetItem *item =
                                m_highMemoryProcessTable->item(row, column);
                            if (!item) // the trend column holds a widget, not an item
                                continue;
                            QFont font = item->font();
                            font.setBold(true);
                            item->setFont(font);
                            item->setForeground(QColor(QStringLiteral("#cf222e")));
                        }
                    }

                    if (row < kHighMemoryTrendRows) {
                        const QVector<double> samples =
                            history.value(process.pid);
                        auto *chart = new ProcessMemorySparkline;
                        chart->setHistory(samples, historyMaxKb);
                        auto *cell = new QWidget;
                        cell->setToolTip(
                            QStringLiteral("Resident memory for %1 (PID %2) "
                                           "over the last %3 refreshes")
                                .arg(process.name)
                                .arg(process.pid)
                                .arg(samples.size()));
                        auto *cellRow = new QHBoxLayout(cell);
                        cellRow->setContentsMargins(0, 0, 0, 0);
                        cellRow->addWidget(chart, 0, Qt::AlignCenter);
                        m_highMemoryProcessTable->setCellWidget(
                            row, kHighMemoryTrendColumn, cell);
                    }

                    auto *kill = new QPushButton(QStringLiteral("Kill"));
                    kill->setProperty("buttonSize", "sm");
                    const bool protectedProcess =
                        isHighMemoryProtected(process.pid);
                    kill->setEnabled(!protectedProcess);
                    kill->setToolTip(
                        protectedProcess
                            ? QStringLiteral("This process is protected: "
                                             "ending it would close ForkMesh")
                            : QStringLiteral("Request that %1 terminate")
                                  .arg(process.name));
                    connect(kill, &QPushButton::clicked, this,
                            [this, pid = process.pid, name = process.name] {
                                killHighMemoryProcess(pid, name);
                            });

                    const QList<qint64> family = pidsByName.value(process.name);
                    QList<qint64> killable;
                    for (qint64 candidate : family) {
                        if (isHighMemoryProtected(candidate))
                            continue;
                        killable.append(candidate);
                    }
                    auto *killAll = new QPushButton(QStringLiteral("Kill all"));
                    killAll->setProperty("buttonSize", "sm");
                    killAll->setEnabled(killable.size() > 1);
                    killAll->setToolTip(
                        killable.size() > 1
                            ? QStringLiteral("Request that all %1 listed “%2” "
                                             "processes terminate")
                                  .arg(killable.size())
                                  .arg(process.name)
                            : QStringLiteral("Only one killable “%1” process is "
                                             "listed")
                                  .arg(process.name));
                    connect(killAll, &QPushButton::clicked, this,
                            [this, name = process.name, killable] {
                                killAllHighMemoryProcesses(name, killable);
                            });

                    auto *actions = new QWidget;
                    auto *actionRow = new QHBoxLayout(actions);
                    actionRow->setContentsMargins(0, 0, 0, 0);
                    actionRow->setSpacing(4);
                    actionRow->addWidget(kill);
                    actionRow->addWidget(killAll);

                    if (agent) {
                        auto *stop = new QPushButton(QStringLiteral("Stop agent"));
                        stop->setProperty("buttonSize", "sm");
                        stop->setEnabled(agent->stoppable);
                        stop->setToolTip(
                            agent->stoppable
                                ? QStringLiteral("Stop the agent run “%1” "
                                                 "and everything it started")
                                      .arg(agent->label)
                                : QStringLiteral("“%1” is no longer running")
                                      .arg(agent->label));
                        connect(stop, &QPushButton::clicked, this,
                                [this, sessionId = agent->sessionId,
                                 label = agent->label] {
                                    QTimer::singleShot(
                                        0, this, [this, sessionId, label] {
                                            stopHighMemoryAgent(sessionId, label);
                                        });
                                });
                        actionRow->addWidget(stop);
                    }
                    m_highMemoryProcessTable->setCellWidget(
                        row, kHighMemoryActionColumn, actions);
                }
                m_highMemoryProcessTable->setUpdatesEnabled(true);
                m_highMemoryProcessTable->viewport()->update();
                if (m_highMemoryAutoKillPending) {
                    m_highMemoryAutoKillPending = false;
                    if (QSettings()
                            .value(kHighMemoryAutoKillSetting, false)
                            .toBool()) {
                        QList<QPair<qint64, QString>> candidates;
                        candidates.reserve(rows.size());
                        for (const ProcessRow &process : rows) {
                            if (!isHighMemoryProtected(process.pid))
                                candidates.append({process.pid, process.name});
                        }
                        QTimer::singleShot(0, this, [this, candidates] {
                            autoKillTopHighMemoryProcess(candidates);
                        });
                    }
                }
                m_highMemoryProcessStatus->setText(
                    QStringLiteral(
                        "Top %1 of %2 processes · %3 resident%4 · refreshed %5")
                        .arg(rows.size())
                        .arg(validProcesses)
                        .arg(SystemStats::formatBytes(shownRssKb * 1024))
                        .arg(agentRows > 0
                                 ? QStringLiteral(" · %1 from agent runs")
                                       .arg(agentRows)
                                 : QString())
                        .arg(QTime::currentTime().toString(
                            QStringLiteral("h:mm:ss AP"))));
            });
    connect(query, &QProcess::errorOccurred, this,
            [this, query](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                const QString detail = query->errorString();
                query->deleteLater();
                if (m_highMemoryProcessQuery == query)
                    m_highMemoryProcessQuery = nullptr;
                if (m_highMemoryProcessStatus)
                    m_highMemoryProcessStatus->setText(
                        QStringLiteral("Could not list processes: %1")
                            .arg(detail));
            });
    query->start(QStringLiteral("ps"),
                 {QStringLiteral("-eo"),
                  QStringLiteral("pid=,ppid=,user=,rss=,%mem=,args="),
                  QStringLiteral("--sort=-rss")});
    QTimer::singleShot(3000, query, [query] {
        if (query->state() != QProcess::NotRunning)
            query->kill();
    });
}

bool MainWindow::isHighMemoryProtected(qint64 pid) const
{
    return pid <= 1 || pid == QCoreApplication::applicationPid() ||
           m_highMemoryProtectedPids.contains(pid);
}

void MainWindow::killHighMemoryProcess(qint64 pid, const QString &name)
{
    if (isHighMemoryProtected(pid))
        return;
    const auto answer = QMessageBox::warning(
        m_highMemoryDialog ? static_cast<QWidget *>(m_highMemoryDialog.data())
                           : this,
        QStringLiteral("Kill process"),
        QStringLiteral("Request that “%1” (PID %2) terminate?\n\n"
                       "Unsaved work in that process may be lost.")
            .arg(name)
            .arg(pid),
        QMessageBox::Cancel | QMessageBox::Yes, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes)
        return;

#if defined(Q_OS_UNIX)
    QString failure;
    const SystemStats::Termination outcome =
        SystemStats::requestTermination(pid, &failure);
    if (outcome == SystemStats::Termination::Failed) {
        QMessageBox::warning(
            m_highMemoryDialog
                ? static_cast<QWidget *>(m_highMemoryDialog.data())
                : this,
            QStringLiteral("Could not kill process"),
            QStringLiteral("%1 (PID %2): %3")
                .arg(name)
                .arg(pid)
                .arg(failure));
        return;
    }
    if (outcome == SystemStats::Termination::PermissionDenied) {
        QMessageBox::warning(
            m_highMemoryDialog ? static_cast<QWidget *>(m_highMemoryDialog.data()) : this,
            QStringLiteral("Administrator permission required"),
            QStringLiteral("Open the process details from Logs → Desktop to authenticate and "
                           "stop %1 (PID %2).")
                .arg(name)
                .arg(pid));
        return;
    }
    if (m_highMemoryProcessStatus)
        m_highMemoryProcessStatus->setText(
            (outcome == SystemStats::Termination::AlreadyExited
                 ? QStringLiteral("%1 (PID %2) had already exited.")
                 : QStringLiteral("Termination requested for %1 (PID %2)."))
                .arg(name)
                .arg(pid));
    QTimer::singleShot(750, this,
                       &MainWindow::refreshHighMemoryProcessTable);
#else
    QMessageBox::information(
        m_highMemoryDialog ? static_cast<QWidget *>(m_highMemoryDialog.data())
                           : this,
        QStringLiteral("Kill process"),
        QStringLiteral("Per-process termination is not supported on this "
                       "platform yet."));
#endif
}

void MainWindow::autoKillTopHighMemoryProcess(
    const QList<QPair<qint64, QString>> &candidates)
{
#if defined(Q_OS_UNIX)
    QStringList skipped;
    for (const auto &[pid, name] : candidates) {
        if (isHighMemoryProtected(pid) ||
            m_highMemoryAutoKilledPids.contains(pid))
            continue;
        if (::kill(static_cast<pid_t>(pid), SIGTERM) == 0) {
            m_highMemoryAutoKilledPids.insert(pid);
            m_highMemoryAutoKilledNames.append(
                QStringLiteral("%1 (PID %2)").arg(name).arg(pid));
            if (m_highMemoryProcessStatus)
                m_highMemoryProcessStatus->setText(
                    QStringLiteral("Automatically requested termination of "
                                   "%1 — waiting for host memory to drop below "
                                   "%2%.%3")
                        .arg(m_highMemoryAutoKilledNames.join(
                            QStringLiteral(", ")))
                        .arg(kHighMemoryAutoKillTargetPct, 0, 'f', 0)
                        .arg(skipped.isEmpty()
                                 ? QString()
                                 : QStringLiteral(" Could not end %1.")
                                       .arg(skipped.join(QStringLiteral(", ")))));
            QTimer::singleShot(750, this,
                               &MainWindow::refreshHighMemoryProcessTable);
            QTimer::singleShot(1000, this, [this, pid] {
                continueHighMemoryAutoKill(pid, 1);
            });
            return;
        }
        if (errno != ESRCH)
            skipped.append(QStringLiteral("%1 (PID %2)").arg(name).arg(pid));
    }
    if (m_highMemoryProcessStatus)
        m_highMemoryProcessStatus->setText(
            m_highMemoryAutoKilledNames.isEmpty()
                ? QStringLiteral("Auto-kill found no listed process ForkMesh "
                                 "is allowed to end.")
                : QStringLiteral("Auto-kill ended %1, but host memory is still "
                                 "high and no further listed process can be "
                                 "ended.")
                      .arg(m_highMemoryAutoKilledNames.join(
                          QStringLiteral(", "))));
    endHighMemoryAutoKill();
#else
    Q_UNUSED(candidates);
    if (m_highMemoryProcessStatus)
        m_highMemoryProcessStatus->setText(QStringLiteral(
            "Auto-kill is not supported on this platform yet."));
#endif
}

void MainWindow::continueHighMemoryAutoKill(qint64 pid, int waitedSeconds)
{
#if defined(Q_OS_UNIX)
    if (!m_highMemoryDialog || !m_highMemoryAutoKilledPids.contains(pid) ||
        !QSettings().value(kHighMemoryAutoKillSetting, false).toBool())
        return;
    const qint64 total = SystemStats::totalMemoryBytes();
    const qint64 avail = SystemStats::availableMemoryBytes();
    if (total <= 0 || avail < 0 || avail > total) {
        endHighMemoryAutoKill();
        return;
    }
    const double pct = 100.0 * double(total - avail) / double(total);
    if (pct < kHighMemoryAutoKillTargetPct) {
        if (m_highMemoryProcessStatus)
            m_highMemoryProcessStatus->setText(
                QStringLiteral("Host memory is down to %1% after auto-kill "
                               "ended %2.")
                    .arg(pct, 0, 'f', 1)
                    .arg(m_highMemoryAutoKilledNames.join(
                        QStringLiteral(", "))));
        endHighMemoryAutoKill();
        return;
    }
    const bool exited =
        ::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH;
    if (!exited && waitedSeconds < kHighMemoryAutoKillWaitSeconds) {
        QTimer::singleShot(1000, this, [this, pid, waitedSeconds] {
            continueHighMemoryAutoKill(pid, waitedSeconds + 1);
        });
        return;
    }
    m_highMemoryAutoKillPending = true;
    refreshHighMemoryProcessTable();
#else
    Q_UNUSED(pid);
    Q_UNUSED(waitedSeconds);
#endif
}

void MainWindow::endHighMemoryAutoKill()
{
    m_highMemoryAutoKillPending = false;
    m_highMemoryAutoKilledPids.clear();
    m_highMemoryAutoKilledNames.clear();
}

void MainWindow::stopHighMemoryAgent(int sessionId, const QString &label)
{
    if (sessionId <= 0)
        return;
    if (!isStoppableAgentSession(sessionId)) {
        if (m_highMemoryProcessStatus)
            m_highMemoryProcessStatus->setText(
                QStringLiteral("“%1” is no longer running.").arg(label));
        return;
    }
    auto *parent = m_highMemoryDialog
                       ? static_cast<QWidget *>(m_highMemoryDialog.data())
                       : static_cast<QWidget *>(this);
    if (!isExternalSession(sessionId) &&
        QMessageBox::warning(
            parent, QStringLiteral("Stop agent"),
            QStringLiteral("Stop the agent run “%1”?\n\n"
                           "Everything it started — builds, tests and tools — "
                           "goes down with it. Its worktree and branch are "
                           "kept, so the run can be started again.")
                .arg(label),
            QMessageBox::Cancel | QMessageBox::Yes,
            QMessageBox::Cancel) != QMessageBox::Yes)
        return;

    if (!stopAgentSessionById(sessionId)) {
        if (m_highMemoryProcessStatus)
            m_highMemoryProcessStatus->setText(
                QStringLiteral("Could not stop “%1” — it is no longer "
                               "running.")
                    .arg(label));
        return;
    }
    if (m_highMemoryProcessStatus)
        m_highMemoryProcessStatus->setText(
            QStringLiteral("Stop requested for the agent run “%1”.")
                .arg(label));
    QTimer::singleShot(750, this, &MainWindow::refreshHighMemoryProcessTable);
}

void MainWindow::killAllHighMemoryProcesses(const QString &name,
                                            const QList<qint64> &pids)
{
    QList<qint64> targets;
    for (qint64 pid : pids) {
        if (isHighMemoryProtected(pid))
            continue;
        targets.append(pid);
    }
    if (targets.isEmpty())
        return;
    auto *parent = m_highMemoryDialog
                       ? static_cast<QWidget *>(m_highMemoryDialog.data())
                       : this;

#if defined(Q_OS_UNIX)
    int sent = 0;
    QStringList failures;
    for (qint64 pid : targets) {
        QString failure;
        switch (SystemStats::requestTermination(pid, &failure)) {
        case SystemStats::Termination::Requested:
            ++sent;
            break;
        case SystemStats::Termination::Failed:
            failures.append(QStringLiteral("PID %1: %2").arg(pid).arg(failure));
            break;
        case SystemStats::Termination::PermissionDenied:
            failures.append(QStringLiteral("PID %1: administrator permission required").arg(pid));
            break;
        case SystemStats::Termination::AlreadyExited:
        case SystemStats::Termination::Unsupported:
            break;
        }
    }
    if (!failures.isEmpty())
        QMessageBox::warning(parent,
                             QStringLiteral("Could not kill every process"),
                             QStringLiteral("%1\n%2")
                                 .arg(name, failures.join(QLatin1Char('\n'))));
    if (m_highMemoryProcessStatus)
        m_highMemoryProcessStatus->setText(
            QStringLiteral("Termination requested for %1 “%2” process%3.")
                .arg(sent)
                .arg(name)
                .arg(sent == 1 ? QString() : QStringLiteral("es")));
    QTimer::singleShot(750, this, &MainWindow::refreshHighMemoryProcessTable);
#else
    QMessageBox::information(
        parent, QStringLiteral("Kill all processes"),
        QStringLiteral("Per-process termination is not supported on this "
                       "platform yet."));
#endif
}

void MainWindow::showTreasuryDonateDialog()
{
    // Read only the Worker's public pool state. The Worker never creates,
    // receives, decrypts, or signs with the pool key; that key is imported into
    // the first-instance owner's encrypted local Qt vault.
    const QUrl endpoint =
        rewardPoolWorkerEndpoint(QStringLiteral("/api/rewards/pool"));
    if (endpoint.isEmpty() || !m_networkAccess) {
        QMessageBox::information(
            this, QStringLiteral("Community reward pool"),
            QStringLiteral("A secure HTTPS reward-pool endpoint is not "
                           "available for the active ForkMesh server."));
        return;
    }

    QNetworkRequest request(endpoint);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Cache-Control", "no-store");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::SameOriginRedirectPolicy);
    request.setTransferTimeout(15000);
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        constexpr qsizetype kMaximumPoolResponse = 1024 * 1024;
        const int httpStatus =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QNetworkReply::NetworkError networkError = reply->error();
        const QByteArray body = reply->read(kMaximumPoolResponse + 1);
        reply->deleteLater();
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
        const QJsonObject pool =
            parseError.error == QJsonParseError::NoError && document.isObject()
                ? document.object()
                : QJsonObject();
        const QString address = pool.value("address").toString().trimmed();
        const bool validAddress =
            forkmesh::rewards::decodeBase58(address, 32).size() == 32;
        const bool externalSigner =
            pool.value("custody").toString() ==
                QLatin1String("external-local-signer") &&
            pool.value("privateKeyStoredByWorker").isBool() &&
            !pool.value("privateKeyStoredByWorker").toBool(true);
        if (networkError != QNetworkReply::NoError || httpStatus != 200 ||
            body.size() > kMaximumPoolResponse || !validAddress ||
            !externalSigner) {
            QMessageBox::information(
                this, QStringLiteral("Community reward pool"),
                QStringLiteral(
                    "ForkMesh could not verify a non-custodial community-pool "
                    "address from this server. No transfer has been requested."));
            return;
        }
        showTreasuryDonateDialogForPool(pool);
    });
}

void MainWindow::showTreasuryDonateDialogForPool(const QJsonObject &resp)
{
    const QString address = resp.value("address").toString().trimmed();

    QString uri = resp.value("uri").toString().trimmed();
    if (!uri.startsWith(QStringLiteral("solana:%1").arg(address)))
        uri = QStringLiteral("solana:%1").arg(address);

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Community reward pool"));
    auto *l = new QVBoxLayout(&dialog);
    l->setContentsMargins(20, 20, 20, 20);
    l->setSpacing(12);

    auto *intro = new QLabel(
        QStringLiteral(
            "<b>Non-custodial:</b> this is a voluntary transfer from your "
            "self-custodial wallet to the transparent public community reward "
            "pool. ForkMesh never receives your private key. The pool signer "
            "key remains in the first-instance owner's encrypted local desktop "
            "vault; the Worker stores public plans only. Rewards are community "
            "incentives, not investments, and no return is guaranteed."));
    intro->setWordWrap(true);
    intro->setTextFormat(Qt::RichText);
    l->addWidget(intro);

    auto *qrLabel = new QLabel;
    qrLabel->setAlignment(Qt::AlignCenter);
    const QImage qr = QrCode::encodeToImage(uri, 6, 4);
    if (!qr.isNull())
        qrLabel->setPixmap(QPixmap::fromImage(qr));
    l->addWidget(qrLabel);

    auto *addrLabel = new QLabel(address);
    addrLabel->setObjectName("payAddr");
    addrLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    addrLabel->setWordWrap(true);
    addrLabel->setAlignment(Qt::AlignCenter);
    l->addWidget(addrLabel);

    const QString network =
        resp.value("network").toString(QStringLiteral("unknown"));
    const QString balance =
        resp.value("balanceSol").toString(QStringLiteral("0"));
    auto *poolState = new QLabel(
        QStringLiteral(
            "Network: %1 · Public on-chain pool balance: %2 SOL\n"
            "Pending rewards are ledger allocations only; funds remain in the "
            "source wallet until the local owner reviews, signs, broadcasts, "
            "and finalizes the exact transfer. Completed rewards have a "
            "finalized on-chain signature.")
            .arg(network.toHtmlEscaped(), balance.toHtmlEscaped()));
    poolState->setObjectName(QStringLiteral("modeHint"));
    poolState->setWordWrap(true);
    l->addWidget(poolState);

    auto *row = new QHBoxLayout;
    auto *copyBtn = new QPushButton("Copy address");
    copyBtn->setObjectName("primaryButton");
    copyBtn->setCursor(Qt::PointingHandCursor);
    connect(copyBtn, &QPushButton::clicked, this, [address, copyBtn] {
        QApplication::clipboard()->setText(address);
        copyBtn->setText("Copied!");
    });
    auto *closeBtn = new QPushButton("Close");
    closeBtn->setCursor(Qt::PointingHandCursor);
    connect(closeBtn, &QPushButton::clicked, &dialog, &QDialog::accept);
    row->addStretch(1);
    row->addWidget(copyBtn);
    row->addWidget(closeBtn);
    l->addLayout(row);

    dialog.exec();
}
