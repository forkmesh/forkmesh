// The Ledger page. The installed Ledger package projects the relay's public
// financial system of record (GET /api/ledger) into a table here rather than
// bouncing the reader out to a browser, and lets a repository owner settle a
// pending contributor reward without leaving the app.
//
// Paying does not introduce a second money path: the row hands the exact
// figures to the audited member-tip signer, which builds one System Program
// transfer, signs it with the wallet linked to this desktop, broadcasts it,
// and only then reports the public signature back. The relay flips its own
// estimate to paid after verifying that exact finalized transfer, and this
// file writes the same signature onto the issue or pull it paid for, so the
// public ledger and the repository record agree on one receipt.

#include "MainWindow.h"
#include "MessageBoxClicks.h"

#include "ContributorPayoutReceipt.h"
#include "KebabHeaderView.h"
#include "LedgerFeature.h"
#include "MainWindowInternal.h"
#include "RewardPoolSigner.h"
#include "TipSigner.h"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCompleter>
#include <QCoreApplication>
#include <QDate>
#include <QDateEdit>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMetaType>
#include <QModelIndex>
#include <QMouseEvent>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QSettings>
#include <QStringListModel>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>
#include <QToolTip>
#include <QUrlQuery>
#include <QUuid>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidgetAction>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

using forkmesh::ledger::LedgerFeature;
using forkmesh::ledger::LedgerFlow;
using forkmesh::ledger::LedgerPage;
using forkmesh::ledger::LedgerRecord;
using forkmesh::ledger::LedgerTotals;
using forkmesh::ledger::LedgerWallet;
using namespace forkmesh::ui;

namespace {

constexpr int kLedgerPageSize = 60;
constexpr int kLedgerMaximumResponse = 1024 * 1024;
// Re-reading the whole page on every visit is wasted relay traffic; the ledger
// is a slow-moving public record and the Refresh button is always there.
constexpr qint64 kLedgerFreshMs = 60 * 1000;
// Fixed, so it has to fit the longest label the column ever shows with its
// icon beside it ("Reminder sent").
constexpr int kLedgerActionColumnWidth = 170;
// The held-payout badge sits left of the Pay button in a column that is fixed
// precisely because ResizeToContents cannot see cell widgets. Nothing would
// grow to fit it, so the column is given the room explicitly while it shows.
constexpr int kLedgerHeldBadgeWidth = 26;
// The value a cell means, kept beside the text it shows. See LedgerCell.
constexpr int kLedgerSortKeyRole = Qt::UserRole + 8;
// A payment started from the row temporarily replaces the relay's status with
// "Processing payment…". Keep the canonical text on the item so a cancellation
// or failed signing attempt can restore it without waiting on a page reload.
constexpr int kLedgerStatusTextRole = Qt::UserRole + 9;
constexpr int kLedgerStatusTooltipRole = Qt::UserRole + 10;

// A payable row's own transfer running, one approved behind whatever holds
// the wallet, or neither. Drawn here for both the full rebuild and the
// in-place refresh, so the two never disagree about a row.
void applyLedgerRowPaymentState(QWidget *cell, QTableWidgetItem *status,
                                bool processing, bool queued, bool payEnabled)
{
    const QString processingTip = QStringLiteral(
        "The wallet is unlocked. This payment is being signed, broadcast, and "
        "reconciled.");
    const QString queuedTip = QStringLiteral(
        "Approved. It is signed and sent as soon as the payment in progress "
        "finishes.");
    if (cell) {
        if (auto *progress = cell->findChild<QWidget *>(
                QStringLiteral("ledgerPaymentProgress")))
            progress->setVisible(processing || queued);
        if (auto *spinner = cell->findChild<QWidget *>(
                QStringLiteral("ledgerPaymentSpinner")))
            spinner->setVisible(processing);
        if (auto *label = cell->findChild<QLabel *>(
                QStringLiteral("ledgerPaymentProgressLabel"))) {
            label->setText(processing
                               ? QString::fromUtf8("Processing\xE2\x80\xA6")
                               : QStringLiteral("Queued"));
            label->setToolTip(processing ? processingTip : queuedTip);
        }
        if (auto *pay = cell->findChild<QPushButton *>(
                QStringLiteral("ledgerPayButton"))) {
            pay->setVisible(!processing && !queued);
            pay->setEnabled(payEnabled);
        }
    }
    if (!status)
        return;
    if (processing) {
        status->setText(QString::fromUtf8("Processing payment\xE2\x80\xA6"));
        status->setToolTip(processingTip);
    } else if (queued) {
        status->setText(QStringLiteral("Queued for payment"));
        status->setToolTip(queuedTip);
    } else {
        status->setText(status->data(kLedgerStatusTextRole).toString());
        status->setToolTip(status->data(kLedgerStatusTooltipRole).toString());
    }
}
// Where a cell leads when it is clicked: the Transaction column carries the
// transfer's own page on a public Solana explorer, and the When column carries
// the issue or pull the money is about, for the whole row to share.
constexpr int kLedgerLinkUrlRole = Qt::UserRole + 1;
// The relay serves a wallet balance from an hourly cache, so asking again
// inside that window costs a request and returns the same number.
constexpr qint64 kLedgerWalletBalanceTtlMs = 60 * 60 * 1000;
constexpr char kLedgerSolUsdPriceEndpoint[] =
    "https://api.coingecko.com/api/v3/simple/price"
    "?ids=solana&vs_currencies=usd";
// Where the reader's own records live between runs. The relay's ledger is a
// public record; this is one machine's private book beside it. Deliberately
// not under the Ledger package's own data prefix: removing a view of the
// financial records must not delete a year of somebody's expenses with it.
QString ledgerLocalRecordsKey()
{
    return QStringLiteral("ledger/localRecords");
}

enum LedgerColumn {
    kLedgerWhenColumn = 0,
    kLedgerRecordColumn,
    kLedgerTitleColumn,
    kLedgerCounterpartyColumn,
    kLedgerAmountColumn,
    kLedgerStatusColumn,
    kLedgerTransactionColumn,
    kLedgerActionColumn,
    kLedgerColumnCount,
};

// Sorting a money table by its rendered text is wrong in ways a reader will
// trust anyway: "0.5 SOL" lands under "0.05 SOL", and an em dash sorts as a
// date. Cells that stand for a value carry that value in kLedgerSortKeyRole
// and are compared on it; the rest fall back to Qt's own text ordering.
class LedgerCell : public QTableWidgetItem {
public:
    explicit LedgerCell(const QString &text = QString())
        : QTableWidgetItem(text)
    {
    }

