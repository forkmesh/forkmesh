#include "DesktopCommands.h"

#include <QCryptographicHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <algorithm>

#include <openssl/evp.h>

namespace forkmesh::desktopcommands {

namespace {

constexpr int kKeyBytes = 32;
constexpr int kSignatureBytes = 64;
constexpr int kMaxTsDigits = 16;

struct Spec {
    const char *name;
    Kind kind;
};

constexpr Spec kSpecs[] = {
    {"pull.merge", Kind::PullMerge},
    {"pull.merge_delete", Kind::PullMergeDelete},
    {"pull.fix_agent", Kind::PullFixAgent},
    {"pull.close", Kind::PullClose},
    {"pull.reopen", Kind::PullReopen},
    {"pull.delete", Kind::PullDelete},
    {"action.rerun", Kind::ActionRerun},
    {"release.draft", Kind::ReleaseDraft},
    {"release.notes", Kind::ReleaseNotes},
    {"release.create", Kind::ReleaseCreate},
    {"release.publish", Kind::ReleasePublish},
};

const QRegularExpression &pullTarget()
{
    static const QRegularExpression re(
        QStringLiteral("\\Apull:([1-9][0-9]{0,8})\\z"));
    return re;
}

const QRegularExpression &runTarget()
{
    static const QRegularExpression re(
        QStringLiteral("\\Arun:([A-Za-z0-9][A-Za-z0-9._-]{0,127})\\z"));
    return re;
}

const QRegularExpression &releaseTarget()
{
    static const QRegularExpression re(
        QStringLiteral("\\Arelease:([A-Za-z0-9][A-Za-z0-9._+-]{0,127})\\z"));
    return re;
}

QByteArray fromBase64Url(const QString &text, int bytes)
{
    if (text.isEmpty() || text.size() > 200)
        return {};
    for (const QChar c : text) {
        const bool ok = (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) ||
                        (c >= QLatin1Char('a') && c <= QLatin1Char('z')) ||
                        (c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
                        c == QLatin1Char('-') || c == QLatin1Char('_');
        if (!ok)
            return {};
    }
    const QByteArray decoded = QByteArray::fromBase64(
        text.toLatin1(),
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    return decoded.size() == bytes ? decoded : QByteArray();
}

bool digitsOnly(const QString &text)
{
    if (text.isEmpty() || text.size() > kMaxTsDigits)
        return false;
    for (const QChar c : text)
        if (c < QLatin1Char('0') || c > QLatin1Char('9'))
            return false;
    return true;
}

QString boundedString(const QJsonObject &object, const char *key, int max)
{
    const QJsonValue value = object.value(QLatin1String(key));
    if (!value.isString())
        return {};
    const QString text = value.toString().trimmed();
    return text.size() > max ? QString() : text;
}

qint64 jsonInteger(const QJsonValue &value)
{
    if (value.isDouble())
        return qint64(value.toDouble());
    if (value.isString()) {
        bool ok = false;
        const qint64 parsed = value.toString().toLongLong(&ok);
        return ok ? parsed : 0;
    }
    return 0;
}

QList<QPair<QString, QString>> identityVariants(const QString &owner,
                                                const QString &repo)
{
    QList<QPair<QString, QString>> out;
    const auto add = [&out](const QString &o, const QString &r) {
        const QPair<QString, QString> pair(o, r);
        if (!out.contains(pair))
            out.append(pair);
    };
    add(owner, repo);
    add(owner.toLower(), repo);
    add(owner.toLower(), repo.toLower());
    return out;
}

} // namespace

QString signingDomain()
{
    return QStringLiteral("forkmesh-desktop-command-v1");
}

QStringList supportedCommands()
{
    QStringList out;
    for (const Spec &spec : kSpecs)
        out.append(QLatin1String(spec.name));
    return out;
}

Kind kindFor(const QString &command)
{
    for (const Spec &spec : kSpecs)
        if (command == QLatin1String(spec.name))
            return spec.kind;
    return Kind::Unknown;
}

bool isPullKind(Kind kind)
{
    switch (kind) {
    case Kind::PullMerge:
    case Kind::PullMergeDelete:
    case Kind::PullFixAgent:
    case Kind::PullClose:
    case Kind::PullReopen:
    case Kind::PullDelete:
        return true;
    default:
        return false;
    }
}

bool isReleaseKind(Kind kind)
{
    switch (kind) {
    case Kind::ReleaseDraft:
    case Kind::ReleaseNotes:
    case Kind::ReleaseCreate:
    case Kind::ReleasePublish:
        return true;
    default:
        return false;
    }
}

bool isDestructive(Kind kind)
{
    return kind == Kind::PullDelete || kind == Kind::PullMergeDelete;
}

bool runsUnattended(Kind kind)
{
    return kind == Kind::ReleaseCreate || kind == Kind::ReleasePublish;
}

bool parseTarget(Kind kind, const QString &target, Command *command)
{
    if (!command)
        return false;
    if (isPullKind(kind)) {
        const QRegularExpressionMatch match = pullTarget().match(target);
        if (!match.hasMatch())
            return false;
        bool ok = false;
        const int number = match.captured(1).toInt(&ok);
        if (!ok || number <= 0)
            return false;
        command->pullNumber = number;
        return true;
    }
    if (kind == Kind::ActionRerun) {
        const QRegularExpressionMatch match = runTarget().match(target);
        if (!match.hasMatch())
            return false;
        command->runId = match.captured(1);
        return true;
    }
    if (isReleaseKind(kind)) {
        const QRegularExpressionMatch match = releaseTarget().match(target);
        if (!match.hasMatch())
            return false;
        command->tag = match.captured(1);
        return true;
    }
    return false;
}

QByteArray canonical(const QString &owner, const QString &repo,
                     const QString &command, const QString &target,
                     const QString &ts)
{
    return QStringList{signingDomain(), owner, repo, command, target, ts}
        .join(QLatin1Char('\n'))
        .toUtf8();
}

bool verifyEd25519(const QString &publicKeyB64url,
                   const QString &signatureB64url, const QByteArray &data)
{
    const QByteArray key = fromBase64Url(publicKeyB64url, kKeyBytes);
    const QByteArray sig = fromBase64Url(signatureB64url, kSignatureBytes);
    if (key.isEmpty() || sig.isEmpty())
        return false;
    EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char *>(key.constData()), kKeyBytes);
    if (!pkey)
        return false;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    const bool ok =
        ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey) == 1 &&
        EVP_DigestVerify(
            ctx, reinterpret_cast<const unsigned char *>(sig.constData()),
            size_t(sig.size()),
            reinterpret_cast<const unsigned char *>(data.constData()),
            size_t(data.size())) == 1;
    if (ctx)
        EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return ok;
}

QString keyFingerprint(const QString &publicKeyB64url)
{
    const QByteArray key = fromBase64Url(publicKeyB64url, kKeyBytes);
    if (key.isEmpty())
        return {};
    const QByteArray hex =
        QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex().left(16);
    QStringList groups;
    for (int i = 0; i < hex.size(); i += 4)
        groups.append(QString::fromLatin1(hex.mid(i, 4)));
    return groups.join(QLatin1Char(':'));
}

QString signatureDigest(const QString &signature)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(signature.toUtf8(), QCryptographicHash::Sha256)
            .toHex());
}

