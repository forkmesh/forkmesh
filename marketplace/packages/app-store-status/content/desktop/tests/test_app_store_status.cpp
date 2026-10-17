#include "AppStoreStatusFeature.h"
#include "AppStoreStatusModel.h"

#include <QApplication>
#include <QBuffer>
#include <QDateTime>
#include <QDialog>
#include <QImage>
#include <QToolButton>
#include <QFrame>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QTimeZone>

#include <cstdio>

using namespace forkmesh::appstorestatus;

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

QJsonObject sampleSnapshot()
{
    return QJsonDocument::fromJson(R"({
  "format": "forkmesh-app-store-status-v1",
  "generatedAt": 1760000000000,
  "app": {"id": "6752000000", "bundleId": "com.forkmesh.app"},
  "attention": ["1.4.0 was rejected: Apple's message is in App Store Connect -> App Review", "  "],
  "versions": [
    {"version": "1.4.0", "build": "19", "releaseType": "AFTER_APPROVAL",
     "createdAt": "2026-09-16T09:45:00Z",
     "status": {"state": "REJECTED", "label": "Rejected", "tone": "action"}}
  ],
  "submissions": [
    {"id": "s1", "submittedAt": "2026-10-08T18:29:00Z",
     "status": {"state": "UNRESOLVED_ISSUES", "label": "Unresolved issues", "tone": "action"},
     "items": [{"name": "1.4.0", "status": {"state": "REJECTED", "label": "Rejected", "tone": "action"}}]}
  ],
  "downloads": {
    "available": true, "note": "", "since": "2026-08",
    "total": {"downloads": 18, "redownloads": 2, "updates": 7},
    "recent": [{"days": 7, "downloads": 1, "redownloads": 0, "updates": 2},
               {"days": 30, "downloads": 104, "redownloads": 0, "updates": 2}]
  },
  "builds": [
    {"version": "1.4.0", "build": "19", "uploadedAt": "2026-10-05T12:51:00Z", "expired": false,
     "processing": {"state": "VALID", "label": "Valid", "tone": "good"},
     "internal": {"state": "IN_BETA_TESTING", "label": "In beta testing", "tone": "good"},
     "external": {"state": "WAITING_FOR_BETA_REVIEW", "label": "Waiting for beta review", "tone": "pending"},
     "betaReview": {"state": "", "label": "Not submitted", "tone": "neutral"},
     "groups": ["forkmesh-team"]}
  ],
  "groups": [
    {"name": "outside", "internal": false, "testers": 1, "allBuilds": false,
     "publicLinkEnabled": true, "publicLink": "https://testflight.apple.com/join/x",
     "publicLinkLimit": 0, "feedbackEnabled": true},
    {"name": "forkmesh-team", "internal": true, "testers": 3, "allBuilds": true,
     "publicLinkEnabled": false, "feedbackEnabled": false}
  ],
  "feedback": [],
  "reviews": [
    {"createdAt": "2026-10-01T10:00:00Z", "rating": 5, "title": "Great", "territory": "USA",
     "body": "Works well"}
  ],
  "unavailable": {"feedback": "ERROR: App Store Connect GET answered 404"}
})").object();
}

void testTones()
{
    check(toneOf(QJsonObject{{QStringLiteral("tone"), QStringLiteral("action")}}) == Tone::Action &&
              toneOf(QJsonObject{{QStringLiteral("tone"), QStringLiteral("pending")}}) == Tone::Pending &&
              toneOf(QJsonObject{{QStringLiteral("tone"), QStringLiteral("good")}}) == Tone::Good &&
              toneOf(QJsonObject{{QStringLiteral("tone"), QStringLiteral("loud")}}) == Tone::Neutral &&
              toneOf(QJsonObject{}) == Tone::Neutral,
          "only the four documented tones are recognised; anything else is neutral");
    check(toneName(Tone::Action) == QLatin1String("danger") &&
              toneName(Tone::Pending) == QLatin1String("warning") &&
              toneName(Tone::Good) == QLatin1String("success") &&
              toneName(Tone::Neutral) == QLatin1String("neutral"),
          "tones map to the destructive, warning, success and muted badge roles");
    check(statusLabel(QJsonObject{{QStringLiteral("label"), QStringLiteral("Rejected")},
                                  {QStringLiteral("state"), QStringLiteral("REJECTED")}}) ==
                  QLatin1String("Rejected") &&
              statusLabel(QJsonObject{{QStringLiteral("state"), QStringLiteral("WAITING_FOR_REVIEW")}}) ==
                  QLatin1String("Waiting for review") &&
              statusLabel(QJsonObject{}) == QLatin1String("Unknown"),
          "the label is shown, the raw state is humanised when the label is missing");
}

