#include "PrivateMirrorStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <limits>

namespace {

constexpr int kOpaqueIdBytes = 32;
constexpr quint64 kMaximumSafeJsonInteger = 9007199254740991ULL;

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

void clearBytes(QByteArray *bytes)
{
    if (!bytes || bytes->isEmpty())
        return;
    OPENSSL_cleanse(bytes->data(), size_t(bytes->size()));
    bytes->clear();
}

QString sha256Hex(const QByteArray &value)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(value, QCryptographicHash::Sha256).toHex());
}

bool prepareRoot(const QString &storageRoot, QString *error)
{
    if (storageRoot.trimmed().isEmpty()) {
        setError(error, QStringLiteral("A private-mirror storage root is required."));
        return false;
    }
    QDir root(storageRoot);
    if (!root.exists() && !QDir().mkpath(root.absolutePath())) {
        setError(error, QStringLiteral("Could not create the private-mirror store."));
        return false;
    }
    const QFileInfo rootInfo(root.absolutePath());
    if (!rootInfo.isDir() || rootInfo.isSymLink()) {
        setError(
            error,
            QStringLiteral("The private-mirror storage root must be a real local directory."));
        return false;
    }
    QFile rootPermissions(root.absolutePath());
    if (!rootPermissions.setPermissions(
            QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner)) {
        setError(error,
                 QStringLiteral("Could not restrict the private-mirror store."));
        return false;
    }
    return true;
}

QStringList recipientIds(const QJsonObject &envelope)
{
    QStringList result;
    for (const QJsonValue &value :
         envelope.value(QStringLiteral("recipients")).toArray()) {
        const QString id =
            value.toObject().value(QStringLiteral("kid")).toString();
        if (!id.isEmpty())
            result.append(id);
    }
    result.sort();
    return result;
}

bool parseUnsignedJson(const QJsonValue &value, quint64 *result)
{
    if (!result || !value.isDouble())
        return false;
    const double number = value.toDouble(-1);
    const quint64 integer = number >= 0 ? quint64(number) : 0;
    if (number < 0 || number > double(kMaximumSafeJsonInteger) ||
        double(integer) != number)
        return false;
    *result = integer;
    return true;
}

bool parseStored(const QByteArray &bytes, const QString &expectedOpaqueId,
                 QJsonObject *stored, PrivateMirrorStore::Metadata *metadata,
                 QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        setError(error, QStringLiteral("The private replica metadata is invalid."));
        return false;
    }
    const QJsonObject object = document.object();
    const QJsonObject envelope =
        object.value(QStringLiteral("envelope")).toObject();
    quint64 epoch = 0;
    quint64 created = 0;
    quint64 updated = 0;
    const QString opaqueId =
        object.value(QStringLiteral("opaqueId")).toString();
    const QString ownerKeyId =
        object.value(QStringLiteral("ownerKeyId")).toString();
    const QString expectedHash =
        object.value(QStringLiteral("ciphertextSha256")).toString();
    const QByteArray envelopeBytes =
        QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    const QStringList recipients = recipientIds(envelope);
    if (object.value(QStringLiteral("schemaVersion")).toInt() != 2 ||
        object.value(QStringLiteral("kind")).toString() !=
            QLatin1String("forkmesh.private-replica") ||
        opaqueId != expectedOpaqueId ||
        !PrivateMirrorStore::isOpaqueId(opaqueId) ||
        !parseUnsignedJson(object.value(QStringLiteral("keyEpoch")), &epoch) ||
        epoch == 0 ||
        !parseUnsignedJson(object.value(QStringLiteral("createdAt")), &created) ||
        !parseUnsignedJson(object.value(QStringLiteral("updatedAt")), &updated) ||
        created == 0 || updated < created ||
        expectedHash.size() != 64 ||
        expectedHash != sha256Hex(envelopeBytes) ||
        envelope.value(QStringLiteral("kind")).toString() !=
            QLatin1String("forkmesh.mirror") ||
        envelope.value(QStringLiteral("v")).toInt() != 1 ||
        envelope.value(QStringLiteral("alg")).toString() !=
            QLatin1String("x25519+mlkem768/aes256gcm") ||
        envelope.value(QStringLiteral("recipients")).toArray().isEmpty() ||
        ownerKeyId.isEmpty() || !recipients.contains(ownerKeyId)) {
        setError(
            error,
            QStringLiteral("The private replica failed metadata or ciphertext verification."));
        return false;
    }
    if (stored)
        *stored = object;
    if (metadata) {
        metadata->opaqueId = opaqueId;
        metadata->ownerKeyId = ownerKeyId;
        metadata->keyEpoch = epoch;
        metadata->createdAtMs = qint64(created);
        metadata->updatedAtMs = qint64(updated);
        metadata->replicaFileSha256 = sha256Hex(bytes);
        metadata->ciphertextSha256 = expectedHash;
        metadata->recipientKeyIds = recipients;
    }
    return true;
}

