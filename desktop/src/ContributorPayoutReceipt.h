#pragma once

#include "TipSigner.h"

#include <QList>
#include <QSet>
#include <QSettings>
#include <QString>
#include <QStringList>

// The one public receipt a contributor reward payout leaves behind between
// "signed" and "reconciled".
//
// A reward transfer is signed on this machine and broadcast straight to a
// public Solana RPC. Before that irreversible attempt, the payout's *public*
// fields — reward id, chain signature, recipient, amount, source address — are
// written here. They are normally deleted only once the relay confirms that
// exact signature as paid. The owner can also release the local hold from the
// Ledger after explicitly confirming that the transfer did not land. While a
// receipt exists the desktop refuses to sign any further contributor payout,
// because a lost RPC or receipt response cannot be told apart from a transfer
// that already landed: signing a replacement would pay the same contributor
// twice.
//
// Both halves of the app read this. MainWindowTips.cpp owns the writes and the
// refusal; MainWindowLedger.cpp reads it so the rows whose Pay button would be
// refused can say so before anybody presses one. Keeping the keys and the copy
// in one place is what stops those two accounts of the same state drifting.
namespace ContributorPayoutReceipt {

inline QString recipientKey()
{
    return QStringLiteral("control/contributorPayout/pending/recipient");
}
inline QString lamportsKey()
{
    return QStringLiteral("control/contributorPayout/pending/lamports");
}
inline QString signatureKey()
{
    return QStringLiteral("control/contributorPayout/pending/signature");
}
inline QString sourceAddressKey()
{
    return QStringLiteral("control/contributorPayout/pending/sourceAddress");
}
inline QString rewardIdKey()
{
    return QStringLiteral("control/contributorPayout/pending/rewardId");
}
inline QString endpointKey()
{
    return QStringLiteral("control/contributorPayout/pending/endpoint");
}
inline QString group()
{
    return QStringLiteral("control/contributorPayout/pending");
}

struct Pending {
    QString recipientName;
    QString chainSignature;
    QString sourceAddress;
    QString rewardId;
    QString endpoint;
    quint64 lamports = 0;

