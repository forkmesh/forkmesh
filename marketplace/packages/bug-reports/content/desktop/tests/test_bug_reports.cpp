#include "BugReportsFeature.h"
#include "BugReportsModel.h"

#include <QApplication>
#include <QComboBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>

#include <cstdio>

using namespace forkmesh::bugreports;

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

const QString kReportA = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
const QString kReportB = QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");

QJsonObject listPayload(bool more)
{
    QJsonObject payload = QJsonDocument::fromJson(QStringLiteral(R"({
  "ok": true,
  "counts": {"new": 1, "triaged": 1, "closed": 0},
  "reports": [
    {"id": "%1", "status": "new", "severity": "crash", "platform": "linux",
     "appVersion": "1.2.3", "createdAt": 1767225600000, "message": "The window froze",
     "screen": "Bugs", "account": "alice", "contact": "alice@example.com",
     "attachments": [{"name": "shot.png", "size": 10}]},
    {"id": "%2", "status": "triaged", "severity": "normal", "platform": "ios",
     "createdAt": 1767225500000, "message": "Typo on Settings", "confirmedFixedAt": 0},
    {"status": "new", "message": "no id, dropped"}
  ]
})").arg(kReportA, kReportB).toUtf8()).object();
    payload.insert(QStringLiteral("nextOffset"), more ? QJsonValue(50) : QJsonValue());
    return payload;
}

void testModel()
{
    const ReportList list = parseReportList(listPayload(true));
    check(list.reports.size() == 2, "reports without an id are dropped");
    check(list.counts.fresh == 1 && list.counts.triaged == 1 && list.counts.closed == 0,
          "the status counts are read");
    check(list.more, "a next offset means more reports");
    check(!parseReportList(listPayload(false)).more, "a null next offset means no more");
    const BugReport first = list.reports.first();
    check(first.attachments == 1 && first.account == QLatin1String("alice"),
          "attachments and the reporter are read");
    check(detailsLine(first) == QStringLiteral("linux · 1.2.3 · Bugs · 1 attachment"),
          "the details line joins what is known", detailsLine(first));

    check(inboxStatusLabel(QStringLiteral("triaged")) == QLatin1String("Triaged"),
          "administrators see the stored status");
    check(mineStatusLabel(first) == QLatin1String("Received") &&
              mineStatusLabel(list.reports.at(1)) == QLatin1String("In progress"),
          "reporters see what happened to their report");
    BugReport fixed = first;
    fixed.status = QStringLiteral("closed");
    fixed.confirmedFixedAt = 5;
    check(mineStatusLabel(fixed) == QLatin1String("Fixed, confirmed by you") &&
              !canConfirmFixed(fixed) && canConfirmFixed(first),
          "a confirmed fix cannot be confirmed again");

    check(!messageLongEnough(QStringLiteral("  ab ")) && messageLongEnough(QStringLiteral("abcd")),
          "a stray tap is not a report");
    Draft draft;
    draft.message = QStringLiteral("  It crashed  ");
    draft.contact = QStringLiteral(" ");
    draft.severity = QStringLiteral("bogus");
    draft.appVersion = QStringLiteral("2.0.0");
    const QJsonObject payload = reportPayload(draft);
    check(payload.value(QStringLiteral("message")).toString() == QLatin1String("It crashed"),
          "the message is trimmed");
    check(payload.value(QStringLiteral("severity")).toString() == QLatin1String("normal"),
          "an unknown severity falls back to normal");
    check(payload.value(QStringLiteral("source")).toString() == QLatin1String("desktop") &&
              payload.value(QStringLiteral("platform")).toString() == desktopPlatform(),
          "the report says it came from the desktop");
    check(!payload.contains(QStringLiteral("contact")) &&
              payload.value(QStringLiteral("appVersion")).toString() == QLatin1String("2.0.0"),
          "an empty contact is left out and the app version is sent");
    draft.message = QString(kMaxMessage + 50, QLatin1Char('x'));
    check(reportPayload(draft).value(QStringLiteral("message")).toString().size() == kMaxMessage,
          "long messages are clipped to what the server stores");

    check(confirmFixedPayload(kReportA).value(QStringLiteral("fixed")).toBool() &&
              statusPayload(kReportA, QStringLiteral("closed")).value(QStringLiteral("status")) ==
                  QStringLiteral("closed"),
          "the action payloads match the server");
    check(signedOutError(QStringLiteral("sign in required")) &&
              signedOutError(QStringLiteral("Sign in to your ForkMesh account on this device to use this package.")) &&
              adminRequiredError(QStringLiteral("admin required")) &&
              !adminRequiredError(QStringLiteral("HTTP 500")),
          "sign-in and administrator answers are told apart");
    check(sendErrorText(QStringLiteral("rate limited")).contains(QStringLiteral("last hour")),
          "the hourly limit is explained");
}

