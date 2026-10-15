#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <functional>

class QLocalServer;
class QLocalSocket;

namespace forkmesh::agentintake {

inline const QString SocketEnv = QStringLiteral("FORKMESH_AGENT_INTAKE");
inline const QString TokenEnv = QStringLiteral("FORKMESH_AGENT_INTAKE_TOKEN");
inline const QString CreateMethod = QStringLiteral("agents.create");
inline const QString ReleaseMethodPrefix = QStringLiteral("releases.");
inline const QString ReleaseListMethod = QStringLiteral("releases.list");
inline const QString ReleaseCreateMethod = QStringLiteral("releases.create");
inline const QString ReleasePublishMethod = QStringLiteral("releases.publish");
inline const QString WindowMethodPrefix = QStringLiteral("windows.");

constexpr qsizetype kMaxRequestBytes = 64 * 1024;
constexpr int kMaxPromptChars = 8000;
constexpr int kMaxTitleChars = 200;
constexpr int kMaxModelChars = 100;
constexpr int kMaxProviderChars = 40;
constexpr int kMaxPathChars = 4096;
constexpr int kRateLimitCount = 12;
constexpr qint64 kRateWindowMs = 10 * 60 * 1000;
constexpr int kRequestTimeoutMs = 10000;
constexpr int kReleaseRateLimitCount = 6;
constexpr int kMaxReleaseTitleChars = 200;
constexpr int kMaxReleaseNotesChars = 20000;
constexpr int kMaxReleaseListCount = 50;

struct CreateRequest {
    QString owner;
    QString name;
    QString prompt;
    QString title;
    QString branch;
    QString worktree;
    QString provider;
    QString model;
    QString task;
    bool handoff = false;
    bool paused = false;
    bool hasPriority = false;
    int priority = 50;
};

struct ReleaseRequest {
    QString method;
    QString owner;
    QString name;
    QString tag;
    QString target;
    QString title;
    QString notes;
    bool hasRollout = false;
    bool rollout = false;
    int count = 10;
};

bool repositorySlugValid(const QString &slug, QString *owner, QString *name);
bool modelIdValid(const QString &model);
bool providerIdValid(const QString &provider);
bool worktreePathValid(const QString &path);
bool taskIdValid(const QString &taskId);
bool parseCreateRequest(const QJsonObject &params, CreateRequest *out,
                        QString *error);
bool releaseTagValid(const QString &tag);
bool parseReleaseRequest(const QString &method, const QJsonObject &params,
                         ReleaseRequest *out, QString *error);

QString mintToken(int sessionId);
int tokenSessionId(const QString &token);
bool tokenMatches(const QString &expected, const QString &presented);
int authorizedSession(const QHash<int, QString> &tokens, const QString &presented);

class RateLimiter
{
public:
    explicit RateLimiter(int count = kRateLimitCount, qint64 windowMs = kRateWindowMs)
        : m_count(count), m_windowMs(windowMs)
    {
    }
    bool allow(qint64 nowMs);
    int used(qint64 nowMs);

private:
    int m_count;
    qint64 m_windowMs;
    QList<qint64> m_times;
};

QJsonObject errorReply(const QString &code, const QString &message);
QJsonObject unauthorizedReply(const QString &method);

QString socketDirectory();

struct Request {
    QString token;
    QString method;
    QJsonObject params;
};

class Server
{
public:
    using Handler = std::function<QJsonObject(const Request &)>;
    using Reply = std::function<void(const QJsonObject &)>;
    using AsyncHandler = std::function<void(const Request &, Reply)>;

    Server();
    ~Server();

    bool listen(QString *error = nullptr);
    QString socketPath() const;
    void setHandler(Handler handler) { m_handler = std::move(handler); }
    void setAsyncHandler(AsyncHandler handler) { m_asyncHandler = std::move(handler); }

private:
    void accept();
    void readRequest(QLocalSocket *socket);
    static void writeReply(QLocalSocket *socket, const QJsonObject &reply);

    QLocalServer *m_server = nullptr;
    Handler m_handler;
    AsyncHandler m_asyncHandler;
};

}
