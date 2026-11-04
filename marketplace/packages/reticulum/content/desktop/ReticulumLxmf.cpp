#include "ReticulumLxmf.h"

#include <QVariantList>
#include <QVariantMap>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <memory>

namespace forkmesh::reticulum {

namespace {

struct DigestDeleter {
    void operator()(EVP_MD_CTX *context) const { EVP_MD_CTX_free(context); }
};
using DigestPointer = std::unique_ptr<EVP_MD_CTX, DigestDeleter>;

int leadingZeroBits(const unsigned char *digest, int length)
{
    int value = 0;
    for (int i = 0; i < length; ++i) {
        const unsigned char byte = digest[i];
        if (byte == 0) {
            value += 8;
            continue;
        }
        for (int bit = 7; bit >= 0 && !(byte & (1 << bit)); --bit)
            ++value;
        break;
    }
    return value;
}

QString textOf(const QVariant &value)
{
    if (value.typeId() == QMetaType::QByteArray)
        return QString::fromUtf8(value.toByteArray());
    return value.toString();
}

}

QByteArray lxmfPayload(double timestamp, const QString &title, const QString &content)
{
    return MsgPackWriter().array(4).real(timestamp).bin(title.toUtf8()).bin(content.toUtf8()).map(0).bytes();
}

QByteArray lxmfMessageId(const QByteArray &destinationHash, const QByteArray &sourceHash, const QByteArray &payload)
{
    return fullHash(destinationHash + sourceHash + payload);
}

QByteArray lxmfPack(const Identity &source, const QByteArray &sourceHash, const QByteArray &destinationHash,
                    double timestamp, const QString &title, const QString &content, const QByteArray &stamp,
                    LxmfMessage *message)
{
    if (!source.hasPrivateKey() || sourceHash.size() != kHashLength || destinationHash.size() != kHashLength)
        return {};
    const QByteArray payload = lxmfPayload(timestamp, title, content);
    const QByteArray hash = lxmfMessageId(destinationHash, sourceHash, payload);
    const QByteArray signature = source.sign(destinationHash + sourceHash + payload + hash);
    if (signature.isEmpty())
        return {};
    QByteArray packedPayload = payload;
    if (!stamp.isEmpty()) {
        packedPayload[0] = char(0x95);
        packedPayload += MsgPackWriter().bin(stamp).bytes();
    }
    const QByteArray packed = destinationHash + sourceHash + signature + packedPayload;
    if (message) {
        message->destinationHash = destinationHash;
        message->sourceHash = sourceHash;
        message->signature = signature;
        message->hash = hash;
        message->stamp = stamp;
        message->signedPayload = payload;
        message->packed = packed;
        message->timestamp = timestamp;
        message->title = title;
        message->content = content;
        message->contentSize = int(packedPayload.size()) - 16;
    }
    return packed;
}

std::optional<LxmfMessage> lxmfUnpack(const QByteArray &packed, QString *error)
{
    const auto fail = [error](const QString &text) {
        if (error)
            *error = text;
        return std::nullopt;
    };
    const int header = 2 * kHashLength + kSignatureLength;
    if (packed.size() <= header)
        return fail(QStringLiteral("message is too short"));
    LxmfMessage message;
    message.destinationHash = packed.left(kHashLength);
    message.sourceHash = packed.mid(kHashLength, kHashLength);
    message.signature = packed.mid(2 * kHashLength, kSignatureLength);
    message.packed = packed;
    const QByteArray payload = packed.mid(header);
    QList<QPair<int, int>> items;
    if (!msgPackArrayItems(payload, &items) || items.size() < 4)
        return fail(QStringLiteral("message payload is not an LXMF array"));
    const auto item = [&payload, &items](int index) {
        return payload.mid(items.at(index).first, items.at(index).second - items.at(index).first);
    };
    message.signedPayload = QByteArray(1, char(0x94)) +
                            payload.mid(items.at(0).first, items.at(3).second - items.at(0).first);
    if (items.size() >= 5)
        message.stamp = decodeMsgPack(item(4)).toByteArray().left(64);
    message.hash = lxmfMessageId(message.destinationHash, message.sourceHash, message.signedPayload);
    bool ok = false;
    const QVariant timestamp = decodeMsgPack(item(0), &ok);
    if (!ok)
        return fail(QStringLiteral("message timestamp is unreadable"));
    message.timestamp = timestamp.toDouble();
    message.title = textOf(decodeMsgPack(item(1))).left(1024);
    message.content = textOf(decodeMsgPack(item(2)));
    const QVariantMap fields = decodeMsgPack(item(3)).toMap();
    for (const QVariant &entry : fields.value(QStringLiteral("5")).toList()) {
        const QVariantList pair = entry.toList();
        if (pair.size() >= 2)
            message.attachments.append({textOf(pair.at(0)).left(255), pair.at(1).toByteArray()});
    }
    message.contentSize = int(payload.size()) - 16;
    return message;
}

bool lxmfVerify(const LxmfMessage &message, const QByteArray &sourcePublicKey)
{
    if (sourcePublicKey.size() != kPublicKeyLength)
        return false;
    return ed25519Verify(sourcePublicKey.mid(32), message.signature,
                         message.destinationHash + message.sourceHash + message.signedPayload + message.hash);
}

QByteArray lxmfAnnounceData(const QString &displayName, int stampCost)
{
    MsgPackWriter writer;
    writer.array(3);
    if (displayName.isEmpty())
        writer.nil();
    else
        writer.bin(displayName.toUtf8());
    if (stampCost > 0 && stampCost < 255)
        writer.integer(stampCost);
    else
        writer.nil();
    writer.array(0);
    return writer.bytes();
}

int lxmfStampCostFromAppData(const QByteArray &appData)
{
    if (appData.isEmpty())
        return 0;
    const quint8 first = quint8(appData.at(0));
    if (!((first >= 0x90 && first <= 0x9f) || first == 0xdc))
        return 0;
    bool ok = false;
    const QVariantList list = decodeMsgPack(appData, &ok).toList();
    if (!ok || list.size() < 2 || list.at(1).isNull())
        return 0;
    const int cost = list.at(1).toInt();
    return cost > 0 && cost < 255 ? cost : 0;
}

QByteArray stampWorkblock(const QByteArray &material, int rounds)
{
    QByteArray workblock;
    workblock.reserve(rounds * 256);
    for (int n = 0; n < rounds; ++n)
        workblock += hkdf(256, material, fullHash(material + MsgPackWriter().integer(n).bytes()));
    return workblock;
}

int stampValueFromWorkblock(const QByteArray &workblock, const QByteArray &stamp)
{
    const QByteArray digest = fullHash(workblock + stamp);
    return leadingZeroBits(reinterpret_cast<const unsigned char *>(digest.constData()), int(digest.size()));
}

QByteArray generateStamp(const QByteArray &messageId, int cost, const std::atomic_bool *cancel, int *value)
{
    if (cost <= 0 || cost > kMaxStampCost)
        return {};
    const QByteArray workblock = stampWorkblock(messageId, kLxmfStampRounds);
    DigestPointer base(EVP_MD_CTX_new());
    DigestPointer work(EVP_MD_CTX_new());
    if (!base || !work || EVP_DigestInit_ex(base.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(base.get(), workblock.constData(), size_t(workblock.size())) != 1)
        return {};
    unsigned char candidate[32];
    unsigned char digest[32];
    unsigned int length = 0;
    for (quint64 attempt = 0;; ++attempt) {
        if (cancel && (attempt & 0xff) == 0 && cancel->load())
            return {};
        if (RAND_bytes(candidate, sizeof candidate) != 1)
            return {};
        if (EVP_MD_CTX_copy_ex(work.get(), base.get()) != 1 ||
            EVP_DigestUpdate(work.get(), candidate, sizeof candidate) != 1 ||
            EVP_DigestFinal_ex(work.get(), digest, &length) != 1)
            return {};
        const int bits = leadingZeroBits(digest, int(length));
        if (bits >= cost) {
            if (value)
                *value = bits;
            return QByteArray(reinterpret_cast<const char *>(candidate), sizeof candidate);
        }
    }
}

bool validateStamp(const QByteArray &messageId, const QByteArray &stamp, int cost)
{
    if (cost <= 0)
        return true;
    if (stamp.size() != 32)
        return false;
    return stampValueFromWorkblock(stampWorkblock(messageId, kLxmfStampRounds), stamp) >= cost;
}

QByteArray packResourceAdvertisement(const ResourceAdvertisement &advertisement, int segment)
{
    const int start = segment * kResourceHashmapMaxLength;
    const int end = qMin((segment + 1) * kResourceHashmapMaxLength, advertisement.parts);
    MsgPackWriter writer;
    writer.map(11);
    writer.str(QStringLiteral("t")).integer(advertisement.transferSize);
    writer.str(QStringLiteral("d")).integer(advertisement.dataSize);
    writer.str(QStringLiteral("n")).integer(advertisement.parts);
    writer.str(QStringLiteral("h")).bin(advertisement.hash);
    writer.str(QStringLiteral("r")).bin(advertisement.randomHash);
    writer.str(QStringLiteral("o")).bin(advertisement.originalHash);
    writer.str(QStringLiteral("i")).integer(advertisement.segmentIndex);
    writer.str(QStringLiteral("l")).integer(advertisement.segments);
    writer.str(QStringLiteral("q"));
    if (advertisement.requestId.isEmpty())
        writer.nil();
    else
        writer.bin(advertisement.requestId);
    writer.str(QStringLiteral("f")).integer(advertisement.flags);
    writer.str(QStringLiteral("m"))
        .bin(advertisement.hashmap.mid(start * kResourceMapHashLength, (end - start) * kResourceMapHashLength));
    return writer.bytes();
}

std::optional<ResourceAdvertisement> unpackResourceAdvertisement(const QByteArray &bytes)
{
    bool ok = false;
    const QVariantMap map = decodeMsgPack(bytes, &ok).toMap();
    if (!ok)
        return std::nullopt;
    for (const char *key : {"t", "d", "n", "h", "r", "o", "i", "l", "f", "m"}) {
        if (!map.contains(QString::fromLatin1(key)))
            return std::nullopt;
    }
    ResourceAdvertisement advertisement;
    advertisement.transferSize = map.value(QStringLiteral("t")).toLongLong();
    advertisement.dataSize = map.value(QStringLiteral("d")).toLongLong();
    advertisement.parts = map.value(QStringLiteral("n")).toInt();
    advertisement.hash = map.value(QStringLiteral("h")).toByteArray();
    advertisement.randomHash = map.value(QStringLiteral("r")).toByteArray();
    advertisement.originalHash = map.value(QStringLiteral("o")).toByteArray();
    advertisement.segmentIndex = map.value(QStringLiteral("i")).toInt();
    advertisement.segments = map.value(QStringLiteral("l")).toInt();
    advertisement.requestId = map.value(QStringLiteral("q")).toByteArray();
    advertisement.flags = map.value(QStringLiteral("f")).toInt();
    advertisement.hashmap = map.value(QStringLiteral("m")).toByteArray();
    if (advertisement.transferSize <= 0 || advertisement.transferSize > 3LL * kResourceMaxSize ||
        advertisement.parts <= 0 || advertisement.parts > 100000 || advertisement.hash.size() != kFullHashLength ||
        advertisement.randomHash.size() != kResourceRandomLength ||
        advertisement.hashmap.size() % kResourceMapHashLength != 0)
        return std::nullopt;
    return advertisement;
}

QByteArray resourceMapHash(const QByteArray &part, const QByteArray &randomHash)
{
    return fullHash(part + randomHash).left(kResourceMapHashLength);
}

}
