#include "ReticulumProtocol.h"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QRegularExpression>
#include <QVariantList>
#include <QVariantMap>

#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <cmath>
#include <cstring>
#include <memory>

namespace forkmesh::reticulum {

namespace {

constexpr quint8 kHdlcFlag = 0x7E;
constexpr quint8 kHdlcEscape = 0x7D;
constexpr quint8 kHdlcMask = 0x20;
constexpr quint8 kKissFend = 0xC0;
constexpr quint8 kKissFesc = 0xDB;
constexpr quint8 kKissTfend = 0xDC;
constexpr quint8 kKissTfesc = 0xDD;
constexpr quint8 kKissData = 0x00;

struct PkeyDeleter {
    void operator()(EVP_PKEY *key) const { EVP_PKEY_free(key); }
};
struct PkeyContextDeleter {
    void operator()(EVP_PKEY_CTX *context) const { EVP_PKEY_CTX_free(context); }
};
struct DigestContextDeleter {
    void operator()(EVP_MD_CTX *context) const { EVP_MD_CTX_free(context); }
};
using PkeyPointer = std::unique_ptr<EVP_PKEY, PkeyDeleter>;

void setError(QString *error, const QString &text)
{
    if (error)
        *error = text;
}

PkeyPointer generateKey(int type)
{
    std::unique_ptr<EVP_PKEY_CTX, PkeyContextDeleter> context(EVP_PKEY_CTX_new_id(type, nullptr));
    if (!context || EVP_PKEY_keygen_init(context.get()) <= 0)
        return {};
    EVP_PKEY *key = nullptr;
    if (EVP_PKEY_keygen(context.get(), &key) <= 0)
        return {};
    return PkeyPointer(key);
}

QByteArray rawPublic(EVP_PKEY *key)
{
    size_t length = 32;
    QByteArray out(32, '\0');
    if (!key || EVP_PKEY_get_raw_public_key(key, reinterpret_cast<unsigned char *>(out.data()), &length) <= 0 ||
        length != 32)
        return {};
    return out;
}

QByteArray rawPrivate(EVP_PKEY *key)
{
    size_t length = 32;
    QByteArray out(32, '\0');
    if (!key || EVP_PKEY_get_raw_private_key(key, reinterpret_cast<unsigned char *>(out.data()), &length) <= 0 ||
        length != 32)
        return {};
    return out;
}

PkeyPointer makePrivateKey(int type, const QByteArray &raw)
{
    return PkeyPointer(EVP_PKEY_new_raw_private_key(
        type, nullptr, reinterpret_cast<const unsigned char *>(raw.constData()), size_t(raw.size())));
}

PkeyPointer makePublicKey(int type, const QByteArray &raw)
{
    return PkeyPointer(EVP_PKEY_new_raw_public_key(
        type, nullptr, reinterpret_cast<const unsigned char *>(raw.constData()), size_t(raw.size())));
}

class MsgPackReader
{
public:
    explicit MsgPackReader(const QByteArray &bytes) : m_bytes(bytes) {}

    QVariant read(int depth, bool *ok)
    {
        if (depth > 8 || m_offset >= m_bytes.size() || ++m_items > 4096) {
            *ok = false;
            return {};
        }
        const quint8 type = quint8(m_bytes.at(m_offset++));
        if (type <= 0x7f)
            return int(type);
        if (type >= 0xe0)
            return int(qint8(type));
        if ((type & 0xf0) == 0x80)
            return readMap(type & 0x0f, depth, ok);
        if ((type & 0xf0) == 0x90)
            return readArray(type & 0x0f, depth, ok);
        if ((type & 0xe0) == 0xa0)
            return readString(type & 0x1f, ok);
        switch (type) {
        case 0xc0:
            return {};
        case 0xc2:
            return false;
        case 0xc3:
            return true;
        case 0xc4:
            return readBytes(readUnsigned(1, ok), ok);
        case 0xc5:
            return readBytes(readUnsigned(2, ok), ok);
        case 0xc6:
            return readBytes(readUnsigned(4, ok), ok);
        case 0xca: {
            const quint64 bits = readUnsigned(4, ok);
            float value = 0;
            const quint32 narrow = quint32(bits);
            std::memcpy(&value, &narrow, sizeof value);
            return double(value);
        }
        case 0xcb: {
            const quint64 bits = readUnsigned(8, ok);
            double value = 0;
            std::memcpy(&value, &bits, sizeof value);
            return value;
        }
        case 0xcc:
            return readUnsigned(1, ok);
        case 0xcd:
            return readUnsigned(2, ok);
        case 0xce:
            return readUnsigned(4, ok);
        case 0xcf:
            return readUnsigned(8, ok);
        case 0xd0:
            return qint64(qint8(readUnsigned(1, ok)));
        case 0xd1:
            return qint64(qint16(readUnsigned(2, ok)));
        case 0xd2:
            return qint64(qint32(readUnsigned(4, ok)));
        case 0xd3:
            return qint64(readUnsigned(8, ok));
        case 0xd9:
            return readString(readUnsigned(1, ok), ok);
        case 0xda:
            return readString(readUnsigned(2, ok), ok);
        case 0xdb:
            return readString(readUnsigned(4, ok), ok);
        case 0xdc:
            return readArray(readUnsigned(2, ok), depth, ok);
        case 0xdd:
            return readArray(readUnsigned(4, ok), depth, ok);
        case 0xde:
            return readMap(readUnsigned(2, ok), depth, ok);
        case 0xdf:
            return readMap(readUnsigned(4, ok), depth, ok);
        default:
            *ok = false;
            return {};
        }
    }

    bool atEnd() const { return m_offset == m_bytes.size(); }
    int offset() const { return m_offset; }

private:
    quint64 readUnsigned(int width, bool *ok)
    {
        if (m_offset + width > m_bytes.size()) {
            *ok = false;
            return 0;
        }
        quint64 value = 0;
        for (int i = 0; i < width; ++i)
            value = (value << 8) | quint8(m_bytes.at(m_offset++));
        return value;
    }

    QByteArray readBytes(quint64 length, bool *ok)
    {
        if (!*ok || length > quint64(m_bytes.size() - m_offset)) {
            *ok = false;
            return {};
        }
        const QByteArray out = m_bytes.mid(m_offset, int(length));
        m_offset += int(length);
        return out;
    }

