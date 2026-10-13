#include "LedgerFeature.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <utility>

namespace forkmesh::ledger {
namespace {

// The relay caps one page at 100 records. Everything below is a defensive
// bound on untrusted network input, not a display preference.
constexpr int kMaximumPageRecords = 100;
// The relay names two public system wallets. The bound is on untrusted input,
// not on how many it is expected to publish.
constexpr int kMaximumPageWallets = 8;
constexpr int kMaximumTextLength = 400;
constexpr int kMaximumIdentifierLength = 300;
constexpr double kMaximumExactInteger = 9007199254740991.0;
// The reader's own book, kept in this app's settings. The bound is on how
// much of it this page will carry into memory and re-sort on every rebuild.
constexpr int kMaximumLocalRecords = 500;

QString boundedText(const QJsonValue &value, int limit = kMaximumTextLength)
{
    return value.toString().simplified().left(limit);
}

// JSON numbers arrive as doubles. Only an exact, non-negative integer inside
// the range JavaScript itself round-trips is a usable amount or timestamp.
bool exactInteger(const QJsonValue &value, qint64 *out)
{
    const double number = value.toDouble(-1);
    if (!std::isfinite(number) || number < 0 ||
        number > kMaximumExactInteger || std::floor(number) != number)
        return false;
    *out = qint64(number);
    return true;
}

// Absolute https/http only, or a site-relative path resolved against the host
// the page itself came from. Anything else (javascript:, file:, protocol
// relative "//elsewhere") is dropped rather than shown as an openable link.
QUrl safeLink(const QJsonValue &value, const QUrl &baseUrl)
{
    const QString text =
        value.toString().trimmed().left(kMaximumIdentifierLength);
    if (text.isEmpty() || text.startsWith(QLatin1String("//")))
        return {};
    if (text.startsWith(QLatin1Char('/'))) {
        if (!baseUrl.isValid() || baseUrl.host().isEmpty())
            return {};
        QUrl resolved(baseUrl);
        resolved.setUserInfo(QString());
        resolved.setPath(text.section(QLatin1Char('?'), 0, 0)
                             .section(QLatin1Char('#'), 0, 0));
        resolved.setQuery(QString());
        resolved.setFragment(QString());
        return resolved;
    }
    const QUrl parsed(text, QUrl::StrictMode);
    if (!parsed.isValid() || parsed.host().isEmpty() ||
        (parsed.scheme() != QLatin1String("https") &&
         parsed.scheme() != QLatin1String("http")))
        return {};
    return parsed;
}

// The relay describes each record's participants as typed links. Pull out the
// two a reader actually scans for: what the money is about, and who gets it.
void applyLinks(const QJsonArray &links, const QUrl &baseUrl,
                LedgerRecord *record)
{
    for (const QJsonValue &value : links) {
        const QJsonObject link = value.toObject();
        const QString role = boundedText(link.value(QStringLiteral("role")), 40);
        const QString type = boundedText(link.value(QStringLiteral("type")), 40);
        if (record->subjectLabel.isEmpty() && role == QLatin1String("subject")) {
            record->subjectLabel =
                boundedText(link.value(QStringLiteral("label")), 120);
            // The subject's own headline, when the relay publishes one. The
            // label identifies the record ("forkmesh/forkmesh issue #628");
            // this is what it is actually about.
            record->subjectTitle =
                boundedText(link.value(QStringLiteral("title")), 160);
            record->subjectSummary =
                boundedText(link.value(QStringLiteral("summary")), 360);
            record->subjectUrl =
                safeLink(link.value(QStringLiteral("url")), baseUrl);
        } else if (record->counterpartyName.isEmpty() &&
                   type == QLatin1String("user") &&
                   (role == QLatin1String("recipient") ||
                    role == QLatin1String("claimant") ||
                    role == QLatin1String("buyer"))) {
            QString name = boundedText(link.value(QStringLiteral("key")), 80);
            if (name.isEmpty())
                name = boundedText(link.value(QStringLiteral("label")), 80);
            if (name.startsWith(QLatin1Char('@')))
                name.remove(0, 1);
            record->counterpartyName = name;
            QString discordUsername =
                boundedText(link.value(QStringLiteral("discordUsername")), 80);
            while (discordUsername.startsWith(QLatin1Char('@')))
                discordUsername.remove(0, 1);
            record->counterpartyDiscordUsername = discordUsername;
            record->counterpartyDiscordDisplayName = boundedText(
                link.value(QStringLiteral("discordDisplayName")), 100);
        }
    }
}

bool parseRecord(const QJsonObject &object, const QUrl &baseUrl,
                 LedgerRecord *record)
{
    LedgerRecord parsed;
    parsed.id = boundedText(object.value(QStringLiteral("id")),
                            kMaximumIdentifierLength);
    if (parsed.id.isEmpty())
        return false;
    parsed.category = boundedText(object.value(QStringLiteral("category")), 60);
    parsed.title = boundedText(object.value(QStringLiteral("title")), 120);
    parsed.status = boundedText(object.value(QStringLiteral("status")), 60);
    parsed.direction =
        boundedText(object.value(QStringLiteral("direction")), 40);
    parsed.verification =
        boundedText(object.value(QStringLiteral("verification")), 60);
    parsed.network = boundedText(object.value(QStringLiteral("network")), 40);
    parsed.transactionId =
        boundedText(object.value(QStringLiteral("transactionId")), 130);
    parsed.explorerUrl =
        safeLink(object.value(QStringLiteral("explorerUrl")), baseUrl);
    parsed.currency =
        boundedText(object.value(QStringLiteral("currency")), 8).toUpper();
    parsed.amountSol =
        boundedText(object.value(QStringLiteral("amountSol")), 40);
    parsed.amountUsd =
        boundedText(object.value(QStringLiteral("amountUsd")), 40);
    parsed.valuationBasis =
        boundedText(object.value(QStringLiteral("valuationBasis")), 60);
    parsed.amountKnown = object.value(QStringLiteral("amountKnown")).toBool(true);
    parsed.sourceTable =
        boundedText(object.value(QStringLiteral("sourceTable")), 80);
    parsed.sourceId = boundedText(object.value(QStringLiteral("sourceId")),
                                  kMaximumIdentifierLength);
    parsed.note = boundedText(object.value(QStringLiteral("note")));

    qint64 lamports = 0;
    if (exactInteger(object.value(QStringLiteral("amountLamports")), &lamports))
        parsed.amountLamports = quint64(lamports);
    qint64 occurredAt = 0;
    if (exactInteger(object.value(QStringLiteral("occurredAt")), &occurredAt))
        parsed.occurredAt = occurredAt;
    qint64 valuationObservedAt = 0;
    if (exactInteger(object.value(QStringLiteral("valuationObservedAt")),
                     &valuationObservedAt))
        parsed.valuationObservedAt = valuationObservedAt;
    applyLinks(object.value(QStringLiteral("links")).toArray(), baseUrl,
               &parsed);

    *record = parsed;
    return true;
}

} // namespace

LedgerFeature::LedgerFeature(OpenUrl openUrl) : m_openUrl(std::move(openUrl))
{
}

QUrl LedgerFeature::urlFor(const QUrl &baseUrl)
{
    QUrl url(baseUrl);
    if (!url.isValid() || url.host().isEmpty() ||
        (url.scheme() != QLatin1String("http") &&
         url.scheme() != QLatin1String("https")))
        return {};
    url.setUserName(QString());
    url.setPassword(QString());
    url.setPath(QStringLiteral("/ledger"));
    url.setQuery(QString());
    url.setFragment(QString());
    return url;
}

QUrl LedgerFeature::apiUrlFor(const QUrl &baseUrl, int limit, bool refresh)
{
    QUrl url = urlFor(baseUrl);
    if (!url.isValid())
        return {};
    url.setPath(QStringLiteral("/api/ledger"));
    QString query =
        QStringLiteral("limit=%1").arg(qBound(1, limit, kMaximumPageRecords));
    if (refresh)
        query += QStringLiteral("&refresh=1");
    url.setQuery(query);
    return url;
}

namespace {

// Public Solana addresses are base58 and 32-44 characters. An address that
// does not look like one is dropped rather than shown as an explorer link.
bool looksLikeSolanaAddress(const QString &address)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[1-9A-HJ-NP-Za-km-z]{32,44}$"));
    return pattern.match(address).hasMatch();
}

bool parseWallet(const QJsonObject &object, const QUrl &baseUrl,
                 LedgerWallet *wallet)
{
    const QString address =
        boundedText(object.value(QStringLiteral("address")), 64);
    if (!looksLikeSolanaAddress(address))
        return false;
    wallet->address = address;
    wallet->key = boundedText(object.value(QStringLiteral("key")), 40);
    wallet->label = boundedText(object.value(QStringLiteral("label")), 60);
    wallet->purpose = boundedText(object.value(QStringLiteral("purpose")), 200);
    wallet->network = boundedText(object.value(QStringLiteral("network")), 40);
    wallet->explorerUrl =
        safeLink(object.value(QStringLiteral("explorerUrl")), baseUrl);
    qint64 lamports = 0;
    if (exactInteger(object.value(QStringLiteral("balanceLamports")), &lamports))
        wallet->balanceLamports = quint64(lamports);
    qint64 checkedAt = 0;
    if (exactInteger(object.value(QStringLiteral("balanceCheckedAt")), &checkedAt))
        wallet->balanceCheckedAt = checkedAt;
    // A relay that has never managed a reading reports zero, which is not the
    // same claim as "this address holds nothing".
    wallet->balanceKnown =
        object.value(QStringLiteral("balanceKnown")).toBool() &&
        wallet->balanceCheckedAt > 0;
    wallet->sharedAddress =
        object.value(QStringLiteral("sharedAddress")).toBool();
    return !wallet->label.isEmpty();
}

} // namespace

