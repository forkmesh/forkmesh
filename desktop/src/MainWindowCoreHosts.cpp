#include "ActionStore.h"
#include "ControlNode.h"
#include "CoreClient.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "MirrorGatewayHealth.h"
#include "Tooltips.h"

#include <QColor>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QSet>
#include <QTableWidget>
#include <QTimer>

using namespace forkmesh::ui;

namespace {

constexpr int kCoreHostLineFlushMs = 100;
constexpr int kCoreHostLineBatch = 400;
constexpr int kCoreExecGraceMs = 15000;

QJsonArray &pendingCoreHostLines()
{
    static QJsonArray lines;
    return lines;
}

bool &coreHostLineFlushScheduled()
{
    static bool scheduled = false;
    return scheduled;
}

QSet<QString> &coreHostOperationsSeenRunning()
{
    static QSet<QString> ids;
    return ids;
}

QString operationLine(const QJsonObject &op)
{
    const QString what = op.value(QStringLiteral("action")).toString().isEmpty()
                             ? op.value(QStringLiteral("kind")).toString()
                             : op.value(QStringLiteral("action")).toString();
    return QStringLiteral("Host %1 on \"%2\" (%3)")
        .arg(what, op.value(QStringLiteral("host")).toString(),
             op.value(QStringLiteral("origin")).toString());
}

}

bool MainWindow::coreHostsAttached() const
{
    return m_coreClient && m_coreClient->isConnected() &&
           m_coreClient->hasNativeRoute(QStringLiteral("core.hosts.exec"));
}

QJsonObject MainWindow::coreHostsContext()
{
    QJsonArray roster;
    for (const MemberInfo &member : std::as_const(m_homeRoster))
        if (!member.nodeName.isEmpty())
            roster.append(QJsonObject{{QStringLiteral("name"), member.nodeName},
                                      {QStringLiteral("version"), member.version},
                                      {QStringLiteral("online"), member.online}});
    const bool editKey =
        m_vultrApiKeyEdit && !m_vultrApiKeyEdit->text().trimmed().isEmpty();
    const bool vultrKey =
        editKey ||
        !forkmesh::control::vultrApiKeyFromVariables(ActionStore::variables()).isEmpty();
    const bool busy =
        m_vultrProvisionActive ||
        (m_hostInstallProcess && m_hostInstallProcess->state() != QProcess::NotRunning) ||
        m_hostDeployRemaining > 0;
    QJsonObject probes;
    for (auto it = m_hostReachability.cbegin(); it != m_hostReachability.cend(); ++it)
        probes.insert(it.key(), it.value());
    QJsonArray repos;
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        if (repos.size() >= 300)
            break;
        if (repo.owner.isEmpty() || repo.name.isEmpty() || repo.localPath.isEmpty())
            continue;
        const QString key = repo.owner + QLatin1Char('/') + repo.name;
        repos.append(QJsonObject{
            {QStringLiteral("repo"), key},
            {QStringLiteral("private"), repo.isPrivate},
            {QStringLiteral("published"), repo.publishToNetwork},
            {QStringLiteral("pushing"), m_sshMirrorPushing.contains(key) ||
                                            m_sshMirrorPushing.contains(key.toLower())}});
    }
    return QJsonObject{
        {QStringLiteral("desktop"), machineNodeName()},
        {QStringLiteral("roster"), roster},
        {QStringLiteral("canCreate"), vultrKey},
        {QStringLiteral("editKey"), editKey},
        {QStringLiteral("busy"), busy},
        {QStringLiteral("provisioning"),
         m_vultrProvisionActive ? m_vultrProvisionNode : QString()},
        {QStringLiteral("installStatus"),
         m_hostInstallStatus ? m_hostInstallStatus->text() : QString()},
        {QStringLiteral("linkedNodes"), QJsonArray::fromStringList(m_profileLinkedNodes)},
        {QStringLiteral("probes"), probes},
        {QStringLiteral("provision"), localControlProvisionStatus()},
        {QStringLiteral("mirrorClient"),
         QJsonObject{{QStringLiteral("status"),
                      m_controlNodeStatus ? m_controlNodeStatus->text() : QString()},
                     {QStringLiteral("health"),
                      m_controlNodeHealth ? m_controlNodeHealth->text() : QString()}}},
        {QStringLiteral("mirrorPush"),
         QJsonObject{{QStringLiteral("pushing"), int(m_sshMirrorPushing.size())},
                     {QStringLiteral("pending"), int(m_sshMirrorPushPending.size())}}},
        {QStringLiteral("deploying"), m_hostDeployRemaining},
        {QStringLiteral("admin"), m_isAdmin},
        {QStringLiteral("nodeDelete"),
         QJsonObject{{QStringLiteral("inProgress"), m_nodeDeleteInProgress},
                     {QStringLiteral("target"), m_nodeDeleteTarget}}},
        {QStringLiteral("fleetRunning"), QJsonArray::fromStringList(m_fleetControlRunning.values())},
        {QStringLiteral("gateways"), QJsonArray::fromStringList(m_sshMirrorHealth.persistedState())},
        {QStringLiteral("provisionLog"), vultrProvisionLogPath()},
        {QStringLiteral("repos"), repos},
    };
}

