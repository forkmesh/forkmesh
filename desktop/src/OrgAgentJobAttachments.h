#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace forkmesh::orgagentjobs {

constexpr int kMaxAttachments = 5;
constexpr qint64 kMaxAttachmentBytes = 5 << 20;

struct Attachment {
    QString id;
    QString mediaType;
    qint64 size = 0;
    QString sha256;
};

bool parseAttachments(const QJsonArray &raw, QList<Attachment> *out);

QString checkAttachment(const Attachment &attachment, const QByteArray &data);

QString checkImage(const QString &mediaType, qint64 size, const QByteArray &data);

QString saveAttachment(const QString &directory, const Attachment &attachment,
                       const QByteArray &data,
                       const QString &prefix = QStringLiteral("relay"));

QString promptWithAttachments(const QString &prompt, const QStringList &paths);

} // namespace forkmesh::orgagentjobs
