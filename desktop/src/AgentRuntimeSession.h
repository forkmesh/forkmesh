#pragma once

#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

// Provider-neutral contract between ForkMesh and a compatible coding runtime.
// Provider adapters may expose additional native methods, but the UI and
// persistence layers should depend on this vocabulary as they are migrated.
namespace AgentRuntimeCapabilities {
inline const QString SessionLifecycle = QStringLiteral("session.lifecycle");
inline const QString StreamingText = QStringLiteral("output.text.streaming");
inline const QString StreamingReasoning =
    QStringLiteral("output.reasoning.streaming");
inline const QString ToolLifecycle = QStringLiteral("tool.lifecycle");
inline const QString ToolResultSubmission =
    QStringLiteral("tool.result.submit");
inline const QString Usage = QStringLiteral("usage");
inline const QString Permissions = QStringLiteral("permissions");
inline const QString DynamicPermissionMode =
    QStringLiteral("permissions.dynamic-mode");
inline const QString Resume = QStringLiteral("session.resume");
inline const QString Steering = QStringLiteral("turn.steering");
inline const QString Interrupt = QStringLiteral("turn.interrupt");
inline const QString ModelDiscovery = QStringLiteral("models.discovery");
inline const QString Artifacts = QStringLiteral("artifacts");
inline const QString Diagnostics = QStringLiteral("diagnostics");
inline const QString Subagents = QStringLiteral("subagents");
inline const QString TerminalAttachment = QStringLiteral("terminal.attachment");
} // namespace AgentRuntimeCapabilities

namespace AgentRuntimeIds {
inline const QString CodexAppServer = QStringLiteral("codex-app-server");
inline const QString ClaudeStreamJson = QStringLiteral("claude-stream-json");
inline const QString ManagedRunner = QStringLiteral("forkmesh-managed-runner");
} // namespace AgentRuntimeIds

QString agentRuntimeIdForLegacyProvider(const QString &provider);

namespace AgentRuntimeEventTypes {
inline const QString SessionStarted = QStringLiteral("session.started");
inline const QString SessionFinished = QStringLiteral("session.finished");
inline const QString TextDelta = QStringLiteral("output.text.delta");
inline const QString ReasoningDelta = QStringLiteral("output.reasoning.delta");
inline const QString ToolStarted = QStringLiteral("tool.started");
inline const QString ToolUpdated = QStringLiteral("tool.updated");
inline const QString ToolCompleted = QStringLiteral("tool.completed");
inline const QString UsageUpdated = QStringLiteral("usage.updated");
inline const QString PermissionRequested = QStringLiteral("permission.requested");
inline const QString PermissionResolved = QStringLiteral("permission.resolved");
inline const QString ArtifactAvailable = QStringLiteral("artifact.available");
inline const QString DiagnosticReported = QStringLiteral("diagnostic.reported");
inline const QString SubagentUpdated = QStringLiteral("subagent.updated");
inline const QString TerminalAttached = QStringLiteral("terminal.attached");
inline const QString RuntimeNotice = QStringLiteral("runtime.notice");
} // namespace AgentRuntimeEventTypes

namespace AgentRuntimeCommandTypes {
inline const QString InputSubmit = QStringLiteral("input.submit");
inline const QString RequestRespond = QStringLiteral("request.respond");
inline const QString TurnInterrupt = QStringLiteral("turn.interrupt");
inline const QString SessionStop = QStringLiteral("session.stop");
inline const QString PermissionSet = QStringLiteral("permission.set");
inline const QString TerminalInput = QStringLiteral("terminal.input");
} // namespace AgentRuntimeCommandTypes

struct AgentRuntimeDescriptor {
    QString id;
    QString displayName;
    int protocolMinimum = 1;
    int protocolMaximum = 1;
    QSet<QString> capabilities;

    bool isValid(QString *error = nullptr) const;
    bool supports(const QString &capability) const;
    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject &json, AgentRuntimeDescriptor *out,
                         QString *error = nullptr);
};

struct AgentRuntimeLaunch {
    // ForkMesh's logical session identity. Runtime-native resume/thread ids
    // stay in resumeId and must never replace this authorization scope.
    QString sessionId;
    QString workingDirectory;
    QStringList environment;
    QString initialPrompt;
    QString resumeId;
    QString model;
    QString permissionMode;
    QString effort;
    QStringList fallbackModels;
    int memoryLimitMb = 0;
    QString resumeFallbackPrompt;
    QJsonObject options;
};

struct AgentRuntimeEvent {
    int schemaVersion = 1;
    QString type;
    QString sessionId;
    QString requestId;
    qint64 sequence = 0;
    QJsonObject data;

    bool isValid(QString *error = nullptr) const;
    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject &json, AgentRuntimeEvent *out,
                         QString *error = nullptr);
};

struct AgentRuntimeCommand {
    int schemaVersion = 1;
    QString type;
    QString requestId;
    qint64 sequence = 0;
    QJsonObject data;

    bool isValid(QString *error = nullptr) const;
    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject &json, AgentRuntimeCommand *out,
                         QString *error = nullptr);
};

class AgentRuntimeSession : public QObject
{
    Q_OBJECT
public:
    explicit AgentRuntimeSession(QObject *parent = nullptr) : QObject(parent) {}
    ~AgentRuntimeSession() override = default;

    virtual AgentRuntimeDescriptor runtimeDescriptor() const = 0;
    virtual void startRuntime(const AgentRuntimeLaunch &launch) = 0;
    virtual bool sendCommand(const AgentRuntimeCommand &command);
    virtual bool sendUserText(const QString &text) = 0;
    virtual void stop() = 0;
    virtual bool running() const = 0;
    virtual bool acceptsInput() const = 0;
    virtual qint64 processId() const = 0;
    virtual QString resumeId() const = 0;

signals:
    void started();
    void finished(int exitCode);
    void event(const QJsonObject &normalized);
    void rawLine(const QString &line);
    void stderrText(const QString &text);
};
