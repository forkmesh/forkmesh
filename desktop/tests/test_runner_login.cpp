#include "NodeDirectoryRows.h"
#include "RunnerListView.h"
#include "RunnerLogin.h"
#include "TerminalWidget.h"

#include <QApplication>
#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMenu>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QUrlQuery>

#include <iostream>

using namespace forkmesh::runners;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

class UrlCatcher : public QObject
{
    Q_OBJECT
public:
    QList<QUrl> opened;
public slots:
    void open(const QUrl &url) { opened.append(url); }
};

static const QString kOpenId = QStringLiteral("0123456789abcdef0123456789abcdef");
static const QString kNewId = QStringLiteral("fedcba9876543210fedcba9876543210");

static QJsonObject runnersPayload()
{
    return QJsonDocument::fromJson(QStringLiteral(R"json({
      "ok": true, "providers": ["claude-code", "codex"], "now": 1000000,
      "runners": [
        {"name": "fmnext-n1", "scope": "pool", "status": "active",
         "registeredBy": "jett", "allowedOwners": ["jett"],
         "lastSeenAt": 970000, "availabilityAt": 970000,
         "availability": {
           "claude-code": {"binaryFound": true, "loginState": "missing",
                           "source": ""},
           "codex": {"binaryFound": false, "loginState": "missing"}
         },
         "logins": []},
        {"name": "fmnext-r1", "scope": "pool", "status": "active",
         "lastSeenAt": 0,
         "availability": {
           "claude-code": {"binaryFound": true, "loginState": "available",
                           "source": "credentials-file"},
           "codex": {"binaryFound": true, "loginState": "missing"}
         },
         "logins": [
           {"id": "%1", "runner": "fmnext-r1", "provider": "codex",
            "state": "running", "createdAt": 1, "expiresAt": 2},
           {"id": "not-an-id", "provider": "claude-code", "state": "running"}
         ]},
        {"name": "old-runner", "scope": "personal", "status": "revoked",
         "availability": {}, "logins": []},
        {"name": "Bad Name!", "status": "active"}
      ]})json")
                                       .arg(kOpenId)
                                       .toUtf8())
        .object();
}

static void testParseRunners()
{
    const QList<RunnerInfo> runners = parseRunners(runnersPayload());
    check(runners.size() == 3, "runners with invalid names are dropped");
    if (runners.size() != 3)
        return;
    const RunnerInfo &n1 = runners.at(0);
    check(n1.name == "fmnext-n1" && n1.scope == "pool" &&
              n1.status == "active" && n1.registeredBy == "jett" &&
              n1.lastSeenAtMs == 970000,
          "runner fields are parsed");
    check(runnerProviderStateText(n1.provider("claude-code")) ==
              "Needs sign-in",
          "an installed CLI that is not signed in needs sign-in");
    check(runnerProviderStateText(n1.provider("codex")) == "Not installed",
          "a missing binary reads as not installed");
    const RunnerInfo &r1 = runners.at(1);
    check(r1.provider("claude-code").signedIn() &&
              runnerProviderStateText(r1.provider("claude-code")) ==
                  "Signed in",
          "loginState available means signed in");
    check(r1.logins.size() == 1, "logins with malformed ids are dropped");
    check(r1.openLoginId("codex") == kOpenId &&
              r1.openLoginId("claude-code").isEmpty(),
          "the open sign-in of a provider is found");
    check(runnerProviderStateText(runners.at(2).provider("claude-code")) ==
              "Not reported",
          "a runner that reported nothing says so");
    check(runnerProviderLabel("claude-code") == "Claude" &&
              runnerProviderLabel("codex") == "Codex",
          "provider labels");

    check(runnerForTarget(runners, "fmnext-r1", "claude-code") == "fmnext-r1",
          "a run's own runner is picked by name");
    check(runnerForTarget(runners, "jett", "claude-code") == "fmnext-n1",
          "otherwise the one active runner still needing that sign-in");
    check(runnerForTarget(runners, "", "codex") == "fmnext-r1",
          "a runner without the CLI is not offered for its sign-in");
    QList<RunnerInfo> one = {runners.at(1)};
    check(runnerForTarget(one, "", "claude-code") == "fmnext-r1",
          "a single active runner is picked");
    check(runnerForTarget({}, "x", "claude-code").isEmpty(),
          "no runners, no pick");
}

static void testRequests()
{
    const QUrl base(QStringLiteral("https://app.forkmesh.com/dashboard?x=1#y"));
    check(runnersListUrl(base).toString() ==
              "https://app.forkmesh.com/api/runners",
          "the list URL replaces path, query and fragment");
    check(runnerLoginStartUrl(base, " FMNext-N1 ").path() ==
              "/api/runners/fmnext-n1/login",
          "the start URL names the lower-cased runner");
    const QUrl output = runnerLoginOutputUrl(base, "fmnext-n1", kNewId, 7);
    check(output.path() == "/api/runners/fmnext-n1/login/" + kNewId + "/output",
          "the output path names the session");
    check(QUrlQuery(output).queryItemValue("after") == "7",
          "output is read after the last seq");
    check(QUrlQuery(runnerLoginOutputUrl(base, "fmnext-n1", kNewId, -3))
                  .queryItemValue("after") == "0",
          "a negative seq reads from the start");
    check(runnerLoginInputUrl(base, "fmnext-n1", kNewId).path() ==
              "/api/runners/fmnext-n1/login/" + kNewId + "/input",
          "the input path");
    check(runnerLoginCancelUrl(base, "fmnext-n1", kNewId).path() ==
              "/api/runners/fmnext-n1/login/" + kNewId + "/cancel",
          "the cancel path");
    check(runnerLoginStartBody("codex") == QByteArray(R"({"provider":"codex"})"),
          "the start body names only the provider");
    check(runnerLoginInputBody("abc\r") ==
              QByteArray(R"({"data":"abc\r"})"),
          "the input body carries the text as data");
}

