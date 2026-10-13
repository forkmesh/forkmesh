#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

// Automatic device-to-device encryption layered inside RoomCrypto.
//
// RoomCrypto's passphrase comes from the relay, so its operator can derive the
// room key. RoomSeal creates one persistent X25519 key on this device and
// exchanges only its public half. Each outgoing application frame gets a fresh
// AES-256-GCM content key; that key is wrapped for every known room device with
// an X25519/HKDF pairwise key. The relay can retain and forward the resulting
// envelope but cannot unwrap it.
//
//   RoomCrypto({type:"e2ee-key",v:2,kid,pub})
//   RoomCrypto({type:"sealed",v:2,kid,pub,nonce,tag,body,keys:[...]})
//
// No phrase, toggle, or controller-provided secret participates in this layer.
class RoomSeal
{
public:
    RoomSeal() = default;
    explicit RoomSeal(const QString &roomName);
    RoomSeal(const QString &roomName, const QString &storageScope);

    // Deterministic identity constructor for interoperability tests. The raw
    // 32-byte X25519 private key never appears in a production frame.
    RoomSeal(const QString &roomName, const QByteArray &privateKey);

    static bool isKeyFrame(const QJsonObject &frame);
    static bool isSealedFrame(const QJsonObject &frame);
    static QString frameKeyId(const QJsonObject &frame);

    bool isValid() const { return m_privateKey.size() == 32 &&
                                  m_publicKey.size() == 32 &&
                                  !m_roomName.isEmpty(); }
    QString errorString() const { return m_error; }
    QString keyId() const { return m_keyId; }
    int peerCount() const { return m_peers.size(); }
    QStringList peerKeyIds() const { return m_peerOrder; }

    QJsonObject keyFrame() const;
    // Returns true only for a newly learned, well-formed peer key.
    bool acceptKeyFrame(const QJsonObject &frame);
    // At most one retained public-key announcement per day and room. Live
    // announcements are still sent on every connection.
    bool shouldPersistAdvertisement() const;

    QJsonObject sealObject(const QJsonObject &plain) const;
    QJsonObject sealObjectFor(const QJsonObject &plain,
                              const QStringList &keyIds) const;
    QJsonObject unsealObject(const QJsonObject &frame);

private:
    bool initialize(const QByteArray &privateKey, bool loadStoredPeers);
    bool rememberPeer(const QString &keyId, const QByteArray &publicKey);
    void loadPeers();
    void persistPeers() const;

    QString m_roomName;
    QString m_storageScope;
    QByteArray m_privateKey;
    QByteArray m_publicKey;
    QString m_keyId;
    QString m_error;
    QHash<QString, QByteArray> m_peers;
    QStringList m_peerOrder;
};
