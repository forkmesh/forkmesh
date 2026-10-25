#include "CodexAppServerSession.h"

#include "AgentCliLocator.h"
#include "AgentGuardrails.h"
#include "AgentToolPolicy.h"

#include "AgentCapabilityBroker.h"
#include "AgentJail.h"
#include "AgentResources.h"
#include "VirtualMachineRuntime.h"
#include "TranscriptCoverage.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>

#include <utility>
#include <QTimer>

namespace {

QString compactJson(const QJsonValue &value)
{
    QByteArray bytes;
    if (value.isObject())
        bytes = QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
    else if (value.isArray())
        bytes = QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact);
    else if (value.isString())
        return value.toString();
    else if (value.isBool())
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    else if (value.isDouble())
        return QString::number(value.toDouble());
    else if (value.isNull())
        return QStringLiteral("null");
    return QString::fromUtf8(bytes);
}

QString normalizedMode(const QString &mode)
{
    return mode.trimmed().toLower();
}

QString approvalPolicyForMode(const QString &)
{
    return QStringLiteral("never");
}

const QString kFullAccessSandbox = QStringLiteral("danger-full-access");
const QString kAutonomousInstructions =
    QStringLiteral("Run autonomously until the task is complete. Make routine implementation choices using your best judgment and reasonable assumptions. Do not ask the user for approvals, confirmations, or optional choices. Ask a concise question only when essential information is missing and no reasonable assumption can resolve it; continue independent work while waiting. Preserve repository data and follow repository instructions.");

bool codexModelSkipsServiceTier(const QString &model)
{
    const QString trimmed = model.trimmed();
    return trimmed == QLatin1String("gpt-5.3-spark") ||
           trimmed == QLatin1String("gpt-5.3-codex-spark");
}

QJsonObject fullAccessSandboxPolicy()
{
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("dangerFullAccess")}};
}

QJsonObject contextFields(const QJsonObject &params)
{
    QJsonObject fields;
    const QList<QPair<QString, QString>> keys{
        {QStringLiteral("threadId"), QStringLiteral("thread_id")},
        {QStringLiteral("turnId"), QStringLiteral("turn_id")},
        {QStringLiteral("itemId"), QStringLiteral("item_id")}};
    for (const auto &key : keys) {
        if (params.contains(key.first)) {
            fields.insert(key.first, params.value(key.first));
            fields.insert(key.second, params.value(key.first));
        }
    }
    return fields;
}

void mergeObject(QJsonObject &target, const QJsonObject &source)
{
    for (auto it = source.constBegin(); it != source.constEnd(); ++it)
        target.insert(it.key(), it.value());
}

QJsonObject mergeRateLimitSnapshot(QJsonObject current,
                                   const QJsonObject &update)
{
    // Rolling updates are sparse. Nullable account metadata is commonly null
    // in those updates and, per the app-server contract, must not erase the
    // value returned by account/rateLimits/read. Window nulls are meaningful:
    // they say that particular window is unavailable.
    static const QSet<QString> nullableMetadata{
        QStringLiteral("credits"),
        QStringLiteral("individualLimit"),
        QStringLiteral("limitId"),
        QStringLiteral("limitName"),
        QStringLiteral("planType"),
        QStringLiteral("rateLimitReachedType"),
        QStringLiteral("spendControlReached")};
    for (auto it = update.constBegin(); it != update.constEnd(); ++it) {
        if (it.value().isNull() && nullableMetadata.contains(it.key()) &&
            current.contains(it.key()))
            continue;
        if ((it.key() == QLatin1String("primary") ||
             it.key() == QLatin1String("secondary")) &&
            it.value().isObject() && current.value(it.key()).isObject()) {
            QJsonObject window = current.value(it.key()).toObject();
            mergeObject(window, it.value().toObject());
            current.insert(it.key(), window);
            continue;
        }
        current.insert(it.key(), it.value());
    }
    return current;
}

} // namespace

CodexAppServerSession::CodexAppServerSession(QObject *parent)
    : AgentRuntimeSession(parent)
{}

CodexAppServerSession::~CodexAppServerSession()
{
    stopProcess(/*finalTeardown=*/true);
}

AgentRuntimeDescriptor CodexAppServerSession::runtimeDescriptor() const
{
    return {AgentRuntimeIds::CodexAppServer,
            QStringLiteral("Codex app server"),
            1,
            1,
            {AgentRuntimeCapabilities::SessionLifecycle,
             AgentRuntimeCapabilities::StreamingText,
             AgentRuntimeCapabilities::StreamingReasoning,
             AgentRuntimeCapabilities::ToolLifecycle,
             AgentRuntimeCapabilities::Usage,
             AgentRuntimeCapabilities::Permissions,
             AgentRuntimeCapabilities::Resume,
             AgentRuntimeCapabilities::Steering,
             AgentRuntimeCapabilities::Interrupt,
             AgentRuntimeCapabilities::ModelDiscovery}};
}

void CodexAppServerSession::startRuntime(const AgentRuntimeLaunch &launch)
{
    m_configOverrides = AgentToolPolicy::codexConfigOverrides(
        AgentToolPolicy::disabledFamiliesFromOptions(launch.options));
    m_workspaceSandbox = launch.options.value(QStringLiteral("workspaceWriteRoots")).isArray();
    m_workspaceWriteRoots =
        launch.options.value(QStringLiteral("workspaceWriteRoots")).toArray();
    start(launch.workingDirectory, launch.environment, launch.initialPrompt,
          launch.resumeId, launch.model, launch.permissionMode, launch.effort,
          launch.memoryLimitMb, launch.resumeFallbackPrompt, launch.sessionId);
}

void CodexAppServerSession::setAppServerCommand(const QString &program,
                                                const QStringList &arguments)
{
    m_program = program;
    m_arguments = arguments;
}

void CodexAppServerSession::start(const QString &cwd,
                                  const QStringList &extraEnv,
                                  const QString &initialPrompt,
                                  const QString &resumeThreadId,
                                  const QString &model, const QString &mode,
                                  const QString &effort, int memoryLimitMb,
                                  const QString &resumeFallbackPrompt,
                                  const QString &hostSessionId)
{
    stop();
    resetProtocolState();
    m_cwd = cwd;
    m_initialPrompt = initialPrompt;
    m_resumeThreadId = resumeThreadId.trimmed();
    m_resumeFallbackPrompt = resumeFallbackPrompt.trimmed();
    m_hostSessionId = hostSessionId;
    if (m_resumeFallbackPrompt.isEmpty())
        m_resumeFallbackPrompt = m_initialPrompt;
    m_model = model.trimmed();
    m_mode = mode;
    m_effort = effort.trimmed();

    QProcess *proc = new QProcess(this);
    m_proc = proc;
    if (!cwd.isEmpty())
        proc->setWorkingDirectory(cwd);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const QString &entry : extraEnv) {
        const int equals = entry.indexOf(QLatin1Char('='));
        if (equals > 0)
            env.insert(entry.left(equals), entry.mid(equals + 1));
        else if (equals < 0 && !entry.isEmpty())
            env.remove(entry);
    }
    // Same reason as ClaudeStreamSession: the inherited PATH may not hold
    // the user's install directories, and a locked launch never reads the
    // profile that would add them.
    forkmesh::agentcli::widenPath(env);
    forkmesh::vm::applyGuestEnvironmentPolicy(env);
    forkmesh::agentresources::applyEnvironment(env);
    proc->setProcessEnvironment(env);
    proc->setProcessChannelMode(QProcess::SeparateChannels);

    connect(proc, &QProcess::readyReadStandardOutput, this,
            [this] { onStdout(false); });
    connect(proc, &QProcess::readyReadStandardError, this,
            &CodexAppServerSession::onStderr);
    connect(proc, &QProcess::started, this,
            &CodexAppServerSession::onProcessStarted);
    connect(proc, &QProcess::errorOccurred, this,
            [this, proc](QProcess::ProcessError error) {
                if (m_proc != proc || error != QProcess::FailedToStart)
                    return;
                emitLocalNotice(QStringLiteral("error"),
                                QStringLiteral("Unable to start Codex app-server: %1")
                                    .arg(proc->errorString()));
                m_proc = nullptr;
                proc->deleteLater();
                emit finished(-1);
            });
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, proc](int code, QProcess::ExitStatus) {
                if (m_proc != proc)
                    return;
                onProcessFinished(code);
            });

    QString program;
    QStringList arguments;
    const bool skipServiceTier = codexModelSkipsServiceTier(m_model);
    if (!m_program.isEmpty()) {
        program = m_program;
        arguments = m_arguments;
    } else {
        // Config overrides come from the operator's tool policy; the policy
        // helper only emits [A-Za-z0-9_.="-] so they are shell-safe inside
        // single quotes.
        static const QRegularExpression safeOverride(
            QStringLiteral("^[A-Za-z0-9_.=\"-]+$"));
        QStringList overrides;
        if (skipServiceTier)
            overrides << QStringLiteral("service_tier=auto");
        for (const QString &entry : std::as_const(m_configOverrides))
            if (safeOverride.match(entry).hasMatch() && !overrides.contains(entry))
                overrides << entry;
#ifdef Q_OS_WIN
        program = QStringLiteral("codex");
        arguments.clear();
        for (const QString &entry : std::as_const(overrides))
            arguments << QStringLiteral("-c") << entry;
        arguments << QStringLiteral("app-server");
#else
        program = QStringLiteral("bash");
        QString codexCommand = QStringLiteral("exec codex");
        for (const QString &entry : std::as_const(overrides))
            codexCommand += QStringLiteral(" -c '%1'").arg(entry);
        codexCommand += QStringLiteral(" app-server");
        arguments = {QStringLiteral("-c"),
                    forkmesh::agentresources::wrapShellCommand(
                        AgentJail::wrapCommand(codexCommand, memoryLimitMb))};
#endif
    }
    const forkmesh::agentguard::LaunchCommand guarded =
        forkmesh::agentguard::restrictedCommand(
            program, arguments, cwd, m_guardrails, env);
    if (!guarded.error.isEmpty()) {
        emitLocalNotice(QStringLiteral("error"),
                        QStringLiteral("Agent guardrails refused the launch: %1")
                            .arg(guarded.error));
        m_proc = nullptr;
        proc->deleteLater();
        QTimer::singleShot(0, this, [this] { emit finished(-1); });
        return;
    }
    const forkmesh::vm::LaunchCommand launch =
        forkmesh::vm::isolateCommand(guarded.program, guarded.arguments, cwd);
    if (!launch.error.isEmpty()) {
        emitLocalNotice(QStringLiteral("error"),
                        QStringLiteral("KVM launch failed: %1")
                            .arg(launch.error));
        m_proc = nullptr;
        proc->deleteLater();
        QTimer::singleShot(0, this, [this] { emit finished(-1); });
        return;
    }
    const forkmesh::agentresources::Launch contained =
        launch.isolated
            ? forkmesh::agentresources::Launch{launch.program, launch.arguments}
            : forkmesh::agentresources::contain(
                  launch.program, launch.arguments,
                  forkmesh::agentresources::launchLabel(
                      QStringLiteral("codex"), env));
    proc->start(contained.program, contained.arguments);
}

