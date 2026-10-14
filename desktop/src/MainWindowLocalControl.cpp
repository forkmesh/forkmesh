// The phone on this network driving the desktop: agents, files and status
// over the local control channel (LocalControl.h), with no relay involved.

#include "AccountCapability.h"
#include "AgentLogText.h"
#include "AppPaths.h"
#include "ClaudePeerMessaging.h"
#include "TerminalHandoff.h"
#include "ClaudeSessionScan.h"
#include "DesktopBuildCommit.h"
#include "ForkMeshVersion.h"
#include "LocalControl.h"
#include "LocalControlAgentOps.h"
#include "LocalControlAttachments.h"
#include "LocalControlHandoff.h"
#include "LocalControlPromptImages.h"
#include "LocalControlTerminals.h"
#include "LocalMeshNode.h"
#include "MainWindow.h"
#include "MainWindowAgentsInternal.h"
#include "MachineAgentScan.h"
#include "MainWindowInternal.h"
#include "NodeEventSocket.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QRegularExpression>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>

using namespace forkmesh::ui;

namespace localcontrolattachments = forkmesh::localcontrol::attachments;
namespace agentops = forkmesh::localcontrol::agentops;

namespace {

QStringList localControlAttachmentPaths(const AgentStore &store, const AgentSession &session,
                                        const QStringList &known)
{
    QStringList paths;
    for (const QString &raw : store.attachmentPaths(session)) {
        const QString path = AgentPromptImages::resolve(raw);
        if (!path.isEmpty() && !paths.contains(path))
            paths.append(path);
    }
    for (const QString &path : known)
        if (!paths.contains(path))
            paths.append(path);
    return paths;
}

constexpr int kMaxListedSessions = 200;
constexpr int kMaxTranscriptChars = 64000;
// A transcript read that asks only for what is new since `since` (the Agent
// Watch follows a run this way) gets at most this much at once.
constexpr int kMaxTranscriptDeltaChars = 16000;
constexpr int kMaxPromptChars = 8000;
constexpr qint64 kMaxOutsideTranscriptBytes = 2 * 1024 * 1024;
constexpr int kMaxFollowsPerConnection = 8;
constexpr int kOutsideFollowWatchMs = 200;
constexpr int kAgentsBellSpacingMs = 300;

QString receivedFilesDirectory()
{
    // Files a person sends from their phone belong with their other
    // downloads, where they will look for them, not in the app's data.
    const QString downloads =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (!downloads.isEmpty())
        return QDir(downloads).filePath(QStringLiteral("ForkMesh from phone"));
    return forkmesh::paths::dataRoot() + QStringLiteral("/phone-files");
}

QString deviceLabel(const LocalControlServer::Device &device)
{
    QString label = device.name.isEmpty() ? QStringLiteral("Phone") : device.name;
    if (!device.user.isEmpty())
        label += QStringLiteral(" (%1)").arg(device.user);
    return label;
}

bool sessionActive(const AgentSession &session)
{
    return session.status == AgentStatus::Queued ||
           session.status == AgentStatus::Running ||
           session.status == AgentStatus::Waiting;
}

QJsonArray providerChoices()
{
    struct Choice {
        const char *id;
        const char *label;
    };
    static const Choice choices[] = {
        {"claude-code", "Claude Code"}, {"codex", "Codex"},
        {"claude-api", "Claude API"},   {"openai", "OpenAI"},
        {"cloudflare-ai", "Cloudflare AI"}, {"local-llama", "Local Llama"},
    };
    QJsonArray out;
    for (const Choice &choice : choices)
        out.append(QJsonObject{{QStringLiteral("id"), QLatin1String(choice.id)},
                               {QStringLiteral("label"),
                                QLatin1String(choice.label)}});
    return out;
}

QString localControlAgentMode(const QJsonObject &params)
{
    const QString wanted = params.value(QStringLiteral("mode")).toString().trimmed();
    if (wanted.isEmpty())
        return QString();
    const QStringList modes = {kClaudeAutoModeLabel, kAgentAskModeLabel,
                               QStringLiteral("Plan"), QStringLiteral("Edit")};
    for (const QString &mode : modes)
        if (mode.compare(wanted, Qt::CaseInsensitive) == 0)
            return mode;
    return QString();
}

QString localControlDefaultAgentMode(const QComboBox *selector)
{
    if (selector)
        return selector->currentText();
    return QSettings()
        .value(kAgentModeSetting,
               QSettings().value(kClaudeAutoModeSetting, true).toBool()
                   ? kClaudeAutoModeLabel
                   : kAgentAskModeLabel)
        .toString()
        .trimmed();
}

} // namespace

bool MainWindow::ensureLocalControlServer()
{
    if (m_localControl)
        return true;
    if (m_profileIdentity.publicKey().isEmpty())
        return false;
    m_localControl = new LocalControlServer(
        m_profileIdentity.publicKey(),
        [this](const QByteArray &data) {
            return m_profileIdentity.signData(data);
        },
        this);
    m_localControl->setDeviceStorePath(
        forkmesh::paths::dataRoot() +
        QStringLiteral("/local_control/paired-devices.json"));
    m_localControl->setReceiveDirectory(receivedFilesDirectory());
    m_localControl->onPairingRequested =
        [this](int connectionId, const LocalControlServer::Device &device,
               const QString &code) {
            showLocalControlPairingPrompt(connectionId, device.name,
                                          device.user, code);
        };
    m_localControl->accountDeviceCheck =
        [this](const LocalControlServer::Device &device,
               std::function<void(LocalControlServer::AccountCheck)> done) {
            checkLocalControlAccountDevice(
                device.key, [done](bool answered, bool owned) {
                    done(!answered ? LocalControlServer::AccountCheck::Unreachable
                         : owned   ? LocalControlServer::AccountCheck::Owned
                                   : LocalControlServer::AccountCheck::NotOwned);
                });
        };
    m_localControl->onPairingClosed = [this](int connectionId) {
        if (QPointer<QWidget> prompt = m_localControlPrompts.take(connectionId))
            prompt->close();
    };
    m_localControl->onRequest =
        [this](int connectionId, const LocalControlServer::Device &,
               int requestId, const QString &method,
               const QJsonObject &params) {
            handleLocalControlRequest(connectionId, requestId, method, params);
        };
    registerLocalControlFamilies();
    m_localControl->onDevicesChanged = [this] { refreshLocalControlTab(); };
    m_localControl->onConnectionClosed = [this](int connectionId) {
        closeLocalControlStreams(connectionId);
    };
    m_localControl->onFileReceived =
        [this](const LocalControlServer::Device &device, const QString &path) {
            logSystem(QStringLiteral("Received %1 from %2 over the local network.")
                          .arg(QDir::toNativeSeparators(path), deviceLabel(device)));
            flashMessage(QStringLiteral("Received %1 from %2.")
                             .arg(QFileInfo(path).fileName(), deviceLabel(device)));
        };
    // Away from home the same channel rides this desktop's relay socket; the
    // relay forwards sealed chunks and cannot read or forge them.
    m_localControl->setRelaySink(
        [this](const QJsonObject &frame) {
            return m_nodeEventSocket &&
                   m_nodeEventSocket->sendOwnerControlFrame(frame);
        },
        machineNodeName());
    m_localControl->setDesktopInfo(machineNodeName(), nodeOwnerDisplayName());
    return true;
}

void MainWindow::startLocalControl()
{
    if (!m_localMesh || !ensureLocalControlServer())
        return;
    m_localControl->setDesktopInfo(machineNodeName(), nodeOwnerDisplayName());
    m_localMesh->setControlHandler(
        [this](QTcpSocket *socket, const QByteArray &hello, const QByteArray &rest) {
            if (m_localControl)
                m_localControl->adopt(socket, hello, rest);
        });
    refreshLocalControlTab();
}

void MainWindow::onNodeControlFrame(const QJsonObject &frame)
{
    // Addressed by machine name: the relay hands a phone's frame to every node
    // socket of the account, and only this machine's name is ours.
    if (frame.value(QStringLiteral("node")).toString().trimmed().toLower() !=
        machineNodeName().trimmed().toLower())
        return;
    if (!ensureLocalControlServer())
        return;
    m_localControl->setDesktopInfo(machineNodeName(), nodeOwnerDisplayName());
    m_localControl->relayFrame(frame);
}

void MainWindow::onNodeControlRelayRestored()
{
    if (m_localControl)
        m_localControl->relayRestored();
}

void MainWindow::checkLocalControlAccountDevice(
    const QString &key,
    const std::function<void(bool answered, bool owned)> &done)
{
    const QUrl url = accountsApiUrl(QStringLiteral("devices/check"));
    const QString token = accountSessionTokenForUrl(url);
    const QString account = AccountCapability::normalizedAccount(accountOwner());
    if (!m_networkAccess || account.isEmpty()) {
        done(false, false);
        return;
    }
    QJsonObject body{{QStringLiteral("pubkey"), key}};
    if (!token.isEmpty()) {
        body.insert(QStringLiteral("sessionToken"), token);
    } else {
        QString node, ts, sig;
        if (!signOrgTaskProof(QStringLiteral("forkmesh-device-check-v1"), key,
                              &node, &ts, &sig)) {
            done(false, false);
            return;
        }
        body.insert(QStringLiteral("node"), node);
        body.insert(QStringLiteral("ts"), ts);
        body.insert(QStringLiteral("sig"), sig);
    }
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setRawHeader("Accept", "application/json");
    request.setTransferTimeout(8000);
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, done, account, key] {
                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                        .toInt();
                const QJsonObject resp =
                    QJsonDocument::fromJson(reply->readAll()).object();
                reply->deleteLater();
                const bool answered =
                    (status >= 200 && status < 300 &&
                     resp.value(QStringLiteral("ok")).toBool()) ||
                    status == 400;
                const bool owned =
                    answered && status != 400 &&
                    resp.value(QStringLiteral("owned")).toBool() &&
                    AccountCapability::normalizedAccount(accountOwner()) ==
                        account &&
                    AccountCapability::normalizedAccount(
                        resp.value(QStringLiteral("nodeName")).toString()) ==
                        account;
                if (owned && !(m_localControl && m_localControl->isPaired(key)))
                    logSystem(QStringLiteral(
                        "A phone signed in to %1 connected; it was approved "
                        "without a pairing code.").arg(account));
                else if (answered && !owned &&
                         m_localControl && m_localControl->isPaired(key))
                    logSystem(QStringLiteral(
                        "%1 no longer lists a phone this desktop had "
                        "approved; it cannot control this desktop through the "
                        "account.").arg(account));
                done(answered, owned);
            });
}

