
#include "Tooltips.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "MainWindowNetworkInternal.h"
#include "SystemPressure.h"
#include "AgentCliLocator.h"
#include "AgentLoginPty.h"
#include "AgentWorktree.h"
#include "ActionStore.h"
#include "ControlNode.h"
#include "CurrentPageStack.h"
#include "MirrorSyncVerdict.h"
#include "D1QuerySource.h"
#include "DatabaseManagerWidget.h"
#include "KebabHeaderView.h"
#include "NetworkReplyError.h"
#include "NodeEventSocket.h"
#include "OfficeChannelMirror.h"
#include "PromptDraftStore.h"
#include "QtRuntimeDiagnostics.h"
#include "RewardPoolSigner.h"
#include "TerminalWidget.h"
#include "WorldSpeechBridge.h"

#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrlQuery>
#include <QUuid>
#include <QtConcurrentRun>

#include <utility>

#include <atomic>

#if defined(Q_OS_UNIX)
#include <signal.h>
#endif

using namespace forkmesh::ui;

namespace {
constexpr int kVultrProvisionStageCount = 6;
} // namespace

QString MainWindow::vultrProvisionLogPath() const
{
    const QString appData =
        forkmesh::paths::dataRoot();
    if (appData.isEmpty())
        return {};
    const QString dir = QDir(appData).filePath(QStringLiteral("deployments"));
    if (!QDir().mkpath(dir))
        return {};
    return QDir(dir).filePath(QStringLiteral("vultr-mirror-latest.log"));
}

namespace {
QString conciseElapsedTime(qint64 elapsedMs)
{
    const qint64 seconds = (qMax<qint64>(0, elapsedMs) + 999) / 1000;
    if (seconds < 60)
        return QStringLiteral("%1s").arg(seconds);
    const qint64 minutes = seconds / 60;
    if (minutes < 60) {
        return QStringLiteral("%1m %2s")
            .arg(minutes)
            .arg(seconds % 60);
    }
    return QStringLiteral("%1h %2m %3s")
        .arg(minutes / 60)
        .arg(minutes % 60)
        .arg(seconds % 60);
}

// Shared by the detached writers below, so they outlive the MainWindow that
// queued them and never race each other onto the same path.
QMutex &vultrLogWriteMutex()
{
    static QMutex mutex;
    return mutex;
}

quint64 &vultrLogWrittenSequence() // guarded by vultrLogWriteMutex()
{
    static quint64 written = 0;
    return written;
}

std::atomic<quint64> &vultrLogWriteSequence()
{
    static std::atomic<quint64> next{0};
    return next;
}
} // namespace

void MainWindow::saveVultrProvisionLog()
{
    if (!m_hostInstallLog)
        return;
    const QString path = vultrProvisionLogPath();
    if (path.isEmpty())
        return;
    QString plain = m_hostInstallLog->toPlainText();
    constexpr int kMaxPersistedChars = 1024 * 1024;
    if (plain.size() > kMaxPersistedChars)
        plain = plain.right(kMaxPersistedChars);
    // Reading the widget has to happen here, but QSaveFile::commit() fsyncs and
    // renames up to a megabyte while the install is still streaming output —
    // sampled blocking the GUI thread. The sequence number keeps a slow write
    // from landing on top of a newer one.
    const quint64 sequence = ++vultrLogWriteSequence();
    (void)QtConcurrent::run([path, bytes = plain.toUtf8(), sequence] {
        QMutexLocker locker(&vultrLogWriteMutex());
        if (sequence < vultrLogWrittenSequence())
            return; // a newer snapshot already reached the disk
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return;
        file.write(bytes);
        if (file.commit())
            vultrLogWrittenSequence() = sequence;
    });
}

void MainWindow::scheduleVultrProvisionLogSave()
{
    if (m_vultrLogSaveScheduled)
        return;
    m_vultrLogSaveScheduled = true;
    QTimer::singleShot(250, this, [this] {
        m_vultrLogSaveScheduled = false;
        saveVultrProvisionLog();
    });
}

void MainWindow::renderVultrProvisionProgress(bool failed)
{
    const int current = qBound(0, m_vultrProvisionStage,
                               kVultrProvisionStageCount);
    const bool dark = currentThemeIsDark();
    const QString pendingBg = dark ? QStringLiteral("#30363d") : QStringLiteral("#eaeef2");
    const QString pendingFg = dark ? QStringLiteral("#8b949e") : QStringLiteral("#57606a");
    const QString pendingBorder = dark ? QStringLiteral("#484f58") : QStringLiteral("#d0d7de");
    const QString reachedLabelColor = dark ? QStringLiteral("#f0f6fc") : QStringLiteral("#1f2328");
    const QString pendingLabelColor = pendingFg;
    const QString activeDetailColor = dark ? QStringLiteral("#58a6ff") : QStringLiteral("#0969da");
    const QString failedDetailColor = dark ? QStringLiteral("#f85149") : QStringLiteral("#cf222e");
    const QString completeDetailColor = dark ? QStringLiteral("#3fb950") : QStringLiteral("#1f883d");
    // Every stage update re-renders all stages, and most keep their look; a
    // repeated setStyleSheet() still reparses and repolishes the label.
    const auto applySheet = [](QLabel *label, const QString &sheet) {
        if (label->styleSheet() != sheet)
            label->setStyleSheet(Theme::remapSheet(sheet));
    };
    for (int i = 0; i < m_vultrStageNumbers.size(); ++i) {
        const int stage = i + 1;
        const bool complete =
            m_vultrProvisionState == QLatin1String("succeeded") ||
            (current > stage);
        const bool active = current == stage &&
                            m_vultrProvisionState == QLatin1String("active");
        const bool stageFailed = failed && current == stage;
        QString background = pendingBg;
        QString foreground = pendingFg;
        QString border = pendingBorder;
        if (complete) {
            background = QStringLiteral("#238636");
            foreground = QStringLiteral("#ffffff");
            border = QStringLiteral("#2ea043");
        } else if (active) {
            background = QStringLiteral("#1f6feb");
            foreground = QStringLiteral("#ffffff");
            border = QStringLiteral("#58a6ff");
        } else if (stageFailed) {
            background = QStringLiteral("#da3633");
            foreground = QStringLiteral("#ffffff");
            border = QStringLiteral("#f85149");
        }
        applySheet(m_vultrStageNumbers.at(i),
            QStringLiteral("QLabel { background:%1; color:%2; border:2px solid %3; "
                           "border-radius:14px; font-weight:700; }")
                .arg(background, foreground, border));
        applySheet(m_vultrStageLabels.at(i),
            QStringLiteral("QLabel { color:%1; font-size:11px; %2 }")
                .arg((complete || active || stageFailed)
                         ? reachedLabelColor
                         : pendingLabelColor,
                     active ? QStringLiteral("font-weight:700;") : QString()));
        if (i < m_vultrStageDetails.size() && m_vultrStageDetails.at(i)) {
            QLabel *detailLabel = m_vultrStageDetails.at(i);
            const QString text =
                i < m_vultrStageDetailText.size() ? m_vultrStageDetailText.at(i) : QString();
            detailLabel->setText(text);
            detailLabel->setToolTip(text);
            detailLabel->setVisible(!text.trimmed().isEmpty());
            const QString detailColor = stageFailed
                ? failedDetailColor
                : active ? activeDetailColor
                : complete ? completeDetailColor
                : pendingLabelColor;
            applySheet(detailLabel,
                QStringLiteral("QLabel { color:%1; font-size:11px; }").arg(detailColor));
        }
    }
    if (m_vultrLiveBadge) {
        const bool live = m_vultrProvisionState == QLatin1String("succeeded");
        const bool verifying = m_vultrProvisionState == QLatin1String("active") &&
                               current == kVultrProvisionStageCount;
        m_vultrLiveBadge->setVisible(live || verifying);
        m_vultrLiveBadge->setText(
            live
                ? QString::fromUtf8("\xE2\x97\x8F LIVE \xE2\x80\x94 mirror is serving repository traffic")
                : QString::fromUtf8("\xE2\x97\x8F VERIFYING \xE2\x80\x94 waiting for healthy public traffic"));
        m_vultrLiveBadge->setStyleSheet(Theme::remapSheet(
            live
                ? QStringLiteral("QLabel { color:#3fb950; background:#0d2818; "
                                 "border:1px solid #238636; border-radius:6px; "
                                 "padding:7px; font-weight:700; }")
                : QStringLiteral("QLabel { color:#d29922; background:#2d2106; "
                                 "border:1px solid #9e6a03; border-radius:6px; "
                                 "padding:7px; font-weight:700; }")));
    }
}

void MainWindow::recordVultrStageDetail(int stage, const QString &detail)
{
    const int idx = qBound(1, stage, kVultrProvisionStageCount) - 1;
    if (idx >= 0 && idx < m_vultrStageDetailText.size())
        m_vultrStageDetailText[idx] = detail;
}

void MainWindow::resetVultrStageDetails()
{
    for (QString &text : m_vultrStageDetailText)
        text.clear();
    for (QLabel *label : std::as_const(m_vultrStageDetails)) {
        if (!label)
            continue;
        label->clear();
        label->setVisible(false);
    }
}

void MainWindow::persistVultrProvisionState(const QString &state,
                                             const QString &message)
{
    if (!state.isEmpty())
        m_vultrProvisionState = state;
    if (!message.isNull())
        m_vultrProvisionMessage = message;
    QJsonObject checkpoint{
        {QStringLiteral("version"), 1},
        {QStringLiteral("state"), m_vultrProvisionState},
        {QStringLiteral("stage"), m_vultrProvisionStage},
        {QStringLiteral("detail"), m_vultrProvisionDetail},
        {QStringLiteral("message"), m_vultrProvisionMessage},
        {QStringLiteral("node"), m_vultrProvisionNode},
        {QStringLiteral("dnsHostname"), m_vultrDnsHostname},
        {QStringLiteral("instanceId"), m_vultrInstanceId},
        {QStringLiteral("ip"), m_vultrInstanceIp},
        {QStringLiteral("identityFile"), m_vultrIdentityFile},
        {QStringLiteral("installAgentClis"), m_vultrInstallAgentClis},
        {QStringLiteral("installUseLocalBinary"),
         m_vultrInstallUseLocalBinary},
        {QStringLiteral("pollCount"), m_vultrPollCount},
        {QStringLiteral("installAttempts"), m_vultrInstallAttempts},
        {QStringLiteral("hostMetadata"), m_vultrHostMetadata},
        {QStringLiteral("updatedAt"), QDateTime::currentMSecsSinceEpoch()},
    };
    QSettings settings;
    settings.setValue(
        kVultrProvisionSetting,
        QString::fromUtf8(
            QJsonDocument(checkpoint).toJson(QJsonDocument::Compact)));
    settings.sync();
    saveVultrProvisionLog();
}