    bool operator<(const QTableWidgetItem &other) const override
    {
        const QVariant mine = data(kLedgerSortKeyRole);
        const QVariant theirs = other.data(kLedgerSortKeyRole);
        if (!mine.isValid() || !theirs.isValid())
            return QTableWidgetItem::operator<(other);
        if (mine.typeId() == QMetaType::QString ||
            theirs.typeId() == QMetaType::QString) {
            return QString::compare(mine.toString(), theirs.toString(),
                                    Qt::CaseInsensitive) < 0;
        }
        return mine.toDouble() < theirs.toDouble();
    }
};

QTableWidgetItem *ledgerCell(const QString &text = QString(),
                             const QVariant &sortKey = QVariant())
{
    auto *item = new LedgerCell(text);
    if (sortKey.isValid())
        item->setData(kLedgerSortKeyRole, sortKey);
    return item;
}

void hardenLedgerRequest(QNetworkRequest &request)
{
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Cache-Control", "no-store");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::SameOriginRedirectPolicy);
    request.setTransferTimeout(15000);
}

QByteArray boundedLedgerBody(QNetworkReply *reply)
{
    if (!reply)
        return {};
    const QByteArray body = reply->read(kLedgerMaximumResponse + 1);
    return body.size() <= kLedgerMaximumResponse ? body : QByteArray();
}

QLabel *ledgerHint(const QString &text)
{
    auto *label = new QLabel(text);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    return label;
}

// One wallet on the strip above the table, in the order a reader needs them:
// the two this machine knows about first (their state is the one a payment
// depends on), then the public system wallets the relay names.
struct LedgerWalletCard {
    QString label;
    QString purpose;
    QString address;
    QUrl explorerUrl;
    QString emptyText;     // shown instead of an amount when there is no address
    QString emptyPurpose;  // and what to do about it
    qint64 lamports = -1;  // < 0 = no reading yet
    qint64 usdMicros = -1; // < 0 = nothing to price the balance with
    qint64 checkedAt = 0;
    bool sharedAddress = false;
    bool signsPayments = false; // an empty one is why a payout is refused
    std::function<void()> onRemove;  // set only for a reader-entered address
    std::function<void()> onRefresh; // re-read this one balance
    QString setUpText;
    QString setUpIcon;
    std::function<void()> onSetUp;
};

QString ledgerAbbreviatedAddress(const QString &address)
{
    if (address.size() <= 18)
        return address;
    return address.left(7) + QString::fromUtf8("\xE2\x80\xA6") +
           address.right(7);
}

QWidget *makeLedgerWalletCard(const LedgerWalletCard &wallet)
{
    auto *card = new QFrame;
    card->setObjectName(QStringLiteral("ledgerWalletCard"));
    card->setFrameShape(QFrame::NoFrame);
    card->setMinimumWidth(210);
    auto *layout = new QHBoxLayout(card);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(8);

    auto *details = new QWidget(card);
    auto *detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    detailsLayout->setSpacing(2);

    const bool linked = !wallet.address.isEmpty();
    const bool unfunded =
        linked && wallet.signsPayments && wallet.lamports == 0;

    // Keep the status beside the wallet name so a linked card's footer no
    // longer creates a second full-height row beneath the QR code.
    QString statusText;
    if (linked && wallet.lamports >= 0) {
        statusText = LedgerFeature::balanceAgeText(
            wallet.checkedAt, QDateTime::currentMSecsSinceEpoch());
        if (wallet.sharedAddress)
            statusText += QStringLiteral(" · shared address");
    } else if (linked) {
        statusText = QStringLiteral("not read yet");
    }

    // An address that signs payments and holds nothing is the whole reason a
    // signed payout comes back refused, so say that rather than "0 SOL".
    QString noteText;
    if (!linked) {
        noteText = wallet.emptyPurpose;
    } else if (unfunded) {
        noteText = QStringLiteral(
            "Empty. Solana refuses a transfer signed from an unfunded account "
            "before it ever becomes a transaction.");
    } else if (wallet.lamports < 0) {
        noteText = wallet.purpose;
    }

    auto *headerRow = new QHBoxLayout;
    headerRow->setSpacing(4);
    auto *label = new QLabel(wallet.label.toUpper());
    label->setObjectName(QStringLiteral("ledgerWalletLabel"));
    headerRow->addWidget(label, 1);
    if (!statusText.isEmpty()) {
        auto *status = new QLabel(statusText);
        status->setObjectName(QStringLiteral("ledgerWalletAge"));
        status->setTextFormat(Qt::PlainText);
        status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        status->setToolTip(wallet.purpose);
        headerRow->addWidget(status);
    }
    // Only a reader-typed address gets a way to take it back; the two this
    // machine names itself and whatever the relay publishes are not this
    // card's to remove.
    if (wallet.onRemove) {
        auto *removeButton = new QPushButton;
        removeButton->setObjectName(QStringLiteral("ghostButton"));
        setOcticon(removeButton, QStringLiteral("x"), 12);
        removeButton->setCursor(Qt::PointingHandCursor);
        removeButton->setToolTip(QStringLiteral("Stop tracking this address"));
        removeButton->setFocusPolicy(Qt::NoFocus);
        QObject::connect(removeButton, &QPushButton::clicked, removeButton,
                         wallet.onRemove);
        headerRow->addWidget(removeButton);
    }
    detailsLayout->addLayout(headerRow);

    auto *amount = new QLabel(
        !linked ? wallet.emptyText
                : wallet.lamports < 0
                      ? QStringLiteral("Not read yet")
                      : LedgerFeature::solText(quint64(wallet.lamports)) +
                            QStringLiteral(" SOL"));
    amount->setObjectName(QStringLiteral("ledgerWalletAmount"));
    amount->setTextFormat(Qt::PlainText);
    auto *amountRow = new QHBoxLayout;
    amountRow->setSpacing(6);
    amountRow->addWidget(amount);

    // What that balance is worth, beside the exact figure rather than instead
    // of it: the SOL amount is what the chain holds, and this is one live
    // spot estimate of it, at the same quote the records below were priced at.
    if (linked && wallet.usdMicros >= 0) {
        auto *usd = new QLabel(QString::fromUtf8("\xE2\x89\x88 $") +
                               LedgerFeature::usdText(wallet.usdMicros));
        usd->setObjectName(QStringLiteral("ledgerWalletUsd"));
        usd->setTextFormat(Qt::PlainText);
        usd->setToolTip(QStringLiteral(
            "A live spot estimate at the price this page was read with, not "
            "transaction-time accounting."));
        amountRow->addWidget(usd);
    }
    // Every balance on this strip comes from a shared cache the relay only
    // re-reads hourly, so the card that shows how old a reading is also
    // carries the way to take a new one.
    if (linked && wallet.onRefresh) {
        auto *refresh = new QPushButton;
        refresh->setObjectName(QStringLiteral("ledgerWalletRefresh"));
        setOcticon(refresh, QStringLiteral("sync"), 11);
        refresh->setCursor(Qt::PointingHandCursor);
        refresh->setFocusPolicy(Qt::NoFocus);
        refresh->setFixedSize(22, 22);
        refresh->setToolTip(QStringLiteral("Read this balance again"));
        QObject::connect(refresh, &QPushButton::clicked, refresh,
                         wallet.onRefresh);
        amountRow->addWidget(refresh);
    }
    amountRow->addStretch();
    detailsLayout->addLayout(amountRow);

    QLabel *qrLabel = nullptr;
    if (linked) {
        auto *address = new QLabel;
        address->setObjectName(QStringLiteral("ledgerWalletAddress"));
        address->setTextFormat(Qt::RichText);
        address->setOpenExternalLinks(true);
        address->setTextInteractionFlags(Qt::TextBrowserInteraction);
        const QString shown =
            ledgerAbbreviatedAddress(wallet.address).toHtmlEscaped();
        // Rich text takes its anchor colour from the palette, not from the
        // object-name rule this label otherwise follows, so the address would
        // land as system link blue on a card built out of muted greys.
        address->setText(
            wallet.explorerUrl.isValid()
                ? QStringLiteral("<a href=\"%1\" style=\"color:%2;\">%3</a>")
                      .arg(wallet.explorerUrl.toString(QUrl::FullyEncoded)
                               .toHtmlEscaped(),
                           currentThemeIsDark() ? QStringLiteral("#8b949e")
                                                : QStringLiteral("#656d76"),
                           shown)
                : shown);
        address->setToolTip(wallet.address);
        detailsLayout->addWidget(address);

        // A scannable funding target beats a copy-pasted address: this is the
        // same "solana:<address>" URI a wallet app's own share sheet produces,
        // so any scanner that already understands Solana pay links accepts it.
        // Scale two keeps the code comfortably scannable while reserving only
        // a small corner of this compact card.
        const QImage qr = QrCode::encodeToImage(
            QStringLiteral("solana:%1").arg(wallet.address), 2, 2);
        if (!qr.isNull()) {
            qrLabel = new QLabel;
            qrLabel->setObjectName(QStringLiteral("ledgerWalletQr"));
            qrLabel->setPixmap(QPixmap::fromImage(qr));
            qrLabel->setAlignment(Qt::AlignCenter);
            qrLabel->setToolTip(
                QStringLiteral("Scan with a Solana wallet app to send SOL "
                               "to this address"));
        }
    }

    if (!noteText.isEmpty()) {
        auto *note = new QLabel(noteText);
        note->setObjectName(unfunded ? QStringLiteral("ledgerWalletWarning")
                                     : QStringLiteral("ledgerWalletNote"));
        note->setWordWrap(true);
        note->setToolTip(wallet.purpose);
        detailsLayout->addWidget(note);
    }
    layout->addWidget(details, 1);
    if (qrLabel)
        layout->addWidget(qrLabel, 0, Qt::AlignTop);
    return card;
}

QString ledgerWhenText(qint64 occurredAt)
{
    if (occurredAt <= 0)
        return QString::fromUtf8("\xE2\x80\x94");
    return QDateTime::fromMSecsSinceEpoch(occurredAt)
        .toLocalTime()
        .toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

// The relay reports USD to six decimal places because that is how it stores
// the quote. Money on screen is two.
QString ledgerUsdText(const QString &amountUsd)
{
    const int point = amountUsd.indexOf(QLatin1Char('.'));
    if (point < 0)
        return amountUsd + QStringLiteral(".00");
    return amountUsd.left(point + 3);
}

QString ledgerAmountText(const LedgerRecord &record)
{
    if (!record.amountKnown)
        return QStringLiteral("undisclosed");
    QString text;
    if (record.currency == QLatin1String("SOL")) {
        const QString sol =
            record.amountSol.isEmpty()
                ? LedgerFeature::solText(record.amountLamports)
                : record.amountSol;
        text = sol + QStringLiteral(" SOL");
        if (!record.amountUsd.isEmpty()) {
            if (record.valuationBasis ==
                QLatin1String("recorded-at-payment-sol-usd")) {
                text += QStringLiteral(" ($") + ledgerUsdText(record.amountUsd) +
                        QStringLiteral(" at send)");
            } else {
                text += QStringLiteral(" (~$") + ledgerUsdText(record.amountUsd) +
                        QStringLiteral(")");
            }
        }
    } else if (!record.amountUsd.isEmpty()) {
        text = QStringLiteral("$") + ledgerUsdText(record.amountUsd);
    } else {
        return QString::fromUtf8("\xE2\x80\x94");
    }
    // A book that mixes money in with money out is read by its signs. A
    // transfer between two other people is neither, and carries none. Money
    // that is owed and has not moved carries none either: it is not spent
    // until the transaction that pays it exists, and the Status column beside
    // it already says what it is waiting on.
    switch (LedgerFeature::flowOf(record)) {
    case LedgerFlow::Income:
        return QStringLiteral("+") + text;
    case LedgerFlow::Expense:
        return QString::fromUtf8("\xE2\x88\x92") + text;
    case LedgerFlow::Committed:
    case LedgerFlow::Neither:
        break;
    }
    return text;
}

QString ledgerDiscordIdentity(const LedgerRecord &record)
{
    const QString username = record.counterpartyDiscordUsername.trimmed();
    const QString displayName =
        record.counterpartyDiscordDisplayName.trimmed();
    if (!username.isEmpty() && !displayName.isEmpty()) {
        return QStringLiteral("Discord @%1 (%2)").arg(username, displayName);
    }
    if (!username.isEmpty())
        return QStringLiteral("Discord @%1").arg(username);
    return displayName.isEmpty() ? QString()
                                 : QStringLiteral("Discord %1").arg(displayName);
}

QString ledgerCounterpartyText(const LedgerRecord &record)
{
    if (record.counterpartyName.isEmpty())
        return QString::fromUtf8("\xE2\x80\x94");
    const QString discord = ledgerDiscordIdentity(record);
    return QLatin1Char('@') + record.counterpartyName +
           (discord.isEmpty() ? QString()
                              : QString::fromUtf8(" \xC2\xB7 ") + discord);
}

// A total that mixes exact SOL with fiat, in the order a reader reads it:
// what the chain moved, then what it is worth.
QString ledgerTotalText(quint64 lamports, qint64 usdMicros)
{
    QStringList parts;
    if (lamports > 0)
        parts << LedgerFeature::solText(lamports) + QStringLiteral(" SOL");
    if (usdMicros > 0) {
        parts << (lamports > 0 ? QString::fromUtf8("\xE2\x89\x88 $")
                               : QStringLiteral("$")) +
                     LedgerFeature::usdText(usdMicros);
    }
    return parts.isEmpty() ? QStringLiteral("0 SOL")
                           : parts.join(QLatin1Char(' '));
}

QString ledgerTransactionText(const QString &signature)
{
    if (signature.isEmpty())
        return QString::fromUtf8("\xE2\x80\x94");
    if (signature.size() <= 18)
        return signature;
    return signature.left(8) + QString::fromUtf8("\xE2\x80\xA6") +
           signature.right(8);
}

// Where a signature can be looked up, spelled the way the relay spells it:
// mainnet is the explorer's default and names no cluster, and anything else
// names itself. Used only when the relay published no link of its own, which
// is every transfer it has not been told about yet.
QUrl ledgerExplorerUrl(const QString &signature, const QString &network)
{
    if (signature.isEmpty())
        return {};
    QUrl url(QStringLiteral("https://explorer.solana.com"));
    url.setPath(QStringLiteral("/tx/") + signature);
    const QString cluster = network.trimmed().toLower();
    if (cluster == QLatin1String("devnet") ||
        cluster == QLatin1String("testnet")) {
        url.setQuery(QStringLiteral("cluster=") + cluster);
    }
    return url;
}

// What "biggest first" means on a page that mixes SOL with fiat. The relay
// quotes one spot price per page, so either every SOL row carries a USD
// estimate or none does: within a page the key is a single scale either way.
// An undisclosed amount is not zero, and sorts below every figure rather than
// among the small ones.
double ledgerAmountSortKey(const LedgerRecord &record)
{
    if (!record.amountKnown)
        return -1.0;
    if (!record.amountUsd.isEmpty())
        return record.amountUsd.toDouble();
    if (record.currency == QLatin1String("SOL"))
        return double(record.amountLamports) / 1000000000.0;
    return 0.0;
}

// A reward whose recipient published no payout address is the one pending
// state nobody on this side can clear: the estimate is approved, the money is
// ready, and it waits on a profile field only the contributor can fill in.
bool ledgerAwaitsRecipientWallet(const LedgerRecord &record)
{
    return record.category == QLatin1String("contributor-reward") &&
           record.status == QLatin1String("pending_wallet") &&
           record.transactionId.isEmpty() && !record.sourceId.isEmpty() &&
           !record.counterpartyName.isEmpty();
}

// The relay's per-contributor reminder window (ledger.py
// REWARD_WALLET_REMINDER_INTERVAL_MS). The relay stays the authority; this
// only decides whether the button offers a request the relay would refuse.
constexpr qint64 kLedgerReminderIntervalMs = 3LL * 24 * 60 * 60 * 1000;

// "Sep 14, 2026, 2:05 PM" in local time. Fixed English month names, because
// every sentence around the time is English.
QString ledgerReminderTimeText(qint64 epochMs)
{
    return QLocale::c().toString(QDateTime::fromMSecsSinceEpoch(epochMs),
                                 QStringLiteral("MMM d, yyyy, h:mm AP"));
}

// The relay's owner-authored payout plan, reduced to the records this desktop
// would be willing to sign. parseContributorPayout accepts only a ready
// estimate with an exact in-range amount and a valid destination address, so
// anything it rejects simply never becomes payable.
QList<LedgerRecord> parsePayableRewards(const QByteArray &body)
{
    QList<LedgerRecord> payable;
    const QJsonObject response = QJsonDocument::fromJson(body).object();
    if (!response.value(QStringLiteral("ok")).toBool() ||
        response.value(QStringLiteral("custody")).toString() !=
            QLatin1String("direct-owner-wallet"))
        return payable;
    for (const QJsonValue &value :
         response.value(QStringLiteral("rewards")).toArray()) {
        forkmesh::tips::ContributorPayout payout;
        if (!forkmesh::tips::parseContributorPayout(value.toObject(), &payout))
            continue;
        payable.append(LedgerFeature::payableRecord(
            payout.rewardId, payout.recipientName, payout.recipientAddress,
            payout.repository, payout.sourceKind, payout.subjectNumber,
            payout.lamports));
    }
    return payable;
}

// Why a pending record has no Pay button. Saying nothing here reads as a bug;
// every one of these is a state the reader can actually act on somewhere.
QString ledgerUnpayableReason(const LedgerRecord &record)
{
    if (record.category != QLatin1String("contributor-reward"))
        return QStringLiteral("relay-scheduled");
    if (record.status == QLatin1String("pending_merge"))
        return QStringLiteral("pays on merge");
    if (record.status == QLatin1String("pending_wallet"))
        return QStringLiteral("no payout address");
    if (record.status == QLatin1String("submitted"))
        return QStringLiteral("awaiting finality");
    return QStringLiteral("not payable here");
}

// A held payout is a page-wide condition with a per-row meaning: one signed
// reward transfer is awaiting reconciliation, and until it settles no reward
// on this page can be paid. Rows say which of the two they are — the payout
// that is actually held, or one queued behind it — because "why can I not pay
// this?" has a different answer in each case.
bool ledgerRowIsHeldPayout(const LedgerRecord &record,
                           const ContributorPayoutReceipt::Pending &held)
{
    if (held.rewardId.isEmpty())
        return false;
    return record.rewardId == held.rewardId || record.sourceId == held.rewardId;
}

// Appended to the Status cell of every reward this desktop cannot pay right
// now. Short enough to sit beside the relay's own status without pushing the
// column wide; the detail lives in the tooltip beside it.
QString ledgerHeldStatusSuffix(const LedgerRecord &record,
                               const ContributorPayoutReceipt::Pending &held)
{
    return ledgerRowIsHeldPayout(record, held)
               ? QString::fromUtf8(" \xC2\xB7 signed, reconciling")
               : QString::fromUtf8(" \xC2\xB7 payout held");
}

// The whole held receipt, for the row's tooltip. Every figure that identifies
// the transfer is named here, so a reader can check it on an explorer without
// leaving the page or opening the alert again.
QString ledgerHeldTooltip(const LedgerRecord &record,
                          const ContributorPayoutReceipt::Pending &held)
{
    QStringList lines;
    lines.append(ledgerRowIsHeldPayout(record, held)
                     ? QStringLiteral(
                           "This reward's payment is already signed and is "
                           "awaiting reconciliation.")
                     : QStringLiteral(
                           "Blocked: another contributor payout is signed and "
                           "awaiting reconciliation."));
    lines.append(QString());
    lines.append(ContributorPayoutReceipt::detailLines(held));
    lines.append(QString());
    lines.append(ContributorPayoutReceipt::reconciliationAdvice(held));
    return lines.join(QLatin1Char('\n'));
}

// A reward this desktop paid and proved finalized on Solana, whose notice has
// not reached the relay yet. The relay therefore still publishes it as ready to
// pay, and the row must not: the money has gone, and pressing Pay again would
// be the one double payment the whole receipt mechanism exists to prevent.
bool ledgerRowIsSettledHere(const LedgerRecord &record,
                            const QSet<QString> &settled)
{
    if (settled.isEmpty() || record.category != QLatin1String("contributor-reward"))
        return false;
    return settled.contains(record.rewardId) || settled.contains(record.sourceId);
}

// Which key the settled receipt is filed under. ledgerRowIsSettledHere accepts
// either projection of the reward id, so read back whichever one matched rather
// than guessing a second time and finding an empty record.
QString ledgerSettledRewardId(const LedgerRecord &record,
                              const QSet<QString> &settled)
{
    if (settled.contains(record.rewardId))
        return record.rewardId;
    if (settled.contains(record.sourceId))
        return record.sourceId;
    return record.rewardId.isEmpty() ? record.sourceId : record.rewardId;
}

QString ledgerSettledTooltip(const LedgerRecord &record,
                             const QSet<QString> &settled)
{
    const QString rewardId = ledgerSettledRewardId(record, settled);
    const ContributorPayoutReceipt::Settled::Notice notice =
        ContributorPayoutReceipt::Settled::read(QSettings(), rewardId);
    QStringList lines;
    lines.append(ContributorPayoutReceipt::Settled::summary(notice));
    lines.append(QString());
    lines.append(QStringLiteral("Reward: %1").arg(rewardId));
    if (notice.lamports > 0) {
        lines.append(QStringLiteral("Amount: %1")
                         .arg(forkmesh::tips::formatLamports(notice.lamports)));
    }
    if (!notice.chainSignature.isEmpty()) {
        lines.append(
            QStringLiteral("Signature: %1").arg(notice.chainSignature));
    }
    return lines.join(QLatin1Char('\n'));
}

// The transaction that paid a row, from whichever side of the errand knows it.
// The relay publishes one once the payout's notice reaches it; until then only
// this desktop knows, because it signed and broadcast the transfer itself. A
// Transaction column reading "—" for a payment this machine can name on a
// public explorer hides the one fact the reader opened the page for, and hides
// it in exactly the state — money gone, relay silent — where they most need it.
struct LedgerTransactionRef {
    QString signature;
    QUrl url;
    bool relayKnows = true; // false while the notice is still owed
};

LedgerTransactionRef ledgerTransactionRef(
    const LedgerRecord &record, const QString &pageNetwork,
    const QSet<QString> &settled,
    const ContributorPayoutReceipt::Pending &held)
{
    const QString network =
        record.network.isEmpty() ? pageNetwork : record.network;
    LedgerTransactionRef ref;
    if (!record.transactionId.isEmpty()) {
        ref.signature = record.transactionId;
        ref.url = record.explorerUrl.isValid()
                      ? record.explorerUrl
                      : ledgerExplorerUrl(ref.signature, network);
        return ref;
    }
    if (ledgerRowIsSettledHere(record, settled)) {
        ref.signature =
            ContributorPayoutReceipt::Settled::read(
                QSettings(), ledgerSettledRewardId(record, settled))
                .chainSignature;
    } else if (held.blocking() && ledgerRowIsHeldPayout(record, held)) {
        ref.signature = held.chainSignature;
    }
    if (ref.signature.isEmpty())
        return {};
    ref.relayKnows = false;
    ref.url = ledgerExplorerUrl(ref.signature, network);
    return ref;
}

QString ledgerUnpayableTooltip(const LedgerRecord &record)
{
    if (record.category != QLatin1String("contributor-reward"))
        return QStringLiteral(
            "The relay schedules this payout and signs it from the community "
            "pool key, not from your personal wallet. Review and sign it in "
            "Control → Reward pool.");
    if (record.status == QLatin1String("pending_merge"))
        return QStringLiteral(
            "This estimate pays once the pull request lands. If it has already "
            "merged, Refresh: the relay confirms the merge from the pull's own "
            "record when this page loads, and the row then offers Send payment, "
            "or Remind while the recipient has no payout address.");
    if (record.status == QLatin1String("pending_wallet"))
        return QStringLiteral(
            "The recipient has not published a payout address on their "
            "ForkMesh profile, so there is nothing exact to sign yet.");
    if (record.status == QLatin1String("submitted"))
        return QStringLiteral(
            "A transfer for this reward is already on-chain and waiting to be "
            "confirmed final. No replacement will be signed.");
    return QStringLiteral(
        "The relay has not published an exact payout plan for this record, so "
        "this desktop has nothing to sign for it.");
}

constexpr int kLedgerStatusToneRole = Qt::UserRole + 11;
constexpr int kLedgerNextGutter = 24;
constexpr int kLedgerNextRowHeight = 48;

enum LedgerTone {
    kLedgerToneNeutral = 0,
    kLedgerToneAccent,
    kLedgerToneSuccess,
    kLedgerToneWarning,
    kLedgerToneDanger,
};

int ledgerStatusTone(const LedgerRecord &record, bool pending, bool settledHere,
                     bool held)
{
    const QString status = record.status.toLower();
    if (status.contains(QLatin1String("fail")) ||
        status.contains(QLatin1String("cancel")) ||
        status.contains(QLatin1String("reject")))
        return kLedgerToneDanger;
    if (held)
        return kLedgerToneWarning;
    if (settledHere)
        return kLedgerToneSuccess;
    if (pending)
        return kLedgerToneAccent;
    return kLedgerToneSuccess;
}

QColor ledgerToneColor(int tone)
{
    switch (tone) {
    case kLedgerToneAccent:
        return Theme::color(Theme::Role::Accent);
    case kLedgerToneSuccess:
        return Theme::color(Theme::Role::Success);
    case kLedgerToneWarning:
        return Theme::color(Theme::Role::Warning);
    case kLedgerToneDanger:
        return Theme::color(Theme::Role::Danger);
    default:
        return Theme::color(Theme::Role::FgSubtle);
    }
}

class LedgerNextRowDelegate : public QStyledItemDelegate
{
public:
    explicit LedgerNextRowDelegate(QTableWidget *table)
        : QStyledItemDelegate(table), m_table(table)
    {
        table->viewport()->installEventFilter(this);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (!Theme::isNext()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        painter->save();
        const QRect rect = option.rect;
        if (option.state & QStyle::State_Selected)
            painter->fillRect(rect, Theme::color(Theme::Role::AccentSubtle));
        else if (index.row() == m_hoverRow)
            painter->fillRect(rect, Theme::color(Theme::Role::SurfaceRaised));
        painter->fillRect(QRect(rect.left(), rect.bottom(), rect.width(), 1),
                          Theme::color(Theme::Role::Border));
        const int column = index.column();
        QString text = shownText(index);
        QRect box = rect.adjusted(leftPad(column), 0, -rightPad(column), 0);
        if (column == kLedgerStatusColumn && !text.isEmpty()) {
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(
                ledgerToneColor(index.data(kLedgerStatusToneRole).toInt()));
            painter->drawEllipse(
                QRectF(box.left(), rect.top() + rect.height() / 2.0 - 3.0, 6.0, 6.0));
            box.setLeft(box.left() + 12);
        }
        const QFont font = cellFont(option, column);
        const QFontMetrics metrics(font);
        const Qt::TextElideMode mode =
            m_table ? m_table->textElideMode() : Qt::ElideRight;
        if (column == kLedgerAmountColumn) {
            const int split = text.indexOf(QStringLiteral(" ("));
            const QString main = split < 0 ? text : text.left(split);
            const QString extra = split < 0 ? QString() : usdLine(text.mid(split));
            const QColor mainColor = main.startsWith(QLatin1Char('+'))
                                         ? Theme::color(Theme::Role::Success)
                                         : cellColor(index, column, main);
            drawLines(painter, box, font, mainColor, main, extra,
                      Qt::AlignRight, Qt::ElideLeft);
        } else if (column == kLedgerRecordColumn) {
            drawLines(painter, box, font, Theme::color(Theme::Role::Fg), text,
                      subtitle(index), Qt::AlignLeft, mode);
        } else {
            painter->setFont(font);
            painter->setPen(cellColor(index, column, text));
            painter->drawText(box, Qt::AlignLeft | Qt::AlignVCenter,
                              metrics.elidedText(text, mode, box.width()));
        }
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        if (!Theme::isNext())
            return QStyledItemDelegate::sizeHint(option, index);
        const int column = index.column();
        QString text = shownText(index);
        if (column == kLedgerAmountColumn) {
            const int split = text.indexOf(QStringLiteral(" ("));
            if (split >= 0)
                text = text.left(split);
        }
        int width = QFontMetrics(cellFont(option, column)).horizontalAdvance(text) +
                    leftPad(column) + rightPad(column) + 2;
        if (column == kLedgerStatusColumn)
            width += 12;
        return QSize(width, kLedgerNextRowHeight);
    }

    bool helpEvent(QHelpEvent *event, QAbstractItemView *view,
                   const QStyleOptionViewItem &option,
                   const QModelIndex &index) override
    {
        if (Theme::isNext() && event && event->type() == QEvent::ToolTip &&
            index.column() == kLedgerRecordColumn && titleFolded()) {
            const QModelIndex title = index.siblingAtColumn(kLedgerTitleColumn);
            QStringList parts;
            for (const QModelIndex &cell : {index, title}) {
                const QString tip = cell.data(Qt::ToolTipRole).toString();
                if (!tip.isEmpty() && !parts.contains(tip))
                    parts << tip;
            }
            if (!parts.isEmpty()) {
                QToolTip::showText(event->globalPos(),
                                   parts.join(QStringLiteral("\n\n")), view,
                                   option.rect);
                return true;
            }
        }
        return QStyledItemDelegate::helpEvent(event, view, option, index);
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (m_table && watched == m_table->viewport()) {
            int row = m_hoverRow;
            if (event->type() == QEvent::MouseMove)
                row = m_table->rowAt(
                    static_cast<QMouseEvent *>(event)->position().toPoint().y());
            else if (event->type() == QEvent::Leave)
                row = -1;
            if (row != m_hoverRow) {
                m_hoverRow = row;
                if (Theme::isNext())
                    m_table->viewport()->update();
            }
            return false;
        }
        return QStyledItemDelegate::eventFilter(watched, event);
    }

private:
    bool titleFolded() const
    {
        return m_table && m_table->isColumnHidden(kLedgerTitleColumn);
    }

    QString subtitle(const QModelIndex &index) const
    {
        if (!titleFolded())
            return QString();
        const QString title =
            index.siblingAtColumn(kLedgerTitleColumn).data(Qt::DisplayRole).toString();
        return title == QString::fromUtf8("\xE2\x80\x94") ? QString() : title;
    }

    QString shownText(const QModelIndex &index) const
    {
        QString text = index.data(Qt::DisplayRole).toString();
        if (index.column() == kLedgerStatusColumn && !text.isEmpty())
            text[0] = text.at(0).toUpper();
        if (index.column() == kLedgerTransactionColumn && m_table &&
            text == QString::fromUtf8("\xE2\x80\x94") &&
            m_table->cellWidget(index.row(), kLedgerActionColumn))
            text.clear();
        return text;
    }

    static QString usdLine(QString extra)
    {
        extra = extra.trimmed();
        if (extra.startsWith(QLatin1Char('(')))
            extra.remove(0, 1);
        if (extra.endsWith(QLatin1Char(')')))
            extra.chop(1);
        if (extra.startsWith(QLatin1Char('~')))
            return QString::fromUtf8("\xE2\x89\x88 ") + extra.mid(1);
        return extra;
    }

    static void drawLines(QPainter *painter, const QRect &box, const QFont &font,
                          const QColor &color, const QString &main,
                          const QString &second, Qt::AlignmentFlag side,
                          Qt::TextElideMode mode)
    {
        const QFontMetrics metrics(font);
        QFont small = font;
        small.setPixelSize(main.isEmpty() ? font.pixelSize() : 11);
        small.setWeight(QFont::Normal);
        const QFontMetrics smallMetrics(small);
        const int gap = 2;
        const int total = second.isEmpty()
                              ? metrics.height()
                              : metrics.height() + gap + smallMetrics.height();
        const int top = box.top() + (box.height() - total) / 2;
        painter->setFont(font);
        painter->setPen(color);
        painter->drawText(QRect(box.left(), top, box.width(), metrics.height()),
                          side | Qt::AlignVCenter,
                          metrics.elidedText(main, mode, box.width()));
        if (second.isEmpty())
            return;
        painter->setFont(small);
        painter->setPen(side == Qt::AlignRight
                            ? Theme::color(Theme::Role::FgSubtle)
                            : Theme::color(Theme::Role::FgMuted));
        painter->drawText(QRect(box.left(), top + metrics.height() + gap,
                                box.width(), smallMetrics.height()),
                          side | Qt::AlignVCenter,
                          smallMetrics.elidedText(second, mode, box.width()));
    }

    static int leftPad(int column)
    {
        return column == kLedgerWhenColumn ? 16 : 10;
    }

    static int rightPad(int column)
    {
        return column == kLedgerAmountColumn ? 16 : 10;
    }

    static QFont cellFont(const QStyleOptionViewItem &option, int column)
    {
        QFont font = option.font;
        font.setUnderline(false);
        if (column == kLedgerAmountColumn || column == kLedgerTransactionColumn) {
            font.setFamily(Theme::monoFontFamily());
            font.setPixelSize(13);
            return font;
        }
        font.setPixelSize(column == kLedgerActionColumn ? 12 : 13);
        if (column == kLedgerRecordColumn)
            font.setWeight(QFont::Medium);
        return font;
    }

    static QColor cellColor(const QModelIndex &index, int column,
                            const QString &text)
    {
        if (text == QString::fromUtf8("\xE2\x80\x94"))
            return Theme::color(Theme::Role::FgSubtle);
        switch (column) {
        case kLedgerRecordColumn:
        case kLedgerAmountColumn:
            return Theme::color(Theme::Role::Fg);
        case kLedgerTransactionColumn:
            return index.data(kLedgerLinkUrlRole).toUrl().isValid()
                       ? Theme::color(Theme::Role::AccentFg)
                       : Theme::color(Theme::Role::FgSubtle);
        case kLedgerActionColumn:
            return Theme::color(Theme::Role::FgSubtle);
        default:
            return Theme::color(Theme::Role::FgMuted);
        }
    }

    QTableWidget *m_table = nullptr;
    int m_hoverRow = -1;
};

QIcon ledgerQrIcon(const QColor &color)
{
    const qreal ratio =
        qMax<qreal>(2.0, qGuiApp ? qGuiApp->devicePixelRatio() : 1.0);
    const int px = 14;
    QPixmap pixmap(qRound(px * ratio), qRound(px * ratio));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 1.2));
    painter.setBrush(Qt::NoBrush);
    for (const QPointF &corner :
         {QPointF(1.6, 1.6), QPointF(8.4, 1.6), QPointF(1.6, 8.4)})
        painter.drawRoundedRect(QRectF(corner, QSizeF(4.0, 4.0)), 0.8, 0.8);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    for (const QRectF &dot :
         {QRectF(8.6, 8.6, 1.8, 1.8), QRectF(11.4, 11.4, 1.8, 1.8),
          QRectF(8.6, 11.4, 1.8, 1.8), QRectF(11.4, 8.6, 1.8, 1.8)})
        painter.drawRect(dot);
    return QIcon(pixmap);
}

QString ledgerWalletStatusText(const LedgerWalletCard &wallet)
{
    const bool linked = !wallet.address.isEmpty();
    QString text;
    if (linked && wallet.lamports >= 0) {
        text = LedgerFeature::balanceAgeText(
            wallet.checkedAt, QDateTime::currentMSecsSinceEpoch());
        if (wallet.sharedAddress)
            text += QStringLiteral(" · shared address");
    } else if (linked) {
        text = QStringLiteral("not read yet");
    }
    return text;
}

QPixmap ledgerQrTile(const QImage &qr, int padding)
{
    const qreal ratio =
        qMax<qreal>(2.0, qGuiApp ? qGuiApp->devicePixelRatio() : 1.0);
    const QSize size(qr.width() + 2 * padding, qr.height() + 2 * padding);
    QPixmap tile(qRound(size.width() * ratio), qRound(size.height() * ratio));
    tile.setDevicePixelRatio(ratio);
    tile.fill(Qt::transparent);
    QPainter painter(&tile);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(Qt::white));
    painter.drawRoundedRect(QRectF(0, 0, size.width(), size.height()), 10, 10);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(QRect(padding, padding, qr.width(), qr.height()), qr);
    return tile;
}

QPushButton *makeLedgerCardIconButton(const QString &objectName,
                                      const QString &toolTip)
{
    auto *button = new QPushButton;
    button->setObjectName(objectName);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    button->setFixedSize(22, 22);
    button->setToolTip(toolTip);
    button->setProperty("nextIconRole", int(Theme::Role::FgMuted));
    return button;
}

QWidget *makeLedgerNextWalletCard(const LedgerWalletCard &wallet)
{
    auto *card = new QFrame;
    card->setObjectName(QStringLiteral("ledgerWalletCard"));
    card->setFrameShape(QFrame::NoFrame);
    card->setMinimumWidth(200);
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(2);

    const bool linked = !wallet.address.isEmpty();
    const bool unfunded =
        linked && wallet.signsPayments && wallet.lamports == 0;
    const QString statusText = ledgerWalletStatusText(wallet);
    QString noteText;
    if (!linked) {
        noteText = wallet.emptyPurpose;
    } else if (unfunded) {
        noteText = QStringLiteral(
            "Empty. Solana refuses a transfer signed from an unfunded account "
            "before it ever becomes a transaction.");
    } else if (wallet.lamports < 0) {
        noteText = wallet.purpose;
    }

    auto *headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(6);
    auto *icon = new QLabel;
    icon->setObjectName(QStringLiteral("ledgerWalletIcon"));
    icon->setPixmap(kit::tintedIcon(QStringLiteral("credit-card"),
                                    Theme::color(Theme::Role::FgSubtle), 13,
                                    qGuiApp ? qGuiApp->devicePixelRatio() : 1.0));
    headerRow->addWidget(icon);
    auto *label = new QLabel(wallet.label.toUpper());
    label->setObjectName(QStringLiteral("ledgerWalletLabel"));
    label->setTextFormat(Qt::PlainText);
    label->setToolTip(wallet.purpose);
    QFont labelFont = label->font();
    labelFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
    label->setFont(labelFont);
    headerRow->addWidget(label, 1);
    if (linked && wallet.onRefresh) {
        auto *refresh = makeLedgerCardIconButton(
            QStringLiteral("ledgerWalletRefresh"),
            QStringLiteral("Read this balance again"));
        setOcticon(refresh, QStringLiteral("sync"), 12);
        QObject::connect(refresh, &QPushButton::clicked, refresh,
                         wallet.onRefresh);
        headerRow->addWidget(refresh);
    }
    if (wallet.onRemove) {
        auto *removeButton = makeLedgerCardIconButton(
            QStringLiteral("ghostButton"),
            QStringLiteral("Stop tracking this address"));
        setOcticon(removeButton, QStringLiteral("x"), 12);
        QObject::connect(removeButton, &QPushButton::clicked, removeButton,
                         wallet.onRemove);
        headerRow->addWidget(removeButton);
    }
    if (linked) {
        const QImage qr = QrCode::encodeToImage(
            QStringLiteral("solana:%1").arg(wallet.address), 6, 0);
        if (!qr.isNull()) {
            auto *qrButton = makeLedgerCardIconButton(
                QStringLiteral("ledgerWalletQrButton"),
                QStringLiteral("Show a QR code a Solana wallet app can scan to "
                               "send SOL to this address"));
            qrButton->setIcon(ledgerQrIcon(Theme::color(Theme::Role::FgMuted)));
            qrButton->setIconSize(QSize(14, 14));
            auto *menu = new QMenu(qrButton);
            menu->setObjectName(QStringLiteral("ledgerWalletQrMenu"));
            auto *panel = new QWidget;
            panel->setObjectName(QStringLiteral("ledgerWalletQrPanel"));
            auto *panelLayout = new QVBoxLayout(panel);
            panelLayout->setContentsMargins(16, 16, 16, 12);
            panelLayout->setSpacing(10);
            auto *qrLabel = new QLabel(panel);
            qrLabel->setObjectName(QStringLiteral("ledgerWalletQr"));
            qrLabel->setPixmap(ledgerQrTile(qr, 14));
            qrLabel->setAlignment(Qt::AlignCenter);
            qrLabel->setToolTip(
                QStringLiteral("Scan with a Solana wallet app to send SOL "
                               "to this address"));
            panelLayout->addWidget(qrLabel, 0, Qt::AlignHCenter);
            auto *addressRow = new QHBoxLayout;
            addressRow->setContentsMargins(0, 0, 0, 0);
            addressRow->setSpacing(6);
            auto *shortAddress =
                new QLabel(ledgerAbbreviatedAddress(wallet.address), panel);
            shortAddress->setObjectName(QStringLiteral("ledgerWalletQrAddress"));
            shortAddress->setTextFormat(Qt::PlainText);
            shortAddress->setToolTip(wallet.address);
            QFont mono = shortAddress->font();
            mono.setFamily(Theme::monoFontFamily());
            shortAddress->setFont(mono);
            addressRow->addWidget(shortAddress);
            addressRow->addStretch(1);
            auto *copy = makeLedgerCardIconButton(
                QStringLiteral("ledgerWalletQrCopy"),
                QStringLiteral("Copy the full address"));
            copy->setParent(panel);
            setOcticon(copy, QStringLiteral("copy"), 14);
            const QString address = wallet.address;
            QObject::connect(copy, &QPushButton::clicked, copy, [address] {
                if (QClipboard *clipboard = QGuiApplication::clipboard())
                    clipboard->setText(address);
            });
            addressRow->addWidget(copy);
            panelLayout->addLayout(addressRow);
            auto *action = new QWidgetAction(menu);
            action->setDefaultWidget(panel);
            menu->addAction(action);
            QObject::connect(qrButton, &QPushButton::clicked, qrButton,
                             [qrButton, menu] {
                const QSize size = menu->sizeHint();
                menu->popup(qrButton->mapToGlobal(
                    QPoint(qrButton->width() - size.width(),
                           qrButton->height() + 6)));
            });
            headerRow->addWidget(qrButton);
        }
    }
    layout->addLayout(headerRow);
    layout->addSpacing(8);

    auto *amount = new QLabel(
        !linked ? wallet.emptyText
                : wallet.lamports < 0
                      ? QStringLiteral("Not read yet")
                      : LedgerFeature::solText(quint64(wallet.lamports)) +
                            QStringLiteral(" SOL"));
    amount->setObjectName(QStringLiteral("ledgerWalletAmount"));
    amount->setTextFormat(Qt::PlainText);
    amount->setProperty("ledgerEmpty", !linked || wallet.lamports < 0);
    layout->addWidget(amount);

    if (linked) {
        auto *metaRow = new QHBoxLayout;
        metaRow->setContentsMargins(0, 0, 0, 0);
        metaRow->setSpacing(8);
        if (wallet.usdMicros >= 0) {
            auto *usd = new QLabel(QString::fromUtf8("\xE2\x89\x88 $") +
                                   LedgerFeature::usdText(wallet.usdMicros));
            usd->setObjectName(QStringLiteral("ledgerWalletUsd"));
            usd->setTextFormat(Qt::PlainText);
            usd->setToolTip(QStringLiteral(
                "A live spot estimate at the price this page was read with, not "
                "transaction-time accounting."));
            metaRow->addWidget(usd);
        }
        auto *address = new QLabel;
        address->setObjectName(QStringLiteral("ledgerWalletAddress"));
        address->setTextFormat(Qt::RichText);
        address->setOpenExternalLinks(true);
        address->setTextInteractionFlags(Qt::TextBrowserInteraction);
        QFont mono = address->font();
        mono.setFamily(Theme::monoFontFamily());
        address->setFont(mono);
        const QString shown =
            ledgerAbbreviatedAddress(wallet.address).toHtmlEscaped();
        address->setText(
            wallet.explorerUrl.isValid()
                ? QStringLiteral(
                      "<a href=\"%1\" style=\"color:%2; text-decoration:none;\">%3</a>")
                      .arg(wallet.explorerUrl.toString(QUrl::FullyEncoded)
                               .toHtmlEscaped(),
                           Theme::hex(Theme::Role::FgSubtle), shown)
                : shown);
        address->setToolTip(wallet.address);
        metaRow->addWidget(address);
        metaRow->addStretch(1);
        if (!statusText.isEmpty()) {
            auto *status = new QLabel(statusText);
            status->setObjectName(QStringLiteral("ledgerWalletAge"));
            status->setTextFormat(Qt::PlainText);
            status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            status->setToolTip(wallet.purpose);
            metaRow->addWidget(status);
        }
        layout->addLayout(metaRow);
    }

    if (!noteText.isEmpty()) {
        auto *note = new QLabel(noteText);
        note->setObjectName(unfunded ? QStringLiteral("ledgerWalletWarning")
                                     : QStringLiteral("ledgerWalletNote"));
        note->setWordWrap(true);
        note->setToolTip(wallet.purpose);
        layout->addSpacing(2);
        layout->addWidget(note);
    }
    layout->addStretch(1);
    if (!linked && wallet.onSetUp && !wallet.setUpText.isEmpty()) {
        auto *setUp = new QPushButton(wallet.setUpText);
        setUp->setObjectName(QStringLiteral("ledgerWalletSetUpButton"));
        setUp->setCursor(Qt::PointingHandCursor);
        setUp->setToolTip(wallet.emptyPurpose);
        setUp->setProperty("ledgerVariant", QStringLiteral("outline"));
        if (!wallet.setUpIcon.isEmpty()) {
            setUp->setProperty("nextIconRole", int(Theme::Role::FgSecondary));
            setOcticon(setUp, wallet.setUpIcon, 14);
        }
        QObject::connect(setUp, &QPushButton::clicked, setUp, wallet.onSetUp);
        layout->addSpacing(10);
        layout->addWidget(setUp, 0, Qt::AlignLeft);
    }
    return card;
}