void MainWindow::showLocalControlPairingPrompt(int connectionId,
                                               const QString &name,
                                               const QString &user,
                                               const QString &code)
{
    const QString who = (name.isEmpty() ? QStringLiteral("A phone") : name) +
                        (user.isEmpty() ? QString()
                                        : QStringLiteral(" (%1)").arg(user));
    const QString spaced = code.left(3) + QLatin1Char(' ') + code.mid(3);
    auto *box = new QMessageBox(this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setIcon(QMessageBox::Question);
    box->setWindowTitle(QStringLiteral("Allow phone control?"));
    box->setText(QStringLiteral("<b>%1</b> on this network wants to control "
                                "this desktop.")
                     .arg(who.toHtmlEscaped()));
    box->setInformativeText(
        QStringLiteral("Allow only if the phone shows the code <b>%1</b>. It will "
                       "be able to start, steer and stop agents and send files, "
                       "and will reconnect without asking until you remove it "
                       "in Settings → Phone.")
            .arg(spaced));
    QPushButton *allow = box->addButton(QStringLiteral("Allow"),
                                        QMessageBox::AcceptRole);
    box->addButton(QStringLiteral("Deny"), QMessageBox::RejectRole);
    box->setDefaultButton(allow);
    box->setModal(false);
    m_localControlPrompts.insert(connectionId, box);
    connect(box, &QMessageBox::finished, this, [this, box, allow, connectionId] {
        m_localControlPrompts.remove(connectionId);
        if (!m_localControl)
            return;
        if (box->clickedButton() == allow)
            m_localControl->approvePairing(connectionId);
        else
            m_localControl->denyPairing(connectionId);
    });
    box->show();
    box->raise();
    QApplication::alert(this);
    logSystem(QStringLiteral("%1 asked to control this desktop over the local "
                             "network (code %2).")
                  .arg(who, spaced));
}

QJsonObject MainWindow::localControlAgentsPayload()
{
    QList<const AgentSession *> sessions;
    for (const AgentSession &s : std::as_const(m_agentSessions)) {
        if (s.cloud || isExternalSession(s.id) || s.deletedAtMs > 0 ||
            s.associationOnly)
            continue;
        sessions.append(&s);
    }
    std::sort(sessions.begin(), sessions.end(),
              [](const AgentSession *a, const AgentSession *b) {
                  const bool aActive = sessionActive(*a);
                  const bool bActive = sessionActive(*b);
                  if (aActive != bActive)
                      return aActive;
                  return a->createdAtMs > b->createdAtMs;
              });
    if (sessions.size() > kMaxListedSessions)
        sessions = sessions.mid(0, kMaxListedSessions);

    const QStringList credentials = localProviderCredentialValues();
    QJsonArray list;
    for (const AgentSession *s : std::as_const(sessions)) {
        QJsonObject row{
            {QStringLiteral("id"), s->id},
            {QStringLiteral("repositoryOwner"), s->owner},
            {QStringLiteral("repositoryName"), s->name},
            {QStringLiteral("issueNumber"), s->issueNumber},
            {QStringLiteral("issueTitle"), s->issueTitle},
            {QStringLiteral("priority"), s->priority},
            {QStringLiteral("prompt"), s->prompt.left(600)},
            {QStringLiteral("status"), s->status},
            {QStringLiteral("provider"), s->provider},
            {QStringLiteral("model"), s->model},
            {QStringLiteral("mode"), s->mode},
            {QStringLiteral("branchName"), s->branchName},
            {QStringLiteral("baseBranch"), s->baseBranch},
            {QStringLiteral("prNumber"), s->prNumber},
            {QStringLiteral("merged"), s->merged},
            {QStringLiteral("createdAtMs"), double(s->createdAtMs)},
            {QStringLiteral("startedAtMs"), double(s->startedAtMs)},
            {QStringLiteral("finishedAtMs"), double(s->finishedAtMs)},
            {QStringLiteral("numTurns"), s->numTurns},
            {QStringLiteral("durationMs"), double(s->durationMs)},
            {QStringLiteral("costUsd"), s->costUsd},
            {QStringLiteral("totalTokens"), s->totalTokens},
            {QStringLiteral("lastError"), s->lastError.left(600)},
            {QStringLiteral("stoppable"), isStoppableAgentSession(s->id)},
        };
        if (s->awaitingStart())
            row.insert(QStringLiteral("held"), true);
        row.insert(QStringLiteral("unread"), agentSessionUnread(*s));
        if (const QString terminal = localControlTerminalForRow(s->id); !terminal.isEmpty())
            row.insert(QStringLiteral("terminal"), terminal);
        const auto stat = m_agentDiffStats.constFind(s->id);
        if (stat != m_agentDiffStats.cend()) {
            row.insert(QStringLiteral("diffStats"),
                       QJsonObject{{QStringLiteral("files"), stat->files},
                                   {QStringLiteral("ahead"), stat->ahead},
                                   {QStringLiteral("behind"), stat->behind},
                                   {QStringLiteral("added"), stat->added},
                                   {QStringLiteral("removed"), stat->removed},
                                   {QStringLiteral("dirty"), stat->dirty},
                                   {QStringLiteral("conflicted"), stat->conflicted}});
            row.insert(QStringLiteral("worktree"), !stat->worktree.isEmpty());
        }
        const QStringList images = agentSessionImages(s->id);
        if (!images.isEmpty()) {
            const QList<localcontrolattachments::Entry> first = localcontrolattachments::entries(
                images, s->id, localcontrolattachments::processSalt(), 1);
            if (!first.isEmpty())
                row.insert(QStringLiteral("attachments"), localcontrolattachments::toJson(first));
        }
        list.append(redactProviderCredentials(QJsonValue(row), credentials));
    }

    QJsonArray repositories;
    QSet<QString> seen;
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        if (repo.localPath.isEmpty() || repo.owner.isEmpty() || repo.name.isEmpty())
            continue;
        const QString key = repo.owner.toLower() + QLatin1Char('/') + repo.name.toLower();
        if (seen.contains(key))
            continue;
        seen.insert(key);
        repositories.append(QJsonObject{{QStringLiteral("owner"), repo.owner},
                                        {QStringLiteral("name"), repo.name}});
    }
    const auto repositoryFor = [this](const QString &cwd) -> const RepositoryRecord * {
        const QString path = QDir::cleanPath(cwd);
        for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
            if (repo.localPath.isEmpty() || repo.owner.isEmpty() || repo.name.isEmpty())
                continue;
            const QString root = QDir::cleanPath(repo.localPath);
            if (path == root || path.startsWith(root + QLatin1Char('/')))
                return &repo;
        }
        return nullptr;
    };
    QJsonArray outside;
    for (auto it = m_externalSurfaced.constBegin(); it != m_externalSurfaced.constEnd(); ++it) {
        if (!externalIsLive(it->uuid))
            continue;
        const QString repo = m_externalSurfacedRepo.value(it.key());
        const QString folder = QFileInfo(it->cwd).fileName();
        outside.append(QJsonObject{
            {QStringLiteral("id"), it.key()},
            {QStringLiteral("kind"), QStringLiteral("claude")},
            {QStringLiteral("label"), MachineAgentScan::label(QStringLiteral("claude"))},
            {QStringLiteral("provider"), MachineAgentScan::provider(QStringLiteral("claude"))},
            {QStringLiteral("model"), QString()},
            {QStringLiteral("title"), it->title.isEmpty() ? folder : it->title.left(200)},
            {QStringLiteral("cwd"), it->cwd},
            {QStringLiteral("branchName"), it->gitBranch},
            {QStringLiteral("repositoryOwner"), repo.section(QLatin1Char('/'), 0, 0)},
            {QStringLiteral("repositoryName"), repo.section(QLatin1Char('/'), 1)},
            {QStringLiteral("lastActivityMs"), double(it->lastActivityMs)},
            {QStringLiteral("steer"), outsideAgentSteerable(it.key())},
            {QStringLiteral("typeable"), outsideAgentTerminal(it.key()) != nullptr},
            {QStringLiteral("stoppable"), outsideAgentTerminal(it.key()) != nullptr},
        });
    }
    for (auto it = m_machineAgents.constBegin(); it != m_machineAgents.constEnd(); ++it) {
        if (const auto surfaced = m_externalSurfaced.constFind(it.key());
            surfaced != m_externalSurfaced.constEnd() && externalIsLive(surfaced->uuid))
            continue;
        const QString folder = QFileInfo(it->cwd).fileName();
        const QString label = MachineAgentScan::label(it->kind);
        const RepositoryRecord *repo = repositoryFor(it->cwd);
        outside.append(QJsonObject{
            {QStringLiteral("id"), it.key()},
            {QStringLiteral("kind"), it->kind},
            {QStringLiteral("label"), label},
            {QStringLiteral("provider"), MachineAgentScan::provider(it->kind)},
            {QStringLiteral("model"), it->model},
            {QStringLiteral("title"),
             !it->otherUser.isEmpty()
                 ? QStringLiteral("%1 (%2)").arg(label, it->otherUser)
                 : folder.isEmpty() ? label : folder},
            {QStringLiteral("cwd"), it->cwd},
            {QStringLiteral("branchName"), QString()},
            {QStringLiteral("repositoryOwner"), repo ? repo->owner : QString()},
            {QStringLiteral("repositoryName"), repo ? repo->name : QString()},
            {QStringLiteral("lastActivityMs"), 0.0},
            {QStringLiteral("steer"), outsideAgentSteerable(it.key())},
            {QStringLiteral("typeable"), outsideAgentTerminal(it.key()) != nullptr},
            {QStringLiteral("stoppable"), outsideAgentTerminal(it.key()) != nullptr},
            {QStringLiteral("otherUser"), it->otherUser},
        });
    }
    if (!m_localControlTerminals.isEmpty()) {
        QSet<QString> listed;
        for (qsizetype i = 0; i < outside.size(); ++i) {
            QJsonObject row = outside.at(i).toObject();
            const QString terminal =
                localControlTerminalForOutside(row.value(QStringLiteral("id")).toInt());
            if (terminal.isEmpty() || listed.contains(terminal))
                continue;
            if (m_localControlTerminals.value(terminal).session > 0) {
                outside.removeAt(i--);
                continue;
            }
            listed.insert(terminal);
            row.insert(QStringLiteral("terminal"), terminal);
            outside.replace(i, row);
        }
        for (const forkmesh::localcontrol::terminals::Terminal &terminal :
             std::as_const(m_localControlTerminals)) {
            if (terminal.pid <= 0 || terminal.session > 0 || !terminal.mode.isEmpty() ||
                listed.contains(terminal.id))
                continue;
            outside.append(forkmesh::localcontrol::terminals::outsideRow(
                terminal, externalTempIdFor(QStringLiteral("terminal:") + terminal.id)));
        }
    }
    return QJsonObject{
        {QStringLiteral("sessions"), list},
        {QStringLiteral("outside"),
         redactProviderCredentials(QJsonValue(outside), credentials)},
        {QStringLiteral("repositories"), repositories},
        {QStringLiteral("folders"), localControlAgentFolders()},
        {QStringLiteral("providers"), providerChoices()},
        {QStringLiteral("defaultProvider"), defaultAgentProvider()},
        {QStringLiteral("defaultMode"), localControlDefaultAgentMode(m_quickAddModeSelector)},
        {QStringLiteral("running"), runningAgentCount()},
        {QStringLiteral("queue"), agentQueuePayload()},
    };
}

QJsonArray MainWindow::localControlAgentFolders() const
{
    QHash<QString, QJsonArray> members;
    for (const AgentSession &s : std::as_const(m_agentSessions)) {
        if (s.deletedAtMs > 0 || s.associationOnly ||
            (isExternalSession(s.id) && !s.cloud))
            continue;
        const QString key = agentReadKey(s);
        if (key.isEmpty())
            continue;
        const QString repo = AgentFolders::repoKey(s.owner, s.name);
        const QString folderId = m_agentFolders.folderOf(repo, key);
        if (!folderId.isEmpty())
            members[repo + QLatin1Char('\n') + folderId].append(key);
    }
    QJsonArray folders;
    QSet<QString> seen;
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        const QString key = AgentFolders::repoKey(repo.owner, repo.name);
        if (repo.owner.isEmpty() || repo.name.isEmpty() || seen.contains(key))
            continue;
        seen.insert(key);
        for (const AgentFolder &folder : m_agentFolders.folders(key))
            folders.append(QJsonObject{
                {QStringLiteral("repositoryOwner"), repo.owner},
                {QStringLiteral("repositoryName"), repo.name},
                {QStringLiteral("id"), folder.id},
                {QStringLiteral("name"), folder.name},
                {QStringLiteral("collapsed"), folder.collapsed},
                {QStringLiteral("members"),
                 members.value(key + QLatin1Char('\n') + folder.id)},
            });
    }
    return folders;
}

