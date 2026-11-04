#pragma once

#include "ReticulumProtocol.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <atomic>
#include <optional>

namespace forkmesh::reticulum {

constexpr int kLxmfOverhead = 2 * kHashLength + kSignatureLength + 8 + 8;
constexpr int kEncryptedPacketMdu = 383;
constexpr int kLinkMdu = 431;
constexpr int kLxmfOpportunisticMaxContent = kEncryptedPacketMdu - kLxmfOverhead + kHashLength;
constexpr int kLxmfLinkPacketMaxContent = kLinkMdu - kLxmfOverhead;
constexpr int kLxmfStampRounds = 3000;
constexpr int kMaxStampCost = 24;
constexpr int kResourceMapHashLength = 4;
constexpr int kResourceRandomLength = 4;
constexpr int kResourceHashmapMaxLength = (kLinkMdu - 134) / kResourceMapHashLength;
constexpr int kResourceMaxSize = 1024 * 1024 - 1;

struct LxmfAttachment {
    QString name;
    QByteArray data;
};

struct LxmfMessage {
    QByteArray destinationHash;
    QByteArray sourceHash;
    QByteArray signature;
    QByteArray hash;
    QByteArray stamp;
    QByteArray signedPayload;
    QByteArray packed;
    double timestamp = 0;
    QString title;
    QString content;
    QList<LxmfAttachment> attachments;
    int contentSize = 0;
};

QByteArray lxmfPayload(double timestamp, const QString &title, const QString &content);
QByteArray lxmfMessageId(const QByteArray &destinationHash, const QByteArray &sourceHash, const QByteArray &payload);
QByteArray lxmfPack(const Identity &source, const QByteArray &sourceHash, const QByteArray &destinationHash,
                    double timestamp, const QString &title, const QString &content, const QByteArray &stamp = {},
                    LxmfMessage *message = nullptr);
std::optional<LxmfMessage> lxmfUnpack(const QByteArray &packed, QString *error = nullptr);
bool lxmfVerify(const LxmfMessage &message, const QByteArray &sourcePublicKey);
QByteArray lxmfAnnounceData(const QString &displayName, int stampCost = 0);
int lxmfStampCostFromAppData(const QByteArray &appData);

QByteArray stampWorkblock(const QByteArray &material, int rounds);
int stampValueFromWorkblock(const QByteArray &workblock, const QByteArray &stamp);
QByteArray generateStamp(const QByteArray &messageId, int cost, const std::atomic_bool *cancel = nullptr,
                         int *value = nullptr);
bool validateStamp(const QByteArray &messageId, const QByteArray &stamp, int cost);

struct ResourceAdvertisement {
    qint64 transferSize = 0;
    qint64 dataSize = 0;
    int parts = 0;
    QByteArray hash;
    QByteArray randomHash;
    QByteArray originalHash;
    int segmentIndex = 1;
    int segments = 1;
    QByteArray requestId;
    int flags = 0;
    QByteArray hashmap;
    bool encrypted() const { return flags & 0x01; }
    bool compressed() const { return flags & 0x02; }
    bool split() const { return flags & 0x04; }
    bool isRequest() const { return flags & 0x08; }
    bool isResponse() const { return flags & 0x10; }
    bool hasMetadata() const { return flags & 0x20; }
};

QByteArray packResourceAdvertisement(const ResourceAdvertisement &advertisement, int segment = 0);
std::optional<ResourceAdvertisement> unpackResourceAdvertisement(const QByteArray &bytes);
QByteArray resourceMapHash(const QByteArray &part, const QByteArray &randomHash);

}
