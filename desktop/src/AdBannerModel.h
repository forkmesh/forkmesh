#pragma once

// ForkMesh Ads, "desktop" placement: the pure half.
//
// The network is first-party, human-reviewed and text-only, and the serve call
// carries nothing about the viewer. Everything here treats the response as
// untrusted anyway: a field that is missing, over its published limit, or a
// destination that is not plain https means the slot renders nothing. No
// QObject and no widgets, so the rules compile and test on their own
// (desktop/tests/test_ad_banner_model.cpp).

#include <QJsonObject>
#include <QString>
#include <QUrl>

namespace forkmesh::ads {

// The published creative limits (GET /api/ads/policy -> limits).
inline constexpr int kHeadlineLimit = 60;
inline constexpr int kBodyLimit = 160;
inline constexpr int kCtaLimit = 24;
inline constexpr int kSponsorLimit = 48;
inline constexpr int kUrlLimit = 2048;
// The serve response is a few hundred bytes; anything past this is not ours.
inline constexpr qint64 kServeResponseLimit = 64 * 1024;
// "Hide" is remembered locally for a week; no server call.
inline constexpr qint64 kHideDurationMs = qint64(7) * 24 * 60 * 60 * 1000;
// Long-lived clients ask at most every 30 minutes.
inline constexpr int kRefreshIntervalMs = 30 * 60 * 1000;
inline constexpr int kFirstFetchDelayMs = 20 * 1000;

// QSettings keys. "Show sponsored messages" in Settings > General (default
// on), and the "Hide" stamp, written once per click and never on a timer.
inline const QString kAdsEnabledSetting = QStringLiteral("ads/enabled");
inline const QString kAdsHiddenUntilSetting = QStringLiteral("ads/hiddenUntilMs");

struct AdBannerContent {
    QString id;       // 32 lowercase hex
    QString headline; // never empty when valid
    QString body;     // may be empty
    QString cta;      // never empty when valid ("Learn more" when the ad has none)
    QString sponsor;  // may be empty
    QUrl url;         // https, a host, no credentials

    bool isValid() const { return !id.isEmpty(); }
};

// The ad in a serve response, or an invalid AdBannerContent when the slot
// must stay empty: `ad` null or not an object, ok:false, a malformed id, an
// empty headline, a field over its limit, or an unsafe destination.
AdBannerContent adFromServeResponse(const QJsonObject &response);

// https only, a real host, and no user:password@ part (the classic
// "https://forkmesh.com@elsewhere.example" disguise).
bool adDestinationIsSafe(const QUrl &url);

// Ads are reviewed by ForkMesh, so they are only ever asked of ForkMesh's own
// relay (or one on this machine, for development). A self-hosted or
// third-party relay is never asked for one.
bool adServeHostAllowed(const QUrl &apiBase);

// <scheme>://<host>/api/ads/serve?placement=desktop, from any URL on the relay.
QUrl adServeUrl(const QUrl &apiBase);
// <scheme>://<host>/api/ads/<id>/click; invalid when the id is not 32 hex.
QUrl adClickUrl(const QUrl &apiBase, const QString &adId);

// Whether a remembered "Hide" still applies.
inline bool adHiddenAt(qint64 hiddenUntilMs, qint64 nowMs)
{
    return hiddenUntilMs > nowMs;
}

} // namespace forkmesh::ads
