#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

namespace forkmesh::chatv3 {

constexpr qint64 kDayMs = 24ll * 60 * 60 * 1000;
constexpr qint64 kPrekeyLifeMs = 8 * kDayMs;
constexpr qint64 kPrekeyRotateMs = kDayMs;
constexpr int kMaxPrekeys = 10;
constexpr int kMaxRecipients = 64;
constexpr qint64 kReplayWindow = 1024;
constexpr int kMaxHeldFrames = 64;
constexpr int kMlkemPublicBytes = 1184;
constexpr int kMlkemCiphertextBytes = 1088;
constexpr int kMlkemSeedBytes = 64;

QString b64u(const QByteArray &data);
QByteArray fromB64u(const QString &text, bool *ok = nullptr);
QString b64(const QByteArray &data);
QByteArray fromB64(const QString &text, bool *ok = nullptr);
QByteArray sha256(const QByteArray &data);
QString id16(const QByteArray &data);
QByteArray randomBytes(int count);
void wipe(QByteArray &bytes);
QByteArray hkdfSha256(const QByteArray &ikm, const QByteArray &salt,
                      const QByteArray &info, int length = 32);
bool aesGcmSeal(const QByteArray &key, const QByteArray &nonce,
                const QByteArray &plain, const QByteArray &aad,
                QByteArray *cipher, QByteArray *tag);
bool aesGcmOpen(const QByteArray &key, const QByteArray &nonce,
                const QByteArray &cipher, const QByteArray &tag,
                const QByteArray &aad, QByteArray *plain);
QByteArray x25519Public(const QByteArray &privateKey);
QByteArray x25519Shared(const QByteArray &privateKey, const QByteArray &publicKey);
QByteArray ed25519Public(const QByteArray &seed);
QByteArray ed25519Sign(const QByteArray &seed, const QByteArray &message);
bool ed25519Verify(const QByteArray &publicKey, const QByteArray &signature,
                   const QByteArray &message);
QByteArray mlkemPublic(const QByteArray &seed);
bool mlkemEncapsulate(const QByteArray &publicKey, QByteArray *ciphertext,
                      QByteArray *shared);
QByteArray mlkemDecapsulate(const QByteArray &seed, const QByteArray &ciphertext);

struct Prekey {
    QString id;
    QByteArray x;
    QByteArray pq;
    qint64 ts = 0;
    qint64 exp = 0;
    QByteArray xPriv;
    QByteArray pqSeed;
};

Prekey makePrekey(const QByteArray &xPriv, const QByteArray &pqSeed, qint64 ts);
QJsonObject publicPrekey(const Prekey &prekey);
bool validAcct(const QString &acct);
QByteArray bundleMessage(const QString &room, const QString &kid,
                         const QString &idk, const QString &acct,
                         const QString &node, const QJsonObject &spk);
bool verifyBundle(const QJsonObject &bundle, const QString &room, qint64 now);
QByteArray kek(const QString &room, const QString &sender,
               const QString &recipient, const QString &spkId,
               const QByteArray &ephPub, const QByteArray &ciphertext,
               const QByteArray &ss1, const QByteArray &ss2);
QByteArray contentAad(const QString &room, const QString &sender, qint64 ctr,
                      qint64 ts);
QByteArray wrapAad(const QString &room, const QString &sender,
                   const QString &recipient, const QString &nonce,
                   const QString &tag);
QByteArray sealedMessage(const QString &room, const QJsonObject &frame);

struct SafetyNumber {
    QString digits;
    QString qr;
};
SafetyNumber safetyNumber(const QString &scope, QList<QByteArray> idks);
QStringList safetyGroups(const QString &digits);

QJsonObject linkHello(const QByteArray &idk, const QByteArray &ephPub,
                      const QByteArray &nonce);
bool isLinkHello(const QJsonObject &frame);
QByteArray linkTranscript(const QJsonObject &initiatorHello,
                          const QJsonObject &responderHello);
QByteArray linkAuthMessage(const QString &role, const QByteArray &transcript);
bool linkKeys(const QByteArray &ownEphPriv, const QByteArray &peerEphPub,
              const QByteArray &transcript, QByteArray *i2r, QByteArray *r2i);
QByteArray linkNonce(quint64 counter);
QByteArray linkSeal(const QByteArray &key, quint64 counter,
                    const QByteArray &plain);
bool linkOpen(const QByteArray &key, quint64 counter, const QByteArray &data,
              QByteArray *plain);

enum class TrustState { Approved, Pending, Blocked };
enum class Policy { Open, Strict };

QString trustStateName(TrustState state);

struct TrustRecord {
    QString kid;
    QString idk;
    QString acct;
    QString node;
    QJsonObject spk;
    TrustState state = TrustState::Pending;
    qint64 firstSeen = 0;
    qint64 lastSeen = 0;
    bool verified = false;
};

struct MeshLinkedDevice {
    QString kid;
    bool bluetooth = false;
    QSet<QString> focus;
};

class ChatDevice
{
public:
    enum class Event { PrekeyRotated, TrustChanged, DevicePending, SafetyNumberChanged };
    using Listener = std::function<void(Event event, const QString &scope,
                                        const QString &kid)>;

