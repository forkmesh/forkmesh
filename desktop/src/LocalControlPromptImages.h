#pragma once

#include "OrgAgentJobAttachments.h"

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QString>
#include <QStringList>

namespace forkmesh::localcontrolimages {

constexpr int kMaxChunkBytes = 192 * 1024;
constexpr int kMaxStagedPerConnection = forkmesh::orgagentjobs::kMaxAttachments;

class Staging
{
public:
    QString begin(int connectionId, const QString &mediaType, qint64 size,
                  const QString &sha256, QString *error);
    qint64 chunk(int connectionId, const QString &imageId, const QByteArray &data,
                 QString *error);
    void cancel(int connectionId, const QString &imageId);
    void dropConnection(int connectionId);
    bool parseIds(const QJsonValue &raw, QStringList *ids) const;
    QStringList take(int connectionId, const QStringList &ids,
                     const QString &directory, QString *error);
    int stagedCount(int connectionId) const;

private:
    struct Staged {
        forkmesh::orgagentjobs::Attachment meta;
        QByteArray data;
    };
    QHash<int, QHash<QString, Staged>> m_staged;
};

} // namespace forkmesh::localcontrolimages