Command parse(const QJsonObject &object)
{
    Command command;
    command.id = jsonInteger(object.value(QStringLiteral("id")));
    command.command = boundedString(object, "command", 64);
    command.target = boundedString(object, "target", 160);
    command.ts = boundedString(object, "ts", kMaxTsDigits);
    command.sig = boundedString(object, "sig", kMaxSignatureChars);
    command.signer = boundedString(object, "signer", 120);
    command.ownerAccount = boundedString(object, "ownerAccount", 120).toLower();
    command.relayCanonical = boundedString(object, "canonical", 800);
    command.queuedAtMs = jsonInteger(object.value(QStringLiteral("queuedAt")));
    command.kind = kindFor(command.command);
    if (digitsOnly(command.ts))
        command.tsMs = command.ts.toLongLong();
    if (command.kind != Kind::Unknown)
        parseTarget(command.kind, command.target, &command);
    return command;
}

QList<Command> parseDrain(const QJsonObject &payload)
{
    QList<Command> out;
    const QJsonArray rows = payload.value(QStringLiteral("commands")).toArray();
    QSet<qint64> seen;
    for (const QJsonValue &value : rows) {
        if (out.size() >= kMaxCommandsPerDrain)
            break;
        if (!value.isObject())
            continue;
        Command command = parse(value.toObject());
        if (command.id <= 0 || seen.contains(command.id))
            continue;
        seen.insert(command.id);
        out.append(command);
    }
    return out;
}