bool LedgerFeature::parseLedgerPage(const QByteArray &body, const QUrl &baseUrl,
                                    LedgerPage *page, QString *error)
{
    if (!page)
        return false;
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = QStringLiteral("The ledger response was not readable.");
        return false;
    }
    const QJsonObject root = document.object();
    if (!root.value(QStringLiteral("ok")).toBool() ||
        !root.value(QStringLiteral("entries")).isArray()) {
        if (error)
            *error = QStringLiteral("The relay did not return ledger records.");
        return false;
    }

    LedgerPage parsed;
    parsed.network = boundedText(root.value(QStringLiteral("network")), 40);
    qint64 observedAt = 0;
    if (exactInteger(root.value(QStringLiteral("observedAt")), &observedAt))
        parsed.observedAt = observedAt;
    const QJsonObject quote = root.value(QStringLiteral("solUsd")).toObject();
    if (quote.value(QStringLiteral("available")).toBool())
        parsed.solUsdPrice =
            boundedText(quote.value(QStringLiteral("price")), 40);
    const QJsonObject pageInfo = root.value(QStringLiteral("page")).toObject();
    parsed.hasMore = pageInfo.value(QStringLiteral("hasMore")).toBool();

    const QJsonArray entries = root.value(QStringLiteral("entries")).toArray();
    for (const QJsonValue &value : entries) {
        if (parsed.records.size() >= kMaximumPageRecords)
            break;
        if (!value.isObject())
            continue;
        LedgerRecord record;
        if (!parseRecord(value.toObject(), baseUrl, &record))
            continue;
        if (record.currency == QLatin1String("SOL"))
            parsed.pageLamports += record.amountLamports;
        if (!record.transactionId.isEmpty())
            ++parsed.settledCount;
        parsed.records.append(record);
    }
    parsed.recordCount = parsed.records.size();

    const QJsonArray wallets = root.value(QStringLiteral("wallets")).toArray();
    for (const QJsonValue &value : wallets) {
        if (parsed.wallets.size() >= kMaximumPageWallets)
            break;
        if (!value.isObject())
            continue;
        LedgerWallet wallet;
        if (parseWallet(value.toObject(), baseUrl, &wallet))
            parsed.wallets.append(wallet);
    }

    *page = parsed;
    if (error)
        error->clear();
    return true;
}

