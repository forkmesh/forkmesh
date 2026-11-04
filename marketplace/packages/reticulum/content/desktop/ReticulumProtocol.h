#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QVariant>

#include <optional>

namespace forkmesh::reticulum {

constexpr int kHashLength = 16;
constexpr int kFullHashLength = 32;
constexpr int kNameHashLength = 10;
constexpr int kRandomHashLength = 10;
constexpr int kPublicKeyLength = 64;
constexpr int kPrivateKeyLength = 64;
constexpr int kSignatureLength = 64;
constexpr int kRatchetLength = 32;
constexpr int kMtu = 500;
constexpr int kHeaderMinSize = 2 + 1 + kHashLength;
constexpr int kHeaderMaxSize = 2 + 1 + kHashLength * 2;
constexpr int kStreamHardwareMtu = 262144;
constexpr int kMaxHops = 128;
constexpr int kStampSize = 32;
constexpr int kDiscoveryExpandRounds = 20;
constexpr int kDiscoveryStampValue = 16;

enum PacketType { PacketData = 0, PacketAnnounce = 1, PacketLinkRequest = 2, PacketProof = 3 };
enum DestinationType { DestinationSingle = 0, DestinationGroup = 1, DestinationPlain = 2, DestinationLink = 3 };
enum HeaderType { Header1 = 0, Header2 = 1 };
enum TransportType { TransportBroadcast = 0, TransportForward = 1 };

enum PacketContext : quint8 {
    ContextNone = 0x00,
    ContextResource = 0x01,
    ContextResourceAdvertisement = 0x02,
    ContextResourceRequest = 0x03,
    ContextResourceHashmapUpdate = 0x04,
    ContextResourceProof = 0x05,
    ContextResourceInitiatorCancel = 0x06,
    ContextResourceReceiverCancel = 0x07,
    ContextCacheRequest = 0x08,
    ContextRequest = 0x09,
    ContextResponse = 0x0A,
    ContextPathResponse = 0x0B,
    ContextCommand = 0x0C,
    ContextCommandStatus = 0x0D,
    ContextChannel = 0x0E,
    ContextKeepalive = 0xFA,
    ContextLinkIdentify = 0xFB,
    ContextLinkClose = 0xFC,
    ContextLinkProof = 0xFD,
    ContextLinkRtt = 0xFE,
    ContextLinkRequestProof = 0xFF,
};

struct Packet {
    bool ifac = false;
    int headerType = Header1;
    bool contextFlag = false;
    int transportType = TransportBroadcast;
    int destinationType = DestinationSingle;
    int packetType = PacketData;
    int hops = 0;
    QByteArray transportId;
    QByteArray destinationHash;
    quint8 context = ContextNone;
    QByteArray data;
};

struct Announce {
    QByteArray destinationHash;
    QByteArray publicKey;
    QByteArray nameHash;
    QByteArray randomHash;
    QByteArray ratchet;
    QByteArray signature;
    QByteArray appData;
    bool hasAppData = false;
    QByteArray identityHash;
    qint64 emittedAt = 0;
};

struct KnownAspect {
    QString name;
    QString label;
    QString kind;
    QByteArray nameHash;
};

struct DiscoveredInterface {
    QString name;
    QString type;
    QString reachableOn;
    int port = 0;
    bool transport = false;
    bool hasLocation = false;
    double latitude = 0;
    double longitude = 0;
    double height = 0;
    QString implementation;
    QString version;
    QByteArray transportId;
    QString networkName;
    bool publishesPassphrase = false;
    double frequency = 0;
    double bandwidth = 0;
    QString modulation;
    bool encrypted = false;
    int stampValue = 0;
    bool stampValid = false;
};

QByteArray fullHash(const QByteArray &data);
QByteArray truncatedHash(const QByteArray &data);
QByteArray hmacSha256(const QByteArray &key, const QByteArray &data);
QByteArray hkdf(int length, const QByteArray &material, const QByteArray &salt,
                const QByteArray &context = {});
QByteArray randomBytes(int length);

QByteArray nameHash(const QString &fullName);
QByteArray destinationHash(const QByteArray &nameHash, const QByteArray &identityHash = {});
QByteArray destinationHashForName(const QString &fullName, const QByteArray &identityHash = {});
bool validAspectName(const QString &fullName);

std::optional<Packet> parsePacket(const QByteArray &raw, QString *error = nullptr);
QByteArray packPacket(const Packet &packet);
QByteArray packetHash(const QByteArray &raw);
quint8 packedFlags(const Packet &packet);

QByteArray hdlcFrame(const QByteArray &data);
QByteArray hdlcKeepalive();
QByteArray kissFrame(const QByteArray &data);

class FrameDecoder
{
public:
    enum Framing { Hdlc, Kiss };

    explicit FrameDecoder(Framing framing = Hdlc, int maximumFrame = kStreamHardwareMtu);
    QList<QByteArray> feed(const QByteArray &bytes);
    void reset();
    void setFraming(Framing framing);
    Framing framing() const { return m_framing; }
    qint64 invalidFrames() const { return m_invalid; }

private:
    QList<QByteArray> feedHdlc(const QByteArray &bytes);
    QList<QByteArray> feedKiss(const QByteArray &bytes);
    void finishFrame(QList<QByteArray> *frames);

    Framing m_framing;
    int m_maximumFrame;
    QByteArray m_buffer;
    bool m_inFrame = false;
    bool m_escape = false;
    int m_kissCommand = -1;
    bool m_overflow = false;
    qint64 m_invalid = 0;
};

class Identity
{
public:
    Identity() = default;
    static Identity generate();
    static std::optional<Identity> fromPrivateKey(const QByteArray &privateKey);
    static std::optional<Identity> fromPublicKey(const QByteArray &publicKey);

    bool isValid() const { return m_publicKey.size() == kPublicKeyLength; }
    bool hasPrivateKey() const { return m_privateKey.size() == kPrivateKeyLength; }
    QByteArray publicKey() const { return m_publicKey; }
    QByteArray privateKey() const { return m_privateKey; }
    QByteArray hash() const { return truncatedHash(m_publicKey); }
    QByteArray sign(const QByteArray &data) const;
    bool verify(const QByteArray &signature, const QByteArray &data) const;

private:
    QByteArray m_publicKey;
    QByteArray m_privateKey;
};

std::optional<Announce> parseAnnounce(const Packet &packet, QString *error = nullptr);
bool validateAnnounce(const Announce &announce, QString *error = nullptr);
QByteArray buildAnnounce(const Identity &identity, const QString &fullName,
                         const QByteArray &appData, bool pathResponse,
                         qint64 unixTime, const QByteArray &randomPrefix = {});
QByteArray pathRequestDestination();
QByteArray buildPathRequest(const QByteArray &destinationHash, const QByteArray &tag);
bool parsePathRequest(const Packet &packet, QByteArray *destinationHash, QByteArray *tag);

QVariant decodeMsgPack(const QByteArray &bytes, bool *ok = nullptr);
const QList<KnownAspect> &knownAspects();
std::optional<KnownAspect> knownAspect(const QByteArray &nameHash);
QString displayNameFromAppData(const QString &kind, const QByteArray &appData);
std::optional<DiscoveredInterface> parseDiscoveryAppData(const QByteArray &appData,
                                                         QString *error = nullptr);
int stampValue(const QByteArray &material, const QByteArray &stamp, int expandRounds);

QString hexHash(const QByteArray &hash);
QString prettyHash(const QByteArray &hash);
QByteArray parseHash(const QString &text);
QString contextName(quint8 context);
QString packetTypeName(int type);
QString destinationTypeName(int type);

}