void styleLedgerNextButton(QPushButton *button, const char *variant,
                           const QString &octicon, Theme::Role iconRole)
{
    if (!button)
        return;
    button->setProperty("ledgerVariant", QString::fromLatin1(variant));
    button->setProperty("nextIconRole", int(iconRole));
    if (!octicon.isEmpty())
        setOcticon(button, octicon, 14);
}

QFrame *makeLedgerNextStat(const QString &key, const QString &name)
{
    auto *card = new QFrame;
    card->setObjectName(QStringLiteral("ledgerNextStat"));
    card->setFrameShape(QFrame::NoFrame);
    card->setMinimumWidth(140);
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(2);
    auto *label = new QLabel(name);
    label->setProperty("ledgerRole", QStringLiteral("statName"));
    layout->addWidget(label);
    auto *value = new QLabel(QString::fromUtf8("\xE2\x80\x94"));
    value->setObjectName(QStringLiteral("ledgerStat%1Value").arg(key));
    value->setProperty("ledgerRole", QStringLiteral("statValue"));
    value->setTextFormat(Qt::PlainText);
    layout->addWidget(value);
    auto *note = new QLabel;
    note->setObjectName(QStringLiteral("ledgerStat%1Note").arg(key));
    note->setProperty("ledgerRole", QStringLiteral("statNote"));
    note->setTextFormat(Qt::PlainText);
    layout->addWidget(note);
    return card;
}

void setLedgerNextStat(QWidget *section, const QString &key,
                       const QString &value, const QString &note,
                       const QString &tip)
{
    auto *valueLabel =
        section->findChild<QLabel *>(QStringLiteral("ledgerStat%1Value").arg(key));
    if (!valueLabel)
        return;
    valueLabel->setText(value);
    if (auto *noteLabel = section->findChild<QLabel *>(
            QStringLiteral("ledgerStat%1Note").arg(key)))
        noteLabel->setText(note);
    if (QWidget *card = valueLabel->parentWidget())
        card->setToolTip(tip);
}

QString ledgerStatAmount(quint64 lamports, qint64 usdMicros)
{
    if (lamports > 0)
        return LedgerFeature::solText(lamports) + QStringLiteral(" SOL");
    if (usdMicros > 0)
        return QStringLiteral("$") + LedgerFeature::usdText(usdMicros);
    return QStringLiteral("0 SOL");
}

QString ledgerStatNote(quint64 lamports, qint64 usdMicros, const QString &count)
{
    if (lamports > 0 && usdMicros > 0)
        return QString::fromUtf8("\xE2\x89\x88 $") +
               LedgerFeature::usdText(usdMicros);
    return count;
}

void applyLedgerSearch(QTableWidget *table, const QString &query)
{
    if (!table)
        return;
    const QString needle = query.trimmed();
    for (int row = 0; row < table->rowCount(); ++row) {
        bool match = needle.isEmpty();
        for (int column = kLedgerWhenColumn; !match && column < kLedgerActionColumn;
             ++column) {
            if (QTableWidgetItem *item = table->item(row, column))
                match = item->text().contains(needle, Qt::CaseInsensitive);
        }
        if (table->isRowHidden(row) == match)
            table->setRowHidden(row, !match);
    }
}

void refreshLedgerNextStats(QWidget *anchor, const LedgerTotals &totals,
                            const LedgerPage &page, const QString &totalsTip)
{
    QWidget *section = anchor;
    while (section && section->objectName() != QLatin1String("ledgerSection"))
        section = section->parentWidget();
    if (!section)
        return;
    setLedgerNextStat(
        section, QStringLiteral("Expenses"),
        ledgerStatAmount(totals.expenseLamports, totals.expenseUsdMicros),
        ledgerStatNote(totals.expenseLamports, totals.expenseUsdMicros,
                       QStringLiteral("%1 paid record(s)").arg(totals.expenseCount)),
        totalsTip);
    setLedgerNextStat(
        section, QStringLiteral("Income"),
        ledgerStatAmount(totals.incomeLamports, totals.incomeUsdMicros),
        ledgerStatNote(totals.incomeLamports, totals.incomeUsdMicros,
                       QStringLiteral("%1 record(s)").arg(totals.incomeCount)),
        totalsTip);
    setLedgerNextStat(section, QStringLiteral("Records"),
                      QString::number(page.recordCount),
                      QStringLiteral("%1 on-chain").arg(page.settledCount),
                      QString());
    setLedgerNextStat(
        section, QStringLiteral("Awaiting"),
        QString::number(totals.pendingCount),
        totals.pendingCount > 0
            ? ledgerTotalText(totals.pendingLamports, totals.pendingUsdMicros)
            : QStringLiteral("Nothing owed"),
        QStringLiteral("Records still owed either way, counted in money as well "
                       "as in rows."));
    if (auto *price = section->findChild<QLabel *>(QStringLiteral("ledgerNextPrice"))) {
        price->setText(page.solUsdPrice.isEmpty()
                           ? QString()
                           : QString::fromUtf8("SOL \xE2\x89\x88 $") +
                                 ledgerUsdText(page.solUsdPrice));
        price->setVisible(!page.solUsdPrice.isEmpty());
    }
    if (auto *search = section->findChild<QLineEdit *>(
            QStringLiteral("ledgerNextSearch")))
        applyLedgerSearch(qobject_cast<QTableWidget *>(anchor), search->text());
}

} // namespace

