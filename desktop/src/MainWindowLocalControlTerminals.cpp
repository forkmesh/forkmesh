#include "AgentCliLocator.h"
#include "AgentLoginPty.h"
#include "ControlNode.h"
#include "LocalControl.h"
#include "LocalControlStream.h"
#include "LocalControlTerminals.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "NodeEventSocket.h"
#include "TerminalHandoff.h"
#include "VirtualMachineRuntime.h"

#if defined(FORKMESH_HAVE_TERM)
#include "ForkMeshTerm.h"
#endif

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>

namespace terminals = forkmesh::localcontrol::terminals;

namespace {

QString terminalRowKey(const QString &id)
{
    return QStringLiteral("terminal:") + id;
}

} // namespace

void MainWindow::handleLocalControlTerminalRequest(int connectionId, int requestId,
                                                   const QString &method,
                                                   const QJsonObject &params)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    if (method == QLatin1String("terminal.start")) {
        startLocalControlTerminal(connectionId, requestId, params);
        return;
    }
    if (method == QLatin1String("terminal.resume")) {
        resumeLocalControlTerminal(connectionId, requestId, params);
        return;
    }
    if (method == QLatin1String("terminal.list")) {
        QJsonArray list;
        for (const terminals::Terminal &terminal : std::as_const(m_localControlTerminals))
            list.append(terminals::terminalJson(terminal));
        server->respond(connectionId, requestId, {{QStringLiteral("terminals"), list}});
        return;
    }
    const QString id = params.value(QStringLiteral("terminal")).toString();
    auto it = terminals::validTerminalId(id) ? m_localControlTerminals.find(id)
                                             : m_localControlTerminals.end();
    if (it == m_localControlTerminals.end() || !it->pty) {
        server->respondError(connectionId, requestId, QStringLiteral("unknown_terminal"));
        return;
    }
    const int cols = params.value(QStringLiteral("cols")).toInt();
    const int rows = params.value(QStringLiteral("rows")).toInt();
    if (method == QLatin1String("terminal.attach")) {
        attachLocalControlTerminal(connectionId, requestId, *it, cols, rows);
        return;
    }
    if (method == QLatin1String("terminal.input")) {
        QString error;
        const QByteArray bytes =
            terminals::decodeInput(params.value(QStringLiteral("data")), &error);
        if (!error.isEmpty()) {
            server->respondError(connectionId, requestId, error);
            return;
        }
        it->pty->write(bytes);
        server->respond(connectionId, requestId, {});
        return;
    }
    if (method == QLatin1String("terminal.resize")) {
        if (cols <= 0 || rows <= 0) {
            server->respondError(connectionId, requestId, QStringLiteral("bad_request"));
            return;
        }
        it->cols = terminals::clampCols(cols);
        it->rows = terminals::clampRows(rows);
        it->pty->resize(it->cols, it->rows);
        server->respond(connectionId, requestId, {});
        return;
    }
    if (method == QLatin1String("terminal.detach")) {
        it->viewers.remove(connectionId);
        server->respond(connectionId, requestId, {});
        return;
    }
    if (method == QLatin1String("terminal.close")) {
        logSystem(QStringLiteral("Closed the terminal running %1 in %2, requested by %3.")
                      .arg(terminals::terminalLabel(*it), it->cwd,
                           localControlDeviceLabel(connectionId)));
        server->respond(connectionId, requestId, {});
        endLocalControlTerminal(id, -1);
        return;
    }
    server->respondError(connectionId, requestId, QStringLiteral("unsupported"));
}

void MainWindow::attachLocalControlTerminal(int connectionId, int requestId,
                                            terminals::Terminal &terminal, int cols, int rows)
{
    if (cols > 0 && rows > 0) {
        terminal.cols = terminals::clampCols(cols);
        terminal.rows = terminals::clampRows(rows);
        terminal.pty->resize(terminal.cols, terminal.rows);
    }
    terminal.viewers.insert(connectionId, terminals::Outbox{});
    QJsonObject answer = terminals::terminalJson(terminal);
    answer.insert(QStringLiteral("replay"), QString::fromLatin1(terminal.replay.toBase64()));
    m_localControl->respond(connectionId, requestId, answer);
    logSystem(QStringLiteral("Opened the terminal running %1 in %2 for %3.")
                  .arg(terminals::terminalLabel(terminal), terminal.cwd,
                       localControlDeviceLabel(connectionId)));
}

