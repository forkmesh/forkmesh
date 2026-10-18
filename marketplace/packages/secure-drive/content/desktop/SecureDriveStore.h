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

inline constexpr qint64 kPageBytes = 16384;
inline constexpr qint64 kUnitOverhead = 28;
inline constexpr qint64 kChunkBytes = kPageBytes - kUnitOverhead;
inline constexpr qint64 kLeasePage = 0;
inline constexpr qint64 kRootPages = 4;
inline constexpr qint64 kRootAPage = 1;
inline constexpr qint64 kRootBPage = 5;
inline constexpr qint64 kMailboxPage = 9;
inline constexpr int kMailboxSlots = 64;
inline constexpr qint64 kSlotBytes = 8192;
inline constexpr qint64 kDataPage = 41;
inline constexpr qint64 kInitialDataPages = 32;

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

struct Run
{
    qint64 start = 0;
    qint64 count = 0;
};

struct Membership
{
    QString id;
    QString container;
    QString label;
    QString driveId;
    QString rootDevice;
    QByteArray rootEd25519;
    QList<EpochKey> epochs;
    int mailbox = -1;
    qint64 lastSeq = -1;
    QString lastHash;
    qint64 lamport = 0;
    QMap<QString, Head> heads;
    qint64 rootGeneration = -1;
    QString rootHash;
    QStringList removedDevices;
    QString cachedRoot;
    bool keyringSeen = false;
    QByteArray joinSecret;
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
    QJsonObject exportDevice() const;
    bool importDevice(const QJsonObject &device, const QString &passphrase, Failure *failure);

    Membership *membership(const QString &id);
    const Membership *membership(const QString &id) const;
    void forget(const QString &id);

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
    int mailbox = -1;
    bool removed = false;
    QString removedBy;
    qint64 removedAt = 0;
    qint64 removedSeq = -1;
    bool root = false;
};

struct FileVersion
{
    QString blob;
    QByteArray key;
    QList<Run> pages;
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
    QJsonObject stored;
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
    int mailbox = -1;
    qint64 created = 0;
    qint64 expires = 0;
    QString createdBy;
    bool used = false;
    bool revoked = false;
    bool open(qint64 now) const { return !used && !revoked && expires > now; }
};

struct JoinRequest
{
    int mailbox = -1;
    QString inviteId;
    QString device;
    QString name;
    PublicKeys keys;
    qint64 time = 0;
    QString safety;
};

struct LeaseTiming
{
    int settleMs = 600;
    int staleMs = 120000;
    int pollMs = 250;
};

QString fileKindForName(const QString &name);
QString describeRecord(const Record &record, const std::function<QString(const QString &)> &deviceName);
QString issueText(const Issue &issue, const std::function<QString(const QString &)> &deviceName);
int mailboxForInvite(const QByteArray &secret);
QList<Run> runsFromJson(const QJsonArray &array);
QJsonArray runsToJson(const QList<Run> &runs);

class ContainerFile;

class Drive
{
public:
    Drive(LocalState *state, QString id);
    ~Drive();

    static void setLeaseTiming(const LeaseTiming &timing);
    static void setInviteLifetime(qint64 milliseconds);
    static bool create(LocalState *state, const QString &path, const QString &label, QString *id,
                       Failure *failure);
    static bool requestJoin(LocalState *state, const QString &path, const QString &code, QString *id,
                            QString *safety, Failure *failure);
    bool cancelJoin(Failure *failure);

    QString id() const { return m_id; }
    const Membership *membership() const;
    bool scan(Failure *failure);
    bool scanned() const { return m_scanned; }
    bool changedSinceScan() const { return fingerprint() != m_fingerprint; }
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
    qint64 containerBytes() const { return m_totalPages * kPageBytes; }
    qint64 usedBytes() const;
    QList<Run> dataRunsInUse() const;
    QList<int> mailboxesInUse() const;

    bool createInvite(QString *code, QString *inviteId, Failure *failure);
    bool revokeInvite(const QString &inviteId, Failure *failure);
    bool approve(int mailbox, Failure *failure);
    bool deny(int mailbox, Failure *failure);

    bool addFile(const QString &name, QIODevice *source, QString *fileId, Failure *failure,
                 const QString &kind = QString());
    bool saveFile(const QString &fileId, QIODevice *source, Failure *failure, const QString &base = QString());
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
    struct Txn;
    struct RootCandidate;

    Membership *mutableMembership() const;
    QByteArray fingerprint() const;
    bool load(ContainerFile &file, Failure *failure);
    bool loadPending(ContainerFile &file, Failure *failure);
    bool transact(const std::function<bool(Txn &, Failure *)> &body, Failure *failure,
                  const std::function<void()> &renew = {});
    bool acquireLease(ContainerFile &file, Failure *failure);
    bool renewLease(ContainerFile &file);
    void releaseLease(ContainerFile &file);
    bool commit(Txn &txn, Failure *failure);
    QList<qint64> freePages(const Txn &txn) const;
    bool allocate(Txn &txn, qint64 count, bool contiguous, QList<Run> *runs, Failure *failure);
    bool writeBlob(Txn &txn, QIODevice *source, FileVersion *version, Failure *failure);
    bool readBlob(ContainerFile &file, const FileVersion &version, const std::function<bool(const QByteArray &)> &sink,
                  Failure *failure) const;
    QByteArray keyringUnit(const DeviceInfo &recipient, int mailbox, const QByteArray &approveKey,
                           const QString &inviteId, Failure *failure) const;
    void fold(const QList<Record> &records);
    void checkWitnesses(const QMap<QString, QMap<qint64, Record>> &chains);
    void addIssue(const QString &code, const QString &device, const QString &detail);
    QSet<int> usedMailboxes() const;

    LocalState *m_state = nullptr;
    QString m_id;
    bool m_scanned = false;
    bool m_opened = false;
    bool m_joinRequestMissing = false;
    bool m_removed = false;
    bool m_repairKeyring = false;
    QByteArray m_fingerprint;
    QByteArray m_leaseBytes;
    QByteArray m_rootABytes;
    QByteArray m_rootBBytes;
    qint64 m_rootGeneration = -1;
    QString m_rootHash;
    qint64 m_totalPages = 0;
    QMap<QString, QList<Run>> m_logs;
    QJsonArray m_myLastRecords;
    QList<Record> m_records;
    QMap<QString, DeviceInfo> m_devices;
    QMap<QString, FileEntry> m_files;
    QMap<QString, Invite> m_invites;
    QList<JoinRequest> m_requests;
    QList<Issue> m_issues;
    QString m_driveLabel;
};

}  // namespace forkmesh::securedrive
