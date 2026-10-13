#include "LocalControlHosts.h"
#include "LocalControlWallet.h"

#include <QJsonArray>
#include <QJsonObject>

#include <iostream>

using namespace forkmesh::localcontrol::wallet;
namespace hostctl = forkmesh::localcontrol::hosts;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static const QString kAddress = QStringLiteral("9xQeWvG816bUx9EPjHmaT23yvVM2ZWbrrpZb9PusVFin");

static void testPrefixAndActions()
{
    check(familyPrefix() == QLatin1String("wallet."), "prefix");
    check(isKnownAction(QStringLiteral("tip")), "tip known");
    check(isKnownAction(QStringLiteral("reward")), "reward known");
    check(!isKnownAction(QStringLiteral("export")), "export unknown");
    check(!isKnownAction(QStringLiteral("sign")), "sign unknown");
}

static void testValidators()
{
    check(validRecipient(QStringLiteral("bob")), "plain recipient");
    check(validRecipient(QStringLiteral("acme-2")), "dashed recipient");
    check(!validRecipient(QStringLiteral("Bob")), "uppercase refused");
    check(!validRecipient(QStringLiteral("bob-")), "trailing dash refused");
    check(!validRecipient(QStringLiteral("../x")), "path refused");
    check(!validRecipient(QString(64, QLatin1Char('a'))), "too long refused");
    check(plausibleAddress(kAddress), "address plausible");
    check(!plausibleAddress(QStringLiteral("0OIl") + kAddress.mid(4)), "non-base58 refused");
    check(!plausibleAddress(QStringLiteral("short")), "short refused");
    check(validRewardId(QStringLiteral("r_1:abc")), "reward id ok");
    check(!validRewardId(QStringLiteral("r 1")), "reward id space refused");
    check(!validRewardId(QString(129, QLatin1Char('a'))), "reward id too long");
}

static void testLamports()
{
    quint64 value = 0;
    check(parseLamports(QJsonValue(50000000.0), &value) && value == 50000000ULL, "integer lamports");
    check(!parseLamports(QJsonValue(0.5), &value), "fraction refused");
    check(!parseLamports(QJsonValue(-5.0), &value), "negative refused");
    check(!parseLamports(QJsonValue(0.0), &value), "zero refused");
    check(!parseLamports(QJsonValue(QStringLiteral("5")), &value), "string refused");
    check(!parseLamports(QJsonValue(1e17), &value), "huge refused");
    check(lamportsInRange(5, 1, 10), "in range");
    check(!lamportsInRange(11, 1, 10), "over max");
    check(!lamportsInRange(0, 0, 10), "zero");
    check(lamportsInRange(11, 1, 0), "no max");
    check(solText(0) == QLatin1String("0 SOL"), "zero sol");
    check(solText(50000000) == QLatin1String("0.05 SOL"), "fraction sol");
    check(solText(1000000001) == QLatin1String("1.000000001 SOL"), "tiny fraction");
    check(solText(2000000000) == QLatin1String("2 SOL"), "whole sol");
}

static void testTargetsAndSummaries()
{
    const QString a = tipTarget(QStringLiteral("bob"), kAddress, 5);
    const QString b = tipTarget(QStringLiteral("bob"), kAddress, 6);
    check(a != b, "amount binds the target");
    check(tipTarget(QStringLiteral("eve"), kAddress, 5) != a, "recipient binds the target");
    check(rewardTarget(QStringLiteral("r1")) != rewardTarget(QStringLiteral("r2")), "reward binds");
    const QString summary = tipSummary(QStringLiteral("bob"), kAddress, 50000000);
    check(summary.contains(QLatin1String("0.05 SOL")), "summary amount");
    check(summary.contains(QLatin1String("@bob")), "summary recipient");
    check(summary.contains(QLatin1String("passphrase")), "summary says the desktop signs");
    check(!summary.contains(kAddress), "summary shortens the address");
    const QString reward = rewardSummary(QStringLiteral("bob"), 5000000, QString(400, QLatin1Char('x')));
    check(reward.size() < 500, "reward summary bounded");
}