bool readStored(const QString &path, const QString &opaqueId,
                QJsonObject *stored, PrivateMirrorStore::Metadata *metadata,
                QString *error)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || info.isSymLink() ||
        info.size() <= 0 || info.size() > std::numeric_limits<int>::max()) {
        setError(error, QStringLiteral("The private replica was not found."));
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("The private replica could not be opened."));
        return false;
    }
    return parseStored(file.readAll(), opaqueId, stored, metadata, error);
}

bool writeStored(const QString &path, const QJsonObject &stored,
                 QString *error)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, QStringLiteral("The encrypted private replica could not be written."));
        return false;
    }
    if (!file.setPermissions(
            QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        file.cancelWriting();
        setError(error,
                 QStringLiteral("Could not restrict the encrypted replica file."));
        return false;
    }
    const QByteArray bytes =
        QJsonDocument(stored).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        setError(error,
                 QStringLiteral("The encrypted private replica was not committed."));
        return false;
    }
    return true;
}

bool writeSerialized(const QString &path, const QByteArray &bytes,
                     QString *error)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(
            QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        file.cancelWriting();
        setError(error,
                 QStringLiteral("The encrypted private replica could not be imported."));
        return false;
    }
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        setError(error,
                 QStringLiteral("The encrypted private replica import was not committed."));
        return false;
    }
    return true;
}

QJsonObject makeStored(const QString &opaqueId, quint64 epoch,
                       qint64 createdAt, qint64 updatedAt,
                       const QString &ownerKeyId,
                       const QJsonObject &envelope)
{
    const QByteArray envelopeBytes =
        QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    return {
        {QStringLiteral("schemaVersion"), 2},
        {QStringLiteral("kind"), QStringLiteral("forkmesh.private-replica")},
        {QStringLiteral("opaqueId"), opaqueId},
        {QStringLiteral("ownerKeyId"), ownerKeyId},
        {QStringLiteral("keyEpoch"), double(epoch)},
        {QStringLiteral("createdAt"), double(createdAt)},
        {QStringLiteral("updatedAt"), double(updatedAt)},
        {QStringLiteral("ciphertextSha256"), sha256Hex(envelopeBytes)},
        {QStringLiteral("envelope"), envelope},
    };
}

} // namespace

bool PrivateMirrorStore::isOpaqueId(const QString &value)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[0-9a-f]{64}$"));
    return pattern.match(value).hasMatch();
}

QString PrivateMirrorStore::replicaPath(const QString &storageRoot,
                                        const QString &opaqueId)
{
    if (!isOpaqueId(opaqueId))
        return {};
    return QDir(storageRoot).absoluteFilePath(
        opaqueId + QStringLiteral(".fm-private"));
}

