#include "LocalControlAttachments.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QRandomGenerator>
#include <QSet>

namespace forkmesh::localcontrol::attachments {

namespace {

QMutex &saltMutex()
{
    static QMutex mutex;
    return mutex;
}

QByteArray &sharedSalt()
{
    static QByteArray salt;
    return salt;
}

} // namespace

void setProcessSalt(const QByteArray &salt)
{
    if (salt.size() < 16)
        return;
    const QMutexLocker lock(&saltMutex());
    sharedSalt() = salt;
}

QByteArray processSalt()
{
    {
        const QMutexLocker lock(&saltMutex());
        if (!sharedSalt().isEmpty())
            return sharedSalt();
    }
    static const QByteArray salt = [] {
        QByteArray bytes(32, Qt::Uninitialized);
        QRandomGenerator::system()->generate(
            reinterpret_cast<quint32 *>(bytes.data()),
            reinterpret_cast<quint32 *>(bytes.data() + bytes.size()));
        return bytes;
    }();
    return salt;
}

QString mediaTypeForName(const QString &name)
{
    const QString ext = QFileInfo(name).suffix().toLower();
    if (ext == QLatin1String("jpg") || ext == QLatin1String("jpeg"))
        return QStringLiteral("image/jpeg");
    if (ext == QLatin1String("png"))
        return QStringLiteral("image/png");
    if (ext == QLatin1String("gif"))
        return QStringLiteral("image/gif");
    if (ext == QLatin1String("webp"))
        return QStringLiteral("image/webp");
    if (ext == QLatin1String("heic") || ext == QLatin1String("heif"))
        return QStringLiteral("image/heic");
    if (ext == QLatin1String("bmp"))
        return QStringLiteral("image/bmp");
    if (ext == QLatin1String("mp4") || ext == QLatin1String("m4v"))
        return QStringLiteral("video/mp4");
    if (ext == QLatin1String("mov"))
        return QStringLiteral("video/quicktime");
    if (ext == QLatin1String("webm"))
        return QStringLiteral("video/webm");
    return QStringLiteral("application/octet-stream");
}

bool isImageType(const QString &mediaType)
{
    return mediaType.startsWith(QLatin1String("image/"));
}

QString sniffImage(const QByteArray &head)
{
    if (head.startsWith("\xff\xd8\xff"))
        return QStringLiteral("image/jpeg");
    if (head.startsWith("\x89PNG\r\n\x1a\n"))
        return QStringLiteral("image/png");
    if (head.startsWith("GIF87a") || head.startsWith("GIF89a"))
        return QStringLiteral("image/gif");
    if (head.size() >= 12 && head.startsWith("RIFF") && head.mid(8, 4) == "WEBP")
        return QStringLiteral("image/webp");
    return QString();
}

QString attachmentId(const QByteArray &salt, int sessionId, const QString &path)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(salt);
    hash.addData(QByteArray("\n") + QByteArray::number(sessionId) + "\n");
    hash.addData(path.toUtf8());
    return QString::fromLatin1(hash.result().toHex().left(32));
}

QList<Entry> entries(const QStringList &paths, int sessionId, const QByteArray &salt, int limit)
{
    QList<Entry> list;
    QSet<QString> seen;
    for (const QString &raw : paths) {
        if (list.size() >= limit)
            break;
        const QFileInfo info(raw);
        if (!info.exists() || !info.isFile() || !info.isReadable())
            continue;
        const QString path = info.canonicalFilePath();
        if (path.isEmpty() || seen.contains(path))
            continue;
        seen.insert(path);
        Entry entry;
        entry.path = path;
        entry.name = info.fileName();
        entry.mediaType = mediaTypeForName(entry.name);
        entry.size = info.size();
        entry.id = attachmentId(salt, sessionId, path);
        list.append(entry);
    }
    return list;
}

QJsonArray toJson(const QList<Entry> &list)
{
    QJsonArray array;
    for (const Entry &entry : list) {
        array.append(QJsonObject{
            {QStringLiteral("id"), entry.id},
            {QStringLiteral("name"), entry.name},
            {QStringLiteral("mediaType"), entry.mediaType},
            {QStringLiteral("size"), double(entry.size)},
        });
    }
    return array;
}

