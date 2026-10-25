#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QQueue>
#include <QString>
#include <QThread>
#include <QWaitCondition>

#include <functional>
#include <utility>

namespace forkmesh {

class OrderedAppendWriter final : public QThread
{
public:
    enum class Kind { Event, Log, Remove, Barrier };

    struct Task {
        Kind kind = Kind::Event;
        QString path;
        QJsonObject event;
        QString text;
        QPointer<QObject> context;
        std::function<void()> callback;
        quint64 sequence = 0;
    };

    ~OrderedAppendWriter() override
    {
        {
            const QMutexLocker lock(&m_mutex);
            m_stopping = true;
            m_ready.wakeOne();
        }
        wait();
    }

    void appendEvent(const QString &path, const QJsonObject &event)
    {
        Task task;
        task.kind = Kind::Event;
        task.path = path;
        task.event = event;
        enqueue(std::move(task));
    }

    void appendLog(const QString &path, const QString &text)
    {
        Task task;
        task.kind = Kind::Log;
        task.path = path;
        task.text = text;
        enqueue(std::move(task));
    }

    void removeFile(const QString &path)
    {
        Task task;
        task.kind = Kind::Remove;
        task.path = path;
        enqueue(std::move(task));
    }

    void whenDrained(QObject *context, std::function<void()> callback)
    {
        if (!context || !callback)
            return;
        Task task;
        task.kind = Kind::Barrier;
        task.context = context;
        task.callback = std::move(callback);
        enqueue(std::move(task));
    }

    void flush()
    {
        if (QThread::currentThread() == this)
            return;
        QMutexLocker lock(&m_mutex);
        const quint64 target = m_nextSequence;
        while (m_completedSequence < target)
            m_drained.wait(&m_mutex);
    }

    void flushPath(const QString &path)
    {
        if (QThread::currentThread() == this)
            return;
        QMutexLocker lock(&m_mutex);
        const quint64 target = m_lastPathSequence.value(path, 0);
        while (m_completedSequence < target)
            m_drained.wait(&m_mutex);
    }

protected:
    void run() override
    {
        while (true) {
            Task task;
            {
                QMutexLocker lock(&m_mutex);
                while (m_tasks.isEmpty() && !m_stopping)
                    m_ready.wait(&m_mutex);
                if (m_tasks.isEmpty() && m_stopping)
                    break;
                task = std::move(m_tasks.dequeue());
            }

            perform(task);

            {
                const QMutexLocker lock(&m_mutex);
                m_completedSequence = task.sequence;
                m_drained.wakeAll();
            }
        }
    }

private:
    void enqueue(Task task)
    {
        const QMutexLocker lock(&m_mutex);
        task.sequence = ++m_nextSequence;
        if (!task.path.isEmpty())
            m_lastPathSequence.insert(task.path, task.sequence);
        m_tasks.enqueue(std::move(task));
        if (!isRunning())
            start();
        m_ready.wakeOne();
    }

    static void perform(Task &task)
    {
        if (task.kind == Kind::Remove) {
            QFile::remove(task.path);
            return;
        }
        if (task.kind == Kind::Barrier) {
            const QPointer<QObject> context = task.context;
            if (!context)
                return;
            QMetaObject::invokeMethod(
                context,
                [context, callback = std::move(task.callback)]() mutable {
                    if (context)
                        callback();
                },
                Qt::QueuedConnection);
            return;
        }

        QDir().mkpath(QFileInfo(task.path).absolutePath());
        QFile file(task.path);
        if (!file.open(QIODevice::Append))
            return;
        if (task.kind == Kind::Event) {
            file.write(QJsonDocument(task.event).toJson(QJsonDocument::Compact));
            file.write("\n");
        } else {
            file.write(task.text.toUtf8());
            if (!task.text.endsWith(QLatin1Char('\n')))
                file.write("\n");
        }
    }

    QMutex m_mutex;
    QWaitCondition m_ready;
    QWaitCondition m_drained;
    QQueue<Task> m_tasks;
    QHash<QString, quint64> m_lastPathSequence;
    quint64 m_nextSequence = 0;
    quint64 m_completedSequence = 0;
    bool m_stopping = false;
};

} // namespace forkmesh
