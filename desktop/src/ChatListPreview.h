#pragma once

#include "ChatBackend.h"
#include "ChatBlocks.h"
#include "ChatFormat.h"

#include <QDate>
#include <QDateTime>
#include <QList>
#include <QString>

namespace forkmesh::chatlist {

inline constexpr int kMaxPreviewSourceChars = 600;
inline constexpr int kMaxPreviewChars = 200;

struct Preview {
    QString text;
    qint64 timestampMs = 0;

    bool isEmpty() const { return text.isEmpty(); }
    bool operator==(const Preview &other) const
    {
        return text == other.text && timestampMs == other.timestampMs;
    }
};

inline bool countsForPreview(const ChatMessage &message)
{
    if (message.deleted)
        return false;
    if (!message.threadRootId.isEmpty() &&
        !message.conversation.startsWith(QStringLiteral("#discord/")))
        return false;
    return !message.text.trimmed().isEmpty() || message.hasFile() ||
           !message.blocks.isEmpty();
}

inline QString previewBody(const ChatMessage &message)
{
    const QString formatted =
        !message.blocks.isEmpty() &&
                forkmesh::chatblocks::senderMayFormat(message.senderId)
            ? forkmesh::chatblocks::plainSummary(message.blocks)
            : forkmesh::chatformat::toPlainText(
                  message.text.left(kMaxPreviewSourceChars));
    const QString plain = formatted.simplified();
    if (!plain.isEmpty())
        return plain.left(kMaxPreviewChars);
    if (message.hasFile())
        return (QStringLiteral("\U0001F4CE ") + message.fileName.simplified())
            .left(kMaxPreviewChars);
    return QString();
}

inline Preview preview(const QList<ChatMessage> &history, bool direct,
                       const QString &ownName)
{
    const QString own = ownName.trimmed();
    for (qsizetype index = history.size() - 1; index >= 0; --index) {
        const ChatMessage &message = history.at(index);
        if (!countsForPreview(message))
            continue;
        const QString body = previewBody(message);
        if (body.isEmpty())
            continue;
        const QString sender = message.senderName.simplified();
        const bool self =
            message.self ||
            (!own.isEmpty() && sender.compare(own, Qt::CaseInsensitive) == 0);
        Preview out;
        out.timestampMs = message.timestampMs;
        if (self)
            out.text = QStringLiteral("You: ") + body;
        else if (direct || sender.isEmpty())
            out.text = body;
        else
            out.text = sender + QStringLiteral(": ") + body;
        return out;
    }
    return Preview();
}

inline QString timeLabel(qint64 timestampMs, const QDateTime &now)
{
    if (timestampMs <= 0)
        return QString();
    const QDateTime sent = QDateTime::fromMSecsSinceEpoch(timestampMs);
    const QDate today = now.date();
    const QDate day = sent.date();
    if (day >= today)
        return sent.toString(QStringLiteral("h:mm AP"));
    if (day > today.addDays(-7))
        return sent.toString(QStringLiteral("ddd"));
    if (day.year() == today.year())
        return sent.toString(QStringLiteral("MMM d"));
    return sent.toString(QStringLiteral("M/d/yy"));
}

}