    // Either half on its own blocks: a receipt saved without its signature, or
    // a signature whose reward id was lost, is exactly the ambiguous state the
    // guard exists for. Only an empty record clears the way for a new payout.
    bool blocking() const
    {
        return !rewardId.isEmpty() || !chainSignature.isEmpty();
    }
};

inline Pending read(const QSettings &settings)
{
    Pending pending;
    pending.recipientName = settings.value(recipientKey()).toString().trimmed();
    pending.chainSignature = settings.value(signatureKey()).toString().trimmed();
    pending.sourceAddress =
        settings.value(sourceAddressKey()).toString().trimmed();
    pending.rewardId = settings.value(rewardIdKey()).toString().trimmed();
    pending.endpoint = settings.value(endpointKey()).toString().trimmed();
    pending.lamports = settings.value(lamportsKey()).toULongLong();
    return pending;
}

inline Pending read()
{
    const QSettings settings;
    return read(settings);
}

// A signature is 88 base58 characters and a reward id can be nearly as long.
// Neither fits a table cell or a tooltip line, and both are recognised by
// their ends, so long values are shown head-and-tail rather than truncated.
inline QString shorten(const QString &value, int keep = 10)
{
    if (value.size() <= keep * 2 + 1)
        return value;
    return value.left(keep) + QString::fromUtf8("\xE2\x80\xA6") +
           value.right(keep);
}

// The refusal, in one line. This is what the system log and the Ledger status
// line have always said; the alert and the row tooltips lead with the same
// sentence so all four surfaces are recognisably one message.
inline QString refusalSummary()
{
    return QStringLiteral(
        "A contributor payout signature is already awaiting reconciliation. "
        "No replacement payment will be signed.");
}

// What is actually being held, field by field. A held payout is somebody's
// money in an unknown state, so every figure that identifies it is named
// rather than summarised: which reward, who it was for, how much, and the
// exact signature to look up on any Solana explorer.
inline QStringList detailLines(const Pending &pending, bool shortened = true)
{
    const auto value = [shortened](const QString &raw) {
        return shortened ? shorten(raw) : raw;
    };
    QStringList lines;
    if (!pending.rewardId.isEmpty())
        lines.append(QStringLiteral("Reward: %1").arg(value(pending.rewardId)));
    if (!pending.recipientName.isEmpty())
        lines.append(QStringLiteral("Recipient: @%1").arg(pending.recipientName));
    if (pending.lamports > 0)
        lines.append(QStringLiteral("Amount: %1")
                         .arg(forkmesh::tips::formatLamports(pending.lamports)));
    if (!pending.chainSignature.isEmpty())
        lines.append(QStringLiteral("Signature: %1")
                         .arg(value(pending.chainSignature)));
    if (!pending.sourceAddress.isEmpty())
        lines.append(QStringLiteral("Paid from: %1")
                         .arg(value(pending.sourceAddress)));
    if (lines.isEmpty()) {
        // A record can block on a reward id alone if the app stopped between
        // saving the receipt and signing. Saying so beats an empty panel.
        lines.append(QStringLiteral(
            "The held receipt names no signature yet, so the transfer may "
            "never have been broadcast."));
    }
    return lines;
}

// Why it is held and how it clears, said the same way everywhere. Retrying the
// public receipt never signs again; releasing it is deliberately available
// only behind the Ledger's explicit double-payment acknowledgement.
inline QString reconciliationAdvice(const Pending &pending)
{
    // QString::fromUtf8, never QStringLiteral: a UTF-16 literal reads
    // "\xE2\x80\x94" as three separate code units and paints "\u00E2" where the
    // em dash should be.
    if (pending.chainSignature.isEmpty()) {
        return QString::fromUtf8(
            "Payouts stay blocked until this receipt is settled, because a "
            "receipt saved without a confirmed signature cannot be told apart "
            "from a transfer that already landed. On the Ledger, undo the "
            "local payment hold only after confirming that nothing was "
            "broadcast.");
    }
    return QString::fromUtf8(
        "That transfer may already be on-chain, so signing another one could "
        "pay the same contributor twice. Use Retry reconciliation on the "
        "Ledger to submit this exact public signature again without signing a "
        "replacement. The desktop also checks it on every reward-pool run and "
        "releases payouts once it is confirmed paid \xE2\x80\x94 inspect the "
        "signature on Solana before undoing the local hold.");
}

// --- Payouts the chain has already settled ----------------------------------
//
// The hold above exists for exactly one question: did that transfer land? Only
// the relay used to be able to answer it, and reaching the relay needs a
// signed-in ForkMesh account. A payout is signed with a wallet on this disk and
// has nothing to do with that account, so a machine can be entirely able to pay
// and unable to say so. The queue then wedged behind a receipt whose transfer
// had already landed, with no safe way out: retrying hit the same signed-out
// wall every time, and undoing the hold is precisely the double payment the
// hold exists to prevent.
//
// Solana answers that question to anybody, over the same keyless endpoints this
// desktop broadcast through. Once the saved signature reads back as finalized,
// the payout is proven landed and the receipt is resolved: it moves here, and
// the hold on every other reward clears. What is left — emailing the recipient,
// moving the relay's own record to paid — is an errand, retried whenever a
// session exists. An entry here is what keeps that reward from being signed a
// second time in the meantime, because the relay goes on publishing it as ready
// to pay until the errand runs.
namespace Settled {

// One entry per reward, keyed by its id, holding everything the deferred
// notice needs to be sent later. Reward ids are validated against
// ^reward-[a-z0-9-]{8,88}$ before anything is written, so they are safe as
// settings group names.
inline QString group()
{
    return QStringLiteral("control/contributorPayout/settled");
}
inline QString key(const QString &rewardId, const QString &field)
{
    return group() + QLatin1Char('/') + rewardId + QLatin1Char('/') + field;
}

// Settling is what unblocks the queue, so an unbounded queue of undelivered
// notices would be a way to lose the guard entirely. Past this many the
// desktop keeps holding instead: a machine that has paid hundreds of rewards
// without ever reaching the relay has a problem worth stopping for.
inline constexpr int noticeLimit() { return 256; }

struct Notice {
    QString rewardId;
    QString recipientName;
    QString chainSignature;
    QString sourceAddress;
    QString endpoint;
    quint64 lamports = 0;
};

inline QStringList rewardIds(QSettings &settings)
{
    settings.beginGroup(group());
    const QStringList ids = settings.childGroups();
    settings.endGroup();
    return ids;
}

inline QStringList rewardIds()
{
    QSettings settings;
    return rewardIds(settings);
}

inline Notice read(const QSettings &settings, const QString &rewardId)
{
    Notice notice;
    notice.rewardId = rewardId;
    notice.recipientName =
        settings.value(key(rewardId, QStringLiteral("recipient")))
            .toString()
            .trimmed();
    notice.chainSignature =
        settings.value(key(rewardId, QStringLiteral("signature")))
            .toString()
            .trimmed();
    notice.sourceAddress =
        settings.value(key(rewardId, QStringLiteral("sourceAddress")))
            .toString()
            .trimmed();
    notice.endpoint = settings.value(key(rewardId, QStringLiteral("endpoint")))
                          .toString()
                          .trimmed();
    notice.lamports =
        settings.value(key(rewardId, QStringLiteral("lamports"))).toULongLong();
    return notice;
}

inline QList<Notice> readAll()
{
    QSettings settings;
    QList<Notice> notices;
    const QStringList ids = rewardIds(settings);
    notices.reserve(ids.size());
    for (const QString &rewardId : ids)
        notices.append(read(settings, rewardId));
    return notices;
}

// The set every payout entry point checks before signing. A reward in here has
// been paid from this machine and proven on-chain, whatever the relay's
// published status still says.
inline QSet<QString> rewardIdSet()
{
    const QStringList ids = rewardIds();
    return QSet<QString>(ids.cbegin(), ids.cend());
}

inline bool contains(const QSettings &settings, const QString &rewardId)
{
    if (rewardId.isEmpty())
        return false;
    return !settings.value(key(rewardId, QStringLiteral("signature")))
                .toString()
                .trimmed()
                .isEmpty();
}

inline bool contains(const QString &rewardId)
{
    if (rewardId.isEmpty())
        return false;
    const QSettings settings;
    return contains(settings, rewardId);
}

// Short enough to ride beside the relay's own status in a table cell. The row
// is not blocked and not payable: the money has gone, only the announcement
// has not.
inline QString statusSuffix()
{
    return QString::fromUtf8(" \xC2\xB7 paid, notifying");
}

inline QString summary(const Notice &notice)
{
    return QString::fromUtf8(
        "This reward was paid from this machine and the transfer is finalized "
        "on Solana, so it will never be signed again. ForkMesh has not been "
        "told yet \xE2\x80\x94 that needs a signed-in account, and it is "
        "retried on every sign-in and reward-pool run. Until it goes through, "
        "%1 gets no email and the relay still lists this reward as ready.")
        .arg(notice.recipientName.isEmpty()
                 ? QStringLiteral("the recipient")
                 : QLatin1Char('@') + notice.recipientName);
}

} // namespace Settled

} // namespace ContributorPayoutReceipt
