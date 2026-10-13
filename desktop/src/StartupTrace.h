#pragma once

#include <QDebug>
#include <QElapsedTimer>
#include <QString>

#include <utility>

namespace forkmesh {

inline QElapsedTimer &startupTraceClock()
{
    static QElapsedTimer clock;
    return clock;
}

inline void beginStartupTrace()
{
    if (!startupTraceClock().isValid())
        startupTraceClock().start();
}

inline void logStartupTrace(const QString &message)
{
    beginStartupTrace();
    qInfo().noquote() << QStringLiteral("[startup +%1ms] %2")
                             .arg(startupTraceClock().elapsed(), 5)
                             .arg(message);
}

class StartupTraceStep
{
public:
    explicit StartupTraceStep(QString name) : m_name(std::move(name))
    {
        m_elapsed.start();
        logStartupTrace(QStringLiteral("BEGIN %1").arg(m_name));
    }

    ~StartupTraceStep()
    {
        logStartupTrace(QStringLiteral("DONE  %1 (%2ms)")
                            .arg(m_name)
                            .arg(m_elapsed.elapsed()));
    }

    StartupTraceStep(const StartupTraceStep &) = delete;
    StartupTraceStep &operator=(const StartupTraceStep &) = delete;

private:
    QString m_name;
    QElapsedTimer m_elapsed;
};

} // namespace forkmesh
