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
class QShowEvent;
class QStackedWidget;
class QTextBrowser;
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
        std::function<void()> chatChanged;
        std::function<int()> currentVersion;
        std::function<bool()> canEdit;
        std::function<void(int)> restoreVersion;
        std::function<void(const NoteReactionGroup &)> selectPassage;
        std::function<void()> reactionsChanged;
        std::function<void(const QString &)> failed;
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
    void showTab(NoteSideTab tab);
    void showTab(bool chat) { showTab(chat ? NoteSideTab::Chat : NoteSideTab::Comments); }
    NoteSideTab tab() const { return m_tab; }
    bool chatShown() const { return m_chatTab; }
    int chatUnread() const { return m_chatUnread; }
    void reloadChat();
    void focusChat();
    void reloadActivity();
    QList<NoteReactionGroup> reactionGroups() const;
    void react(const QString &emoji, const CommentQuote &quote);

protected:
    void showEvent(QShowEvent *event) override;

private:
    QWidget *buildActivityPage();
    QWidget *buildHistoryPage();
    void loadShownTab();
    void loadActivity();
    void renderActivity();
    void loadHistory();
    void renderHistory();
    void showVersion(int version);
    void renderVersion();
    void closeVersion();
    void restoreVersion(int version);
    void setActivityStatus(const QString &text);
    void setHistoryStatus(const QString &text);
    void applyChat(const QJsonObject &payload);
    void renderChat();
    void sendChat();
    void deleteChat(const QString &messageId);
    void markChatSeen();
    void setChatStatus(const QString &text);
    QString chatPath(const QString &messageId = QString()) const;
    void apply(const QJsonObject &payload);
    void render();
    QWidget *threadCard(const CommentThread &thread);
    QWidget *reactionsSection(const QList<NoteReactionGroup> &groups);
    QWidget *reactionGroupCard(const NoteReactionGroup &group);
    void showReactionMenu(QWidget *below, const NoteReaction &like);
    void sendReaction(const QByteArray &method, const QString &path,
                      const QJsonObject &body);
    QString reactionsPath(const QString &reactionId = QString()) const;
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
    QList<NoteReaction> m_reactions;
    QString m_reactionSignature;
    bool m_reactionBusy = false;
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

    QList<NoteChatMessage> m_chat;
    bool m_chatTab = false;
    bool m_chatLoaded = false;
    bool m_chatModerate = false;
    bool m_chatBusy = false;
    int m_chatSequence = 0;
    int m_chatUnread = 0;
    QPushButton *m_commentsTabButton = nullptr;
    QPushButton *m_chatTabButton = nullptr;
    QPushButton *m_newButton = nullptr;
    QStackedWidget *m_pages = nullptr;
    QLabel *m_chatStatus = nullptr;
    QScrollArea *m_chatScroll = nullptr;
    QWidget *m_chatList = nullptr;
    QVBoxLayout *m_chatListLayout = nullptr;
    QPlainTextEdit *m_chatBox = nullptr;
    QPushButton *m_chatSend = nullptr;

    NoteSideTab m_tab = NoteSideTab::Comments;
    QPushButton *m_activityTabButton = nullptr;
    QPushButton *m_historyTabButton = nullptr;
    QList<NoteActivityEntry> m_activity;
    bool m_activityLoaded = false;
    bool m_activityStale = true;
    int m_activitySequence = 0;
    QLabel *m_activityStatus = nullptr;
    QScrollArea *m_activityScroll = nullptr;
    QVBoxLayout *m_activityListLayout = nullptr;
    QList<NoteVersionEntry> m_versions;
    bool m_historyLoaded = false;
    bool m_historyStale = true;
    int m_historySequence = 0;
    int m_versionSequence = 0;
    QJsonObject m_versionShown;
    QLabel *m_historyStatus = nullptr;
    QStackedWidget *m_historyPages = nullptr;
    QScrollArea *m_historyScroll = nullptr;
    QVBoxLayout *m_historyListLayout = nullptr;
    QLabel *m_versionHeading = nullptr;
    QPushButton *m_versionRestore = nullptr;
    QTextBrowser *m_versionBody = nullptr;
};

}