bool ReplayLedger::contains(const QString &signature) const
{
    return !signature.isEmpty() && m_entries.contains(signatureDigest(signature));
}

bool ReplayLedger::containsId(qint64 id) const
{
    if (id <= 0)
        return false;
    for (const Entry &entry : m_entries)
        if (entry.id == id)
            return true;
    return false;
}

Outcome ReplayLedger::outcomeFor(const QString &signature, bool *found) const
{
    const auto it = m_entries.constFind(signatureDigest(signature));
    if (found)
        *found = it != m_entries.constEnd();
    return it == m_entries.constEnd() ? Outcome::Rejected : it->outcome;
}

void ReplayLedger::record(const Command &command, Outcome outcome, qint64 nowMs)
{
    if (command.sig.isEmpty())
        return;
    Entry entry;
    entry.id = command.id;
    entry.seenAtMs = nowMs;
    entry.outcome = outcome;
    m_entries.insert(signatureDigest(command.sig), entry);
    prune(nowMs);
}

void ReplayLedger::prune(qint64 nowMs)
{
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (nowMs - it->seenAtMs > kLedgerKeepMs)
            it = m_entries.erase(it);
        else
            ++it;
    }
    if (m_entries.size() <= kMaxLedgerEntries)
        return;
    QList<QPair<qint64, QString>> order;
    order.reserve(m_entries.size());
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it)
        order.append({it->seenAtMs, it.key()});
    std::sort(order.begin(), order.end());
    const int excess = int(m_entries.size()) - kMaxLedgerEntries;
    for (int i = 0; i < excess; ++i)
        m_entries.remove(order.at(i).second);
}

QJsonObject ReplayLedger::toJson() const
{
    QJsonArray rows;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it)
        rows.append(QJsonObject{
            {QStringLiteral("digest"), it.key()},
            {QStringLiteral("id"), double(it->id)},
            {QStringLiteral("seenAt"), double(it->seenAtMs)},
            {QStringLiteral("outcome"), outcomeCode(it->outcome)},
        });
    return QJsonObject{{QStringLiteral("version"), 1},
                       {QStringLiteral("entries"), rows}};
}

ReplayLedger ReplayLedger::fromJson(const QJsonObject &object)
{
    ReplayLedger ledger;
    const QJsonArray rows = object.value(QStringLiteral("entries")).toArray();
    for (const QJsonValue &value : rows) {
        if (ledger.m_entries.size() >= kMaxLedgerEntries)
            break;
        const QJsonObject row = value.toObject();
        const QString digest = row.value(QStringLiteral("digest")).toString();
        if (digest.size() != 64)
            continue;
        Entry entry;
        entry.id = jsonInteger(row.value(QStringLiteral("id")));
        entry.seenAtMs = jsonInteger(row.value(QStringLiteral("seenAt")));
        const QString outcome = row.value(QStringLiteral("outcome")).toString();
        entry.outcome = Outcome::Rejected;
        for (Outcome candidate : {Outcome::Done, Outcome::Started, Outcome::Failed,
                                  Outcome::Declined, Outcome::Rejected})
            if (outcomeCode(candidate) == outcome)
                entry.outcome = candidate;
        ledger.m_entries.insert(digest, entry);
    }
    return ledger;
}

