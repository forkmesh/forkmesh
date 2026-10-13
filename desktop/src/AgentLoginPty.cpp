#include "AgentLoginPty.h"
#include "AgentCliLocator.h"

#include <QSocketNotifier>

#ifndef Q_OS_WIN
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

extern char **environ;
#endif

namespace {

// Provider API keys are stripped from the child's environment for the same
// reason the desktop's own hand-off terminal strips them: a sign-in must
// authenticate the account the operator is entering, not whatever key happens
// to be exported into this process.
const char *const kStrippedEnvironment[] = {
    "ANTHROPIC_API_KEY=", "ANTHROPIC_AUTH_TOKEN=", "OPENAI_API_KEY=",
    "TERM=", "COLORTERM=",
};

} // namespace

AgentLoginPty::AgentLoginPty(QObject *parent) : QObject(parent) {}

AgentLoginPty::~AgentLoginPty()
{
    stop();
}

QString AgentLoginPty::loginProgram(const QString &provider)
{
    if (provider == QLatin1String("claude-code"))
        return QStringLiteral("claude");
    if (provider == QLatin1String("codex"))
        return QStringLiteral("codex");
    return {};
}

QStringList AgentLoginPty::loginArguments(const QString &provider)
{
    if (provider == QLatin1String("claude-code"))
        return {QStringLiteral("auth"), QStringLiteral("login")};
    if (provider == QLatin1String("codex"))
        return {QStringLiteral("login")};
    return {};
}

bool AgentLoginPty::start(const QString &provider, int cols, int rows,
                          QString *error)
{
    if (isRunning()) {
        if (error)
            *error = QStringLiteral("A sign-in is already running on this node.");
        return false;
    }
    const QString program = loginProgram(provider);
    if (program.isEmpty()) {
        if (error)
            *error = QStringLiteral("That agent provider has no sign-in here.");
        return false;
    }
    const QString resolved = forkmesh::agentcli::executable(program);
    if (resolved.isEmpty()) {
        if (error)
            *error = QStringLiteral("%1 is not installed on this node.").arg(program);
        return false;
    }
    return run(resolved, loginArguments(provider), QString(), cols, rows, true, error);
}

bool AgentLoginPty::run(const QString &program, const QStringList &arguments,
                        const QString &workingDirectory, int cols, int rows,
                        bool stripCredentials, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (isRunning())
        return fail(QStringLiteral("A terminal is already running here."));
    if (program.isEmpty())
        return fail(QStringLiteral("Nothing to run."));
#ifdef Q_OS_WIN
    Q_UNUSED(arguments)
    Q_UNUSED(workingDirectory)
    Q_UNUSED(cols)
    Q_UNUSED(rows)
    Q_UNUSED(stripCredentials)
    return fail(QStringLiteral(
        "Streamed sign-in needs a pseudo-terminal, which this node's platform "
        "does not provide. Sign in from the ForkMesh app on this machine."));
#else
    m_cols = qBound(20, cols, 500);
    m_rows = qBound(5, rows, 200);

    QList<QByteArray> argStore;
    argStore.append(program.toLocal8Bit());
    for (const QString &argument : arguments)
        argStore.append(argument.toLocal8Bit());
    QVector<char *> argv;
    argv.reserve(argStore.size() + 1);
    for (QByteArray &argument : argStore)
        argv.append(argument.data());
    argv.append(nullptr);

    QList<QByteArray> envStore;
    for (char **entry = environ; entry && *entry; ++entry) {
        const QByteArray value(*entry);
        bool stripped = false;
        const qsizetype equals = value.indexOf('=');
        if (equals > 0 && m_environment.contains(QString::fromLocal8Bit(value.left(equals))))
            continue;
        for (const char *prefix : kStrippedEnvironment) {
            if (value.startsWith(prefix) &&
                (stripCredentials || value.startsWith("TERM=") ||
                 value.startsWith("COLORTERM="))) {
                stripped = true;
                break;
            }
        }
        if (!stripped)
            envStore.append(value);
    }
    for (auto entry = m_environment.cbegin(); entry != m_environment.cend(); ++entry)
        envStore.append((entry.key() + QLatin1Char('=') + entry.value()).toLocal8Bit());
    envStore.append(QByteArrayLiteral("TERM=xterm-256color"));
    envStore.append(QByteArrayLiteral("COLORTERM=truecolor"));
    QVector<char *> envp;
    envp.reserve(envStore.size() + 1);
    for (QByteArray &entry : envStore)
        envp.append(entry.data());
    envp.append(nullptr);
    const QByteArray directory = workingDirectory.toLocal8Bit();

    struct winsize ws = {};
    ws.ws_row = (unsigned short)m_rows;
    ws.ws_col = (unsigned short)m_cols;

    int master = -1;
    const pid_t pid = forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0)
        return fail(QStringLiteral("This node could not allocate a terminal."));
    if (pid == 0) {
        if (!directory.isEmpty() && ::chdir(directory.constData()) != 0)
            ::_exit(126);
        environ = envp.data();
        ::execv(argv.at(0), argv.data());
        ::_exit(127);
    }

    m_master = master;
    m_childPid = pid;
    ::fcntl(m_master, F_SETFL, ::fcntl(m_master, F_GETFL, 0) | O_NONBLOCK);
    m_notifier = new QSocketNotifier(m_master, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this,
            &AgentLoginPty::onReadyRead);
    return true;