void MainWindow::setVultrProvisionStage(int stage, const QString &detail,
                                        bool failed)
{
    const int bounded = qBound(1, stage, kVultrProvisionStageCount);
    const int previousStage = m_vultrProvisionStage;
    const QString previousDetail = m_vultrProvisionDetail;
    m_vultrProvisionStage = m_vultrResumeChain
                                ? qMax(m_vultrProvisionStage, bounded)
                                : bounded;
    if (!detail.isEmpty()) {
        m_vultrProvisionDetail = detail;
        recordVultrStageDetail(m_vultrProvisionStage, detail);
    }
    if (failed) {
        pingVultrProvisionStage(m_vultrProvisionStage, false,
                                m_vultrProvisionDetail);
    } else if (previousStage > 0 &&
               m_vultrProvisionStage > previousStage) {
        pingVultrProvisionStage(previousStage, true, previousDetail);
    }
    if (m_vultrStatus && !m_vultrProvisionDetail.isEmpty())
        m_vultrStatus->setText(m_vultrProvisionDetail);
    renderVultrProvisionProgress(failed);
    persistVultrProvisionState(failed ? QStringLiteral("failed")
                                      : QStringLiteral("active"),
                                 failed ? m_vultrProvisionDetail : QString());
}

void MainWindow::pingVultrProvisionStage(int stage, bool ok,
                                         const QString &detail)
{
    static const QStringList stageNames = {
        QStringLiteral("Credentials"),
        QStringLiteral("Plan + image"),
        QStringLiteral("Create server"),
        QStringLiteral("Boot + connect"),
        QStringLiteral("Install"),
        QStringLiteral("Live traffic"),
    };
    const int bounded = qBound(1, stage, kVultrProvisionStageCount);
    const QString node = m_vultrProvisionNode.isEmpty()
                             ? QStringLiteral("new mirror")
                             : m_vultrProvisionNode;
    const QString outcome = ok ? QStringLiteral("succeeded")
                               : QStringLiteral("failed");
    QString body = QStringLiteral("%1 · step %2 of %3 %4.")
                       .arg(node)
                       .arg(bounded)
                       .arg(kVultrProvisionStageCount)
                       .arg(outcome);
    const QString summary = detail.simplified();
    if (!summary.isEmpty())
        body += QLatin1Char(' ') + summary;
    addNotification(
        QStringLiteral("Vultr mirror deployment · %1 %2")
            .arg(stageNames.at(bounded - 1), outcome),
        body, !ok);
}

void MainWindow::restoreVultrProvision()
{
    const QJsonObject saved = QJsonDocument::fromJson(
        QSettings().value(kVultrProvisionSetting).toString().toUtf8()).object();
    if (saved.value(QStringLiteral("version")).toInt() != 1)
        return;
    m_vultrProvisionState = saved.value(QStringLiteral("state")).toString();
    m_vultrProvisionStage = saved.value(QStringLiteral("stage")).toInt();
    m_vultrProvisionDetail = saved.value(QStringLiteral("detail")).toString();
    m_vultrProvisionMessage = saved.value(QStringLiteral("message")).toString();
    m_vultrProvisionNode = saved.value(QStringLiteral("node")).toString();
    m_vultrDnsHostname =
        saved.value(QStringLiteral("dnsHostname")).toString();
    m_vultrInstanceId = saved.value(QStringLiteral("instanceId")).toString();
    m_vultrInstanceIp = saved.value(QStringLiteral("ip")).toString();
    m_vultrIdentityFile =
        saved.value(QStringLiteral("identityFile")).toString();
    m_vultrInstallAgentClis =
        saved.value(QStringLiteral("installAgentClis")).toBool();
    m_vultrInstallUseLocalBinary =
        saved.value(QStringLiteral("installUseLocalBinary")).toBool();
    m_vultrPollCount = saved.value(QStringLiteral("pollCount")).toInt();
    m_vultrInstallAttempts =
        saved.value(QStringLiteral("installAttempts")).toInt();
    m_vultrHostMetadata =
        saved.value(QStringLiteral("hostMetadata")).toObject();
    if (m_vultrNameEdit && !m_vultrProvisionNode.isEmpty())
        m_vultrNameEdit->setText(m_vultrProvisionNode);
    if (m_vultrAgentClisCheck)
        m_vultrAgentClisCheck->setChecked(m_vultrInstallAgentClis);
    const QString logPath = vultrProvisionLogPath();
    QFile log(logPath);
    if (m_hostInstallLog && log.open(QIODevice::ReadOnly))
        m_hostInstallLog->setPlainText(
            QString::fromUtf8(log.read(1024 * 1024)));
    const bool failed = m_vultrProvisionState == QLatin1String("failed");
    resetVultrStageDetails();
    if (!m_vultrProvisionDetail.isEmpty())
        recordVultrStageDetail(m_vultrProvisionStage, m_vultrProvisionDetail);
    renderVultrProvisionProgress(failed);
    updateVultrEndDeploymentButtonVisibility();
    if (m_vultrStatus) {
        const QString restored = !m_vultrProvisionMessage.isEmpty()
                                     ? m_vultrProvisionMessage
                                     : m_vultrProvisionDetail;
        m_vultrStatus->setText(restored);
    }
    if (m_vultrCreateButton) {
        m_vultrCreateButton->setText(
            failed ? QStringLiteral("Retry deployment")
                   : m_vultrProvisionState == QLatin1String("active")
                         ? QStringLiteral("Resuming deployment…")
                         : QStringLiteral("Create another mirror"));
    }
    m_vultrResumeRequested =
        m_vultrProvisionState == QLatin1String("active") || failed;
    if (m_vultrProvisionState == QLatin1String("active")) {
        QTimer::singleShot(0, this, &MainWindow::resumeVultrProvision);
    }
}

void MainWindow::resumeVultrProvision()
{
    if (resumeVultrProvisionThroughCore())
        return;
    if (!m_vultrResumeRequested || m_vultrProvisionNode.isEmpty())
        return;
    QString apiKey = m_vultrApiKeyEdit
                         ? m_vultrApiKeyEdit->text().trimmed()
                         : QString();
    if (apiKey.isEmpty())
        apiKey = forkmesh::control::vultrApiKeyFromVariables(
            ActionStore::variables());
    const QString invalid = forkmesh::control::validateVultrMirrorRequest(
        apiKey, m_vultrProvisionNode);
    if (!invalid.isEmpty()) {
        m_vultrProvisionActive = false;
        if (m_vultrStatus)
            m_vultrStatus->setText(
                QStringLiteral("Deployment is paused. Enter the saved Vultr "
                               "API key, then click Resume deployment. %1")
                    .arg(invalid));
        if (m_vultrCreateButton) {
            m_vultrCreateButton->setEnabled(true);
            m_vultrCreateButton->setText(
                QStringLiteral("Resume deployment"));
        }
        updateVultrEndDeploymentButtonVisibility();
        return;
    }
    const QMap<QString, QString> variables = ActionStore::variables();
    m_vultrTunnelApiToken =
        forkmesh::control::cloudflareApiTokenFromVariables(variables);
    m_vultrProvisionActive = true;
    if (m_vultrCreateButton) {
        m_vultrCreateButton->setEnabled(false);
        m_vultrCreateButton->setText(QStringLiteral("Deployment running…"));
    }
    updateVultrEndDeploymentButtonVisibility();
    appendHostInstallLog(QString::fromUtf8(
        "\n\xE2\x86\xBB Desktop restarted \xE2\x80\x94 resuming Vultr deployment at stage %1.\n")
        .arg(m_vultrProvisionStage));
    if (m_vultrProvisionStage >= 6 && !m_vultrProvisionNode.isEmpty()) {
        const QString address = m_vultrDnsHostname.isEmpty()
                                    ? m_vultrInstanceIp
                                    : m_vultrDnsHostname;
        seedVultrMirrorRepositoryThenVerify(
            m_vultrProvisionNode,
            QStringLiteral("Vultr mirror \"%1\" (%2) is installed, linked, "
                           "and serving traffic.")
                .arg(m_vultrProvisionNode, address));
        return;
    }
    if (m_vultrProvisionStage >= 5 && !m_vultrInstanceIp.isEmpty() &&
        !m_vultrIdentityFile.isEmpty()) {
        m_vultrInstallAttempts = 0;
        persistVultrProvisionState();
        QTimer::singleShot(1000, this, [this] {
            startVultrHostInstall(m_vultrProvisionNode, m_vultrInstanceIp,
                                  m_vultrIdentityFile);
        });
        return;
    }
    if (!m_vultrInstanceId.isEmpty() && !m_vultrIdentityFile.isEmpty()) {
        setVultrProvisionStage(4, QStringLiteral("Resuming server boot checks…"));
        pollVultrInstance(apiKey, m_vultrInstanceId, m_vultrProvisionNode,
                          m_vultrIdentityFile);
        return;
    }
    m_vultrResumeRequested = false;
    m_vultrResumeChain = true;
    m_vultrProvisionActive = false;
    createVultrMirrorFromForm();
}

void MainWindow::updateVultrEndDeploymentButtonVisibility()
{
    // Every Vultr provisioning transition already ends here, so this is also
    // where the Network rail lamp is brought back in step.
    updateNetworkDeployBlink();
    if (!m_vultrEndDeploymentButton)
        return;
    const bool hasDeployment =
        m_vultrProvisionActive || m_vultrProvisionState == QLatin1String("active") ||
        m_vultrProvisionState == QLatin1String("failed");
    m_vultrEndDeploymentButton->setVisible(hasDeployment);
}