void MainWindow::startLocalControlTerminal(int connectionId, int requestId,
                                           const QJsonObject &params)
{
    LocalControlServer *server = m_localControl;
#if !defined(FORKMESH_HAVE_TERM)
    Q_UNUSED(params)
    server->respondError(connectionId, requestId, QStringLiteral("unsupported"));
#else
    const QString provider = params.value(QStringLiteral("provider")).toString().trimmed();
    const QString program = terminals::cliProgram(provider);
    if (program.isEmpty()) {
        server->respondError(connectionId, requestId, QStringLiteral("unsupported_provider"));
        return;
    }
    const QString mode = params.value(QStringLiteral("mode")).toString().trimmed();
    if (!terminals::validMode(mode)) {
        server->respondError(connectionId, requestId, QStringLiteral("bad_request"));
        return;
    }
    const bool signIn = mode == QLatin1String("login") || mode == QLatin1String("logout");
    const QString accountId = params.value(QStringLiteral("account")).toString().trimmed();
    forkmesh::ui::AgentAccountProfile account = forkmesh::ui::activeAgentAccount(provider);
    if (!accountId.isEmpty()) {
        const QList<forkmesh::ui::AgentAccountProfile> profiles =
            forkmesh::ui::agentAccountProfiles(provider);
        const auto found = std::find_if(profiles.cbegin(), profiles.cend(),
                                        [&accountId](const forkmesh::ui::AgentAccountProfile &profile) {
                                            return profile.id == accountId;
                                        });
        if (found == profiles.cend()) {
            server->respondError(connectionId, requestId, QStringLiteral("unknown_account"));
            return;
        }
        account = *found;
    }
    const QString owner = params.value(QStringLiteral("owner")).toString().trimmed();
    const QString name = params.value(QStringLiteral("name")).toString().trimmed();
    const RepositoryRecord *repo = nullptr;
    for (const RepositoryRecord &record : std::as_const(m_repositories)) {
        if (record.localPath.isEmpty() || !QDir(record.localPath).exists())
            continue;
        if (record.owner.compare(owner, Qt::CaseInsensitive) == 0 &&
            record.name.compare(name, Qt::CaseInsensitive) == 0) {
            repo = &record;
            break;
        }
    }
    if (!signIn && (owner.isEmpty() || name.isEmpty() || !repo)) {
        server->respondError(connectionId, requestId, QStringLiteral("unknown_repository"));
        return;
    }
    if (!signIn && repo->agentGuardrailsEnabled) {
        server->respondError(connectionId, requestId, QStringLiteral("guardrails_on"));
        return;
    }
    if (!signIn && forkmesh::vm::active()) {
        server->respondError(connectionId, requestId, QStringLiteral("terminal_vm"));
        return;
    }
    if (m_localControlTerminals.size() >= terminals::kMaxTerminals) {
        server->respondError(connectionId, requestId, QStringLiteral("terminal_limit"));
        return;
    }
    const QString resolved = forkmesh::agentcli::executable(program);
    if (resolved.isEmpty()) {
        server->respondError(connectionId, requestId, QStringLiteral("cli_missing"));
        return;
    }
    terminals::Terminal terminal;
    terminal.id = terminals::newTerminalId();
    terminal.provider = provider;
    terminal.cli = program;
    terminal.owner = repo ? repo->owner : QString();
    terminal.name = repo ? repo->name : QString();
    terminal.cwd = QDir::cleanPath(repo ? repo->localPath : QDir::homePath());
    terminal.device = server->deviceFor(connectionId).name;
    terminal.mode = signIn ? mode : QString();
    terminal.account = account.id;
    terminal.startedAtMs = QDateTime::currentMSecsSinceEpoch();
    terminal.cols = terminals::clampCols(params.value(QStringLiteral("cols")).toInt(80));
    terminal.rows = terminals::clampRows(params.value(QStringLiteral("rows")).toInt(24));
    terminal.paste = std::make_shared<forkmesh::term::PasteModeTracker>();
    auto *pty = new AgentLoginPty(this);
    if (!account.builtIn)
        pty->setEnvironment({{terminals::accountEnvVariable(provider), account.configDir}});
    const QStringList cliArgs = terminals::cliArguments(provider, mode);
    const QString what = signIn ? QStringLiteral("%1 %2 for %3")
                                      .arg(terminals::cliLabel(provider),
                                           mode == QLatin1String("login")
                                               ? QStringLiteral("sign-in")
                                               : QStringLiteral("sign-out"),
                                           account.label)
                                : QStringLiteral("%1 as %2 with full permissions")
                                      .arg(terminals::cliLabel(provider), account.label);
    QString error;
    const bool ran = signIn ? pty->run(resolved, cliArgs, terminal.cwd, terminal.cols,
                                       terminal.rows, true, &error)
                            : pty->run(QCoreApplication::applicationFilePath(),
                                       QStringList{QStringLiteral("term"), resolved} + cliArgs,
                                       terminal.cwd, terminal.cols, terminal.rows, false, &error);
    if (!ran) {
        pty->deleteLater();
        logSystem(QStringLiteral("Could not start %1 in a terminal in %2 for %3: %4")
                      .arg(what, terminal.cwd, localControlDeviceLabel(connectionId), error));
        server->respondError(connectionId, requestId, QStringLiteral("terminal_failed"));
        return;
    }
    terminal.pty = pty;
    terminal.pid = pty->childPid();
    terminal.viewers.insert(connectionId, terminals::Outbox{});
    const QString id = terminal.id;
    connect(pty, &AgentLoginPty::output, this,
            [this, id](const QByteArray &chunk) { onLocalControlTerminalOutput(id, chunk); });
    connect(pty, &AgentLoginPty::finished, this,
            [this, id](int code) { onLocalControlTerminalFinished(id, code); });
    m_localControlTerminals.insert(id, terminal);
    QJsonObject answer = terminals::terminalJson(terminal);
    answer.insert(QStringLiteral("replay"), QString());
    server->respond(connectionId, requestId, answer);
    logSystem(QStringLiteral("Started %1 in a terminal in %2, requested by %3.")
                  .arg(what, terminal.cwd, localControlDeviceLabel(connectionId)));
    if (!signIn)
        notifyLocalControlAgentsChanged();
#endif
}