QString LedgerFeature::solText(quint64 lamports)
{
    const quint64 whole = lamports / 1000000000ULL;
    const quint64 fraction = lamports % 1000000000ULL;
    if (fraction == 0)
        return QString::number(whole);
    QString digits = QStringLiteral("%1").arg(fraction, 9, 10, QLatin1Char('0'));
    while (digits.endsWith(QLatin1Char('0')))
        digits.chop(1);
    return QString::number(whole) + QLatin1Char('.') + digits;
}

QString LedgerFeature::balanceAgeText(qint64 checkedAt, qint64 nowMs)
{
    if (checkedAt <= 0)
        return QStringLiteral("never read");
    const qint64 minutes = (nowMs - checkedAt) / 60000;
    if (minutes < 1)
        return QStringLiteral("read just now");
    if (minutes < 60)
        return QStringLiteral("read %1 min ago").arg(minutes);
    const qint64 hours = minutes / 60;
    if (hours < 24) {
        return hours == 1 ? QStringLiteral("read 1 hour ago")
                          : QStringLiteral("read %1 hours ago").arg(hours);
    }
    const qint64 days = hours / 24;
    return days == 1 ? QStringLiteral("read 1 day ago")
                     : QStringLiteral("read %1 days ago").arg(days);
}

bool LedgerFeature::isPendingPayment(const LedgerRecord &record)
{
    // A record the reader entered here has no relay state machine behind it:
    // it is outstanding exactly as long as they say it is.
    if (record.local)
        return record.status == QLatin1String("outstanding");
    if (record.currency != QLatin1String("SOL") ||
        !record.transactionId.isEmpty())
        return false;
    // A record that already failed is finished, not pending: listing it as
    // awaiting payment would invite a transfer for work the relay wrote off.
    static const QStringList settledOrFailed{
        QStringLiteral("expired"),   QStringLiteral("rejected"),
        QStringLiteral("failed"),    QStringLiteral("cancelled"),
        QStringLiteral("distribution_failed"),
    };
    if (settledOrFailed.contains(record.status))
        return false;
    static const QStringList pending{
        QStringLiteral("pending_wallet"),  QStringLiteral("pending_merge"),
        QStringLiteral("pending_review"),  QStringLiteral("pending_signature"),
        QStringLiteral("prepared"),        QStringLiteral("approved"),
        QStringLiteral("queued"),          QStringLiteral("ready"),
        QStringLiteral("submitted"),       QStringLiteral("awaiting_finality"),
        QStringLiteral("awaiting_distribution"),
    };
    return pending.contains(record.status);
}

