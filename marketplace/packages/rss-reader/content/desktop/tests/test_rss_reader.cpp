#include "RssReaderFeature.h"
#include "RssReaderFetch.h"
#include "RssReaderModel.h"

#include <QApplication>
#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDialog>
#include <QLabel>
#include <QToolButton>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QTimeZone>
#include <QTimer>

#include <cstdio>
#include <cstring>
#include <functional>

using namespace forkmesh::rssreader;

namespace {

int failures = 0;

void check(bool condition, const char *message, const QString &detail = QString())
{
    if (condition) {
        std::printf("PASS: %s\n", message);
    } else {
        std::printf("FAIL: %s%s%s\n", message, detail.isEmpty() ? "" : " -- ",
                    detail.toUtf8().constData());
        ++failures;
    }
}

qint64 utc(int year, int month, int day, int hour = 0, int minute = 0, int second = 0)
{
    return QDateTime(QDate(year, month, day), QTime(hour, minute, second), QTimeZone::UTC).toMSecsSinceEpoch();
}

bool waitFor(const std::function<bool()> &condition, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return condition();
}

const QByteArray kRss = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE rss [<!ENTITY boom "boom">]>
<rss version="2.0" xmlns:content="http://purl.org/rss/1.0/modules/content/" xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:atom="http://www.w3.org/2005/Atom">
<channel>
<title>Example Engineering</title>
<link>https://engineering.example.com/</link>
<atom:link rel="self" href="https://engineering.example.com/feed.xml"/>
<image><url>https://engineering.example.com/icon.png</url></image>
<item>
 <title>Shipping the new build&nbsp;system</title>
 <link>https://engineering.example.com/posts/build</link>
 <guid isPermaLink="false">post-1</guid>
 <description>Short summary</description>
 <content:encoded><![CDATA[<p>Full <b>text</b> &nbsp;here<script>alert(1)</script></p>]]></content:encoded>
 <dc:creator>Avery Example</dc:creator>
 <pubDate>Tue, 14 Oct 2026 14:20:00 GMT</pubDate>
 <comments>https://engineering.example.com/posts/build#comments</comments>
 <enclosure url="https://cdn.example.com/episode.mp3" type="audio/mpeg" length="12345"/>
</item>
<item>
 <title>No guid item</title>
 <link>https://engineering.example.com/posts/no-guid</link>
 <description>&lt;p&gt;Escaped &amp;amp; html &copy;&lt;/p&gt;</description>
 <author>editor@example.com (Sam Example)</author>
 <pubDate>Mon, 13 Oct 2026 09:00:00 -0400</pubDate>
</item>
<item>
 <title>Title only</title>
 <description>Nothing else &undefined; here</description>
</item>
</channel></rss>)";

const QByteArray kRdf = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#" xmlns="http://purl.org/rss/1.0/" xmlns:dc="http://purl.org/dc/elements/1.1/">
<channel rdf:about="https://news.example.org/"><title>Example News</title><link>https://news.example.org/</link></channel>
<item rdf:about="https://news.example.org/1"><title>First</title><link>https://news.example.org/1</link><description>One</description><dc:date>2026-10-12T08:30:00+02:00</dc:date><dc:creator>Riley Example</dc:creator></item>
</rdf:RDF>)";

const QByteArray kAtom = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom" xml:base="https://blog.example.net/">
 <title type="text">Example Notes</title>
 <link rel="self" href="/atom.xml"/>
 <link rel="alternate" type="text/html" href="/"/>
 <icon>/favicon.png</icon>
 <entry>
  <id>tag:blog.example.net,2026:1</id>
  <title type="html">Hello &amp;lt;World&amp;gt;</title>
  <link rel="alternate" type="text/html" href="/2026/hello"/>
  <link rel="enclosure" href="/media/clip.mp4" type="video/mp4" length="99"/>
  <updated>2026-10-11T10:00:00Z</updated>
  <published>2026-10-10T10:00:00.123456Z</published>
  <author><name>Jordan Example</name></author>
  <summary>Summary text</summary>
  <content type="xhtml"><div xmlns="http://www.w3.org/1999/xhtml"><p>Rich <em>xhtml</em> &amp; more</p></div></content>
 </entry>
 <entry>
  <id>tag:blog.example.net,2026:2</id>
  <title>Second</title>
  <link href="https://blog.example.net/2026/second"/>
  <updated>2026-10-11T12:00:00+0100</updated>
  <summary type="text">Line one
Line two &lt;b&gt;</summary>
 </entry>
</feed>)";

const QByteArray kJson = R"({"version":"https://jsonfeed.org/version/1.1","title":"Example Podcast",
 "home_page_url":"https://pod.example.com/","favicon":"https://pod.example.com/fav.png",
 "authors":[{"name":"Feed Author"}],
 "items":[{"id":42,"url":"https://pod.example.com/42","title":"Episode 42","content_html":"<p>Show notes</p>",
           "date_published":"2026-10-09T07:00:00-07:00",
           "attachments":[{"url":"https://pod.example.com/42.mp3","mime_type":"audio/mpeg","size_in_bytes":1000}]},
          {"id":"b","external_url":"https://elsewhere.example.com/b","content_text":"Plain & simple\nsecond","summary":"S"}]})";

QByteArray rssWithItems(int count, int startDay, const QString &prefix)
{
    QByteArray xml = "<rss version=\"2.0\"><channel><title>Bulk Example</title>";
    for (int i = 0; i < count; ++i) {
        const QDateTime when = QDateTime(QDate(2020, 1, 1), QTime(0, 0), QTimeZone::UTC)
                                   .addSecs(qint64(startDay) * 86400 + qint64(i) * 60);
        xml += QStringLiteral("<item><title>%1 %2</title><guid>%1-%2</guid><pubDate>%3</pubDate></item>")
                   .arg(prefix)
                   .arg(i)
                   .arg(when.toString(Qt::RFC2822Date))
                   .toUtf8();
    }
    xml += "</channel></rss>";
    return xml;
}

class FakeReply final : public QNetworkReply
{
public:
    FakeReply(const QNetworkRequest &request, int status, const QByteArray &body,
              const QList<QPair<QByteArray, QByteArray>> &headers, QObject *parent)
        : QNetworkReply(parent), m_body(body)
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        for (const auto &header : headers) {
            setRawHeader(header.first, header.second);
            if (header.first.toLower() == "content-type")
                setHeader(QNetworkRequest::ContentTypeHeader, QString::fromUtf8(header.second));
        }
        if (status >= 400)
            setError(status >= 500 ? QNetworkReply::InternalServerError : QNetworkReply::ContentNotFoundError,
                     QStringLiteral("HTTP %1").arg(status));
        QTimer::singleShot(0, this, [this] { deliver(); });
    }

    void abort() override
    {
        if (isFinished())
            return;
        setError(QNetworkReply::OperationCanceledError, QStringLiteral("Operation canceled"));
        setFinished(true);
        emit errorOccurred(QNetworkReply::OperationCanceledError);
        emit finished();
    }

    qint64 bytesAvailable() const override
    {
        return m_body.size() - m_position + QIODevice::bytesAvailable();
    }

    bool isSequential() const override { return true; }

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        const qint64 count = qMin(maxSize, qint64(m_body.size()) - m_position);
        if (count <= 0)
            return isFinished() ? -1 : 0;
        std::memcpy(data, m_body.constData() + m_position, size_t(count));
        m_position += count;
        return count;
    }

private:
    void deliver()
    {
        if (isFinished())
            return;
        emit metaDataChanged();
        emit downloadProgress(m_body.size(), m_body.size());
        if (isFinished())
            return;
        emit readyRead();
        setFinished(true);
        emit finished();
    }

    QByteArray m_body;
    qint64 m_position = 0;
};

class FakeNetwork final : public QNetworkAccessManager
{
public:
    struct Route {
        int status = 200;
        QByteArray body;
        QList<QPair<QByteArray, QByteArray>> headers;
    };
    QHash<QString, Route> routes;
    QStringList requested;
    int active = 0;
    int peak = 0;

    void route(const QString &url, int status, const QByteArray &body, const QByteArray &type = "application/rss+xml",
               QList<QPair<QByteArray, QByteArray>> extra = {})
    {
        extra.prepend({"Content-Type", type});
        routes.insert(url, Route{status, body, extra});
    }

protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override
    {
        const QString url = request.url().toString();
        requested.append(url);
        Route route = routes.value(url, Route{404, "missing", {{"Content-Type", "text/plain"}}});
        for (const auto &header : route.headers) {
            if (header.first == "ETag" && request.rawHeader("If-None-Match") == header.second) {
                route.status = 304;
                route.body.clear();
            }
        }
        auto *reply = new FakeReply(request, route.status, route.body, route.headers, this);
        ++active;
        peak = qMax(peak, active);
        connect(reply, &QNetworkReply::finished, this, [this] { --active; });
        return reply;
    }
};

FetchResult fetchNow(FetchQueue &queue, const FetchRequest &request)
{
    FetchResult result;
    bool done = false;
    queue.get(request, [&](const FetchResult &value) {
        result = value;
        done = true;
    });
    waitFor([&] { return done; });
    return result;
}

void testDates()
{
    check(parseDate(QStringLiteral("Tue, 14 Oct 2026 14:20:00 GMT")) == utc(2026, 10, 14, 14, 20), "RFC 822 date with GMT");
    check(parseDate(QStringLiteral("14 Oct 26 14:20 +0200")) == utc(2026, 10, 14, 12, 20), "RFC 822 two-digit year, offset");
    check(parseDate(QStringLiteral("Tue, 14 Oct 2026 14:20:00 EST")) == utc(2026, 10, 14, 19, 20), "RFC 822 named zone");
    check(parseDate(QStringLiteral("2026-10-14")) == utc(2026, 10, 14), "ISO date only");
    check(parseDate(QStringLiteral("2026-10-14T14:20:00")) == utc(2026, 10, 14, 14, 20), "ISO without zone is UTC");
    check(parseDate(QStringLiteral("2026-10-14 14:20:00Z")) == utc(2026, 10, 14, 14, 20), "ISO with space separator");
    check(parseDate(QStringLiteral("2026-10-14T16:20:00.5+02:00")) == utc(2026, 10, 14, 14, 20) + 500, "ISO with fraction and offset");
    check(parseDate(QStringLiteral("whenever")) < 0, "garbage date rejected");
    check(parseDate(QString()) < 0, "missing date rejected");
}

void testParsing()
{
    const ParsedFeed rss = parseFeed(kRss, QUrl(QStringLiteral("https://engineering.example.com/feed.xml")));
    check(rss.ok && rss.format == QLatin1String("RSS 2.0"), "RSS 2.0 parses despite DOCTYPE and undefined entities", rss.error);
    check(rss.title == QLatin1String("Example Engineering"), "RSS channel title");
    check(rss.homePage == QLatin1String("https://engineering.example.com/"), "RSS home page ignores atom:link");
    check(rss.iconUrl == QLatin1String("https://engineering.example.com/icon.png"), "RSS image url");
    check(rss.items.size() == 3, "RSS item count", QString::number(rss.items.size()));
    if (rss.items.size() == 3) {
        const ParsedItem &first = rss.items.at(0);
        check(first.title == QLatin1String("Shipping the new build system"), "nbsp in title repaired", first.title);
        check(first.html.contains(QStringLiteral("<p>Full <b>text</b>")), "content:encoded preferred over description");
        check(first.author == QLatin1String("Avery Example"), "dc:creator author");
        check(first.publishedMs == utc(2026, 10, 14, 14, 20), "RSS pubDate");
        check(first.id == QLatin1String("post-1"), "RSS guid");
        check(first.comments == QLatin1String("https://engineering.example.com/posts/build#comments"), "RSS comments");
        check(first.enclosures.size() == 1 && first.enclosures.first().type == QLatin1String("audio/mpeg") &&
                  first.enclosures.first().length == 12345,
              "RSS enclosure");
        const ParsedItem &second = rss.items.at(1);
        check(second.author == QLatin1String("Sam Example"), "RSS author email form", second.author);
        check(second.publishedMs == utc(2026, 10, 13, 13, 0), "RSS pubDate with offset");
        check(second.html.contains(QStringLiteral("<p>Escaped &amp; html")), "escaped description is HTML", second.html);
        check(makeGuid(second.id, second.link, second.title, second.publishedMs) == second.link, "guid falls back to link");
        const ParsedItem &third = rss.items.at(2);
        check(makeGuid(third.id, third.link, third.title, third.publishedMs) == QLatin1String("Title only"),
              "guid falls back to title without date");
        check(third.html.contains(QStringLiteral("&undefined;")), "unknown entity kept as text", third.html);
    }
    check(makeGuid(QString(), QString(), QStringLiteral("  A   title "), 1000) == QLatin1String("A title|1000"),
          "guid title plus published ms, whitespace collapsed");
    check(makeGuid(QString(601, QLatin1Char('x')), QString(), QString(), -1).size() == kMaxGuidLength, "guid capped at 600");

    const ParsedFeed rdf = parseFeed(kRdf, QUrl(QStringLiteral("https://news.example.org/index.rdf")));
    check(rdf.ok && rdf.format == QLatin1String("RSS 1.0") && rdf.items.size() == 1, "RDF parses", rdf.error);
    if (!rdf.items.isEmpty()) {
        check(rdf.items.first().id == QLatin1String("https://news.example.org/1"), "RDF rdf:about is the id");
        check(rdf.items.first().publishedMs == utc(2026, 10, 12, 6, 30), "RDF dc:date");
        check(rdf.items.first().author == QLatin1String("Riley Example"), "RDF dc:creator");
    }
    check(rdf.title == QLatin1String("Example News"), "RDF channel title");

    const ParsedFeed atom = parseFeed(kAtom, QUrl(QStringLiteral("https://blog.example.net/atom.xml")));
    check(atom.ok && atom.format == QLatin1String("Atom 1.0") && atom.items.size() == 2, "Atom parses", atom.error);
    check(atom.homePage == QLatin1String("https://blog.example.net/"), "Atom alternate link resolved against xml:base");
    check(atom.iconUrl == QLatin1String("https://blog.example.net/favicon.png"), "Atom icon");
    if (atom.items.size() == 2) {
        const ParsedItem &first = atom.items.at(0);
        check(first.title == QLatin1String("Hello <World>"), "Atom html title", first.title);
        check(first.link == QLatin1String("https://blog.example.net/2026/hello"), "Atom alternate entry link");
        check(first.html == QLatin1String("<p>Rich <em>xhtml</em> &amp; more</p>"), "Atom xhtml content", first.html);
        check(first.publishedMs == utc(2026, 10, 10, 10, 0) + 123, "Atom published preferred over updated");
        check(first.author == QLatin1String("Jordan Example"), "Atom author name");
        check(first.enclosures.size() == 1 && first.enclosures.first().url == QLatin1String("https://blog.example.net/media/clip.mp4"),
              "Atom enclosure link");
        const ParsedItem &second = atom.items.at(1);
        check(second.publishedMs == utc(2026, 10, 11, 11, 0), "Atom updated when no published");
        check(second.html.contains(QStringLiteral("Line one<br>Line two &lt;b&gt;")), "Atom text summary escaped", second.html);
    }

    const ParsedFeed json = parseFeed(kJson, QUrl(QStringLiteral("https://pod.example.com/feed.json")));
    check(json.ok && json.format == QLatin1String("JSON Feed 1.1") && json.items.size() == 2, "JSON Feed parses", json.error);
    if (json.items.size() == 2) {
        check(json.items.at(0).id == QLatin1String("42"), "JSON Feed numeric id");
        check(json.items.at(0).publishedMs == utc(2026, 10, 9, 14, 0), "JSON Feed date");
        check(json.items.at(0).author == QLatin1String("Feed Author"), "JSON Feed feed-level author");
        check(json.items.at(0).enclosures.size() == 1, "JSON Feed attachments");
        check(json.items.at(1).link == QLatin1String("https://elsewhere.example.com/b"), "JSON Feed external_url");
        check(json.items.at(1).html == QLatin1String("Plain &amp; simple<br>second"), "JSON Feed content_text", json.items.at(1).html);
        check(json.items.at(1).publishedMs < 0, "JSON Feed missing date");
    }
    check(json.iconUrl == QLatin1String("https://pod.example.com/fav.png"), "JSON Feed favicon");

    const ParsedFeed bulk = parseFeed(rssWithItems(250, 0, QStringLiteral("Bulk")), QUrl(QStringLiteral("https://bulk.example.com/")));
    check(bulk.items.size() == kMaxItemsPerFetch, "at most 200 items per fetch", QString::number(bulk.items.size()));
    check(!parseFeed("<html><body>hi</body></html>", QUrl()).ok, "HTML is not a feed");
    check(!parseFeed("not xml at all", QUrl()).ok, "junk is not a feed");
    check(prepareXml("a &#1; b").contains("&#xFFFD;"), "invalid character reference replaced");
}