void testTimes()
{
    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-10-15T12:00:00Z"), Qt::ISODate);
    check(parseTimestamp(QJsonValue(1760000000000.0)).toMSecsSinceEpoch() == 1760000000000LL &&
              parseTimestamp(QJsonValue(1760000000.0)).toMSecsSinceEpoch() == 1760000000000LL &&
              !parseTimestamp(QJsonValue(0)).isValid() && !parseTimestamp(QJsonValue()).isValid(),
          "epoch milliseconds and seconds both parse; zero means never published");
    check(parseTimestamp(QJsonValue(QStringLiteral("2026-10-08T18:29:00Z"))) ==
                  parseTimestamp(QJsonValue(QStringLiteral("2026-10-08T20:29:00+02:00"))) &&
              parseTimestamp(QJsonValue(QStringLiteral("2026-10-08T18:29:00.250Z"))).isValid() &&
              !parseTimestamp(QJsonValue(QStringLiteral("yesterday"))).isValid(),
          "App Store Connect dates with Z, an offset or milliseconds parse; garbage does not");
    check(relativeTime(now.addSecs(-20), now) == QLatin1String("just now") &&
              relativeTime(now.addSecs(30), now) == QLatin1String("just now") &&
              relativeTime(now.addSecs(-60), now) == QLatin1String("1 minute ago") &&
              relativeTime(now.addSecs(-5 * 60), now) == QLatin1String("5 minutes ago") &&
              relativeTime(now.addSecs(-3 * 3600), now) == QLatin1String("3 hours ago") &&
              relativeTime(now.addDays(-4), now) == QLatin1String("4 days ago") &&
              relativeTime(now.addDays(-60), now).startsWith(QLatin1String("on ")),
          "relative times read naturally and fall back to a date after a month");
    check(publishedText(QJsonValue(double(now.addSecs(-120).toMSecsSinceEpoch())), now) ==
                  QLatin1String("Published 2 minutes ago") &&
              publishedText(QJsonValue(0), now).isEmpty(),
          "the header says when the snapshot was published, and nothing before the first publish");
    check(!dateText(QJsonValue(QStringLiteral("2026-10-08T18:29:00Z"))).isEmpty() &&
              dateText(QJsonValue(QStringLiteral("nope"))).isEmpty(),
          "dates format locally and an unreadable date is left out");
}