void MainWindow::notifyLocalControlAgentsChanged()
{
    if (!m_localControl || m_localControl->connectedDevices().isEmpty())
        return;
    // A running session changes many times a second; the phone re-reads the
    // list on each bell, so ring at most a few times a second.
    markLocalControlFollowsDirty();
    if (!m_localControlAgentsTimer) {
        m_localControlAgentsTimer = new QTimer(this);
        m_localControlAgentsTimer->setSingleShot(true);
        connect(m_localControlAgentsTimer, &QTimer::timeout, this, [this] {
            m_localControlAgentsRungAtMs = QDateTime::currentMSecsSinceEpoch();
            if (m_localControl)
                m_localControl->broadcast(QStringLiteral("agents.changed"));
        });
    }
    if (m_localControlAgentsTimer->isActive())
        return;
    const qint64 wait = m_localControlAgentsRungAtMs + kAgentsBellSpacingMs -
                        QDateTime::currentMSecsSinceEpoch();
    m_localControlAgentsTimer->start(int(std::clamp<qint64>(wait, 0, kAgentsBellSpacingMs)));
}

void MainWindow::answerLocalControlOutsideTranscript(int connectionId, int requestId, int id,
                                                     qint64 since)
{
    readLocalControlOutsideTranscript(
        id, since, [this, connectionId, requestId](const QJsonObject &payload, const QString &error) {
            if (!m_localControl)
                return;
            if (!error.isEmpty())
                m_localControl->respondError(connectionId, requestId, error);
            else
                m_localControl->respond(connectionId, requestId, payload);
        });
}

void MainWindow::readLocalControlOutsideTranscript(
    int id, qint64 since,
    std::function<void(const QJsonObject &payload, const QString &error)> done)
{
    if (!localControlTerminalForRow(id).isEmpty()) {
        done({}, QStringLiteral("no_transcript"));
        return;
    }
    QString kind;
    QString path;
    QString cwd;
    if (const auto surfaced = m_externalSurfaced.constFind(id);
        surfaced != m_externalSurfaced.constEnd() && externalIsLive(surfaced->uuid)) {
        kind = QStringLiteral("claude");
        path = surfaced->path;
        cwd = surfaced->cwd;
    } else if (const auto machine = m_machineAgents.constFind(id);
               machine != m_machineAgents.constEnd()) {
        kind = machine->kind;
        path = machine->transcript;
        cwd = machine->cwd;
    } else {
        done({}, QStringLiteral("unknown_session"));
        return;
    }
    if (path.isEmpty() || !AgentLogText::readsTranscript(kind)) {
        done({}, QStringLiteral("no_transcript"));
        return;
    }
    const QString header = QStringLiteral("==> %1 started outside ForkMesh, in %2\n")
                               .arg(MachineAgentScan::label(kind), cwd);
    OutsideTranscriptTail tail = m_localControlOutsideTails.value(id);
    const bool fresh = tail.path != path;
    if (fresh) {
        tail = OutsideTranscriptTail{};
        tail.path = path;
        tail.kind = kind;
        tail.log = header;
    }
    struct Read {
        OutsideTranscriptTail tail;
        bool rebuilt = false;
    };
    runOffThread<Read>(
        [tail, header] {
            Read read{tail};
            OutsideTranscriptTail &t = read.tail;
            if (t.offset > QFileInfo(t.path).size()) {
                t = OutsideTranscriptTail{t.path, t.kind};
                t.log = header;
                read.rebuilt = true;
            }
            if (t.offset < 0)
                t.offset = ClaudeSessionScan::tailStartOffset(t.path, kMaxOutsideTranscriptBytes);
            qint64 next = t.offset;
            const QList<QJsonObject> entries = ClaudeSessionScan::readEvents(t.path, t.offset, &next);
            for (const QJsonObject &entry : entries)
                t.log += AgentLogText::outsideEntry(t.kind, entry);
            t.offset = next;
            t.seenSize = QFileInfo(t.path).size();
            if (t.log.size() > 4 * kMaxTranscriptChars) {
                const qint64 drop = t.log.size() - 2 * kMaxTranscriptChars;
                t.log.remove(0, drop);
                t.base += drop;
            }
            return read;
        },
        [this, id, since, fresh, done](Read read) {
            if (!m_localControl)
                return;
            const OutsideTranscriptTail &t = read.tail;
            const auto kept = m_localControlOutsideTails.constFind(id);
            if (kept == m_localControlOutsideTails.constEnd() || kept->path != t.path ||
                kept->offset <= t.offset)
                m_localControlOutsideTails.insert(id, t);
            const qint64 total = t.base + t.log.size();
            const bool restarted = (fresh || read.rebuilt) && since > 0;
            qint64 from = t.base;
            bool reset = false;
            if (since >= 0 && since <= total && !restarted) {
                from = std::max(since, total - kMaxTranscriptDeltaChars);
            } else {
                reset = since >= 0;
                from = std::max(t.base, total - kMaxTranscriptChars);
            }
            done(
                {{QStringLiteral("id"), id},
                 {QStringLiteral("transcript"),
                  redactProviderCredentials(t.log.mid(from - t.base),
                                            localProviderCredentialValues())},
                 {QStringLiteral("from"), double(from)},
                 {QStringLiteral("next"), double(total)},
                 {QStringLiteral("reset"), reset},
                 {QStringLiteral("active"), true},
                 {QStringLiteral("status"), QStringLiteral("running")},
                 {QStringLiteral("outside"), true},
                 {QStringLiteral("attachments"), QJsonArray{}}},
                QString());
        });
}

bool MainWindow::outsideAgentSteerable(int id) const
{
    if (const auto surfaced = m_externalSurfaced.constFind(id);
        surfaced != m_externalSurfaced.constEnd())
        return externalIsLive(surfaced->uuid) &&
               ClaudePeerMessaging::find(m_claudePeerSessions,
                                         m_externalSurfacedPids.value(surfaced->uuid),
                                         surfaced->uuid) != nullptr;
    if (const auto machine = m_machineAgents.constFind(id); machine != m_machineAgents.constEnd())
        return machine->kind == QLatin1String("claude") &&
               ClaudePeerMessaging::find(m_claudePeerSessions, {machine->pid}, QString()) != nullptr;
    return false;
}

const TerminalHandoffSession *MainWindow::outsideAgentTerminal(int id) const
{
    const TerminalHandoffSession *terminal = outsideAgentRegistration(id);
    return terminal && terminal->remote ? terminal : nullptr;
}

const TerminalHandoffSession *MainWindow::outsideAgentRegistration(int id) const
{
    if (m_terminalHandoffSessions.isEmpty())
        return nullptr;
    QList<qint64> pids;
    if (const auto surfaced = m_externalSurfaced.constFind(id);
        surfaced != m_externalSurfaced.constEnd()) {
        if (!externalIsLive(surfaced->uuid))
            return nullptr;
        pids = m_externalSurfacedPids.value(surfaced->uuid);
        if (const ClaudePeerSession *peer =
                ClaudePeerMessaging::find(m_claudePeerSessions, {}, surfaced->uuid))
            pids.append(peer->pid);
    } else if (const auto machine = m_machineAgents.constFind(id);
               machine != m_machineAgents.constEnd()) {
        pids.append(machine->pid);
    }
    if (pids.isEmpty())
        return nullptr;
    return TerminalHandoff::find(m_terminalHandoffSessions, pids);
}

int MainWindow::outsideAgentReach(int id) const
{
    return (outsideAgentSteerable(id) ? 1 : 0) | (outsideAgentTerminal(id) ? 2 : 0);
}

void MainWindow::answerLocalControlOutsideStop(int connectionId, int requestId, int id)
{
    if (!m_machineAgents.value(id).otherUser.isEmpty()) {
        m_localControl->respondError(connectionId, requestId, QStringLiteral("outside_other_user"));
        return;
    }
    const TerminalHandoffSession *terminal = outsideAgentTerminal(id);
    if (!terminal) {
        m_localControl->respondError(connectionId, requestId,
                                     QStringLiteral("outside_stop_unsupported"));
        return;
    }
    const LocalControlServer::Device device = m_localControl->deviceFor(connectionId);
    if (device.key.isEmpty() || !m_localControl->isPaired(device.key)) {
        m_localControl->respondError(connectionId, requestId, QStringLiteral("not_paired"));
        return;
    }
    const TerminalHandoffSession session = *terminal;
    const QString keyName = TerminalHandoff::interruptKey(session.cli);
    const QString label = MachineAgentScan::label(session.cli);
    logSystem(QStringLiteral("Pressed %1 in the terminal running %2 in %3 to stop it, "
                             "requested by %4.")
                  .arg(keyName == QLatin1String("esc") ? QStringLiteral("Esc")
                                                         : QStringLiteral("Ctrl-C"),
                       label, session.cwd, localControlDeviceLabel(connectionId)));
    const QString from = device.name;
    runOffThread<TerminalHandoffReply>(
        [session, from] { return TerminalHandoff::key(session, from, QStringLiteral("interrupt")); },
        [this, connectionId, requestId, keyName, session](TerminalHandoffReply reply) {
            if (!m_localControl)
                return;
            if (!reply.ok) {
                logSystem(QStringLiteral("Could not stop the terminal session in %1: %2.")
                              .arg(session.cwd, reply.detail));
                m_localControl->respondError(connectionId, requestId,
                                             TerminalHandoff::errorCode(reply));
                return;
            }
            m_localControl->respond(connectionId, requestId,
                                    {{QStringLiteral("status"), QStringLiteral("interrupted")},
                                     {QStringLiteral("key"), keyName}});
            notifyLocalControlAgentsChanged();
        });
}

