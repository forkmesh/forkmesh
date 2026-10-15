#include "RssReaderModel.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonValue>
#include <QPair>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTime>
#include <QTimeZone>
#include <QUuid>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <algorithm>
#include <cctype>
#include <utility>

namespace forkmesh::rssreader {
namespace {

using Attributes = QList<QPair<QString, QString>>;

const QHash<QString, char32_t> &entityTable()
{
    static const QHash<QString, char32_t> table = [] {
        QHash<QString, char32_t> map;
        static const char *const latin1[] = {
            "nbsp", "iexcl", "cent", "pound", "curren", "yen", "brvbar", "sect", "uml", "copy",
            "ordf", "laquo", "not", "shy", "reg", "macr", "deg", "plusmn", "sup2", "sup3",
            "acute", "micro", "para", "middot", "cedil", "sup1", "ordm", "raquo", "frac14",
            "frac12", "frac34", "iquest", "Agrave", "Aacute", "Acirc", "Atilde", "Auml", "Aring",
            "AElig", "Ccedil", "Egrave", "Eacute", "Ecirc", "Euml", "Igrave", "Iacute", "Icirc",
            "Iuml", "ETH", "Ntilde", "Ograve", "Oacute", "Ocirc", "Otilde", "Ouml", "times",
            "Oslash", "Ugrave", "Uacute", "Ucirc", "Uuml", "Yacute", "THORN", "szlig", "agrave",
            "aacute", "acirc", "atilde", "auml", "aring", "aelig", "ccedil", "egrave", "eacute",
            "ecirc", "euml", "igrave", "iacute", "icirc", "iuml", "eth", "ntilde", "ograve",
            "oacute", "ocirc", "otilde", "ouml", "divide", "oslash", "ugrave", "uacute", "ucirc",
            "uuml", "yacute", "thorn", "yuml"};
        for (int i = 0; i < 96; ++i)
            map.insert(QString::fromLatin1(latin1[i]), char32_t(160 + i));
        static const std::pair<const char *, char32_t> extra[] = {
            {"amp", 38}, {"lt", 60}, {"gt", 62}, {"quot", 34}, {"apos", 39}, {"OElig", 338},
            {"oelig", 339}, {"Scaron", 352}, {"scaron", 353}, {"Yuml", 376}, {"fnof", 402},
            {"circ", 710}, {"tilde", 732}, {"ensp", 8194}, {"emsp", 8195}, {"thinsp", 8201},
            {"zwnj", 8204}, {"zwj", 8205}, {"lrm", 8206}, {"rlm", 8207}, {"ndash", 8211},
            {"mdash", 8212}, {"lsquo", 8216}, {"rsquo", 8217}, {"sbquo", 8218}, {"ldquo", 8220},
            {"rdquo", 8221}, {"bdquo", 8222}, {"dagger", 8224}, {"Dagger", 8225}, {"bull", 8226},
            {"hellip", 8230}, {"permil", 8240}, {"prime", 8242}, {"Prime", 8243},
            {"lsaquo", 8249}, {"rsaquo", 8250}, {"oline", 8254}, {"frasl", 8260},
            {"euro", 8364}, {"trade", 8482}, {"larr", 8592}, {"uarr", 8593}, {"rarr", 8594},
            {"darr", 8595}, {"harr", 8596}, {"minus", 8722}, {"infin", 8734}, {"asymp", 8776},
            {"ne", 8800}, {"le", 8804}, {"ge", 8805}, {"sdot", 8901}, {"loz", 9674},
            {"spades", 9824}, {"clubs", 9827}, {"hearts", 9829}, {"diams", 9830},
            {"Alpha", 913}, {"Beta", 914}, {"Gamma", 915}, {"Delta", 916}, {"Omega", 937},
            {"alpha", 945}, {"beta", 946}, {"gamma", 947}, {"delta", 948}, {"epsilon", 949},
            {"lambda", 955}, {"mu", 956}, {"pi", 960}, {"sigma", 963}, {"tau", 964},
            {"phi", 966}, {"omega", 969}, {"check", 10003}, {"star", 9734}};
        for (const auto &entry : extra)
            map.insert(QString::fromLatin1(entry.first), entry.second);
        return map;
    }();
    return table;
}

bool validXmlChar(qint64 code)
{
    return code == 9 || code == 10 || code == 13 || (code >= 0x20 && code <= 0xD7FF) ||
           (code >= 0xE000 && code <= 0xFFFD) || (code >= 0x10000 && code <= 0x10FFFF);
}

bool isAsciiAlnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool startsWithAt(const QByteArray &data, int at, const char *needle, bool caseless = false)
{
    const int length = int(qstrlen(needle));
    if (at + length > data.size())
        return false;
    for (int i = 0; i < length; ++i) {
        char a = data.at(at + i);
        char b = needle[i];
        if (caseless) {
            a = char(QChar::toLower(uchar(a)));
            b = char(QChar::toLower(uchar(b)));
        }
        if (a != b)
            return false;
    }
    return true;
}

QStringView localOf(QStringView name)
{
    const qsizetype colon = name.lastIndexOf(QLatin1Char(':'));
    return colon < 0 ? name : name.mid(colon + 1);
}

QStringView prefixOf(QStringView name)
{
    const qsizetype colon = name.lastIndexOf(QLatin1Char(':'));
    return colon < 0 ? QStringView() : name.left(colon);
}

QString attributeValue(const QXmlStreamAttributes &attributes, QStringView local)
{
    for (const QXmlStreamAttribute &attribute : attributes) {
        if (localOf(attribute.qualifiedName()).compare(local, Qt::CaseInsensitive) == 0)
            return attribute.value().toString();
    }
    return {};
}

QString readInner(QXmlStreamReader &xml, bool escapeText)
{
    QString out;
    int depth = 0;
    while (!xml.atEnd()) {
        const QXmlStreamReader::TokenType token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            ++depth;
            out += QLatin1Char('<') + localOf(xml.qualifiedName()).toString();
            for (const QXmlStreamAttribute &attribute : xml.attributes()) {
                out += QLatin1Char(' ') + localOf(attribute.qualifiedName()).toString() +
                       QStringLiteral("=\"") + escapeHtml(attribute.value().toString()) +
                       QLatin1Char('"');
            }
            out += QLatin1Char('>');
        } else if (token == QXmlStreamReader::EndElement) {
            if (depth == 0)
                break;
            --depth;
            out += QStringLiteral("</") + localOf(xml.qualifiedName()).toString() +
                   QLatin1Char('>');
        } else if (token == QXmlStreamReader::Characters ||
                   token == QXmlStreamReader::EntityReference) {
            const QString text = xml.text().toString();
            out += (escapeText && !xml.isCDATA()) ? escapeHtml(text) : text;
        }
    }
    return out;
}

QString readText(QXmlStreamReader &xml)
{
    return xml.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
}

QString textToHtml(const QString &text)
{
    QString escaped = escapeHtml(text.trimmed());
    escaped.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    escaped.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    return escaped;
}

QString resolveUrl(const QUrl &base, const QString &reference)
{
    const QString trimmed = reference.trimmed();
    if (trimmed.isEmpty())
        return {};
    const QUrl relative(trimmed, QUrl::TolerantMode);
    if (!relative.isValid())
        return {};
    const QUrl resolved = base.isValid() ? base.resolved(relative) : relative;
    return resolved.toString();
}

QString rssAuthor(const QString &raw)
{
    const QString text = plainText(raw, 200);
    static const QRegularExpression named(QStringLiteral("^\\S+@\\S+\\s*\\((.+)\\)$"));
    const QRegularExpressionMatch match = named.match(text);
    if (match.hasMatch())
        return match.captured(1).trimmed();
    return text;
}

void parseEnclosure(QXmlStreamReader &xml, QList<Enclosure> &list, const QUrl &base,
                    const QString &urlAttribute)
{
    Enclosure enclosure;
    enclosure.url = resolveUrl(base, attributeValue(xml.attributes(), urlAttribute));
    enclosure.type = attributeValue(xml.attributes(), u"type").trimmed();
    enclosure.length = attributeValue(xml.attributes(), u"length").trimmed().toLongLong();
    if (!enclosure.url.isEmpty())
        list.append(enclosure);
}

void parseRssItem(QXmlStreamReader &xml, ParsedItem &item, const QUrl &base, bool rdf)
{
    QString description;
    QString encoded;
    QString guid;
    QString pubDate;
    QString dcDate;
    bool guidIsPermalink = true;
    if (rdf)
        item.id = attributeValue(xml.attributes(), u"about").trimmed();
    while (xml.readNextStartElement()) {
        const QString qualified = xml.qualifiedName().toString();
        const QStringView local = localOf(qualified);
        const QStringView prefix = prefixOf(qualified);
        if (prefix.isEmpty()) {
            if (local == u"title") {
                item.title = plainText(readInner(xml, false), 2000);
            } else if (local == u"link") {
                item.link = readText(xml);
            } else if (local == u"guid") {
                guidIsPermalink = attributeValue(xml.attributes(), u"isPermaLink")
                                      .trimmed()
                                      .compare(QStringLiteral("false"), Qt::CaseInsensitive) != 0;
                guid = readText(xml);
            } else if (local == u"description") {
                description = readInner(xml, false);
            } else if (local == u"author") {
                const QString author = rssAuthor(readInner(xml, false));
                if (item.author.isEmpty())
                    item.author = author;
            } else if (local == u"pubDate") {
                pubDate = readText(xml);
            } else if (local == u"comments") {
                item.comments = resolveUrl(base, readText(xml));
            } else if (local == u"enclosure") {
                parseEnclosure(xml, item.enclosures, base, QStringLiteral("url"));
                xml.skipCurrentElement();
            } else {
                xml.skipCurrentElement();
            }
        } else if (local == u"encoded") {
            encoded = readInner(xml, false);
        } else if (local == u"creator") {
            item.author = plainText(readInner(xml, false), 200);
        } else if (local == u"date") {
            dcDate = readText(xml);
        } else {
            xml.skipCurrentElement();
        }
    }
    item.html = encoded.trimmed().isEmpty() ? description : encoded;
    if (!rdf || item.id.isEmpty())
        item.id = guid;
    if (item.link.isEmpty() && guidIsPermalink &&
        (guid.startsWith(QStringLiteral("http://")) || guid.startsWith(QStringLiteral("https://"))))
        item.link = guid;
    item.link = resolveUrl(base, item.link);
    item.publishedMs = parseDate(pubDate);
    if (item.publishedMs < 0)
        item.publishedMs = parseDate(dcDate);
}

void parseRssImage(QXmlStreamReader &xml, ParsedFeed &feed, const QUrl &base)
{
    while (xml.readNextStartElement()) {
        if (localOf(xml.qualifiedName()) == u"url" && feed.iconUrl.isEmpty())
            feed.iconUrl = resolveUrl(base, readText(xml));
        else
            xml.skipCurrentElement();
    }
}

void parseRssChannel(QXmlStreamReader &xml, ParsedFeed &feed, const QUrl &base, bool rdf)
{
    while (xml.readNextStartElement()) {
        const QString qualified = xml.qualifiedName().toString();
        const QStringView local = localOf(qualified);
        const bool plain = prefixOf(qualified).isEmpty();
        if (plain && local == u"title") {
            feed.title = plainText(readInner(xml, false), 300);
        } else if (plain && local == u"link") {
            feed.homePage = resolveUrl(base, readText(xml));
        } else if (plain && local == u"image") {
            parseRssImage(xml, feed, base);
        } else if (plain && local == u"item") {
            if (feed.items.size() < kMaxItemsPerFetch) {
                ParsedItem item;
                parseRssItem(xml, item, base, rdf);
                feed.items.append(item);
            } else {
                xml.skipCurrentElement();
            }
        } else {
            xml.skipCurrentElement();
        }
    }
}

QString atomContent(QXmlStreamReader &xml)
{
    if (!attributeValue(xml.attributes(), u"src").isEmpty()) {
        xml.skipCurrentElement();
        return {};
    }
    const QString type = attributeValue(xml.attributes(), u"type").trimmed().toLower();
    if (type == QLatin1String("xhtml")) {
        QString inner = readInner(xml, true).trimmed();
        if (inner.startsWith(QStringLiteral("<div"))) {
            const qsizetype open = inner.indexOf(QLatin1Char('>'));
            const qsizetype close = inner.lastIndexOf(QStringLiteral("</div>"));
            if (open >= 0 && close > open)
                inner = inner.mid(open + 1, close - open - 1);
        }
        return inner;
    }
    if (type == QLatin1String("html") || type == QLatin1String("text/html"))
        return readInner(xml, false);
    if (type.isEmpty() || type == QLatin1String("text") || type.startsWith(QStringLiteral("text/")))
        return textToHtml(readText(xml));
    xml.skipCurrentElement();
    return {};
}

void parseAtomEntry(QXmlStreamReader &xml, ParsedItem &item, const QUrl &base)
{
    QString content;
    QString summary;
    QString published;
    QString updated;
    bool htmlLink = false;
    while (xml.readNextStartElement()) {
        const QStringView local = localOf(xml.qualifiedName());
        if (local == u"id") {
            item.id = readText(xml);
        } else if (local == u"title") {
            item.title = plainText(atomContent(xml), 2000);
        } else if (local == u"link") {
            const QString rel = attributeValue(xml.attributes(), u"rel").trimmed().toLower();
            const QString href = resolveUrl(base, attributeValue(xml.attributes(), u"href"));
            const QString type = attributeValue(xml.attributes(), u"type").trimmed().toLower();
            if (rel.isEmpty() || rel == QLatin1String("alternate")) {
                const bool isHtml = type.isEmpty() || type.contains(QStringLiteral("html"));
                if (!href.isEmpty() && (item.link.isEmpty() || (isHtml && !htmlLink))) {
                    item.link = href;
                    htmlLink = isHtml;
                }
            } else if (rel == QLatin1String("enclosure")) {
                parseEnclosure(xml, item.enclosures, base, QStringLiteral("href"));
            } else if (rel == QLatin1String("replies") && type.contains(QStringLiteral("html"))) {
                item.comments = href;
            }
            xml.skipCurrentElement();
        } else if (local == u"content") {
            content = atomContent(xml);
        } else if (local == u"summary") {
            summary = atomContent(xml);
        } else if (local == u"published" || local == u"issued") {
            published = readText(xml);
        } else if (local == u"updated" || local == u"modified") {
            updated = readText(xml);
        } else if (local == u"author") {
            while (xml.readNextStartElement()) {
                if (localOf(xml.qualifiedName()) == u"name" && item.author.isEmpty())
                    item.author = plainText(readText(xml), 200);
                else
                    xml.skipCurrentElement();
            }
        } else {
            xml.skipCurrentElement();
        }
    }
    item.html = content.trimmed().isEmpty() ? summary : content;
    item.publishedMs = parseDate(published);
    if (item.publishedMs < 0)
        item.publishedMs = parseDate(updated);
}

void parseAtomFeed(QXmlStreamReader &xml, ParsedFeed &feed, const QUrl &fetched)
{
    QUrl base = fetched;
    const QString xmlBase = attributeValue(xml.attributes(), u"base");
    if (!xmlBase.isEmpty())
        base = QUrl(resolveUrl(fetched, xmlBase));
    bool htmlLink = false;
    while (xml.readNextStartElement()) {
        const QStringView local = localOf(xml.qualifiedName());
        if (local == u"title") {
            feed.title = plainText(atomContent(xml), 300);
        } else if (local == u"link") {
            const QString rel = attributeValue(xml.attributes(), u"rel").trimmed().toLower();
            const QString type = attributeValue(xml.attributes(), u"type").trimmed().toLower();
            const QString href = resolveUrl(base, attributeValue(xml.attributes(), u"href"));
            if ((rel.isEmpty() || rel == QLatin1String("alternate")) && !href.isEmpty()) {
                const bool isHtml = type.isEmpty() || type.contains(QStringLiteral("html"));
                if (feed.homePage.isEmpty() || (isHtml && !htmlLink)) {
                    feed.homePage = href;
                    htmlLink = isHtml;
                }
            }
            xml.skipCurrentElement();
        } else if (local == u"icon") {
            feed.iconUrl = resolveUrl(base, readText(xml));
        } else if (local == u"logo") {
            const QString logo = resolveUrl(base, readText(xml));
            if (feed.iconUrl.isEmpty())
                feed.iconUrl = logo;
        } else if (local == u"entry") {
            if (feed.items.size() < kMaxItemsPerFetch) {
                ParsedItem item;
                parseAtomEntry(xml, item, base);
                feed.items.append(item);
            } else {
                xml.skipCurrentElement();
            }
        } else {
            xml.skipCurrentElement();
        }
    }
}

QString jsonString(const QJsonObject &object, const char *key)
{
    const QJsonValue value = object.value(QLatin1String(key));
    if (value.isString())
        return value.toString().trimmed();
    if (value.isDouble())
        return QString::number(qint64(value.toDouble()));
    return {};
}

ParsedFeed parseJsonFeed(const QByteArray &data, const QUrl &base)
{
    ParsedFeed feed;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(data, &error);
    const QJsonObject root = document.object();
    if (error.error != QJsonParseError::NoError || !document.isObject() ||
        (!jsonString(root, "version").contains(QStringLiteral("jsonfeed.org")) &&
         !root.value(QStringLiteral("items")).isArray())) {
        feed.error = QStringLiteral("This is not a JSON Feed.");
        return feed;
    }
    feed.format = QStringLiteral("JSON Feed");
    feed.title = plainText(jsonString(root, "title"), 300);
    feed.homePage = resolveUrl(base, jsonString(root, "home_page_url"));
    feed.iconUrl = resolveUrl(base, jsonString(root, "favicon"));
    if (feed.iconUrl.isEmpty())
        feed.iconUrl = resolveUrl(base, jsonString(root, "icon"));
    QString feedAuthor;
    const QJsonArray feedAuthors = root.value(QStringLiteral("authors")).toArray();
    if (!feedAuthors.isEmpty())
        feedAuthor = jsonString(feedAuthors.first().toObject(), "name");
    if (feedAuthor.isEmpty())
        feedAuthor = jsonString(root.value(QStringLiteral("author")).toObject(), "name");
    for (const QJsonValue &value : root.value(QStringLiteral("items")).toArray()) {
        if (feed.items.size() >= kMaxItemsPerFetch)
            break;
        const QJsonObject object = value.toObject();
        ParsedItem item;
        item.id = jsonString(object, "id");
        item.link = resolveUrl(base, jsonString(object, "url"));
        if (item.link.isEmpty())
            item.link = resolveUrl(base, jsonString(object, "external_url"));
        item.title = plainText(jsonString(object, "title"), 2000);
        const QString html = object.value(QStringLiteral("content_html")).toString();
        const QString text = object.value(QStringLiteral("content_text")).toString();
        const QString summary = object.value(QStringLiteral("summary")).toString();
        item.html = !html.trimmed().isEmpty() ? html
                    : !text.trimmed().isEmpty() ? textToHtml(text)
                                                : textToHtml(summary);
        item.publishedMs = parseDate(jsonString(object, "date_published"));
        if (item.publishedMs < 0)
            item.publishedMs = parseDate(jsonString(object, "date_modified"));
        const QJsonArray authors = object.value(QStringLiteral("authors")).toArray();
        if (!authors.isEmpty())
            item.author = jsonString(authors.first().toObject(), "name");
        if (item.author.isEmpty())
            item.author = jsonString(object.value(QStringLiteral("author")).toObject(), "name");
        if (item.author.isEmpty())
            item.author = feedAuthor;
        for (const QJsonValue &attachmentValue : object.value(QStringLiteral("attachments")).toArray()) {
            const QJsonObject attachment = attachmentValue.toObject();
            Enclosure enclosure;
            enclosure.url = resolveUrl(base, jsonString(attachment, "url"));
            enclosure.type = jsonString(attachment, "mime_type");
            enclosure.length = qint64(attachment.value(QStringLiteral("size_in_bytes")).toDouble());
            if (!enclosure.url.isEmpty())
                item.enclosures.append(enclosure);
        }
        feed.items.append(item);
    }
    feed.ok = true;
    return feed;
}

struct HtmlToken {
    enum Kind { Text, Start, End };
    Kind kind = Text;
    QString name;
    Attributes attributes;
    bool selfClosing = false;
    bool raw = false;
    QString text;
};

bool isSpace(QChar c)
{
    return c == QLatin1Char(' ') || c == QLatin1Char('\n') || c == QLatin1Char('\t') ||
           c == QLatin1Char('\r') || c == QLatin1Char('\f');
}

bool rawTextElement(const QString &name)
{
    static const QSet<QString> names{
        QStringLiteral("script"), QStringLiteral("style"), QStringLiteral("textarea"),
        QStringLiteral("title"), QStringLiteral("iframe"), QStringLiteral("noembed"),
        QStringLiteral("noframes"), QStringLiteral("xmp"), QStringLiteral("noscript")};
    return names.contains(name);
}

QList<HtmlToken> tokenizeHtml(const QString &html)
{
    QList<HtmlToken> tokens;
    QString text;
    const qsizetype n = html.size();
    qsizetype i = 0;
    auto flush = [&] {
        if (text.isEmpty())
            return;
        HtmlToken token;
        token.kind = HtmlToken::Text;
        token.text = decodeEntities(text);
        tokens.append(token);
        text.clear();
    };
    while (i < n) {
        const QChar c = html.at(i);
        if (c != QLatin1Char('<') || i + 1 >= n) {
            text += c;
            ++i;
            continue;
        }
        const QChar next = html.at(i + 1);
        if (html.mid(i, 4) == QLatin1String("<!--")) {
            flush();
            const qsizetype end = html.indexOf(QStringLiteral("-->"), i + 4);
            i = end < 0 ? n : end + 3;
            continue;
        }
        if (next == QLatin1Char('!') || next == QLatin1Char('?')) {
            flush();
            const qsizetype end = html.indexOf(QLatin1Char('>'), i + 2);
            i = end < 0 ? n : end + 1;
            continue;
        }
        const bool closing = next == QLatin1Char('/');
        const qsizetype nameStart = closing ? i + 2 : i + 1;
        if (nameStart >= n || !html.at(nameStart).isLetter()) {
            text += c;
            ++i;
            continue;
        }
        qsizetype k = nameStart;
        while (k < n && (html.at(k).isLetterOrNumber() || html.at(k) == QLatin1Char('-') ||
                         html.at(k) == QLatin1Char(':') || html.at(k) == QLatin1Char('_')))
            ++k;
        HtmlToken token;
        token.kind = closing ? HtmlToken::End : HtmlToken::Start;
        token.name = html.mid(nameStart, k - nameStart).toLower();
        while (k < n) {
            while (k < n && isSpace(html.at(k)))
                ++k;
            if (k >= n)
                break;
            if (html.at(k) == QLatin1Char('>')) {
                ++k;
                break;
            }
            if (html.at(k) == QLatin1Char('/')) {
                if (k + 1 < n && html.at(k + 1) == QLatin1Char('>')) {
                    token.selfClosing = true;
                    k += 2;
                    break;
                }
                ++k;
                continue;
            }
            const qsizetype attributeStart = k;
            while (k < n && !isSpace(html.at(k)) && html.at(k) != QLatin1Char('=') &&
                   html.at(k) != QLatin1Char('>') && html.at(k) != QLatin1Char('/'))
                ++k;
            const QString attributeName = html.mid(attributeStart, k - attributeStart).toLower();
            while (k < n && isSpace(html.at(k)))
                ++k;
            QString value;
            if (k < n && html.at(k) == QLatin1Char('=')) {
                ++k;
                while (k < n && isSpace(html.at(k)))
                    ++k;
                if (k < n && (html.at(k) == QLatin1Char('"') || html.at(k) == QLatin1Char('\''))) {
                    const QChar quote = html.at(k++);
                    qsizetype end = html.indexOf(quote, k);
                    if (end < 0)
                        end = n;
                    value = html.mid(k, end - k);
                    k = end < n ? end + 1 : n;
                } else {
                    const qsizetype valueStart = k;
                    while (k < n && !isSpace(html.at(k)) && html.at(k) != QLatin1Char('>'))
                        ++k;
                    value = html.mid(valueStart, k - valueStart);
                }
            }
            if (attributeName.isEmpty()) {
                if (k < n && html.at(k) != QLatin1Char('>'))
                    ++k;
                continue;
            }
            if (!closing)
                token.attributes.append({attributeName, decodeEntities(value)});
        }
        flush();
        tokens.append(token);
        i = k;
        if (!closing && !token.selfClosing && rawTextElement(token.name)) {
            const QString closer = QStringLiteral("</") + token.name;
            qsizetype end = html.indexOf(closer, i, Qt::CaseInsensitive);
            if (end < 0)
                end = n;
            HtmlToken rawToken;
            rawToken.kind = HtmlToken::Text;
            rawToken.raw = true;
            rawToken.text = html.mid(i, end - i);
            tokens.append(rawToken);
            i = end;
        }
    }
    flush();
    return tokens;
}

const QSet<QString> &allowedTags()
{
    static const QSet<QString> tags = [] {
        QSet<QString> set;
        const char *const names[] = {
            "a", "abbr", "b", "blockquote", "br", "caption", "cite", "code", "dd", "del",
            "details", "dfn", "div", "dl", "dt", "em", "figcaption", "figure", "h1", "h2", "h3",
            "h4", "h5", "h6", "hr", "i", "img", "ins", "kbd", "li", "mark", "ol", "p", "pre", "q",
            "s", "samp", "section", "small", "span", "strike", "strong", "sub", "summary", "sup",
            "table", "tbody", "td", "tfoot", "th", "thead", "time", "tr", "u", "ul", "audio",
            "video", "source", "wbr", "article", "aside", "header", "footer", "main"};
        for (const char *name : names)
            set.insert(QString::fromLatin1(name));
        return set;
    }();
    return tags;
}

const QSet<QString> &voidTags()
{
    static const QSet<QString> tags{
        QStringLiteral("br"), QStringLiteral("hr"), QStringLiteral("img"), QStringLiteral("wbr"),
        QStringLiteral("source"), QStringLiteral("input"), QStringLiteral("link"),
        QStringLiteral("meta"), QStringLiteral("base"), QStringLiteral("param"),
        QStringLiteral("embed"), QStringLiteral("area"), QStringLiteral("track"),
        QStringLiteral("col"), QStringLiteral("keygen"), QStringLiteral("frame")};
    return tags;
}

const QSet<QString> &droppedWithContent()
{
    static const QSet<QString> tags = [] {
        QSet<QString> set;
        const char *const names[] = {
            "script", "style", "iframe", "object", "embed", "svg", "math", "form", "noscript",
            "template", "textarea", "select", "button", "title", "head", "canvas", "applet",
            "frame", "frameset", "noembed", "noframes", "input", "link", "meta", "base", "param",
            "xmp", "dialog", "portal", "option", "datalist", "map", "area", "track", "keygen"};
        for (const char *name : names)
            set.insert(QString::fromLatin1(name));
        return set;
    }();
    return tags;
}

QString safeUrl(const QUrl &base, const QString &value, bool allowMailto)
{
    QString cleaned;
    cleaned.reserve(value.size());
    for (const QChar c : value) {
        if (c.unicode() > 0x20 && c.unicode() != 0x7f)
            cleaned += c;
    }
    if (cleaned.isEmpty())
        return {};
    const QUrl relative(cleaned, QUrl::TolerantMode);
    if (!relative.isValid())
        return {};
    const QUrl resolved = relative.isRelative() ? (base.isValid() ? base.resolved(relative) : QUrl())
                                                : relative;
    const QString scheme = resolved.scheme().toLower();
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https")) {
        if (resolved.host().isEmpty())
            return {};
        return resolved.toString(QUrl::FullyEncoded);
    }
    if (allowMailto && scheme == QLatin1String("mailto"))
        return resolved.toString(QUrl::FullyEncoded);
    return {};
}

