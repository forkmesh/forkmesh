#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

namespace forkmesh {

constexpr int kGitDefaultTimeoutMs = 8000;
constexpr int kGitMergeTimeoutMs = 120000;
constexpr int kGitIndexQueueWaitMs = 300000;
constexpr int kGitIndexQueueGuiOwnerWaitMs = 15000;
constexpr qint64 kGitLockStaleMs = 60000;
constexpr int kGitHeldIndexLockRetryWaitMs = 1500;
constexpr int kGitLockBackoffAttempts = 6;

QString gitSubcommand(const QStringList &args);
bool gitArgsWriteIndex(const QStringList &args);
bool gitArgsMergeLike(const QStringList &args);
int gitTimeoutMsForArgs(const QStringList &args,
                        int defaultMs = kGitDefaultTimeoutMs);

QString gitDirForCheckout(const QString &dir);
QString gitRepositoryKey(const QString &dir);

bool gitLockContentionError(const QString &err);
QString gitLockPathFromError(const QString &err);
bool gitIndexWriteRefused(const QString &err);

enum class GitLockState { Missing, Held, Fresh, Stale };

bool gitLockHeldByLiveProcess(const QString &lockPath);
GitLockState inspectGitLock(const QString &lockPath,
                            qint64 staleMs = kGitLockStaleMs);
bool removeGitLockIfStale(const QString &lockPath,
                          qint64 staleMs = kGitLockStaleMs);

struct GitLockRetry {
    bool retry = false;
    int waitMs = 0;
    QString removedLock;
};

class GitLockRetryPolicy
{
public:
    explicit GitLockRetryPolicy(const QString &dir = QString()) : m_dir(dir) {}
    GitLockRetry next(const QString &error);

private:
    QString m_dir;
    int m_backoff = 0;
    bool m_heldRetried = false;
    bool m_removedStale = false;
};

void waitBeforeGitRetry(int ms);

class GitIndexWriteGuard
{
public:
    GitIndexWriteGuard(const QString &dir, const QStringList &args);
    ~GitIndexWriteGuard();
    GitIndexWriteGuard(const GitIndexWriteGuard &) = delete;
    GitIndexWriteGuard &operator=(const GitIndexWriteGuard &) = delete;

    bool holdsLane() const { return m_holds; }
    bool bypassed() const { return m_bypassed; }
    QString repositoryKey() const { return m_key; }

    struct Lane;

private:
    std::shared_ptr<Lane> m_lane;
    QString m_key;
    bool m_holds = false;
    bool m_bypassed = false;
};

struct GitAttempt {
    bool ok = false;
    bool timedOut = false;
    QByteArray output;
    QString error;
};

bool gitIndexLaneHeldOnThisThread();

GitAttempt runGitIndexSerialized(const QString &dir, const QStringList &args,
                                 const std::function<GitAttempt()> &attempt);

} // namespace forkmesh
