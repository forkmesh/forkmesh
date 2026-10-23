#pragma once

#include "AgentGuardrails.h"
#include "AgentJsonlFramer.h"
#include "AgentRuntimeSession.h"

#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThreadPool>

#include <functional>
#include <optional>

class QProcess;

class ClaudeStreamSession : public AgentRuntimeSession
{
    Q_OBJECT
public:
    explicit ClaudeStreamSession(QObject *parent = nullptr);
    ~ClaudeStreamSession() override;

    AgentRuntimeDescriptor runtimeDescriptor() const override;
    void startRuntime(const AgentRuntimeLaunch &launch) override;
    QString resumeId() const override { return m_resumeSessionId; }

    // permissionMode is the CLI's own id ("bypassPermissions", "plan",
    // "acceptEdits", "default"); empty launches under the CLI's default.
    void start(const QString &cwd, const QStringList &extraEnv,
               const QString &initialPrompt,
               const QString &permissionMode = QString(),
               const QString &resumeSessionId = QString(),
               const QString &model = QString(),
               const QString &effort = QString(),
               const QString &fallbackModels = QString(),
               int memoryLimitMb = 0,
               const QString &resumeFallbackPrompt = QString());
    QString permissionMode() const { return m_permissionMode; }
    // Repository guardrails for the next start(); set before startRuntime()
    // so the CLI launches inside the allowlist.
    void setGuardrails(const forkmesh::agentguard::Config &guardrails)
    {
        m_guardrails = guardrails;
    }
    // Claude tool names passed through --disallowedTools; derived from the
    // launch's disabled tool families by startRuntime().
    void setDisallowedTools(const QStringList &tools) { m_disallowedTools = tools; }
    QStringList disallowedTools() const { return m_disallowedTools; }
    QStringList availableTools() const { return m_toolAllowlist ? m_availableTools : QStringList(); }
    // Switches the mode a running session applies from its next turn on, via
    // the CLI's set_permission_mode control request. Returns false when the
    // switch cannot be made — promoting a running session to bypassPermissions
    // is refused unless it was launched with --dangerously-skip-permissions —
    // in which case only a restart under the new mode carries it over.
    bool setPermissionMode(const QString &permissionMode);
    bool sendCommand(const AgentRuntimeCommand &command) override;
    bool sendUserText(const QString &text) override;
    bool sendToolResult(const QString &toolUseId, const QString &content);
    void stop() override;
    bool running() const override;
    bool acceptsInput() const override;
    qint64 processId() const override;

    // Event delivery is synchronous, but transcript persistence is deliberately
    // asynchronous.  The owner can install a barrier so finished() cannot race
    // past the final event writes it scheduled while handling event().
    using FinishBarrier =
        std::function<void(std::function<void()> completion)>;
    void setFinishBarrier(FinishBarrier barrier);

#ifdef FORKMESH_AGENT_TRANSPORT_TESTS
    void ingestStdoutForTest(const QByteArray &bytes);
    void reportProcessExitForTest(int exitCode);
    void configureResumeFallbackForTest(
        const QString &resumeSessionId,
        std::function<void()> relaunchHandler);
#endif

private:
    struct ParsedLine {
        QString raw;
        QJsonObject event;
        bool hasEvent = false;
        bool opensConversation = false;
    };

    void launch();
    void onStdout();
    void onStderr();
    void ingestStdout(QByteArray bytes);
    void queueStdoutLine(QByteArray line);
    void receiveParsedLine(quint64 sequence, quint64 generation,
                           ParsedLine parsed);
    void deliverParsedLines();
    void notePermissionModeResponse(const QJsonObject &ev);
    void processExited(int exitCode);
    void finishAfterParsing();
    bool writeUserTurn(const QString &text);
    bool writeLine(const QJsonObject &msg);
    void reportExit(int exitCode);
    void resetParsing();
    void stopProcess(bool finalTeardown);

    QProcess *m_proc = nullptr;
    AgentJsonlFramer m_framer;
    QThreadPool m_parsePool;
    QMap<quint64, ParsedLine> m_parsedLines;
    quint64 m_parseGeneration = 0;
    quint64 m_nextQueuedLine = 0;
    quint64 m_nextDeliveredLine = 0;
    int m_pendingParseCount = 0;
    std::optional<int> m_pendingProcessExit;
    QString m_cwd;
    QStringList m_extraEnv;
    QStringList m_disallowedTools;
    QStringList m_availableTools;
    bool m_toolAllowlist = false;
    // Extra --settings JSON (the external-call gate hook); no single quotes.
    QString m_extraSettings;
    QString m_initialPrompt;
    QString m_permissionMode;
    QString m_launchedPermissionMode; // what the running process started under
    QString m_pendingPermissionMode;  // switch awaiting the CLI's answer
    QString m_pendingPermissionRequestId;
    // Whether the process now running was launched with the skip-permissions
    // switch: the CLI only lets a session be put into bypassPermissions if it
    // was, so this is what setPermissionMode() checks before asking.
    bool m_launchedBypassing = false;
    quint64 m_controlRequestSeq = 0;
    QString m_resumeSessionId;
    bool m_forkSession = false;
    QString m_model;
    QString m_effort;
    QString m_fallbackModels;
    int m_memoryLimitMb = 0;
    forkmesh::agentguard::Config m_guardrails;
    QString m_resumeFallbackPrompt;
    bool m_conversationOpened = false;
    bool m_resumeFallbackUsed = false; // one retry per start(), never a loop
    bool m_exitReported = false;       // one finished() per launch, never two
    bool m_exitBarrierPending = false;
    FinishBarrier m_finishBarrier;
#ifdef FORKMESH_AGENT_TRANSPORT_TESTS
    std::function<void()> m_testRelaunchHandler;
#endif
};
