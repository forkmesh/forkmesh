#include "RssReaderFeature.h"
#include "RssReaderFetch.h"
#include "RssReaderModel.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
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
    check(rss.ok && rss.format == QLatin1String("RSS"), "RSS 2.0 parses despite DOCTYPE and undefined entities", rss.error);
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
    check(atom.ok && atom.format == QLatin1String("Atom") && atom.items.size() == 2, "Atom parses", atom.error);
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
    check(json.ok && json.format == QLatin1String("JSON Feed") && json.items.size() == 2, "JSON Feed parses", json.error);
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
    check(fallbacks.size() == 4 && fallbacks.first().toString() == QLatin1String("https://site.example.com:8443/feed") &&
              fallbacks.last().toString() == QLatin1String("https://site.example.com:8443/atom.xml"),
          "fallback paths on the same origin");
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

QByteArray liveFeed()
{
    const QDateTime current = QDateTime::currentDateTimeUtc();
    QByteArray xml = kRss;
    xml.replace("Tue, 14 Oct 2026 14:20:00 GMT", current.addSecs(-3600).toString(Qt::RFC2822Date).toUtf8());
    xml.replace("Mon, 13 Oct 2026 09:00:00 -0400", current.addSecs(-7200).toString(Qt::RFC2822Date).toUtf8());
    return xml;
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
        check(page.navigationBadge() == 2, "opening an article marks it read");
        page.toggleStar();
        check(page.store().article(page.currentArticle())->starred, "star toggled");
        page.toggleRead();
        check(page.navigationBadge() == 3, "mark as unread");
        page.toggleRead();

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
                  about->text().contains(QStringLiteral("https://github.com/Ranchero-Software/NetNewsWire")),
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
    testPage(home);

    if (failures > 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("All RSS Reader checks passed\n");
    return 0;
}
