#include "../src/ProjectInvite.h"

#include <QCoreApplication>
#include <QUrlQuery>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        qInfo("PASS: %s", message);
    else {
        qCritical("FAIL: %s", message);
        ++failures;
    }
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    using namespace forkmesh::projectinvite;

    check(instanceOrigin(QStringLiteral("wss://forkmesh.com/room/mainnode")) ==
              QStringLiteral("https://forkmesh.com"),
          "a wss relay address becomes its https origin");
    check(instanceOrigin(QStringLiteral("ws://127.0.0.1:8787/room/x")) ==
              QStringLiteral("http://127.0.0.1:8787"),
          "a local ws relay keeps http and its port");
    check(instanceOrigin(QStringLiteral("relay.example.com")) ==
              QStringLiteral("https://relay.example.com"),
          "a bare host is treated as https");
    check(instanceOrigin(QStringLiteral("ftp://relay.example.com")).isEmpty(),
          "an unknown scheme has no origin");
    check(instanceOrigin(QString()).isEmpty(), "no instance has no origin");
    check(instanceHost(QStringLiteral("ws://127.0.0.1:8787/room/x")) ==
              QStringLiteral("127.0.0.1:8787"),
          "the host keeps a non-default port");

    Invitation invitation;
    invitation.projectName = QStringLiteral("Garden & Co");
    invitation.inviter = QStringLiteral("alice");
    invitation.instanceUrl = QStringLiteral("wss://forkmesh.com/room/mainnode");

    check(invitationLink(invitation) == QStringLiteral("https://forkmesh.com/r/alice"),
          "the invitation link is the inviter's referral link on the instance");
    Invitation anonymous = invitation;
    anonymous.inviter.clear();
    check(invitationLink(anonymous) == QStringLiteral("https://forkmesh.com/signup"),
          "without an account the link is the instance's sign-up page");
    Invitation offline = invitation;
    offline.instanceUrl.clear();
    check(invitationLink(offline).isEmpty(), "a project without an instance has no link");
    check(emailBody(offline).contains(QStringLiteral("ForkMesh")) &&
              !emailBody(offline).contains(QStringLiteral("Accept the invitation")),
          "an email without a link leaves out the accept line");

    const QString dm = directMessageText(invitation);
    check(dm.contains(QStringLiteral("“Garden & Co”")) &&
              dm.contains(QStringLiteral("forkmesh.com")),
          "the direct message names the project and its instance");

    check(emailSubject(invitation) ==
              QStringLiteral("alice invited you to “Garden & Co” on ForkMesh"),
          "the email subject names the inviter and project");
    check(emailBody(invitation).contains(QStringLiteral("https://forkmesh.com/r/alice")),
          "the email body carries the invitation link");

    QString invalid;
    const QStringList recipients = emailRecipients(
        QStringLiteral(" bob@example.com, carol@example.org;bob@example.com "), &invalid);
    check(recipients == QStringList({QStringLiteral("bob@example.com"),
                                     QStringLiteral("carol@example.org")}) &&
              invalid.isEmpty(),
          "recipients split on commas, semicolons and spaces without repeats");
    check(emailRecipients(QStringLiteral("bob@example.com, nope"), &invalid).isEmpty() &&
              invalid == QStringLiteral("nope"),
          "a malformed address is reported");
    check(emailRecipients(QString(), &invalid).isEmpty() && invalid.isEmpty(),
          "an empty address list is allowed");

    const QUrl mailto = mailtoUrl({QStringLiteral("bob+x@example.com")}, invitation);
    check(mailto.isValid() && mailto.scheme() == QStringLiteral("mailto"),
          "the mailto link is valid");
    check(mailto.path() == QStringLiteral("bob+x@example.com"),
          "the recipient keeps its plus sign");
    const QUrlQuery query(mailto);
    check(query.queryItemValue(QStringLiteral("subject"), QUrl::FullyDecoded) ==
              emailSubject(invitation),
          "the subject survives encoding, ampersand included");
    const QString body = query.queryItemValue(QStringLiteral("body"), QUrl::FullyDecoded);
    check(body.contains(QStringLiteral("\r\n")) &&
              QString(body).remove(QLatin1Char('\r')) == emailBody(invitation),
          "the body survives encoding with CRLF line breaks");
    check(!QString::fromLatin1(mailto.toEncoded()).contains(QLatin1Char(' ')),
          "the encoded link has no raw spaces");
    check(mailtoUrl({}, invitation).toString().startsWith(QStringLiteral("mailto:?subject=")),
          "a link without recipients lets the mail app ask for one");

    return failures == 0 ? 0 : 1;
}
