#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace forkmesh::composerconnectors {

struct Connector {
    QString name;
    QString source;
};

inline QString builtInName()
{
    return QStringLiteral("forkmesh");
}

inline QString toolPrefix(const QString &name)
{
    static const QRegularExpression unsafe(QStringLiteral("[^A-Za-z0-9_-]"));
    QString normalized = name;
    normalized.replace(unsafe, QStringLiteral("_"));
    return QStringLiteral("mcp__") + normalized;
}

inline QList<Connector> claudeConnectors(const QByteArray &userConfig,
                                         const QString &projectPath,
                                         const QByteArray &projectConfig)
{
    QList<Connector> out;
    QStringList seen{builtInName()};
    const auto add = [&out, &seen](const QJsonObject &servers, const QString &source) {
        for (const QString &name : servers.keys()) {
            if (name.trimmed().isEmpty() || seen.contains(name))
                continue;
            seen << name;
            out.append({name, source});
        }
    };
    const QJsonObject user = QJsonDocument::fromJson(userConfig).object();
    add(user.value(QStringLiteral("mcpServers")).toObject(),
        QStringLiteral("Your Claude Code settings"));
    if (!projectPath.isEmpty())
        add(user.value(QStringLiteral("projects"))
                .toObject()
                .value(projectPath)
                .toObject()
                .value(QStringLiteral("mcpServers"))
                .toObject(),
            QStringLiteral("This project, on this computer"));
    add(QJsonDocument::fromJson(projectConfig)
            .object()
            .value(QStringLiteral("mcpServers"))
            .toObject(),
        QStringLiteral("This project's .mcp.json"));
    return out;
}

inline QStringList disallowedTools(const QStringList &disabled)
{
    QStringList out;
    for (const QString &name : disabled) {
        if (name.trimmed().isEmpty() || name == builtInName())
            continue;
        const QString prefix = toolPrefix(name);
        if (!out.contains(prefix))
            out << prefix;
    }
    return out;
}

}