void testSnapshotParts()
{
    const QJsonObject snapshot = sampleSnapshot();
    check(snapshotOf(QJsonObject{{QStringLiteral("snapshot"), snapshot}}) == snapshot &&
              snapshotOf(QJsonObject{{QStringLiteral("snapshot"), QJsonValue::Null}}).isEmpty(),
          "a null snapshot reads as nothing published");
    check(exportHasData(QJsonObject{{QStringLiteral("export"),
                                     QJsonObject{{QStringLiteral("snapshot"), snapshot}}}}) &&
              !exportHasData(QJsonObject{{QStringLiteral("export"),
                                          QJsonObject{{QStringLiteral("snapshot"), QJsonValue::Null},
                                                      {QStringLiteral("account"), QStringLiteral("a")}}}}),
          "the export only counts as data when it carries a snapshot");
    check(attentionLines(snapshot).size() == 1, "blank attention lines are dropped");
    check(versionTitle(QStringLiteral("1.4.0"), QStringLiteral("19")) == QLatin1String("1.4.0 (19)") &&
              versionTitle(QStringLiteral("1.4.0"), QString()) == QLatin1String("1.4.0") &&
              releaseTypeLabel(QStringLiteral("AFTER_APPROVAL")) == QLatin1String("Releases after approval") &&
              releaseTypeLabel(QStringLiteral("MANUAL")) == QLatin1String("Manual release"),
          "versions show their build and release type");
    const QList<Figures> figures = downloadFigures(snapshot.value(QStringLiteral("downloads")).toObject());
    check(figures.size() == 3 && figures.at(0).label.startsWith(QLatin1String("Since ")) &&
              figures.at(0).downloads == 18 && figures.at(0).redownloads == 2 && figures.at(0).updates == 7 &&
              figures.at(1).label == QLatin1String("Last 7 days") && figures.at(2).downloads == 104,
          "downloads give the total and every recent span with three figures each");
    check(downloadFigures(QJsonObject{{QStringLiteral("available"), false},
                                      {QStringLiteral("note"), QStringLiteral("No vendor number")}})
              .isEmpty(),
          "unavailable downloads give no figures");
    const QJsonArray groups = sortedGroups(snapshot.value(QStringLiteral("groups")).toArray());
    check(groups.size() == 2 &&
              groups.at(0).toObject().value(QStringLiteral("name")).toString() == QLatin1String("forkmesh-team"),
          "internal TestFlight groups come first");
    const QString outside = groupDetail(groups.at(1).toObject());
    const QString team = groupDetail(groups.at(0).toObject());
    check(outside.contains(QLatin1String("1 tester")) && outside.contains(QLatin1String("chosen builds")) &&
              outside.contains(QLatin1String("https://testflight.apple.com/join/x")) &&
              outside.contains(QLatin1String("feedback on")) && team.contains(QLatin1String("3 testers")) &&
              team.contains(QLatin1String("every build")) && !team.contains(QLatin1String("public link")) &&
              team.contains(QLatin1String("feedback off")),
          "group lines name testers, builds, the public link and feedback");
    check(stars(5) == QString(5, QChar(0x2605)) && stars(9) == stars(5) &&
              stars(-2) == QString(5, QChar(0x2606)) &&
              stars(3) == QString(3, QChar(0x2605)) + QString(2, QChar(0x2606)),
          "ratings become five stars, clamped");
    check(unavailableText(snapshot, QStringLiteral("feedback")).contains(QLatin1String("404")) &&
              unavailableText(snapshot, QStringLiteral("reviews")).isEmpty(),
          "a section's unavailable message is read by its key");
    check(feedbackTitle(QJsonObject{{QStringLiteral("kind"), QStringLiteral("screenshot")},
                                    {QStringLiteral("from"), QStringLiteral("t@example.com")}}) ==
              QLatin1String("Screenshot feedback from t@example.com"),
          "feedback names its kind and tester");
}

QList<QLabel *> badges(QWidget *root, const QString &tone)
{
    QList<QLabel *> found;
    for (QLabel *label : root->findChildren<QLabel *>(QStringLiteral("badge"))) {
        if (label->property("tone").toString() == tone)
            found.append(label);
    }
    return found;
}

bool hasLabel(QWidget *root, const QString &text)
{
    for (QLabel *label : root->findChildren<QLabel *>()) {
        if (label->text() == text)
            return true;
    }
    return false;
}