bool CodexAppServerSession::sendCommand(const AgentRuntimeCommand &command)
{
    if (!command.isValid())
        return false;
    if (command.type == AgentRuntimeCommandTypes::RequestRespond) {
        QString answer = command.data.value(QStringLiteral("answer")).toString();
        if (answer.isEmpty()) {
            const QString decision =
                command.data.value(QStringLiteral("decision")).toString();
            if (decision == QLatin1String("allow"))
                // Host grants remain authoritative. A normalized response
                // approves this operation only; allowing the provider process
                // to cache a broader scope would bypass later broker checks.
                answer = QStringLiteral("allow once");
            else if (decision == QLatin1String("deny"))
                answer = QStringLiteral("deny");
        }
        return !answer.isEmpty() &&
               respondToRequest(command.requestId, answer);
    }
    if (command.type == AgentRuntimeCommandTypes::TurnInterrupt) {
        if (!running() || !m_turnActive)
            return false;
        interrupt();
        return true;
    }
    if (command.type == AgentRuntimeCommandTypes::PermissionSet) {
        const QJsonValue mode = command.data.value(QStringLiteral("mode"));
        if (!mode.isString() || mode.toString().trimmed().isEmpty())
            return false;
        m_mode = mode.toString().trimmed();
        return true;
    }
    return AgentRuntimeSession::sendCommand(command);
}

void CodexAppServerSession::resetProtocolState()
{
    m_framer.reset();
    m_nextRequestId = 1;
    m_nextQuestionToken = 1;
    m_pendingCalls.clear();
    m_pendingSteerTexts.clear();
    m_pendingServerRequests.clear();
    m_toolUses.clear();
    m_toolResults.clear();
    m_reasoningDeltas.clear();
    m_noticeItems.clear();
    m_resultTurns.clear();
    m_syntheticTools.clear();
    m_effectiveModel.clear();
    m_threadId.clear();
    m_turnId.clear();
    m_activeTurnMode.clear();
    m_lastAgentText.clear();
    m_models = QJsonArray();
    m_lastTokenUsage = QJsonObject();
    m_rateLimits = QJsonObject();
    m_rateLimitsByLimitId = QJsonObject();
    m_queuedUserTexts.clear();
    m_threadReady = false;
    m_initialTurnSent = false;
    m_turnActive = false;
    m_systemInitEmitted = false;
    m_nextNormalizedSequence = 0;
}

void CodexAppServerSession::onProcessStarted()
{
    emit started();
    sendRequest(
        QStringLiteral("initialize"),
        QJsonObject{
            {QStringLiteral("clientInfo"),
             QJsonObject{{QStringLiteral("name"), QStringLiteral("forkmesh")},
                         {QStringLiteral("title"), QStringLiteral("ForkMesh")},
                         {QStringLiteral("version"),
                          QCoreApplication::applicationVersion().isEmpty()
                              ? QStringLiteral("0")
                              : QCoreApplication::applicationVersion()}}},
            {QStringLiteral("capabilities"),
             QJsonObject{{QStringLiteral("experimentalApi"), true},
                         {QStringLiteral("requestAttestation"), false}}}});
}

void CodexAppServerSession::onProcessFinished(int exitCode)
{
    QProcess *proc = m_proc;
    onStdout(true);
    onStderr();
    m_proc = nullptr;
    if (proc)
        proc->deleteLater();
    emit finished(exitCode);
}

bool CodexAppServerSession::sendUserText(const QString &text)
{
    if (text.trimmed().isEmpty())
        return true;
    if (!running())
        return false;
    if (!m_threadReady || (m_turnActive && m_turnId.isEmpty())) {
        m_queuedUserTexts.append(text);
        return true;
    }
    if (!m_turnActive) {
        beginTurn(text);
        return true;
    }

    // `turn/steer` only accepts text and the expected turn id; it cannot carry
    // the approval or collaboration settings in m_mode. Sending a follow-up
    // there after the composer changed modes would make it look as though the
    // new choice landed while Codex kept applying the old one (most visibly,
    // Plan still asking for approval after the user picked Auto). Let the
    // current turn finish, then flush this text through beginTurn(), which
    // sends the new policy as a real turn/start request.
    if (normalizedMode(m_activeTurnMode) != normalizedMode(m_mode)) {
        m_queuedUserTexts.append(text);
        emitLocalNotice(
            QStringLiteral("info"),
            QStringLiteral("Permission mode changed to %1. This message will "
                           "start after the current turn completes so Codex can "
                           "apply it.")
                .arg(m_mode.trimmed().isEmpty() ? QStringLiteral("Ask")
                                                 : m_mode.trimmed()));
        return true;
    }

    QJsonObject params{{QStringLiteral("threadId"), m_threadId},
                       {QStringLiteral("input"), userInputs(text)},
                       {QStringLiteral("expectedTurnId"), m_turnId}};
    const qint64 requestId = sendRequest(QStringLiteral("turn/steer"), params);
    m_pendingSteerTexts.insert(requestId, text);
    return true;
}

void CodexAppServerSession::setTurnOptions(const QString &model,
                                           const QString &mode,
                                           const QString &effort)
{
    if (const QString trimmed = model.trimmed(); !trimmed.isEmpty()) {
        m_model = trimmed;
        m_effectiveModel = trimmed;
    }
    m_mode = mode;
    m_effort = effort.trimmed();
}