void testSanitizer()
{
    const QUrl base(QStringLiteral("https://example.com/post"));
    const QString links = sanitizeHtml(QStringLiteral(
        "<p onclick=\"x\">Hi <a href=\"javascript:alert(1)\">bad</a> <a href=\"/rel\" title=\"t\" target=\"_blank\">ok</a> "
        "<a href=\"mailto:someone@example.com\">m</a> <a href=\"java&#x0A;script:alert(1)\">sneaky</a></p>"), base);
    check(links == QLatin1String("<p>Hi <a>bad</a> <a href=\"https://example.com/rel\" title=\"t\">ok</a> "
                                 "<a href=\"mailto:someone@example.com\">m</a> <a>sneaky</a></p>"),
          "links filtered to http(s) and mailto", links);
    const QString dropped = sanitizeHtml(QStringLiteral(
        "<style>p{color:red}</style><script>evil()</script><svg><text>svg</text></svg>"
        "<form><input value=\"x\"><p>form text</p></form><object>obj</object>kept"), base);
    check(dropped == QLatin1String("kept"), "script, style, svg, form, object dropped with content", dropped);
    const QString iframe = sanitizeHtml(QStringLiteral("<iframe src=\"https://video.example.com/embed/1\">fallback</iframe>"), base);
    check(iframe.contains(QStringLiteral("<a href=\"https://video.example.com/embed/1\">Embedded content: video.example.com</a>")) &&
              !iframe.contains(QStringLiteral("iframe")) && !iframe.contains(QStringLiteral("fallback")),
          "iframe becomes a link", iframe);
    const QString images = sanitizeHtml(QStringLiteral(
        "<img src=\"data:image/png;base64,AAAA\"><img src=\"https://img.example.com/a.png\" width=\"100\" height=\"x\" onerror=\"y\" alt=\"A\">"), base);
    check(images == QLatin1String("<img src=\"https://img.example.com/a.png\" width=\"100\" alt=\"A\">"),
          "images need an http(s) src and keep allowed attributes", images);
    check(sanitizeHtml(QStringLiteral("<b>bold"), base) == QLatin1String("<b>bold</b>"), "unclosed tags closed");
    check(sanitizeHtml(QStringLiteral("<custom-el>kept</custom-el>"), base) == QLatin1String("kept"), "unknown tags unwrapped");
    check(sanitizeHtml(QStringLiteral("<table><tr><td colspan=\"2\" style=\"x\">c</td></tr></table>"), base) ==
              QLatin1String("<table><tr><td colspan=\"2\">c</td></tr></table>"),
          "table cell spans kept");
    const QString entities = sanitizeHtml(QStringLiteral("Tom &amp; Jerry &copy; 1 < 2 &bogus;"), base);
    check(entities == QStringLiteral("Tom &amp; Jerry © 1 &lt; 2 &amp;bogus;"), "text re-escaped", entities);
    check(sanitizeHtml(QStringLiteral("<ol start=\"3\" type=\"a\"><li>x</li></ol><time datetime=\"2026\">t</time>"), base) ==
              QLatin1String("<ol start=\"3\"><li>x</li></ol><time datetime=\"2026\">t</time>"),
          "ol start and time datetime kept");
    check(plainText(QStringLiteral("<p>Hello</p><p>World&nbsp;&amp; more</p><script>x</script>")) ==
              QStringLiteral("Hello World & more"),
          "plain text from HTML");
}

void testDiscovery()
{
    const QString html = QStringLiteral(
        "<html><head><link rel=\"alternate\" type=\"application/atom+xml\" href=\"/atom.xml\">"
        "<link rel=\"alternate\" type=\"application/rss+xml\" href=\"https://site.example.com/rss.xml\">"
        "<link rel=\"alternate\" type=\"application/feed+json\" href=\"feed.json\">"
        "<link rel=\"alternate\" type=\"application/rss+xml\" title=\"Comments\" href=\"/comments.xml\">"
        "<link rel=\"stylesheet\" href=\"x.css\"></head><body>"
        "<link rel=\"alternate\" type=\"application/rss+xml\" href=\"/late.xml\"></body></html>");
    const QList<QUrl> links = discoverFeedLinks(html, QUrl(QStringLiteral("https://site.example.com/blog/")));
    check(links.size() == 3, "at most three discovered feeds", QString::number(links.size()));
    if (links.size() == 3) {
        check(links.at(0).toString() == QLatin1String("https://site.example.com/rss.xml") &&
                  links.at(1).toString() == QLatin1String("https://site.example.com/comments.xml") &&
                  links.at(2).toString() == QLatin1String("https://site.example.com/atom.xml"),
              "RSS before Atom before JSON Feed");
    }
    const QList<QUrl> fallbacks = fallbackFeedUrls(QUrl(QStringLiteral("https://site.example.com:8443/some/page")));
    check(fallbacks.size() == 5 && fallbacks.first().toString() == QLatin1String("https://site.example.com:8443/feed") &&
              fallbacks.at(3).toString() == QLatin1String("https://site.example.com:8443/atom.xml") &&
              fallbacks.last().toString() == QLatin1String("https://site.example.com:8443/rss/"),
          "fallback paths on the same origin, Ghost's /rss/ last");
    check(normalizeAddress(QStringLiteral("feed://example.com/rss")).toString() == QLatin1String("https://example.com/rss"), "feed:// becomes https");
    check(normalizeAddress(QStringLiteral("feed:https://example.com/a")).toString() == QLatin1String("https://example.com/a"), "feed:https becomes https");
    check(normalizeAddress(QStringLiteral("  Example.com ")).toString() == QLatin1String("https://example.com/"), "bare host gets https");
    check(!normalizeAddress(QStringLiteral("ftp://example.com/x")).isValid(), "ftp rejected");
    check(!normalizeAddress(QStringLiteral("   ")).isValid(), "blank rejected");
    check(feedKey(QUrl(QStringLiteral("http://Example.com/feed"))) == feedKey(QUrl(QStringLiteral("https://example.com/feed"))),
          "duplicate detection ignores scheme and case");
    check(feedKey(QUrl(QStringLiteral("https://example.com/feed?a=1"))) != feedKey(QUrl(QStringLiteral("https://example.com/feed?a=2"))),
          "duplicate detection keeps the query");
    check(feedKey(QUrl(QStringLiteral("https://example.com:8443/feed"))) != feedKey(QUrl(QStringLiteral("https://example.com/feed"))),
          "duplicate detection keeps an explicit port");
}

void testStore()
{
    const qint64 at = utc(2026, 10, 15, 12, 0);
    Store store;
    QString error;
    const QString id = store.addFeed(QUrl(QStringLiteral("https://engineering.example.com/feed.xml")), QString(), QString(), at, &error);
    check(!id.isEmpty(), "feed added", error);
    check(store.addFeed(QUrl(QStringLiteral("http://engineering.example.com/feed.xml")), QString(), QString(), at, &error).isEmpty() &&
              error.contains(QStringLiteral("already follow")),
          "duplicate feed refused", error);
    ParsedFeed parsed = parseFeed(kRss, QUrl(QStringLiteral("https://engineering.example.com/feed.xml")));
    check(store.merge(id, parsed, at) == 3, "three new articles");
    check(store.feed(id)->title == QLatin1String("Example Engineering"), "feed title taken from the feed");
    const ArticleRef first{id, QStringLiteral("post-1")};
    const Article *article = store.article(first);
    check(article && !article->html.contains(QStringLiteral("script")) && article->html.contains(QStringLiteral("<b>text</b>")),
          "stored HTML is sanitized", article ? article->html : QString());
    check(article && article->summary.startsWith(QStringLiteral("Full text")), "summary is plain text", article ? article->summary : QString());
    const Article *titleOnly = store.article({id, QStringLiteral("Title only")});
    check(titleOnly && titleOnly->publishedMs == at && !titleOnly->datedByFeed, "undated article uses first-seen time");
    check(store.feedUnread(id) == 3 && store.totalUnread() == 3, "unread counts");
    check(store.unreadCount({SourceKind::Today, QString()}, at) == 2, "Today counts the last 24 hours",
          QString::number(store.unreadCount({SourceKind::Today, QString()}, at)));
    store.setRead(first, true);
    store.setStarred(first, true);
    check(store.feedUnread(id) == 2, "reading lowers the unread count");
    check(store.unreadCount({SourceKind::Starred, QString()}, at) == 0, "starred unread count");

    QByteArray edited = kRss;
    edited.replace("Shipping the new build&nbsp;system", "Shipping the new build system, updated");
    edited.replace("<pubDate>Mon, 13 Oct 2026", "<pubDate>Mon, 13 Oct 2031");
    parsed = parseFeed(edited, QUrl(QStringLiteral("https://engineering.example.com/feed.xml")));
    check(store.merge(id, parsed, at) == 0, "re-merge adds nothing");
    article = store.article(first);
    check(article && article->read && article->starred && article->title.endsWith(QStringLiteral("updated")),
          "updated content keeps read and star");
    const Article *noGuid = store.article({id, QStringLiteral("https://engineering.example.com/posts/no-guid")});
    check(noGuid && noGuid->publishedMs == utc(2026, 10, 13, 13, 0), "a date far in the future is ignored on update");

    const QString bulkId = store.addFeed(QUrl(QStringLiteral("https://bulk.example.com/feed")), QString(), QString(), at, &error);
    store.merge(bulkId, parseFeed(rssWithItems(200, 0, QStringLiteral("A")), QUrl()), at);
    store.setStarred({bulkId, QStringLiteral("A-0")}, true);
    store.merge(bulkId, parseFeed(rssWithItems(200, 10, QStringLiteral("B")), QUrl()), at);
    store.merge(bulkId, parseFeed(rssWithItems(200, 20, QStringLiteral("C")), QUrl()), at);
    check(store.articles(bulkId).size() == kMaxArticlesPerFeed + 1, "newest 400 kept plus starred",
          QString::number(store.articles(bulkId).size()));
    check(store.article({bulkId, QStringLiteral("A-0")}) && !store.article({bulkId, QStringLiteral("A-1")}),
          "old starred article survives pruning");

    QList<ArticleRef> refs = store.query({SourceKind::Feed, id}, QString(), false, at);
    check(refs.size() == 3 && refs.first().guid == QLatin1String("Title only"), "newest first");
    store.settings().oldestFirst = true;
    refs = store.query({SourceKind::Feed, id}, QString(), false, at);
    check(!refs.isEmpty() && refs.first().guid == QLatin1String("https://engineering.example.com/posts/no-guid"), "oldest first");
    store.settings().oldestFirst = false;
    check(store.query({SourceKind::Feed, id}, QString(), true, at).size() == 2, "unread only");
    check(store.query({SourceKind::AllUnread, QString()}, QStringLiteral("title only"), false, at).size() == 1, "search");
    check(store.query({SourceKind::Starred, QString()}, QString(), false, at).size() == 2, "starred smart feed");
    store.settings().groupByFeed = true;
    refs = store.query({SourceKind::Starred, QString()}, QString(), false, at);
    check(refs.size() == 2 && refs.first().feedId == bulkId, "group by feed sorts by feed name");
    store.settings().groupByFeed = false;

    check(store.addFolder(QStringLiteral("Tech"), &error), "folder added");
    check(!store.addFolder(QStringLiteral("tech"), &error), "duplicate folder refused");
    check(store.moveFeed(id, QStringLiteral("Tech")) && store.feed(id)->folder == QLatin1String("Tech"), "feed moved");
    check(store.unreadCount({SourceKind::Folder, QStringLiteral("Tech")}, at) == store.feedUnread(id), "folder unread count");
    check(store.renameFolder(QStringLiteral("Tech"), QStringLiteral("Technology"), &error) &&
              store.feed(id)->folder == QLatin1String("Technology"),
          "folder rename moves its feeds");
    store.renameFeed(id, QStringLiteral("My Engineering"));
    check(store.feed(id)->displayName() == QLatin1String("My Engineering"), "custom feed name");

    check(backoffMs(0) == kCheckIntervalMs && backoffMs(1) == 2 * kCheckIntervalMs &&
              backoffMs(2) == 4 * kCheckIntervalMs && backoffMs(12) == kMaxBackoffMs,
          "failure backoff doubles up to a day");
    store.recordFailure(id, QStringLiteral("HTTP 500"), at);
    check(store.feed(id)->failures == 1 && store.feed(id)->nextDueMs == at + backoffMs(1) && !store.errorLog().isEmpty(),
          "failure recorded and backed off");
    check(!store.dueFeeds(at, false).contains(id) && store.dueFeeds(at, true).contains(id), "backing-off feed not due unless forced");
    store.recordSuccess(id, at, QStringLiteral("\"v2\""), QString(), QUrl(QStringLiteral("https://engineering.example.com/new-feed.xml")));
    check(store.feed(id)->failures == 0 && store.feed(id)->nextDueMs == at + kCheckIntervalMs &&
              store.feed(id)->url == QLatin1String("https://engineering.example.com/new-feed.xml") &&
              store.feed(id)->etag == QLatin1String("\"v2\""),
          "success resets backoff and follows a permanent move");

    QTemporaryDir directory;
    DiskStore disk(directory.filePath(QStringLiteral("rss-reader")));
    store.setStoreId(QStringLiteral("store-one"));
    store.settings().summaryLines = 3;
    check(disk.saveState(store), "state saved");
    for (const Feed &owner : store.feeds())
        check(disk.saveArticles(store, owner.id), "articles saved");
    Store loaded;
    check(!disk.load(loaded, QStringLiteral("other-store")), "a different store id is not loaded");
    check(disk.load(loaded, QStringLiteral("store-one")), "store reloaded");
    check(loaded.feeds().size() == 2 && loaded.folders().size() == 1 && loaded.settings().summaryLines == 3, "feeds, folders, settings survive");
    const Article *reloaded = loaded.article(first);
    check(reloaded && reloaded->read && reloaded->starred && loaded.feedUnread(id) == store.feedUnread(id), "read and star survive");
    disk.removeFeedFiles(bulkId);
    check(!QFile::exists(directory.filePath(QStringLiteral("rss-reader/articles/%1.json").arg(bulkId))), "unsubscribe removes the article file");

    const QJsonObject bundle = store.exportBundle(at);
    check(bundle.value(QStringLiteral("subscriptionsOpml")).toString().contains(QStringLiteral("new-feed.xml")) &&
              bundle.value(QStringLiteral("starred")).toArray().size() == 2 &&
              bundle.value(QStringLiteral("settings")).toObject().value(QStringLiteral("summaryLines")).toInt() == 3,
          "export bundle holds OPML, starred articles and settings");
    Store restored;
    const OpmlImportResult imported =
        restored.importOpml(parseOpml(bundle.value(QStringLiteral("subscriptionsOpml")).toString().toUtf8()), at);
    check(imported.added == 2 && imported.folders == 1, "subscriptions restored from OPML");
    check(restored.restoreStarred(bundle.value(QStringLiteral("starred")).toArray(), at) == 2 &&
              restored.query({SourceKind::Starred, QString()}, QString(), false, at).size() == 2,
          "starred articles restored");
    check(restored.feedByUrl(QUrl(QStringLiteral("https://engineering.example.com/new-feed.xml")))->displayName() ==
              QLatin1String("My Engineering"),
          "custom names travel in the OPML");
}

void testOpml()
{
    const QByteArray opml = R"(<?xml version="1.0"?><opml version="1.0"><head><title>t</title></head><body>
<outline text="Tech"><outline text="Example Engineering" type="rss" xmlUrl="https://engineering.example.com/feed.xml" htmlUrl="https://engineering.example.com/"/>
  <outline text="Nested"><outline text="Deep Example" xmlUrl="https://deep.example.com/rss"/></outline></outline>
<outline title="Loose Example" xmlUrl="https://loose.example.org/atom.xml"/>
<outline text="Empty Folder"/>
<outline text="Broken" xmlUrl="not a url at all ::"/>
</body></opml>)";
    const OpmlDocument document = parseOpml(opml);
    check(document.ok && document.folders == QStringList({QStringLiteral("Tech"), QStringLiteral("Empty Folder")}), "OPML folders",
          document.folders.join(QLatin1Char(',')));
    check(document.feeds.size() == 4, "OPML feeds", QString::number(document.feeds.size()));
    if (document.feeds.size() == 4) {
        check(document.feeds.at(1).folder == QLatin1String("Tech"), "nested folders flatten into the top-level folder");
        check(document.feeds.at(2).folder.isEmpty() && document.feeds.at(2).title == QLatin1String("Loose Example"), "top-level feed");
    }
    Store store;
    const OpmlImportResult result = store.importOpml(document, 1000);
    check(result.added == 3 && result.skipped == 1 && result.folders == 2, "OPML import counts");
    const OpmlDocument again = parseOpml(writeOpml(store.feeds(), store.folders(), 1000));
    check(again.ok && again.feeds.size() == 3 && again.folders.size() == 2, "OPML export round-trips");
    check(store.importOpml(again, 2000).skipped == 3, "re-import skips feeds already followed");
    check(!parseOpml("<rss/>").ok, "non-OPML refused");
}

