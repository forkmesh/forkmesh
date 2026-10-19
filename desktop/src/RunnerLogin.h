#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace forkmesh::runners {

constexpr int kRunnerLoginMaxInputBytes = 4096;
constexpr int kRunnerLoginPollMs = 1000;

struct RunnerUsageWindow {
    QString id;
    QString label;
    double usedPercent = 0;
    qint64 resetsAtMs = 0;
};

struct RunnerAccountInfo {
    QString id;
    QString label;
    QString email;
    QString plan;
    bool active = false;
    bool usageRead = false;
    QList<RunnerUsageWindow> usage;
    QString usageError;
    qint64 usageFetchedAtMs = 0;

    QString displayName() const;
};

struct RunnerProviderState {
    QString provider;
    bool reported = false;
    bool binaryFound = false;
    QString loginState;
    QString source;
    QString authState;
    qint64 authExpiresAtMs = 0;
    QList<RunnerAccountInfo> accounts;

    bool signedIn() const;
    bool loginExpired() const;
    bool loginExpiring() const;
    const RunnerAccountInfo *activeAccount() const;
};

struct RunnerAccountRequestInfo {
    QString id;
    QString provider;
    QString action;
    QString account;
    QString state;
    QString message;
};

struct RunnerLoginInfo {
    QString id;
    QString provider;
    QString state;
    QString createdBy;
    QString message;
    qint64 createdAtMs = 0;
    qint64 expiresAtMs = 0;
    qint64 endedAtMs = 0;

    bool open() const;
};

struct RunnerInfo {
    QString name;
    QString scope;
    QString status;
    QString registeredBy;
    qint64 lastSeenAtMs = 0;
    qint64 availabilityAtMs = 0;
    int activeLeases = -1;
    QList<RunnerProviderState> providers;
    QList<RunnerLoginInfo> logins;
    QList<RunnerAccountRequestInfo> accountRequests;

    bool manageable() const;
    RunnerProviderState provider(const QString &provider) const;
    QString openLoginId(const QString &provider) const;
    bool accountRequestPending(const QString &provider,
                               const QString &account) const;
};

QStringList runnerProviders();
QString runnerProviderLabel(const QString &provider);
QString runnerProviderStateText(const RunnerProviderState &state);
QString runnerUsageWindowText(const RunnerUsageWindow &window, qint64 nowMs);
QString runnerAccountUsageText(const RunnerAccountInfo &account, qint64 nowMs);
QList<RunnerInfo> parseRunners(const QJsonObject &payload);
QString runnerForTarget(const QList<RunnerInfo> &runners,
                        const QString &targetNode, const QString &provider);

struct RunnerAttentionItem {
    QString runner;
    QString provider;
    QString state;
    QString message;
    qint64 expiresAtMs = 0;
    QString openLoginId;
    bool canConnect = true;

    bool expired() const;
};

struct RunnerAttentionBadge {
    bool visible = false;
    bool expired = false;
    QString text;
    QString toolTip;
};

QUrl runnerAttentionUrl(const QUrl &base);
QUrl runnerConnectPageUrl(const QUrl &base, const RunnerAttentionItem &item);
qint64 runnerAttentionPollMs(const QJsonObject &payload);
QList<RunnerAttentionItem> runnerAttentionFromRunners(
    const QList<RunnerInfo> &runners);
QList<RunnerAttentionItem> parseRunnerAttention(const QJsonObject &payload);
QList<RunnerAttentionItem> sortedRunnerAttention(
    const QList<RunnerAttentionItem> &items);
QString runnerAttentionTitle(const RunnerAttentionItem &item);
QString runnerAttentionDetail(const RunnerAttentionItem &item);
QString runnerAttentionActionText(const RunnerAttentionItem &item);
RunnerAttentionBadge runnerAttentionBadge(
    const QList<RunnerAttentionItem> &items);
bool runnerSignInLink(const QUrl &url, QString *runner, QString *provider);

bool runnerLoginStateOpen(const QString &state);
QString runnerLoginStateText(const QString &state);

QUrl runnersListUrl(const QUrl &base);
QUrl runnerLoginStartUrl(const QUrl &base, const QString &runner);
QUrl runnerLoginOutputUrl(const QUrl &base, const QString &runner,
                          const QString &id, qint64 afterSeq);
QUrl runnerLoginInputUrl(const QUrl &base, const QString &runner,
                         const QString &id);
QUrl runnerLoginCancelUrl(const QUrl &base, const QString &runner,
                          const QString &id);

QUrl runnerAccountsUrl(const QUrl &base, const QString &runner);

QByteArray runnerLoginStartBody(const QString &provider);
QByteArray runnerAccountRequestBody(const QString &provider,
                                    const QString &account,
                                    const QString &action);
bool runnerLoginInputValid(const QString &data);
QByteArray runnerLoginInputBody(const QString &data);
QString runnerLoginCodeSubmission(const QString &code);

QString applyRunnerLoginKeystrokes(const QString &line, const QByteArray &bytes,
                                   QStringList *submitted);

class RunnerLoginSession
{
public:
    enum class Start { Started, Attached, Failed };

    RunnerLoginSession() = default;
    RunnerLoginSession(const QString &runner, const QString &provider);

    Start applyStartReply(int httpStatus, const QJsonObject &body);
    QByteArray applyOutputReply(const QJsonObject &body);
    void applyCancelReply(const QJsonObject &body);
    void attach(const QString &id);
    void markFailed(const QString &message);

    QString runner() const { return m_runner; }
    QString provider() const { return m_provider; }
    QString id() const { return m_id; }
    QString state() const { return m_state; }
    QString message() const { return m_message; }
    QString error() const { return m_error; }
    qint64 afterSeq() const { return m_afterSeq; }
    bool more() const { return m_more; }
    bool open() const;
    bool needsCancelOnClose() const;

private:
    void applyPublic(const QJsonObject &body);

    QString m_runner;
    QString m_provider;
    QString m_id;
    QString m_state;
    QString m_message;
    QString m_error;
    qint64 m_afterSeq = 0;
    bool m_more = false;
    bool m_failed = false;
};

} // namespace forkmesh::runners
