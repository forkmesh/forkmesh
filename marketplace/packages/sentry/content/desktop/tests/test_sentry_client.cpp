// The Sentry package's issue model and polling client: what the desktop is
// willing to render from an issue-list response, which differences between
// two reads become log activity, how the lines are badged, and how a refused
// token stops the poll instead of retrying it every minute. Everything the
// client reads is untrusted network input, so the interesting cases are the
// ones that must be bounded or refused.

#include "SentryClient.h"
#include "SentryDesktopExtension.h"

#include <QByteArray>
#include <QApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QWidget>
#include <QUrl>
#include <QUrlQuery>

#include <cstdio>
#include <functional>

using namespace forkmesh::sentry;

namespace {

int failures = 0;

void check(bool condition, const char *message,
           const QString &detail = QString())
{
    if (condition) {
        std::printf("PASS: %s\n", message);
    } else {
        std::printf("FAIL: %s%s%s\n", message, detail.isEmpty() ? "" : " -- ",
                    detail.toUtf8().constData());
        ++failures;
    }
}

bool waitUntil(const std::function<bool()> &predicate, int timeoutMs)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

QJsonObject issueObject(const QString &id, const QString &shortId,
                        const QString &title, const QJsonValue &count,
                        int users, const QString &project,
                        const QString &level = QStringLiteral("error"))
{
    return QJsonObject{
        {QStringLiteral("id"), id},
        {QStringLiteral("shortId"), shortId},
        {QStringLiteral("title"), title},
        {QStringLiteral("culprit"), QStringLiteral("app/world.js in render")},
        {QStringLiteral("level"), level},
        {QStringLiteral("status"), QStringLiteral("unresolved")},
        {QStringLiteral("type"), QStringLiteral("error")},
        {QStringLiteral("count"), count},
        {QStringLiteral("userCount"), users},
        {QStringLiteral("firstSeen"), QStringLiteral("2026-08-20T10:00:00Z")},
        {QStringLiteral("lastSeen"), QStringLiteral("2026-08-21T09:30:00.123Z")},
        {QStringLiteral("isUnhandled"), level == QLatin1String("fatal")},
        {QStringLiteral("permalink"),
         QStringLiteral("https://acme.sentry.io/issues/%1/").arg(id)},
        {QStringLiteral("project"),
         QJsonObject{{QStringLiteral("id"), QStringLiteral("42")},
                     {QStringLiteral("slug"), project},
                     {QStringLiteral("name"), project.toUpper()}}},
        {QStringLiteral("metadata"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("TypeError")},
                     {QStringLiteral("value"), QStringLiteral("boom")}}},
        {QStringLiteral("stats"),
         QJsonObject{{QStringLiteral("24h"),
                      QJsonArray{QJsonArray{1755700000, 2},
                                 QJsonArray{1755703600, 3}}}}},
    };
}

QByteArray body(const QJsonArray &issues)
{
    return QJsonDocument(issues).toJson(QJsonDocument::Compact);
}

Connection connectionFor(const QString &base, const QString &organization,
                         const QString &project = QString())
{
    Connection connection;
    connection.baseUrl = normalizeBaseUrl(base);
    connection.organization = organization;
    connection.project = project;
    connection.statsPeriod = kDefaultStatsPeriod;
    connection.token = QStringLiteral("sntrys_test_token_0123456789");
    return connection;
}

