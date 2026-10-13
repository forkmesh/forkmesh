#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class TerminalBackend : public QObject
{
public:
    struct Callbacks {
        std::function<void(const QByteArray &bytes)> output;
        std::function<void(int code)> exited;
        std::function<void(const QString &error)> failed;
        std::function<void()> started;
    };

    struct Start {
        QString commandLine;
        QString cwd;
        QStringList extraEnv;
        bool keepHostShell = true;
        int cols = 80;
        int rows = 24;
        QString persistKey;
    };

    struct Hooks {
        std::function<TerminalBackend *(QObject *parent, const Start &start, Callbacks callbacks)> start;
        std::function<TerminalBackend *(QObject *parent, const QString &persistKey, int cols, int rows,
                                        Callbacks callbacks)>
            resume;
        std::function<QString(const QString &persistKey)> persisted;
    };

    static Hooks &hooks()
    {
        static Hooks installed;
        return installed;
    }

    using QObject::QObject;

    virtual bool running() const = 0;
    virtual void write(const QByteArray &bytes) = 0;
    virtual void resize(int cols, int rows) = 0;
    virtual void close() = 0;
    virtual void keepRunning() = 0;
};