bool digits(const QString &value)
{
    if (value.isEmpty() || value.size() > 5)
        return false;
    for (const QChar c : value) {
        if (!c.isDigit())
            return false;
    }
    return true;
}

QString sanitizedAttributes(const QString &tag, const Attributes &attributes, const QUrl &base,
                            bool *valid)
{
    *valid = true;
    QString out;
    auto add = [&out](const QString &name, const QString &value) {
        out += QLatin1Char(' ') + name + QStringLiteral("=\"") + escapeHtml(value) +
               QLatin1Char('"');
    };
    QSet<QString> seen;
    bool hasSource = false;
    for (const auto &attribute : attributes) {
        const QString &name = attribute.first;
        const QString &value = attribute.second;
        if (seen.contains(name))
            continue;
        if (tag == QLatin1String("a")) {
            if (name == QLatin1String("href")) {
                const QString url = safeUrl(base, value, true);
                if (!url.isEmpty())
                    add(name, url);
            } else if (name == QLatin1String("title")) {
                add(name, value);
            } else {
                continue;
            }
        } else if (tag == QLatin1String("img")) {
            if (name == QLatin1String("src")) {
                const QString url = safeUrl(base, value, false);
                if (url.isEmpty())
                    continue;
                hasSource = true;
                add(name, url);
            } else if (name == QLatin1String("alt") || name == QLatin1String("title")) {
                add(name, value);
            } else if ((name == QLatin1String("width") || name == QLatin1String("height")) &&
                       digits(value.trimmed())) {
                add(name, value.trimmed());
            } else {
                continue;
            }
        } else if (tag == QLatin1String("td") || tag == QLatin1String("th")) {
            if ((name == QLatin1String("colspan") || name == QLatin1String("rowspan")) &&
                digits(value.trimmed()))
                add(name, value.trimmed());
            else
                continue;
        } else if (tag == QLatin1String("ol")) {
            if (name == QLatin1String("start") && digits(value.trimmed()))
                add(name, value.trimmed());
            else
                continue;
        } else if (tag == QLatin1String("time")) {
            if (name == QLatin1String("datetime"))
                add(name, value);
            else
                continue;
        } else if (tag == QLatin1String("abbr")) {
            if (name == QLatin1String("title"))
                add(name, value);
            else
                continue;
        } else {
            continue;
        }
        seen.insert(name);
    }
    if (tag == QLatin1String("img") && !hasSource)
        *valid = false;
    return out;
}