struct FakeHost {
    QStringList calls;
    QList<QJsonObject> bodies;
    bool admin = false;
    bool signedIn = true;
    int badgeChanges = 0;
    forkmesh::market::NativePackageHost host()
    {
        forkmesh::market::NativePackageHost result;
        result.request = [this](const QByteArray &method, const QString &path,
                                const QJsonObject &body,
                                forkmesh::market::NativePackageReplyHandler done) {
            calls.append(QString::fromLatin1(method) + QLatin1Char(' ') + path);
            bodies.append(body);
            if (!signedIn && path != QLatin1String("/api/bug-reports")) {
                done(false, {}, QStringLiteral("sign in required"));
                return;
            }
            if (path.startsWith(QLatin1String("/api/bug-reports/inbox"))) {
                if (!admin) {
                    done(false, {}, QStringLiteral("admin required"));
                    return;
                }
                done(true, method == "GET" ? listPayload(false)
                                           : QJsonObject{{QStringLiteral("ok"), true}},
                     QString());
                return;
            }
            if (path == QLatin1String("/api/bug-reports/mine")) {
                if (method == "POST") {
                    done(true,
                         QJsonObject{{QStringLiteral("ok"), true},
                                     {QStringLiteral("report"),
                                      QJsonObject{{QStringLiteral("id"), body.value(QStringLiteral("report"))},
                                                  {QStringLiteral("status"), QStringLiteral("closed")},
                                                  {QStringLiteral("message"), QStringLiteral("Typo on Settings")},
                                                  {QStringLiteral("confirmedFixedAt"), 9}}}},
                         QString());
                    return;
                }
                done(true, listPayload(false), QString());
                return;
            }
            done(true, QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("id"), kReportA}},
                 QString());
        };
        result.navigationBadgeChanged = [this] { ++badgeChanges; };
        return result;
    }
};

QPushButton *actionFor(QWidget *root, const QString &report, const QString &action)
{
    for (QPushButton *candidate : root->findChildren<QPushButton *>()) {
        if (candidate->property("bugReportAction").toString() != action || !candidate->isVisibleTo(root))
            continue;
        QWidget *parent = candidate->parentWidget();
        while (parent && parent->property("bugReport").toString().isEmpty())
            parent = parent->parentWidget();
        if (parent && parent->property("bugReport").toString() == report)
            return candidate;
    }
    return nullptr;
}

QPushButton *tabButton(QWidget *root, const char *name)
{
    for (QPushButton *candidate : root->findChildren<QPushButton *>()) {
        if (candidate->property("bugReportsTab").toString() == QLatin1String(name))
            return candidate;
    }
    return nullptr;
}

void testRegistration()
{
    const forkmesh::market::NativePackageContribution *contribution =
        forkmesh::market::nativePackageContribution(QStringLiteral("builtin:bug-reports"),
                                                    QStringLiteral("extension.native"));
    check(contribution && contribution->navigationLabel == QLatin1String("Bugs") &&
              contribution->factory,
          "the desktop accepts the Bugs page registration");
}

