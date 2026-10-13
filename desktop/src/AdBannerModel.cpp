#include "AdBannerModel.h"

#include <QJsonValue>

namespace forkmesh::ads {

namespace {

bool isAdId(const QString &id)
{
    if (id.size() != 32)
        return false;
    for (const QChar c : id) {
        const bool digit = c >= QLatin1Char('0') && c <= QLatin1Char('9');
        const bool hex = c >= QLatin1Char('a') && c <= QLatin1Char('f');
        if (!digit && !hex)
            return false;
    }
    return true;
}

// One line of display text: control and format characters (newlines, NULs,
// bidi overrides) dropped, runs of whitespace collapsed. *ok turns false when
// the value is not a string or is over `limit` as sent.
QString cleanText(const QJsonValue &value, int limit, bool *ok)
{
    if (value.isUndefined() || value.isNull())
        return {};
    if (!value.isString()) {
        *ok = false;
        return {};
    }
    const QString raw = value.toString();
    if (raw.size() > limit) {
        *ok = false;
        return {};
    }
    QString kept;
    kept.reserve(raw.size());
    for (const QChar c : raw) {
        if (c.isSpace()) {
            kept.append(QLatin1Char(' '));
            continue;
        }
        const QChar::Category category = c.category();
        if (category == QChar::Other_Control || category == QChar::Other_Format)
            continue;
        kept.append(c);
    }
    return kept.simplified();
}

QUrl relayUrl(const QUrl &apiBase, const QString &path)
{
    if (!apiBase.isValid() || apiBase.host().isEmpty())
        return {};
    QUrl url;
    url.setScheme(apiBase.scheme());
    url.setHost(apiBase.host());
    url.setPort(apiBase.port());
    url.setPath(path);
    return url;
}

} // namespace

bool adDestinationIsSafe(const QUrl &url)
{
    return url.isValid() && url.scheme() == QLatin1String("https") &&
           !url.host().isEmpty() && url.userInfo().isEmpty();
}

bool adServeHostAllowed(const QUrl &apiBase)
{
    const QString host = apiBase.host().toLower();
    if (host.isEmpty())
        return false;
    if (host == QLatin1String("localhost") || host == QLatin1String("127.0.0.1") ||
        host == QLatin1String("::1"))
        return true;
    if (apiBase.scheme() != QLatin1String("https"))
        return false;
    return host == QLatin1String("forkmesh.com") ||
           host.endsWith(QLatin1String(".forkmesh.com"));
}

QUrl adServeUrl(const QUrl &apiBase)
{
    QUrl url = relayUrl(apiBase, QStringLiteral("/api/ads/serve"));
    if (url.isValid())
        url.setQuery(QStringLiteral("placement=desktop"));
    return url;
}

QUrl adClickUrl(const QUrl &apiBase, const QString &adId)
{
    if (!isAdId(adId))
        return {};
    return relayUrl(apiBase, QStringLiteral("/api/ads/%1/click").arg(adId));
}

AdBannerContent adFromServeResponse(const QJsonObject &response)
{
    if (response.contains(QStringLiteral("ok")) &&
        !response.value(QStringLiteral("ok")).toBool())
        return {};
    const QJsonValue adValue = response.value(QStringLiteral("ad"));
    if (!adValue.isObject())
        return {};
    const QJsonObject ad = adValue.toObject();

    const QString id = ad.value(QStringLiteral("id")).toString();
    if (!isAdId(id))
        return {};

    bool ok = true;
    AdBannerContent content;
    content.headline = cleanText(ad.value(QStringLiteral("headline")), kHeadlineLimit, &ok);
    content.body = cleanText(ad.value(QStringLiteral("body")), kBodyLimit, &ok);
    content.cta = cleanText(ad.value(QStringLiteral("cta")), kCtaLimit, &ok);
    content.sponsor = cleanText(ad.value(QStringLiteral("sponsor")), kSponsorLimit, &ok);
    if (!ok || content.headline.isEmpty())
        return {};
    if (content.cta.isEmpty())
        content.cta = QStringLiteral("Learn more");

    const QString rawUrl = ad.value(QStringLiteral("url")).toString();
    if (rawUrl.isEmpty() || rawUrl.size() > kUrlLimit)
        return {};
    const QUrl url(rawUrl, QUrl::StrictMode);
    if (!adDestinationIsSafe(url))
        return {};
    content.url = url;
    content.id = id; // last: isValid() keys off it
    return content;
}

} // namespace forkmesh::ads
