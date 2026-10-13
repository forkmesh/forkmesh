#include "LocalControlPromptImages.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QTemporaryDir>

#include <iostream>

using namespace forkmesh::localcontrolimages;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static QString digest(const QByteArray &data)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

static QByteArray readAll(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

int main()
{
    QTemporaryDir dir;
    check(dir.isValid(), "a temporary directory");
    const QString images = dir.filePath(QStringLiteral("agent-images"));
    const QByteArray jpeg =
        QByteArray("\xff\xd8\xff\xe0", 4) + QByteArray(kMaxChunkBytes + 100, 'x');
    const QByteArray png = QByteArray("\x89PNG\r\n\x1a\n", 8) + QByteArray(40, '\0');
    QString error;

    {
        Staging staging;
        const QString a = staging.begin(1, QStringLiteral("image/jpeg"), jpeg.size(),
                                        digest(jpeg), &error);
        check(a.size() == 32, "begin names a 32-hex image");
        check(staging.chunk(1, a, jpeg.left(kMaxChunkBytes), &error) == kMaxChunkBytes,
              "a full chunk is taken");
        check(staging.chunk(1, a, jpeg.mid(kMaxChunkBytes), &error) == jpeg.size(),
              "the rest arrives");
        const QString b = staging.begin(1, QStringLiteral("image/png"), png.size(),
                                        digest(png).toUpper(), &error);
        check(!b.isEmpty(), "an upper-case digest is accepted");
        check(staging.chunk(1, b, png, &error) == png.size(), "a small image in one chunk");

        QStringList ids;
        check(staging.parseIds(QJsonArray{a, b}, &ids) && ids == QStringList{a, b},
              "ids parse in order");
        error.clear();
        const QStringList paths = staging.take(1, ids, images, &error);
        check(error.isEmpty() && paths.size() == 2, "both images are written");
        check(paths.size() == 2 && readAll(paths.at(0)) == jpeg && readAll(paths.at(1)) == png,
              "the bytes on disk are the bytes sent");
        check(paths.size() == 2 && QFileInfo(paths.at(0)).fileName().startsWith(QStringLiteral("phone-")) &&
                  paths.at(0).endsWith(QStringLiteral(".jpg")) &&
                  paths.at(1).endsWith(QStringLiteral(".png")),
              "files are named for the phone with the right extension");
        check(staging.stagedCount(1) == 0, "taken images leave the stage");
        error.clear();
        check(staging.take(1, ids, images, &error).isEmpty() &&
                  error == QStringLiteral("unknown_image"),
              "an image cannot be used twice");
    }

    {
        Staging staging;
        error.clear();
        check(staging.take(1, {}, images, &error).isEmpty() && error.isEmpty(),
              "no images is not an error");
        QStringList ids;
        check(staging.parseIds(QJsonValue(), &ids) && ids.isEmpty(), "a missing list is empty");
        check(!staging.parseIds(QJsonValue(QStringLiteral("x")), &ids), "a string is not a list");
        check(!staging.parseIds(QJsonArray{QStringLiteral("../etc/passwd")}, &ids),
              "a path is not an id");
        const QString id = QStringLiteral("0123456789abcdef0123456789abcdef");
        check(!staging.parseIds(QJsonArray{id, id}, &ids), "a repeated id is refused");
        QJsonArray six;
        for (int i = 0; i < 6; ++i)
            six.append(QStringLiteral("%1").arg(i, 32, 16, QLatin1Char('0')));
        check(!staging.parseIds(six, &ids), "more than five images are refused");
    }

    {
        Staging staging;
        error.clear();
        check(staging.begin(1, QStringLiteral("image/svg+xml"), png.size(), digest(png), &error)
                      .isEmpty() &&
                  error == QStringLiteral("bad_image"),
              "an unsupported type is refused");
        check(staging.begin(1, QStringLiteral("image/png"), 6 << 20, digest(png), &error)
                  .isEmpty(),
              "an image over 5 MB is refused");
        check(staging.begin(1, QStringLiteral("image/png"), png.size(), QStringLiteral("zz"), &error)
                  .isEmpty(),
              "a malformed digest is refused");
        check(staging.stagedCount(1) == 0, "refusals stage nothing");

        for (int i = 0; i < kMaxStagedPerConnection; ++i)
            staging.begin(1, QStringLiteral("image/png"), png.size(), digest(png), &error);
        error.clear();
        check(staging.begin(1, QStringLiteral("image/png"), png.size(), digest(png), &error)
                      .isEmpty() &&
                  error == QStringLiteral("too_many_images"),
              "a connection stages at most five images");
        check(!staging.begin(2, QStringLiteral("image/png"), png.size(), digest(png), &error)
                   .isEmpty(),
              "another connection has its own stage");
        staging.dropConnection(1);
        check(staging.stagedCount(1) == 0 && staging.stagedCount(2) == 1,
              "a closed connection drops only its images");
    }

    {
        Staging staging;
        const QString a = staging.begin(1, QStringLiteral("image/png"), png.size(),
                                        digest(png), &error);
        error.clear();
        check(staging.chunk(2, a, png, &error) < 0 && error == QStringLiteral("unknown_image"),
              "another connection cannot write the image");
        error.clear();
        check(staging.chunk(1, a, png + QByteArray("x"), &error) < 0 &&
                  error == QStringLiteral("bad_chunk"),
              "more bytes than declared are refused");
        check(staging.stagedCount(1) == 0, "an overflowing image is dropped");

        const QString b = staging.begin(1, QStringLiteral("image/png"), png.size(),
                                        digest(png), &error);
        staging.chunk(1, b, png.left(10), &error);
        error.clear();
        check(staging.take(1, {b}, images, &error).isEmpty() &&
                  error == QStringLiteral("image_incomplete"),
              "a partial image is refused");

        const QByteArray fake = QByteArray("GIF89a") + QByteArray(40, '\0');
        const QString c = staging.begin(1, QStringLiteral("image/png"), fake.size(),
                                        digest(fake), &error);
        staging.chunk(1, c, fake, &error);
        error.clear();
        check(staging.take(1, {c}, images, &error).isEmpty() &&
                  error == QStringLiteral("bad_image"),
              "bytes that are not the declared type are refused");

        const QByteArray other = QByteArray("\x89PNG\r\n\x1a\n", 8) + QByteArray(40, '\1');
        const QString d = staging.begin(1, QStringLiteral("image/png"), other.size(),
                                        digest(png), &error);
        staging.chunk(1, d, other, &error);
        error.clear();
        check(staging.take(1, {d}, images, &error).isEmpty() &&
                  error == QStringLiteral("bad_image"),
              "bytes that do not match the digest are refused");

        const QString good = staging.begin(1, QStringLiteral("image/png"), png.size(),
                                           digest(png), &error);
        staging.chunk(1, good, png, &error);
        const QString missing = QStringLiteral("0123456789abcdef0123456789abcdef");
        error.clear();
        check(staging.take(1, {good, missing}, images, &error).isEmpty() &&
                  error == QStringLiteral("unknown_image"),
              "one missing image fails the whole prompt");
        check(staging.stagedCount(1) == 0, "a failed prompt clears what it named");
    }

    if (failures == 0)
        std::cout << "local control prompt images: all passed\n";
    return failures == 0 ? 0 : 1;
}
