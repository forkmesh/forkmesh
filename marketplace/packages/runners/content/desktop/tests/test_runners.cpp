#include "RunnersFeature.h"
#include "RunnersModel.h"

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>

#include <cstdio>

using namespace forkmesh::runnerspage;

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

const QString kAccountA = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
const QString kAccountB = QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
const QString kRequest = QStringLiteral("cccccccccccccccccccccccccccccccc");

QJsonObject payloadAt(qint64 now)
{
    const QString text = QStringLiteral(R"({
  "ok": true,
  "now": %1,
  "runners": [
    {"name": "zeta", "scope": "personal", "status": "revoked", "lastSeenAt": %2, "activeLeases": 0,
     "availability": {"codex": {"binaryFound": true, "loginState": "missing", "accounts": []}}},
    {"name": "box", "scope": "personal", "status": "active", "lastSeenAt": %3, "activeLeases": 2,
     "availability": {
       "claude-code": {"binaryFound": true, "loginState": "available", "auth": {"state": "ok"},
         "accounts": [{"id": "%5", "label": "work", "active": true},
                      {"id": "%6", "email": "me@example.com", "active": false}]},
       "codex": {"binaryFound": true, "loginState": "missing", "accounts": []}},
     "accountRequests": [{"id": "%7", "provider": "claude-code", "action": "activate",
                          "account": "%6", "state": "pending"}]},
    {"name": "attic", "scope": "pool", "status": "paused", "lastSeenAt": %4, "activeLeases": 0,
     "availability": {
       "claude-code": {"binaryFound": false, "loginState": "missing"},
       "codex": {"binaryFound": true, "loginState": "expired", "auth": {"state": "expired"}}}}
  ]
})")
                             .arg(now)
                             .arg(now - 3 * 86400000LL)
                             .arg(now - 2 * 60 * 1000)
                             .arg(now - 11 * 60 * 1000)
                             .arg(kAccountA, kAccountB, kRequest);
    return QJsonDocument::fromJson(text.toUtf8()).object();
}

forkmesh::runners::RunnerInfo runnerWith(const QString &status, qint64 lastSeen)
{
    forkmesh::runners::RunnerInfo runner;
    runner.name = QStringLiteral("box");
    runner.status = status;
    runner.lastSeenAtMs = lastSeen;
    return runner;
}

void testStatus()
{
    const qint64 now = 1760000000000LL;
    check(runnerOnline(runnerWith(QStringLiteral("active"), now - 9 * 60 * 1000), now) &&
              !runnerOnline(runnerWith(QStringLiteral("active"), now - 11 * 60 * 1000), now) &&
              !runnerOnline(runnerWith(QStringLiteral("active"), 0), now),
          "a runner is online when it was seen in the last 10 minutes");
    check(!runnerOnline(runnerWith(QStringLiteral("revoked"), now - 1000), now),
          "a revoked runner is never shown online");
    check(runnerStatusLabel(QStringLiteral("active")).isEmpty() &&
              runnerStatusLabel(QStringLiteral("paused")) == QStringLiteral("Paused") &&
              runnerStatusLabel(QStringLiteral("revoked")) == QStringLiteral("Revoked"),
          "only paused and revoked runners carry a status label");
    check(jobsText(-1).isEmpty() && jobsText(0) == QStringLiteral("No active jobs") &&
              jobsText(1) == QStringLiteral("1 active job") &&
              jobsText(3) == QStringLiteral("3 active jobs"),
          "the active job count reads as words");
    check(actionsFor(runnerWith(QStringLiteral("active"), now)) ==
                  QList<RunnerAction>{RunnerAction::Pause, RunnerAction::Revoke} &&
              actionsFor(runnerWith(QStringLiteral("paused"), now)) ==
                  QList<RunnerAction>{RunnerAction::Resume, RunnerAction::Revoke} &&
              actionsFor(runnerWith(QStringLiteral("revoked"), now)).isEmpty(),
          "active runners pause, paused runners resume, revoked runners offer nothing");
}