struct DomNode {
    QString name;
    Attributes attributes;
    QString text;
    QList<int> children;
    int parent = -1;
};

bool closesParagraph(const QString &name)
{
    static const QSet<QString> names{
        QStringLiteral("p"), QStringLiteral("div"), QStringLiteral("ul"), QStringLiteral("ol"),
        QStringLiteral("h1"), QStringLiteral("h2"), QStringLiteral("h3"), QStringLiteral("h4"),
        QStringLiteral("h5"), QStringLiteral("h6"), QStringLiteral("pre"),
        QStringLiteral("blockquote"), QStringLiteral("table"), QStringLiteral("section"),
        QStringLiteral("article"), QStringLiteral("figure"), QStringLiteral("hr"),
        QStringLiteral("dl"), QStringLiteral("main"), QStringLiteral("aside"),
        QStringLiteral("header"), QStringLiteral("footer"), QStringLiteral("nav")};
    return names.contains(name);
}

bool readerDropped(const QString &name)
{
    static const QSet<QString> names{
        QStringLiteral("script"), QStringLiteral("style"), QStringLiteral("nav"),
        QStringLiteral("header"), QStringLiteral("footer"), QStringLiteral("aside"),
        QStringLiteral("form"), QStringLiteral("noscript"), QStringLiteral("iframe"),
        QStringLiteral("svg"), QStringLiteral("template"), QStringLiteral("button"),
        QStringLiteral("select"), QStringLiteral("textarea"), QStringLiteral("head"),
        QStringLiteral("object"), QStringLiteral("canvas"), QStringLiteral("dialog")};
    return names.contains(name);
}

QList<DomNode> buildDom(const QList<HtmlToken> &tokens, QString *title)
{
    QList<DomNode> nodes;
    nodes.append(DomNode{QStringLiteral("#root"), {}, {}, {}, -1});
    QList<int> stack{0};
    QString dropTag;
    int dropDepth = 0;
    bool inTitle = false;
    for (const HtmlToken &token : tokens) {
        if (token.kind == HtmlToken::Start && token.name == QLatin1String("title"))
            inTitle = true;
        else if (token.kind == HtmlToken::End && token.name == QLatin1String("title"))
            inTitle = false;
        else if (inTitle && token.kind == HtmlToken::Text && title && title->isEmpty())
            *title = collapseWhitespace(decodeEntities(token.text));
        if (dropDepth > 0) {
            if (token.kind == HtmlToken::Start && token.name == dropTag && !token.selfClosing)
                ++dropDepth;
            else if (token.kind == HtmlToken::End && token.name == dropTag)
                --dropDepth;
            continue;
        }
        if (token.kind == HtmlToken::Text) {
            if (token.raw)
                continue;
            DomNode node;
            node.name = QStringLiteral("#text");
            node.text = token.text;
            node.parent = stack.last();
            nodes.append(node);
            nodes[stack.last()].children.append(int(nodes.size() - 1));
            continue;
        }
        if (token.kind == HtmlToken::Start) {
            if (readerDropped(token.name)) {
                if (!token.selfClosing && !voidTags().contains(token.name)) {
                    dropTag = token.name;
                    dropDepth = 1;
                }
                continue;
            }
            if (stack.size() > 1) {
                const QString &top = nodes.at(stack.last()).name;
                if ((top == QLatin1String("p") && closesParagraph(token.name)) ||
                    (top == QLatin1String("li") && token.name == QLatin1String("li")))
                    stack.removeLast();
            }
            DomNode node;
            node.name = token.name;
            node.attributes = token.attributes;
            node.parent = stack.last();
            nodes.append(node);
            const int index = int(nodes.size() - 1);
            nodes[stack.last()].children.append(index);
            if (!voidTags().contains(token.name) && !token.selfClosing && stack.size() < 512)
                stack.append(index);
            continue;
        }
        for (int at = int(stack.size()) - 1; at > 0; --at) {
            if (nodes.at(stack.at(at)).name == token.name) {
                stack.resize(at);
                break;
            }
        }
    }
    return nodes;
}

int textLength(const QList<DomNode> &nodes, int index, QList<int> &memo)
{
    if (memo.at(index) >= 0)
        return memo.at(index);
    const DomNode &node = nodes.at(index);
    int total = 0;
    if (node.name == QLatin1String("#text")) {
        total = int(collapseWhitespace(node.text).size());
    } else {
        for (const int child : node.children)
            total += textLength(nodes, child, memo);
    }
    memo[index] = total;
    return total;
}

void serializeNode(const QList<DomNode> &nodes, int index, QString &out)
{
    const DomNode &node = nodes.at(index);
    if (node.name == QLatin1String("#text")) {
        out += escapeHtml(node.text);
        return;
    }
    const bool element = !node.name.startsWith(QLatin1Char('#'));
    if (element) {
        out += QLatin1Char('<') + node.name;
        for (const auto &attribute : node.attributes)
            out += QLatin1Char(' ') + attribute.first + QStringLiteral("=\"") +
                   escapeHtml(attribute.second) + QLatin1Char('"');
        out += QLatin1Char('>');
    }
    for (const int child : node.children)
        serializeNode(nodes, child, out);
    if (element && !voidTags().contains(node.name))
        out += QStringLiteral("</") + node.name + QLatin1Char('>');
}

qint64 parseIso(QString text)
{
    text = text.trimmed();
    if (text.size() == 10) {
        const QDate date = QDate::fromString(text, Qt::ISODate);
        return date.isValid() ? QDateTime(date, QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch() : -1;
    }
    if (text.size() > 10 && (text.at(10) == QLatin1Char(' ') || text.at(10) == QLatin1Char('t')))
        text[10] = QLatin1Char('T');
    if (text.endsWith(QLatin1Char('z')))
        text[text.size() - 1] = QLatin1Char('Z');
    static const QRegularExpression fraction(QStringLiteral("(\\.\\d{3})\\d+"));
    text.replace(fraction, QStringLiteral("\\1"));
    static const QRegularExpression compactZone(QStringLiteral("([+-]\\d{2})(\\d{2})$"));
    const qsizetype timeStart = text.indexOf(QLatin1Char('T'));
    if (timeStart > 0) {
        QString time = text.mid(timeStart);
        time.replace(compactZone, QStringLiteral("\\1:\\2"));
        text = text.left(timeStart) + time;
    }
    static const QRegularExpression zone(QStringLiteral("(Z|[+-]\\d{2}(:\\d{2})?)$"));
    const bool hasZone = timeStart > 0 && zone.match(text.mid(timeStart)).hasMatch();
    QDateTime value = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!value.isValid())
        value = QDateTime::fromString(text, Qt::ISODate);
    if (!value.isValid())
        return -1;
    if (!hasZone)
        value = QDateTime(value.date(), value.time(), QTimeZone::UTC);
    return value.toMSecsSinceEpoch();
}

int zoneOffsetSeconds(const QString &zone)
{
    if (zone.isEmpty())
        return 0;
    if (zone.startsWith(QLatin1Char('+')) || zone.startsWith(QLatin1Char('-'))) {
        QString digitsOnly = zone.mid(1);
        digitsOnly.remove(QLatin1Char(':'));
        if (digitsOnly.size() != 4)
            return 0;
        const int hours = digitsOnly.left(2).toInt();
        const int minutes = digitsOnly.mid(2).toInt();
        const int seconds = hours * 3600 + minutes * 60;
        return zone.startsWith(QLatin1Char('-')) ? -seconds : seconds;
    }
    static const QHash<QString, int> named{
        {QStringLiteral("EST"), -5}, {QStringLiteral("EDT"), -4}, {QStringLiteral("CST"), -6},
        {QStringLiteral("CDT"), -5}, {QStringLiteral("MST"), -7}, {QStringLiteral("MDT"), -6},
        {QStringLiteral("PST"), -8}, {QStringLiteral("PDT"), -7}};
    return named.value(zone.toUpper(), 0) * 3600;
}