bool CodexAppServerSession::respondToRequest(const QString &token,
                                             const QString &answer)
{
    auto it = m_pendingServerRequests.find(token);
    if (it == m_pendingServerRequests.end() || !acceptsInput())
        return false;
    const PendingServerRequest pending = it.value();

    QJsonObject result;
    if (pending.method == QStringLiteral("item/commandExecution/requestApproval") ||
        pending.method == QStringLiteral("item/fileChange/requestApproval")) {
        QJsonValue decision = pending.choices.value(answer.trimmed().toLower());
        if (decision.isUndefined()) {
            const QString key = answer.trimmed().toLower();
            if (key == QStringLiteral("accept") || key == QStringLiteral("allow") ||
                key == QStringLiteral("yes") || key.contains(QStringLiteral("once")))
                decision = QStringLiteral("accept");
            else if (key.contains(QStringLiteral("session")) ||
                     key.contains(QStringLiteral("always")))
                decision = QStringLiteral("acceptForSession");
            else if (key.contains(QStringLiteral("cancel")))
                decision = QStringLiteral("cancel");
            else
                decision = QStringLiteral("decline");
        }
        result.insert(QStringLiteral("decision"), decision);
    } else if (pending.method == QStringLiteral("item/tool/requestUserInput")) {
        QJsonObject answers;
        QJsonParseError parseError;
        const QJsonDocument parsed =
            QJsonDocument::fromJson(answer.trimmed().toUtf8(), &parseError);
        const QJsonObject supplied = parsed.isObject() ? parsed.object() : QJsonObject();
        QHash<QString, QString> displayedAnswers;
        if (supplied.isEmpty() && pending.questionIds.size() > 1) {
            for (const QString &line : answer.split(QLatin1Char('\n'),
                                                    Qt::SkipEmptyParts)) {
                int separator = line.indexOf(QChar(0x2192));
                int width = 1;
                if (separator < 0) {
                    separator = line.indexOf(QStringLiteral(" -> "));
                    width = 4;
                }
                if (separator < 0)
                    continue;
                const QString question = line.left(separator).trimmed();
                const QString value = line.mid(separator + width).trimmed();
                const QString questionId = pending.questionIdsByText.value(question);
                if (!questionId.isEmpty() && !value.isEmpty())
                    displayedAnswers.insert(questionId, value);
            }
        }
        for (int i = 0; i < pending.questionIds.size(); ++i) {
            const QString id = pending.questionIds.at(i);
            QJsonValue value = supplied.value(id);
            if (value.isUndefined() && displayedAnswers.contains(id))
                value = displayedAnswers.value(id);
            if (value.isUndefined() && pending.questionIds.size() == 1)
                value = answer;
            QJsonArray values;
            if (value.isArray())
                values = value.toArray();
            else if (value.isObject() && value.toObject().value(
                         QStringLiteral("answers")).isArray())
                values = value.toObject().value(QStringLiteral("answers")).toArray();
            else if (!value.isUndefined() && !value.isNull())
                values.append(value.toString(compactJson(value)));
            answers.insert(id, QJsonObject{{QStringLiteral("answers"), values}});
        }
        result.insert(QStringLiteral("answers"), answers);
    } else if (pending.method == QStringLiteral("mcpServer/elicitation/request")) {
        const QString key = answer.trimmed().toLower();
        const QString action = key.startsWith(QStringLiteral("accept"))
                                   ? QStringLiteral("accept")
                                   : key.startsWith(QStringLiteral("cancel"))
                                         ? QStringLiteral("cancel")
                                         : QStringLiteral("decline");
        result = QJsonObject{{QStringLiteral("action"), action},
                             {QStringLiteral("content"), QJsonValue(QJsonValue::Null)},
                             {QStringLiteral("_meta"), QJsonValue(QJsonValue::Null)}};
    } else if (pending.method == QStringLiteral("item/permissions/requestApproval")) {
        const bool accepted = answer.trimmed().toLower().startsWith(
                                  QStringLiteral("allow")) ||
                              answer.trimmed().compare(QStringLiteral("accept"),
                                                       Qt::CaseInsensitive) == 0;
        result.insert(QStringLiteral("permissions"),
                      accepted ? pending.params.value(QStringLiteral("permissions"))
                               : QJsonObject());
        result.insert(QStringLiteral("scope"),
                      accepted && answer.contains(QStringLiteral("session"),
                                                  Qt::CaseInsensitive)
                          ? QStringLiteral("session")
                          : QStringLiteral("turn"));
    }

    if (!sendObject(QJsonObject{{QStringLiteral("id"), pending.rpcId},
                                {QStringLiteral("result"), result}}))
        return false;
    m_pendingServerRequests.erase(it);
    return true;
}

void CodexAppServerSession::stop()
{
    stopProcess(/*finalTeardown=*/false);
}

// code-health: allow(blocking-wait) waits only on the finalTeardown path from the destructor, bounded to 2.5 s
void CodexAppServerSession::stopProcess(bool finalTeardown)
{
    if (!m_proc)
        return;
    interrupt();
    QProcess *proc = m_proc;
    m_proc = nullptr;
    proc->disconnect(this);
    if (finalTeardown) {
        // MainWindow is destroyed after QApplication::exec() returns. At that
        // point a detached process cannot reach a deleteLater() event. Reap it
        // while both the transport and Qt application are still alive instead
        // of leaving QProcessPrivate allocations for global/widget teardown.
        if (proc->state() != QProcess::NotRunning) {
            proc->closeWriteChannel();
            proc->terminate();
            if (!proc->waitForFinished(1500)) {
                proc->kill();
                proc->waitForFinished(1000);
            }
        }
        delete proc;
        return;
    }
    proc->setParent(nullptr);
    if (proc->state() == QProcess::NotRunning) {
        proc->deleteLater();
        return;
    }
    connect(proc, &QProcess::finished, proc, &QObject::deleteLater);
    proc->closeWriteChannel();
    proc->terminate();
    QTimer::singleShot(1500, proc, [proc] {
        if (proc->state() != QProcess::NotRunning)
            proc->kill();
    });
}

void CodexAppServerSession::interrupt()
{
    if (running() && m_turnActive && !m_threadId.isEmpty() &&
        !m_turnId.isEmpty()) {
        sendRequest(QStringLiteral("turn/interrupt"),
                    QJsonObject{{QStringLiteral("threadId"), m_threadId},
                                {QStringLiteral("turnId"), m_turnId}});
    }
}

bool CodexAppServerSession::running() const
{
    return m_proc && m_proc->state() != QProcess::NotRunning;
}

bool CodexAppServerSession::acceptsInput() const
{
    return running() && m_proc->isWritable();
}

qint64 CodexAppServerSession::processId() const
{
    return running() ? m_proc->processId() : 0;
}

QString CodexAppServerSession::threadId() const
{
    return m_threadId;
}

void CodexAppServerSession::onStdout(bool flushRemainder)
{
    AgentJsonlFramer::Result framed =
        m_framer.append(m_proc ? m_proc->readAllStandardOutput() : QByteArray(),
                        flushRemainder);
    for (const QString &error : std::as_const(framed.errors)) {
        emit stderrText(error);
        emitLocalNotice(QStringLiteral("error"), error);
    }
    for (const QByteArray &line : std::as_const(framed.frames))
        handleLine(line);
}

void CodexAppServerSession::onStderr()
{
    if (m_proc) {
        const QByteArray bytes = m_proc->readAllStandardError();
        if (!bytes.isEmpty())
            emit stderrText(QString::fromUtf8(bytes));
    }
}

void CodexAppServerSession::handleLine(const QByteArray &line)
{
    if (line.trimmed().isEmpty())
        return;
    emit rawLine(QString::fromUtf8(line));
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    if (error.error == QJsonParseError::NoError && document.isObject())
        handleMessage(document.object());
}

void CodexAppServerSession::handleMessage(const QJsonObject &message)
{
    const QString method = message.value(QStringLiteral("method")).toString();
    if (!method.isEmpty()) {
        if (message.contains(QStringLiteral("id")))
            handleServerRequest(message.value(QStringLiteral("id")), method,
                                message.value(QStringLiteral("params")).toObject());
        else
            handleNotification(method,
                               message.value(QStringLiteral("params")).toObject());
        return;
    }
    if (message.contains(QStringLiteral("id")))
        handleResponse(message);
}

qint64 CodexAppServerSession::sendRequest(const QString &method,
                                          const QJsonValue &params)
{
    const qint64 id = m_nextRequestId++;
    m_pendingCalls.insert(id, method);
    sendObject(QJsonObject{{QStringLiteral("method"), method},
                           {QStringLiteral("id"), id},
                           {QStringLiteral("params"), params}});
    return id;
}

void CodexAppServerSession::sendNotification(const QString &method,
                                              const QJsonObject &params)
{
    sendObject(QJsonObject{{QStringLiteral("method"), method},
                           {QStringLiteral("params"), params}});
}

bool CodexAppServerSession::sendObject(const QJsonObject &message)
{
    if (!acceptsInput())
        return false;
    QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact);
    line.append('\n');
    return m_proc->write(line) == line.size();
}