    static std::shared_ptr<ChatDevice> local();
    static std::shared_ptr<ChatDevice> inMemory(const QByteArray &identitySeed);
    static std::shared_ptr<ChatDevice> sharedForTests(const QByteArray &identitySeed);

    ~ChatDevice();

    bool isValid() const { return m_idk.size() == 32; }
    QString kid() const { return m_kid; }
    QByteArray idk() const { return m_idk; }
    QString idkText() const { return b64u(m_idk); }
    QByteArray sign(const QByteArray &message) const;

    qint64 now() const;
    void setClock(std::function<qint64()> clock);

    QList<Prekey> prekeys() const { return m_prekeys; }
    void addPrekey(const Prekey &prekey);
    const Prekey *newestPrekey() const;
    const Prekey *findPrekey(const QString &id) const;
    bool maintainPrekeys();
    void wipeExpired(qint64 now);

    Policy policy(const QString &scope);
    void setPolicy(const QString &scope, Policy policy);
    QList<TrustRecord> records(const QString &scope);
    const TrustRecord *record(const QString &scope, const QString &kid);
    void ensureOwnRecord(const QString &scope, const QString &acct,
                         const QString &node);
    enum class BundleResult { Rejected, Known, Approved, Pending, Blocked };
    BundleResult acceptBundle(const QString &scope, Policy policy,
                              const QString &room, const QJsonObject &bundle,
                              bool *changed = nullptr,
                              const QStringList &participants = {});
    void touch(const QString &scope, const QString &kid);
    void setState(const QString &scope, const QString &kid, TrustState state);
    void markVerified(const QString &scope);
    bool isVerified(const QString &scope);
    bool isBlockedAnywhere(const QString &kid);
    SafetyNumber safetyNumberFor(const QString &scope);

    qint64 nextCounter(const QString &room);
    bool isReplay(const QString &room, const QString &kid, qint64 ctr);
    void markSeen(const QString &room, const QString &kid, qint64 ctr);

    void hold(const QString &scope, const QString &room, const QJsonObject &frame);
    QList<QJsonObject> held(const QString &scope, const QString &room);
    void setHeld(const QString &scope, const QString &room,
                 const QList<QJsonObject> &frames);

    int subscribe(Listener listener);
    void unsubscribe(int token);

    void flush();

private:
    struct Scope {
        bool loaded = false;
        Policy policy = Policy::Open;
        bool policyKnown = false;
        QHash<QString, TrustRecord> records;
        QHash<QString, QList<QJsonObject>> held;
        QString safety;
    };
    struct Seen {
        qint64 max = 0;
        QSet<qint64> set;
        qint64 at = 0;
    };
    struct Room {
        bool loaded = false;
        qint64 counter = 0;
        qint64 reserved = 0;
        bool seenLoaded = false;
        QHash<QString, Seen> seen;
    };

