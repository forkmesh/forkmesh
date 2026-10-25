#include "BrowserPrivacy.h"

#include <QRegularExpression>
#include <QSet>
#include <QUrlQuery>

namespace forkmesh::browser {

namespace {

const QString kDisableFeatures = QStringLiteral("--disable-features=");

const char *const kGoogleServiceHosts[] = {
    "update.googleapis.com",
    "safebrowsing.googleapis.com",
    "safebrowsing.google.com",
    "sb-ssl.google.com",
    "clientservices.googleapis.com",
    "optimizationguide-pa.googleapis.com",
    "content-autofill.googleapis.com",
    "chromesyncpasswords-pa.googleapis.com",
    "clients2.google.com",
    "clients2.googleusercontent.com",
    "redirector.gvt1.com",
    "edgedl.me.gvt1.com",
    "gvt2.com",
};

void appendUnique(QStringList &list, QSet<QString> &seen, const QString &value)
{
    if (value.isEmpty() || seen.contains(value))
        return;
    seen.insert(value);
    list.append(value);
}

}

QStringList ungoogledSwitches()
{
    return {
        QStringLiteral("--disable-background-networking"),
        QStringLiteral("--disable-component-update"),
        QStringLiteral("--disable-domain-reliability"),
        QStringLiteral("--disable-breakpad"),
        QStringLiteral("--disable-sync"),
        QStringLiteral("--disable-client-side-phishing-detection"),
        QStringLiteral("--no-pings"),
    };
}

QStringList ungoogledDisabledFeatures()
{
    return {
        QStringLiteral("AutofillServerCommunication"),
        QStringLiteral("CertificateTransparencyComponentUpdater"),
        QStringLiteral("MediaRouter"),
        QStringLiteral("DialMediaRouteProvider"),
        QStringLiteral("OptimizationHints"),
        QStringLiteral("OptimizationHintsFetching"),
        QStringLiteral("NetworkTimeServiceQuerying"),
        QStringLiteral("Translate"),
        QStringLiteral("InterestFeedContentSuggestions"),
        QStringLiteral("BrowsingTopics"),
        QStringLiteral("Fledge"),
        QStringLiteral("InterestGroupStorage"),
        QStringLiteral("AdInterestGroupAPI"),
        QStringLiteral("PrivacySandboxAdsAPIsOverride"),
    };
}

QByteArray mergeChromiumFlags(const QByteArray &existing)
{
    QStringList features;
    QSet<QString> seenFeatures;
    for (const QString &feature : ungoogledDisabledFeatures())
        appendUnique(features, seenFeatures, feature);

    QStringList switches;
    QSet<QString> seenSwitches;
    for (const QString &flag : ungoogledSwitches())
        appendUnique(switches, seenSwitches, flag);

    QStringList userSwitches;
    for (const QString &token :
         QString::fromLocal8Bit(existing).split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        if (token.startsWith(kDisableFeatures)) {
            for (const QString &feature :
                 token.mid(kDisableFeatures.size()).split(QLatin1Char(','), Qt::SkipEmptyParts))
                appendUnique(features, seenFeatures, feature.trimmed());
            continue;
        }
        if (seenSwitches.contains(token))
            continue;
        appendUnique(userSwitches, seenSwitches, token);
    }

    QStringList merged = switches;
    merged.append(kDisableFeatures + features.join(QLatin1Char(',')));
    merged.append(userSwitches);
    return merged.join(QLatin1Char(' ')).toLocal8Bit();
}

void installUngoogledChromiumFlags()
{
    if (qEnvironmentVariableIntValue(kKeepGoogleVariable) == 1)
        return;
    qputenv(kChromiumFlagsVariable, mergeChromiumFlags(qgetenv(kChromiumFlagsVariable)));
}

bool isGoogleServiceHost(const QString &host)
{
    QString name = host.trimmed().toLower();
    while (name.endsWith(QLatin1Char('.')))
        name.chop(1);
    if (name.isEmpty())
        return false;
    for (const char *blocked : kGoogleServiceHosts) {
        const QLatin1String entry(blocked);
        if (name == entry ||
            (name.endsWith(entry) && name.at(name.size() - entry.size() - 1) == QLatin1Char('.')))
            return true;
    }
    return false;
}

QString ungoogledUserAgent(const QString &userAgent)
{
    static const QRegularExpression qtToken(QStringLiteral("\\s*QtWebEngine/\\S+"));
    QString cleaned = userAgent;
    cleaned.remove(qtToken);
    return cleaned.simplified();
}

QUrl searchUrl(const QString &query)
{
    QUrl url(QStringLiteral("https://duckduckgo.com/"));
    QUrlQuery params;
    params.addQueryItem(QStringLiteral("q"), query.trimmed());
    url.setQuery(params);
    return url;
}

QUrl urlFromInput(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return QUrl();
    static const QRegularExpression port(QStringLiteral("^:?(\\d{2,5})(/.*)?$"));
    const QRegularExpressionMatch match = port.match(trimmed);
    if (match.hasMatch())
        return QUrl(QStringLiteral("http://localhost:%1%2")
                        .arg(match.captured(1), match.captured(2)));
    static const QRegularExpression local(
        QStringLiteral("^(localhost|127\\.0\\.0\\.1|\\[::1\\])(:\\d+)?(/.*)?$"),
        QRegularExpression::CaseInsensitiveOption);
    if (local.match(trimmed).hasMatch())
        return QUrl(QStringLiteral("http://") + trimmed);
    static const QRegularExpression scheme(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*://"));
    const bool hasScheme = scheme.match(trimmed).hasMatch();
    static const QRegularExpression space(QStringLiteral("\\s"));
    if (!hasScheme && (trimmed.contains(space) || !trimmed.contains(QLatin1Char('.'))))
        return searchUrl(trimmed);
    const QUrl url = QUrl::fromUserInput(trimmed);
    const QString urlScheme = url.scheme().toLower();
    if (url.isValid() && urlScheme != QLatin1String("http") &&
        urlScheme != QLatin1String("https"))
        return QUrl();
    if (!url.isValid() || url.host().isEmpty())
        return hasScheme ? QUrl() : searchUrl(trimmed);
    return url;
}

}