void CodexAppServerSession::handleResponse(const QJsonObject &message)
{
    const qint64 id = message.value(QStringLiteral("id")).toVariant().toLongLong();
    const QString method = m_pendingCalls.take(id);
    if (method.isEmpty())
        return;
    if (message.contains(QStringLiteral("error"))) {
        const QString detail = messageText(message.value(QStringLiteral("error")));
        const QString failure =
            QStringLiteral("Codex %1 failed: %2").arg(method, detail);
        // Older packaged Codex versions predate the snapshot endpoint. Their
        // rolling notification remains usable, so this optional read should
        // not become a transcript error or terminate the session.
        if (method == QStringLiteral("account/rateLimits/read"))
            return;
        if (method == QStringLiteral("thread/resume")) {
            startFreshThreadAfterResumeFailure(failure);
            return;
        }
        emitLocalNotice(QStringLiteral("error"), failure);
        if (method == QStringLiteral("turn/start")) {
            m_turnActive = false;
            m_turnId.clear();
            m_activeTurnMode.clear();
            emit event(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("result")},
                {QStringLiteral("subtype"), QStringLiteral("error")},
                {QStringLiteral("is_error"), true},
                {QStringLiteral("result"), failure},
                {QStringLiteral("session_id"), m_threadId},
                {QStringLiteral("thread_id"), m_threadId},
                {QStringLiteral("num_turns"), 1}});
            flushQueuedUserText();
        } else if (method == QStringLiteral("turn/steer")) {
            const QString rejectedText = m_pendingSteerTexts.take(id);
            if (!rejectedText.isEmpty())
                m_queuedUserTexts.append(rejectedText);
            if (!m_turnActive)
                flushQueuedUserText();
        } else if (method == QStringLiteral("initialize") ||
                   method == QStringLiteral("thread/start")) {
            // A session cannot recover from a failed handshake or a fresh-thread
            // launch failure. Preserve the normal finished() path so the host
            // clears its Running state and reports the launch failure.
            QProcess *proc = m_proc;
            if (proc) {
                proc->closeWriteChannel();
                proc->terminate();
                QTimer::singleShot(1500, proc, [proc] {
                    if (proc->state() != QProcess::NotRunning)
                        proc->kill();
                });
            }
        }
        return;
    }

    const QJsonObject result = message.value(QStringLiteral("result")).toObject();
    if (method == QStringLiteral("initialize")) {
        sendNotification(QStringLiteral("initialized"), QJsonObject());
        sendRequest(QStringLiteral("account/rateLimits/read"),
                    QJsonValue(QJsonValue::Null));
        sendRequest(QStringLiteral("model/list"),
                    QJsonObject{{QStringLiteral("limit"), 100},
                                {QStringLiteral("includeHidden"), false}});
        sendThreadRequest();
    } else if (method == QStringLiteral("model/list")) {
        for (const QJsonValue &model : result.value(QStringLiteral("data")).toArray())
            m_models.append(model);
        const QString cursor = result.value(QStringLiteral("nextCursor")).toString();
        if (!cursor.isEmpty()) {
            sendRequest(QStringLiteral("model/list"),
                        QJsonObject{{QStringLiteral("cursor"), cursor},
                                    {QStringLiteral("limit"), 100},
                                    {QStringLiteral("includeHidden"), false}});
        } else {
            emit modelsListed(m_models);
        }
    } else if (method == QStringLiteral("account/rateLimits/read")) {
        m_rateLimits = result.value(QStringLiteral("rateLimits")).toObject();
        m_rateLimitsByLimitId =
            result.value(QStringLiteral("rateLimitsByLimitId")).toObject();
        const QString legacyId =
            m_rateLimits.value(QStringLiteral("limitId")).toString();
        if (!legacyId.isEmpty() &&
            !m_rateLimitsByLimitId.contains(legacyId))
            m_rateLimitsByLimitId.insert(legacyId, m_rateLimits);
        emit event(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("_codex_rate_limits")},
            {QStringLiteral("rateLimits"), m_rateLimits},
            {QStringLiteral("rateLimitsByLimitId"), m_rateLimitsByLimitId}});
    } else if (method == QStringLiteral("thread/start") ||
               method == QStringLiteral("thread/resume")) {
        acceptThread(result);
    } else if (method == QStringLiteral("turn/start")) {
        const QString id = result.value(QStringLiteral("turn"))
                               .toObject()
                               .value(QStringLiteral("id"))
                               .toString();
        if (!id.isEmpty())
            m_turnId = id;
        m_turnActive = true;
        flushQueuedUserText();
    } else if (method == QStringLiteral("turn/steer")) {
        m_pendingSteerTexts.remove(id);
    }
}

void CodexAppServerSession::startFreshThreadAfterResumeFailure(
    const QString &failure)
{
    m_resumeThreadId.clear(); // sendThreadRequest() now issues thread/start.
    m_threadId.clear();
    m_turnId.clear();
    m_activeTurnMode.clear();
    m_threadReady = false;
    m_turnActive = false;
    m_initialTurnSent = false;
    m_initialPrompt = m_resumeFallbackPrompt;
    emitLocalNotice(
        QStringLiteral("warning"),
        QStringLiteral("%1. The saved Codex thread is unavailable, so ForkMesh "
                       "started a new thread with the saved task and latest "
                       "instruction.")
            .arg(failure));
    sendThreadRequest();
}

void CodexAppServerSession::sendThreadRequest()
{
    QJsonObject params{{QStringLiteral("developerInstructions"), kAutonomousInstructions},
                       {QStringLiteral("cwd"), m_cwd},
                       {QStringLiteral("approvalPolicy"),
                        approvalPolicyForMode(m_mode)},
                       {QStringLiteral("sandbox"), m_workspaceSandbox
                                                       ? QStringLiteral("workspace-write")
                                                       : kFullAccessSandbox}};
    if (!m_model.isEmpty())
        params.insert(QStringLiteral("model"), m_model);
    if (m_resumeThreadId.isEmpty()) {
        sendRequest(QStringLiteral("thread/start"), params);
    } else {
        params.insert(QStringLiteral("threadId"), m_resumeThreadId);
        sendRequest(QStringLiteral("thread/resume"), params);
    }
}

void CodexAppServerSession::acceptThread(const QJsonObject &result)
{
    const QJsonObject thread = result.value(QStringLiteral("thread")).toObject();
    const QString id = thread.value(QStringLiteral("id")).toString();
    if (!id.isEmpty())
        m_threadId = id;
    if (m_threadId.isEmpty())
        return;
    const QString effectiveModel = result.value(QStringLiteral("model")).toString();
    m_effectiveModel = effectiveModel.isEmpty() ? m_model : effectiveModel;
    m_threadReady = true;
    emitSystemInit();
    if (!m_initialTurnSent && !m_initialPrompt.trimmed().isEmpty()) {
        m_initialTurnSent = true;
        beginTurn(m_initialPrompt);
    } else {
        flushQueuedUserText();
    }
}

QJsonArray CodexAppServerSession::userInputs(const QString &text) const
{
    static const QRegularExpression attachment(
        QStringLiteral("^\\s*Attached image:\\s*(.+?)\\s*$"),
        QRegularExpression::CaseInsensitiveOption);
    QStringList textLines;
    QStringList imagePaths;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QRegularExpressionMatch match = attachment.match(line);
        if (match.hasMatch()) {
            QString path = match.captured(1).trimmed();
            if (path.size() >= 2 &&
                ((path.startsWith(QLatin1Char('"')) &&
                  path.endsWith(QLatin1Char('"'))) ||
                 (path.startsWith(QLatin1Char('\'')) &&
                  path.endsWith(QLatin1Char('\'')))))
                path = path.mid(1, path.size() - 2);
            if (!path.isEmpty())
                imagePaths.append(path);
        } else {
            textLines.append(line);
        }
    }

    QJsonArray inputs;
    const QString remainingText = textLines.join(QLatin1Char('\n')).trimmed();
    if (!remainingText.isEmpty()) {
        inputs.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                  {QStringLiteral("text"), remainingText},
                                  {QStringLiteral("text_elements"), QJsonArray()}});
    }
    for (const QString &path : imagePaths) {
        inputs.append(
            QJsonObject{{QStringLiteral("type"), QStringLiteral("localImage")},
                        {QStringLiteral("path"), path}});
    }
    return inputs;
}

void CodexAppServerSession::beginTurn(const QString &text)
{
    const QJsonArray inputs = userInputs(text);
    if (inputs.isEmpty() || m_threadId.isEmpty())
        return;
    QJsonObject params{{QStringLiteral("threadId"), m_threadId},
                       {QStringLiteral("input"), inputs},
                       {QStringLiteral("approvalPolicy"),
                        approvalPolicyForMode(m_mode)},
                       {QStringLiteral("sandboxPolicy"),
                        m_workspaceSandbox
                            ? QJsonObject{{QStringLiteral("type"),
                                           QStringLiteral("workspaceWrite")},
                                          {QStringLiteral("writableRoots"),
                                           m_workspaceWriteRoots},
                                          {QStringLiteral("networkAccess"), true}}
                            : fullAccessSandboxPolicy()}};
    if (!m_model.isEmpty())
        params.insert(QStringLiteral("model"), m_model);
    if (!m_effort.isEmpty())
        params.insert(QStringLiteral("effort"), m_effort);
    // Codex keeps collaboration mode in the thread settings. `null` means no
    // override, not necessarily "leave Plan" for an already-plan thread, so
    // every non-Plan turn names the `default` mode explicitly. That makes a
    // Plan -> Auto change observable by the app-server as well as by our UI.
    const QString collaborationModel =
        m_effectiveModel.isEmpty() ? m_model : m_effectiveModel;
    if (!collaborationModel.isEmpty()) {
        params.insert(
            QStringLiteral("collaborationMode"),
            QJsonObject{
                {QStringLiteral("mode"),
                 normalizedMode(m_mode).contains(QStringLiteral("plan"))
                     ? QStringLiteral("plan")
                     : QStringLiteral("default")},
                {QStringLiteral("settings"),
                 QJsonObject{
                     {QStringLiteral("model"), collaborationModel},
                     {QStringLiteral("reasoning_effort"),
                      m_effort.isEmpty() ? QJsonValue(QJsonValue::Null)
                                         : QJsonValue(m_effort)},
                     {QStringLiteral("developer_instructions"),
                      kAutonomousInstructions}}}});
    } else {
        params.insert(QStringLiteral("collaborationMode"),
                      QJsonValue(QJsonValue::Null));
    }
    m_turnActive = true;
    m_turnId.clear();
    m_activeTurnMode = m_mode;
    m_lastAgentText.clear();
    sendRequest(QStringLiteral("turn/start"), params);
}