void MainWindow::answerLocalControlOutsidePrompt(int connectionId, int requestId, int id,
                                                 const QString &text)
{
    QString kind;
    QString uuid;
    QString cwd;
    QString transcript;
    QString configDir;
    qint64 pid = 0;
    if (const auto surfaced = m_externalSurfaced.constFind(id);
        surfaced != m_externalSurfaced.constEnd() && externalIsLive(surfaced->uuid)) {
        kind = QStringLiteral("claude");
        uuid = surfaced->uuid;
        cwd = surfaced->cwd;
        transcript = surfaced->path;
    } else if (const auto machine = m_machineAgents.constFind(id);
               machine != m_machineAgents.constEnd()) {
        if (!machine->otherUser.isEmpty()) {
            m_localControl->respondError(connectionId, requestId,
                                         QStringLiteral("outside_other_user"));
            return;
        }
        kind = machine->kind;
        pid = machine->pid;
        cwd = machine->cwd;
        transcript = machine->transcript;
        configDir = machine->configDir;
    } else {
        m_localControl->respondError(connectionId, requestId, QStringLiteral("unknown_session"));
        return;
    }
    std::optional<TerminalHandoffSession> terminal;
    if (const TerminalHandoffSession *found = outsideAgentTerminal(id)) {
        const LocalControlServer::Device device = m_localControl->deviceFor(connectionId);
        const QString refused = TerminalHandoff::admit(
            m_terminalTypingGate, m_localControl->isPaired(device.key), device.key,
            int(text.size()), QDateTime::currentMSecsSinceEpoch());
        if (!refused.isEmpty()) {
            logSystem(QStringLiteral("Refused to type into the terminal in %1 for %2: %3.")
                          .arg(cwd, localControlDeviceLabel(connectionId), refused));
            m_localControl->respondError(connectionId, requestId, refused);
            return;
        }
        terminal = *found;
    }
    if (!terminal && kind != QLatin1String("claude")) {
        m_localControl->respondError(connectionId, requestId,
                                     QStringLiteral("outside_cli_unsupported"));
        return;
    }
    if (configDir.isEmpty() && !transcript.isEmpty())
        configDir = QFileInfo(transcript).dir().absolutePath().section(QLatin1Char('/'), 0, -3);
    if (terminal)
        logSystem(QStringLiteral("Typing into the terminal running %1 in %2, requested by %3.")
                      .arg(MachineAgentScan::label(kind), cwd,
                           localControlDeviceLabel(connectionId)));
    else
        logSystem(QStringLiteral("Message sent to Claude Code started outside ForkMesh in %1, "
                                 "requested by %2.")
                      .arg(cwd, localControlDeviceLabel(connectionId)));
    const QString typedFrom = m_localControl->deviceFor(connectionId).name;
    const bool claude = kind == QLatin1String("claude");
    const QString home = QDir::homePath();
    const QString dataRoot = forkmesh::paths::dataRoot();
    struct Sent {
        bool ok = false;
        QString code;
        QString error;
        QString status;
        QString cause;
        QString via;
        QString fallback;
    };
    runOffThread<Sent>(
        [text, uuid, cwd, transcript, configDir, pid, home, dataRoot, terminal, typedFrom,
         claude] {
            QString fallback;
            if (terminal) {
                const TerminalHandoffReply reply = TerminalHandoff::type(*terminal, typedFrom, text);
                Sent typed;
                typed.via = QStringLiteral("terminal");
                if (reply.ok) {
                    typed.ok = true;
                    typed.status = QStringLiteral("typed");
                    return typed;
                }
                typed.code = TerminalHandoff::errorCode(reply);
                typed.error = QStringLiteral("the terminal answered %1").arg(reply.detail);
                const bool gone = reply.code == QLatin1String("unreachable") ||
                                  reply.code == QLatin1String("remote_off") ||
                                  reply.code == QLatin1String("exited");
                if (!gone || !claude)
                    return typed;
                fallback = typed.error;
            }
            const QList<qint64> pids =
                pid > 0 ? QList<qint64>{pid} : ClaudeSessionScan::findSessionPids(uuid, cwd);
            const QList<ClaudePeerSession> peers = ClaudePeerMessaging::liveSessions(
                ClaudePeerMessaging::configDirs(home, dataRoot,
                                                configDir.isEmpty() ? QStringList{}
                                                                    : QStringList{configDir}));
            const ClaudePeerSession *session =
                ClaudePeerMessaging::find(peers, pids, pid > 0 ? QString() : uuid);
            Sent sent;
            sent.via = QStringLiteral("peer");
            sent.fallback = fallback;
            if (!session) {
                sent.code = QStringLiteral("not_steerable");
                sent.error = QStringLiteral("no live messaging socket");
                return sent;
            }
            QFile cmdline(QStringLiteral("/proc/%1/cmdline").arg(session->pid));
            QByteArrayList argv;
            if (cmdline.open(QIODevice::ReadOnly))
                argv = cmdline.readAll().split('\0');
            QString own = session->configDir + QStringLiteral("/projects/") +
                          MachineAgentScan::claudeProjectKey(session->cwd) + QLatin1Char('/') +
                          session->sessionId + QStringLiteral(".jsonl");
            if (session->sessionId.isEmpty() || !QFileInfo(own).isFile())
                own = transcript;
            const ClaudeInboundPolicy policy = ClaudePeerMessaging::predictInbound(
                *session, argv, ClaudePeerMessaging::permissionClass(argv, own),
                ClaudePeerMessaging::managedSettingsPath());
            sent.cause = policy.cause;
            if (policy.policy == QLatin1String("refuse")) {
                sent.code = QStringLiteral("outside_refuses_messages");
                sent.error = QStringLiteral("the session refuses cross-session messages (%1)")
                                 .arg(policy.cause);
                return sent;
            }
            const bool held = policy.policy == QLatin1String("hold");
            if (held && session->entrypoint != QLatin1String("cli")) {
                sent.code = QStringLiteral("outside_held_no_terminal");
                sent.error = QStringLiteral("the session would hold the message (%1) and has no "
                                            "terminal to approve it in")
                                 .arg(policy.cause);
                return sent;
            }
            sent.ok = ClaudePeerMessaging::deliver(*session, text, &sent.error);
            if (!sent.ok)
                sent.code = QStringLiteral("not_steerable");
            sent.status = held ? QStringLiteral("held_for_approval") : QStringLiteral("delivered");
            return sent;
        },
        [this, connectionId, requestId, cwd](Sent sent) {
            if (!m_localControl)
                return;
            if (!sent.ok) {
                logSystem(QStringLiteral("Could not deliver the message to the session in %1: %2.")
                              .arg(cwd, sent.error));
                m_localControl->respondError(connectionId, requestId, sent.code);
                return;
            }
            if (!sent.fallback.isEmpty())
                logSystem(QStringLiteral("Could not type into the terminal in %1 (%2); sent it as a "
                                         "message from another session instead.")
                              .arg(cwd, sent.fallback));
            if (sent.status == QLatin1String("typed"))
                logSystem(QStringLiteral("Typed into the terminal in %1.").arg(cwd));
            if (sent.status == QLatin1String("held_for_approval"))
                logSystem(QStringLiteral("Claude Code in %1 holds the message for approval in its "
                                         "terminal (%2).")
                              .arg(cwd, sent.cause));
            m_localControl->respond(connectionId, requestId,
                                    {{QStringLiteral("status"), sent.status},
                                     {QStringLiteral("cause"), sent.cause},
                                     {QStringLiteral("via"), sent.via}});
            notifyLocalControlAgentsChanged();
        });
}

void MainWindow::ringLocalControlOutsideTranscripts()
{
    if (!m_localControl || m_localControl->connectedDevices().isEmpty()) {
        m_localControlOutsideTails.clear();
        return;
    }
    bool grew = false;
    for (auto it = m_localControlOutsideTails.begin(); it != m_localControlOutsideTails.end();) {
        if (!m_externalSurfaced.contains(it.key()) && !m_machineAgents.contains(it.key())) {
            it = m_localControlOutsideTails.erase(it);
            continue;
        }
        const qint64 size = QFileInfo(it->path).size();
        if (size != it->seenSize) {
            it->seenSize = size;
            grew = true;
        }
        ++it;
    }
    if (grew)
        notifyLocalControlAgentsChanged();
}

