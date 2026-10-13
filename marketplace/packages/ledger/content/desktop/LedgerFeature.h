#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QUrl>

#include <functional>

namespace forkmesh::ledger {

// One normalized public financial record as published by GET /api/ledger.
// Everything here is untrusted network input reduced to bounded fields; the
// desktop renders only what parseLedgerPage() accepted.
struct LedgerRecord {
    QString id;
    QString category;
    QString title;
    QString status;
    QString direction;
    QString verification;
    QString network;
    QString transactionId;
    QUrl explorerUrl;
    QString currency;
    quint64 amountLamports = 0;
    QString amountSol;
    QString amountUsd;
    QString valuationBasis;
    qint64 valuationObservedAt = 0;
    bool amountKnown = true;
    qint64 occurredAt = 0;
    QString sourceTable;
    QString sourceId;
    QString note;
    QString subjectLabel;   // the issue, pull, element or page it is about
    QString subjectTitle;   // that subject's own headline, when the relay names it
    QString subjectSummary; // bounded synopsis retained with the financial record
    QUrl subjectUrl;
    QString counterpartyName; // recipient/claimant/buyer, without the leading @
    // The linked Discord identity is attached only to a reward still awaiting
    // payment, so an owner can recognize the contributor beside their
    // ForkMesh account without confusing either identity with the payout key.
    QString counterpartyDiscordUsername;
    QString counterpartyDiscordDisplayName;

    // Filled in by applyPayableRewards() from the relay's owner-authored
    // estimate list. A record is payable from this desktop only when the relay
    // published an exact destination address and amount for it.
    QString rewardId;
    QString recipientAddress;
    QString repository;   // owner/name
    QString sourceKind;   // issue | pull
    int subjectNumber = 0;
    bool payable = false;

    // A record the reader entered on this machine rather than one the relay
    // published. It is kept in this app's own settings, is never sent
    // anywhere, and is the only kind of record the reader may edit or delete.
    bool local = false;
};

// Which side of the book a record falls on. The relay names a direction for
// every public record it publishes; a reader-entered record carries the one
// the form asked for.
enum class LedgerFlow {
    Neither,   // a transfer between two other people, or an unclassified record
    Income,
    Expense,   // money out that has moved, with the transaction that moved it
    Committed, // money out that is owed and has not been paid yet
};

// What a page of records adds up to, in the two figures a ledger is read for
// plus the one this page acts on. SOL is exact to the lamport; USD is the sum
// of each record's own estimate, which is what the rows themselves show.
struct LedgerTotals {
    quint64 incomeLamports = 0;
    quint64 expenseLamports = 0;
    quint64 pendingLamports = 0; // awaiting payment, either direction
    qint64 incomeUsdMicros = 0;
    qint64 expenseUsdMicros = 0;
    qint64 pendingUsdMicros = 0;
    int incomeCount = 0;
    int expenseCount = 0;
    int pendingCount = 0;
};

// One public system wallet named by GET /api/ledger. The relay publishes the
// addresses it spends from and receives into, plus the last public balance
// reading it holds for each; a personal wallet is never in this list.
struct LedgerWallet {
    QString key;      // community-reward-pool | treasury
    QString label;
    QString purpose;
    QString address;
    QString network;
    QUrl explorerUrl;
    quint64 balanceLamports = 0;
    bool balanceKnown = false;   // false until any reading has ever succeeded
    qint64 balanceCheckedAt = 0; // when that reading was taken, 0 = never
    bool sharedAddress = false;  // this host configured one address for both
};

// One page of /api/ledger plus the public totals it reports for that page.
struct LedgerPage {
    QString network;
    QString solUsdPrice;   // "123.456789", or empty when unavailable
    qint64 observedAt = 0;
    int recordCount = 0;
    int settledCount = 0;
    quint64 pageLamports = 0;
    bool hasMore = false;
    QList<LedgerRecord> records;
    QList<LedgerWallet> wallets;
};

// Runtime for the Ledger package. The host supplies the bounded capability to
// open one URL; the package owns construction of the public Ledger targets and
// the projection of the relay's public financial records.
class LedgerFeature final {
public:
    using OpenUrl = std::function<bool(const QUrl &)>;

    explicit LedgerFeature(OpenUrl openUrl);

    bool open(const QUrl &baseUrl) const;
    static QUrl urlFor(const QUrl &baseUrl);

    // GET endpoint for one page of public records, newest first. `limit` is
    // clamped to what the relay itself accepts. `refresh` asks the relay to
    // re-read the public system wallet balances it publishes with the page;
    // it still floors that to one live read per address per minute, so only a
    // reader's explicit Refresh press should set it.
    static QUrl apiUrlFor(const QUrl &baseUrl, int limit = 40,
                          bool refresh = false);

