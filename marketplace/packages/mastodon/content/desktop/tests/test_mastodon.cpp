#include "MastodonClient.h"
#include "MastodonCore.h"
#include "MastodonDialogs.h"
#include "MastodonFeature.h"
#include "MastodonWidgets.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTimeZone>
#include <QTimer>
#include <QUrlQuery>
#include <QVariantMap>

#include <cstdio>
#include <functional>
#include <memory>

using namespace forkmesh::mastodon;

namespace {

int failures = 0;

void check(bool condition, const char *message, const QString &detail = QString())
{
    if (condition) {
        std::printf("PASS: %s\n", message);
    } else {
        std::printf("FAIL: %s%s%s\n", message, detail.isEmpty() ? "" : " -- ", detail.toUtf8().constData());
        ++failures;
    }
}

bool waitFor(const std::function<bool()> &condition, int timeoutMs = 4000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return condition();
}

struct Recorded {
    QByteArray method;
    QUrl url;
    QByteArray body;
    QByteArray authorization;
    QByteArray contentType;
};

struct Canned {
    int status = 200;
    QByteArray body;
    QByteArray link;
};

class FakeReply : public QNetworkReply
{
public:
    FakeReply(const QNetworkRequest &request, QNetworkAccessManager::Operation operation, const Canned &canned,
              QObject *parent)
        : QNetworkReply(parent), m_data(canned.body)
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(operation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, canned.status);
        setRawHeader("Content-Type", "application/json");
        if (!canned.link.isEmpty())
            setRawHeader("Link", canned.link);
        if (canned.status >= 400)
            setError(canned.status == 404 ? QNetworkReply::ContentNotFoundError
                     : canned.status == 401 ? QNetworkReply::AuthenticationRequiredError
                                            : QNetworkReply::UnknownContentError,
                     QStringLiteral("HTTP %1").arg(canned.status));
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this] {
            setFinished(true);
            emit readyRead();
            emit finished();
        });
    }

    void abort() override {}
    qint64 bytesAvailable() const override { return m_data.size() - m_offset + QIODevice::bytesAvailable(); }
    bool isSequential() const override { return true; }

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        const qint64 count = qMin<qint64>(maxSize, m_data.size() - m_offset);
        if (count <= 0)
            return m_offset >= m_data.size() ? -1 : 0;
        memcpy(data, m_data.constData() + m_offset, static_cast<size_t>(count));
        m_offset += count;
        return count;
    }

private:
    QByteArray m_data;
    qint64 m_offset = 0;
};

class FakeServer : public QNetworkAccessManager
{
public:
    using Handler = std::function<Canned(const Recorded &)>;

    void route(const QByteArray &method, const QString &path, Handler handler)
    {
        m_routes.insert(method + ' ' + path.toUtf8(), std::move(handler));
    }

    void json(const QByteArray &method, const QString &path, const QJsonValue &value, int status = 200)
    {
        const QByteArray body = value.isArray() ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)
                                                : QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
        route(method, path, [body, status](const Recorded &) { return Canned{status, body, {}}; });
    }

    QList<Recorded> requests;

    int count(const QByteArray &method, const QString &path) const
    {
        int total = 0;
        for (const Recorded &request : requests) {
            if (request.method == method && request.url.path() == path)
                ++total;
        }
        return total;
    }

    Recorded last(const QByteArray &method, const QString &path) const
    {
        for (auto it = requests.crbegin(); it != requests.crend(); ++it) {
            if (it->method == method && it->url.path() == path)
                return *it;
        }
        return {};
    }

protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request, QIODevice *outgoing) override
    {
        Recorded recorded;
        switch (operation) {
        case GetOperation:
            recorded.method = "GET";
            break;
        case PostOperation:
            recorded.method = "POST";
            break;
        case PutOperation:
            recorded.method = "PUT";
            break;
        case DeleteOperation:
            recorded.method = "DELETE";
            break;
        default:
            recorded.method = request.attribute(QNetworkRequest::CustomVerbAttribute).toByteArray();
        }
        recorded.url = request.url();
        recorded.authorization = request.rawHeader("Authorization");
        recorded.contentType = request.header(QNetworkRequest::ContentTypeHeader).toByteArray();
        if (outgoing)
            recorded.body = outgoing->readAll();
        requests.append(recorded);
        Canned canned{404, QByteArrayLiteral("{\"error\":\"Record not found\"}"), {}};
        const auto handler = m_routes.constFind(recorded.method + ' ' + recorded.url.path().toUtf8());
        if (handler != m_routes.constEnd())
            canned = handler.value()(recorded);
        return new FakeReply(request, operation, canned, this);
    }

private:
    QHash<QByteArray, Handler> m_routes;
};

QJsonObject account(const QString &id, const QString &acct, const QString &name)
{
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("username"), acct.section(QLatin1Char('@'), 0, 0)},
        {QStringLiteral("acct"), acct},
        {QStringLiteral("display_name"), name},
        {QStringLiteral("note"), QStringLiteral("<p>Hello there</p>")},
        {QStringLiteral("avatar_static"), QStringLiteral("https://files.social.example/avatars/%1.png").arg(id)},
        {QStringLiteral("url"), QStringLiteral("https://social.example/@%1").arg(acct)},
        {QStringLiteral("created_at"), QStringLiteral("2024-01-02T00:00:00.000Z")},
        {QStringLiteral("statuses_count"), 12},
        {QStringLiteral("followers_count"), 3400},
        {QStringLiteral("following_count"), 7},
    };
}

