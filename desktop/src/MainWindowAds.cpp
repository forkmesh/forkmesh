#include "MainWindow.h"

#include "AdBannerModel.h"
#include "AdBannerWidget.h"
#include "BackoffNetworkAccessManager.h"
#include "MainWindowInternal.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QFrame>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

// ForkMesh Ads, "desktop" placement.
//
// The rules this file keeps (docs/operations/ad-network):
//  - The serve call says which surface is asking and nothing else: no
//    Authorization header, no cookies, no account, no repository.
//  - An ad is never needed. Anything in the way -- offline mode, a firewall
//    rule, a rate-limit cooldown, a failed or odd response, a headless or test
//    run -- leaves the strip hidden, silently. It never raises the firewall
//    prompt, never logs an error at the user, never retries early.
//  - Ad text is plain text (AdBannerWidget.cpp), and only an https
//    destination is ever handed to the browser.
//  - "Hide" is local and lasts a week; Settings > General turns ads off.

namespace {

using forkmesh::ads::AdBannerContent;

const QUrl kWhyThisAdUrl(
    QStringLiteral("https://forkmesh.com/docs/operations/ad-network/"));
const QUrl kAdvertiseUrl(QStringLiteral("https://app.forkmesh.com/dashboard/ads"));

// A headless node or a scripted offscreen run has nobody to show an ad to.
bool adsHaveNoAudience()
{
#ifdef FORKMESH_WINDOW_TESTS
    return true;
#else
    const QString platform = QGuiApplication::platformName();
    return platform.contains(QStringLiteral("offscreen"), Qt::CaseInsensitive) ||
           platform.contains(QStringLiteral("minimal"), Qt::CaseInsensitive);
#endif
}

// No viewer input: the shared manager's cookie jar stays out of it, and there
// is deliberately no token parameter to pass by accident.
QNetworkRequest adRequest(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setRawHeader(QByteArrayLiteral("Accept"),
                         QByteArrayLiteral("application/json"));
    request.setTransferTimeout(8000);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
                         QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
                         QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute,
                         QNetworkRequest::Manual);
    return request;
}

// Whether `url` can go out right now without asking anybody anything. With
// the request firewall on, an unlisted destination opens a modal prompt from
// inside createRequest(); an ad must never be the reason that prompt appears,
// so anything short of an existing allow rule means "do not send".
bool adRequestWouldBeSilent(const BackoffNetworkAccessManager *network,
                            const QUrl &url)
{
    if (!network || !url.isValid())
        return false;
    if (network->offlineMode() || network->offMachineRequestsBlocked())
        return false;
    if (network->firewallEnabled() &&
        !BackoffNetworkAccessManager::isOnThisMachine(url) &&
        network->firewallMatch(url) != BackoffNetworkAccessManager::FirewallAllowed)
        return false;
    return !network->hostInCooldown(url.host());
}

} // namespace

void MainWindow::scheduleAdBanner()
{
    if (m_adBannerScheduled || adsHaveNoAudience())
        return;
    // No timer: nothing about an ad is worth waking the process on time. One
    // request after launch, then one when the window is activated again after
    // kRefreshIntervalMs (refreshAdBannerOnActivation).
    m_adBannerScheduled = true;
    // Well after first paint and the startup pollers.
    QTimer::singleShot(forkmesh::ads::kFirstFetchDelayMs, this, [this] {
        // Marks the launch attempt as made even when it is skipped (hidden
        // window, cooldown), so the next activation is free to ask.
        m_adBannerAskedMs = 1;
        refreshAdBanner();
    });
}

void MainWindow::refreshAdBannerOnActivation()
{
    if (!m_adBannerScheduled || m_adBannerAskedMs <= 0 ||
        QDateTime::currentMSecsSinceEpoch() - m_adBannerAskedMs <
            forkmesh::ads::kRefreshIntervalMs)
        return;
    refreshAdBanner();
}