void MainWindow::resumeLocalControlTerminal(int connectionId, int requestId,
                                            const QJsonObject &params)
{
    LocalControlServer *server = m_localControl;
#if !defined(FORKMESH_HAVE_TERM)
    Q_UNUSED(params)
    server->respondError(connectionId, requestId, QStringLiteral("unsupported"));
#else
    const int sessionId = params.value(QStringLiteral("id")).toInt();
    const int cols = params.value(QStringLiteral("cols")).toInt();
    const int rows = params.value(QStringLiteral("rows")).toInt();
    const AgentSession *live =
        sessionId > 0 && !isExternalSession(sessionId) ? findAgentSession(sessionId) : nullptr;
    if (!live || live->deletedAtMs > 0 || live->associationOnly) {
        server->respondError(connectionId, requestId, QStringLiteral("unknown_session"));
        return;
    }
    if (const QString open = localControlTerminalForRow(sessionId); !open.isEmpty()) {
        attachLocalControlTerminal(connectionId, requestId, m_localControlTerminals[open], cols,
                                   rows);
        return;
    }
    const QString provider =
        forkmesh::ui::agentIsCodexProvider(live->provider) ? QStringLiteral("codex") : live->provider;
    const QString owner = live->owner;
    const QString name = live->name;
    if (forkmesh::vm::active()) {
        server->respondError(connectionId, requestId, QStringLiteral("terminal_vm"));
        return;
    }
    if (m_localControlTerminals.size() >= terminals::kMaxTerminals) {
        server->respondError(connectionId, requestId, QStringLiteral("terminal_limit"));
        return;
    }
    const AgentTerminalHandoff plan = agentTerminalHandoff(sessionId);
    if (!plan.ok) {
        server->respondError(connectionId, requestId,
                             plan.code.isEmpty() ? QStringLiteral("terminal_failed") : plan.code);
        return;
    }
    terminals::Terminal terminal;
    terminal.id = terminals::newTerminalId();
    terminal.provider = provider;
    terminal.cli = terminals::cliProgram(provider);
    terminal.owner = owner;
    terminal.name = name;
    terminal.cwd = QDir::cleanPath(plan.cwd);
    terminal.session = sessionId;
    terminal.device = server->deviceFor(connectionId).name;
    terminal.startedAtMs = QDateTime::currentMSecsSinceEpoch();
    terminal.cols = terminals::clampCols(cols > 0 ? cols : 80);
    terminal.rows = terminals::clampRows(rows > 0 ? rows : 24);
    terminal.paste = std::make_shared<forkmesh::term::PasteModeTracker>();
    const bool stopped = stopAgentSessionById(sessionId);
    auto *pty = new AgentLoginPty(this);
    pty->setEnvironment(plan.env);
    QString error;
    if (!pty->run(QCoreApplication::applicationFilePath(),
                  QStringList{QStringLiteral("term"), plan.program} +
                      terminals::cliArguments(provider, QStringLiteral("agent")) + plan.args,
                  terminal.cwd,
                  terminal.cols, terminal.rows, false, &error)) {
        pty->deleteLater();
        logSystem(QStringLiteral("Could not open agent %1 in %2 in a terminal for %3: %4")
                      .arg(sessionId)
                      .arg(terminals::cliLabel(provider), terminal.cwd,
                           localControlDeviceLabel(connectionId), error));
        server->respondError(connectionId, requestId, QStringLiteral("terminal_failed"));
        return;
    }
    terminal.pty = pty;
    terminal.pid = pty->childPid();
    terminal.viewers.insert(connectionId, terminals::Outbox{});
    const QString id = terminal.id;
    connect(pty, &AgentLoginPty::output, this,
            [this, id](const QByteArray &chunk) { onLocalControlTerminalOutput(id, chunk); });
    connect(pty, &AgentLoginPty::finished, this,
            [this, id](int code) { onLocalControlTerminalFinished(id, code); });
    m_localControlTerminals.insert(id, terminal);
    QJsonObject answer = terminals::terminalJson(terminal);
    answer.insert(QStringLiteral("replay"), QString());
    answer.insert(QStringLiteral("stopped"), stopped);
    answer.insert(QStringLiteral("resumed"), !plan.conversationId.isEmpty());
    server->respond(connectionId, requestId, answer);
    logSystem(QStringLiteral("Opened agent %1 in %2 in a terminal in %3%4, requested by %5.")
                  .arg(sessionId)
                  .arg(terminals::cliLabel(provider), terminal.cwd,
                       stopped ? QStringLiteral(" after stopping its run") : QString(),
                       localControlDeviceLabel(connectionId)));
    notifyLocalControlAgentsChanged();
#endif
}