void testReader()
{
    const QString page = QStringLiteral(
        "<html><head><title>Example Article</title><script>var x;</script></head><body>"
        "<nav><p>Home About Contact navigation text that is long enough to matter in scoring</p></nav>"
        "<div class=\"content\"><h1>Headline</h1><p>First paragraph with enough words to count as body text in the scoring.</p>"
        "<p>Second paragraph also has plenty of words so the scorer picks this container.</p><blockquote>A quote</blockquote>"
        "<form><p>Subscribe form text</p></form><img src=\"/picture.png\" alt=\"Picture\"></div>"
        "<aside><p>Sidebar</p></aside><footer><p>Footer text</p></footer></body></html>");
    const Readable readable = extractReadable(page, QUrl(QStringLiteral("https://reader.example.com/a/b")));
    check(readable.title == QLatin1String("Example Article"), "reader title");
    check(readable.html.contains(QStringLiteral("First paragraph")) && readable.html.contains(QStringLiteral("Second paragraph")) &&
              readable.html.contains(QStringLiteral("A quote")),
          "reader keeps the main text", readable.html);
    check(!readable.html.contains(QStringLiteral("navigation")) && !readable.html.contains(QStringLiteral("Sidebar")) &&
              !readable.html.contains(QStringLiteral("Footer")) && !readable.html.contains(QStringLiteral("Subscribe")),
          "reader drops nav, aside, footer and forms", readable.html);
    check(readable.html.contains(QStringLiteral("https://reader.example.com/picture.png")), "reader resolves image URLs");
    const QString articlePage = QStringLiteral(
        "<body><div><p>A short teaser paragraph that is not the article.</p><p>Another teaser.</p></div><article><p>%1</p></article></body>")
                                    .arg(QString(30, QLatin1Char('w')) + QStringLiteral(" long article text ") + QString(220, QLatin1Char('x')));
    const Readable preferred = extractReadable(articlePage, QUrl(QStringLiteral("https://reader.example.com/")));
    check(preferred.html.contains(QStringLiteral("long article text")) && !preferred.html.contains(QStringLiteral("teaser")),
          "reader prefers <article>", preferred.html);
}

void testFetch()
{
    FakeNetwork network;
    FetchQueue queue(&network);
    network.route(QStringLiteral("https://cond.example.com/feed.xml"), 200, kRss, "application/rss+xml",
                  {{"ETag", "\"v1\""}, {"Last-Modified", "Tue, 14 Oct 2026 14:20:00 GMT"}});
    FetchRequest request;
    request.url = QUrl(QStringLiteral("https://cond.example.com/feed.xml"));
    FetchResult result = fetchNow(queue, request);
    check(result.ok() && result.status == 200 && result.body == kRss && result.etag == QLatin1String("\"v1\""),
          "fetch returns body and validators", result.error);
    request.etag = result.etag;
    request.lastModified = result.lastModified;
    result = fetchNow(queue, request);
    check(result.ok() && result.notModified, "conditional GET answers 304");

    network.route(QStringLiteral("https://moved.example.com/old.xml"), 301, {}, "text/html", {{"Location", "/new.xml"}});
    network.route(QStringLiteral("https://moved.example.com/new.xml"), 200, kAtom, "application/atom+xml");
    network.route(QStringLiteral("https://temp.example.com/a.xml"), 302, {}, "text/html", {{"Location", "https://moved.example.com/new.xml"}});
    network.route(QStringLiteral("https://down.example.com/a.xml"), 301, {}, "text/html", {{"Location", "http://down.example.com/a.xml"}});
    result = fetchNow(queue, {QUrl(QStringLiteral("https://moved.example.com/old.xml")), {}, {}, kMaxFetchBytes, {}});
    check(result.ok() && result.permanentRedirect && result.finalUrl.toString() == QLatin1String("https://moved.example.com/new.xml"),
          "permanent redirect reported", result.error);
    result = fetchNow(queue, {QUrl(QStringLiteral("https://temp.example.com/a.xml")), {}, {}, kMaxFetchBytes, {}});
    check(result.ok() && !result.permanentRedirect, "temporary redirect is not permanent");
    result = fetchNow(queue, {QUrl(QStringLiteral("https://down.example.com/a.xml")), {}, {}, kMaxFetchBytes, {}});
    check(!result.ok() && result.error.contains(QStringLiteral("less safe")), "https to http redirect refused", result.error);
    network.route(QStringLiteral("https://big.example.com/feed"), 200, QByteArray(int(kMaxFetchBytes) + 1, 'x'));
    result = fetchNow(queue, {QUrl(QStringLiteral("https://big.example.com/feed")), {}, {}, kMaxFetchBytes, {}});
    check(!result.ok() && result.error.contains(QStringLiteral("larger than")), "2 MB cap", result.error);
    result = fetchNow(queue, {QUrl(QStringLiteral("https://missing.example.com/feed")), {}, {}, kMaxFetchBytes, {}});
    check(!result.ok() && result.error.contains(QStringLiteral("404")), "HTTP errors reported", result.error);
    result = fetchNow(queue, {QUrl(QStringLiteral("file:///etc/passwd")), {}, {}, kMaxFetchBytes, {}});
    check(!result.ok(), "non-http addresses refused");

    network.route(QStringLiteral("https://site.example.com/"), 200,
                  "<html><head><link rel=\"alternate\" type=\"application/feed+json\" href=\"/feed.json\"></head></html>", "text/html");
    network.route(QStringLiteral("https://site.example.com/feed.json"), 200, kJson, "application/feed+json");
    DiscoveryResult discovered;
    bool done = false;
    discoverFeed(&queue, QUrl(QStringLiteral("https://site.example.com/")), [&](const DiscoveryResult &value) {
        discovered = value;
        done = true;
    });
    waitFor([&] { return done; });
    check(discovered.ok && discovered.feedUrl.toString() == QLatin1String("https://site.example.com/feed.json") &&
              discovered.feed.title == QLatin1String("Example Podcast"),
          "discovery follows <link rel=alternate>", discovered.error);
    network.route(QStringLiteral("https://plain.example.com/page"), 200, "<html><body>No links</body></html>", "text/html");
    network.route(QStringLiteral("https://plain.example.com/rss"), 200, kRdf);
    done = false;
    discoverFeed(&queue, QUrl(QStringLiteral("https://plain.example.com/page")), [&](const DiscoveryResult &value) {
        discovered = value;
        done = true;
    });
    waitFor([&] { return done; });
    check(discovered.ok && discovered.feedUrl.toString() == QLatin1String("https://plain.example.com/rss"),
          "discovery falls back to common feed paths", discovered.error);
    done = false;
    discoverFeed(&queue, QUrl(QStringLiteral("https://nothing.example.com/")), [&](const DiscoveryResult &value) {
        discovered = value;
        done = true;
    });
    waitFor([&] { return done; });
    check(!discovered.ok && discovered.error.contains(QStringLiteral("No feed")), "discovery reports no feed", discovered.error);

    network.peak = 0;
    int finished = 0;
    for (int i = 0; i < 14; ++i) {
        network.route(QStringLiteral("https://many.example.com/%1").arg(i), 200, kRdf);
        queue.get({QUrl(QStringLiteral("https://many.example.com/%1").arg(i)), {}, {}, kMaxFetchBytes, {}},
                  [&](const FetchResult &) { ++finished; });
    }
    waitFor([&] { return finished == 14; });
    check(finished == 14 && network.peak <= kMaxConcurrentFetches && network.peak > 1, "about six fetches at a time",
          QString::number(network.peak));
}

const QByteArray kYouTube = R"XML(<?xml version="1.0" encoding="UTF-8"?>
<feed xmlns:yt="http://www.youtube.com/xml/schemas/2015" xmlns:media="http://search.yahoo.com/mrss/" xmlns="http://www.w3.org/2005/Atom">
 <link rel="self" href="http://www.youtube.com/feeds/videos.xml?channel_id=UCuAXFkgsw1L7xaCfnd5JJOw"/>
 <id>yt:channel:uAXFkgsw1L7xaCfnd5JJOw</id>
 <yt:channelId>uAXFkgsw1L7xaCfnd5JJOw</yt:channelId>
 <title>Rick Astley</title>
 <link rel="alternate" href="https://www.youtube.com/channel/UCuAXFkgsw1L7xaCfnd5JJOw"/>
 <author>
  <name>Rick Astley</name>
  <uri>https://www.youtube.com/channel/UCuAXFkgsw1L7xaCfnd5JJOw</uri>
 </author>
 <published>2015-02-01T15:57:40+00:00</published>
 <entry>
  <id>yt:video:dQw4w9WgXcQ</id>
  <yt:videoId>dQw4w9WgXcQ</yt:videoId>
  <yt:channelId>UCuAXFkgsw1L7xaCfnd5JJOw</yt:channelId>
  <title>Rick Astley - Never Gonna Give You Up (Official Video)</title>
  <link rel="alternate" href="https://www.youtube.com/watch?v=dQw4w9WgXcQ"/>
  <author>
   <name>Rick Astley</name>
   <uri>https://www.youtube.com/channel/UCuAXFkgsw1L7xaCfnd5JJOw</uri>
  </author>
  <published>2009-10-25T06:57:33+00:00</published>
  <updated>2026-10-12T18:01:40+00:00</updated>
  <media:group>
   <media:title>Rick Astley - Never Gonna Give You Up (Official Video)</media:title>
   <media:content url="https://www.youtube.com/v/dQw4w9WgXcQ?version=3" type="application/x-shockwave-flash" width="640" height="390"/>
   <media:thumbnail url="https://i2.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg" width="480" height="360"/>
   <media:description>The official video for “Never Gonna Give You Up” by Rick Astley.

Listen to Rick Astley: https://RickAstley.lnk.to/_listenYD
Subscribe &amp; enjoy</media:description>
   <media:community>
    <media:starRating count="18740339" average="5.00" min="1" max="5"/>
    <media:statistics views="1650000000"/>
   </media:community>
  </media:group>
 </entry>
</feed>)XML";

const QByteArray kMastodon = R"XML(<?xml version="1.0" encoding="UTF-8"?>
<rss version="2.0" xmlns:webfeeds="http://webfeeds.org/rss/1.0" xmlns:media="http://search.yahoo.com/mrss/">
 <channel>
  <title>Example User</title>
  <description>Public posts from @user@mastodon.example</description>
  <link>https://mastodon.example/@user</link>
  <image><url>https://files.mastodon.example/accounts/avatars/original/avatar.png</url><title>Example User</title><link>https://mastodon.example/@user</link></image>
  <lastBuildDate>Wed, 15 Oct 2026 10:00:00 +0000</lastBuildDate>
  <webfeeds:icon>https://files.mastodon.example/accounts/avatars/original/avatar.png</webfeeds:icon>
  <generator>Mastodon v4.3.0</generator>
  <item>
   <guid isPermaLink="true">https://mastodon.example/@user/113300000000000001</guid>
   <link>https://mastodon.example/@user/113300000000000001</link>
   <pubDate>Wed, 15 Oct 2026 10:00:00 +0000</pubDate>
   <description>&lt;p&gt;Sunset over the harbour&lt;/p&gt;</description>
   <media:content url="https://files.mastodon.example/media_attachments/files/113/original/sunset.jpg" type="image/jpeg" fileSize="183456" medium="image">
    <media:rating scheme="urn:simple">nonadult</media:rating>
    <media:description type="plain">A red sun sets behind the cranes</media:description>
   </media:content>
   <category>photography</category>
  </item>
 </channel>
</rss>)XML";

QByteArray rssItems(const QString &channel, const QList<QStringList> &items, const QString &extra = QString())
{
    QString xml = QStringLiteral("<rss version=\"2.0\" xmlns:media=\"http://search.yahoo.com/mrss/\"><channel><title>%1</title>%2").arg(channel, extra);
    for (const QStringList &item : items)
        xml += QStringLiteral("<item><title>%1</title><link>%2</link><guid>%3</guid><description>%4</description>%5</item>")
                   .arg(item.value(0), item.value(1), item.value(2), item.value(3).toHtmlEscaped(), item.value(4));
    xml += QStringLiteral("</channel></rss>");
    return xml.toUtf8();
}

QByteArray pngBytes()
{
    QImage image(8, 6, QImage::Format_ARGB32);
    image.fill(Qt::red);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

void testMediaRss()
{
    const ParsedFeed youtube = parseFeed(kYouTube, QUrl(QStringLiteral("https://www.youtube.com/feeds/videos.xml?channel_id=UCuAXFkgsw1L7xaCfnd5JJOw")));
    check(youtube.ok && youtube.items.size() == 1 && youtube.format == QLatin1String("Atom 1.0"), "YouTube channel feed parses", youtube.error);
    if (youtube.items.size() == 1) {
        const ParsedItem &video = youtube.items.first();
        check(video.title == QLatin1String("Rick Astley - Never Gonna Give You Up (Official Video)"), "YouTube entry title", video.title);
        check(video.thumbnails == QStringList{QStringLiteral("https://i2.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg")},
              "media:thumbnail inside media:group is an image", video.thumbnails.join(QLatin1Char(',')));
        check(video.mediaImages.isEmpty(), "a Flash media:content is not an image");
        check(video.html.startsWith(QStringLiteral("<p>The official video for")) &&
                  video.html.contains(QStringLiteral("</p><p>Listen to Rick Astley: <a href=\"https://RickAstley.lnk.to/_listenYD\">"
                                                     "https://RickAstley.lnk.to/_listenYD</a><br>Subscribe &amp; enjoy</p>")),
              "plain media:description becomes paragraphs, line breaks and links", video.html);
        Store store;
        QString error;
        const QString id = store.addFeed(QUrl(QStringLiteral("https://www.youtube.com/feeds/videos.xml?channel_id=UCuAXFkgsw1L7xaCfnd5JJOw")),
                                         QString(), QString(), utc(2026, 10, 15), &error);
        store.merge(id, youtube, utc(2026, 10, 15));
        const Article *stored = store.article({id, QStringLiteral("yt:video:dQw4w9WgXcQ")});
        check(stored && stored->image == QLatin1String("https://i2.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg") &&
                  stored->images.size() == 1 && stored->html.contains(QStringLiteral("<a href=\"https://rickastley.lnk.to/_listenYD\">")),
              "YouTube article keeps the thumbnail as its image and the description as its text");
        if (stored) {
            const QString lead = leadImagesHtml(stored->images, stored->html);
            check(lead == QLatin1String("<p><img src=\"https://i2.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg\"></p>"),
                  "the thumbnail is drawn above the description", lead);
        }
    }
    const QByteArray titled = R"XML(<feed xmlns="http://www.w3.org/2005/Atom" xmlns:media="http://search.yahoo.com/mrss/"><entry><id>e1</id>
<media:group><media:title>Media Title</media:title><media:description type="html">&lt;p&gt;Rich &lt;b&gt;text&lt;/b&gt;&lt;/p&gt;</media:description></media:group></entry></feed>)XML";
    const ParsedFeed media = parseFeed(titled, QUrl(QStringLiteral("https://media.example.com/atom")));
    check(media.items.size() == 1 && media.items.first().title == QLatin1String("Media Title") &&
              media.items.first().html == QLatin1String("<p>Rich <b>text</b></p>"),
          "media:title fills a missing title and an html media:description is HTML",
          media.items.value(0).title + QLatin1Char('|') + media.items.value(0).html);

    const ParsedFeed mastodon = parseFeed(kMastodon, QUrl(QStringLiteral("https://mastodon.example/@user.rss")));
    check(mastodon.ok && mastodon.items.size() == 1, "Mastodon RSS parses", mastodon.error);
    if (mastodon.items.size() == 1) {
        const ParsedItem &post = mastodon.items.first();
        check(post.mediaImages == QStringList{QStringLiteral("https://files.mastodon.example/media_attachments/files/113/original/sunset.jpg")},
              "Mastodon image media:content is an image");
        check(post.html.contains(QStringLiteral("Sunset over the harbour")) && !post.html.contains(QStringLiteral("cranes")),
              "description wins over media:description", post.html);
        check(articleImage(post, sanitizeHtml(post.html, QUrl())) == post.mediaImages.first(), "Mastodon article image");
        const QString lead = leadImagesHtml(itemImages(post), sanitizeHtml(post.html, QUrl()));
        check(lead.contains(QStringLiteral("sunset.jpg")), "Mastodon image drawn at the top of the post", lead);
    }

    const QByteArray mixed = rssItems(QStringLiteral("Mixed"), {
        {QStringLiteral("Pictures"), QStringLiteral("https://pics.example.com/1"), QStringLiteral("p1"),
         QStringLiteral("<p>Inline <img src=\"https://pics.example.com/inline.png\" alt=\"Inline\"></p>"),
         QStringLiteral("<enclosure url=\"https://pics.example.com/enclosure.png\" type=\"image/png\" length=\"10\"/>"
                        "<enclosure url=\"https://pics.example.com/talk.mp3\" type=\"audio/mpeg\" length=\"20\"/>"
                        "<media:thumbnail url=\"https://pics.example.com/thumb.jpg\"/>"
                        "<media:content url=\"https://pics.example.com/content.webp\" type=\"image/webp\"/>")},
        {QStringLiteral("Inline only"), QStringLiteral("https://pics.example.com/2"), QStringLiteral("p2"),
         QStringLiteral("<p><img src=\"/relative.png\"></p>"), QString()}});
    const ParsedFeed pictures = parseFeed(mixed, QUrl(QStringLiteral("https://pics.example.com/feed")));
    if (pictures.items.size() == 2) {
        const ParsedItem &first = pictures.items.first();
        check(itemImages(first) == QStringList({QStringLiteral("https://pics.example.com/thumb.jpg"),
                                                QStringLiteral("https://pics.example.com/content.webp"),
                                                QStringLiteral("https://pics.example.com/enclosure.png")}),
              "images in order: thumbnail, image media:content, image enclosure", itemImages(first).join(QLatin1Char(',')));
        check(first.enclosures.size() == 2, "audio enclosures stay enclosures");
        const ParsedItem &second = pictures.items.at(1);
        check(articleImage(second, sanitizeHtml(second.html, QUrl(QStringLiteral("https://pics.example.com/2")))) ==
                  QLatin1String("https://pics.example.com/relative.png"),
              "the first content image is the fallback article image");
    } else {
        check(false, "mixed image feed parses", QString::number(pictures.items.size()));
    }
    const QByteArray jsonImages = R"({"version":"https://jsonfeed.org/version/1","items":[{"id":"1","content_html":"<img src=\"https://j.example.com/in.png\">",
        "image":"https://j.example.com/image.png","banner_image":"https://j.example.com/banner.png"},
        {"id":"2","content_text":"x","banner_image":"https://j.example.com/banner2.png"}]})";
    const ParsedFeed json = parseFeed(jsonImages, QUrl(QStringLiteral("https://j.example.com/feed.json")));
    check(json.format == QLatin1String("JSON Feed 1") && json.items.size() == 2 &&
              articleImage(json.items.at(0), QString()) == QLatin1String("https://j.example.com/image.png") &&
              articleImage(json.items.at(1), QString()) == QLatin1String("https://j.example.com/banner2.png"),
          "JSON Feed image, then banner_image", json.format);
    check(leadImagesHtml({QStringLiteral("https://x.example.com/a.png?x=1&y=2")},
                         QStringLiteral("<img src=\"https://x.example.com/a.png?x=1&amp;y=2\">")).isEmpty(),
          "an image already in the content is not repeated");
    check(plainTextToHtml(QStringLiteral("See https://a.example.com/x.")) ==
              QLatin1String("<p>See <a href=\"https://a.example.com/x\">https://a.example.com/x</a>.</p>"),
          "trailing punctuation is not part of a link", plainTextToHtml(QStringLiteral("See https://a.example.com/x.")));
}

