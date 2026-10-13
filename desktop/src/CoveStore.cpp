#include "CoveStore.h"

#include "CoveCrypto.h"
#include "ForkMeshIdentity.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRandomGenerator>
#include <QUuid>

namespace {

constexpr int kGitTimeoutMs = 10000;
constexpr int kCoveVersion = 2;
constexpr int kContentKeyBytes = 32;
constexpr int kGrantSlotBlock = 4;
// AES-GCM nonce/tag sizes, mirrored from CoveCrypto so decoy slots are
// byte-for-byte indistinguishable from real key wraps.
constexpr int kGrantNonceBytes = 12;
constexpr int kGrantTagBytes = 16;

bool runGit(const QString &dir, const QStringList &args, QByteArray *output = nullptr,
            QString *errText = nullptr, int timeoutMs = kGitTimeoutMs)
{
    QProcess process;
    process.start("git", QStringList{"-C", dir} + args);
    if (!process.waitForFinished(timeoutMs)) {
        if (errText)
            *errText = QStringLiteral("git timed out");
        return false;
    }
    if (output)
        *output = process.readAllStandardOutput();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (errText)
            *errText =
                QString::fromUtf8(process.readAllStandardError()).trimmed().left(200);
        return false;
    }
    return true;
}

QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// An opaque, content-free file stem (32 hex chars). Cove files are named by this
// alone so a repo's tree/history never hints at what a cove holds — the name is
// kept inside the encrypted payload, not in the filename.
QString obscureSlug()
{
    return QUuid::createUuid().toString(QUuid::Id128);
}

QString normalizedAccount(QString account)
{
    return account.trimmed().toLower();
}

QStringList normalizedAccounts(const QStringList &accounts)
{
    QStringList out;
    for (const QString &account : accounts) {
        const QString normalized = normalizedAccount(account);
        if (!normalized.isEmpty() && !out.contains(normalized))
            out << normalized;
    }
    out.sort(Qt::CaseInsensitive);
    return out;
}