static void testInputValidation()
{
    check(runnerLoginInputValid("abc-DEF_123\r"), "printable text and CR");
    check(runnerLoginInputValid("a\tb\n"), "tab and LF are allowed");
    check(runnerLoginInputValid(QString::fromUtf8("caf\xC3\xA9\r")),
          "non-ASCII printable text is allowed");
    check(!runnerLoginInputValid(""), "empty input is refused");
    check(!runnerLoginInputValid("\x1b[A"), "escape sequences are refused");
    check(!runnerLoginInputValid("ab\x7f"), "DEL is refused");
    check(!runnerLoginInputValid(QString(QChar(0x03))), "Ctrl-C is refused");
    check(runnerLoginInputValid(QString(kRunnerLoginMaxInputBytes, QLatin1Char('a'))),
          "4096 bytes are allowed");
    check(!runnerLoginInputValid(QString(kRunnerLoginMaxInputBytes + 1, QLatin1Char('a'))),
          "4097 bytes are refused");
    check(!runnerLoginInputValid(
              QString(kRunnerLoginMaxInputBytes / 2 + 1, QChar(0x00e9))),
          "the limit counts UTF-8 bytes");

    check(runnerLoginCodeSubmission("  code#state\n") == "code#state\r",
          "a pasted code is trimmed and sent with CR");
    check(runnerLoginCodeSubmission(" \r\n").isEmpty(),
          "a blank code sends nothing");

    QStringList submitted;
    QString line = applyRunnerLoginKeystrokes("", "ab\x7f" "c", &submitted);
    check(line == "ac" && submitted.isEmpty(),
          "keystrokes build a line and backspace edits it locally");
    line = applyRunnerLoginKeystrokes(line, "d\rxy\x1b", &submitted);
    check(submitted == QStringList{"acd\r"} && line == "xy",
          "Enter submits the line with CR and drops escapes");
    line = applyRunnerLoginKeystrokes(line, "\x15z", &submitted);
    check(line == "z", "Ctrl-U clears the line");
}

static QJsonObject chunk(qint64 seq, const QByteArray &data)
{
    return QJsonObject{{"seq", seq}, {"data", QString::fromLatin1(data.toBase64())}};
}

static void testSessionState()
{
    RunnerLoginSession started("FMNEXT-N1", "Claude-Code");
    check(started.runner() == "fmnext-n1" && started.provider() == "claude-code",
          "runner and provider are normalised");
    check(!started.needsCancelOnClose(), "nothing to cancel before a start");
    check(started.applyStartReply(
              201, QJsonObject{{"ok", true},
                               {"id", kNewId},
                               {"session", QJsonObject{{"state", "pending"}}}}) ==
              RunnerLoginSession::Start::Started,
          "201 starts a session");
    check(started.id() == kNewId && started.open() &&
              started.needsCancelOnClose(),
          "a started session is open and cancelled on close");

    QJsonArray chunks;
    chunks.append(chunk(3, "C"));
    chunks.append(chunk(1, "A"));
    chunks.append(QJsonObject{{"seq", 4}, {"data", "!!not base64!!"}});
    chunks.append(chunk(2, "B"));
    QByteArray fed = started.applyOutputReply(QJsonObject{
        {"state", "running"}, {"chunks", chunks}, {"nextSeq", 4},
        {"truncated", false}, {"more", false}});
    check(fed == "ABC", "chunks are decoded in seq order and bad ones skipped");
    check(started.afterSeq() == 4, "the next read starts after nextSeq");
    check(started.state() == "running" && started.open(), "running is open");

    QJsonArray again;
    again.append(chunk(4, "stale"));
    again.append(chunk(9, "D"));
    fed = started.applyOutputReply(QJsonObject{
        {"state", "running"}, {"chunks", again}, {"nextSeq", 9},
        {"truncated", true}, {"more", true}});
    check(fed.startsWith("\r\n[older sign-in output was trimmed]\r\n") &&
              fed.endsWith("D") && !fed.contains("stale"),
          "a trimmed backlog is marked and already-read chunks are skipped");
    check(started.more(), "more asks for the next page at once");

    fed = started.applyOutputReply(QJsonObject{
        {"state", "succeeded"}, {"chunks", QJsonArray{}}, {"nextSeq", 9},
        {"message", ""}});
    check(fed.isEmpty() && started.state() == "succeeded" && !started.open() &&
              !started.needsCancelOnClose(),
          "an ended state stops polling and needs no cancel");

    RunnerLoginSession attached("fmnext-n1", "claude-code");
    check(attached.applyStartReply(
              409, QJsonObject{{"ok", false},
                               {"error", "login_active"},
                               {"id", kOpenId},
                               {"session",
                                QJsonObject{{"state", "running"}}}}) ==
              RunnerLoginSession::Start::Attached,
          "409 login_active re-attaches");
    check(attached.id() == kOpenId && attached.state() == "running" &&
              attached.open(),
          "the re-attached session is the open one");
    attached.applyCancelReply(QJsonObject{{"ok", true},
                                          {"state", "cancelled"},
                                          {"message", "Cancelled by jett."}});
    check(attached.state() == "cancelled" && !attached.needsCancelOnClose() &&
              attached.message() == "Cancelled by jett.",
          "a cancel ends the session");

    RunnerLoginSession stale("fmnext-n1", "codex");
    stale.attach(kOpenId);
    stale.applyCancelReply(QJsonObject{{"ok", false}});
    check(stale.state() == "cancelled",
          "a cancel with no state in the answer still ends it locally");

    RunnerLoginSession conflict("fmnext-n1", "codex");
    check(conflict.applyStartReply(409, QJsonObject{{"error", "login_active"}}) ==
                  RunnerLoginSession::Start::Failed &&
              !conflict.open(),
          "409 without an id is a failure");
    RunnerLoginSession forbidden("fmnext-n1", "codex");
    check(forbidden.applyStartReply(403, QJsonObject{{"error", "forbidden"}}) ==
                  RunnerLoginSession::Start::Failed &&
              forbidden.error().contains("administrator"),
          "403 explains who may sign a runner in");
    RunnerLoginSession inactive("fmnext-n1", "codex");
    inactive.applyStartReply(409, QJsonObject{{"error", "runner_inactive"},
                                              {"message", "That runner is revoked."}});
    check(inactive.error() == "That runner is revoked.",
          "the server's message is shown");
    check(runnerLoginStateOpen("pending") && runnerLoginStateOpen("running") &&
              !runnerLoginStateOpen("expired") &&
              !runnerLoginStateOpen("failed"),
          "only pending and running are open");
}

