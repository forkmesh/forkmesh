#include "AgentRuntimeRegistry.h"

#include "ClaudeStreamSession.h"
#include "CodexAppServerSession.h"

QList<AgentRuntimeDescriptor> AgentRuntimeRegistry::builtInRuntimes()
{
    CodexAppServerSession codex;
    ClaudeStreamSession claude;
    return {codex.runtimeDescriptor(), claude.runtimeDescriptor()};
}

AgentRuntimeDescriptor AgentRuntimeRegistry::descriptor(const QString &runtimeId)
{
    for (const AgentRuntimeDescriptor &candidate : builtInRuntimes()) {
        if (candidate.id == runtimeId)
            return candidate;
    }
    return {};
}

AgentRuntimeSession *AgentRuntimeRegistry::create(const QString &runtimeId,
                                                  QObject *parent)
{
    if (runtimeId == AgentRuntimeIds::CodexAppServer)
        return new CodexAppServerSession(parent);
    if (runtimeId == AgentRuntimeIds::ClaudeStreamJson)
        return new ClaudeStreamSession(parent);
    return nullptr;
}

bool AgentRuntimeRegistry::protocolCompatible(
    const AgentRuntimeDescriptor &runtime, int hostMinimum, int hostMaximum)
{
    return runtime.isValid() && hostMinimum >= 1 && hostMaximum >= hostMinimum &&
           runtime.protocolMaximum >= hostMinimum &&
           runtime.protocolMinimum <= hostMaximum;
}
