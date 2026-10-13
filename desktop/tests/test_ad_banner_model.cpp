#include "AdBannerModel.h"

#include <QCoreApplication>
#include <QJsonDocument>

#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

const QString kId = QStringLiteral("0123456789abcdef0123456789abcdef");

QJsonObject goodAd()
{
    return {{QStringLiteral("id"), kId},
            {QStringLiteral("label"), QStringLiteral("Sponsored")},
            {QStringLiteral("headline"), QStringLiteral("Ship faster with Acme CI")},
            {QStringLiteral("body"), QStringLiteral("Hosted runners for small teams.")},
            {QStringLiteral("cta"), QStringLiteral("Try it")},
            {QStringLiteral("sponsor"), QStringLiteral("Acme")},
            {QStringLiteral("url"), QStringLiteral("https://acme.example/ci")},
            {QStringLiteral("category"), QStringLiteral("developer-tools")}};
}

QJsonObject response(const QJsonValue &ad)
{
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("network"), QStringLiteral("forkmesh-ads-v1")},
            {QStringLiteral("placement"), QStringLiteral("desktop")},
            {QStringLiteral("ad"), ad}};
}

QJsonObject with(const QString &key, const QJsonValue &value)
{
    QJsonObject ad = goodAd();
    ad.insert(key, value);
    return ad;
}