static void testTable()
{
    QTableWidget table;
    setUpRunnersTable(&table);
    QList<QPair<QString, QString>> clicks;
    const QList<RunnerInfo> runners = parseRunners(runnersPayload());
    fillRunnersTable(&table, runners, 1000000,
                     [&clicks](const QString &runner, const QString &provider) {
                         clicks.append({runner, provider});
                     });
    check(table.rowCount() == 3, "one row per runner");
    check(table.item(0, kRunnerNameColumn)->text() == "fmnext-n1",
          "fmnext-n1 is listed");
    check(table.item(0, kRunnerClaudeColumn)->text() == "Needs sign-in" &&
              table.item(0, kRunnerCodexColumn)->text() == "Not installed",
          "provider states are shown per row");
    check(table.item(0, kRunnerLastSeenColumn)->text() == "30 s ago",
          "last seen is relative");
    QWidget *n1Actions = table.cellWidget(0, kRunnerActionsColumn);
    auto *signIn = n1Actions ? n1Actions->findChild<QPushButton *>(
                                   runnerSignInButtonName("claude-code"))
                             : nullptr;
    check(signIn && signIn->text() == "Sign in Claude",
          "a runner needing sign-in offers Sign in Claude");
    check(n1Actions && !n1Actions->findChild<QPushButton *>(
                           runnerSignInButtonName("codex")),
          "no sign-in for a CLI that is not installed");
    if (signIn)
        signIn->click();
    check(clicks.size() == 1 && clicks.first().first == "fmnext-n1" &&
              clicks.first().second == "claude-code",
          "the button opens the sign-in for that runner and provider");

    QWidget *r1Actions = table.cellWidget(1, kRunnerActionsColumn);
    auto *resign = r1Actions ? r1Actions->findChild<QPushButton *>(
                                   runnerSignInButtonName("claude-code"))
                             : nullptr;
    auto *reopen = r1Actions ? r1Actions->findChild<QPushButton *>(
                                   runnerSignInButtonName("codex"))
                             : nullptr;
    check(resign && resign->text() == "Re-sign in Claude",
          "a signed-in CLI can be signed in again");
    check(reopen && reopen->text() == "Open Codex sign-in" &&
              table.item(1, kRunnerCodexColumn)->text().contains("sign-in open"),
          "an open sign-in is offered for re-attaching");
    QWidget *oldActions = table.cellWidget(2, kRunnerActionsColumn);
    check(oldActions && oldActions->findChildren<QPushButton *>().isEmpty(),
          "a revoked runner offers no sign-in");
}

static const QString kAccountA = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
static const QString kAccountB = QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");

static QJsonObject accountsPayload()
{
    return QJsonDocument::fromJson(QStringLiteral(R"json({
      "ok": true,
      "runners": [
        {"name": "fmnext-run-1", "scope": "pool", "status": "active",
         "lastSeenAt": 990000,
         "availability": {
           "claude-code": {"binaryFound": true, "loginState": "available",
             "source": "device login", "accounts": [
               {"id": "%1", "label": "one@example.com", "plan": "max",
                "active": true, "usage": {"fetchedAt": 990000, "error": "",
                  "windows": [
                    {"id": "five_hour", "label": "5-hour session",
                     "usedPercent": 42.4, "resetsAt": 4600000},
                    {"id": "seven_day", "label": "Weekly", "usedPercent": 130}
                  ]}},
               {"id": "%2", "label": "", "email": "two@example.com",
                "active": false, "usage": {"error": "token_expired",
                  "windows": []}},
               {"id": "bad", "label": "dropped"}
             ]},
           "codex": {"binaryFound": true, "loginState": "missing",
                     "accounts": []}
         },
         "logins": [],
         "accountRequests": [
           {"id": "%3", "provider": "claude-code", "action": "activate",
            "account": "%2", "state": "pending"},
           {"id": "%4", "provider": "claude-code", "action": "remove",
            "account": "%1", "state": "failed", "message": "busy"}
         ]}
      ]})json")
                                       .arg(kAccountA, kAccountB, kOpenId, kNewId)
                                       .toUtf8())
        .object();
}

