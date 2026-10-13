#include "ClaudeStreamSession.h"

#include "AgentCliLocator.h"
#include "AgentGuardrails.h"
#include "AgentToolPolicy.h"

#include "AgentJail.h"
#include "AgentResources.h"
#include "VirtualMachineRuntime.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTimer>

namespace {

// The raw pane retains at most 300k characters.  Converting a multi-megabyte
// stream-json event just to immediately discard nearly all of it blocked the UI
// for seconds.  Oversized protocol lines still get parsed and rendered; only
// their redundant debug serialization is represented by this small marker.
constexpr qsizetype kRawLogRetainedCharacters = 300'000;

// The one permission mode the CLI will not switch into mid-session.
constexpr const char *kBypassPermissionsMode = "bypassPermissions";

bool isWhitespaceOnly(const QByteArray &line)
{
    for (const char ch : line) {
        if (ch != ' ' && ch != '\t' && ch != '\r')
            return false;
    }
    return true;
}

QString rawLineForDebugView(const QByteArray &line)
{
    if (line.size() <= kRawLogRetainedCharacters)
        return QString::fromUtf8(line);
    return QStringLiteral(
               "[stream-json line omitted from raw view: %1 bytes; parsed "
               "event retained]")
        .arg(line.size());
}

} // namespace