// A loopback stand-in for the Sentry API: answers every request with the
// status and body the test put on it, and remembers what it was asked. It
// keeps each connection open and serves any number of requests on it, so a
// client that re-requests the instant a reply lands never races a closing
// socket.
class FakeSentry
{
    // Declared first so it outlives the server: the server's dying sockets
    // still emit disconnected() into the lambda below during teardown.
    QHash<QTcpSocket *, QByteArray> buffers;

public:
    FakeSentry()
    {
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (server.hasPendingConnections()) {
                QTcpSocket *socket = server.nextPendingConnection();
                ++connections;
                buffers.remove(socket); // a reused address must not inherit bytes
                QObject::connect(socket, &QTcpSocket::disconnected, socket,
                                 [this, socket] {
                                     buffers.remove(socket);
                                     socket->deleteLater();
                                 });
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                                 [this, socket] { serve(socket); });
            }
        });
    }

    bool listen() { return server.listen(QHostAddress::LocalHost, 0); }
    QAbstractSocket::SocketError serverError() const
    {
        return server.serverError();
    }
    QString errorString() const { return server.errorString(); }

    QString baseUrl() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    }

    struct Request {
        QString path;
        QByteArray authorization;
        QByteArray accept;
        int status = 0; // what the stand-in answered
    };

    // The last few exchanges, for a failing check's diagnostics.
    QString recent() const
    {
        QStringList lines;
        for (int i = qMax(0, int(requests.size()) - 4); i < requests.size(); ++i) {
            lines << QStringLiteral("%1 -> %2")
                         .arg(requests.at(i).path.section(QLatin1Char('?'), 0, 0))
                         .arg(requests.at(i).status);
        }
        return QStringLiteral("connections=%1 requests=%2 [%3]")
            .arg(connections)
            .arg(requests.size())
            .arg(lines.join(QStringLiteral(", ")));
    }

    int connections = 0;

    QTcpServer server;
    QList<Request> requests;
    int organizationStatus = 200;
    QByteArray organizationBody =
        "{\"slug\":\"acme\",\"name\":\"Acme Corp\"}";
    int issuesStatus = 200;
    QByteArray issuesBody = "[]";
    QByteArray retryAfter;

private:
    void serve(QTcpSocket *socket)
    {
        QByteArray &buffer = buffers[socket];
        buffer += socket->readAll();
        // Every request is a bodiless GET, so a header block is a whole
        // request; answer each complete one and keep the rest for later.
        while (true) {
            const int end = buffer.indexOf("\r\n\r\n");
            if (end < 0)
                return;
            const QByteArray request = buffer.left(end);
            buffer.remove(0, end + 4);
            const QList<QByteArray> lines = request.split('\n');
            const QList<QByteArray> requestLine =
                lines.value(0).trimmed().split(' ');
            Request seen;
            seen.path = QString::fromUtf8(requestLine.value(1));
            for (const QByteArray &line : lines) {
                const QByteArray trimmed = line.trimmed();
                if (trimmed.toLower().startsWith("authorization:"))
                    seen.authorization = trimmed.mid(14).trimmed();
                else if (trimmed.toLower().startsWith("accept:"))
                    seen.accept = trimmed.mid(7).trimmed();
            }
            int status = 404;
            QByteArray payload = "{\"detail\":\"not found\"}";
            QByteArray extraHeaders;
            if (seen.path.contains(QLatin1String("/issues/"))) {
                status = issuesStatus;
                payload = issuesBody;
                if (!retryAfter.isEmpty())
                    extraHeaders += "Retry-After: " + retryAfter + "\r\n";
            } else if (seen.path.contains(QLatin1String("/organizations/"))) {
                status = organizationStatus;
                payload = organizationBody;
            }
            seen.status = status;
            requests.append(seen);
            socket->write("HTTP/1.1 " + QByteArray::number(status) +
                          " Status\r\nContent-Type: application/json\r\n"
                          "Content-Length: " + QByteArray::number(payload.size()) +
                          "\r\n" + extraHeaders + "Connection: keep-alive\r\n\r\n" +
                          payload);
            socket->flush();
        }
    }
};