Verdict verify(const Command &command, const QString &owner,
               const QString &repo, const QString &account,
               const QList<TrustedKey> &trusted, const ReplayLedger &ledger,
               qint64 nowMs)
{
    Verdict verdict;
    if (command.id <= 0 || command.sig.isEmpty() || command.command.isEmpty() ||
        command.target.isEmpty() || !digitsOnly(command.ts)) {
        verdict.status = Status::Malformed;
        return verdict;
    }
    if (ledger.contains(command.sig)) {
        verdict.status = Status::Replayed;
        return verdict;
    }
    if (command.kind == Kind::Unknown) {
        verdict.status = Status::UnknownKind;
        return verdict;
    }
    Command probe;
    if (!parseTarget(command.kind, command.target, &probe)) {
        verdict.status = Status::BadTarget;
        return verdict;
    }
    const QString wantedOwner = owner.trimmed().toLower();
    if (wantedOwner.isEmpty() || account.trimmed().toLower() != wantedOwner ||
        command.ownerAccount != wantedOwner) {
        verdict.status = Status::WrongOwner;
        return verdict;
    }
    if (!command.relayCanonical.isEmpty()) {
        const QByteArray ours = canonical(owner, repo, command.command,
                                          command.target, command.ts);
        if (QString::fromUtf8(ours).compare(command.relayCanonical,
                                            Qt::CaseInsensitive) != 0) {
            verdict.status = Status::WrongRepo;
            return verdict;
        }
    }
    if (nowMs - command.tsMs > kRetentionMs) {
        verdict.status = Status::Expired;
        return verdict;
    }
    if (command.tsMs - nowMs > kMaxSkewMs) {
        verdict.status = Status::FromFuture;
        return verdict;
    }
    QList<QByteArray> messages;
    for (const auto &identity : identityVariants(owner.trimmed(), repo.trimmed()))
        messages.append(canonical(identity.first, identity.second,
                                  command.command, command.target, command.ts));
    const auto signedBy = [&](const QString &key) {
        for (const QByteArray &message : messages)
            if (verifyEd25519(key, command.sig, message))
                return true;
        return false;
    };
    for (const TrustedKey &key : trusted) {
        if (key.key.isEmpty() || !signedBy(key.key))
            continue;
        verdict.status = Status::Ok;
        verdict.signerKey = key.key;
        verdict.signerLabel = key.label;
        return verdict;
    }
    if (!command.signer.isEmpty() && signedBy(command.signer)) {
        verdict.status = Status::UnrecognizedSigner;
        verdict.signerKey = command.signer;
        verdict.signerLabel = QStringLiteral("unrecognized device %1")
                                  .arg(keyFingerprint(command.signer));
        return verdict;
    }
    verdict.status = Status::BadSignature;
    return verdict;
}

QString statusCode(Status status)
{
    switch (status) {
    case Status::Ok: return QStringLiteral("ok");
    case Status::UnrecognizedSigner: return QStringLiteral("unrecognized_signer");
    case Status::Malformed: return QStringLiteral("malformed");
    case Status::UnknownKind: return QStringLiteral("unsupported_command");
    case Status::BadTarget: return QStringLiteral("bad_target");
    case Status::WrongOwner: return QStringLiteral("wrong_owner");
    case Status::WrongRepo: return QStringLiteral("wrong_repository");
    case Status::Expired: return QStringLiteral("expired");
    case Status::FromFuture: return QStringLiteral("future_timestamp");
    case Status::BadSignature: return QStringLiteral("invalid_signature");
    case Status::Replayed: return QStringLiteral("replayed");
    }
    return QStringLiteral("malformed");
}

QString statusText(Status status)
{
    switch (status) {
    case Status::Ok: return QStringLiteral("verified");
    case Status::UnrecognizedSigner:
        return QStringLiteral("signed by a device key this desktop does not know");
    case Status::Malformed: return QStringLiteral("the request is incomplete");
    case Status::UnknownKind: return QStringLiteral("this desktop does not support that action");
    case Status::BadTarget: return QStringLiteral("the request names no valid target");
    case Status::WrongOwner: return QStringLiteral("it was not signed for this account");
    case Status::WrongRepo: return QStringLiteral("it was signed for a different repository");
    case Status::Expired: return QStringLiteral("it is more than a day old");
    case Status::FromFuture: return QStringLiteral("its timestamp is in the future");
    case Status::BadSignature: return QStringLiteral("its signature does not verify");
    case Status::Replayed: return QStringLiteral("it was already handled");
    }
    return {};
}

QString outcomeCode(Outcome outcome)
{
    switch (outcome) {
    case Outcome::Done: return QStringLiteral("done");
    case Outcome::Started: return QStringLiteral("started");
    case Outcome::Failed: return QStringLiteral("failed");
    case Outcome::Declined: return QStringLiteral("declined");
    case Outcome::Rejected: return QStringLiteral("rejected");
    }
    return QStringLiteral("rejected");
}