QJsonObject status(const QString &id, const QJsonObject &author, const QString &content)
{
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("account"), author},
        {QStringLiteral("content"), content},
        {QStringLiteral("created_at"), QDateTime::currentDateTimeUtc().addSecs(-300).toString(Qt::ISODate)},
        {QStringLiteral("visibility"), QStringLiteral("public")},
        {QStringLiteral("url"), QStringLiteral("https://social.example/@x/%1").arg(id)},
        {QStringLiteral("replies_count"), 1},
        {QStringLiteral("reblogs_count"), 2},
        {QStringLiteral("favourites_count"), 3},
        {QStringLiteral("favourited"), false},
        {QStringLiteral("reblogged"), false},
        {QStringLiteral("bookmarked"), false},
        {QStringLiteral("media_attachments"), QJsonArray()},
        {QStringLiteral("mentions"), QJsonArray()},
        {QStringLiteral("tags"), QJsonArray()},
    };
}

QJsonObject jsonBody(const Recorded &request)
{
    return QJsonDocument::fromJson(request.body).object();
}

void testInstances()
{
    check(normalizeInstance(QStringLiteral("mastodon.social")) == QLatin1String("https://mastodon.social"), "bare host gets https");
    check(normalizeInstance(QStringLiteral(" https://Mastodon.Social/ ")) == QLatin1String("https://mastodon.social"),
          "host is lower-cased and the slash dropped");
    check(normalizeInstance(QStringLiteral("@me@hachyderm.io")) == QLatin1String("https://hachyderm.io"), "a full handle names its server");
    check(normalizeInstance(QStringLiteral("me@fosstodon.org")) == QLatin1String("https://fosstodon.org"), "user@host names its server");
    check(normalizeInstance(QStringLiteral("https://social.example:8443")) == QLatin1String("https://social.example:8443"), "a port is kept");
    check(normalizeInstance(QStringLiteral("http://social.example")).isEmpty(), "plain http is refused");
    check(normalizeInstance(QStringLiteral("https://social.example/web")).isEmpty(), "a path is refused");
    check(normalizeInstance(QStringLiteral("https://user:pw@social.example")).isEmpty(), "credentials in the address are refused");
    check(normalizeInstance(QStringLiteral("javascript:alert(1)")).isEmpty(), "a script address is refused");
    check(normalizeInstance(QStringLiteral("intranet")).isEmpty(), "a dotless name is refused");
}

void testContent()
{
    const QJsonArray mentions{QJsonObject{{QStringLiteral("id"), QStringLiteral("7")}, {QStringLiteral("url"), QStringLiteral("https://social.example/@bob")},
                                          {QStringLiteral("acct"), QStringLiteral("bob")}}};
    const QString html = QStringLiteral(
        "<p>Hi <span class=\"h-card\"><a href=\"https://social.example/@bob\" class=\"u-url mention\">@<span>bob</span></a></span> see "
        "<a href=\"https://example.com/very/long/path\" rel=\"nofollow\"><span class=\"invisible\">https://</span>"
        "<span class=\"ellipsis\">example.com/very/lo</span><span class=\"invisible\">ng/path</span></a> "
        "<a href=\"https://social.example/tags/Qt\" class=\"mention hashtag\" rel=\"tag\">#<span>Qt</span></a></p>"
        "<p>Fish &amp; chips<br>tonight &#x1F41F;</p><script>alert(1)</script><img src=x onerror=alert(2)>"
        "<a href=\"javascript:alert(3)\">bad</a>");
    const RichText rendered = renderContent(html, mentions);
    check(rendered.html.contains(QLatin1String("href=\"fm-account:7\"")), "a mention links to the account inside ForkMesh", rendered.html);
    check(rendered.html.contains(QLatin1String("href=\"fm-tag:Qt\"")), "a hashtag links to the tag timeline", rendered.html);
    check(rendered.html.contains(QStringLiteral("example.com/very/lo…")), "long links show Mastodon's shortened form", rendered.html);
    check(!rendered.html.contains(QLatin1String("https://</")) && !rendered.plain.contains(QLatin1String("ng/path")),
          "invisible link parts are dropped", rendered.plain);
    check(!rendered.html.contains(QLatin1String("<script")) && !rendered.html.contains(QLatin1String("alert(1)")), "script text is removed",
          rendered.html);
    check(!rendered.html.contains(QLatin1String("<img")) && !rendered.html.contains(QLatin1String("onerror")), "images and handlers are removed");
    check(!rendered.html.contains(QLatin1String("javascript:")), "script links lose their target", rendered.html);
    check(rendered.html.contains(QLatin1String("Fish &amp; chips<br>tonight")), "entities are decoded then re-escaped", rendered.html);
    check(rendered.plain.contains(QStringLiteral("\U0001F41F")), "numeric entities decode to the character", rendered.plain);
    check(rendered.html.contains(QLatin1String("<br><br>Fish")), "paragraphs become breaks", rendered.html);
    check(plainText(QStringLiteral("<p>a</p><p>b</p>")) == QLatin1String("a\n\nb"), "plain text keeps paragraph breaks");
    check(elide(QStringLiteral("abcdefghij"), 5) == QStringLiteral("abcd…"), "elide adds an ellipsis");
}