void MainWindow::endVultrProvision()
{
    const bool hasDeployment =
        m_vultrProvisionActive || m_vultrProvisionState == QLatin1String("active") ||
        m_vultrProvisionState == QLatin1String("failed");
    if (!hasDeployment)
        return;
    const QString node = m_vultrProvisionNode;
    const QString instanceId = m_vultrInstanceId;
    QString warning = QStringLiteral(
        "This stops ForkMesh from tracking the current deployment attempt%1 so "
        "you can start a fresh one.")
        .arg(node.isEmpty() ? QString()
                            : QStringLiteral(" for \"%1\"").arg(node));
    if (!instanceId.isEmpty()) {
        warning += QString::fromUtf8(
            " The Vultr server already created for it (instance %1) keeps "
            "running and billing until you destroy it \xE2\x80\x94 from the "
            "Vultr panel, or from this list's Destroy button once the host is "
            "saved here.").arg(instanceId);
    }
    warning += QStringLiteral(" Continue?");
    const auto reply = QMessageBox::question(
        this, QStringLiteral("End deployment"), warning,
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;
    if (endVultrProvisionThroughCore())
        return;

    m_vultrProvisionActive = false;
    if (m_vultrSshProbeProcess) {
        QProcess *probe = m_vultrSshProbeProcess;
        m_vultrSshProbeProcess = nullptr;
        probe->disconnect(this);
        if (probe->state() != QProcess::NotRunning)
            probe->kill();
        probe->deleteLater();
    }
    m_vultrResumeRequested = false;
    m_vultrResumeChain = false;
    m_vultrProvisionState.clear();
    m_vultrProvisionStage = 0;
    m_vultrProvisionDetail.clear();
    m_vultrProvisionMessage.clear();
    m_vultrProvisionNode.clear();
    m_vultrInstanceId.clear();
    m_vultrInstanceIp.clear();
    m_vultrIdentityFile.clear();
    m_vultrDnsHostname.clear();
    m_vultrHostMetadata = QJsonObject();
    m_vultrPollCount = 0;
    m_vultrInstallAttempts = 0;
    m_vultrTunnelApiToken.clear();
    resetVultrStageDetails();
    QSettings().remove(kVultrProvisionSetting);
    if (m_vultrNameEdit)
        m_vultrNameEdit->clear();
    if (m_vultrStatus)
        m_vultrStatus->clear();
    if (m_vultrCreateButton) {
        m_vultrCreateButton->setEnabled(true);
        m_vultrCreateButton->setText(QStringLiteral("Create Vultr mirror"));
    }
    appendHostInstallLog(QString::fromUtf8(
        "\n\xE2\x9A\xA0 Deployment ended by request%1.\n")
            .arg(node.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(node)));
    renderVultrProvisionProgress();
    updateVultrEndDeploymentButtonVisibility();
    saveVultrProvisionLog();
}

void MainWindow::findVultrProvisionInstance(
    const QString &apiKey, const QString &node,
    std::function<void(QString, QString)> onDone)
{
    vultrApiCall(
        apiKey, QStringLiteral("/v2/instances?per_page=500"),
        QByteArrayLiteral("GET"), {},
        [node, onDone](QJsonObject result, QString error) {
            if (!error.isEmpty()) {
                onDone({}, error);
                return;
            }
            for (const QJsonValue &value :
                 result.value(QStringLiteral("instances")).toArray()) {
                const QJsonObject instance = value.toObject();
                const QString label =
                    instance.value(QStringLiteral("label")).toString();
                const QString hostname =
                    instance.value(QStringLiteral("hostname")).toString();
                if (label.compare(node, Qt::CaseInsensitive) == 0 ||
                    hostname.compare(node, Qt::CaseInsensitive) == 0) {
                    onDone(instance.value(QStringLiteral("id")).toString(), {});
                    return;
                }
            }
            onDone({}, {});
        });
}

QString MainWindow::savedHostAddressNamed(const QString &node) const
{
    QSettings settings;
    const QJsonArray hosts = forkmesh::control::loadSavedHosts(
        settings, kHostsSetting, nullptr);
    for (const QJsonValue &value : hosts) {
        const QJsonObject host = value.toObject();
        if (host.value(QStringLiteral("name")).toString().trimmed().compare(
                node, Qt::CaseInsensitive) == 0) {
            return host.value(QStringLiteral("ip")).toString().trimmed();
        }
    }
    return {};
}

void MainWindow::diagnoseVultrMirrorNode(const QString &node,
                                         bool restartService,
                                         std::function<void(QString)> onDone)
{
    // Report the service state plus the newest sync warning, which is where the
    // mirror node records a failed first clone ("all upstreams failed: … clone
    // failed") and the gateway's refusal to serve a repository whose refs it
    // cannot read. Restart last so the reported lines describe the run that
    // stalled rather than the one the restart just began.
    const QString remoteCommand = QStringLiteral(
        "sh -lc 'systemctl is-active forkmesh-mirror-node >/dev/null 2>&1 && "
        "echo \"service: running\" || echo \"service: not running\"; "
        "test -d /var/lib/forkmesh/repositories && "
        "find /var/lib/forkmesh/repositories -maxdepth 1 -name \"*.git\" "
        "-printf \"clone: %f\\\\n\" 2>/dev/null | head -n 4; "
        "journalctl -u forkmesh-mirror-node -n 200 --no-pager 2>/dev/null | "
        "grep -E \"warnings|failed\" | tail -n 2; "
        "%1'")
        .arg(restartService
                 ? QStringLiteral(
                       "if ls -d /var/lib/forkmesh/repositories/.clone-* "
                       ">/dev/null 2>&1; then "
                       "echo \"restart: skipped, a clone is still running\"; "
                       "elif systemctl restart forkmesh-mirror-node "
                       ">/dev/null 2>&1; then echo \"restart: requested\"; "
                       "else echo \"restart: refused\"; fi")
                 : QStringLiteral("true"));
    runMirrorNodeSsh(node, remoteCommand, 45000,
                     [onDone](bool ok, QString output) {
                         onDone(ok ? output : QString());
                     },
                     /*preferProvisioningHost=*/true);
}

void MainWindow::runMirrorNodeSsh(
    const QString &node, const QString &remoteCommand, int timeoutMs,
    std::function<void(bool ok, QString output)> onDone,
    bool preferProvisioningHost)
{
    if (runMirrorNodeSshThroughCore(node, remoteCommand, timeoutMs, onDone,
                                    preferProvisioningHost))
        return;
    // The provisioning checks reach the server being created, whose address
    // and key are not saved yet; everything else names a saved host.
    QString ip = preferProvisioningHost ? m_vultrInstanceIp.trimmed() : QString();
    if (ip.isEmpty())
        ip = savedHostAddressNamed(node);
    if (ip.isEmpty()) {
        onDone(false, {});
        return;
    }
    const QString user = QStringLiteral("root");
    QString identityFile =
        preferProvisioningHost ? m_vultrIdentityFile.trimmed() : QString();
    if (identityFile.isEmpty())
        identityFile = savedHostIdentityFile(node, ip, user);
    QString sshError;
    const forkmesh::control::HostSshCommand ssh =
        forkmesh::control::buildHostSshCommand(
            ip, user, QString(), remoteCommand, &sshError, identityFile);
    if (ssh.program.isEmpty()) {
        onDone(false, sshError);
        return;
    }

    auto *probe = new QProcess(this);
    probe->setProcessChannelMode(QProcess::MergedChannels);
    probe->setProcessEnvironment(ssh.environment);
    auto output = std::make_shared<QByteArray>();
    auto reported = std::make_shared<bool>(false);
    const auto report = [onDone, output, reported](bool ok) {
        if (*reported)
            return;
        *reported = true;
        onDone(ok, forkmesh::control::redactProcessOutput(
                       QString::fromUtf8(output->right(2048)))
                       .trimmed());
    };
    connect(probe, &QProcess::readyReadStandardOutput, probe,
            [probe, output] { output->append(probe->readAllStandardOutput()); });
    auto *deadline = new QTimer(probe);
    deadline->setSingleShot(true);
    deadline->setInterval(timeoutMs);
    connect(deadline, &QTimer::timeout, probe, [probe] { probe->kill(); });
    connect(probe, &QProcess::errorOccurred, this,
            [probe, report](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                probe->deleteLater();
                report(false);
            });
    connect(probe, &QProcess::finished, this,
            [probe, deadline, output, report](int code,
                                              QProcess::ExitStatus status) {
                deadline->stop();
                output->append(probe->readAllStandardOutput());
                probe->deleteLater();
                report(status == QProcess::NormalExit && code == 0);
            });
    deadline->start();
    probe->start(ssh.program, ssh.arguments);
}

void MainWindow::finishVultrProvision(bool ok, const QString &message)
{
    if (!m_vultrProvisionActive)
        return;
    m_vultrProvisionActive = false;
    if (m_vultrSshProbeProcess) {
        QProcess *probe = m_vultrSshProbeProcess;
        m_vultrSshProbeProcess = nullptr;
        probe->disconnect(this);
        if (probe->state() != QProcess::NotRunning)
            probe->kill();
        probe->deleteLater();
    }
    if (m_vultrCreateButton)
        m_vultrCreateButton->setText(ok ? QStringLiteral("Create another mirror")
                                        : QStringLiteral("Retry deployment"));
    if (!ok && m_vultrStatus)
        m_vultrStatus->setText(QString::fromUtf8("\xE2\x9C\x98 ") + message);
    if (!ok && !message.isEmpty())
        recordVultrStageDetail(m_vultrProvisionStage, message);
    if (!message.isEmpty()) {
        const QString outcome =
            (ok ? QString::fromUtf8("\xE2\x9C\x94 ")
                : QString::fromUtf8("\xE2\x9C\x98 ")) +
            message + QStringLiteral("\n");
        appendHostInstallLog(
            QStringLiteral("\n") + timestampedHostInstallEvent(outcome));
    }
    pingVultrProvisionStage(m_vultrProvisionStage, ok, message);
    // A create the owner's phone started hears how it ended.
    m_vultrRequestedPlan.clear();
    m_vultrRequestedRegion.clear();
    finishLocalControlHostAction(QStringLiteral("install"), m_vultrProvisionNode,
                                 ok, message);
    if (ok) {
        if (m_vultrNameEdit)
            m_vultrNameEdit->clear();
        if (m_vultrStatus)
            m_vultrStatus->clear();
    }
    saveVultrProvisionLog();
    m_vultrProvisionActive = false;
    m_vultrResumeRequested = !ok;
    m_vultrResumeChain = false;
    persistVultrProvisionState(ok ? QStringLiteral("succeeded")
                                  : QStringLiteral("failed"),
                                 message);
    renderVultrProvisionProgress(!ok);
    updateVultrEndDeploymentButtonVisibility();
    m_vultrTunnelApiToken.clear();
    if (m_vultrCreateButton)
        m_vultrCreateButton->setEnabled(true);
}

void MainWindow::runVultrMirrorSeedPush(std::function<void(bool)> onDone)
{
    const QString bareMirror = repositoryMirrorRoot() +
                               QStringLiteral("/forkmesh-forkmesh.git");
    const QStringList arguments =
        forkmesh::control::mirrorSeedPushArguments(bareMirror,
                                                   m_vultrInstanceIp);
    if (arguments.isEmpty() || m_vultrIdentityFile.trimmed().isEmpty() ||
        !QFileInfo::exists(bareMirror)) {
        appendHostInstallLog(QStringLiteral(
            "Skipping the repository seed push (no local bare mirror, "
            "address, or SSH identity available).\n"));
        if (onDone)
            onDone(false);
        return;
    }
    if (m_vultrSeedProcess &&
        m_vultrSeedProcess->state() != QProcess::NotRunning) {
        appendHostInstallLog(QStringLiteral(
            "A repository seed push is already running; not starting "
            "another.\n"));
        if (onDone)
            onDone(false);
        return;
    }
    appendHostInstallLog(QString::fromUtf8(
        "Seeding forkmesh/forkmesh on %1 from this node's local mirror "
        "(a no-op when the node already cloned it)\xE2\x80\xA6\n")
        .arg(m_vultrInstanceIp));
    auto *process = new QProcess(this);
    m_vultrSeedProcess = process;
    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral("GIT_SSH_COMMAND"),
        forkmesh::control::mirrorSeedSshCommand(m_vultrIdentityFile));
    process->setProcessEnvironment(environment);
    process->setProcessChannelMode(QProcess::MergedChannels);
    // A stalled transfer must not park the deployment: kill and continue —
    // verification below decides whether the node ever becomes healthy.
    auto *hardCap = new QTimer(process);
    hardCap->setSingleShot(true);
    connect(hardCap, &QTimer::timeout, process, [process] {
        process->kill();
    });
    hardCap->start(15 * 60 * 1000);
    auto finished = std::make_shared<bool>(false);
    const auto conclude = [this, process, onDone, finished](bool ok) {
        if (*finished)
            return;
        *finished = true;
        const QString output =
            QString::fromUtf8(process->readAll()).trimmed();
        process->deleteLater();
        if (ok) {
            appendHostInstallLog(QString::fromUtf8(
                "\xE2\x9C\x94 Repository seed push completed.\n"));
        } else {
            appendHostInstallLog(QStringLiteral(
                "The repository seed push did not complete%1 — the node "
                "will rely on the public mesh for its first sync.\n")
                .arg(output.isEmpty()
                         ? QString()
                         : QStringLiteral(": %1")
                               .arg(output.section(QLatin1Char('\n'), -1)
                                        .trimmed())));
        }
        if (onDone)
            onDone(ok);
    };
    connect(process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [conclude](int exitCode, QProcess::ExitStatus status) {
        conclude(status == QProcess::NormalExit && exitCode == 0);
    });
    connect(process, &QProcess::errorOccurred, this,
            [conclude](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            conclude(false);
    });
    process->start(QStringLiteral("git"), arguments);
}