// Legacy (v1) account-cove secret: derived from envelope fields that used to be
// stored in plaintext. Kept only so coves written before v2 still unlock; every
// save rewrites them as v2 (random content key + per-account key wraps).
QString accountCoveSecretV1(const Cove &cove)
{
    if (!cove.accountScoped())
        return {};
    QStringList invitees = normalizedAccounts(cove.invitedAccounts);
    const QString creator = normalizedAccount(cove.creatorAccount);
    if (!creator.isEmpty() && !invitees.contains(creator))
        invitees << creator;
    invitees.sort(Qt::CaseInsensitive);
    const QByteArray material =
        QStringLiteral("forkmesh-account-cove-v1\n%1\n%2\n%3\n%4")
            .arg(cove.id, cove.creator, creator, invitees.join('\n'))
            .toUtf8();
    return QString::fromLatin1(
        QCryptographicHash::hash(material, QCryptographicHash::Sha256)
            .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

// The per-account secret a v2 grant's wrapping key is derived from. Contains no
// list of members, so knowing one account name proves nothing about the others.
QString grantSecret(const QString &coveId, const QString &account)
{
    return QStringLiteral("forkmesh-cove-grant-v2\n%1\n%2")
        .arg(coveId, normalizedAccount(account));
}

// Parse a decrypted payload into the cove. Everything identifying — name, access
// mode, creator pubkey/account, invitees, notify flag, creation time — lives in
// here (v2) rather than in the plaintext envelope; missing keys (legacy v1
// payloads) leave whatever the envelope provided.
bool applyCovePayload(Cove &cove, const QByteArray &plain)
{
    const QJsonDocument doc = QJsonDocument::fromJson(plain);
    if (!doc.isObject())
        return false;
    const QJsonObject payload = doc.object();
    cove.name = payload.value("name").toString(cove.name);
    cove.accessMode = payload.value("mode").toString(
        cove.accessMode.isEmpty() ? QStringLiteral("password") : cove.accessMode);
    cove.creator = payload.value("creator").toString(cove.creator);
    cove.creatorAccount = normalizedAccount(
        payload.value("creatorAccount").toString(cove.creatorAccount));
    if (payload.contains(QStringLiteral("invitedAccounts"))) {
        cove.invitedAccounts.clear();
        for (const QJsonValue &v : payload.value("invitedAccounts").toArray()) {
            const QString account = normalizedAccount(v.toString());
            if (!account.isEmpty() && !cove.invitedAccounts.contains(account))
                cove.invitedAccounts << account;
        }
    }
    if (payload.contains(QStringLiteral("notifyOnOpen")))
        cove.notifyOnOpen = payload.value("notifyOnOpen").toBool();
    if (payload.contains(QStringLiteral("createdAtMs")))
        cove.createdAtMs = qint64(payload.value("createdAtMs").toDouble());
    cove.documents.clear();
    for (const QJsonValue &v : payload.value("documents").toArray())
        cove.documents << CoveDocument::fromJson(v.toObject());
    cove.accessLog.clear();
    for (const QJsonValue &v : payload.value("accessLog").toArray())
        cove.accessLog << CoveAccessEntry::fromJson(v.toObject());
    cove.unlocked = true;
    return true;
}

bool decryptCoveWithSecret(Cove &cove, const QString &secret)
{
    CoveCrypto crypto(secret, cove.salt, cove.rounds);
    if (!crypto.isValid())
        return false;
    const QByteArray plain = crypto.decrypt(cove.cipher);
    if (plain.isEmpty())
        return false; // wrong secret (GCM tag mismatch) or corrupt payload
    return applyCovePayload(cove, plain);
}

bool decryptCoveWithKey(Cove &cove, const QByteArray &contentKey)
{
    const CoveCrypto crypto = CoveCrypto::withKey(contentKey);
    if (!crypto.isValid())
        return false;
    const QByteArray plain = crypto.decrypt(cove.cipher);
    if (plain.isEmpty())
        return false;
    return applyCovePayload(cove, plain);
}

QByteArray covePayload(const Cove &cove)
{
    QJsonArray docs;
    for (const CoveDocument &d : cove.documents)
        docs.append(d.toJson());
    QJsonArray log;
    for (const CoveAccessEntry &e : cove.accessLog)
        log.append(e.toJson());
    QJsonArray invited;
    for (const QString &account : normalizedAccounts(cove.invitedAccounts))
        invited.append(account);
    QJsonObject payload{{"name", cove.name},
                        {"mode", cove.accessMode},
                        {"creator", cove.creator},
                        {"notifyOnOpen", cove.notifyOnOpen},
                        {"createdAtMs", double(cove.createdAtMs)},
                        {"documents", docs},
                        {"accessLog", log}};
    if (cove.accountScoped()) {
        payload.insert(QStringLiteral("creatorAccount"),
                       normalizedAccount(cove.creatorAccount));
        payload.insert(QStringLiteral("invitedAccounts"), invited);
    }
    return QJsonDocument(payload).toJson(QJsonDocument::Compact);
}

// A decoy grant slot: random bytes shaped exactly like a real AES-GCM key wrap
// (12-byte nonce, 16-byte tag, 32-byte body), so an observer cannot tell decoys
// from real grants — and therefore cannot tell a password cove from an account
// cove, nor count its members.
QJsonObject decoyGrant()
{
    return {{"nonce", QString::fromLatin1(
                 CoveCrypto::randomBytes(kGrantNonceBytes).toBase64())},
            {"tag", QString::fromLatin1(
                 CoveCrypto::randomBytes(kGrantTagBytes).toBase64())},
            {"body", QString::fromLatin1(
                 CoveCrypto::randomBytes(kContentKeyBytes).toBase64())}};
}

QJsonArray buildGrants(const Cove &cove, const QByteArray &contentKey)
{
    QList<QJsonObject> slotList; // "slots" itself would collide with Qt's macro
    if (cove.accountScoped()) {
        QStringList accounts = normalizedAccounts(cove.invitedAccounts);
        const QString creator = normalizedAccount(cove.creatorAccount);
        if (!creator.isEmpty() && !accounts.contains(creator))
            accounts << creator;
        for (const QString &account : std::as_const(accounts)) {
            CoveCrypto kek(grantSecret(cove.id, account), cove.salt, cove.rounds);
            if (!kek.isValid())
                return {};
            const QJsonObject wrap = kek.encrypt(contentKey);
            if (wrap.isEmpty())
                return {};
            slotList << wrap;
        }
    }
    int target = qMax(int(slotList.size()), 1);
    target = ((target + kGrantSlotBlock - 1) / kGrantSlotBlock) * kGrantSlotBlock;
    while (slotList.size() < target)
        slotList << decoyGrant();
    for (int i = int(slotList.size()) - 1; i > 0; --i)
        slotList.swapItemsAt(i, int(QRandomGenerator::global()->bounded(i + 1)));
    QJsonArray out;
    for (const QJsonObject &grant : std::as_const(slotList))
        out.append(grant);
    return out;
}

QJsonObject toEnvelope(const Cove &cove)
{
    return {{"kind", "cove"},
            {"v", kCoveVersion},
            {"id", cove.id},
            {"kdf", QJsonObject{{"algo", "pbkdf2-sha256"},
                                {"rounds", cove.rounds},
                                {"salt", QString::fromLatin1(cove.salt.toBase64())}}},
            {"grants", cove.grants},
            {"cipher", cove.cipher}};
}

bool fromEnvelope(const QByteArray &bytes, Cove &out)
{
    const QJsonDocument doc = QJsonDocument::fromJson(bytes);
    if (!doc.isObject())
        return false;
    const QJsonObject obj = doc.object();
    if (obj.value("kind").toString() != "cove")
        return false;
    out.id = obj.value("id").toString();
    out.version = obj.value("v").toInt(1);
    out.name = obj.value("name").toString();
    out.creator = obj.value("creator").toString();
    const QJsonObject access = obj.value("access").toObject();
    out.accessMode = access.value("mode").toString(
        out.version >= 2 ? QString() : QStringLiteral("password"));
    out.creatorAccount = normalizedAccount(access.value("creatorAccount").toString());
    out.invitedAccounts.clear();
    for (const QJsonValue &v : access.value("invitedAccounts").toArray()) {
        const QString account = normalizedAccount(v.toString());
        if (!account.isEmpty() && !out.invitedAccounts.contains(account))
            out.invitedAccounts << account;
    }
    out.createdAtMs = qint64(obj.value("createdAtMs").toDouble());
    out.notifyOnOpen = obj.value("notifyOnOpen").toBool();
    out.grants = obj.value("grants").toArray();
    const QJsonObject kdf = obj.value("kdf").toObject();
    out.salt = QByteArray::fromBase64(kdf.value("salt").toString().toLatin1());
    out.rounds = kdf.value("rounds").toInt();
    out.cipher = obj.value("cipher").toObject();
    return !out.id.isEmpty() && !out.salt.isEmpty() && out.rounds > 0;
}

} // namespace


QJsonObject CoveDocument::toJson() const
{
    return {{"id", id}, {"name", name}, {"mime", mime}, {"body", body},
            {"updatedAtMs", double(updatedAtMs)}};
}

CoveDocument CoveDocument::fromJson(const QJsonObject &obj)
{
    CoveDocument d;
    d.id = obj.value("id").toString();
    d.name = obj.value("name").toString();
    d.mime = obj.value("mime").toString(QStringLiteral("text/markdown"));
    d.body = obj.value("body").toString();
    d.updatedAtMs = qint64(obj.value("updatedAtMs").toDouble());
    return d;
}

QJsonObject CoveAccessEntry::toJson() const
{
    return {{"who", who}, {"name", name}, {"node", node},
            {"ts", double(ts)}, {"action", action}};
}

CoveAccessEntry CoveAccessEntry::fromJson(const QJsonObject &obj)
{
    CoveAccessEntry e;
    e.who = obj.value("who").toString();
    e.name = obj.value("name").toString();
    e.node = obj.value("node").toString();
    e.ts = qint64(obj.value("ts").toDouble());
    e.action = obj.value("action").toString(QStringLiteral("open"));
    return e;
}

bool Cove::createdByMe(const ForkMeshIdentity *identity) const
{
    return identity && identity->isValid() && !creator.isEmpty() &&
           creator == identity->publicKey();
}


CoveStore::CoveStore(QString workTreePath, QString mirrorPath,
                     const ForkMeshIdentity *identity, QString authorName)
    : m_workTree(std::move(workTreePath)), m_mirror(std::move(mirrorPath)),
      m_identity(identity), m_authorName(std::move(authorName))
{
}

bool CoveStore::canWrite() const
{
    if (m_workTree.isEmpty() || !m_identity || !m_identity->isValid())
        return false;
    return QFileInfo::exists(m_workTree + "/.git");
}

QString CoveStore::covesDir() const { return m_workTree + "/" + covesDirRel(); }

QString CoveStore::mirrorRef() const
{
    QByteArray output;
    if (runGit(m_mirror, {"rev-parse", "--verify", "-q", "HEAD"}, &output) &&
        !output.trimmed().isEmpty())
        return QStringLiteral("HEAD");
    if (runGit(m_mirror,
               {"for-each-ref", "--format=%(refname)", "--count=1", "refs/heads/"},
               &output)) {
        const QString ref = QString::fromUtf8(output).trimmed();
        if (!ref.isEmpty())
            return ref;
    }
    return QString();
}

QByteArray CoveStore::readCoveBytes(const QString &relPath, bool *ok) const
{
    if (!m_workTree.isEmpty()) {
        QFile file(m_workTree + "/" + relPath);
        if (file.open(QIODevice::ReadOnly)) {
            if (ok)
                *ok = true;
            return file.readAll();
        }
    }
    if (!m_mirror.isEmpty()) {
        const QString ref = mirrorRef();
        QByteArray output;
        if (!ref.isEmpty() &&
            runGit(m_mirror, {"show", ref + ":" + relPath}, &output)) {
            if (ok)
                *ok = true;
            return output;
        }
    }
    if (ok)
        *ok = false;
    return {};
}

QList<Cove> CoveStore::listCoves(QString *error) const
{
    QList<Cove> coves;
    QStringList relPaths;

    if (!m_workTree.isEmpty() && QFileInfo::exists(covesDir())) {
        const QStringList files = QDir(covesDir()).entryList(
            {QStringLiteral("*.cove")}, QDir::Files, QDir::Name);
        for (const QString &f : files)
            relPaths << covesDirRel() + "/" + f;
    } else if (!m_mirror.isEmpty()) {
        const QString ref = mirrorRef();
        QByteArray listing;
        if (!ref.isEmpty() &&
            runGit(m_mirror, {"ls-tree", ref, covesDirRel() + "/"}, &listing)) {
            for (const QString &line :
                 QString::fromUtf8(listing).split('\n', Qt::SkipEmptyParts)) {
                const int tab = line.indexOf('\t');
                if (tab < 0)
                    continue;
                const QString path = line.mid(tab + 1);
                if (path.endsWith(QStringLiteral(".cove")))
                    relPaths << path;
            }
        }
    }

    for (const QString &relPath : std::as_const(relPaths)) {
        Cove cove;
        if (loadEnvelope(relPath, cove))
            coves << cove;
    }
    if (coves.isEmpty() && error)
        error->clear();
    return coves;
}

bool CoveStore::loadEnvelope(const QString &relPath, Cove &out, QString *error) const
{
    bool ok = false;
    const QByteArray bytes = readCoveBytes(relPath, &ok);
    if (!ok) {
        if (error)
            *error = QStringLiteral("Could not read %1").arg(relPath);
        return false;
    }
    if (!fromEnvelope(bytes, out)) {
        if (error)
            *error = QStringLiteral("%1 is not a valid cove file").arg(relPath);
        return false;
    }
    out.relPath = relPath;
    out.slug = QFileInfo(relPath).completeBaseName();
    return true;
}

bool CoveStore::unlock(Cove &cove, const QString &password)
{
    return decryptCoveWithSecret(cove, password);
}

bool CoveStore::accountCanAccess(const Cove &cove, const QString &accountName)
{
    if (!cove.accountScoped())
        return false;
    const QString account = normalizedAccount(accountName);
    if (account.isEmpty())
        return false;
    if (normalizedAccount(cove.creatorAccount) == account)
        return true;
    return normalizedAccounts(cove.invitedAccounts).contains(account);
}

bool CoveStore::unlockForAccount(Cove &cove, const QString &accountName)
{
    const QString account = normalizedAccount(accountName);
    if (account.isEmpty() || cove.salt.isEmpty() || cove.rounds <= 0)
        return false;
    if (!cove.grants.isEmpty()) {
        // v2: prove membership by unwrapping a key grant with a secret only this
        // account can derive. Decoys and other accounts' grants fail the GCM tag.
        CoveCrypto kek(grantSecret(cove.id, account), cove.salt, cove.rounds);
        if (!kek.isValid())
            return false;
        for (const QJsonValue &slot : std::as_const(cove.grants)) {
            const QByteArray key = kek.decrypt(slot.toObject());
            if (key.size() != kContentKeyBytes)
                continue;
            if (!decryptCoveWithKey(cove, key))
                return false; // unwrapped a grant but the body is corrupt
            cove.contentKey = key;
            return true;
        }
        return false;
    }
    // Legacy v1: the ACL (and the secret derived from it) sit in the plaintext
    // envelope. Still honored so pre-v2 coves open; the next save rewrites them.
    if (!accountCanAccess(cove, account))
        return false;
    return decryptCoveWithSecret(cove, accountCoveSecretV1(cove));
}

void CoveStore::appendAccess(Cove &cove, const CoveAccessEntry &entry)
{
    cove.accessLog.append(entry);
    constexpr int kMaxAccess = 500;
    while (cove.accessLog.size() > kMaxAccess)
        cove.accessLog.removeFirst();
}

QString CoveStore::uniqueSlug() const
{
    QString slug = obscureSlug();
    while (QFileInfo::exists(covesDir() + "/" + slug + ".cove"))
        slug = obscureSlug();
    return slug;
}

bool CoveStore::createCove(const QString &name, const QString &password,
                           bool notifyOnOpen, const QList<CoveDocument> &documents,
                           Cove *out, QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository has no local working tree to "
                                    "store a cove in.");
        return false;
    }
    if (name.trimmed().isEmpty() || password.isEmpty()) {
        if (error)
            *error = QStringLiteral("A cove needs a name and a password.");
        return false;
    }

    Cove cove;
    cove.id = newId();
    cove.name = name.trimmed();
    cove.slug = uniqueSlug();
    cove.relPath = covesDirRel() + "/" + cove.slug + ".cove";
    cove.creator = m_identity ? m_identity->publicKey() : QString();
    cove.createdAtMs = QDateTime::currentMSecsSinceEpoch();
    cove.notifyOnOpen = notifyOnOpen;
    cove.salt = CoveCrypto::randomSalt();
    cove.rounds = CoveCrypto::defaultRounds();
    cove.documents = documents;
    cove.unlocked = true;

    if (!save(cove, password, error))
        return false;
    if (out)
        *out = cove;
    return true;
}