void testPage()
{
    QJsonObject payload{{QStringLiteral("ok"), true},
                        {QStringLiteral("actor"), QStringLiteral("jett")},
                        {QStringLiteral("published"), false},
                        {QStringLiteral("publishedAt"), 0},
                        {QStringLiteral("snapshot"), QJsonValue::Null}};
    QStringList calls;
    forkmesh::market::NativePackageHost host;
    host.request = [&](const QByteArray &method, const QString &path, const QJsonObject &,
                       forkmesh::market::NativePackageReplyHandler done) {
        calls.append(QString::fromLatin1(method) + QLatin1Char(' ') + path);
        done(true, payload, QString());
    };
    AppStoreStatusFeature page(host);
    page.resize(900, 700);
    page.refresh();
    check(calls == QStringList{QStringLiteral("GET /api/app-store-status")},
          "refresh reads the package API once", calls.join(QStringLiteral(", ")));
    check(page.findChild<QWidget *>(QStringLiteral("emptyState")) &&
              hasLabel(&page, QStringLiteral("Nothing published yet")),
          "before the first publish the page explains how the snapshot arrives");

    const qint64 published = QDateTime::currentMSecsSinceEpoch() - 5 * 60 * 1000;
    payload.insert(QStringLiteral("published"), true);
    payload.insert(QStringLiteral("publishedAt"), double(published));
    payload.insert(QStringLiteral("snapshot"), sampleSnapshot());
    page.refresh();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    check(!page.findChild<QWidget *>(QStringLiteral("emptyState")), "the empty state goes once published");
    check(hasLabel(&page, QStringLiteral("com.forkmesh.app")) &&
              hasLabel(&page, QStringLiteral("Published 5 minutes ago")),
          "the header shows the bundle id and when it was published");
    bool banner = false;
    for (QFrame *frame : page.findChildren<QFrame *>(QStringLiteral("inlineNotice"))) {
        if (frame->property("appStoreAttention").toBool() &&
            frame->property("tone").toString() == QLatin1String("danger"))
            banner = true;
    }
    check(banner, "attention lines become one destructive banner");
    check(badges(&page, QStringLiteral("danger")).size() >= 3 &&
              badges(&page, QStringLiteral("warning")).size() == 1 &&
              badges(&page, QStringLiteral("success")).size() >= 3,
          "status pills are coloured by tone");
    bool unavailable = false;
    for (QFrame *frame : page.findChildren<QFrame *>(QStringLiteral("inlineNotice"))) {
        if (frame->property("unavailable").toString() == QLatin1String("feedback"))
            unavailable = true;
    }
    check(unavailable && !hasLabel(&page, QStringLiteral("No tester feedback.")),
          "an unavailable section shows its message instead of an empty line");
    check(hasLabel(&page, QStringLiteral("104")) && hasLabel(&page, QStringLiteral("Last 30 days")),
          "download figures are on the page");
    check(hasLabel(&page, QStringLiteral("Works well")) && hasLabel(&page, QStringLiteral("Great")),
          "customer reviews show their title and text");
    QStringList sections;
    for (QWidget *section : page.findChildren<QWidget *>(QStringLiteral("appStoreSection")))
        sections.append(section->property("section").toString());
    check(sections == QStringList{QStringLiteral("versions"), QStringLiteral("submissions"),
                                  QStringLiteral("downloads"), QStringLiteral("builds"),
                                  QStringLiteral("groups"), QStringLiteral("feedback"),
                                  QStringLiteral("reviews")},
          "sections follow the documented order", sections.join(QStringLiteral(", ")));

    bool exported = false;
    payload = QJsonObject{{QStringLiteral("ok"), true},
                          {QStringLiteral("export"), QJsonObject{{QStringLiteral("snapshot"), sampleSnapshot()}}}};
    check(page.exportData([&](bool ok, const QJsonObject &result, const QString &) {
              exported = ok && result.value(QStringLiteral("hasData")).toBool();
          }) && exported && calls.last() == QLatin1String("GET /api/app-store-status/export"),
          "the export reads the package export route and reports data");

    forkmesh::market::NativePackageHost failing;
    failing.request = [](const QByteArray &, const QString &, const QJsonObject &,
                         forkmesh::market::NativePackageReplyHandler done) {
        done(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("invalid_session")}},
             QStringLiteral("HTTP 401"));
    };
    AppStoreStatusFeature broken(failing);
    broken.refresh();
    bool error = false;
    for (QFrame *frame : broken.findChildren<QFrame *>(QStringLiteral("inlineNotice"))) {
        if (frame->property("tone").toString() == QLatin1String("danger") && !frame->isHidden())
            error = true;
    }
    check(error, "a failed read shows a destructive notice");

    const auto *contribution = forkmesh::market::nativePackageContribution(
        QStringLiteral("builtin:app-store-status"), QStringLiteral("extension.native"));
    check(contribution && contribution->apiPrefix == QLatin1String("/api/app-store-status") &&
              contribution->requestProof == QLatin1String("forkmesh-app-store-status-request-v1") &&
              contribution->factory,
          "the page registers its entry point with the signed request proof");
}

}

