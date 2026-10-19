#pragma once

#include "AgentJsonlFramer.h"

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

class QProcess;
class QTimer;

class CodexRateLimitsProbe : public QObject
{
    Q_OBJECT
public:
    static constexpr int DefaultTimeoutMs = 15000;

    explicit CodexRateLimitsProbe(QObject *parent = nullptr);
    ~CodexRateLimitsProbe() override;

    void setAppServerCommand(const QString &program,
                             const QStringList &arguments = QStringList());
    void start(const QString &codexHome, int timeoutMs = DefaultTimeoutMs);
    bool running() const;

    static QJsonObject withLegacyLimit(const QJsonObject &rateLimits,
                                       const QJsonObject &rateLimitsByLimitId);

signals:
    void finished(bool ok, const QJsonObject &rateLimits,
                  const QJsonObject &rateLimitsByLimitId, const QString &error);

private:
    void onStarted();
    void onStdout(bool flushRemainder);
    void handleMessage(const QJsonObject &message);
    void send(const QJsonObject &message);
    void finish(bool ok, const QJsonObject &rateLimits,
                const QJsonObject &rateLimitsByLimitId, const QString &error);
    void releaseProcess();

    QProcess *m_proc = nullptr;
    QTimer *m_timeout = nullptr;
    AgentJsonlFramer m_framer;
    QString m_program;
    QStringList m_arguments;
    bool m_done = false;
};