static void testAccounts()
{
    const QList<RunnerInfo> runners = parseRunners(accountsPayload());
    check(runners.size() == 1, "the accounts runner is parsed");
    if (runners.isEmpty())
        return;
    const RunnerInfo &runner = runners.first();
    const RunnerProviderState claude = runner.provider("claude-code");
    check(claude.accounts.size() == 2, "accounts with invalid ids are dropped");
    const RunnerAccountInfo *active = claude.activeAccount();
    check(active && active->id == kAccountA && active->plan == "max",
          "the active account is found");
    check(claude.accounts.value(1).displayName() == "two@example.com",
          "an account without a label is named by its email");
    check(active && active->usage.size() == 2 &&
              active->usage.at(1).usedPercent == 100.0,
          "usage windows are parsed and bounded");
    check(active && runnerAccountUsageText(*active, 1000000) ==
                        QString::fromUtf8("5-hour session 42% (resets in 1 h 0 min) "
                                          "\xC2\xB7 Weekly 100%"),
          "usage text shows percent and reset time");
    check(runnerAccountUsageText(claude.accounts.value(1), 1000000) ==
              "usage refreshes after its next run",
          "an expired token explains why usage is stale");
    check(runner.accountRequestPending("claude-code", kAccountB) &&
              !runner.accountRequestPending("claude-code", kAccountA),
          "pending requests are tracked per account");
    check(runnerAccountsUrl(QUrl("https://app.forkmesh.com/x?y=1"), "FMNEXT-RUN-1")
                  .toString() == "https://app.forkmesh.com/api/runners/fmnext-run-1/accounts",
          "the accounts URL is built from the base");
    const QJsonObject body =
        QJsonDocument::fromJson(runnerAccountRequestBody("codex", kAccountA, "remove"))
            .object();
    check(body.value("provider").toString() == "codex" &&
              body.value("account").toString() == kAccountA &&
              body.value("action").toString() == "remove",
          "the account request body names provider, account and action");

    QTableWidget table;
    setUpRunnersTable(&table);
    QList<QStringList> actions;
    fillRunnersTable(
        &table, runners, 1000000, [](const QString &, const QString &) {},
        [&actions](const QString &name, const QString &provider,
                   const RunnerAccountInfo &account, const QString &action) {
            actions.append({name, provider, account.id, action});
        });
    const QString cell = table.item(0, kRunnerClaudeColumn)->text();
    check(cell.contains("one@example.com (1 of 2)") &&
              cell.contains("5-hour session 42%"),
          "the provider cell shows the active account and its usage");
    const QString tip = table.item(0, kRunnerClaudeColumn)->toolTip();
    check(tip.contains("two@example.com") && tip.contains("change pending") &&
              tip.contains("Last change failed: busy"),
          "the tooltip lists every account and request state");
    QWidget *cellActions = table.cellWidget(0, kRunnerActionsColumn);
    auto *add = cellActions ? cellActions->findChild<QPushButton *>(
                                  runnerSignInButtonName("claude-code"))
                            : nullptr;
    check(add && add->text() == "Add Claude account",
          "a provider with accounts offers to add another");
    auto *menuButton = cellActions ? cellActions->findChild<QPushButton *>(
                                         runnerAccountsButtonName("claude-code"))
                                   : nullptr;
    check(menuButton && menuButton->menu(), "the accounts menu is offered");
    check(cellActions && !cellActions->findChild<QPushButton *>(
                             runnerAccountsButtonName("codex")),
          "no accounts menu for a provider without accounts");
    if (!menuButton || !menuButton->menu())
        return;
    QMenu *menu = menuButton->menu();
    auto *useActive = menu->findChild<QAction *>("runnerAccountUse-" + kAccountA);
    auto *usePending = menu->findChild<QAction *>("runnerAccountUse-" + kAccountB);
    auto *removeA = menu->findChild<QAction *>("runnerAccountRemove-" + kAccountA);
    check(useActive && !useActive->isEnabled() && usePending &&
              !usePending->isEnabled(),
          "the active account and a pending switch cannot be chosen again");
    if (removeA) {
        check(removeA->isEnabled(), "an account can be removed");
        removeA->trigger();
    }
    check(actions.size() == 1 &&
              actions.first() == QStringList({"fmnext-run-1", "claude-code",
                                              kAccountA, "remove"}),
          "a menu action reaches the handler with runner, provider and account");
}

