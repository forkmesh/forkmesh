#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>

namespace forkmesh::keepawake {

inline constexpr int kRetryMs = 60000;

class SleepInhibitor : public QObject
{
public:
    SleepInhibitor(std::function<bool()> wanted, QObject *parent);
    ~SleepInhibitor() override;

    bool held() const;

private:
    void update();
    void take();
    void release();

    struct Lock;
    std::function<bool()> m_wanted;
    std::unique_ptr<Lock> m_lock;
    QTimer m_timer;
    bool m_pending = false;
    QString m_lastError;
};

void holdWhileRunning(QObject *owner);

} // namespace forkmesh::keepawake
