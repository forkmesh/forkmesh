#include "LocalControlPromptImages.h"

#include <QFile>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUuid>

namespace forkmesh::localcontrolimages {

namespace jobattachments = forkmesh::orgagentjobs;

QString Staging::begin(int connectionId, const QString &mediaType, qint64 size,
                       const QString &sha256, QString *error)
{
    QHash<QString, Staged> &staged = m_staged[connectionId];
    if (staged.size() >= kMaxStagedPerConnection) {
        if (error)
            *error = QStringLiteral("too_many_images");
        return QString();
    }
    const QString id = QUuid::createUuid().toString(QUuid::Id128);
    QList<jobattachments::Attachment> parsed;
    const QJsonArray raw{QJsonObject{{QStringLiteral("id"), id},
                                     {QStringLiteral("mediaType"), mediaType},
                                     {QStringLiteral("size"), double(size)},
                                     {QStringLiteral("sha256"), sha256.toLower()}}};
    if (!jobattachments::parseAttachments(raw, &parsed) || parsed.size() != 1) {
        if (staged.isEmpty())
            m_staged.remove(connectionId);
        if (error)
            *error = QStringLiteral("bad_image");
        return QString();
    }
    Staged entry;
    entry.meta = parsed.first();
    entry.data.reserve(int(entry.meta.size));
    staged.insert(id, entry);
    return id;
}

qint64 Staging::chunk(int connectionId, const QString &imageId,
                      const QByteArray &data, QString *error)
{
    auto connection = m_staged.find(connectionId);
    if (connection == m_staged.end() || !connection->contains(imageId)) {
        if (error)
            *error = QStringLiteral("unknown_image");
        return -1;
    }
    Staged &entry = (*connection)[imageId];
    if (data.isEmpty() || data.size() > kMaxChunkBytes ||
        entry.data.size() + data.size() > entry.meta.size) {
        cancel(connectionId, imageId);
        if (error)
            *error = QStringLiteral("bad_chunk");
        return -1;
    }
    entry.data.append(data);
    return entry.data.size();
}

void Staging::cancel(int connectionId, const QString &imageId)
{
    auto connection = m_staged.find(connectionId);
    if (connection == m_staged.end())
        return;
    connection->remove(imageId);
    if (connection->isEmpty())
        m_staged.erase(connection);
}

void Staging::dropConnection(int connectionId)
{
    m_staged.remove(connectionId);
}

bool Staging::parseIds(const QJsonValue &raw, QStringList *ids) const
{
    static const QRegularExpression idPattern(QStringLiteral("^[0-9a-f]{32}$"));
    QStringList parsed;
    if (!raw.isUndefined() && !raw.isNull()) {
        if (!raw.isArray() || raw.toArray().size() > kMaxStagedPerConnection)
            return false;
        for (const QJsonValue &value : raw.toArray()) {
            const QString id = value.toString();
            if (!idPattern.match(id).hasMatch() || parsed.contains(id))
                return false;
            parsed.append(id);
        }
    }
    if (ids)
        *ids = parsed;
    return true;
}

QStringList Staging::take(int connectionId, const QStringList &ids,
                          const QString &directory, QString *error)
{
    if (ids.isEmpty())
        return {};
    auto connection = m_staged.find(connectionId);
    QList<Staged> taken;
    QString problem;
    for (const QString &id : ids) {
        if (connection == m_staged.end() || !connection->contains(id)) {
            problem = QStringLiteral("unknown_image");
            break;
        }
        const Staged entry = connection->value(id);
        if (entry.data.size() != entry.meta.size) {
            problem = QStringLiteral("image_incomplete");
            break;
        }
        if (!jobattachments::checkAttachment(entry.meta, entry.data).isEmpty()) {
            problem = QStringLiteral("bad_image");
            break;
        }
        taken.append(entry);
    }
    for (const QString &id : ids)
        cancel(connectionId, id);
    QStringList paths;
    if (problem.isEmpty()) {
        for (const Staged &entry : taken) {
            const QString path = jobattachments::saveAttachment(
                directory, entry.meta, entry.data, QStringLiteral("phone"));
            if (path.isEmpty()) {
                problem = QStringLiteral("cannot_save_image");
                break;
            }
            paths.append(path);
        }
    }
    if (!problem.isEmpty()) {
        for (const QString &path : paths)
            QFile::remove(path);
        if (error)
            *error = problem;
        return {};
    }
    return paths;
}

int Staging::stagedCount(int connectionId) const
{
    return m_staged.value(connectionId).size();
}

} // namespace forkmesh::localcontrolimages