void MainWindow::registerCoreHostsHandlers()
{
    if (!m_coreClient)
        return;
    registerCoreHostJobHandlers();
    m_coreClient->handle(QStringLiteral("ui.hosts.context"),
                         [this](const QJsonObject &, const CoreClient::Responder &respond) {
                             respond(coreHostsContext(), QString());
                         });
    connect(m_coreClient, &CoreClient::connectedChanged, this, [this](bool connected) {
        if (!connected || !coreHostsAttached())
            return;
        m_coreClient->request(
            QStringLiteral("core.hosts.operations"), {},
            [this](bool ok, const QJsonValue &result, const QString &) {
                if (!ok)
                    return;
                QStringList running;
                for (const QJsonValue &value :
                     result.toObject().value(QStringLiteral("operations")).toArray()) {
                    const QJsonObject op = value.toObject();
                    if (!op.value(QStringLiteral("running")).toBool())
                        continue;
                    coreHostOperationsSeenRunning().insert(op.value(QStringLiteral("id")).toString());
                    running.append(operationLine(op));
                }
                if (!running.isEmpty())
                    logSystem(QStringLiteral("Still running in the core: %1.")
                                  .arg(running.join(QStringLiteral("; "))));
            });
    });
    connect(m_coreClient, &CoreClient::coreEvent, this,
            [this](const QString &event, const QJsonObject &data) {
                if (event != QLatin1String("core.hosts.operation") ||
                    data.value(QStringLiteral("running")).toBool())
                    return;
                if (!coreHostOperationsSeenRunning().remove(data.value(QStringLiteral("id")).toString()))
                    return;
                logSystem(QStringLiteral("%1 %2 in the core: %3")
                              .arg(operationLine(data),
                                   data.value(QStringLiteral("ok")).toBool()
                                       ? QStringLiteral("finished")
                                       : QStringLiteral("failed"),
                                   data.value(QStringLiteral("output")).toString().left(300)));
            });
}

void MainWindow::forwardHostLogLineToCore(const QString &host, const QString &source,
                                          const QString &text)
{
    if (!coreHostsAttached())
        return;
    QJsonArray &lines = pendingCoreHostLines();
    lines.append(QJsonObject{{QStringLiteral("node"), host},
                             {QStringLiteral("src"), source},
                             {QStringLiteral("text"), text}});
    while (lines.size() > kCoreHostLineBatch)
        lines.removeFirst();
    if (coreHostLineFlushScheduled())
        return;
    coreHostLineFlushScheduled() = true;
    QTimer::singleShot(kCoreHostLineFlushMs, this, [this] {
        coreHostLineFlushScheduled() = false;
        QJsonArray batch;
        batch.swap(pendingCoreHostLines());
        if (!batch.isEmpty() && m_coreClient)
            m_coreClient->notify(QStringLiteral("core.hosts.lines"),
                                 {{QStringLiteral("lines"), batch}});
    });
}