void MainWindow::handleLocalControlRequest(int connectionId, int requestId,
                                           const QString &method,
                                           const QJsonObject &params)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    const int sessionId = params.value(QStringLiteral("id")).toInt();
    auto ownSession = [this](int id) -> AgentSession * {
        if (id <= 0 || isExternalSession(id))
            return nullptr;
        AgentSession *session = findAgentSession(id);
        return session && !session->cloud && session->deletedAtMs == 0 ? session
                                                                       : nullptr;
    };

    if (method.startsWith(QLatin1String("hosts."))) {
        handleLocalControlHostsRequest(connectionId, requestId, method, params);
        return;
    }
    // Changes to agents are written to the desktop log with who asked and
    // how they reached this desktop.
    const auto audit = [this, connectionId](const QString &what) {
        logSystem(QStringLiteral("%1, requested by %2.")
                      .arg(what, localControlDeviceLabel(connectionId)));
    };

    if (method == QLatin1String("status")) {
        QJsonArray capabilities{QStringLiteral("agents"),
                                QStringLiteral("agents.questions"),
                                QStringLiteral("agents.answer"),
                                QStringLiteral("agents.transcript.since"),
                                QStringLiteral("agents.stream"),
                                QStringLiteral("agents.outside"),
                                QStringLiteral("agents.outside.transcript"),
                                QStringLiteral("agents.handoff"),
                                QStringLiteral("agents.takeover"),
                                QStringLiteral("agents.images"),
                                QStringLiteral("agents.queue"),
                                QStringLiteral("agentops.diff"),
                                QStringLiteral("agentops.file"),
                                QStringLiteral("agentops.delete"),
                                QStringLiteral("agentops.bin"),
                                QStringLiteral("agentops.accounts"),
                                QStringLiteral("agentops.mergestats"),
                                QStringLiteral("hosts"),
                                QStringLiteral("repos")};
#if defined(Q_OS_UNIX)
        capabilities.insert(4, QStringLiteral("agents.outside.prompt"));
        capabilities.insert(5, QStringLiteral("agents.outside.type"));
        capabilities.insert(6, QStringLiteral("agents.outside.stop"));
#endif
#if defined(FORKMESH_HAVE_TERM)
        capabilities.append(forkmesh::localcontrol::terminals::capability());
        capabilities.append(forkmesh::localcontrol::terminals::resumeCapability());
        capabilities.append(forkmesh::localcontrol::terminals::accountsCapability());
#endif
        server->respond(connectionId, requestId,
                        {{QStringLiteral("name"), machineNodeName()},
                         {QStringLiteral("user"), nodeOwnerDisplayName()},
                         {QStringLiteral("version"), QStringLiteral(FORKMESH_VERSION)},
                         {QStringLiteral("commit"), forkmesh::desktopBuildCommit()},
                         {QStringLiteral("running"), runningAgentCount()},
                         {QStringLiteral("transport"),
                          server->isRelayed(connectionId) ? QStringLiteral("relay")
                                                          : QStringLiteral("lan")},
                         {QStringLiteral("capabilities"), capabilities}});
        return;
    }
    if (method == QLatin1String("agents.follow") ||
        method == QLatin1String("agents.unfollow")) {
        handleLocalControlFollowRequest(connectionId, requestId, method, params);
        return;
    }
    if (method == QLatin1String("agents.list")) {
        server->respond(connectionId, requestId, localControlAgentsPayload());
        return;
    }
    if (method == QLatin1String("agents.questions")) {
        if (!ownSession(sessionId)) {
            server->respondError(connectionId, requestId, QStringLiteral("unknown_session"));
            return;
        }
        QJsonArray questions;
        const auto events = m_streamEvents.constFind(sessionId);
        if (events != m_streamEvents.constEnd()) {
            for (const agentops::PendingQuestion &question : agentops::questionsInEvents(*events))
                questions.append(QJsonObject{{QStringLiteral("id"), question.requestId},
                                             {QStringLiteral("input"), question.input}});
        }
        server->respond(connectionId, requestId, {{QStringLiteral("questions"), questions}});
        return;
    }
    if (((sessionId < 0 &&
          (method == QLatin1String("agents.prompt") || method == QLatin1String("agents.stop") ||
           method == QLatin1String("agents.transcript"))) ||
         (sessionId > 0 &&
          (method == QLatin1String("agents.prompt") || method == QLatin1String("agents.stop") ||
           method == QLatin1String("agents.continue")))) &&
        answerLocalControlTerminalAgent(connectionId, requestId, method, sessionId, params))
        return;
    if (method == QLatin1String("agents.transcript") && sessionId < 0) {
        answerLocalControlOutsideTranscript(
            connectionId, requestId, sessionId,
            params.contains(QStringLiteral("since"))
                ? qint64(params.value(QStringLiteral("since")).toDouble())
                : -1);
        return;
    }
    if (method == QLatin1String("agents.transcript")) {
        AgentSession *session = ownSession(sessionId);
        if (!session || !m_agentStore) {
            server->respondError(connectionId, requestId,
                                 QStringLiteral("unknown_session"));
            return;
        }
        AgentStore *store = m_agentStore;
        const AgentSession copy = *session;
        // With `since` (the length the phone already has) only what is new
        // comes back, so a live view can follow a run on each agents.changed
        // bell without re-reading the whole log.
        const qint64 since = params.contains(QStringLiteral("since"))
                                 ? qint64(params.value(QStringLiteral("since")).toDouble())
                                 : -1;
        const bool active = sessionActive(*session);
        const QString status = session->status;
        const QStringList known = agentSessionImages(sessionId);
        struct TranscriptRead {
            QString log;
            QJsonArray attachments;
        };
        runOffThread<TranscriptRead>(
            [store, copy, known, sessionId] {
                TranscriptRead read;
                read.log = store->readLog(copy);
                read.attachments = localcontrolattachments::toJson(
                    localcontrolattachments::entries(
                        localControlAttachmentPaths(*store, copy, known), sessionId,
                        localcontrolattachments::processSalt()));
                return read;
            },
            [this, connectionId, requestId, sessionId, store, since, active,
             status](TranscriptRead read) {
                const QString &log = read.log;
                if (!m_localControl || m_agentStore != store)
                    return;
                const qint64 total = log.size();
                qint64 from = 0;
                bool reset = false;
                if (since >= 0 && since <= total) {
                    from = since;
                    if (total - from > kMaxTranscriptDeltaChars)
                        from = total - kMaxTranscriptDeltaChars;
                } else {
                    reset = since >= 0; // the log was rewritten; start over
                    if (total > kMaxTranscriptChars)
                        from = total - kMaxTranscriptChars;
                }
                m_localControl->respond(
                    connectionId, requestId,
                    {{QStringLiteral("id"), sessionId},
                     {QStringLiteral("transcript"),
                      redactProviderCredentials(log.mid(from),
                                                localProviderCredentialValues())},
                     {QStringLiteral("from"), double(from)},
                     {QStringLiteral("next"), double(total)},
                     {QStringLiteral("reset"), reset},
                     {QStringLiteral("active"), active},
                     {QStringLiteral("status"), status},
                     {QStringLiteral("attachments"), read.attachments}});
            });
        return;
    }
    if (method == QLatin1String("agents.attachment")) {
        AgentSession *session = ownSession(sessionId);
        const QString wanted =
            params.value(QStringLiteral("attachment")).toString().trimmed().toLower();
        static const QRegularExpression idPattern(QStringLiteral("^[0-9a-f]{32}$"));
        if (!session || !m_agentStore || !idPattern.match(wanted).hasMatch()) {
            server->respondError(connectionId, requestId,
                                 QStringLiteral("unknown_attachment"));
            return;
        }
        AgentStore *store = m_agentStore;
        const AgentSession copy = *session;
        const QStringList known = agentSessionImages(sessionId);
        const bool relayPreview = server->isRelayed(connectionId) ||
                                  params.value(QStringLiteral("preview")).toBool();
        runOffThread<localcontrolattachments::Payload>(
            [store, copy, known, sessionId, wanted, relayPreview] {
                const QList<localcontrolattachments::Entry> list =
                    localcontrolattachments::entries(
                        localControlAttachmentPaths(*store, copy, known), sessionId,
                        localcontrolattachments::processSalt());
                for (const localcontrolattachments::Entry &entry : list) {
                    if (entry.id == wanted)
                        return relayPreview ? localcontrolattachments::relayPayload(entry)
                                            : localcontrolattachments::payload(entry);
                }
                localcontrolattachments::Payload missing;
                missing.error = QStringLiteral("unknown_attachment");
                return missing;
            },
            [this, connectionId, requestId, store](localcontrolattachments::Payload out) {
                if (!m_localControl || m_agentStore != store)
                    return;
                if (!out.error.isEmpty())
                    m_localControl->respondError(connectionId, requestId, out.error);
                else
                    m_localControl->respond(connectionId, requestId, out.json);
            });
        return;
    }
    if (method == QLatin1String("agents.image.begin")) {
        QString error;
        const QString image = m_localControlImages.begin(
            connectionId, params.value(QStringLiteral("mediaType")).toString(),
            qint64(params.value(QStringLiteral("size")).toDouble()),
            params.value(QStringLiteral("sha256")).toString(), &error);
        if (image.isEmpty()) {
            server->respondError(connectionId, requestId, error);
            return;
        }
        server->respond(connectionId, requestId, {{QStringLiteral("image"), image}});
        return;
    }
    if (method == QLatin1String("agents.image.chunk")) {
        const auto decoded = QByteArray::fromBase64Encoding(
            params.value(QStringLiteral("data")).toString().toLatin1(),
            QByteArray::AbortOnBase64DecodingErrors);
        const QString image = params.value(QStringLiteral("image")).toString();
        QString error;
        const qint64 received = m_localControlImages.chunk(
            connectionId, image, decoded ? *decoded : QByteArray(), &error);
        if (received < 0) {
            server->respondError(connectionId, requestId, error);
            return;
        }
        server->respond(connectionId, requestId,
                        {{QStringLiteral("received"), double(received)}});
        return;
    }
    if (method == QLatin1String("agents.image.cancel")) {
        m_localControlImages.cancel(connectionId,
                                    params.value(QStringLiteral("image")).toString());
        server->respond(connectionId, requestId, {});
        return;
    }
    QStringList imageIds;
    const bool takesImages = method == QLatin1String("agents.start") ||
                             method == QLatin1String("agents.prompt");
    if (takesImages &&
        !m_localControlImages.parseIds(params.value(QStringLiteral("images")), &imageIds)) {
        server->respondError(connectionId, requestId, QStringLiteral("bad_images"));
        return;
    }
    const auto dropImages = [this, connectionId, &imageIds] {
        for (const QString &id : imageIds)
            m_localControlImages.cancel(connectionId, id);
    };
    const auto takeImages = [this, connectionId, &imageIds](QString *error) {
        return m_localControlImages.take(connectionId, imageIds,
                                         AgentPromptImages::directory(), error);
    };
    if (method == QLatin1String("agents.prompt") && sessionId < 0) {
        const QString text = params.value(QStringLiteral("text")).toString().trimmed();
        if (!imageIds.isEmpty()) {
            dropImages();
            server->respondError(connectionId, requestId,
                                 QStringLiteral("outside_images_unsupported"));
            return;
        }
        if (text.isEmpty() || text.size() > kMaxPromptChars) {
            server->respondError(connectionId, requestId, QStringLiteral("bad_task"));
            return;
        }
        answerLocalControlOutsidePrompt(connectionId, requestId, sessionId, text);
        return;
    }
    if (method == QLatin1String("agents.stop") && sessionId < 0) {
        answerLocalControlOutsideStop(connectionId, requestId, sessionId);
        return;
    }
    if (method == QLatin1String("agents.start")) {
        const QString owner = params.value(QStringLiteral("owner")).toString().trimmed();
        const QString name = params.value(QStringLiteral("name")).toString().trimmed();
        const QString task = params.value(QStringLiteral("task")).toString().trimmed();
        if (task.isEmpty() || task.size() > kMaxPromptChars) {
            dropImages();
            server->respondError(connectionId, requestId, QStringLiteral("bad_task"));
            return;
        }
        int repoIndex = -1;
        for (int i = 0; i < m_repositories.size(); ++i) {
            const RepositoryRecord &r = m_repositories.at(i);
            if (r.owner.compare(owner, Qt::CaseInsensitive) == 0 &&
                r.name.compare(name, Qt::CaseInsensitive) == 0 &&
                !r.localPath.isEmpty()) {
                repoIndex = i;
                break;
            }
        }
        if (repoIndex < 0) {
            dropImages();
            server->respondError(connectionId, requestId,
                                 QStringLiteral("no_checkout"));
            return;
        }
        QString imageError;
        const QStringList images = takeImages(&imageError);
        if (!imageError.isEmpty()) {
            server->respondError(connectionId, requestId, imageError);
            return;
        }
        const QString requested = params.value(QStringLiteral("provider")).toString();
        const QString provider =
            agentProviderIsKnown(requested) ? requested : defaultAgentProvider();
        QString model = params.value(QStringLiteral("model")).toString().trimmed();
        if (model.isEmpty() && provider == QLatin1String("claude-code"))
            model = QSettings().value(kClaudeCodeModelSetting).toString();
        audit(images.isEmpty()
                  ? QStringLiteral("Starting a new agent for %1/%2").arg(owner, name)
                  : QStringLiteral("Starting a new agent for %1/%2 with %3 image(s)")
                        .arg(owner, name)
                        .arg(images.size()));
        m_lastLocalOperationError.clear();
        const int id = startAdHocAgentForRepo(
            repoIndex, forkmesh::orgagentjobs::promptWithAttachments(task, images),
            provider, /*createPr=*/false, model, QString(), /*genie=*/false,
            /*switchToTab=*/false, QString(), localControlAgentMode(params),
            QString(), /*priority=*/50,
            /*directCheckout=*/!params.value(QStringLiteral("worktree")).toBool(true),
            QString(), params.value(QStringLiteral("vibe")).toBool());
        if (id <= 0) {
            for (const QString &path : images)
                QFile::remove(path);
            server->respondError(connectionId, requestId,
                                 QStringLiteral("not_started"), m_lastLocalOperationError);
            return;
        }
        server->respond(connectionId, requestId, {{QStringLiteral("id"), id}});
        notifyLocalControlAgentsChanged();
        return;
    }
    if (method == QLatin1String("agents.handoff")) {
        const QString owner = params.value(QStringLiteral("owner")).toString().trimmed();
        const QString name = params.value(QStringLiteral("name")).toString().trimmed();
        const QString task = params.value(QStringLiteral("task")).toString().trimmed();
        const QString branch = params.value(QStringLiteral("branch")).toString().trimmed();
        const QString base = params.value(QStringLiteral("base")).toString().trimmed();
        if (task.isEmpty() || task.size() > kMaxPromptChars) {
            server->respondError(connectionId, requestId, QStringLiteral("bad_task"));
            return;
        }
        if (!forkmesh::localcontrol::handoffBranchValid(branch) ||
            (!base.isEmpty() && !forkmesh::localcontrol::handoffRefNameValid(base))) {
            server->respondError(connectionId, requestId, QStringLiteral("bad_branch"));
            return;
        }
        int repoIndex = -1;
        for (int i = 0; i < m_repositories.size(); ++i) {
            const RepositoryRecord &r = m_repositories.at(i);
            if (r.owner.compare(owner, Qt::CaseInsensitive) == 0 &&
                r.name.compare(name, Qt::CaseInsensitive) == 0 &&
                !r.localPath.isEmpty()) {
                repoIndex = i;
                break;
            }
        }
        if (repoIndex < 0) {
            server->respondError(connectionId, requestId,
                                 QStringLiteral("no_checkout"));
            return;
        }
        const RepositoryRecord local = writableRecordFor(m_repositories.at(repoIndex));
        if (local.localPath.trimmed().isEmpty() ||
            !fetchServedBranch(local, branch, /*update=*/true)) {
            server->respondError(connectionId, requestId,
                                 QStringLiteral("branch_not_found"));
            return;
        }
        const QString requested = params.value(QStringLiteral("provider")).toString();
        const QString provider =
            agentProviderIsKnown(requested) ? requested : defaultAgentProvider();
        QString model = params.value(QStringLiteral("model")).toString().trimmed();
        if (model.isEmpty() && provider == QLatin1String("claude-code"))
            model = QSettings().value(kClaudeCodeModelSetting).toString();
        QString title = params.value(QStringLiteral("title")).toString().trimmed().left(200);
        if (title.isEmpty())
            title = forkmesh::agentwt::promptTitle(task);
        audit(QStringLiteral("Continuing a phone agent for %1/%2 on %3")
                  .arg(owner, name, branch));
        m_lastLocalOperationError.clear();
        const int id = startAgentOnExistingBranch(repoIndex, branch, base,
                                                  /*prNumber=*/0, title, task,
                                                  provider, codexChatGptModelId(model));
        if (id <= 0) {
            server->respondError(connectionId, requestId,
                                 QStringLiteral("not_started"), m_lastLocalOperationError);
            return;
        }
        if (AgentSession *session = findAgentSession(id); session && m_agentStore)
            m_agentStore->appendLog(
                *session,
                QStringLiteral("==> Continuing an agent moved from %1, on branch %2.\n")
                    .arg(localControlDeviceLabel(connectionId), branch));
        server->respond(connectionId, requestId, {{QStringLiteral("id"), id}});
        notifyLocalControlAgentsChanged();
        return;
    }
    if (method == QLatin1String("agents.takeover")) {
        const QString cloudSession =
            params.value(QStringLiteral("cloudSession")).toString().trimmed();
        const QString owner = params.value(QStringLiteral("owner")).toString().trimmed();
        const QString name = params.value(QStringLiteral("name")).toString().trimmed();
        const QString task = params.value(QStringLiteral("task")).toString().trimmed();
        const QString branch = params.value(QStringLiteral("branch")).toString().trimmed();
        const QString base = params.value(QStringLiteral("base")).toString().trimmed();
        const QString runner = params.value(QStringLiteral("runner")).toString().trimmed().left(80);
        if (!forkmesh::localcontrol::takeoverSessionValid(cloudSession)) {
            server->respondError(connectionId, requestId, QStringLiteral("bad_session"));
            return;
        }
        if (task.isEmpty() || task.size() > kMaxPromptChars) {
            server->respondError(connectionId, requestId, QStringLiteral("bad_task"));
            return;
        }
        if ((!branch.isEmpty() && !forkmesh::localcontrol::handoffBranchValid(branch)) ||
            (!base.isEmpty() && !forkmesh::localcontrol::handoffRefNameValid(base))) {
            server->respondError(connectionId, requestId, QStringLiteral("bad_branch"));
            return;
        }
        if (const int existing = localSessionForCloudSession(cloudSession); existing > 0) {
            const AgentSession *session = findAgentSession(existing);
            server->respond(connectionId, requestId,
                            {{QStringLiteral("id"), existing},
                             {QStringLiteral("branch"),
                              session ? session->branchName : QString()},
                             {QStringLiteral("already"), true}});
            return;
        }
        QList<QPair<QString, QString>> checkouts;
        for (const RepositoryRecord &r : std::as_const(m_repositories))
            checkouts.append(r.previewOnly || r.localPath.trimmed().isEmpty()
                                 ? QPair<QString, QString>()
                                 : QPair<QString, QString>(r.owner, r.name));
        const int repoIndex =
            forkmesh::localcontrol::takeoverRepoIndex(checkouts, owner, name);
        if (repoIndex < 0) {
            server->respondError(connectionId, requestId,
                                 QStringLiteral("no_checkout"));
            return;
        }
        const RepositoryRecord local = writableRecordFor(m_repositories.at(repoIndex));
        if (params.value(QStringLiteral("check")).toBool()) {
            server->respond(connectionId, requestId,
                            {{QStringLiteral("id"), 0},
                             {QStringLiteral("check"), true},
                             {QStringLiteral("repo"),
                              QStringLiteral("%1/%2").arg(local.owner, local.name)}});
            return;
        }
        const QString requested = params.value(QStringLiteral("provider")).toString();
        const QString provider =
            agentProviderIsKnown(requested) ? requested : defaultAgentProvider();
        QString model = params.value(QStringLiteral("model")).toString().trimmed();
        if (model.isEmpty() && provider == QLatin1String("claude-code"))
            model = QSettings().value(kClaudeCodeModelSetting).toString();
        QString title = params.value(QStringLiteral("title")).toString().trimmed().left(200);
        if (title.isEmpty())
            title = forkmesh::agentwt::promptTitle(task);
        const bool onBranch = !branch.isEmpty() &&
                              !local.localPath.trimmed().isEmpty() &&
                              fetchServedBranch(local, branch, /*update=*/true);
        const QString from = runner.isEmpty() ? QStringLiteral("a runner") : runner;
        audit(onBranch ? QStringLiteral("Taking over run %1 from %2 for %3/%4 on %5")
                             .arg(cloudSession, from, local.owner, local.name, branch)
                       : QStringLiteral("Taking over run %1 from %2 for %3/%4 from the start")
                             .arg(cloudSession, from, local.owner, local.name));
        m_lastLocalOperationError.clear();
        const int id =
            onBranch ? startAgentOnExistingBranch(repoIndex, branch, base, /*prNumber=*/0,
                                                  title, task, provider,
                                                  codexChatGptModelId(model))
                     : startAdHocAgentForRepo(repoIndex, task, provider, /*createPr=*/false,
                                              model, title, /*genie=*/false,
                                              /*switchToTab=*/false);
        if (id <= 0) {
            server->respondError(connectionId, requestId,
                                 QStringLiteral("not_started"), m_lastLocalOperationError);
            return;
        }
        QString started;
        if (AgentSession *session = findAgentSession(id)) {
            session->webSessionId = cloudSession;
            started = session->branchName;
            if (m_agentStore) {
                m_agentStore->saveSession(*session);
                m_agentStore->appendLog(
                    *session,
                    onBranch
                        ? QStringLiteral("==> Continuing run %1 from %2, moved here by %3, "
                                         "on branch %4.\n")
                              .arg(cloudSession, from,
                                   localControlDeviceLabel(connectionId), branch)
                        : QStringLiteral("==> Continuing run %1 from %2, moved here by %3. "
                                         "Its branch %4 could not be fetched, so this run "
                                         "starts from the base branch.\n")
                              .arg(cloudSession, from,
                                   localControlDeviceLabel(connectionId),
                                   branch.isEmpty() ? QStringLiteral("(none)") : branch));
            }
        }
        for (auto it = m_cloudAgentSessions.begin(); it != m_cloudAgentSessions.end(); ++it) {
            if (it->sessionId != cloudSession)
                continue;
            m_cloudAgentIds.remove(it->key());
            if (m_selectedAgentSessionId == it.key())
                m_selectedAgentSessionId = id;
            m_cloudAgentSessions.erase(it);
            break;
        }
        reloadAgents();
        server->respond(connectionId, requestId,
                        {{QStringLiteral("id"), id},
                         {QStringLiteral("branch"), onBranch ? branch : started},
                         {QStringLiteral("fresh"), !onBranch}});
        notifyLocalControlAgentsChanged();
        return;
    }
    if (method == QLatin1String("agents.prompt")) {
        const QString text = params.value(QStringLiteral("text")).toString().trimmed();
        if (!ownSession(sessionId) || text.isEmpty() || text.size() > kMaxPromptChars) {
            dropImages();
            server->respondError(connectionId, requestId,
                                 QStringLiteral("unknown_session"));
            return;
        }
        QString imageError;
        const QStringList images = takeImages(&imageError);
        if (!imageError.isEmpty()) {
            server->respondError(connectionId, requestId, imageError);
            return;
        }
        audit(images.isEmpty()
                  ? QStringLiteral("Prompt sent to agent %1").arg(sessionId)
                  : QStringLiteral("Prompt with %1 image(s) sent to agent %2")
                        .arg(images.size())
                        .arg(sessionId));
        applyLocalControlAgentChoice(sessionId, params);
        sendPromptToAgentSession(
            sessionId, forkmesh::orgagentjobs::promptWithAttachments(text, images));
        server->respond(connectionId, requestId, {});
        notifyLocalControlAgentsChanged();
        return;
    }
    if (method == QLatin1String("agents.answer")) {
        const QString questionId = params.value(QStringLiteral("question")).toString();
        const QString answer = params.value(QStringLiteral("answer")).toString().trimmed();
        if (!ownSession(sessionId) || questionId.isEmpty() || questionId.size() > 256 ||
            answer.isEmpty() || answer.size() > kMaxPromptChars) {
            server->respondError(connectionId, requestId, QStringLiteral("bad_answer"));
            return;
        }
        bool found = false;
        bool sensitive = false;
        const auto events = m_streamEvents.constFind(sessionId);
        if (events != m_streamEvents.constEnd()) {
            for (const agentops::PendingQuestion &question : agentops::questionsInEvents(*events)) {
                if (question.requestId != questionId)
                    continue;
                found = true;
                for (const QJsonValue &item : question.input.value(QStringLiteral("questions")).toArray())
                    sensitive = sensitive || item.toObject().value(QStringLiteral("isSecret")).toBool();
                break;
            }
        }
        if (!found) {
            server->respondError(connectionId, requestId, QStringLiteral("unknown_question"));
            return;
        }
        audit(QStringLiteral("Answering agent question for %1").arg(sessionId));
        answerAgentQuestion(sessionId, questionId, answer, sensitive);
        server->respond(connectionId, requestId, {});
        notifyLocalControlAgentsChanged();
        return;
    }
    if (method == QLatin1String("agents.stop")) {
        audit(QStringLiteral("Stopping agent %1").arg(sessionId));
        if (!ownSession(sessionId) || !stopAgentSessionById(sessionId)) {
            server->respondError(connectionId, requestId,
                                 QStringLiteral("not_running"));
            return;
        }
        server->respond(connectionId, requestId, {});
        notifyLocalControlAgentsChanged();
        return;
    }
    if (method == QLatin1String("agents.queue")) {
        server->respond(connectionId, requestId, agentQueuePayload());
        return;
    }
    if (method == QLatin1String("agents.queue.limit")) {
        const int limit = params.value(QStringLiteral("limit")).toInt();
        if (limit < kMinMaxRunningAgents || limit > 99) {
            server->respondError(connectionId, requestId, QStringLiteral("bad_limit"));
            return;
        }
        audit(QStringLiteral("Setting the concurrent-agent limit to %1").arg(limit));
        setAgentConcurrencyLimit(limit);
        server->respond(connectionId, requestId, agentQueuePayload());
        notifyLocalControlAgentsChanged();
        return;
    }
    if (method == QLatin1String("agents.markRead")) {
        if (!ownSession(sessionId)) {
            server->respondError(connectionId, requestId, QStringLiteral("unknown_session"));
            return;
        }
        markAgentSessionRead(sessionId);
        server->respond(connectionId, requestId, {});
        return;
    }
    if (method == QLatin1String("agents.queue.move")) {
        const int to = params.value(QStringLiteral("to")).toInt(-1);
        if (!ownSession(sessionId) || to < 0) {
            server->respondError(connectionId, requestId, QStringLiteral("not_queued"));
            return;
        }
        audit(QStringLiteral("Moving queued agent %1 to place %2").arg(sessionId).arg(to + 1));
        if (!moveQueuedAgentSession(sessionId, to)) {
            server->respondError(connectionId, requestId, QStringLiteral("not_queued"));
            return;
        }
        server->respond(connectionId, requestId, agentQueuePayload());
        notifyLocalControlAgentsChanged();
        return;
    }
    if (method == QLatin1String("agents.continue")) {
        AgentSession *session = ownSession(sessionId);
        if (!session || sessionActive(*session)) {
            server->respondError(connectionId, requestId,
                                 QStringLiteral("not_continuable"));
            return;
        }
        audit(QStringLiteral("Continuing agent %1").arg(sessionId));
        continueAgentSession(sessionId);
        server->respond(connectionId, requestId, {});
        notifyLocalControlAgentsChanged();
        return;
    }
    server->respondError(connectionId, requestId, QStringLiteral("unsupported"));
}