void testArticleWins()
{
    const QUrl base(QStringLiteral("https://example.com/post"));
    const QString ignored = sanitizeHtml(QStringLiteral(
        "<p>Keep</p><div class=\"share instapaper_ignore\"><p>Share this</p><div>nested</div></div><p>After</p>"
        "<span class=\"instapaper_ignored\">kept</span><img class=\"instapaper_ignore\" src=\"https://example.com/a.png\">"), base);
    check(ignored == QLatin1String("<p>Keep</p><p>After</p><span>kept</span>"), "instapaper_ignore drops the element and its content", ignored);

    const QString captioned = addAltCaptions(QStringLiteral("<p><img src=\"https://i.example.com/a.png\" alt=\"A cat &amp; a dog\"></p>"));
    check(captioned == QLatin1String("<p><img src=\"https://i.example.com/a.png\" alt=\"A cat &amp; a dog\"><br>"
                                     "<span class=\"caption\">A cat &amp; a dog</span></p>"),
          "alt text becomes a caption", captioned);
    const QString figure = QStringLiteral("<figure><img src=\"https://i.example.com/b.png\" alt=\"Dog\"><figcaption>A dog</figcaption></figure>");
    check(!addAltCaptions(figure).contains(QStringLiteral("caption\">")), "no caption inside a figure with a figcaption", addAltCaptions(figure));
    check(addAltCaptions(QStringLiteral("<figure><img src=\"https://i.example.com/c.png\" alt=\"Bird\"></figure>")).contains(QStringLiteral("<span class=\"caption\">Bird</span>")),
          "caption inside a figure without a figcaption");
    check(!addAltCaptions(QStringLiteral("<img src=\"https://i.example.com/d.png\" alt=\" \">")).contains(QStringLiteral("caption")),
          "no caption for an empty alt");

    check(textDirection(QStringLiteral("ar")) == QLatin1String("rtl") && textDirection(QStringLiteral("he-IL")) == QLatin1String("rtl") &&
              textDirection(QStringLiteral("iw")) == QLatin1String("rtl") && textDirection(QStringLiteral("fa_IR")) == QLatin1String("rtl") &&
              textDirection(QStringLiteral("ckb")) == QLatin1String("rtl") && textDirection(QStringLiteral("SYR")) == QLatin1String("rtl") &&
              textDirection(QStringLiteral("ur")) == QLatin1String("rtl") && textDirection(QStringLiteral("dv")) == QLatin1String("rtl"),
          "right-to-left languages");
    check(textDirection(QStringLiteral("en-US")) == QLatin1String("auto") && textDirection(QString()) == QLatin1String("auto") &&
              textDirection(QStringLiteral("arn")) == QLatin1String("auto"),
          "other languages are auto");
    const ParsedFeed rss = parseFeed(rssItems(QStringLiteral("Arabic"), {{QStringLiteral("خبر"), QStringLiteral("https://ar.example.com/1"), QStringLiteral("a1"), QStringLiteral("نص")}},
                                              QStringLiteral("<language>ar-EG</language>")), QUrl());
    check(rss.language == QLatin1String("ar-EG"), "RSS language", rss.language);
    const QByteArray atom = R"XML(<feed xmlns="http://www.w3.org/2005/Atom" xml:lang="he"><title>Hebrew</title>
<entry xml:lang="en"><id>1</id><title>English entry</title></entry><entry><id>2</id><title>כותרת</title></entry></feed>)XML";
    const ParsedFeed hebrew = parseFeed(atom, QUrl(QStringLiteral("https://he.example.com/atom")));
    check(hebrew.language == QLatin1String("he") && hebrew.items.size() == 2 && hebrew.items.at(0).language == QLatin1String("en") &&
              hebrew.items.at(1).language.isEmpty(),
          "Atom xml:lang on the feed and the entry");
    const ParsedFeed rdf = parseFeed(R"XML(<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#" xmlns="http://purl.org/rss/1.0/" xmlns:dc="http://purl.org/dc/elements/1.1/">
<channel rdf:about="https://x.example/"><title>X</title><dc:language>fa</dc:language></channel></rdf:RDF>)XML", QUrl());
    check(rdf.language == QLatin1String("fa"), "RSS 1.0 dc:language", rdf.language);
    const ParsedFeed json = parseFeed(R"({"version":"https://jsonfeed.org/version/1.1","language":"ur","items":[{"id":"1","content_text":"x","language":"en"}]})", QUrl());
    check(json.language == QLatin1String("ur") && json.items.value(0).language == QLatin1String("en"), "JSON Feed language");
    Store store;
    QString error;
    const QString id = store.addFeed(QUrl(QStringLiteral("https://he.example.com/atom")), QString(), QString(), 1000, &error);
    store.merge(id, hebrew, 1000);
    check(store.feed(id)->language == QLatin1String("he") && store.article({id, QStringLiteral("1")})->language == QLatin1String("en") &&
              store.article({id, QStringLiteral("2")})->language.isEmpty(),
          "feed language stored, article language only when it differs");

    check(readingMinutes(QString()) == 0 && readingMinutes(QStringLiteral("  \n ")) == 0, "no reading time for empty text");
    const auto words = [](int count) {
        QStringList list;
        for (int i = 0; i < count; ++i)
            list << QStringLiteral("word");
        return list.join(QLatin1Char(' '));
    };
    check(readingMinutes(QStringLiteral("one")) == 1 && readingMinutes(words(230)) == 1 && readingMinutes(words(231)) == 2 &&
              readingMinutes(words(460)) == 2,
          "230 words a minute, rounded up, at least one");
    check(readingMinutes(QString(500, QChar(0x6F22))) == 1 && readingMinutes(QString(501, QChar(0x3042))) == 2 &&
              readingMinutes(words(230) + QLatin1Char(' ') + QString(500, QChar(0xD55C))) == 2,
          "CJK characters counted separately at 500 a minute");
    check(readingMinutes(QStringLiteral("abc東京def")) == 1 && readingMinutes(words(228) + QStringLiteral(" abc東京def")) == 1,
          "CJK characters do not split a word");
}

void testFiltersAndDuplicates()
{
    const qint64 at = utc(2026, 10, 15, 12, 0);
    Store store;
    QString error;
    const QString a = store.addFeed(QUrl(QStringLiteral("https://engineering.example.com/feed.xml")), QString(), QString(), at, &error);
    const QString b = store.addFeed(QUrl(QStringLiteral("https://bulk.example.com/feed")), QString(), QString(), at, &error);
    store.merge(a, parseFeed(kRss, QUrl(QStringLiteral("https://engineering.example.com/feed.xml"))), at);
    store.merge(b, parseFeed(rssWithItems(3, 0, QStringLiteral("Bulk")), QUrl()), at);
    const Source feedA{SourceKind::Feed, a};
    check(store.feedUnread(a) == 3 && store.totalUnread() == 6, "six unread before filters");

    ContentFilter hide;
    hide.pattern = QStringLiteral("SHIPPING");
    check(store.saveFilter(hide, &error) && store.settings().filters.size() == 1 && !store.settings().filters.first().id.isEmpty(),
          "filter saved with an id", error);
    check(store.query(feedA, QString(), false, at).size() == 2 && store.feedUnread(a) == 2 && store.totalUnread() == 5 &&
              store.unreadCount({SourceKind::Today, QString()}, at) == 1,
          "a plain filter hides case-insensitively from timelines and counts",
          QString::number(store.unreadCount({SourceKind::Today, QString()}, at)));
    check(store.article({a, QStringLiteral("post-1")}) != nullptr, "hidden articles stay stored");
    store.setStarred({a, QStringLiteral("post-1")}, true);
    check(store.query(feedA, QString(), false, at).size() == 3 && store.feedUnread(a) == 3, "starred articles are never hidden");
    store.setStarred({a, QStringLiteral("post-1")}, false);
    check(store.feedUnread(a) == 2, "unstarring hides it again");

    ContentFilter bad;
    bad.pattern = QStringLiteral("([");
    bad.regex = true;
    check(!store.saveFilter(bad, &error) && error == QLatin1String("Invalid regular expression"), "invalid regex refused", error);
    check(filterPatternError(QStringLiteral("(["), true) == QLatin1String("Invalid regular expression") &&
              filterPatternError(QStringLiteral("(["), false).isEmpty() && !filterPatternError(QStringLiteral(" "), false).isEmpty(),
          "editor validation messages");
    check(!TextMatcher(QStringLiteral("(["), true).matches(QStringLiteral("([")), "an invalid regex never matches");
    store.settings().filters.append(ContentFilter::fromJson(QJsonObject{{QStringLiteral("pattern"), QStringLiteral("([")},
                                                                       {QStringLiteral("regex"), true}}));
    store.filtersChanged();
    check(store.query(feedA, QString(), false, at).size() == 2, "a stored invalid regex hides nothing");

    ContentFilter regex;
    regex.pattern = QStringLiteral("^bulk\\s+[12]$");
    regex.regex = true;
    regex.feedId = b;
    check(store.saveFilter(regex, &error), "regex filter saved", error);
    check(store.query({SourceKind::Feed, b}, QString(), false, at).size() == 1 && store.feedUnread(b) == 1,
          "regex filter, case-insensitive, scoped to one feed");
    ContentFilter body;
    body.pattern = QStringLiteral("nothing else");
    body.field = QStringLiteral("titleOrText");
    store.saveFilter(body, &error);
    ContentFilter author;
    author.pattern = QStringLiteral("sam example");
    author.field = QStringLiteral("author");
    store.saveFilter(author, &error);
    check(store.query(feedA, QString(), false, at).isEmpty() && store.feedUnread(a) == 0, "title-or-text and author filters");
    store.removeFilter(author.id.isEmpty() ? store.settings().filters.last().id : author.id);
    check(store.query(feedA, QString(), false, at).size() == 1, "removing a filter brings its articles back");

    const QString c = store.addFeed(QUrl(QStringLiteral("https://arrive.example.com/feed")), QString(), QString(), at, &error);
    ContentFilter arrive;
    arrive.pattern = QStringLiteral("Arrive 1");
    arrive.action = QStringLiteral("markRead");
    store.saveFilter(arrive, &error);
    store.merge(c, parseFeed(rssWithItems(3, 0, QStringLiteral("Arrive")), QUrl()), at);
    check(store.article({c, QStringLiteral("Arrive-1")})->read && !store.article({c, QStringLiteral("Arrive-0")})->read &&
              store.query({SourceKind::Feed, c}, QString(), false, at).size() == 3,
          "mark-as-read filters apply to arriving articles and do not hide them");
    ContentFilter later;
    later.pattern = QStringLiteral("Arrive 2");
    later.action = QStringLiteral("markRead");
    store.saveFilter(later, &error);
    store.merge(c, parseFeed(rssWithItems(3, 0, QStringLiteral("Arrive")), QUrl()), at);
    check(!store.article({c, QStringLiteral("Arrive-2")})->read, "mark-as-read filters leave stored articles alone");
    while (store.settings().filters.size() < kMaxFilters) {
        ContentFilter filler;
        filler.pattern = QStringLiteral("filler %1").arg(store.settings().filters.size());
        store.saveFilter(filler, &error);
    }
    ContentFilter extra;
    extra.pattern = QStringLiteral("one too many");
    check(!store.saveFilter(extra, &error) && store.settings().filters.size() == kMaxFilters, "at most 100 filters", error);

    check(linkKey(QStringLiteral("http://Dup.Example.com/post?id=1#top")) == linkKey(QStringLiteral("https://dup.example.com/post?id=1")) &&
              linkKey(QStringLiteral("https://dup.example.com/post?id=2")) != linkKey(QStringLiteral("https://dup.example.com/post?id=1")) &&
              linkKey(QStringLiteral("not a link")).isEmpty(),
          "link identity ignores scheme, case and fragment");
    for (const bool fold : {true, false}) {
        Store dupes;
        dupes.settings().foldDuplicates = fold;
        const QString one = dupes.addFeed(QUrl(QStringLiteral("https://one.example.com/feed")), QString(), QString(), at, &error);
        const QString two = dupes.addFeed(QUrl(QStringLiteral("https://two.example.com/feed")), QString(), QString(), at, &error);
        dupes.merge(one, parseFeed(rssItems(QStringLiteral("One"), {{QStringLiteral("Story"), QStringLiteral("https://news.example.com/story?id=7#top"), QStringLiteral("s1"), QStringLiteral("x")}}), QUrl()), at);
        dupes.merge(two, parseFeed(rssItems(QStringLiteral("Two"), {{QStringLiteral("Same story"), QStringLiteral("http://NEWS.example.com/story?id=7"), QStringLiteral("t1"), QStringLiteral("x")},
                                                                    {QStringLiteral("Other"), QStringLiteral("https://news.example.com/story?id=8"), QStringLiteral("t2"), QStringLiteral("x")}}), QUrl()), at);
        check(dupes.article({two, QStringLiteral("t1")})->read == fold && !dupes.article({two, QStringLiteral("t2")})->read &&
                  !dupes.article({one, QStringLiteral("s1")})->read,
              fold ? "a duplicate link from another feed arrives read" : "duplicates stay unread when the setting is off");
    }
}