QWidget *MainWindow::buildLedgerSection()
{
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("ledgerSection"));
    const bool next = Theme::isNext();
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(24, 20, 24, 20);
    outer->setSpacing(12);
    QVBoxLayout *body = outer;
    if (next) {
        page->setProperty("ledgerNextPage", true);
        page->setAttribute(Qt::WA_StyledBackground, true);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);
    }
    const QString ledgerDescription = QStringLiteral(
        "Every relay-owned financial record, projected from the tables that "
        "own each state transition. Amounts are exact; USD figures are a live "
        "spot estimate, not transaction-time accounting.");

    auto *header = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Ledger"));
    title->setObjectName(QStringLiteral("sectionTitle"));
    title->setAccessibleName(QStringLiteral("ForkMesh Ledger"));
    if (next) {
        header->setContentsMargins(kLedgerNextGutter, 12, kLedgerNextGutter, 12);
        header->setSpacing(6);
        auto *titleColumn = new QVBoxLayout;
        titleColumn->setContentsMargins(0, 0, 0, 0);
        titleColumn->setSpacing(2);
        title->setAccessibleDescription(ledgerDescription);
        title->setToolTip(ledgerDescription);
        titleColumn->addWidget(title);
        auto *lede = new QLabel(QStringLiteral(
            "Every relay-owned financial record. Amounts are exact; USD is a "
            "live spot estimate."));
        lede->setObjectName(QStringLiteral("mutedLabel"));
        lede->setProperty("ledgerRole", QStringLiteral("summary"));
        lede->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        lede->setToolTip(ledgerDescription);
        titleColumn->addWidget(lede);
        header->addLayout(titleColumn, 1);
    } else {
        header->addWidget(title);
        header->addStretch();
    }
    m_ledgerSendPaymentButton =
        new QPushButton(QString::fromUtf8("Send payment\xE2\x80\xA6"));
    m_ledgerSendPaymentButton->setObjectName(
        QStringLiteral("ledgerSendPaymentButton"));
    m_ledgerSendPaymentButton->setCursor(Qt::PointingHandCursor);
    m_ledgerSendPaymentButton->setToolTip(QStringLiteral(
        "Pay a ForkMesh user for completed work from your linked Solana "
        "wallet and record the finalized receipt here."));
    setOcticon(m_ledgerSendPaymentButton, QStringLiteral("credit-card"), 14);
    connect(m_ledgerSendPaymentButton, &QPushButton::clicked, this,
            &MainWindow::sendLedgerPayment);
    header->addWidget(m_ledgerSendPaymentButton);
    m_ledgerRefreshButton = new QPushButton(QStringLiteral("Refresh"));
    m_ledgerRefreshButton->setObjectName(QStringLiteral("ledgerRefreshButton"));
    m_ledgerRefreshButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_ledgerRefreshButton, QStringLiteral("sync"), 14);
    connect(m_ledgerRefreshButton, &QPushButton::clicked, this, [this] {
        // Asking for the current state is asking for the standing summary
        // back, so the last payment stops holding the status line.
        m_ledgerPaymentLine.clear();
        refreshLedgerPage(true);
    });
    header->addWidget(m_ledgerRefreshButton);
    auto *browserButton = new QPushButton(QStringLiteral("Open in browser"));
    browserButton->setObjectName(QStringLiteral("ledgerBrowserButton"));
    browserButton->setCursor(Qt::PointingHandCursor);
    browserButton->setToolTip(
        QStringLiteral("Open this host's public ledger page"));
    setOcticon(browserButton, QStringLiteral("link"), 14);
    connect(browserButton, &QPushButton::clicked, this,
            &MainWindow::openLedgerInBrowser);
    header->addWidget(browserButton);
    QPushButton *addRecordToggle = nullptr;
    if (next) {
        styleLedgerNextButton(m_ledgerRefreshButton, "ghost",
                              QStringLiteral("sync"), Theme::Role::FgMuted);
        styleLedgerNextButton(browserButton, "ghost", QStringLiteral("link"),
                              Theme::Role::FgMuted);
        addRecordToggle = new QPushButton(QStringLiteral("Add record"));
        addRecordToggle->setObjectName(QStringLiteral("ledgerAddRecordToggle"));
        addRecordToggle->setCursor(Qt::PointingHandCursor);
        addRecordToggle->setToolTip(QStringLiteral(
            "Show or hide the form for your own expense and income records"));
        styleLedgerNextButton(addRecordToggle, "outline", QStringLiteral("plus"),
                              Theme::Role::FgSecondary);
        header->removeWidget(m_ledgerSendPaymentButton);
        header->addWidget(addRecordToggle);
        styleLedgerNextButton(m_ledgerSendPaymentButton, "inverse",
                              QStringLiteral("paper-airplane"), Theme::Role::Bg);
        header->addWidget(m_ledgerSendPaymentButton);
    }
    outer->addLayout(header);

    if (next) {
        auto *rule = new QFrame;
        rule->setProperty("ledgerRule", true);
        rule->setFrameShape(QFrame::NoFrame);
        rule->setFixedHeight(1);
        outer->addWidget(rule);
        body = new QVBoxLayout;
        body->setContentsMargins(kLedgerNextGutter, 20, kLedgerNextGutter, 16);
        body->setSpacing(16);
        outer->addLayout(body, 1);
    } else {
        outer->addWidget(ledgerHint(ledgerDescription));
    }

    // The entry form. The relay's records are the ones this app can pay and
    // verify, but a ledger that can only hold them is not a ledger: this is
    // where any other expense or income goes, in the same columns, counted in
    // the same totals, stored on this machine and sent nowhere.
    auto *entryForm = new QFrame;
    entryForm->setObjectName(QStringLiteral("ledgerEntryForm"));
    entryForm->setFrameShape(QFrame::NoFrame);
    auto *entryLayout = new QVBoxLayout(entryForm);
    entryLayout->setContentsMargins(12, 10, 12, 10);
    entryLayout->setSpacing(6);
    if (next) {
        entryLayout->setContentsMargins(16, 12, 16, 14);
        entryLayout->setSpacing(8);
        auto *formHead = new QHBoxLayout;
        formHead->setContentsMargins(0, 0, 0, 0);
        auto *formTitle = new QLabel(QStringLiteral("New record"));
        formTitle->setProperty("ledgerRole", QStringLiteral("formTitle"));
        formHead->addWidget(formTitle);
        formHead->addStretch();
        auto *formClose = new QPushButton;
        formClose->setObjectName(QStringLiteral("ledgerEntryCloseButton"));
        formClose->setCursor(Qt::PointingHandCursor);
        formClose->setToolTip(QStringLiteral("Hide this form"));
        formClose->setFixedSize(26, 26);
        styleLedgerNextButton(formClose, "icon", QStringLiteral("x"),
                              Theme::Role::FgMuted);
        connect(formClose, &QPushButton::clicked, entryForm, &QWidget::hide);
        formHead->addWidget(formClose);
        entryLayout->addLayout(formHead);
    }

    auto *entryTop = new QHBoxLayout;
    entryTop->setSpacing(8);
    m_ledgerEntryDirection = new QComboBox;
    m_ledgerEntryDirection->setObjectName(
        QStringLiteral("ledgerEntryDirection"));
    m_ledgerEntryDirection->addItem(QStringLiteral("Expense"),
                                    QStringLiteral("expense"));
    m_ledgerEntryDirection->addItem(QStringLiteral("Income"),
                                    QStringLiteral("income"));
    m_ledgerEntryDirection->setToolTip(
        QStringLiteral("Money out of this account, or into it"));
    entryTop->addWidget(m_ledgerEntryDirection);
    m_ledgerEntryDate = new QDateEdit(QDate::currentDate());
    m_ledgerEntryDate->setObjectName(QStringLiteral("ledgerEntryDate"));
    m_ledgerEntryDate->setCalendarPopup(true);
    m_ledgerEntryDate->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_ledgerEntryDate->setToolTip(QStringLiteral("When it happened"));
    entryTop->addWidget(m_ledgerEntryDate);
    m_ledgerEntryCategory = new QLineEdit;
    m_ledgerEntryCategory->setObjectName(
        QStringLiteral("ledgerEntryCategory"));
    m_ledgerEntryCategory->setPlaceholderText(
        QStringLiteral("Kind (hosting, hardware, donation)"));
    m_ledgerEntryCategory->setMaxLength(80);
    entryTop->addWidget(m_ledgerEntryCategory, 1);
    m_ledgerEntryDescription = new QLineEdit;
    m_ledgerEntryDescription->setObjectName(
        QStringLiteral("ledgerEntryDescription"));
    m_ledgerEntryDescription->setPlaceholderText(
        QStringLiteral("What this record is (required)"));
    m_ledgerEntryDescription->setMaxLength(160);
    entryTop->addWidget(m_ledgerEntryDescription, 2);
    entryLayout->addLayout(entryTop);

    auto *entryBottom = new QHBoxLayout;
    entryBottom->setSpacing(8);
    m_ledgerEntryCounterparty = new QLineEdit;
    m_ledgerEntryCounterparty->setObjectName(
        QStringLiteral("ledgerEntryCounterparty"));
    m_ledgerEntryCounterparty->setPlaceholderText(
        QString::fromUtf8("To / from \xE2\x80\x94 a name, @user or Solana "
                          "address"));
    m_ledgerEntryCounterparty->setMaxLength(80);
    m_ledgerEntryCounterparty->setToolTip(QStringLiteral(
        "A Solana address here is also tracked as a wallet card above, so a "
        "payment you record is a balance you can watch."));
    entryBottom->addWidget(m_ledgerEntryCounterparty, 2);
    m_ledgerEntryAmount = new QLineEdit;
    m_ledgerEntryAmount->setObjectName(QStringLiteral("ledgerEntryAmount"));
    m_ledgerEntryAmount->setPlaceholderText(QStringLiteral("0.00"));
    m_ledgerEntryAmount->setMaxLength(32);
    m_ledgerEntryAmount->setFixedWidth(120);
    entryBottom->addWidget(m_ledgerEntryAmount);
    m_ledgerEntryCurrency = new QComboBox;
    m_ledgerEntryCurrency->setObjectName(
        QStringLiteral("ledgerEntryCurrency"));
    m_ledgerEntryCurrency->addItem(QStringLiteral("SOL"),
                                   QStringLiteral("SOL"));
    m_ledgerEntryCurrency->addItem(QStringLiteral("USD"),
                                   QStringLiteral("USD"));
    entryBottom->addWidget(m_ledgerEntryCurrency);
    m_ledgerEntryOutstanding =
        new QCheckBox(QStringLiteral("Not settled yet"));
    m_ledgerEntryOutstanding->setObjectName(
        QStringLiteral("ledgerEntryOutstanding"));
    m_ledgerEntryOutstanding->setToolTip(QStringLiteral(
        "Tick for money still owed either way. It joins the awaiting-payment "
        "total until you settle it."));
    entryBottom->addWidget(m_ledgerEntryOutstanding);
    entryBottom->addStretch();
    auto *entryAddButton = new QPushButton(QStringLiteral("Add record"));
    entryAddButton->setObjectName(QStringLiteral("ledgerEntryAddButton"));
    entryAddButton->setProperty("buttonSize", "sm");
    entryAddButton->setCursor(Qt::PointingHandCursor);
    setOcticon(entryAddButton, QStringLiteral("plus"), 13);
    entryBottom->addWidget(entryAddButton);
    entryLayout->addLayout(entryBottom);
    const QString entryHint = QStringLiteral(
        "A record you add here stays on this machine, counts towards the "
        "totals below, and is never published. Entering only a Solana "
        "address tracks that wallet without recording anything.");
    if (next) {
        m_ledgerEntryAmount->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        styleLedgerNextButton(entryAddButton, "inverse", QStringLiteral("plus"),
                              Theme::Role::Bg);
        auto *formHint = ledgerHint(entryHint);
        formHint->setProperty("ledgerRole", QStringLiteral("formHint"));
        entryLayout->addSpacing(2);
        entryLayout->addWidget(formHint);
        entryForm->setParent(page, Qt::Popup | Qt::FramelessWindowHint |
                                       Qt::NoDropShadowWindowHint);
        entryForm->setAttribute(Qt::WA_TranslucentBackground, true);
        entryForm->setProperty("ledgerPopover", true);
        entryForm->setFixedWidth(660);
        entryForm->hide();
        connect(addRecordToggle, &QPushButton::clicked, entryForm,
                [entryForm, addRecordToggle, this] {
            if (entryForm->isVisible()) {
                entryForm->hide();
                return;
            }
            entryForm->adjustSize();
            const QPoint anchor = addRecordToggle->mapToGlobal(
                QPoint(addRecordToggle->width(), addRecordToggle->height() + 6));
            QPoint at(anchor.x() - entryForm->width(), anchor.y());
            if (QScreen *screen = addRecordToggle->screen()) {
                const QRect area = screen->availableGeometry();
                at.setX(qBound(area.left() + 8, at.x(),
                               qMax(area.left() + 8,
                                    area.right() - entryForm->width() - 8)));
            }
            entryForm->move(at);
            entryForm->show();
            entryForm->raise();
            if (m_ledgerEntryCategory)
                m_ledgerEntryCategory->setFocus();
        });
    } else {
        outer->addWidget(entryForm);
        outer->addWidget(ledgerHint(entryHint));
    }

    connect(entryAddButton, &QPushButton::clicked, this,
            &MainWindow::addLedgerEntry);
    for (QLineEdit *field :
         {m_ledgerEntryCategory, m_ledgerEntryDescription,
          m_ledgerEntryCounterparty, m_ledgerEntryAmount}) {
        connect(field, &QLineEdit::returnPressed, this,
                &MainWindow::addLedgerEntry);
    }

    m_ledgerWalletStrip = new QWidget;
    m_ledgerWalletStrip->setObjectName(QStringLiteral("ledgerWalletStrip"));
    auto *walletRow = new QHBoxLayout(m_ledgerWalletStrip);
    walletRow->setContentsMargins(0, 0, 0, 0);
    walletRow->setSpacing(10);
    m_ledgerWalletStrip->hide(); // nothing to say until a wallet is known
    body->addWidget(m_ledgerWalletStrip);
    if (next)
        walletRow->setSpacing(12);

    // A saved contributor receipt is a page-wide payment state, not a detail
    // hidden behind whichever row happens to be clicked. Put its recovery
    // tools immediately above the records it blocks. None of these signs or
    // broadcasts a new transaction: retry sends the same public receipt,
    // View opens that signature, and Undo removes only the local hold after a
    // separate acknowledgement.
    auto *payoutHold = new QFrame;
    m_ledgerPayoutHoldPanel = payoutHold;
    payoutHold->setObjectName(QStringLiteral("ledgerPayoutHoldPanel"));
    payoutHold->setFrameShape(QFrame::NoFrame);
    auto *payoutHoldLayout = new QVBoxLayout(payoutHold);
    payoutHoldLayout->setContentsMargins(14, 12, 14, 12);
    payoutHoldLayout->setSpacing(7);

    auto *payoutHoldHeader = new QHBoxLayout;
    payoutHoldHeader->setSpacing(8);
    auto *payoutHoldIcon = new QLabel;
    payoutHoldIcon->setObjectName(QStringLiteral("ledgerPayoutHoldIcon"));
    setLabelOcticon(payoutHoldIcon, QStringLiteral("alert"), 14);
    payoutHoldHeader->addWidget(payoutHoldIcon);
    m_ledgerPayoutHoldTitle = new QLabel;
    m_ledgerPayoutHoldTitle->setObjectName(
        QStringLiteral("ledgerPayoutHoldTitle"));
    m_ledgerPayoutHoldTitle->setTextFormat(Qt::PlainText);
    payoutHoldHeader->addWidget(m_ledgerPayoutHoldTitle, 1);
    payoutHoldLayout->addLayout(payoutHoldHeader);

    m_ledgerPayoutHoldDetails = new QLabel;
    m_ledgerPayoutHoldDetails->setObjectName(
        QStringLiteral("ledgerPayoutHoldDetails"));
    m_ledgerPayoutHoldDetails->setTextFormat(Qt::PlainText);
    m_ledgerPayoutHoldDetails->setWordWrap(true);
    payoutHoldLayout->addWidget(m_ledgerPayoutHoldDetails);

    auto *payoutHoldActions = new QHBoxLayout;
    payoutHoldActions->setSpacing(8);
    m_ledgerPayoutRetryButton =
        new QPushButton(QStringLiteral("Retry reconciliation"));
    m_ledgerPayoutRetryButton->setObjectName(
        QStringLiteral("ledgerPayoutRetryButton"));
    m_ledgerPayoutRetryButton->setProperty("buttonSize", "sm");
    m_ledgerPayoutRetryButton->setCursor(Qt::PointingHandCursor);
    m_ledgerPayoutRetryButton->setToolTip(QStringLiteral(
        "Send the same saved public signature to ForkMesh again. This never "
        "signs or broadcasts a replacement payment."));
    setOcticon(m_ledgerPayoutRetryButton, QStringLiteral("sync"), 13);
    connect(m_ledgerPayoutRetryButton, &QPushButton::clicked, this,
            &MainWindow::retryHeldLedgerPayout);
    payoutHoldActions->addWidget(m_ledgerPayoutRetryButton);

    m_ledgerPayoutExplorerButton =
        new QPushButton(QStringLiteral("View on Solana"));
    m_ledgerPayoutExplorerButton->setObjectName(
        QStringLiteral("ledgerPayoutExplorerButton"));
    m_ledgerPayoutExplorerButton->setProperty("buttonSize", "sm");
    m_ledgerPayoutExplorerButton->setCursor(Qt::PointingHandCursor);
    m_ledgerPayoutExplorerButton->setToolTip(QStringLiteral(
        "Inspect the exact saved transaction signature in the Solana explorer"));
    setOcticon(m_ledgerPayoutExplorerButton, QStringLiteral("link"), 13);
    connect(m_ledgerPayoutExplorerButton, &QPushButton::clicked, this,
            &MainWindow::openHeldLedgerPayoutInExplorer);
    payoutHoldActions->addWidget(m_ledgerPayoutExplorerButton);
    payoutHoldActions->addStretch();

    m_ledgerPayoutUndoButton = new QPushButton(
        QString::fromUtf8("Undo payment hold\xE2\x80\xA6"));
    m_ledgerPayoutUndoButton->setObjectName(
        QStringLiteral("ledgerPayoutUndoButton"));
    m_ledgerPayoutUndoButton->setProperty("buttonSize", "sm");
    m_ledgerPayoutUndoButton->setCursor(Qt::PointingHandCursor);
    m_ledgerPayoutUndoButton->setToolTip(QStringLiteral(
        "Remove only this desktop's saved hold after confirming the transfer "
        "did not land. This cannot reverse an on-chain payment."));
    setOcticon(m_ledgerPayoutUndoButton, QStringLiteral("x"), 13);
    connect(m_ledgerPayoutUndoButton, &QPushButton::clicked, this,
            &MainWindow::undoHeldLedgerPayout);
    payoutHoldActions->addWidget(m_ledgerPayoutUndoButton);
    payoutHoldLayout->addLayout(payoutHoldActions);
    payoutHold->hide();
    body->addWidget(payoutHold);

    auto *controls = new QHBoxLayout;
    m_ledgerPendingOnly =
        new QCheckBox(QStringLiteral("Only records awaiting payment"));
    m_ledgerPendingOnly->setObjectName(QStringLiteral("ledgerPendingOnly"));
    connect(m_ledgerPendingOnly, &QCheckBox::toggled, this,
            [this] { rebuildLedgerTable(); });
    controls->addWidget(m_ledgerPendingOnly);
    controls->addSpacing(18);
    // What the page adds up to, beside the filter that decides how much of it
    // is on screen. The totals are the whole page either way: hiding settled
    // rows must not quietly change what the ledger says was spent.
    m_ledgerTotals = new QLabel;
    m_ledgerTotals->setObjectName(QStringLiteral("ledgerTotals"));
    m_ledgerTotals->setTextFormat(Qt::PlainText);
    controls->addWidget(m_ledgerTotals);
    controls->addStretch();
    m_ledgerSummary = new QLabel;
    m_ledgerSummary->setObjectName(QStringLiteral("ledgerSummary"));
    m_ledgerSummary->setTextFormat(Qt::PlainText);
    controls->addWidget(m_ledgerSummary);
    body->addLayout(controls);

    QVBoxLayout *tableBlock = nullptr;
    QLineEdit *nextSearch = nullptr;
    if (next) {
        m_ledgerPendingOnly->hide();
        m_ledgerTotals->hide();
        m_ledgerSummary->hide();

        auto *stats = new QHBoxLayout;
        stats->setContentsMargins(0, 0, 0, 0);
        stats->setSpacing(12);
        stats->addWidget(makeLedgerNextStat(QStringLiteral("Expenses"),
                                            QStringLiteral("Expenses")), 1);
        stats->addWidget(makeLedgerNextStat(QStringLiteral("Income"),
                                            QStringLiteral("Income")), 1);
        stats->addWidget(makeLedgerNextStat(QStringLiteral("Records"),
                                            QStringLiteral("Records")), 1);
        stats->addWidget(makeLedgerNextStat(QStringLiteral("Awaiting"),
                                            QStringLiteral("Awaiting payment")),
                         1);
        body->addLayout(stats);

        tableBlock = new QVBoxLayout;
        tableBlock->setContentsMargins(0, 0, 0, 0);
        tableBlock->setSpacing(0);
        auto *toolbar = new QWidget;
        toolbar->setObjectName(QStringLiteral("ledgerNextToolbar"));
        toolbar->setAttribute(Qt::WA_StyledBackground, true);
        toolbar->setFixedHeight(44);
        auto *toolbarLayout = new QHBoxLayout(toolbar);
        toolbarLayout->setContentsMargins(10, 0, 14, 0);
        toolbarLayout->setSpacing(10);
        auto *filter = new kit::SegmentedControl(
            {QStringLiteral("All"), QStringLiteral("Awaiting payment")});
        filter->setProperty("ledgerRole", QStringLiteral("filter"));
        filter->setToolTip(m_ledgerPendingOnly->text());
        filter->setCurrentIndex(m_ledgerPendingOnly->isChecked() ? 1 : 0);
        filter->onChanged([this](int index) {
            if (m_ledgerPendingOnly)
                m_ledgerPendingOnly->setChecked(index == 1);
        });
        connect(m_ledgerPendingOnly, &QCheckBox::toggled, filter,
                [filter](bool on) { filter->setCurrentIndex(on ? 1 : 0); });
        toolbarLayout->addWidget(filter);
        toolbarLayout->addStretch(1);
        auto *search = new QLineEdit;
        search->setObjectName(QStringLiteral("ledgerNextSearch"));
        search->setPlaceholderText(QStringLiteral("Search records"));
        search->setClearButtonEnabled(true);
        search->setFixedWidth(224);
        search->setToolTip(QStringLiteral(
            "Show only the records whose date, record, title, person, amount, "
            "status or transaction contains this text. The totals still cover "
            "the whole page."));
        search->addAction(themedOcticon(QStringLiteral("search"),
                                        Theme::color(Theme::Role::FgSubtle), 14),
                          QLineEdit::LeadingPosition);
        toolbarLayout->addWidget(search);
        nextSearch = search;
        auto *price = new QLabel;
        price->setObjectName(QStringLiteral("ledgerNextPrice"));
        price->setTextFormat(Qt::PlainText);
        price->setToolTip(QStringLiteral(
            "The live SOL/USD spot price the USD estimates on this page use"));
        price->hide();
        toolbarLayout->addWidget(price);
        tableBlock->addWidget(toolbar);
    }

    m_ledgerTable = new QTableWidget(0, kLedgerColumnCount);
    m_ledgerTable->setObjectName(QStringLiteral("ledgerTable"));
    installColumnHeaderMenu(m_ledgerTable, /*showSortIndicator=*/true);
    m_ledgerTable->setHorizontalHeaderLabels(
        {QStringLiteral("When"), QStringLiteral("Record"),
         QStringLiteral("Title"), QStringLiteral("To"),
         QStringLiteral("Amount"), QStringLiteral("Status"),
         QStringLiteral("Transaction"), QString()});
    m_ledgerTable->verticalHeader()->setVisible(false);
    m_ledgerTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_ledgerTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_ledgerTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_ledgerTable->setShowGrid(false);
    m_ledgerTable->setWordWrap(false);
    m_ledgerTable->setTextElideMode(Qt::ElideMiddle);
    QHeaderView *ledgerHeader = m_ledgerTable->horizontalHeader();
    ledgerHeader->setStretchLastSection(false);
    for (int column = 0; column < kLedgerColumnCount; ++column)
        ledgerHeader->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    ledgerHeader->setSectionResizeMode(kLedgerRecordColumn,
                                       QHeaderView::Stretch);
    ledgerHeader->setSectionResizeMode(kLedgerTitleColumn,
                                       QHeaderView::Stretch);
    // ResizeToContents measures items, not cell widgets, so the Pay button
    // would be squeezed into a sliver of its own column.
    ledgerHeader->setSectionResizeMode(kLedgerActionColumn, QHeaderView::Fixed);
    m_ledgerTable->setColumnWidth(kLedgerActionColumn,
                                  kLedgerActionColumnWidth);
    // Every column reorders on a header click. Cells that stand for a value
    // sort on that value (LedgerCell), and rebuildLedgerTable re-applies the
    // reader's chosen order to each fresh page. Newest first to start with,
    // which is the order the relay publishes.
    m_ledgerTable->setSortingEnabled(true);
    m_ledgerTable->sortByColumn(kLedgerWhenColumn, Qt::DescendingOrder);
    // A signature is painted as a link, so it behaves like one: a single
    // click on the Transaction column opens that transfer on Solana, and the
    // cursor says so before the click. Every other column still selects its
    // row on a click and opens the row's subject on a double-click.
    m_ledgerTable->setMouseTracking(true);
    connect(m_ledgerTable, &QTableWidget::entered, this,
            [this](const QModelIndex &index) {
        const Qt::CursorShape want =
            index.column() == kLedgerTransactionColumn &&
                    index.data(kLedgerLinkUrlRole).toUrl().isValid()
                ? Qt::PointingHandCursor
                : Qt::ArrowCursor;
        if (m_ledgerTable->viewport()->cursor().shape() != want)
            m_ledgerTable->viewport()->setCursor(want);
    });
    connect(m_ledgerTable, &QTableWidget::cellClicked, this,
            [this](int row, int column) {
        if (column == kLedgerTransactionColumn)
            openLedgerLink(ledgerCellLink(row, column));
    });
    connect(m_ledgerTable, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int column) {
        // The Transaction column already opened on the first click of this
        // double-click. Following the row's subject as well would put two
        // pages in front of a reader who asked for one.
        if (column != kLedgerTransactionColumn)
            openLedgerLink(ledgerCellLink(row, kLedgerWhenColumn));
    });
    if (tableBlock) {
        m_ledgerTable->setProperty("ledgerNext", true);
        m_ledgerTable->setFrameShape(QFrame::NoFrame);
        m_ledgerTable->verticalHeader()->setDefaultSectionSize(
            kLedgerNextRowHeight);
        ledgerHeader->setFixedHeight(36);
        if (QTableWidgetItem *amountHeader =
                m_ledgerTable->horizontalHeaderItem(kLedgerAmountColumn))
            amountHeader->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_ledgerTable->setItemDelegate(new LedgerNextRowDelegate(m_ledgerTable));
        m_ledgerTable->setColumnHidden(kLedgerTitleColumn, true);
        if (QTableWidgetItem *counterpartyHeader =
                m_ledgerTable->horizontalHeaderItem(kLedgerCounterpartyColumn))
            counterpartyHeader->setText(QStringLiteral("To / from"));
        if (QLineEdit *search = nextSearch) {
            QTableWidget *table = m_ledgerTable;
            connect(search, &QLineEdit::textChanged, table,
                    [table](const QString &text) { applyLedgerSearch(table, text); });
            connect(table->model(), &QAbstractItemModel::layoutChanged, search,
                    [table, search] { applyLedgerSearch(table, search->text()); });
        }
        tableBlock->addWidget(m_ledgerTable, 1);
        body->addLayout(tableBlock, 1);
    } else {
        outer->addWidget(m_ledgerTable, 1);
    }

    m_ledgerStatus = new QLabel;
    m_ledgerStatus->setObjectName(QStringLiteral("ledgerStatus"));
    m_ledgerStatus->setWordWrap(true);
    body->addWidget(m_ledgerStatus);

    QLabel *payingHint = ledgerHint(QStringLiteral(
        "Paying a reward signs one Solana transfer with the wallet you linked "
        "in Settings and broadcasts it yourself. ForkMesh never holds the "
        "funds; it verifies the finalized transfer and records the same "
        "signature on the issue or pull request it paid for."));
    if (next)
        payingHint->setProperty("ledgerRole", QStringLiteral("footnote"));
    body->addWidget(payingHint);

    // Paint the two wallets this machine already knows about, and the
    // reader's own records, before the first relay answer: a ledger that
    // cannot be reached is exactly when someone wants to see whether their
    // wallet is funded and what their own book says.
    rebuildLedgerTable();
    setLedgerStatus(QStringLiteral("Loading the public ledger") +
                    QString::fromUtf8("\xE2\x80\xA6"));
    return page;
}

// Where a cell leads, for a row and column that may not exist: the table is
// rebuilt under every filter and sort, so a stale coordinate is ordinary.
QUrl MainWindow::ledgerCellLink(int row, int column) const
{
    QTableWidgetItem *item =
        m_ledgerTable ? m_ledgerTable->item(row, column) : nullptr;
    return item ? item->data(kLedgerLinkUrlRole).toUrl() : QUrl();
}

// The hand-off to the reader's browser, which is the one step an offscreen
// test cannot take. Which link a click follows is decided the same way in
// both builds; only this last move is recorded rather than made.
void MainWindow::openLedgerLink(const QUrl &url)
{
    if (!url.isValid())
        return;
#ifdef FORKMESH_WINDOW_TESTS
    m_ledgerLastOpenedLink = url;
#else
    QDesktopServices::openUrl(url);
#endif
}

void MainWindow::setLedgerStatus(const QString &message)
{
    if (!m_ledgerStatus)
        return;
    m_ledgerStatus->setStyleSheet(QString());
    m_ledgerStatus->setText(message);
    m_ledgerStatus->show();
}

void MainWindow::openLedgerInBrowser()
{
    if (!m_ledgerFeature) {
        showLocalNotice(m_ledgerStatus,
                        QStringLiteral(
                            "Ledger is not installed. Install it from Market %1 Library.")
                            .arg(QString::fromUtf8("\xE2\x86\x92")),
                        true);
        return;
    }
    if (!m_ledgerFeature->open(catalogApiUrl()))
        showLocalNotice(m_ledgerStatus,
                        QStringLiteral("Couldn't open the public Ledger."), true);
}

void MainWindow::refreshLedgerPage(bool force)
{
    if (!m_ledgerTable)
        return;
    // Opening the Ledger is the moment this app is most likely to be both
    // signed in and looking at the rewards that owe a notice. Sending them
    // here costs one request, signs nothing, and is what keeps a payout that
    // settled while signed out from staying silent indefinitely.
    flushSettledContributorNotices();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!force && m_ledgerRecordsLoaded && m_ledgerLoadedAtMs > 0 &&
        now - m_ledgerLoadedAtMs < kLedgerFreshMs) {
        rebuildLedgerTable();
        return;
    }
    if (m_ledgerRecordsInFlight || m_ledgerPayableInFlight)
        return;
    fetchLedgerRecords(force);
    fetchLedgerPayableRewards();
    refreshLedgerLocalWalletBalances(force);
}

void MainWindow::fetchLedgerRecords(bool refreshBalances)
{
    if (!m_networkAccess) {
        setLedgerStatus(QStringLiteral(
            "Network access is unavailable, so the ledger cannot be read."));
        return;
    }
    const QUrl endpoint = LedgerFeature::apiUrlFor(
        catalogApiUrl(), kLedgerPageSize, refreshBalances);
    if (!endpoint.isValid()) {
        setLedgerStatus(QStringLiteral(
            "This host does not publish a readable ledger endpoint."));
        return;
    }
    m_ledgerRecordsInFlight = true;
    if (m_ledgerRefreshButton)
        m_ledgerRefreshButton->setEnabled(false);
    setLedgerStatus(QStringLiteral("Loading the public ledger") +
                    QString::fromUtf8("\xE2\x80\xA6"));
    QNetworkRequest request(endpoint);
    hardenLedgerRequest(request);
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, endpoint] {
        const QByteArray body = boundedLedgerBody(reply);
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        m_ledgerRecordsInFlight = false;
        if (m_ledgerRefreshButton)
            m_ledgerRefreshButton->setEnabled(true);
        LedgerPage page;
        QString error;
        if (!networkOk ||
            !LedgerFeature::parseLedgerPage(body, endpoint, &page, &error)) {
            setLedgerStatus(
                status > 0
                    ? QStringLiteral("The ledger could not be read (HTTP %1).")
                          .arg(status)
                    : (error.isEmpty()
                           ? QStringLiteral("The ledger could not be reached.")
                           : error));
            return;
        }
        m_ledgerPage = page;
        m_ledgerRecordsLoaded = true;
        m_ledgerLoadedAtMs = QDateTime::currentMSecsSinceEpoch();
        rebuildLedgerTable();
    });
}

void MainWindow::fetchLedgerPayableRewards()
{
    m_ledgerPayable.clear();
    m_ledgerPayableLoaded = false;
    if (!m_networkAccess)
        return;
    // Deliberately the reward-pool helper rather than the catalog URL: a
    // payout destination may only come from a trusted HTTPS relay this app is
    // actually connected to. Without one the page still reads, and simply
    // offers no Pay button.
    const QUrl endpoint =
        rewardPoolWorkerEndpoint(QStringLiteral("/api/rewards/estimates"));
    if (!endpoint.isValid())
        return;
    m_ledgerPayableInFlight = true;
    QNetworkRequest request(endpoint);
    hardenLedgerRequest(request);
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const QByteArray body = boundedLedgerBody(reply);
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        m_ledgerPayableInFlight = false;
        if (!networkOk) {
            rebuildLedgerTable();
            return;
        }
        m_ledgerPayable = parsePayableRewards(body);
        m_ledgerPayableLoaded = true;
        rebuildLedgerTable();
        fetchLedgerReminderTimes(body);
    });
}

void MainWindow::fetchLedgerReminderTimes(const QByteArray &publicEstimatesBody)
{
    if (!m_networkAccess || m_ledgerReminderTimesInFlight > 0)
        return;
    // Only repositories that owe someone a payout address are asked about,
    // and only a few per load: each is one owner-authenticated read.
    static const QRegularExpression repositoryPattern(QStringLiteral(
        "^[a-z0-9][a-z0-9_.-]{0,99}/[a-z0-9][a-z0-9_.-]{0,99}$"));
    QStringList repositories;
    const QJsonObject response =
        QJsonDocument::fromJson(publicEstimatesBody).object();
    for (const QJsonValue &value :
         response.value(QStringLiteral("rewards")).toArray()) {
        const QJsonObject reward = value.toObject();
        if (reward.value(QStringLiteral("status")).toString() !=
            QLatin1String("pending_wallet"))
            continue;
        const QString repository = reward.value(QStringLiteral("repository"))
                                       .toString()
                                       .trimmed()
                                       .toLower();
        if (repositoryPattern.match(repository).hasMatch() &&
            !repositories.contains(repository))
            repositories.append(repository);
        if (repositories.size() >= 3)
            break;
    }
    for (const QString &repository : repositories) {
        QUrl endpoint =
            rewardPoolWorkerEndpoint(QStringLiteral("/api/rewards/estimates"));
        if (!endpoint.isValid())
            return;
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("ownerScope"), QStringLiteral("1"));
        query.addQueryItem(QStringLiteral("repository"), repository);
        endpoint.setQuery(query);
        const QString sessionToken = accountSessionTokenForUrl(endpoint);
        // Signed out: the Remind button simply shows no time.
        if (sessionToken.isEmpty())
            return;
        QNetworkRequest request(endpoint);
        hardenLedgerRequest(request);
        request.setRawHeader("Authorization",
                             QByteArrayLiteral("Bearer ") +
                                 sessionToken.toUtf8());
        ++m_ledgerReminderTimesInFlight;
        QNetworkReply *reply = m_networkAccess->get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            const QByteArray body = boundedLedgerBody(reply);
            const bool networkOk = reply->error() == QNetworkReply::NoError;
            reply->deleteLater();
            m_ledgerReminderTimesInFlight =
                qMax(0, m_ledgerReminderTimesInFlight - 1);
            // Not this repository's owner, or an older relay: no times, and
            // nothing else on the page changes.
            if (networkOk)
                applyLedgerReminderTimes(body);
        });
    }
}

void MainWindow::applyLedgerReminderTimes(const QByteArray &ownerEstimatesBody)
{
    const QJsonObject response =
        QJsonDocument::fromJson(ownerEstimatesBody).object();
    if (!response.value(QStringLiteral("ok")).toBool() ||
        response.value(QStringLiteral("authorization")).toString() !=
            QLatin1String("owner-session"))
        return;
    bool changed = false;
    for (const QJsonValue &value :
         response.value(QStringLiteral("rewards")).toArray()) {
        const QJsonObject reward = value.toObject();
        const QString recipient = reward.value(QStringLiteral("recipient"))
                                      .toString()
                                      .trimmed()
                                      .toLower();
        const double sentAt =
            reward.value(QStringLiteral("walletReminderSentAt")).toDouble(0);
        if (recipient.isEmpty() || !std::isfinite(sentAt) || sentAt <= 0 ||
            sentAt > 9.0e15)
            continue;
        const qint64 stamp = qint64(sentAt);
        if (stamp > m_ledgerReminderSentAt.value(recipient, 0)) {
            m_ledgerReminderSentAt.insert(recipient, stamp);
            changed = true;
        }
    }
    if (changed)
        rebuildLedgerTable();
}

// The payout address published on the account signed in here. It is what a
// tip or reward from anyone else arrives in, and it is not the same wallet as
// the one this machine signs with.
QString MainWindow::ledgerMemberPayoutAddress() const
{
    const QString account = topBarUserName().trimmed().toLower();
    const QString published =
        m_webSolanaKnown && m_webSolanaAccount == account
            ? m_webSolanaAddress
            : savedSolanaAddress();
    return published.trimmed();
}

