#include "LocalControlAttachments.h"

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTemporaryDir>

#include <iostream>

using namespace forkmesh::localcontrol::attachments;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static QString writeFile(const QTemporaryDir &dir, const QString &name, const QByteArray &bytes)
{
    const QString path = dir.filePath(name);
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(bytes);
    file.close();
    return path;
}

static QByteArray encoded(const QImage &image, const char *format, int quality = -1)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, format, quality);
    return bytes;
}

static QImage decoded(const QJsonObject &json)
{
    const QByteArray bytes = QByteArray::fromBase64(json.value(QStringLiteral("data")).toString().toLatin1());
    return QImage::fromData(bytes);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    check(dir.isValid(), "temp dir");

    QImage small(32, 20, QImage::Format_RGB32);
    small.fill(Qt::red);
    const QString png = writeFile(dir, QStringLiteral("paste-1.png"), encoded(small, "PNG"));
    const QString text = writeFile(dir, QStringLiteral("notes.png"), QByteArray("not a picture"));
    const QString video = writeFile(dir, QStringLiteral("clip.mp4"), QByteArray(64, 'x'));
    const QString missing = dir.filePath(QStringLiteral("gone.png"));

    const QByteArray salt = QByteArray(32, 's');
    const QList<Entry> list =
        entries({png, missing, png, text, video}, 7, salt);
    check(list.size() == 3, "missing files and duplicates are dropped");
    check(list.value(0).name == QLatin1String("paste-1.png"), "name is the file name");
    check(list.value(0).mediaType == QLatin1String("image/png"), "type from the extension");
    check(list.value(2).mediaType == QLatin1String("video/mp4"), "video type");
    const QString id = list.value(0).id;
    check(id.size() == 32, "id is 32 hex");
    check(!id.contains(QLatin1String("paste")), "id carries no path");
    check(attachmentId(salt, 7, list.value(0).path) == id, "id is stable");
    check(attachmentId(salt, 8, list.value(0).path) != id, "id differs per session");
    check(attachmentId(QByteArray(32, 't'), 7, list.value(0).path) != id,
          "id differs per process secret");
    check(processSalt() == processSalt() && processSalt().size() == 32, "process secret");
    check(entries({png, text, video}, 7, salt, 2).size() == 2, "list is capped");

    const QJsonArray json = toJson(list);
    check(json.size() == 3, "json rows");
    const QJsonObject row = json.at(0).toObject();
    check(row.value(QStringLiteral("id")).toString() == id, "json id");
    check(!row.contains(QStringLiteral("path")), "json never carries the path");
    check(!QString::fromUtf8(QJsonDocument(json).toJson()).contains(dir.path()),
          "no path anywhere in the list");

    const Payload raw = payload(list.value(0));
    check(raw.error.isEmpty(), "small picture is served");
    check(raw.json.value(QStringLiteral("preview")).toBool() == false, "served as it is");
    check(raw.json.value(QStringLiteral("mediaType")).toString() == QLatin1String("image/png"),
          "sniffed type");
    check(decoded(raw.json).size() == QSize(32, 20), "bytes decode");

    check(payload(list.value(1)).error == QLatin1String("not_an_image"),
          "a .png that is not a picture is refused");
    check(payload(list.value(2)).error == QLatin1String("not_an_image"), "video is refused");

    QImage big(3000, 1500, QImage::Format_ARGB32);
    big.fill(QColor(0, 128, 255, 128));
    const QString bigPath = writeFile(dir, QStringLiteral("shot.png"), encoded(big, "PNG"));
    const QList<Entry> bigList = entries({bigPath}, 7, salt);
    const Payload preview = payload(bigList.value(0), /*maxRawBytes=*/1024);
    check(preview.error.isEmpty(), "large picture gets a preview");
    check(preview.json.value(QStringLiteral("preview")).toBool(), "marked as a preview");
    check(preview.json.value(QStringLiteral("mediaType")).toString() == QLatin1String("image/jpeg"),
          "preview is JPEG");
    const QImage shown = decoded(preview.json);
    check(shown.width() == 2048 && shown.height() == 1024, "preview bounded on its long side");

    check(payload(bigList.value(0), 1024, /*maxDecodeBytes=*/16).error == QLatin1String("too_large"),
          "files past the decode cap are not decoded");

    QRandomGenerator noise(42);
    QImage photo(3000, 2000, QImage::Format_RGB32);
    for (int y = 0; y < photo.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(photo.scanLine(y));
        for (int x = 0; x < photo.width(); ++x)
            line[x] = 0xff000000u | (noise.generate() & 0xffffffu);
    }
    photo.setText(QStringLiteral("Description"), QStringLiteral("secret-location-tag"));
    const QByteArray photoBytes = encoded(photo, "JPEG", 95);
    check(photoBytes.contains("secret-location-tag"), "source carries metadata");
    check(photoBytes.size() > kMaxRelayPreviewBytes, "source is larger than a relay preview");
    const QString photoPath = writeFile(dir, QStringLiteral("photo.jpg"), photoBytes);
    const QList<Entry> photoList = entries({photoPath}, 7, salt);
    const Payload relayed = relayPayload(photoList.value(0));
    check(relayed.error.isEmpty(), "relay preview of a large photo");
    check(relayed.json.value(QStringLiteral("preview")).toBool(), "relay reply is a preview");
    check(relayed.json.value(QStringLiteral("mediaType")).toString() == QLatin1String("image/jpeg"),
          "relay preview is JPEG");
    const QByteArray relayedBytes =
        QByteArray::fromBase64(relayed.json.value(QStringLiteral("data")).toString().toLatin1());
    check(relayedBytes.size() <= kMaxRelayPreviewBytes, "relay preview fits 700 KB");
    check(relayed.json.value(QStringLiteral("size")).toDouble() == relayedBytes.size(),
          "size is the preview's");
    check(relayed.json.value(QStringLiteral("originalSize")).toDouble() == photoBytes.size(),
          "original size is reported");
    check(!relayedBytes.contains("secret-location-tag"), "metadata is stripped");
    const QImage relayedImage = QImage::fromData(relayedBytes);
    check(!relayedImage.isNull(), "relay preview decodes");
    check(qMax(relayedImage.width(), relayedImage.height()) <= kRelayPreviewDimension,
          "relay preview bounded on its long side");
    check(relayed.json.value(QStringLiteral("width")).toInt() == relayedImage.width() &&
              relayed.json.value(QStringLiteral("height")).toInt() == relayedImage.height(),
          "dimensions are reported");
    check(QJsonDocument(relayed.json).toJson(QJsonDocument::Compact).size() < 1000 * 1000,
          "relay reply fits one relay frame");

    const Payload smallRelayed = relayPayload(list.value(0));
    check(smallRelayed.error.isEmpty(), "small picture over the relay");
    check(smallRelayed.json.value(QStringLiteral("preview")).toBool(), "always re-encoded over the relay");
    check(decoded(smallRelayed.json).size() == QSize(32, 20), "small picture keeps its size");

    const Payload alphaRelayed = relayPayload(bigList.value(0));
    check(alphaRelayed.error.isEmpty(), "transparent picture over the relay");
    check(decoded(alphaRelayed.json).width() == kRelayPreviewDimension, "downscaled to the relay bound");

    const QString bigVideo = writeFile(dir, QStringLiteral("long.mp4"), QByteArray(800 * 1024, 'v'));
    const QList<Entry> videoList = entries({bigVideo}, 7, salt);
    check(relayPayload(videoList.value(0)).error == QLatin1String("too_large_over_relay"),
          "a large file is refused over the relay");
    check(relayPayload(list.value(2)).error == QLatin1String("not_an_image"),
          "a small file that is not a picture");
    const QString bigJunk = writeFile(dir, QStringLiteral("fake.png"), QByteArray(800 * 1024, 'j'));
    check(relayPayload(entries({bigJunk}, 7, salt).value(0)).error ==
              QLatin1String("too_large_over_relay"),
          "a large undecodable picture is refused over the relay");
    check(relayPayload(photoList.value(0), kMaxRelayPreviewBytes, 16).error == QLatin1String("too_large"),
          "the decode cap holds over the relay");

    check(encodeWithin(photo, 64).jpeg.isEmpty(), "gives up below the smallest size");
    const Encoded stepped = encodeWithin(photo, 120 * 1024);
    check(!stepped.jpeg.isEmpty() && stepped.jpeg.size() <= 120 * 1024, "steps down to fit");
    check(qMax(stepped.size.width(), stepped.size.height()) >= kMinRelayPreviewDimension,
          "never below the smallest size");
    check(flattened(big).pixelColor(0, 0).alpha() == 255, "flattened is opaque");
    check(flattened(photo).textKeys().isEmpty(), "flattened drops text metadata");

    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "local control attachments: ok\n";
    return 0;
}
