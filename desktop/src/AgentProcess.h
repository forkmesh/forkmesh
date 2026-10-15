#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <functional>

class AgentProcess : public QObject
{
    Q_OBJECT
public:
    struct Options {
        QString key;
        bool detachable = false;
        QJsonObject meta;
        QString attachKey;
        qint64 stdoutOffset = -1;
        qint64 stderrOffset = -1;
        qint64 pid = 0;
    };
    using Factory = std::function<AgentProcess *(QObject *parent, const Options &options)>;

    explicit AgentProcess(QObject *parent = nullptr) : QObject(parent) {}
    ~AgentProcess() override = default;

    static AgentProcess *create(QObject *parent);
    static AgentProcess *create(QObject *parent, const Options &options);
    static void setFactory(Factory factory);
    static bool hasFactory();
    static void setDesktopQuitting(bool quitting);
    static bool desktopQuitting();

    virtual void setWorkingDirectory(const QString &directory) = 0;
    virtual void setProcessEnvironment(const QProcessEnvironment &environment) = 0;
    virtual void setProcessChannelMode(QProcess::ProcessChannelMode mode) = 0;
    virtual void setProgram(const QString &program) = 0;
    virtual void setArguments(const QStringList &arguments) = 0;
    virtual void start() = 0;
    void start(const QString &program, const QStringList &arguments);
    virtual void startContained(const QString &program, const QStringList &arguments,
                                const QString &label) = 0;
    virtual qint64 write(const QByteArray &data) = 0;
    virtual void closeWriteChannel() = 0;
    virtual QByteArray readAllStandardOutput() = 0;
    virtual QByteArray readAllStandardError() = 0;
    virtual QProcess::ProcessState state() const = 0;
    virtual qint64 processId() const = 0;
    virtual bool isWritable() const = 0;
    virtual void terminate() = 0;
    virtual void kill() = 0;
    virtual bool waitForStarted(int msecs = 30000) = 0;
    virtual bool waitForFinished(int msecs = 30000) = 0;
    virtual QString errorString() const = 0;
    virtual int exitCode() const = 0;
    virtual QProcess::ExitStatus exitStatus() const = 0;

    virtual bool coreBacked() const { return false; }
    virtual QString key() const { return QString(); }
    virtual qint64 stdoutOffset() const { return -1; }
    virtual qint64 stderrOffset() const { return -1; }
    virtual void acknowledge(qint64 stdoutOffset, qint64 stderrOffset)
    {
        Q_UNUSED(stdoutOffset)
        Q_UNUSED(stderrOffset)
    }
    virtual void updateMeta(const QJsonObject &meta) { Q_UNUSED(meta) }
    virtual void setForgetOnExit(bool forget) { Q_UNUSED(forget) }
    virtual void forget() {}
    virtual bool keepsRunningAfterQuit() const { return false; }
    virtual void setIdle(bool idle) { Q_UNUSED(idle) }
    virtual bool parsedByCore() const { return false; }

signals:
    void started();
    void finished(int exitCode, QProcess::ExitStatus exitStatus);
    void errorOccurred(QProcess::ProcessError error);
    void readyReadStandardOutput();
    void readyReadStandardError();
};

class LocalAgentProcess : public AgentProcess
{
public:
    explicit LocalAgentProcess(QObject *parent = nullptr);
    ~LocalAgentProcess() override;

    void setWorkingDirectory(const QString &directory) override;
    void setProcessEnvironment(const QProcessEnvironment &environment) override;
    void setProcessChannelMode(QProcess::ProcessChannelMode mode) override;
    void setProgram(const QString &program) override;
    void setArguments(const QStringList &arguments) override;
    using AgentProcess::start;
    void start() override;
    void startContained(const QString &program, const QStringList &arguments,
                        const QString &label) override;
    qint64 write(const QByteArray &data) override;
    void closeWriteChannel() override;
    QByteArray readAllStandardOutput() override;
    QByteArray readAllStandardError() override;
    QProcess::ProcessState state() const override;
    qint64 processId() const override;
    bool isWritable() const override;
    void terminate() override;
    void kill() override;
    bool waitForStarted(int msecs = 30000) override;
    bool waitForFinished(int msecs = 30000) override;
    QString errorString() const override;
    int exitCode() const override;
    QProcess::ExitStatus exitStatus() const override;

private:
    QProcess *m_process = nullptr;
};