void MainWindow::applyLocalControlAgentChoice(int sessionId, const QJsonObject &params)
{
    AgentSession *session = findAgentSession(sessionId);
    if (!session)
        return;
    bool changed = false;
    bool modelChanged = false;
    const QString provider = params.value(QStringLiteral("provider")).toString().trimmed();
    if ((provider == kClaudeCodeProvider || agentIsCodexProvider(provider)) &&
        handOffAgentSessionToProvider(sessionId, provider)) {
        session = findAgentSession(sessionId);
        if (!session)
            return;
        changed = true;
    }
    const QString mode = localControlAgentMode(params);
    if (!mode.isEmpty() && session->mode != mode) {
        session->mode = mode;
        changed = true;
    }
    QString model = params.value(QStringLiteral("model")).toString().trimmed();
    if (!model.isEmpty() && (session->provider == QLatin1String("claude-code") ||
                             agentIsCodexProvider(session->provider)))
        model = codexChatGptModelId(model);
    if (!model.isEmpty() && session->model != model &&
        agentModelMatchesProvider(session->provider, model)) {
        session->model = model;
        changed = true;
        modelChanged = true;
    }
    if (!changed)
        return;
    if (m_agentStore)
        m_agentStore->saveSession(*session);
    if (modelChanged && !isStoppableAgentSession(sessionId)) {
        if (ClaudeStreamSession *idle = m_streamSessions.take(sessionId)) {
            idle->stop();
            idle->deleteLater();
        }
    }
    if (sessionId == m_selectedAgentSessionId)
        showAgentSession(sessionId);
}

