#pragma once

#include <QByteArray>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSize>
#include <QString>
#include <QStringList>

namespace forkmesh::localcontrol::attachments {

struct Entry {
    QString id;
    QString path;
    QString name;
    QString mediaType;
    qint64 size = 0;
};

constexpr qint64 kMaxRawBytes = 2500 * 1024;
constexpr qint64 kMaxDecodeBytes = 40ll * 1024 * 1024;
constexpr int kPreviewDimension = 2048;
constexpr int kMaxEntries = 50;
constexpr qint64 kMaxRelayPreviewBytes = 700 * 1024;
constexpr int kRelayPreviewDimension = 1600;
constexpr int kMinRelayPreviewDimension = 320;

QByteArray processSalt();

QString mediaTypeForName(const QString &name);
bool isImageType(const QString &mediaType);

QString sniffImage(const QByteArray &head);

QString attachmentId(const QByteArray &salt, int sessionId, const QString &path);

QList<Entry> entries(const QStringList &paths, int sessionId, const QByteArray &salt,
                     int limit = kMaxEntries);

QJsonArray toJson(const QList<Entry> &list);

struct Payload {
    QJsonObject json;
    QString error;
};

Payload payload(const Entry &entry, qint64 maxRawBytes = kMaxRawBytes,
                qint64 maxDecodeBytes = kMaxDecodeBytes,
                int previewDimension = kPreviewDimension,
                qint64 maxPreviewBytes = kMaxRawBytes);

struct Encoded {
    QByteArray jpeg;
    QSize size;
};

QImage flattened(const QImage &image);

Encoded encodeWithin(const QImage &image, qint64 maxBytes,
                     int maxDimension = kRelayPreviewDimension,
                     int minDimension = kMinRelayPreviewDimension);

Payload relayPayload(const Entry &entry, qint64 maxBytes = kMaxRelayPreviewBytes,
                     qint64 maxDecodeBytes = kMaxDecodeBytes,
                     int previewDimension = kRelayPreviewDimension);

}