void CodexAppServerSession::flushQueuedUserText()
{
    while (!m_queuedUserTexts.isEmpty() && m_threadReady &&
           (!m_turnActive || !m_turnId.isEmpty())) {
        // A mode change while the previous turn was being acknowledged leaves
        // an active turn with an id by the time this flush runs. Keep the
        // message parked until its completion; otherwise sendUserText() would
        // re-queue it for the same mode mismatch instead of starting it fresh.
        if (m_turnActive &&
            normalizedMode(m_activeTurnMode) != normalizedMode(m_mode))
            return;
        const QString text = m_queuedUserTexts.takeFirst();
        sendUserText(text);
        if (m_turnActive && m_turnId.isEmpty())
            break;
    }
}

void CodexAppServerSession::handleNotification(const QString &method,
                                               const QJsonObject &params)
{
    if (method == QStringLiteral("thread/started")) {
        const QJsonObject thread = params.value(QStringLiteral("thread")).toObject();
        const QString id = thread.value(QStringLiteral("id")).toString();
        if (!id.isEmpty() && m_threadId.isEmpty())
            m_threadId = id;
        return;
    }
    if (method == QStringLiteral("turn/started")) {
        m_turnActive = true;
        const QString id = params.value(QStringLiteral("turn"))
                               .toObject()
                               .value(QStringLiteral("id"))
                               .toString();
        if (!id.isEmpty())
            m_turnId = id;
        flushQueuedUserText();
        return;
    }
    if (method == QStringLiteral("turn/completed")) {
        const bool replayQueuedText = !m_queuedUserTexts.isEmpty();
        finishSyntheticTools(params.value(QStringLiteral("turn"))
                                 .toObject()
                                 .value(QStringLiteral("id"))
                                 .toString());
        if (replayQueuedText) {
            emitLocalNotice(
                QStringLiteral("warning"),
                QStringLiteral("Starting the queued message as a new turn."));
        } else {
            emitTurnResult(params);
        }
        m_turnActive = false;
        m_turnId.clear();
        m_activeTurnMode.clear();
        flushQueuedUserText();
        return;
    }
    if (method == QStringLiteral("item/started")) {
        emitItemStarted(params.value(QStringLiteral("item")).toObject());
        return;
    }
    if (method == QStringLiteral("item/completed")) {
        emitItemCompleted(params.value(QStringLiteral("item")).toObject());
        return;
    }
    if (method == QStringLiteral("item/agentMessage/delta")) {
        emitAgentDelta(params);
        return;
    }
    if (method == QStringLiteral("item/reasoning/summaryTextDelta") ||
        method == QStringLiteral("item/reasoning/textDelta")) {
        emitReasoningDelta(params);
        return;
    }
    if (method == QStringLiteral("thread/tokenUsage/updated")) {
        emitUsage(params);
        return;
    }
    if (method == QStringLiteral("account/rateLimits/updated")) {
        const QJsonObject update =
            params.value(QStringLiteral("rateLimits")).toObject();
        const QString limitId =
            update.value(QStringLiteral("limitId")).toString();
        if (m_rateLimits.isEmpty() ||
            limitId.isEmpty() ||
            m_rateLimits.value(QStringLiteral("limitId")).toString() == limitId)
            m_rateLimits = mergeRateLimitSnapshot(m_rateLimits, update);
        if (!limitId.isEmpty()) {
            const QJsonObject current =
                m_rateLimitsByLimitId.value(limitId).toObject();
            m_rateLimitsByLimitId.insert(
                limitId, mergeRateLimitSnapshot(current, update));
        }
        emit event(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("_codex_rate_limits")},
            {QStringLiteral("rateLimits"),
             m_rateLimits.isEmpty() ? update : m_rateLimits},
            {QStringLiteral("rateLimitsByLimitId"),
             m_rateLimitsByLimitId}});
        return;
    }
    if (method == QStringLiteral("turn/plan/updated")) {
        const QString turnId = params.value(QStringLiteral("turnId")).toString();
        const QString itemId = QStringLiteral("plan-%1").arg(turnId);
        QJsonArray todos;
        QStringList lines;
        for (const QJsonValue &value : params.value(QStringLiteral("plan")).toArray()) {
            const QJsonObject step = value.toObject();
            const QString text = step.value(QStringLiteral("step")).toString();
            QString status = step.value(QStringLiteral("status")).toString();
            if (status == QStringLiteral("inProgress"))
                status = QStringLiteral("in_progress");
            todos.append(QJsonObject{{QStringLiteral("content"), text},
                                     {QStringLiteral("status"), status}});
            lines.append(text);
        }
        QJsonObject item{{QStringLiteral("type"), QStringLiteral("plan")},
                         {QStringLiteral("id"), itemId},
                         {QStringLiteral("text"), lines.join(QLatin1Char('\n'))},
                         {QStringLiteral("todos"), todos},
                         {QStringLiteral("_turnId"), turnId}};
        m_syntheticTools.insert(itemId, item);
        emitToolUse(item);
        QJsonObject deltaParams = params;
        deltaParams.insert(QStringLiteral("itemId"), itemId);
        deltaParams.insert(QStringLiteral("delta"), params.value(QStringLiteral("plan")));
        emitToolDelta(method, deltaParams);
        return;
    }
    if (method == QStringLiteral("error") || method == QStringLiteral("warning") ||
        method == QStringLiteral("configWarning")) {
        const QString level = method == QStringLiteral("error")
                                  ? QStringLiteral("error")
                                  : QStringLiteral("warning");
        const QString text = messageText(params);
        emitLocalNotice(level, text);
        if (method == QStringLiteral("error") &&
            !params.value(QStringLiteral("willRetry")).toBool()) {
            const QString turnId = params.value(QStringLiteral("turnId")).toString();
            if (!turnId.isEmpty())
                m_resultTurns.insert(turnId);
            emit event(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("result")},
                {QStringLiteral("subtype"), QStringLiteral("error")},
                {QStringLiteral("is_error"), true},
                {QStringLiteral("result"), text},
                {QStringLiteral("session_id"), m_threadId},
                {QStringLiteral("thread_id"), m_threadId},
                {QStringLiteral("turn_id"), turnId},
                {QStringLiteral("num_turns"), 1}});
            m_turnActive = false;
            m_turnId.clear();
            m_activeTurnMode.clear();
        }
        return;
    }
    if (method.startsWith(QStringLiteral("item/")) ||
        method == QStringLiteral("turn/diff/updated"))
        emitToolDelta(method, params);
}