void testSmartFeedsAndReadLater()
{
    const qint64 at = utc(2026, 10, 15, 12, 0);
    Store store;
    QString error;
    store.addFolder(QStringLiteral("Tech"), &error);
    const QString a = store.addFeed(QUrl(QStringLiteral("https://engineering.example.com/feed.xml")), QString(), QStringLiteral("Tech"), at, &error);
    const QString b = store.addFeed(QUrl(QStringLiteral("https://bulk.example.com/feed")), QString(), QString(), at, &error);
    store.merge(a, parseFeed(kRss, QUrl(QStringLiteral("https://engineering.example.com/feed.xml"))), at);
    store.merge(b, parseFeed(rssWithItems(5, 0, QStringLiteral("Bulk")), QUrl()), at);

    SmartFeed unnamed;
    check(!store.saveSmartFeed(unnamed, &error) && error == QLatin1String("Enter a name."), "a smart feed needs a name");
    SmartFeed invalid;
    invalid.name = QStringLiteral("Broken");
    invalid.words = QStringLiteral("([");
    invalid.regex = true;
    check(!store.saveSmartFeed(invalid, &error) && error == QLatin1String("Invalid regular expression"), "invalid smart feed regex refused");
    SmartFeed builds;
    builds.name = QStringLiteral("  Builds ");
    builds.words = QStringLiteral("BUILD");
    check(store.saveSmartFeed(builds, &error) && store.settings().smartFeeds.size() == 1 &&
              store.settings().smartFeeds.first().name == QLatin1String("Builds"),
          "smart feed saved", error);
    const QString buildsId = store.settings().smartFeeds.first().id;
    const Source smart{SourceKind::Smart, buildsId};
    check(store.query(smart, QString(), false, at).size() == 1 && store.unreadCount(smart, at) == 1,
          "words match the title, case-insensitively", QString::number(store.query(smart, QString(), false, at).size()));
    SmartFeed text;
    text.name = QStringLiteral("Text");
    text.words = QStringLiteral("full TEXT here");
    store.saveSmartFeed(text, &error);
    check(store.query({SourceKind::Smart, store.settings().smartFeeds.last().id}, QString(), false, at).size() == 1,
          "words match the article text");
    store.removeSmartFeed(store.settings().smartFeeds.last().id);
    SmartFeed edited = *store.smartFeed(buildsId);
    edited.words.clear();
    edited.scope = QStringLiteral("folder:Tech");
    store.saveSmartFeed(edited, &error);
    check(store.settings().smartFeeds.size() == 1 && store.query(smart, QString(), false, at).size() == 3, "folder scope, any words");
    store.renameFolder(QStringLiteral("Tech"), QStringLiteral("Technology"), &error);
    check(store.smartFeed(buildsId)->scope == QLatin1String("folder:Technology") && store.query(smart, QString(), false, at).size() == 3,
          "renaming a folder keeps the smart feed scope");
    edited = *store.smartFeed(buildsId);
    edited.scope = QStringLiteral("feed:") + b;
    edited.maxAgeDays = 3;
    store.saveSmartFeed(edited, &error);
    check(store.query(smart, QString(), false, at).isEmpty(), "max age excludes old articles");
    edited.scope.clear();
    store.saveSmartFeed(edited, &error);
    check(store.query(smart, QString(), false, at).size() == 3, "max age keeps recent ones in every feed");
    edited.unreadOnly = true;
    store.saveSmartFeed(edited, &error);
    store.setRead({a, QStringLiteral("post-1")}, true);
    check(store.query(smart, QString(), false, at).size() == 2 &&
              store.query(smart, QString(), false, at, {articleKey({a, QStringLiteral("post-1")})}).size() == 3,
          "unread only, with articles kept while the timeline is open");
    edited.unreadOnly = false;
    edited.starredOnly = true;
    store.saveSmartFeed(edited, &error);
    store.setStarred({a, QStringLiteral("post-1")}, true);
    check(store.query(smart, QString(), false, at).size() == 1 && store.unreadCount(smart, at) == 0, "starred only; the count is unread");
    edited.starredOnly = false;
    store.saveSmartFeed(edited, &error);
    store.setFeedFlags(a, false, false, true);
    check(store.unreadCount(smart, at) == 0 && store.query(smart, QString(), false, at).size() == 3,
          "Hide from Unread Counts leaves smart feed counts but not the timeline");
    store.setFeedFlags(a, false, false, false);
    ContentFilter hide;
    hide.pattern = QStringLiteral("no guid");
    store.saveFilter(hide, &error);
    check(store.query(smart, QString(), false, at).size() == 2, "filter-hidden articles are left out of smart feeds");
    while (store.settings().smartFeeds.size() < kMaxSmartFeeds) {
        SmartFeed filler;
        filler.name = QStringLiteral("Smart %1").arg(store.settings().smartFeeds.size());
        store.saveSmartFeed(filler, &error);
    }
    SmartFeed extra;
    extra.name = QStringLiteral("Too many");
    check(!store.saveSmartFeed(extra, &error) && store.settings().smartFeeds.size() == kMaxSmartFeeds, "at most 50 smart feeds");
    check(store.removeSmartFeed(buildsId) && !store.smartFeed(buildsId) && store.query(smart, QString(), false, at).isEmpty(),
          "smart feed deleted");

    const ArticleRef keep{b, QStringLiteral("Bulk-0")};
    check(store.readLaterCount() == 0 && store.setReadLater(keep, true) && store.readLaterCount() == 1, "Read Later toggled");
    const Source readLater{SourceKind::ReadLater, QString()};
    check(store.query(readLater, QString(), false, at).size() == 1 && store.unreadCount(readLater, at) == 1, "Read Later lists its articles");
    store.setRead(keep, true);
    check(store.article(keep)->readLater && store.unreadCount(readLater, at) == 0 && store.query(readLater, QString(), false, at).size() == 1,
          "reading does not remove an article from Read Later");
    store.merge(b, parseFeed(rssWithItems(200, 10, QStringLiteral("B")), QUrl()), at);
    store.merge(b, parseFeed(rssWithItems(200, 20, QStringLiteral("C")), QUrl()), at);
    check(store.article(keep) && !store.article({b, QStringLiteral("Bulk-1")}), "Read Later articles are kept by pruning");
    check(hideReadApplies({SourceKind::Feed, b}) && hideReadApplies(readLater) && hideReadApplies({SourceKind::Today, QString()}) &&
              !hideReadApplies({SourceKind::Starred, QString()}),
          "Hide Read Articles applies everywhere but Starred");
}

void testFeedSwitches()
{
    const qint64 at = utc(2026, 10, 15, 12, 0);
    Store store;
    QString error;
    store.addFolder(QStringLiteral("Tech"), &error);
    const QString a = store.addFeed(QUrl(QStringLiteral("https://engineering.example.com/feed.xml")), QString(), QStringLiteral("Tech"), at, &error);
    const QString b = store.addFeed(QUrl(QStringLiteral("https://bulk.example.com/feed")), QString(), QString(), at, &error);
    store.merge(a, parseFeed(kRss, QUrl(QStringLiteral("https://engineering.example.com/feed.xml"))), at);
    store.merge(b, parseFeed(rssWithItems(5, 0, QStringLiteral("Bulk")), QUrl()), at);
    const Source today{SourceKind::Today, QString()};
    check(store.unreadCount(today, at) == 2, "Today before switches");
    store.setFeedFlags(a, true, false, false);
    check(!store.dueFeeds(at, true).contains(a) && store.dueFeeds(at, true).contains(b) && store.articles(a).size() == 3,
          "a paused feed is skipped even by a full refresh and keeps its articles");
    store.setFeedFlags(a, false, true, false);
    check(store.unreadCount(today, at) == 0 && store.query(today, QString(), false, at).isEmpty() && store.totalUnread() == 8,
          "Hide from Today");
    store.setFeedFlags(a, false, false, true);
    check(store.totalUnread() == 5 && store.unreadCount({SourceKind::AllUnread, QString()}, at) == 5 &&
              store.query({SourceKind::AllUnread, QString()}, QString(), false, at).size() == 5 &&
              store.unreadCount(today, at) == 0 && store.unreadCount({SourceKind::Folder, QStringLiteral("Tech")}, at) == 0 &&
              store.feedUnread(a) == 3 && store.query({SourceKind::Folder, QStringLiteral("Tech")}, QString(), false, at).size() == 3,
          "Hide from Unread Counts: left out of All Unread, Today, folder totals and the badge; its own count stays");
    store.setStarred({a, QStringLiteral("post-1")}, true);
    check(store.unreadCount({SourceKind::Starred, QString()}, at) == 0, "and out of the Starred count");

    check(!store.changeFeedAddress(a, QStringLiteral("ftp://engineering.example.com/feed.xml"), at, &error) && error == QLatin1String("Enter a valid feed address."),
          "invalid address refused", error);
    check(!store.changeFeedAddress(a, QStringLiteral("http://BULK.example.com/feed"), at, &error) && error.contains(QStringLiteral("already follow")),
          "an address another feed uses is refused", error);
    store.recordFailure(a, QStringLiteral("HTTP 500"), at);
    store.recordSuccess(a, at, QStringLiteral("\"tag\""), QStringLiteral("Tue"), QUrl());
    store.recordFailure(a, QStringLiteral("HTTP 500"), at);
    check(store.changeFeedAddress(a, QStringLiteral("feed://engineering.example.com/new.xml"), at, &error), "address changed", error);
    const Feed *moved = store.feed(a);
    check(moved->url == QLatin1String("https://engineering.example.com/new.xml") &&
              moved->previousUrls == QStringList{QStringLiteral("https://engineering.example.com/feed.xml")} &&
              moved->etag.isEmpty() && moved->lastModified.isEmpty() && moved->lastError.isEmpty() && moved->failures == 0 &&
              store.dueFeeds(at, false).contains(a) && store.articles(a).size() == 3,
          "the old address moves to previousUrls and the feed is due now with fresh validators");
    check(store.changeFeedAddress(a, QStringLiteral("file:///srv/feeds/local.xml"), at, &error) &&
              store.feed(a)->url == QLatin1String("file:///srv/feeds/local.xml") && store.feed(a)->previousUrls.size() == 2,
          "a file address is accepted on the desktop", error);

    QTemporaryDir directory;
    DiskStore disk(directory.filePath(QStringLiteral("rss-reader")));
    store.setStoreId(QStringLiteral("switches"));
    store.setReadLater({b, QStringLiteral("Bulk-2")}, true);
    disk.saveState(store);
    for (const Feed &owner : store.feeds())
        disk.saveArticles(store, owner.id);
    Store loaded;
    check(disk.load(loaded, QStringLiteral("switches")), "store with switches reloads");
    const Feed *reloaded = loaded.feed(a);
    check(reloaded && reloaded->excludeFromUnread && !reloaded->paused && reloaded->previousUrls.size() == 2 &&
              loaded.article({b, QStringLiteral("Bulk-2")}) && loaded.article({b, QStringLiteral("Bulk-2")})->readLater &&
              loaded.totalUnread() == store.totalUnread(),
          "feed switches, previous addresses and Read Later survive a reload");
    check(disk.storedUnread(QStringLiteral("switches")) == store.totalUnread(), "the startup badge leaves out hidden feeds",
          QString::number(disk.storedUnread(QStringLiteral("switches"))));

    const qint64 cutoff = at - 7 * kDayMs;
    check(store.olderUnreadEverywhere(cutoff, false).size() == 5, "catch up finds old unread articles everywhere",
          QString::number(store.olderUnreadEverywhere(cutoff, false).size()));
    store.setStarred({b, QStringLiteral("Bulk-0")}, true);
    check(store.olderUnreadEverywhere(cutoff, true).size() == 4, "catch up can skip starred articles");
    check(store.olderUnread({SourceKind::Feed, a}, at - kDayMs, false, at).size() == 1 &&
              store.olderUnread({SourceKind::Feed, b}, at - kDayMs, false, at).size() == 5,
          "catch up in one timeline");
    check(catchUpDays() == QList<int>({1, 3, 7, 14, 30}) && catchUpLabel(1) == QLatin1String("Older Than 1 Day") &&
              catchUpLabel(3) == QLatin1String("Older Than 3 Days") && catchUpLabel(7) == QLatin1String("Older Than 1 Week") &&
              catchUpLabel(14) == QLatin1String("Older Than 2 Weeks") && catchUpLabel(30) == QLatin1String("Older Than 1 Month"),
          "catch-up choices");

    QList<SourceEntry> entries{{QStringLiteral("beta"), QStringLiteral("b"), false, 1},
                               {QStringLiteral("Alpha"), QStringLiteral("a"), false, 1},
                               {QStringLiteral("gamma"), QStringLiteral("g"), true, 5},
                               {QStringLiteral("delta"), QStringLiteral("d"), false, 0}};
    sortSourceEntries(entries, true);
    check(entries.at(0).id == QLatin1String("g") && entries.at(1).id == QLatin1String("a") && entries.at(2).id == QLatin1String("b") &&
              entries.at(3).id == QLatin1String("d"),
          "sort by unread count, then by name");
    sortSourceEntries(entries, false);
    check(entries.at(0).id == QLatin1String("a") && entries.at(3).id == QLatin1String("g"), "name order by default");
    Feed feed;
    feed.url = QStringLiteral("https://engineering.example.com/feed.xml");
    feed.customTitle = QStringLiteral("My Engineering");
    check(feedMatchesSearch(feed, QStringLiteral("my eng")) && feedMatchesSearch(feed, QStringLiteral("EXAMPLE.com")) &&
              feedMatchesSearch(feed, QStringLiteral("feed.xml")) && feedMatchesSearch(feed, QString()) &&
              !feedMatchesSearch(feed, QStringLiteral("zzz")),
          "find a feed by title, host or address");
}

QByteArray liveFeed()
{
    const QDateTime current = QDateTime::currentDateTimeUtc();
    QByteArray xml = kRss;
    xml.replace("Tue, 14 Oct 2026 14:20:00 GMT", current.addSecs(-3600).toString(Qt::RFC2822Date).toUtf8());
    xml.replace("Mon, 13 Oct 2026 09:00:00 -0400", current.addSecs(-7200).toString(Qt::RFC2822Date).toUtf8());
    return xml;
}