void MainWindow::seedVultrMirrorRepositoryThenVerify(
    const QString &node, const QString &successMessage)
{
    if (!m_vultrProvisionActive)
        return;
    runVultrMirrorSeedPush([this, node, successMessage](bool) {
        if (!m_vultrProvisionActive)
            return;
        waitForVultrMirrorPublication(node, successMessage);
    });
}

void MainWindow::waitForVultrMirrorPublication(
    const QString &node, const QString &successMessage, int attempt)
{
    // Verification is bounded: a mirror that never became reachable used to keep this
    // stage retrying once a minute forever, which left the deployment "running"
    // with no cause shown and blocked the fleet reconciler behind it. Poll fast
    // for fifteen minutes, nudge the node once and say what it reported, then
    // poll slowly for fifteen more before ending the deployment with that
    // reason so it can be retried.
    constexpr int kFastPublicationPolls = 90; // fifteen minutes at 10 seconds
    constexpr int kSlowPublicationPolls = 15; // fifteen more at 60 seconds
    constexpr int kMaxPublicationPolls =
        kFastPublicationPolls + kSlowPublicationPolls;
    if (!m_vultrProvisionActive)
        return;
    m_vultrProvisionNode = node;
    m_vultrResumeChain = false;
    if (attempt == 0)
        m_vultrPublicationGap.clear();
    setVultrProvisionStage(
        6,
        QString::fromUtf8(
            "ForkMesh is running on %1 — verifying healthy public traffic "
            "(%2/%3)…%4")
            .arg(node)
            .arg(attempt + 1)
            .arg(kMaxPublicationPolls)
            .arg(m_vultrPublicationGap.isEmpty()
                     ? QString()
                     : QStringLiteral(" Still missing: %1.")
                           .arg(m_vultrPublicationGap)));

    const qint64 issuedAt = QDateTime::currentMSecsSinceEpoch();
    const QString nonce =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    QUrl url(QStringLiteral("https://") +
             m_vultrDnsHostname.trimmed().toLower());
    url.setPath(QStringLiteral("/health"));
    QUrlQuery healthQuery;
    healthQuery.addQueryItem(QStringLiteral("nonce"), nonce);
    healthQuery.addQueryItem(QStringLiteral("issuedAt"),
                             QString::number(issuedAt));
    healthQuery.addQueryItem(QStringLiteral("owner"),
                             QStringLiteral("forkmesh"));
    healthQuery.addQueryItem(QStringLiteral("repo"),
                             QStringLiteral("forkmesh"));
    url.setQuery(healthQuery);
    QNetworkRequest request(url);
    request.setRawHeader(QByteArrayLiteral("accept"),
                         QByteArrayLiteral("application/json"));
    request.setTransferTimeout(5000);
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, node, successMessage, attempt, nonce, issuedAt] {
        const QJsonObject payload =
            QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();

        const QJsonObject proof =
            payload.value(QStringLiteral("repositoryProof")).toObject();
        const bool published = verifiedFlagshipMirrorHealth(
            payload, node, nonce, issuedAt);
        // Name the gate the node has not cleared yet. Without it this stage
        // shows only a rising poll counter, so an install that is really just
        // waiting on its endpoint lease is indistinguishable from one that
        // failed, and the wait reads as a hang.
        if (!published) {
            const bool emptyRepository =
                !proof.isEmpty() &&
                !forkmesh::control::mirrorRepositoryProofServesContent(
                    proof.value(QStringLiteral("refsSha256")).toString());
            const QString gap = payload.isEmpty()
                                    ? QStringLiteral(
                                          "its direct HTTPS endpoint is not answering")
                                    : proof.isEmpty()
                                          ? QStringLiteral(
                                                "the forkmesh/forkmesh repository proof is missing")
                                          : emptyRepository
                                                ? QStringLiteral(
                                                      "its repository is still empty — no refs to serve yet")
                                                : QStringLiteral(
                                                      "the signed repository health proof is invalid");
            if (!gap.isEmpty() && gap != m_vultrPublicationGap) {
                appendHostInstallLog(
                    QStringLiteral("%1 is not a healthy public mirror yet: %2.\n")
                        .arg(node, gap));
            }
            m_vultrPublicationGap = gap;
        }
        if (published) {
            appendHostInstallLog(QString::fromUtf8(
                "\n\xE2\x9C\x94 Verified %1 directly: its HTTPS endpoint returned "
                "a valid signed forkmesh/forkmesh repository proof. No relay "
                "Worker was used.\n").arg(node));
            rememberHost(node, m_vultrInstanceIp, QStringLiteral("root"),
                         QString(), QStringLiteral("installed"),
                         m_vultrIdentityFile, m_vultrHostMetadata);
            finishVultrProvision(true, successMessage);
            return;
        }
        if (attempt + 1 >= kMaxPublicationPolls) {
            appendHostInstallLog(QString::fromUtf8(
                "%1 never became a healthy public mirror; asking the node "
                "why\xE2\x80\xA6\n").arg(node));
            diagnoseVultrMirrorNode(
                node, /*restartService=*/false,
                [this, node](QString report) {
                    if (!report.isEmpty())
                        appendHostInstallLog(
                            QStringLiteral("%1\n").arg(report));
                    finishVultrProvision(false, QString::fromUtf8(
                        "%1 is installed and running but never returned a valid "
                        "signed repository health proof, so the deployment was "
                        "ended after thirty minutes.%2%3 The server is saved "
                        "under Hosts \xE2\x80\x94 click Retry deployment once "
                        "the cause is cleared.")
                        .arg(node,
                             m_vultrPublicationGap.isEmpty()
                                 ? QString()
                                 : QStringLiteral(" Direct verification was still "
                                                  "missing: %1.")
                                       .arg(m_vultrPublicationGap),
                             report.isEmpty()
                                 ? QString()
                                 : QStringLiteral(" It last reported: %1")
                                       .arg(report.section(
                                                QLatin1Char('\n'), -1)
                                                .trimmed())));
                });
            return;
        }
        if (attempt + 1 == kFastPublicationPolls) {
            // One nudge at the fifteen-minute mark: re-seed the repository
            // from this node's local mirror (the only source that works when
            // the public mesh itself is down), then restart the service so a
            // first clone that lost its connection retries instead of waiting
            // for an upstream push that may be hours away.
            appendHostInstallLog(QString::fromUtf8(
                "%1 has not served a valid repository proof after fifteen minutes; re-seeding "
                "its repository and restarting its mirror node\xE2\x80\xA6\n")
                .arg(node));
            runVultrMirrorSeedPush([this, node](bool) {
                diagnoseVultrMirrorNode(
                    node, /*restartService=*/true,
                    [this, node](QString report) {
                        if (report.isEmpty())
                            return;
                        appendHostInstallLog(
                            QStringLiteral("%1\n").arg(report));
                    });
            });
        }
        QTimer::singleShot(
            attempt + 1 < kFastPublicationPolls ? 10000 : 60000, this,
            [this, node, successMessage, attempt] {
                waitForVultrMirrorPublication(
                    node, successMessage, attempt + 1);
            });
    });
}