namespace {

QByteArray pngBytes()
{
    QImage image(40, 80, QImage::Format_RGB32);
    image.fill(QColor(40, 90, 200));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

void testScreenshots()
{
    const QString first = QStringLiteral("3f1c2a9b8d7e6f5a4b3c2d1e");
    const QString second = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaa");
    const QJsonObject feedback{
        {QStringLiteral("kind"), QStringLiteral("screenshot")},
        {QStringLiteral("from"), QStringLiteral("t@example.com")},
        {QStringLiteral("comment"), QStringLiteral("Show time as 5:45pm")},
        {QStringLiteral("screenshots"),
         QJsonArray{QJsonObject{{QStringLiteral("id"), first}, {QStringLiteral("width"), 1179},
                                {QStringLiteral("height"), 2556}},
                    QJsonObject{{QStringLiteral("id"), QStringLiteral("../etc/passwd")}},
                    QJsonObject{{QStringLiteral("id"), second}, {QStringLiteral("width"), -4}}}}};
    const QList<Screenshot> shots = screenshotsOf(feedback);
    check(shots.size() == 2 && shots[0].id == first && shots[0].width == 1179 && shots[0].height == 2556 &&
              shots[1].id == second && shots[1].width == 0,
          "screenshotsOf keeps well-formed ids and clamps sizes");
    check(screenshotPath(first) == QLatin1String("/api/app-store-status/screenshots/3f1c2a9b8d7e6f5a4b3c2d1e"),
          "a screenshot is read from the package API");
    const QByteArray png = pngBytes();
    const QJsonObject good{{QStringLiteral("mime"), QStringLiteral("image/png")},
                           {QStringLiteral("data"), QString::fromLatin1(png.toBase64())}};
    check(screenshotBytes(good) == png, "screenshotBytes decodes a PNG");
    QJsonObject lying = good;
    lying.insert(QStringLiteral("mime"), QStringLiteral("image/jpeg"));
    check(screenshotBytes(lying).isEmpty(), "screenshotBytes refuses bytes that do not match the type");
    check(screenshotBytes(QJsonObject{{QStringLiteral("mime"), QStringLiteral("image/png")},
                                      {QStringLiteral("data"), QStringLiteral("%%%")}})
              .isEmpty(),
          "screenshotBytes refuses broken base64");

    QJsonObject snapshot = QJsonDocument::fromJson(R"({"format": "forkmesh-app-store-status-v1",
        "app": {"bundleId": "com.forkmesh.app"}})").object();
    snapshot.insert(QStringLiteral("feedback"), QJsonArray{feedback});
    const QJsonObject payload{{QStringLiteral("ok"), true},
                              {QStringLiteral("published"), true},
                              {QStringLiteral("publishedAt"), double(QDateTime::currentMSecsSinceEpoch())},
                              {QStringLiteral("snapshot"), snapshot}};
    QStringList calls;
    forkmesh::market::NativePackageHost host;
    host.request = [&](const QByteArray &method, const QString &path, const QJsonObject &,
                       forkmesh::market::NativePackageReplyHandler done) {
        calls.append(QString::fromLatin1(method) + QLatin1Char(' ') + path);
        if (path == screenshotPath(first))
            done(true, good, QString());
        else if (path.startsWith(QLatin1String("/api/app-store-status/screenshots/")))
            done(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("screenshot_not_found")}},
                 QStringLiteral("HTTP 404"));
        else
            done(true, payload, QString());
    };
    AppStoreStatusFeature page(host);
    page.resize(900, 700);
    page.refresh();
    const QList<QToolButton *> buttons = page.findChildren<QToolButton *>(QStringLiteral("appStoreScreenshot"));
    check(buttons.size() == 2, "each screenshot gets a thumbnail button");
    check(buttons.size() == 2 && buttons[0]->property("loaded").toBool() && !buttons[0]->icon().isNull() &&
              !buttons[1]->property("loaded").toBool() && buttons[1]->text() == QLatin1String("Not uploaded"),
          "thumbnails show the picture, or say it is not uploaded yet");
    check(calls.count(QStringLiteral("GET ") + screenshotPath(first)) == 1,
          "each screenshot is fetched once", calls.join(QStringLiteral(", ")));
    page.refresh();
    check(calls.count(QStringLiteral("GET ") + screenshotPath(first)) == 1,
          "a refresh reuses screenshots already fetched", calls.join(QStringLiteral(", ")));
    QToolButton *opened = page.findChildren<QToolButton *>(QStringLiteral("appStoreScreenshot")).value(0);
    if (opened)
        opened->click();
    auto *dialog = page.findChild<QDialog *>(QStringLiteral("appStoreScreenshotDialog"));
    auto *image = dialog ? dialog->findChild<QLabel *>(QStringLiteral("appStoreScreenshotImage")) : nullptr;
    check(dialog && image && !image->pixmap().isNull() && image->pixmap().height() == 80,
          "clicking a thumbnail opens the full-size picture");
    check(dialog && hasLabel(dialog, QStringLiteral("Screenshot 1 of 2")),
          "the viewer says which screenshot it shows");
    if (dialog)
        dialog->close();
}

}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication app(argc, argv);
    testTones();
    testTimes();
    testSnapshotParts();
    testPage();
    testScreenshots();
    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