    // Parse a /api/ledger response body. Site-relative link targets are
    // resolved against `baseUrl` so a record always carries an absolute,
    // openable address or none at all. Returns false, with `error` set, for
    // anything that is not an ok page of records.
    static bool parseLedgerPage(const QByteArray &body, const QUrl &baseUrl,
                                LedgerPage *page, QString *error = nullptr);

    // Reader-facing SOL text for a lamport amount, exact to the lamport with
    // its trailing zeroes dropped ("0.05", not "0.050000000").
    static QString solText(quint64 lamports);

    // How old a cached balance reading is, in the words a card shows ("read
    // 2 hours ago"). Every balance on this page comes from a shared hourly
    // cache, so its age is part of the number.
    static QString balanceAgeText(qint64 checkedAt, qint64 nowMs);

    // True when the record is still waiting for money to move: a SOL amount,
    // no transaction, and a status that says "not settled yet" rather than
    // "will never settle".
    static bool isPendingPayment(const LedgerRecord &record);

    // Reader-facing wording for a raw relay status ("pending_wallet" ->
    // "awaiting the recipient's wallet"). Unknown states are returned with
    // their underscores relaxed rather than hidden.
    static QString statusText(const QString &status);

    // Project the owner-authored payout plan (from /api/rewards/estimates,
    // already validated by the caller) onto the matching contributor-reward
    // records, and clear it from every record it no longer covers. Returns how
    // many records are payable afterwards. A record the relay published no
    // exact destination address and matching amount for is never payable.
    static int applyPayableRewards(LedgerPage *page,
                                   const QList<LedgerRecord> &payable);

    // Which side of the book a record falls on. The relay's own vocabulary
    // ("inbound", "outbound", "allocation") plus the two words the entry form
    // on the Ledger page uses. A tip between two other people is neither.
    // Money out is an expense only once it has actually been paid; until then
    // it is Committed, which is owed rather than spent. See isPaidExpense().
    static LedgerFlow flowOf(const LedgerRecord &record);

    // True when money on the expense side has left: the relay published the
    // transaction that moved it. A record the reader keeps on this machine is
    // witnessed by no chain, so their own settled mark is its receipt.
    static bool isPaidExpense(const LedgerRecord &record);

    // What the page adds up to: income, expenses, and what is still owed.
    // Every record on the page counts, including the reader's own. Only paid
    // expenses reach the expense figures; what is still owed is counted once,
    // as pending.
    static LedgerTotals totalsFor(const LedgerPage &page);

    // A decimal money string ("142.5", "0.030000377") as an exact integer of
    // its smallest unit: `decimals` places, so 9 for lamports and 6 for USD
    // micros. Returns false for anything that is not a plain, non-negative
    // decimal with no more precision than the unit holds.
    static bool parseDecimalAmount(const QString &text, int decimals,
                                   qint64 *minorUnits);

    // The relay quotes USD to six decimal places. usdMicros() reads one of
    // those strings and returns -1 for anything that is not a figure, which
    // is a different answer from zero; usdText() renders micros as the
    // two-decimal figure money is read in, rounded rather than truncated.
    static qint64 usdMicros(const QString &amountUsd);
    static QString usdText(qint64 micros);

    // USD estimate for a lamport amount at the page's quoted spot price, in
    // micros. Returns -1 when the page carries no usable quote, which is a
    // different answer from "worth nothing".
    static qint64 usdMicrosForLamports(quint64 lamports,
                                       const QString &solUsdPrice);

    // Build one reader-entered record. `direction` is "income" or "expense",
    // `status` is "settled" or "outstanding", and the amount is lamports for
    // SOL or USD micros for a fiat entry. Returns a record with an empty id
    // for anything the page must refuse: no description, no amount, or a
    // currency this ledger does not keep.
    static LedgerRecord localRecord(const QString &id,
                                    const QString &direction,
                                    const QString &status,
                                    const QString &category,
                                    const QString &description,
                                    const QString &counterparty,
                                    const QString &currency, qint64 amount,
                                    qint64 occurredAt);

    // The reader's own records, as this app stores them between runs. Parsing
    // applies the same bounds as a relay response: a stored file that has been
    // edited by hand is still untrusted input.
    static QByteArray serializeLocalRecords(const QList<LedgerRecord> &records);
    static QList<LedgerRecord> parseLocalRecords(const QByteArray &body);

    // Project the reader's own records onto a relay page: replace whatever
    // local records the page already carries, price the SOL ones at the page's
    // quote, and re-order the whole page newest first so an entry lands where
    // its date says it belongs. The page's own counts are recomputed.
    static void applyLocalRecords(LedgerPage *page,
                                  const QList<LedgerRecord> &records);

    // Build the payable overlay record for one validated estimate.
    static LedgerRecord payableRecord(const QString &rewardId,
                                      const QString &recipientName,
                                      const QString &recipientAddress,
                                      const QString &repository,
                                      const QString &sourceKind,
                                      int subjectNumber, quint64 lamports);

private:
    OpenUrl m_openUrl;
};

} // namespace forkmesh::ledger
