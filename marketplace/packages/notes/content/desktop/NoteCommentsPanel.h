#pragma once

#include "NoteComments.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QWidget>

#include <functional>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QTimer;
class QVBoxLayout;

namespace forkmesh::notes {

class NoteCommentsPanel : public QWidget
{
public:
    using Reply = std::function<void(bool, const QJsonObject &, const QString &)>;
    struct Hooks {
        std::function<void(const QByteArray &, const QString &,
                           const QJsonObject &, Reply)>
            request;
        std::function<QString()> markdown;
        std::function<QString()> me;
        std::function<bool()> noteOwner;
        std::function<void(const CommentThread *)> showPassage;
        std::function<void()> countChanged;
        std::function<void()> newComment;
        std::function<void()> closed;
    };

    explicit NoteCommentsPanel(Hooks hooks, QWidget *parent = nullptr);

    void setNote(const QString &noteId);
    QString noteId() const { return m_noteId; }
    void reload();
    void markdownChanged();
    void compose(const CommentQuote &quote);
    void showThread(const QString &commentId);
    void clearActive();
    int openCount() const { return m_openCount; }
    bool loaded() const { return m_loaded; }

private:
    void apply(const QJsonObject &payload);
    void render();
    QWidget *threadCard(const CommentThread &thread);
    QWidget *commentBlock(const QJsonObject &comment, bool root);
    QPlainTextEdit *commentBox(const QString &placeholder, const QString &key,
                               std::function<void()> submit);
    void send(const QByteArray &method, const QString &path,
              const QJsonObject &body, const QString &errorKey,
              std::function<void(const QJsonObject &)> done = {});
    void submitThread();
    void submitReply(const QString &threadId);
    void submitEdit(const QString &commentId);
    void removeComment(const QJsonObject &comment);
    void setResolved(const QString &threadId, bool resolved);
    void setStatus(const QString &text);
    void activate(const QString &threadId, bool reveal);
    void restyleCards();
    void closeComposer();
    QString threadIdFor(const QString &commentId) const;
    QString commentsPath(const QString &commentId = QString()) const;

    Hooks m_hooks;
    QString m_noteId;
    QJsonArray m_comments;
    QStringList m_people;
    bool m_canResolve = false;
    bool m_loaded = false;
    bool m_busy = false;
    int m_sequence = 0;
    int m_openCount = 0;
    QString m_signature;
    QString m_activeId;
    QString m_pendingId;
    QString m_editingId;
    bool m_showResolved = false;
    QHash<QString, QString> m_drafts;
    QHash<QString, QString> m_errors;
    CommentQuote m_composeQuote;

    QLabel *m_status = nullptr;
    QWidget *m_composer = nullptr;
    QLabel *m_composeQuoteLabel = nullptr;
    QPlainTextEdit *m_composeBox = nullptr;
    QLabel *m_composeError = nullptr;
    QPushButton *m_composeSend = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_list = nullptr;
    QVBoxLayout *m_listLayout = nullptr;
    QTimer *m_anchorTimer = nullptr;
    QHash<QString, QWidget *> m_cards;
    QHash<QString, QPlainTextEdit *> m_boxes;
};

}