void testHelpers()
{
    const QByteArray link = "<https://social.example/api/v1/timelines/home?max_id=10>; rel=\"next\", "
                            "<https://social.example/api/v1/timelines/home?min_id=20>; rel=\"prev\"";
    check(linkTarget(link, QStringLiteral("next")).toString() == QLatin1String("https://social.example/api/v1/timelines/home?max_id=10"),
          "the next page comes from the Link header");
    check(linkTarget(link, QStringLiteral("prev")).query() == QLatin1String("min_id=20"), "the previous page comes from the Link header");
    check(codeFromCallback(QStringLiteral("http://127.0.0.1:5555/callback?code=AbCdEf123456&state=s")) == QLatin1String("AbCdEf123456"),
          "a pasted callback address yields its code");
    check(codeFromCallback(QStringLiteral("  QWErty_-12345  ")) == QLatin1String("QWErty_-12345"), "a pasted code is accepted");
    check(codeFromCallback(QStringLiteral("not a code")).isEmpty(), "junk is not a code");
    const QUrlQuery query = callbackQuery("GET /callback?code=xyz&state=abc HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    check(query.queryItemValue(QStringLiteral("code")) == QLatin1String("xyz") && query.queryItemValue(QStringLiteral("state")) == QLatin1String("abc"),
          "the loopback request line is parsed");
    check(callbackQuery("GET /other?code=xyz HTTP/1.1\r\n\r\n").isEmpty(), "other paths are ignored");
    check(pkceChallenge(QStringLiteral("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk")) ==
              QLatin1String("E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"),
          "PKCE S256 matches RFC 7636");
    check(randomToken(32).size() == 43 && randomToken(32) != randomToken(32), "random tokens are long and differ");
    const QUrlQuery authorize(authorizeUrl(QStringLiteral("https://social.example"), QStringLiteral("cid"),
                                           QStringLiteral("http://127.0.0.1:9/callback"), QStringLiteral("st"), QStringLiteral("ch")));
    check(authorize.queryItemValue(QStringLiteral("scope"), QUrl::FullyDecoded) == kScopes &&
              authorize.queryItemValue(QStringLiteral("code_challenge_method")) == QLatin1String("S256"),
          "the authorize address asks for the scopes with PKCE");
    check(statusLength(QStringLiteral("see https://example.com/a/very/long/link/that/goes/on")) == 4 + 23,
          "links count as 23 characters");
    check(statusLength(QStringLiteral("@bob@far.example hi")) == 7, "remote mentions count only the user name");
    check(countText(999) == QLatin1String("999") && countText(1200) == QLatin1String("1.2K") && countText(25000) == QLatin1String("25K") &&
              countText(3000000) == QLatin1String("3M"),
          "counts are shortened");
    const QDateTime now = QDateTime::currentDateTimeUtc();
    check(relativeTime(now.addSecs(-30), now) == QLatin1String("now") && relativeTime(now.addSecs(-600), now) == QLatin1String("10m") &&
              relativeTime(now.addSecs(-7200), now) == QLatin1String("2h") && relativeTime(now.addDays(-3), now) == QLatin1String("3d"),
          "relative times read naturally");
    SavedAccount saved;
    saved.instance = QStringLiteral("https://social.example");
    saved.token = QStringLiteral("t");
    saved.id = QStringLiteral("1");
    saved.acct = QStringLiteral("me");
    const QList<SavedAccount> round = accountsFromJson(accountsToJson({saved}));
    check(round.size() == 1 && round.first().key() == saved.key() && round.first().handle() == QLatin1String("@me@social.example"),
          "saved accounts round-trip");
    check(accountsFromJson("[{\"instance\":\"http://bad\",\"token\":\"t\"}]").isEmpty(), "saved accounts on bad addresses are dropped");
    bool hide = false;
    const QJsonObject filtered{{QStringLiteral("filtered"),
                                QJsonArray{QJsonObject{{QStringLiteral("filter"),
                                                        QJsonObject{{QStringLiteral("title"), QStringLiteral("Spoilers")},
                                                                    {QStringLiteral("filter_action"), QStringLiteral("hide")},
                                                                    {QStringLiteral("context"), QJsonArray{QStringLiteral("home")}}}}}}}};
    check(filterHideReason(filtered, QStringLiteral("home"), &hide) == QLatin1String("Spoilers") && hide, "hide filters hide in their context");
    check(filterHideReason(filtered, QStringLiteral("public"), &hide).isEmpty() && !hide, "filters stay out of other contexts");
}