static void testAccountsMenu()
{
    const QList<RunnerInfo> runners = parseRunners(accountsPayload());
    QMenu menu;
    QList<QStringList> actions;
    QStringList signIns;
    const auto signIn = [&signIns](const QString &name,
                                   const QString &provider) {
        signIns.append(name + "/" + provider);
    };
    const auto accountAction = [&actions](const QString &name,
                                          const QString &provider,
                                          const RunnerAccountInfo &account,
                                          const QString &action) {
        actions.append({name, provider, account.id, action});
    };
    fillRunnerAccountsMenu(&menu, runners, "claude-code", 1000000, "none",
                           signIn, accountAction);
    auto *heading =
        menu.findChild<QAction *>("runnerAccountsHeading-fmnext-run-1");
    check(heading && !heading->isEnabled(), "each runner gets a heading");
    auto *active =
        menu.findChild<QAction *>("runnerAccount-fmnext-run-1-" + kAccountA);
    auto *pending =
        menu.findChild<QAction *>("runnerAccount-fmnext-run-1-" + kAccountB);
    check(active && active->isCheckable() && active->isChecked() &&
              active->text().contains("one@example.com") &&
              active->text().contains("max"),
          "the active runner account is checked and shows its plan");
    check(pending && !pending->isChecked() && !pending->isEnabled() &&
              pending->text().contains("switching"),
          "an account with a pending switch cannot be chosen again");
    auto *usage =
        menu.findChild<QAction *>("runnerAccountUsage-fmnext-run-1-" + kAccountA);
    check(usage && !usage->isEnabled() &&
              usage->text().contains("5-hour session 42%"),
          "each account shows its usage under it");
    if (active)
        active->trigger();
    check(actions.isEmpty(), "choosing the active account sends nothing");
    auto *add = menu.findChild<QAction *>("runnerAccountAdd-fmnext-run-1");
    check(add && add->isEnabled() && add->text().startsWith("Add Claude account"),
          "a runner with accounts offers to add another");
    if (add)
        add->trigger();
    check(signIns == QStringList({"fmnext-run-1/claude-code"}),
          "adding an account starts the runner sign-in");
    check(!menu.findChild<QAction *>("runnerAccountsEmpty"),
          "no empty line when a runner is listed");

    QJsonObject payload = accountsPayload();
    QJsonArray list = payload.value("runners").toArray();
    QJsonObject runner = list.at(0).toObject();
    QJsonArray requests;
    runner.insert("accountRequests", requests);
    list.replace(0, runner);
    payload.insert("runners", list);
    fillRunnerAccountsMenu(&menu, parseRunners(payload), "claude-code", 1000000,
                           "none", signIn, accountAction);
    auto *other =
        menu.findChild<QAction *>("runnerAccount-fmnext-run-1-" + kAccountB);
    check(other && other->isEnabled(), "another account can be chosen");
    if (other)
        other->trigger();
    check(actions.size() == 1 &&
              actions.first() == QStringList({"fmnext-run-1", "claude-code",
                                              kAccountB, "activate"}),
          "choosing another account asks the runner to switch to it");
    check(menu.findChildren<QAction *>("runnerAccountsHeading-fmnext-run-1")
                  .size() == 1,
          "refilling replaces the earlier entries");

    fillRunnerAccountsMenu(&menu, runners, "codex", 1000000, "none", signIn,
                           accountAction);
    auto *signInCodex = menu.findChild<QAction *>("runnerAccountAdd-fmnext-run-1");
    check(signInCodex && signInCodex->text().startsWith("Sign in Codex"),
          "a runner with no accounts offers its first sign-in");

    fillRunnerAccountsMenu(&menu, {}, "codex", 1000000, "No runners", signIn,
                           accountAction);
    auto *empty = menu.findChild<QAction *>("runnerAccountsEmpty");
    check(empty && empty->text() == "No runners" && !empty->isEnabled() &&
              menu.actions().size() == 1,
          "no runners shows the caller's empty line");
}

static void testTerminalRemoteMode()
{
    UrlCatcher catcher;
    QDesktopServices::setUrlHandler("https", &catcher, "open");
    TerminalWidget terminal;
    terminal.resize(640, 320);
    terminal.setRemoteInput(true);
    check(terminal.remoteInput(), "remote input is on");
    QSignalSpy typed(&terminal, &TerminalWidget::inputTyped);
    QSignalSpy url(&terminal, &TerminalWidget::signInUrlDetected);
    QKeyEvent key(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, "a");
    QApplication::sendEvent(&terminal, &key);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, "\r");
    QApplication::sendEvent(&terminal, &enter);
    check(typed.size() == 2 && typed.at(0).at(0).toByteArray() == "a" &&
              typed.at(1).at(0).toByteArray() == "\r",
          "keys become inputTyped instead of pty writes");
    terminal.feed("Open https://claude.ai/oauth/authorize?code=true&"
                  "code_challenge=abc&state=s");
    check(url.isEmpty(), "a URL still arriving is not opened");
    terminal.feed(" \r\nPaste code here: ");
    check(url.size() == 1 && catcher.opened.size() == 1 &&
              catcher.opened.first().host() == "claude.ai",
          "fed output opens the authorize URL once it is complete");

    TerminalWidget codex;
    codex.setRemoteInput(true);
    QSignalSpy codexUrl(&codex, &TerminalWidget::signInUrlDetected);
    codex.feed("1. Open this link\r\n   https://auth.openai.com/codex/device\r\n"
               "2. Enter this one-time code ABCD-EFGH\r\n");
    check(codexUrl.size() == 1 &&
              codexUrl.at(0).at(0).toUrl().toString() ==
                  "https://auth.openai.com/codex/device",
          "the Codex device page is opened too");
    QDesktopServices::unsetUrlHandler("https");
}