qint64 parseRfc822(const QString &text)
{
    static const QRegularExpression pattern(
        QStringLiteral("^(?:[A-Za-z]+\\.?,?\\s*)?(\\d{1,2})[\\s-]+([A-Za-z]+)\\.?[\\s-]+(\\d{2,4})"
                       "(?:[\\sT]+(\\d{1,2}):(\\d{2})(?::(\\d{2}))?(?:\\.\\d+)?)?\\s*"
                       "([A-Za-z]{1,5}|[+-]\\d{2}:?\\d{2})?"));
    const QRegularExpressionMatch match = pattern.match(text.trimmed());
    if (!match.hasMatch())
        return -1;
    static const QStringList months{
        QStringLiteral("jan"), QStringLiteral("feb"), QStringLiteral("mar"), QStringLiteral("apr"),
        QStringLiteral("may"), QStringLiteral("jun"), QStringLiteral("jul"), QStringLiteral("aug"),
        QStringLiteral("sep"), QStringLiteral("oct"), QStringLiteral("nov"), QStringLiteral("dec")};
    const int month = int(months.indexOf(match.captured(2).left(3).toLower())) + 1;
    if (month <= 0)
        return -1;
    int year = match.captured(3).toInt();
    if (match.captured(3).size() == 2)
        year += year < 50 ? 2000 : 1900;
    const QDate date(year, month, match.captured(1).toInt());
    const QTime time(match.captured(4).isEmpty() ? 0 : match.captured(4).toInt(),
                     match.captured(5).isEmpty() ? 0 : match.captured(5).toInt(),
                     match.captured(6).isEmpty() ? 0 : match.captured(6).toInt());
    if (!date.isValid() || !time.isValid())
        return -1;
    const QDateTime utc(date, time, QTimeZone::UTC);
    return utc.toMSecsSinceEpoch() - qint64(zoneOffsetSeconds(match.captured(7))) * 1000;
}

QJsonArray enclosuresJson(const QList<Enclosure> &list)
{
    QJsonArray array;
    for (const Enclosure &enclosure : list)
        array.append(QJsonObject{{QStringLiteral("u"), enclosure.url},
                                 {QStringLiteral("t"), enclosure.type},
                                 {QStringLiteral("n"), double(enclosure.length)}});
    return array;
}

QList<Enclosure> enclosuresFrom(const QJsonArray &array)
{
    QList<Enclosure> list;
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        Enclosure enclosure;
        enclosure.url = object.value(QStringLiteral("u")).toString();
        enclosure.type = object.value(QStringLiteral("t")).toString();
        enclosure.length = qint64(object.value(QStringLiteral("n")).toDouble());
        if (!enclosure.url.isEmpty())
            list.append(enclosure);
    }
    return list;
}

QJsonObject articleJson(const Article &article)
{
    QJsonObject object{
        {QStringLiteral("g"), article.guid},
        {QStringLiteral("t"), article.title},
        {QStringLiteral("l"), article.link},
        {QStringLiteral("a"), article.author},
        {QStringLiteral("h"), article.html},
        {QStringLiteral("s"), article.summary},
        {QStringLiteral("c"), article.comments},
        {QStringLiteral("p"), double(article.publishedMs)},
        {QStringLiteral("f"), double(article.firstSeenMs)},
        {QStringLiteral("u"), double(article.updatedMs)},
        {QStringLiteral("d"), article.datedByFeed},
        {QStringLiteral("rd"), article.read},
        {QStringLiteral("st"), article.starred},
    };
    if (!article.readerHtml.isEmpty())
        object.insert(QStringLiteral("r"), article.readerHtml);
    if (!article.enclosures.isEmpty())
        object.insert(QStringLiteral("e"), enclosuresJson(article.enclosures));
    return object;
}

Article articleFrom(const QString &feedId, const QJsonObject &object)
{
    Article article;
    article.feedId = feedId;
    article.guid = object.value(QStringLiteral("g")).toString();
    article.title = object.value(QStringLiteral("t")).toString();
    article.link = object.value(QStringLiteral("l")).toString();
    article.author = object.value(QStringLiteral("a")).toString();
    article.html = object.value(QStringLiteral("h")).toString();
    article.summary = object.value(QStringLiteral("s")).toString();
    article.comments = object.value(QStringLiteral("c")).toString();
    article.readerHtml = object.value(QStringLiteral("r")).toString();
    article.enclosures = enclosuresFrom(object.value(QStringLiteral("e")).toArray());
    article.publishedMs = qint64(object.value(QStringLiteral("p")).toDouble());
    article.firstSeenMs = qint64(object.value(QStringLiteral("f")).toDouble());
    article.updatedMs = qint64(object.value(QStringLiteral("u")).toDouble());
    article.datedByFeed = object.value(QStringLiteral("d")).toBool();
    article.read = object.value(QStringLiteral("rd")).toBool();
    article.starred = object.value(QStringLiteral("st")).toBool();
    return article;
}

QJsonArray logJson(const QList<LogEntry> &log)
{
    QJsonArray array;
    for (const LogEntry &entry : log)
        array.append(QJsonObject{{QStringLiteral("at"), double(entry.atMs)},
                                 {QStringLiteral("text"), entry.text}});
    return array;
}

QList<LogEntry> logFrom(const QJsonArray &array)
{
    QList<LogEntry> log;
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        log.append(LogEntry{qint64(object.value(QStringLiteral("at")).toDouble()),
                            object.value(QStringLiteral("text")).toString()});
        if (log.size() >= kMaxLogEntries)
            break;
    }
    return log;
}

void appendLog(QList<LogEntry> &log, qint64 nowMs, const QString &text)
{
    log.prepend(LogEntry{nowMs, text});
    while (log.size() > kMaxLogEntries)
        log.removeLast();
}

bool writeFile(const QString &path, const QByteArray &bytes, QString *error)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

}

QString forkMeshBlogFeed()
{
    return QStringLiteral("https://forkmesh.com/blog/rss.xml");
}

QString Feed::displayName() const
{
    if (!customTitle.trimmed().isEmpty())
        return customTitle.trimmed();
    if (!title.trimmed().isEmpty())
        return title.trimmed();
    const QUrl parsed(url);
    return parsed.host().isEmpty() ? url : parsed.host();
}

QJsonObject Settings::toJson() const
{
    return QJsonObject{
        {QStringLiteral("oldestFirst"), oldestFirst},
        {QStringLiteral("groupByFeed"), groupByFeed},
        {QStringLiteral("refreshClearsRead"), refreshClearsRead},
        {QStringLiteral("confirmMarkAllRead"), confirmMarkAllRead},
        {QStringLiteral("summaryLines"), summaryLines},
        {QStringLiteral("showFeedIcons"), showFeedIcons},
        {QStringLiteral("theme"), theme},
        {QStringLiteral("fontFamily"), fontFamily},
        {QStringLiteral("textSize"), textSize},
        {QStringLiteral("loadRemoteImages"), loadRemoteImages},
        {QStringLiteral("unreadOnlyFeeds"), unreadOnlyFeeds},
        {QStringLiteral("unreadOnlyTimeline"), unreadOnlyTimeline},
    };
}

Settings Settings::fromJson(const QJsonObject &object)
{
    Settings settings;
    auto flag = [&object](const char *key, bool fallback) {
        const QJsonValue value = object.value(QLatin1String(key));
        return value.isBool() ? value.toBool() : fallback;
    };
    settings.oldestFirst = flag("oldestFirst", settings.oldestFirst);
    settings.groupByFeed = flag("groupByFeed", settings.groupByFeed);
    settings.refreshClearsRead = flag("refreshClearsRead", settings.refreshClearsRead);
    settings.confirmMarkAllRead = flag("confirmMarkAllRead", settings.confirmMarkAllRead);
    settings.showFeedIcons = flag("showFeedIcons", settings.showFeedIcons);
    settings.loadRemoteImages = flag("loadRemoteImages", settings.loadRemoteImages);
    settings.unreadOnlyFeeds = flag("unreadOnlyFeeds", settings.unreadOnlyFeeds);
    settings.unreadOnlyTimeline = flag("unreadOnlyTimeline", settings.unreadOnlyTimeline);
    settings.summaryLines =
        qBound(0, object.value(QStringLiteral("summaryLines")).toInt(settings.summaryLines), 4);
    settings.textSize =
        qBound(10, object.value(QStringLiteral("textSize")).toInt(settings.textSize), 32);
    const QString theme = object.value(QStringLiteral("theme")).toString();
    static const QStringList themes{QStringLiteral("default"), QStringLiteral("sepia"),
                                    QStringLiteral("paper"), QStringLiteral("contrast")};
    if (themes.contains(theme))
        settings.theme = theme;
    settings.fontFamily = object.value(QStringLiteral("fontFamily")).toString().left(100);
    return settings;
}

QString escapeHtml(const QString &text)
{
    QString out;
    out.reserve(text.size() + 16);
    for (const QChar c : text) {
        switch (c.unicode()) {
        case '&':
            out += QStringLiteral("&amp;");
            break;
        case '<':
            out += QStringLiteral("&lt;");
            break;
        case '>':
            out += QStringLiteral("&gt;");
            break;
        case '"':
            out += QStringLiteral("&quot;");
            break;
        default:
            out += c;
        }
    }
    return out;
}

QString decodeEntities(const QString &text)
{
    if (!text.contains(QLatin1Char('&')))
        return text;
    QString out;
    out.reserve(text.size());
    const qsizetype n = text.size();
    qsizetype i = 0;
    while (i < n) {
        const QChar c = text.at(i);
        if (c != QLatin1Char('&')) {
            out += c;
            ++i;
            continue;
        }
        qsizetype k = i + 1;
        if (k < n && text.at(k) == QLatin1Char('#')) {
            ++k;
            const bool hex = k < n && (text.at(k) == QLatin1Char('x') || text.at(k) == QLatin1Char('X'));
            if (hex)
                ++k;
            const qsizetype start = k;
            while (k < n && k - start < 8 &&
                   (hex ? isxdigit(text.at(k).toLatin1()) : text.at(k).isDigit()))
                ++k;
            if (k > start) {
                bool ok = false;
                const qint64 code = text.mid(start, k - start).toLongLong(&ok, hex ? 16 : 10);
                if (k < n && text.at(k) == QLatin1Char(';'))
                    ++k;
                const bool usable = ok && code > 0 && code <= 0x10FFFF && !(code >= 0xD800 && code <= 0xDFFF);
                const char32_t value = usable ? char32_t(code) : char32_t(0xFFFD);
                out += QString::fromUcs4(&value, 1);
                i = k;
                continue;
            }
            out += c;
            ++i;
            continue;
        }
        const qsizetype start = k;
        while (k < n && k - start < 32 && text.at(k).unicode() < 128 &&
               isAsciiAlnum(char(text.at(k).unicode())))
            ++k;
        const QString name = text.mid(start, k - start);
        const auto found = entityTable().constFind(name);
        if (!name.isEmpty() && found != entityTable().constEnd()) {
            const bool terminated = k < n && text.at(k) == QLatin1Char(';');
            static const QSet<QString> loose{QStringLiteral("amp"), QStringLiteral("lt"),
                                             QStringLiteral("gt"), QStringLiteral("quot"),
                                             QStringLiteral("nbsp")};
            if (terminated || loose.contains(name)) {
                const char32_t value = found.value();
                out += QString::fromUcs4(&value, 1);
                i = terminated ? k + 1 : k;
                continue;
            }
        }
        out += c;
        ++i;
    }
    return out;
}

QString collapseWhitespace(const QString &text)
{
    return text.simplified();
}