QString PrivateMirrorStore::createReplica(
    const QString &storageRoot, const QByteArray &archive,
    const QList<QJsonObject> &recipientPublicBundles, QString *error)
{
    if (archive.isEmpty()) {
        setError(error, QStringLiteral("A private mirror archive cannot be empty."));
        return {};
    }
    if (!prepareRoot(storageRoot, error))
        return {};

    QByteArray random(kOpaqueIdBytes, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(random.data()),
                   random.size()) != 1) {
        clearBytes(&random);
        setError(error, QStringLiteral("Could not generate a secure opaque replica id."));
        return {};
    }
    const QString opaqueId = QString::fromLatin1(random.toHex());
    clearBytes(&random);
    const QString path = replicaPath(storageRoot, opaqueId);
    if (path.isEmpty() || QFileInfo::exists(path)) {
        setError(error, QStringLiteral("Could not allocate an opaque replica id."));
        return {};
    }

    QString cryptoError;
    if (recipientPublicBundles.isEmpty()) {
        setError(error, QStringLiteral("A private mirror owner is required."));
        return {};
    }
    const QString ownerKeyId =
        MirrorCrypto::publicKeyId(recipientPublicBundles.constFirst());
    if (ownerKeyId.isEmpty()) {
        setError(error, QStringLiteral("The private mirror owner identity is invalid."));
        return {};
    }
    const QJsonObject envelope = MirrorCrypto::sealArchive(
        archive, recipientPublicBundles, &cryptoError);
    if (envelope.isEmpty()) {
        setError(error, cryptoError);
        return {};
    }
    const qint64 now = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    if (!writeStored(path, makeStored(opaqueId, 1, now, now,
                                     ownerKeyId, envelope),
                     error))
        return {};
    if (error)
        error->clear();
    return opaqueId;
}

QByteArray PrivateMirrorStore::openReplica(
    const QString &storageRoot, const QString &opaqueId,
    const MirrorCrypto::Identity &identity, Metadata *metadata,
    QString *error)
{
    const QString path = replicaPath(storageRoot, opaqueId);
    if (path.isEmpty()) {
        setError(error, QStringLiteral("The opaque private-replica id is invalid."));
        return {};
    }
    QJsonObject stored;
    if (!readStored(path, opaqueId, &stored, metadata, error))
        return {};
    return MirrorCrypto::openArchive(
        stored.value(QStringLiteral("envelope")).toObject(), identity, error);
}

bool PrivateMirrorStore::replaceReplica(
    const QString &storageRoot, const QString &opaqueId,
    const QByteArray &archive, const MirrorCrypto::Identity &currentOwner,
    const QList<QJsonObject> &recipientPublicBundles, QString *error)
{
    if (archive.isEmpty()) {
        setError(error, QStringLiteral("A private mirror archive cannot be empty."));
        return false;
    }
    const QString path = replicaPath(storageRoot, opaqueId);
    if (path.isEmpty()) {
        setError(error, QStringLiteral("The opaque private-replica id is invalid."));
        return false;
    }
    QJsonObject stored;
    Metadata metadata;
    if (!readStored(path, opaqueId, &stored, &metadata, error))
        return false;
    if (!currentOwner.isValid() ||
        metadata.ownerKeyId != currentOwner.keyId() ||
        !recipientPublicBundles.contains(currentOwner.publicBundle())) {
        setError(error,
                 QStringLiteral("The current owner must authorize a replica replacement."));
        return false;
    }
    if (metadata.keyEpoch >= kMaximumSafeJsonInteger) {
        setError(error, QStringLiteral("The private-replica key epoch is exhausted."));
        return false;
    }
    QString cryptoError;
    const QJsonObject envelope = MirrorCrypto::sealArchive(
        archive, recipientPublicBundles, &cryptoError);
    if (envelope.isEmpty()) {
        setError(error, cryptoError);
        return false;
    }
    const qint64 now = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    if (!writeStored(
            path,
            makeStored(opaqueId, metadata.keyEpoch + 1,
                       metadata.createdAtMs, now, metadata.ownerKeyId,
                       envelope),
            error))
        return false;
    if (error)
        error->clear();
    return true;
}