QString LedgerFeature::statusText(const QString &status)
{
    const QString clean = status.trimmed();
    if (clean.isEmpty())
        return QStringLiteral("recorded");
    if (clean == QLatin1String("pending_merge"))
        return QStringLiteral("pays on merge");
    if (clean == QLatin1String("pending_wallet"))
        return QStringLiteral("awaiting the recipient's wallet");
    if (clean == QLatin1String("ready"))
        return QStringLiteral("ready to pay");
    if (clean == QLatin1String("submitted"))
        return QStringLiteral("payment submitted");
    if (clean == QLatin1String("awaiting_finality"))
        return QStringLiteral("awaiting finality");
    if (clean == QLatin1String("awaiting_distribution"))
        return QStringLiteral("awaiting distribution");
    if (clean == QLatin1String("reported-on-chain"))
        return QStringLiteral("reported on-chain");
    QString relaxed = clean;
    return relaxed.replace(QLatin1Char('_'), QLatin1Char(' '));
}

bool LedgerFeature::isPaidExpense(const LedgerRecord &record)
{
    // A record the reader entered here has no chain behind it and never
    // carries a transaction: it is paid exactly when they say it is.
    if (record.local)
        return record.status != QLatin1String("outstanding");
    // Everything the relay publishes is paid by one transaction, and the
    // relay counts its own settled records the same way: a signature, or it
    // did not happen.
    return !record.transactionId.isEmpty();
}

LedgerFlow LedgerFeature::flowOf(const LedgerRecord &record)
{
    const QString direction = record.direction.trimmed().toLower();
    if (direction == QLatin1String("inbound") ||
        direction == QLatin1String("income"))
        return LedgerFlow::Income;
    if (direction == QLatin1String("outbound") ||
        direction == QLatin1String("allocation") ||
        direction == QLatin1String("expense")) {
        // An allocation is money the relay has committed and not yet moved,
        // and a reward that is ready to pay is a promise until someone pays
        // it. Money that has not left is not spent: counting it as an expense
        // charges the book twice, once when it is promised and again when the
        // transaction that settles it appears. It is owed until then, which
        // this page already reports as awaiting payment.
        return isPaidExpense(record) ? LedgerFlow::Expense
                                     : LedgerFlow::Committed;
    }
    return LedgerFlow::Neither;
}