void MainWindow::startLocalControlHostTerminal(int connectionId, int requestId,
                                               const QString &host, const QJsonObject &params)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    QSettings settings;
    const QJsonArray hosts = forkmesh::control::loadSavedHosts(
        settings, forkmesh::ui::kHostsSetting, &m_hostSessionPasswords);
    QJsonObject saved;
    for (const QJsonValue &value : hosts) {
        if (value.toObject().value(QStringLiteral("name")).toString().trimmed().compare(
                host.trimmed(), Qt::CaseInsensitive) == 0) {
            saved = value.toObject();
            break;
        }
    }
    const QString name = saved.value(QStringLiteral("name")).toString().trimmed();
    const QString ip = saved.value(QStringLiteral("ip")).toString().trimmed();
    const QString user = saved.value(QStringLiteral("user")).toString().trimmed();
    if (name.isEmpty() || ip.isEmpty() || user.isEmpty()) {
        server->respondError(connectionId, requestId, QStringLiteral("unknown_host"));
        return;
    }
    if (m_localControlTerminals.size() >= terminals::kMaxTerminals) {
        server->respondError(connectionId, requestId, QStringLiteral("terminal_limit"));
        return;
    }
    const QString password =
        m_hostSessionPasswords.value(forkmesh::control::savedHostCredentialKey(name, ip, user));
    QString sshError;
    const forkmesh::control::HostSshCommand ssh = forkmesh::control::buildHostLoginShellSshCommand(
        ip, user, password, &sshError, savedHostIdentityFile(name, ip, user));
    const QString program =
        ssh.program.isEmpty() ? QString() : QStandardPaths::findExecutable(ssh.program);
    if (program.isEmpty()) {
        logSystem(QStringLiteral("Could not open an SSH terminal on \"%1\" for %2: %3")
                      .arg(name, localControlDeviceLabel(connectionId),
                           sshError.isEmpty()
                               ? QStringLiteral("%1 is not installed on this desktop.")
                                     .arg(ssh.program)
                               : sshError));
        server->respondError(connectionId, requestId,
                             ssh.program.isEmpty() ? QStringLiteral("ssh_unavailable")
                                                   : QStringLiteral("ssh_missing"));
        return;
    }
    terminals::Terminal terminal;
    terminal.id = terminals::newTerminalId();
    terminal.provider = terminals::hostShellProvider();
    terminal.cli = ssh.program;
    terminal.mode = terminals::hostShellProvider();
    terminal.host = name;
    terminal.cwd = QStringLiteral("%1@%2").arg(user, ip);
    terminal.device = server->deviceFor(connectionId).name;
    terminal.startedAtMs = QDateTime::currentMSecsSinceEpoch();
    terminal.cols = terminals::clampCols(params.value(QStringLiteral("cols")).toInt(80));
    terminal.rows = terminals::clampRows(params.value(QStringLiteral("rows")).toInt(24));
    auto *pty = new AgentLoginPty(this);
    if (!password.isEmpty())
        pty->setEnvironment({{QStringLiteral("SSHPASS"), password}});
    QString error;
    if (!pty->run(program, ssh.arguments, QDir::homePath(), terminal.cols, terminal.rows, true,
                  &error)) {
        pty->deleteLater();
        logSystem(QStringLiteral("Could not open an SSH terminal on \"%1\" for %2: %3")
                      .arg(name, localControlDeviceLabel(connectionId), error));
        server->respondError(connectionId, requestId, QStringLiteral("terminal_failed"));
        return;
    }
    terminal.pty = pty;
    terminal.pid = pty->childPid();
    const QString id = terminal.id;
    connect(pty, &AgentLoginPty::output, this,
            [this, id](const QByteArray &chunk) { onLocalControlTerminalOutput(id, chunk); });
    connect(pty, &AgentLoginPty::finished, this,
            [this, id](int code) { onLocalControlTerminalFinished(id, code); });
    m_localControlTerminals.insert(id, terminal);
    QJsonObject answer = terminals::terminalJson(terminal);
    answer.insert(QStringLiteral("started"), true);
    answer.insert(QStringLiteral("action"), QStringLiteral("terminal"));
    answer.insert(QStringLiteral("message"),
                  QStringLiteral("Connecting to %1 as %2.").arg(name, terminal.cwd));
    server->respond(connectionId, requestId, answer);
    logSystem(QStringLiteral("Opened an SSH terminal on \"%1\" (%2), requested by %3.")
                  .arg(name, terminal.cwd, localControlDeviceLabel(connectionId)));
}