void MainWindow::rebuildLedgerWalletStrip()
{
    if (!m_ledgerWalletStrip)
        return;
    auto *layout = qobject_cast<QHBoxLayout *>(m_ledgerWalletStrip->layout());
    if (!layout)
        return;

    QList<LedgerWalletCard> cards;

    LedgerWalletCard mine;
    mine.label = QStringLiteral("Your wallet");
    mine.purpose = QStringLiteral(
        "The public payout address published on your ForkMesh profile. Tips "
        "and rewards other people send you arrive here.");
    mine.address = ledgerMemberPayoutAddress();
    mine.emptyText = QStringLiteral("Not published");
    mine.emptyPurpose = QStringLiteral(
        "Publish a public payout address in Settings so tips and rewards can "
        "reach you.");
    if (Theme::isNext()) {
        mine.setUpText = QStringLiteral("Publish address");
        mine.onSetUp = [this] {
            openAccountManagement();
            QTimer::singleShot(0, this, [this] {
                if (m_settingsSolanaEdit && m_settingsSolanaEdit->isVisible())
                    m_settingsSolanaEdit->setFocus(Qt::OtherFocusReason);
            });
        };
    }
    cards.append(mine);

    LedgerWalletCard paying;
    paying.label = QStringLiteral("Payment wallet");
    paying.purpose = QStringLiteral(
        "The wallet linked to this app. Every tip and reward you pay is "
        "signed locally from it; its key never leaves this machine.");
    paying.address =
        forkmesh::tips::linkedTipAddress(forkmesh::tips::defaultTipVaultPath())
            .trimmed();
    paying.emptyText = QStringLiteral("Not linked");
    paying.emptyPurpose = QStringLiteral(
        "Link a Solana wallet in Settings to pay a reward from this page.");
    paying.signsPayments = true;
    if (Theme::isNext()) {
        paying.setUpText = QStringLiteral("Link wallet");
        paying.setUpIcon = QStringLiteral("link");
        paying.onSetUp = [this] {
            openAccountManagement();
            QTimer::singleShot(0, this, [this] {
                if (m_profileTipLinkButton && m_profileTipLinkButton->isVisible())
                    m_profileTipLinkButton->setFocus(Qt::OtherFocusReason);
            });
        };
    }
    cards.append(paying);

    for (const LedgerWallet &wallet : m_ledgerPage.wallets) {
        LedgerWalletCard card;
        card.label = wallet.label;
        card.purpose = wallet.purpose;
        card.address = wallet.address;
        card.explorerUrl = wallet.explorerUrl;
        card.sharedAddress = wallet.sharedAddress;
        card.lamports =
            wallet.balanceKnown ? qint64(wallet.balanceLamports) : -1;
        card.checkedAt = wallet.balanceCheckedAt;
        cards.append(card);
    }

    // Addresses a reader typed into the form above, to watch and fund a
    // wallet the relay does not otherwise name on this page.
    for (const QString &address : m_ledgerManualWallets) {
        LedgerWalletCard card;
        card.label = QStringLiteral("Tracked wallet");
        card.purpose = QStringLiteral(
            "A Solana address entered manually on this machine, to watch its "
            "balance and share a funding QR code from this page.");
        card.address = address;
        card.onRemove = [this, address] {
            m_ledgerManualWallets.removeAll(address);
            rebuildLedgerWalletStrip();
        };
        cards.append(card);
    }

    // The relay reads the system wallets for us; the rest are addresses only
    // this machine can name, so their readings are cached here. A card the
    // reader re-read from this page takes over from the relay's copy only
    // while its own reading is the newer of the two.
    for (LedgerWalletCard &card : cards) {
        if (card.address.isEmpty())
            continue;
        const auto reading = m_ledgerWalletBalances.value(card.address,
                                                          {-1, 0});
        if (reading.first >= 0 &&
            (card.lamports < 0 || reading.second > card.checkedAt)) {
            card.lamports = reading.first;
            card.checkedAt = reading.second;
        }
        if (!card.explorerUrl.isValid()) {
            card.explorerUrl = QUrl(
                QStringLiteral("https://explorer.solana.com/address/") +
                QString::fromUtf8(QUrl::toPercentEncoding(card.address)));
        }
        if (card.lamports >= 0) {
            card.usdMicros = LedgerFeature::usdMicrosForLamports(
                quint64(card.lamports), m_ledgerPage.solUsdPrice);
        }
        const QString address = card.address;
        card.onRefresh = [this, address] {
            fetchLedgerWalletBalance(address, /*force=*/true);
        };
    }

    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    for (const LedgerWalletCard &card : cards)
        layout->addWidget(Theme::isNext() ? makeLedgerNextWalletCard(card)
                                          : makeLedgerWalletCard(card),
                          1);
    m_ledgerWalletStrip->setVisible(!cards.isEmpty());
}

// A reader-supplied address gets a card of its own, same as the wallets this
// desktop already knows about, including its own funding QR code.
void MainWindow::trackLedgerWallet(const QString &address)
{
    const QString clean = address.trimmed();
    if (clean.isEmpty() ||
        forkmesh::rewards::decodeBase58(clean, 32).size() != 32)
        return;
    if (m_ledgerManualWallets.contains(clean))
        return;
    // A second card for an address the strip already names is a duplicate
    // balance, not a wallet the reader gained.
    QStringList shown{ledgerMemberPayoutAddress(),
                      forkmesh::tips::linkedTipAddress(
                          forkmesh::tips::defaultTipVaultPath())
                          .trimmed()};
    for (const LedgerWallet &wallet : m_ledgerPage.wallets)
        shown << wallet.address;
    if (shown.contains(clean))
        return;
    m_ledgerManualWallets.append(clean);
    fetchLedgerWalletBalance(clean, false);
    rebuildLedgerWalletStrip();
}

// The entry form above the wallet strip. Everything it accepts is one record
// in the reader's own book: a date, what it was, who it was with, and an
// exact amount in SOL or dollars. Nothing here is signed, sent or published.
void MainWindow::addLedgerEntry()
{
    if (!m_ledgerEntryDescription || !m_ledgerEntryAmount ||
        !m_ledgerEntryDirection || !m_ledgerEntryCurrency ||
        !m_ledgerEntryDate || !m_ledgerEntryCounterparty)
        return;
    const QString counterparty = m_ledgerEntryCounterparty->text().trimmed();
    const bool isAddress =
        forkmesh::rewards::decodeBase58(counterparty, 32).size() == 32;
    const QString description = m_ledgerEntryDescription->text().trimmed();
    const QString amountText = m_ledgerEntryAmount->text().trimmed();

    // The form this replaced took an address and nothing else, and that is
    // still the fastest way to watch a wallet: an address on its own tracks
    // it rather than being refused for the fields a record would need.
    if (isAddress && description.isEmpty() && amountText.isEmpty()) {
        m_ledgerEntryCounterparty->clear();
        trackLedgerWallet(counterparty);
        const QString shown = ledgerAbbreviatedAddress(counterparty);
        setLedgerStatus(
            m_ledgerManualWallets.contains(counterparty)
                ? QStringLiteral("Tracking %1. Its balance is read from the "
                                 "relay's shared hourly cache.")
                      .arg(shown)
                : QStringLiteral("%1 already has a card on this page.")
                      .arg(shown));
        return;
    }
    if (description.isEmpty()) {
        showLocalNotice(m_ledgerStatus,
                        QStringLiteral("Say what the record is before adding it."),
                        true);
        m_ledgerEntryDescription->setFocus();
        return;
    }
    const QString currency =
        m_ledgerEntryCurrency->currentData().toString();
    // Lamports for SOL, micros for dollars, and no rounding of either: a
    // figure this page cannot hold exactly is refused rather than filed wrong.
    const bool isSol = currency == QLatin1String("SOL");
    qint64 amount = 0;
    if (!LedgerFeature::parseDecimalAmount(amountText, isSol ? 9 : 2,
                                           &amount) ||
        amount <= 0) {
        showLocalNotice(m_ledgerStatus,
                        isSol ? QStringLiteral(
                                    "Enter the amount in SOL, to at most nine "
                                    "decimal places.")
                              : QStringLiteral(
                                    "Enter the amount in dollars and cents."),
                        true);
        m_ledgerEntryAmount->setFocus();
        return;
    }
    if (!isSol)
        amount *= 10000; // cents to the micros the ledger adds up in

    loadLocalLedgerRecords();
    const LedgerRecord record = LedgerFeature::localRecord(
        QUuid::createUuid().toString(QUuid::WithoutBraces),
        m_ledgerEntryDirection->currentData().toString(),
        m_ledgerEntryOutstanding && m_ledgerEntryOutstanding->isChecked()
            ? QStringLiteral("outstanding")
            : QStringLiteral("settled"),
        m_ledgerEntryCategory ? m_ledgerEntryCategory->text() : QString(),
        description, counterparty, currency, amount,
        m_ledgerEntryDate->date().startOfDay().toMSecsSinceEpoch());
    if (record.id.isEmpty()) {
        showLocalNotice(m_ledgerStatus,
                        QStringLiteral("That record could not be added."), true);
        return;
    }
    m_ledgerLocalRecords.append(record);
    saveLocalLedgerRecords();

    m_ledgerEntryDescription->clear();
    m_ledgerEntryAmount->clear();
    m_ledgerEntryCounterparty->clear();
    if (m_ledgerEntryOutstanding)
        m_ledgerEntryOutstanding->setChecked(false);
    // The kind and the date stay: a reader entering a month of receipts is
    // entering the same kind of thing over and over.
    if (isAddress)
        trackLedgerWallet(counterparty);
    m_ledgerPaymentLine.clear(); // this action owns the status line now
    rebuildLedgerTable();
    setLedgerStatus(
        QStringLiteral("Added \"%1\" to your own records. It is stored on "
                       "this machine and counted in the totals above.")
            .arg(record.subjectTitle));
}

void MainWindow::sendLedgerPayment()
{
    // A payment already in flight is no reason to wait: this one queues.
    if (m_ledgerRecipientLookupInFlight)
        return;
    if (!m_networkAccess) {
        setLedgerStatus(QStringLiteral(
            "Connect to ForkMesh before sending a Ledger payment."));
        return;
    }
    const QString wallet = forkmesh::tips::linkedTipAddress(
        forkmesh::tips::defaultTipVaultPath()).trimmed();
    if (!forkmesh::tips::isValidTipAddress(wallet)) {
        if (QMessageBox::question(
                this, QStringLiteral("Link a payment wallet first"),
                QStringLiteral(
                    "Ledger payments are signed on this machine with a "
                    "Solana wallet you link yourself. Link one now?"),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::Yes) == QMessageBox::Yes) {
            linkTipWallet();
        }
        return;
    }
    const QUrl noticeEndpoint = accountsApiUrl(QStringLiteral("tip-notice"));
    const QString owner = accountOwner().trimmed().toLower();
    const bool canSignNotice =
        !owner.isEmpty() && hasOwnerSigningCapability(owner) &&
        (m_profileIdentity.isValid() || m_profileIdentity.load());
    if (accountSessionTokenForUrl(noticeEndpoint).isEmpty() && !canSignNotice) {
        setLedgerStatus(QStringLiteral(
            "Sign in to ForkMesh before sending a Ledger payment. The app "
            "must be able to attach the finalized receipt to your account."));
        return;
    }

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("ledgerSendPaymentDialog"));
    dialog.setWindowTitle(QStringLiteral("Send a work payment"));
    dialog.resize(660, 520);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 20, 20, 16);
    layout->setSpacing(12);
    auto *intro = ledgerHint(QStringLiteral(
        "Pay a ForkMesh username at the public Solana address on their "
        "profile. The task, work-product link, exact amount, and finalized "
        "transaction will appear together in the public Ledger."));
    layout->addWidget(intro);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setVerticalSpacing(10);
    auto *recipientEdit = new QLineEdit;
    recipientEdit->setObjectName(QStringLiteral("ledgerPaymentRecipient"));
    recipientEdit->setPlaceholderText(QStringLiteral("username"));
    recipientEdit->setMaxLength(63);
    auto *recipientModel = new QStringListModel(
        ledgerRecipientCandidates(), &dialog);
    auto *recipientCompleter = new QCompleter(recipientModel, &dialog);
    recipientCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    recipientCompleter->setFilterMode(Qt::MatchContains);
    recipientCompleter->setCompletionMode(QCompleter::PopupCompletion);
    recipientCompleter->setMaxVisibleItems(12);
    recipientEdit->setCompleter(recipientCompleter);
    connect(this, &MainWindow::chatUserDirectoryChanged, &dialog,
            [this, recipientModel] {
                recipientModel->setStringList(ledgerRecipientCandidates());
            });
    refreshChatUserDirectory();
    form->addRow(QStringLiteral("Recipient @"), recipientEdit);
    auto *taskEdit = new QTextEdit;
    taskEdit->setObjectName(QStringLiteral("ledgerPaymentTask"));
    taskEdit->setPlaceholderText(QStringLiteral(
        "Describe the completed task and what this payment covers"));
    taskEdit->setAcceptRichText(false);
    taskEdit->setMaximumHeight(100);
    form->addRow(QStringLiteral("Task"), taskEdit);
    auto *outputEdit = new QLineEdit;
    outputEdit->setObjectName(QStringLiteral("ledgerPaymentOutputUrl"));
    outputEdit->setPlaceholderText(
        QStringLiteral("https://… link to the completed work"));
    outputEdit->setMaxLength(1000);
    form->addRow(QStringLiteral("Work product"), outputEdit);

    auto *amountRow = new QWidget;
    auto *amountLayout = new QHBoxLayout(amountRow);
    amountLayout->setContentsMargins(0, 0, 0, 0);
    amountLayout->setSpacing(8);
    auto *amountEdit = new QLineEdit(QStringLiteral("0.01"));
    amountEdit->setObjectName(QStringLiteral("ledgerPaymentAmount"));
    amountEdit->setPlaceholderText(QStringLiteral("0.00"));
    amountEdit->setMaxLength(32);
    amountLayout->addWidget(amountEdit, 1);
    auto *currency = new QComboBox;
    currency->setObjectName(QStringLiteral("ledgerPaymentCurrency"));
    currency->addItem(QStringLiteral("SOL"), QStringLiteral("SOL"));
    currency->addItem(QStringLiteral("USD"), QStringLiteral("USD"));
    amountLayout->addWidget(currency);
    form->addRow(QStringLiteral("Amount"), amountRow);
    auto *conversion = new QLabel(
        QStringLiteral("Loading the SOL/USD conversion…"));
    conversion->setObjectName(QStringLiteral("ledgerPaymentConversion"));
    conversion->setWordWrap(true);
    form->addRow(QStringLiteral("You will send"), conversion);
    layout->addLayout(form);

    auto *problem = new QLabel;
    problem->setObjectName(QStringLiteral("statusLine"));
    problem->setWordWrap(true);
    problem->hide();
    layout->addWidget(problem);
    layout->addStretch(1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                         QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(
        QStringLiteral("Review payment"));
    buttons->button(QDialogButtonBox::Ok)->setObjectName(
        QStringLiteral("primaryButton"));
    layout->addWidget(buttons);

    double solUsdPrice = 0.0;
    qint64 priceObservedAt = 0;
    const auto convertedLamports =
        [amountEdit, currency, &solUsdPrice](quint64 *lamports,
                                             qint64 *usdMicros,
                                             QString *error) {
        if (lamports)
            *lamports = 0;
        if (usdMicros)
            *usdMicros = 0;
        if (currency->currentData().toString() == QLatin1String("SOL")) {
            return forkmesh::tips::parseSolAmount(amountEdit->text(), lamports,
                                                   error);
        }
        qint64 cents = 0;
        if (!LedgerFeature::parseDecimalAmount(amountEdit->text(), 2, &cents) ||
            cents <= 0 ||
            cents > std::numeric_limits<qint64>::max() / 10000) {
            if (error)
                *error = QStringLiteral(
                    "Enter a positive USD amount in dollars and cents.");
            return false;
        }
        if (!(solUsdPrice > 0.0) || !std::isfinite(solUsdPrice)) {
            if (error)
                *error = QStringLiteral(
                    "A current SOL/USD quote is required to enter USD.");
            return false;
        }
        const qint64 micros = cents * 10000;
        const long double converted =
            (static_cast<long double>(micros) * 1000000000.0L) /
            std::llround(solUsdPrice * 1000000.0);
        if (!(converted > 0.0L) ||
            converted > static_cast<long double>(
                            forkmesh::tips::maximumTipLamports())) {
            if (error)
                *error = QStringLiteral("That USD amount is outside the "
                                        "supported payment range.");
            return false;
        }
        const quint64 rounded = quint64(std::llround(converted));
        if (rounded < forkmesh::tips::minimumTipLamports() ||
            rounded > forkmesh::tips::maximumTipLamports()) {
            if (error)
                *error = QStringLiteral("That USD amount converts outside the "
                                        "supported payment range.");
            return false;
        }
        if (lamports)
            *lamports = rounded;
        if (usdMicros)
            *usdMicros = micros;
        return true;
    };
    const auto refreshConversion = [=, &solUsdPrice] {
        quint64 lamports = 0;
        qint64 usdMicros = 0;
        QString error;
        if (!convertedLamports(&lamports, &usdMicros, &error)) {
            conversion->setText(error);
            return;
        }
        QString text = forkmesh::tips::formatLamports(lamports);
        if (currency->currentData().toString() == QLatin1String("SOL") &&
            solUsdPrice > 0.0) {
            const qint64 estimate = qint64(std::llround(
                (static_cast<long double>(lamports) / 1000000000.0L) *
                solUsdPrice * 1000000.0L));
            text += QStringLiteral(" (about %1)")
                        .arg(LedgerFeature::usdText(estimate));
        }
        conversion->setText(text);
    };
    connect(amountEdit, &QLineEdit::textChanged, &dialog,
            [refreshConversion] { refreshConversion(); });
    connect(currency, &QComboBox::currentIndexChanged, &dialog,
            [refreshConversion](int) { refreshConversion(); });

    QNetworkReply *priceReply = nullptr;
    if (m_networkAccess) {
        QNetworkRequest priceRequest{
            QUrl(QString::fromLatin1(kLedgerSolUsdPriceEndpoint))};
        hardenLedgerRequest(priceRequest);
        priceReply = m_networkAccess->get(priceRequest);
        connect(priceReply, &QNetworkReply::finished, &dialog,
                [priceReply, &solUsdPrice, &priceObservedAt,
                 refreshConversion] {
            const QByteArray body = boundedLedgerBody(priceReply);
            const QJsonObject root = QJsonDocument::fromJson(body).object();
            const double price = root.value(QStringLiteral("solana"))
                                     .toObject()
                                     .value(QStringLiteral("usd"))
                                     .toDouble();
            if (priceReply->error() == QNetworkReply::NoError &&
                std::isfinite(price) && price > 0.0 &&
                price <= 1000000000.0) {
                solUsdPrice = price;
                priceObservedAt = QDateTime::currentMSecsSinceEpoch();
            }
            priceReply->deleteLater();
            refreshConversion();
        });
        connect(&dialog, &QDialog::finished, priceReply, [priceReply] {
            if (priceReply->isRunning())
                priceReply->abort();
        });
    }

    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        static const QRegularExpression usernamePattern(
            QStringLiteral("^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$"));
        const QString recipient = recipientEdit->text().trimmed().toLower();
        const QString task = taskEdit->toPlainText().simplified();
        const QUrl output(outputEdit->text().trimmed(), QUrl::StrictMode);
        quint64 lamports = 0;
        qint64 usdMicros = 0;
        QString error;
        if (!usernamePattern.match(recipient).hasMatch())
            error = QStringLiteral("Enter a valid ForkMesh username.");
        else if (task.isEmpty() || task.size() > 360)
            error = QStringLiteral(
                "Describe the completed task in 360 characters or fewer.");
        else if (!output.isValid() || output.host().isEmpty() ||
                 !output.userInfo().isEmpty() ||
                 (output.scheme() != QLatin1String("https") &&
                  output.scheme() != QLatin1String("http")))
            error = QStringLiteral(
                "Enter a complete HTTPS or HTTP link to the work product.");
        else if (!convertedLamports(&lamports, &usdMicros, &error)) {
        }
        if (!error.isEmpty()) {
            problem->setText(error);
            problem->show();
            return;
        }
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString recipient = recipientEdit->text().trimmed().toLower();
    const QString task = taskEdit->toPlainText().simplified();
    const QUrl output(outputEdit->text().trimmed(), QUrl::StrictMode);
    quint64 lamports = 0;
    qint64 usdMicros = 0;
    QString conversionError;
    if (!convertedLamports(&lamports, &usdMicros, &conversionError))
        return;
    const QString enteredCurrency = currency->currentData().toString();
    const qint64 priceMicros =
        enteredCurrency == QLatin1String("USD")
            ? qint64(std::llround(solUsdPrice * 1000000.0))
            : 0;

    m_ledgerRecipientLookupInFlight = true;
    if (!m_tipInFlight) {
        m_ledgerPaymentReporting = true;
        m_ledgerPaymentLine.clear();
    }
    updateLedgerPayButtons();
    setLedgerStatus(QStringLiteral("Looking up @%1's payout address…")
                        .arg(recipient));
    QUrl profileEndpoint = accountsApiUrl(recipient);
    QNetworkRequest profileRequest(profileEndpoint);
    hardenLedgerRequest(profileRequest);
    QNetworkReply *reply = m_networkAccess->get(profileRequest);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, recipient, task, output, lamports, usdMicros,
             enteredCurrency, priceMicros, priceObservedAt, wallet] {
        const QByteArray body = boundedLedgerBody(reply);
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        m_ledgerRecipientLookupInFlight = false;
        updateLedgerPayButtons();
        const QJsonObject profile = QJsonDocument::fromJson(body).object();
        const QString address =
            profile.value(QStringLiteral("solana")).toString().trimmed();
        if (!networkOk || status != 200 ||
            !profile.value(QStringLiteral("exists")).toBool()) {
            setLedgerStatus(QStringLiteral(
                "@%1 is not an active ForkMesh user, so nothing was signed.")
                                .arg(recipient));
            return;
        }
        if (!forkmesh::tips::isValidTipAddress(address)) {
            setLedgerStatus(QStringLiteral(
                "@%1 has not published a valid Solana payout address, so "
                "nothing was signed.")
                                .arg(recipient));
            return;
        }
        const QString entered =
            enteredCurrency == QLatin1String("USD")
                ? LedgerFeature::usdText(usdMicros)
                : forkmesh::tips::formatLamports(lamports);
        const QString confirmation = QStringLiteral(
            "Pay @%1 %2 (%3) from %4?\n\nTask:\n%5\n\nWork product:\n%6\n\n"
            "Recipient address:\n%7\n\nThe Solana transfer cannot be reversed. "
            "ForkMesh will publish the task, work link, amount, and finalized "
            "transaction in the Ledger.")
            .arg(recipient, forkmesh::tips::formatLamports(lamports), entered,
                 wallet, task, output.toString(QUrl::FullyEncoded), address);
        if (QMessageBox::question(
                this, QStringLiteral("Confirm work payment"), confirmation,
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) != QMessageBox::Yes) {
            setLedgerStatus(QStringLiteral(
                "Payment cancelled; nothing was signed."));
            return;
        }
        TipTransferRequest request;
        request.recipientName = recipient;
        request.recipientAddress = address;
        request.lamports = lamports;
        request.reportToLedger = true;
        request.taskDescription = task;
        request.outputUrl = output;
        request.enteredCurrency = enteredCurrency;
        request.enteredUsdMicros =
            enteredCurrency == QLatin1String("USD") ? usdMicros : 0;
        request.solUsdPriceMicros = priceMicros;
        request.priceObservedAt =
            enteredCurrency == QLatin1String("USD") ? priceObservedAt : 0;
        // Behind another transfer this is signed minutes from now, when
        // nobody may be watching, so the passphrase is asked for while the
        // reader is still here rather than when its turn comes.
        if (m_tipInFlight &&
            (!promptTipPassphrase(recipient, lamports, &request.passphrase) ||
             request.passphrase.isEmpty())) {
            forkmesh::rewards::secureErase(request.passphrase);
            setLedgerStatus(
                QStringLiteral("Payment cancelled; nothing was signed."));
            return;
        }
        beginOrQueueTipTransfer(std::move(request));
    });
}

