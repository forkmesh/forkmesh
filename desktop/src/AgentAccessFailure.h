#pragma once

#include <QJsonObject>
#include <QString>

// Reads a tool call's output for signs that the agent could not reach
// something: a folder outside the project lock, a host its resolver could not
// find, a host it could not connect to. The desktop turns each into a prompt
// that lets the user grant the access, so the agent waits for an answer
// instead of working around the failure (another resolver, a copied file).
//
// Claude Code reports tool output to the gate socket through its PostToolUse
// and PostToolUseFailure hooks (AgentNetworkGate.h); the hook holds the agent
// until the user answers.
namespace forkmesh::agentnet {

struct AccessFailure {
    enum class Kind {
        None,
        Folder,  // permission denied on a path
        Resolve, // a host name did not resolve
        Connect, // a host did not answer (timeout, unreachable network)
    };
    Kind kind = Kind::None;
    QString target;   // absolute path, or host name / address
    bool write = false; // Folder: the call tried to create or change something
    QString evidence; // the output line that shows it, trimmed

    bool found() const { return kind != Kind::None; }
};

// `toolName` and `input` are the Claude Code tool call (Bash's input holds
// "command"); `output` is what came back, error text included. Loopback hosts
// never count.
AccessFailure detectAccessFailure(const QString &toolName, const QJsonObject &input,
                                  const QString &output);

QString accessFailureKindKey(AccessFailure::Kind kind);

} // namespace forkmesh::agentnet