void MainWindow::onLocalControlTerminalOutput(const QString &id, const QByteArray &chunk)
{
    auto it = m_localControlTerminals.find(id);
    if (it == m_localControlTerminals.end())
        return;
#if defined(FORKMESH_HAVE_TERM)
    if (it->paste)
        it->paste->feed(chunk.constData(), std::size_t(chunk.size()));
#endif
    terminals::appendReplay(it->replay, chunk);
    for (terminals::Outbox &box : it->viewers)
        terminals::queueOutput(box, chunk);
    if (!it->viewers.isEmpty())
        scheduleLocalControlTerminalFlush();
}

void MainWindow::onLocalControlTerminalFinished(const QString &id, int code)
{
    auto it = m_localControlTerminals.find(id);
    if (it == m_localControlTerminals.end())
        return;
    logSystem(QStringLiteral("%1 in the terminal in %2 ended with status %3.")
                  .arg(terminals::terminalLabel(*it), it->cwd)
                  .arg(code));
    it->pid = 0;
    endLocalControlTerminal(id, code);
}

void MainWindow::endLocalControlTerminal(const QString &id, int code)
{
    auto it = m_localControlTerminals.find(id);
    if (it == m_localControlTerminals.end())
        return;
    terminals::Terminal terminal = *it;
    m_localControlTerminals.erase(it);
    m_externalTempId.remove(terminalRowKey(id));
    if (terminal.pty) {
        terminal.pty->disconnect(this);
        terminal.pty->stop();
        terminal.pty->deleteLater();
    }
    if (m_localControl) {
        for (auto viewer = terminal.viewers.begin(); viewer != terminal.viewers.end(); ++viewer) {
            bool open = true;
            while (open && terminals::hasOutput(viewer.value()))
                open = m_localControl->sendEventTo(
                    viewer.key(), QStringLiteral("terminal.output"),
                    terminals::takeEvent(viewer.value(), id, terminal.replay));
            if (open)
                m_localControl->sendEventTo(viewer.key(), QStringLiteral("terminal.exit"),
                                            {{QStringLiteral("terminal"), id},
                                             {QStringLiteral("code"), code}});
        }
    }
    if (terminal.provider == terminals::hostShellProvider())
        return;
    if (terminal.mode.isEmpty()) {
        notifyLocalControlAgentsChanged();
    } else if (terminal.provider == QLatin1String("codex")) {
        refreshCodexUsageRemaining();
    } else {
        refreshClaudeCodeUsage();
    }
}

