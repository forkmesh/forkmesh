#include "OrgAgentJobAttachments.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUuid>

namespace forkmesh::orgagentjobs {

namespace {

QString extensionFor(const QString &mediaType)
{
    if (mediaType == QLatin1String("image/jpeg"))
        return QStringLiteral("jpg");
    if (mediaType == QLatin1String("image/png"))
        return QStringLiteral("png");
    if (mediaType == QLatin1String("image/webp"))
        return QStringLiteral("webp");
    if (mediaType == QLatin1String("image/gif"))
        return QStringLiteral("gif");
    return QString();
}

bool magicMatches(const QString &mediaType, const QByteArray &data)
{
    if (mediaType == QLatin1String("image/jpeg"))
        return data.startsWith(QByteArray("\xff\xd8\xff", 3));
    if (mediaType == QLatin1String("image/png"))
        return data.startsWith(QByteArray("\x89PNG\r\n\x1a\n", 8));
    if (mediaType == QLatin1String("image/gif"))
        return data.startsWith("GIF87a") || data.startsWith("GIF89a");
    if (mediaType == QLatin1String("image/webp"))
        return data.size() >= 12 && data.startsWith("RIFF") &&
               data.mid(8, 4) == "WEBP";
    return false;
}

} // namespace

bool parseAttachments(const QJsonArray &raw, QList<Attachment> *out)
{
    static const QRegularExpression idPattern(QStringLiteral("^[0-9a-f]{32}$"));
    static const QRegularExpression digestPattern(QStringLiteral("^[0-9a-f]{64}$"));
    QList<Attachment> parsed;
    if (raw.size() > kMaxAttachments)
        return false;
    for (const QJsonValue &value : raw) {
        if (!value.isObject())
            return false;
        const QJsonObject object = value.toObject();
        Attachment attachment;
        attachment.id = object.value(QStringLiteral("id")).toString();
        attachment.mediaType = object.value(QStringLiteral("mediaType")).toString();
        attachment.size = qint64(object.value(QStringLiteral("size")).toDouble());
        attachment.sha256 = object.value(QStringLiteral("sha256")).toString();
        if (!idPattern.match(attachment.id).hasMatch() ||
            extensionFor(attachment.mediaType).isEmpty() ||
            attachment.size <= 0 || attachment.size > kMaxAttachmentBytes ||
            !digestPattern.match(attachment.sha256).hasMatch())
            return false;
        parsed.append(attachment);
    }
    if (out)
        *out = parsed;
    return true;
}

QString checkAttachment(const Attachment &attachment, const QByteArray &data)
{
    if (data.size() != attachment.size)
        return QStringLiteral("the downloaded image has the wrong size");
    if (QString::fromLatin1(
            QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()) !=
        attachment.sha256)
        return QStringLiteral("the downloaded image does not match its digest");
    return checkImage(attachment.mediaType, attachment.size, data);
}

QString checkImage(const QString &mediaType, qint64 size, const QByteArray &data)
{
    if (data.size() != size)
        return QStringLiteral("the downloaded image has the wrong size");
    if (!magicMatches(mediaType, data))
        return QStringLiteral("the downloaded file is not the image type it claims to be");
    return QString();
}

QString saveAttachment(const QString &directory, const Attachment &attachment,
                       const QByteArray &data, const QString &prefix)
{
    const QString extension = extensionFor(attachment.mediaType);
    if (directory.isEmpty() || extension.isEmpty() || !QDir().mkpath(directory))
        return QString();
    const QString path = QDir(directory).filePath(
        QStringLiteral("%1-%2.%3")
            .arg(prefix, QUuid::createUuid().toString(QUuid::WithoutBraces), extension));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return QString();
    const bool written = file.write(data) == data.size();
    file.close();
    if (!written) {
        QFile::remove(path);
        return QString();
    }
    return path;
}

QString promptWithAttachments(const QString &prompt, const QStringList &paths)
{
    QString out = prompt;
    for (const QString &path : paths) {
        if (!out.endsWith(QLatin1Char('\n')))
            out += QLatin1Char('\n');
        out += QStringLiteral("Attached image: ") + path;
    }
    return out;
}

} // namespace forkmesh::orgagentjobs
