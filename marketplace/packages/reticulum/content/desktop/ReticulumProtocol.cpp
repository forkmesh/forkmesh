#include "ReticulumProtocol.h"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QRegularExpression>
#include <QVariantList>
#include <QVariantMap>

#include <openssl/evp.h>
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
    PkeyPointer key = makePrivateKey(EVP_PKEY_ED25519, m_privateKey.mid(32));
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
            {QStringLiteral("forkmesh.node"), QStringLiteral("ForkMesh desktop"), QStringLiteral("forkmesh")},
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
