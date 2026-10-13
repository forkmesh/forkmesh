#pragma once

#include <QString>
#include <QStringList>

namespace forkmesh::agentenv {

inline QStringList claudeCredentialVariables()
{
    return {QStringLiteral("ANTHROPIC_API_KEY"),
            QStringLiteral("ANTHROPIC_AUTH_TOKEN"),
            QStringLiteral("ANTHROPIC_ADMIN_KEY"),
            QStringLiteral("CLAUDE_CODE_OAUTH_TOKEN")};
}

inline QStringList codexCredentialVariables()
{
    return {QStringLiteral("OPENAI_API_KEY"),
            QStringLiteral("CODEX_API_KEY"),
            QStringLiteral("OPENAI_ACCESS_TOKEN"),
            QStringLiteral("OPENAI_ADMIN_KEY")};
}

inline QStringList providerCredentialVariables()
{
    return claudeCredentialVariables() + codexCredentialVariables();
}

} // namespace forkmesh::agentenv
