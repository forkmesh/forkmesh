#pragma once

#include "SecureDriveCrypto.h"

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

class QIODevice;

namespace forkmesh::securedrive {

struct EpochKey
{
    QString id;
    int number = 0;
    QByteArray key;
};

struct Head
{
    qint64 seq = -1;
    QString hash;
};

struct Membership
{
    QString slot;
    QString folder;
    QString label;
    QString driveId;
    QString rootDevice;
    QByteArray rootEd25519;
    QList<EpochKey> epochs;
    QString segment;
    QString segmentKey;
    qint64 lastSeq = -1;
    QString lastHash;
    qint64 lamport = 0;
    QMap<QString, Head> heads;
    QByteArray joinSecret;
    QString joinRequest;
    qint64 joinedAt = 0;

    bool isMember() const { return !driveId.isEmpty(); }
    bool isPending() const { return driveId.isEmpty() && !joinSecret.isEmpty(); }
    const EpochKey *currentEpoch() const;
    const EpochKey *epoch(const QString &id) const;
    QJsonObject toJson() const;
    static Membership fromJson(const QJsonObject &json);
};

class LocalState
{
public:
    explicit LocalState(QString directory);
    ~LocalState();
    LocalState(const LocalState &) = delete;
    LocalState &operator=(const LocalState &) = delete;

    QString directory() const { return m_directory; }
    QString filePath() const;
    bool exists() const;
    bool isUnlocked() const { return !m_key.isEmpty(); }
    bool create(const QString &passphrase, const QString &deviceName, Failure *failure);
    bool unlock(const QString &passphrase, Failure *failure);
    bool changePassphrase(const QString &current, const QString &next, Failure *failure);
    void lock();
    bool save(Failure *failure);

    Membership *membership(const QString &slot);
    const Membership *membership(const QString &slot) const;
    void forget(const QString &slot);

    QString deviceId;
    QString deviceName;
    DeviceKeys keys;
    PublicKeys publicKeys;
    QList<Membership> drives;

private:
    bool load(const QByteArray &key, const Kdf &kdf, const QJsonObject &document, Failure *failure);

    QString m_directory;
    QByteArray m_key;
    Kdf m_kdf;
};

struct DeviceInfo
{
    QString id;
    QString name;
    PublicKeys keys;
    QString addedBy;
    qint64 addedAt = 0;
    qint64 addedLamport = 0;
    bool removed = false;
    QString removedBy;
    qint64 removedAt = 0;
    qint64 removedSeq = -1;
    QString keyring;
    bool root = false;
};

struct FileVersion
{
    QString blob;
    QByteArray key;
    qint64 size = 0;
    QString sha256;
    qint64 time = 0;
    QString device;
};

struct FileEntry
{
    QString id;
    QString name;
    QString kind;
    QList<FileVersion> versions;
    qint64 created = 0;
    qint64 modified = 0;
    QString modifiedBy;
    QString createdBy;
    bool deleted = false;
    int conflicts = 0;
    int opens = 0;
    qint64 lastOpened = 0;
    QString lastOpenedBy;

    const FileVersion *current() const { return versions.isEmpty() ? nullptr : &versions.last(); }
};

struct Record
{
    QString device;
    qint64 seq = 0;
    qint64 lamport = 0;
    qint64 time = 0;
    QString action;
    QJsonObject data;
    QString prev;
    QString hash;
    QJsonObject witness;
    QJsonObject signedJson;
};

struct Issue
{
    QString code;
    QString device;
    QString detail;
};

struct Invite
{
    QString id;
    QByteArray requestKey;
    QByteArray approveKey;
    qint64 created = 0;
    qint64 expires = 0;
    QString createdBy;
    bool used = false;
    bool revoked = false;
    bool open(qint64 now) const { return !used && !revoked && expires > now; }
};

struct JoinRequest
{
    QString object;
    QString inviteId;
    QString device;
    QString name;
    PublicKeys keys;
    qint64 time = 0;
    QString safety;
};

QString fileKindForName(const QString &name);
QString describeRecord(const Record &record, const std::function<QString(const QString &)> &deviceName);
QString issueText(const Issue &issue, const std::function<QString(const QString &)> &deviceName);

class Drive
{
public:
    Drive(LocalState *state, QString slot);