void MainWindow::refreshAdBanner()
{
    if (m_closingDown || adsHaveNoAudience())
        return;
    // Reads only. The one QSettings write in this file is the Hide click.
    const QSettings settings;
    if (!settings.value(forkmesh::ads::kAdsEnabledSetting, true).toBool() ||
        forkmesh::ads::adHiddenAt(
            settings.value(forkmesh::ads::kAdsHiddenUntilSetting, 0).toLongLong(),
            QDateTime::currentMSecsSinceEpoch())) {
        hideAdBanner();
        return;
    }
    // Nobody is looking at a hidden or minimised window; ask next time.
    if (!isVisible() || isMinimized() || m_adBannerReply)
        return;

    const QUrl apiBase = catalogApiUrl();
    if (!forkmesh::ads::adServeHostAllowed(apiBase)) {
        hideAdBanner();
        return;
    }
    const QUrl url = forkmesh::ads::adServeUrl(apiBase);
    auto *network = qobject_cast<BackoffNetworkAccessManager *>(m_networkAccess);
    if (!adRequestWouldBeSilent(network, url)) {
        // Offline, denied, unlisted or cooling down. A firewall rule against
        // this endpoint is the user saying "no ads"; take any shown one down.
        if (network && network->firewallEnabled() &&
            network->firewallMatch(url) != BackoffNetworkAccessManager::FirewallAllowed)
            hideAdBanner();
        return;
    }

    m_adBannerAskedMs = QDateTime::currentMSecsSinceEpoch();
    QNetworkReply *reply = m_networkAccess->get(adRequest(url));
    m_adBannerReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (m_closingDown)
            return;
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status != 200) {
            // Keep whatever is on screen; a later activation asks again. No retry.
            return;
        }
        const QByteArray body =
            reply->read(forkmesh::ads::kServeResponseLimit + 1);
        if (body.size() > forkmesh::ads::kServeResponseLimit) {
            hideAdBanner();
            return;
        }
        showAdBanner(QJsonDocument::fromJson(body).object());
    });
}

void MainWindow::showAdBanner(const QJsonObject &serveResponse)
{
    const AdBannerContent ad = forkmesh::ads::adFromServeResponse(serveResponse);
    // The answer may be minutes old by the time it lands: re-check the two
    // switches the user can have flipped since the request went out.
    const QSettings settings;
    if (!ad.isValid() || m_closingDown || !m_appStatusArea ||
        !settings.value(forkmesh::ads::kAdsEnabledSetting, true).toBool() ||
        forkmesh::ads::adHiddenAt(
            settings.value(forkmesh::ads::kAdsHiddenUntilSetting, 0).toLongLong(),
            QDateTime::currentMSecsSinceEpoch())) {
        hideAdBanner();
        return;
    }

    if (!m_adBanner) {
        auto *statusLayout = qobject_cast<QVBoxLayout *>(m_appStatusArea->layout());
        if (!statusLayout)
            return;
        const forkmesh::ads::AdBannerWidgets widgets =
            forkmesh::ads::buildAdBanner(m_appStatusArea);
        m_adBanner = widgets.banner;
        // Above the status bar: the one strip on screen on every page, and the
        // one a reduced view mode already takes away with the rest of the
        // chrome (MainWindowViews.cpp hides m_appStatusArea).
        statusLayout->insertWidget(0, m_adBanner);

        connect(widgets.cta, &QPushButton::clicked, this, [this] {
            if (m_closingDown || m_adBannerId.isEmpty() ||
                !forkmesh::ads::adDestinationIsSafe(m_adBannerUrl))
                return;
            QDesktopServices::openUrl(m_adBannerUrl);
            // Fire and forget. It counts a click for the advertiser and says
            // nothing about who clicked; if it cannot go out quietly it does
            // not go out.
            const QUrl clickUrl =
                forkmesh::ads::adClickUrl(catalogApiUrl(), m_adBannerId);
            auto *network =
                qobject_cast<BackoffNetworkAccessManager *>(m_networkAccess);
            if (!forkmesh::ads::adServeHostAllowed(clickUrl) ||
                !adRequestWouldBeSilent(network, clickUrl))
                return;
            QNetworkRequest request = adRequest(clickUrl);
            request.setHeader(QNetworkRequest::ContentTypeHeader,
                              QByteArrayLiteral("application/json"));
            QNetworkReply *reply = m_networkAccess->post(
                request, QByteArrayLiteral("{\"placement\":\"desktop\"}"));
            connect(reply, &QNetworkReply::finished, reply,
                    &QObject::deleteLater);
        });
        connect(widgets.why, &QPushButton::clicked, this,
                [] { QDesktopServices::openUrl(kWhyThisAdUrl); });
        connect(widgets.advertise, &QPushButton::clicked, this,
                [] { QDesktopServices::openUrl(kAdvertiseUrl); });
        connect(widgets.hide, &QPushButton::clicked, this, [this] {
            // The only settings write here, and only on this click: QSettings
            // rewrites the whole file on sync.
            QSettings().setValue(forkmesh::ads::kAdsHiddenUntilSetting,
                                 QDateTime::currentMSecsSinceEpoch() +
                                     forkmesh::ads::kHideDurationMs);
            hideAdBanner();
        });
    }

    forkmesh::ads::applyAdBannerContent(
        forkmesh::ads::adBannerWidgets(m_adBanner), ad);
    m_adBannerId = ad.id;
    m_adBannerUrl = ad.url;
    m_adBanner->show();
}

void MainWindow::hideAdBanner()
{
    m_adBannerId.clear();
    m_adBannerUrl.clear();
    if (m_adBanner)
        m_adBanner->hide();
}