void testBodies()
{
    ComposeDraft draft;
    draft.text = QStringLiteral("Hello");
    draft.visibility = QStringLiteral("unlisted");
    draft.mediaIds = {QStringLiteral("m1")};
    draft.sensitive = true;
    draft.pollOptions = {QStringLiteral("a"), QStringLiteral("b")};
    draft.inReplyToId = QStringLiteral("9");
    QJsonObject body = composeBody(draft, false);
    check(body.value(QStringLiteral("visibility")).toString() == QLatin1String("unlisted") &&
              body.value(QStringLiteral("in_reply_to_id")).toString() == QLatin1String("9") &&
              body.value(QStringLiteral("media_ids")).toArray().size() == 1 && body.value(QStringLiteral("sensitive")).toBool(),
          "a new post carries visibility, reply and media");
    check(!body.contains(QStringLiteral("poll")), "a poll is not sent alongside media");
    draft.mediaIds.clear();
    body = composeBody(draft, true);
    check(!body.contains(QStringLiteral("visibility")) && !body.contains(QStringLiteral("in_reply_to_id")) &&
              body.value(QStringLiteral("media_ids")).toArray().isEmpty() && body.contains(QStringLiteral("spoiler_text")) &&
              body.value(QStringLiteral("poll")).toObject().value(QStringLiteral("options")).toArray().size() == 2,
          "an edit clears media and warning explicitly and keeps the poll");
    draft.scheduledAt = QDateTime(QDate(2030, 1, 2), QTime(3, 4), QTimeZone::utc());
    check(composeBody(draft, false).value(QStringLiteral("scheduled_at")).toString() == QLatin1String("2030-01-02T03:04:00Z"),
          "scheduled posts send a UTC time");

    const QJsonObject existing{{QStringLiteral("keywords"),
                                QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("k1")}, {QStringLiteral("keyword"), QStringLiteral("old")}},
                                           QJsonObject{{QStringLiteral("id"), QStringLiteral("k2")}, {QStringLiteral("keyword"), QStringLiteral("keep")}}}}};
    const QJsonObject filter = filterBody(existing, QStringLiteral("T"), {QStringLiteral("home")}, QStringLiteral("warn"), 0,
                                          {QStringLiteral("keep"), QStringLiteral("new")}, false);
    const QJsonArray keywords = filter.value(QStringLiteral("keywords_attributes")).toArray();
    check(keywords.size() == 3 && keywords.at(0).toObject().value(QStringLiteral("_destroy")).toBool() &&
              keywords.at(1).toObject().value(QStringLiteral("keyword")).toString() == QLatin1String("keep") &&
              keywords.at(2).toObject().value(QStringLiteral("keyword")).toString() == QLatin1String("new") &&
              !keywords.at(2).toObject().contains(QStringLiteral("id")),
          "filter edits remove, keep and add keywords");
    check(filter.value(QStringLiteral("expires_in")).toString().isEmpty() && filter.contains(QStringLiteral("expires_in")),
          "editing a filter to never expire clears the expiry");
}