ClaudeStreamSession::ClaudeStreamSession(QObject *parent)
    : AgentRuntimeSession(parent)
{
    // One parser per transport gives deterministic FIFO delivery without
    // letting several giant JSON values compete for memory at once.
    m_parsePool.setMaxThreadCount(1);
    m_parsePool.setExpiryTimeout(15'000);
}

ClaudeStreamSession::~ClaudeStreamSession()
{
    resetParsing();
    stopProcess(/*finalTeardown=*/true);
    m_parsePool.waitForDone();
    // Parser workers deliver through queued functors.  waitForDone() proves no
    // more can be posted; release any already queued payloads while every
    // transport member is still alive instead of leaving them for QObject's
    // base destructor during application teardown.
    QCoreApplication::removePostedEvents(this);
}

AgentRuntimeDescriptor ClaudeStreamSession::runtimeDescriptor() const
{
    return {AgentRuntimeIds::ClaudeStreamJson,
            QStringLiteral("Claude stream JSON"),
            1,
            1,
            {AgentRuntimeCapabilities::SessionLifecycle,
             AgentRuntimeCapabilities::StreamingText,
             AgentRuntimeCapabilities::StreamingReasoning,
             AgentRuntimeCapabilities::ToolLifecycle,
             AgentRuntimeCapabilities::ToolResultSubmission,
             AgentRuntimeCapabilities::Usage,
             AgentRuntimeCapabilities::Permissions,
             AgentRuntimeCapabilities::DynamicPermissionMode,
             AgentRuntimeCapabilities::Resume,
             AgentRuntimeCapabilities::Steering}};
}

void ClaudeStreamSession::startRuntime(const AgentRuntimeLaunch &launch)
{
    const QStringList disabledFamilies =
        AgentToolPolicy::disabledFamiliesFromOptions(launch.options);
    m_disallowedTools = AgentToolPolicy::claudeDisallowedTools(disabledFamilies);
    static const QRegularExpression safeToolName(QStringLiteral("^[A-Za-z][A-Za-z0-9_]*$"));
    for (const QJsonValue &value :
         launch.options.value(QStringLiteral("disallowedToolNames")).toArray()) {
        const QString name = value.toString();
        if (safeToolName.match(name).hasMatch() && !m_disallowedTools.contains(name))
            m_disallowedTools.append(name);
    }
    const QStringList allowedFamilies =
        AgentToolPolicy::allowedFamiliesFromOptions(launch.options, &m_toolAllowlist);
    m_availableTools = m_toolAllowlist
                           ? AgentToolPolicy::claudeAvailableTools(allowedFamilies,
                                                                   disabledFamilies)
                           : QStringList();
    m_extraSettings = launch.options.value(QStringLiteral("claudeSettings")).toString();
    if (m_extraSettings.contains(QLatin1Char('\'')))
        m_extraSettings.clear(); // would break out of the quoted argument
    start(launch.workingDirectory, launch.environment, launch.initialPrompt,
          launch.permissionMode, launch.resumeId, launch.model, launch.effort,
          launch.fallbackModels.value(0), launch.memoryLimitMb,
          launch.resumeFallbackPrompt);
}

void ClaudeStreamSession::setFinishBarrier(FinishBarrier barrier)
{
    m_finishBarrier = std::move(barrier);
}

void ClaudeStreamSession::start(const QString &cwd, const QStringList &extraEnv,
                                const QString &initialPrompt,
                                const QString &permissionMode,
                                const QString &resumeSessionId, const QString &model,
                                const QString &effort, const QString &fallbackModels,
                                int memoryLimitMb,
                                const QString &resumeFallbackPrompt)
{
    m_cwd = cwd;
    m_extraEnv = extraEnv;
    m_initialPrompt = initialPrompt;
    m_permissionMode = permissionMode.trimmed();
    m_resumeSessionId = resumeSessionId;
    m_model = model;
    m_effort = effort;
    m_fallbackModels = fallbackModels;
    m_memoryLimitMb = memoryLimitMb;
    m_resumeFallbackPrompt = resumeFallbackPrompt.trimmed();
    m_resumeFallbackUsed = false;
    launch();
}

void ClaudeStreamSession::launch()
{
    stop();
    m_framer.reset();
    m_conversationOpened = false;
    m_exitReported = false;
    m_exitBarrierPending = false;

    const QString cwd = m_cwd;
    const QString initialPrompt = m_initialPrompt;

    m_proc = new QProcess(this);
    m_proc->setWorkingDirectory(cwd);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const QString &kv : m_extraEnv) {
        const int eq = kv.indexOf(QLatin1Char('='));
        if (eq > 0)
            env.insert(kv.left(eq), kv.mid(eq + 1));
        else if (eq < 0 && !kv.isEmpty())
            env.remove(kv);
    }
    // The desktop's own PATH is whatever started it: a systemd user service
    // gets the bare system default, without ~/.local/bin where the native
    // installer puts `claude`. A locked launch runs a plain -c shell that
    // never reads ~/.profile, so without this every such session exited 127.
    forkmesh::agentcli::widenPath(env);
    forkmesh::vm::applyGuestEnvironmentPolicy(env);
    forkmesh::agentresources::applyEnvironment(env);
    m_proc->setProcessEnvironment(env);

    connect(m_proc, &QProcess::readyReadStandardOutput, this,
            &ClaudeStreamSession::onStdout);
    connect(m_proc, &QProcess::readyReadStandardError, this,
            &ClaudeStreamSession::onStderr);
    connect(m_proc, &QProcess::started, this, &ClaudeStreamSession::started);
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus) {
                processExited(code);
            });
    connect(m_proc, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                emit stderrText(
                    QStringLiteral(
                        "Claude Code could not be started in %1 (the working "
                        "directory may be gone, or `claude` is not on PATH).\n")
                        .arg(m_cwd));
                reportExit(-1);
            });

    QString cmd = QStringLiteral(
        "exec claude --output-format stream-json --input-format stream-json "
        "--verbose --include-partial-messages");
    // Auto is the skip-permissions switch; every other mode is a
    // --permission-mode the CLI enforces per tool call. Passing the flag is
    // what makes "Plan" read-only and "Edit" edit-but-ask instead of all three
    // non-Auto labels collapsing into the same ask-for-everything run.
    m_launchedPermissionMode = m_permissionMode;
    m_pendingPermissionMode.clear();
    m_pendingPermissionRequestId.clear();
    m_launchedBypassing = m_permissionMode == QLatin1String(kBypassPermissionsMode);
    if (m_launchedBypassing)
        cmd += QStringLiteral(" --dangerously-skip-permissions");
    else if (!m_permissionMode.isEmpty())
        cmd += QStringLiteral(" --permission-mode '%1'").arg(m_permissionMode);
    if (!m_model.trimmed().isEmpty())
        cmd += QStringLiteral(" --model '%1'").arg(m_model.trimmed());
    if (!m_effort.trimmed().isEmpty())
        cmd += QStringLiteral(" --effort '%1'").arg(m_effort.trimmed());
    // Tool families the operator switched off. Names are validated by the
    // policy helper, so they are safe inside the single-quoted argument.
    if (!m_disallowedTools.isEmpty())
        cmd += QStringLiteral(" --disallowedTools '%1'")
                   .arg(m_disallowedTools.join(QLatin1Char(',')));
    if (m_toolAllowlist)
        cmd += QStringLiteral(" --tools '%1'").arg(m_availableTools.join(QLatin1Char(',')));
    if (!m_extraSettings.isEmpty())
        cmd += QStringLiteral(" --settings '%1'").arg(m_extraSettings);
    if (!m_fallbackModels.trimmed().isEmpty())
        cmd += QStringLiteral(" --fallback-model '%1'").arg(m_fallbackModels.trimmed());
    if (!m_resumeSessionId.isEmpty())
        cmd += QStringLiteral(" --resume '%1'").arg(m_resumeSessionId);
    cmd = AgentJail::wrapCommand(cmd, m_memoryLimitMb);
    cmd = forkmesh::agentresources::wrapShellCommand(cmd);
    const forkmesh::agentguard::LaunchCommand guarded =
        forkmesh::agentguard::restrictedCommand(
            QStringLiteral("bash"), {QStringLiteral("-lc"), cmd}, cwd,
            m_guardrails, env);
    if (!guarded.error.isEmpty()) {
        QProcess *failed = m_proc;
        m_proc = nullptr;
        failed->deleteLater();
        emit stderrText(QStringLiteral("Agent guardrails refused the launch: %1\n")
                            .arg(guarded.error));
        QTimer::singleShot(0, this, [this] { reportExit(-1); });
        return;
    }
    const forkmesh::vm::LaunchCommand launchCmd =
        forkmesh::vm::isolateCommand(guarded.program, guarded.arguments, cwd);
    if (!launchCmd.error.isEmpty()) {
        QProcess *failed = m_proc;
        m_proc = nullptr;
        failed->deleteLater();
        emit stderrText(QStringLiteral("KVM launch failed: %1").arg(launchCmd.error));
        QTimer::singleShot(0, this, [this] { reportExit(-1); });
        return;
    }
    const forkmesh::agentresources::Launch contained =
        launchCmd.isolated
            ? forkmesh::agentresources::Launch{launchCmd.program,
                                               launchCmd.arguments}
            : forkmesh::agentresources::contain(
                  launchCmd.program, launchCmd.arguments,
                  forkmesh::agentresources::launchLabel(
                      QStringLiteral("claude"), env));
    m_proc->start(contained.program, contained.arguments);

    if (!initialPrompt.isEmpty())
        writeUserTurn(initialPrompt);
}

