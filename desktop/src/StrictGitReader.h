#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QString>
#include <QStringList>

#include <optional>

namespace StrictGitReadInternal {

struct Limits {
    qsizetype maxStdoutBytes = 32 * 1024 * 1024;
    qsizetype maxStderrBytes = 16 * 1024;
    qsizetype maxTotalBytes = 64 * 1024 * 1024;
    int maxGitCommands = 8192;
    int maxPullItems = 1024;
    int maxEventItems = 16384;
    int maxElapsedMs = 30000;
};

bool appendBounded(QByteArray *destination, const QByteArray &chunk,
                   qsizetype maxDestinationBytes,
                   qsizetype *remainingTotalBytes);

class ScopedLimitsForTests
{
public:
    explicit ScopedLimitsForTests(const Limits &limits);
    ~ScopedLimitsForTests();

    ScopedLimitsForTests(const ScopedLimitsForTests &) = delete;
    ScopedLimitsForTests &operator=(const ScopedLimitsForTests &) = delete;

private:
    std::optional<Limits> m_previous;
};

class Reader
{
public:
    Reader();

    bool run(const QString &dir, const QStringList &args,
             QByteArray *output = nullptr, QString *error = nullptr,
             int timeoutMs = 15000);
    bool runInput(const QString &dir, const QStringList &args,
                  const QByteArray &input, QByteArray *output = nullptr,
                  QString *error = nullptr, int timeoutMs = 15000);

    bool overflowed() const { return m_overflowed; }
    qsizetype remainingBytes() const { return m_remainingBytes; }
    bool consumePullItem();
    bool consumeEventItem();

private:
    bool runInternal(const QString &dir, const QStringList &args,
                     const QByteArray *input, QByteArray *output,
                     QString *error, int timeoutMs);

    Limits m_limits;
    qsizetype m_remainingBytes = 0;
    int m_remainingCommands = 0;
    int m_remainingPullItems = 0;
    int m_remainingEventItems = 0;
    QElapsedTimer m_elapsed;
    bool m_overflowed = false;
};

} // namespace StrictGitReadInternal