QByteArray prepareXml(const QByteArray &data)
{
    qsizetype begin = 0;
    if (data.startsWith("\xEF\xBB\xBF"))
        begin = 3;
    while (begin < data.size() && (data.at(begin) == ' ' || data.at(begin) == '\n' ||
                                   data.at(begin) == '\r' || data.at(begin) == '\t'))
        ++begin;
    const QByteArray in = data.mid(begin);
    QByteArray out;
    out.reserve(in.size() + 64);
    const int n = int(in.size());
    int i = 0;
    while (i < n) {
        const char c = in.at(i);
        if (c == '<') {
            if (startsWithAt(in, i, "<![CDATA[")) {
                int end = int(in.indexOf("]]>", i + 9));
                end = end < 0 ? n : end + 3;
                out.append(in.mid(i, end - i));
                i = end;
                continue;
            }
            if (startsWithAt(in, i, "<!--")) {
                int end = int(in.indexOf("-->", i + 4));
                end = end < 0 ? n : end + 3;
                out.append(in.mid(i, end - i));
                i = end;
                continue;
            }
            if (startsWithAt(in, i, "<!DOCTYPE", true)) {
                int depth = 0;
                int k = i + 9;
                while (k < n) {
                    const char d = in.at(k);
                    if (d == '[')
                        ++depth;
                    else if (d == ']')
                        --depth;
                    else if (d == '>' && depth <= 0)
                        break;
                    ++k;
                }
                i = k + 1;
                continue;
            }
            out.append(c);
            ++i;
            continue;
        }
        if (c == '&') {
            int k = i + 1;
            if (k < n && in.at(k) == '#') {
                ++k;
                const bool hex = k < n && (in.at(k) == 'x' || in.at(k) == 'X');
                if (hex)
                    ++k;
                const int start = k;
                while (k < n && k - start < 8 && (hex ? isxdigit(uchar(in.at(k))) : isdigit(uchar(in.at(k)))))
                    ++k;
                if (k > start && k < n && in.at(k) == ';') {
                    bool ok = false;
                    const qint64 code = in.mid(start, k - start).toLongLong(&ok, hex ? 16 : 10);
                    if (ok && validXmlChar(code))
                        out.append(in.mid(i, k + 1 - i));
                    else
                        out.append("&#xFFFD;");
                    i = k + 1;
                    continue;
                }
                out.append("&amp;");
                ++i;
                continue;
            }
            const int start = k;
            while (k < n && k - start < 32 && isAsciiAlnum(in.at(k)))
                ++k;
            if (k > start && k < n && in.at(k) == ';') {
                const QByteArray name = in.mid(start, k - start);
                if (name == "amp" || name == "lt" || name == "gt" || name == "quot" || name == "apos") {
                    out.append(in.mid(i, k + 1 - i));
                } else {
                    const auto found = entityTable().constFind(QString::fromLatin1(name));
                    if (found != entityTable().constEnd())
                        out.append("&#" + QByteArray::number(quint32(found.value())) + ';');
                    else
                        out.append("&amp;" + name + ';');
                }
                i = k + 1;
                continue;
            }
            out.append("&amp;");
            ++i;
            continue;
        }
        if (uchar(c) < 0x20 && c != '\t' && c != '\n' && c != '\r') {
            ++i;
            continue;
        }
        out.append(c);
        ++i;
    }
    return out;
}

qint64 parseDate(const QString &raw)
{
    const QString text = raw.trimmed();
    if (text.isEmpty())
        return -1;
    if (text.size() >= 10 && text.at(0).isDigit() && text.at(4) == QLatin1Char('-')) {
        return parseIso(text);
    }
    qint64 value = parseRfc822(text);
    if (value >= 0)
        return value;
    const QDateTime rfc = QDateTime::fromString(text, Qt::RFC2822Date);
    if (rfc.isValid())
        return rfc.toMSecsSinceEpoch();
    return parseIso(text);
}

QString makeGuid(const QString &id, const QString &link, const QString &title, qint64 publishedMs)
{
    QString guid = collapseWhitespace(id);
    if (guid.isEmpty())
        guid = collapseWhitespace(link);
    if (guid.isEmpty()) {
        const QString name = collapseWhitespace(title);
        if (!name.isEmpty())
            guid = publishedMs >= 0 ? name + QLatin1Char('|') + QString::number(publishedMs) : name;
    }
    return guid.left(kMaxGuidLength);
}

ParsedFeed parseFeed(const QByteArray &data, const QUrl &base)
{
    qsizetype first = 0;
    if (data.startsWith("\xEF\xBB\xBF"))
        first = 3;
    while (first < data.size() && isspace(uchar(data.at(first))))
        ++first;
    if (first < data.size() && (data.at(first) == '{' || data.at(first) == '['))
        return parseJsonFeed(data.mid(first), base);

    ParsedFeed feed;
    QXmlStreamReader xml(prepareXml(data));
    xml.setNamespaceProcessing(false);
    if (!xml.readNextStartElement()) {
        feed.error = xml.hasError() ? QStringLiteral("The feed is not valid XML: %1").arg(xml.errorString())
                                    : QStringLiteral("The address did not return a feed.");
        return feed;
    }
    const QStringView root = localOf(xml.qualifiedName());
    if (root == u"rss") {
        feed.format = QStringLiteral("RSS");
        while (xml.readNextStartElement()) {
            if (localOf(xml.qualifiedName()) == u"channel")
                parseRssChannel(xml, feed, base, false);
            else
                xml.skipCurrentElement();
        }
    } else if (root == u"RDF") {
        feed.format = QStringLiteral("RSS 1.0");
        while (xml.readNextStartElement()) {
            const QStringView local = localOf(xml.qualifiedName());
            if (local == u"channel") {
                while (xml.readNextStartElement()) {
                    const QStringView child = localOf(xml.qualifiedName());
                    if (child == u"title")
                        feed.title = plainText(readInner(xml, false), 300);
                    else if (child == u"link")
                        feed.homePage = resolveUrl(base, readText(xml));
                    else
                        xml.skipCurrentElement();
                }
            } else if (local == u"image") {
                parseRssImage(xml, feed, base);
            } else if (local == u"item") {
                if (feed.items.size() < kMaxItemsPerFetch) {
                    ParsedItem item;
                    parseRssItem(xml, item, base, true);
                    feed.items.append(item);
                } else {
                    xml.skipCurrentElement();
                }
            } else {
                xml.skipCurrentElement();
            }
        }
    } else if (root == u"feed") {
        feed.format = QStringLiteral("Atom");
        parseAtomFeed(xml, feed, base);
    } else {
        feed.error = QStringLiteral("The address did not return a feed.");
        return feed;
    }
    if (xml.hasError() && feed.items.isEmpty() && feed.title.isEmpty()) {
        feed.error = QStringLiteral("The feed is not valid XML: %1").arg(xml.errorString());
        return feed;
    }
    feed.ok = true;
    return feed;
}

bool looksLikeHtml(const QByteArray &data, const QString &contentType)
{
    const QString type = contentType.toLower();
    if (type.contains(QStringLiteral("text/html")) || type.contains(QStringLiteral("xhtml")))
        return true;
    const QByteArray head = data.left(2048).toLower();
    return head.contains("<!doctype html") || head.contains("<html");
}

QList<QUrl> discoverFeedLinks(const QString &html, const QUrl &base)
{
    QUrl documentBase = base;
    QList<QPair<int, QUrl>> found;
    for (const HtmlToken &token : tokenizeHtml(html)) {
        if (token.kind != HtmlToken::Start)
            continue;
        if (token.name == QLatin1String("base")) {
            for (const auto &attribute : token.attributes) {
                if (attribute.first == QLatin1String("href")) {
                    const QUrl candidate(resolveUrl(base, attribute.second));
                    if (candidate.isValid())
                        documentBase = candidate;
                }
            }
            continue;
        }
        if (token.name == QLatin1String("body"))
            break;
        if (token.name != QLatin1String("link"))
            continue;
        QString rel;
        QString type;
        QString href;
        for (const auto &attribute : token.attributes) {
            if (attribute.first == QLatin1String("rel"))
                rel = attribute.second.toLower();
            else if (attribute.first == QLatin1String("type"))
                type = attribute.second.trimmed().toLower();
            else if (attribute.first == QLatin1String("href"))
                href = attribute.second;
        }
        if (!rel.split(QLatin1Char(' '), Qt::SkipEmptyParts).contains(QStringLiteral("alternate")))
            continue;
        int rank = -1;
        if (type == QLatin1String("application/rss+xml") || type == QLatin1String("application/rdf+xml"))
            rank = 0;
        else if (type == QLatin1String("application/atom+xml"))
            rank = 1;
        else if (type == QLatin1String("application/feed+json") || type == QLatin1String("application/json"))
            rank = 2;
        if (rank < 0)
            continue;
        const QString url = safeUrl(documentBase, href, false);
        if (!url.isEmpty())
            found.append({rank, QUrl(url)});
    }
    std::stable_sort(found.begin(), found.end(),
                     [](const auto &a, const auto &b) { return a.first < b.first; });
    QList<QUrl> links;
    for (const auto &entry : found) {
        if (!links.contains(entry.second))
            links.append(entry.second);
        if (links.size() >= kMaxDiscoveryLinks)
            break;
    }
    return links;
}

QList<QUrl> fallbackFeedUrls(const QUrl &address)
{
    if (!address.isValid() || address.host().isEmpty())
        return {};
    QUrl origin;
    origin.setScheme(address.scheme());
    origin.setHost(address.host());
    origin.setPort(address.port());
    QList<QUrl> list;
    for (const char *path : {"/feed", "/rss", "/feed.xml", "/atom.xml"}) {
        QUrl candidate = origin;
        candidate.setPath(QString::fromLatin1(path));
        list.append(candidate);
    }
    return list;
}

QUrl normalizeAddress(const QString &input)
{
    QString text = input.trimmed();
    if (text.isEmpty())
        return {};
    const QString lower = text.toLower();
    if (lower.startsWith(QStringLiteral("feed://"))) {
        text = QStringLiteral("https://") + text.mid(7);
    } else if (lower.startsWith(QStringLiteral("feed:"))) {
        QString rest = text.mid(5);
        if (rest.startsWith(QStringLiteral("//")))
            rest = rest.mid(2);
        const QString restLower = rest.toLower();
        text = (restLower.startsWith(QStringLiteral("http://")) ||
                restLower.startsWith(QStringLiteral("https://")))
                   ? rest
                   : QStringLiteral("https://") + rest;
    }
    if (!text.contains(QStringLiteral("://")))
        text = QStringLiteral("https://") + text;
    QUrl url(text, QUrl::TolerantMode);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || url.host().isEmpty() ||
        (scheme != QLatin1String("http") && scheme != QLatin1String("https")))
        return {};
    url.setScheme(scheme);
    url.setHost(url.host().toLower());
    url.setFragment(QString());
    if (url.path().isEmpty())
        url.setPath(QStringLiteral("/"));
    return url;
}

QString feedKey(const QUrl &url)
{
    QString path = url.path(QUrl::FullyEncoded);
    if (path.isEmpty())
        path = QStringLiteral("/");
    return url.host().toLower() + QLatin1Char(':') + QString::number(url.port(-1)) + path +
           QLatin1Char('?') + url.query(QUrl::FullyEncoded);
}

QString sanitizeHtml(const QString &html, const QUrl &base)
{
    const QList<HtmlToken> tokens = tokenizeHtml(html);
    QString out;
    out.reserve(html.size());
    QStringList open;
    QString dropTag;
    int dropDepth = 0;
    for (const HtmlToken &token : tokens) {
        if (dropDepth > 0) {
            if (token.kind == HtmlToken::Start && token.name == dropTag && !token.selfClosing)
                ++dropDepth;
            else if (token.kind == HtmlToken::End && token.name == dropTag)
                --dropDepth;
            continue;
        }
        if (token.kind == HtmlToken::Text) {
            if (!token.raw)
                out += escapeHtml(token.text);
            continue;
        }
        if (token.kind == HtmlToken::Start) {
            if (droppedWithContent().contains(token.name)) {
                if (token.name == QLatin1String("iframe")) {
                    for (const auto &attribute : token.attributes) {
                        if (attribute.first != QLatin1String("src"))
                            continue;
                        const QString url = safeUrl(base, attribute.second, false);
                        if (!url.isEmpty())
                            out += QStringLiteral("<p><a href=\"%1\">Embedded content: %2</a></p>")
                                       .arg(escapeHtml(url), escapeHtml(QUrl(url).host()));
                        break;
                    }
                }
                if (!token.selfClosing && !voidTags().contains(token.name)) {
                    dropTag = token.name;
                    dropDepth = 1;
                }
                continue;
            }
            if (!allowedTags().contains(token.name))
                continue;
            bool valid = true;
            const QString attributes = sanitizedAttributes(token.name, token.attributes, base, &valid);
            if (!valid)
                continue;
            out += QLatin1Char('<') + token.name + attributes + QLatin1Char('>');
            if (!voidTags().contains(token.name)) {
                if (token.selfClosing)
                    out += QStringLiteral("</") + token.name + QLatin1Char('>');
                else
                    open.append(token.name);
            }
            continue;
        }
        if (!allowedTags().contains(token.name) || voidTags().contains(token.name))
            continue;
        const qsizetype at = open.lastIndexOf(token.name);
        if (at < 0)
            continue;
        while (open.size() > at) {
            out += QStringLiteral("</") + open.takeLast() + QLatin1Char('>');
        }
    }
    while (!open.isEmpty())
        out += QStringLiteral("</") + open.takeLast() + QLatin1Char('>');
    return out;
}