bool ClaudeStreamSession::setPermissionMode(const QString &permissionMode)
{
    const QString mode = permissionMode.trimmed();
    if (mode.isEmpty())
        return false;
    if (!running()) {
        m_permissionMode = mode; // the next launch() carries it as its flag
        return true;
    }
    if (mode == m_permissionMode)
        return true;
    // "Cannot set permission mode to bypassPermissions because the session was
    // not launched with --dangerously-skip-permissions" — the CLI answers the
    // request with that error rather than switching, so don't ask: report the
    // switch as impossible and let the caller restart the session under it.
    if (mode == QLatin1String(kBypassPermissionsMode) && !m_launchedBypassing)
        return false;
    const QString requestId =
        QStringLiteral("forkmesh-permission-%1").arg(++m_controlRequestSeq);
    if (!writeLine(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("control_request")},
            {QStringLiteral("request_id"), requestId},
            {QStringLiteral("request"),
             QJsonObject{{QStringLiteral("subtype"),
                          QStringLiteral("set_permission_mode")},
                         {QStringLiteral("mode"), mode}}}}))
        return false;
    m_permissionMode = mode;
    m_pendingPermissionMode = mode;
    m_pendingPermissionRequestId = requestId;
    return true;
}

void ClaudeStreamSession::notePermissionModeResponse(const QJsonObject &ev)
{
    if (m_pendingPermissionMode.isEmpty() ||
        ev.value(QStringLiteral("type")).toString() !=
            QLatin1String("control_response"))
        return;
    const QJsonObject response = ev.value(QStringLiteral("response")).toObject();
    if (response.value(QStringLiteral("request_id")).toString() !=
        m_pendingPermissionRequestId)
        return;
    // A CLI too old for set_permission_mode (or one refusing the mode) answers
    // with an error instead of switching. Keep permissionMode() honest about
    // what the process is really enforcing: the mode is stored on the session
    // either way, so the next launch still carries it.
    if (response.value(QStringLiteral("subtype")).toString() ==
        QLatin1String("error"))
        m_permissionMode = m_launchedPermissionMode;
    m_pendingPermissionMode.clear();
    m_pendingPermissionRequestId.clear();
}