void testNormalization()
{
    check(normalizeAuthToken(QStringLiteral("  Bearer sntrys_abc.DEF-123=  ")) ==
              QStringLiteral("sntrys_abc.DEF-123="),
          "an API key loses its whitespace and Bearer prefix");
    check(normalizeAuthToken(QStringLiteral("has space 0123456789")).isEmpty() &&
              normalizeAuthToken(QStringLiteral("short")).isEmpty() &&
              normalizeAuthToken(QString()).isEmpty(),
          "an API key with whitespace, a short key and an empty key are refused");
    check(normalizeSlug(QStringLiteral("  Acme-Corp ")) ==
              QStringLiteral("acme-corp") &&
              normalizeSlug(QStringLiteral("acme corp")).isEmpty() &&
              normalizeSlug(QStringLiteral("-acme")).isEmpty() &&
              normalizeSlug(QStringLiteral("a")) == QStringLiteral("a"),
          "slugs are lower-cased and anything Sentry would not accept is refused");
    check(normalizeBaseUrl(QString()) == QUrl(QStringLiteral("https://sentry.io")) &&
              normalizeBaseUrl(QStringLiteral("sentry.example.com")) ==
                  QUrl(QStringLiteral("https://sentry.example.com")) &&
              normalizeBaseUrl(QStringLiteral("https://Sentry.io/api/0/")) ==
                  QUrl(QStringLiteral("https://sentry.io")) &&
              normalizeBaseUrl(QStringLiteral("https://sentry.example.com/prefix/")) ==
                  QUrl(QStringLiteral("https://sentry.example.com/prefix")),
          "the base URL defaults to sentry.io and drops a trailing /api/0");
    check(normalizeBaseUrl(QStringLiteral("http://sentry.io")).isEmpty() &&
              normalizeBaseUrl(QStringLiteral("ftp://sentry.io")).isEmpty() &&
              normalizeBaseUrl(QStringLiteral("https://user:pw@sentry.io")).isEmpty() &&
              normalizeBaseUrl(QStringLiteral("https://sentry.io/?x=1")).isEmpty() &&
              !normalizeBaseUrl(QStringLiteral("http://127.0.0.1:8080")).isEmpty(),
          "plain http is refused except to this machine, as are credentials "
          "and queries in the host");
    check(normalizeStatsPeriod(QStringLiteral("14D")) == QStringLiteral("14d") &&
              normalizeStatsPeriod(QStringLiteral("90d")) == QStringLiteral("24h") &&
              statsPeriodLabel(QStringLiteral("14d")) == QStringLiteral("last 14 days"),
          "the stats period is one of the two Sentry accepts for issue lists");
}

void testEndpoints()
{
    const Connection organization =
        connectionFor(QStringLiteral("https://sentry.io"), QStringLiteral("acme"));
    const QUrl issues = issuesUrlFor(organization);
    const QUrlQuery query(issues);
    check(issues.scheme() == QLatin1String("https") &&
              issues.host() == QLatin1String("sentry.io") &&
              issues.path() == QLatin1String("/api/0/organizations/acme/issues/") &&
              query.queryItemValue(QStringLiteral("query")) ==
                  QStringLiteral("is:unresolved") &&
              query.queryItemValue(QStringLiteral("statsPeriod")) ==
                  QStringLiteral("24h") &&
              query.queryItemValue(QStringLiteral("limit")) ==
                  QString::number(kMaxIssues) &&
              query.queryItemValue(QStringLiteral("sort")) == QStringLiteral("freq"),
          "an organization read asks for unresolved issues, most frequent first");
    const Connection project = connectionFor(
        QStringLiteral("https://sentry.example.com/prefix"),
        QStringLiteral("acme"), QStringLiteral("web"));
    check(issuesUrlFor(project).path() ==
                  QLatin1String("/prefix/api/0/projects/acme/web/issues/") &&
              organizationUrlFor(project).path() ==
                  QLatin1String("/prefix/api/0/organizations/acme/") &&
              browserUrlFor(project).path() ==
                  QLatin1String("/prefix/organizations/acme/issues/") &&
              QUrlQuery(browserUrlFor(project))
                      .queryItemValue(QStringLiteral("query"),
                                      QUrl::FullyDecoded) ==
                  QStringLiteral("is:unresolved project:web") &&
              authTokenPageFor(project).path() ==
                  QLatin1String("/prefix/settings/account/api/auth-tokens/"),
          "a project slug narrows the read and a self-hosted prefix is kept");
    check(issuesUrlFor(Connection()).isEmpty() && !Connection().isValid() &&
              organization.isValid(),
          "a connection without an organization or token is not readable");
}

