#include "ProjectInvite.h"

#include <QRegularExpression>

namespace forkmesh::projectinvite {

namespace {

QString projectLabel(const Invitation &invitation)
{
    const QString name = invitation.projectName.trimmed();
    return name.isEmpty() ? QStringLiteral("my project")
                          : QStringLiteral("“%1”").arg(name);
}

QString hostOrFallback(const Invitation &invitation)
{
    const QString host = instanceHost(invitation.instanceUrl);
    return host.isEmpty() ? QStringLiteral("ForkMesh") : host;
}

QByteArray mailtoField(const QString &value)
{
    QString text = value;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
    return QUrl::toPercentEncoding(text);
}

} // namespace

QString instanceOrigin(const QString &serverUrl)
{
    QString raw = serverUrl.trimmed();
    if (raw.isEmpty())
        return {};
    if (!raw.contains(QStringLiteral("://")))
        raw.prepend(QStringLiteral("https://"));
    QUrl url(raw, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty())
        return {};
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("wss") || scheme == QLatin1String("https"))
        url.setScheme(QStringLiteral("https"));
    else if (scheme == QLatin1String("ws") || scheme == QLatin1String("http"))
        url.setScheme(QStringLiteral("http"));
    else
        return {};
    url.setUserInfo(QString());
    url.setPath(QString());
    url.setQuery(QString());
    url.setFragment(QString());
    return url.toString(QUrl::FullyEncoded);
}

QString instanceHost(const QString &serverUrl)
{
    const QString origin = instanceOrigin(serverUrl);
    if (origin.isEmpty())
        return {};
    const QUrl url(origin);
    return url.port() > 0 ? QStringLiteral("%1:%2").arg(url.host()).arg(url.port())
                          : url.host();
}

QString invitationLink(const Invitation &invitation)
{
    const QString origin = instanceOrigin(invitation.instanceUrl);
    if (origin.isEmpty())
        return {};
    const QString inviter = invitation.inviter.trimmed();
    if (inviter.isEmpty())
        return origin + QStringLiteral("/signup");
    return origin + QStringLiteral("/r/") +
           QString::fromLatin1(QUrl::toPercentEncoding(inviter));
}

QString directMessageText(const Invitation &invitation)
{
    const QString host = hostOrFallback(invitation);
    return QStringLiteral(
               "I’d like you to join %1 on %2. To open it in ForkMesh, choose "
               "Add project… in the project menu at the top left and connect "
               "the new project to %2.")
        .arg(projectLabel(invitation), host);
}

QString emailSubject(const Invitation &invitation)
{
    const QString inviter = invitation.inviter.trimmed();
    if (inviter.isEmpty())
        return QStringLiteral("You’re invited to %1 on ForkMesh")
            .arg(projectLabel(invitation));
    return QStringLiteral("%1 invited you to %2 on ForkMesh")
        .arg(inviter, projectLabel(invitation));
}

QString emailBody(const Invitation &invitation)
{
    const QString inviter = invitation.inviter.trimmed();
    const QString host = hostOrFallback(invitation);
    const QString link = invitationLink(invitation);
    QString body = QStringLiteral("Hi,\n\n");
    body += QStringLiteral("I’d like you to join %1 on ForkMesh. It lives on the "
                           "instance %2.\n\n")
                .arg(projectLabel(invitation), host);
    if (!link.isEmpty())
        body += QStringLiteral("Accept the invitation and create your account "
                               "here:\n%1\n\n")
                    .arg(link);
    body += QStringLiteral("Then, in the ForkMesh app, choose Add project… in the "
                           "project menu at the top left and connect it to %1. "
                           "Reply with your account name and I’ll add you.\n")
                .arg(host);
    if (!inviter.isEmpty())
        body += QStringLiteral("\n%1\n").arg(inviter);
    return body;
}

QStringList emailRecipients(const QString &input, QString *invalid)
{
    static const QRegularExpression separators(QStringLiteral("[,;\\s]+"));
    static const QRegularExpression address(
        QStringLiteral("^[^@\\s<>\"]+@[^@\\s<>\"]+\\.[^@\\s<>\".]+$"));
    QStringList recipients;
    for (const QString &part : input.split(separators, Qt::SkipEmptyParts)) {
        if (!address.match(part).hasMatch()) {
            if (invalid)
                *invalid = part;
            return {};
        }
        if (!recipients.contains(part, Qt::CaseInsensitive))
            recipients.append(part);
    }
    if (invalid)
        invalid->clear();
    return recipients;
}

QUrl mailtoUrl(const QStringList &recipients, const Invitation &invitation)
{
    QStringList encoded;
    for (const QString &recipient : recipients)
        encoded.append(QString::fromLatin1(
            QUrl::toPercentEncoding(recipient.trimmed(), "@+")));
    const QByteArray raw = "mailto:" + encoded.join(QLatin1Char(',')).toLatin1() +
                           "?subject=" + mailtoField(emailSubject(invitation)) +
                           "&body=" + mailtoField(emailBody(invitation));
    return QUrl::fromEncoded(raw, QUrl::StrictMode);
}

} // namespace forkmesh::projectinvite