void CodexAppServerSession::handleServerRequest(const QJsonValue &id,
                                                const QString &method,
                                                const QJsonObject &params)
{
    const bool commandApproval =
        method == QStringLiteral("item/commandExecution/requestApproval");
    const bool fileApproval =
        method == QStringLiteral("item/fileChange/requestApproval");
    const bool userInput = method == QStringLiteral("item/tool/requestUserInput");
    const bool elicitation =
        method == QStringLiteral("mcpServer/elicitation/request");
    const bool permissions =
        method == QStringLiteral("item/permissions/requestApproval");
    if (!commandApproval && !fileApproval && !userInput && !elicitation &&
        !permissions) {
        sendObject(QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("error"),
             QJsonObject{{QStringLiteral("code"), -32601},
                         {QStringLiteral("message"),
                          QStringLiteral("Unsupported Codex server request")}}}});
        return;
    }

    const bool approvalRequest = commandApproval || fileApproval || permissions;
    const QString token =
        QStringLiteral("codex-%1-%2")
            .arg(approvalRequest ? QStringLiteral("approval")
                                 : QStringLiteral("request"))
            .arg(m_nextQuestionToken++);
    PendingServerRequest pending;
    pending.rpcId = id;
    pending.method = method;
    pending.params = params;
    QJsonArray questions;

    if (commandApproval || fileApproval) {
        QJsonArray decisions = params.value(QStringLiteral("availableDecisions")).toArray();
        if (decisions.isEmpty()) {
            decisions = QJsonArray{QStringLiteral("accept"),
                                   QStringLiteral("acceptForSession"),
                                   QStringLiteral("decline")};
        }
        QJsonArray options;
        for (const QJsonValue &decision : decisions) {
            const QString label = decisionLabel(decision);
            if (label.isEmpty())
                continue;
            options.append(QJsonObject{
                {QStringLiteral("label"), label},
                {QStringLiteral("description"),
                 decision.isString() ? decision.toString() : compactJson(decision)}});
            pending.choices.insert(label.toLower(), decision);
            if (decision.isString())
                pending.choices.insert(decision.toString().toLower(), decision);
        }
        QString question = params.value(QStringLiteral("reason")).toString();
        if (question.isEmpty()) {
            question = commandApproval
                           ? QStringLiteral("Allow Codex to run this command?\n%1")
                                 .arg(params.value(QStringLiteral("command")).toString())
                           : QStringLiteral("Allow Codex to apply these file changes?");
        }
        questions.append(QJsonObject{
            {QStringLiteral("question"), question},
            {QStringLiteral("header"),
             commandApproval ? QStringLiteral("Command approval")
                             : QStringLiteral("File approval")},
            {QStringLiteral("options"), options},
            {QStringLiteral("multiSelect"), false}});
    } else if (userInput) {
        for (const QJsonValue &value : params.value(QStringLiteral("questions")).toArray()) {
            const QJsonObject source = value.toObject();
            const QString questionId =
                source.value(QStringLiteral("id")).toString();
            const QString questionText =
                source.value(QStringLiteral("question")).toString();
            pending.questionIds.append(questionId);
            pending.questionIdsByText.insert(questionText, questionId);
            QJsonObject question{
                {QStringLiteral("question"), source.value(QStringLiteral("question"))},
                {QStringLiteral("header"), source.value(QStringLiteral("header"))},
                {QStringLiteral("options"), source.value(QStringLiteral("options"))},
                {QStringLiteral("multiSelect"), false}};
            if (source.value(QStringLiteral("isOther")).toBool())
                question.insert(QStringLiteral("isOther"), true);
            if (source.value(QStringLiteral("isSecret")).toBool())
                question.insert(QStringLiteral("isSecret"), true);
            questions.append(question);
        }
    } else {
        const QString prompt = elicitation
                                   ? params.value(QStringLiteral("message")).toString()
                                   : QStringLiteral("Allow the requested permissions?");
        questions.append(QJsonObject{
            {QStringLiteral("question"), prompt},
            {QStringLiteral("header"),
             elicitation ? QStringLiteral("MCP request")
                         : QStringLiteral("Permission request")},
            {QStringLiteral("options"),
             QJsonArray{
                 QJsonObject{{QStringLiteral("label"), QStringLiteral("Allow")},
                             {QStringLiteral("description"),
                              QStringLiteral("Approve this request")}},
                 QJsonObject{{QStringLiteral("label"), QStringLiteral("Deny")},
                             {QStringLiteral("description"),
                              QStringLiteral("Reject this request")}}}},
            {QStringLiteral("multiSelect"), false}});
    }

    m_pendingServerRequests.insert(token, pending);
    const bool autoApprove =
        approvalRequest && approvalPolicyForMode(m_mode) == QLatin1String("never");
    if (autoApprove) {
        const QString answer = commandApproval || fileApproval
                                   ? (pending.choices.contains(
                                          QStringLiteral("acceptforsession"))
                                          ? QStringLiteral("acceptForSession")
                                          : QStringLiteral("accept"))
                                   : QStringLiteral("Allow for session");
        respondToRequest(token, answer);
        return;
    }
    if (commandApproval && !m_hostSessionId.isEmpty()) {
        const QString command =
            params.value(QStringLiteral("command")).toString();
        const QByteArray fingerprint =
            command.isEmpty()
                ? QJsonDocument(params).toJson(QJsonDocument::Compact)
                : command.toUtf8();
        const QString digest = QString::fromLatin1(
            QCryptographicHash::hash(fingerprint, QCryptographicHash::Sha256)
                .toHex());
        QString description = command.isEmpty()
                                  ? QStringLiteral("Allow this process to execute?")
                                  : QStringLiteral("Run command:\n%1").arg(command);
        const QString reason = params.value(QStringLiteral("reason")).toString();
        if (!reason.isEmpty())
            description += QStringLiteral("\n\nReason: %1").arg(reason);
        description = description.left(1024);
        const AgentRuntimeEvent permission{
            1,
            AgentRuntimeEventTypes::PermissionRequested,
            m_hostSessionId,
            token,
            m_nextNormalizedSequence++,
            {{QStringLiteral("capability"),
              AgentHostCapabilities::ProcessExecute},
             {QStringLiteral("resource"),
              QStringLiteral("command:sha256:%1").arg(digest)},
             {QStringLiteral("description"), description},
             {QStringLiteral("command"), command.left(8192)}}};
        emit event(permission.toJson());
        return;
    }
    const QJsonObject block{
        {QStringLiteral("type"), QStringLiteral("tool_use")},
        {QStringLiteral("id"), token},
        {QStringLiteral("name"), QStringLiteral("AskUserQuestion")},
        {QStringLiteral("input"), QJsonObject{{QStringLiteral("questions"), questions}}}};
    emit event(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("assistant")},
        {QStringLiteral("session_id"), m_threadId},
        {QStringLiteral("thread_id"), m_threadId},
        {QStringLiteral("message"),
         QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                     {QStringLiteral("content"), QJsonArray{block}}}}});
}

void CodexAppServerSession::emitSystemInit()
{
    if (m_systemInitEmitted || m_threadId.isEmpty())
        return;
    m_systemInitEmitted = true;
    emit event(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("system")},
        {QStringLiteral("subtype"), QStringLiteral("init")},
        {QStringLiteral("session_id"), m_threadId},
        {QStringLiteral("cwd"), m_cwd},
        {QStringLiteral("model"), m_effectiveModel},
        {QStringLiteral("permissionMode"), m_mode},
        {QStringLiteral("provider"), QStringLiteral("codex")}});
}

void CodexAppServerSession::emitLocalNotice(const QString &level,
                                            const QString &text)
{
    if (text.trimmed().isEmpty())
        return;
    emit event(QJsonObject{{QStringLiteral("type"),
                            QStringLiteral("_local_notice")},
                           {QStringLiteral("level"), level},
                           {QStringLiteral("text"), text}});
}

void CodexAppServerSession::emitAgentDelta(const QJsonObject &params)
{
    QJsonObject normalized{{QStringLiteral("type"),
                            QStringLiteral("_codex_agent_delta")},
                           {QStringLiteral("delta"),
                            params.value(QStringLiteral("delta"))},
                           {QStringLiteral("text"),
                            params.value(QStringLiteral("delta"))}};
    mergeObject(normalized, contextFields(params));
    emit event(normalized);
}

void CodexAppServerSession::emitReasoningDelta(const QJsonObject &params)
{
    const QString itemId = params.value(QStringLiteral("itemId")).toString();
    if (!itemId.isEmpty())
        m_reasoningDeltas.insert(itemId);
    QJsonObject normalized{
        {QStringLiteral("type"), QStringLiteral("stream_event")},
        {QStringLiteral("event"),
         QJsonObject{
             {QStringLiteral("type"), QStringLiteral("content_block_delta")},
             {QStringLiteral("delta"),
              QJsonObject{{QStringLiteral("type"),
                           QStringLiteral("thinking_delta")},
                          {QStringLiteral("thinking"),
                           params.value(QStringLiteral("delta"))}}}}}};
    mergeObject(normalized, contextFields(params));
    emit event(normalized);
}

void CodexAppServerSession::emitToolDelta(const QString &method,
                                          const QJsonObject &params)
{
    QJsonValue delta = params.value(QStringLiteral("delta"));
    if (delta.isUndefined())
        delta = params.value(QStringLiteral("message"));
    if (delta.isUndefined())
        delta = params.value(QStringLiteral("diff"));
    if (delta.isUndefined())
        delta = params.value(QStringLiteral("changes"));
    if (delta.isUndefined())
        delta = params.value(QStringLiteral("plan"));
    if (delta.isArray() || delta.isObject())
        delta = compactJson(delta);
    QJsonObject normalized{{QStringLiteral("type"),
                            QStringLiteral("_codex_tool_delta")},
                           {QStringLiteral("method"), method},
                           {QStringLiteral("delta"), delta}};
    mergeObject(normalized, contextFields(params));
    emit event(normalized);
}