    explicit ChatDevice(bool persistent);
    bool loadIdentity();
    void setIdentity(const QByteArray &seed);
    void loadPrekeys();
    void savePrekeys();
    Scope &scopeState(const QString &scope);
    Room &roomState(const QString &room);
    void loadSeen(const QString &room, Room &state);
    void saveScope(const QString &scope, bool now);
    void saveSeen(const QString &room);
    void refreshSafety(const QString &scope);
    void notify(Event event, const QString &scope, const QString &kid);

    bool m_persistent = false;
    QByteArray m_seed;
    QByteArray m_idk;
    QString m_kid;
    QList<Prekey> m_prekeys;
    std::function<qint64()> m_clock;
    QHash<QString, Scope> m_scopes;
    QHash<QString, Room> m_rooms;
    QHash<int, Listener> m_listeners;
    int m_nextToken = 1;
};

} // namespace forkmesh::chatv3

class RoomSeal
{
public:
    using Policy = forkmesh::chatv3::Policy;
    enum class Status { Opened, Held, NotForMe, Replay, Invalid };
    struct Opened {
        Status status = Status::Invalid;
        QJsonObject plain;
        QString senderKid;
        QString senderAcct;
        QString senderNode;
        qint64 ts = 0;
        bool self = false;
    };
    static void useSignedTime(QJsonObject &plain, const Opened &opened);
    using BundleResult = forkmesh::chatv3::ChatDevice::BundleResult;

    RoomSeal() = default;
    RoomSeal(const QString &roomName, const QString &trustScope, Policy policy,
             const QString &acct, const QString &node,
             std::shared_ptr<forkmesh::chatv3::ChatDevice> device =
                 forkmesh::chatv3::ChatDevice::local());

    static bool isKeyFrame(const QJsonObject &frame);
    static bool isSealedFrame(const QJsonObject &frame);
    static bool isLegacyFrame(const QJsonObject &frame);
    static QString frameKeyId(const QJsonObject &frame);

    bool isValid() const { return m_device && m_device->isValid() && !m_room.isEmpty(); }
    QString errorString() const { return m_error; }
    QString keyId() const { return m_device ? m_device->kid() : QString(); }
    QString roomName() const { return m_room; }
    QString trustScope() const { return m_scope; }
    QString acct() const { return m_acct; }
    QString node() const { return m_node; }
    QStringList participants() const { return m_participants; }
    void setParticipants(const QStringList &participants);
    Policy policy() const { return m_policy; }
    std::shared_ptr<forkmesh::chatv3::ChatDevice> device() const { return m_device; }

    QJsonObject keyFrame() const;
    BundleResult acceptKeyFrame(const QJsonObject &frame, bool *changed = nullptr) const;

    QJsonObject sealObject(const QJsonObject &plain) const;
    QJsonObject sealObjectFor(const QJsonObject &plain, const QStringList &keyIds) const;
    Opened open(const QJsonObject &frame, bool historyReplay = false) const;
    QJsonObject unsealObject(const QJsonObject &frame) const;
    QList<QJsonObject> takeReleasedHeld() const;

    void approve(const QString &kid) const;
    void block(const QString &kid) const;
    QList<forkmesh::chatv3::TrustRecord> records() const;
    QStringList approvedKeyIds() const;
    QStringList pendingKeyIds() const;
    QStringList keyIdsForAcct(const QString &acct) const;
    QStringList keyIdsForNode(const QString &node) const;
    QString acctForKeyId(const QString &kid) const;
    QString nodeForKeyId(const QString &kid) const;
    bool isApproved(const QString &kid) const;
    int peerCount() const { return int(peerKeyIds().size()); }
    QStringList peerKeyIds() const;
    forkmesh::chatv3::SafetyNumber safetyNumber() const;
    bool isVerified() const;
    void markVerified() const;
    bool shouldPersistAdvertisement() const;

private:
    QJsonObject seal(const QJsonObject &plain, const QStringList *only) const;

    QString m_room;
    QString m_scope;
    QString m_acct;
    QString m_node;
    QStringList m_participants;
    Policy m_policy = Policy::Open;
    QString m_error;
    std::shared_ptr<forkmesh::chatv3::ChatDevice> m_device;
};
