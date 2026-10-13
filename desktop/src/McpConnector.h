#pragma once

#include <QString>

namespace forkmesh::mcp {

struct Connector
{
    QString token;      // fmcp_<base64url>; the string the agent presents
    QString node;       // base64url identity public key that minted it
    QString label;      // free-form ("Claude Code on this machine")
    qint64 createdMs = 0;

    bool isValid() const { return !token.isEmpty(); }
};

QString connectorPath(const QString &appDataDir);

QString generateToken();
bool isWellFormedToken(const QString &token);

Connector loadConnector(const QString &appDataDir);

// Writes connector.json 0600 (it is a bearer credential). Returns false and
// fills error on any failure.
bool saveConnector(const QString &appDataDir, const Connector &connector,
                   QString *error);

bool revokeConnector(const QString &appDataDir, QString *error);

QString configJson(const QString &python, const QString &serverScript,
                   const QString &reposDir, const QString &token);

QString cliCommand(const QString &python, const QString &serverScript,
                   const QString &reposDir, const QString &token);

QString maskToken(const QString &token);

} // namespace forkmesh::mcp
