#pragma once

#include <QByteArray>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>

class QSocketNotifier;

// One provider sign-in running on a pseudo-terminal, for the website's
// streamed login (MainWindow::onNodeTerminalFrame).
//
// The desktop already opens this command in its own TerminalWidget; what is
// different here is that nothing renders it locally — the bytes go to the
// account owner's browser over the node event socket, and keystrokes come
// back the same way. So this class is the pty and the process, without the
// widget: no parser, no grid, no selection. The remote emulator owns all of
// that (public/agent-terminal.js).
//
// It runs one fixed command per provider and takes no argument from the
// frame that asked for it. A pty rather than a pipe because the provider
// CLIs draw a full-screen prompt and will not read a code from something
// that is not a terminal.
class AgentLoginPty : public QObject
{
    Q_OBJECT
public:
    explicit AgentLoginPty(QObject *parent = nullptr);
    ~AgentLoginPty() override;

    // The command a provider signs in with, or empty for an unknown provider.
    // Kept static so the caller can refuse a frame before spawning anything.
    static QString loginProgram(const QString &provider);
    static QStringList loginArguments(const QString &provider);

    // Starts `provider`'s login on a `cols` x `rows` terminal. Returns false
    // (with `error` set) when the provider is unknown, its CLI is missing, or
    // the pty could not be allocated.
    bool start(const QString &provider, int cols, int rows, QString *error);
    bool run(const QString &program, const QStringList &arguments,
             const QString &workingDirectory, int cols, int rows,
             bool stripCredentials, QString *error);
    void setEnvironment(const QMap<QString, QString> &environment) { m_environment = environment; }
    long long childPid() const { return m_childPid; }
    void write(const QByteArray &bytes);
    void resize(int cols, int rows);
    void stop();
    bool isRunning() const { return m_childPid > 0; }

signals:
    void output(const QByteArray &chunk);
    void finished(int exitCode);

private:
    void onReadyRead();
    void reap();

    int m_master = -1;
    long long m_childPid = -1;
    int m_cols = 80;
    int m_rows = 24;
    QSocketNotifier *m_notifier = nullptr;
    QMap<QString, QString> m_environment;
};
