#include "RelayRepositoryScope.h"

#include <QCoreApplication>
#include <QJsonDocument>

#include <iostream>

using namespace forkmesh::relayscope;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

const QString kDefault = QStringLiteral("https://app.forkmesh.com");
const QString kOwn = QStringLiteral("https://relay.example.org");

Scope twoRelays()
{
    Scope scope;
    scope.configuredOrigins = {kDefault, kOwn};
    scope.defaultOrigin = kDefault;
    return scope;
}

QJsonArray catalog(const char *json)
{
    return QJsonDocument::fromJson(QByteArray(json)).array();
}

void checkOrigins()
{
    check(normalizedOrigin(QUrl(QStringLiteral(
              "wss://App.ForkMesh.com:443/ws?room=general"))) == kDefault,
          "a websocket URL normalizes to its https origin");
    check(normalizedOrigin(QUrl(QStringLiteral(
              "https://user:pw@relay.example.org/api/repositories"))) == kOwn,
          "user info and path are not part of the origin");
    check(normalizedOrigin(QUrl(QStringLiteral("http://localhost:8787/x"))) ==
              QStringLiteral("http://localhost:8787"),
          "a non-default port stays");
    check(normalizedOrigin(QUrl(QStringLiteral("not a url"))).isEmpty(),
          "a value with no host is no origin");
}

void checkListedKeys()
{
    const QJsonArray rows = catalog(R"([
        {"owner": "Alice", "name": "Tools"},
        {"owner": "bob", "name": "mirrored"},
        {"owner": "carol", "name": "unrelated"},
        {"owner": "", "name": "broken"}
    ])");
    const QSet<QString> keys = listedKeys(
        rows, QStringLiteral("alice"), {QStringLiteral("bob/mirrored")});
    check(keys == QSet<QString>{QStringLiteral("alice/tools"),
                                QStringLiteral("bob/mirrored")},
          "the remembered list is the account's repositories plus the ones "
          "this machine has a record for");
    check(listedKeys(rows, QString(), {}).isEmpty(),
          "an anonymous read with no local records remembers nothing");
}

void checkStampedRecordIsFinal()
{
    Scope scope = twoRelays();
    // The cross-relay publish this guards against: the default relay lists a
    // repository that was added under the user's own relay.
    scope.relayLists.insert(kDefault, {QStringLiteral("alice/tools")});
    const QStringList keys =
        repositoryKeys(QStringLiteral("alice"), QStringLiteral("alice"),
                       QStringLiteral("tools"));
    check(resolveOrigin(scope, kOwn, QString(), keys) == kOwn,
          "a stamped record stays with its relay whatever another relay lists");
    check(!belongsTo(scope, kDefault, kOwn, QString(), keys),
          "a record stamped for another relay is not served to the active one");
    check(belongsTo(scope, QStringLiteral("wss://relay.example.org/ws"), kOwn,
                    QString(), keys),
          "the active relay is compared by origin");
}

void checkLegacyRecordFollowsRelayList()
{
    Scope scope = twoRelays();
    const QStringList keys =
        repositoryKeys(QStringLiteral("alice"), QStringLiteral("alice"),
                       QStringLiteral("tools"));
    check(resolveOrigin(scope, QString(), QString(), keys) == kDefault,
          "with no relay list yet a legacy record keeps the default relay");

    scope.relayLists.insert(kDefault, {});
    scope.relayLists.insert(kOwn, {QStringLiteral("alice/tools")});
    check(resolveOrigin(scope, QString(), QString(), keys) == kOwn,
          "the one relay that lists a legacy record owns it");
    check(!belongsTo(scope, kDefault, QString(), QString(), keys),
          "and the default relay no longer gets it");

    scope.relayLists[kDefault].insert(QStringLiteral("alice/tools"));
    check(resolveOrigin(scope, QString(), QString(), keys) == kDefault,
          "two relays listing it is ambiguous and keeps the default");

    check(resolveOrigin(scope, QString(),
                        QStringLiteral("https://relay.example.org/alice/tools"),
                        keys) == kOwn,
          "a source URL on a configured relay decides first");
    check(resolveOrigin(scope, QString(),
                        QStringLiteral("https://github.com/alice/tools"),
                        keys) == kDefault,
          "a source URL elsewhere decides nothing");
}

void checkEitherOwnerNameMatches()
{
    Scope scope = twoRelays();
    scope.relayLists.insert(kDefault, {});
    scope.relayLists.insert(kOwn, {QStringLiteral("alice/tools")});
    // Signed in as bob on the active relay, the record still names alice.
    const QStringList keys =
        repositoryKeys(QStringLiteral("Alice"), QStringLiteral("bob"),
                       QStringLiteral("Tools"));
    check(keys == QStringList{QStringLiteral("alice/tools"),
                              QStringLiteral("bob/tools")},
          "a record is looked up under its owner and the publishing account");
    check(resolveOrigin(scope, QString(), QString(), keys) == kOwn,
          "either name finds the relay's listing");
    check(repositoryKeys(QStringLiteral("alice"), QStringLiteral("alice"),
                         QStringLiteral("tools"))
                  .size() == 1,
          "the same name twice is one key");
}

void checkStamping()
{
    Scope scope = twoRelays();
    const QStringList keys =
        repositoryKeys(QStringLiteral("alice"), QStringLiteral("alice"),
                       QStringLiteral("tools"));
    scope.relayLists.insert(kDefault, {QStringLiteral("alice/tools")});
    check(!shouldStamp(scope, kDefault, QString(), keys),
          "nothing is stamped until every configured relay has answered");

    scope.relayLists.insert(kOwn, {});
    check(shouldStamp(scope, kDefault, QString(), keys),
          "the only relay listing a legacy record may claim it");
    check(!shouldStamp(scope, kOwn, QString(), keys),
          "a relay that does not list it may not");
    check(!shouldStamp(scope, kDefault, kOwn, keys),
          "a stamped record is never restamped");

    scope.relayLists[kOwn].insert(QStringLiteral("alice/tools"));
    check(!shouldStamp(scope, kDefault, QString(), keys),
          "a record two relays list is left unstamped");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    checkOrigins();
    checkListedKeys();
    checkStampedRecordIsFinal();
    checkLegacyRecordFollowsRelayList();
    checkEitherOwnerNameMatches();
    checkStamping();
    if (failures == 0)
        std::cout << "relay repository scope tests passed\n";
    return failures == 0 ? 0 : 1;
}