void testParsing()
{
    QJsonArray entries;
    entries.append(issueObject(QStringLiteral("101"), QStringLiteral("ACME-7"),
                               QStringLiteral("TypeError: Cannot read x"),
                               QStringLiteral("12"), 3, QStringLiteral("web")));
    entries.append(issueObject(QStringLiteral("102"), QStringLiteral("ACME-9"),
                               QStringLiteral("Failed to fetch"), 40, 7,
                               QStringLiteral("api"), QStringLiteral("fatal")));
    QJsonObject sparse{{QStringLiteral("id"), 103},
                       {QStringLiteral("count"), QStringLiteral("-5")},
                       {QStringLiteral("permalink"),
                        QStringLiteral("javascript:alert(1)")},
                       {QStringLiteral("metadata"),
                        QJsonObject{{QStringLiteral("type"),
                                     QStringLiteral("ValueError")},
                                    {QStringLiteral("value"),
                                     QStringLiteral("bad \n input\t here")}}}};
    entries.append(sparse);
    entries.append(QJsonObject{{QStringLiteral("title"), QStringLiteral("no id")}});
    entries.append(QStringLiteral("not an object"));

    QList<Issue> issues;
    QString error;
    check(parseIssues(body(entries), &issues, &error) && error.isEmpty() &&
              issues.size() == 3,
          "a list of issues parses, skipping entries without an id");
    check(issues.size() == 3 && issues.at(0).shortId == QStringLiteral("ACME-9") &&
              issues.at(0).count == 40 && issues.at(0).userCount == 7 &&
              issues.at(0).level == QStringLiteral("fatal") &&
              issues.at(0).unhandled && issues.at(1).shortId == QStringLiteral("ACME-7") &&
              issues.at(1).count == 12 && issues.at(2).id == QStringLiteral("103"),
          "issues are ordered most events first and counts read as strings or numbers");
    check(issues.size() == 3 && issues.at(1).recentKnown &&
              issues.at(1).recentCount == 5 &&
              issues.at(1).projectSlug == QStringLiteral("web") &&
              issues.at(1).projectName == QStringLiteral("WEB") &&
              issues.at(1).permalink ==
                  QUrl(QStringLiteral("https://acme.sentry.io/issues/101/")) &&
              issues.at(1).culprit == QStringLiteral("app/world.js in render") &&
              issues.at(1).firstSeenMs > 0 && issues.at(1).lastSeenMs > issues.at(1).firstSeenMs,
          "the stats series, project, permalink and timestamps survive parsing");
    check(issues.size() == 3 && issues.at(2).count == 0 &&
              issues.at(2).permalink.isEmpty() &&
              issues.at(2).title == QStringLiteral("ValueError: bad input here") &&
              !issues.at(2).recentKnown,
          "a sparse issue gets a bounded title from its metadata, no negative "
          "count and no javascript link");

    QList<Issue> none;
    check(!parseIssues("{\"detail\":\"Invalid token\"}", &none, &error) &&
              error == QStringLiteral("Invalid token") && none.isEmpty(),
          "a refusal body is reported with Sentry's own detail");
    check(!parseIssues("not json", &none, &error) && !error.isEmpty(),
          "a non-JSON body is refused");
    check(parseIssues("[]", &none, &error) && none.isEmpty() && error.isEmpty(),
          "an empty list is a successful read of nothing");

    QJsonArray many;
    for (int i = 0; i < kMaxIssues + 20; ++i) {
        many.append(issueObject(QString::number(1000 + i),
                                QStringLiteral("ACME-%1").arg(i),
                                QStringLiteral("issue %1").arg(i), i, 1,
                                QStringLiteral("web")));
    }
    QList<Issue> bounded;
    check(parseIssues(body(many), &bounded, &error) &&
              bounded.size() == kMaxIssues &&
              bounded.first().count == kMaxIssues + 19,
          "a longer list is bounded to the most frequent issues");

    QString longTitle;
    for (int i = 0; i < 400; ++i)
        longTitle += QLatin1Char('x');
    QJsonArray longEntries{issueObject(QStringLiteral("1"), QStringLiteral("A-1"),
                                       longTitle, 1, 1, QStringLiteral("web"))};
    QList<Issue> longIssues;
    check(parseIssues(body(longEntries), &longIssues, &error) &&
              longIssues.size() == 1 &&
              longIssues.first().title.size() == kMaxTitleChars &&
              longIssues.first().title.endsWith(QStringLiteral("...")),
          "a runaway title is cut to the bounded length");

    check(parseOrganizationName("{\"slug\":\"acme\",\"name\":\"Acme Corp\"}") ==
                  QStringLiteral("Acme Corp") &&
              parseOrganizationName("[]").isEmpty(),
          "the organization name is read from its own endpoint");
}