static void testNodeDirectoryRunners()
{
    namespace nodedir = forkmesh::nodedir;
    check(nodedir::runnerKey("r1") == "fmnext-r1" &&
              nodedir::runnerKey("fmnext-run-1") == "fmnext-r1" &&
              nodedir::runnerKey("FMNEXT-R1") == "fmnext-r1" &&
              nodedir::runnerKey("fmnext-burst-3") == "fmnext-b3" &&
              nodedir::runnerKey("b3") == "fmnext-b3" &&
              nodedir::runnerKey("alice-box") == "alice-box" &&
              nodedir::runnerKey("fmnext-n1") == "fmnext-n1",
          "fleet labels, short names and registry names give one runner key");
    check(nodedir::runnerFeedName("fmnext-run-1") == "r1" &&
              nodedir::runnerFeedName("fmnext-b3") == "b3" &&
              nodedir::runnerFeedName("alice-box").isEmpty(),
          "a fleet runner maps to its log feed name");

    QJsonObject payload = runnersPayload();
    QJsonArray rows = payload.value("runners").toArray();
    QJsonObject r1 = rows.at(1).toObject();
    r1.insert("activeLeases", 2);
    r1.insert("lastSeenAt", 990000);
    rows.replace(1, r1);
    payload.insert("runners", rows);
    const QList<RunnerInfo> registered = parseRunners(payload);
    check(registered.size() == 3 && registered.at(1).activeLeases == 2 &&
              registered.at(0).activeLeases == -1,
          "activeLeases is parsed and absent means unknown");

    forkmesh::corelogs::CoreNode run1;
    run1.name = "r1";
    run1.label = "fmnext-run-1";
    run1.role = "runner";
    run1.address = "192.0.2.9";
    forkmesh::corelogs::CoreNode burst;
    burst.name = "b2";
    burst.label = "fmnext-burst-2";
    burst.role = "runner";
    burst.address = "192.0.2.10";
    forkmesh::corelogs::CoreNode core;
    core.name = "n1";
    core.label = "fmnext-core-1";
    core.address = "192.0.2.1";
    const QList<nodedir::RunnerRow> merged =
        nodedir::mergeRunnerRows(registered, {run1, core, burst, run1});
    QStringList names;
    for (const nodedir::RunnerRow &row : merged)
        names.append(row.name);
    check(names.count("fmnext-r1") == 1 && names.contains("fmnext-b2") &&
              !names.contains("n1"),
          "the fleet and the registry merge into one row per runner, cores "
          "left out");
    const nodedir::RunnerRow *mergedR1 = nullptr;
    const nodedir::RunnerRow *mergedB2 = nullptr;
    for (const nodedir::RunnerRow &row : merged) {
        if (row.name == "fmnext-r1")
            mergedR1 = &row;
        if (row.name == "fmnext-b2")
            mergedB2 = &row;
    }
    check(mergedR1 && mergedR1->registered && mergedR1->inFleet &&
              mergedR1->feedName == "r1" &&
              mergedR1->address == "192.0.2.9" &&
              mergedR1->scope == "pool" && mergedR1->activeLeases == 2,
          "a registered fleet runner keeps its address, feed and leases");
    check(mergedB2 && !mergedB2->registered && mergedB2->scope == "pool" &&
              nodedir::runnerStatusText(*mergedB2, 1000000) == "Not registered",
          "a fleet runner missing from the registry says so");
    check(mergedR1 && nodedir::runnerOnline(*mergedR1, 1000000) &&
              nodedir::runnerStatusText(*mergedR1, 1000000) == "Online" &&
              nodedir::runnerStatusText(*mergedR1, 990000 +
                                                       nodedir::kRunnerFreshMs + 1) ==
                  "Offline" &&
              nodedir::runnerActivityText(*mergedR1) == "2 job leases",
          "a runner is online while its check-ins are fresh");
    nodedir::RunnerRow revoked;
    revoked.registered = true;
    revoked.status = "revoked";
    revoked.lastSeenAtMs = 1000000;
    check(nodedir::runnerStatusText(revoked, 1000000) == "Revoked" &&
              !nodedir::runnerOnline(revoked, 1000000),
          "a revoked runner is never online");
    nodedir::RelayProbe checking;
    checking.state = "Checking";
    nodedir::RelayProbe online;
    online.state = "Online";
    nodedir::RelayProbe offline;
    offline.state = "Offline";
    check(nodedir::relaySummaryText({online, offline}, "12:00:00") ==
                  QString::fromUtf8("1 of 2 relay(s) online \xC2\xB7 updated 12:00:00") &&
              nodedir::relaySummaryText({online, checking}, "x")
                  .startsWith("probing 2 relay(s)") &&
              nodedir::relayVersionText("1.2.5", "abcdef0123456789") ==
                  QString::fromUtf8("1.2.5 \xC2\xB7 abcdef012345") &&
              nodedir::relayVersionText("", "") == "dev",
          "relay summary and version text match the former Relays tab");
}