bool CoveStore::createAccountCove(const QString &name, const QString &creatorAccount,
                                  const QStringList &invitedAccounts,
                                  bool notifyOnOpen,
                                  const QList<CoveDocument> &documents, Cove *out,
                                  QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository has no local working tree to "
                                    "store a cove in.");
        return false;
    }
    const QString creatorAccountNorm = normalizedAccount(creatorAccount);
    if (name.trimmed().isEmpty() || creatorAccountNorm.isEmpty()) {
        if (error)
            *error = QStringLiteral("A cove needs a name and a creator account.");
        return false;
    }

    Cove cove;
    cove.id = newId();
    cove.name = name.trimmed();
    cove.slug = uniqueSlug();
    cove.relPath = covesDirRel() + "/" + cove.slug + ".cove";
    cove.creator = m_identity ? m_identity->publicKey() : QString();
    cove.accessMode = QStringLiteral("account");
    cove.creatorAccount = creatorAccountNorm;
    cove.invitedAccounts = normalizedAccounts(invitedAccounts);
    if (!cove.invitedAccounts.contains(creatorAccountNorm))
        cove.invitedAccounts << creatorAccountNorm;
    cove.invitedAccounts.sort(Qt::CaseInsensitive);
    cove.createdAtMs = QDateTime::currentMSecsSinceEpoch();
    cove.notifyOnOpen = notifyOnOpen;
    cove.salt = CoveCrypto::randomSalt();
    cove.rounds = CoveCrypto::defaultRounds();
    cove.contentKey = CoveCrypto::randomKey();
    cove.documents = documents;
    cove.unlocked = true;

    if (!saveAccountCove(cove, error))
        return false;
    if (out)
        *out = cove;
    return true;
}