void testRows()
{
    const qint64 now = 1760000000000LL;
    const QList<RunnerRow> rows = runnerRows(payloadAt(now), now);
    QStringList names;
    for (const RunnerRow &row : rows)
        names.append(row.name);
    check(names == QStringList{QStringLiteral("attic"), QStringLiteral("box"), QStringLiteral("zeta")},
          "runners are sorted by name with revoked runners last", names.join(QLatin1Char(',')));
    if (rows.size() != 3)
        return;
    const RunnerRow &attic = rows.at(0);
    const RunnerRow &box = rows.at(1);
    const RunnerRow &zeta = rows.at(2);
    check(box.online && !attic.online && box.detail.contains(QStringLiteral("2 active jobs")) &&
              box.detail.startsWith(QStringLiteral("Online")),
          "the detail line says online, last seen and the job count", box.detail);
    check(box.scope == QStringLiteral("Personal") && attic.scope == QStringLiteral("Pool"),
          "the scope is labelled");
    check(zeta.revoked && zeta.providers.isEmpty() && zeta.actions.isEmpty(),
          "a revoked runner lists no providers and no actions");
    check(box.providers.size() == 2 && box.providers.at(0).title == QStringLiteral("Claude Code") &&
              box.providers.at(1).title == QStringLiteral("Codex"),
          "each runner shows Claude Code and Codex");
    const ProviderRow &claude = box.providers.at(0);
    check(claude.state == QStringLiteral("Signed in") && claude.tone == QStringLiteral("success") &&
              claude.account == QStringLiteral("work") && !claude.signInOffered &&
              claude.accountsOffered && claude.accounts.size() == 2 &&
              claude.accounts.at(1).pending && claude.accounts.at(1).name ==
                  QStringLiteral("me@example.com"),
          "a signed-in provider shows its active account and its account menu");
    const ProviderRow &codex = box.providers.at(1);
    check(codex.state == QStringLiteral("Needs sign-in") && codex.signInOffered &&
              codex.signInText == QStringLiteral("Sign in Codex") && !codex.accountsOffered,
          "a provider that needs a sign-in offers Sign in");
    check(!attic.providers.at(0).signInOffered &&
              attic.providers.at(0).state == QStringLiteral("Not installed"),
          "a provider whose CLI is missing offers no sign-in");
    check(attic.providers.at(1).signInOffered &&
              attic.providers.at(1).tone == QStringLiteral("danger") &&
              attic.providers.at(1).state == QStringLiteral("Sign-in expired"),
          "a paused runner can still sign an expired provider in");
}

void testRequests()
{
    check(actionPath(RunnerAction::Pause) == QStringLiteral("/api/runners/pause") &&
              actionPath(RunnerAction::Resume) == QStringLiteral("/api/runners/resume") &&
              actionPath(RunnerAction::Revoke) == QStringLiteral("/api/runners/revoke"),
          "pause, resume and revoke post to their own route");
    check(actionBody(QStringLiteral(" Box ")) ==
              QJsonObject{{QStringLiteral("name"), QStringLiteral("box")}},
          "the action body names the runner");
    check(accountsPath(QStringLiteral("box")) == QStringLiteral("/api/runners/box/accounts"),
          "account changes post to the runner's accounts route");
    const QJsonObject body = accountBody(QStringLiteral("codex"), kAccountA, QStringLiteral("remove"));
    check(body.value(QStringLiteral("provider")).toString() == QStringLiteral("codex") &&
              body.value(QStringLiteral("account")).toString() == kAccountA &&
              body.value(QStringLiteral("action")).toString() == QStringLiteral("remove"),
          "the account body carries provider, account and action");
    check(connectPath(QStringLiteral("box"), QStringLiteral("claude-code")) ==
              QStringLiteral("/dashboard/runners?runner=box&provider=claude-code&connect=1"),
          "Sign in opens the web connect page for that runner and provider");
    check(addRunnerPath() == QStringLiteral("/dashboard/runners"), "Add runner opens the runners page");
    check(signedOutError(QStringLiteral("unauthorized")) && signedOutError(QStringLiteral("HTTP 401")) &&
              signedOutError(QStringLiteral("Sign in to your ForkMesh account on this device to use "
                                            "this package.")) &&
              !signedOutError(QStringLiteral("not found")),
          "a refused session reads as signed out");
    check(actionErrorText(QStringLiteral("runner inactive")) == QStringLiteral("That runner is revoked."),
          "a revoked runner's refusal is explained");
    const QJsonObject attention = QJsonDocument::fromJson(QStringLiteral(R"({
      "ok": true,
      "attention": [{"runner": "box", "provider": "codex", "state": "expired"},
                    {"runner": "attic", "provider": "claude-code", "state": "expiring"}]
    })").toUtf8()).object();
    check(attentionCount(attention) == 2 && attentionCount({}) == 0,
          "the badge counts the attention items");
}