bool MainWindow::probeSavedHostThroughCore(
    const QString &key, const QString &name, const QString &ip, const QString &user,
    const QString &savedStatus, const QString &password, const QString &identityFile,
    const std::function<void(const QString &, const QString &, const QString &,
                             const QString &)> &render)
{
    if (!coreHostsAttached())
        return false;
    m_coreClient->request(
        QStringLiteral("core.hosts.probe"),
        {{QStringLiteral("name"), name},
         {QStringLiteral("ip"), ip},
         {QStringLiteral("user"), user},
         {QStringLiteral("status"), savedStatus},
         {QStringLiteral("password"), password},
         {QStringLiteral("identityFile"), identityFile}},
        [this, key, name, render](bool ok, const QJsonValue &value, const QString &error) {
            m_hostProbesInFlight.remove(key);
            const QJsonObject probe = value.toObject();
            if (!ok) {
                render(QString::fromUtf8("\xE2\x97\x8F Attention \xC2\xB7 SSH unavailable"),
                       QString::fromUtf8("\xE2\x80\x94"), QString::fromUtf8("\xE2\x80\x94"),
                       QStringLiteral("#cf222e"));
                logSystem(QStringLiteral("Host probe of \"%1\" failed in the core: %2.")
                              .arg(name, error));
                return;
            }
            render(probe.value(QStringLiteral("status")).toString(),
                   probe.value(QStringLiteral("claude")).toString(),
                   probe.value(QStringLiteral("codex")).toString(),
                   probe.value(QStringLiteral("color")).toString());
            if (!m_hostsTable)
                return;
            int row = -1;
            for (int i = 0; i < m_hostsTable->rowCount(); ++i) {
                const QTableWidgetItem *item = m_hostsTable->item(i, 0);
                if (item && item->data(Qt::UserRole).toString() == key) {
                    row = i;
                    break;
                }
            }
            if (row < 0)
                return;
            const QString claudeColor = probe.value(QStringLiteral("claudeColor")).toString();
            const QString codexColor = probe.value(QStringLiteral("codexColor")).toString();
            if (QTableWidgetItem *item = m_hostsTable->item(row, 4); item && !claudeColor.isEmpty())
                item->setForeground(QColor(claudeColor));
            if (QTableWidgetItem *item = m_hostsTable->item(row, 5); item && !codexColor.isEmpty())
                item->setForeground(QColor(codexColor));
            const QString tooltip = probe.value(QStringLiteral("tooltip")).toString();
            const QString tooltipKind = probe.value(QStringLiteral("tooltipKind")).toString();
            if (!tooltipKind.isEmpty())
                if (QTableWidgetItem *item = m_hostsTable->item(row, 3))
                    item->setToolTip(tooltipKind == QLatin1String("raw") ? tooltip
                                                                         : Tooltips::preview(tooltip));
        },
        40000);
    return true;
}

bool MainWindow::runMirrorNodeSshThroughCore(const QString &node, const QString &remoteCommand,
                                             int timeoutMs,
                                             const std::function<void(bool, QString)> &onDone,
                                             bool preferProvisioningHost)
{
    if (!coreHostsAttached())
        return false;
    m_coreClient->request(
        QStringLiteral("core.hosts.exec"),
        {{QStringLiteral("node"), node},
         {QStringLiteral("command"), remoteCommand},
         {QStringLiteral("timeoutMs"), timeoutMs},
         {QStringLiteral("ip"),
          preferProvisioningHost ? m_vultrInstanceIp.trimmed() : QString()},
         {QStringLiteral("identityFile"),
          preferProvisioningHost ? m_vultrIdentityFile.trimmed() : QString()}},
        [onDone](bool ok, const QJsonValue &value, const QString &) {
            const QJsonObject result = value.toObject();
            onDone(ok && result.value(QStringLiteral("ok")).toBool(),
                   ok ? result.value(QStringLiteral("output")).toString() : QString());
        },
        timeoutMs + kCoreExecGraceMs);
    return true;
}