bool ClaudeStreamSession::sendUserText(const QString &text)
{
    if (text.trimmed().isEmpty())
        return true; // nothing to deliver, so nothing failed to arrive
    return writeUserTurn(text);
}

bool ClaudeStreamSession::sendCommand(const AgentRuntimeCommand &command)
{
    if (!command.isValid())
        return false;
    if (command.type == AgentRuntimeCommandTypes::RequestRespond) {
        QString answer = command.data.value(QStringLiteral("answer")).toString();
        if (answer.isEmpty())
            answer =
                command.data.value(QStringLiteral("decision")).toString();
        return !answer.isEmpty() &&
               sendToolResult(command.requestId, answer);
    }
    if (command.type == AgentRuntimeCommandTypes::PermissionSet) {
        const QJsonValue mode = command.data.value(QStringLiteral("mode"));
        return mode.isString() && setPermissionMode(mode.toString());
    }
    return AgentRuntimeSession::sendCommand(command);
}

bool ClaudeStreamSession::writeUserTurn(const QString &text)
{
    return writeLine(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("user")},
        {QStringLiteral("message"),
         QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                     {QStringLiteral("content"), text}}}});
}

bool ClaudeStreamSession::sendToolResult(const QString &toolUseId,
                                         const QString &content)
{
    if (toolUseId.isEmpty())
        return false;
    const QJsonObject block{
        {QStringLiteral("type"), QStringLiteral("tool_result")},
        {QStringLiteral("tool_use_id"), toolUseId},
        {QStringLiteral("content"), content}};
    return writeLine(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("user")},
        {QStringLiteral("message"),
         QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                     {QStringLiteral("content"), QJsonArray{block}}}}});
}

bool ClaudeStreamSession::writeLine(const QJsonObject &msg)
{
    if (!running())
        return false;
    if (!m_proc->isWritable())
        return false;
    return m_proc->write(QJsonDocument(msg).toJson(QJsonDocument::Compact) + '\n') >= 0;
}

void ClaudeStreamSession::reportExit(int exitCode)
{
    if (m_exitReported || m_exitBarrierPending)
        return;
    if (!m_finishBarrier) {
        m_exitReported = true;
        emit finished(exitCode);
        return;
    }

    m_exitBarrierPending = true;
    const quint64 generation = m_parseGeneration;
    QPointer<ClaudeStreamSession> guard(this);
    m_finishBarrier([guard, generation, exitCode] {
        if (!guard)
            return;
        QMetaObject::invokeMethod(
            guard,
            [guard, generation, exitCode] {
                if (!guard || guard->m_parseGeneration != generation ||
                    !guard->m_exitBarrierPending || guard->m_exitReported)
                    return;
                guard->m_exitBarrierPending = false;
                guard->m_exitReported = true;
                emit guard->finished(exitCode);
            },
            Qt::QueuedConnection);
    });
}