QString plainText(const QString &html, int maxLength)
{
    static const QSet<QString> blocks{
        QStringLiteral("br"), QStringLiteral("p"), QStringLiteral("div"), QStringLiteral("li"),
        QStringLiteral("h1"), QStringLiteral("h2"), QStringLiteral("h3"), QStringLiteral("h4"),
        QStringLiteral("h5"), QStringLiteral("h6"), QStringLiteral("tr"), QStringLiteral("td"),
        QStringLiteral("th"), QStringLiteral("blockquote"), QStringLiteral("pre"),
        QStringLiteral("hr"), QStringLiteral("figcaption"), QStringLiteral("dt"), QStringLiteral("dd")};
    if (!html.contains(QLatin1Char('<')) && !html.contains(QLatin1Char('&')))
        return collapseWhitespace(html).left(maxLength);
    QString out;
    QString dropTag;
    int dropDepth = 0;
    for (const HtmlToken &token : tokenizeHtml(html)) {
        if (dropDepth > 0) {
            if (token.kind == HtmlToken::Start && token.name == dropTag && !token.selfClosing)
                ++dropDepth;
            else if (token.kind == HtmlToken::End && token.name == dropTag)
                --dropDepth;
            continue;
        }
        if (token.kind == HtmlToken::Text) {
            if (!token.raw)
                out += token.text;
        } else if (token.kind == HtmlToken::Start && droppedWithContent().contains(token.name)) {
            if (!token.selfClosing && !voidTags().contains(token.name)) {
                dropTag = token.name;
                dropDepth = 1;
            }
        } else if (blocks.contains(token.name)) {
            out += QLatin1Char(' ');
        }
        if (out.size() > maxLength * 2 + 200)
            break;
    }
    return collapseWhitespace(out).left(maxLength);
}

Readable extractReadable(const QString &html, const QUrl &base)
{
    Readable result;
    const QList<DomNode> nodes = buildDom(tokenizeHtml(html), &result.title);
    QList<int> memo(nodes.size(), -1);
    auto best = [&](const QString &name) {
        int chosen = -1;
        int length = 0;
        for (int i = 0; i < nodes.size(); ++i) {
            if (nodes.at(i).name != name)
                continue;
            const int size = textLength(nodes, i, memo);
            if (size > length) {
                chosen = i;
                length = size;
            }
        }
        return std::make_pair(chosen, length);
    };
    static const QSet<QString> scored{QStringLiteral("p"), QStringLiteral("pre"),
                                      QStringLiteral("blockquote"), QStringLiteral("li"),
                                      QStringLiteral("h2"), QStringLiteral("h3")};
    int scoredNode = -1;
    int topScore = 0;
    for (int i = 0; i < nodes.size(); ++i) {
        if (nodes.at(i).name.startsWith(QLatin1Char('#')))
            continue;
        int score = 0;
        for (const int child : nodes.at(i).children) {
            if (scored.contains(nodes.at(child).name))
                score += textLength(nodes, child, memo);
        }
        if (score > topScore) {
            topScore = score;
            scoredNode = i;
        }
    }
    int chosen = -1;
    const auto article = best(QStringLiteral("article"));
    const auto main = best(QStringLiteral("main"));
    if (article.first >= 0 && (article.second >= 200 || scoredNode < 0))
        chosen = article.first;
    else if (main.first >= 0 && (main.second >= 200 || scoredNode < 0))
        chosen = main.first;
    else if (scoredNode >= 0)
        chosen = scoredNode;
    else
        chosen = best(QStringLiteral("body")).first;
    if (chosen < 0)
        chosen = 0;
    QString serialized;
    for (const int child : nodes.at(chosen).children)
        serializeNode(nodes, child, serialized);
    result.html = sanitizeHtml(serialized, base);
    return result;
}

OpmlDocument parseOpml(const QByteArray &data)
{
    OpmlDocument document;
    QXmlStreamReader xml(prepareXml(data));
    xml.setNamespaceProcessing(false);
    if (!xml.readNextStartElement() || localOf(xml.qualifiedName()) != u"opml") {
        document.error = QStringLiteral("This file is not an OPML subscription list.");
        return document;
    }
    QStringList stack;
    QString topFolder;
    while (!xml.atEnd()) {
        const QXmlStreamReader::TokenType token = xml.readNext();
        if (token == QXmlStreamReader::StartElement && localOf(xml.qualifiedName()) == u"outline") {
            const QXmlStreamAttributes attributes = xml.attributes();
            QString title = collapseWhitespace(attributeValue(attributes, u"title"));
            if (title.isEmpty())
                title = collapseWhitespace(attributeValue(attributes, u"text"));
            const QString xmlUrl = attributeValue(attributes, u"xmlUrl").trimmed();
            if (!xmlUrl.isEmpty()) {
                document.feeds.append(OpmlEntry{title, xmlUrl,
                                                attributeValue(attributes, u"htmlUrl").trimmed(),
                                                topFolder});
                stack.append(QString());
            } else {
                if (topFolder.isEmpty() && !title.isEmpty()) {
                    topFolder = title;
                    if (!document.folders.contains(title))
                        document.folders.append(title);
                    stack.append(title);
                } else {
                    stack.append(QString());
                }
            }
        } else if (token == QXmlStreamReader::EndElement && localOf(xml.qualifiedName()) == u"outline") {
            if (!stack.isEmpty()) {
                const QString closed = stack.takeLast();
                if (!closed.isEmpty() && closed == topFolder)
                    topFolder.clear();
            }
        }
    }
    if (xml.hasError() && document.feeds.isEmpty()) {
        document.error = QStringLiteral("The OPML file could not be read: %1").arg(xml.errorString());
        return document;
    }
    document.ok = true;
    return document;
}

QByteArray writeOpml(const QList<Feed> &feeds, const QList<Folder> &folders, qint64 nowMs)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    QXmlStreamWriter xml(&buffer);
    xml.setAutoFormatting(true);
    xml.setAutoFormattingIndent(2);
    xml.writeStartDocument();
    xml.writeStartElement(QStringLiteral("opml"));
    xml.writeAttribute(QStringLiteral("version"), QStringLiteral("2.0"));
    xml.writeStartElement(QStringLiteral("head"));
    xml.writeTextElement(QStringLiteral("title"), QStringLiteral("RSS Reader subscriptions"));
    xml.writeTextElement(QStringLiteral("dateCreated"),
                         QDateTime::fromMSecsSinceEpoch(nowMs, QTimeZone::UTC).toString(Qt::RFC2822Date));
    xml.writeEndElement();
    xml.writeStartElement(QStringLiteral("body"));
    auto byName = [](const Feed *a, const Feed *b) {
        return a->displayName().compare(b->displayName(), Qt::CaseInsensitive) < 0;
    };
    auto writeFeed = [&xml](const Feed &feed) {
        xml.writeEmptyElement(QStringLiteral("outline"));
        xml.writeAttribute(QStringLiteral("type"), QStringLiteral("rss"));
        xml.writeAttribute(QStringLiteral("text"), feed.displayName());
        xml.writeAttribute(QStringLiteral("title"), feed.displayName());
        xml.writeAttribute(QStringLiteral("xmlUrl"), feed.url);
        if (!feed.homePage.isEmpty())
            xml.writeAttribute(QStringLiteral("htmlUrl"), feed.homePage);
    };
    QList<Folder> sortedFolders = folders;
    std::sort(sortedFolders.begin(), sortedFolders.end(), [](const Folder &a, const Folder &b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    for (const Folder &folder : sortedFolders) {
        xml.writeStartElement(QStringLiteral("outline"));
        xml.writeAttribute(QStringLiteral("text"), folder.name);
        xml.writeAttribute(QStringLiteral("title"), folder.name);
        QList<const Feed *> inside;
        for (const Feed &feed : feeds) {
            if (feed.folder == folder.name)
                inside.append(&feed);
        }
        std::sort(inside.begin(), inside.end(), byName);
        for (const Feed *feed : inside)
            writeFeed(*feed);
        xml.writeEndElement();
    }
    QList<const Feed *> top;
    for (const Feed &feed : feeds) {
        bool known = false;
        for (const Folder &folder : folders)
            known = known || folder.name == feed.folder;
        if (feed.folder.isEmpty() || !known)
            top.append(&feed);
    }
    std::sort(top.begin(), top.end(), byName);
    for (const Feed *feed : top)
        writeFeed(*feed);
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndDocument();
    return bytes;
}

qint64 backoffMs(int failures)
{
    qint64 delay = kCheckIntervalMs;
    for (int i = 0; i < failures && delay < kMaxBackoffMs; ++i)
        delay *= 2;
    return qMin(delay, kMaxBackoffMs);
}

QString newId()
{
    return QUuid::createUuid().toString(QUuid::Id128).left(16);
}

const Feed *Store::feed(const QString &id) const
{
    for (const Feed &candidate : m_feeds) {
        if (candidate.id == id)
            return &candidate;
    }
    return nullptr;
}

Feed *Store::mutableFeed(const QString &id)
{
    for (Feed &candidate : m_feeds) {
        if (candidate.id == id)
            return &candidate;
    }
    return nullptr;
}

const Feed *Store::feedByUrl(const QUrl &url) const
{
    const QString key = feedKey(url);
    for (const Feed &candidate : m_feeds) {
        if (feedKey(QUrl(candidate.url)) == key)
            return &candidate;
    }
    return nullptr;
}

const Folder *Store::folder(const QString &name) const
{
    for (const Folder &candidate : m_folders) {
        if (candidate.name.compare(name, Qt::CaseInsensitive) == 0)
            return &candidate;
    }
    return nullptr;
}

QStringList Store::feedIdsIn(const Source &source) const
{
    QStringList ids;
    for (const Feed &candidate : m_feeds) {
        if (source.kind == SourceKind::Feed && candidate.id != source.id)
            continue;
        if (source.kind == SourceKind::Folder && candidate.folder != source.id)
            continue;
        if (source.kind == SourceKind::None)
            continue;
        ids.append(candidate.id);
    }
    return ids;
}

QString Store::addFeed(const QUrl &url, const QString &title, const QString &folderName,
                       qint64 nowMs, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QString();
    };
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || url.host().isEmpty() ||
        (scheme != QLatin1String("http") && scheme != QLatin1String("https")))
        return fail(QStringLiteral("That is not a web address."));
    if (m_feeds.size() >= kMaxFeeds)
        return fail(QStringLiteral("RSS Reader follows at most %1 feeds.").arg(kMaxFeeds));
    if (const Feed *existing = feedByUrl(url))
        return fail(QStringLiteral("You already follow %1.").arg(existing->displayName()));
    QString target = folderName.trimmed();
    if (!target.isEmpty()) {
        if (const Folder *known = folder(target))
            target = known->name;
        else if (!addFolder(target, nullptr))
            target.clear();
    }
    Feed added;
    added.id = newId();
    added.url = url.toString();
    added.title = collapseWhitespace(title);
    added.folder = target;
    added.addedMs = nowMs;
    m_feeds.append(added);
    m_articles.insert(added.id, {});
    m_unread.insert(added.id, 0);
    markExportDirty();
    return added.id;
}

bool Store::removeFeed(const QString &id)
{
    for (int i = 0; i < m_feeds.size(); ++i) {
        if (m_feeds.at(i).id != id)
            continue;
        m_feeds.removeAt(i);
        m_articles.remove(id);
        m_unread.remove(id);
        m_dirtyArticles.remove(id);
        markExportDirty();
        return true;
    }
    return false;
}

bool Store::renameFeed(const QString &id, const QString &title)
{
    Feed *target = mutableFeed(id);
    if (!target)
        return false;
    const QString cleaned = collapseWhitespace(title).left(300);
    target->customTitle = cleaned == target->title ? QString() : cleaned;
    markExportDirty();
    return true;
}

bool Store::moveFeed(const QString &id, const QString &folderName)
{
    Feed *target = mutableFeed(id);
    if (!target)
        return false;
    if (folderName.isEmpty()) {
        target->folder.clear();
    } else {
        const Folder *known = folder(folderName);
        if (!known)
            return false;
        target->folder = known->name;
    }
    markExportDirty();
    return true;
}

bool Store::addFolder(const QString &name, QString *error)
{
    const QString cleaned = collapseWhitespace(name).left(100);
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (cleaned.isEmpty())
        return fail(QStringLiteral("Enter a folder name."));
    if (folder(cleaned))
        return fail(QStringLiteral("A folder named %1 already exists.").arg(cleaned));
    if (m_folders.size() >= kMaxFolders)
        return fail(QStringLiteral("RSS Reader keeps at most %1 folders.").arg(kMaxFolders));
    m_folders.append(Folder{cleaned, false});
    markExportDirty();
    return true;
}

bool Store::renameFolder(const QString &from, const QString &to, QString *error)
{
    const QString cleaned = collapseWhitespace(to).left(100);
    if (cleaned.isEmpty()) {
        if (error)
            *error = QStringLiteral("Enter a folder name.");
        return false;
    }
    const Folder *clash = folder(cleaned);
    if (clash && clash->name.compare(from, Qt::CaseInsensitive) != 0) {
        if (error)
            *error = QStringLiteral("A folder named %1 already exists.").arg(cleaned);
        return false;
    }
    bool found = false;
    for (Folder &candidate : m_folders) {
        if (candidate.name == from) {
            candidate.name = cleaned;
            found = true;
        }
    }
    if (!found)
        return false;
    for (Feed &candidate : m_feeds) {
        if (candidate.folder == from)
            candidate.folder = cleaned;
    }
    markExportDirty();
    return true;
}