void testPage()
{
    FakeHost fake;
    BugReportsFeature page(fake.host());
    page.resize(800, 600);
    page.show();
    page.refresh();
    check(fake.calls.contains(QStringLiteral("GET /api/bug-reports/mine")) &&
              fake.calls.contains(QStringLiteral("GET /api/bug-reports/inbox?status=new")),
          "refresh reads my reports and the inbox", fake.calls.join(QStringLiteral(", ")));
    check(!page.admin() && !tabButton(&page, "reported")->isVisibleTo(&page),
          "the Reported tab stays hidden for someone who is not an administrator");
    check(page.navigationBadge() == 0, "non-administrators get no badge");
    check(tabButton(&page, "mine")->text() == QLatin1String("My reports (2)"),
          "the My reports tab counts open reports", tabButton(&page, "mine")->text());

    auto *message = page.findChild<QPlainTextEdit *>(QStringLiteral("bugReportsMessage"));
    auto *send = page.findChild<QPushButton *>(QStringLiteral("primaryButton"));
    message->setPlainText(QStringLiteral("ab"));
    fake.calls.clear();
    fake.bodies.clear();
    send->click();
    check(!fake.calls.contains(QStringLiteral("POST /api/bug-reports")),
          "a too-short report is not sent");
    message->setPlainText(QStringLiteral("The rail lost its Bugs button"));
    page.findChild<QComboBox *>(QStringLiteral("bugReportsSeverity"))->setCurrentIndex(2);
    page.findChild<QLineEdit *>(QStringLiteral("bugReportsContact"))->setText(QStringLiteral("me"));
    send->click();
    const int posted = fake.calls.indexOf(QStringLiteral("POST /api/bug-reports"));
    check(posted >= 0, "a report is posted", fake.calls.join(QStringLiteral(", ")));
    if (posted >= 0) {
        const QJsonObject body = fake.bodies.at(posted);
        check(body.value(QStringLiteral("severity")).toString() == QLatin1String("high") &&
                  body.value(QStringLiteral("contact")).toString() == QLatin1String("me"),
              "the severity and contact are sent");
    }
    check(message->toPlainText().isEmpty(), "the form clears after sending");
    check(fake.calls.contains(QStringLiteral("GET /api/bug-reports/mine")),
          "sending reads my reports again");

    page.showTab(Tab::Mine);
    QPushButton *confirm = actionFor(&page, kReportB, QStringLiteral("fixed"));
    check(confirm, "an open report offers Confirm it's fixed");
    if (confirm) {
        fake.calls.clear();
        fake.bodies.clear();
        confirm->click();
        check(fake.calls.contains(QStringLiteral("POST /api/bug-reports/mine")) &&
                  fake.bodies.last().value(QStringLiteral("fixed")).toBool(),
              "confirming posts the fix");
        check(!actionFor(&page, kReportB, QStringLiteral("fixed")),
              "a confirmed report no longer offers the button");
    }

    page.showTab(Tab::Reported);
    check(page.currentTab() == Tab::Mine, "a non-administrator cannot open Reported");

    FakeHost adminFake;
    adminFake.admin = true;
    BugReportsFeature adminPage(adminFake.host());
    adminPage.resize(800, 600);
    adminPage.show();
    adminPage.prepareNavigationBadge();
    check(adminPage.admin() && tabButton(&adminPage, "reported")->isVisibleTo(&adminPage),
          "administrators get the Reported tab");
    check(adminPage.navigationBadge() == 1 && adminFake.badgeChanges == 1,
          "the rail badge counts new reports for administrators");
    adminPage.showTab(Tab::Reported);
    QPushButton *close = actionFor(&adminPage, kReportA, QStringLiteral("closed"));
    check(close, "an inbox report can be closed");
    if (close) {
        adminFake.calls.clear();
        adminFake.bodies.clear();
        close->click();
        check(adminFake.calls.value(0) == QLatin1String("POST /api/bug-reports/inbox") &&
                  adminFake.bodies.value(0).value(QStringLiteral("status")).toString() ==
                      QLatin1String("closed") &&
                  adminFake.calls.contains(QStringLiteral("GET /api/bug-reports/inbox?status=new")),
              "closing posts the status and reloads the inbox",
              adminFake.calls.join(QStringLiteral(", ")));
    }
    adminFake.calls.clear();
    adminFake.bodies.clear();
    auto *filter = adminPage.findChild<QComboBox *>(QStringLiteral("bugReportsInboxFilter"));
    filter->setCurrentIndex(filter->findData(QString()));
    check(adminFake.calls.contains(QStringLiteral("GET /api/bug-reports/inbox")),
          "the All filter reads every report", adminFake.calls.join(QStringLiteral(", ")));
    check(!adminPage.searchHits(QStringLiteral("froze"), 5).isEmpty(),
          "the top search finds reports by their text");

    FakeHost signedOut;
    signedOut.signedIn = false;
    BugReportsFeature guest(signedOut.host());
    guest.refresh();
    bool asked = false;
    for (QLabel *label : guest.findChildren<QLabel *>(QStringLiteral("emptyStateTitle")))
        asked = asked || label->text().contains(QStringLiteral("Sign in"));
    check(asked, "a signed-out page asks to sign in");
}

}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication app(argc, argv);
    testModel();
    testRegistration();
    testPage();
    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
