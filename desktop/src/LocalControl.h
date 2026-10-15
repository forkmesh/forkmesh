#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class QFile;
class QTcpSocket;
class QTimer;

// Local control: the phone on the same network drives this desktop — lists,
// starts, steers, stops and continues agents, and moves files both ways —
// with nothing sent through the relay.
//
// The channel rides the local mesh's TCP port (LocalMeshNode hands over any
// inbound link whose first frame is a control hello), so the one firewall rule
// that lets the #mesh room through covers it too. The mesh room key is shared
// by every ForkMesh install, so it proves nothing about who is dialling; this
// channel carries its own authentication and encryption:
//
//   phone   -> {t:"fm-control-hello",   key, eph, nonce, name, user, platform}
//   desktop -> {t:"fm-control-welcome", key, eph, name, user, sig}
//   phone   -> {t:"fm-control-auth",    sig}
//
// `key` is each device's Ed25519 identity (the mesh node id), `eph` a fresh
// X25519 key. Both sign the transcript T = hello and welcome keys, so neither
// side can be replayed or impersonated, and every later frame is AES-256-GCM
// under per-direction keys HKDF'd from the ephemeral secret with an implicit
// frame counter as the nonce. A device the desktop has never approved gets a
// six-digit code both screens show; only the person at the desktop can
// approve it, after which its key is remembered and it reconnects silently.
//
// Once the channel is open, both sides exchange JSON:
//   request  {id, method, params}      phone -> desktop
//   response {id, ok, result | error}  desktop -> phone
//   event    {event, data}             desktop -> phone
//
// Away from home the very same channel rides the account's relay socket
// instead of TCP (app/src/node_control.py): each protocol frame, the plain
// hello and welcome included, is base64'd into {"type":"control"} frames that
// the per-owner relay object hands between the phone's browser-role socket and
// this desktop's node-role socket. The relay sees only the envelope; the
// handshake, the signatures and the AES-GCM keys are exactly the LAN ones. A
// relayed channel is accepted only from a phone already approved at this
// desktop, never paired through the relay, and it carries no file transfer.
namespace forkmesh::localcontrol {

inline constexpr int kProtocolVersion = 1;

QString helloTag();
QString welcomeTag();
QString authTag();

// True for a mesh link's first frame that opens a control channel instead.
bool isControlHello(const QByteArray &payload);

// The bytes both sides sign and derive from, in a fixed order.
QByteArray transcript(const QString &clientKey, const QString &clientEph,
                      const QString &clientNonce, const QString &serverKey,
                      const QString &serverEph);
QByteArray serverSignedBytes(const QByteArray &transcript);
QByteArray clientSignedBytes(const QByteArray &transcript);

struct SessionKeys {
    QByteArray clientToServer;
    QByteArray serverToClient;
    bool isValid() const
    {
        return clientToServer.size() == 32 && serverToClient.size() == 32;
    }
};
SessionKeys deriveKeys(const QByteArray &sharedSecret,
                       const QByteArray &transcript);

// Six digits both screens show while a device waits for approval. Any
// substituted key on the way changes the transcript, and with it the code.
QString pairingCode(const QByteArray &transcript);

// One encrypted frame: ciphertext || 16-byte tag, nonce = 4 zero bytes then
// the direction's frame counter, big endian.
QByteArray sealFrame(const QByteArray &key, quint64 counter,
                     const QByteArray &plain);
// Empty when the frame does not authenticate under this key and counter.
QByteArray openFrame(const QByteArray &key, quint64 counter,
                     const QByteArray &frame);

struct Ephemeral {
    QByteArray privateKey;
    QByteArray publicKey;
};
Ephemeral newEphemeral();
QByteArray x25519(const QByteArray &privateKey, const QByteArray &peerPublic);

bool verifyEd25519(const QString &publicKeyB64url,
                   const QString &signatureB64url, const QByteArray &data);

QString toBase64Url(const QByteArray &bytes);
// Empty unless `text` is base64url of exactly `bytes` bytes.
QByteArray fromBase64Url(const QString &text, int bytes);

// A file name from another device reduced to one safe path component.
QString safeFileName(const QString &name);

// Largest frame either side accepts, and the file chunk that fits in one.
qsizetype maxFrameBytes();
int fileChunkBytes();
// Largest protocol frame a relayed channel reassembles, and the base64
// characters of one relayed chunk (inside the relay's own envelope limit).
qsizetype maxRelayFrameBytes();
int relayChunkChars();

} // namespace forkmesh::localcontrol

class LocalControlServer : public QObject
{
public:
    struct Device {
        QString key; // Ed25519 identity, base64url
        QString name;
        QString user;
        QString platform;
        qint64 pairedAtMs = 0;
        qint64 lastSeenMs = 0;
        QString approval;
        qint64 accountCheckedMs = 0;
    };

