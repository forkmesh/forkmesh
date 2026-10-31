#pragma once

#include "AgentAccessFailure.h"
#include "CommandVariables.h"

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>

class QLocalServer;
class QLocalSocket;

// The external-call gate: every tool call an agent makes that would leave the
// machine (a web fetch, a search, `curl`, `git push`, a package install) waits
// for the project's permission before it runs.
//
// Claude Code enforces it through a PreToolUse hook (claudeHookSettings())
// that sends each Bash/Monitor/WebFetch/WebSearch call to this process over a local
//
// The gate works at the tool level. It catches the calls an agent makes the
// ordinary way; it is not a network sandbox against an agent determined to
// hide a connection inside a program it wrote.
namespace forkmesh::agentnet {

struct ExternalCall {
    bool external = false;
    QString target;  // host, or a label such as "web search" when there is none
    QString summary; // one line naming what the call does
};

// Whether `command` (a shell command line) reaches the network, and where.
// `cwd` resolves named git remotes; a remote that is a local path is local.
ExternalCall classifyCommand(const QString &command, const QString &cwd);
bool isShellTool(const QString &toolName);
// Same for a Claude Code tool call (Bash, Monitor, WebFetch, WebSearch).
ExternalCall classifyToolCall(const QString &toolName, const QJsonObject &input,
                              const QString &cwd);

struct CommandRisk {
    QString destructive;
    QString permission;
    QString summary;
    QString command;
    qsizetype matchStart = -1;
    qsizetype matchLength = 0;
    QList<std::pair<qsizetype, qsizetype>> removeSpans;
    bool onlyRemoves = false;
    QStringList removedPaths;
    QString scratch;
    bool any() const { return !destructive.isEmpty() || !permission.isEmpty(); }
};
CommandRisk classifyCommandRisk(const QString &command, const QString &scratch = QString());
CommandRisk classifyCommandRisk(const QString &command,
                                const forkmesh::commandvars::Environment &environment);

// Loopback addresses and localhost names never count as external.
bool isLocalHost(const QString &host);

// Environment variables the hook reads.
inline const QString SocketEnv = QStringLiteral("FORKMESH_AGENT_GATE");
inline const QString SessionEnv = QStringLiteral("FORKMESH_AGENT_GATE_SESSION");
inline const QString LabelEnv = QStringLiteral("FORKMESH_AGENT_GATE_LABEL");

// The Python hook, and the `--settings` JSON that installs it. The JSON holds
// no single quote, so it can sit inside a single-quoted shell argument.
// The PreToolUse gate is installed only with `gateCalls`; the PostToolUse and
// PostToolUseFailure hooks that report access failures always are.
QString hookScript();
QString claudeHookSettings(bool gateCalls = true);
// `command` with `--settings '<json>'` after its first `claude` word; the
// command unchanged when it runs no `claude`.
QString withClaudeHookSettings(const QString &command);
QString withClaudeArguments(const QString &command, const QString &arguments);

struct Request {
    QString id;
    QString session; // opaque per-launch token from SessionEnv
    QString label;   // who is asking, from LabelEnv
    QString tool;
    ExternalCall call;
    CommandRisk risk;
    QJsonObject input;
    QString cwd;
    forkmesh::commandvars::Environment environment;
    // Set on a report from the post-tool hooks instead of a call to approve.
    AccessFailure failure;
};

// Local socket the hooks connect to. One line of JSON in, one line out. The
// socket answers only the connection that asked, so an agent can ask but can
// never answer its own request.
class Server
{
public:
    enum class Decision { Allow, Deny };
    // Called for each external request; call answer() later, from anywhere
    // on this thread. Local calls are answered at once without a callback.
    using RequestHandler = std::function<void(const Request &)>;
    // Called for each tool output that shows an access failure; call
    // answerFailure() later. Outputs without one are answered at once.
    using FailureHandler = std::function<void(const Request &)>;
    // Called when a request's hook gave up waiting (timeout, agent stopped).
    using CancelHandler = std::function<void(const QString &id)>;

    Server();
    ~Server();

    bool listen(QString *error = nullptr);
    QString socketPath() const;
    void setRequestHandler(RequestHandler handler) { m_onRequest = std::move(handler); }
    void setFailureHandler(FailureHandler handler) { m_onFailure = std::move(handler); }
    void setCancelHandler(CancelHandler handler) { m_onCancel = std::move(handler); }
    void answer(const QString &id, Decision decision, const QString &reason = QString());
    // `context` reaches the agent as the hook's additional context.
    void answerFailure(const QString &id, const QString &context);
    int pendingCount() const { return m_pending.size(); }

private:
    void accept();
    void readRequest(QLocalSocket *socket);

    QLocalServer *m_server = nullptr;
    QHash<QString, QLocalSocket *> m_pending;
    void reply(const QString &id, const QJsonObject &object);

    RequestHandler m_onRequest;
    FailureHandler m_onFailure;
    CancelHandler m_onCancel;
    quint64 m_nextId = 1;
};

} // namespace forkmesh::agentnet