QByteArray squarePng(int width, int height)
{
    QImage image(width, height, QImage::Format_ARGB32);
    image.fill(Qt::blue);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

void testIconsCommentsAndSharing()
{
    const QString html = QStringLiteral(
        "<html><head><base href=\"/static/\">"
        "<link rel=\"shortcut icon\" href=\"favicon.ico\">"
        "<link rel=\"icon\" type=\"image/png\" sizes=\"16x16\" href=\"icon-16.png\">"
        "<link rel=\"icon\" type=\"image/png\" sizes=\"32x32 192x192\" href=\"icon-multi.png\">"
        "<link rel=\"icon\" type=\"image/svg+xml\" href=\"icon.svg\">"
        "<link rel=\"apple-touch-icon-precomposed\" href=\"https://cdn.example.org/touch.png\">"
        "<link rel=\"mask-icon\" href=\"mask.svg\">"
        "<link rel=\"icon\" href=\"javascript:alert(1)\">"
        "<link rel=\"icon\" sizes=\"300x100\" href=\"wide.png\">"
        "</head><body><link rel=\"icon\" href=\"/late.png\"></body></html>");
    const QUrl page(QStringLiteral("https://site.example.com/blog/"));
    const QStringList pngFirst = discoverIconLinks(html, page, false);
    check(pngFirst == QStringList{QStringLiteral("https://site.example.com/static/icon-multi.png"),
                                  QStringLiteral("https://cdn.example.org/touch.png"),
                                  QStringLiteral("https://site.example.com/static/favicon.ico"),
                                  QStringLiteral("https://site.example.com/static/icon-16.png")},
          "icon links ranked by size and kind, resolved against <base>, unsafe and body links dropped",
          pngFirst.join(QStringLiteral(", ")));
    const QStringList withSvg = discoverIconLinks(html, page, true);
    check(withSvg.size() == kMaxIconLinks && withSvg.last() == QLatin1String("https://site.example.com/static/icon.svg"),
          "an SVG icon is a candidate only when SVG decodes, after sized PNG and ICO icons", withSvg.join(QStringLiteral(", ")));
    check(discoverIconLinks(QStringLiteral("<html><head><title>x</title></head></html>"), page, true).isEmpty(),
          "a page without icon links offers none");
    check(fallbackIconUrls(QUrl(QStringLiteral("https://www.example.com/blog")), QUrl(QStringLiteral("http://feeds.example.net:8080/rss"))) ==
              QStringList{QStringLiteral("https://www.example.com/favicon.ico"), QStringLiteral("http://feeds.example.net:8080/favicon.ico"),
                          QStringLiteral("https://www.example.com/apple-touch-icon.png"),
                          QStringLiteral("http://feeds.example.net:8080/apple-touch-icon.png")},
          "fallback icons try favicon.ico on both origins, then apple-touch-icon.png");
    check(fallbackIconUrls(QUrl(QStringLiteral("https://www.example.com/")), QUrl(QStringLiteral("https://www.example.com/feed"))).size() == 2,
          "one origin is tried once");

    Feed feed;
    feed.id = QStringLiteral("icon-feed");
    feed.url = QStringLiteral("https://feeds.example.net/rss");
    check(iconPageUrl(feed) == QUrl(QStringLiteral("https://feeds.example.net/")), "icon page falls back to the feed origin");
    feed.homePage = QStringLiteral("https://www.example.com/blog");
    check(iconPageUrl(feed) == QUrl(QStringLiteral("https://www.example.com/blog")), "icon page is the home page when known");
    Feed local;
    local.url = QUrl::fromLocalFile(QStringLiteral("/tmp/feed.xml")).toString();
    check(!iconPageUrl(local).isValid() && fallbackIconUrls(QUrl(), QUrl(local.url)).isEmpty(), "a local feed has no icon page");

    const qint64 at = utc(2026, 10, 18, 12);
    check(iconFetchDue(feed, true, at), "a feed never checked for an icon is due");
    feed.iconKey = iconSourceKey(feed);
    feed.iconCheckedMs = at;
    check(!iconFetchDue(feed, true, at + kDayMs * 3), "a feed with an icon is not refetched");
    check(!iconFetchDue(feed, false, at + kDayMs / 2), "a failed icon search waits a day");
    check(iconFetchDue(feed, false, at + kDayMs), "a failed icon search is retried after a day");
    feed.iconUrl = QStringLiteral("https://feeds.example.net/new-icon.png");
    check(iconFetchDue(feed, true, at + 1), "a changed feed icon address is refetched");

    Store store;
    QString error;
    const QString id = store.addFeed(QUrl(QStringLiteral("https://feeds.example.net/rss")), QStringLiteral("Icons"), QString(), at, &error);
    store.recordIconAttempt(id, at);
    check(store.feed(id) && store.feed(id)->iconCheckedMs == at && store.feed(id)->iconKey == iconSourceKey(*store.feed(id)),
          "an icon attempt is recorded on the feed");
    QJsonObject state = store.stateJson();
    Store reloaded;
    reloaded.loadState(state);
    check(reloaded.feed(id) && reloaded.feed(id)->iconCheckedMs == at && !iconFetchDue(*reloaded.feed(id), false, at + 1),
          "the icon attempt survives a restart");
    QJsonArray feeds = state.value(QStringLiteral("feeds")).toArray();
    QJsonObject legacy = feeds.at(0).toObject();
    legacy.remove(QStringLiteral("iconChecked"));
    legacy.remove(QStringLiteral("iconKey"));
    feeds.replace(0, legacy);
    state.insert(QStringLiteral("feeds"), feeds);
    Store older;
    older.loadState(state);
    check(older.feed(id) && !iconFetchDue(*older.feed(id), true, at) && iconFetchDue(*older.feed(id), false, at),
          "older state keeps its icons and retries feeds that have none");

    const QString hackerNews = QStringLiteral(
        "<p>Article URL: <a href=\"https://example.com/x\">https://example.com/x</a></p>"
        "<p>Comments URL: <a href=\"https://news.ycombinator.com/item?id=4242\">https://news.ycombinator.com/item?id=4242</a></p>");
    const QUrl articleBase(QStringLiteral("https://example.com/x"));
    check(htmlLinksTo(hackerNews, QStringLiteral("https://news.ycombinator.com/item?id=4242"), articleBase),
          "content that links to the comments page is detected");
    check(htmlLinksTo(QStringLiteral("<a href=\"/item?id=4242\">c</a>"), QStringLiteral("https://news.ycombinator.com/item?id=4242"),
                      QUrl(QStringLiteral("https://news.ycombinator.com/news"))),
          "a relative comments link is resolved against the article");
    check(htmlLinksTo(QStringLiteral("<a href=\"https://blog.example.com/post/1\">c</a>"), QStringLiteral("http://blog.example.com/post/1/"), QUrl()),
          "scheme and trailing slash do not make a different comments link");
    check(htmlLinksTo(QStringLiteral("<a href=\"https://x.example.com/c?id=1&amp;page=2\">c</a>"),
                      QStringLiteral("https://x.example.com/c?id=1&page=2"), QUrl()),
          "entities in the link are decoded before comparing");
    check(!htmlLinksTo(QStringLiteral("<a href=\"https://engineering.example.com/posts/build\">more</a>"),
                       QStringLiteral("https://engineering.example.com/posts/build#comments"), QUrl()),
          "a link to the article is not a link to its comments");
    check(!htmlLinksTo(QStringLiteral("<p>https://news.ycombinator.com/item?id=4242</p>"),
                       QStringLiteral("https://news.ycombinator.com/item?id=4242"), QUrl()),
          "plain text is not a comments link");

    check(shareMessage(QStringLiteral("  Shipping\n the  build "), QStringLiteral("https://x.example.com/y")) ==
              QLatin1String("Shipping the build\nhttps://x.example.com/y"),
          "a shared article is its title and link");
    check(shareMessage(QString(), QStringLiteral("https://x.example.com/y")) == QLatin1String("https://x.example.com/y") &&
              shareMessage(QStringLiteral("Title"), QString()) == QLatin1String("Title"),
          "a shared article without a title or link sends what it has");
}

void testPage(const QString &home)
{
    const auto *contribution = forkmesh::market::nativePackageContribution(QStringLiteral("builtin:rss-reader"),
                                                                           QStringLiteral("extension.native"));
    check(contribution && contribution->navigationLabel == QLatin1String("Reader") &&
              contribution->navigationIcon == QLatin1String("broadcast") && contribution->factory,
          "registered as the Reader rail page");

    FakeNetwork network;
    network.route(QStringLiteral("https://engineering.example.com/"), 200,
                  "<html><head><link rel=\"alternate\" type=\"application/rss+xml\" href=\"/feed.xml\"></head></html>", "text/html");
    network.route(QStringLiteral("https://engineering.example.com/feed.xml"), 200, liveFeed(), "application/rss+xml",
                  {{"ETag", "\"e1\""}});
    network.route(QStringLiteral("https://engineering.example.com/posts/build"), 200,
                  "<html><head><title>Build</title></head><body><nav>menu</nav><article><p>" +
                      QByteArray(40, 'r') + " reader view body text " + QByteArray(200, 'z') + "</p></article></body></html>",
                  "text/html; charset=utf-8");
    network.route(QStringLiteral("https://broken.example.com/feed"), 500, "oops", "text/plain");
    int badges = 0;
    forkmesh::market::NativePackageHost host;
    host.packageId = QStringLiteral("rss-reader");
    host.packageName = QStringLiteral("RSS Reader");
    host.network = &network;
    host.navigationBadgeChanged = [&badges] { ++badges; };

    QString storeId;
    {
        RssReaderFeature page(host);
        page.resize(1280, 800);
        check(page.dataDirectory().startsWith(home), "data lives in the per-user data folder", page.dataDirectory());
        page.refresh();
        check(page.store().feeds().isEmpty() && page.statusText() == QLatin1String("No feeds yet"), "empty reader", page.statusText());
        bool done = false;
        bool ok = false;
        QString message;
        page.subscribe(QStringLiteral("engineering.example.com"), QString(), QString(), [&](bool success, const QString &text) {
            ok = success;
            message = text;
            done = true;
        });
        waitFor([&] { return done; });
        check(ok && page.store().feeds().size() == 1, "subscribe discovers the feed from the site address", message);
        check(page.store().feeds().value(0).url == QLatin1String("https://engineering.example.com/feed.xml"), "discovered feed address stored");
        check(page.navigationBadge() == 3 && badges > 0, "rail badge is the unread count", QString::number(page.navigationBadge()));
        check(page.currentSource().kind == SourceKind::Feed && page.timelineRows() == 3, "new feed selected with its articles");
        check(page.store().unreadCount({SourceKind::Today, QString()}, QDateTime::currentMSecsSinceEpoch()) == 3,
              "Today holds articles from the last day");
        done = false;
        page.subscribe(QStringLiteral("https://engineering.example.com/feed.xml"), QString(), QString(), [&](bool success, const QString &text) {
            ok = success;
            message = text;
            done = true;
        });
        waitFor([&] { return done; });
        check(!ok && message.contains(QStringLiteral("already follow")), "duplicate subscription refused", message);

        const auto hits = page.searchHits(QStringLiteral("build"), 10);
        check(hits.size() == 1 && hits.first().title.startsWith(QStringLiteral("Shipping")), "top search finds articles");
        if (!hits.isEmpty())
            page.activateSearchHit(hits.first().id);
        check(page.currentArticle().guid == QLatin1String("post-1"), "search hit opens the article");
        const QString html = page.renderedArticleHtml();
        check(html.contains(QStringLiteral("Shipping the new build system")) && html.contains(QStringLiteral(" AT ")) &&
                  html.contains(QStringLiteral("Avery Example")) && html.contains(QStringLiteral("episode.mp3")) &&
                  html.contains(QStringLiteral(">Comments<")),
              "article shows title, date, author, enclosure and comments");
        check(!html.contains(QStringLiteral("<script")) && !html.contains(QStringLiteral("alert(1)")), "article HTML has no script");
        bool shareHidden = true;
        for (QToolButton *button : page.findChildren<QToolButton *>()) {
            if (button->property("rssIcon").toString() == QLatin1String("paper-airplane"))
                shareHidden = button->isHidden();
        }
        check(shareHidden, "Share to Chat is hidden when the host has no chat");
        check(page.navigationBadge() == 2, "opening an article marks it read");
        page.toggleStar();
        check(page.store().article(page.currentArticle())->starred, "star toggled");
        page.toggleRead();
        check(page.navigationBadge() == 3, "mark as unread");
        page.toggleRead();

        const QString lastIcon = QStringLiteral("https://engineering.example.com/apple-touch-icon.png");
        waitFor([&] { return network.requested.contains(lastIcon) && network.active == 0; });
        const qsizetype iconAt = network.requested.indexOf(QStringLiteral("https://engineering.example.com/icon.png"));
        const qsizetype faviconAt = network.requested.indexOf(QStringLiteral("https://engineering.example.com/favicon.ico"));
        check(iconAt >= 0 && faviconAt > iconAt && network.requested.indexOf(lastIcon) > faviconAt &&
                  !QFile::exists(DiskStore(page.dataDirectory()).iconPath(page.currentArticle().feedId)),
              "a site without a usable icon tries the feed icon, then favicon.ico, then apple-touch-icon.png",
              network.requested.join(QStringLiteral(" ")));
        const int before = int(network.requested.size());
        page.showReaderView();
        waitFor([&] { return page.renderedArticleHtml().contains(QStringLiteral("reader view body text")); });
        check(page.renderedArticleHtml().contains(QStringLiteral("reader view body text")) &&
                  !page.renderedArticleHtml().contains(QStringLiteral("menu")),
              "reader view extracts the original page");
        check(network.requested.size() == before + 1, "reader view fetched through the host network");

        page.nextUnread();
        const Article *next = page.store().article(page.currentArticle());
        check(next && next->guid != QLatin1String("post-1"), "next unread moves on");

        bool exported = false;
        QJsonObject payload;
        check(page.exportData([&](bool success, const QJsonObject &value, const QString &) {
                  exported = success;
                  payload = value;
              }),
              "exportData answers");
        const QJsonObject bundle = payload.value(QStringLiteral("export")).toObject();
        check(exported && payload.value(QStringLiteral("hasData")).toBool() &&
                  bundle.value(QStringLiteral("subscriptionsOpml")).toString().contains(QStringLiteral("engineering.example.com/feed.xml")) &&
                  bundle.value(QStringLiteral("starred")).toArray().size() == 1,
              "export carries OPML and starred articles");

        page.saveNow();
        QSettings settings;
        check(settings.value(QStringLiteral("market/packages/rss-reader/data/subscriptions")).toString().contains(QStringLiteral("feed.xml")),
              "subscriptions kept in the package data the host saves on removal");
        storeId = settings.value(QStringLiteral("market/packages/rss-reader/data/store")).toString();
        check(!storeId.isEmpty() && storeId == page.store().storeId(), "store id written beside the package data");
        check(QFile::exists(QDir(page.dataDirectory()).filePath(QStringLiteral("state.json"))), "state written to disk");

        const int requestsBefore = int(network.requested.size());
        page.checkFeeds(true);
        waitFor([&] { return !page.refreshing(); });
        check(network.requested.size() > requestsBefore && page.statusText().startsWith(QStringLiteral("Updated")),
              "refresh finishes with the time", page.statusText());
        check(page.store().articles(page.store().feeds().value(0).id).size() == 3, "304 keeps the articles");

        done = false;
        network.route(QStringLiteral("https://broken.example.com/"), 200,
                      "<html><head><link rel=\"alternate\" type=\"application/rss+xml\" href=\"/feed\"></head></html>", "text/html");
        network.route(QStringLiteral("https://broken.example.com/feed"), 200, kRdf);
        page.subscribe(QStringLiteral("broken.example.com"), QStringLiteral("Broken Example"), QString(),
                       [&](bool success, const QString &) {
            ok = success;
            done = true;
        });
        waitFor([&] { return done; });
        check(ok, "second feed added with a custom name");
        network.route(QStringLiteral("https://broken.example.com/feed"), 500, "oops", "text/plain");
        page.checkFeeds(true);
        waitFor([&] { return !page.refreshing(); });
        const Feed *broken = page.store().feedByUrl(QUrl(QStringLiteral("https://broken.example.com/feed")));
        check(broken && broken->failures == 1 && broken->lastError.contains(QStringLiteral("500")) &&
                  broken->displayName() == QLatin1String("Broken Example"),
              "failing feed keeps its error", broken ? broken->lastError : QString());

        QToolButton *settingsButton = nullptr;
        for (QToolButton *button : page.findChildren<QToolButton *>()) {
            if (button->toolTip() == QLatin1String("Settings"))
                settingsButton = button;
        }
        check(settingsButton != nullptr, "settings button present");
        if (settingsButton)
            settingsButton->click();
        auto *about = page.findChild<QLabel *>(QStringLiteral("rssAboutText"));
        check(about && about->text().contains(QStringLiteral(
                           "Inspired by NetNewsWire, the free and open source RSS reader created by Brent Simmons and "
                           "developed by Ranchero Software and its contributors (MIT licensed). RSS Reader shares no code with "
                           "NetNewsWire and is not affiliated with or endorsed by it.")) &&
                  about->text().contains(QStringLiteral("https://netnewswire.com/")) &&
                  about->text().contains(QStringLiteral("https://github.com/Ranchero-Software/NetNewsWire")) &&
                  about->text().contains(QStringLiteral("feature requests NetNewsWire&#39;s users have filed")) &&
                  about->text().contains(QStringLiteral("https://github.com/Ranchero-Software/NetNewsWire/issues")),
              "About credits NetNewsWire");
        if (auto *dialog = page.findChild<QDialog *>(QStringLiteral("rssSettingsDialog")))
            dialog->close();

        page.selectSource({SourceKind::AllUnread, QString()});
        page.markAllRead(false);
        check(page.navigationBadge() == 0, "mark all as read");
        const auto again = page.searchHits(QStringLiteral("Title only"), 5);
        if (!again.isEmpty())
            page.activateSearchHit(again.first().id);
        page.toggleRead();
        check(page.navigationBadge() == 1, "one article marked unread again");
        page.resize(1280, 800);
        page.show();
        QApplication::processEvents();
        const QString shot = QDir(home).filePath(QStringLiteral("rss-reader-page.png"));
        page.grab().save(shot);
        check(QFile::exists(shot), "page renders offscreen");
    }

    {
        RssReaderFeature page(host);
        page.prepareNavigationBadge();
        check(page.navigationBadge() == 1, "rail badge read from the saved counts without loading articles",
              QString::number(page.navigationBadge()));
    }

    {
        const int requestsBefore = int(network.requested.size());
        RssReaderFeature page(host);
        page.refresh();
        check(page.store().feeds().size() == 2 && page.store().storeId() == storeId, "a new window reloads the same data");
        check(page.store().query({SourceKind::Starred, QString()}, QString(), false, QDateTime::currentMSecsSinceEpoch()).size() == 1,
              "starred article reloaded");
        waitFor([&] { return !page.refreshing(); }, 1000);
        check(network.requested.size() == requestsBefore, "opening again does not refetch feeds that are not due");
    }

    {
        QSettings().setValue(QStringLiteral("market/packages/rss-reader/data/store"), QStringLiteral("imported-copy"));
        RssReaderFeature page(host);
        page.prepareSearch();
        check(page.store().feeds().size() == 2 && page.store().storeId() == QLatin1String("imported-copy"),
              "an imported saved copy rebuilds the subscriptions");
        check(page.store().query({SourceKind::Starred, QString()}, QString(), false, QDateTime::currentMSecsSinceEpoch()).size() == 1,
              "an imported saved copy restores starred articles");
        check(page.store().feedByUrl(QUrl(QStringLiteral("https://broken.example.com/feed")))->displayName() ==
                  QLatin1String("Broken Example"),
              "an imported saved copy keeps custom names");
    }
}

DiscoveryResult discoverNow(FetchQueue &queue, const QUrl &address)
{
    DiscoveryResult discovered;
    bool done = false;
    discoverFeed(&queue, address, [&](const DiscoveryResult &value) {
        discovered = value;
        done = true;
    });
    waitFor([&] { return done; });
    return discovered;
}

void testDiscoveryRewrites(const QString &home)
{
    const auto feedOf = [](const QString &address) { return discoveryRewrite(QUrl(address)).feed.toString(); };
    const auto lookup = [](const QString &address) { return discoveryRewrite(QUrl(address)).channelLookup; };
    check(feedOf(QStringLiteral("https://www.youtube.com/channel/UCuAXFkgsw1L7xaCfnd5JJOw")) ==
              QLatin1String("https://www.youtube.com/feeds/videos.xml?channel_id=UCuAXFkgsw1L7xaCfnd5JJOw"),
          "YouTube channel address becomes the channel feed");
    check(feedOf(QStringLiteral("https://m.youtube.com/channel/UCuAXFkgsw1L7xaCfnd5JJOw/videos")) ==
              QLatin1String("https://www.youtube.com/feeds/videos.xml?channel_id=UCuAXFkgsw1L7xaCfnd5JJOw"),
          "mobile YouTube channel tab");
    check(feedOf(QStringLiteral("https://youtube.com/playlist?list=PLFgquLnL59alCl_2TQvOiD5Vgm1hCaGSI")) ==
              QLatin1String("https://www.youtube.com/feeds/videos.xml?playlist_id=PLFgquLnL59alCl_2TQvOiD5Vgm1hCaGSI"),
          "YouTube playlist becomes the playlist feed");
    check(feedOf(QStringLiteral("https://www.youtube.com/user/RickAstleyVEVO")) ==
              QLatin1String("https://www.youtube.com/feeds/videos.xml?user=RickAstleyVEVO"),
          "YouTube user becomes the user feed");
    check(lookup(QStringLiteral("https://www.youtube.com/@RickAstleyYT")) && lookup(QStringLiteral("https://www.youtube.com/c/RickAstley")) &&
              lookup(QStringLiteral("https://www.youtube.com/watch?v=dQw4w9WgXcQ")) && lookup(QStringLiteral("https://youtu.be/dQw4w9WgXcQ")) &&
              lookup(QStringLiteral("https://www.youtube.com/shorts/abc123")) &&
              !discoveryRewrite(QUrl(QStringLiteral("https://www.youtube.com/@RickAstleyYT"))).feed.isValid(),
          "handles, custom names, videos and shorts need the page");
    check(!lookup(QStringLiteral("https://www.youtube.com/")) && feedOf(QStringLiteral("https://www.youtube.com/")).isEmpty() &&
              !lookup(QStringLiteral("https://www.youtube.com/watch")),
          "other YouTube pages are left to normal discovery");
    check(feedOf(QStringLiteral("https://mastodon.social/@Gargron")) == QLatin1String("https://mastodon.social/@Gargron.rss") &&
              feedOf(QStringLiteral("https://mastodon.social/@Gargron/")) == QLatin1String("https://mastodon.social/@Gargron.rss") &&
              feedOf(QStringLiteral("https://mastodon.social/@Gargron/113300000000000001")).isEmpty() &&
              feedOf(QStringLiteral("https://mastodon.social/@Gargron.rss")).isEmpty(),
          "Mastodon-style profiles try @user.rss");
    check(feedOf(QStringLiteral("https://bsky.app/profile/jay.bsky.team")) == QLatin1String("https://bsky.app/profile/jay.bsky.team/rss") &&
              feedOf(QStringLiteral("https://bsky.app/profile/jay.bsky.team/post/3k")).isEmpty(),
          "Bluesky profiles become their RSS feed");
    check(feedOf(QStringLiteral("https://example.com/blog/")).isEmpty() && !lookup(QStringLiteral("https://example.com/blog/")),
          "ordinary sites are not rewritten");
    check(youtubeChannelId(QStringLiteral("<script>var x = {\"channelId\":\"UCuAXFkgsw1L7xaCfnd5JJOw\",\"title\":\"x\"}</script>")) ==
              QLatin1String("UCuAXFkgsw1L7xaCfnd5JJOw"),
          "channel id from the page data");
    check(youtubeChannelId(QStringLiteral("<meta itemprop=\"channelId\" content=\"UCuAXFkgsw1L7xaCfnd5JJOw\">")) ==
              QLatin1String("UCuAXFkgsw1L7xaCfnd5JJOw"),
          "channel id from the meta tag");
    check(youtubeChannelId(QStringLiteral("<link rel=\"canonical\" href=\"https://www.youtube.com/channel/UCuAXFkgsw1L7xaCfnd5JJOw\">")) ==
              QLatin1String("UCuAXFkgsw1L7xaCfnd5JJOw"),
          "channel id from the canonical link");
    check(youtubeChannelId(QStringLiteral("<p>\"channelId\":\"UCshort\"</p>")).isEmpty(), "no channel id on the page");
    check(youtubeChannelFeed(QStringLiteral("UCuAXFkgsw1L7xaCfnd5JJOw")).toString() ==
              QLatin1String("https://www.youtube.com/feeds/videos.xml?channel_id=UCuAXFkgsw1L7xaCfnd5JJOw"),
          "channel feed address");

    FakeNetwork network;
    FetchQueue queue(&network);
    network.route(QStringLiteral("https://www.youtube.com/@RickAstleyYT"), 200,
                  "<html><head><link rel=\"canonical\" href=\"https://www.youtube.com/channel/UCuAXFkgsw1L7xaCfnd5JJOw\"></head>"
                  "<body><script>{\"channelId\":\"UCuAXFkgsw1L7xaCfnd5JJOw\"}</script></body></html>", "text/html");
    network.route(QStringLiteral("https://www.youtube.com/feeds/videos.xml?channel_id=UCuAXFkgsw1L7xaCfnd5JJOw"), 200, kYouTube, "application/atom+xml");
    DiscoveryResult found = discoverNow(queue, QUrl(QStringLiteral("https://www.youtube.com/@RickAstleyYT")));
    check(found.ok && found.feedUrl.toString() == QLatin1String("https://www.youtube.com/feeds/videos.xml?channel_id=UCuAXFkgsw1L7xaCfnd5JJOw"),
          "a YouTube handle is found through the channel page", found.error);
    network.route(QStringLiteral("https://www.youtube.com/@nobody"), 200, "<html><head><link rel=\"alternate\" type=\"application/rss+xml\" href=\"/alt.xml\"></head></html>", "text/html");
    network.route(QStringLiteral("https://www.youtube.com/alt.xml"), 200, kRdf);
    found = discoverNow(queue, QUrl(QStringLiteral("https://www.youtube.com/@nobody")));
    check(found.ok && found.feedUrl.toString() == QLatin1String("https://www.youtube.com/alt.xml"),
          "without a channel id the normal discovery runs", found.error);
    network.route(QStringLiteral("https://mastodon.example/@user.rss"), 200, kMastodon);
    found = discoverNow(queue, QUrl(QStringLiteral("https://mastodon.example/@user")));
    check(found.ok && found.feedUrl.toString() == QLatin1String("https://mastodon.example/@user.rss"), "Mastodon profile", found.error);
    network.route(QStringLiteral("https://blog.example.org/@writer"), 200, "<html><head><link rel=\"alternate\" type=\"application/atom+xml\" href=\"/writer.atom\"></head></html>", "text/html");
    network.route(QStringLiteral("https://blog.example.org/writer.atom"), 200, kAtom, "application/atom+xml");
    found = discoverNow(queue, QUrl(QStringLiteral("https://blog.example.org/@writer")));
    check(found.ok && found.feedUrl.toString() == QLatin1String("https://blog.example.org/writer.atom"),
          "a missing @user.rss falls back to normal discovery", found.error);
    network.route(QStringLiteral("https://bsky.app/profile/jay.bsky.team/rss"), 200, kRss);
    found = discoverNow(queue, QUrl(QStringLiteral("https://bsky.app/profile/jay.bsky.team")));
    check(found.ok && found.feedUrl.toString() == QLatin1String("https://bsky.app/profile/jay.bsky.team/rss"), "Bluesky profile", found.error);
    network.route(QStringLiteral("https://ghost.example.com/"), 200, "<html><body>No links</body></html>", "text/html");
    network.route(QStringLiteral("https://ghost.example.com/rss/"), 200, kRss);
    found = discoverNow(queue, QUrl(QStringLiteral("https://ghost.example.com/")));
    check(found.ok && found.feedUrl.toString() == QLatin1String("https://ghost.example.com/rss/"), "Ghost's /rss/ fallback", found.error);

    const QString path = QDir(home).filePath(QStringLiteral("feeds/local.xml"));
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(kRdf);
    file.close();
    const QUrl local = QUrl::fromLocalFile(path);
    check(normalizeAddress(local.toString(), true) == local && !normalizeAddress(local.toString()).isValid() &&
              !normalizeAddress(QStringLiteral("file://server/share/feed.xml"), true).isValid() &&
              !normalizeAddress(QStringLiteral("file:relative.xml"), true).isValid(),
          "file addresses: absolute paths only, and only where allowed");
    FetchRequest request;
    request.url = local;
    FetchResult result = fetchNow(queue, request);
    check(!result.ok(), "file addresses are refused unless the request allows them", result.error);
    request.allowFile = true;
    const int before = int(network.requested.size());
    result = fetchNow(queue, request);
    check(result.ok() && result.body == kRdf && network.requested.size() == before, "a file feed is read from disk, not the network", result.error);
    request.url = QUrl::fromLocalFile(QDir(home).filePath(QStringLiteral("feeds/missing.xml")));
    check(!fetchNow(queue, request).ok(), "a missing file is an error");
    const QString big = QDir(home).filePath(QStringLiteral("feeds/big.xml"));
    QFile bigFile(big);
    bigFile.open(QIODevice::WriteOnly);
    bigFile.write(QByteArray(int(kMaxFetchBytes) + 1, 'x'));
    bigFile.close();
    request.url = QUrl::fromLocalFile(big);
    result = fetchNow(queue, request);
    check(!result.ok() && result.error.contains(QStringLiteral("larger than")), "file feeds have the same size cap", result.error);
    found = discoverNow(queue, local);
    check(found.ok && found.feedUrl == local && found.feed.title == QLatin1String("Example News"), "discovery reads a file feed", found.error);
    Store store;
    QString error;
    check(!store.addFeed(local, QString(), QString(), 1000, &error).isEmpty(), "a file feed can be followed", error);
    const QByteArray opml = QStringLiteral("<opml version=\"2.0\"><body><outline text=\"Local\" xmlUrl=\"%1\"/></body></opml>").arg(local.toString()).toUtf8();
    Store fresh;
    check(fresh.importOpml(parseOpml(opml), 1000).skipped == 1 && fresh.importOpml(parseOpml(opml), 1000, true).added == 1,
          "OPML from the web cannot add file feeds; a local import can");
    check(parseOpml(writeOpml(store.feeds(), store.folders(), 1000)).feeds.value(0).xmlUrl == local.toString(), "file feeds export");
    Store html;
    const OpmlImportResult imported = html.importOpml(parseOpml(R"(<opml version="2.0"><body><outline text="Site" xmlUrl="https://site.example.com/feed" htmlUrl="https://site.example.com/"/></body></opml>)"), 1000);
    check(imported.added == 1 && html.feeds().value(0).homePage == QLatin1String("https://site.example.com/") &&
              writeOpml(html.feeds(), html.folders(), 1000).contains("htmlUrl=\"https://site.example.com/\""),
          "OPML keeps and writes htmlUrl");
}

void testSettingsJson()
{
    const Settings old = Settings::fromJson(QJsonObject{{QStringLiteral("oldestFirst"), true},
                                                        {QStringLiteral("summaryLines"), 3},
                                                        {QStringLiteral("theme"), QStringLiteral("sepia")},
                                                        {QStringLiteral("unreadOnlyTimeline"), true},
                                                        {QStringLiteral("loadRemoteImages"), true}});
    check(old.oldestFirst && old.summaryLines == 3 && old.theme == QLatin1String("sepia") && old.unreadOnlyTimeline &&
              old.loadRemoteImages && !old.showThumbnails && !old.foldDuplicates && !old.markAllKeepsStarred &&
              !old.sortFeedsByUnread && old.filters.isEmpty() && old.smartFeeds.isEmpty(),
          "settings saved before these features still load, new ones off");
    check(Settings::fromJson(QJsonObject{{QStringLiteral("hideReadArticles"), true}}).unreadOnlyTimeline &&
              !Settings::fromJson(QJsonObject{{QStringLiteral("hideReadArticles"), true}, {QStringLiteral("unreadOnlyTimeline"), false}}).unreadOnlyTimeline,
          "hideReadArticles is read as unreadOnlyTimeline");
    Settings settings;
    settings.showThumbnails = true;
    settings.foldDuplicates = true;
    settings.markAllKeepsStarred = true;
    settings.sortFeedsByUnread = true;
    ContentFilter filter;
    filter.id = QStringLiteral("f1");
    filter.feedId = QStringLiteral("feed-1");
    filter.field = QStringLiteral("titleOrText");
    filter.pattern = QStringLiteral("^ad:");
    filter.regex = true;
    filter.action = QStringLiteral("markRead");
    settings.filters.append(filter);
    SmartFeed smart;
    smart.id = QStringLiteral("s1");
    smart.name = QStringLiteral("Rust");
    smart.words = QStringLiteral("rust");
    smart.scope = QStringLiteral("folder:Tech");
    smart.unreadOnly = true;
    smart.starredOnly = true;
    smart.maxAgeDays = 7;
    settings.smartFeeds.append(smart);
    const QJsonObject json = settings.toJson();
    for (const char *key : {"showThumbnails", "foldDuplicates", "markAllKeepsStarred", "sortFeedsByUnread", "filters", "smartFeeds", "unreadOnlyTimeline"})
        check(json.contains(QLatin1String(key)), "settings JSON key", QString::fromLatin1(key));
    check(json.value(QStringLiteral("filters")).toArray().first().toObject() ==
              QJsonObject({{QStringLiteral("id"), QStringLiteral("f1")}, {QStringLiteral("feedId"), QStringLiteral("feed-1")},
                           {QStringLiteral("field"), QStringLiteral("titleOrText")}, {QStringLiteral("pattern"), QStringLiteral("^ad:")},
                           {QStringLiteral("regex"), true}, {QStringLiteral("action"), QStringLiteral("markRead")}}),
          "filter JSON shape");
    check(json.value(QStringLiteral("smartFeeds")).toArray().first().toObject().value(QStringLiteral("maxAgeDays")).toInt() == 7,
          "smart feed JSON shape");
    const Settings again = Settings::fromJson(QJsonDocument::fromJson(QJsonDocument(json).toJson()).object());
    check(again.toJson() == json && again.filters == settings.filters && again.smartFeeds == settings.smartFeeds,
          "settings JSON round-trips");
    QJsonArray many;
    for (int i = 0; i < kMaxFilters + 5; ++i)
        many.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("x%1").arg(i)}, {QStringLiteral("pattern"), QStringLiteral("p%1").arg(i)},
                                {QStringLiteral("field"), QStringLiteral("bogus")}, {QStringLiteral("action"), QStringLiteral("explode")}});
    const Settings capped = Settings::fromJson(QJsonObject{{QStringLiteral("filters"), many}});
    check(capped.filters.size() == kMaxFilters && capped.filters.first().field == QLatin1String("title") &&
              capped.filters.first().action == QLatin1String("hide"),
          "loaded filters are capped and cleaned");
}

