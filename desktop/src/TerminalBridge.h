#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace forkmesh::terminalbridge {

inline const QString MethodPrefix = QStringLiteral("terminals.");
inline const QString ListMethod = QStringLiteral("terminals.list");
inline const QString OpenMethod = QStringLiteral("terminals.open");
inline const QString ReadMethod = QStringLiteral("terminals.read");
inline const QString InputMethod = QStringLiteral("terminals.input");
inline const QString CloseMethod = QStringLiteral("terminals.close");
inline const QString TerminalIdEnv = QStringLiteral("FORKMESH_TERMINAL_ID");

inline const QString ShellKind = QStringLiteral("shell");
inline const QString ClaudeKind = QStringLiteral("claude");
inline const QString CodexKind = QStringLiteral("codex");
inline const QString CommandKind = QStringLiteral("command");

inline constexpr int kMaxTabs = 12;
inline constexpr int kMaxInputChars = 8000;
inline constexpr int kMaxKeys = 32;
inline constexpr int kMaxCommandChars = 2000;
inline constexpr int kMaxTitleChars = 60;
inline constexpr int kDefaultReadLines = 200;
inline constexpr int kMaxReadLines = 2000;
inline constexpr int kAgentRankNeeded = 3;
inline constexpr int kInputLimit = 120;
inline constexpr qint64 kInputWindowMs = 60 * 1000;
inline constexpr int kOpenLimit = 10;
inline constexpr qint64 kOpenWindowMs = 10 * 60 * 1000;

struct OpenRequest {
    QString kind = ShellKind;
    QString command;
    QString owner;
    QString name;
    QString title;
};

struct InputRequest {
    QString terminal;
    QByteArray bytes;
    QString summary;
};

struct ReadRequest {
    QString terminal;
    int lines = kDefaultReadLines;
};

QStringList kinds();
QString kindLabel(const QString &kind);
bool validTerminalId(const QString &id);
QString newTerminalId();
QString newToken();

QByteArray keyBytes(const QString &name, bool appCursorKeys, bool *ok);
bool parseOpenRequest(const QJsonObject &params, OpenRequest *out, QString *error);
bool parseInputRequest(const QJsonObject &params, bool appCursorKeys, InputRequest *out,
                       QString *error);
bool parseReadRequest(const QJsonObject &params, ReadRequest *out, QString *error);
QString terminalParam(const QJsonObject &params);

QString shellQuote(const QString &value);
QString launchCommand(const QString &kind, const QString &command, const QString &mcpConfigPath,
                      bool windows);
QString tailLines(const QString &text, int lines);
QString errorMessage(const QString &code);

}
