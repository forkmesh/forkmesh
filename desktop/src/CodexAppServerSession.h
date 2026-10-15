#pragma once

#include "AgentGuardrails.h"
#include "AgentJsonlFramer.h"
#include "AgentRuntimeSession.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

class AgentProcess;

// Owns a Codex app-server process and adapts its JSONL protocol to the
// Claude-shaped events consumed by ForkMesh's native transcript.
class CodexAppServerSession : public AgentRuntimeSession
{
    Q_OBJECT
public:
    explicit CodexAppServerSession(QObject *parent = nullptr);
    ~CodexAppServerSession() override;

    AgentRuntimeDescriptor runtimeDescriptor() const override;
    void startRuntime(const AgentRuntimeLaunch &launch) override;
    QString resumeId() const override { return threadId(); }

    void start(const QString &cwd, const QStringList &extraEnv,
               const QString &initialPrompt,
               const QString &resumeThreadId = QString(),
               const QString &model = QString(),
               const QString &mode = QString(),
               const QString &effort = QString(),
               int memoryLimitMb = 0,
               const QString &resumeFallbackPrompt = QString(),
               const QString &hostSessionId = QString());
    // Repository guardrails for the next start(); set before startRuntime()
    // so the app-server launches inside the allowlist.
    void setGuardrails(const forkmesh::agentguard::Config &guardrails)
    {
        m_guardrails = guardrails;
    }
    bool sendCommand(const AgentRuntimeCommand &command) override;
    bool sendUserText(const QString &text) override;
    bool respondToRequest(const QString &token, const QString &answer);
    void setTurnOptions(const QString &model, const QString &mode,
                        const QString &effort);
    void interrupt();
    void stop() override;
    bool running() const override;
    // Running *and* still able to take a turn. A failed handshake closes the
    // app-server's stdin while the process winds down, so running() alone can
    // report a transport that no follow-up will ever reach.
    bool acceptsInput() const override;
    qint64 processId() const override;
    bool turnActive() const { return m_turnActive; }
    QString threadId() const;

    // Primarily useful for deterministic protocol tests and packaged Codex
    // binaries. Empty values restore the default `codex app-server` command.
    // Codex `-c key=value` overrides; derived from the launch's disabled
    // tool families by startRuntime().
    void setConfigOverrides(const QStringList &overrides) { m_configOverrides = overrides; }
    QStringList configOverrides() const { return m_configOverrides; }
    void setAppServerCommand(const QString &program,
                             const QStringList &arguments = QStringList());
    bool attachProcess(const QString &key, const QJsonObject &meta, qint64 stdoutOffset,
                       qint64 stderrOffset, qint64 pid);
    QString processKey() const { return m_currentKey; }

signals:
    void modelsListed(const QJsonArray &models);
    void toolCallOutput(const QString &toolName, const QJsonObject &input, const QString &output);

private:
    struct PendingServerRequest {
        QJsonValue rpcId;
        QString method;
        QJsonObject params;
        QStringList questionIds;
        QHash<QString, QString> questionIdsByText;
        QHash<QString, QJsonValue> choices;
    };

    void resetProtocolState();
    void onProcessStarted();
    void onProcessFinished(int exitCode);
    void onStdout(bool flushRemainder = false);
    void onStderr();
    void handleLine(const QByteArray &line);
    void handleMessage(const QJsonObject &message);
    void handleResponse(const QJsonObject &message);
    void handleNotification(const QString &method, const QJsonObject &params);
    void handleServerRequest(const QJsonValue &id, const QString &method,
                             const QJsonObject &params);

    qint64 sendRequest(const QString &method, const QJsonValue &params);
    void sendNotification(const QString &method, const QJsonObject &params);
    bool sendObject(const QJsonObject &message);
    void sendThreadRequest();
    void startFreshThreadAfterResumeFailure(const QString &failure);
    void acceptThread(const QJsonObject &result);
    void beginTurn(const QString &text);
    void flushQueuedUserText();
    QJsonArray userInputs(const QString &text) const;

    void emitSystemInit();
    void emitLocalNotice(const QString &level, const QString &text);
    void emitAgentDelta(const QJsonObject &params);
    void emitReasoningDelta(const QJsonObject &params);
    void emitToolDelta(const QString &method, const QJsonObject &params);
    void emitItemStarted(const QJsonObject &item);
    void emitItemCompleted(const QJsonObject &item);
    void emitSpecialItemNotice(const QJsonObject &item);
    void emitToolUse(const QJsonObject &item);
    void emitToolResult(const QJsonObject &item);
    void finishSyntheticTools(const QString &turnId);
    void emitTurnResult(const QJsonObject &params);
    void emitUsage(const QJsonObject &params);
    void stopProcess(bool finalTeardown);
    void connectProcess(AgentProcess *proc);
    void beginCoreParse(qint64 after);
    void endCoreParse();
    void onCoreRunEvent(const QString &event, const QJsonObject &data);
    void applyCoreState(const QJsonObject &state);
    bool coreOp(const QString &op, QJsonObject params = QJsonObject());
    void finishFromCore(int exitCode);

    static bool isToolItem(const QString &type);
    static QString toolName(const QJsonObject &item);
    static QJsonObject toolInput(const QJsonObject &item);
    static QString toolOutput(const QJsonObject &item);
    static bool toolFailed(const QJsonObject &item);
    static QString messageText(const QJsonValue &value);
    static QString decisionLabel(const QJsonValue &decision);

    AgentProcess *m_proc = nullptr;
    AgentJsonlFramer m_framer;
    qint64 m_nextRequestId = 1;
    quint64 m_nextQuestionToken = 1;
    QHash<qint64, QString> m_pendingCalls;
    QHash<qint64, QString> m_pendingSteerTexts;
    QHash<QString, PendingServerRequest> m_pendingServerRequests;
    QSet<QString> m_toolUses;
    QSet<QString> m_toolResults;
    QSet<QString> m_reasoningDeltas;
    QSet<QString> m_noticeItems;
    QSet<QString> m_resultTurns;
    QHash<QString, QJsonObject> m_syntheticTools;

    QString m_program;
    QStringList m_arguments;
    QStringList m_configOverrides;
    QString m_hostSessionId;
    QString m_cwd;
    QString m_initialPrompt;
    QString m_resumeThreadId;
    QString m_resumeFallbackPrompt;
    QString m_model;
    QString m_effectiveModel;
    QString m_mode;
    QJsonArray m_workspaceWriteRoots;
    bool m_workspaceSandbox = false;
    QString m_effort;
    forkmesh::agentguard::Config m_guardrails;
    QString m_threadId;
    QString m_turnId;
    // A turn/steer request has no policy fields, so keep the mode that the
    // active turn actually started with. A composer mode change must wait for
    // a fresh turn instead of silently steering into the old policy.
    QString m_activeTurnMode;
    QString m_lastAgentText;
    QStringList m_queuedUserTexts;
    QJsonArray m_models;
    QJsonObject m_lastTokenUsage;
    QJsonObject m_rateLimits;
    QJsonObject m_rateLimitsByLimitId;
    bool m_threadReady = false;
    bool m_initialTurnSent = false;
    bool m_turnActive = false;
    bool m_systemInitEmitted = false;
    qint64 m_nextNormalizedSequence = 0;
    QString m_processKey;
    QJsonObject m_processMeta;
    QString m_currentKey;
    bool m_parseRequested = false;
    bool m_coreParsed = false;
    bool m_coreAttachPending = false;
    qint64 m_storedOffset = 0;
    int m_deferredExit = 0;
    bool m_hasDeferredExit = false;
    QSet<QString> m_coreTokens;
};
