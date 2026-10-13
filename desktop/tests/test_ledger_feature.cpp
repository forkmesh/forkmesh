// The Ledger package's record model: what the desktop is willing to render
// from a public /api/ledger response, and which of those records it will put
// a "Send payment" button on. Everything here is untrusted network input, so
// the interesting cases are the ones that must NOT become payable.

#include "LedgerFeature.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QUrl>

#include <algorithm>
#include <cstdio>

using forkmesh::ledger::LedgerFeature;
using forkmesh::ledger::LedgerPage;
using forkmesh::ledger::LedgerRecord;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition) {
        std::printf("PASS: %s\n", message);
    } else {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
}

QJsonObject entry(const QString &id, const QString &category,
                  const QString &status, qint64 lamports,
                  const QString &signature = QString())
{
    return QJsonObject{
        {QStringLiteral("id"), id},
        {QStringLiteral("category"), category},
        {QStringLiteral("title"), QStringLiteral("Pull request reward")},
        {QStringLiteral("status"), status},
        {QStringLiteral("currency"), QStringLiteral("SOL")},
        {QStringLiteral("amountLamports"), double(lamports)},
        {QStringLiteral("amountSol"), QStringLiteral("0.004")},
        {QStringLiteral("amountKnown"), true},
        {QStringLiteral("occurredAt"), 1750000000000.0},
        {QStringLiteral("sourceId"), id.section(QLatin1Char(':'), 1)},
        {QStringLiteral("transactionId"), signature},
    };
}

