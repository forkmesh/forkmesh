#pragma once

#include "AgentRuntimeSession.h"

#include <QList>

class AgentRuntimeRegistry
{
public:
    static QList<AgentRuntimeDescriptor> builtInRuntimes();
    static AgentRuntimeDescriptor descriptor(const QString &runtimeId);
    static AgentRuntimeSession *create(const QString &runtimeId,
                                       QObject *parent = nullptr);
    static bool protocolCompatible(const AgentRuntimeDescriptor &runtime,
                                   int hostMinimum = 1,
                                   int hostMaximum = 1);
};