void CodexAppServerSession::emitItemStarted(const QJsonObject &item)
{
    emitSpecialItemNotice(item);
    if (isToolItem(item.value(QStringLiteral("type")).toString()))
        emitToolUse(item);
}

void CodexAppServerSession::emitItemCompleted(const QJsonObject &item)
{
    emitSpecialItemNotice(item);
    const QString type = item.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("agentMessage")) {
        m_lastAgentText = item.value(QStringLiteral("text")).toString();
        emit event(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("_codex_agent_complete")},
            {QStringLiteral("item_id"), item.value(QStringLiteral("id"))},
            {QStringLiteral("text"), m_lastAgentText}});
        return;
    }
    if (type == QStringLiteral("reasoning")) {
        QStringList parts;
        for (const QJsonValue &part : item.value(QStringLiteral("summary")).toArray())
            parts.append(part.toString());
        for (const QJsonValue &part : item.value(QStringLiteral("content")).toArray())
            parts.append(part.toString());
        const QString itemId = item.value(QStringLiteral("id")).toString();
        const QString text = parts.join(QLatin1Char('\n'));
        if (!text.isEmpty() && !m_reasoningDeltas.contains(itemId)) {
            emitReasoningDelta(QJsonObject{
                {QStringLiteral("itemId"), itemId},
                {QStringLiteral("delta"), text}});
        }
        emit event(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("_codex_reasoning_complete")},
            {QStringLiteral("item_id"), itemId},
            {QStringLiteral("text"), text}});
        return;
    }
    if (isToolItem(type)) {
        emitToolUse(item);
        emitToolResult(item);
    }
}

void CodexAppServerSession::emitSpecialItemNotice(const QJsonObject &item)
{
    const QString type = item.value(QStringLiteral("type")).toString();
    if (type != QStringLiteral("enteredReviewMode") &&
        type != QStringLiteral("exitedReviewMode") &&
        type != QStringLiteral("contextCompaction"))
        return;
    const QString id = item.value(QStringLiteral("id")).toString();
    if (!id.isEmpty() && m_noticeItems.contains(id))
        return;
    if (!id.isEmpty())
        m_noticeItems.insert(id);

    QString text;
    if (type == QStringLiteral("enteredReviewMode"))
        text = QStringLiteral("Codex entered review mode.");
    else if (type == QStringLiteral("exitedReviewMode"))
        text = QStringLiteral("Codex exited review mode.");
    else
        text = QStringLiteral("Codex compacted the conversation context.");
    const QString review = item.value(QStringLiteral("review")).toString().trimmed();
    if (!review.isEmpty())
        text += QLatin1Char(' ') + review;
    emitLocalNotice(QStringLiteral("info"), text);
}

void CodexAppServerSession::emitToolUse(const QJsonObject &item)
{
    const QString id = item.value(QStringLiteral("id")).toString();
    if (id.isEmpty() || m_toolUses.contains(id))
        return;
    m_toolUses.insert(id);
    const QJsonObject block{{QStringLiteral("type"), QStringLiteral("tool_use")},
                            {QStringLiteral("id"), id},
                            {QStringLiteral("name"), toolName(item)},
                            {QStringLiteral("input"), toolInput(item)}};
    emit event(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("assistant")},
        {QStringLiteral("session_id"), m_threadId},
        {QStringLiteral("message"),
         QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                     {QStringLiteral("content"), QJsonArray{block}}}}});
}

void CodexAppServerSession::emitToolResult(const QJsonObject &item)
{
    const QString id = item.value(QStringLiteral("id")).toString();
    if (id.isEmpty() || m_toolResults.contains(id))
        return;
    m_toolResults.insert(id);
    const QJsonObject block{
        {QStringLiteral("type"), QStringLiteral("tool_result")},
        {QStringLiteral("tool_use_id"), id},
        {QStringLiteral("content"), toolOutput(item)},
        {QStringLiteral("is_error"), toolFailed(item)}};
    emit event(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("user")},
        {QStringLiteral("session_id"), m_threadId},
        {QStringLiteral("message"),
         QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                     {QStringLiteral("content"), QJsonArray{block}}}}});
    emit toolCallOutput(toolName(item), toolInput(item), block.value(QStringLiteral("content")).toString());
}

void CodexAppServerSession::finishSyntheticTools(const QString &turnId)
{
    QStringList completed;
    for (auto it = m_syntheticTools.constBegin();
         it != m_syntheticTools.constEnd(); ++it) {
        if (it.value().value(QStringLiteral("_turnId")).toString() == turnId) {
            emitToolResult(it.value());
            completed.append(it.key());
        }
    }
    for (const QString &id : completed)
        m_syntheticTools.remove(id);
}

void CodexAppServerSession::emitTurnResult(const QJsonObject &params)
{
    const QJsonObject turn = params.value(QStringLiteral("turn")).toObject();
    const QString turnId = turn.value(QStringLiteral("id")).toString();
    if (!turnId.isEmpty() && m_resultTurns.contains(turnId))
        return;
    if (!turnId.isEmpty())
        m_resultTurns.insert(turnId);
    const QString status = turn.value(QStringLiteral("status")).toString();
    const bool cleanCompletion = status == QStringLiteral("completed");
    QString resultText = m_lastAgentText;
    if (resultText.isEmpty()) {
        const QJsonArray items = turn.value(QStringLiteral("items")).toArray();
        for (int i = items.size() - 1; i >= 0; --i) {
            const QJsonObject item = items.at(i).toObject();
            if (item.value(QStringLiteral("type")).toString() ==
                QStringLiteral("agentMessage")) {
                resultText = item.value(QStringLiteral("text")).toString();
                break;
            }
        }
    }
    if (!cleanCompletion && resultText.isEmpty())
        resultText = messageText(turn.value(QStringLiteral("error")));
    if (!cleanCompletion && resultText.isEmpty())
        resultText = status == QStringLiteral("interrupted")
                         ? QStringLiteral("Codex turn was interrupted.")
                         : QStringLiteral("Codex turn did not complete successfully.");
    QJsonObject normalized{
        {QStringLiteral("type"), QStringLiteral("result")},
        {QStringLiteral("subtype"),
         cleanCompletion ? QStringLiteral("success")
                         : status == QStringLiteral("interrupted")
                               ? QStringLiteral("interrupted")
                               : QStringLiteral("error")},
        {QStringLiteral("is_error"), !cleanCompletion},
        {QStringLiteral("result"), resultText},
        {QStringLiteral("session_id"), m_threadId},
        {QStringLiteral("thread_id"), m_threadId},
        {QStringLiteral("turn_id"), turn.value(QStringLiteral("id"))}};
    if (turn.value(QStringLiteral("durationMs")).isDouble())
        normalized.insert(QStringLiteral("duration_ms"),
                          turn.value(QStringLiteral("durationMs")));
    normalized.insert(QStringLiteral("turn_status"), status);
    normalized.insert(QStringLiteral("num_turns"), 1);
    if (turn.contains(QStringLiteral("startedAt")))
        normalized.insert(QStringLiteral("started_at"),
                          turn.value(QStringLiteral("startedAt")));
    if (turn.contains(QStringLiteral("completedAt")))
        normalized.insert(QStringLiteral("completed_at"),
                          turn.value(QStringLiteral("completedAt")));
    if (!m_lastTokenUsage.isEmpty()) {
        normalized.insert(QStringLiteral("tokenUsage"), m_lastTokenUsage);
        const QJsonObject total =
            m_lastTokenUsage.value(QStringLiteral("total")).toObject();
        normalized.insert(QStringLiteral("input_tokens"),
                          total.value(QStringLiteral("inputTokens")));
        normalized.insert(QStringLiteral("output_tokens"),
                          total.value(QStringLiteral("outputTokens")));
        normalized.insert(QStringLiteral("total_tokens"),
                          total.value(QStringLiteral("totalTokens")));
        normalized.insert(QStringLiteral("context_window"),
                          m_lastTokenUsage.value(QStringLiteral("modelContextWindow")));
    }
    emit event(normalized);
}

void CodexAppServerSession::emitUsage(const QJsonObject &params)
{
    QJsonObject normalized = params;
    normalized.insert(QStringLiteral("type"), QStringLiteral("_codex_usage"));
    const QJsonObject tokenUsage =
        params.value(QStringLiteral("tokenUsage")).toObject();
    if (!tokenUsage.isEmpty()) {
        m_lastTokenUsage = tokenUsage;
        const QJsonObject total = tokenUsage.value(QStringLiteral("total")).toObject();
        normalized.insert(QStringLiteral("input_tokens"),
                          total.value(QStringLiteral("inputTokens")));
        normalized.insert(QStringLiteral("output_tokens"),
                          total.value(QStringLiteral("outputTokens")));
        normalized.insert(QStringLiteral("total_tokens"),
                          total.value(QStringLiteral("totalTokens")));
        normalized.insert(QStringLiteral("context_window"),
                          tokenUsage.value(QStringLiteral("modelContextWindow")));
    }
    emit event(normalized);
}