struct FakeHost {
    QStringList calls;
    QList<QJsonObject> bodies;
    QStringList opened;
    int badgeChanges = 0;
    QJsonObject runners;
    QString runnersError;
    forkmesh::market::NativePackageHost host()
    {
        forkmesh::market::NativePackageHost result;
        result.request = [this](const QByteArray &method, const QString &path, const QJsonObject &body,
                                forkmesh::market::NativePackageReplyHandler done) {
            calls.append(QString::fromLatin1(method) + QLatin1Char(' ') + path);
            bodies.append(body);
            if (path == QLatin1String("/api/runners")) {
                if (!runnersError.isEmpty())
                    done(false, {}, runnersError);
                else
                    done(true, runners, QString());
                return;
            }
            if (path == QLatin1String("/api/runners/attention")) {
                done(true,
                     QJsonObject{{QStringLiteral("attention"),
                                  QJsonArray{QJsonObject{{QStringLiteral("runner"), QStringLiteral("attic")},
                                                         {QStringLiteral("provider"), QStringLiteral("codex")},
                                                         {QStringLiteral("state"), QStringLiteral("expired")}}}}},
                     QString());
                return;
            }
            done(true, QJsonObject{{QStringLiteral("ok"), true}}, QString());
        };
        result.navigationBadgeChanged = [this] { ++badgeChanges; };
        result.openWebPage = [this](const QString &path) { opened.append(path); };
        return result;
    }
};

QPushButton *buttonIn(QWidget *root, const char *property, const QString &value,
                      const QString &runner = QString())
{
    for (QPushButton *candidate : root->findChildren<QPushButton *>()) {
        if (candidate->property(property).toString() != value)
            continue;
        if (!runner.isEmpty()) {
            QWidget *parent = candidate->parentWidget();
            while (parent && parent->property("runner").toString().isEmpty())
                parent = parent->parentWidget();
            if (!parent || parent->property("runner").toString() != runner)
                continue;
        }
        return candidate;
    }
    return nullptr;
}

QWidget *runnerFrame(QWidget *root, const QString &name)
{
    for (QWidget *frame : root->findChildren<QWidget *>(QStringLiteral("settingsRow")))
        if (frame->property("runner").toString() == name)
            return frame;
    return nullptr;
}

QString stateOf(QWidget *root)
{
    QWidget *empty = root->findChild<QWidget *>(QStringLiteral("emptyState"));
    return empty ? empty->property("runnersState").toString() : QString();
}

