#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>

namespace forkmesh::projectinvite {

struct Invitation {
    QString projectName;
    QString inviter;
    QString instanceUrl;
};

QString instanceOrigin(const QString &serverUrl);
QString instanceHost(const QString &serverUrl);
QString invitationLink(const Invitation &invitation);
QString directMessageText(const Invitation &invitation);
QString emailSubject(const Invitation &invitation);
QString emailBody(const Invitation &invitation);
QStringList emailRecipients(const QString &input, QString *invalid = nullptr);
QUrl mailtoUrl(const QStringList &recipients, const Invitation &invitation);

} // namespace forkmesh::projectinvite