QStringList MainWindow::ledgerRecipientCandidates() const
{
    static const QRegularExpression usernamePattern(
        QStringLiteral("^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$"));
    const QString ownName = accountOwner().trimmed().toLower();
    QSet<QString> seen;
    QStringList names;
    names.reserve(m_chatDirectoryUsers.size());
    for (const MemberInfo &member : m_chatDirectoryUsers) {
        const QString name = member.name.trimmed().toLower();
        if (name.isEmpty() || name == ownName || seen.contains(name) ||
            !usernamePattern.match(name).hasMatch()) {
            continue;
        }
        seen.insert(name);
        names.append(name);
    }
    names.sort(Qt::CaseInsensitive);
    return names;
}

void MainWindow::settleLocalLedgerRecord(const QString &recordId)
{
    m_ledgerPaymentLine.clear(); // this action owns the status line now
    // Rebuilding the table destroys the very button whose click is still on
    // the stack, exactly as in payLedgerRecord. Do the work on the next turn.
    QTimer::singleShot(0, this, [this, recordId] {
        loadLocalLedgerRecords();
        for (LedgerRecord &record : m_ledgerLocalRecords) {
            if (record.id != recordId)
                continue;
            record.status = QStringLiteral("settled");
            saveLocalLedgerRecords();
            rebuildLedgerTable();
            setLedgerStatus(QStringLiteral("Marked \"%1\" as settled.")
                                .arg(record.subjectTitle));
            return;
        }
    });
}

void MainWindow::removeLocalLedgerRecord(const QString &recordId)
{
    m_ledgerPaymentLine.clear(); // this action owns the status line now
    QTimer::singleShot(0, this, [this, recordId] {
        loadLocalLedgerRecords();
        const qsizetype removed = m_ledgerLocalRecords.removeIf(
            [&recordId](const LedgerRecord &record) {
                return record.id == recordId;
            });
        if (removed <= 0)
            return;
        saveLocalLedgerRecords();
        rebuildLedgerTable();
        setLedgerStatus(QStringLiteral("Removed that record from your book. "
                                       "The relay's own records are "
                                       "untouched."));
    });
}

void MainWindow::loadLocalLedgerRecords()
{
    if (m_ledgerLocalRecordsLoaded)
        return;
    m_ledgerLocalRecordsLoaded = true;
    m_ledgerLocalRecords = LedgerFeature::parseLocalRecords(
        QSettings().value(ledgerLocalRecordsKey()).toByteArray());
}

void MainWindow::saveLocalLedgerRecords()
{
    QSettings().setValue(ledgerLocalRecordsKey(),
                         LedgerFeature::serializeLocalRecords(
                             m_ledgerLocalRecords));
}

void MainWindow::refreshLedgerLocalWalletBalances(bool force)
{
    QStringList addresses{
        ledgerMemberPayoutAddress(),
        forkmesh::tips::linkedTipAddress(forkmesh::tips::defaultTipVaultPath())
            .trimmed(),
    };
    addresses.removeDuplicates();
    for (const QString &address : addresses) {
        if (address.isEmpty())
            continue;
        fetchLedgerWalletBalance(address, force);
    }
}

void MainWindow::fetchLedgerWalletBalance(const QString &address, bool force)
{
    if (!m_networkAccess || address.isEmpty())
        return;
    if (m_ledgerWalletBalanceFetches.contains(address)) {
        // A completed payment must supersede any page-opening read that was
        // already under way. That reply can only describe the wallet before (or
        // during) the transfer, so remember one forced follow-up instead of
        // dropping the completion refresh on the floor.
        if (force)
            m_ledgerWalletBalanceRefreshQueued.insert(address);
        return;
    }
    // The relay's own reading is hourly, so re-asking inside that window can
    // only return the same number. Refresh still asks: the relay floors a
    // forced read to one a minute per address on its side.
    const QPair<qint64, qint64> reading =
        m_ledgerWalletBalances.value(address, {-1, 0});
    if (!force && reading.first >= 0 && reading.second > 0 &&
        QDateTime::currentMSecsSinceEpoch() - reading.second <
            kLedgerWalletBalanceTtlMs)
        return;
    // Deliberately the relay's public per-address balance endpoint rather than
    // a public Solana RPC from here: it answers from the same shared hourly
    // cache the rest of the site reads, so opening this page costs the public
    // RPC nothing that some other surface was not already spending.
    QUrl endpoint = catalogApiUrl();
    if (!endpoint.isValid() || endpoint.host().isEmpty())
        return;
    endpoint.setPath(QStringLiteral("/api/world/wallet"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("address"), address);
    if (force)
        query.addQueryItem(QStringLiteral("refresh"), QStringLiteral("1"));
    endpoint.setQuery(query);

    m_ledgerWalletBalanceFetches.insert(address);
    QNetworkRequest request(endpoint);
    hardenLedgerRequest(request);
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, address] {
        const QByteArray body = boundedLedgerBody(reply);
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        m_ledgerWalletBalanceFetches.remove(address);
        const bool refreshQueued =
            m_ledgerWalletBalanceRefreshQueued.remove(address);
        if (networkOk) {
            const QJsonObject response = QJsonDocument::fromJson(body).object();
            const QJsonValue lamports =
                response.value(QStringLiteral("lamports"));
            // A relay that could not reach the chain reports null rather than
            // zero. Keeping "not read yet" is the honest card for that.
            if (response.value(QStringLiteral("ok")).toBool() &&
                lamports.isDouble()) {
                applyLedgerWalletBalance(
                    address, qint64(lamports.toDouble()),
                    qint64(response.value(QStringLiteral("checkedAt"))
                               .toDouble()));
            }
        }
        if (refreshQueued)
            fetchLedgerWalletBalance(address, /*force=*/true);
    });
}

void MainWindow::applyLedgerWalletBalance(const QString &address,
                                          qint64 lamports, qint64 checkedAt)
{
    if (address.isEmpty() || lamports < 0)
        return;
    if (checkedAt <= 0)
        checkedAt = QDateTime::currentMSecsSinceEpoch();
    const QPair<qint64, qint64> current =
        m_ledgerWalletBalances.value(address, {-1, 0});
    if (current.second > checkedAt)
        return;
    m_ledgerWalletBalances.insert(address, {lamports, checkedAt});
    rebuildLedgerWalletStrip();
}

void MainWindow::rebuildLedgerTable()
{
    // The reader's own records are merged into the relay's page before
    // anything is drawn or counted, so one table, one set of totals and one
    // sort order cover the whole book.
    loadLocalLedgerRecords();
    LedgerFeature::applyLocalRecords(&m_ledgerPage, m_ledgerLocalRecords);
    rebuildLedgerWalletStrip();
    if (!m_ledgerTable)
        return;
    int payableCount =
        LedgerFeature::applyPayableRewards(&m_ledgerPage, m_ledgerPayable);
    // One signed reward payout awaiting reconciliation blocks every reward on
    // this page, so the rows are drawn knowing it rather than each Pay button
    // finding out on the click. Read once: this runs per rebuild, not per row.
    const ContributorPayoutReceipt::Pending heldPayout =
        ContributorPayoutReceipt::read();
    // Rewards already paid from here and proven finalized on-chain, still
    // owing the relay their notice. Same reasoning as the held receipt: read
    // once per rebuild, not once per row.
    const QSet<QString> settledHere =
        ContributorPayoutReceipt::Settled::rewardIdSet();
    // A settled reward is still published as ready to pay, so it counts as
    // payable above. It is not blocked by anything and must not be counted as
    // a payment the held receipt is holding up.
    for (const LedgerRecord &record : m_ledgerPage.records) {
        if (record.payable && ledgerRowIsSettledHere(record, settledHere))
            --payableCount;
    }
    const bool payoutHeld = heldPayout.blocking();
    // The receipt the transfer in flight saved for itself holds up no other
    // row: a payment approved now queues behind that transfer, and is refused
    // then only if the receipt is still held when its turn comes.
    const bool rowsHeld = payoutHeld && !heldPayoutIsInFlight(heldPayout);
    const bool heldSignatureUsable =
        forkmesh::rewards::decodeBase58(heldPayout.chainSignature, 64).size() ==
        64;
    // The panel covers the two states a reader has to act on. A held receipt
    // blocks the queue and needs reconciling or releasing. Payouts settled from
    // chain state block nothing, but nobody has been told about them: the
    // recipients are waiting on an email this app owes and cannot send while
    // signed out, which is exactly the kind of silence that goes unnoticed
    // once the payments themselves start flowing again.
    const QList<ContributorPayoutReceipt::Settled::Notice> settledNotices =
        settledHere.isEmpty()
            ? QList<ContributorPayoutReceipt::Settled::Notice>()
            : ContributorPayoutReceipt::Settled::readAll();
    if (m_ledgerPayoutHoldPanel)
        m_ledgerPayoutHoldPanel->setVisible(payoutHeld || !settledNotices.isEmpty());
    if (payoutHeld) {
        if (m_ledgerPayoutHoldTitle) {
            m_ledgerPayoutHoldTitle->setText(
                payableCount > 0
                    ? QStringLiteral(
                          "%1 ready payment(s) blocked by one unresolved receipt")
                          .arg(payableCount)
                    : QStringLiteral(
                          "Payment queue held by one unresolved receipt"));
        }
        if (m_ledgerPayoutHoldDetails) {
            QStringList details =
                ContributorPayoutReceipt::detailLines(heldPayout);
            details.append(
                heldSignatureUsable
                    ? QStringLiteral(
                          "Retrying reuses this exact signature and never "
                          "signs again. Check it on Solana before undoing "
                          "the local hold.")
                    : QStringLiteral(
                          "This saved receipt has no usable signature. "
                          "Undo the hold only after confirming that no "
                          "transfer was broadcast."));
            m_ledgerPayoutHoldDetails->setText(details.join(QLatin1Char('\n')));
            QStringList full = ContributorPayoutReceipt::detailLines(
                heldPayout, /*shortened=*/false);
            full.append(
                ContributorPayoutReceipt::reconciliationAdvice(heldPayout));
            m_ledgerPayoutHoldDetails->setToolTip(full.join(QLatin1Char('\n')));
        }
    } else if (!settledNotices.isEmpty()) {
        if (m_ledgerPayoutHoldTitle) {
            m_ledgerPayoutHoldTitle->setText(
                QStringLiteral(
                    "%1 payment(s) sent on-chain, not reported to ForkMesh yet")
                    .arg(settledNotices.size()));
        }
        if (m_ledgerPayoutHoldDetails) {
            QStringList details;
            QStringList full;
            for (const ContributorPayoutReceipt::Settled::Notice &notice :
                 settledNotices) {
                const QString line =
                    QStringLiteral("@%1 · %2 · %3")
                        .arg(notice.recipientName,
                             forkmesh::tips::formatLamports(notice.lamports));
                details.append(
                    line.arg(ContributorPayoutReceipt::shorten(
                        notice.chainSignature)));
                full.append(line.arg(notice.chainSignature));
            }
            const QString advice = QStringLiteral(
                "These transfers are finalized on Solana and will never be "
                "signed again. Sign in to this app as the repository owner and "
                "the recipients are emailed and the relay's records updated; "
                "until then they are told nothing.");
            details.append(advice);
            full.append(advice);
            m_ledgerPayoutHoldDetails->setText(details.join(QLatin1Char('\n')));
            m_ledgerPayoutHoldDetails->setToolTip(full.join(QLatin1Char('\n')));
        }
    }
    if (m_ledgerPayoutRetryButton) {
        m_ledgerPayoutRetryButton->setText(
            payoutHeld ? QStringLiteral("Retry reconciliation")
                       : QStringLiteral("Report to ForkMesh"));
        m_ledgerPayoutRetryButton->setEnabled(
            !m_tipInFlight && (payoutHeld ? heldSignatureUsable
                                          : !settledNotices.isEmpty()));
        m_ledgerPayoutRetryButton->setToolTip(
            !payoutHeld
                ? QStringLiteral(
                      "Send these already-paid public signatures to ForkMesh "
                      "so the recipients are notified. This never signs or "
                      "broadcasts a payment.")
            : heldSignatureUsable
                ? QStringLiteral(
                      "Send the same saved public signature to ForkMesh again. "
                      "This never signs or broadcasts a replacement payment.")
                : QStringLiteral(
                      "This receipt has no usable transaction signature to "
                      "reconcile."));
    }
    if (m_ledgerPayoutExplorerButton) {
        m_ledgerPayoutExplorerButton->setVisible(payoutHeld &&
                                                 heldSignatureUsable);
        m_ledgerPayoutExplorerButton->setEnabled(payoutHeld &&
                                                 heldSignatureUsable);
    }
    // Nothing to undo once the chain has settled a payout: the money is
    // provably gone, so the only honest action left is reporting it.
    if (m_ledgerPayoutUndoButton) {
        m_ledgerPayoutUndoButton->setVisible(payoutHeld);
        m_ledgerPayoutUndoButton->setEnabled(payoutHeld && !m_tipInFlight);
    }
    const bool showsHeldBadge = heldPayout.blocking() && payableCount > 0;
    m_ledgerTable->setColumnWidth(kLedgerActionColumn,
                                  showsHeldBadge
                                      ? kLedgerActionColumnWidth +
                                            kLedgerHeldBadgeWidth
                                      : kLedgerActionColumnWidth);
    const bool pendingOnly =
        m_ledgerPendingOnly && m_ledgerPendingOnly->isChecked();

    // Repopulating under a live sort re-sorts on every insert and moves rows
    // out from under the loop. Build the page in relay order, then hand the
    // finished table back to whatever column the reader is sorting by.
    const bool wasSorting = m_ledgerTable->isSortingEnabled();
    m_ledgerTable->setSortingEnabled(false);
    m_ledgerTable->setRowCount(0);
    int actionRowHeight = 0;
    for (const LedgerRecord &record : m_ledgerPage.records) {
        const bool isPending = LedgerFeature::isPendingPayment(record);
        if (pendingOnly && !isPending)
            continue;
        const bool paymentProcessing =
            m_ledgerPaymentProcessing && record.id == m_ledgerPayingRecordId;
        const bool paymentQueued = ledgerRecordQueued(record.id);
        const int row = m_ledgerTable->rowCount();
        m_ledgerTable->insertRow(row);

        QTableWidgetItem *whenItem = ledgerCell(
            ledgerWhenText(record.occurredAt), double(record.occurredAt));
        whenItem->setData(Qt::UserRole, record.id);
        whenItem->setData(kLedgerLinkUrlRole,
                          record.subjectUrl.isValid() ? record.subjectUrl
                                                      : record.explorerUrl);
        m_ledgerTable->setItem(row, kLedgerWhenColumn, whenItem);

        QString label = record.title;
        if (!record.subjectLabel.isEmpty())
            label += QString::fromUtf8(" \xC2\xB7 ") + record.subjectLabel;
        QTableWidgetItem *recordItem = ledgerCell(label);
        recordItem->setToolTip(record.note.isEmpty() ? record.category
                                                     : record.note);
        m_ledgerTable->setItem(row, kLedgerRecordColumn, recordItem);

        // What the money is about, in the words of the issue or pull itself.
        // The relay names it only for records that have one, and a row with
        // no subject says so rather than borrowing the record's own title.
        const bool titled = !record.subjectTitle.isEmpty();
        QTableWidgetItem *titleItem =
            ledgerCell(titled ? record.subjectTitle
                              : QString::fromUtf8("\xE2\x80\x94"),
                       record.subjectTitle);
        if (titled || !record.subjectSummary.isEmpty()) {
            QStringList context;
            if (titled)
                context << record.subjectTitle;
            if (!record.subjectSummary.isEmpty())
                context << record.subjectSummary;
            titleItem->setToolTip(context.join(QStringLiteral("\n\n")));
        }
        m_ledgerTable->setItem(row, kLedgerTitleColumn, titleItem);

        QTableWidgetItem *counterpartyItem = ledgerCell(
            ledgerCounterpartyText(record), record.counterpartyName);
        const QString discordIdentity = ledgerDiscordIdentity(record);
        if (!discordIdentity.isEmpty()) {
            counterpartyItem->setToolTip(
                QStringLiteral("ForkMesh @%1\n%2")
                    .arg(record.counterpartyName, discordIdentity));
        }
        m_ledgerTable->setItem(row, kLedgerCounterpartyColumn,
                               counterpartyItem);
        QTableWidgetItem *amountItem =
            ledgerCell(ledgerAmountText(record), ledgerAmountSortKey(record));
        // An unsigned amount on a row that is money out is the page saying
        // this money has not moved. Say so in words rather than leaving the
        // missing sign to be read as a mistake.
        if (isPending &&
            LedgerFeature::flowOf(record) == LedgerFlow::Committed) {
            amountItem->setToolTip(QStringLiteral(
                "Owed, not paid yet. It counts towards awaiting payment "
                "below, and becomes an expense when the transaction that "
                "pays it exists."));
        } else if (record.valuationBasis ==
                   QLatin1String("recorded-at-payment-sol-usd")) {
            amountItem->setToolTip(QStringLiteral(
                "The USD value is fixed to the SOL/USD quote recorded when "
                "this payment was sent; later spot-price changes do not "
                "reprice it."));
        }
        m_ledgerTable->setItem(row, kLedgerAmountColumn, amountItem);
        // Rewards this desktop cannot sign for right now say so in the one
        // column a reader already scans for "where is this up to", and carry
        // the held receipt itself as the detail behind it. The sort key stays
        // the relay's own status so the marker never reorders the page.
        // A published payout plan is the fresher of the relay's two answers:
        // the estimate list re-checks landed pulls whenever it is read, so a
        // row the ledger page still calls "pays on merge" can already be
        // payable. The status then says what the row's Pay button means.
        const bool planIsFresher =
            record.payable &&
            (record.status == QLatin1String("pending_merge") ||
             record.status == QLatin1String("pending_wallet"));
        const QString statusText = LedgerFeature::statusText(
            planIsFresher ? QStringLiteral("ready") : record.status);
        const bool settledHereRow = ledgerRowIsSettledHere(record, settledHere);
        const bool heldHere =
            !settledHereRow && rowsHeld &&
            record.category == QLatin1String("contributor-reward") &&
            (record.payable || ledgerRowIsHeldPayout(record, heldPayout));
        QTableWidgetItem *statusItem =
            settledHereRow
                ? ledgerCell(statusText +
                                 ContributorPayoutReceipt::Settled::
                                     statusSuffix(),
                             statusText)
            : heldHere ? ledgerCell(statusText +
                                        ledgerHeldStatusSuffix(record,
                                                               heldPayout),
                                    statusText)
                       : ledgerCell(statusText);
        if (settledHereRow)
            statusItem->setToolTip(ledgerSettledTooltip(record, settledHere));
        else if (heldHere)
            statusItem->setToolTip(ledgerHeldTooltip(record, heldPayout));
        if (Theme::isNext())
            statusItem->setData(kLedgerStatusToneRole,
                                ledgerStatusTone(record, isPending,
                                                 settledHereRow, heldHere));
        statusItem->setData(kLedgerStatusTextRole, statusItem->text());
        statusItem->setData(kLedgerStatusTooltipRole, statusItem->toolTip());
        if (paymentProcessing || paymentQueued)
            applyLedgerRowPaymentState(nullptr, statusItem, paymentProcessing,
                                       paymentQueued, false);
        m_ledgerTable->setItem(row, kLedgerStatusColumn, statusItem);

        const LedgerTransactionRef transaction = ledgerTransactionRef(
            record, m_ledgerPage.network, settledHere, heldPayout);
        QTableWidgetItem *transactionItem = ledgerCell(
            ledgerTransactionText(transaction.signature), transaction.signature);
        if (!transaction.signature.isEmpty()) {
            QStringList lines{transaction.signature};
            if (!transaction.relayKnows) {
                lines.append(QStringLiteral(
                    "Broadcast from this desktop. ForkMesh has not published "
                    "it yet, so only Solana can confirm it for now."));
            }
            if (transaction.url.isValid()) {
                lines.append(transaction.url.toString());
                lines.append(QStringLiteral(
                    "Click to open it in the Solana explorer."));
                // The cell's own destination, which one click follows. Every
                // other column keeps opening the issue or pull the money is
                // about, on a double-click.
                transactionItem->setData(kLedgerLinkUrlRole, transaction.url);
                transactionItem->setForeground(
                    m_ledgerTable->palette().color(QPalette::Link));
                QFont linked = transactionItem->font();
                linked.setUnderline(true);
                transactionItem->setFont(linked);
            }
            transactionItem->setToolTip(lines.join(QLatin1Char('\n')));
        }
        m_ledgerTable->setItem(row, kLedgerTransactionColumn, transactionItem);

        // A record the reader entered is the only kind they own: it can be
        // settled or taken back here, and nothing about it is signed or sent.
        if (record.local) {
            auto *cell = new QWidget;
            auto *cellLayout = new QHBoxLayout(cell);
            cellLayout->setContentsMargins(4, 2, 4, 2);
            cellLayout->setSpacing(6);
            const QString localId = record.id;
            if (isPending) {
                auto *settle = new QPushButton(QStringLiteral("Settle"));
                settle->setObjectName(QStringLiteral("ledgerSettleButton"));
                settle->setProperty("buttonSize", "sm");
                settle->setCursor(Qt::PointingHandCursor);
                settle->setToolTip(QStringLiteral(
                    "Mark this record as paid. It keeps its date and amount, "
                    "leaves the awaiting-payment total, and counts towards "
                    "expenses from then on."));
                connect(settle, &QPushButton::clicked, this,
                        [this, localId] { settleLocalLedgerRecord(localId); });
                cellLayout->addWidget(settle);
            }
            auto *remove = new QPushButton;
            remove->setObjectName(QStringLiteral("ledgerRemoveButton"));
            remove->setProperty("buttonSize", "sm");
            remove->setCursor(Qt::PointingHandCursor);
            setOcticon(remove, QStringLiteral("trash"), 12);
            remove->setToolTip(
                QStringLiteral("Remove this record from your own book"));
            connect(remove, &QPushButton::clicked, this,
                    [this, localId] { removeLocalLedgerRecord(localId); });
            cellLayout->addWidget(remove);
            cellLayout->addStretch();
            m_ledgerTable->setItem(
                row, kLedgerActionColumn,
                ledgerCell(QString(), isPending ? QStringLiteral("2 settle")
                                                : QStringLiteral("4 yours")));
            m_ledgerTable->setCellWidget(row, kLedgerActionColumn, cell);
            actionRowHeight = qMax(actionRowHeight, cell->sizeHint().height());
            continue;
        }
        if (!isPending) {
            m_ledgerTable->setItem(row, kLedgerActionColumn, ledgerCell());
            continue;
        }
        // Paid here, unreported there. No Pay button, and the cell says which
        // of the two halves is outstanding rather than leaving the row looking
        // like an unpaid reward nobody will act on.
        if (settledHereRow) {
            const QString reasonText = QStringLiteral("paid, notifying");
            QTableWidgetItem *reason = ledgerCell(reasonText, reasonText);
            reason->setToolTip(ledgerSettledTooltip(record, settledHere));
            m_ledgerTable->setItem(row, kLedgerActionColumn, reason);
            continue;
        }
        const bool remindable = ledgerAwaitsRecipientWallet(record);
        if (!record.payable && !remindable) {
            const QString reasonText = ledgerUnpayableReason(record);
            QTableWidgetItem *reason = ledgerCell(reasonText, reasonText);
            reason->setToolTip(ledgerUnpayableTooltip(record));
            m_ledgerTable->setItem(row, kLedgerActionColumn, reason);
            continue;
        }

        auto *cell = new QWidget;
        auto *cellLayout = new QHBoxLayout(cell);
        cellLayout->setContentsMargins(4, 2, 4, 2);
        cellLayout->setSpacing(6);
        const QString recordId = record.id;
        cell->setProperty("ledgerRecordId", recordId);
        QString actionKey;
        if (record.payable) {
            // Left of the button, and only while a payout is held: the row has
            // to warn before the click, not just refuse after it. The button
            // itself stays live so the refusal — and the alert that explains
            // it — is still reachable from the row it belongs to.
            if (rowsHeld && !paymentProcessing && !paymentQueued) {
                auto *held = new QLabel;
                held->setObjectName(QStringLiteral("ledgerPayoutHeldBadge"));
                setLabelOcticon(held, QStringLiteral("alert"), 13);
                held->setToolTip(ledgerHeldTooltip(record, heldPayout));
                cellLayout->addWidget(held);
            }
            auto *progress = new QWidget;
            progress->setObjectName(QStringLiteral("ledgerPaymentProgress"));
            auto *progressLayout = new QHBoxLayout(progress);
            progressLayout->setContentsMargins(0, 0, 0, 0);
            progressLayout->setSpacing(6);
            auto *spinner = new BusySpinner(progress, 15);
            spinner->setObjectName(QStringLiteral("ledgerPaymentSpinner"));
            spinner->setToolTip(QStringLiteral("Processing this payment"));
            progressLayout->addWidget(spinner);
            auto *progressLabel =
                new QLabel(QString::fromUtf8("Processing\xE2\x80\xA6"));
            progressLabel->setObjectName(
                QStringLiteral("ledgerPaymentProgressLabel"));
            progressLabel->setToolTip(QStringLiteral(
                "The wallet is unlocked. This payment is being signed, "
                "broadcast, and reconciled."));
            progressLayout->addWidget(progressLabel);
            cellLayout->addWidget(progress);
            auto *pay = new QPushButton(Theme::isNext()
                                            ? QStringLiteral("Pay reward")
                                            : QStringLiteral("Send payment"));
            pay->setObjectName(QStringLiteral("ledgerPayButton"));
            pay->setProperty("buttonSize", "primary");
            pay->setCursor(Qt::PointingHandCursor);
            pay->setToolTip(
                rowsHeld
                    ? ledgerHeldTooltip(record, heldPayout)
                    : QStringLiteral(
                          "Pay %1 to %2 from the wallet linked to this app")
                          .arg(forkmesh::tips::formatLamports(
                                   record.amountLamports),
                               record.counterpartyName));
            connect(pay, &QPushButton::clicked, this,
                    [this, recordId] { payLedgerRecord(recordId); });
            cellLayout->addWidget(pay);
            applyLedgerRowPaymentState(cell, nullptr, paymentProcessing,
                                       paymentQueued,
                                       m_ledgerPreparingRecordId.isEmpty());
            actionKey = paymentProcessing ? QStringLiteral("0 processing payment")
                        : paymentQueued   ? QStringLiteral("0 queued payment")
                                          : QStringLiteral("1 send payment");
        } else {
            // The relay limits reminders per person, not per reward, so one
            // answer settles every row owed to the same contributor.
            // The relay's own record of the last reminder counts too, so one
            // sent from another machine or an earlier run does not look unsent.
            const qint64 lastReminded = m_ledgerReminderSentAt.value(
                record.counterpartyName.toLower(), 0);
            const bool remindedRecently =
                lastReminded > 0 && QDateTime::currentMSecsSinceEpoch() -
                                            lastReminded <
                                        kLedgerReminderIntervalMs;
            const bool asked =
                remindedRecently ||
                m_ledgerRemindedRecipients.contains(record.counterpartyName);
            auto *remind = new QPushButton(asked
                                               ? QStringLiteral("Reminder sent")
                                               : QStringLiteral("Remind"));
            remind->setObjectName(QStringLiteral("ledgerRemindButton"));
            remind->setCursor(Qt::PointingHandCursor);
            setOcticon(remind, QStringLiteral("bell"), 13);
            QString remindTip =
                asked
                    ? QStringLiteral(
                          "%1 has already been asked for a payout address. The "
                          "relay allows one reminder every few days.")
                          .arg(record.counterpartyName)
                    : QStringLiteral(
                          "Email %1 asking them to publish a payout address on "
                          "their ForkMesh profile. Nothing here can be paid "
                          "until they do.")
                          .arg(record.counterpartyName);
            if (lastReminded > 0) {
                remindTip += QStringLiteral("\n\nLast reminder sent %1.")
                                 .arg(ledgerReminderTimeText(lastReminded));
                if (remindedRecently) {
                    remindTip +=
                        QStringLiteral(" The next one can be sent after %1.")
                            .arg(ledgerReminderTimeText(
                                lastReminded + kLedgerReminderIntervalMs));
                }
            }
            remind->setToolTip(remindTip);
            remind->setEnabled(!asked && m_ledgerRemindingRecordId.isEmpty());
            connect(remind, &QPushButton::clicked, this,
                    [this, recordId] { remindLedgerRecipient(recordId); });
            cellLayout->addWidget(remind);
            actionKey = asked ? QStringLiteral("3 reminder sent")
                              : QStringLiteral("2 remind");
        }
        // An item behind the widget keeps the column sortable: without one the
        // model has nothing to compare and drops these rows to the bottom of
        // every order. It shows no text; the button covers the cell.
        m_ledgerTable->setItem(row, kLedgerActionColumn,
                               ledgerCell(QString(), actionKey));
        m_ledgerTable->setCellWidget(row, kLedgerActionColumn, cell);
        actionRowHeight = qMax(actionRowHeight, cell->sizeHint().height());
    }

    // Row heights live on the vertical header, not on the items, so they stay
    // where they are when a sort moves the rows. One height for the whole
    // table is the only way a reordered page keeps its buttons whole.
    if (actionRowHeight > 0) {
        int uniform = actionRowHeight;
        for (int row = 0; row < m_ledgerTable->rowCount(); ++row)
            uniform = qMax(uniform, m_ledgerTable->rowHeight(row));
        for (int row = 0; row < m_ledgerTable->rowCount(); ++row)
            m_ledgerTable->setRowHeight(row, uniform);
    }
    m_ledgerTable->setSortingEnabled(wasSorting);

    const LedgerTotals totals = LedgerFeature::totalsFor(m_ledgerPage);
    if (m_ledgerTotals) {
        m_ledgerTotals->setText(
            QStringLiteral("Expenses %1").arg(ledgerTotalText(
                totals.expenseLamports, totals.expenseUsdMicros)) +
            QString::fromUtf8("  \xC2\xB7  ") +
            QStringLiteral("Income %1").arg(ledgerTotalText(
                totals.incomeLamports, totals.incomeUsdMicros)));
        m_ledgerTotals->setToolTip(
            QStringLiteral(
                "%1 paid expense record(s) and %2 income record(s) on this "
                "page, your own included, whatever the filter is showing. "
                "Only money that has actually moved is an expense: a record "
                "with no transaction behind it is owed, and is counted as "
                "awaiting payment instead. Exact in SOL; paid contributor "
                "rewards keep their send-time USD value while other SOL rows "
                "use the live spot estimate.")
                .arg(totals.expenseCount)
                .arg(totals.incomeCount));
    }

    QStringList summary;
    summary << QStringLiteral("%1 record(s)").arg(m_ledgerPage.recordCount);
    summary << QStringLiteral("%1 on-chain").arg(m_ledgerPage.settledCount);
    // What is still owed, in money rather than in rows: the count alone says
    // nothing about whether the wallet above can cover it.
    summary << (totals.pendingCount > 0
                    ? QStringLiteral("%1 awaiting payment (%2)")
                          .arg(totals.pendingCount)
                          .arg(ledgerTotalText(totals.pendingLamports,
                                               totals.pendingUsdMicros))
                    : QStringLiteral("0 awaiting payment"));
    if (!m_ledgerPage.solUsdPrice.isEmpty())
        summary << QStringLiteral("SOL ~ $") +
                       ledgerUsdText(m_ledgerPage.solUsdPrice);
    if (m_ledgerSummary)
        m_ledgerSummary->setText(summary.join(QString::fromUtf8("  \xC2\xB7  ")));
    if (Theme::isNext())
        refreshLedgerNextStats(m_ledgerTable, totals, m_ledgerPage,
                               m_ledgerTotals ? m_ledgerTotals->toolTip()
                                              : QString());

    // A payment made from this page gets the last word. Paying refreshes the
    // ledger, and the rebuild that lands afterwards would otherwise replace
    // "sent" with the page's standing summary — which is how a payment that
    // worked still managed to look like nothing happened. Cleared by whatever
    // the reader does next here, including Refresh.
    if (!m_ledgerPaymentLine.isEmpty()) {
        setLedgerStatus(m_ledgerPaymentLine);
        return;
    }
    if (!m_ledgerRecordsLoaded) {
        // The relay's page has not arrived, but the reader's own book is on
        // screen already and is not waiting on anything.
        if (!m_ledgerLocalRecords.isEmpty())
            setLedgerStatus(
                QStringLiteral("Showing your own %1 record(s). The public "
                               "ledger has not been read yet.")
                    .arg(m_ledgerLocalRecords.size()));
        return;
    }
    if (payoutHeld && payableCount > 0) {
        setLedgerStatus(
            QStringLiteral(
                "%1 ready payment(s) are held by the unresolved receipt "
                "above. Retry its reconciliation, inspect the signature, or "
                "undo only the local hold after confirming it did not land.")
                .arg(payableCount));
    } else if (payableCount > 0) {
        setLedgerStatus(
            QStringLiteral(
                "%1 pending reward(s) can be paid from this app's linked "
                "wallet. Everything else is waiting on the relay, a merge, or "
                "a recipient payout address.")
                .arg(payableCount));
    } else if (!m_ledgerPayableLoaded) {
        setLedgerStatus(QStringLiteral(
            "Showing public records only. Connect to a trusted HTTPS ForkMesh "
            "relay to see which pending rewards you can pay from here."));
    } else {
        setLedgerStatus(QStringLiteral(
            "No pending reward is payable from this app right now."));
    }
}

