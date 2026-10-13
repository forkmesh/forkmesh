#pragma once

#include <QPointer>
#include <QUrl>
#include <QWidget>

class QLabel;
class QNetworkAccessManager;
class QNetworkReply;
class QPushButton;
class QVBoxLayout;

// Read-only projection of remote ActivityPub replies. This widget never
// creates IssueEvent/PullEvent/DiscussionEvent records and never writes to a
// repository's signed native event log. It is presented as its own section in
// the thread: a titled card that links out to the public thread and to the
// repository's fediverse handle, so a reader can pick the conversation up on
// their own instance.
class FederatedThreadView final : public QWidget
{
public:
    explicit FederatedThreadView(QNetworkAccessManager *network,
                                 QWidget *parent = nullptr);

    void load(const QUrl &server, const QString &owner, const QString &repo,
              const QString &kind, int number);

private:
    void showMessage(const QString &message);
    void render(const QByteArray &payload);
    void clearReplies();
    void setSummary(const QString &summary);
    static void openHttps(const QUrl &url);

    QPointer<QNetworkAccessManager> m_network;
    QPointer<QNetworkReply> m_reply;
    QVBoxLayout *m_replies = nullptr;
    QLabel *m_heading = nullptr;
    QLabel *m_threadLink = nullptr;
    QPushButton *m_copyHandle = nullptr;
    QUrl m_threadUrl;
    QString m_handle; // @owner.repo@instance
    quint64 m_generation = 0;
};