Payload payload(const Entry &entry, qint64 maxRawBytes, qint64 maxDecodeBytes,
                int previewDimension, qint64 maxPreviewBytes)
{
    Payload out;
    if (!isImageType(entry.mediaType)) {
        out.error = QStringLiteral("not_an_image");
        return out;
    }
    QFile file(entry.path);
    if (!file.open(QIODevice::ReadOnly)) {
        out.error = QStringLiteral("unreadable");
        return out;
    }
    const qint64 size = file.size();
    if (size <= 0) {
        out.error = QStringLiteral("unreadable");
        return out;
    }
    if (size > maxDecodeBytes) {
        out.error = QStringLiteral("too_large");
        return out;
    }
    const QByteArray bytes = file.readAll();
    file.close();
    if (bytes.size() != size) {
        out.error = QStringLiteral("unreadable");
        return out;
    }
    const QString sniffed = sniffImage(bytes.left(16));
    if (!sniffed.isEmpty() && size <= maxRawBytes) {
        out.json = QJsonObject{
            {QStringLiteral("name"), entry.name},
            {QStringLiteral("mediaType"), sniffed},
            {QStringLiteral("size"), double(size)},
            {QStringLiteral("data"), QString::fromLatin1(bytes.toBase64())},
            {QStringLiteral("preview"), false},
        };
        return out;
    }
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    const QSize full = reader.size();
    if (full.isValid() && (full.width() > previewDimension || full.height() > previewDimension))
        reader.setScaledSize(full.scaled(previewDimension, previewDimension, Qt::KeepAspectRatio));
    QImage image = reader.read();
    if (image.isNull()) {
        out.error = QStringLiteral("not_an_image");
        return out;
    }
    if (image.width() > previewDimension || image.height() > previewDimension)
        image = image.scaled(previewDimension, previewDimension, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    if (image.hasAlphaChannel()) {
        QImage flat(image.size(), QImage::Format_RGB32);
        flat.fill(Qt::white);
        QPainter painter(&flat);
        painter.drawImage(0, 0, image);
        painter.end();
        image = flat;
    }
    QByteArray jpeg;
    QBuffer sink(&jpeg);
    sink.open(QIODevice::WriteOnly);
    int quality = 80;
    while (true) {
        jpeg.clear();
        sink.seek(0);
        if (!image.save(&sink, "JPEG", quality)) {
            out.error = QStringLiteral("unreadable");
            return out;
        }
        if (jpeg.size() <= maxPreviewBytes || quality <= 40)
            break;
        quality -= 20;
    }
    if (jpeg.size() > maxPreviewBytes) {
        out.error = QStringLiteral("too_large");
        return out;
    }
    out.json = QJsonObject{
        {QStringLiteral("name"), entry.name},
        {QStringLiteral("mediaType"), QStringLiteral("image/jpeg")},
        {QStringLiteral("size"), double(jpeg.size())},
        {QStringLiteral("data"), QString::fromLatin1(jpeg.toBase64())},
        {QStringLiteral("preview"), true},
    };
    return out;
}

QImage flattened(const QImage &image)
{
    QImage flat(image.size(), QImage::Format_RGB32);
    flat.fill(Qt::white);
    QPainter painter(&flat);
    painter.drawImage(0, 0, image);
    painter.end();
    return flat;
}

Encoded encodeWithin(const QImage &image, qint64 maxBytes, int maxDimension, int minDimension)
{
    Encoded out;
    if (image.isNull() || maxBytes <= 0)
        return out;
    const QImage flat = flattened(image);
    const int longSide = qMax(flat.width(), flat.height());
    int dimension = qMin(maxDimension, longSide);
    static const int qualities[] = {82, 70, 58, 46, 34};
    while (true) {
        const QImage scaled =
            longSide > dimension
                ? flat.scaled(dimension, dimension, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                : flat;
        for (const int quality : qualities) {
            QByteArray jpeg;
            QBuffer sink(&jpeg);
            sink.open(QIODevice::WriteOnly);
            if (!scaled.save(&sink, "JPEG", quality))
                return out;
            if (jpeg.size() <= maxBytes) {
                out.jpeg = jpeg;
                out.size = scaled.size();
                return out;
            }
        }
        if (dimension <= minDimension)
            return out;
        dimension = qMax(minDimension, dimension * 2 / 3);
    }
}

Payload relayPayload(const Entry &entry, qint64 maxBytes, qint64 maxDecodeBytes,
                     int previewDimension)
{
    Payload out;
    QFile file(entry.path);
    if (!file.open(QIODevice::ReadOnly)) {
        out.error = QStringLiteral("unreadable");
        return out;
    }
    const qint64 size = file.size();
    if (size <= 0) {
        out.error = QStringLiteral("unreadable");
        return out;
    }
    if (!isImageType(entry.mediaType)) {
        out.error = size > maxBytes ? QStringLiteral("too_large_over_relay")
                                    : QStringLiteral("not_an_image");
        return out;
    }
    if (size > maxDecodeBytes) {
        out.error = QStringLiteral("too_large");
        return out;
    }
    const QByteArray bytes = file.readAll();
    file.close();
    if (bytes.size() != size) {
        out.error = QStringLiteral("unreadable");
        return out;
    }
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    const QSize full = reader.size();
    if (full.isValid() && (full.width() > previewDimension || full.height() > previewDimension))
        reader.setScaledSize(full.scaled(previewDimension, previewDimension, Qt::KeepAspectRatio));
    const QImage image = reader.read();
    if (image.isNull()) {
        out.error = size > maxBytes ? QStringLiteral("too_large_over_relay")
                                    : QStringLiteral("not_an_image");
        return out;
    }
    const Encoded encoded = encodeWithin(image, maxBytes, previewDimension);
    if (encoded.jpeg.isEmpty()) {
        out.error = QStringLiteral("too_large_over_relay");
        return out;
    }
    out.json = QJsonObject{
        {QStringLiteral("name"), entry.name},
        {QStringLiteral("mediaType"), QStringLiteral("image/jpeg")},
        {QStringLiteral("size"), double(encoded.jpeg.size())},
        {QStringLiteral("data"), QString::fromLatin1(encoded.jpeg.toBase64())},
        {QStringLiteral("preview"), true},
        {QStringLiteral("originalSize"), double(size)},
        {QStringLiteral("width"), encoded.size.width()},
        {QStringLiteral("height"), encoded.size.height()},
    };
    return out;
}

}