void testFeature()
{
    FakeServer server;
    QVariantMap settings;
    forkmesh::market::NativePackageHost host;
    host.packageId = QStringLiteral("mastodon");
    host.network = &server;
    host.setting = [&settings](const QString &key, const QVariant &fallback) { return settings.value(key, fallback); };
    host.setSetting = [&settings](const QString &key, const QVariant &value) { settings.insert(key, value); };
    int badgeChanges = 0;
    host.navigationBadgeChanged = [&badgeChanges] { ++badgeChanges; };

    QJsonObject me = account(QStringLiteral("1"), QStringLiteral("me"), QStringLiteral("Me Myself"));
    me.insert(QStringLiteral("source"), QJsonObject{{QStringLiteral("privacy"), QStringLiteral("unlisted")},
                                                     {QStringLiteral("note"), QStringLiteral("Hello there")},
                                                     {QStringLiteral("fields"), QJsonArray()}});
    const QJsonObject bob = account(QStringLiteral("2"), QStringLiteral("bob@far.example"), QStringLiteral("Bob"));
    auto current = std::make_shared<QJsonObject>(me);
    server.route("GET", QStringLiteral("/api/v1/accounts/verify_credentials"), [current](const Recorded &) {
        return Canned{200, QJsonDocument(*current).toJson(QJsonDocument::Compact), {}};
    });
    server.json("GET", QStringLiteral("/api/v2/instance"),
                QJsonObject{{QStringLiteral("configuration"),
                             QJsonObject{{QStringLiteral("statuses"), QJsonObject{{QStringLiteral("max_characters"), 1000}}}}}});
    server.json("GET", QStringLiteral("/api/v1/lists"),
                QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("L1")}, {QStringLiteral("title"), QStringLiteral("Friends")}}});
    server.json("GET", QStringLiteral("/api/v1/follow_requests"), QJsonArray{bob});
    server.json("GET", QStringLiteral("/api/v1/markers"),
                QJsonObject{{QStringLiteral("notifications"), QJsonObject{{QStringLiteral("last_read_id"), QStringLiteral("100")}}}});
    const QJsonObject s1 = status(QStringLiteral("11"), bob, QStringLiteral("<p>First post</p>"));
    const QJsonObject s3 = status(QStringLiteral("13"), bob, QStringLiteral("<p>Boosted post</p>"));
    QJsonObject s2 = status(QStringLiteral("12"), me, QString());
    s2.insert(QStringLiteral("reblog"), s3);
    QJsonObject s4 = status(QStringLiteral("14"), bob, QStringLiteral("<p>Spoiler!</p>"));
    s4.insert(QStringLiteral("filtered"),
              QJsonArray{QJsonObject{{QStringLiteral("filter"), QJsonObject{{QStringLiteral("title"), QStringLiteral("S")},
                                                                         {QStringLiteral("filter_action"), QStringLiteral("hide")},
                                                                         {QStringLiteral("context"), QJsonArray{QStringLiteral("home")}}}}}});
    server.route("GET", QStringLiteral("/api/v1/timelines/home"), [=](const Recorded &request) {
        const bool more = QUrlQuery(request.url).hasQueryItem(QStringLiteral("max_id"));
        const QJsonArray rows = more ? QJsonArray{status(QStringLiteral("5"), bob, QStringLiteral("<p>Older</p>"))} : QJsonArray{s1, s2, s4};
        return Canned{200, QJsonDocument(rows).toJson(QJsonDocument::Compact),
                      more ? QByteArray() : QByteArray("<https://social.example/api/v1/timelines/home?max_id=11>; rel=\"next\"")};
    });
    server.route("GET", QStringLiteral("/api/v1/notifications"), [=](const Recorded &request) {
        const bool badge = QUrlQuery(request.url).hasQueryItem(QStringLiteral("since_id"));
        QJsonArray rows{QJsonObject{{QStringLiteral("id"), QStringLiteral("120")}, {QStringLiteral("type"), QStringLiteral("favourite")},
                                    {QStringLiteral("account"), bob}, {QStringLiteral("status"), s1},
                                    {QStringLiteral("created_at"), QStringLiteral("2026-10-30T10:00:00.000Z")}},
                        QJsonObject{{QStringLiteral("id"), QStringLiteral("119")}, {QStringLiteral("type"), QStringLiteral("follow")},
                                    {QStringLiteral("account"), bob},
                                    {QStringLiteral("created_at"), QStringLiteral("2026-10-30T09:00:00.000Z")}},
                        QJsonObject{{QStringLiteral("id"), QStringLiteral("118")}, {QStringLiteral("type"), QStringLiteral("mention")},
                                    {QStringLiteral("account"), bob}, {QStringLiteral("status"), s3},
                                    {QStringLiteral("created_at"), QStringLiteral("2026-10-30T08:00:00.000Z")}}};
        Q_UNUSED(badge);
        return Canned{200, QJsonDocument(rows).toJson(QJsonDocument::Compact), {}};
    });

    MastodonFeature feature(host);
    feature.resize(1100, 800);
    feature.show();
    int opened = 0;
    QUrl lastOpened;
    feature.setExternalOpener([&](const QUrl &url) {
        ++opened;
        lastOpened = url;
    });
    feature.setConfirmer([](const QString &, const QString &) { return true; });
    check(!feature.signedIn(), "a fresh page starts signed out");
    check(feature.findChild<QLineEdit *>(QStringLiteral("mastodonInstance")) != nullptr, "the sign-in page asks for a server");

    feature.signInWithToken(QStringLiteral("social.example"), QStringLiteral("tok-1"));
    check(waitFor([&] { return feature.signedIn() && feature.postCards().size() == 2; }), "a token signs in and loads Home",
          QString::number(feature.postCards().size()));
    check(server.last("GET", QStringLiteral("/api/v1/timelines/home")).authorization == "Bearer tok-1", "requests carry the bearer token");
    check(settings.value(QStringLiteral("accounts")).toString().contains(QLatin1String("tok-1")), "the account is kept in package settings");
    check(feature.currentPage().kind == QLatin1String("home"), "Home is the first page");
    check(waitFor([&] { return feature.limits().maxCharacters == 1000; }), "the server's post length limit is read");
    check(waitFor([&] { return feature.navigationBadge() == 3; }) && badgeChanges > 0, "unread notifications become the rail badge",
          QString::number(feature.navigationBadge()));
    bool sawBoost = false;
    for (PostCard *card : feature.postCards())
        sawBoost = sawBoost || card->shownId() == QLatin1String("13");
    check(sawBoost, "a boost shows the boosted post");

    feature.loadMore();
    check(waitFor([&] { return feature.postCards().size() == 3; }), "the next page is appended");

    feature.act(s1, QStringLiteral("favourite"));
    QJsonObject liked = s1;
    liked.insert(QStringLiteral("favourited"), true);
    liked.insert(QStringLiteral("favourites_count"), 4);
    server.json("POST", QStringLiteral("/api/v1/statuses/11/favourite"), liked);
    feature.act(s1, QStringLiteral("favourite"));
    const auto shownFavourited = [&] {
        for (PostCard *card : feature.postCards()) {
            if (card->shownId() == QLatin1String("11"))
                return card->shown().value(QStringLiteral("favourited")).toBool();
        }
        return false;
    };
    check(waitFor(shownFavourited), "favouriting updates the card in place");
    check(waitFor([&] { return !feature.notice().isEmpty() || shownFavourited(); }), "a failed action leaves a notice or succeeds");

    QJsonObject boosted = s3;
    boosted.insert(QStringLiteral("reblogged"), true);
    server.json("POST", QStringLiteral("/api/v1/statuses/13/reblog"),
                QJsonObject{{QStringLiteral("id"), QStringLiteral("99")}, {QStringLiteral("reblog"), boosted}, {QStringLiteral("account"), me}});
    feature.act(s3, QStringLiteral("reblog"));
    check(waitFor([&] {
              for (PostCard *card : feature.postCards()) {
                  if (card->shownId() == QLatin1String("13"))
                      return card->shown().value(QStringLiteral("reblogged")).toBool();
              }
              return false;
          }),
          "boosting marks the inner post as boosted");

    QJsonObject posted = status(QStringLiteral("50"), me, QStringLiteral("<p>Hello world</p>"));
    server.route("POST", QStringLiteral("/api/v1/statuses"), [posted](const Recorded &) {
        return Canned{200, QJsonDocument(posted).toJson(QJsonDocument::Compact), {}};
    });
    ComposeDialog *dialog = feature.compose();
    dialog->setText(QStringLiteral("Hello world"));
    check(dialog->draft().visibility == QLatin1String("unlisted"), "the composer starts from the account's default visibility");
    check(dialog->remaining() == 1000 - 11, "the counter uses the server limit");
    dialog->submit();
    check(waitFor([&] { return server.count("POST", QStringLiteral("/api/v1/statuses")) == 1; }), "posting sends the status");
    const QJsonObject sent = jsonBody(server.last("POST", QStringLiteral("/api/v1/statuses")));
    check(sent.value(QStringLiteral("status")).toString() == QLatin1String("Hello world") &&
              sent.value(QStringLiteral("visibility")).toString() == QLatin1String("unlisted"),
          "the posted body has the text and visibility");
    check(waitFor([&] {
              for (PostCard *card : feature.postCards()) {
                  if (card->shownId() == QLatin1String("50"))
                      return true;
              }
              return false;
          }),
          "the new post appears at the top of Home");

    const QJsonObject source = status(QStringLiteral("11"), bob, QString());
    ComposeDialog *reply = feature.compose(s3);
    check(reply->draft().inReplyToId == QLatin1String("13") && reply->draft().text.startsWith(QLatin1String("@bob@far.example")),
          "a reply mentions the author and links the parent", reply->draft().text);
    reply->close();
    Q_UNUSED(source);

    server.json("DELETE", QStringLiteral("/api/v1/statuses/50"), posted);
    feature.deleteStatus(posted, false);
    check(waitFor([&] {
              for (PostCard *card : feature.postCards()) {
                  if (card->shownId() == QLatin1String("50"))
                      return false;
              }
              return true;
          }),
          "deleting removes the card");

    feature.openPage({QStringLiteral("notifications"), QString(), QString()});
    check(waitFor([&] { return server.count("POST", QStringLiteral("/api/v1/markers")) == 1; }), "opening notifications marks them read");
    const QJsonObject marker = jsonBody(server.last("POST", QStringLiteral("/api/v1/markers")));
    check(marker.value(QStringLiteral("notifications")).toObject().value(QStringLiteral("last_read_id")).toString() == QLatin1String("120"),
          "the read marker moves to the newest notification");
    check(waitFor([&] { return feature.postCards().size() >= 1; }), "mentions show as full posts");

    feature.openPage({QStringLiteral("notifications"), QString(), QStringLiteral("mention")});
    check(waitFor([&] {
              const Recorded last = server.last("GET", QStringLiteral("/api/v1/notifications"));
              return QUrlQuery(last.url).queryItemValue(QStringLiteral("types[]"), QUrl::FullyDecoded) == QLatin1String("mention");
          }),
          "the Mentions tab filters by type");

    feature.goBack();
    check(feature.currentPage().kind == QLatin1String("home"), "Back skips tab switches and returns to the previous page",
          feature.currentPage().kind);

    server.json("GET", QStringLiteral("/api/v1/accounts/relationships"),
                QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("2")}, {QStringLiteral("following"), false},
                                       {QStringLiteral("followed_by"), true}}});
    server.json("GET", QStringLiteral("/api/v1/accounts/2"), bob);
    server.json("GET", QStringLiteral("/api/v1/accounts/2/statuses"), QJsonArray{s1});
    feature.openPage({QStringLiteral("account"), QStringLiteral("2"), QString()});
    check(waitFor([&] {
              for (QPushButton *button : feature.findChildren<QPushButton *>()) {
                  if (button->isVisible() && button->text() == QLatin1String("Follow back"))
                      return true;
              }
              return false;
          }),
          "a profile offers Follow back to a follower");
    server.json("POST", QStringLiteral("/api/v1/accounts/2/follow"),
                QJsonObject{{QStringLiteral("id"), QStringLiteral("2")}, {QStringLiteral("following"), true}});
    feature.follow(QStringLiteral("2"), true, {{QStringLiteral("notify"), true}});
    check(waitFor([&] { return server.count("POST", QStringLiteral("/api/v1/accounts/2/follow")) == 1; }) &&
              jsonBody(server.last("POST", QStringLiteral("/api/v1/accounts/2/follow"))).value(QStringLiteral("notify")).toBool(),
          "following can ask for post notifications");

    feature.openPage({QStringLiteral("profile"), QString(), QString()});
    check(waitFor([&] { return feature.findChild<QLineEdit *>(QStringLiteral("mastodonDisplayName")) != nullptr; }), "the profile editor opens");
    auto *bio = feature.findChild<QPlainTextEdit *>(QStringLiteral("mastodonBio"));
    check(bio && bio->toPlainText() == QLatin1String("Hello there"), "the bio is edited as plain source text");
    QJsonObject renamed = me;
    renamed.insert(QStringLiteral("display_name"), QStringLiteral("New Name"));
    server.route("PATCH", QStringLiteral("/api/v1/accounts/update_credentials"), [current, renamed](const Recorded &) {
        *current = renamed;
        return Canned{200, QJsonDocument(renamed).toJson(QJsonDocument::Compact), {}};
    });
    feature.saveProfile({{QStringLiteral("display_name"), QStringLiteral("New Name")}, {QStringLiteral("locked"), true}}, QString(), QString());
    check(waitFor([&] { return feature.self().value(QStringLiteral("display_name")).toString() == QLatin1String("New Name"); }),
          "saving the profile updates the account");
    const Recorded patch = server.last("PATCH", QStringLiteral("/api/v1/accounts/update_credentials"));
    check(patch.contentType.startsWith("multipart/form-data") && patch.body.contains("name=\"display_name\"") &&
              patch.body.contains("New Name") && patch.body.contains("name=\"locked\"\r\n\r\ntrue"),
          "the profile is sent as a form", QString::fromUtf8(patch.body.left(400)));

    feature.savePostingDefaults(QStringLiteral("private"), true, QStringLiteral("de"));
    check(waitFor([&] { return server.count("PATCH", QStringLiteral("/api/v1/accounts/update_credentials")) == 2; }) &&
              jsonBody(server.last("PATCH", QStringLiteral("/api/v1/accounts/update_credentials")))
                      .value(QStringLiteral("source")).toObject().value(QStringLiteral("privacy")).toString() == QLatin1String("private"),
          "posting defaults are saved as source settings");

    server.json("POST", QStringLiteral("/api/v1/domain_blocks"), QJsonObject());
    feature.blockDomain(QStringLiteral(" Bad.Example "), true);
    check(waitFor([&] { return server.count("POST", QStringLiteral("/api/v1/domain_blocks")) == 1; }) &&
              jsonBody(server.last("POST", QStringLiteral("/api/v1/domain_blocks"))).value(QStringLiteral("domain")).toString() == QLatin1String("bad.example"),
          "blocking a server sends the cleaned domain");

    server.json("POST", QStringLiteral("/api/v1/lists"), QJsonObject{{QStringLiteral("id"), QStringLiteral("L2")}});
    feature.createList(QStringLiteral("  Work  "));
    check(waitFor([&] { return server.count("POST", QStringLiteral("/api/v1/lists")) == 1; }) &&
              jsonBody(server.last("POST", QStringLiteral("/api/v1/lists"))).value(QStringLiteral("title")).toString() == QLatin1String("Work"),
          "creating a list sends its name");
    server.json("POST", QStringLiteral("/api/v1/lists/L1/accounts"), QJsonObject());
    feature.listMember(QStringLiteral("L1"), QStringLiteral("2"), true);
    check(waitFor([&] { return server.count("POST", QStringLiteral("/api/v1/lists/L1/accounts")) == 1; }), "people can be added to a list");

    server.json("POST", QStringLiteral("/api/v1/tags/caf%C3%A9/follow"), QJsonObject());
    feature.followTag(QStringLiteral("#café"), true);
    check(waitFor([&] {
              for (const Recorded &request : server.requests) {
                  if (request.method == "POST" && request.url.path().startsWith(QLatin1String("/api/v1/tags/")) &&
                      request.url.path().endsWith(QLatin1String("/follow")))
                      return true;
              }
              return false;
          }),
          "following a hashtag strips the # and encodes the name");

    server.json("POST", QStringLiteral("/api/v2/filters"), QJsonObject{{QStringLiteral("id"), QStringLiteral("F1")}});
    feature.saveFilter(QString(), filterBody({}, QStringLiteral("Spoilers"), {QStringLiteral("home")}, QStringLiteral("warn"), 3600,
                                             {QStringLiteral("finale")}, true));
    check(waitFor([&] { return server.count("POST", QStringLiteral("/api/v2/filters")) == 1; }) &&
              jsonBody(server.last("POST", QStringLiteral("/api/v2/filters"))).value(QStringLiteral("expires_in")).toInt() == 3600,
          "a new filter is created");

    server.json("GET", QStringLiteral("/api/v2/search"),
                QJsonObject{{QStringLiteral("accounts"), QJsonArray{bob}},
                            {QStringLiteral("hashtags"), QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("qt")}}}},
                            {QStringLiteral("statuses"), QJsonArray{s1}}});
    feature.openPage({QStringLiteral("search"), QStringLiteral("qt"), QString()});
    check(waitFor([&] { return feature.accountRows().size() == 1 && feature.postCards().size() == 1; }), "search shows people and posts");
    check(QUrlQuery(server.last("GET", QStringLiteral("/api/v2/search")).url).queryItemValue(QStringLiteral("resolve")) == QLatin1String("true"),
          "search resolves remote accounts and posts");
    const auto hits = feature.searchHits(QStringLiteral("First"), 10);
    bool statusHit = false;
    for (const auto &hit : hits)
        statusHit = statusHit || hit.id == QLatin1String("status:11");
    check(statusHit, "the top search box finds loaded posts");

    feature.openPage({QStringLiteral("lists"), QString(), QString()});
    check(waitFor([&] { return feature.findChild<QLineEdit *>(QStringLiteral("mastodonNewList")) != nullptr; }), "the lists page opens");
    for (const QString &kind : {QStringLiteral("explore"), QStringLiteral("conversations"), QStringLiteral("follows"),
                                QStringLiteral("moderation"), QStringLiteral("hashtags"), QStringLiteral("filters"),
                                QStringLiteral("scheduled"), QStringLiteral("preferences"), QStringLiteral("bookmarks"),
                                QStringLiteral("local"), QStringLiteral("federated"), QStringLiteral("favourites")}) {
        feature.openPage({kind, QString(), QString()});
        QCoreApplication::processEvents();
        check(feature.currentPage().kind == kind, "every page opens", kind);
    }
    waitFor([] { return false; }, 200);

    server.json("GET", QStringLiteral("/api/v1/bookmarks"), QJsonObject{{QStringLiteral("error"), QStringLiteral("The access token was revoked")}}, 401);
    feature.openPage({QStringLiteral("bookmarks"), QString(), QString()});
    check(waitFor([&] { return !feature.signedIn(); }), "a revoked token returns to sign-in");
    check(feature.notice().contains(QLatin1String("expired or was revoked")), "the user is told why", feature.notice());
    check(feature.accounts().isEmpty() && !settings.value(QStringLiteral("accounts")).toString().contains(QLatin1String("tok-1")),
          "the revoked token is forgotten");

    server.json("POST", QStringLiteral("/api/v1/apps"),
                QJsonObject{{QStringLiteral("client_id"), QStringLiteral("cid")}, {QStringLiteral("client_secret"), QStringLiteral("secret")}});
    feature.beginBrowserSignIn(QStringLiteral("https://social.example"));
    check(waitFor([&] { return opened == 1; }), "browser sign-in opens the authorize page");
    const Recorded apps = server.last("POST", QStringLiteral("/api/v1/apps"));
    check(jsonBody(apps).value(QStringLiteral("scopes")).toString() == kScopes &&
              jsonBody(apps).value(QStringLiteral("redirect_uris")).toString().contains(kOutOfBand),
          "the app registers its scopes and a code fallback");
    const QUrlQuery authorize(lastOpened);
    const PendingSignIn pending = feature.pendingSignIn();
    check(lastOpened.path() == QLatin1String("/oauth/authorize") && authorize.queryItemValue(QStringLiteral("client_id")) == QLatin1String("cid") &&
              authorize.queryItemValue(QStringLiteral("state")) == pending.state &&
              authorize.queryItemValue(QStringLiteral("code_challenge")) == pkceChallenge(pending.verifier),
          "the authorize page gets the client, state and PKCE challenge");
    check(pending.loopback.isEmpty() || (pending.redirect == pending.loopback && feature.callbackPort() > 0),
          "the browser returns to a local listener when one is available");
    feature.finishSignIn(QStringLiteral("http://127.0.0.1:1/callback?code=abcdefgh123&state=wrong"));
    check(server.count("POST", QStringLiteral("/oauth/token")) == 0, "a callback for another attempt is refused");
    feature.useCodeInstead();
    check(feature.pendingSignIn().redirect == kOutOfBand && opened == 2, "the code fallback reopens the page with the out-of-band redirect");

    server.json("POST", QStringLiteral("/oauth/token"), QJsonObject{{QStringLiteral("access_token"), QStringLiteral("tok-2")}});
    feature.finishSignIn(QStringLiteral("abcdefgh123"));
    check(waitFor([&] { return feature.signedIn(); }), "a pasted code completes sign-in");
    const QJsonObject token = jsonBody(server.last("POST", QStringLiteral("/oauth/token")));
    check(token.value(QStringLiteral("code")).toString() == QLatin1String("abcdefgh123") &&
              token.value(QStringLiteral("code_verifier")).toString() == pending.verifier &&
              token.value(QStringLiteral("redirect_uri")).toString() == kOutOfBand,
          "the code is exchanged with its verifier and redirect");
    check(feature.accounts().size() == 1 && feature.accounts().first().clientId == QLatin1String("cid"), "the client is remembered for sign-out");

    server.json("POST", QStringLiteral("/oauth/revoke"), QJsonObject());
    feature.signOut(feature.activeAccount().key());
    check(waitFor([&] { return server.count("POST", QStringLiteral("/oauth/revoke")) == 1; }), "signing out revokes the token");
    check(!feature.signedIn() && feature.accounts().isEmpty(), "signing out returns to the sign-in page");

    bool offline = true;
    for (const Recorded &request : server.requests)
        offline = offline && (request.url.host() == QLatin1String("social.example") || request.url.host() == QLatin1String("files.social.example"));
    check(offline, "every request goes to the chosen server");
}

}

int main(int argc, char **argv)
{
    QTemporaryDir sandbox;
    if (!sandbox.isValid()) {
        std::printf("FAIL: no temporary directory\n");
        return 1;
    }
    const QString home = sandbox.path();
    qputenv("HOME", home.toUtf8());
    qputenv("XDG_DATA_HOME", (home + QStringLiteral("/data")).toUtf8());
    qputenv("XDG_CONFIG_HOME", (home + QStringLiteral("/config")).toUtf8());
    qputenv("XDG_CACHE_HOME", (home + QStringLiteral("/cache")).toUtf8());
    qunsetenv("FORKMESH_HOME");
    qunsetenv("FORKMESH_PROJECTS_DIR");
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForkMeshMastodonTest"));
    QCoreApplication::setApplicationName(QStringLiteral("MastodonTest"));

    testInstances();
    testContent();
    testHelpers();
    testBodies();
    testFeature();

    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
