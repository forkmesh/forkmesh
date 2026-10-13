#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

class ForkMeshIdentity;

// One document inside a cove (a note, a password list, a plan). The body is only
// ever present in memory after the cove is unlocked; on disk it lives inside the
// AES-256-GCM ciphertext.
struct CoveDocument {
    QString id;
    QString name;
    QString mime = "text/markdown";
    QString body;
    qint64 updatedAtMs = 0;

    QJsonObject toJson() const;
    static CoveDocument fromJson(const QJsonObject &obj);
};

// One entry in a cove's access log: who opened (or edited) it, and when. Kept
// inside the encrypted payload so the creator sees the trail once it syncs.
struct CoveAccessEntry {
    QString who;    // opener pubkey (base64url)
    QString name;   // opener display name
    QString node;   // opener node id (short)
    qint64 ts = 0;
    QString action = "open"; // open | edit

    QJsonObject toJson() const;
    static CoveAccessEntry fromJson(const QJsonObject &obj);
};

// A cove: an anonymous on-disk envelope (nothing identifying is readable while
// locked) plus the decrypted payload, present only after a successful unlock().
// v2 envelopes carry only {id, kdf, grants, cipher}: the name, creator pubkey,
// access mode, creator/invited accounts, notify flag and creation time all live
// inside the ciphertext. Access-identity fields below are therefore empty until
// unlock() succeeds (legacy v1 envelopes stored them in plaintext and still
// populate them on load, for back-compat).
struct Cove {
    QString id;
    QString name;        // human-readable; kept inside the ciphertext, not the repo
    QString slug;        // obscure file stem — reveals nothing about the cove
    QString relPath;     // .forkmesh/coves/<slug>.cove (repo-relative)
    int version = 1;     // envelope version read from disk (new coves save as v2)
    QString creator;     // creator pubkey (base64url) — routes open-notifications
    QString accessMode = "password"; // password | account (empty while a v2 cove is locked)
    QString creatorAccount;          // account-scoped owner username
    QStringList invitedAccounts;     // account-scoped viewers/editors
    qint64 createdAtMs = 0;
    bool notifyOnOpen = false;
    QByteArray salt;     // KDF salt (raw bytes)
    int rounds = 0;      // KDF rounds
    QJsonObject cipher;  // {nonce,tag,body} base64
    QJsonArray grants;   // v2: per-account key wraps + decoy slots (opaque)

    bool unlocked = false;
    QByteArray contentKey; // v2 account coves: the unwrapped 32-byte body key
    QList<CoveDocument> documents;
    QList<CoveAccessEntry> accessLog;

    bool createdByMe(const ForkMeshIdentity *identity) const;
    bool accountScoped() const { return accessMode == QStringLiteral("account"); }
};

// Repo-scoped vault of encrypted coves backed by.forkmesh/coves/*.cove. For
// repos with a local working tree the store reads/writes/commits files directly;
// for mirror-only repos it lists + reads envelopes read-only via `git show`.
class CoveStore
{
public:
    CoveStore(QString workTreePath, QString mirrorPath,
              const ForkMeshIdentity *identity, QString authorName = QString());

    bool canWrite() const;

    // List cove envelopes (metadata only; nothing is decrypted).
    QList<Cove> listCoves(QString *error = nullptr) const;
    bool loadEnvelope(const QString &relPath, Cove &out, QString *error = nullptr) const;

    // Decrypt a cove with a password. On success fills documents/accessLog and
    // sets unlocked. Returns false (cove untouched) when the password is wrong.
    static bool unlock(Cove &cove, const QString &password);
    // Whether an account is on the cove's ACL. For v2 envelopes the ACL is
    // encrypted, so this only answers from in-memory fields (unlocked coves and
    // legacy v1 plaintext envelopes) — membership of a locked v2 cove is proven
    // by unlockForAccount() unwrapping one of its key grants.
    static bool accountCanAccess(const Cove &cove, const QString &accountName);
    static bool unlockForAccount(Cove &cove, const QString &accountName);

    // Create a new cove, encrypt it, write the.cove file and commit it.
    bool createCove(const QString &name, const QString &password, bool notifyOnOpen,
                    const QList<CoveDocument> &documents, Cove *out,
                    QString *error = nullptr);
    bool createAccountCove(const QString &name, const QString &creatorAccount,
                           const QStringList &invitedAccounts, bool notifyOnOpen,
                           const QList<CoveDocument> &documents, Cove *out,
                           QString *error = nullptr);
    // Re-encrypt an unlocked cove's documents + access log under the password,
    // write the file and commit. The cove must already be unlocked.
    bool save(const Cove &cove, const QString &password, QString *error = nullptr);
    bool saveAccountCove(const Cove &cove, QString *error = nullptr);
    // Seal an unlocked account cove without writing it. Mirror-only members use
    // these bytes to propose an encrypted edit through the repository's signed
    // pull-request path.
    static bool encryptedAccountEnvelope(const Cove &cove, QByteArray *out,
                                         QString *error = nullptr);

    static void appendAccess(Cove &cove, const CoveAccessEntry &entry);

    static QString covesDirRel() { return QStringLiteral(".forkmesh/coves"); }

private:
    QString covesDir() const; // <workTree>/.forkmesh/coves
    QByteArray readCoveBytes(const QString &relPath, bool *ok) const;
    bool commit(const QString &message, const QString &relPath, QString *error) const;
    QString mirrorRef() const;
    QString uniqueSlug() const; // obscure, name-free file stem

    QString m_workTree;
    QString m_mirror;
    const ForkMeshIdentity *m_identity;
    QString m_authorName;
};
