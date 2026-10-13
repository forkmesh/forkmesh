#include "AgentProcess.h"

#include "AgentResources.h"

#include <atomic>
#include <utility>

namespace {

AgentProcess::Factory &factoryStorage()
{
    static AgentProcess::Factory factory;
    return factory;
}

std::atomic<bool> &quittingFlag()
{
    static std::atomic<bool> flag{false};
    return flag;
}

} // namespace

AgentProcess *AgentProcess::create(QObject *parent)
{
    return create(parent, Options());
}

AgentProcess *AgentProcess::create(QObject *parent, const Options &options)
{
    if (const Factory &factory = factoryStorage()) {
        if (AgentProcess *process = factory(parent, options))
            return process;
    }
    if (!options.attachKey.isEmpty())
        return nullptr;
    return new LocalAgentProcess(parent);
}

void AgentProcess::setFactory(Factory factory)
{
    factoryStorage() = std::move(factory);
}

bool AgentProcess::hasFactory()
{
    return bool(factoryStorage());
}

void AgentProcess::setDesktopQuitting(bool quitting)
{
    quittingFlag() = quitting;
}

bool AgentProcess::desktopQuitting()
{
    return quittingFlag();
}

void AgentProcess::start(const QString &program, const QStringList &arguments)
{
    setProgram(program);
    setArguments(arguments);
    start();
}

LocalAgentProcess::LocalAgentProcess(QObject *parent)
    : AgentProcess(parent), m_process(new QProcess(this))
{
    connect(m_process, &QProcess::started, this, &AgentProcess::started);
    connect(m_process, &QProcess::finished, this, &AgentProcess::finished);
    connect(m_process, &QProcess::errorOccurred, this, &AgentProcess::errorOccurred);
    connect(m_process, &QProcess::readyReadStandardOutput, this,
            &AgentProcess::readyReadStandardOutput);
    connect(m_process, &QProcess::readyReadStandardError, this,
            &AgentProcess::readyReadStandardError);
}

LocalAgentProcess::~LocalAgentProcess()
{
    m_process->disconnect(this);
}

void LocalAgentProcess::setWorkingDirectory(const QString &directory)
{
    m_process->setWorkingDirectory(directory);
}

void LocalAgentProcess::setProcessEnvironment(const QProcessEnvironment &environment)
{
    m_process->setProcessEnvironment(environment);
}

void LocalAgentProcess::setProcessChannelMode(QProcess::ProcessChannelMode mode)
{
    m_process->setProcessChannelMode(mode);
}

void LocalAgentProcess::setProgram(const QString &program)
{
    m_process->setProgram(program);
}

void LocalAgentProcess::setArguments(const QStringList &arguments)
{
    m_process->setArguments(arguments);
}

void LocalAgentProcess::start()
{
    m_process->start();
}

void LocalAgentProcess::startContained(const QString &program, const QStringList &arguments,
                                       const QString &label)
{
    const forkmesh::agentresources::Launch contained =
        forkmesh::agentresources::contain(program, arguments, label);
    m_process->start(contained.program, contained.arguments);
}

qint64 LocalAgentProcess::write(const QByteArray &data)
{
    return m_process->write(data);
}

void LocalAgentProcess::closeWriteChannel()
{
    m_process->closeWriteChannel();
}

QByteArray LocalAgentProcess::readAllStandardOutput()
{
    return m_process->readAllStandardOutput();
}

QByteArray LocalAgentProcess::readAllStandardError()
{
    return m_process->readAllStandardError();
}

QProcess::ProcessState LocalAgentProcess::state() const
{
    return m_process->state();
}

qint64 LocalAgentProcess::processId() const
{
    return m_process->processId();
}

bool LocalAgentProcess::isWritable() const
{
    return m_process->isWritable();
}

void LocalAgentProcess::terminate()
{
    m_process->terminate();
}

void LocalAgentProcess::kill()
{
    m_process->kill();
}

// code-health: allow(blocking-wait) forwards the caller's own bounded QProcess wait
bool LocalAgentProcess::waitForStarted(int msecs)
{
    return m_process->waitForStarted(msecs);
}

// code-health: allow(blocking-wait) forwards the caller's own bounded QProcess wait
bool LocalAgentProcess::waitForFinished(int msecs)
{
    return m_process->waitForFinished(msecs);
}

QString LocalAgentProcess::errorString() const
{
    return m_process->errorString();
}

int LocalAgentProcess::exitCode() const
{
    return m_process->exitCode();
}

QProcess::ExitStatus LocalAgentProcess::exitStatus() const
{
    return m_process->exitStatus();
}
