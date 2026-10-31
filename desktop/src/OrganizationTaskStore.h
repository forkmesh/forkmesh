#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>

class QNetworkAccessManager;
class QNetworkRequest;

namespace forkmesh::tasks {

using TaskReplyHandler =
    std::function<void(bool, const QJsonObject &, const QString &)>;
using TaskAttachmentHandler = std::function<void(
    bool, const QByteArray &, const QString &, const QString &)>;

inline constexpr int kTaskTitleLimit = 160;
inline const QString kTaskBackoffKey = QStringLiteral("_backoffMs");

QString taskText(const QJsonObject &task, const QString &field);
bool taskIsClosed(const QJsonObject &task);
int openTaskCount(const QJsonArray &tasks);
QString taskBlockedText(const QJsonObject &payload);
QJsonArray taskImageAttachments(const QStringList &paths, QString *error);
QByteArray taskImagePreviewBytes(const QString &path, QString *mime);
QString taskImageMimeForPath(const QString &path);
bool taskImageMimeAllowed(const QString &mime);

struct TaskTransport {
    QNetworkAccessManager *network = nullptr;
    std::function<QUrl()> apiUrl;
    std::function<QString(const QUrl &)> sessionToken;
    std::function<bool(QUrl &, QNetworkRequest &, const QString &,
                       const QString &)>
        authenticate;
    std::function<bool()> accountAuthenticated;
    std::function<void(const QString &)> log;
};

class OrganizationTaskStore : public QObject
{
    Q_OBJECT

public:
    explicit OrganizationTaskStore(QObject *parent = nullptr);
    ~OrganizationTaskStore() override;

    static OrganizationTaskStore *current();

    void setTransport(const TaskTransport &transport);
    void setCatalogHook(std::function<void(const QJsonArray &, bool)> hook);

    void request(const QByteArray &method, const QString &path,
                 const QJsonObject &body, TaskReplyHandler handler,
                 const QList<int> &expectedHttpStatuses = {});
    void requestAttachment(const QString &taskId, const QString &attachmentId,
                           TaskAttachmentHandler handler);
    void requestCatalog(TaskReplyHandler handler);

    void refresh();
    void applyPayload(const QJsonObject &payload);
    void refreshOnce();
    void handleEvent();
    void reset();

    bool loading() const { return m_loading; }
    bool loaded() const { return m_loaded; }
    qint64 loadedAt() const { return m_loadedAt; }
    QJsonObject payload() const { return m_payload; }
    QJsonArray tasks() const;
    QString actor() const;
    QStringList members() const;
    bool canManage() const;

    void replaceTask(const QJsonObject &task);
    void updateTaskField(const QString &taskId, const QString &field,
                         const QJsonValue &value);
    void removeTask(const QString &taskId);

    int badgeCount() const { return m_badgeCount; }
    void setBadgeCount(int openCount);

signals:
    void loadingChanged();
    void catalogLoaded(const QJsonObject &payload);
    void catalogFailed(const QString &error);
    void taskChanged(const QJsonObject &task);

private:
    void requestCatalogPage(int offset, QJsonArray catalog,
                            QJsonObject firstPage, TaskReplyHandler handler);
    void setLoading(bool loading);
    void drainPendingEvent();

    TaskTransport m_transport;
    std::function<void(const QJsonArray &, bool)> m_catalogHook;
    QJsonObject m_payload;
    bool m_loading = false;
    bool m_loaded = false;
    bool m_eventPending = false;
    qint64 m_loadedAt = 0;
    int m_badgeCount = 0;
};

}