void ClaudeStreamSession::stop()
{
    resetParsing();
    stopProcess(/*finalTeardown=*/false);
}

void ClaudeStreamSession::resetParsing()
{
    // Any queued parser callbacks belong to the launch being stopped.  A new
    // launch starts with a fresh generation, while an in-flight parse may finish
    // harmlessly in the background.
    ++m_parseGeneration;
    m_parsePool.clear();
    m_parsedLines.clear();
    m_nextQueuedLine = 0;
    m_nextDeliveredLine = 0;
    m_pendingParseCount = 0;
    m_pendingProcessExit.reset();
    m_exitBarrierPending = false;
    m_framer.reset();
}

// code-health: allow(blocking-wait) waits only on the finalTeardown path from the destructor, bounded to 2.5 s
void ClaudeStreamSession::stopProcess(bool finalTeardown)
{
    if (!m_proc)
        return;
    QProcess *proc = m_proc;
    m_proc = nullptr;
    proc->disconnect(this);
    if (finalTeardown) {
        // MainWindow is destroyed after QApplication::exec() returns.  At that
        // point deleteLater() has no event-loop turn left in which to run.
        // Drain the process here so the transport does not detach a live or
        // stopped QObject into Qt's global shutdown.
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
    proc->closeWriteChannel(); // signal end-of-input first
    proc->terminate();
    QTimer::singleShot(1500, proc, [proc] {
        if (proc->state() != QProcess::NotRunning)
            proc->kill(); // finished() → deleteLater() then frees it
    });
}

bool ClaudeStreamSession::running() const
{
    return m_proc && m_proc->state() != QProcess::NotRunning;
}

bool ClaudeStreamSession::acceptsInput() const
{
    return running() && m_proc->isWritable();
}

qint64 ClaudeStreamSession::processId() const
{
    return running() ? m_proc->processId() : 0;
}

void ClaudeStreamSession::onStdout()
{
    if (!m_proc)
        return;
    ingestStdout(m_proc->readAllStandardOutput());
}

void ClaudeStreamSession::ingestStdout(QByteArray bytes)
{
    AgentJsonlFramer::Result framed = m_framer.append(std::move(bytes));
    for (const QString &error : std::as_const(framed.errors)) {
        emit stderrText(error);
        emit event(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("_runtime_protocol_error")},
            {QStringLiteral("message"), error}});
    }
    for (QByteArray &line : framed.frames) {
        if (line.isEmpty() || isWhitespaceOnly(line))
            continue;
        queueStdoutLine(std::move(line));
    }
}

void ClaudeStreamSession::queueStdoutLine(QByteArray line)
{
    const quint64 generation = m_parseGeneration;
    const quint64 sequence = m_nextQueuedLine++;
    ++m_pendingParseCount;
    QPointer<ClaudeStreamSession> guard(this);
    m_parsePool.start([guard, generation, sequence, line = std::move(line)]() mutable {
        ParsedLine parsed;
        parsed.raw = rawLineForDebugView(line);
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject()) {
            parsed.event = doc.object();
            parsed.hasEvent = true;
            const QString type =
                parsed.event.value(QStringLiteral("type")).toString();
            const QString subtype =
                parsed.event.value(QStringLiteral("subtype")).toString();
            // A failed --resume can itself produce a parsed `system/error`
            // event.  Only the initialization envelope proves that a Claude
            // conversation actually opened; status/error envelopes must not
            // suppress the one-shot fallback.
            parsed.opensConversation =
                type == QLatin1String("system") &&
                (subtype == QLatin1String("init") ||
                 (subtype.isEmpty() &&
                  !parsed.event.value(QStringLiteral("session_id"))
                       .toString()
                       .isEmpty() &&
                  parsed.event.value(QStringLiteral("error")).isUndefined()));
        }
        if (!guard)
            return;
        QMetaObject::invokeMethod(
            guard,
            [guard, generation, sequence, parsed = std::move(parsed)]() mutable {
                if (guard)
                    guard->receiveParsedLine(sequence, generation,
                                             std::move(parsed));
            },
            Qt::QueuedConnection);
    });
}