void MainWindow::scheduleLocalControlTerminalFlush()
{
    if (!m_localControlTerminalTimer) {
        m_localControlTerminalTimer = new QTimer(this);
        m_localControlTerminalTimer->setSingleShot(true);
        connect(m_localControlTerminalTimer, &QTimer::timeout, this,
                &MainWindow::flushLocalControlTerminals);
    }
    if (!m_localControlTerminalTimer->isActive())
        m_localControlTerminalTimer->start(0);
}

void MainWindow::flushLocalControlTerminals()
{
    if (!m_localControl)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    qint64 wait = -1;
    const auto later = [&wait](qint64 ms) { wait = wait < 0 ? ms : std::min(wait, ms); };
    QList<int> gone;
    for (auto terminal = m_localControlTerminals.begin(); terminal != m_localControlTerminals.end();
         ++terminal) {
        for (auto viewer = terminal->viewers.begin(); viewer != terminal->viewers.end();) {
            terminals::Outbox &box = viewer.value();
            if (!terminals::hasOutput(box)) {
                ++viewer;
                continue;
            }
            const bool relayed = m_localControl->isRelayed(viewer.key());
            const qint64 interval = forkmesh::localcontrol::followIntervalMs(relayed);
            const qint64 remaining = box.sentAtMs + interval - now;
            if (remaining > 0) {
                later(remaining);
                ++viewer;
                continue;
            }
            if (relayed && m_nodeEventSocket && m_nodeEventSocket->controlBacklog() > 0) {
                later(interval);
                ++viewer;
                continue;
            }
            box.sentAtMs = now;
            if (!m_localControl->sendEventTo(viewer.key(), QStringLiteral("terminal.output"),
                                             terminals::takeEvent(box, terminal->id,
                                                                  terminal->replay))) {
                gone.append(viewer.key());
                viewer = terminal->viewers.erase(viewer);
                continue;
            }
            if (terminals::hasOutput(box))
                later(interval);
            ++viewer;
        }
    }
    for (const int connectionId : std::as_const(gone))
        dropLocalControlTerminalViewer(connectionId);
    if (wait >= 0 && m_localControlTerminalTimer && !m_localControlTerminalTimer->isActive())
        m_localControlTerminalTimer->start(int(wait));
}

void MainWindow::dropLocalControlTerminalViewer(int connectionId)
{
    for (terminals::Terminal &terminal : m_localControlTerminals)
        terminal.viewers.remove(connectionId);
}

QSet<qint64> MainWindow::localControlTerminalPids() const
{
    QSet<qint64> pids;
    for (const terminals::Terminal &terminal : m_localControlTerminals)
        if (terminal.pid > 0)
            pids.insert(terminal.pid);
    return pids;
}

QString MainWindow::localControlTerminalForOutside(int id) const
{
    if (m_localControlTerminals.isEmpty())
        return {};
    qint64 pid = 0;
    if (const TerminalHandoffSession *registration = outsideAgentRegistration(id))
        pid = registration->pid;
    else if (const auto machine = m_machineAgents.constFind(id); machine != m_machineAgents.constEnd())
        pid = machine->pid;
    if (pid <= 0)
        return {};
    for (const terminals::Terminal &terminal : m_localControlTerminals)
        if (terminal.pid == pid)
            return terminal.id;
    return {};
}