void testAggregationAndDiff()
{
    QJsonArray entries;
    entries.append(issueObject(QStringLiteral("101"), QStringLiteral("ACME-7"),
                               QStringLiteral("TypeError: Cannot read x"),
                               QStringLiteral("12"), 3, QStringLiteral("web")));
    entries.append(issueObject(QStringLiteral("102"), QStringLiteral("ACME-9"),
                               QStringLiteral("Failed to fetch"), 40, 7,
                               QStringLiteral("api"), QStringLiteral("fatal")));
    entries.append(issueObject(QStringLiteral("104"), QStringLiteral("ACME-11"),
                               QStringLiteral("Slow query"), 8, 2,
                               QStringLiteral("api"), QStringLiteral("warning")));
    QList<Issue> issues;
    parseIssues(body(entries), &issues);
    const Aggregate total = aggregate(issues);
    check(total.issueCount == 3 && total.eventCount == 60 &&
              total.userCount == 12 && total.unhandledCount == 1 &&
              total.issuesByProject.value(QStringLiteral("api")) == 2 &&
              total.eventsByProject.value(QStringLiteral("api")) == 48 &&
              total.eventsByProject.value(QStringLiteral("web")) == 12 &&
              total.issuesByLevel.value(QStringLiteral("fatal")) == 1 &&
              total.eventsByLevel.value(QStringLiteral("warning")) == 8 &&
              total.newestLastSeenMs == issues.first().lastSeenMs,
          "the aggregate totals issues, events and users by project and level");

    const QString summary = summaryLine(QStringLiteral("Acme Corp (acme)"), total,
                                        QStringLiteral("24h"));
    check(summary.startsWith(kLogPrefix) &&
              summary.contains(QStringLiteral("3 unresolved issues")) &&
              summary.contains(QStringLiteral("60 events in the last 24 hours")) &&
              summary.contains(QStringLiteral("12 users affected")),
          "the summary line opens with the Sentry prefix and states the totals");

    // The next read: ACME-7 grew, ACME-9 is unchanged, ACME-11 was resolved
    // and a brand-new ACME-12 appeared.
    QJsonArray next;
    next.append(issueObject(QStringLiteral("101"), QStringLiteral("ACME-7"),
                            QStringLiteral("TypeError: Cannot read x"),
                            QStringLiteral("17"), 4, QStringLiteral("web")));
    next.append(issueObject(QStringLiteral("102"), QStringLiteral("ACME-9"),
                            QStringLiteral("Failed to fetch"), 40, 7,
                            QStringLiteral("api"), QStringLiteral("fatal")));
    next.append(issueObject(QStringLiteral("105"), QStringLiteral("ACME-12"),
                            QStringLiteral("Error: socket hang up"), 3, 2,
                            QStringLiteral("api")));
    QList<Issue> current;
    parseIssues(body(next), &current);
    const QList<Change> changes = diffIssues(issues, current);
    check(changes.size() == 2 && changes.at(0).kind == Change::MoreEvents &&
              changes.at(0).issue.shortId == QStringLiteral("ACME-7") &&
              changes.at(0).previousCount == 12 &&
              changes.at(1).kind == Change::NewIssue &&
              changes.at(1).issue.shortId == QStringLiteral("ACME-12"),
          "a diff reports grown and new issues, not unchanged or resolved ones");
    check(changes.size() == 2 &&
              changeLine(changes.at(0)) ==
                  QString::fromUtf8("Sentry: ACME-7 +5 events (17 events) \xC2\xB7 "
                                    "TypeError: Cannot read x \xC2\xB7 web") &&
              changeLine(changes.at(1)) ==
                  QString::fromUtf8("Sentry: new issue ACME-12 \xC2\xB7 Error: "
                                    "socket hang up \xC2\xB7 3 events \xC2\xB7 "
                                    "2 users \xC2\xB7 api"),
          "change lines name the issue, the growth and the project");
    check(diffIssues(QList<Issue>(), current).size() == 3 &&
              diffIssues(current, current).isEmpty(),
          "every issue is new against nothing and nothing changes against itself");

    const QUrl base(QStringLiteral("https://sentry.io"));
    const QString refused = failureLine(base, 401, "{\"detail\":\"Invalid token\"}",
                                        QString());
    check(refused.startsWith(kLogPrefix) &&
              refused.contains(QStringLiteral("sentry.io (401 Unauthorized): "
                                              "Invalid token")) &&
              refused.contains(QStringLiteral("check the API key")),
          "a refused token is explained with Sentry's detail and the fix");
    check(failureLine(base, 0, {}, QStringLiteral("Host not found"))
                  .endsWith(QStringLiteral("(Host not found)")) &&
              failureLine(base, 429, {}, QString())
                  .contains(QStringLiteral("rate limit")) &&
              failureLine(base, 404, {}, QString())
                  .contains(QStringLiteral("organization and project slugs")),
          "transport errors, rate limits and a wrong slug each get their own line");
    check(relativeTimeText(0, 1000) == QStringLiteral("never") &&
              relativeTimeText(1000, 3000) == QStringLiteral("just now") &&
              relativeTimeText(1000, 60000) == QStringLiteral("59 s ago") &&
              relativeTimeText(1000, 61000) == QStringLiteral("1 min ago") &&
              relativeTimeText(1000, 3'601'000) == QStringLiteral("1 h ago") &&
              levelLabel(QStringLiteral("fatal")) == QStringLiteral("Fatal") &&
              levelLabel(QString()) == QStringLiteral("Unknown"),
          "ages and levels read the way the tab shows them");
}

