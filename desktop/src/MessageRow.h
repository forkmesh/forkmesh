#pragma once

#include "ChatBackend.h"

#include <QFrame>
#include <QHash>
#include <QList>
#include <QMap>
#include <QPair>
#include <QPixmap>
#include <QString>
#include <QStringList>

class QDate;
class QDateTime;
class QEnterEvent;
class QHBoxLayout;
class QPushButton;
class QToolButton;
class QLabel;
class QVBoxLayout;

// What the summary row under a message says about its thread: how many replies,
// who wrote the newest one, and the faces of the people who replied (most
// recent first). MainWindow fills this in — it owns the avatar cache.
struct ThreadReplySummary {
    int count = 0;
    QString latestSender;
    QList<QPixmap> faces;

    bool isEmpty() const { return count <= 0; }
};

class MessageRow : public QFrame
{
    Q_OBJECT
public:
    // Faces shown before the pile starts crowding the message it hangs off,
    // and the side MainWindow should render each of them at.
    static constexpr int kThreadSummaryFaces = 3;
    static constexpr int kThreadSummaryFaceSide = 18;

    MessageRow(const ChatMessage &message, const QString &nameColor,
               const QHash<QString, MemberInfo> &mentionProfiles,
               bool canModerate = false, QWidget *parent = nullptr,
               const ThreadReplySummary &threadSummary = {},
               bool threadContext = false);

    QString messageId() const { return m_message.id; }
    QString senderId() const { return m_message.senderId; }
    QString senderName() const { return m_message.senderName; }
    qint64 timestampMs() const;
    static constexpr qint64 kContinuationWindowMs = 7 * 60 * 1000;
    static QString chatStampLabel(const QDateTime &sent);
    static QString chatDayLabel(const QDate &day);
    bool continues(const MessageRow *previous) const;
    void setContinuation(bool continuation);

    static QPixmap initialsAvatar(const QString &name, const QString &color,
                                  int side = 36);

    void setAvatar(const QPixmap &pixmap);
    // The sender's custom status emoji, painted right after the name, and
    // the hover text that explains it. Empty strings remove it.
    void setSenderStatus(const QString &emoji, const QString &tooltip);
    void setReactions(const QMap<QString, QStringList> &reactions);
    // A short delivery state painted after the time ("queued"), with the hover
    // text that explains it. An empty note removes it.
    void setDeliveryNote(const QString &note, const QString &tooltip);
    void setRecipientCount(int count, const QStringList &names = {});
    int recipientCount() const { return m_message.recipientCount; }
    void setReceiptMark(forkmesh::recipients::ReceiptMark mark);
    forkmesh::recipients::ReceiptMark receiptMark() const { return m_receiptMark; }
    static QPixmap receiptMarkPixmap(forkmesh::recipients::ReceiptMark mark,
                                     const QColor &muted, const QColor &accent,
                                     qreal dpr);

signals:
    void reactionToggled(const QString &messageId, const QString &emoji);
    void threadRequested(const QString &messageId);
    void editRequested(const QString &messageId, const QString &currentText);
    void createIssueRequested(const QString &text, const QString &senderName,
                              qint64 timestampMs);
    void sendToPromptRequested(const QString &text);
    void deleteRequested(const QString &messageId);
    void moderateDeleteRequested(const QString &messageId);
    void saveFileRequested(const QString &fileName, const QByteArray &data);
    void imageActivated(const QString &fileName, const QByteArray &data);
    void videoActivated(const QString &fileName, const QString &fileMime,
                        const QByteArray &data);
    void senderClicked(const QString &id, const QString &name);
    void mentionClicked(const QString &id, const QString &name);
    // A link written into the message text, already sanitized to a scheme the
    // browser may be handed.
    void linkActivated(const QString &url);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    void buildAttachment(QWidget *parent, QVBoxLayout *layout);
    void showReactionPicker();
    void showStickerPicker();
    void setStickers(const QList<QPair<QString, QStringList>> &placed);
    void placeStickers();
    void applyOuterMargins();
    QWidget *stickerAnchor() const;
    int idealBodyWidth() const;
    void renderSenderHeader();

    ChatMessage m_message;
    QString m_nameColor;
    QHash<QString, MemberInfo> m_mentionProfiles;
    QLabel *m_avatarLabel;
    QLabel *m_senderLabel = nullptr;
    QString m_senderStatusEmoji;
    QString m_senderStatusTooltip;
    QString m_deliveryNote;
    QString m_deliveryTooltip;
    QHBoxLayout *m_headerRow = nullptr;
    QHBoxLayout *m_outer = nullptr;
    QWidget *m_expiryRing = nullptr;
    QToolButton *m_menuButton = nullptr;
    QWidget *m_reactionsRow = nullptr;
    QPushButton *m_hoverReactButton = nullptr;
    QPushButton *m_hoverStickerButton = nullptr;
    QLabel *m_bodyLabel = nullptr;
    QList<QLabel *> m_stickerLabels;
    QHash<QObject *, QString> m_stickerValues;
    int m_reactionsBarFixed = 0;
    int m_stickerReserve = 0;
    int m_baseRightMargin = 0;
    mutable int m_idealWidthFor = -1;
    mutable int m_idealWidth = 0;
    bool m_continuation = false;
    QLabel *m_recipientsLabel = nullptr;
    QLabel *m_receiptLabel = nullptr;
    forkmesh::recipients::ReceiptMark m_receiptMark =
        forkmesh::recipients::ReceiptMark::None;
    QLabel *m_imageLabel = nullptr; // clickable inline image (opens full-size)
    QLabel *m_videoLabel = nullptr; // clickable video poster (opens the player)
    QHBoxLayout *m_reactionsBar;
};