bool CoveStore::save(const Cove &cove, const QString &password, QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("No local working tree to save the cove to.");
        return false;
    }
    if (cove.accountScoped()) {
        if (error)
            *error = QStringLiteral("Account-scoped coves are saved without a "
                                    "password.");
        return false;
    }
    if (cove.relPath.isEmpty() || cove.salt.isEmpty() || cove.rounds <= 0) {
        if (error)
            *error = QStringLiteral("The cove is missing its encryption parameters.");
        return false;
    }

    CoveCrypto crypto(password, cove.salt, cove.rounds);
    if (!crypto.isValid()) {
        if (error)
            *error = crypto.errorString();
        return false;
    }
    Cove sealed = cove;
    if (sealed.accessMode.isEmpty())
        sealed.accessMode = QStringLiteral("password");
    sealed.cipher = crypto.encrypt(covePayload(sealed));
    if (sealed.cipher.isEmpty()) {
        if (error)
            *error = QStringLiteral("Could not encrypt the cove.");
        return false;
    }
    sealed.grants = buildGrants(sealed, QByteArray());
    if (sealed.grants.isEmpty()) {
        if (error)
            *error = QStringLiteral("Could not seal the cove's access grants.");
        return false;
    }

    QDir().mkpath(covesDir());
    const QString absPath = m_workTree + "/" + sealed.relPath;
    QFile file(absPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("Could not write %1").arg(absPath);
        return false;
    }
    file.write(QJsonDocument(toEnvelope(sealed)).toJson(QJsonDocument::Indented));
    file.close();

    return commit(QStringLiteral("cove: %1").arg(sealed.slug), sealed.relPath, error);
}