bool CodexAppServerSession::isToolItem(const QString &type)
{
    return forkmesh::transcriptcoverage::isCodexToolItem(type);
}

QString CodexAppServerSession::toolName(const QJsonObject &item)
{
    const QString type = item.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("commandExecution"))
        return QStringLiteral("Bash");
    if (type == QStringLiteral("fileChange"))
        return QStringLiteral("FileChange");
    if (type == QStringLiteral("mcpToolCall")) {
        const QString server = item.value(QStringLiteral("server")).toString();
        const QString tool = item.value(QStringLiteral("tool")).toString();
        return server.isEmpty() ? tool : server + QStringLiteral("::") + tool;
    }
    if (type == QStringLiteral("dynamicToolCall"))
        return item.value(QStringLiteral("tool")).toString(
            QStringLiteral("DynamicTool"));
    if (type == QStringLiteral("collabAgentToolCall"))
        return QStringLiteral("Task");
    if (type == QStringLiteral("webSearch"))
        return QStringLiteral("WebSearch");
    if (type == QStringLiteral("imageView"))
        return QStringLiteral("Read");
    if (type == QStringLiteral("imageGeneration"))
        return QStringLiteral("ImageGeneration");
    if (type == QStringLiteral("plan"))
        return QStringLiteral("TodoWrite");
    if (type == QStringLiteral("subAgentActivity"))
        return QStringLiteral("Task");
    if (type == QStringLiteral("sleep"))
        return QStringLiteral("Sleep");
    return QStringLiteral("CodexTool");
}

QJsonObject CodexAppServerSession::toolInput(const QJsonObject &item)
{
    const QString type = item.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("commandExecution"))
        return QJsonObject{{QStringLiteral("command"), item.value(QStringLiteral("command"))},
                           {QStringLiteral("cwd"), item.value(QStringLiteral("cwd"))}};
    if (type == QStringLiteral("fileChange"))
        return QJsonObject{{QStringLiteral("changes"), item.value(QStringLiteral("changes"))}};
    if (type == QStringLiteral("mcpToolCall"))
        return QJsonObject{{QStringLiteral("server"), item.value(QStringLiteral("server"))},
                           {QStringLiteral("tool"), item.value(QStringLiteral("tool"))},
                           {QStringLiteral("arguments"), item.value(QStringLiteral("arguments"))}};
    if (type == QStringLiteral("dynamicToolCall"))
        return QJsonObject{{QStringLiteral("namespace"), item.value(QStringLiteral("namespace"))},
                           {QStringLiteral("tool"), item.value(QStringLiteral("tool"))},
                           {QStringLiteral("arguments"), item.value(QStringLiteral("arguments"))}};
    if (type == QStringLiteral("collabAgentToolCall"))
        return QJsonObject{{QStringLiteral("tool"), item.value(QStringLiteral("tool"))},
                           {QStringLiteral("prompt"), item.value(QStringLiteral("prompt"))},
                           {QStringLiteral("model"), item.value(QStringLiteral("model"))},
                           {QStringLiteral("receiverThreadIds"),
                            item.value(QStringLiteral("receiverThreadIds"))}};
    if (type == QStringLiteral("webSearch"))
        return QJsonObject{{QStringLiteral("query"), item.value(QStringLiteral("query"))},
                           {QStringLiteral("action"), item.value(QStringLiteral("action"))}};
    if (type == QStringLiteral("imageView"))
        return QJsonObject{{QStringLiteral("file_path"), item.value(QStringLiteral("path"))}};
    if (type == QStringLiteral("imageGeneration"))
        return QJsonObject{{QStringLiteral("prompt"),
                            item.value(QStringLiteral("revisedPrompt"))}};
    if (type == QStringLiteral("plan"))
        return item.value(QStringLiteral("todos")).isArray()
                   ? QJsonObject{{QStringLiteral("todos"),
                                  item.value(QStringLiteral("todos"))}}
                   : QJsonObject{{QStringLiteral("todos"),
                                  QJsonArray{QJsonObject{
                                      {QStringLiteral("content"),
                                       item.value(QStringLiteral("text"))},
                                      {QStringLiteral("status"),
                                       QStringLiteral("in_progress")}}}}};
    if (type == QStringLiteral("subAgentActivity"))
        return QJsonObject{
            {QStringLiteral("description"), item.value(QStringLiteral("kind"))},
            {QStringLiteral("agent_thread_id"),
             item.value(QStringLiteral("agentThreadId"))},
            {QStringLiteral("agent_path"), item.value(QStringLiteral("agentPath"))}};
    if (type == QStringLiteral("sleep"))
        return QJsonObject{{QStringLiteral("duration_ms"),
                            item.value(QStringLiteral("durationMs"))}};
    return item;
}

QString CodexAppServerSession::toolOutput(const QJsonObject &item)
{
    const QString type = item.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("commandExecution")) {
        QString output = item.value(QStringLiteral("aggregatedOutput")).toString();
        if (item.value(QStringLiteral("exitCode")).isDouble())
            output += QStringLiteral("\nExit code: %1")
                          .arg(item.value(QStringLiteral("exitCode")).toInt());
        return output.trimmed();
    }
    if (type == QStringLiteral("fileChange"))
        return compactJson(item.value(QStringLiteral("changes")));
    if (type == QStringLiteral("mcpToolCall")) {
        const QJsonValue error = item.value(QStringLiteral("error"));
        return error.isNull() || error.isUndefined()
                   ? compactJson(item.value(QStringLiteral("result")))
                   : compactJson(error);
    }
    if (type == QStringLiteral("dynamicToolCall"))
        return compactJson(item.value(QStringLiteral("contentItems")));
    if (type == QStringLiteral("collabAgentToolCall"))
        return compactJson(item.value(QStringLiteral("agentsStates")));
    if (type == QStringLiteral("webSearch"))
        return compactJson(item.value(QStringLiteral("action")));
    if (type == QStringLiteral("imageView"))
        return item.value(QStringLiteral("path")).toString();
    if (type == QStringLiteral("imageGeneration")) {
        const QString path = item.value(QStringLiteral("savedPath")).toString();
        return path.isEmpty() ? item.value(QStringLiteral("result")).toString() : path;
    }
    if (type == QStringLiteral("plan"))
        return item.value(QStringLiteral("text")).toString();
    if (type == QStringLiteral("subAgentActivity"))
        return compactJson(QJsonObject{
            {QStringLiteral("kind"), item.value(QStringLiteral("kind"))},
            {QStringLiteral("agentThreadId"),
             item.value(QStringLiteral("agentThreadId"))},
            {QStringLiteral("agentPath"), item.value(QStringLiteral("agentPath"))}});
    if (type == QStringLiteral("sleep"))
        return QStringLiteral("Waited %1 ms")
            .arg(item.value(QStringLiteral("durationMs")).toInt());
    return compactJson(item);
}

bool CodexAppServerSession::toolFailed(const QJsonObject &item)
{
    if (item.value(QStringLiteral("success")).isBool() &&
        !item.value(QStringLiteral("success")).toBool())
        return true;
    if (item.value(QStringLiteral("exitCode")).isDouble() &&
        item.value(QStringLiteral("exitCode")).toInt() != 0)
        return true;
    const QString status = item.value(QStringLiteral("status")).toString().toLower();
    return status.contains(QStringLiteral("fail")) ||
           status.contains(QStringLiteral("error")) ||
           status.contains(QStringLiteral("declin")) ||
           status.contains(QStringLiteral("cancel"));
}

QString CodexAppServerSession::messageText(const QJsonValue &value)
{
    if (value.isString())
        return value.toString();
    if (value.isArray()) {
        QStringList parts;
        for (const QJsonValue &part : value.toArray()) {
            const QString text = messageText(part);
            if (!text.isEmpty())
                parts.append(text);
        }
        return parts.join(QLatin1Char('\n'));
    }
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (const QString &key : {QStringLiteral("message"), QStringLiteral("text"),
                                   QStringLiteral("error"), QStringLiteral("reason")}) {
            if (object.contains(key)) {
                const QString text = messageText(object.value(key));
                if (!text.isEmpty())
                    return text;
            }
        }
        return compactJson(object);
    }
    return compactJson(value);
}

QString CodexAppServerSession::decisionLabel(const QJsonValue &decision)
{
    if (!decision.isString())
        return QStringLiteral("Allow with changes");
    const QString value = decision.toString();
    if (value == QStringLiteral("accept"))
        return QStringLiteral("Allow once");
    if (value == QStringLiteral("acceptForSession"))
        return QStringLiteral("Always allow");
    if (value == QStringLiteral("decline"))
        return QStringLiteral("Deny");
    if (value == QStringLiteral("cancel"))
        return QStringLiteral("Cancel");
    return value;
}
