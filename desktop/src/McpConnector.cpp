#include "McpConnector.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>

namespace forkmesh::mcp {

namespace {

QString shellQuote(const QString &value)
{
    static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9_@%+=:,./-]+$"));
    if (value.isEmpty())
        return QStringLiteral("''");
    if (safe.match(value).hasMatch())
        return value;
    QString escaped = value;
    escaped.replace(QLatin1String("'"), QLatin1String("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

} // namespace

QString connectorPath(const QString &appDataDir)
{
    return QDir(appDataDir).absoluteFilePath(QStringLiteral("mcp/connector.json"));
}

QString generateToken()
{
    QByteArray raw(32, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(
        reinterpret_cast<quint32 *>(raw.data()), raw.size() / 4);
    return QStringLiteral("fmcp_") +
           QString::fromLatin1(raw.toBase64(QByteArray::Base64UrlEncoding |
                                            QByteArray::OmitTrailingEquals));
}

bool isWellFormedToken(const QString &token)
{
    static const QRegularExpression shape(
        QStringLiteral("^fmcp_[A-Za-z0-9_-]{32,}$"));
    return shape.match(token).hasMatch();
}

Connector loadConnector(const QString &appDataDir)
{
    Connector connector;
    QFile file(connectorPath(appDataDir));
    if (!file.open(QIODevice::ReadOnly))
        return connector;
    const QJsonObject object =
        QJsonDocument::fromJson(file.readAll()).object();
    const QString token = object.value(QStringLiteral("token")).toString();
    if (!isWellFormedToken(token))
        return connector; // truncated or hand-edited file: treat as absent
    connector.token = token;
    connector.node = object.value(QStringLiteral("node")).toString();
    connector.label = object.value(QStringLiteral("label")).toString();
    connector.createdMs =
        static_cast<qint64>(object.value(QStringLiteral("created_ms")).toDouble());
    return connector;
}

bool saveConnector(const QString &appDataDir, const Connector &connector,
                   QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!isWellFormedToken(connector.token))
        return fail(QStringLiteral("Refusing to save a malformed connector token."));

    const QString path = connectorPath(appDataDir);
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return fail(QStringLiteral("Could not create %1.")
                        .arg(QFileInfo(path).absolutePath()));

    const QJsonObject object{
        {QStringLiteral("version"), 1},
        {QStringLiteral("token"), connector.token},
        {QStringLiteral("node"), connector.node},
        {QStringLiteral("label"), connector.label},
        {QStringLiteral("created_ms"),
         static_cast<double>(connector.createdMs != 0
                                 ? connector.createdMs
                                 : QDateTime::currentMSecsSinceEpoch())},
    };

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(QStringLiteral("Could not write %1.").arg(path));
    // Owner-only before anything is written: this is a bearer credential that
    // signs as this node, exactly like the identity key next to it.
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    const QByteArray payload =
        QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size())
        return fail(QStringLiteral("Could not write %1.").arg(path));
    file.close();
    return true;
}

bool revokeConnector(const QString &appDataDir, QString *error)
{
    const QString path = connectorPath(appDataDir);
    if (!QFile::exists(path))
        return true;
    if (QFile::remove(path))
        return true;
    if (error)
        *error = QStringLiteral("Could not remove %1.").arg(path);
    return false;
}

QString configJson(const QString &python, const QString &serverScript,
                   const QString &reposDir, const QString &token)
{
    QJsonObject env;
    if (!reposDir.isEmpty())
        env.insert(QStringLiteral("FORKMESH_REPOS_DIR"), reposDir);
    if (!token.isEmpty())
        env.insert(QStringLiteral("FORKMESH_MCP_TOKEN"), token);

    QJsonObject server{
        {QStringLiteral("command"),
         python.isEmpty() ? QStringLiteral("python3") : python},
        {QStringLiteral("args"), QJsonArray{serverScript}},
    };
    if (!env.isEmpty())
        server.insert(QStringLiteral("env"), env);

    const QJsonObject root{
        {QStringLiteral("mcpServers"),
         QJsonObject{{QStringLiteral("forkmesh"), server}}},
    };
    return QString::fromUtf8(
        QJsonDocument(root).toJson(QJsonDocument::Indented).trimmed());
}

QString cliCommand(const QString &python, const QString &serverScript,
                   const QString &reposDir, const QString &token)
{
    QStringList parts{QStringLiteral("claude"), QStringLiteral("mcp"),
                      QStringLiteral("add"), QStringLiteral("forkmesh"),
                      QStringLiteral("--scope"), QStringLiteral("user")};
    if (!reposDir.isEmpty())
        parts << QStringLiteral("--env")
              << shellQuote(QStringLiteral("FORKMESH_REPOS_DIR=") + reposDir);
    if (!token.isEmpty())
        parts << QStringLiteral("--env")
              << shellQuote(QStringLiteral("FORKMESH_MCP_TOKEN=") + token);
    parts << QStringLiteral("--")
          << shellQuote(python.isEmpty() ? QStringLiteral("python3") : python)
          << shellQuote(serverScript);
    return parts.join(QLatin1Char(' '));
}

QString maskToken(const QString &token)
{
    if (token.isEmpty())
        return {};
    if (token.size() <= 16)
        return token;
    return token.left(9) + QString::fromUtf8("\xe2\x80\xa6") + token.right(4);
}

} // namespace forkmesh::mcp