bool LedgerFeature::parseDecimalAmount(const QString &text, int decimals,
                                       qint64 *minorUnits)
{
    if (!minorUnits || decimals < 0 || decimals > 9)
        return false;
    const QString clean = text.trimmed();
    if (clean.isEmpty() || clean.size() > 32)
        return false;
    // A plain non-negative decimal and nothing else: no sign, no separators,
    // no exponent. A figure this page cannot read exactly is refused rather
    // than rounded into the reader's own books.
    static const QRegularExpression pattern(
        QStringLiteral("^([0-9]{0,15})(?:\\.([0-9]{1,9}))?$"));
    const QRegularExpressionMatch match = pattern.match(clean);
    if (!match.hasMatch())
        return false;
    const QString wholeText = match.captured(1);
    QString fraction = match.captured(2);
    if (wholeText.isEmpty() && fraction.isEmpty())
        return false;
    if (fraction.size() > decimals)
        return false; // more precision than the unit itself holds
    fraction = fraction.leftJustified(decimals, QLatin1Char('0'));

    qint64 scale = 1;
    for (int place = 0; place < decimals; ++place)
        scale *= 10;
    const qint64 whole = wholeText.isEmpty() ? 0 : wholeText.toLongLong();
    if (whole > qint64(kMaximumExactInteger) / scale)
        return false;
    const qint64 minor =
        whole * scale + (fraction.isEmpty() ? 0 : fraction.toLongLong());
    if (double(minor) > kMaximumExactInteger)
        return false;
    *minorUnits = minor;
    return true;
}

qint64 LedgerFeature::usdMicros(const QString &amountUsd)
{
    qint64 micros = 0;
    if (!parseDecimalAmount(amountUsd, 6, &micros))
        return -1;
    return micros;
}

QString LedgerFeature::usdText(qint64 micros)
{
    const bool negative = micros < 0;
    const qint64 absolute = negative ? -micros : micros;
    const qint64 cents = (absolute + 5000) / 10000; // to the nearest cent
    const QString text = QStringLiteral("%1.%2")
                             .arg(cents / 100)
                             .arg(cents % 100, 2, 10, QLatin1Char('0'));
    return negative ? QLatin1Char('-') + text : text;
}

qint64 LedgerFeature::usdMicrosForLamports(quint64 lamports,
                                           const QString &solUsdPrice)
{
    const qint64 priceMicros = usdMicros(solUsdPrice);
    if (priceMicros < 0)
        return -1;
    // The product overflows 64 bits above ~64 SOL at a mainnet price, and a
    // spot estimate is not exact accounting to begin with, so this one figure
    // is computed in floating point and rounded back to whole micros.
    const double micros = double(lamports) * double(priceMicros) / 1e9;
    if (!std::isfinite(micros) || micros < 0 || micros > kMaximumExactInteger)
        return -1;
    return qint64(micros + 0.5);
}

LedgerTotals LedgerFeature::totalsFor(const LedgerPage &page)
{
    LedgerTotals totals;
    for (const LedgerRecord &record : page.records) {
        const bool isSol = record.currency == QLatin1String("SOL");
        // An undisclosed amount is not zero. Such a record is still one of
        // the page's records, so it is counted, but it adds up to nothing.
        const quint64 lamports =
            isSol && record.amountKnown ? record.amountLamports : 0;
        qint64 usd = record.amountKnown ? usdMicros(record.amountUsd) : -1;
        if (usd < 0 && lamports > 0)
            usd = usdMicrosForLamports(lamports, page.solUsdPrice);
        if (usd < 0)
            usd = 0;
        switch (flowOf(record)) {
        case LedgerFlow::Income:
            totals.incomeLamports += lamports;
            totals.incomeUsdMicros += usd;
            ++totals.incomeCount;
            break;
        case LedgerFlow::Expense:
            totals.expenseLamports += lamports;
            totals.expenseUsdMicros += usd;
            ++totals.expenseCount;
            break;
        // Money out that has not moved is owed, not spent. It is counted
        // below as awaiting payment instead, so the page reports it once.
        case LedgerFlow::Committed:
        case LedgerFlow::Neither:
            break;
        }
        if (!isPendingPayment(record))
            continue;
        totals.pendingLamports += lamports;
        totals.pendingUsdMicros += usd;
        ++totals.pendingCount;
    }
    return totals;
}

