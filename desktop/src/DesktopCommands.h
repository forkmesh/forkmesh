#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace forkmesh::desktopcommands {

constexpr qint64 kMaxSkewMs = 5LL * 60 * 1000;
constexpr qint64 kRetentionMs = 24LL * 60 * 60 * 1000;
constexpr qint64 kLedgerKeepMs = kRetentionMs + 2 * kMaxSkewMs;
constexpr int kMaxCommandsPerDrain = 50;
constexpr int kMaxLedgerEntries = 4096;
constexpr int kMaxSignatureChars = 200;

enum class Kind {
    Unknown,
    PullMerge,
    PullMergeDelete,
    PullFixAgent,
    PullClose,
    PullReopen,
    PullDelete,
    ActionRerun,
    ReleaseDraft,
    ReleaseNotes,
    ReleaseCreate,
    ReleasePublish,
};

enum class Status {
    Ok,
    UnrecognizedSigner,
    Malformed,
    UnknownKind,
    BadTarget,
    WrongOwner,
    WrongRepo,
    Expired,
    FromFuture,
    BadSignature,
    Replayed,
};

enum class Outcome {
    Done,
    Started,
    Failed,
    Declined,
    Rejected,
};

struct Command {
    qint64 id = 0;
    QString command;
    QString target;
    Kind kind = Kind::Unknown;
    int pullNumber = 0;
    QString runId;
    QString tag;
    QString ts;
    qint64 tsMs = 0;
    QString sig;
    QString signer;
    QString ownerAccount;
    QString repoOwner;
    QString relayCanonical;
    qint64 queuedAtMs = 0;
};

struct TrustedKey {
    QString key;
    QString label;
};

struct Verdict {
    Status status = Status::Malformed;
    QString signerKey;
    QString signerLabel;
};

QString signingDomain();
QStringList supportedCommands();
Kind kindFor(const QString &command);
bool isPullKind(Kind kind);
bool isReleaseKind(Kind kind);
bool isDestructive(Kind kind);
bool runsUnattended(Kind kind);
bool parseTarget(Kind kind, const QString &target, Command *command);
QString signedOwner(const Command &command, const QString &owner);

QByteArray canonical(const QString &owner, const QString &repo,
                     const QString &command, const QString &target,
                     const QString &ts);
bool verifyEd25519(const QString &publicKeyB64url,
                   const QString &signatureB64url, const QByteArray &data);
QString keyFingerprint(const QString &publicKeyB64url);
QString signatureDigest(const QString &signature);

Command parse(const QJsonObject &object);
QList<Command> parseDrain(const QJsonObject &payload);

class ReplayLedger
{
public:
    bool contains(const QString &signature) const;
    bool containsId(qint64 id) const;
    void record(const Command &command, Outcome outcome, qint64 nowMs);
    void prune(qint64 nowMs);
    int size() const { return m_entries.size(); }
    Outcome outcomeFor(const QString &signature, bool *found = nullptr) const;
    QJsonObject toJson() const;
    static ReplayLedger fromJson(const QJsonObject &object);

private:
    struct Entry {
        qint64 id = 0;
        qint64 seenAtMs = 0;
        Outcome outcome = Outcome::Rejected;
    };
    QHash<QString, Entry> m_entries;
};

Verdict verify(const Command &command, const QString &owner,
               const QString &repo, const QString &account,
               const QList<TrustedKey> &trusted, const ReplayLedger &ledger,
               qint64 nowMs);

QString statusCode(Status status);
QString statusText(Status status);
QString outcomeCode(Outcome outcome);
QString actionLabel(const Command &command);
QString promptText(const Command &command, const QString &owner,
                   const QString &repo, const Verdict &verdict, qint64 nowMs);
QString ageLabel(qint64 ageMs);
QJsonObject ackBody(const QList<qint64> &ids,
                    const QHash<qint64, Outcome> &outcomes);
QJsonObject auditRecord(const Command &command, const QString &owner,
                        const QString &repo, const QString &signerLabel,
                        Outcome outcome, const QString &detail, qint64 nowMs);

} // namespace forkmesh::desktopcommands