void MainWindow::vultrApiCall(const QString &apiKey, const QString &path,
                              const QByteArray &method, const QJsonObject &body,
                              std::function<void(QJsonObject, QString)> onDone)
{
    QNetworkRequest request(
        QUrl(QStringLiteral("https://api.vultr.com") + path));
    request.setRawHeader(QByteArrayLiteral("Authorization"),
                         QByteArrayLiteral("Bearer ") + apiKey.toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    QNetworkReply *reply = nullptr;
    if (method == QByteArrayLiteral("POST")) {
        reply = m_networkAccess->post(
            request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    } else if (method == QByteArrayLiteral("GET") || method.isEmpty()) {
        reply = m_networkAccess->get(request);
    } else {
        reply = m_networkAccess->sendCustomRequest(
            request, method,
            body.isEmpty()
                ? QByteArray()
                : QJsonDocument(body).toJson(QJsonDocument::Compact));
    }
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, apiKey, onDone] {
        reply->deleteLater();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                .toInt();
        const QJsonObject object =
            QJsonDocument::fromJson(reply->read(1024 * 1024)).object();
        if (reply->error() != QNetworkReply::NoError || status < 200 ||
            status >= 300) {
            QString detail =
                object.value(QStringLiteral("error")).toString();
            if (detail.isEmpty())
                detail = reply->errorString();
            onDone({}, QStringLiteral("Vultr API error (HTTP %1): %2")
                           .arg(status)
                           .arg(detail));
            return;
        }
        // Vultr just authenticated this key, so it is worth keeping: every
        // flow on this page (create, destroy) then resolves it from the store
        // instead of asking for it again. Later calls in the same run are a
        // no-op.
        QString rememberError;
        const QStringList remembered =
            rememberVultrApiKey(apiKey, &rememberError);
        if (!remembered.isEmpty()) {
            appendHostInstallLog(
                QStringLiteral("Saved your Vultr API key to %1.\n")
                    .arg(remembered.join(QStringLiteral(" and "))));
            if (!rememberError.isEmpty())
                appendHostInstallLog(rememberError + QLatin1Char('\n'));
        }
        onDone(object, QString());
    });
}