    static bool create(LocalState *state, const QString &folder, const QString &label,
                       QString *slot, Failure *failure);
    static bool requestJoin(LocalState *state, const QString &folder, const QString &code,
                            QString *slot, QString *safety, Failure *failure);
    bool cancelJoin(Failure *failure);

    QString slot() const { return m_slot; }
    const Membership *membership() const;
    bool scan(Failure *failure);
    bool scanned() const { return m_scanned; }
    bool changedSinceScan() const { return folderFingerprint() != m_fingerprint; }
    QString joinSafetyNumber() const;
    bool joinRequestMissing() const { return m_joinRequestMissing; }
    bool removedFromDrive() const { return m_removed; }

    QList<FileEntry> files() const;
    const FileEntry *file(const QString &id) const;
    QList<Record> records() const { return m_records; }
    QList<DeviceInfo> devices() const;
    QList<Invite> openInvites() const;
    QList<JoinRequest> requests() const { return m_requests; }
    QList<Issue> issues() const { return m_issues; }
    QString deviceName(const QString &id) const;
    QString deviceSafetyNumber(const QString &id) const;
    int objectCount() const { return m_objectCount; }
    int foreignCount() const { return m_foreignCount; }

    bool createInvite(QString *code, QString *inviteId, Failure *failure);
    bool revokeInvite(const QString &inviteId, Failure *failure);
    bool approve(const QString &object, Failure *failure);
    bool deny(const QString &object, Failure *failure);

    bool addFile(const QString &name, QIODevice *source, QString *fileId, Failure *failure,
                 const QString &kind = QString());
    bool saveFile(const QString &fileId, QIODevice *source, Failure *failure);
    bool readFile(const QString &fileId, int version, QIODevice *out, const QString &purpose,
                  Failure *failure);
    bool renameFile(const QString &fileId, const QString &name, Failure *failure);
    bool deleteFile(const QString &fileId, Failure *failure);
    bool restoreVersion(const QString &fileId, int version, Failure *failure);
    bool removeDevice(const QString &deviceId, const std::function<void(int, int)> &progress,
                      Failure *failure);
    bool note(const QString &action, const QJsonObject &data, Failure *failure);
    QString activityCsv() const;

private:
    struct ParsedSegment
    {
        QString object;
        QString device;
        QString keyId;
        QJsonArray records;
    };

    Membership *mutableMembership() const;
    QString objectPath(const QString &object) const;
    bool writeObject(const QString &object, const QByteArray &bytes, Failure *failure) const;
    bool removeObject(const QString &object) const;
    bool append(const QList<QPair<QString, QJsonObject>> &entries, Failure *failure);
    bool writeSegment(Failure *failure);
    bool sealKeyring(const DeviceInfo &recipient, const QByteArray &approveKey,
                     const QString &inviteId, QString *object, Failure *failure);
    bool writeBlob(QIODevice *source, QString *object, QByteArray *key, qint64 *size,
                   QString *digest, Failure *failure);
    bool transcodeBlob(const FileVersion &version, QString *object, QByteArray *key,
                       Failure *failure);
    bool tryAdopt(const QHash<QString, QByteArray> &keyrings, Failure *failure);
    void fold(const QList<Record> &records);
    void checkWitnesses(const QMap<QString, QMap<qint64, Record>> &chains);
    void addIssue(const QString &code, const QString &device, const QString &detail);
    QString keyringLabel() const;
    QByteArray folderFingerprint() const;

    LocalState *m_state = nullptr;
    QString m_slot;
    bool m_scanned = false;
    QByteArray m_fingerprint;
    bool m_opened = false;
    bool m_joinRequestMissing = false;
    bool m_removed = false;
    int m_objectCount = 0;
    int m_foreignCount = 0;
    QJsonArray m_segmentRecords;
    QList<Record> m_records;
    QMap<QString, DeviceInfo> m_devices;
    QMap<QString, FileEntry> m_files;
    QMap<QString, Invite> m_invites;
    QList<JoinRequest> m_requests;
    QList<Issue> m_issues;
    QSet<QString> m_blobs;
    QStringList m_keyringObjects;
    QString m_driveLabel;
};

}  // namespace forkmesh::securedrive