    QVariant readString(quint64 length, bool *ok)
    {
        const QByteArray bytes = readBytes(length, ok);
        return *ok ? QVariant(QString::fromUtf8(bytes)) : QVariant();
    }

    QVariant readArray(quint64 count, int depth, bool *ok)
    {
        if (!*ok || count > quint64(m_bytes.size() - m_offset)) {
            *ok = false;
            return {};
        }
        QVariantList list;
        for (quint64 i = 0; i < count && *ok; ++i)
            list.append(read(depth + 1, ok));
        return list;
    }

    QVariant readMap(quint64 count, int depth, bool *ok)
    {
        if (!*ok || count * 2 > quint64(m_bytes.size() - m_offset)) {
            *ok = false;
            return {};
        }
        QVariantMap map;
        for (quint64 i = 0; i < count && *ok; ++i) {
            const QVariant key = read(depth + 1, ok);
            const QVariant value = read(depth + 1, ok);
            map.insert(key.toString(), value);
        }
        return map;
    }

    const QByteArray &m_bytes;
    int m_offset = 0;
    int m_items = 0;
};

QString cleanText(const QVariant &value, int limit = 120)
{
    QString text = value.typeId() == QMetaType::QByteArray ? QString::fromUtf8(value.toByteArray())
                                                          : value.toString();
    text.remove(QChar(0));
    QString out;
    out.reserve(qMin(text.size(), limit));
    for (const QChar ch : text) {
        if (out.size() >= limit)
            break;
        out.append(ch.isPrint() ? ch : QChar(' '));
    }
    return out.simplified();
}

QByteArray msgPackSmallInt(int value)
{
    return QByteArray(1, char(quint8(value)));
}

}

QByteArray fullHash(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

QByteArray truncatedHash(const QByteArray &data)
{
    return fullHash(data).left(kHashLength);
}

QByteArray hmacSha256(const QByteArray &key, const QByteArray &data)
{
    return QMessageAuthenticationCode::hash(data, key, QCryptographicHash::Sha256);
}

QByteArray hkdf(int length, const QByteArray &material, const QByteArray &salt, const QByteArray &context)
{
    if (length < 1 || material.isEmpty())
        return {};
    const QByteArray key = hmacSha256(salt.isEmpty() ? QByteArray(kFullHashLength, '\0') : salt, material);
    QByteArray block;
    QByteArray derived;
    for (int i = 0; derived.size() < length; ++i) {
        block = hmacSha256(key, block + context + QByteArray(1, char(quint8((i + 1) % 256))));
        derived += block;
    }
    return derived.left(length);
}

QByteArray randomBytes(int length)
{
    QByteArray out(length, '\0');
    if (length > 0 && RAND_bytes(reinterpret_cast<unsigned char *>(out.data()), length) != 1)
        return {};
    return out;
}

bool validAspectName(const QString &fullName)
{
    static const QRegularExpression pattern(QStringLiteral("^[^.\\s]+(\\.[^.\\s]+)*$"));
    return !fullName.isEmpty() && fullName.size() <= 128 && pattern.match(fullName).hasMatch();
}

QByteArray nameHash(const QString &fullName)
{
    return fullHash(fullName.toUtf8()).left(kNameHashLength);
}

QByteArray destinationHash(const QByteArray &name, const QByteArray &identityHash)
{
    return truncatedHash(name + identityHash);
}

QByteArray destinationHashForName(const QString &fullName, const QByteArray &identityHash)
{
    return destinationHash(nameHash(fullName), identityHash);
}

quint8 packedFlags(const Packet &packet)
{
    return quint8((packet.ifac ? 0x80 : 0) | ((packet.headerType & 1) << 6) | ((packet.contextFlag ? 1 : 0) << 5) |
                  ((packet.transportType & 1) << 4) | ((packet.destinationType & 3) << 2) | (packet.packetType & 3));
}

std::optional<Packet> parsePacket(const QByteArray &raw, QString *error)
{
    if (raw.size() <= kHeaderMinSize) {
        setError(error, QStringLiteral("frame shorter than a Reticulum header"));
        return std::nullopt;
    }
    Packet packet;
    const quint8 flags = quint8(raw.at(0));
    packet.ifac = flags & 0x80;
    packet.headerType = (flags & 0x40) >> 6;
    packet.contextFlag = flags & 0x20;
    packet.transportType = (flags & 0x10) >> 4;
    packet.destinationType = (flags & 0x0C) >> 2;
    packet.packetType = flags & 0x03;
    packet.hops = quint8(raw.at(1));
    if (packet.ifac) {
        setError(error, QStringLiteral("packet carries an interface access code"));
        return std::nullopt;
    }
    if (packet.hops >= kMaxHops) {
        setError(error, QStringLiteral("hop count %1 is out of range").arg(packet.hops));
        return std::nullopt;
    }
    int offset = 2;
    if (packet.headerType == Header2) {
        if (raw.size() <= kHeaderMaxSize) {
            setError(error, QStringLiteral("frame shorter than a transport header"));
            return std::nullopt;
        }
        packet.transportId = raw.mid(offset, kHashLength);
        offset += kHashLength;
    }
    packet.destinationHash = raw.mid(offset, kHashLength);
    offset += kHashLength;
    packet.context = quint8(raw.at(offset));
    ++offset;
    packet.data = raw.mid(offset);
    if (packet.data.isEmpty()) {
        setError(error, QStringLiteral("packet has no data"));
        return std::nullopt;
    }
    return packet;
}

QByteArray packPacket(const Packet &packet)
{
    QByteArray raw;
    raw.reserve(kHeaderMaxSize + packet.data.size());
    raw.append(char(packedFlags(packet)));
    raw.append(char(quint8(packet.hops)));
    if (packet.headerType == Header2)
        raw.append(packet.transportId.left(kHashLength).leftJustified(kHashLength, '\0'));
    raw.append(packet.destinationHash.left(kHashLength).leftJustified(kHashLength, '\0'));
    raw.append(char(packet.context));
    raw.append(packet.data);
    return raw;
}

QByteArray packetHash(const QByteArray &raw)
{
    if (raw.size() < 3)
        return {};
    const bool header2 = (quint8(raw.at(0)) & 0x40) != 0;
    QByteArray hashable(1, char(quint8(raw.at(0)) & 0x0F));
    hashable += header2 ? raw.mid(kHashLength + 2) : raw.mid(2);
    return fullHash(hashable);
}

QByteArray hdlcFrame(const QByteArray &data)
{
    QByteArray out;
    out.reserve(data.size() + 8);
    out.append(char(kHdlcFlag));
    for (const char byte : data) {
        const quint8 value = quint8(byte);
        if (value == kHdlcEscape || value == kHdlcFlag) {
            out.append(char(kHdlcEscape));
            out.append(char(value ^ kHdlcMask));
        } else {
            out.append(byte);
        }
    }
    out.append(char(kHdlcFlag));
    return out;
}

QByteArray hdlcKeepalive()
{
    return QByteArray(2, char(kHdlcFlag));
}

QByteArray kissFrame(const QByteArray &data)
{
    QByteArray out;
    out.reserve(data.size() + 8);
    out.append(char(kKissFend));
    out.append(char(kKissData));
    for (const char byte : data) {
        const quint8 value = quint8(byte);
        if (value == kKissFesc) {
            out.append(char(kKissFesc));
            out.append(char(kKissTfesc));
        } else if (value == kKissFend) {
            out.append(char(kKissFesc));
            out.append(char(kKissTfend));
        } else {
            out.append(byte);
        }
    }
    out.append(char(kKissFend));
    return out;
}

FrameDecoder::FrameDecoder(Framing framing, int maximumFrame)
    : m_framing(framing)
    , m_maximumFrame(maximumFrame)
{
}

void FrameDecoder::reset()
{
    m_buffer.clear();
    m_inFrame = false;
    m_escape = false;
    m_kissCommand = -1;
    m_overflow = false;
}

void FrameDecoder::setFraming(Framing framing)
{
    m_framing = framing;
    reset();
}

QList<QByteArray> FrameDecoder::feed(const QByteArray &bytes)
{
    return m_framing == Kiss ? feedKiss(bytes) : feedHdlc(bytes);
}

void FrameDecoder::finishFrame(QList<QByteArray> *frames)
{
    if (m_overflow)
        ++m_invalid;
    else if (!m_buffer.isEmpty()) {
        if (m_buffer.size() <= kHeaderMinSize)
            ++m_invalid;
        else
            frames->append(m_buffer);
    }
    m_buffer.clear();
    m_escape = false;
    m_overflow = false;
}

QList<QByteArray> FrameDecoder::feedHdlc(const QByteArray &bytes)
{
    QList<QByteArray> frames;
    for (const char byte : bytes) {
        const quint8 value = quint8(byte);
        if (value == kHdlcFlag) {
            if (m_inFrame)
                finishFrame(&frames);
            m_inFrame = true;
            continue;
        }
        if (!m_inFrame)
            continue;
        if (m_overflow)
            continue;
        if (m_escape) {
            m_escape = false;
            m_buffer.append(char(value ^ kHdlcMask));
        } else if (value == kHdlcEscape) {
            m_escape = true;
            continue;
        } else {
            m_buffer.append(byte);
        }
        if (m_buffer.size() > m_maximumFrame) {
            m_overflow = true;
            m_buffer.clear();
        }
    }
    return frames;
}

QList<QByteArray> FrameDecoder::feedKiss(const QByteArray &bytes)
{
    QList<QByteArray> frames;
    for (const char byte : bytes) {
        const quint8 value = quint8(byte);
        if (value == kKissFend) {
            if (m_inFrame && m_kissCommand == kKissData)
                finishFrame(&frames);
            m_inFrame = true;
            m_kissCommand = -1;
            m_buffer.clear();
            m_escape = false;
            m_overflow = false;
            continue;
        }
        if (!m_inFrame)
            continue;
        if (m_kissCommand < 0) {
            m_kissCommand = value & 0x0F;
            continue;
        }
        if (m_kissCommand != kKissData || m_overflow)
            continue;
        if (value == kKissFesc) {
            m_escape = true;
            continue;
        }
        quint8 out = value;
        if (m_escape) {
            if (value == kKissTfend)
                out = kKissFend;
            else if (value == kKissTfesc)
                out = kKissFesc;
            m_escape = false;
        }
        m_buffer.append(char(out));
        if (m_buffer.size() > m_maximumFrame) {
            m_overflow = true;
            m_buffer.clear();
        }
    }
    return frames;
}

Identity Identity::generate()
{
    Identity identity;
    PkeyPointer encryption = generateKey(EVP_PKEY_X25519);
    PkeyPointer signing = generateKey(EVP_PKEY_ED25519);
    const QByteArray encryptionPrivate = rawPrivate(encryption.get());
    const QByteArray signingPrivate = rawPrivate(signing.get());
    const QByteArray encryptionPublic = rawPublic(encryption.get());
    const QByteArray signingPublic = rawPublic(signing.get());
    if (encryptionPrivate.isEmpty() || signingPrivate.isEmpty() || encryptionPublic.isEmpty() ||
        signingPublic.isEmpty())
        return identity;
    identity.m_privateKey = encryptionPrivate + signingPrivate;
    identity.m_publicKey = encryptionPublic + signingPublic;
    return identity;
}

std::optional<Identity> Identity::fromPrivateKey(const QByteArray &key)
{
    if (key.size() != kPrivateKeyLength)
        return std::nullopt;
    PkeyPointer encryption = makePrivateKey(EVP_PKEY_X25519, key.left(32));
    PkeyPointer signing = makePrivateKey(EVP_PKEY_ED25519, key.mid(32));
    const QByteArray encryptionPublic = rawPublic(encryption.get());
    const QByteArray signingPublic = rawPublic(signing.get());
    if (encryptionPublic.isEmpty() || signingPublic.isEmpty())
        return std::nullopt;
    Identity identity;
    identity.m_privateKey = key;
    identity.m_publicKey = encryptionPublic + signingPublic;
    return identity;
}

std::optional<Identity> Identity::fromPublicKey(const QByteArray &key)
{
    if (key.size() != kPublicKeyLength)
        return std::nullopt;
    if (!makePublicKey(EVP_PKEY_X25519, key.left(32)) || !makePublicKey(EVP_PKEY_ED25519, key.mid(32)))
        return std::nullopt;
    Identity identity;
    identity.m_publicKey = key;
    return identity;
}

QByteArray Identity::sign(const QByteArray &data) const
{
    if (!hasPrivateKey())
        return {};
    return ed25519Sign(m_privateKey.mid(32), data);
}

bool Identity::verify(const QByteArray &signature, const QByteArray &data) const
{
    if (!isValid() || signature.size() != kSignatureLength)
        return false;
    PkeyPointer key = makePublicKey(EVP_PKEY_ED25519, m_publicKey.mid(32));
    std::unique_ptr<EVP_MD_CTX, DigestContextDeleter> context(EVP_MD_CTX_new());
    if (!key || !context || EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) <= 0)
        return false;
    return EVP_DigestVerify(context.get(), reinterpret_cast<const unsigned char *>(signature.constData()),
                            size_t(signature.size()), reinterpret_cast<const unsigned char *>(data.constData()),
                            size_t(data.size())) == 1;
}

std::optional<Announce> parseAnnounce(const Packet &packet, QString *error)
{
    if (packet.packetType != PacketAnnounce) {
        setError(error, QStringLiteral("not an announce"));
        return std::nullopt;
    }
    const int ratchetLength = packet.contextFlag ? kRatchetLength : 0;
    const int fixed = kPublicKeyLength + kNameHashLength + kRandomHashLength + ratchetLength + kSignatureLength;
    if (packet.data.size() < fixed) {
        setError(error, QStringLiteral("announce is %1 bytes, shorter than %2").arg(packet.data.size()).arg(fixed));
        return std::nullopt;
    }
    Announce announce;
    int offset = 0;
    announce.destinationHash = packet.destinationHash;
    announce.publicKey = packet.data.mid(offset, kPublicKeyLength);
    offset += kPublicKeyLength;
    announce.nameHash = packet.data.mid(offset, kNameHashLength);
    offset += kNameHashLength;
    announce.randomHash = packet.data.mid(offset, kRandomHashLength);
    offset += kRandomHashLength;
    announce.ratchet = packet.data.mid(offset, ratchetLength);
    offset += ratchetLength;
    announce.signature = packet.data.mid(offset, kSignatureLength);
    offset += kSignatureLength;
    announce.appData = packet.data.mid(offset);
    announce.hasAppData = !announce.appData.isEmpty();
    announce.identityHash = truncatedHash(announce.publicKey);
    qint64 emitted = 0;
    for (int i = 5; i < kRandomHashLength; ++i)
        emitted = (emitted << 8) | quint8(announce.randomHash.at(i));
    announce.emittedAt = emitted;
    return announce;
}

bool validateAnnounce(const Announce &announce, QString *error)
{
    const std::optional<Identity> identity = Identity::fromPublicKey(announce.publicKey);
    if (!identity) {
        setError(error, QStringLiteral("announce public key is not a valid key pair"));
        return false;
    }
    const QByteArray signedData = announce.destinationHash + announce.publicKey + announce.nameHash +
                                  announce.randomHash + announce.ratchet + announce.appData;
    if (!identity->verify(announce.signature, signedData)) {
        setError(error, QStringLiteral("announce signature does not verify"));
        return false;
    }
    if (destinationHash(announce.nameHash, identity->hash()) != announce.destinationHash) {
        setError(error, QStringLiteral("announce destination does not match its key"));
        return false;
    }
    return true;
}

QByteArray buildAnnounce(const Identity &identity, const QString &fullName, const QByteArray &appData,
                         bool pathResponse, qint64 unixTime, const QByteArray &randomPrefix)
{
    if (!identity.hasPrivateKey() || !validAspectName(fullName))
        return {};
    const QByteArray name = nameHash(fullName);
    const QByteArray destination = destinationHash(name, identity.hash());
    QByteArray random = randomPrefix.size() >= 5 ? randomPrefix.left(5) : fullHash(randomBytes(32)).left(5);
    for (int shift = 32; shift >= 0; shift -= 8)
        random.append(char(quint8((quint64(unixTime) >> shift) & 0xff)));
    const QByteArray signature =
        identity.sign(destination + identity.publicKey() + name + random + appData);
    if (signature.isEmpty())
        return {};
    Packet packet;
    packet.headerType = Header1;
    packet.transportType = TransportBroadcast;
    packet.destinationType = DestinationSingle;
    packet.packetType = PacketAnnounce;
    packet.destinationHash = destination;
    packet.context = pathResponse ? ContextPathResponse : ContextNone;
    packet.data = identity.publicKey() + name + random + signature + appData;
    return packPacket(packet);
}

QByteArray pathRequestDestination()
{
    static const QByteArray hash = destinationHashForName(QStringLiteral("rnstransport.path.request"));
    return hash;
}

QByteArray buildPathRequest(const QByteArray &destination, const QByteArray &tag)
{
    if (destination.size() != kHashLength || tag.isEmpty())
        return {};
    Packet packet;
    packet.headerType = Header1;
    packet.transportType = TransportBroadcast;
    packet.destinationType = DestinationPlain;
    packet.packetType = PacketData;
    packet.destinationHash = pathRequestDestination();
    packet.data = destination + tag.left(kHashLength);
    return packPacket(packet);
}

bool parsePathRequest(const Packet &packet, QByteArray *destination, QByteArray *tag)
{
    if (packet.packetType != PacketData || packet.destinationType != DestinationPlain ||
        packet.destinationHash != pathRequestDestination() || packet.data.size() < kHashLength)
        return false;
    if (destination)
        *destination = packet.data.left(kHashLength);
    if (tag) {
        if (packet.data.size() > kHashLength * 2)
            *tag = packet.data.mid(kHashLength * 2, kHashLength);
        else if (packet.data.size() > kHashLength)
            *tag = packet.data.mid(kHashLength, kHashLength);
        else
            tag->clear();
    }
    return true;
}

QVariant decodeMsgPack(const QByteArray &bytes, bool *ok)
{
    bool good = !bytes.isEmpty();
    MsgPackReader reader(bytes);
    const QVariant value = good ? reader.read(0, &good) : QVariant();
    if (ok)
        *ok = good;
    return good ? value : QVariant();
}

const QList<KnownAspect> &knownAspects()
{
    static const QList<KnownAspect> aspects = [] {
        const QList<QStringList> rows = {
            {QStringLiteral("lxmf.delivery"), QStringLiteral("LXMF messaging"), QStringLiteral("peer")},
            {QStringLiteral("lxmf.propagation"), QStringLiteral("LXMF propagation node"), QStringLiteral("propagation")},
            {QStringLiteral("lxmf.propagation.control"), QStringLiteral("Propagation node control"),
             QStringLiteral("propagation")},
            {QStringLiteral("nomadnetwork.node"), QStringLiteral("Nomad Network node"), QStringLiteral("node")},
            {QStringLiteral("rnstransport.discovery.interface"), QStringLiteral("Interface discovery"),
             QStringLiteral("discovery")},
            {QStringLiteral("rnstransport.remote.management"), QStringLiteral("Transport management"),
             QStringLiteral("transport")},
            {QStringLiteral("rnstransport.info.blackhole"), QStringLiteral("Blackhole list"),
             QStringLiteral("transport")},
            {QStringLiteral("rnstransport.probe"), QStringLiteral("Transport probe"), QStringLiteral("transport")},
            {QStringLiteral("lxst.telephony"), QStringLiteral("LXST voice calls"), QStringLiteral("peer")},
            {QStringLiteral("call.audio"), QStringLiteral("Voice calls"), QStringLiteral("peer")},
            {QStringLiteral("rncp.receive"), QStringLiteral("File transfer (rncp)"), QStringLiteral("service")},
            {QStringLiteral("rnx.execute"), QStringLiteral("Remote command (rnx)"), QStringLiteral("service")},
            {QStringLiteral("forkmesh.node"), QStringLiteral("ForkMesh device"), QStringLiteral("forkmesh")},
        };
        QList<KnownAspect> out;
        for (const QStringList &row : rows)
            out.append({row.at(0), row.at(1), row.at(2), nameHash(row.at(0))});
        return out;
    }();
    return aspects;
}

std::optional<KnownAspect> knownAspect(const QByteArray &hash)
{
    for (const KnownAspect &aspect : knownAspects()) {
        if (aspect.nameHash == hash)
            return aspect;
    }
    return std::nullopt;
}

QString displayNameFromAppData(const QString &kind, const QByteArray &appData)
{
    if (appData.isEmpty())
        return {};
    const quint8 first = quint8(appData.at(0));
    if (kind == QStringLiteral("propagation")) {
        bool ok = false;
        const QVariantList list = decodeMsgPack(appData, &ok).toList();
        if (ok && list.size() >= 7) {
            const QVariantMap metadata = list.at(6).toMap();
            return cleanText(metadata.value(QStringLiteral("1")));
        }
        return {};
    }
    if (kind == QStringLiteral("discovery"))
        return {};
    if ((first >= 0x90 && first <= 0x9f) || first == 0xdc) {
        bool ok = false;
        const QVariantList list = decodeMsgPack(appData, &ok).toList();
        if (!ok || list.isEmpty())
            return {};
        return cleanText(list.first());
    }
    const QString text = QString::fromUtf8(appData);
    if (text.contains(QChar::ReplacementCharacter))
        return {};
    return cleanText(text);
}

int stampValue(const QByteArray &material, const QByteArray &stamp, int expandRounds)
{
    QByteArray workblock;
    workblock.reserve(expandRounds * 256);
    for (int n = 0; n < expandRounds; ++n)
        workblock += hkdf(256, material, fullHash(material + msgPackSmallInt(n)));
    const QByteArray result = fullHash(workblock + stamp);
    int value = 0;
    for (const char byte : result) {
        const quint8 bits = quint8(byte);
        if (bits == 0) {
            value += 8;
            continue;
        }
        for (int bit = 7; bit >= 0 && !(bits & (1 << bit)); --bit)
            ++value;
        break;
    }
    return value;
}

std::optional<DiscoveredInterface> parseDiscoveryAppData(const QByteArray &appData, QString *error)
{
    if (appData.size() <= kStampSize + 1) {
        setError(error, QStringLiteral("discovery data is too short"));
        return std::nullopt;
    }
    DiscoveredInterface found;
    const quint8 flags = quint8(appData.at(0));
    found.encrypted = flags & 0x02;
    if (found.encrypted) {
        setError(error, QStringLiteral("discovery data is encrypted for its network"));
        return std::nullopt;
    }
    const QByteArray payload = appData.mid(1);
    const QByteArray packed = payload.left(payload.size() - kStampSize);
    const QByteArray stamp = payload.right(kStampSize);
    bool ok = false;
    const QVariantMap info = decodeMsgPack(packed, &ok).toMap();
    if (!ok || info.isEmpty()) {
        setError(error, QStringLiteral("discovery data is not a msgpack map"));
        return std::nullopt;
    }
    const auto value = [&info](int key) { return info.value(QString::number(key)); };
    found.type = cleanText(value(0x00), 40);
    found.transport = value(0x01).toBool();
    found.reachableOn = cleanText(value(0x02), 255);
    found.port = value(0x06).toInt();
    found.name = cleanText(value(0xFF), 80);
    found.transportId = value(0xFE).toByteArray().left(kHashLength);
    found.implementation = cleanText(value(0xFD), 40);
    found.version = cleanText(value(0xFC), 40);
    found.networkName = cleanText(value(0x07), 80);
    found.publishesPassphrase = value(0x08).isValid() && !value(0x08).isNull();
    found.frequency = value(0x09).toDouble();
    found.bandwidth = value(0x0A).toDouble();
    found.modulation = cleanText(value(0x0D), 40);
    if (value(0x03).isValid() && !value(0x03).isNull() && value(0x04).isValid() && !value(0x04).isNull()) {
        found.latitude = value(0x03).toDouble();
        found.longitude = value(0x04).toDouble();
        found.height = value(0x05).toDouble();
        found.hasLocation = std::isfinite(found.latitude) && std::isfinite(found.longitude) &&
                            qAbs(found.latitude) <= 90 && qAbs(found.longitude) <= 180;
    }
    if (found.port < 0 || found.port > 65535)
        found.port = 0;
    found.stampValue = stampValue(fullHash(packed), stamp, kDiscoveryExpandRounds);
    found.stampValid = found.stampValue >= kDiscoveryStampValue;
    return found;
}

QByteArray x25519PublicFromPrivate(const QByteArray &privateKey)
{
    if (privateKey.size() != 32)
        return {};
    PkeyPointer key = makePrivateKey(EVP_PKEY_X25519, privateKey);
    return rawPublic(key.get());
}

QByteArray x25519Exchange(const QByteArray &privateKey, const QByteArray &peerPublicKey)
{
    if (privateKey.size() != 32 || peerPublicKey.size() != 32)
        return {};
    PkeyPointer own = makePrivateKey(EVP_PKEY_X25519, privateKey);
    PkeyPointer peer = makePublicKey(EVP_PKEY_X25519, peerPublicKey);
    if (!own || !peer)
        return {};
    std::unique_ptr<EVP_PKEY_CTX, PkeyContextDeleter> context(EVP_PKEY_CTX_new(own.get(), nullptr));
    if (!context || EVP_PKEY_derive_init(context.get()) <= 0 || EVP_PKEY_derive_set_peer(context.get(), peer.get()) <= 0)
        return {};
    size_t length = 32;
    QByteArray shared(32, '\0');
    if (EVP_PKEY_derive(context.get(), reinterpret_cast<unsigned char *>(shared.data()), &length) <= 0 || length != 32)
        return {};
    return shared;
}

QByteArray ed25519PublicFromPrivate(const QByteArray &privateKey)
{
    if (privateKey.size() != 32)
        return {};
    PkeyPointer key = makePrivateKey(EVP_PKEY_ED25519, privateKey);
    return rawPublic(key.get());
}

QByteArray ed25519Sign(const QByteArray &privateKey, const QByteArray &data)
{
    if (privateKey.size() != 32)
        return {};
    PkeyPointer key = makePrivateKey(EVP_PKEY_ED25519, privateKey);
    std::unique_ptr<EVP_MD_CTX, DigestContextDeleter> context(EVP_MD_CTX_new());
    if (!key || !context || EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key.get()) <= 0)
        return {};
    QByteArray signature(kSignatureLength, '\0');
    size_t length = size_t(signature.size());
    if (EVP_DigestSign(context.get(), reinterpret_cast<unsigned char *>(signature.data()), &length,
                       reinterpret_cast<const unsigned char *>(data.constData()), size_t(data.size())) <= 0 ||
        length != size_t(kSignatureLength))
        return {};
    return signature;
}