static QJsonObject attentionRunnersPayload()
{
    return QJsonDocument::fromJson(QStringLiteral(R"json({
      "ok": true,
      "runners": [
        {"name": "fmnext-r2", "scope": "pool", "status": "active",
         "availability": {
           "claude-code": {"binaryFound": true, "loginState": "available",
                           "auth": {"state": "expiring",
                                    "expiresAt": 1790000000000}},
           "codex": {"binaryFound": true, "loginState": "available",
                     "auth": {"state": "expired"}}
         },
         "logins": []},
        {"name": "fmnext-r1", "scope": "pool", "status": "active",
         "availability": {
           "claude-code": {"binaryFound": true, "loginState": "expired",
                           "auth": {"state": "expired",
                                    "reason": "job_failed"}},
           "codex": {"binaryFound": true, "loginState": "missing"}
         },
         "logins": [
           {"id": "%1", "provider": "claude-code", "state": "running"}
         ]},
        {"name": "fmnext-r3", "scope": "pool", "status": "active",
         "availability": {
           "claude-code": {"binaryFound": false, "loginState": "expired"},
           "codex": {"binaryFound": true, "loginState": "available",
                     "auth": {"state": "ok"}}
         }},
        {"name": "old-runner", "scope": "pool", "status": "revoked",
         "availability": {
           "claude-code": {"binaryFound": true, "loginState": "expired"}
         }}
      ]})json")
                                       .arg(kOpenId)
                                       .toUtf8())
        .object();
}

