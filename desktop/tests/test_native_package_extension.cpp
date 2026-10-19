#include "NativePackageExtension.h"
#include "PackageFeedback.h"
#include "PackageRailTile.h"

#include <QApplication>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QUrlQuery>

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition) {
        std::printf("PASS: %s\n", message);
    } else {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

forkmesh::market::NativePackageFactory emptyPageFactory()
{
    return [](const forkmesh::market::NativePackageHost &, QWidget *)
               -> forkmesh::market::NativePackageWidget * { return nullptr; };
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    using namespace forkmesh::market;

    NativePackageRegistration openType({
        QStringLiteral("native:sample-tool"),
        {},
        QStringLiteral("Sample Tool"),
        QStringLiteral("tools"),
        {},
        {},
        emptyPageFactory(),
    });
    check(openType.accepted() && openType.errorString().isEmpty(),
          "a package-neutral native entry point registers");
    check(nativePackageContribution(QStringLiteral("native:sample-tool"),
                                    QStringLiteral("extension.vscode")),
          "an unrestricted contribution accepts an arbitrary manifest type");

    NativePackageRegistration typed({
        QStringLiteral("native:typed-page"),
        {QStringLiteral("extension.native")},
        QStringLiteral("Typed Page"),
        QStringLiteral("apps"),
        QStringLiteral("/api/typed-page"),
        QStringLiteral("typed-page-request-v1"),
        emptyPageFactory(),
    });
    check(typed.accepted() &&
              nativePackageContribution(QStringLiteral("native:typed-page"),
                                        QStringLiteral("extension.native")),
          "a contribution activates for a declared manifest type");
    check(!nativePackageContribution(QStringLiteral("native:typed-page"),
                                     QStringLiteral("service.compute")),
          "a contribution fails closed for an undeclared manifest type");

    NativePackageRegistration duplicate({
        QStringLiteral("native:sample-tool"),
        {},
        QStringLiteral("Replacement"),
        QStringLiteral("tools"),
        {},
        {},
        emptyPageFactory(),
    });
    check(!duplicate.accepted() &&
              duplicate.errorString().contains(QStringLiteral("duplicate")) &&
              nativePackageContribution(QStringLiteral("native:sample-tool"))
                      ->navigationLabel == QStringLiteral("Sample Tool"),
          "duplicate entry points cannot replace an existing package runtime");

    NativePackageRegistration unsafe({
        QStringLiteral("native:../escape"),
        {},
        QStringLiteral("Unsafe"),
        QStringLiteral("tools"),
        QStringLiteral("/api/../escape"),
        QStringLiteral("unsafe-proof"),
        emptyPageFactory(),
    });
    check(!unsafe.accepted() &&
              !nativePackageContribution(QStringLiteral("native:../escape")),
          "unsafe entry points and API boundaries never enter the registry");

    NativePackageRegistration link({
        QStringLiteral("native:sample-link"),
        {QStringLiteral("extension.native")},
        QStringLiteral("Sample Link"),
        QStringLiteral("link"),
        {},
        {},
        {},
        QStringLiteral("/design"),
    });
    const NativePackageContribution *linkContribution = nativePackageContribution(
        QStringLiteral("native:sample-link"), QStringLiteral("extension.native"));
    check(link.accepted() && linkContribution &&
              linkContribution->browserPath == QStringLiteral("/design") &&
              !linkContribution->factory,
          "a rail link registers a host page path without a page factory");

    NativePackageRegistration linkWithPage({
        QStringLiteral("native:link-and-page"),
        {},
        QStringLiteral("Both"),
        QStringLiteral("link"),
        {},
        {},
        emptyPageFactory(),
        QStringLiteral("/design"),
    });
    check(!linkWithPage.accepted() &&
              linkWithPage.errorString() == QStringLiteral("invalid browser link"),
          "a contribution is either a page or a link, never both");

    for (const QString &path :
         {QStringLiteral("https://example.com/design"),
          QStringLiteral("/../design"), QStringLiteral("//example.com"),
          QStringLiteral("/design?next=x"), QStringLiteral("design")}) {
        NativePackageRegistration unsafeLink({
            QStringLiteral("native:unsafe-link"),
            {},
            QStringLiteral("Unsafe Link"),
            QStringLiteral("link"),
            {},
            {},
            {},
            path,
        });
        check(!unsafeLink.accepted() &&
                  !nativePackageContribution(QStringLiteral("native:unsafe-link")),
              "a rail link only names a path on the active host");
    }

    NativePackageRegistration linkWithApi({
        QStringLiteral("native:link-with-api"),
        {},
        QStringLiteral("Link With API"),
        QStringLiteral("link"),
        QStringLiteral("/api/link"),
        QStringLiteral("link-proof"),
        {},
        QStringLiteral("/design"),
    });
    check(!linkWithApi.accepted(),
          "a rail link gets no package API boundary");

    NativePackageHost host{
        QStringLiteral("sample-tool"),
        QStringLiteral("Sample Tool"),
        QStringLiteral("service.design"),
        QStringLiteral("2.4.1"),
        {QStringLiteral("documents.read")},
        {},
    };
    check(host.packageId == QStringLiteral("sample-tool") &&
              host.packageName == QStringLiteral("Sample Tool") &&
              host.packageType == QStringLiteral("service.design") &&
              host.packageVersion == QStringLiteral("2.4.1") &&
              host.permissionIds == QStringList{QStringLiteral("documents.read")},
          "the generic host carries manifest identity, type, version and permissions");

    const QString marketingInstall = nativePackageLifecyclePath(
        QStringLiteral("forkmesh"), QStringLiteral("marketing"),
        QStringLiteral("install"));
    check(marketingInstall ==
              QStringLiteral("/api/plugins/forkmesh/marketing/install"),
          "the host constructs the Marketing organization install path");
    check(nativePackageRequestPathAllowed(
              QStringLiteral("marketing"), QStringLiteral("/api/marketing"),
              marketingInstall) &&
              nativePackageRequestPathAllowed(
                  QStringLiteral("marketing"),
                  QStringLiteral("/api/marketing"),
                  QStringLiteral("/api/marketing/forkmesh/content")),
          "Marketing lifecycle and package API paths share one boundary");
    check(!nativePackageRequestPathAllowed(
              QStringLiteral("marketing"), QStringLiteral("/api/marketing"),
              QStringLiteral("/api/plugins/forkmesh/crm/install")) &&
              !nativePackageRequestPathAllowed(
                  QStringLiteral("marketing"),
                  QStringLiteral("/api/marketing"),
                  QStringLiteral("https://evil.invalid/api/marketing")) &&
              !nativePackageRequestPathAllowed(
                  QStringLiteral("marketing"),
                  QStringLiteral("/api/marketing"),
                  QStringLiteral("/api/marketing/../plugins/forkmesh/crm")) &&
              nativePackageLifecyclePath(
                  QString(), QStringLiteral("marketing"),
                  QStringLiteral("install")).isEmpty(),
          "a package cannot cross identities, origins, or an empty organization");

    const QUrl webBase(QStringLiteral("https://user:secret@app.example.test/api/x?q=1#f"));
    check(nativePackageWebPageUrl(
              webBase,
              QStringLiteral("/dashboard/runners?runner=box&provider=codex&connect=1"))
                  .toString() ==
              QStringLiteral("https://app.example.test/dashboard/runners"
                             "?runner=box&provider=codex&connect=1"),
          "a package web page opens on the active host with its query and no credentials");
    check(!nativePackageWebPageUrl(webBase, QStringLiteral("https://evil.invalid/x")).isValid() &&
              !nativePackageWebPageUrl(webBase, QStringLiteral("//evil.invalid/x")).isValid() &&
              !nativePackageWebPageUrl(webBase, QStringLiteral("dashboard")).isValid() &&
              !nativePackageWebPageUrl(webBase, QStringLiteral("/dashboard/../x")).isValid() &&
              !nativePackageWebPageUrl(QUrl(), QStringLiteral("/dashboard")).isValid() &&
              !nativePackageWebPageUrl(QUrl(QStringLiteral("file:///tmp")),
                                       QStringLiteral("/dashboard")).isValid(),
          "a package web page stays on the active web host");

    NativePackageRailTile tile;
    tile.id = QStringLiteral("youtube");
    tile.label = QStringLiteral("YouTube");
    tile.count = QStringLiteral("1.5k");
    tile.countLabel = QStringLiteral("followers");
    tile.tone = NativePackageRailTone::Warning;
    tile.stateLabel = QStringLiteral("Stale numbers");
    tile.details = {QStringLiteral("Numbers from 20 days ago (youtube)")};
    tile.steps = {{QStringLiteral("refresh"), QStringLiteral("Refresh the numbers")},
                  {QStringLiteral("post"), QStringLiteral("Post on YouTube")}};
    tile.pending = 2;
    const QStringList tip = packageRailTileToolTip(tile).split(QLatin1Char('\n'));
    check(tip.value(0) == QString::fromUtf8("YouTube \xC2\xB7 Stale numbers") &&
              tip.value(1) == QStringLiteral("1.5k followers") &&
              tip.contains(QStringLiteral("Next steps:")) &&
              tip.contains(QString::fromUtf8("\xE2\x80\xA2 Refresh the numbers")) &&
              tip.contains(QString::fromUtf8("\xE2\x80\xA2 Post on YouTube")),
          "a rail tile's hover names its state, count and every next step");
    check(tip.constLast() == QStringLiteral("Click for next steps."),
          "a rail tile without a page says a click lists its steps");
    tile.opensPage = true;
    check(packageRailTileToolTip(tile).endsWith(
              QStringLiteral("Click for details, right-click for next steps.")),
          "a rail tile with a page says a click opens it");
    tile.steps.clear();
    check(!packageRailTileToolTip(tile).contains(QStringLiteral("Next steps:")),
          "a rail tile with no steps lists none");
    check(packageRailToneColor(NativePackageRailTone::Good, true) !=
                  packageRailToneColor(NativePackageRailTone::Warning, true) &&
              packageRailToneColor(NativePackageRailTone::Attention, false) !=
                  packageRailToneColor(NativePackageRailTone::Paused, false),
          "rail tones are told apart by colour");

    NativePackageRailTile stale = tile;
    stale.steps = {{QStringLiteral("refresh"), QStringLiteral("Refresh the numbers")},
                   {QStringLiteral("open"), QStringLiteral("Open YouTube")}};
    NativePackageRailTile attention;
    attention.id = QStringLiteral("reddit");
    attention.label = QStringLiteral("Reddit");
    attention.tone = NativePackageRailTone::Attention;
    attention.stateLabel = QStringLiteral("Needs attention");
    attention.pending = 3;
    attention.steps = {{QStringLiteral("open"), QStringLiteral("Open Reddit")},
                       {QStringLiteral("post"), QStringLiteral("Post on Reddit.")}};
    NativePackageRailTile setup;
    setup.id = QStringLiteral("x");
    setup.label = QStringLiteral("X");
    setup.tone = NativePackageRailTone::Neutral;
    setup.pending = 1;
    NativePackageRailTile good;
    good.id = QStringLiteral("github");
    good.tone = NativePackageRailTone::Good;
    NativePackageRailTile paused = stale;
    paused.tone = NativePackageRailTone::Paused;
    check(packageRailUrgency(good) == 0 && packageRailUrgency(paused) == 0 &&
              packageRailUrgency(NativePackageRailTile()) == 0,
          "an on-track, paused or empty channel needs no reminder");
    check(packageRailNeedsMoreAttention(stale, attention) &&
              packageRailNeedsMoreAttention(attention, setup) &&
              packageRailNeedsMoreAttention(setup, good) &&
              !packageRailNeedsMoreAttention(good, NativePackageRailTile()) &&
              !packageRailNeedsMoreAttention(paused, NativePackageRailTile()),
          "stale numbers outrank a failing check, which outranks a channel not set up");
    NativePackageRailTile worse = attention;
    worse.pending = 4;
    check(packageRailNeedsMoreAttention(worse, attention) &&
              !packageRailNeedsMoreAttention(attention, worse) &&
              !packageRailNeedsMoreAttention(attention, attention),
          "between two channels in the same state the one failing more checks wins");
    check(packageRailReminderTitle(stale) == QString::fromUtf8("ForkMesh \xE2\x80\x94 fix YouTube") &&
              packageRailReminderBody(stale) ==
                  QStringLiteral("No channel has changed in the last hour. YouTube needs the most "
                                 "attention (stale numbers, 2 checks failing). Next step: Refresh "
                                 "the numbers."),
          "the reminder names the channel, its state and its first next step");
    check(packageRailReminderBody(attention).endsWith(
              QStringLiteral("(needs attention, 3 checks failing). Next step: Post on Reddit.")),
          "the reminder skips the Open step and keeps a step's own full stop");
    NativePackageRailTile older = stale;
    older.details = {QStringLiteral("Numbers from 21 days ago (youtube)")};
    older.steps.clear();
    NativePackageRailTile grown = stale;
    grown.count = QStringLiteral("1.6k");
    check(packageRailStateKey(older) == packageRailStateKey(stale) &&
              packageRailStateKey(grown) != packageRailStateKey(stale) &&
              packageRailStateKey(attention) != packageRailStateKey(worse),
          "only a channel's count, state or failing checks count as a change");

    PackageFeedbackReport report = packageFeedbackReport(
        QStringLiteral("RSS-Reader"), QStringLiteral("RSS Reader"),
        QStringLiteral("1.4.0"), QStringLiteral("0.9.2"));
    report.message = QStringLiteral("  Feeds stop refreshing after sleep.  ");
    report.contact = QStringLiteral("  reader@example.com ");
    const QJsonObject payload = packageFeedbackPayload(report);
    check(payload.value(QStringLiteral("message")).toString() ==
                  QStringLiteral("Feeds stop refreshing after sleep.") &&
              payload.value(QStringLiteral("contact")).toString() ==
                  QStringLiteral("reader@example.com") &&
              payload.value(QStringLiteral("package")).toString() ==
                  QStringLiteral("rss-reader") &&
              payload.value(QStringLiteral("source")).toString() ==
                  QStringLiteral("desktop") &&
              payload.value(QStringLiteral("severity")).toString() ==
                  QStringLiteral("normal") &&
              payload.value(QStringLiteral("screen")).toString() ==
                  QStringLiteral("Plugin: RSS Reader 1.4.0") &&
              payload.value(QStringLiteral("appVersion")).toString() ==
                  QStringLiteral("0.9.2") &&
              payload.value(QStringLiteral("platform")).toString() ==
                  desktopFeedbackPlatform() &&
              payload.contains(QStringLiteral("osVersion")) &&
              payload.contains(QStringLiteral("device")),
          "a plugin report carries the trimmed message, the contact, the "
          "lower-case package id and source desktop");
    PackageFeedbackReport anonymous = report;
    anonymous.contact = QStringLiteral("   ");
    anonymous.message = QString(kPackageFeedbackMaxMessage + 50, QLatin1Char('x'));
    const QJsonObject anonymousPayload = packageFeedbackPayload(anonymous);
    check(!anonymousPayload.contains(QStringLiteral("contact")) &&
              anonymousPayload.value(QStringLiteral("message")).toString().size() ==
                  kPackageFeedbackMaxMessage,
          "a blank contact is left out and an over-long message is clipped to "
          "what the server keeps");

    const PackageFeedbackOutcome filed = packageFeedbackOutcome(
        201, QByteArrayLiteral("{\"ok\":true,\"id\":\"r1\",\"issueNumber\":42}"),
        {});
    const PackageFeedbackOutcome unfiled = packageFeedbackOutcome(
        201, QByteArrayLiteral("{\"ok\":true,\"id\":\"r2\",\"issueNumber\":0}"),
        {});
    const PackageFeedbackOutcome limited = packageFeedbackOutcome(
        429, QByteArrayLiteral("{\"error\":\"rate_limited\"}"), {});
    const PackageFeedbackOutcome empty = packageFeedbackOutcome(
        400, QByteArrayLiteral("{\"error\":\"empty_report\"}"), {});
    const PackageFeedbackOutcome offline =
        packageFeedbackOutcome(0, {}, QStringLiteral("Host not found"));
    const PackageFeedbackOutcome broken = packageFeedbackOutcome(
        502, QByteArrayLiteral("<html>"), {});
    check(filed.sent && filed.text.contains(QStringLiteral("#42")) &&
              unfiled.sent && !unfiled.text.contains(QLatin1Char('#')) &&
              !limited.sent && limited.text.contains(QStringLiteral("last hour")) &&
              !empty.sent && empty.text.contains(QStringLiteral("more words")) &&
              !offline.sent && offline.text.contains(QStringLiteral("Host not found")) &&
              !broken.sent && broken.text.contains(QStringLiteral("HTTP 502")),
          "each server answer to a report becomes a plain sentence");

    const QUrl rating = packageRatingUrl(QStringLiteral("https://app.forkmesh.com"),
                                         QString(), QStringLiteral("Crm"));
    const QUrlQuery ratingQuery(rating);
    check(rating.toString(QUrl::RemoveQuery) ==
                  QStringLiteral("https://app.forkmesh.com/market-reviews") &&
              ratingQuery.queryItemValue(QStringLiteral("market")) ==
                  QStringLiteral("forkmesh") &&
              ratingQuery.queryItemValue(QStringLiteral("package")) ==
                  QStringLiteral("crm") &&
              packageRatingUrl(QStringLiteral("https://app.forkmesh.com"),
                               QStringLiteral("forkmesh-apps"),
                               QStringLiteral("crm"))
                      .query()
                      .contains(QStringLiteral("market=forkmesh-apps")) &&
              !packageRatingUrl(QString(), QString(), QStringLiteral("crm")).isValid() &&
              !packageRatingUrl(QStringLiteral("file:///tmp"), QString(),
                                QStringLiteral("crm"))
                   .isValid(),
          "Rate opens the package's reviews page on the active host");

    check(packageShareUrl(QString(), QStringLiteral("Crm")) ==
                  QUrl(QStringLiteral("https://forkmesh.com/market/crm")) &&
              packageShareUrl(QStringLiteral("forkmesh"),
                              QStringLiteral("nemotron-3.5-lightning"))
                      .toString() ==
                  QStringLiteral(
                      "https://forkmesh.com/market/nemotron-3.5-lightning") &&
              !packageShareUrl(QStringLiteral("forkmesh-apps"),
                               QStringLiteral("crm"))
                   .isValid() &&
              !packageShareUrl(QString(), QStringLiteral("previews")).isValid() &&
              !packageShareUrl(QString(), QStringLiteral("a/b")).isValid(),
          "Copy link gives the plugin's public Market page");

    QJsonObject sentPayload;
    PackageFeedbackReplyHandler pendingReply;
    int sends = 0;
    PackageFeedbackDialog dialog(
        packageFeedbackReport(QStringLiteral("crm"), QStringLiteral("CRM"),
                              QStringLiteral("2.0.0"), QStringLiteral("0.9.2")),
        [&](const QJsonObject &body, PackageFeedbackReplyHandler done) {
            ++sends;
            sentPayload = body;
            pendingReply = std::move(done);
        });
    auto *message = dialog.findChild<QPlainTextEdit *>(
        QStringLiteral("packageFeedbackMessage"));
    auto *contact =
        dialog.findChild<QLineEdit *>(QStringLiteral("packageFeedbackContact"));
    auto *send = dialog.findChild<QPushButton *>(QStringLiteral("packageFeedbackSend"));
    auto *cancel =
        dialog.findChild<QPushButton *>(QStringLiteral("packageFeedbackCancel"));
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("packageFeedbackStatus"));
    check(dialog.objectName() == QStringLiteral("packageFeedbackDialog") &&
              dialog.windowTitle() == QStringLiteral("Feedback for CRM") && message &&
              contact && send && cancel && status && !send->isEnabled(),
          "the feedback dialog has a description, a contact field, Send and "
          "Cancel, and Send waits for a description");
    if (message && contact && send && cancel && status) {
        message->setPlainText(QStringLiteral("ok"));
        const bool shortBlocked = !send->isEnabled();
        message->setPlainText(QStringLiteral("Deals vanish after a merge."));
        contact->setText(QStringLiteral("jett"));
        send->click();
        check(shortBlocked && sends == 1 && !send->isEnabled() &&
                  sentPayload == dialog.payload() &&
                  sentPayload.value(QStringLiteral("package")).toString() ==
                      QStringLiteral("crm") &&
                  sentPayload.value(QStringLiteral("message")).toString() ==
                      QStringLiteral("Deals vanish after a merge.") &&
                  sentPayload.value(QStringLiteral("contact")).toString() ==
                      QStringLiteral("jett"),
              "Send posts the description and contact tagged with the package "
              "and waits for the answer");
        if (pendingReply)
            pendingReply(429, QByteArrayLiteral("{\"error\":\"rate_limited\"}"), {});
        check(!status->isHidden() &&
                  status->text().contains(QStringLiteral("last hour")) &&
                  send->isEnabled() && !message->isReadOnly(),
              "a refused report says why and can be sent again");
        send->click();
        if (pendingReply)
            pendingReply(201, QByteArrayLiteral("{\"ok\":true,\"issueNumber\":7}"), {});
        check(sends == 2 && status->text().contains(QStringLiteral("#7")) &&
                  !send->isEnabled() && cancel->text() == QStringLiteral("Close"),
              "a sent report says so inline and Cancel becomes Close");
    }

    return failures == 0 ? 0 : 1;
}