bool ed25519Verify(const QByteArray &publicKey, const QByteArray &signature, const QByteArray &data)
{
    if (publicKey.size() != 32 || signature.size() != kSignatureLength)
        return false;
    PkeyPointer key = makePublicKey(EVP_PKEY_ED25519, publicKey);
    std::unique_ptr<EVP_MD_CTX, DigestContextDeleter> context(EVP_MD_CTX_new());
    if (!key || !context || EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) <= 0)
        return false;
    return EVP_DigestVerify(context.get(), reinterpret_cast<const unsigned char *>(signature.constData()),
                            size_t(signature.size()), reinterpret_cast<const unsigned char *>(data.constData()),
                            size_t(data.size())) == 1;
}

namespace {

QByteArray aes256Cbc(bool encrypt, const QByteArray &key, const QByteArray &iv, const QByteArray &input)
{
    struct CipherDeleter {
        void operator()(EVP_CIPHER_CTX *context) const { EVP_CIPHER_CTX_free(context); }
    };
    std::unique_ptr<EVP_CIPHER_CTX, CipherDeleter> context(EVP_CIPHER_CTX_new());
    if (!context || key.size() != 32 || iv.size() != 16 ||
        EVP_CipherInit_ex(context.get(), EVP_aes_256_cbc(), nullptr,
                          reinterpret_cast<const unsigned char *>(key.constData()),
                          reinterpret_cast<const unsigned char *>(iv.constData()), encrypt ? 1 : 0) != 1)
        return {};
    QByteArray out(input.size() + 32, '\0');
    int written = 0;
    int finalWritten = 0;
    if (EVP_CipherUpdate(context.get(), reinterpret_cast<unsigned char *>(out.data()), &written,
                         reinterpret_cast<const unsigned char *>(input.constData()), int(input.size())) != 1 ||
        EVP_CipherFinal_ex(context.get(), reinterpret_cast<unsigned char *>(out.data()) + written, &finalWritten) != 1)
        return {};
    out.truncate(written + finalWritten);
    return out;
}

}