void MainWindow::cloudflareApiCall(const QString &apiToken, const QString &path,
                                   const QByteArray &method,
                                   const QJsonObject &body,
                                   std::function<void(QJsonObject, QString)> onDone)
{
    QNetworkRequest request(
        QUrl(QStringLiteral("https://api.cloudflare.com/client/v4") + path));
    request.setRawHeader(QByteArrayLiteral("Authorization"),
                         QByteArrayLiteral("Bearer ") + apiToken.toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    const QByteArray payload =
        QJsonDocument(body).toJson(QJsonDocument::Compact);
    QNetworkReply *reply =
        method == QByteArrayLiteral("GET")
            ? m_networkAccess->get(request)
            : m_networkAccess->sendCustomRequest(request, method, payload);
    connect(reply, &QNetworkReply::finished, this, [reply, onDone] {
        reply->deleteLater();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                .toInt();
        const QJsonObject object =
            QJsonDocument::fromJson(reply->read(1024 * 1024)).object();
        const bool succeeded =
            object.value(QStringLiteral("success")).toBool();
        if (reply->error() != QNetworkReply::NoError || status < 200 ||
            status >= 300 || !succeeded) {
            QString detail = object.value(QStringLiteral("errors"))
                                 .toArray()
                                 .first()
                                 .toObject()
                                 .value(QStringLiteral("message"))
                                 .toString();
            if (detail.isEmpty())
                detail = reply->errorString();
            onDone({}, QStringLiteral("Cloudflare API error (HTTP %1): %2")
                           .arg(status)
                           .arg(detail));
            return;
        }
        onDone(object, QString());
    });
}

void MainWindow::ensureVultrMirrorDns(const QString &node, const QString &ip,
                                      std::function<void(QString)> onDone)
{
    auto skip = [this, onDone](const QString &reason) {
        appendHostInstallLog(
            QStringLiteral("Skipping the Cloudflare DNS record: %1\n")
                .arg(reason));
        onDone(QString());
    };
    if (!m_networkAccess) {
        skip(QStringLiteral("network access is unavailable"));
        return;
    }
    const QMap<QString, QString> variables = ActionStore::variables();
    const QString apiToken =
        forkmesh::control::cloudflareApiTokenFromVariables(variables);
    if (apiToken.isEmpty()) {
        skip(QStringLiteral(
            "store a CLOUDFLARE_API_TOKEN device variable with DNS edit "
            "permission to name mirrors automatically"));
        return;
    }
    QSettings settings;
    QString zone = settings.value(QStringLiteral("control/cloudflareZone"))
                       .toString()
                       .trimmed();
    if (zone.isEmpty())
        zone = forkmesh::control::cloudflareZoneNameFromVariables(variables);
    const QString hostname =
        forkmesh::control::vultrMirrorDnsHostname(node, zone);
    if (hostname.isEmpty()) {
        skip(zone.isEmpty()
                 ? QStringLiteral("set the Cloudflare zone on the Control "
                                  "node page first")
                 : QStringLiteral("\"%1\" and \"%2\" do not form a valid "
                                  "hostname")
                       .arg(node, zone));
        return;
    }
    const QJsonObject payload =
        forkmesh::control::vultrMirrorDnsRecordPayload(hostname, ip);
    if (payload.isEmpty()) {
        skip(QStringLiteral("the instance address %1 is not a usable IPv4 "
                            "answer").arg(ip));
        return;
    }
    appendHostInstallLog(
        QString::fromUtf8("Pointing %1 at %2 in Cloudflare\xE2\x80\xA6\n")
            .arg(hostname, ip));
    cloudflareApiCall(
        apiToken,
        QStringLiteral("/zones?name=%1")
            .arg(QString::fromLatin1(QUrl::toPercentEncoding(zone))),
        QByteArrayLiteral("GET"), {},
        [this, apiToken, zone, hostname, payload, skip, onDone](
            QJsonObject result, QString error) {
            if (!error.isEmpty()) {
                skip(error);
                return;
            }
            const QString zoneId = forkmesh::control::cloudflareZoneId(
                result.value(QStringLiteral("result")).toArray(), zone);
            if (zoneId.isEmpty()) {
                skip(QStringLiteral(
                         "this API token does not see exactly one \"%1\" zone")
                         .arg(zone));
                return;
            }
            cloudflareApiCall(
                apiToken,
                QStringLiteral("/zones/%1/dns_records?type=A&name=%2")
                    .arg(zoneId,
                         QString::fromLatin1(
                             QUrl::toPercentEncoding(hostname))),
                QByteArrayLiteral("GET"), {},
                [this, apiToken, zoneId, hostname, payload, skip, onDone](
                    QJsonObject existing, QString listError) {
                    if (!listError.isEmpty()) {
                        skip(listError);
                        return;
                    }
                    const QString recordId =
                        forkmesh::control::cloudflareDnsRecordId(
                            existing.value(QStringLiteral("result")).toArray(),
                            hostname, QStringLiteral("A"));
                    const QString path =
                        recordId.isEmpty()
                            ? QStringLiteral("/zones/%1/dns_records")
                                  .arg(zoneId)
                            : QStringLiteral("/zones/%1/dns_records/%2")
                                  .arg(zoneId, recordId);
                    cloudflareApiCall(
                        apiToken, path,
                        recordId.isEmpty() ? QByteArrayLiteral("POST")
                                           : QByteArrayLiteral("PUT"),
                        payload,
                        [this, hostname, recordId, skip, onDone](
                            QJsonObject, QString writeError) {
                            if (!writeError.isEmpty()) {
                                skip(writeError);
                                return;
                            }
                            appendHostInstallLog(
                                QStringLiteral("Cloudflare DNS record %1 "
                                               "for %2.\n")
                                    .arg(recordId.isEmpty()
                                             ? QStringLiteral("created")
                                             : QStringLiteral("updated"),
                                         hostname));
                            onDone(hostname);
                        });
                });
        });
}

void MainWindow::ensureSharedHostKeypair(
    std::function<void(QString, QString, QString)> onDone)
{
    const QString keyPath = forkmesh::control::sharedHostKeyPath();
    if (keyPath.isEmpty()) {
        onDone({}, {}, QStringLiteral(
            "ForkMesh could not create its managed SSH key directory."));
        return;
    }
    const QString pubPath = keyPath + QStringLiteral(".pub");
    const auto readPublicKey = [pubPath]() {
        QFile pub(pubPath);
        if (!pub.open(QIODevice::ReadOnly))
            return QString();
        return QString::fromUtf8(pub.read(64 * 1024)).trimmed();
    };
    if (QFileInfo(keyPath).isFile()) {
        const QString publicKey = readPublicKey();
        if (!publicKey.isEmpty()) {
            onDone(keyPath, publicKey, {});
            return;
        }
    }
    appendHostInstallLog(QString::fromUtf8(
        "Generating the shared ForkMesh SSH key for this fleet\xE2\x80\xA6\n"));
    auto *keygen = new QProcess(this);
    keygen->setProcessChannelMode(QProcess::MergedChannels);
    connect(keygen, &QProcess::errorOccurred, this,
            [keygen, onDone](QProcess::ProcessError processError) {
                if (processError != QProcess::FailedToStart)
                    return;
                keygen->deleteLater();
                onDone({}, {}, QStringLiteral(
                    "Could not start ssh-keygen. Install OpenSSH and try "
                    "again."));
            });
    connect(keygen, &QProcess::finished, this,
            [keygen, keyPath, readPublicKey, onDone](
                int code, QProcess::ExitStatus exitStatus) {
                keygen->deleteLater();
                if (exitStatus != QProcess::NormalExit || code != 0 ||
                    !QFileInfo(keyPath).isFile()) {
                    onDone({}, {}, QStringLiteral(
                        "ssh-keygen could not create the managed key "
                        "(exit %1).").arg(code));
                    return;
                }
                QFile::setPermissions(
                    keyPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                const QString publicKey = readPublicKey();
                if (publicKey.isEmpty()) {
                    onDone({}, {}, QStringLiteral(
                        "The managed key was created but its public half "
                        "could not be read."));
                    return;
                }
                onDone(keyPath, publicKey, {});
            });
    keygen->start(QStringLiteral("ssh-keygen"),
                  {QStringLiteral("-q"), QStringLiteral("-t"),
                   QStringLiteral("ed25519"), QStringLiteral("-N"),
                   QString(), QStringLiteral("-C"),
                   QStringLiteral("forkmesh-shared-host-key"),
                   QStringLiteral("-f"), keyPath});
}

void MainWindow::resolveVultrSshKeyId(
    const QString &apiKey, const QString &publicKey,
    std::function<void(QString, QString)> onDone)
{
    const QStringList parts =
        publicKey.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const QString blob = parts.size() >= 2
                             ? parts.at(0) + QLatin1Char(' ') + parts.at(1)
                             : publicKey;
    vultrApiCall(
        apiKey, QStringLiteral("/v2/ssh-keys?per_page=500"),
        QByteArrayLiteral("GET"), {},
        [this, apiKey, publicKey, blob, onDone](QJsonObject result,
                                                QString error) {
            if (!error.isEmpty()) {
                onDone({}, error);
                return;
            }
            for (const QJsonValue &value :
                 result.value(QStringLiteral("ssh_keys")).toArray()) {
                const QJsonObject key = value.toObject();
                const QString id =
                    key.value(QStringLiteral("id")).toString();
                if (!id.isEmpty() &&
                    key.value(QStringLiteral("ssh_key"))
                        .toString()
                        .trimmed()
                        .startsWith(blob)) {
                    onDone(id, {});
                    return;
                }
            }
            const QJsonObject body{
                {QStringLiteral("name"),
                 QStringLiteral("forkmesh-shared-host-key")},
                {QStringLiteral("ssh_key"), publicKey},
            };
            vultrApiCall(
                apiKey, QStringLiteral("/v2/ssh-keys"),
                QByteArrayLiteral("POST"), body,
                [onDone](QJsonObject created, QString postError) {
                    if (!postError.isEmpty()) {
                        onDone({}, postError);
                        return;
                    }
                    const QString id =
                        created.value(QStringLiteral("ssh_key"))
                            .toObject()
                            .value(QStringLiteral("id"))
                            .toString();
                    if (id.isEmpty()) {
                        onDone({}, QStringLiteral(
                            "Vultr did not return an SSH key id."));
                        return;
                    }
                    onDone(id, {});
                });
        });
}

void MainWindow::createVultrMirrorFromForm()
{
    if (createVultrMirrorThroughCore())
        return;
    if (m_vultrResumeRequested) {
        resumeVultrProvision();
        return;
    }
    if (m_vultrProvisionActive) {
        if (m_vultrStatus)
            m_vultrStatus->setText(
                QStringLiteral("A Vultr mirror is already being created."));
        return;
    }
    if ((m_hostInstallProcess &&
         m_hostInstallProcess->state() != QProcess::NotRunning) ||
        m_hostDeployRemaining > 0) {
        if (m_vultrStatus)
            m_vultrStatus->setText(QStringLiteral(
                "Wait for the running install to finish first."));
        return;
    }
    QString apiKey =
        m_vultrApiKeyEdit ? m_vultrApiKeyEdit->text().trimmed() : QString();
    bool storedKey = false;
    if (apiKey.isEmpty()) {
        // Same convenience as the Cloudflare tooling: reuse a credential this
        // node already stores as a device-local Actions variable.
        apiKey = forkmesh::control::vultrApiKeyFromVariables(
            ActionStore::variables());
        storedKey = !apiKey.isEmpty();
    }
    QString node =
        m_vultrNameEdit ? m_vultrNameEdit->text().trimmed() : QString();
    QSettings settings;
    const QJsonArray hosts = forkmesh::control::loadSavedHosts(
        settings, kHostsSetting, &m_hostSessionPasswords);
    QSet<QString> used;
    QStringList knownNames;
    for (const QJsonValue &value : hosts) {
        const QString name = value.toObject()
                                 .value(QStringLiteral("name"))
                                 .toString()
                                 .toLower();
        used.insert(name);
        knownNames.append(name);
    }
    if (node.isEmpty()) {
        knownNames += m_profileLinkedNodes;
        node = forkmesh::control::nextMirrorNodeName(knownNames);
        if (node.isEmpty()) {
            if (m_vultrStatus)
                m_vultrStatus->setText(QStringLiteral(
                    "Could not pick a free mirror name — enter one."));
            return;
        }
    } else if (used.contains(node.toLower())) {
        if (m_vultrStatus)
            m_vultrStatus->setText(QStringLiteral(
                "A saved host named \"%1\" already exists — choose a "
                "different node name.").arg(node));
        return;
    }
    const QString invalid =
        forkmesh::control::validateVultrMirrorRequest(apiKey, node);
    if (!invalid.isEmpty()) {
        if (m_vultrStatus)
            m_vultrStatus->setText(invalid);
        return;
    }

    const QMap<QString, QString> deviceVariables = ActionStore::variables();
    QString cloudflareToken =
        forkmesh::control::cloudflareApiTokenFromVariables(deviceVariables);
    QSettings tunnelSettings;
    QString tunnelZone =
        tunnelSettings.value(QStringLiteral("control/cloudflareZone"))
            .toString()
            .trimmed();
    if (tunnelZone.isEmpty())
        tunnelZone =
            forkmesh::control::cloudflareZoneNameFromVariables(deviceVariables);
    const QString tunnelHostname =
        forkmesh::control::vultrMirrorDnsHostname(node, tunnelZone);
    if (cloudflareToken.isEmpty() ||
        cloudflareToken.contains(QLatin1Char('\n')) ||
        cloudflareToken.contains(QLatin1Char('\r')) ||
        tunnelHostname.isEmpty()) {
        if (m_vultrStatus)
            m_vultrStatus->setText(QStringLiteral(
                "A Cloudflare API token and valid zone are required so the "
                "new mirror can create its Tunnel and serve live. Save "
                "CLOUDFLARE_API_TOKEN plus CLOUDFLARE_ZONE in Settings first."));
        return;
    }
    QString cloudflareStoreError;
    const QStringList cloudflareStores =
        rememberCloudflareApiToken(cloudflareToken, &cloudflareStoreError);
    if (!cloudflareStores.isEmpty()) {
        appendHostInstallLog(
            QStringLiteral("Cloudflare credential kept in %1.\n")
                .arg(cloudflareStores.join(QStringLiteral(" and "))));
    }
    if (!cloudflareStoreError.isEmpty())
        appendHostInstallLog(cloudflareStoreError + QLatin1Char('\n'));

    const bool resumedPreInstance = m_vultrResumeChain;
    if (!resumedPreInstance)
        resetVultrStageDetails(); // a brand-new run starts every caption blank
    m_vultrProvisionActive = true;
    updateVultrEndDeploymentButtonVisibility();
    // Set this before the first transition so every stage ping identifies the
    // mirror it belongs to, including credentials and plan discovery.
    m_vultrProvisionNode = node;
    m_vultrPollCount = 0;
    m_vultrInstallAttempts = 0;
    m_vultrSshWaitCount = 0;
    m_vultrInstallUseLocalBinary =
         forkmesh::control::localBinaryRunsOnVultrMirror(
             QSysInfo::kernelType(), QSysInfo::currentCpuArchitecture()) &&
         QFileInfo(QCoreApplication::applicationFilePath()).isReadable();
    m_vultrInstallAgentClis =
        m_vultrAgentClisCheck && m_vultrAgentClisCheck->isChecked();
    m_vultrInstallAttemptLog.clear();
    m_vultrDnsHostname = tunnelHostname;
    m_vultrTunnelApiToken = cloudflareToken;
    if (!resumedPreInstance) {
        m_vultrHostMetadata = QJsonObject{
            {QStringLiteral("provider"), QStringLiteral("Vultr")},
            {QStringLiteral("displayName"), node},
            {QStringLiteral("dnsHostname"), tunnelHostname},
            {QStringLiteral("publicOrigin"),
             QStringLiteral("https://") + tunnelHostname},
        };
    }
    m_hostInstallAttemptBanner.clear();
    if (m_vultrCreateButton) {
        m_vultrCreateButton->setEnabled(false);
        m_vultrCreateButton->setText(QStringLiteral("Deployment running…"));
    }
    if (m_hostInstallLog && !resumedPreInstance) {
        m_hostInstallLog->clear();
        m_hostInstallLogCarry.clear();
        m_hostInstallLogFg = -1;
        m_hostInstallLogBold = false;
        saveVultrProvisionLog();
    }
    appendHostInstallLog(timestampedHostInstallEvent(QString::fromUtf8(
        "Creating Vultr mirror \"%1\" \xE2\x80\x94 cheapest supported US plan, latest "
        "Debian, managed SSH key\xE2\x80\xA6\n").arg(node)));
    if (storedKey)
        appendHostInstallLog(QStringLiteral(
            "Using the VULTR_API_KEY stored as a device variable.\n"));
    if (m_vultrInstallUseLocalBinary)
        appendHostInstallLog(QString::fromUtf8(
            "This app's own binary will be uploaded over SSH, so the new "
            "mirror needs no published release to install.\n"));
    if (m_vultrInstallAgentClis) {
        const QString agentAccess = forkmesh::control::describeAgentCliCredentials(
            localAgentCliCredentials());
        appendHostInstallLog(
            agentAccess.isEmpty()
                ? QString::fromUtf8(
                      "Claude Code and Codex will be installed, but this device "
                      "has no provider login to copy \xE2\x80\x94 the mirror "
                      "will need its own sign-in.\n")
                : QStringLiteral(
                      "Claude Code and Codex will be installed and signed in "
                      "with this device's access (%1).\n").arg(agentAccess));
    }
    setVultrProvisionStage(
        1, resumedPreInstance
               ? QStringLiteral("Resuming credentials and shared SSH key…")
               : QStringLiteral("Preparing credentials and shared SSH key…"));

    ensureSharedHostKeypair([this, apiKey, node](QString keyPath,
                                                 QString publicKey,
                                                 QString keyError) {
        if (!keyError.isEmpty()) {
            finishVultrProvision(false, keyError);
            return;
        }
        m_vultrIdentityFile = keyPath;
        persistVultrProvisionState();
        appendHostInstallLog(timestampedHostInstallEvent(
            QStringLiteral("Shared ForkMesh SSH key: %1\n").arg(keyPath)));
        if (m_vultrStatus)
            m_vultrStatus->setText(QString::fromUtf8(
                "Registering the SSH key with Vultr\xE2\x80\xA6"));
        resolveVultrSshKeyId(apiKey, publicKey, [this, apiKey, node, keyPath](
                                                    QString sshKeyId,
                                                    QString sshError) {
            if (!sshError.isEmpty()) {
                finishVultrProvision(false, sshError);
                return;
            }
            setVultrProvisionStage(
                2, QStringLiteral("Choosing a supported plan and Debian image…"));
            vultrApiCall(
                apiKey, QStringLiteral("/v2/plans?per_page=500"),
                QByteArrayLiteral("GET"), {},
                [this, apiKey, node, keyPath, sshKeyId](
                    QJsonObject plansResult, QString planError) {
                    if (!planError.isEmpty()) {
                        finishVultrProvision(false, planError);
                        return;
                    }
                    // The cheapest supported plan in the preferred region,
                    // unless the owner's phone picked one of the same choices
                    // (hosts.run "install"); consumed by this one create.
                    QString region;
                    QString choiceError;
                    const QString requestedPlan =
                        std::exchange(m_vultrRequestedPlan, QString());
                    const QString requestedRegion =
                        std::exchange(m_vultrRequestedRegion, QString());
                    const QJsonObject plan =
                        forkmesh::control::chooseVultrPlan(
                            plansResult.value(QStringLiteral("plans"))
                                .toArray(),
                            requestedPlan, requestedRegion, &region,
                            &choiceError);
                    if (plan.isEmpty() || region.isEmpty()) {
                        finishVultrProvision(false,
                            !requestedPlan.isEmpty() || !requestedRegion.isEmpty()
                                ? choiceError
                                : QStringLiteral(
                            "No deployable US plan is available on this Vultr "
                            "account (New Jersey, Atlanta, or another supported "
                            "US region)."));
                        return;
                    }
                    m_vultrHostMetadata.insert(
                        QStringLiteral("planType"),
                        plan.value(QStringLiteral("id")));
                    m_vultrHostMetadata.insert(
                        QStringLiteral("region"), region);
                    m_vultrHostMetadata.insert(
                        QStringLiteral("monthlyCost"),
                        plan.value(QStringLiteral("monthly_cost")));
                    m_vultrHostMetadata.insert(
                        QStringLiteral("ramMb"),
                        plan.value(QStringLiteral("ram")));
                    m_vultrHostMetadata.insert(
                        QStringLiteral("diskGb"),
                        plan.value(QStringLiteral("disk")));
                    appendHostInstallLog(timestampedHostInstallEvent(
                        QStringLiteral(
                            "Cheapest supported plan: %1 ($%2/month, %3 MB RAM, %4 GB "
                            "disk) in region %5\n")
                            .arg(plan.value(QStringLiteral("id")).toString())
                            .arg(plan.value(QStringLiteral("monthly_cost"))
                                     .toDouble())
                            .arg(plan.value(QStringLiteral("ram")).toInt())
                            .arg(plan.value(QStringLiteral("disk")).toInt())
                            .arg(region)));
                    vultrApiCall(
                        apiKey, QStringLiteral("/v2/os?per_page=500"),
                        QByteArrayLiteral("GET"), {},
                        [this, apiKey, node, keyPath, sshKeyId, plan, region](
                            QJsonObject osResult, QString osError) {
                            if (!osError.isEmpty()) {
                                finishVultrProvision(false, osError);
                                return;
                            }
                            const QJsonObject debian =
                                forkmesh::control::latestVultrDebianOs(
                                    osResult.value(QStringLiteral("os"))
                                        .toArray());
                            if (debian.isEmpty()) {
                                finishVultrProvision(false, QStringLiteral(
                                    "Vultr offers no Debian x64 image right "
                                    "now."));
                                return;
                            }
                            appendHostInstallLog(timestampedHostInstallEvent(
                                QStringLiteral("Operating system: %1\n")
                                    .arg(debian.value(QStringLiteral("name"))
                                             .toString())));
                            setVultrProvisionStage(
                                3, QStringLiteral(
                                       "Checking for an existing server before creation…"));
                            const QJsonObject payload =
                                forkmesh::control::vultrInstanceCreatePayload(
                                    node,
                                    plan.value(QStringLiteral("id"))
                                        .toString(),
                                    region,
                                    debian.value(QStringLiteral("id"))
                                        .toInt(),
                                    sshKeyId);
                            findVultrProvisionInstance(
                                apiKey, node,
                                [this, apiKey, node, keyPath, payload](
                                    QString existingId, QString findError) {
                                    if (!findError.isEmpty()) {
                                        finishVultrProvision(false, findError);
                                        return;
                                    }
                                    const auto continueWithInstance =
                                        [this, apiKey, node, keyPath](
                                            const QString &instanceId,
                                            bool reused) {
                                            if (instanceId.isEmpty()) {
                                                finishVultrProvision(
                                                    false,
                                                    QStringLiteral(
                                                        "Vultr did not return an "
                                                        "instance id."));
                                                return;
                                            }
                                            m_vultrInstanceId = instanceId;
                                            m_vultrIdentityFile = keyPath;
                                            m_vultrHostMetadata.insert(
                                                QStringLiteral("instanceId"),
                                                instanceId);
                                            appendHostInstallLog(
                                                timestampedHostInstallEvent(
                                                    reused
                                                        ? QString::fromUtf8(
                                                              "Found existing instance %1 for %2; resuming instead of creating a duplicate.\n")
                                                              .arg(instanceId, node)
                                                        : QString::fromUtf8(
                                                              "Instance %1 created \xE2\x80\x94 waiting for it to boot\xE2\x80\xA6\n")
                                                              .arg(instanceId)));
                                            m_vultrResumeChain = false;
                                            setVultrProvisionStage(
                                                4, QStringLiteral(
                                                       "Waiting for the server to boot and accept SSH…"));
                                            pollVultrInstance(
                                                apiKey, instanceId, node,
                                                keyPath);
                                        };
                                    if (!existingId.isEmpty()) {
                                        continueWithInstance(existingId, true);
                                        return;
                                    }
                                    setVultrProvisionStage(
                                        3, QStringLiteral("Creating the Vultr server…"));
                                    vultrApiCall(
                                        apiKey,
                                        QStringLiteral("/v2/instances"),
                                        QByteArrayLiteral("POST"), payload,
                                        [this, continueWithInstance](
                                            QJsonObject createResult,
                                            QString createError) {
                                            if (!createError.isEmpty()) {
                                                finishVultrProvision(
                                                    false, createError);
                                                return;
                                            }
                                            continueWithInstance(
                                                createResult
                                                    .value(QStringLiteral("instance"))
                                                    .toObject()
                                                    .value(QStringLiteral("id"))
                                                    .toString(),
                                                false);
                                        });
                                });
                        });
                });
        });
    });
}

void MainWindow::pollVultrInstance(const QString &apiKey,
                                   const QString &instanceId,
                                   const QString &node,
                                   const QString &identityFile)
{
    constexpr int kMaxPolls = 60; // ~10 minutes at one poll every 10 s
    if (!m_vultrProvisionActive)
        return;
    vultrApiCall(
        apiKey, QStringLiteral("/v2/instances/") + instanceId,
        QByteArrayLiteral("GET"), {},
        [this, apiKey, instanceId, node, identityFile](QJsonObject result,
                                                       QString error) {
            if (!m_vultrProvisionActive)
                return;
            if (!error.isEmpty()) {
                appendHostInstallLog(timestampedHostInstallEvent(
                    QStringLiteral("Server status check failed: %1; retrying.\n")
                        .arg(error)));
                setVultrProvisionStage(
                    4, QStringLiteral(
                           "Vultr status check was interrupted; retrying automatically…"));
                QTimer::singleShot(
                    10000, this,
                    [this, apiKey, instanceId, node, identityFile] {
                        pollVultrInstance(apiKey, instanceId, node,
                                          identityFile);
                    });
                return;
            }
            const QJsonObject instance =
                result.value(QStringLiteral("instance")).toObject();
            const QString ip =
                forkmesh::control::vultrInstanceReadyIp(instance);
            if (ip.isEmpty()) {
                if (forkmesh::control::vultrInstanceIsIpv6Only(instance)) {
                    finishVultrProvision(false, QStringLiteral(
                        "Vultr gave this instance an IPv6 address only, which "
                        "the mesh cannot reach. Destroy instance %1 in the "
                        "Vultr panel and retry — ForkMesh only deploys IPv4 "
                        "plans.").arg(instanceId));
                    return;
                }
                if (++m_vultrPollCount >= kMaxPolls) {
                    finishVultrProvision(false, QStringLiteral(
                        "The instance did not become ready in time. Check "
                        "it in the Vultr panel, then Add host + Install "
                        "ForkMesh manually once it is up."));
                    return;
                }
                setVultrProvisionStage(
                    4,
                    QString::fromUtf8(
                        "Waiting for the server to boot (status: %1, check %2/%3)…")
                        .arg(instance.value(QStringLiteral("status"))
                                 .toString())
                        .arg(m_vultrPollCount + 1)
                        .arg(kMaxPolls));
                QTimer::singleShot(
                    10000, this,
                    [this, apiKey, instanceId, node, identityFile] {
                        pollVultrInstance(apiKey, instanceId, node,
                                          identityFile);
                    });
                return;
            }
            appendHostInstallLog(timestampedHostInstallEvent(
                QStringLiteral("Instance is up at %1.\n").arg(ip)));
            m_vultrInstanceId = instanceId;
            m_vultrInstanceIp = ip;
            m_vultrIdentityFile = identityFile;
            // Persist the host with its managed key path before the install
            // so every later SSH action (install, logs, uninstall, Actions)
            // authenticates with that key. Vultr Debian images boot as root.
            rememberHost(node, ip, QStringLiteral("root"), QString(),
                         QStringLiteral("vultr booting"), identityFile,
                         m_vultrHostMetadata);
            m_vultrSshWaitCount = 0;
            waitForVultrSshReady(node, ip, identityFile);
        });
}

void MainWindow::waitForVultrSshReady(const QString &node, const QString &ip,
                                      const QString &identityFile)
{
    constexpr int kMaxSshProbes = 20;
    constexpr int kSshProbeIntervalMs = 60000;
    if (!m_vultrProvisionActive)
        return;
    if (m_vultrSshProbeProcess &&
        m_vultrSshProbeProcess->state() != QProcess::NotRunning)
        return;

    const QString unroutable = forkmesh::control::nonRoutableAddressNote(ip);
    if (!unroutable.isEmpty()) {
        finishVultrProvision(false, QString::fromUtf8(
            "%1 is in %2, so SSH from this machine can never reach it. Give "
            "the instance a public address (or run the install from the "
            "network that owns that range); it is saved under Hosts "
            "\xE2\x80\x94 fix the address there and click Update.")
            .arg(ip, unroutable));
        return;
    }

    QString sshError;
    const forkmesh::control::HostSshCommand ssh =
        forkmesh::control::buildHostSshCommand(
            ip, QStringLiteral("root"), QString(),
            forkmesh::control::vultrSshProbeRemoteCommand(), &sshError,
            identityFile);
    if (ssh.program.isEmpty()) {
        finishVultrProvision(false, sshError);
        return;
    }

    ++m_vultrSshWaitCount;
    if (m_vultrStatus)
        m_vultrStatus->setText(
            QString::fromUtf8("Waiting for SSH on %1 (knock %2 of %3)"
                              "\xE2\x80\xA6")
                .arg(ip)
                .arg(m_vultrSshWaitCount)
                .arg(kMaxSshProbes));

    auto *probe = new QProcess(this);
    m_vultrSshProbeProcess = probe;
    probe->setProcessChannelMode(QProcess::MergedChannels);
    probe->setProcessEnvironment(ssh.environment);
    auto output = std::make_shared<QByteArray>();
    auto started = std::make_shared<QElapsedTimer>();
    started->start();
    connect(probe, &QProcess::readyReadStandardOutput, probe,
            [probe, output] { output->append(probe->readAllStandardOutput()); });
    connect(probe, &QProcess::errorOccurred, this,
            [this, probe, program = ssh.program](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                if (m_vultrSshProbeProcess == probe)
                    m_vultrSshProbeProcess = nullptr;
                probe->disconnect(this);
                probe->deleteLater();
                if (!m_vultrProvisionActive)
                    return;
                finishVultrProvision(false, QStringLiteral(
                    "\"%1\" could not be started on this machine, so the new "
                    "instance cannot be reached. Install it and click Update "
                    "on the host saved under Hosts.").arg(program));
            });
    connect(probe, &QProcess::finished, this,
            [this, probe, output, started, node, ip, identityFile](
                int code, QProcess::ExitStatus status) {
                if (m_vultrSshProbeProcess == probe)
                    m_vultrSshProbeProcess = nullptr;
                output->append(probe->readAllStandardOutput());
                probe->deleteLater();
                if (!m_vultrProvisionActive)
                    return;
                const int exitCode =
                    status == QProcess::NormalExit ? code : 255;
                const QString tail = forkmesh::control::redactProcessOutput(
                    QString::fromUtf8(output->right(2048)));
                if (forkmesh::control::vultrSshProbeReady(exitCode, tail)) {
                    appendHostInstallLog(timestampedHostInstallEvent(
                        QStringLiteral("SSH answered on %1 after %2 %3.\n")
                            .arg(ip)
                            .arg(m_vultrSshWaitCount)
                            .arg(m_vultrSshWaitCount == 1
                                     ? QStringLiteral("knock")
                                     : QStringLiteral("knocks"))));
                    startVultrHostInstall(node, ip, identityFile);
                    return;
                }
                if (m_vultrSshWaitCount >= 3 &&
                    forkmesh::control::sshFailureNeedsPassword(exitCode,
                                                               tail)) {
                    appendHostInstallLog(
                        QStringLiteral("SSH probe: %1\n")
                            .arg(forkmesh::control::sshFailureSummary(exitCode,
                                                                      tail)));
                    finishVultrProvision(false, QString::fromUtf8(
                        "The new instance refused the managed SSH key. It is "
                        "saved under Hosts \xE2\x80\x94 fix its credentials "
                        "there and click Update to finish the install."));
                    return;
                }
                if (m_vultrSshWaitCount >= kMaxSshProbes) {
                    appendHostInstallLog(
                        QStringLiteral("SSH probe: %1\n")
                            .arg(forkmesh::control::sshFailureSummary(exitCode,
                                                                      tail)));
                    finishVultrProvision(false, QString::fromUtf8(
                        "SSH on %1 never answered (%2 attempts over %3 "
                        "minutes). The instance is saved under Hosts "
                        "\xE2\x80\x94 click Update there once it is "
                        "reachable.")
                        .arg(ip)
                        .arg(kMaxSshProbes)
                        .arg(kMaxSshProbes));
                    return;
                }
                const qint64 elapsed = started->elapsed();
                const int wait = static_cast<int>(qBound<qint64>(
                    0, qint64(kSshProbeIntervalMs) - elapsed,
                    qint64(kSshProbeIntervalMs)));
                QTimer::singleShot(
                    wait, this, [this, node, ip, identityFile] {
                        waitForVultrSshReady(node, ip, identityFile);
                    });
            });
    probe->start(ssh.program, ssh.arguments);
}

void MainWindow::appendVultrAttemptHistory()
{
    if (m_vultrInstallAttemptLog.isEmpty())
        return;
    QString block = QString::fromUtf8("\nInstall attempts this run:\n");
    for (const QString &entry : std::as_const(m_vultrInstallAttemptLog))
        block += QString::fromUtf8("  \xE2\x80\xA2 %1\n").arg(entry);
    appendHostInstallLog(block);
}

void MainWindow::startVultrHostInstall(const QString &node, const QString &ip,
                                       const QString &identityFile)
{
    constexpr int kMaxInstallAttempts = 1;
    if (!m_vultrProvisionActive)
        return;
    if (m_hostIpEdit)
        m_hostIpEdit->setText(ip);
    if (m_hostUserEdit)
        m_hostUserEdit->setText(QStringLiteral("root"));
    if (m_hostPassEdit)
        m_hostPassEdit->clear();
    if (m_hostNameEdit)
        m_hostNameEdit->setText(node);
    if (m_hostUploadBinaryCheck)
        m_hostUploadBinaryCheck->setChecked(false);
    setVultrProvisionStage(
        5, QStringLiteral("SSH is ready — installing ForkMesh…"));
    ++m_vultrInstallAttempts;
    m_vultrProvisionNode = node;
    m_vultrInstanceIp = ip;
    m_vultrIdentityFile = identityFile;
    const QString attemptStartedAt = hostInstallEventTimestamp();
    const QString attemptLabel =
        QStringLiteral("Attempt %1 of %2 started at %3")
            .arg(QString::number(m_vultrInstallAttempts),
                 QString::number(kMaxInstallAttempts),
                 attemptStartedAt);
    auto attemptTimer = std::make_shared<QElapsedTimer>();
    attemptTimer->start();
    m_hostInstallAttemptBanner = attemptLabel;
    if (m_vultrStatus)
        m_vultrStatus->setText(
            QString::fromUtf8(
                "Installing the Go mirror server (attempt %1 of %2) \xE2\x80\x94 "
                "no Qt or GTK packages\xE2\x80\xA6")
                .arg(m_vultrInstallAttempts)
                .arg(kMaxInstallAttempts));
    const bool isFinalAttempt = m_vultrInstallAttempts >= kMaxInstallAttempts;
    runHostInstall(m_vultrInstallUseLocalBinary, [this, node, ip, identityFile,
                           attemptLabel, attemptTimer, isFinalAttempt](bool ok) {
        if (!m_vultrProvisionActive)
            return;
        const QString attemptFinishedAt = hostInstallEventTimestamp();
        const QString attemptElapsed =
            conciseElapsedTime(attemptTimer->elapsed());
        m_vultrInstallAttemptLog.append(
            QString::fromUtf8("%1 \xE2\x80\x94 finished at %2 after %3 \xE2\x80\x94 %4")
                .arg(attemptLabel, attemptFinishedAt, attemptElapsed,
                     ok ? QStringLiteral("installed")
                        : (m_hostInstallLastFailure.isEmpty()
                               ? QStringLiteral("failed")
                               : m_hostInstallLastFailure)));
        if (ok) {
            appendVultrAttemptHistory();
            const QString address =
                m_vultrDnsHostname.isEmpty()
                    ? ip
                    : QStringLiteral("%1, %2").arg(m_vultrDnsHostname, ip);
            const QString done = QString::fromUtf8(
                "Vultr mirror \"%1\" (%2) is installed, linked, healthy, and "
                "serving repository traffic.").arg(node, address);
            if (!m_vultrInstallAgentClis) {
                seedVultrMirrorRepositoryThenVerify(node, done);
                return;
            }
            // The node is up and authenticated to the mesh; give it this
            // device's agent access too so it can run sessions immediately
            // A failure here does not undo the mirror itself.
            // With no login to copy the CLIs are still installed, so the
            // mirror only needs its own sign-in rather than everything.
            const bool copyLogins =
                !forkmesh::control::agentCliCredentialsAreEmpty(
                    localAgentCliCredentials());
            setVultrProvisionStage(
                5, QStringLiteral(
                       "ForkMesh installed; setting up Claude Code and Codex…"));
            runAgentCliInstall(
                node, ip, QStringLiteral("root"), QString(), identityFile,
                copyLogins,
                [this, node, done](bool agentOk, QString agentMessage) {
                    if (!m_vultrProvisionActive)
                        return;
                    seedVultrMirrorRepositoryThenVerify(
                        node,
                        agentOk
                            ? QStringLiteral("%1 %2").arg(done, agentMessage)
                            : QString::fromUtf8(
                                  "%1 The agent CLIs were not set up: %2")
                                  .arg(done, agentMessage));
                });
            return;
        }
        const QString unroutable = forkmesh::control::nonRoutableAddressNote(ip);
        if (!unroutable.isEmpty()) {
            appendVultrAttemptHistory();
            finishVultrProvision(false, QString::fromUtf8(
                "%1 is in %2, so SSH from this machine can never reach it. "
                "Give the instance a public address (or run the install from "
                "the network that owns that range); it is saved under Hosts "
                "\xE2\x80\x94 fix the address there and click Update.")
                .arg(ip, unroutable));
            return;
        }
        appendVultrAttemptHistory();
        finishVultrProvision(false, QString::fromUtf8(
            "Install did not succeed. The instance is saved under Hosts "
            "\xE2\x80\x94 the transcript above shows why; click Update there "
            "to retry."));
    }, /*reinstall=*/false, /*fromSource=*/false,
    /*suppressFailureStatus=*/!isFinalAttempt);
}