void testPageFeatures(const QString &home)
{
    FakeNetwork network;
    network.route(QStringLiteral("https://engineering.example.com/feed.xml"), 200, liveFeed(), "application/rss+xml", {{"ETag", "\"e1\""}});
    network.route(QStringLiteral("https://engineering.example.com/moved.xml"), 200, liveFeed(), "application/rss+xml");
    network.route(QStringLiteral("https://www.youtube.com/@RickAstleyYT"), 200,
                  "<html><body><script>{\"channelId\":\"UCuAXFkgsw1L7xaCfnd5JJOw\"}</script></body></html>", "text/html");
    QByteArray youtube = kYouTube;
    youtube.replace("2009-10-25T06:57:33+00:00", QDateTime::currentDateTimeUtc().addSecs(-600).toString(Qt::ISODate).toUtf8());
    network.route(QStringLiteral("https://www.youtube.com/feeds/videos.xml?channel_id=UCuAXFkgsw1L7xaCfnd5JJOw"), 200, youtube, "application/atom+xml");
    network.route(QStringLiteral("https://i2.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg"), 200, pngBytes(), "image/png");
    const QString now = QDateTime::currentDateTimeUtc().addSecs(-300).toString(Qt::RFC2822Date);
    network.route(QStringLiteral("https://arabic.example.com/feed.xml"), 200,
                  rssItems(QStringLiteral("عربي"), {{QStringLiteral("عنوان"), QStringLiteral("https://arabic.example.com/1"), QStringLiteral("ar-1"),
                                                     QStringLiteral("<p>نص المقال</p><pre>code line that must not wrap</pre>"),
                                                     QStringLiteral("<pubDate>%1</pubDate><enclosure url=\"https://arabic.example.com/photo.jpg\" type=\"image/jpeg\" length=\"5\"/>"
                                                                    "<enclosure url=\"https://arabic.example.com/talk.mp3\" type=\"audio/mpeg\" length=\"9\"/>").arg(now)}},
                           QStringLiteral("<language>ar</language>")));
    const QString localPath = QDir(home).filePath(QStringLiteral("feeds/page-local.xml"));
    QDir().mkpath(QFileInfo(localPath).absolutePath());
    auto writeLocal = [&localPath](const QByteArray &bytes) {
        QFile file(localPath);
        file.open(QIODevice::WriteOnly | QIODevice::Truncate);
        file.write(bytes);
    };
    writeLocal(rssWithItems(4, 0, QStringLiteral("Old")));
    const QString sneakyPath = QDir(home).filePath(QStringLiteral("feeds/sneaky.xml"));
    QFile sneaky(sneakyPath);
    sneaky.open(QIODevice::WriteOnly);
    sneaky.write(kRdf);
    sneaky.close();
    network.route(QStringLiteral("https://lists.example.com/subscriptions.opml"), 200,
                  QStringLiteral("<opml version=\"2.0\"><body><outline text=\"Remote\" xmlUrl=\"https://remote.example.com/feed\" htmlUrl=\"https://remote.example.com/\"/>"
                                 "<outline text=\"Sneaky\" xmlUrl=\"%1\"/></body></opml>").arg(QUrl::fromLocalFile(sneakyPath).toString()).toUtf8(),
                  "text/x-opml");
    network.route(QStringLiteral("https://remote.example.com/feed"), 200, kRdf);
    network.route(QStringLiteral("https://engineering.example.com/"), 200,
                  "<html><head><base href=\"https://engineering.example.com/assets/\">"
                  "<link rel=\"icon\" sizes=\"64x64\" href=\"blank.png\">"
                  "<link rel=\"apple-touch-icon\" href=\"/touch.png\"></head><body></body></html>",
                  "text/html");
    network.route(QStringLiteral("https://engineering.example.com/assets/blank.png"), 200, squarePng(1, 1), "image/png");
    network.route(QStringLiteral("https://engineering.example.com/touch.png"), 200, squarePng(120, 120), "image/png");

    QList<QPair<QString, QString>> shared;
    forkmesh::market::NativePackageHost host;
    host.packageId = QStringLiteral("rss-reader-features");
    host.packageName = QStringLiteral("RSS Reader");
    host.network = &network;
    host.chatTargets = [] {
        return QList<forkmesh::market::NativePackageChatTarget>{
            {QStringLiteral("office:general"), QStringLiteral("#general"), false},
            {QStringLiteral("office:dm:avery"), QStringLiteral("@avery"), true}};
    };
    host.sendChatMessage = [&shared](const QString &conversation, const QString &text) {
        shared.append({conversation, text});
        return conversation != QLatin1String("office:closed");
    };
    RssReaderFeature page(host);
    page.resize(1280, 800);
    page.refresh();
    auto subscribe = [&page](const QString &address) {
        bool done = false;
        bool ok = false;
        QString message;
        page.subscribe(address, QString(), QString(), [&](bool success, const QString &text) {
            ok = success;
            message = text;
            done = true;
        });
        waitFor([&] { return done; });
        check(ok, "subscribed", address + QStringLiteral(" ") + message);
        return ok ? page.currentSource().id : QString();
    };
    const QString engineering = subscribe(QStringLiteral("https://engineering.example.com/feed.xml"));
    const qint64 at = QDateTime::currentMSecsSinceEpoch();
    const Source feedSource{SourceKind::Feed, engineering};
    const QList<ArticleRef> refs = page.store().query(feedSource, QString(), false, at);
    check(refs.size() == 3 && page.timelineRows() == 3, "three articles in the feed timeline");

    const QString iconFile = DiskStore(page.dataDirectory()).iconPath(engineering);
    waitFor([&] { return QFile::exists(iconFile); });
    const QImage storedIcon(iconFile);
    const qsizetype blankAt = network.requested.indexOf(QStringLiteral("https://engineering.example.com/assets/blank.png"));
    const qsizetype touchAt = network.requested.indexOf(QStringLiteral("https://engineering.example.com/touch.png"));
    check(storedIcon.size() == QSize(64, 64) && blankAt >= 0 && touchAt > blankAt &&
              network.requested.contains(QStringLiteral("https://engineering.example.com/icon.png")) &&
              !network.requested.contains(QStringLiteral("https://engineering.example.com/favicon.ico")),
          "the feed icon comes from the home page's icon links, skipping a 1x1 image",
          network.requested.join(QStringLiteral(" ")));
    const Feed *iconOwner = page.store().feed(engineering);
    check(iconOwner && iconOwner->iconCheckedMs > 0 && !iconFetchDue(*iconOwner, true, at), "the icon search is recorded");

    QToolButton *shareButton = nullptr;
    for (QToolButton *button : page.findChildren<QToolButton *>()) {
        if (button->property("rssIcon").toString() == QLatin1String("paper-airplane"))
            shareButton = button;
    }
    check(shareButton && !shareButton->isHidden(),
          "Share to Chat is offered when the host provides chat");
    check(page.shareTargets().size() == 2 && page.shareTargets().at(1).person, "share targets come from the host");
    const Article *sharedArticle = page.store().article(refs.value(0));
    const QString shareError = page.shareArticle(refs.value(0), QStringLiteral("office:general"));
    check(shareError.isEmpty() && shared.size() == 1 && shared.first().first == QLatin1String("office:general") && sharedArticle &&
              shared.first().second == shareMessage(sharedArticle->title, sharedArticle->link) &&
              shared.first().second.contains(sharedArticle->link),
          "sharing sends the title and link to the chosen conversation", shareError);
    check(!page.shareArticle(refs.value(0), QStringLiteral("office:closed")).isEmpty(), "a refused share reports an error");

    page.updateSettings([](Settings &settings) { settings.unreadOnlyTimeline = true; }, true, false);
    page.selectSource(feedSource);
    page.selectArticle(refs.value(0));
    page.selectArticle(refs.value(1));
    check(page.timelineRows() == 3, "articles read while the timeline is open stay until it is left");
    page.selectSource({SourceKind::AllUnread, QString()});
    check(page.timelineRows() == 1, "All Unread shows what is still unread", QString::number(page.timelineRows()));
    page.selectSource(feedSource);
    check(page.timelineRows() == 1, "Hide Read Articles applies to a feed timeline after leaving it");
    page.selectArticle(refs.value(0));
    page.toggleStar();
    page.selectSource({SourceKind::Starred, QString()});
    check(page.timelineRows() == 1, "Starred still shows read starred articles");
    page.selectSource(feedSource);
    check(page.timelineRows() == 1, "one unread article left in the feed");
    page.markAllRead(false);
    check(page.timelineRows() == 0 && page.currentArticle().isNull() && page.store().article(refs.value(2))->read,
          "Mark All as Read empties the timeline at once");
    page.updateSettings([](Settings &settings) { settings.unreadOnlyTimeline = false; }, true, false);
    check(page.timelineRows() == 3, "turning Hide Read Articles off shows them again");

    page.selectArticle(refs.value(0));
    page.toggleRead();
    page.selectArticle(refs.value(1));
    page.toggleRead();
    page.updateSettings([](Settings &settings) { settings.markAllKeepsStarred = true; }, false, false);
    page.markAllRead(false);
    check(!page.store().article(refs.value(0))->read && page.store().article(refs.value(1))->read,
          "Mark All as Read Skips Starred");
    page.updateSettings([](Settings &settings) { settings.markAllKeepsStarred = false; }, false, false);

    page.selectArticle(refs.value(2));
    page.toggleReadLater();
    check(page.store().article(refs.value(2))->readLater && page.visibleSourceIds().contains(QStringLiteral("readlater")),
          "Read Later appears once it holds an article");
    page.selectSource({SourceKind::ReadLater, QString()});
    check(page.timelineRows() == 1, "Read Later timeline");
    page.selectArticle(refs.value(2));
    page.toggleReadLater();
    page.selectSource(feedSource);
    check(!page.visibleSourceIds().contains(QStringLiteral("readlater")), "Read Later hides when empty");

    page.setFeedFlags(engineering, true, false, false);
    const QString feedUrl = QStringLiteral("https://engineering.example.com/feed.xml");
    const qsizetype fetchesBefore = network.requested.count(feedUrl);
    page.checkFeeds(true);
    waitFor([&] { return !page.refreshing(); }, 1000);
    check(network.requested.count(feedUrl) == fetchesBefore, "a paused feed is not refreshed");
    QString line;
    page.checkNow(engineering, [&line](const QString &text) { line = text; });
    waitFor([&] { return !line.isEmpty(); });
    check(line.startsWith(QStringLiteral("RSS 2.0 · 3 items · checked ")) && network.requested.count(feedUrl) == fetchesBefore + 1,
          "Check Now works on a paused feed and reports the format, items and time", line);
    page.setFeedFlags(engineering, false, false, false);

    const QString youtubeId = subscribe(QStringLiteral("https://www.youtube.com/@RickAstleyYT"));
    const Feed *youtubeFeed = page.store().feed(youtubeId);
    check(youtubeFeed && youtubeFeed->url == QLatin1String("https://www.youtube.com/feeds/videos.xml?channel_id=UCuAXFkgsw1L7xaCfnd5JJOw"),
          "a YouTube handle subscribes to the channel feed");
    const ArticleRef video{youtubeId, QStringLiteral("yt:video:dQw4w9WgXcQ")};
    page.selectArticle(video);
    QString html = page.renderedArticleHtml();
    check(html.contains(QStringLiteral("href=\"https://i2.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg\">[Image]</a>")) &&
              html.contains(QStringLiteral("https://RickAstley.lnk.to/_listenYD")) && html.contains(QStringLiteral(" min read")),
          "the video thumbnail is a link while remote images are off; description and reading time shown", html);
    page.selectSource({SourceKind::Feed, youtubeId});
    page.updateSettings([](Settings &settings) {
        settings.loadRemoteImages = true;
        settings.showThumbnails = true;
    }, true, true);
    page.show();
    page.grab();
    QApplication::processEvents();
    waitFor([&] { return network.requested.contains(QStringLiteral("https://i2.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg")); });
    check(network.requested.contains(QStringLiteral("https://i2.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg")),
          "the timeline thumbnail loads through the host network");
    QApplication::processEvents();
    page.grab();
    page.selectArticle(video);
    html = page.renderedArticleHtml();
    check(html.contains(QStringLiteral("<img src=\"https://i2.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg\"")), "the thumbnail is drawn above the description", html);
    page.updateSettings([](Settings &settings) {
        settings.loadRemoteImages = false;
        settings.showThumbnails = false;
    }, true, true);

    const QString arabic = subscribe(QStringLiteral("https://arabic.example.com/feed.xml"));
    page.selectArticle({arabic, QStringLiteral("ar-1")});
    html = page.renderedArticleHtml();
    check(html.contains(QStringLiteral("lang=\"ar\" dir=\"rtl\"")) && html.contains(QStringLiteral("talk.mp3")) &&
              !html.contains(QStringLiteral("Attachment: <a href=\"https://arabic.example.com/photo.jpg\"")) &&
              html.contains(QStringLiteral("href=\"https://arabic.example.com/photo.jpg\">[Image]</a>")),
          "right-to-left article, image enclosure shown as an image and not listed as an attachment", html);

    page.setFeedSearch(QStringLiteral("youtube"));
    QStringList visible = page.visibleSourceIds();
    check(visible.contains(youtubeId) && !visible.contains(engineering) && !visible.contains(QStringLiteral("today")),
          "find a feed filters the list and hides smart feeds", visible.join(QLatin1Char(',')));
    page.setFeedSearch(QString());
    check(page.visibleSourceIds().contains(QStringLiteral("today")) && page.visibleSourceIds().contains(engineering), "clearing the search");
    page.updateSettings([](Settings &settings) { settings.sortFeedsByUnread = true; }, false, false);
    visible = page.visibleSourceIds();
    const int unreadEngineering = page.store().feedUnread(engineering);
    const int unreadYoutube = page.store().feedUnread(youtubeId);
    check(unreadEngineering != unreadYoutube &&
              (visible.indexOf(engineering) < visible.indexOf(youtubeId)) == (unreadEngineering > unreadYoutube),
          "Sort by Unread Count puts the feed with more unread first", visible.join(QLatin1Char(',')));
    page.updateSettings([](Settings &settings) { settings.sortFeedsByUnread = false; }, false, false);

    const QString localId = subscribe(QUrl::fromLocalFile(localPath).toString());
    check(page.store().articles(localId).size() == 4 && page.store().feedUnread(localId) == 4, "a file feed is followed from disk");
    writeLocal(rssWithItems(5, 0, QStringLiteral("Old")));
    page.checkFeeds(true);
    waitFor([&] { return !page.refreshing(); });
    check(page.store().articles(localId).size() == 5, "a file feed is read again on refresh");
    page.catchUp(7, true, false);
    check(page.store().feedUnread(localId) == 0 && !page.store().article(refs.value(0))->read, "Mark Older as Read across every feed, recent ones untouched");

    check(!page.changeFeedAddress(engineering, page.store().feed(youtubeId)->url).isEmpty(), "editing to another feed's address is refused");
    check(page.changeFeedAddress(engineering, QStringLiteral("https://engineering.example.com/moved.xml")).isEmpty(), "feed address edited");
    waitFor([&] { return !page.refreshing(); });
    const Feed *moved = page.store().feed(engineering);
    check(moved && moved->url == QLatin1String("https://engineering.example.com/moved.xml") &&
              moved->previousUrls.contains(QStringLiteral("https://engineering.example.com/feed.xml")) && moved->failures == 0 &&
              network.requested.contains(QStringLiteral("https://engineering.example.com/moved.xml")),
          "the new address is checked at once");

    page.selectArticle(video);
    page.toggleRead();
    const int badgeBefore = page.navigationBadge();
    ContentFilter filter;
    filter.pattern = QStringLiteral("Rick Astley");
    page.updateSettings([&filter](Settings &settings) { settings.filters.append(filter); }, true, false);
    check(page.navigationBadge() == badgeBefore - 1 && page.store().query({SourceKind::Feed, youtubeId}, QString(), false, at).isEmpty(),
          "a hide filter removes the article from the timeline and the badge");
    page.updateSettings([](Settings &settings) { settings.filters.clear(); }, true, false);
    check(page.navigationBadge() == badgeBefore, "removing the filter brings it back");

    SmartFeed smart;
    smart.id = QStringLiteral("smart-1");
    smart.name = QStringLiteral("Videos");
    smart.scope = QStringLiteral("feed:") + youtubeId;
    page.updateSettings([&smart](Settings &settings) { settings.smartFeeds.append(smart); }, false, false);
    check(page.visibleSourceIds().contains(QStringLiteral("smart:smart-1")), "custom smart feed listed under Smart Feeds");
    page.selectSource({SourceKind::Smart, QStringLiteral("smart-1")});
    check(page.timelineRows() == 1, "custom smart feed timeline");

    bool imported = false;
    QString importMessage;
    QString importedHome;
    page.importOpmlFromUrl(QStringLiteral("https://lists.example.com/subscriptions.opml"), [&](bool ok, const QString &message) {
        imported = ok;
        importMessage = message;
        const Feed *remote = page.store().feedByUrl(QUrl(QStringLiteral("https://remote.example.com/feed")));
        importedHome = remote ? remote->homePage : QString();
    });
    waitFor([&] { return !importMessage.isEmpty(); });
    check(imported && importedHome == QLatin1String("https://remote.example.com/") && importMessage.contains(QStringLiteral("1 skipped")) &&
              page.store().feedByUrl(QUrl::fromLocalFile(sneakyPath)) == nullptr,
          "Import from URL adds the feeds and refuses file addresses", importMessage + QLatin1Char('|') + importedHome);
    waitFor([&] { return !page.refreshing(); });
    page.saveNow();
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
    QCoreApplication::setOrganizationName(QStringLiteral("ForkMeshRssReaderTest"));
    QCoreApplication::setApplicationName(QStringLiteral("RssReaderTest"));

    testDates();
    testParsing();
    testSanitizer();
    testDiscovery();
    testStore();
    testOpml();
    testReader();
    testFetch();
    testMediaRss();
    testArticleWins();
    testFiltersAndDuplicates();
    testSmartFeedsAndReadLater();
    testFeedSwitches();
    testDiscoveryRewrites(home);
    testSettingsJson();
    testIconsCommentsAndSharing();
    testPage(home);
    testPageFeatures(home);

    if (failures > 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("All RSS Reader checks passed\n");
    return 0;
}