QByteArray tokenEncrypt(const QByteArray &key, const QByteArray &plaintext, const QByteArray &iv)
{
    if (key.size() != 64)
        return {};
    const QByteArray vector = iv.size() == 16 ? iv : randomBytes(16);
    const QByteArray ciphertext = aes256Cbc(true, key.mid(32), vector, plaintext);
    if (ciphertext.isEmpty())
        return {};
    const QByteArray signedParts = vector + ciphertext;
    return signedParts + hmacSha256(key.left(32), signedParts);
}

std::optional<QByteArray> tokenDecrypt(const QByteArray &key, const QByteArray &token)
{
    if (key.size() != 64 || token.size() < 16 + 16 + 32 || (token.size() - 48) % 16 != 0)
        return std::nullopt;
    const QByteArray mac = hmacSha256(key.left(32), token.left(token.size() - 32));
    if (CRYPTO_memcmp(mac.constData(), token.constData() + token.size() - 32, 32) != 0)
        return std::nullopt;
    const QByteArray ciphertext = token.mid(16, token.size() - 48);
    struct CipherDeleter {
        void operator()(EVP_CIPHER_CTX *context) const { EVP_CIPHER_CTX_free(context); }
    };
    std::unique_ptr<EVP_CIPHER_CTX, CipherDeleter> context(EVP_CIPHER_CTX_new());
    const QByteArray encryptionKey = key.mid(32);
    const QByteArray iv = token.left(16);
    if (!context || EVP_DecryptInit_ex(context.get(), EVP_aes_256_cbc(), nullptr,
                                       reinterpret_cast<const unsigned char *>(encryptionKey.constData()),
                                       reinterpret_cast<const unsigned char *>(iv.constData())) != 1)
        return std::nullopt;
    QByteArray out(ciphertext.size() + 16, '\0');
    int written = 0;
    int finalWritten = 0;
    if (EVP_DecryptUpdate(context.get(), reinterpret_cast<unsigned char *>(out.data()), &written,
                          reinterpret_cast<const unsigned char *>(ciphertext.constData()), int(ciphertext.size())) != 1 ||
        EVP_DecryptFinal_ex(context.get(), reinterpret_cast<unsigned char *>(out.data()) + written, &finalWritten) != 1)
        return std::nullopt;
    out.truncate(written + finalWritten);
    return out;
}

