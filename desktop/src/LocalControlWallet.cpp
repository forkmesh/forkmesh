#include "LocalControlWallet.h"

#include <QRegularExpression>

#include <cmath>

namespace forkmesh::localcontrol::wallet {

namespace {

QString oneLine(const QString &text, int max)
{
    QString out = text.simplified();
    if (out.size() > max)
        out = out.left(max - 1) + QChar(0x2026);
    return out;
}

QString shortAddress(const QString &address)
{
    if (address.size() <= 12)
        return address;
    return address.left(4) + QChar(0x2026) + address.right(4);
}

}

QString familyPrefix()
{
    return QStringLiteral("wallet.");
}

QStringList actions()
{
    return {QStringLiteral("tip"), QStringLiteral("reward")};
}

bool isKnownAction(const QString &action)
{
    return actions().contains(action);
}

QString confirmationHolder(const QString &deviceKey, int connectionId)
{
    return deviceKey + QLatin1Char('#') + QString::number(connectionId);
}

bool validRecipient(const QString &name)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$"));
    return name.size() <= kMaxRecipientChars && pattern.match(name).hasMatch();
}

bool plausibleAddress(const QString &address)
{
    static const QRegularExpression pattern(QStringLiteral(
        "^[1-9A-HJ-NP-Za-km-z]{32,44}$"));
    return pattern.match(address).hasMatch();
}

bool validRewardId(const QString &rewardId)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z0-9._:-]{1,128}$"));
    return pattern.match(rewardId).hasMatch();
}

bool parseLamports(const QJsonValue &value, quint64 *lamports)
{
    if (!value.isDouble())
        return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number <= 0 || number > 9007199254740991.0 ||
        std::floor(number) != number)
        return false;
    if (lamports)
        *lamports = quint64(number);
    return true;
}

bool lamportsInRange(quint64 lamports, quint64 minimum, quint64 maximum)
{
    return lamports > 0 && lamports >= minimum && (maximum == 0 || lamports <= maximum);
}

QString tipTarget(const QString &recipient, const QString &address,
                  quint64 lamports)
{
    return QStringLiteral("tip:%1:%2:%3").arg(recipient, address).arg(lamports);
}

QString rewardTarget(const QString &rewardId)
{
    return QStringLiteral("reward:") + rewardId;
}

QString solText(quint64 lamports)
{
    const quint64 whole = lamports / 1000000000ULL;
    QString fraction = QString::number(lamports % 1000000000ULL)
                           .rightJustified(9, QLatin1Char('0'));
    while (fraction.endsWith(QLatin1Char('0')))
        fraction.chop(1);
    return (fraction.isEmpty() ? QString::number(whole)
                               : QString::number(whole) + QLatin1Char('.') + fraction) +
           QStringLiteral(" SOL");
}

QString tipSummary(const QString &recipient, const QString &address,
                   quint64 lamports)
{
    return QStringLiteral("Ask your desktop to send %1 to @%2 (%3). The tip "
                          "dialog opens on the desktop; nothing is signed until "
                          "you review it there and enter your wallet passphrase.")
        .arg(solText(lamports), recipient, shortAddress(address));
}

QString rewardSummary(const QString &recipient, quint64 lamports,
                      const QString &subject)
{
    return QStringLiteral("Ask your desktop to pay %1 to @%2 for %3. The payment "
                          "dialog opens on the desktop; nothing is signed until "
                          "you review it there and enter your wallet passphrase.")
        .arg(solText(lamports), recipient, oneLine(subject, kMaxTitleChars));
}

bool rewardPayable(const RewardFacts &facts)
{
    return validRewardId(facts.rewardId) && !facts.recipient.isEmpty() &&
           plausibleAddress(facts.address) && facts.lamports > 0;
}

QString rewardSubject(const RewardFacts &facts)
{
    QString subject = facts.repository;
    if (facts.number > 0) {
        subject += (facts.kind == QLatin1String("pull") ? QStringLiteral(" PR #")
                                                        : QStringLiteral(" issue #")) +
                   QString::number(facts.number);
    }
    if (subject.trimmed().isEmpty())
        subject = QStringLiteral("reward ") + facts.rewardId;
    return subject;
}

QJsonObject rewardRow(const RewardFacts &facts)
{
    return {{QStringLiteral("rewardId"), facts.rewardId},
            {QStringLiteral("recipient"), oneLine(facts.recipient, kMaxRecipientChars)},
            {QStringLiteral("address"), facts.address},
            {QStringLiteral("lamports"), double(facts.lamports)},
            {QStringLiteral("repository"), oneLine(facts.repository, kMaxTitleChars)},
            {QStringLiteral("kind"), facts.kind == QLatin1String("pull")
                                         ? QStringLiteral("pull")
                                         : QStringLiteral("issue")},
            {QStringLiteral("number"), facts.number},
            {QStringLiteral("title"), oneLine(facts.title, kMaxTitleChars)}};
}

QJsonArray rewardRows(const QList<RewardFacts> &facts, int max)
{
    QJsonArray rows;
    QStringList seen;
    for (const RewardFacts &one : facts) {
        if (rows.size() >= max)
            break;
        if (!rewardPayable(one) || seen.contains(one.rewardId))
            continue;
        seen.append(one.rewardId);
        rows.append(rewardRow(one));
    }
    return rows;
}

bool requestBlocks(const PhoneRequest &request, qint64 nowMs)
{
    return !request.id.isEmpty() &&
           request.status == QLatin1String("awaiting_owner") &&
           nowMs - request.requestedAtMs < kRequestStaleMs;
}

QJsonObject requestRow(const PhoneRequest &request)
{
    if (request.id.isEmpty())
        return {};
    return {{QStringLiteral("id"), request.id},
            {QStringLiteral("action"), request.action},
            {QStringLiteral("summary"), request.summary},
            {QStringLiteral("status"), request.status},
            {QStringLiteral("requestedAt"), double(request.requestedAtMs)}};
}

QString newRequestId(const QByteArray &random)
{
    return QString::fromLatin1(random.toHex().left(32));
}

QJsonObject statusPayload(const WalletFacts &facts, const PhoneRequest &request,
                          qint64 nowMs)
{
    QJsonObject out{{QStringLiteral("linked"), facts.linked},
                    {QStringLiteral("address"), facts.linked ? facts.address : QString()},
                    {QStringLiteral("busy"), facts.busy || requestBlocks(request, nowMs)},
                    {QStringLiteral("queued"), facts.queued},
                    {QStringLiteral("minLamports"), double(facts.minLamports)},
                    {QStringLiteral("maxLamports"), double(facts.maxLamports)}};
    const QJsonObject row = requestRow(request);
    if (!row.isEmpty())
        out.insert(QStringLiteral("request"), row);
    return out;
}

} // namespace forkmesh::localcontrol::wallet
