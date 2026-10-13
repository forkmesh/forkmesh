#pragma once

#include "AgentCapabilityBroker.h"

#include <QString>

// Owner-device persistence for capability policy. Only persistent grants are
// written; session/worktree/once grants remain memory-only and disappear when
// their authority boundary ends.
class AgentCapabilityStore
{
public:
    static QString defaultPath();
    static bool load(const QString &path, AgentCapabilityBroker *broker,
                     QString *error = nullptr);
    static bool save(const QString &path, const AgentCapabilityBroker &broker,
                     QString *error = nullptr);
};