QByteArray Identity::encrypt(const QByteArray &plaintext, const QByteArray &ratchetPublicKey) const
{
    if (!isValid())
        return {};
    const Identity ephemeral = generate();
    const QByteArray ephemeralPrivate = ephemeral.privateKey().left(32);
    const QByteArray target = ratchetPublicKey.size() == 32 ? ratchetPublicKey : encryptionPublicKey();
    const QByteArray shared = x25519Exchange(ephemeralPrivate, target);
    if (shared.isEmpty())
        return {};
    const QByteArray token = tokenEncrypt(hkdf(64, shared, hash()), plaintext);
    if (token.isEmpty())
        return {};
    return ephemeral.encryptionPublicKey() + token;
}

std::optional<QByteArray> Identity::decrypt(const QByteArray &token) const
{
    if (!hasPrivateKey() || token.size() <= 32)
        return std::nullopt;
    const QByteArray shared = x25519Exchange(m_privateKey.left(32), token.left(32));
    if (shared.isEmpty())
        return std::nullopt;
    return tokenDecrypt(hkdf(64, shared, hash()), token.mid(32));
}

void MsgPackWriter::header(quint8 small, int smallLimit, quint8 base8, quint8 base16, quint8 base32, quint32 size)
{
    if (int(size) < smallLimit && small) {
        m_bytes.append(char(small | size));
    } else if (size <= 0xff && base8) {
        m_bytes.append(char(base8));
        m_bytes.append(char(size));
    } else if (size <= 0xffff) {
        m_bytes.append(char(base16));
        m_bytes.append(char(size >> 8));
        m_bytes.append(char(size & 0xff));
    } else {
        m_bytes.append(char(base32));
        for (int shift = 24; shift >= 0; shift -= 8)
            m_bytes.append(char((size >> shift) & 0xff));
    }
}