LedgerRecord LedgerFeature::localRecord(
    const QString &id, const QString &direction, const QString &status,
    const QString &category, const QString &description,
    const QString &counterparty, const QString &currency, qint64 amount,
    qint64 occurredAt)
{
    const QString flow = direction.trimmed().toLower();
    const QString money = currency.trimmed().toUpper();
    const QString text = description.trimmed().left(160);
    QString key = id.trimmed().left(kMaximumIdentifierLength);
    if (key.startsWith(QLatin1String("local:")))
        key = key.mid(6);
    if (key.isEmpty() || text.isEmpty() || amount <= 0 ||
        double(amount) > kMaximumExactInteger || occurredAt < 0 ||
        (flow != QLatin1String("income") && flow != QLatin1String("expense")) ||
        (money != QLatin1String("SOL") && money != QLatin1String("USD")))
        return {};

    LedgerRecord record;
    record.id = QStringLiteral("local:") + key;
    record.local = true;
    record.category = QStringLiteral("local-record");
    record.direction = flow;
    record.status = status.trimmed() == QLatin1String("outstanding")
                        ? QStringLiteral("outstanding")
                        : QStringLiteral("settled");
    record.verification = QStringLiteral("entered-on-this-machine");
    record.sourceTable = QStringLiteral("local");
    record.sourceId = key;
    // The Record column reads "<what kind> · your record", so a reader can
    // tell their own book from the relay's at a glance.
    const QString kind = category.trimmed().left(80);
    record.title = kind.isEmpty() ? (flow == QLatin1String("income")
                                         ? QStringLiteral("Income")
                                         : QStringLiteral("Expense"))
                                  : kind;
    record.subjectLabel = QStringLiteral("your record");
    record.subjectTitle = text;
    QString party = counterparty.trimmed().left(80);
    if (party.startsWith(QLatin1Char('@')))
        party.remove(0, 1);
    record.counterpartyName = party;
    record.currency = money;
    record.occurredAt = occurredAt;
    if (money == QLatin1String("SOL"))
        record.amountLamports = quint64(amount);
    else
        record.amountUsd = usdText(amount);
    return record;
}

QByteArray LedgerFeature::serializeLocalRecords(
    const QList<LedgerRecord> &records)
{
    QJsonArray entries;
    for (const LedgerRecord &record : records) {
        if (!record.local || record.sourceId.isEmpty())
            continue;
        if (entries.size() >= kMaximumLocalRecords)
            break;
        entries.append(QJsonObject{
            {QStringLiteral("id"), record.sourceId},
            {QStringLiteral("direction"), record.direction},
            {QStringLiteral("status"), record.status},
            {QStringLiteral("category"), record.title},
            {QStringLiteral("description"), record.subjectTitle},
            {QStringLiteral("counterparty"), record.counterpartyName},
            {QStringLiteral("currency"), record.currency},
            {QStringLiteral("amountLamports"), double(record.amountLamports)},
            {QStringLiteral("amountUsd"), record.amountUsd},
            {QStringLiteral("occurredAt"), double(record.occurredAt)},
        });
    }
    return QJsonDocument(entries).toJson(QJsonDocument::Compact);
}

QList<LedgerRecord> LedgerFeature::parseLocalRecords(const QByteArray &body)
{
    QList<LedgerRecord> records;
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (!document.isArray())
        return records;
    for (const QJsonValue &value : document.array()) {
        if (records.size() >= kMaximumLocalRecords)
            break;
        if (!value.isObject())
            continue;
        // Stored on disk, so still untrusted: rebuild every record through
        // the same constructor a typed one goes through.
        const QJsonObject object = value.toObject();
        const QString currency =
            boundedText(object.value(QStringLiteral("currency")), 8).toUpper();
        qint64 amount = 0;
        if (currency == QLatin1String("SOL")) {
            if (!exactInteger(object.value(QStringLiteral("amountLamports")),
                              &amount))
                continue;
        } else {
            amount = usdMicros(
                boundedText(object.value(QStringLiteral("amountUsd")), 40));
        }
        qint64 occurredAt = 0;
        if (!exactInteger(object.value(QStringLiteral("occurredAt")),
                          &occurredAt))
            continue;
        const LedgerRecord record = localRecord(
            boundedText(object.value(QStringLiteral("id")),
                        kMaximumIdentifierLength),
            boundedText(object.value(QStringLiteral("direction")), 40),
            boundedText(object.value(QStringLiteral("status")), 40),
            boundedText(object.value(QStringLiteral("category")), 80),
            boundedText(object.value(QStringLiteral("description")), 160),
            boundedText(object.value(QStringLiteral("counterparty")), 80),
            currency, amount, occurredAt);
        if (!record.id.isEmpty())
            records.append(record);
    }
    return records;
}