static void testAttention()
{
    const QList<RunnerInfo> runners = parseRunners(attentionRunnersPayload());
    check(runners.size() == 4, "attention runners are parsed");
    if (runners.size() != 4)
        return;
    check(runners.at(0).provider("claude-code").authState == "expiring" &&
              runners.at(0).provider("claude-code").authExpiresAtMs ==
                  1790000000000LL,
          "the provider auth state and expiry are parsed");
    check(runnerProviderStateText(runners.at(1).provider("claude-code"))
              .startsWith("Login expired"),
          "an expired login reads as expired in the Runners table");

    const QList<RunnerAttentionItem> derived =
        runnerAttentionFromRunners(runners);
    check(derived.size() == 3,
          "expired and expiring logins of active runners with the CLI");
    if (derived.size() == 3) {
        check(derived.at(0).runner == "fmnext-r1" &&
                  derived.at(0).provider == "claude-code" &&
                  derived.at(0).expired() &&
                  derived.at(0).openLoginId == kOpenId,
              "loginState expired, with its open sign-in, sorts first");
        check(derived.at(1).runner == "fmnext-r2" &&
                  derived.at(1).provider == "codex" && derived.at(1).expired(),
              "auth.state expired needs sign-in too");
        check(derived.at(2).runner == "fmnext-r2" &&
                  derived.at(2).provider == "claude-code" &&
                  derived.at(2).state == "expiring" &&
                  derived.at(2).expiresAtMs == 1790000000000LL,
              "a signed-in login that expires soon comes last");
    }

    check(parseRunnerAttention(attentionRunnersPayload()).size() == 3,
          "an attention answer made of runner rows is derived the same way");

    const QJsonObject flat = QJsonDocument::fromJson(QStringLiteral(R"json({
      "ok": true,
      "attention": [
        {"runner": "fmnext-r1", "provider": "claude-code",
         "state": "needs_login", "message": "OAuth session expired",
         "loginId": "%1"},
        {"runner": "fmnext-r1", "provider": "claude-code", "state": "expiring"},
        {"name": "fmnext-r4", "provider": "codex",
         "auth": {"state": "expiring", "expiresAt": 5}},
        {"runner": "fmnext-r5", "provider": "codex", "state": "ok"},
        {"runner": "Bad Name", "provider": "codex", "state": "expired"},
        {"runner": "fmnext-r6", "provider": "gemini", "state": "expired"},
        {"runner": "fmnext-r7", "providers": [
          {"provider": "codex", "loginState": "missing"},
          {"provider": "claude-code", "state": "available"}],
         "logins": [{"id": "%2", "provider": "codex", "state": "pending"}]}
      ]})json")
                                 .arg(kOpenId, kNewId)
                                 .toUtf8())
                                 .object();
    const QList<RunnerAttentionItem> items = parseRunnerAttention(flat);
    check(items.size() == 3, "flat and grouped attention items are parsed");
    if (items.size() == 3) {
        check(items.at(0).runner == "fmnext-r1" && items.at(0).expired() &&
                  items.at(0).message == "OAuth session expired" &&
                  items.at(0).openLoginId == kOpenId,
              "a duplicate keeps the expired state and its open sign-in");
        check(items.at(1).runner == "fmnext-r7" &&
                  items.at(1).provider == "codex" && items.at(1).expired() &&
                  items.at(1).openLoginId == kNewId,
              "a grouped item takes its open sign-in from logins[]");
        check(items.at(2).runner == "fmnext-r4" &&
                  items.at(2).state == "expiring" &&
                  items.at(2).expiresAtMs == 5,
              "state falls back to auth.state");
    }
    check(parseRunnerAttention(QJsonObject()).isEmpty(),
          "an empty answer needs nothing");
    QJsonObject items2;
    items2.insert(QStringLiteral("items"),
                  QJsonArray{QJsonObject{{"runner", "fmnext-r1"},
                                         {"provider", "codex"},
                                         {"state", "expired"}}});
    check(parseRunnerAttention(items2).size() == 1,
          "the items key is read too");

    RunnerAttentionItem expired;
    expired.runner = QStringLiteral("fmnext-r1");
    expired.provider = QStringLiteral("claude-code");
    expired.state = QStringLiteral("expired");
    check(runnerAttentionTitle(expired) == "fmnext-r1 needs Claude sign-in",
          "the badge title names the runner and provider");
    check(runnerAttentionDetail(expired) ==
              QString::fromUtf8("fmnext-r1: Claude login expired \xE2\x80\x94 "
                                "reconnect. Jobs for Claude Code are not "
                                "running on this runner."),
          "the detail matches the web banner");
    check(runnerAttentionActionText(expired) == "Connect Claude",
          "the action is Connect Claude");
    RunnerAttentionItem expiring = expired;
    expiring.provider = QStringLiteral("codex");
    expiring.state = QStringLiteral("expiring");
    expiring.openLoginId = kOpenId;
    check(runnerAttentionTitle(expiring) ==
              "fmnext-r1: Codex sign-in expires soon",
          "an expiring login is worded as a warning");
    check(runnerAttentionActionText(expiring) == "Open sign-in",
          "an open sign-in is reopened, not restarted");

    check(!runnerAttentionBadge({}).visible, "no items, no badge");
    const RunnerAttentionBadge one = runnerAttentionBadge({expired});
    check(one.visible && one.expired &&
              one.text == "fmnext-r1 needs Claude sign-in" &&
              one.toolTip.contains("Connect Claude on fmnext-r1"),
          "one expired login shows a red badge naming it");
    const RunnerAttentionBadge warn = runnerAttentionBadge({expiring});
    check(warn.visible && !warn.expired, "an expiring login alone is amber");
    const RunnerAttentionBadge both = runnerAttentionBadge({expiring, expired});
    check(both.visible && both.expired &&
              both.text == "fmnext-r1 needs Claude sign-in (+1 more)" &&
              both.toolTip.contains("pick the runner"),
          "several items show the most urgent one and a count");
    check(runnerAttentionBadge(runnerAttentionFromRunners({})).visible == false,
          "signed-in runners clear the badge");

    QString runner;
    QString provider;
    check(runnerSignInLink(
              QUrl("https://app.forkmesh.com/dashboard/runners?runner=fmnext-r1"
                   "&provider=claude-code"),
              &runner, &provider) &&
              runner == "fmnext-r1" && provider == "claude-code",
          "a runner ping link names the runner and provider");
    check(runnerSignInLink(QUrl("/dashboard/runners?runner=Bad!&provider=codex"),
                           &runner, &provider) &&
              runner.isEmpty() && provider == "codex",
          "an invalid runner name is dropped from the link");
    check(!runnerSignInLink(QUrl("/dashboard/runners?provider=gemini"), &runner,
                            &provider) &&
              !runnerSignInLink(QUrl("/dashboard/agents?provider=codex"),
                                &runner, &provider),
          "other links are not runner sign-ins");
    const QJsonObject served = QJsonDocument::fromJson(QStringLiteral(R"json({
      "ok": true,
      "items": [
        {"runner": "fmnext-r1", "provider": "claude-code",
         "providerLabel": "Claude", "state": "expired", "scope": "pool",
         "canConnect": true, "online": true, "lastSeenAt": 1790735909692,
         "expiresAt": 0, "title": "fmnext-r1 needs Claude sign-in",
         "connectUrl": "/dashboard/runners?runner=fmnext-r1&provider=claude-code&connect=1",
         "loginId": ""},
        {"runner": "fmnext-p1", "provider": "codex", "state": "expiring",
         "canConnect": false, "expiresAt": 1790800000000,
         "title": "fmnext-p1: Codex sign-in expires soon", "loginId": "%1"}
      ],
      "count": 2, "expired": 1, "runners": 2,
      "checkedAt": 1790736000000, "pollMs": 300000
    })json").arg(kOpenId).toUtf8()).object();
    const QList<RunnerAttentionItem> servedItems = parseRunnerAttention(served);
    check(servedItems.size() == 2 && servedItems.at(0).runner == "fmnext-r1" &&
              servedItems.at(0).expired() && servedItems.at(0).canConnect &&
              servedItems.at(0).openLoginId.isEmpty() &&
              runnerAttentionTitle(servedItems.at(0)) ==
                  "fmnext-r1 needs Claude sign-in",
          "the served attention answer is read, with the server's wording");
    check(servedItems.size() == 2 && !servedItems.at(1).canConnect &&
              servedItems.at(1).openLoginId == kOpenId &&
              servedItems.at(1).expiresAtMs == 1790800000000LL &&
              runnerAttentionTitle(servedItems.at(1)) ==
                  "fmnext-p1: Codex sign-in expires soon",
          "canConnect, loginId and expiresAt are kept");
    check(runnerAttentionPollMs(served) == 300000 &&
              runnerAttentionPollMs(QJsonObject()) == 300000 &&
              runnerAttentionPollMs(QJsonObject{{"pollMs", 5}}) == 60000,
          "the suggested poll interval is bounded");
    check(runnerConnectPageUrl(QUrl("https://app.forkmesh.com/"),
                               servedItems.value(0)) ==
              QUrl("https://app.forkmesh.com/dashboard/runners?runner=fmnext-r1"
                   "&provider=claude-code&connect=1"),
          "the dashboard Connect page for a runner");
    check(runnerAttentionUrl(QUrl("https://app.forkmesh.com/x?y=1")) ==
              QUrl("https://app.forkmesh.com/api/runners/attention"),
          "the attention endpoint URL");
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    testParseRunners();
    testRequests();
    testInputValidation();
    testSessionState();
    testTable();
    testAccounts();
    testAccountsMenu();
    testTerminalRemoteMode();
    testNodeDirectoryRunners();
    testAttention();
    if (failures == 0)
        std::cout << "runner login: all checks passed\n";
    return failures == 0 ? 0 : 1;
}

#include "test_runner_login.moc"