MsgPackWriter &MsgPackWriter::nil()
{
    m_bytes.append(char(0xc0));
    return *this;
}

MsgPackWriter &MsgPackWriter::boolean(bool value)
{
    m_bytes.append(char(value ? 0xc3 : 0xc2));
    return *this;
}

MsgPackWriter &MsgPackWriter::integer(qint64 value)
{
    const auto big = [this](quint8 type, quint64 bits, int width) {
        m_bytes.append(char(type));
        for (int shift = (width - 1) * 8; shift >= 0; shift -= 8)
            m_bytes.append(char((bits >> shift) & 0xff));
    };
    if (value >= 0) {
        if (value <= 0x7f)
            m_bytes.append(char(value));
        else if (value <= 0xff)
            big(0xcc, quint64(value), 1);
        else if (value <= 0xffff)
            big(0xcd, quint64(value), 2);
        else if (value <= 0xffffffffLL)
            big(0xce, quint64(value), 4);
        else
            big(0xcf, quint64(value), 8);
    } else {
        if (value >= -32)
            m_bytes.append(char(value));
        else if (value >= -128)
            big(0xd0, quint64(value), 1);
        else if (value >= -32768)
            big(0xd1, quint64(value), 2);
        else if (value >= -2147483648LL)
            big(0xd2, quint64(value), 4);
        else
            big(0xd3, quint64(value), 8);
    }
    return *this;
}

