#include "ClaudeAccountRouting.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <iostream>

using namespace forkmesh::agentaccounts;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static void writeJson(const QString &path, const QJsonObject &object)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(QJsonDocument(object).toJson());
}

static QJsonObject readJson(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

static QJsonObject account(const QString &uuid, const QString &org,
                           const QString &email)
{
    return {{QStringLiteral("accountUuid"), uuid},
            {QStringLiteral("organizationUuid"), org},
            {QStringLiteral("emailAddress"), email},
            {QStringLiteral("displayName"), QStringLiteral("Someone")}};
}

static QJsonObject credentials(const QString &token)
{
    return {{QStringLiteral("claudeAiOauth"),
             QJsonObject{{QStringLiteral("accessToken"), token},
                         {QStringLiteral("refreshToken"), token + "-r"}}}};
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    check(claudeStateFile(QStringLiteral("/x/acct"), false) ==
              QStringLiteral("/x/acct/.claude.json"),
          "a profile folder keeps its state file inside it");
    check(claudeStateFile(QStringLiteral("/x/acct"), true) ==
              QDir::homePath() + QStringLiteral("/.claude.json"),
          "the plain ~/.claude login keeps its state in ~/.claude.json");

    QTemporaryDir root;
    check(root.isValid(), "temporary folder");
    const QString jettDir = root.filePath(QStringLiteral("jett"));
    const QString jettState = claudeStateFile(jettDir, false);
    writeJson(jettState,
              {{QStringLiteral("numStartups"), 7},
               {QStringLiteral("oauthAccount"),
                account(QStringLiteral("u-jett"), QStringLiteral("o-1"),
                        QStringLiteral("jett@example.com"))}});

    const ClaudeLoginIdentity jett = readClaudeLoginIdentity(jettState);
    check(jett.isValid() && jett.key() == QStringLiteral("u-jett/o-1") &&
              jett.label() == QStringLiteral("Someone"),
          "identity is read from oauthAccount and labelled by name, never email");
    check(ClaudeLoginIdentity{QStringLiteral("u"), QStringLiteral("o"),
                              QStringLiteral("x@example.com"), QString()}
                  .label() == QStringLiteral("Claude account"),
          "an identity without a name is not labelled by its email");
    check(!readClaudeLoginIdentity(root.filePath(QStringLiteral("none.json")))
               .isValid(),
          "a missing state file has no identity");
    const ClaudeLoginIdentity otherOrg = [&] {
        ClaudeLoginIdentity copy = jett;
        copy.organizationUuid = QStringLiteral("o-2");
        return copy;
    }();
    check(otherOrg.key() != jett.key(),
          "the same person in another organization is another account");

    const QList<AccountBinding> accounts{
        {QStringLiteral("default"), QStringLiteral("u-jett/o-1")},
        {QStringLiteral("don"), QStringLiteral("u-don/o-9")},
        {QStringLiteral("empty"), QString()},
        {QStringLiteral("jett2"), QStringLiteral("u-jett/o-1")}};

    LoginRoute route = routeClaudeLogin(QStringLiteral("u-don/o-9"),
                                        QStringLiteral("default"), accounts);
    check(route.kind == LoginRouteKind::Adopt &&
              route.accountId == QStringLiteral("default"),
          "logging in on an account keeps the login on that account");
    route = routeClaudeLogin(QStringLiteral("u-jett/o-1"),
                             QStringLiteral("jett2"), accounts);
    check(route.kind == LoginRouteKind::Existing &&
              route.accountId == QStringLiteral("jett2"),
          "logging in again as the same identity reuses the account");
    route = routeClaudeLogin(QStringLiteral("u-jett/o-1"), QString(), accounts);
    check(route.kind == LoginRouteKind::Existing &&
              route.accountId == QStringLiteral("default"),
          "with no request the first owner is used");
    route = routeClaudeLogin(QStringLiteral("u-new/o-3"),
                             QStringLiteral("empty"), accounts);
    check(route.kind == LoginRouteKind::Adopt &&
              route.accountId == QStringLiteral("empty"),
          "an unused profile adopts a new identity");
    route = routeClaudeLogin(QStringLiteral("u-new/o-3"),
                             QStringLiteral("default"), accounts);
    check(route.kind == LoginRouteKind::Adopt &&
              route.accountId == QStringLiteral("default"),
          "a new identity logged in on an account replaces that account's "
          "login instead of adding an account");
    route = routeClaudeLogin(QStringLiteral("u-don/o-9"),
                             QStringLiteral("gone"), accounts);
    check(route.kind == LoginRouteKind::Existing &&
              route.accountId == QStringLiteral("don"),
          "a request for a removed account falls back to the owner");
    route = routeClaudeLogin(QStringLiteral("u-new/o-3"), QString(), accounts);
    check(route.kind == LoginRouteKind::New, "no request means a new profile");

    const QString stagingDir = root.filePath(QStringLiteral("staging"));
    const QString stagingState = claudeStateFile(stagingDir, false);
    writeJson(stagingState,
              {{QStringLiteral("hasCompletedOnboarding"), true},
               {QStringLiteral("oauthAccount"),
                account(QStringLiteral("u-jett"), QStringLiteral("o-1"),
                        QStringLiteral("jett@example.com"))}});
    writeJson(claudeCredentialFile(stagingDir),
              credentials(QStringLiteral("fresh")));
    writeJson(claudeCredentialFile(jettDir), credentials(QStringLiteral("old")));

    QString error;
    check(moveClaudeLogin(stagingDir, stagingState, jettDir, jettState, &error),
          "a login moves into its profile");
    check(readJson(claudeCredentialFile(jettDir))
                  .value(QStringLiteral("claudeAiOauth"))
                  .toObject()
                  .value(QStringLiteral("accessToken"))
                  .toString() == QStringLiteral("fresh"),
          "the moved login replaces the stale token");
    const QJsonObject jettAfter = readJson(jettState);
    check(jettAfter.value(QStringLiteral("numStartups")).toInt() == 7 &&
              jettAfter.value(QStringLiteral("oauthAccount"))
                      .toObject()
                      .value(QStringLiteral("accountUuid"))
                      .toString() == QStringLiteral("u-jett"),
          "the target state keeps its own keys and gains the account");
    check(!QFileInfo::exists(claudeCredentialFile(stagingDir)) &&
              !readJson(stagingState).contains(QStringLiteral("oauthAccount")),
          "the source no longer holds the login");
    check((QFileInfo(claudeCredentialFile(jettDir)).permissions() &
           (QFileDevice::ReadGroup | QFileDevice::ReadOther)) == 0,
          "the moved token is private to the owner");

    check(!moveClaudeLogin(stagingDir, stagingState, jettDir, jettState, &error) &&
              !error.isEmpty(),
          "a folder with no login has nothing to move");
    check(!moveClaudeLogin(jettDir, jettState, jettDir, jettState, &error),
          "a login is never moved onto itself");

    const QString freshDir = root.filePath(QStringLiteral("fresh"));
    check(moveClaudeLogin(jettDir, jettState, freshDir,
                          claudeStateFile(freshDir, false), &error) &&
              readClaudeLoginIdentity(claudeStateFile(freshDir, false)).key() ==
                  QStringLiteral("u-jett/o-1") &&
              QFileInfo::exists(claudeCredentialFile(freshDir)) &&
              readJson(claudeStateFile(freshDir, false))
                  .value(QStringLiteral("hasCompletedOnboarding"))
                  .toBool(),
          "a login moves into a new, empty profile folder without a fresh "
          "onboarding");

    const QString brokenDir = root.filePath(QStringLiteral("broken"));
    QDir().mkpath(brokenDir);
    {
        QFile broken(claudeStateFile(brokenDir, false));
        if (broken.open(QIODevice::WriteOnly))
            broken.write("{not json");
    }
    check(!moveClaudeLogin(freshDir, claudeStateFile(freshDir, false), brokenDir,
                           claudeStateFile(brokenDir, false), &error) &&
              QFileInfo::exists(claudeCredentialFile(freshDir)),
          "an unreadable target state is never overwritten and the source keeps "
          "its login");

    if (failures == 0)
        std::cout << "claude account routing tests passed\n";
    return failures == 0 ? 0 : 1;
}