    enum class AccountCheck { Owned, NotOwned, Unreachable };

    // `signer` returns the base64url Ed25519 signature of this desktop's
    // identity key (publicKey) over the given bytes.
    LocalControlServer(const QString &publicKey,
                       std::function<QString(const QByteArray &)> signer,
                       QObject *parent = nullptr);
    ~LocalControlServer() override;

    void setDesktopInfo(const QString &name, const QString &user);
    // Where approved devices are kept. Loaded now; empty keeps them in memory.
    void setDeviceStorePath(const QString &path);
    // Where files a phone sends are written.
    void setReceiveDirectory(const QString &path);
    QString receiveDirectory() const { return m_receiveDir; }

    // Takes over a socket whose first frame was `hello`; `rest` is whatever
    // followed it in the buffer.
    void adopt(QTcpSocket *socket, const QByteArray &hello,
               const QByteArray &rest);

    // The relayed transport. `sink` sends one {"type":"control"} frame up
    // the account's relay socket and returns false when it cannot; `node` is
    // the machine name frames are addressed to (lowercase).
    void setRelaySink(std::function<bool(const QJsonObject &frame)> sink,
                      const QString &node);
    // One {"type":"control"} frame the relay labelled as the phone's.
    void relayFrame(const QJsonObject &frame);
    void relayRestored();
    void maintain(qint64 nowMs);
    bool isRelayed(int connectionId) const;
    // The device behind an open connection; empty key when there is none.
    Device deviceFor(int connectionId) const;

    QList<Device> pairedDevices() const;
    bool isPaired(const QString &key) const;
    // Devices with an authenticated connection open now, paired or waiting.
    QList<Device> connectedDevices() const;
    bool isConnected(const QString &key) const;
    void forget(const QString &key);

    void approvePairing(int connectionId);
    void denyPairing(int connectionId);

    void respond(int connectionId, int requestId, const QJsonObject &result);
    void respondError(int connectionId, int requestId, const QString &error);
    void respondError(int connectionId, int requestId, const QString &error,
                      const QString &message);
    // To every paired device connected now.
    void broadcast(const QString &event, const QJsonObject &data = {});
    // To one connection, when it is paired; false when it is gone.
    bool sendEventTo(int connectionId, const QString &event,
                     const QJsonObject &data = {});
    // Streams a file to a connected paired device; false with *error if not.
    bool sendFile(const QString &deviceKey, const QString &path,
                  QString *error = nullptr);

    std::function<void(int connectionId, const Device &device,
                       const QString &code)>
        onPairingRequested;
    // A pairing prompt no longer applies: approved, denied, or the phone left.
    std::function<void(int connectionId)> onPairingClosed;
    std::function<void(const Device &device,
                       std::function<void(AccountCheck verdict)> done)>
        accountDeviceCheck;
    std::function<void(int connectionId, const Device &device, int requestId,
                       const QString &method, const QJsonObject &params)>
        onRequest;

    // Gate 2 (docs/native/DESKTOP_PARITY.md section 19.3): a method family,
    // routed by prefix ("teammates.") to its own handler before onRequest and
    // before the built-in "files." methods. The longest registered prefix
    // wins, so "files.browse." is not taken by the file transfer. Only paired
    // connections reach a family; ownership checks are the handler's.
    using FamilyHandler =
        std::function<void(int connectionId, const Device &device, int requestId,
                           const QString &method, const QJsonObject &params)>;
    // A prefix must end with '.'; registering it again replaces the handler.
    bool registerFamily(const QString &prefix, FamilyHandler handler);
    bool hasFamily(const QString &prefix) const { return m_families.contains(prefix); }
    // The registered prefix a method belongs to, or empty.
    QString familyFor(const QString &method) const;
    std::function<void()> onDevicesChanged;
    std::function<void(const Device &device, const QString &path)>
        onFileReceived;
    // An authenticated connection went away (host log streams end with it).
    std::function<void(int connectionId)> onConnectionClosed;

