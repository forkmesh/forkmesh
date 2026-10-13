#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>

namespace forkmesh::localcontrol::secrets {

constexpr int kMaxNameLength = 128;
constexpr int kMaxValueBytes = 16 * 1024;
constexpr int kMaxRows = 500;

QString familyPrefix();
QStringList mutatingActions();
bool isMutatingAction(const QString &action);
bool validName(const QString &name);
bool validValue(const QString &value);
QString actionSummary(const QString &action, const QString &name);
QString confirmationHolder(const QString &deviceKey, int connectionId);
QJsonArray listRows(const QMap<QString, QString> &variables);
QMap<QString, QString> applySet(QMap<QString, QString> variables, const QString &name,
                                const QString &value);
QMap<QString, QString> applyDelete(QMap<QString, QString> variables, const QString &name);

} // namespace forkmesh::localcontrol::secrets
