#pragma once

#include "AgentProcess.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QPointer>
#include <QString>

#include <memory>

class CoreClient;

namespace forkmesh::coreagentproc {

struct LiveProcess {
    QString key;
    QJsonObject meta;
    qint64 pid = 0;
    qint64 shimPid = 0;
    qint64 ackStdout = 0;
    qint64 ackStderr = 0;
    bool running = false;
    bool exited = false;
    qint64 createdMs = 0;
};

QString stateDirectory(const QString &coreStateDirectory);
QList<LiveProcess> scan(const QString &coreStateDirectory);
bool enabledInSettings();
void terminateShim(const LiveProcess &process);

} // namespace forkmesh::coreagentproc

class CoreProcess : public AgentProcess
{
public:
    CoreProcess(CoreClient *client, const Options &options, QObject *parent = nullptr);
    ~CoreProcess() override;

    static void install(CoreClient *client);
    static bool available();
    static AgentProcess *createFor(QObject *parent, const Options &options);

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

    bool coreBacked() const override;
    QString key() const override { return m_key; }
    qint64 stdoutOffset() const override;
    qint64 stderrOffset() const override;
    void acknowledge(qint64 stdoutOffset, qint64 stderrOffset) override;
    void updateMeta(const QJsonObject &meta) override;
    void setForgetOnExit(bool forget) override { m_forgetOnExit = forget; }
    void forget() override;
    bool keepsRunningAfterQuit() const override;
    void setIdle(bool idle) override;
    bool parsedByCore() const override { return m_parsedByCore; }

    void handleEvent(const QString &event, const QJsonObject &data);
    void handleConnected(bool connected);

private:
    void launch();
    void startLocally();
    void attach();
    void sendOp(const QString &method, const QJsonObject &params);
    void flushOps();
    void finish(int code, QProcess::ExitStatus status);
    void failToStart(const QString &error);
    void maybeSendFlow();
    QJsonObject keyed(QJsonObject params = QJsonObject()) const;

    QPointer<CoreClient> m_client;
    QString m_key;
    bool m_detachable = false;
    bool m_idle = false;
    QJsonObject m_meta;
    QString m_program;
    QStringList m_arguments;
    QString m_workingDirectory;
    QProcessEnvironment m_environment;
    bool m_environmentSet = false;
    bool m_merge = false;
    QJsonObject m_scope;
    QString m_label;
    QProcess::ProcessState m_state = QProcess::NotRunning;
    bool m_live = false;
    bool m_attachMode = false;
    bool m_parsedByCore = false;
    qint64 m_pid = 0;
    QByteArray m_stdout;
    QByteArray m_stderr;
    qint64 m_stdoutEnd = 0;
    qint64 m_stderrEnd = -1;
    qint64 m_stdoutRead = 0;
    qint64 m_stderrRead = 0;
    qint64 m_flowSent = 0;
    QList<QPair<QString, QJsonObject>> m_ops;
    bool m_stdinClosed = false;
    bool m_stopHandedOff = false;
    bool m_forgetOnExit = true;
    bool m_forgotten = false;
    bool m_exited = false;
    QString m_error;
    int m_exitCode = 0;
    QProcess::ExitStatus m_exitStatus = QProcess::NormalExit;
    std::unique_ptr<LocalAgentProcess> m_local;
};