#endif
}

void AgentLoginPty::write(const QByteArray &bytes)
{
#ifndef Q_OS_WIN
    if (m_master < 0 || bytes.isEmpty())
        return;
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const ssize_t written = ::write(m_master, bytes.constData() + offset,
                                        size_t(bytes.size() - offset));
        if (written > 0)
            offset += written;
        else if (written < 0 && errno == EINTR)
            continue;
        else
            break;
    }
#else
    Q_UNUSED(bytes)
#endif
}

void AgentLoginPty::resize(int cols, int rows)
{
#ifndef Q_OS_WIN
    m_cols = qBound(20, cols, 500);
    m_rows = qBound(5, rows, 200);
    if (m_master < 0)
        return;
    struct winsize ws = {};
    ws.ws_row = (unsigned short)m_rows;
    ws.ws_col = (unsigned short)m_cols;
    ::ioctl(m_master, TIOCSWINSZ, &ws); // the kernel raises SIGWINCH for us
#else
    Q_UNUSED(cols)
    Q_UNUSED(rows)
#endif
}

void AgentLoginPty::stop()
{
#ifndef Q_OS_WIN
    if (m_notifier) {
        m_notifier->setEnabled(false);
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }
    if (m_childPid > 0) {
        // SIGHUP is what closing a terminal sends: the CLI unwinds its own
        // half-written credential state instead of dying mid-write.
        ::kill((pid_t)m_childPid, SIGHUP);
        reap();
    }
    if (m_master >= 0) {
        ::close(m_master);
        m_master = -1;
    }
    m_childPid = -1;
#endif
}

void AgentLoginPty::onReadyRead()
{
#ifndef Q_OS_WIN
    if (m_master < 0)
        return;
    char buffer[6144];
    bool eof = false;
    for (;;) {
        const ssize_t read = ::read(m_master, buffer, sizeof(buffer));
        if (read > 0) {
            emit output(QByteArray(buffer, int(read)));
            if (read < (ssize_t)sizeof(buffer))
                break;
        } else if (read == 0) {
            eof = true;
            break;
        } else {
            if (errno == EINTR)
                continue;
            if (errno == EIO)
                eof = true;
            break;
        }
    }
    if (!eof)
        return;
    if (m_notifier) {
        m_notifier->setEnabled(false);
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }
    if (m_master >= 0) {
        ::close(m_master);
        m_master = -1;
    }
    const long long pid = m_childPid;
    int status = 0;
    if (pid > 0) {
        ::waitpid((pid_t)pid, &status, 0);
        m_childPid = -1;
    }
    emit finished(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
#endif
}

void AgentLoginPty::reap()
{
#ifndef Q_OS_WIN
    if (m_childPid <= 0)
        return;
    int status = 0;
    for (int attempt = 0; attempt < 40; ++attempt) {
        const pid_t result = ::waitpid((pid_t)m_childPid, &status, WNOHANG);
        if (result == (pid_t)m_childPid || result < 0)
            break;
        if (attempt == 10)
            ::kill((pid_t)m_childPid, SIGTERM);
        if (attempt == 30)
            ::kill((pid_t)m_childPid, SIGKILL);
        ::usleep(5000);
    }
    m_childPid = -1;
#endif
}