QWidget *MainWindow::buildLocalControlTab()
{
    auto *body = new QWidget;
    auto *col = new QVBoxLayout(body);
    col->setContentsMargins(2, 14, 2, 14);
    col->setSpacing(10);

    auto *head = new QLabel(QStringLiteral("PHONE ON THIS NETWORK"));
    head->setObjectName("sectionLabel");

    auto *hint = new QLabel(QStringLiteral(
        "Open ForkMesh on your phone while it is on the same Wi-Fi as this "
        "desktop and it finds this machine by itself. A phone signed in to "
        "the same ForkMesh account as this desktop is let in without a code; "
        "any other phone is approved here once. From then on the phone can "
        "watch, start, steer, stop and continue "
        "agents, manage your hosts and watch their logs, and send files straight "
        "to this desktop — directly over the local network, encrypted end to "
        "end. Away from home an approved phone reaches this desktop through the "
        "relay instead, still encrypted end to end so the relay cannot read or "
        "forge anything; a phone on another account is only ever approved "
        "here, on this network, and every change a phone makes is written to "
        "the log. A web browser signed in to your ForkMesh account can do the "
        "same from a repository's Agents tab, through the relay; it is checked "
        "against your account each time it connects and never saved here."));
    hint->setObjectName("statusLine");
    hint->setWordWrap(true);

    m_localControlStatus = new QLabel;
    m_localControlStatus->setObjectName("modeHint");
    m_localControlStatus->setWordWrap(true);

    m_localControlList = new QListWidget;
    m_localControlList->setObjectName("localControlDevices");
    m_localControlList->setMinimumHeight(90);
    m_localControlList->setMaximumHeight(180);

    m_localControlSendButton = new QPushButton(QStringLiteral("Send file to phone…"));
    m_localControlSendButton->setObjectName("primaryButton");
    m_localControlSendButton->setCursor(Qt::PointingHandCursor);
    connect(m_localControlSendButton, &QPushButton::clicked, this,
            &MainWindow::sendFileToLocalPhone);

    m_localControlForgetButton = new QPushButton(QStringLiteral("Remove phone"));
    m_localControlForgetButton->setCursor(Qt::PointingHandCursor);
    m_localControlForgetButton->setToolTip(QStringLiteral(
        "Disconnect the selected phone and forget its approval. It has to be "
        "approved again before it can control this desktop."));
    connect(m_localControlForgetButton, &QPushButton::clicked, this, [this] {
        QListWidgetItem *item =
            m_localControlList ? m_localControlList->currentItem() : nullptr;
        if (!item || !m_localControl)
            return;
        m_localControl->forget(item->data(Qt::UserRole).toString());
        refreshLocalControlTab();
    });

    auto *openFolder = new QPushButton(QStringLiteral("Open received files"));
    openFolder->setCursor(Qt::PointingHandCursor);
    connect(openFolder, &QPushButton::clicked, this, [] {
        const QString dir = receivedFilesDirectory();
        QDir().mkpath(dir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });

    connect(m_localControlList, &QListWidget::currentItemChanged, this,
            [this] { refreshLocalControlTab(); });

    auto *buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->addWidget(m_localControlSendButton);
    buttons->addWidget(m_localControlForgetButton);
    buttons->addWidget(openFolder);
    buttons->addStretch();

    col->addWidget(head);
    col->addWidget(hint);
    col->addWidget(m_localControlStatus);
    col->addWidget(m_localControlList);
    col->addLayout(buttons);
    auto *sendNotice = new QLabel;
    sendNotice->setObjectName(QStringLiteral("localControlSendNotice"));
    sendNotice->setWordWrap(true);
    sendNotice->hide();
    col->addWidget(sendNotice);
#if defined(Q_OS_UNIX)
    auto *termHead = new QLabel(QStringLiteral("TYPE INTO A TERMINAL FROM YOUR PHONE"));
    termHead->setObjectName("sectionLabel");
    auto *termHint = new QLabel(QStringLiteral(
        "Start Claude Code or Codex with \"forkmesh term\" in front, for example "
        "\"forkmesh term claude\", and an approved phone can type prompts into that "
        "terminal as if you typed them, and stop the agent (it presses Esc, their "
        "interrupt key). Each time, the terminal shows which phone typed and this "
        "desktop logs it. Ctrl-^ in the terminal turns phone typing off and on for that "
        "session; \"forkmesh term --no-remote claude\" starts with it off. To use it "
        "every time, add one of these lines to your ~/.bashrc or ~/.zshrc yourself; "
        "ForkMesh never edits your shell files."));
    termHint->setObjectName("statusLine");
    termHint->setWordWrap(true);
    col->addSpacing(8);
    col->addWidget(termHead);
    col->addWidget(termHint);
    for (const QString &cli : {QStringLiteral("claude"), QStringLiteral("codex")}) {
        auto *line = new QLineEdit(TerminalHandoff::aliasLine(cli));
        line->setReadOnly(true);
        line->setObjectName(QStringLiteral("terminalAlias_%1").arg(cli));
        auto *copy = new QPushButton(QStringLiteral("Copy"));
        copy->setCursor(Qt::PointingHandCursor);
        const QString text = line->text();
        connect(copy, &QPushButton::clicked, this, [text] {
            if (QClipboard *clipboard = QApplication::clipboard())
                clipboard->setText(text);
        });
        auto *row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->addWidget(line, 1);
        row->addWidget(copy);
        col->addLayout(row);
    }
#endif
    col->addStretch();
    refreshLocalControlTab();
    return body;
}

void MainWindow::refreshLocalControlTab()
{
    if (!m_localControlList || !m_localControlStatus)
        return;
    const QString selected = m_localControlList->currentItem()
                                 ? m_localControlList->currentItem()
                                       ->data(Qt::UserRole)
                                       .toString()
                                 : QString();
    const QSignalBlocker blocker(m_localControlList);
    m_localControlList->clear();
    if (!m_localControl) {
        m_localControlStatus->setText(QStringLiteral(
            "The local network is not available, so no phone can connect."));
        m_localControlSendButton->setEnabled(false);
        m_localControlForgetButton->setEnabled(false);
        return;
    }
    const QList<LocalControlServer::Device> connected =
        m_localControl->connectedDevices();
    QList<LocalControlServer::Device> rows = m_localControl->pairedDevices();
    for (const LocalControlServer::Device &device : connected)
        if (!m_localControl->isPaired(device.key))
            rows.append(device);
    int online = 0;
    for (const LocalControlServer::Device &device : std::as_const(rows)) {
        const bool paired = m_localControl->isPaired(device.key);
        const bool live = m_localControl->isConnected(device.key);
        online += live ? 1 : 0;
        QString state;
        if (!paired)
            state = QStringLiteral("waiting for approval");
        else if (live)
            state = QStringLiteral("connected");
        else if (device.lastSeenMs > 0)
            state = QStringLiteral("last seen %1")
                        .arg(QDateTime::fromMSecsSinceEpoch(device.lastSeenMs)
                                 .toString(QStringLiteral("d MMM, HH:mm")));
        else
            state = QStringLiteral("not connected");
        auto *item = new QListWidgetItem(
            QStringLiteral("%1 %2  —  %3")
                .arg(live ? QStringLiteral("●") : QStringLiteral("○"),
                     deviceLabel(device), state));
        item->setData(Qt::UserRole, device.key);
        item->setToolTip(QStringLiteral("Device key %1").arg(device.key));
        m_localControlList->addItem(item);
        if (device.key == selected)
            m_localControlList->setCurrentItem(item);
    }
    if (!m_localControlList->currentItem() && m_localControlList->count() > 0)
        m_localControlList->setCurrentRow(0);
    const QString current = m_localControlList->currentItem()
                                ? m_localControlList->currentItem()
                                      ->data(Qt::UserRole)
                                      .toString()
                                : QString();
    m_localControlSendButton->setEnabled(!current.isEmpty() &&
                                         m_localControl->isConnected(current));
    m_localControlForgetButton->setEnabled(!current.isEmpty() &&
                                           m_localControl->isPaired(current));
    if (rows.isEmpty())
        m_localControlStatus->setText(QStringLiteral(
            "No phone yet. Open ForkMesh on your phone on this Wi-Fi and "
            "choose Desktop on this network."));
    else if (online > 0)
        m_localControlStatus->setText(
            QStringLiteral("%1 phone%2 connected over the local network.")
                .arg(online)
                .arg(online == 1 ? QString() : QStringLiteral("s")));
    else
        m_localControlStatus->setText(QStringLiteral(
            "No phone is connected right now. An approved phone reconnects on "
            "its own when it is on this network with ForkMesh open."));
}

void MainWindow::sendFileToLocalPhone()
{
    if (!m_localControl || !m_localControlList || !m_localControlList->currentItem())
        return;
    const QString key =
        m_localControlList->currentItem()->data(Qt::UserRole).toString();
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Send a file to your phone"));
    if (path.isEmpty())
        return;
    QWidget *panel = m_localControlSendButton
                         ? m_localControlSendButton->parentWidget()
                         : nullptr;
    QLabel *notice = panel ? panel->findChild<QLabel *>(
                                 QStringLiteral("localControlSendNotice"))
                           : nullptr;
    QString error;
    if (!m_localControl->sendFile(key, path, &error)) {
        showLocalNotice(notice,
                        QStringLiteral("Could not send the file: %1").arg(error),
                        true);
        return;
    }
    showLocalNotice(notice, QStringLiteral("Sending %1 to your phone.")
                                .arg(QFileInfo(path).fileName()));
}

QString MainWindow::localControlOutsideTranscriptPath(int id) const
{
    if (const auto surfaced = m_externalSurfaced.constFind(id);
        surfaced != m_externalSurfaced.constEnd() && externalIsLive(surfaced->uuid))
        return surfaced->path;
    if (const auto machine = m_machineAgents.constFind(id); machine != m_machineAgents.constEnd())
        return machine->transcript;
    return QString();
}

void MainWindow::readLocalControlOwnTranscript(
    int sessionId, qint64 since, forkmesh::localcontrol::LogTail tail,
    std::function<void(const QJsonObject &payload, const QString &error,
                       forkmesh::localcontrol::LogTail tail)>
        done)
{
    AgentSession *session = findAgentSession(sessionId);
    if (sessionId <= 0 || isExternalSession(sessionId) || !session || session->cloud ||
        session->deletedAtMs != 0 || !m_agentStore) {
        done({}, QStringLiteral("unknown_session"), {});
        return;
    }
    AgentStore *store = m_agentStore;
    const AgentSession copy = *session;
    const bool active = sessionActive(*session);
    const QString status = session->status;
    const QStringList known = agentSessionImages(sessionId);
    struct Read {
        QString text;
        qint64 base = 0;
        forkmesh::localcontrol::LogTail tail;
        QJsonArray attachments;
        bool listed = false;
    };
    runOffThread<Read>(
        [store, copy, known, sessionId, since, tail] {
            Read read;
            read.tail = tail;
            store->flushLog(copy);
            const QString path = store->logPath(copy);
            bool rewritten = false;
            if (read.tail.bytes > 0 && since == read.tail.chars) {
                read.text = forkmesh::localcontrol::readLogGrowth(path, read.tail, &rewritten);
                read.base = rewritten ? 0 : since;
            } else {
                read.tail = {};
                read.text = forkmesh::localcontrol::readLogGrowth(path, read.tail);
            }
            read.listed = read.base == 0 || !AgentStore::attachmentPathsIn(read.text).isEmpty();
            if (read.listed)
                read.attachments = localcontrolattachments::toJson(localcontrolattachments::entries(
                    localControlAttachmentPaths(*store, copy, known), sessionId,
                    localcontrolattachments::processSalt()));
            return read;
        },
        [this, store, sessionId, since, active, status, done](Read read) {
            if (!m_localControl || m_agentStore != store) {
                done({}, QStringLiteral("unknown_session"), {});
                return;
            }
            const qint64 total = read.tail.chars;
            qint64 from = read.base;
            bool reset = false;
            if (since >= read.base && since <= total) {
                from = since;
                if (total - from > kMaxTranscriptChars) {
                    from = total - kMaxTranscriptChars;
                    reset = true;
                }
            } else {
                reset = true;
                from = std::max(read.base, total - kMaxTranscriptChars);
            }
            QJsonObject payload{{QStringLiteral("id"), sessionId},
                                {QStringLiteral("transcript"),
                                 redactProviderCredentials(read.text.mid(from - read.base),
                                                           localProviderCredentialValues())},
                                {QStringLiteral("from"), double(from)},
                                {QStringLiteral("next"), double(total)},
                                {QStringLiteral("reset"), reset},
                                {QStringLiteral("active"), active},
                                {QStringLiteral("status"), status}};
            if (read.listed)
                payload.insert(QStringLiteral("attachments"), read.attachments);
            done(payload, QString(), read.tail);
        });
}

void MainWindow::handleLocalControlFollowRequest(int connectionId, int requestId,
                                                 const QString &method,
                                                 const QJsonObject &params)
{
    const int sessionId = params.value(QStringLiteral("id")).toInt();
    if (method == QLatin1String("agents.unfollow")) {
        m_localControlFollows.removeIf([connectionId, sessionId](const LocalControlFollow &f) {
            return f.connectionId == connectionId && f.sessionId == sessionId;
        });
        m_localControl->respond(connectionId, requestId, {});
        return;
    }
    if (sessionId == 0) {
        m_localControl->respondError(connectionId, requestId, QStringLiteral("unknown_session"));
        return;
    }
    m_localControlFollows.removeIf([connectionId, sessionId](const LocalControlFollow &f) {
        return f.connectionId == connectionId && f.sessionId == sessionId;
    });
    int held = 0;
    for (const LocalControlFollow &f : std::as_const(m_localControlFollows))
        held += f.connectionId == connectionId ? 1 : 0;
    if (held >= kMaxFollowsPerConnection) {
        for (qsizetype i = 0; i < m_localControlFollows.size(); ++i) {
            if (m_localControlFollows.at(i).connectionId == connectionId &&
                !m_localControlFollows.at(i).busy) {
                m_localControlFollows.removeAt(i);
                break;
            }
        }
    }
    LocalControlFollow follow;
    follow.connectionId = connectionId;
    follow.sessionId = sessionId;
    follow.next = std::max<qint64>(0, qint64(params.value(QStringLiteral("since")).toDouble()));
    follow.busy = true;
    follow.readAtMs = QDateTime::currentMSecsSinceEpoch();
    m_localControlFollows.append(follow);
    AgentStore::setLogObserver(this, [this](int id) { markLocalControlFollowDirty(id); });
    if (sessionId < 0)
        watchLocalControlOutsideFollows();
    startLocalControlFollowRead(connectionId, sessionId, requestId);
}

void MainWindow::startLocalControlFollowRead(int connectionId, int sessionId, int requestId)
{
    qint64 since = 0;
    forkmesh::localcontrol::LogTail tail;
    for (const LocalControlFollow &f : std::as_const(m_localControlFollows)) {
        if (f.connectionId == connectionId && f.sessionId == sessionId) {
            since = f.next;
            tail = f.tail;
        }
    }
    const auto finish = [this, connectionId, sessionId, requestId](
                            const QJsonObject &payload, const QString &error,
                            forkmesh::localcontrol::LogTail tail) {
        if (!m_localControl)
            return;
        auto it = std::find_if(m_localControlFollows.begin(), m_localControlFollows.end(),
                               [connectionId, sessionId](const LocalControlFollow &f) {
                                   return f.connectionId == connectionId &&
                                          f.sessionId == sessionId;
                               });
        if (!error.isEmpty()) {
            if (it != m_localControlFollows.end())
                m_localControlFollows.erase(it);
            if (requestId > 0)
                m_localControl->respondError(connectionId, requestId, error);
            else
                m_localControl->sendEventTo(connectionId, QStringLiteral("agents.output"),
                                            {{QStringLiteral("id"), sessionId},
                                             {QStringLiteral("error"), error}});
            return;
        }
        if (it == m_localControlFollows.end()) {
            if (requestId > 0)
                m_localControl->respond(connectionId, requestId, payload);
            return;
        }
        it->busy = false;
        it->tail = tail;
        const qint64 next = qint64(payload.value(QStringLiteral("next")).toDouble());
        const QString status = payload.value(QStringLiteral("status")).toString();
        const bool active = payload.value(QStringLiteral("active")).toBool();
        const bool changed = requestId > 0 || payload.value(QStringLiteral("reset")).toBool() ||
                             next != it->next || status != it->status || active != it->active;
        it->next = next;
        it->status = status;
        it->active = active;
        const bool dirty = it->dirty;
        if (requestId > 0) {
            QJsonObject answer = payload;
            answer.insert(QStringLiteral("streaming"), true);
            m_localControl->respond(connectionId, requestId, answer);
        } else if (changed &&
                   !m_localControl->sendEventTo(connectionId, QStringLiteral("agents.output"),
                                                payload)) {
            dropLocalControlFollows(connectionId);
            return;
        }
        if (dirty)
            scheduleLocalControlFollowPump();
    };
    if (sessionId < 0) {
        readLocalControlOutsideTranscript(
            sessionId, since, [finish](const QJsonObject &payload, const QString &error) {
                finish(payload, error, {});
            });
        return;
    }
    readLocalControlOwnTranscript(sessionId, since, tail, finish);
}

void MainWindow::markLocalControlFollowDirty(int sessionId)
{
    bool any = false;
    for (LocalControlFollow &f : m_localControlFollows) {
        if (f.sessionId == sessionId) {
            f.dirty = true;
            any = true;
        }
    }
    if (any)
        scheduleLocalControlFollowPump();
}

void MainWindow::markLocalControlFollowsDirty()
{
    if (m_localControlFollows.isEmpty())
        return;
    for (LocalControlFollow &f : m_localControlFollows)
        f.dirty = true;
    scheduleLocalControlFollowPump();
}

void MainWindow::scheduleLocalControlFollowPump()
{
    if (!m_localControlFollowTimer) {
        m_localControlFollowTimer = new QTimer(this);
        m_localControlFollowTimer->setSingleShot(true);
        connect(m_localControlFollowTimer, &QTimer::timeout, this,
                &MainWindow::pumpLocalControlFollows);
    }
    if (!m_localControlFollowTimer->isActive())
        m_localControlFollowTimer->start(0);
}

void MainWindow::pumpLocalControlFollows()
{
    if (!m_localControl) {
        m_localControlFollows.clear();
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    qint64 wait = -1;
    const auto later = [&wait](qint64 ms) { wait = wait < 0 ? ms : std::min(wait, ms); };
    QList<QPair<int, int>> due;
    for (LocalControlFollow &f : m_localControlFollows) {
        if (!f.dirty || f.busy)
            continue;
        const bool relayed = m_localControl->isRelayed(f.connectionId);
        const qint64 remaining =
            f.readAtMs + forkmesh::localcontrol::followIntervalMs(relayed) - now;
        if (remaining > 0) {
            later(remaining);
            continue;
        }
        if (relayed && m_nodeEventSocket && m_nodeEventSocket->controlBacklog() > 0) {
            later(forkmesh::localcontrol::followIntervalMs(true));
            continue;
        }
        f.dirty = false;
        f.busy = true;
        f.readAtMs = now;
        due.append({f.connectionId, f.sessionId});
    }
    for (const auto &[connectionId, sessionId] : std::as_const(due))
        startLocalControlFollowRead(connectionId, sessionId, 0);
    if (wait >= 0 && m_localControlFollowTimer && !m_localControlFollowTimer->isActive())
        m_localControlFollowTimer->start(int(wait));
}

void MainWindow::watchLocalControlOutsideFollows()
{
    if (!m_localControlFollowWatchTimer) {
        m_localControlFollowWatchTimer = new QTimer(this);
        m_localControlFollowWatchTimer->setInterval(kOutsideFollowWatchMs);
        connect(m_localControlFollowWatchTimer, &QTimer::timeout, this, [this] {
            bool watching = false;
            for (LocalControlFollow &f : m_localControlFollows) {
                if (f.sessionId >= 0)
                    continue;
                watching = true;
                const QString path = localControlOutsideTranscriptPath(f.sessionId);
                const qint64 size = path.isEmpty() ? -2 : QFileInfo(path).size();
                if (size != f.seenSize) {
                    f.seenSize = size;
                    f.dirty = true;
                }
            }
            if (!watching) {
                m_localControlFollowWatchTimer->stop();
                return;
            }
            scheduleLocalControlFollowPump();
        });
    }
    if (!m_localControlFollowWatchTimer->isActive())
        m_localControlFollowWatchTimer->start();
}

void MainWindow::dropLocalControlFollows(int connectionId)
{
    m_localControlFollows.removeIf(
        [connectionId](const LocalControlFollow &f) { return f.connectionId == connectionId; });
}