QByteArray pageBody(const QJsonArray &entries)
{
    const QJsonObject root{
        {QStringLiteral("ok"), true},
        {QStringLiteral("network"), QStringLiteral("mainnet-beta")},
        {QStringLiteral("observedAt"), 1750000001000.0},
        {QStringLiteral("entries"), entries},
        {QStringLiteral("solUsd"),
         QJsonObject{{QStringLiteral("available"), true},
                     {QStringLiteral("price"), QStringLiteral("142.500000")}}},
        {QStringLiteral("page"), QJsonObject{{QStringLiteral("hasMore"), false}}},
    };
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

const QUrl kBase{QStringLiteral("https://app.forkmesh.com/api/repositories")};

void testEndpoints()
{
    const QUrl api = LedgerFeature::apiUrlFor(kBase, 60);
    check(api.scheme() == QLatin1String("https") &&
              api.host() == QLatin1String("app.forkmesh.com") &&
              api.path() == QLatin1String("/api/ledger") &&
              api.query() == QLatin1String("limit=60"),
          "the records endpoint is the active host's /api/ledger");
    check(LedgerFeature::apiUrlFor(kBase, 60, true).query() ==
              QLatin1String("limit=60&refresh=1"),
          "an explicit refresh is the only thing that asks the relay to "
          "re-read a wallet balance");
    check(LedgerFeature::apiUrlFor(kBase, 5000).query() ==
              QLatin1String("limit=100"),
          "an over-large page size is clamped to what the relay accepts");
    check(!LedgerFeature::apiUrlFor(QUrl(QStringLiteral("file:///etc/passwd")),
                                    10)
               .isValid(),
          "a non-http host contributes no ledger endpoint");
    check(LedgerFeature::urlFor(kBase).path() == QLatin1String("/ledger"),
          "the browser target is still the host's public ledger page");
}

void testParsing()
{
    QJsonObject rich = entry(QStringLiteral("contributor-reward:reward-abcdefgh"),
                             QStringLiteral("contributor-reward"),
                             QStringLiteral("pending_wallet"), 4000000);
    rich.insert(QStringLiteral("note"), QStringLiteral("Payment on merge."));
    rich.insert(QStringLiteral("links"),
                QJsonArray{
                    QJsonObject{{QStringLiteral("type"), QStringLiteral("user")},
                                {QStringLiteral("role"), QStringLiteral("recipient")},
                                {QStringLiteral("key"), QStringLiteral("river")},
                                {QStringLiteral("label"), QStringLiteral("@river")},
                                {QStringLiteral("discordUsername"),
                                 QStringLiteral("river.reports")},
                                {QStringLiteral("discordDisplayName"),
                                 QStringLiteral("River Reports")}},
                    QJsonObject{{QStringLiteral("type"), QStringLiteral("pull")},
                                {QStringLiteral("role"), QStringLiteral("subject")},
                                {QStringLiteral("key"), QStringLiteral("forkmesh/forkmesh#12")},
                                {QStringLiteral("label"), QStringLiteral("forkmesh/forkmesh pull #12")},
                                {QStringLiteral("title"), QStringLiteral("Ledger rows name their subject")},
                                {QStringLiteral("summary"), QStringLiteral("Preserve a compact explanation after source deletion.")},
                                {QStringLiteral("url"), QStringLiteral("/forkmesh/forkmesh/pulls/12")}},
                });
    rich.insert(QStringLiteral("valuationBasis"),
                QStringLiteral("recorded-at-payment-sol-usd"));
    rich.insert(QStringLiteral("valuationObservedAt"), 1750000000500.0);

    LedgerPage page;
    QString error;
    check(LedgerFeature::parseLedgerPage(pageBody(QJsonArray{rich}), kBase,
                                         &page, &error) &&
              page.records.size() == 1 && page.recordCount == 1 &&
              page.pageLamports == 4000000ull && page.settledCount == 0 &&
              page.solUsdPrice == QLatin1String("142.500000"),
          "one page of records parses with its public totals");
    const LedgerRecord &record = page.records.first();
    check(record.counterpartyName == QLatin1String("river") &&
              record.counterpartyDiscordUsername ==
                  QLatin1String("river.reports") &&
              record.counterpartyDiscordDisplayName ==
                  QLatin1String("River Reports") &&
              record.subjectLabel == QLatin1String("forkmesh/forkmesh pull #12") &&
              record.subjectUrl.toString() ==
                  QLatin1String("https://app.forkmesh.com/forkmesh/forkmesh/pulls/12"),
          "a site-relative subject link resolves against the serving host");
    check(record.subjectTitle ==
              QLatin1String("Ledger rows name their subject"),
          "the subject's own headline is kept beside the record's label");
    check(record.subjectSummary ==
                  QLatin1String("Preserve a compact explanation after source deletion.") &&
              record.valuationBasis ==
                  QLatin1String("recorded-at-payment-sol-usd") &&
              record.valuationObservedAt == 1750000000500ll,
          "bounded subject context and the payment-time valuation basis survive parsing");

    QJsonObject untitled =
        entry(QStringLiteral("contributor-reward:reward-untitled"),
              QStringLiteral("contributor-reward"),
              QStringLiteral("pending_wallet"), 4000000);
    untitled.insert(QStringLiteral("links"),
                    QJsonArray{QJsonObject{
                        {QStringLiteral("type"), QStringLiteral("issue")},
                        {QStringLiteral("role"), QStringLiteral("subject")},
                        {QStringLiteral("key"), QStringLiteral("forkmesh/forkmesh#9")},
                        {QStringLiteral("label"), QStringLiteral("forkmesh/forkmesh issue #9")}}});
    LedgerPage untitledPage;
    check(LedgerFeature::parseLedgerPage(pageBody(QJsonArray{untitled}), kBase,
                                         &untitledPage, nullptr) &&
              untitledPage.records.size() == 1 &&
              untitledPage.records.first().subjectTitle.isEmpty() &&
              untitledPage.records.first().subjectLabel ==
                  QLatin1String("forkmesh/forkmesh issue #9"),
          "a relay that publishes no subject title leaves the field empty "
          "rather than borrowing the label");

    QJsonObject hostile =
        entry(QStringLiteral("member-tip:sig"), QStringLiteral("member-tip"),
              QStringLiteral("reported-on-chain"), 1000000,
              QStringLiteral("sig"));
    hostile.insert(QStringLiteral("explorerUrl"),
                   QStringLiteral("javascript:alert(1)"));
    hostile.insert(QStringLiteral("links"),
                   QJsonArray{QJsonObject{
                       {QStringLiteral("type"), QStringLiteral("pull")},
                       {QStringLiteral("role"), QStringLiteral("subject")},
                       {QStringLiteral("label"), QStringLiteral("elsewhere")},
                       {QStringLiteral("url"), QStringLiteral("//evil.example/x")}}});
    LedgerPage hostilePage;
    check(LedgerFeature::parseLedgerPage(pageBody(QJsonArray{hostile}), kBase,
                                         &hostilePage, nullptr) &&
              hostilePage.records.size() == 1 &&
              !hostilePage.records.first().explorerUrl.isValid() &&
              !hostilePage.records.first().subjectUrl.isValid(),
          "javascript: and protocol-relative link targets are dropped");

    QJsonObject huge = entry(QStringLiteral("x:1"), QStringLiteral("member-tip"),
                             QStringLiteral("reported-on-chain"), 0);
    huge.insert(QStringLiteral("amountLamports"), 1e30);
    LedgerPage hugePage;
    check(LedgerFeature::parseLedgerPage(pageBody(QJsonArray{huge}), kBase,
                                         &hugePage, nullptr) &&
              hugePage.records.first().amountLamports == 0,
          "an amount outside the exact-integer range is not shown as money");

    LedgerPage rejected;
    check(!LedgerFeature::parseLedgerPage(QByteArray("{\"ok\":false}"), kBase,
                                          &rejected, &error) &&
              !error.isEmpty(),
          "a response that is not an ok page of records is refused");
}

void testPendingClassification()
{
    LedgerRecord pending;
    pending.currency = QStringLiteral("SOL");
    pending.status = QStringLiteral("ready");
    check(LedgerFeature::isPendingPayment(pending),
          "a ready SOL record with no transaction is awaiting payment");

    LedgerRecord settled = pending;
    settled.transactionId = QStringLiteral("5xM");
    check(!LedgerFeature::isPendingPayment(settled),
          "a record carrying a transaction is not awaiting payment");

    LedgerRecord expired = pending;
    expired.status = QStringLiteral("expired");
    check(!LedgerFeature::isPendingPayment(expired),
          "an expired allocation is finished, not awaiting payment");

    LedgerRecord fiat = pending;
    fiat.currency = QStringLiteral("USD");
    check(!LedgerFeature::isPendingPayment(fiat),
          "a fiat record is never offered for a SOL transfer");

    check(LedgerFeature::statusText(QStringLiteral("pending_merge")) ==
                  QLatin1String("pays on merge") &&
              LedgerFeature::statusText(QStringLiteral("some_new_state")) ==
                  QLatin1String("some new state"),
          "statuses read as sentences, and unknown ones are still shown");
}

// Money out is spent only once it has moved. Everything else on the outbound
// side is a promise, and a book that counts promises as spending charges
// itself twice for every reward it eventually pays.
void testExpenseRequiresPayment()
{
    using forkmesh::ledger::LedgerFlow;

    LedgerRecord allocation;
    allocation.currency = QStringLiteral("SOL");
    allocation.direction = QStringLiteral("allocation");
    allocation.status = QStringLiteral("ready");
    allocation.amountLamports = 4000000;
    check(!LedgerFeature::isPaidExpense(allocation) &&
              LedgerFeature::flowOf(allocation) == LedgerFlow::Committed,
          "a reward that is ready to pay is owed, not spent");

    LedgerRecord paid = allocation;
    paid.direction = QStringLiteral("outbound");
    paid.status = QStringLiteral("paid");
    paid.transactionId = QStringLiteral("5xMpaidSignature");
    check(LedgerFeature::isPaidExpense(paid) &&
              LedgerFeature::flowOf(paid) == LedgerFlow::Expense,
          "the transaction that moved the money is what makes it an expense");

    LedgerRecord expired = allocation;
    expired.status = QStringLiteral("expired");
    check(LedgerFeature::flowOf(expired) == LedgerFlow::Committed &&
              !LedgerFeature::isPendingPayment(expired),
          "an allocation that was written off is neither spent nor owed");

    // A record kept on this machine is witnessed by no chain, so the reader's
    // own settled mark is the receipt the transaction would otherwise be.
    LedgerRecord ownSettled = LedgerFeature::localRecord(
        QStringLiteral("paid-1"), QStringLiteral("expense"),
        QStringLiteral("settled"), QStringLiteral("Hosting"),
        QStringLiteral("Vultr, July"), QString(), QStringLiteral("USD"),
        42000000, 1750000000000LL);
    check(ownSettled.transactionId.isEmpty() &&
              LedgerFeature::isPaidExpense(ownSettled) &&
              LedgerFeature::flowOf(ownSettled) == LedgerFlow::Expense,
          "a bill the reader marked settled is an expense without a "
          "transaction it could never have");

    LedgerRecord inbound;
    inbound.direction = QStringLiteral("inbound");
    inbound.currency = QStringLiteral("SOL");
    check(LedgerFeature::flowOf(inbound) == LedgerFlow::Income,
          "the rule is about money leaving: income is unchanged by it");

    LedgerRecord peer;
    peer.direction = QStringLiteral("peer");
    peer.transactionId = QStringLiteral("5xMbetweenOthers");
    check(LedgerFeature::flowOf(peer) == LedgerFlow::Neither,
          "a paid transfer between two other people is still neither side of "
          "this book");
}

void testPayableOverlay()
{
    const QJsonArray entries{
        entry(QStringLiteral("contributor-reward:reward-abcdefgh"),
              QStringLiteral("contributor-reward"), QStringLiteral("ready"),
              4000000),
        entry(QStringLiteral("contributor-reward:reward-ijklmnop"),
              QStringLiteral("contributor-reward"), QStringLiteral("ready"),
              4000000),
        entry(QStringLiteral("pending-reward:reward-qrstuvwx"),
              QStringLiteral("pending-reward"),
              QStringLiteral("pending_wallet"), 4000000),
        entry(QStringLiteral("contributor-reward:reward-yzabcdef"),
              QStringLiteral("contributor-reward"), QStringLiteral("paid"),
              4000000, QStringLiteral("5xMalreadyPaid")),
    };
    LedgerPage page;
    check(LedgerFeature::parseLedgerPage(pageBody(entries), kBase, &page,
                                         nullptr) &&
              page.records.size() == 4,
          "the mixed page of rewards parses");

    const QList<LedgerRecord> plans{
        // Exact match: becomes payable.
        LedgerFeature::payableRecord(
            QStringLiteral("reward-abcdefgh"), QStringLiteral("river"),
            QStringLiteral("6dNVSVJ3wTFRDvhCcRNXfPBS1ykt9WMFhFPeYnkPmUJj"),
            QStringLiteral("forkmesh/forkmesh"), QStringLiteral("pull"), 12,
            4000000),
        // The relay's two projections disagree on the figure: not payable.
        LedgerFeature::payableRecord(
            QStringLiteral("reward-ijklmnop"), QStringLiteral("wren"),
            QStringLiteral("6dNVSVJ3wTFRDvhCcRNXfPBS1ykt9WMFhFPeYnkPmUJj"),
            QStringLiteral("forkmesh/forkmesh"), QStringLiteral("issue"), 7,
            9000000),
        // A relay-scheduled allocation, not an owner-paid estimate.
        LedgerFeature::payableRecord(
            QStringLiteral("reward-qrstuvwx"), QStringLiteral("ash"),
            QStringLiteral("6dNVSVJ3wTFRDvhCcRNXfPBS1ykt9WMFhFPeYnkPmUJj"),
            QStringLiteral("forkmesh/forkmesh"), QStringLiteral("issue"), 9,
            4000000),
        // Already settled on-chain.
        LedgerFeature::payableRecord(
            QStringLiteral("reward-yzabcdef"), QStringLiteral("fern"),
            QStringLiteral("6dNVSVJ3wTFRDvhCcRNXfPBS1ykt9WMFhFPeYnkPmUJj"),
            QStringLiteral("forkmesh/forkmesh"), QStringLiteral("pull"), 4,
            4000000),
    };
    check(LedgerFeature::applyPayableRewards(&page, plans) == 1,
          "exactly the one record both projections agree on becomes payable");
    check(page.records.at(0).payable &&
              page.records.at(0).repository ==
                  QLatin1String("forkmesh/forkmesh") &&
              page.records.at(0).sourceKind == QLatin1String("pull") &&
              page.records.at(0).subjectNumber == 12,
          "the payable record carries the subject the receipt is written to");
    check(!page.records.at(1).payable,
          "a mismatched amount never gets a payment button");
    check(!page.records.at(2).payable,
          "a relay-scheduled allocation is not paid from this desktop");
    check(!page.records.at(3).payable,
          "an already-settled reward cannot be paid a second time");
    check(LedgerFeature::applyPayableRewards(&page, {}) == 0 &&
              !page.records.at(0).payable &&
              page.records.at(0).recipientAddress.isEmpty(),
          "when the relay stops publishing a payout, the record stops being "
          "payable instead of keeping the old plan");
}

// The wallet strip above the table. The relay names its own public system
// wallets in the same response; a reader's personal wallets are never in it.
void testSystemWallets()
{
    const QJsonObject pool{
        {QStringLiteral("key"), QStringLiteral("community-reward-pool")},
        {QStringLiteral("label"), QStringLiteral("Global reward wallet")},
        {QStringLiteral("purpose"), QStringLiteral("Reward rounds pay from it.")},
        {QStringLiteral("address"),
         QStringLiteral("6dNVSVJ3wTFRDvhCcRNXfPBS1ykt9WMFhFPeYnkPmUJj")},
        {QStringLiteral("network"), QStringLiteral("mainnet-beta")},
        {QStringLiteral("explorerUrl"),
         QStringLiteral("https://explorer.solana.com/address/"
                        "6dNVSVJ3wTFRDvhCcRNXfPBS1ykt9WMFhFPeYnkPmUJj")},
        {QStringLiteral("balanceLamports"), 1250000000.0},
        {QStringLiteral("balanceKnown"), true},
        {QStringLiteral("balanceCheckedAt"), 1750000000000.0},
        {QStringLiteral("sharedAddress"), true},
    };
    QJsonObject unread = pool;
    unread[QStringLiteral("key")] = QStringLiteral("treasury");
    unread[QStringLiteral("label")] = QStringLiteral("Treasury wallet");
    unread[QStringLiteral("balanceKnown")] = false;
    unread[QStringLiteral("balanceCheckedAt")] = 0.0;
    unread[QStringLiteral("balanceLamports")] = 0.0;
    QJsonObject bogus = pool;
    bogus[QStringLiteral("address")] = QStringLiteral("not-an-address");
    QJsonObject unnamed = pool;
    unnamed[QStringLiteral("label")] = QString();

    QJsonObject root =
        QJsonDocument::fromJson(pageBody({entry(
                                    QStringLiteral("contributor-reward:a"),
                                    QStringLiteral("contributor-reward"),
                                    QStringLiteral("ready"), 4000000)}))
            .object();
    root[QStringLiteral("wallets")] =
        QJsonArray{pool, unread, bogus, unnamed};

    LedgerPage page;
    check(LedgerFeature::parseLedgerPage(
              QJsonDocument(root).toJson(QJsonDocument::Compact), kBase, &page),
          "a page carrying system wallets still parses");
    check(page.wallets.size() == 2,
          "an unreadable address and an unnamed wallet are both dropped");
    check(page.wallets.at(0).label == QLatin1String("Global reward wallet") &&
              page.wallets.at(0).balanceKnown &&
              page.wallets.at(0).balanceLamports == 1250000000ULL &&
              page.wallets.at(0).sharedAddress,
          "a read wallet keeps its exact lamport balance and shared-address "
          "flag");
    check(!page.wallets.at(1).balanceKnown,
          "a wallet the relay has never read is not reported as holding zero");
    check(page.wallets.at(0).explorerUrl.host() ==
              QLatin1String("explorer.solana.com"),
          "the explorer link survives as an absolute https address");

    root[QStringLiteral("wallets")] = QJsonValue(QStringLiteral("nope"));
    LedgerPage without;
    check(LedgerFeature::parseLedgerPage(
              QJsonDocument(root).toJson(QJsonDocument::Compact), kBase,
              &without) &&
              without.wallets.isEmpty(),
          "a response with no usable wallets block still yields its records");
}

void testWalletTextHelpers()
{
    check(LedgerFeature::solText(0) == QLatin1String("0") &&
              LedgerFeature::solText(50000000) == QLatin1String("0.05") &&
              LedgerFeature::solText(1000000001) ==
                  QLatin1String("1.000000001"),
          "SOL text is exact to the lamport with trailing zeroes dropped");
    const qint64 now = 1750000000000LL;
    check(LedgerFeature::balanceAgeText(0, now) == QLatin1String("never read"),
          "an address with no reading says so instead of claiming a time");
    check(LedgerFeature::balanceAgeText(now - 30000, now) ==
              QLatin1String("read just now"),
          "a reading under a minute old reads as just now");
    check(LedgerFeature::balanceAgeText(now - 2 * 60 * 60 * 1000, now) ==
              QLatin1String("read 2 hours ago"),
          "an hours-old cached reading says how old it is");
    check(LedgerFeature::balanceAgeText(now - 25 * 60 * 60 * 1000, now) ==
              QLatin1String("read 1 day ago"),
          "a day-old cached reading rounds to days");
}

// The general-purpose side of the page: a reader's own expense and income
// records, and the totals a ledger is actually read for.
void testMoneyTextHelpers()
{
    qint64 minor = 0;
    check(LedgerFeature::parseDecimalAmount(QStringLiteral("0.05"), 9,
                                            &minor) &&
              minor == 50000000,
          "a typed SOL figure becomes exact lamports");
    check(LedgerFeature::parseDecimalAmount(QStringLiteral("12"), 2, &minor) &&
              minor == 1200,
          "a whole dollar figure needs no decimal point");
    check(LedgerFeature::parseDecimalAmount(QStringLiteral(".5"), 2, &minor) &&
              minor == 50,
          "a leading point is read as the fraction it is");
    check(!LedgerFeature::parseDecimalAmount(QStringLiteral("1.234"), 2,
                                             &minor),
          "a figure with more precision than the unit holds is refused, not "
          "rounded into the reader's books");
    check(!LedgerFeature::parseDecimalAmount(QStringLiteral("-3"), 9, &minor) &&
              !LedgerFeature::parseDecimalAmount(QStringLiteral("1,250"), 2,
                                                 &minor) &&
              !LedgerFeature::parseDecimalAmount(QStringLiteral("1e3"), 9,
                                                 &minor) &&
              !LedgerFeature::parseDecimalAmount(QString(), 2, &minor),
          "a sign, a separator, an exponent and an empty box are all refused");

    check(LedgerFeature::usdMicros(QStringLiteral("142.500000")) == 142500000,
          "a relay USD quote reads as exact micros");
    check(LedgerFeature::usdMicros(QStringLiteral("nope")) < 0,
          "a USD figure that cannot be read is not zero");
    check(LedgerFeature::usdText(142505000) == QLatin1String("142.51") &&
              LedgerFeature::usdText(0) == QLatin1String("0.00"),
          "USD renders as money, rounded to the nearest cent");
    check(LedgerFeature::usdMicrosForLamports(
              4000000, QStringLiteral("142.500000")) == 570000,
          "a lamport amount is priced at the page's own spot quote");
    check(LedgerFeature::usdMicrosForLamports(4000000, QString()) < 0,
          "without a quote a balance has no dollar figure, rather than none "
          "of the currency");
}

void testLocalRecords()
{
    const LedgerRecord expense = LedgerFeature::localRecord(
        QStringLiteral("entry-1"), QStringLiteral("expense"),
        QStringLiteral("outstanding"), QStringLiteral("Hosting"),
        QStringLiteral("Vultr mirror fleet, August"), QStringLiteral("@vultr"),
        QStringLiteral("USD"), 42000000 /* $42 */, 1750000000000LL);
    check(expense.id == QLatin1String("local:entry-1") && expense.local &&
              expense.direction == QLatin1String("expense") &&
              expense.title == QLatin1String("Hosting") &&
              expense.subjectTitle ==
                  QLatin1String("Vultr mirror fleet, August") &&
              expense.counterpartyName == QLatin1String("vultr") &&
              expense.amountUsd == QLatin1String("42.00"),
          "a typed expense becomes a record with the same shape as a relay "
          "one");
    check(LedgerFeature::flowOf(expense) ==
              forkmesh::ledger::LedgerFlow::Committed &&
              LedgerFeature::isPendingPayment(expense),
          "an outstanding record of the reader's own is money still owed, "
          "which is not yet an expense");

    check(LedgerFeature::localRecord(QStringLiteral("entry-2"),
                                     QStringLiteral("expense"),
                                     QStringLiteral("settled"), QString(),
                                     QString(), QString(),
                                     QStringLiteral("USD"), 100, 0)
              .id.isEmpty(),
          "a record with nothing said about it is refused");
    check(LedgerFeature::localRecord(QStringLiteral("entry-3"),
                                     QStringLiteral("expense"),
                                     QStringLiteral("settled"), QString(),
                                     QStringLiteral("Coffee"), QString(),
                                     QStringLiteral("EUR"), 100, 0)
              .id.isEmpty() &&
              LedgerFeature::localRecord(QStringLiteral("entry-4"),
                                         QStringLiteral("sideways"),
                                         QStringLiteral("settled"), QString(),
                                         QStringLiteral("Coffee"), QString(),
                                         QStringLiteral("USD"), 100, 0)
                  .id.isEmpty(),
          "a currency this ledger does not keep, and a direction it does not "
          "know, are both refused");

    const LedgerRecord income = LedgerFeature::localRecord(
        QStringLiteral("entry-5"), QStringLiteral("income"),
        QStringLiteral("settled"), QStringLiteral("Sponsorship"),
        QStringLiteral("Monthly sponsor"), QStringLiteral("river"),
        QStringLiteral("SOL"), 2000000000LL, 1750000100000LL);
    const QByteArray stored =
        LedgerFeature::serializeLocalRecords({expense, income});
    const QList<LedgerRecord> reread =
        LedgerFeature::parseLocalRecords(stored);
    check(reread.size() == 2 && reread.at(0).id == expense.id &&
              reread.at(1).amountLamports == 2000000000ULL &&
              reread.at(1).status == QLatin1String("settled") &&
              reread.at(0).status == QLatin1String("outstanding"),
          "the reader's book survives a round trip through storage");
    check(LedgerFeature::parseLocalRecords(QByteArray("{\"ok\":true}"))
                  .isEmpty() &&
              LedgerFeature::parseLocalRecords(
                  QByteArray("[{\"id\":\"x\",\"currency\":\"SOL\"}]"))
                  .isEmpty(),
          "a stored file that has been edited by hand is still untrusted "
          "input");

    LedgerPage page;
    QJsonObject reward = entry(QStringLiteral("contributor-reward:r1"),
                               QStringLiteral("contributor-reward"),
                               QStringLiteral("ready"), 4000000);
    reward.insert(QStringLiteral("direction"), QStringLiteral("allocation"));
    reward.insert(QStringLiteral("amountUsd"), QStringLiteral("0.570000"));
    QJsonObject tip = entry(QStringLiteral("member-tip:sig"),
                            QStringLiteral("member-tip"),
                            QStringLiteral("reported-on-chain"), 500000000,
                            QStringLiteral("5xMsettled"));
    tip.insert(QStringLiteral("direction"), QStringLiteral("peer"));
    tip.insert(QStringLiteral("amountUsd"), QStringLiteral("71.250000"));
    QJsonObject payout = entry(QStringLiteral("chain-intent:i1:001"),
                               QStringLiteral("mirror-reward"),
                               QStringLiteral("completed"), 1000000,
                               QStringLiteral("5xMpaidPayout"));
    payout.insert(QStringLiteral("direction"), QStringLiteral("outbound"));
    payout.insert(QStringLiteral("amountUsd"), QStringLiteral("0.142500"));
    check(LedgerFeature::parseLedgerPage(
              pageBody(QJsonArray{reward, tip, payout}), kBase, &page),
          "a page carrying a directed reward, a peer tip and a paid payout "
          "parses");

    LedgerFeature::applyLocalRecords(&page, reread);
    check(page.records.size() == 5 && page.recordCount == 5,
          "the reader's records join the relay's on the same page");
    check(page.records.at(0).occurredAt >= page.records.at(1).occurredAt &&
              page.records.at(1).occurredAt >= page.records.at(2).occurredAt &&
              page.records.at(2).occurredAt >= page.records.at(3).occurredAt,
          "the merged page is still newest first, so an entry lands where its "
          "own date puts it");
    LedgerFeature::applyLocalRecords(&page, reread);
    check(page.records.size() == 5,
          "merging again replaces the reader's records rather than doubling "
          "them");
    const auto sponsorship =
        std::find_if(page.records.cbegin(), page.records.cend(),
                     [](const LedgerRecord &record) {
                         return record.id == QLatin1String("local:entry-5");
                     });
    check(sponsorship != page.records.cend() &&
              sponsorship->amountUsd == QLatin1String("285.00"),
          "a SOL record of the reader's own is priced at the same page quote "
          "as the rows around it");

    const forkmesh::ledger::LedgerTotals totals =
        LedgerFeature::totalsFor(page);
    check(totals.expenseCount == 1 && totals.incomeCount == 1,
          "a tip between two other people is neither the reader's income nor "
          "their expense");
    check(totals.expenseLamports == 1000000ULL &&
              totals.expenseUsdMicros == 142500,
          "expenses add SOL exactly and dollars alongside it, counting only "
          "the payout a transaction actually moved");
    check(totals.incomeLamports == 2000000000ULL &&
              totals.incomeUsdMicros == 285000000,
          "income totals the reader's own settled record");
    check(totals.pendingCount == 2 && totals.pendingLamports == 4000000ULL &&
              totals.pendingUsdMicros == 570000 + 42000000,
          "what is awaiting payment counts the relay's ready reward and the "
          "reader's outstanding bill together");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testEndpoints();
    testParsing();
    testPendingClassification();
    testExpenseRequiresPayment();
    testPayableOverlay();
    testSystemWallets();
    testWalletTextHelpers();
    testMoneyTextHelpers();
    testLocalRecords();
    if (failures)
        std::printf("%d ledger check(s) failed\n", failures);
    return failures ? 1 : 0;
}