bool Store::deleteFolder(const QString &name)
{
    for (int i = 0; i < m_folders.size(); ++i) {
        if (m_folders.at(i).name != name)
            continue;
        m_folders.removeAt(i);
        for (Feed &candidate : m_feeds) {
            if (candidate.folder == name)
                candidate.folder.clear();
        }
        markExportDirty();
        return true;
    }
    return false;
}

void Store::setFolderCollapsed(const QString &name, bool collapsed)
{
    for (Folder &candidate : m_folders) {
        if (candidate.name == name && candidate.collapsed != collapsed) {
            candidate.collapsed = collapsed;
            m_stateDirty = true;
        }
    }
}

const QList<Article> &Store::articles(const QString &feedId) const
{
    static const QList<Article> empty;
    const auto found = m_articles.constFind(feedId);
    return found == m_articles.constEnd() ? empty : found.value();
}

const Article *Store::article(const ArticleRef &ref) const
{
    const auto found = m_articles.constFind(ref.feedId);
    if (found == m_articles.constEnd())
        return nullptr;
    for (const Article &candidate : found.value()) {
        if (candidate.guid == ref.guid)
            return &candidate;
    }
    return nullptr;
}

Article *Store::mutableArticle(const ArticleRef &ref)
{
    const auto found = m_articles.find(ref.feedId);
    if (found == m_articles.end())
        return nullptr;
    for (Article &candidate : found.value()) {
        if (candidate.guid == ref.guid)
            return &candidate;
    }
    return nullptr;
}

void Store::recount(const QString &feedId)
{
    int unread = 0;
    for (const Article &candidate : articles(feedId))
        unread += candidate.read ? 0 : 1;
    if (m_unread.value(feedId, -1) != unread)
        m_stateDirty = true;
    m_unread.insert(feedId, unread);
}

void Store::prune(QList<Article> &list)
{
    std::stable_sort(list.begin(), list.end(), [](const Article &a, const Article &b) {
        return a.publishedMs > b.publishedMs;
    });
    if (list.size() <= kMaxArticlesPerFeed)
        return;
    QList<Article> kept;
    kept.reserve(kMaxArticlesPerFeed);
    for (int i = 0; i < list.size(); ++i) {
        if (i < kMaxArticlesPerFeed || list.at(i).starred)
            kept.append(list.at(i));
    }
    list = kept;
}

int Store::merge(const QString &feedId, const ParsedFeed &parsed, qint64 nowMs)
{
    Feed *target = mutableFeed(feedId);
    if (!target || !parsed.ok)
        return 0;
    const QString title = collapseWhitespace(parsed.title);
    if (!title.isEmpty() && title != target->title) {
        target->title = title;
        markExportDirty();
    }
    if (!parsed.homePage.isEmpty() && parsed.homePage != target->homePage) {
        target->homePage = parsed.homePage;
        markExportDirty();
    }
    if (!parsed.iconUrl.isEmpty() && parsed.iconUrl != target->iconUrl) {
        target->iconUrl = parsed.iconUrl;
        m_stateDirty = true;
    }
    QList<Article> &list = m_articles[feedId];
    QHash<QString, int> index;
    for (int i = 0; i < list.size(); ++i)
        index.insert(list.at(i).guid, i);
    QSet<QString> seen;
    int added = 0;
    const QUrl feedUrl(target->url);
    for (const ParsedItem &item : parsed.items) {
        const QString guid = makeGuid(item.id, item.link, item.title, item.publishedMs);
        if (guid.isEmpty() || seen.contains(guid))
            continue;
        seen.insert(guid);
        const bool dated = item.publishedMs >= 0 && item.publishedMs <= nowMs + kFutureSlackMs;
        const QUrl link(item.link);
        const QUrl base = link.isValid() && !link.host().isEmpty() ? link : feedUrl;
        const QString html = sanitizeHtml(item.html, base);
        const auto existing = index.constFind(guid);
        if (existing != index.constEnd()) {
            Article &article = list[existing.value()];
            if (article.link != item.link)
                article.readerHtml.clear();
            article.title = item.title;
            article.link = item.link;
            article.author = item.author;
            article.html = html;
            article.summary = plainText(html, 600);
            article.comments = item.comments;
            article.enclosures = item.enclosures;
            if (dated) {
                article.publishedMs = item.publishedMs;
                article.datedByFeed = true;
            }
            article.updatedMs = nowMs;
            continue;
        }
        Article article;
        article.feedId = feedId;
        article.guid = guid;
        article.title = item.title;
        article.link = item.link;
        article.author = item.author;
        article.html = html;
        article.summary = plainText(html, 600);
        article.comments = item.comments;
        article.enclosures = item.enclosures;
        article.firstSeenMs = nowMs;
        article.updatedMs = nowMs;
        article.publishedMs = dated ? item.publishedMs : nowMs;
        article.datedByFeed = dated;
        list.append(article);
        index.insert(guid, int(list.size() - 1));
        ++added;
    }
    prune(list);
    recount(feedId);
    m_dirtyArticles.insert(feedId);
    return added;
}

bool Store::setRead(const ArticleRef &ref, bool read)
{
    Article *target = mutableArticle(ref);
    if (!target || target->read == read)
        return false;
    target->read = read;
    recount(ref.feedId);
    m_dirtyArticles.insert(ref.feedId);
    if (target->starred)
        m_exportDirty = true;
    return true;
}

bool Store::setStarred(const ArticleRef &ref, bool starred)
{
    Article *target = mutableArticle(ref);
    if (!target || target->starred == starred)
        return false;
    target->starred = starred;
    m_dirtyArticles.insert(ref.feedId);
    m_exportDirty = true;
    return true;
}

int Store::markRead(const QList<ArticleRef> &refs)
{
    int changed = 0;
    QSet<QString> touched;
    for (const ArticleRef &ref : refs) {
        Article *target = mutableArticle(ref);
        if (!target || target->read)
            continue;
        target->read = true;
        touched.insert(ref.feedId);
        if (target->starred)
            m_exportDirty = true;
        ++changed;
    }
    for (const QString &feedId : std::as_const(touched)) {
        recount(feedId);
        m_dirtyArticles.insert(feedId);
    }
    return changed;
}

void Store::setReaderHtml(const ArticleRef &ref, const QString &html)
{
    Article *target = mutableArticle(ref);
    if (!target)
        return;
    target->readerHtml = html;
    m_dirtyArticles.insert(ref.feedId);
}

bool Store::matches(const Article &candidate, const QString &needle, const QString &feedName) const
{
    if (needle.isEmpty())
        return true;
    return candidate.title.contains(needle, Qt::CaseInsensitive) ||
           candidate.summary.contains(needle, Qt::CaseInsensitive) ||
           candidate.author.contains(needle, Qt::CaseInsensitive) ||
           feedName.contains(needle, Qt::CaseInsensitive);
}

QList<ArticleRef> Store::query(const Source &source, const QString &search, bool unreadOnly,
                               qint64 nowMs) const
{
    const QString needle = search.trimmed();
    QList<const Article *> found;
    QHash<QString, QString> names;
    for (const QString &feedId : feedIdsIn(source)) {
        const Feed *owner = feed(feedId);
        const QString name = owner ? owner->displayName() : QString();
        names.insert(feedId, name.toLower());
        for (const Article &candidate : articles(feedId)) {
            if (source.kind == SourceKind::Today &&
                (candidate.read || candidate.publishedMs < nowMs - kTodayWindowMs))
                continue;
            if (source.kind == SourceKind::AllUnread && candidate.read)
                continue;
            if (source.kind == SourceKind::Starred && !candidate.starred)
                continue;
            if (unreadOnly && candidate.read)
                continue;
            if (!matches(candidate, needle, name))
                continue;
            found.append(&candidate);
        }
    }
    const bool oldest = m_settings.oldestFirst;
    const bool group = m_settings.groupByFeed;
    std::stable_sort(found.begin(), found.end(), [&](const Article *a, const Article *b) {
        if (group && a->feedId != b->feedId) {
            const int order = names.value(a->feedId).compare(names.value(b->feedId));
            if (order != 0)
                return order < 0;
            return a->feedId < b->feedId;
        }
        if (a->publishedMs != b->publishedMs)
            return oldest ? a->publishedMs < b->publishedMs : a->publishedMs > b->publishedMs;
        return a->guid < b->guid;
    });
    QList<ArticleRef> refs;
    refs.reserve(found.size());
    for (const Article *candidate : std::as_const(found))
        refs.append(ArticleRef{candidate->feedId, candidate->guid});
    return refs;
}

int Store::unreadCount(const Source &source, qint64 nowMs) const
{
    switch (source.kind) {
    case SourceKind::Feed:
        return feedUnread(source.id);
    case SourceKind::Folder: {
        int total = 0;
        for (const QString &feedId : feedIdsIn(source))
            total += feedUnread(feedId);
        return total;
    }
    case SourceKind::AllUnread:
        return totalUnread();
    case SourceKind::Starred:
    case SourceKind::Today: {
        int total = 0;
        for (auto it = m_articles.constBegin(); it != m_articles.constEnd(); ++it) {
            for (const Article &candidate : it.value()) {
                if (candidate.read)
                    continue;
                if (source.kind == SourceKind::Starred ? candidate.starred
                                                       : candidate.publishedMs >= nowMs - kTodayWindowMs)
                    ++total;
            }
        }
        return total;
    }
    case SourceKind::None:
        break;
    }
    return 0;
}

int Store::totalUnread() const
{
    int total = 0;
    for (auto it = m_unread.constBegin(); it != m_unread.constEnd(); ++it)
        total += it.value();
    return total;
}

QStringList Store::dueFeeds(qint64 nowMs, bool force) const
{
    QStringList ids;
    for (const Feed &candidate : m_feeds) {
        if (force || candidate.nextDueMs <= nowMs)
            ids.append(candidate.id);
    }
    return ids;
}

void Store::recordSuccess(const QString &feedId, qint64 nowMs, const QString &etag,
                          const QString &lastModified, const QUrl &movedTo)
{
    Feed *target = mutableFeed(feedId);
    if (!target)
        return;
    target->lastCheckedMs = nowMs;
    target->lastSuccessMs = nowMs;
    target->failures = 0;
    target->lastError.clear();
    target->nextDueMs = nowMs + kCheckIntervalMs;
    target->etag = etag.left(400);
    target->lastModified = lastModified.left(100);
    if (movedTo.isValid() && movedTo.toString() != target->url) {
        const Feed *clash = feedByUrl(movedTo);
        if (!clash || clash->id == feedId) {
            appendLog(m_activityLog, nowMs,
                      QStringLiteral("%1 moved to %2").arg(target->displayName(), movedTo.toString()));
            target->url = movedTo.toString();
            m_exportDirty = true;
        }
    }
    m_stateDirty = true;
}

void Store::recordFailure(const QString &feedId, const QString &error, qint64 nowMs)
{
    Feed *target = mutableFeed(feedId);
    if (!target)
        return;
    target->lastCheckedMs = nowMs;
    target->failures = qMin(target->failures + 1, 30);
    target->lastError = error.left(500);
    target->nextDueMs = nowMs + backoffMs(target->failures);
    appendLog(m_errorLog, nowMs, QStringLiteral("%1: %2").arg(target->displayName(), target->lastError));
    m_stateDirty = true;
}

void Store::updateFeedMeta(const QString &feedId, const QString &homePage)
{
    Feed *target = mutableFeed(feedId);
    if (!target || homePage.isEmpty() || target->homePage == homePage)
        return;
    target->homePage = homePage;
    markExportDirty();
}

void Store::logError(qint64 nowMs, const QString &text)
{
    appendLog(m_errorLog, nowMs, text);
    m_stateDirty = true;
}

void Store::logActivity(qint64 nowMs, const QString &text)
{
    appendLog(m_activityLog, nowMs, text);
    m_stateDirty = true;
}