void testClient()
{
    if (qEnvironmentVariableIntValue("FORKMESH_SKIP_LOOPBACK_TESTS") != 0) {
        std::printf("SKIP: loopback Sentry client checks disabled by "
                    "FORKMESH_SKIP_LOOPBACK_TESTS\n");
        return;
    }
    FakeSentry sentry;
    if (!sentry.listen()) {
        // Some seccomp profiles make Qt surface a refused bind as
        // UnknownSocketError rather than SocketAccessError. The parser,
        // aggregation and extension contract remain fully covered without a
        // socket; skip only this transport exercise when no loopback listener
        // can be created at all.
        std::printf("SKIP: loopback Sentry stand-in is unavailable in this "
                    "environment (%s)\n",
                    qPrintable(sentry.errorString()));
        return;
    }
    check(true, "the loopback Sentry stand-in listens");
    QJsonArray first;
    first.append(issueObject(QStringLiteral("101"), QStringLiteral("ACME-7"),
                             QStringLiteral("TypeError: Cannot read x"),
                             QStringLiteral("12"), 3, QStringLiteral("web")));
    first.append(issueObject(QStringLiteral("102"), QStringLiteral("ACME-9"),
                             QStringLiteral("Failed to fetch"), 40, 7,
                             QStringLiteral("api"), QStringLiteral("fatal")));
    sentry.issuesBody = body(first);

    QNetworkAccessManager network;
    SentryClient client(&network);
    QStringList activity;
    QStringList statuses;
    int connectedSignals = 0;
    int issuesSignals = 0;
    QObject::connect(&client, &SentryClient::activity, &client,
                     [&](const QString &text) { activity.append(text); });
    QObject::connect(&client, &SentryClient::statusChanged, &client,
                     [&](const QString &status, bool connected, bool) {
                         statuses.append(status);
                         if (connected)
                             ++connectedSignals;
                     });
    QObject::connect(&client, &SentryClient::issuesChanged, &client,
                     [&] { ++issuesSignals; });

    client.start();
    check(!client.isActive() && activity.isEmpty() &&
              client.status() == QStringLiteral("Not connected"),
          "an unconfigured client does nothing when started");

    client.setConnection(connectionFor(sentry.baseUrl(), QStringLiteral("acme")));
    client.start();
    check(waitUntil([&] { return client.isConnected(); }, 5000),
          "the client connects through the organization and issue reads");
    check(sentry.requests.size() == 2 &&
              sentry.requests.at(0).path == QStringLiteral("/api/0/organizations/acme/") &&
              sentry.requests.at(1).path.startsWith(
                  QStringLiteral("/api/0/organizations/acme/issues/?")) &&
              sentry.requests.at(0).authorization ==
                  QByteArray("Bearer sntrys_test_token_0123456789") &&
              sentry.requests.at(1).authorization ==
                  QByteArray("Bearer sntrys_test_token_0123456789") &&
              sentry.requests.at(1).accept == QByteArray("application/json"),
          "both reads carry the bearer token and ask for JSON");
    check(client.issues().size() == 2 && client.aggregate().eventCount == 52 &&
              client.organizationName() == QStringLiteral("Acme Corp") &&
              client.organizationLabel() == QStringLiteral("Acme Corp (acme)") &&
              client.hasSynced() && !client.hasError() && connectedSignals >= 1 &&
              issuesSignals >= 1,
          "the first read fills the issue list and names the organization");
    check(activity.size() == 2 &&
              activity.at(0) == QStringLiteral("Sentry: connecting to 127.0.0.1 "
                                               "for organization acme") &&
              activity.at(1) ==
                  QString::fromUtf8("Sentry: Acme Corp (acme) \xC2\xB7 2 unresolved "
                                    "issues \xC2\xB7 52 events in the last 24 hours "
                                    "\xC2\xB7 10 users affected"),
          "the first read logs one summary, not a line per already-open issue",
          activity.join(QStringLiteral(" | ")));
    check(!client.isReading() && client.nextPollInMs() > 0 &&
              client.nextPollInMs() <= kPollIntervalMs,
          "the next read is scheduled a minute out",
          QStringLiteral("next=%1 reading=%2 %3")
              .arg(client.nextPollInMs())
              .arg(client.isReading())
              .arg(sentry.recent()));

    // The next read: ACME-7 grew and ACME-12 appeared.
    QJsonArray second;
    second.append(issueObject(QStringLiteral("101"), QStringLiteral("ACME-7"),
                              QStringLiteral("TypeError: Cannot read x"),
                              QStringLiteral("17"), 4, QStringLiteral("web")));
    second.append(issueObject(QStringLiteral("102"), QStringLiteral("ACME-9"),
                              QStringLiteral("Failed to fetch"), 40, 7,
                              QStringLiteral("api"), QStringLiteral("fatal")));
    second.append(issueObject(QStringLiteral("105"), QStringLiteral("ACME-12"),
                              QStringLiteral("Error: socket hang up"), 3, 2,
                              QStringLiteral("api")));
    sentry.issuesBody = body(second);
    activity.clear();
    client.refresh();
    check(waitUntil([&] { return activity.size() >= 2 && !client.isReading(); },
                    5000) &&
              activity.size() == 2 &&
              activity.at(0).startsWith(QStringLiteral("Sentry: ACME-7 +5 events")) &&
              activity.at(1).startsWith(QStringLiteral("Sentry: new issue ACME-12")) &&
              client.issues().size() == 3 && sentry.requests.size() == 3,
          "a refresh reads the issues again and logs only what changed",
          activity.join(QStringLiteral(" | ")) + QStringLiteral(" ") +
              sentry.recent());

    // A refused token: one line, no retry every minute, until refresh().
    sentry.issuesStatus = 401;
    sentry.issuesBody = "{\"detail\":\"Invalid token\"}";
    activity.clear();
    client.refresh();
    check(waitUntil([&] { return client.hasError() && !client.isReading(); },
                    5000) &&
              activity.size() == 1 &&
              activity.at(0).contains(QStringLiteral("(401 Unauthorized): Invalid token")) &&
              !client.isConnected() && client.nextPollInMs() < 0 &&
              client.issues().size() == 3,
          "a refused token is logged once, stops the poll and keeps the last list",
          QStringLiteral("%1 | next=%2").arg(activity.join(QStringLiteral(" | ")))
              .arg(client.nextPollInMs()));
    const int requestsBeforeRetry = sentry.requests.size();
    client.refresh();
    check(waitUntil([&] {
                        return sentry.requests.size() >= requestsBeforeRetry + 2 &&
                               !client.isReading();
                    },
                    5000) &&
              activity.size() == 1 && client.hasError(),
          "retrying a still-refused token goes back through the organization "
          "read and does not log the same failure again");

    // The token fixed: the next good read logs a fresh summary.
    sentry.issuesStatus = 200;
    sentry.issuesBody = body(second);
    activity.clear();
    client.refresh();
    check(waitUntil([&] { return client.isConnected() && !client.isReading(); },
                    5000) &&
              activity.size() == 1 &&
              activity.at(0).startsWith(QStringLiteral("Sentry: Acme Corp (acme)")) &&
              !client.hasError(),
          "recovering from a failure logs the standing summary again",
          activity.join(QStringLiteral(" | ")) + QStringLiteral(" ") +
              sentry.recent());

    // A rate limit honours Retry-After within bounds.
    sentry.issuesStatus = 429;
    sentry.issuesBody = "{\"detail\":\"slow down\"}";
    sentry.retryAfter = "120";
    activity.clear();
    client.refresh();
    check(waitUntil([&] { return client.hasError() && !client.isReading(); },
                    5000) &&
              activity.size() == 1 &&
              activity.at(0).contains(QStringLiteral("rate limit")) &&
              client.nextPollInMs() > kPollIntervalMs &&
              client.nextPollInMs() <= 120'000,
          "a rate limit schedules the next read after Retry-After",
          QStringLiteral("%1 | next=%2 | reading=%3 | status=%4 | %5")
              .arg(activity.join(QStringLiteral(" | ")))
              .arg(client.nextPollInMs())
              .arg(client.isReading())
              .arg(client.status())
              .arg(sentry.recent()));
    sentry.retryAfter.clear();

    // Stopping ignores a reply still in flight and drops the schedule.
    sentry.issuesStatus = 200;
    sentry.issuesBody = body(first);
    activity.clear();
    client.refresh();
    client.stop();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    waitUntil([] { return false; }, 300);
    check(!client.isActive() && !client.isConnected() && activity.isEmpty() &&
              client.nextPollInMs() < 0 && client.issues().size() == 3,
          "stop() drops the schedule and a reply that arrives afterwards");

    // Disconnecting forgets everything but keeps the client usable.
    client.setConnection(Connection());
    check(!client.isConfigured() && client.issues().isEmpty() &&
              !client.hasSynced() && client.status() == QStringLiteral("Not connected"),
          "clearing the connection forgets the snapshot and the organization");
}