void settle()
{
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void testPage()
{
    FakeHost fake;
    fake.runners = payloadAt(QDateTime::currentMSecsSinceEpoch());
    RunnersFeature page(fake.host());
    page.resize(900, 700);
    page.refresh();
    settle();
    check(fake.calls == QStringList{QStringLiteral("GET /api/runners"),
                                    QStringLiteral("GET /api/runners/attention")},
          "refresh reads the runners and their attention", fake.calls.join(QStringLiteral(", ")));
    check(page.navigationBadge() == 1 && fake.badgeChanges == 1,
          "the rail badge counts the attention items");
    check(runnerFrame(&page, QStringLiteral("box")) && runnerFrame(&page, QStringLiteral("attic")) &&
              runnerFrame(&page, QStringLiteral("zeta")),
          "one row per runner");
    QWidget *zeta = runnerFrame(&page, QStringLiteral("zeta"));
    check(zeta && zeta->findChildren<QPushButton *>().isEmpty(), "a revoked runner has no buttons");
    QWidget *attic = runnerFrame(&page, QStringLiteral("attic"));
    bool pausedLabel = false;
    if (attic)
        for (QLabel *badge : attic->findChildren<QLabel *>(QStringLiteral("badge")))
            pausedLabel = pausedLabel || (badge->property("runnerStatus").toString() ==
                                              QLatin1String("paused") &&
                                          badge->text() == QStringLiteral("Paused"));
    check(pausedLabel && buttonIn(&page, "runnerAction", QStringLiteral("resume"), QStringLiteral("attic")),
          "a paused runner is labelled and offers Resume");

    fake.calls.clear();
    fake.bodies.clear();
    if (QPushButton *pause = buttonIn(&page, "runnerAction", QStringLiteral("pause"), QStringLiteral("box")))
        pause->click();
    settle();
    check(fake.calls.value(0) == QStringLiteral("POST /api/runners/pause") &&
              fake.bodies.value(0).value(QStringLiteral("name")).toString() == QStringLiteral("box") &&
              fake.calls.value(1) == QStringLiteral("GET /api/runners"),
          "Pause posts the runner name and reloads", fake.calls.join(QStringLiteral(", ")));
    auto *status = page.findChild<QLabel *>(QStringLiteral("statusLine"));
    check(status && status->text().contains(QStringLiteral("box is paused")),
          "the page says the runner is paused", status ? status->text() : QString());

    fake.calls.clear();
    if (QPushButton *revoke = buttonIn(&page, "runnerAction", QStringLiteral("revoke"), QStringLiteral("box")))
        revoke->click();
    auto *confirm = page.findChild<QMessageBox *>(QStringLiteral("runnerRevokeConfirm"));
    check(confirm && fake.calls.isEmpty(), "Revoke asks before it does anything");
    if (confirm) {
        for (QAbstractButton *choice : confirm->buttons())
            if (choice->text() == QStringLiteral("Revoke"))
                choice->click();
    }
    QApplication::processEvents();
    settle();
    check(fake.calls.value(0) == QStringLiteral("POST /api/runners/revoke"),
          "confirming Revoke posts it", fake.calls.join(QStringLiteral(", ")));

    fake.opened.clear();
    if (QPushButton *signIn = buttonIn(&page, "runnerSignIn", QStringLiteral("codex"), QStringLiteral("box")))
        signIn->click();
    check(fake.opened == QStringList{QStringLiteral("/dashboard/runners?runner=box&provider=codex&connect=1")},
          "Sign in opens the connect page on the active host", fake.opened.join(QStringLiteral(", ")));
    if (QPushButton *add = buttonIn(&page, "runnersAdd", QStringLiteral("true")))
        add->click();
    check(fake.opened.value(1) == QStringLiteral("/dashboard/runners"), "Add runner opens the runners page");

    fake.calls.clear();
    fake.bodies.clear();
    QPushButton *accounts = buttonIn(&page, "runnerAccounts", QStringLiteral("claude-code"), QStringLiteral("box"));
    QAction *pendingUse = nullptr;
    QAction *remove = nullptr;
    if (accounts && accounts->menu()) {
        pendingUse = accounts->menu()->findChild<QAction *>(QStringLiteral("runnerAccountUse-") + kAccountB);
        remove = accounts->menu()->findChild<QAction *>(QStringLiteral("runnerAccountRemove-") + kAccountA);
    }
    check(pendingUse && !pendingUse->isEnabled() && remove && remove->isEnabled(),
          "a pending switch is disabled and other accounts can be removed");
    if (remove)
        remove->trigger();
    auto *removeConfirm = page.findChild<QMessageBox *>(QStringLiteral("runnerAccountRemoveConfirm"));
    if (removeConfirm)
        for (QAbstractButton *choice : removeConfirm->buttons())
            if (choice->text() == QStringLiteral("Remove"))
                choice->click();
    QApplication::processEvents();
    settle();
    check(fake.calls.value(0) == QStringLiteral("POST /api/runners/box/accounts") &&
              fake.bodies.value(0).value(QStringLiteral("action")).toString() == QStringLiteral("remove") &&
              fake.bodies.value(0).value(QStringLiteral("account")).toString() == kAccountA,
          "removing an account asks, then posts the account change", fake.calls.join(QStringLiteral(", ")));

    fake.runners = QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("runners"), QJsonArray{}}};
    page.refresh();
    settle();
    check(stateOf(&page) == QStringLiteral("runnersEmpty"), "no runners shows the empty state");

    fake.runnersError = QStringLiteral("unauthorized");
    page.refresh();
    settle();
    check(stateOf(&page) == QStringLiteral("runnersSignedOut") && page.navigationBadge() == 0,
          "a refused session shows the signed-out state");

    fake.runnersError = QStringLiteral("HTTP 500");
    page.refresh();
    settle();
    auto *error = page.findChild<QLabel *>(QStringLiteral("runnersError"));
    check(error && error->text().contains(QStringLiteral("HTTP 500")), "a failed load says why");
}

}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication app(argc, argv);
    testStatus();
    testRows();
    testRequests();
    testPage();
    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