bool shows(const QJsonObject &ad)
{
    return forkmesh::ads::adFromServeResponse(response(ad)).isValid();
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    using namespace forkmesh::ads;

    {
        const AdBannerContent ad = adFromServeResponse(response(goodAd()));
        check(ad.isValid() && ad.id == kId &&
                  ad.headline == QStringLiteral("Ship faster with Acme CI") &&
                  ad.body == QStringLiteral("Hosted runners for small teams.") &&
                  ad.cta == QStringLiteral("Try it") &&
                  ad.sponsor == QStringLiteral("Acme") &&
                  ad.url == QUrl(QStringLiteral("https://acme.example/ci")),
              "a well-formed serve response yields the ad");
    }

    // The slot renders nothing.
    check(!adFromServeResponse(response(QJsonValue::Null)).isValid(),
          "ad: null shows nothing");
    check(!adFromServeResponse({}).isValid(), "an empty response shows nothing");
    check(!adFromServeResponse(response(QStringLiteral("ad"))).isValid(),
          "an ad that is not an object shows nothing");
    {
        QJsonObject failed = response(goodAd());
        failed.insert(QStringLiteral("ok"), false);
        check(!adFromServeResponse(failed).isValid(), "ok:false shows nothing");
    }

    // Ids go into the click path, so they are exactly what the server mints.
    check(!shows(with(QStringLiteral("id"), QString())), "an empty id is refused");
    check(!shows(with(QStringLiteral("id"), QStringLiteral("abc"))),
          "a short id is refused");
    check(!shows(with(QStringLiteral("id"),
                      QStringLiteral("0123456789abcdef0123456789abcde/"))),
          "an id with a path character is refused");
    check(!shows(with(QStringLiteral("id"),
                      QStringLiteral("0123456789ABCDEF0123456789ABCDEF"))),
          "an upper-case id is refused");
    check(!shows(with(QStringLiteral("id"), 12)), "a numeric id is refused");

    // Destinations.
    for (const char *bad :
         {"http://acme.example/", "javascript:alert(1)", "file:///etc/passwd",
          "ftp://acme.example/", "https://", "//acme.example/", "acme.example",
          "https://forkmesh.com@evil.example/", "https://user:pw@evil.example/",
          "forkmesh://open", ""}) {
        const bool refused = !shows(with(QStringLiteral("url"), QString::fromLatin1(bad)));
        if (!refused)
            std::cerr << "  accepted: " << bad << '\n';
        check(refused, "a destination that is not plain https is refused");
    }
    check(!shows(with(QStringLiteral("url"),
                      QStringLiteral("https://acme.example/") +
                          QString(kUrlLimit, QLatin1Char('a')))),
          "an over-long destination is refused");
    check(adDestinationIsSafe(QUrl(QStringLiteral("https://acme.example/a?b=c"))) &&
              !adDestinationIsSafe(QUrl(QStringLiteral("http://acme.example/"))),
          "only https destinations are safe to open");

    // Text limits are the published ones; over them means it is not ours.
    check(!shows(with(QStringLiteral("headline"), QString())),
          "an empty headline is refused");
    check(!shows(with(QStringLiteral("headline"), QStringLiteral(" \n\t "))),
          "a whitespace headline is refused");
    check(shows(with(QStringLiteral("headline"), QString(kHeadlineLimit, QLatin1Char('h')))) &&
              !shows(with(QStringLiteral("headline"),
                          QString(kHeadlineLimit + 1, QLatin1Char('h')))),
          "the headline limit is 60");
    check(shows(with(QStringLiteral("body"), QString(kBodyLimit, QLatin1Char('b')))) &&
              !shows(with(QStringLiteral("body"), QString(kBodyLimit + 1, QLatin1Char('b')))),
          "the body limit is 160");
    check(!shows(with(QStringLiteral("cta"), QString(kCtaLimit + 1, QLatin1Char('c')))),
          "the call-to-action limit is 24");
    check(!shows(with(QStringLiteral("sponsor"),
                      QString(kSponsorLimit + 1, QLatin1Char('s')))),
          "the sponsor limit is 48");
    check(!shows(with(QStringLiteral("headline"), 7)) &&
              !shows(with(QStringLiteral("body"), QJsonObject())),
          "a text field that is not a string is refused");
    {
        QJsonObject bare = goodAd();
        bare.remove(QStringLiteral("body"));
        bare.remove(QStringLiteral("cta"));
        bare.remove(QStringLiteral("sponsor"));
        const AdBannerContent ad = adFromServeResponse(response(bare));
        check(ad.isValid() && ad.body.isEmpty() && ad.sponsor.isEmpty() &&
                  ad.cta == QStringLiteral("Learn more"),
              "body and sponsor may be empty; a missing call to action gets a default");
    }
    {
        // Markup stays text (the labels are Qt::PlainText); control and bidi
        // override characters never reach a label at all.
        const QString hostile = QStringLiteral("<b>Hi</b>\n") + QChar(0x202E) +
                                QStringLiteral("there") + QChar(0) +
                                QStringLiteral("   friend");
        const AdBannerContent ad =
            adFromServeResponse(response(with(QStringLiteral("headline"), hostile)));
        check(ad.isValid() && ad.headline == QStringLiteral("<b>Hi</b> there friend"),
              "control and format characters are dropped and whitespace collapsed");
    }

    // Where the calls go.
    const QUrl base(QStringLiteral("https://app.forkmesh.com/api/repositories?x=1#f"));
    check(adServeUrl(base) ==
              QUrl(QStringLiteral("https://app.forkmesh.com/api/ads/serve?placement=desktop")),
          "the serve URL names only the placement");
    check(adClickUrl(base, kId) ==
              QUrl(QStringLiteral("https://app.forkmesh.com/api/ads/") + kId +
                   QStringLiteral("/click")),
          "the click URL is built from the ad id");
    check(!adClickUrl(base, QStringLiteral("../accounts")).isValid() &&
              !adServeUrl(QUrl()).isValid(),
          "no URL is built from a bad id or a missing relay");
    check(adServeUrl(QUrl(QStringLiteral("http://127.0.0.1:8787/api/repositories"))) ==
              QUrl(QStringLiteral("http://127.0.0.1:8787/api/ads/serve?placement=desktop")),
          "a local relay keeps its port");

    check(adServeHostAllowed(base) &&
              adServeHostAllowed(QUrl(QStringLiteral("https://forkmesh.com/"))) &&
              adServeHostAllowed(QUrl(QStringLiteral("http://localhost:8787/"))) &&
              adServeHostAllowed(QUrl(QStringLiteral("http://127.0.0.1:8787/"))),
          "ForkMesh's relay and a local one may be asked for an ad");
    check(!adServeHostAllowed(QUrl(QStringLiteral("https://relay.example/"))) &&
              !adServeHostAllowed(QUrl(QStringLiteral("https://evilforkmesh.com/"))) &&
              !adServeHostAllowed(QUrl(QStringLiteral("https://forkmesh.com.evil.example/"))) &&
              !adServeHostAllowed(QUrl(QStringLiteral("http://app.forkmesh.com/"))) &&
              !adServeHostAllowed(QUrl()),
          "a third-party relay is never asked for an ad");

    check(adHiddenAt(2000, 1000) && !adHiddenAt(1000, 1000) && !adHiddenAt(0, 1000),
          "Hide lasts until its stamp passes");
    check(kHideDurationMs == qint64(604800000) && kRefreshIntervalMs >= 30 * 60 * 1000,
          "Hide is seven days and refresh is no more often than every 30 minutes");

    if (failures == 0)
        std::cout << "ad banner model tests passed\n";
    return failures == 0 ? 0 : 1;
}