QString actionLabel(const Command &command)
{
    const QString pull = QStringLiteral("pull request #%1").arg(command.pullNumber);
    switch (command.kind) {
    case Kind::PullMerge: return QStringLiteral("Merge %1").arg(pull);
    case Kind::PullMergeDelete:
        return QStringLiteral("Merge %1, then delete it and its branch").arg(pull);
    case Kind::PullFixAgent: return QStringLiteral("Start an agent to fix %1").arg(pull);
    case Kind::PullClose: return QStringLiteral("Close %1").arg(pull);
    case Kind::PullReopen: return QStringLiteral("Reopen %1").arg(pull);
    case Kind::PullDelete: return QStringLiteral("Delete %1").arg(pull);
    case Kind::ActionRerun: return QStringLiteral("Rerun Actions run #%1").arg(command.runId);
    case Kind::ReleaseDraft: return QStringLiteral("Draft release %1").arg(command.tag);
    case Kind::ReleaseNotes:
        return QStringLiteral("Write release notes for %1").arg(command.tag);
    case Kind::ReleaseCreate: return QStringLiteral("Create release %1").arg(command.tag);
    case Kind::ReleasePublish:
        return QStringLiteral("Publish release %1 to the mirrors").arg(command.tag);
    case Kind::Unknown: break;
    }
    return command.command;
}

QString ageLabel(qint64 ageMs)
{
    const qint64 minutes = qMax<qint64>(0, ageMs) / 60000;
    if (minutes < 1)
        return QStringLiteral("just now");
    if (minutes < 60)
        return minutes == 1 ? QStringLiteral("1 minute ago")
                            : QStringLiteral("%1 minutes ago").arg(minutes);
    const qint64 hours = minutes / 60;
    return hours == 1 ? QStringLiteral("1 hour ago")
                      : QStringLiteral("%1 hours ago").arg(hours);
}

QString promptText(const Command &command, const QString &owner,
                   const QString &repo, const Verdict &verdict, qint64 nowMs)
{
    QStringList lines;
    lines.append(QStringLiteral("%1 asks this desktop to:")
                     .arg(verdict.signerLabel.isEmpty()
                              ? QStringLiteral("A device")
                              : verdict.signerLabel));
    lines.append(QString());
    lines.append(QStringLiteral("%1 in %2/%3.")
                     .arg(actionLabel(command), owner, repo));
    lines.append(QString());
    lines.append(QStringLiteral("Signed by %1 (key %2), requested %3.")
                     .arg(command.ownerAccount,
                          keyFingerprint(verdict.signerKey),
                          ageLabel(nowMs - command.tsMs)));
    if (verdict.status == Status::UnrecognizedSigner)
        lines.append(QStringLiteral(
            "This key is not one of the keys this desktop knows for your "
            "account. Approve only if you just asked for this from your own "
            "phone."));
    if (isDestructive(command.kind))
        lines.append(QStringLiteral("This cannot be undone."));
    return lines.join(QLatin1Char('\n'));
}

QJsonObject ackBody(const QList<qint64> &ids,
                    const QHash<qint64, Outcome> &outcomes)
{
    QJsonArray idArray;
    QJsonArray results;
    for (qint64 id : ids) {
        if (id <= 0 || idArray.size() >= kMaxCommandsPerDrain)
            continue;
        idArray.append(double(id));
        results.append(QJsonObject{
            {QStringLiteral("id"), double(id)},
            {QStringLiteral("outcome"),
             outcomeCode(outcomes.value(id, Outcome::Rejected))},
        });
    }
    return QJsonObject{{QStringLiteral("ids"), idArray},
                       {QStringLiteral("results"), results}};
}

QJsonObject auditRecord(const Command &command, const QString &owner,
                        const QString &repo, const QString &signerLabel,
                        Outcome outcome, const QString &detail, qint64 nowMs)
{
    return QJsonObject{
        {QStringLiteral("at"), double(nowMs)},
        {QStringLiteral("repo"), owner + QLatin1Char('/') + repo},
        {QStringLiteral("id"), double(command.id)},
        {QStringLiteral("command"), command.command.left(64)},
        {QStringLiteral("target"), command.target.left(160)},
        {QStringLiteral("ts"), command.ts},
        {QStringLiteral("signer"), keyFingerprint(command.signer)},
        {QStringLiteral("signerLabel"), signerLabel.left(160)},
        {QStringLiteral("outcome"), outcomeCode(outcome)},
        {QStringLiteral("detail"), detail.left(240)},
    };
}

} // namespace forkmesh::desktopcommands