void ClaudeStreamSession::receiveParsedLine(quint64 sequence,
                                            quint64 generation,
                                            ParsedLine parsed)
{
    if (generation != m_parseGeneration)
        return;
    m_parsedLines.insert(sequence, std::move(parsed));
    deliverParsedLines();
}

void ClaudeStreamSession::deliverParsedLines()
{
    while (true) {
        auto it = m_parsedLines.find(m_nextDeliveredLine);
        if (it == m_parsedLines.end())
            break;
        ParsedLine parsed = std::move(it.value());
        m_parsedLines.erase(it);
        ++m_nextDeliveredLine;
        --m_pendingParseCount;

        emit rawLine(parsed.raw);
        if (parsed.hasEvent) {
            if (parsed.opensConversation)
                m_conversationOpened = true;
            notePermissionModeResponse(parsed.event);
            emit event(parsed.event);
        }
    }
    finishAfterParsing();
}

void ClaudeStreamSession::processExited(int exitCode)
{
    // QProcess may have bytes available at finished() even if the final
    // readyRead signal has not run yet.
    onStdout();
    m_pendingProcessExit = exitCode;
    finishAfterParsing();
}

void ClaudeStreamSession::finishAfterParsing()
{
    if (m_pendingParseCount > 0 || !m_pendingProcessExit.has_value())
        return;
    const int exitCode = *m_pendingProcessExit;
    m_pendingProcessExit.reset();
    if (!m_conversationOpened && !m_resumeSessionId.isEmpty() &&
        !m_resumeFallbackUsed) {
        m_resumeFallbackUsed = true;
        m_resumeSessionId.clear();
        if (!m_resumeFallbackPrompt.isEmpty())
            m_initialPrompt = m_resumeFallbackPrompt;
        emit event(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("_local_notice")},
            {QStringLiteral("level"), QStringLiteral("warning")},
            {QStringLiteral("text"),
             QStringLiteral(
                 "The saved Claude Code conversation could not be resumed, so "
                 "ForkMesh started a new one with the saved task and latest "
                 "instruction. The branch and its work are untouched.")}});
#ifdef FORKMESH_AGENT_TRANSPORT_TESTS
        if (m_testRelaunchHandler) {
            const std::function<void()> relaunch = m_testRelaunchHandler;
            // Mirror launch()'s generation reset without executing an external
            // Claude binary in the focused transport test.
            stop();
            m_framer.reset();
            m_conversationOpened = false;
            m_exitReported = false;
            m_exitBarrierPending = false;
            relaunch();
            return;
        }
#endif
        launch();
        return;
    }
    reportExit(exitCode);
}

void ClaudeStreamSession::onStderr()
{
    if (m_proc)
        emit stderrText(QString::fromUtf8(m_proc->readAllStandardError()));
}

#ifdef FORKMESH_AGENT_TRANSPORT_TESTS
void ClaudeStreamSession::ingestStdoutForTest(const QByteArray &bytes)
{
    ingestStdout(bytes);
}

void ClaudeStreamSession::reportProcessExitForTest(int exitCode)
{
    m_pendingProcessExit = exitCode;
    finishAfterParsing();
}

void ClaudeStreamSession::configureResumeFallbackForTest(
    const QString &resumeSessionId,
    std::function<void()> relaunchHandler)
{
    m_resumeSessionId = resumeSessionId;
    m_resumeFallbackUsed = false;
    m_testRelaunchHandler = std::move(relaunchHandler);
}
#endif