bool PrivateMirrorStore::rotateRecipients(
    const QString &storageRoot, const QString &opaqueId,
    const MirrorCrypto::Identity &currentOwner,
    const QList<QJsonObject> &newRecipientPublicBundles, QString *error)
{
    const QString path = replicaPath(storageRoot, opaqueId);
    if (path.isEmpty()) {
        setError(error, QStringLiteral("The opaque private-replica id is invalid."));
        return false;
    }
    QJsonObject stored;
    Metadata metadata;
    if (!readStored(path, opaqueId, &stored, &metadata, error))
        return false;
    if (!currentOwner.isValid() ||
        metadata.ownerKeyId != currentOwner.keyId() ||
        !newRecipientPublicBundles.contains(currentOwner.publicBundle())) {
        setError(error,
                 QStringLiteral("The current owner must remain a rotation recipient."));
        return false;
    }
    QString cryptoError;
    QByteArray plaintext = MirrorCrypto::openArchive(
        stored.value(QStringLiteral("envelope")).toObject(),
        currentOwner, &cryptoError);
    if (plaintext.isEmpty()) {
        setError(error, cryptoError);
        return false;
    }
    const QJsonObject envelope = MirrorCrypto::sealArchive(
        plaintext, newRecipientPublicBundles, &cryptoError);
    clearBytes(&plaintext);
    if (envelope.isEmpty()) {
        setError(error, cryptoError);
        return false;
    }
    if (metadata.keyEpoch >= kMaximumSafeJsonInteger) {
        setError(error, QStringLiteral("The private-replica key epoch is exhausted."));
        return false;
    }
    const qint64 now = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    if (!writeStored(
            path,
            makeStored(opaqueId, metadata.keyEpoch + 1,
                       metadata.createdAtMs, now, metadata.ownerKeyId,
                       envelope),
            error))
        return false;
    if (error)
        error->clear();
    return true;
}

bool PrivateMirrorStore::inspectReplica(
    const QString &storageRoot, const QString &opaqueId,
    Metadata *metadata, QString *error)
{
    if (!metadata) {
        setError(error, QStringLiteral("A metadata destination is required."));
        return false;
    }
    const QString path = replicaPath(storageRoot, opaqueId);
    if (path.isEmpty()) {
        setError(error, QStringLiteral("The opaque private-replica id is invalid."));
        return false;
    }
    return readStored(path, opaqueId, nullptr, metadata, error);
}

QString PrivateMirrorStore::importReplica(
    const QString &storageRoot, const QByteArray &serializedReplica,
    const QString &expectedFileSha256, Metadata *metadata, QString *error)
{
    if (serializedReplica.isEmpty() ||
        serializedReplica.size() > std::numeric_limits<int>::max()) {
        setError(error, QStringLiteral("The encrypted private replica is invalid."));
        return {};
    }
    static const QRegularExpression digestPattern(
        QStringLiteral("^[0-9a-f]{64}$"));
    const QString actualFileSha256 = sha256Hex(serializedReplica);
    if (!digestPattern.match(expectedFileSha256).hasMatch() ||
        actualFileSha256 != expectedFileSha256) {
        setError(error,
                 QStringLiteral("The encrypted private replica file digest does not match."));
        return {};
    }
    const QJsonObject root =
        QJsonDocument::fromJson(serializedReplica).object();
    const QString opaqueId =
        root.value(QStringLiteral("opaqueId")).toString();
    Metadata incoming;
    if (!isOpaqueId(opaqueId) ||
        !parseStored(serializedReplica, opaqueId, nullptr, &incoming, error) ||
        incoming.replicaFileSha256 != expectedFileSha256) {
        return {};
    }
    if (!prepareRoot(storageRoot, error))
        return {};
    const QString path = replicaPath(storageRoot, opaqueId);
    if (QFileInfo::exists(path)) {
        Metadata current;
        if (!readStored(path, opaqueId, nullptr, &current, error))
            return {};
        if (incoming.ownerKeyId != current.ownerKeyId ||
            incoming.keyEpoch < current.keyEpoch ||
            (incoming.keyEpoch == current.keyEpoch &&
             incoming.replicaFileSha256 != current.replicaFileSha256)) {
            setError(error,
                     QStringLiteral("The encrypted private replica is stale or conflicts with this device."));
            return {};
        }
        if (incoming.replicaFileSha256 == current.replicaFileSha256) {
            if (metadata)
                *metadata = current;
            if (error)
                error->clear();
            return opaqueId;
        }
    }
    if (!writeSerialized(path, serializedReplica, error))
        return {};
    Metadata verified;
    if (!readStored(path, opaqueId, nullptr, &verified, error) ||
        verified.replicaFileSha256 != expectedFileSha256) {
        return {};
    }
    if (metadata)
        *metadata = verified;
    if (error)
        error->clear();
    return opaqueId;
}