QString MainWindow::localControlTerminalForRow(int id) const
{
    if (id == 0 || m_localControlTerminals.isEmpty())
        return {};
    if (id > 0) {
        for (const terminals::Terminal &terminal : m_localControlTerminals)
            if (terminal.session == id && terminal.pty && terminal.pid > 0)
                return terminal.id;
        return {};
    }
    for (const terminals::Terminal &terminal : m_localControlTerminals) {
        const auto known = m_externalTempId.constFind(terminalRowKey(terminal.id));
        if (known != m_externalTempId.constEnd() && known.value() == id)
            return terminal.id;
    }
    return {};
}

bool MainWindow::answerLocalControlTerminalAgent(int connectionId, int requestId,
                                                 const QString &method, int id,
                                                 const QJsonObject &params)
{
    const QString terminalId = localControlTerminalForRow(id);
    if (terminalId.isEmpty())
        return false;
    LocalControlServer *server = m_localControl;
    const terminals::Terminal &terminal = m_localControlTerminals[terminalId];
    if (method == QLatin1String("agents.transcript")) {
        server->respondError(connectionId, requestId, QStringLiteral("no_transcript"));
        return true;
    }
    if (method == QLatin1String("agents.continue")) {
        server->respondError(connectionId, requestId, QStringLiteral("terminal_open"));
        return true;
    }
    if (!terminal.pty || terminal.pid <= 0) {
        server->respondError(connectionId, requestId, QStringLiteral("unknown_session"));
        return true;
    }
    if (method == QLatin1String("agents.stop")) {
        logSystem(QStringLiteral("Pressed Esc in the terminal running %1 in %2 to stop it, "
                                 "requested by %3.")
                      .arg(terminals::cliLabel(terminal.provider), terminal.cwd,
                           localControlDeviceLabel(connectionId)));
        terminal.pty->write(QByteArrayLiteral("\x1b"));
        server->respond(connectionId, requestId,
                        {{QStringLiteral("status"), QStringLiteral("interrupted")},
                         {QStringLiteral("key"), QStringLiteral("esc")}});
        return true;
    }
    if (method != QLatin1String("agents.prompt"))
        return false;
    const QString text = params.value(QStringLiteral("text")).toString().trimmed();
    if (text.isEmpty()) {
        server->respondError(connectionId, requestId, QStringLiteral("bad_task"));
        return true;
    }
    const LocalControlServer::Device device = server->deviceFor(connectionId);
    const QString refused =
        TerminalHandoff::admit(m_terminalTypingGate, server->isPaired(device.key), device.key,
                               int(text.size()), QDateTime::currentMSecsSinceEpoch());
    if (!refused.isEmpty()) {
        server->respondError(connectionId, requestId, refused);
        return true;
    }
    logSystem(QStringLiteral("Typing into the terminal running %1 in %2, requested by %3.")
                  .arg(terminals::cliLabel(terminal.provider), terminal.cwd,
                       localControlDeviceLabel(connectionId)));
    typeIntoLocalControlTerminal(terminalId, text);
    server->respond(connectionId, requestId,
                    {{QStringLiteral("status"), QStringLiteral("typed")},
                     {QStringLiteral("via"), QStringLiteral("terminal")}});
    return true;
}

void MainWindow::typeIntoLocalControlTerminal(const QString &id, const QString &text)
{
#if defined(FORKMESH_HAVE_TERM)
    const auto it = m_localControlTerminals.constFind(id);
    if (it == m_localControlTerminals.constEnd() || !it->pty)
        return;
    const std::string cleaned = forkmesh::term::sanitizeText(text.toStdString());
    if (cleaned.empty())
        return;
    const std::string paste =
        forkmesh::term::pasteBytes(cleaned, it->paste && it->paste->enabled());
    it->pty->write(QByteArray::fromStdString(paste));
    QTimer::singleShot(terminals::kEnterDelayMs, this, [this, id] {
        const auto found = m_localControlTerminals.constFind(id);
        if (found != m_localControlTerminals.constEnd() && found->pty)
            found->pty->write(QByteArray::fromStdString(forkmesh::term::enterBytes()));
    });
#else
    Q_UNUSED(id)
    Q_UNUSED(text)
#endif
}