void MainWindow::updateLedgerPayButtons()
{
    // Called on every change to the tip flow's in-flight state, which makes it
    // the one place that reliably sees a ledger payment end. Hand the flow's
    // reporting back here, so ordinary tip-wallet housekeeping does not go on
    // narrating itself onto this page afterwards.
    const bool paymentFinished = m_ledgerPaymentStarted && !m_tipInFlight;
    const bool refreshPaymentWallet =
        paymentFinished && m_ledgerPaymentProcessing &&
        !m_ledgerPayingWalletAddress.isEmpty();
    const QString paymentWallet = m_ledgerPayingWalletAddress;
    if (paymentFinished) {
        m_ledgerPaymentStarted = false;
        m_ledgerPaymentProcessing = false;
        m_ledgerPayingRecordId.clear();
        m_ledgerPayingWalletAddress.clear();
    }
    if (!m_tipInFlight && !m_ledgerPaymentStarted &&
        m_ledgerPayingRecordId.isEmpty() && m_ledgerPreparingRecordId.isEmpty())
        m_ledgerPaymentReporting = false;
    // Receipt recovery changes the state the transfer in flight owns, so it
    // waits; paying does not, and only waits on a review dialog already open.
    const bool enabled = !m_tipInFlight && m_ledgerPayingRecordId.isEmpty();
    const bool payEnabled = m_ledgerPreparingRecordId.isEmpty();
    if (m_ledgerSendPaymentButton) {
        m_ledgerSendPaymentButton->setEnabled(
            payEnabled && !m_ledgerRecipientLookupInFlight);
    }
    const ContributorPayoutReceipt::Pending heldPayout =
        ContributorPayoutReceipt::read();
    const bool heldSignatureUsable =
        forkmesh::rewards::decodeBase58(heldPayout.chainSignature, 64).size() ==
        64;
    if (m_ledgerPayoutRetryButton) {
        m_ledgerPayoutRetryButton->setEnabled(
            enabled && heldPayout.blocking() && heldSignatureUsable);
    }
    if (m_ledgerPayoutUndoButton)
        m_ledgerPayoutUndoButton->setEnabled(enabled && heldPayout.blocking());
    if (m_ledgerTable) {
        const bool wasSorting = m_ledgerTable->isSortingEnabled();
        m_ledgerTable->setSortingEnabled(false);
        for (int row = 0; row < m_ledgerTable->rowCount(); ++row) {
            QWidget *cell =
                m_ledgerTable->cellWidget(row, kLedgerActionColumn);
            if (!cell)
                continue;
            const QString recordId = cell->property("ledgerRecordId").toString();
            const bool processing = m_ledgerPaymentProcessing &&
                                    recordId == m_ledgerPayingRecordId;
            applyLedgerRowPaymentState(
                cell, m_ledgerTable->item(row, kLedgerStatusColumn),
                processing, !processing && ledgerRecordQueued(recordId),
                payEnabled);
        }
        m_ledgerTable->setSortingEnabled(wasSorting);
        m_ledgerTable->viewport()->repaint();
    }
    if (refreshPaymentWallet)
        fetchLedgerWalletBalance(paymentWallet, /*force=*/true);
}

void MainWindow::markLedgerPaymentProcessing(const QString &walletAddress)
{
    if (!m_ledgerPaymentStarted || m_ledgerPayingRecordId.isEmpty())
        return;
    m_ledgerPaymentProcessing = true;
    if (!walletAddress.isEmpty())
        m_ledgerPayingWalletAddress = walletAddress;
    updateLedgerPayButtons();
    if (m_ledgerStatus)
        m_ledgerStatus->repaint();
}

bool MainWindow::ledgerRecordQueued(const QString &recordId) const
{
    return !recordId.isEmpty() &&
           std::any_of(m_tipTransferQueue.cbegin(), m_tipTransferQueue.cend(),
                       [&recordId](const TipTransferRequest &queued) {
                           return queued.ledgerRecordId == recordId;
                       });
}

void MainWindow::retryHeldLedgerPayout()
{
    if (m_tipInFlight || !m_ledgerPayingRecordId.isEmpty()) {
        setLedgerStatus(QStringLiteral(
            "Another payment action is already running. Wait for it to finish "
            "before retrying reconciliation."));
        return;
    }
    const ContributorPayoutReceipt::Pending heldPayout =
        ContributorPayoutReceipt::read();
    if (!heldPayout.blocking()) {
        // Nothing is held, but payouts already settled from public chain state
        // may still owe the relay their notice. That is the same errand this
        // button is for, so run it rather than answering "nothing to do".
        const int queued = ContributorPayoutReceipt::Settled::rewardIds().size();
        m_ledgerPaymentLine =
            queued > 0
                ? QString::fromUtf8(
                      "Reporting %1 payment(s) already made from this desktop "
                      "to ForkMesh\xE2\x80\xA6")
                      .arg(queued)
                : QStringLiteral("There is no saved payout hold to reconcile.");
        if (queued > 0)
            flushSettledContributorNotices(/*announce=*/true);
        rebuildLedgerTable();
        return;
    }
    if (forkmesh::rewards::decodeBase58(heldPayout.chainSignature, 64).size() !=
        64) {
        m_ledgerPaymentLine = QStringLiteral(
            "The saved receipt has no usable transaction signature. Confirm "
            "that nothing was broadcast before undoing its local hold.");
        rebuildLedgerTable();
        return;
    }

    // An explicit retry takes ownership from any scheduled CFO pass that is
    // between payments. Its already-queued callbacks all check this flag and
    // become no-ops, so two reconciliation attempts cannot race each other.
    m_contributorPayoutRunActive = false;
    m_contributorPayoutCurrentRewardId.clear();
    m_contributorPayoutCurrentLamports = 0;
    m_ledgerPaymentLine.clear();
    setLedgerStatus(QString::fromUtf8(
        "Retrying the saved payout receipt without signing or broadcasting a "
        "replacement transaction\xE2\x80\xA6"));
    if (!resumePendingContributorReceipt(/*automated=*/false,
                                         /*reportToLedger=*/true)) {
        m_ledgerPaymentLine =
            QStringLiteral("There is no saved payout hold to reconcile.");
        rebuildLedgerTable();
    }
}

void MainWindow::openHeldLedgerPayoutInExplorer()
{
    const ContributorPayoutReceipt::Pending heldPayout =
        ContributorPayoutReceipt::read();
    if (forkmesh::rewards::decodeBase58(heldPayout.chainSignature, 64).size() !=
        64) {
        setLedgerStatus(QStringLiteral(
            "The saved payout receipt has no usable Solana signature to open."));
        return;
    }
    QUrl explorer(QStringLiteral("https://explorer.solana.com"));
    explorer.setPath(QStringLiteral("/tx/") + heldPayout.chainSignature);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("cluster"),
                       QStringLiteral("mainnet-beta"));
    explorer.setQuery(query);
    if (!QDesktopServices::openUrl(explorer)) {
        setLedgerStatus(QStringLiteral(
            "The Solana explorer could not be opened. The full signature is "
            "available in the hold details tooltip."));
    }
}