MsgPackWriter &MsgPackWriter::real(double value)
{
    quint64 bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    m_bytes.append(char(0xcb));
    for (int shift = 56; shift >= 0; shift -= 8)
        m_bytes.append(char((bits >> shift) & 0xff));
    return *this;
}

MsgPackWriter &MsgPackWriter::bin(const QByteArray &value)
{
    header(0, 0, 0xc4, 0xc5, 0xc6, quint32(value.size()));
    m_bytes.append(value);
    return *this;
}

MsgPackWriter &MsgPackWriter::str(const QString &value)
{
    const QByteArray utf8 = value.toUtf8();
    header(0xa0, 32, 0xd9, 0xda, 0xdb, quint32(utf8.size()));
    m_bytes.append(utf8);
    return *this;
}

MsgPackWriter &MsgPackWriter::array(int count)
{
    header(0x90, 16, 0, 0xdc, 0xdd, quint32(count));
    return *this;
}

MsgPackWriter &MsgPackWriter::map(int count)
{
    header(0x80, 16, 0, 0xde, 0xdf, quint32(count));
    return *this;
}

MsgPackWriter &MsgPackWriter::raw(const QByteArray &bytes)
{
    m_bytes.append(bytes);
    return *this;
}

bool msgPackArrayItems(const QByteArray &bytes, QList<QPair<int, int>> *items)
{
    if (bytes.isEmpty() || !items)
        return false;
    items->clear();
    const quint8 type = quint8(bytes.at(0));
    int count = 0;
    int offset = 1;
    if ((type & 0xf0) == 0x90) {
        count = type & 0x0f;
    } else if (type == 0xdc && bytes.size() >= 3) {
        count = (quint8(bytes.at(1)) << 8) | quint8(bytes.at(2));
        offset = 3;
    } else {
        return false;
    }
    if (count > 64)
        return false;
    MsgPackReader reader(bytes);
    bool ok = true;
    reader.read(0, &ok);
    if (!ok || !reader.atEnd())
        return false;
    const QByteArray rest = bytes.mid(offset);
    MsgPackReader itemReader(rest);
    for (int i = 0; i < count; ++i) {
        const int start = itemReader.offset();
        itemReader.read(1, &ok);
        if (!ok)
            return false;
        items->append({offset + start, offset + itemReader.offset()});
    }
    return true;
}

