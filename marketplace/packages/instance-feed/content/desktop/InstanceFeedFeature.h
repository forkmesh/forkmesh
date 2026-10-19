#pragma once

#include "NativePackageExtension.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>

#include <functional>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace forkmesh::instancefeed {

class InstanceFeedFeature final : public forkmesh::market::NativePackageWidget
{
public:
    using ReplyHandler =
        std::function<void(bool, const QJsonObject &, const QString &)>;
    using Request = std::function<void(const QByteArray &, const QString &,
                                       const QJsonObject &, ReplyHandler)>;

    enum class View { Timeline, Thread, Profile };

    explicit InstanceFeedFeature(const forkmesh::market::NativePackageHost &host,
                                 QWidget *parent = nullptr);
    void refresh() override;
    bool exportData(forkmesh::market::NativePackageReplyHandler done) override;
    void openRailTile(const QString &id) override;
    bool openLinkTarget(const QString &id);

    void showTimeline();
    void showThread(const QString &postId);
    void showProfile(const QString &author);
    View view() const { return m_view; }

    static QString richText(const QString &text, const QString &linkColor);
    static QString relativeLabel(qint64 createdAtMs, qint64 nowMs);
    static bool validPostId(const QString &id);
    static bool validAuthor(const QString &name);
    static QString timelinePath(View view, const QString &author, const QString &before);
    static bool exportHasData(const QJsonObject &payload);

private:
    void request(const QByteArray &method, const QString &path,
                 const QJsonObject &body, ReplyHandler handler);
    void load(bool append = false);
    void render();
    void renderProfile();
    void renderCompose();
    void clearList();
    QWidget *postRow(const QJsonObject &post, const QString &boostedBy,
                     const QString &boosterName, bool focused);
    QPushButton *actionButton(const QString &label, int count, bool active,
                              const QString &activeColor);
    void openLink(const QString &link);
    void submit();
    void startReply(const QJsonObject &post);
    void toggle(const QString &postId, const QString &kind, bool active);
    void deletePost(const QString &postId);
    void replacePost(const QJsonObject &post);
    void exportMine();
    void setStatus(const QString &text, bool error = false);
    bool dark() const;

    Request m_request;
    QLabel *m_status = nullptr;
    QLabel *m_notInstalled = nullptr;
    QWidget *m_content = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_exportButton = nullptr;
    QWidget *m_replyBar = nullptr;
    QLabel *m_replyLabel = nullptr;
    QLineEdit *m_spoiler = nullptr;
    QPushButton *m_spoilerToggle = nullptr;
    QPlainTextEdit *m_text = nullptr;
    QLabel *m_remaining = nullptr;
    QPushButton *m_postButton = nullptr;
    QPushButton *m_backButton = nullptr;
    QLabel *m_profile = nullptr;
    QScrollArea *m_scroll = nullptr;
    QVBoxLayout *m_list = nullptr;
    QPushButton *m_moreButton = nullptr;
    View m_view = View::Timeline;
    QString m_threadId;
    QString m_author;
    QString m_next;
    QString m_replyTo;
    QString m_replyAuthor;
    QJsonArray m_posts;
    QJsonObject m_thread;
    QJsonObject m_profileData;
    QSet<QString> m_revealed;
    int m_maxLength = 500;
    int m_generation = 0;
    bool m_checked = false;
    bool m_installed = false;
    bool m_loading = false;
};

} // namespace forkmesh::instancefeed