void MainWindow::undoHeldLedgerPayout()
{
    if (m_tipInFlight || !m_ledgerPayingRecordId.isEmpty()) {
        setLedgerStatus(QStringLiteral(
            "Wait for the active payment action before changing its saved "
            "receipt."));
        return;
    }
    const ContributorPayoutReceipt::Pending heldPayout =
        ContributorPayoutReceipt::read();
    if (!heldPayout.blocking()) {
        m_ledgerPaymentLine =
            QStringLiteral("There is no saved payout hold to undo.");
        rebuildLedgerTable();
        return;
    }
    if (auto *existing =
            findChild<QMessageBox *>(QStringLiteral("ledgerUndoPayoutDialog"))) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }

    auto *box = new QMessageBox(this);
    box->setObjectName(QStringLiteral("ledgerUndoPayoutDialog"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setIcon(QMessageBox::Warning);
    box->setWindowTitle(QStringLiteral("Undo the local payment hold?"));
    box->setText(QStringLiteral(
        "A Solana payment cannot be undone. This action removes only this "
        "desktop's saved reconciliation receipt and lets waiting rewards be "
        "signed again."));
    QStringList details = ContributorPayoutReceipt::detailLines(
        heldPayout, /*shortened=*/false);
    details.append(QString());
    details.append(QStringLiteral(
        "If this transfer landed, removing the hold could let the same work be "
        "paid twice."));
    box->setInformativeText(details.join(QLatin1Char('\n')));

    auto *acknowledgement = new QCheckBox(
        heldPayout.chainSignature.isEmpty()
            ? QStringLiteral(
                  "I confirmed that no transfer was broadcast for this receipt.")
            : QStringLiteral(
                  "I checked this exact signature on Solana and confirmed the "
                  "transfer did not land."));
    acknowledgement->setObjectName(
        QStringLiteral("ledgerUndoPayoutAcknowledgement"));
    box->setCheckBox(acknowledgement); // QMessageBox takes ownership

    QPushButton *undo = box->addButton(QStringLiteral("Undo local hold"),
                                       QMessageBox::DestructiveRole);
    undo->setObjectName(QStringLiteral("ledgerUndoPayoutConfirmButton"));
    undo->setEnabled(false);
    QPushButton *cancel = box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(cancel);
    box->setEscapeButton(cancel);
    connect(acknowledgement, &QCheckBox::toggled, undo,
            &QPushButton::setEnabled);

    forkmesh::ui::onMessageBoxClicked(box, this,
            [this, undo, heldPayout](QAbstractButton *clicked) {
        if (clicked != undo)
            return;
        if (m_tipInFlight || !m_ledgerPayingRecordId.isEmpty()) {
            m_ledgerPaymentLine = QStringLiteral(
                "The payment state changed while the confirmation was open; "
                "the hold was not removed.");
            rebuildLedgerTable();
            return;
        }
        const ContributorPayoutReceipt::Pending current =
            ContributorPayoutReceipt::read();
        const bool sameReceipt =
            current.blocking() && current.rewardId == heldPayout.rewardId &&
            current.chainSignature == heldPayout.chainSignature &&
            current.recipientName == heldPayout.recipientName &&
            current.sourceAddress == heldPayout.sourceAddress &&
            current.endpoint == heldPayout.endpoint &&
            current.lamports == heldPayout.lamports;
        if (!sameReceipt) {
            m_ledgerPaymentLine = QStringLiteral(
                "The saved payout receipt changed while the confirmation was "
                "open; its hold was not removed.");
            rebuildLedgerTable();
            return;
        }

        // Stop a scheduled run before opening the queue. Its callbacks check
        // this flag, and the owner's session authorization remains armed only
        // for a future explicit run.
        m_contributorPayoutRunActive = false;
        m_contributorPayoutCurrentRewardId.clear();
        m_contributorPayoutCurrentLamports = 0;
        clearPendingContributorReceipt(heldPayout.rewardId,
                                       heldPayout.chainSignature);
        if (ContributorPayoutReceipt::read().blocking()) {
            m_ledgerPaymentLine = QStringLiteral(
                "The local payout hold could not be removed. No payment state "
                "was changed.");
            logSystem(QStringLiteral(
                "Ledger: the confirmed request to undo a local payout hold "
                "could not be saved."));
        } else {
            m_ledgerPaymentLine = QStringLiteral(
                "Payment hold undone. Only the local receipt was removed; no "
                "on-chain transfer was reversed. Ready payments are available "
                "again.");
            logSystem(QStringLiteral(
                "Ledger: removed the confirmed local payout hold for %1; no "
                "on-chain transfer was reversed.")
                          .arg(heldPayout.rewardId));
        }
        m_ledgerPaymentReporting = false;
        refreshRewardPoolControls();
        rebuildLedgerTable();
    });
    box->setModal(false);
    box->show();
    box->raise();
    box->activateWindow();
}

void MainWindow::payLedgerRecord(const QString &recordId)
{
    // One review dialog at a time, and never a second click on a row already
    // paying or queued. A payment in flight is no reason to wait: the one
    // approved here queues behind it.
    if (!m_ledgerPreparingRecordId.isEmpty() ||
        recordId == m_ledgerPayingRecordId || ledgerRecordQueued(recordId))
        return;
    m_ledgerPreparingRecordId = recordId;
    // Nothing has spoken for this click yet. Whatever does — a refusal here, a
    // line out of the tip flow — lands in m_ledgerPaymentLine. A payment in
    // flight still owns the line until this one has something to say.
    if (!m_tipInFlight)
        m_ledgerPaymentLine.clear();
    // Confirming and unlocking both run a modal event loop, and rebuilding the
    // table destroys the very button whose click is still on the stack. Do the
    // work on the next turn instead, and re-read the payout plan there rather
    // than trusting figures the row was drawn with some seconds ago.
    QTimer::singleShot(0, this, [this, recordId] {
        rebuildLedgerTable(); // Pay buttons stay disabled while the dialog is open
        const auto match = std::find_if(
            m_ledgerPage.records.cbegin(), m_ledgerPage.records.cend(),
            [&recordId](const LedgerRecord &candidate) {
                return candidate.id == recordId;
            });
        // The plan, not the row, carries the figures that would be signed.
        const auto plan =
            match == m_ledgerPage.records.cend()
                ? m_ledgerPayable.cend()
                : std::find_if(m_ledgerPayable.cbegin(), m_ledgerPayable.cend(),
                               [match](const LedgerRecord &candidate) {
                                   return candidate.rewardId ==
                                              match->rewardId &&
                                          candidate.amountLamports ==
                                              match->amountLamports;
                               });
        const bool payable =
            match != m_ledgerPage.records.cend() && match->payable &&
            plan != m_ledgerPayable.cend() &&
            !plan->recipientAddress.isEmpty() &&
            !plan->counterpartyName.isEmpty() && plan->amountLamports > 0;
        if (payable) {
            // Copy before handing over: sendTipToMember takes references and
            // runs a modal loop, during which a ledger reply can replace the
            // very list these iterators point into.
            const QString recipient = plan->counterpartyName;
            const QString address = plan->recipientAddress;
            const QString reward = plan->rewardId;
            const quint64 lamports = plan->amountLamports;
            // Answer the click before the first dialog can open. The flow
            // below is entitled to refuse outright, and a row that simply goes
            // quiet reads as a broken button rather than as a refusal.
            setLedgerStatus(
                QStringLiteral("Preparing a payment of %1 to %2")
                    .arg(forkmesh::tips::formatLamports(lamports), recipient) +
                QString::fromUtf8("\xE2\x80\xA6"));
            // sendTipToMember owns the confirmation, the vault unlock, the
            // broadcast and the public receipt, and clears m_tipInFlight
            // itself on every exit. It reports through tipStatus(), which
            // writes to the system log, so ask it to say the same things here.
            sendTipToMember(recipient, address, lamports, reward,
                            /*reportToLedger=*/true);
        } else {
            m_ledgerPaymentLine = QStringLiteral(
                "That reward is no longer payable from here. Refresh to see "
                "its current state.");
        }
        m_ledgerPreparingRecordId.clear();
        const bool taken =
            (m_tipInFlight && m_ledgerPayingRecordId == recordId) ||
            ledgerRecordQueued(recordId);
        if (taken || m_tipInFlight) {
            // Started or queued; or refused while another payment runs, which
            // keeps its own row and line. Either way re-arm the Pay buttons.
            updateLedgerPayButtons();
        } else {
            m_ledgerPaymentStarted = false;
            m_ledgerPaymentProcessing = false;
            m_ledgerPayingRecordId.clear();
            m_ledgerPayingWalletAddress.clear();
            m_ledgerPaymentReporting = false;
            // Nothing reached the network: the flow refused, or a dialog was
            // dismissed. Something has to answer the click — a row that goes
            // quiet reads as a broken button rather than as a refusal — and a
            // dismissed dialog leaves nothing to report on its own.
            if (m_ledgerPaymentLine.isEmpty())
                m_ledgerPaymentLine =
                    QStringLiteral("Payment cancelled; nothing was signed.");
            // Re-arms the row and puts that line back over the summary.
            rebuildLedgerTable();
        }
    });
}

void MainWindow::remindLedgerRecipient(const QString &recordId)
{
    if (!m_ledgerRemindingRecordId.isEmpty())
        return;
    m_ledgerRemindingRecordId = recordId;
    // This page answers one action at a time: the reminder's own outcome
    // replaces whatever the last payment left on the status line.
    m_ledgerPaymentLine.clear();
    // Rebuilding the table destroys the very button whose click is still on
    // the stack, and every exit below rebuilds. Do the work on the next turn,
    // and re-read the record there rather than trusting a row drawn seconds
    // ago. (Same reasoning as payLedgerRecord.)
    QTimer::singleShot(0, this, [this, recordId] {
        const auto match = std::find_if(
            m_ledgerPage.records.cbegin(), m_ledgerPage.records.cend(),
            [&recordId](const LedgerRecord &candidate) {
                return candidate.id == recordId;
            });
        // Copy before anything can replace the list this iterator points into.
        const bool eligible = match != m_ledgerPage.records.cend() &&
                              ledgerAwaitsRecipientWallet(*match);
        const QString rewardId = eligible ? match->sourceId : QString();
        const QString recipient = eligible ? match->counterpartyName : QString();

        const auto refuse = [this](const QString &message) {
            m_ledgerRemindingRecordId.clear();
            rebuildLedgerTable();
            setLedgerStatus(message);
        };
        if (!eligible) {
            refuse(QStringLiteral(
                "That reward is no longer waiting on a payout address. "
                "Refresh to see its current state."));
            return;
        }
        const qint64 lastReminded =
            m_ledgerReminderSentAt.value(recipient.toLower(), 0);
        if (m_ledgerRemindedRecipients.contains(recipient) ||
            (lastReminded > 0 &&
             QDateTime::currentMSecsSinceEpoch() - lastReminded <
                 kLedgerReminderIntervalMs)) {
            refuse(lastReminded > 0
                       ? QStringLiteral("%1 has already been asked for a payout "
                                        "address. Last reminder sent %2.")
                             .arg(recipient,
                                  ledgerReminderTimeText(lastReminded))
                       : QStringLiteral(
                             "%1 has already been asked for a payout address.")
                             .arg(recipient));
            return;
        }
        if (!m_networkAccess) {
            refuse(QStringLiteral(
                "Network access is unavailable, so no reminder can be sent."));
            return;
        }
        // Deliberately the reward-pool helper: a reminder is addressed to a
        // real person, in the name of whoever is asking, so it may only be
        // requested from the trusted HTTPS relay this app is connected to.
        const QUrl endpoint = rewardPoolWorkerEndpoint(
            QStringLiteral("/api/rewards/wallet-reminder"));
        if (!endpoint.isValid()) {
            refuse(QStringLiteral(
                       "Connect to a trusted HTTPS ForkMesh relay to ask %1 "
                       "for a payout address.")
                       .arg(recipient));
            return;
        }
        const QString sessionToken = accountSessionTokenForUrl(endpoint);
        if (sessionToken.isEmpty()) {
            refuse(QStringLiteral(
                       "Sign in as the owner of this repository to ask %1 for "
                       "a payout address.")
                       .arg(recipient));
            return;
        }

        rebuildLedgerTable(); // every Remind button goes flat for the round trip
        setLedgerStatus(QStringLiteral("Asking %1 for a payout address")
                            .arg(recipient) +
                        QString::fromUtf8("\xE2\x80\xA6"));

        QNetworkRequest request(endpoint);
        hardenLedgerRequest(request);
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/json"));
        request.setRawHeader("Authorization",
                             QByteArrayLiteral("Bearer ") +
                                 sessionToken.toUtf8());
        const QJsonObject payload{{QStringLiteral("rewardId"), rewardId}};
        QNetworkReply *reply = m_networkAccess->post(
            request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, recipient, recordId] {
            const QByteArray body = boundedLedgerBody(reply);
            const bool networkOk = reply->error() == QNetworkReply::NoError;
            const int status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                    .toInt();
            reply->deleteLater();
            if (m_ledgerRemindingRecordId == recordId)
                m_ledgerRemindingRecordId.clear();
            const QJsonObject response =
                QJsonDocument::fromJson(body).object();
            const QString error =
                response.value(QStringLiteral("error")).toString();

            QString message;
            if (networkOk && response.value(QStringLiteral("ok")).toBool()) {
                // The relay allows one reminder per person, so retire every
                // row owed to this contributor, not just the one clicked.
                m_ledgerRemindedRecipients.insert(recipient);
                message = response.value(QStringLiteral("emailed")).toBool()
                              ? QStringLiteral(
                                    "Emailed %1 asking for a payout address. "
                                    "Refresh once they publish one and the "
                                    "reward becomes payable from here.")
                                    .arg(recipient)
                              : QStringLiteral(
                                    "%1 has no verified email address, so the "
                                    "request went to their ForkMesh "
                                    "notifications instead.")
                                    .arg(recipient);
                // Say when this one went out and when the one before it did,
                // which is what the owner is deciding the next nudge against.
                const qint64 sentAt = qint64(
                    response.value(QStringLiteral("sentAt")).toDouble(0));
                if (sentAt > 0) {
                    m_ledgerReminderSentAt.insert(recipient.toLower(), sentAt);
                    message += QStringLiteral(" Reminder sent %1.")
                                   .arg(ledgerReminderTimeText(sentAt));
                }
                if (response.contains(QStringLiteral("previousSentAt"))) {
                    const qint64 previousSentAt = qint64(
                        response.value(QStringLiteral("previousSentAt"))
                            .toDouble(0));
                    message +=
                        previousSentAt > 0
                            ? QStringLiteral(
                                  " The previous reminder went out %1.")
                                  .arg(ledgerReminderTimeText(previousSentAt))
                            : QStringLiteral(
                                  " This was the first reminder sent to %1.")
                                  .arg(recipient);
                }
            } else if (error == QLatin1String("reminder_too_soon")) {
                m_ledgerRemindedRecipients.insert(recipient);
                const qint64 lastSentAt = qint64(
                    response.value(QStringLiteral("lastSentAt")).toDouble(0));
                if (lastSentAt > 0) {
                    m_ledgerReminderSentAt.insert(recipient.toLower(),
                                                  lastSentAt);
                    message =
                        QStringLiteral(
                            "%1 was already asked for a payout address. Last "
                            "reminder sent %2. The relay sends at most one "
                            "every three days, so the next can go out after "
                            "%3.")
                            .arg(recipient, ledgerReminderTimeText(lastSentAt),
                                 ledgerReminderTimeText(
                                     lastSentAt + kLedgerReminderIntervalMs));
                } else {
                    message = QStringLiteral(
                                  "%1 was asked for a payout address recently. "
                                  "The relay sends at most one reminder every "
                                  "few days.")
                                  .arg(recipient);
                }
            } else if (error == QLatin1String("wallet_already_published")) {
                message = QStringLiteral(
                              "%1 has published a payout address. Refresh to "
                              "pick up the rewards that became payable.")
                              .arg(recipient);
            } else if (error == QLatin1String("reward_owner_required")) {
                message = QStringLiteral(
                              "Only the owner of the repository this reward "
                              "came from can ask %1 for a payout address.")
                              .arg(recipient);
            } else if (status == 401) {
                message = QStringLiteral(
                    "Sign in again: this app's ForkMesh session was not "
                    "accepted.");
            } else {
                message =
                    status > 0
                        ? QStringLiteral(
                              "The reminder could not be sent (HTTP %1).")
                              .arg(status)
                        : QStringLiteral(
                              "The reminder could not reach the ForkMesh "
                              "relay.");
            }
            rebuildLedgerTable();
            setLedgerStatus(message);
        });
    });
}

void MainWindow::recordRewardReceiptLocally(const QString &rewardId,
                                            const QString &chainSignature)
{
    const QString reward = rewardId.trimmed();
    const QString signature = chainSignature.trimmed();
    if (reward.isEmpty() || signature.isEmpty())
        return;

    // Either projection of the estimate names the subject; the Ledger page and
    // the reward-pool card each populate one of them.
    QString repository;
    QString sourceKind;
    int subjectNumber = 0;
    // The relay's estimate, for an issue that was filed without its reward.
    RemoteIssueMeta estimate;
    for (const LedgerRecord &candidate : m_ledgerPayable) {
        if (candidate.rewardId != reward)
            continue;
        repository = candidate.repository;
        sourceKind = candidate.sourceKind;
        subjectNumber = candidate.subjectNumber;
        estimate.rewardRecipient = candidate.counterpartyName;
        estimate.rewardEstimateLamports =
            static_cast<qint64>(candidate.amountLamports);
        break;
    }
    const QJsonObject stored = m_contributorRewards.value(reward);
    if (repository.isEmpty()) {
        repository = stored.value(QStringLiteral("repository")).toString();
        sourceKind = stored.value(QStringLiteral("sourceKind")).toString();
        subjectNumber =
            stored.value(QStringLiteral("subjectNumber")).toInt();
        estimate.rewardRecipient =
            stored.value(QStringLiteral("recipient")).toString();
        estimate.rewardEstimateLamports = static_cast<qint64>(
            stored.value(QStringLiteral("amountLamports")).toDouble());
    }
    estimate.rewardCondition =
        stored.value(QStringLiteral("condition")).toString();
    const QStringList parts = repository.split(QLatin1Char('/'));
    if (parts.size() != 2 || subjectNumber < 1)
        return;
    const int index = repoIndexFor(parts.at(0), parts.at(1));
    if (index < 0) {
        // Paying for a repository this machine does not host is legitimate —
        // the relay's record is still authoritative — but say so, because the
        // reader will otherwise look for a receipt that was never written.
        logSystem(QStringLiteral(
            "Ledger: paid %1, but %2 is not checked out here, so the receipt "
            "was recorded only on the relay.")
                      .arg(reward, repository));
        return;
    }
    const RepositoryRecord &repo =
        writableRecordFor(m_repositories.at(index));
    QString error;
    bool recorded = false;
    if (sourceKind == QLatin1String("pull")) {
        PullStore store(repo.localPath, repo.mirrorPath, &m_profileIdentity,
                        m_userName);
        recorded = store.recordRewardPayment(subjectNumber, reward, signature,
                                             &error);
    } else {
        IssueStore store(repo.localPath, repo.mirrorPath, &m_profileIdentity,
                         chatDisplayName());
        recorded = store.recordRewardPayment(subjectNumber, reward, signature,
                                             &error, &estimate);
    }
    if (recorded) {
        logSystem(QStringLiteral("Ledger: recorded transaction %1 on %2 %3 #%4.")
                      .arg(signature.left(20), repository,
                           sourceKind == QLatin1String("pull")
                               ? QStringLiteral("pull")
                               : QStringLiteral("issue"))
                      .arg(subjectNumber));
        // The reward badge on the issue or pull is read from the store, so
        // bring the open list forward only when it is this repository's.
        if (m_repoDetailIndex == index) {
            if (sourceKind == QLatin1String("pull"))
                reloadPullsInBackground();
            else
                reloadIssuesInBackground();
        }
        return;
    }
    logSystem(QStringLiteral(
        "Ledger: %1 is paid on the relay, but the local receipt could not be "
        "written: %2")
                  .arg(reward,
                       error.isEmpty() ? QStringLiteral("unknown error")
                                       : error));
}

#ifdef FORKMESH_WINDOW_TESTS
void MainWindow::testLoadLedgerFixture(const QByteArray &ledgerBody,
                                       const QByteArray &estimatesBody)
{
    ensureSectionBuilt(kLedgerSectionIndex);
    m_ledgerPage = LedgerPage();
    m_ledgerRecordsLoaded = LedgerFeature::parseLedgerPage(
        ledgerBody, catalogApiUrl(), &m_ledgerPage, nullptr);
    m_ledgerPayable = parsePayableRewards(estimatesBody);
    m_ledgerPayableLoaded = !m_ledgerPayable.isEmpty();
    m_ledgerLoadedAtMs = QDateTime::currentMSecsSinceEpoch();
    rebuildLedgerTable();
}

int MainWindow::testLedgerRowCount() const
{
    return m_ledgerTable ? m_ledgerTable->rowCount() : -1;
}

int MainWindow::testLedgerWalletCardCount() const
{
    if (!m_ledgerWalletStrip || !m_ledgerWalletStrip->layout())
        return -1;
    return m_ledgerWalletStrip->layout()->count();
}

QString MainWindow::testLedgerWalletCardText(int index) const
{
    if (!m_ledgerWalletStrip || !m_ledgerWalletStrip->layout())
        return {};
    QLayoutItem *item = m_ledgerWalletStrip->layout()->itemAt(index);
    QWidget *card = item ? item->widget() : nullptr;
    if (!card)
        return {};
    QStringList lines;
    for (QLabel *label : card->findChildren<QLabel *>())
        lines << label->text();
    return lines.join(QLatin1Char('\n'));
}

bool MainWindow::testLedgerWalletCardRefreshable(int index) const
{
    if (!m_ledgerWalletStrip || !m_ledgerWalletStrip->layout())
        return false;
    QLayoutItem *item = m_ledgerWalletStrip->layout()->itemAt(index);
    QWidget *card = item ? item->widget() : nullptr;
    return card && card->findChild<QPushButton *>(
                       QStringLiteral("ledgerWalletRefresh"));
}

void MainWindow::testAddLedgerEntry(const QString &direction,
                                    const QString &category,
                                    const QString &description,
                                    const QString &counterparty,
                                    const QString &currency,
                                    const QString &amount, bool outstanding)
{
    ensureSectionBuilt(kLedgerSectionIndex);
    if (!m_ledgerEntryDirection || !m_ledgerEntryCurrency)
        return;
    m_ledgerEntryDirection->setCurrentIndex(
        qMax(0, m_ledgerEntryDirection->findData(direction)));
    m_ledgerEntryCurrency->setCurrentIndex(
        qMax(0, m_ledgerEntryCurrency->findData(currency)));
    m_ledgerEntryCategory->setText(category);
    m_ledgerEntryDescription->setText(description);
    m_ledgerEntryCounterparty->setText(counterparty);
    m_ledgerEntryAmount->setText(amount);
    m_ledgerEntryOutstanding->setChecked(outstanding);
    addLedgerEntry();
}

bool MainWindow::testLedgerRowHasLocalButtons(int row) const
{
    if (!m_ledgerTable)
        return false;
    QWidget *cell = m_ledgerTable->cellWidget(row, kLedgerActionColumn);
    return cell && cell->findChild<QPushButton *>(
                       QStringLiteral("ledgerRemoveButton"));
}

void MainWindow::testSettleFirstLocalLedgerRecord()
{
    for (int row = 0; m_ledgerTable && row < m_ledgerTable->rowCount(); ++row) {
        QWidget *cell = m_ledgerTable->cellWidget(row, kLedgerActionColumn);
        auto *settle = cell ? cell->findChild<QPushButton *>(
                                  QStringLiteral("ledgerSettleButton"))
                            : nullptr;
        if (!settle)
            continue;
        settle->click();
        // As in testRemoveFirstLocalLedgerRecord: the click only schedules
        // the work that destroys this very button.
        QCoreApplication::processEvents();
        return;
    }
}

void MainWindow::testRemoveFirstLocalLedgerRecord()
{
    for (int row = 0; m_ledgerTable && row < m_ledgerTable->rowCount(); ++row) {
        QWidget *cell = m_ledgerTable->cellWidget(row, kLedgerActionColumn);
        auto *remove = cell ? cell->findChild<QPushButton *>(
                                  QStringLiteral("ledgerRemoveButton"))
                            : nullptr;
        if (!remove)
            continue;
        remove->click();
        // The click only schedules the work: rebuilding the table would
        // otherwise destroy the button whose click is still on the stack.
        QCoreApplication::processEvents();
        return;
    }
}

QString MainWindow::testLedgerCellText(int row, int column) const
{
    if (!m_ledgerTable)
        return {};
    QTableWidgetItem *item = m_ledgerTable->item(row, column);
    return item ? item->text() : QString();
}

QString MainWindow::testLedgerCellToolTip(int row, int column) const
{
    if (!m_ledgerTable)
        return {};
    QTableWidgetItem *item = m_ledgerTable->item(row, column);
    return item ? item->toolTip() : QString();
}

QUrl MainWindow::testLedgerCellLinkUrl(int row, int column) const
{
    return ledgerCellLink(row, column);
}

// The click wiring itself, exercised the way the reader exercises it. An
// invalid URL back means the click opened nothing at all.
QUrl MainWindow::testClickLedgerCell(int row, int column)
{
    if (!m_ledgerTable)
        return {};
    m_ledgerLastOpenedLink.clear();
    emit m_ledgerTable->cellClicked(row, column);
    return m_ledgerLastOpenedLink;
}

QUrl MainWindow::testDoubleClickLedgerCell(int row, int column)
{
    if (!m_ledgerTable)
        return {};
    m_ledgerLastOpenedLink.clear();
    emit m_ledgerTable->cellDoubleClicked(row, column);
    return m_ledgerLastOpenedLink;
}

bool MainWindow::testHoverLedgerCellIsLink(int row, int column)
{
    if (!m_ledgerTable)
        return false;
    emit m_ledgerTable->entered(m_ledgerTable->model()->index(row, column));
    return m_ledgerTable->viewport()->cursor().shape() ==
           Qt::PointingHandCursor;
}

bool MainWindow::testLedgerRowHasHeldPayoutBadge(int row) const
{
    if (!m_ledgerTable)
        return false;
    QWidget *cell = m_ledgerTable->cellWidget(row, kLedgerActionColumn);
    return cell &&
           cell->findChild<QLabel *>(QStringLiteral("ledgerPayoutHeldBadge"));
}

bool MainWindow::testLedgerRowHasPayButton(int row) const
{
    if (!m_ledgerTable)
        return false;
    QWidget *cell = m_ledgerTable->cellWidget(row, kLedgerActionColumn);
    const QPushButton *pay =
        cell ? cell->findChild<QPushButton *>(QStringLiteral("ledgerPayButton"))
             : nullptr;
    return pay && !pay->isHidden();
}

bool MainWindow::testLedgerRowHasPaymentSpinner(int row) const
{
    if (!m_ledgerTable)
        return false;
    QWidget *cell = m_ledgerTable->cellWidget(row, kLedgerActionColumn);
    QWidget *progress = cell ? cell->findChild<QWidget *>(
                                   QStringLiteral("ledgerPaymentProgress"))
                             : nullptr;
    const QWidget *spinner =
        progress ? progress->findChild<QWidget *>(
                       QStringLiteral("ledgerPaymentSpinner"))
                 : nullptr;
    return progress && !progress->isHidden() && spinner &&
           !spinner->isHidden();
}

void MainWindow::testStartLedgerPaymentProcessing(
    const QString &recordId, const QString &walletAddress,
    bool balanceReadInFlight)
{
    m_ledgerPayingRecordId = recordId;
    m_ledgerPayingWalletAddress = walletAddress;
    m_ledgerPaymentStarted = true;
    m_tipInFlight = true;
    m_tipInFlightRewardId.clear();
    if (balanceReadInFlight)
        m_ledgerWalletBalanceFetches.insert(walletAddress);
    markLedgerPaymentProcessing(walletAddress);
}

void MainWindow::testFinishLedgerPaymentProcessing()
{
    m_tipInFlight = false;
    updateLedgerPayButtons();
}

void MainWindow::testQueueLedgerPayment(const QString &recordId)
{
    TipTransferRequest request;
    request.recipientName = QStringLiteral("queued-test");
    request.lamports = 1;
    request.reportToLedger = true;
    request.ledgerRecordId = recordId;
    m_tipTransferQueue.append(std::move(request));
    updateLedgerPayButtons();
}

void MainWindow::testClearTipTransferQueue()
{
    m_tipTransferQueue.clear();
    updateLedgerPayButtons();
}

bool MainWindow::testLedgerWalletRefreshQueued(const QString &address) const
{
    return m_ledgerWalletBalanceRefreshQueued.contains(address);
}

void MainWindow::testApplyLedgerWalletBalance(const QString &address,
                                              qint64 lamports,
                                              qint64 checkedAt)
{
    applyLedgerWalletBalance(address, lamports, checkedAt);
}

qint64 MainWindow::testLedgerWalletBalance(const QString &address) const
{
    return m_ledgerWalletBalances.value(address, {-1, 0}).first;
}

bool MainWindow::testClickLedgerPayButton(int row)
{
    QWidget *cell =
        m_ledgerTable ? m_ledgerTable->cellWidget(row, kLedgerActionColumn)
                      : nullptr;
    auto *pay = cell ? cell->findChild<QPushButton *>(
                           QStringLiteral("ledgerPayButton"))
                     : nullptr;
    if (!pay || !pay->isEnabled())
        return false;
    pay->click();
    // The click only schedules the work: rebuilding the table would otherwise
    // destroy the button whose click is still on the stack.
    QCoreApplication::processEvents();
    return true;
}

bool MainWindow::testLedgerRowHasRemindButton(int row) const
{
    if (!m_ledgerTable)
        return false;
    QWidget *cell = m_ledgerTable->cellWidget(row, kLedgerActionColumn);
    return cell &&
           cell->findChild<QPushButton *>(QStringLiteral("ledgerRemindButton"));
}

void MainWindow::testApplyLedgerReminderTimes(
    const QByteArray &ownerEstimatesBody)
{
    applyLedgerReminderTimes(ownerEstimatesBody);
}

namespace {
QPushButton *ledgerRemindButtonAt(const QTableWidget *table, int row)
{
    QWidget *cell = table ? table->cellWidget(row, kLedgerActionColumn)
                          : nullptr;
    return cell ? cell->findChild<QPushButton *>(
                      QStringLiteral("ledgerRemindButton"))
                : nullptr;
}
} // namespace

QString MainWindow::testLedgerRemindButtonText(int row) const
{
    const QPushButton *button = ledgerRemindButtonAt(m_ledgerTable, row);
    return button ? button->text() : QString();
}

QString MainWindow::testLedgerRemindButtonToolTip(int row) const
{
    const QPushButton *button = ledgerRemindButtonAt(m_ledgerTable, row);
    return button ? button->toolTip() : QString();
}

bool MainWindow::testLedgerRemindButtonEnabled(int row) const
{
    const QPushButton *button = ledgerRemindButtonAt(m_ledgerTable, row);
    return button && button->isEnabled();
}

void MainWindow::testSortLedgerBy(int column, bool ascending)
{
    if (m_ledgerTable)
        m_ledgerTable->sortByColumn(
            column, ascending ? Qt::AscendingOrder : Qt::DescendingOrder);
}
#endif