    struct CoreBridge {
        std::function<bool(const QString &method, const QJsonObject &params)> notify;
        std::function<void(const QString &method, const QJsonObject &params,
                           std::function<void(bool ok, const QJsonObject &result,
                                              const QString &error)> done)>
            request;
    };
    static constexpr int kCoreConnectionBase = 1 << 24;
    void attachCore(CoreBridge bridge);
    void detachCore();
    bool coreAttached() const { return m_coreAttached; }
    static bool isCoreConnection(int connectionId)
    {
        return connectionId >= kCoreConnectionBase;
    }
    void coreOpened(const QJsonObject &params);
    void coreClosed(const QJsonObject &params);
    void coreRequest(const QJsonObject &params);
    void corePairing(const QJsonObject &params);
    void corePairingClosed(const QJsonObject &params);
    void coreDevices(const QJsonObject &params);
    void coreFileReceived(const QJsonObject &params);
    void coreAccountCheck(const QJsonObject &params,
                          std::function<void(const QString &verdict)> done);
    bool coreRelaySend(const QJsonObject &frame);
    static Device deviceFromJson(const QJsonObject &object);
    static QJsonObject deviceToJson(const Device &device);

private:
    QHash<QString, FamilyHandler> m_families;
    enum class State { AwaitAuth, Checking, Pending, Paired };
    struct Incoming {
        QFile *file = nullptr;
        QString finalPath;
        QString name;
        qint64 size = 0;
        qint64 received = 0;
    };
    struct Outgoing {
        QFile *file = nullptr;
        QString transferId;
        qint64 size = 0;
    };
    struct Connection {
        int id = 0;
        QTcpSocket *socket = nullptr;
        QByteArray buffer;
        State state = State::AwaitAuth;
        Device device;
        QByteArray transcript;
        QByteArray sendKey;
        QByteArray receiveKey;
        quint64 sendCounter = 0;
        quint64 receiveCounter = 0;
        QString code;
        QTimer *handshakeTimer = nullptr;
        QHash<QString, Incoming> incoming;
        QList<Outgoing> outgoing;
        // Relayed channels only: the phone's channel id, the next chunk
        // sequence each way, and a protocol frame still being reassembled.
        QString relaySession;
        quint32 relaySendSeq = 0;
        quint32 relayReceiveSeq = 0;
        QByteArray relayPending;
        qint64 lastHeardMs = 0;
        qint64 accountCheckedMs = 0;
        bool accountCheckInFlight = false;
        bool viaCore = false;
        bool coreRelayed = false;
        bool relayed() const { return viaCore ? coreRelayed : !relaySession.isEmpty(); }
    };
    Connection *coreConnection(const QJsonObject &params, bool create);

    Connection *connection(int id);
    const Connection *connection(int id) const;
    Connection *connectionForSocket(QTcpSocket *socket);
    Connection *connectionForRelay(const QString &session);
    bool linkOpen(const Connection &conn) const;
    // Writes one protocol frame (plain handshake bytes or a sealed frame).
    bool writeFrame(Connection &conn, const QByteArray &payload);
    bool sendRelay(const QJsonObject &frame);
    void startHandshakeTimer(Connection &conn);
    bool acceptHello(Connection &conn, const QByteArray &hello);
    void onReadyRead(QTcpSocket *socket);
    void handleFrame(Connection &conn, const QByteArray &payload);
    void handleAuth(Connection &conn, const QByteArray &payload);
    void requestPairing(Connection &conn);
    void pairDevice(Connection &conn, const QString &approval);
    void admitStored(Connection &conn);
    bool needsAccountCheck(const Connection &conn) const;
    void settleAccountCheck(Connection &conn, AccountCheck verdict);
    void refuseRelayed(Connection &conn, const QString &reason);
    void recheckAccount(int connectionId);
    bool evictIdleRelayed(qint64 nowMs);
    void tellRelayGone(const QString &session, const QString &reason);
    void handleMessage(Connection &conn, const QJsonObject &message);
    void handleFileRequest(Connection &conn, int requestId,
                           const QString &method, const QJsonObject &params);
    bool writeJson(Connection &conn, const QJsonObject &object);
    void sendEvent(Connection &conn, const QString &event,
                   const QJsonObject &data = {});
    void pumpOutgoing(Connection &conn);
    void closeConnection(int id, const QString &reason);
    void rememberDevice(const Device &device);
    void loadDevices();
    void saveDevices() const;
    QJsonObject sessionInfo(const Connection &conn) const;

    QString m_publicKey;
    std::function<QString(const QByteArray &)> m_signer;
    QString m_name;
    QString m_user;
    QString m_storePath;
    QString m_receiveDir;
    QHash<QString, Device> m_devices; // approved, by key
    QList<Connection *> m_connections;
    int m_nextId = 1;
    std::function<bool(const QJsonObject &frame)> m_relaySink;
    QString m_relayNode;
    // A relayed hello still arriving in chunks, by channel id.
    QHash<QString, QByteArray> m_relayOpening;
    QHash<QString, quint32> m_relayOpeningSeq;
    QHash<QString, qint64> m_relayOpeningSince;
    QStringList m_relayUndelivered;
    QStringList m_relayToldGone;
    QTimer *m_maintenanceTimer = nullptr;
    CoreBridge m_core;
    bool m_coreAttached = false;
    QHash<QString, Device> m_coreDevices;
    QList<Device> m_coreConnected;
};