QJsonObject Store::stateJson() const
{
    QJsonArray feeds;
    for (const Feed &candidate : m_feeds) {
        feeds.append(QJsonObject{
            {QStringLiteral("id"), candidate.id},
            {QStringLiteral("url"), candidate.url},
            {QStringLiteral("title"), candidate.title},
            {QStringLiteral("customTitle"), candidate.customTitle},
            {QStringLiteral("homePage"), candidate.homePage},
            {QStringLiteral("iconUrl"), candidate.iconUrl},
            {QStringLiteral("folder"), candidate.folder},
            {QStringLiteral("etag"), candidate.etag},
            {QStringLiteral("lastModified"), candidate.lastModified},
            {QStringLiteral("lastError"), candidate.lastError},
            {QStringLiteral("added"), double(candidate.addedMs)},
            {QStringLiteral("lastChecked"), double(candidate.lastCheckedMs)},
            {QStringLiteral("lastSuccess"), double(candidate.lastSuccessMs)},
            {QStringLiteral("nextDue"), double(candidate.nextDueMs)},
            {QStringLiteral("failures"), candidate.failures},
            {QStringLiteral("unread"), m_unread.value(candidate.id)},
        });
    }
    QJsonArray folders;
    for (const Folder &candidate : m_folders)
        folders.append(QJsonObject{{QStringLiteral("name"), candidate.name},
                                   {QStringLiteral("collapsed"), candidate.collapsed}});
    return QJsonObject{
        {QStringLiteral("format"), QStringLiteral("forkmesh-rss-reader-state")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("storeId"), m_storeId},
        {QStringLiteral("feeds"), feeds},
        {QStringLiteral("folders"), folders},
        {QStringLiteral("settings"), m_settings.toJson()},
        {QStringLiteral("errorLog"), logJson(m_errorLog)},
        {QStringLiteral("activityLog"), logJson(m_activityLog)},
    };
}

bool Store::loadState(const QJsonObject &state)
{
    if (state.value(QStringLiteral("format")).toString() != QLatin1String("forkmesh-rss-reader-state"))
        return false;
    clear();
    m_storeId = state.value(QStringLiteral("storeId")).toString();
    for (const QJsonValue &value : state.value(QStringLiteral("folders")).toArray()) {
        const QJsonObject object = value.toObject();
        const QString name = collapseWhitespace(object.value(QStringLiteral("name")).toString());
        if (!name.isEmpty() && !folder(name) && m_folders.size() < kMaxFolders)
            m_folders.append(Folder{name, object.value(QStringLiteral("collapsed")).toBool()});
    }
    for (const QJsonValue &value : state.value(QStringLiteral("feeds")).toArray()) {
        const QJsonObject object = value.toObject();
        Feed loaded;
        loaded.id = object.value(QStringLiteral("id")).toString();
        loaded.url = object.value(QStringLiteral("url")).toString();
        if (loaded.id.isEmpty() || loaded.url.isEmpty() || feed(loaded.id) || m_feeds.size() >= kMaxFeeds)
            continue;
        loaded.title = object.value(QStringLiteral("title")).toString();
        loaded.customTitle = object.value(QStringLiteral("customTitle")).toString();
        loaded.homePage = object.value(QStringLiteral("homePage")).toString();
        loaded.iconUrl = object.value(QStringLiteral("iconUrl")).toString();
        loaded.folder = object.value(QStringLiteral("folder")).toString();
        if (!loaded.folder.isEmpty() && !folder(loaded.folder))
            loaded.folder.clear();
        loaded.etag = object.value(QStringLiteral("etag")).toString();
        loaded.lastModified = object.value(QStringLiteral("lastModified")).toString();
        loaded.lastError = object.value(QStringLiteral("lastError")).toString();
        loaded.addedMs = qint64(object.value(QStringLiteral("added")).toDouble());
        loaded.lastCheckedMs = qint64(object.value(QStringLiteral("lastChecked")).toDouble());
        loaded.lastSuccessMs = qint64(object.value(QStringLiteral("lastSuccess")).toDouble());
        loaded.nextDueMs = qint64(object.value(QStringLiteral("nextDue")).toDouble());
        loaded.failures = object.value(QStringLiteral("failures")).toInt();
        m_feeds.append(loaded);
        m_articles.insert(loaded.id, {});
        m_unread.insert(loaded.id, 0);
    }
    m_settings = Settings::fromJson(state.value(QStringLiteral("settings")).toObject());
    m_errorLog = logFrom(state.value(QStringLiteral("errorLog")).toArray());
    m_activityLog = logFrom(state.value(QStringLiteral("activityLog")).toArray());
    return true;
}

QJsonArray Store::articlesJson(const QString &feedId) const
{
    QJsonArray array;
    for (const Article &candidate : articles(feedId))
        array.append(articleJson(candidate));
    return array;
}

void Store::loadArticles(const QString &feedId, const QJsonArray &array)
{
    if (!feed(feedId))
        return;
    QList<Article> list;
    QSet<QString> seen;
    for (const QJsonValue &value : array) {
        Article loaded = articleFrom(feedId, value.toObject());
        if (loaded.guid.isEmpty() || seen.contains(loaded.guid))
            continue;
        seen.insert(loaded.guid);
        list.append(loaded);
    }
    prune(list);
    m_articles.insert(feedId, list);
    recount(feedId);
}

QSet<QString> Store::takeDirtyArticles()
{
    QSet<QString> dirty;
    dirty.swap(m_dirtyArticles);
    return dirty;
}

bool Store::takeStateDirty()
{
    const bool dirty = m_stateDirty;
    m_stateDirty = false;
    return dirty;
}

bool Store::takeExportDirty()
{
    const bool dirty = m_exportDirty;
    m_exportDirty = false;
    return dirty;
}

QJsonArray Store::starredJson() const
{
    QJsonArray array;
    qint64 budget = kMaxStarredExport;
    for (const Feed &owner : m_feeds) {
        for (const Article &candidate : articles(owner.id)) {
            if (!candidate.starred)
                continue;
            const bool full = candidate.html.size() <= kMaxStarredHtml && candidate.html.size() <= budget;
            const QString html = full ? candidate.html : escapeHtml(candidate.summary);
            budget -= html.size() + candidate.title.size() + candidate.link.size() + 200;
            array.append(QJsonObject{
                {QStringLiteral("feedUrl"), owner.url},
                {QStringLiteral("feedTitle"), owner.displayName()},
                {QStringLiteral("guid"), candidate.guid},
                {QStringLiteral("title"), candidate.title},
                {QStringLiteral("link"), candidate.link},
                {QStringLiteral("author"), candidate.author},
                {QStringLiteral("html"), html},
                {QStringLiteral("published"), double(candidate.publishedMs)},
                {QStringLiteral("firstSeen"), double(candidate.firstSeenMs)},
                {QStringLiteral("read"), candidate.read},
            });
        }
    }
    return array;
}

int Store::restoreStarred(const QJsonArray &array, qint64 nowMs)
{
    int restored = 0;
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        const Feed *owner = feedByUrl(QUrl(object.value(QStringLiteral("feedUrl")).toString()));
        const QString guid = object.value(QStringLiteral("guid")).toString().left(kMaxGuidLength);
        if (!owner || guid.isEmpty())
            continue;
        const ArticleRef ref{owner->id, guid};
        if (Article *existing = mutableArticle(ref)) {
            existing->starred = true;
            m_dirtyArticles.insert(owner->id);
            ++restored;
            continue;
        }
        Article article;
        article.feedId = owner->id;
        article.guid = guid;
        article.title = object.value(QStringLiteral("title")).toString();
        article.link = object.value(QStringLiteral("link")).toString();
        article.author = object.value(QStringLiteral("author")).toString();
        const QUrl link(article.link);
        article.html = sanitizeHtml(object.value(QStringLiteral("html")).toString(),
                                    link.isValid() ? link : QUrl(owner->url));
        article.summary = plainText(article.html, 600);
        article.publishedMs = qint64(object.value(QStringLiteral("published")).toDouble());
        article.firstSeenMs = qint64(object.value(QStringLiteral("firstSeen")).toDouble());
        if (article.publishedMs <= 0)
            article.publishedMs = nowMs;
        if (article.firstSeenMs <= 0)
            article.firstSeenMs = nowMs;
        article.updatedMs = nowMs;
        article.datedByFeed = true;
        article.read = object.value(QStringLiteral("read")).toBool(true);
        article.starred = true;
        m_articles[owner->id].append(article);
        prune(m_articles[owner->id]);
        recount(owner->id);
        m_dirtyArticles.insert(owner->id);
        ++restored;
    }
    return restored;
}

OpmlImportResult Store::importOpml(const OpmlDocument &document, qint64 nowMs)
{
    OpmlImportResult result;
    for (const QString &name : document.folders) {
        if (!folder(name) && addFolder(name, nullptr))
            ++result.folders;
    }
    for (const OpmlEntry &entry : document.feeds) {
        const QUrl url = normalizeAddress(entry.xmlUrl);
        QString error;
        const QString id = addFeed(url, entry.title, entry.folder, nowMs, &error);
        if (id.isEmpty()) {
            ++result.skipped;
            continue;
        }
        if (!entry.htmlUrl.isEmpty())
            updateFeedMeta(id, QUrl(entry.htmlUrl).toString());
        result.addedIds.append(id);
        ++result.added;
    }
    return result;
}

QJsonObject Store::exportBundle(qint64 nowMs) const
{
    return QJsonObject{
        {QStringLiteral("format"), QStringLiteral("forkmesh-rss-reader-export")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("exportedAt"),
         QDateTime::fromMSecsSinceEpoch(nowMs, QTimeZone::UTC).toString(Qt::ISODate)},
        {QStringLiteral("subscriptionsOpml"), QString::fromUtf8(writeOpml(m_feeds, m_folders, nowMs))},
        {QStringLiteral("settings"), m_settings.toJson()},
        {QStringLiteral("starred"), starredJson()},
    };
}

bool Store::hasData() const
{
    return !m_feeds.isEmpty() || !m_folders.isEmpty();
}

Stats Store::stats() const
{
    Stats stats;
    stats.feeds = int(m_feeds.size());
    stats.folders = int(m_folders.size());
    stats.unread = totalUnread();
    for (const Feed &owner : m_feeds) {
        stats.failing += owner.failures > 0 ? 1 : 0;
        for (const Article &candidate : articles(owner.id)) {
            ++stats.articles;
            stats.starred += candidate.starred ? 1 : 0;
        }
    }
    return stats;
}

void Store::clear()
{
    m_feeds.clear();
    m_folders.clear();
    m_articles.clear();
    m_unread.clear();
    m_errorLog.clear();
    m_activityLog.clear();
    m_dirtyArticles.clear();
    m_settings = Settings();
}

bool DiskStore::load(Store &store, const QString &expectedStoreId) const
{
    if (expectedStoreId.isEmpty())
        return false;
    QFile file(QDir(m_directory).filePath(QStringLiteral("state.json")));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject state = QJsonDocument::fromJson(file.readAll()).object();
    if (state.value(QStringLiteral("storeId")).toString() != expectedStoreId)
        return false;
    if (!store.loadState(state))
        return false;
    for (const Feed &owner : store.feeds()) {
        QFile articles(QDir(m_directory).filePath(QStringLiteral("articles/%1.json").arg(owner.id)));
        if (articles.open(QIODevice::ReadOnly))
            store.loadArticles(owner.id, QJsonDocument::fromJson(articles.readAll()).array());
    }
    store.takeDirtyArticles();
    store.takeStateDirty();
    store.takeExportDirty();
    return true;
}

bool DiskStore::saveState(const Store &store, QString *error) const
{
    return writeFile(QDir(m_directory).filePath(QStringLiteral("state.json")),
                     QJsonDocument(store.stateJson()).toJson(QJsonDocument::Compact), error);
}

bool DiskStore::saveArticles(const Store &store, const QString &feedId, QString *error) const
{
    if (!store.feed(feedId))
        return true;
    return writeFile(QDir(m_directory).filePath(QStringLiteral("articles/%1.json").arg(feedId)),
                     QJsonDocument(store.articlesJson(feedId)).toJson(QJsonDocument::Compact), error);
}

void DiskStore::removeFeedFiles(const QString &feedId) const
{
    if (feedId.isEmpty() || feedId.contains(QLatin1Char('/')) || feedId.contains(QLatin1Char('.')))
        return;
    QFile::remove(QDir(m_directory).filePath(QStringLiteral("articles/%1.json").arg(feedId)));
    QFile::remove(iconPath(feedId));
}

int DiskStore::storedUnread(const QString &expectedStoreId) const
{
    if (expectedStoreId.isEmpty())
        return -1;
    QFile file(QDir(m_directory).filePath(QStringLiteral("state.json")));
    if (!file.open(QIODevice::ReadOnly))
        return -1;
    const QJsonObject state = QJsonDocument::fromJson(file.readAll()).object();
    if (state.value(QStringLiteral("storeId")).toString() != expectedStoreId)
        return -1;
    int total = 0;
    for (const QJsonValue &value : state.value(QStringLiteral("feeds")).toArray())
        total += qMax(0, value.toObject().value(QStringLiteral("unread")).toInt());
    return total;
}

QString DiskStore::iconPath(const QString &feedId) const
{
    return QDir(m_directory).filePath(QStringLiteral("icons/%1.png").arg(feedId));
}

qint64 DiskStore::sizeOnDisk() const
{
    qint64 total = 0;
    QDirIterator it(m_directory, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

}