bool CoveStore::saveAccountCove(const Cove &cove, QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("No local working tree to save the cove to.");
        return false;
    }
    QByteArray envelope;
    if (!encryptedAccountEnvelope(cove, &envelope, error))
        return false;

    QDir().mkpath(covesDir());
    const QString absPath = m_workTree + "/" + cove.relPath;
    QFile file(absPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("Could not write %1").arg(absPath);
        return false;
    }
    if (file.write(envelope) != envelope.size()) {
        if (error)
            *error = QStringLiteral("Could not write %1").arg(absPath);
        return false;
    }
    file.close();

    return commit(QStringLiteral("cove: %1").arg(cove.slug), cove.relPath, error);
}

bool CoveStore::encryptedAccountEnvelope(const Cove &cove, QByteArray *out,
                                         QString *error)
{
    if (!out) {
        if (error)
            *error = QStringLiteral("No output buffer was provided.");
        return false;
    }
    out->clear();
    if (!cove.accountScoped()) {
        if (error)
            *error = QStringLiteral("This cove is not account-scoped.");
        return false;
    }
    if (!cove.unlocked) {
        if (error)
            *error = QStringLiteral("Unlock the cove before saving it.");
        return false;
    }
    if (cove.relPath.isEmpty() || cove.salt.isEmpty() || cove.rounds <= 0) {
        if (error)
            *error = QStringLiteral("The cove is missing its encryption parameters.");
        return false;
    }
    Cove sealed = cove;
    sealed.invitedAccounts = normalizedAccounts(sealed.invitedAccounts);
    sealed.creatorAccount = normalizedAccount(sealed.creatorAccount);
    // Coves opened via the legacy v1 secret have no content key yet; minting one
    // here upgrades them to the anonymous v2 envelope on their next save.
    if (sealed.contentKey.size() != kContentKeyBytes)
        sealed.contentKey = CoveCrypto::randomKey();
    const CoveCrypto crypto = CoveCrypto::withKey(sealed.contentKey);
    if (!crypto.isValid()) {
        if (error)
            *error = crypto.errorString();
        return false;
    }
    sealed.cipher = crypto.encrypt(covePayload(sealed));
    if (sealed.cipher.isEmpty()) {
        if (error)
            *error = QStringLiteral("Could not encrypt the cove.");
        return false;
    }
    sealed.grants = buildGrants(sealed, sealed.contentKey);
    if (sealed.grants.isEmpty()) {
        if (error)
            *error = QStringLiteral("Could not seal the cove's access grants.");
        return false;
    }
    *out = QJsonDocument(toEnvelope(sealed)).toJson(QJsonDocument::Indented);
    return !out->isEmpty();
}

bool CoveStore::commit(const QString &message, const QString &relPath, QString *error) const
{
    QString err;
    if (!runGit(m_workTree, {"add", "--", relPath}, nullptr, &err)) {
        if (error)
            *error = "git add failed: " + err;
        return false;
    }
    if (!runGit(m_workTree,
                {"-c", "user.name=forkmesh", "-c", "user.email=coves@forkmesh.invalid",
                 "commit", "-m", message, "--", relPath},
                nullptr, &err)) {
        if (err.contains("nothing to commit") || err.isEmpty())
            return true; // no change to persist — not an error
        if (error)
            *error = "git commit failed: " + err;
        return false;
    }
    return true;
}