InterfaceAccessCode::InterfaceAccessCode(const QString &networkName, const QString &passphrase, int size)
{
    if ((networkName.isEmpty() && passphrase.isEmpty()) || size < 1 || size > 64)
        return;
    QByteArray origin;
    if (!networkName.isEmpty())
        origin += fullHash(networkName.toUtf8());
    if (!passphrase.isEmpty())
        origin += fullHash(passphrase.toUtf8());
    static const QByteArray salt =
        QByteArray::fromHex("adf54d882c9a9b80771eb4995d702d4a3e733391b2a0f53f416d9f907e55cff8");
    m_key = hkdf(64, fullHash(origin), salt);
    m_signingKey = m_key.mid(32);
    m_size = size;
}

QByteArray InterfaceAccessCode::apply(const QByteArray &raw) const
{
    if (!enabled() || raw.size() < 2)
        return raw;
    const QByteArray code = ed25519Sign(m_signingKey, raw).right(m_size);
    QByteArray framed;
    framed.reserve(raw.size() + m_size);
    framed.append(char(quint8(raw.at(0)) | 0x80));
    framed.append(raw.at(1));
    framed.append(code);
    framed.append(raw.mid(2));
    const QByteArray mask = hkdf(int(framed.size()), code, m_key);
    for (int i = 0; i < framed.size(); ++i) {
        if (i >= 2 && i < 2 + m_size)
            continue;
        framed[i] = char(quint8(framed.at(i)) ^ quint8(mask.at(i)));
    }
    framed[0] = char(quint8(framed.at(0)) | 0x80);
    return framed;
}

std::optional<QByteArray> InterfaceAccessCode::remove(const QByteArray &raw) const
{
    if (!enabled() || raw.size() <= 2 + m_size || !(quint8(raw.at(0)) & 0x80))
        return std::nullopt;
    const QByteArray code = raw.mid(2, m_size);
    const QByteArray mask = hkdf(int(raw.size()), code, m_key);
    QByteArray unmasked = raw;
    for (int i = 0; i < unmasked.size(); ++i) {
        if (i >= 2 && i < 2 + m_size)
            continue;
        unmasked[i] = char(quint8(unmasked.at(i)) ^ quint8(mask.at(i)));
    }
    QByteArray packet;
    packet.reserve(raw.size() - m_size);
    packet.append(char(quint8(unmasked.at(0)) & 0x7f));
    packet.append(unmasked.at(1));
    packet.append(unmasked.mid(2 + m_size));
    const QByteArray expected = ed25519Sign(m_signingKey, packet).right(m_size);
    if (expected.size() != code.size() || CRYPTO_memcmp(expected.constData(), code.constData(), size_t(m_size)) != 0)
        return std::nullopt;
    return packet;
}

QByteArray buildProof(const QByteArray &signingPrivateKey, const QByteArray &hash, bool explicitProof,
                      const QByteArray &destination, int destinationType)
{
    const QByteArray signature = ed25519Sign(signingPrivateKey, hash);
    if (signature.isEmpty())
        return {};
    Packet packet;
    packet.packetType = PacketProof;
    packet.destinationType = destinationType;
    packet.destinationHash = destination;
    packet.data = explicitProof ? hash + signature : signature;
    return packPacket(packet);
}

bool validateProof(const QByteArray &signingPublicKey, const QByteArray &hash, const QByteArray &proofData)
{
    if (proofData.size() == kFullHashLength + kSignatureLength)
        return proofData.left(kFullHashLength) == hash &&
               ed25519Verify(signingPublicKey, proofData.mid(kFullHashLength), hash);
    if (proofData.size() == kSignatureLength)
        return ed25519Verify(signingPublicKey, proofData, hash);
    return false;
}

QString hexHash(const QByteArray &hash)
{
    return QString::fromLatin1(hash.toHex());
}

QString prettyHash(const QByteArray &hash)
{
    return QStringLiteral("<%1>").arg(hexHash(hash));
}

QByteArray parseHash(const QString &text)
{
    QString clean = text.trimmed();
    clean.remove(QLatin1Char('<'));
    clean.remove(QLatin1Char('>'));
    clean.remove(QLatin1Char(':'));
    clean.remove(QLatin1Char(' '));
    static const QRegularExpression hex(QStringLiteral("^[0-9a-fA-F]{32}$"));
    if (!hex.match(clean).hasMatch())
        return {};
    return QByteArray::fromHex(clean.toLatin1());
}

QString contextName(quint8 context)
{
    switch (context) {
    case ContextNone:
        return QStringLiteral("none");
    case ContextResource:
    case ContextResourceAdvertisement:
    case ContextResourceRequest:
    case ContextResourceHashmapUpdate:
    case ContextResourceProof:
    case ContextResourceInitiatorCancel:
    case ContextResourceReceiverCancel:
        return QStringLiteral("resource");
    case ContextCacheRequest:
        return QStringLiteral("cache request");
    case ContextRequest:
        return QStringLiteral("request");
    case ContextResponse:
        return QStringLiteral("response");
    case ContextPathResponse:
        return QStringLiteral("path response");
    case ContextCommand:
    case ContextCommandStatus:
        return QStringLiteral("command");
    case ContextChannel:
        return QStringLiteral("channel");
    case ContextKeepalive:
        return QStringLiteral("keepalive");
    case ContextLinkIdentify:
    case ContextLinkClose:
    case ContextLinkProof:
    case ContextLinkRtt:
    case ContextLinkRequestProof:
        return QStringLiteral("link");
    default:
        return QStringLiteral("0x%1").arg(context, 2, 16, QLatin1Char('0'));
    }
}

QString packetTypeName(int type)
{
    switch (type) {
    case PacketData:
        return QStringLiteral("data");
    case PacketAnnounce:
        return QStringLiteral("announce");
    case PacketLinkRequest:
        return QStringLiteral("link request");
    case PacketProof:
        return QStringLiteral("proof");
    default:
        return QStringLiteral("unknown");
    }
}

QString destinationTypeName(int type)
{
    switch (type) {
    case DestinationSingle:
        return QStringLiteral("single");
    case DestinationGroup:
        return QStringLiteral("group");
    case DestinationPlain:
        return QStringLiteral("plain");
    case DestinationLink:
        return QStringLiteral("link");
    default:
        return QStringLiteral("unknown");
    }
}

}