void testDesktopExtensionContract()
{
    QSettings settings;
    settings.remove(kSettingsPrefix);
    settings.sync();

    std::unique_ptr<forkmesh::market::DesktopPackageExtension> extension(
        forkmesh::market::createSentryDesktopExtension());
    const auto categories = extension->logCategories();
    check(extension->packageId() == kPackageId &&
              extension->logPageTitle() == QStringLiteral("Sentry") &&
              categories.size() == 1 && categories.first().badge == kLogBadge &&
              categories.first().icon.startsWith(
                  QStringLiteral(":/forkmesh-market/packages/sentry/")),
          "the package owns its factory metadata, activity lane, and icon");

    QNetworkAccessManager network;
    QStringList activity;
    forkmesh::market::DesktopPackageContext context;
    context.network = &network;
    context.logActivity = [&activity](const QString &line, const QString &badge) {
        activity << badge + QLatin1Char(':') + line;
    };
    context.darkTheme = [] { return false; };
    context.icon = [](const QString &, int) { return QIcon(); };
    extension->activate(context);
    QWidget parent;
    QWidget *page = extension->createLogPage(&parent);
    check(page && page->findChild<QWidget *>(QStringLiteral("packageLogView")) &&
              page->findChild<QWidget *>(QStringLiteral("packageLogStatus")),
          "the package creates its aggregated Log page through the host contract");
    extension->deactivate();
    check(!activity.isEmpty() && activity.last().startsWith(kLogBadge),
          "package activity is emitted with the package-declared badge");
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
    QCoreApplication::setOrganizationName(QStringLiteral("ForkMeshTests"));
    QCoreApplication::setApplicationName(QStringLiteral("SentryPackage"));
    QApplication app(argc, argv);
    testNormalization();
    testEndpoints();
    testParsing();
    testAggregationAndDiff();
    testClient();
    testDesktopExtensionContract();
    std::printf("%s\n", failures == 0 ? "ALL SENTRY TESTS PASSED"
                                      : "SENTRY TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
