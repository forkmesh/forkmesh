#include "LocalControlSecrets.h"

namespace forkmesh::localcontrol::secrets {

QString familyPrefix()
{
    return QStringLiteral("secrets.");
}

QStringList mutatingActions()
{
    return {QStringLiteral("set"), QStringLiteral("delete")};
}

bool isMutatingAction(const QString &action)
{
    return mutatingActions().contains(action);
}

bool validName(const QString &name)
{
    if (name.isEmpty() || name.size() > kMaxNameLength)
        return false;
    for (int i = 0; i < name.size(); ++i) {
        const ushort c = name.at(i).unicode();
        const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        const bool digit = c >= '0' && c <= '9';
        if (!letter && !(digit && i > 0))
            return false;
    }
    return true;
}

bool validValue(const QString &value)
{
    return value.toUtf8().size() <= kMaxValueBytes;
}

QString actionSummary(const QString &action, const QString &name)
{
    if (action == QLatin1String("set"))
        return QStringLiteral("Set the Actions variable %1 on this desktop.").arg(name);
    if (action == QLatin1String("delete"))
        return QStringLiteral("Delete the Actions variable %1 from this desktop.").arg(name);
    return {};
}

QString confirmationHolder(const QString &deviceKey, int connectionId)
{
    return deviceKey + QLatin1Char('#') + QString::number(connectionId);
}

QJsonArray listRows(const QMap<QString, QString> &variables)
{
    QJsonArray rows;
    for (auto it = variables.constBegin(); it != variables.constEnd(); ++it) {
        if (rows.size() >= kMaxRows)
            break;
        if (!validName(it.key()))
            continue;
        rows.append(QJsonObject{{QStringLiteral("name"), it.key()}});
    }
    return rows;
}

QMap<QString, QString> applySet(QMap<QString, QString> variables, const QString &name,
                                const QString &value)
{
    if (validName(name) && validValue(value))
        variables.insert(name, value);
    return variables;
}

QMap<QString, QString> applyDelete(QMap<QString, QString> variables, const QString &name)
{
    variables.remove(name);
    return variables;
}

} // namespace forkmesh::localcontrol::secrets
