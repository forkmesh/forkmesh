#include "InlineImageResources.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QHash>
#include <QImage>
#include <QPixmap>
#include <QTextDocument>
#include <QThread>
#include <QUrl>

namespace forkmesh::ui {

namespace {

constexpr qsizetype kMaxCachedImages = 512;

bool isBase64Char(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') ||
           (u >= '0' && u <= '9') || u == '+' || u == '/' || u == '=';
}

QPixmap decodedImage(const QString &key, const QByteArray &payload)
{
    static QHash<QString, QPixmap> cache;
    const auto found = cache.constFind(key);
    if (found != cache.constEnd())
        return found.value();
    const QImage image = QImage::fromData(QByteArray::fromBase64(payload));
    if (image.isNull())
        return {};
    if (cache.size() >= kMaxCachedImages)
        cache.clear();
    const QPixmap pixmap = QPixmap::fromImage(image);
    cache.insert(key, pixmap);
    return pixmap;
}

bool insideImageTag(const QString &html, qsizetype at)
{
    const qsizetype open = html.lastIndexOf(u'<', at);
    if (open < 0 || html.lastIndexOf(u'>', at) > open)
        return false;
    return QStringView(html).sliced(open + 1).startsWith(u"img",
                                                         Qt::CaseInsensitive);
}

}

QString inlineImageScheme()
{
    return QStringLiteral("fm-inline");
}

QString adoptInlineImages(QTextDocument *document, const QString &html)
{
    static const QString marker = QStringLiteral("data:image/");
    static const QString base64 = QStringLiteral(";base64,");
    if (!document || !html.contains(marker))
        return html;
    if (!QCoreApplication::instance() ||
        QThread::currentThread() != QCoreApplication::instance()->thread())
        return html;
    QString out;
    out.reserve(html.size());
    qsizetype cursor = 0;
    while (true) {
        const qsizetype start = html.indexOf(marker, cursor);
        if (start < 0)
            break;
        qsizetype mimeEnd = start + marker.size();
        while (mimeEnd < html.size() &&
               (html.at(mimeEnd).isLetterOrNumber() ||
                html.at(mimeEnd) == u'+' || html.at(mimeEnd) == u'-' ||
                html.at(mimeEnd) == u'.'))
            ++mimeEnd;
        if (!QStringView(html).sliced(mimeEnd).startsWith(base64) ||
            !insideImageTag(html, start)) {
            out += QStringView(html).sliced(cursor, mimeEnd - cursor);
            cursor = mimeEnd;
            continue;
        }
        const qsizetype payloadStart = mimeEnd + base64.size();
        qsizetype payloadEnd = payloadStart;
        while (payloadEnd < html.size() && isBase64Char(html.at(payloadEnd)))
            ++payloadEnd;
        const QByteArray payload =
            QStringView(html)
                .sliced(payloadStart, payloadEnd - payloadStart)
                .toLatin1();
        const QString key =
            inlineImageScheme() + u':' +
            QString::fromLatin1(
                QCryptographicHash::hash(payload, QCryptographicHash::Sha1)
                    .toHex());
        const QPixmap image =
            payload.isEmpty() ? QPixmap() : decodedImage(key, payload);
        out += QStringView(html).sliced(cursor, start - cursor);
        if (!image.isNull()) {
            document->addResource(QTextDocument::ImageResource, QUrl(key),
                                  image);
            out += key;
        } else {
            out += QStringView(html).sliced(start, payloadEnd - start);
        }
        cursor = payloadEnd;
    }
    out += QStringView(html).sliced(cursor);
    return out;
}

}
