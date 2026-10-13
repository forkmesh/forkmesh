#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

namespace forkmesh::noderole {

enum class Role { Core, Runner, Mirror, Desktop, Relay };

struct Facts {
    QString name;
    QString declaredRole;
    QString runtimeMode;
    bool self = false;
    bool savedHost = false;
};

struct FleetNames {
    QSet<QString> core;
    QSet<QString> runner;
};

inline QList<Role> allRoles()
{
    return {Role::Core, Role::Runner, Role::Mirror, Role::Desktop, Role::Relay};
}

inline QString key(Role role)
{
    switch (role) {
    case Role::Core:
        return QStringLiteral("core");
    case Role::Runner:
        return QStringLiteral("runner");
    case Role::Mirror:
        return QStringLiteral("mirror");
    case Role::Relay:
        return QStringLiteral("relay");
    case Role::Desktop:
        break;
    }
    return QStringLiteral("desktop");
}

inline QString label(Role role)
{
    switch (role) {
    case Role::Core:
        return QStringLiteral("Core");
    case Role::Runner:
        return QStringLiteral("Runner");
    case Role::Mirror:
        return QStringLiteral("Mirror");
    case Role::Relay:
        return QStringLiteral("Relay");
    case Role::Desktop:
        break;
    }
    return QStringLiteral("Desktop node");
}

inline QString description(Role role)
{
    switch (role) {
    case Role::Core:
        return QStringLiteral(
            "Core server: serves repositories and the app, never runs agent "
            "jobs or Actions");
    case Role::Runner:
        return QStringLiteral(
            "Runner: claims agent jobs from the app and runs them; serves no "
            "repositories");
    case Role::Mirror:
        return QStringLiteral(
            "Mirror server: a headless node that keeps and serves copies of "
            "repositories");
    case Role::Relay:
        return QStringLiteral(
            "Relay: a ForkMesh instance this app talks to for accounts, "
            "repositories and chat");
    case Role::Desktop:
        break;
    }
    return QStringLiteral("Desktop node: a person's ForkMesh app");
}

inline bool roleFromKey(const QString &value, Role *role)
{
    const QString normalized = value.trimmed().toLower();
    for (Role candidate : allRoles()) {
        if (normalized == key(candidate) ||
            normalized == label(candidate).toLower()) {
            if (role)
                *role = candidate;
            return true;
        }
    }
    return false;
}

inline FleetNames parseFleetInstances(const QByteArray &fleetInstancesJson)
{
    FleetNames names;
    static const QRegularExpression core(
        QStringLiteral("^fmnext-(core|auto)-([0-9]{1,3})$"));
    static const QRegularExpression runner(
        QStringLiteral("^fmnext-(run|runner|burst)-([0-9]{1,3})$"));
    const QJsonObject instances =
        QJsonDocument::fromJson(fleetInstancesJson).object();
    for (auto it = instances.constBegin(); it != instances.constEnd(); ++it) {
        const QString label = it.key().trimmed().toLower();
        const QRegularExpressionMatch coreMatch = core.match(label);
        if (coreMatch.hasMatch()) {
            const QString n = coreMatch.captured(2);
            const QString prefix = coreMatch.captured(1) == QLatin1String("auto")
                                       ? QStringLiteral("a")
                                       : QStringLiteral("n");
            names.core.insert(label);
            names.core.insert(prefix + n);
            names.core.insert(QStringLiteral("fmnext-") + prefix + n);
            continue;
        }
        const QRegularExpressionMatch runnerMatch = runner.match(label);
        if (runnerMatch.hasMatch()) {
            const QString n = runnerMatch.captured(2);
            const QString prefix =
                runnerMatch.captured(1) == QLatin1String("burst")
                    ? QStringLiteral("b")
                    : QStringLiteral("r");
            names.runner.insert(label);
            names.runner.insert(prefix + n);
            names.runner.insert(QStringLiteral("fmnext-") + prefix + n);
        }
    }
    return names;
}

inline Role classify(const Facts &facts, const FleetNames &fleet = {})
{
    Role declared;
    if (roleFromKey(facts.declaredRole, &declared))
        return declared;
    const QString name = facts.name.trimmed().toLower();
    static const QRegularExpression corePattern(
        QStringLiteral("^fmnext-(?:n|core-|a|auto-)[0-9]{1,3}$"));
    static const QRegularExpression runnerPattern(
        QStringLiteral("^fmnext-(?:r|run-|runner-|b|burst-)[0-9]{1,3}$"));
    if (fleet.core.contains(name) || corePattern.match(name).hasMatch())
        return Role::Core;
    if (fleet.runner.contains(name) || runnerPattern.match(name).hasMatch())
        return Role::Runner;
    if (facts.self)
        return Role::Desktop;
    if (facts.savedHost)
        return Role::Mirror;
    const QString mode = facts.runtimeMode.trimmed().toLower();
    if (mode == QLatin1String("headless"))
        return Role::Mirror;
    if (mode == QLatin1String("desktop"))
        return Role::Desktop;
    if (name.startsWith(QLatin1String("mirror")))
        return Role::Mirror;
    return Role::Desktop;
}

} // namespace forkmesh::noderole
