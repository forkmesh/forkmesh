#include "OrgAgentJobAttachments.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QTemporaryDir>

#include <iostream>

using namespace forkmesh::orgagentjobs;

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

static QJsonObject meta(const QString &id, const QString &mediaType,
                        const QByteArray &data)
{
    return QJsonObject{{QStringLiteral("id"), id},
                       {QStringLiteral("name"), QStringLiteral("photo.jpg")},
                       {QStringLiteral("mediaType"), mediaType},
                       {QStringLiteral("size"), double(data.size())},
                       {QStringLiteral("sha256"), digest(data)}};
}

int main()
{
    const QString id = QStringLiteral("0123456789abcdef0123456789abcdef");
    const QByteArray jpeg = QByteArray("\xff\xd8\xff\xe0", 4) + QByteArray(60, '\0');
    const QByteArray png = QByteArray("\x89PNG\r\n\x1a\n", 8) + QByteArray(60, '\0');

    QList<Attachment> parsed;
    check(parseAttachments(QJsonArray{}, &parsed) && parsed.isEmpty(),
          "an empty list parses");
    check(parseAttachments(QJsonArray{meta(id, QStringLiteral("image/jpeg"), jpeg)}, &parsed) &&
              parsed.size() == 1 && parsed.first().id == id &&
              parsed.first().size == jpeg.size(),
          "a relay job attachment parses");

    QJsonArray six;
    for (int i = 0; i < 6; ++i)
        six.append(meta(id, QStringLiteral("image/jpeg"), jpeg));
    check(!parseAttachments(six, nullptr), "more than five images are refused");
    check(!parseAttachments(QJsonArray{meta(QStringLiteral("../x"), QStringLiteral("image/jpeg"), jpeg)}, nullptr),
          "a malformed id is refused");
    check(!parseAttachments(QJsonArray{meta(id, QStringLiteral("image/svg+xml"), jpeg)}, nullptr),
          "an unsupported media type is refused");
    QJsonObject huge = meta(id, QStringLiteral("image/png"), png);
    huge.insert(QStringLiteral("size"), double(kMaxAttachmentBytes + 1));
    check(!parseAttachments(QJsonArray{huge}, nullptr), "an oversized image is refused");
    QJsonObject noDigest = meta(id, QStringLiteral("image/png"), png);
    noDigest.remove(QStringLiteral("sha256"));
    check(!parseAttachments(QJsonArray{noDigest}, nullptr), "an image without a digest is refused");
    check(!parseAttachments(QJsonArray{QStringLiteral("x")}, nullptr), "a non-object entry is refused");

    parseAttachments(QJsonArray{meta(id, QStringLiteral("image/jpeg"), jpeg)}, &parsed);
    const Attachment good = parsed.first();
    check(checkAttachment(good, jpeg).isEmpty(), "matching bytes pass");
    check(!checkAttachment(good, jpeg.left(10)).isEmpty(), "a short download fails");
    QByteArray tampered = jpeg;
    tampered[10] = 'x';
    check(!checkAttachment(good, tampered).isEmpty(), "a digest mismatch fails");
    Attachment liar = good;
    liar.mediaType = QStringLiteral("image/png");
    check(!checkAttachment(liar, jpeg).isEmpty(), "bytes of another image type fail");

    QTemporaryDir dir;
    check(dir.isValid(), "temp dir");
    const QString target = dir.filePath(QStringLiteral("agent-images"));
    const QString first = saveAttachment(target, good, jpeg);
    const QString second = saveAttachment(target, good, jpeg);
    check(!first.isEmpty() && first.endsWith(QStringLiteral(".jpg")), "an image is saved as .jpg");
    check(first != second, "each save gets its own file");
    check(QFileInfo(first).absolutePath() == QFileInfo(target).absoluteFilePath(),
          "the image lands in the given folder");
    QFile saved(first);
    check(saved.open(QIODevice::ReadOnly) && saved.readAll() == jpeg, "the saved bytes match");
    check(saveAttachment(QString(), good, jpeg).isEmpty(), "no folder, no file");

    check(promptWithAttachments(QStringLiteral("fix it"), {}) == QStringLiteral("fix it"),
          "no images leave the prompt alone");
    check(promptWithAttachments(QStringLiteral("fix it"),
                                {QStringLiteral("/a/1.jpg"), QStringLiteral("/a/2.png")}) ==
              QStringLiteral("fix it\nAttached image: /a/1.jpg\nAttached image: /a/2.png"),
          "each image gets an Attached image line");
    check(promptWithAttachments(QStringLiteral("fix it\n"), {QStringLiteral("/a/1.jpg")}) ==
              QStringLiteral("fix it\nAttached image: /a/1.jpg"),
          "a trailing newline is not doubled");

    if (failures)
        return 1;
    std::cout << "org agent job attachment tests passed\n";
    return 0;
}