void LedgerFeature::applyLocalRecords(LedgerPage *page,
                                      const QList<LedgerRecord> &records)
{
    if (!page)
        return;
    // A projection, not an accumulation: whatever the page carries from an
    // earlier pass is dropped and replaced by the current book.
    page->records.removeIf(
        [](const LedgerRecord &record) { return record.local; });
    for (LedgerRecord record : records) {
        if (!record.local || record.id.isEmpty())
            continue;
        // The relay prices its own records; the reader's SOL entries are
        // priced here, at the same page quote the rows around them used.
        if (record.currency == QLatin1String("SOL") &&
            record.amountUsd.isEmpty()) {
            const qint64 micros = usdMicrosForLamports(record.amountLamports,
                                                       page->solUsdPrice);
            if (micros >= 0)
                record.amountUsd = usdText(micros);
        }
        page->records.append(record);
    }
    std::stable_sort(page->records.begin(), page->records.end(),
                     [](const LedgerRecord &left, const LedgerRecord &right) {
                         if (left.occurredAt != right.occurredAt)
                             return left.occurredAt > right.occurredAt;
                         // Match the relay/browser cursor order so equal-time
                         // rows never reshuffle when a local record is merged.
                         return left.id > right.id;
                     });
    page->recordCount = page->records.size();
    page->settledCount = 0;
    page->pageLamports = 0;
    for (const LedgerRecord &record : page->records) {
        if (!record.transactionId.isEmpty())
            ++page->settledCount;
        if (record.currency == QLatin1String("SOL"))
            page->pageLamports += record.amountLamports;
    }
}

LedgerRecord LedgerFeature::payableRecord(
    const QString &rewardId, const QString &recipientName,
    const QString &recipientAddress, const QString &repository,
    const QString &sourceKind, int subjectNumber, quint64 lamports)
{
    LedgerRecord record;
    record.id = QStringLiteral("contributor-reward:") + rewardId;
    record.category = QStringLiteral("contributor-reward");
    record.sourceId = rewardId;
    record.rewardId = rewardId;
    record.counterpartyName = recipientName;
    record.recipientAddress = recipientAddress;
    record.repository = repository;
    record.sourceKind = sourceKind;
    record.subjectNumber = subjectNumber;
    record.amountLamports = lamports;
    record.currency = QStringLiteral("SOL");
    record.payable = true;
    return record;
}

int LedgerFeature::applyPayableRewards(LedgerPage *page,
                                       const QList<LedgerRecord> &payable)
{
    if (!page)
        return 0;
    int applied = 0;
    for (LedgerRecord &record : page->records) {
        // A projection, not an accumulation: a record that was payable under
        // an earlier plan must stop offering a button the moment the relay
        // stops publishing a payout for it.
        record.payable = false;
        record.rewardId.clear();
        record.recipientAddress.clear();
        record.repository.clear();
        record.sourceKind.clear();
        record.subjectNumber = 0;
        if (record.category != QLatin1String("contributor-reward") ||
            record.sourceId.isEmpty() || !record.transactionId.isEmpty())
            continue;
        for (const LedgerRecord &plan : payable) {
            if (plan.rewardId != record.sourceId)
                continue;
            // The relay's two projections of the same estimate must agree on
            // the exact figure before a Pay button appears: the ledger row is
            // what the owner reads, and the plan is what would be signed.
            if (plan.amountLamports != record.amountLamports)
                break;
            record.rewardId = plan.rewardId;
            record.recipientAddress = plan.recipientAddress;
            record.repository = plan.repository;
            record.sourceKind = plan.sourceKind;
            record.subjectNumber = plan.subjectNumber;
            if (record.counterpartyName.isEmpty())
                record.counterpartyName = plan.counterpartyName;
            record.payable = true;
            ++applied;
            break;
        }
    }
    return applied;
}

bool LedgerFeature::open(const QUrl &baseUrl) const
{
    const QUrl url = urlFor(baseUrl);
    return url.isValid() && m_openUrl && m_openUrl(url);
}

} // namespace forkmesh::ledger