static void testRewardRows()
{
    RewardFacts good;
    good.rewardId = QStringLiteral("r1");
    good.recipient = QStringLiteral("bob");
    good.address = kAddress;
    good.lamports = 5000000;
    good.repository = QStringLiteral("acme/app");
    good.kind = QStringLiteral("pull");
    good.number = 7;
    good.title = QStringLiteral("Fix it");
    RewardFacts noAddress = good;
    noAddress.rewardId = QStringLiteral("r2");
    noAddress.address.clear();
    RewardFacts zero = good;
    zero.rewardId = QStringLiteral("r3");
    zero.lamports = 0;
    RewardFacts duplicate = good;
    const QJsonArray rows = rewardRows({good, noAddress, zero, duplicate});
    check(rows.size() == 1, "only payable, deduplicated rewards");
    const QJsonObject row = rows.first().toObject();
    check(row.value(QStringLiteral("kind")).toString() == QLatin1String("pull"), "kind");
    check(row.value(QStringLiteral("lamports")).toDouble() == 5000000.0, "lamports");
    check(!row.contains(QStringLiteral("passphrase")), "no secrets");
    check(rewardSubject(good) == QLatin1String("acme/app PR #7"), "subject");
    QList<RewardFacts> many;
    for (int i = 0; i < 150; ++i) {
        RewardFacts one = good;
        one.rewardId = QStringLiteral("r%1").arg(i);
        many.append(one);
    }
    check(rewardRows(many).size() == kMaxRewards, "bounded rows");
}

static void testRequestsAndStatus()
{
    PhoneRequest none;
    check(!requestBlocks(none, 10), "no request never blocks");
    PhoneRequest waiting;
    waiting.id = newRequestId(QByteArray(16, '\x01'));
    waiting.action = QStringLiteral("tip");
    waiting.status = QStringLiteral("awaiting_owner");
    waiting.requestedAtMs = 1000;
    check(waiting.id.size() == 32, "request id length");
    check(requestBlocks(waiting, 2000), "a waiting request blocks");
    check(!requestBlocks(waiting, 1000 + kRequestStaleMs), "a stale request stops blocking");
    PhoneRequest done = waiting;
    done.status = QStringLiteral("approved");
    check(!requestBlocks(done, 2000), "an answered request does not block");

    WalletFacts facts;
    facts.linked = true;
    facts.address = kAddress;
    facts.minLamports = 1000000;
    facts.maxLamports = 1000000000;
    const QJsonObject status = statusPayload(facts, waiting, 2000);
    check(status.value(QStringLiteral("linked")).toBool(), "linked");
    check(status.value(QStringLiteral("busy")).toBool(), "busy while waiting");
    check(status.value(QStringLiteral("request")).toObject().value(QStringLiteral("id")).toString() == waiting.id,
          "request row");
    WalletFacts unlinked;
    unlinked.address = kAddress;
    const QJsonObject bare = statusPayload(unlinked, none, 0);
    check(bare.value(QStringLiteral("address")).toString().isEmpty(), "no address when unlinked");
    check(!bare.contains(QStringLiteral("request")), "no request row");
}

static void testConfirmationBinding()
{
    hostctl::ConfirmationStore store;
    const QString holder = confirmationHolder(QStringLiteral("dev"), 3);
    const QString target = tipTarget(QStringLiteral("bob"), kAddress, 5);
    const QString token = store.issue(holder, QStringLiteral("tip"), target, 0);
    check(store.consume(token, holder, QStringLiteral("tip"),
                        tipTarget(QStringLiteral("bob"), kAddress, 6), 1) ==
              hostctl::ConfirmationStore::Result::Mismatch,
          "a changed amount is refused");
    check(store.consume(token, holder, QStringLiteral("tip"), target, 2) ==
              hostctl::ConfirmationStore::Result::Missing,
          "a token is spent by any attempt");
    const QString second = store.issue(holder, QStringLiteral("tip"), target, 0);
    check(store.consume(second, confirmationHolder(QStringLiteral("dev"), 4), QStringLiteral("tip"),
                        target, 1) != hostctl::ConfirmationStore::Result::Ok,
          "another connection cannot spend it");
    const QString third = store.issue(holder, QStringLiteral("reward"), rewardTarget(QStringLiteral("r1")), 0);
    check(store.consume(third, holder, QStringLiteral("reward"), rewardTarget(QStringLiteral("r1")),
                        hostctl::ConfirmationStore::kLifetimeMs + 1) ==
              hostctl::ConfirmationStore::Result::Expired,
          "expired");
    const QString fourth = store.issue(holder, QStringLiteral("reward"), rewardTarget(QStringLiteral("r1")), 0);
    check(store.consume(fourth, holder, QStringLiteral("reward"), rewardTarget(QStringLiteral("r1")), 5) ==
              hostctl::ConfirmationStore::Result::Ok,
          "ok");
}

int main()
{
    testPrefixAndActions();
    testValidators();
    testLamports();
    testTargetsAndSummaries();
    testRewardRows();
    testRequestsAndStatus();
    testConfirmationBinding();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "local control wallet: all checks passed\n";
    return 0;
}
