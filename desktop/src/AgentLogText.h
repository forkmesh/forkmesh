#pragma once

#include <QJsonObject>
#include <QString>

namespace AgentLogText {

QString toolLine(const QString &name, const QJsonObject &input);
QString assistantContent(const QJsonObject &message);
QString taskNotification(const QString &text);
bool isTaskNotificationText(const QString &text);
QString taskNotificationNotice(const QString &text);
bool isInjectedPrompt(const QString &text);
QString userEvent(const QJsonObject &event);
QString claudeEntry(const QJsonObject &entry);
QString codexEntry(const QJsonObject &entry);
QString outsideEntry(const QString &kind, const QJsonObject &entry);
bool readsTranscript(const QString &kind);

} // namespace AgentLogText
