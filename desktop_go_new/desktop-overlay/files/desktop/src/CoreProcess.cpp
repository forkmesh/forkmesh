#include "CoreProcess.h"

#include "AgentResources.h"
#include "CoreClient.h"
#include "ProcessReaper.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QStandardPaths>

#include <atomic>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace {

constexpr qint64 kFlowWindowBytes = 4 * 1024 * 1024;
constexpr qint64 kFlowStepBytes = 1024 * 1024;

QPointer<CoreClient> &hubClient()
{
    static QPointer<CoreClient> client;
    return client;
}

QHash<QString, CoreProcess *> &registry()
{
    static QHash<QString, CoreProcess *> processes;
    return processes;
}

QString generatedKey()
{
    static std::atomic<int> serial{0};
    return QStringLiteral("ui-%1-%2-%3")
        .arg(QCoreApplication::applicationPid())
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(++serial);
}

QJsonObject readObject(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

QString currentBootId()
{
    QFile file(QStringLiteral("/proc/sys/kernel/random/boot_id"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromLatin1(file.readAll()).trimmed();
}

bool pidAlive(qint64 pid)
{
#ifdef Q_OS_WIN
    if (pid <= 0)
        return false;
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!process)
        return ::GetLastError() == ERROR_ACCESS_DENIED;
    DWORD code = 0;
    const bool running = ::GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    ::CloseHandle(process);
    return running;
#else
    if (pid <= 1)
        return false;
    return ::kill(pid_t(pid), 0) == 0 || errno == EPERM;
#endif
}

} // namespace

namespace forkmesh::coreagentproc {

QString stateDirectory(const QString &coreStateDirectory)
{
    return QDir(coreStateDirectory).filePath(QStringLiteral("agentproc"));
}

bool enabledInSettings()
{
    return QSettings().value(QStringLiteral("core/agentProcesses"), true).toBool();
}

QList<LiveProcess> scan(const QString &coreStateDirectory)
{
    QList<LiveProcess> out;
    const QDir root(stateDirectory(coreStateDirectory));
    if (!root.exists())
        return out;
    const QString boot = currentBootId();
    const QStringList names = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : names) {
        const QDir dir(root.filePath(name));
        const QJsonObject spec = readObject(dir.filePath(QStringLiteral("spec.json")));
        if (spec.value(QStringLiteral("key")).toString() != name)
            continue;
        LiveProcess process;
        process.key = name;
        process.meta = readObject(dir.filePath(QStringLiteral("meta.json")));
        process.createdMs = qint64(spec.value(QStringLiteral("createdMs")).toDouble());
        const QJsonObject ack = readObject(dir.filePath(QStringLiteral("ack.json")));
        process.ackStdout = qint64(ack.value(QStringLiteral("stdout")).toDouble());
        process.ackStderr = qint64(ack.value(QStringLiteral("stderr")).toDouble());
        const QJsonObject run = readObject(dir.filePath(QStringLiteral("run.json")));
        process.pid = qint64(run.value(QStringLiteral("pid")).toDouble());
        process.shimPid = qint64(run.value(QStringLiteral("shimPid")).toDouble());
        const QString runBoot = run.value(QStringLiteral("boot")).toString();
        const bool sameBoot = runBoot.isEmpty() || boot.isEmpty() || runBoot == boot;
        process.exited = QFileInfo::exists(dir.filePath(QStringLiteral("exit.json")));
        process.running = !process.exited && sameBoot && pidAlive(process.shimPid);
        if (process.running || process.exited)
            out.append(process);
    }
    return out;
}

void terminateShim(const LiveProcess &process)
{
#ifdef Q_OS_WIN
    Q_UNUSED(process)
#else
    if (process.running && pidAlive(process.shimPid))
        ::kill(pid_t(process.shimPid), SIGTERM);
#endif
}

} // namespace forkmesh::coreagentproc

void CoreProcess::install(CoreClient *client)
{
    if (!client || hubClient() == client)
        return;
    hubClient() = client;
    QObject::connect(client, &CoreClient::coreEvent, client,
                     [](const QString &event, const QJsonObject &data) {
                         if (!event.startsWith(QLatin1String("core.agentproc.")))
                             return;
                         if (CoreProcess *process =
                                 registry().value(data.value(QStringLiteral("key")).toString()))
                             process->handleEvent(event, data);
                     });
    QObject::connect(client, &CoreClient::connectedChanged, client, [](bool connected) {
        const QList<CoreProcess *> processes = registry().values();
        for (CoreProcess *process : processes)
            if (registry().values().contains(process))
                process->handleConnected(connected);
    });
    AgentProcess::setFactory([](QObject *parent, const Options &options) {
        return CoreProcess::createFor(parent, options);
    });
}

bool CoreProcess::available()
{
    CoreClient *client = hubClient();
    return client && client->isConnected() && forkmesh::coreagentproc::enabledInSettings() &&
           client->hasNativeRoute(QStringLiteral("core.agentproc.start"));
}

AgentProcess *CoreProcess::createFor(QObject *parent, const Options &options)
{
    CoreClient *client = hubClient();
    if (!options.attachKey.isEmpty()) {
        if (!client || !client->hasNativeRoute(QStringLiteral("core.agentproc.attach")))
            return nullptr;
        return new CoreProcess(client, options, parent);
    }
    if (!available())
        return nullptr;
    return new CoreProcess(client, options, parent);
}

CoreProcess::CoreProcess(CoreClient *client, const Options &options, QObject *parent)
    : AgentProcess(parent), m_client(client), m_key(options.key),
      m_detachable(options.detachable), m_meta(options.meta)
{
    if (!options.attachKey.isEmpty()) {
        m_key = options.attachKey;
        m_attachMode = true;
        m_detachable = true;
        m_state = QProcess::Running;
        m_pid = options.pid;
        m_stdoutEnd = m_stdoutRead = qMax<qint64>(0, options.stdoutOffset);
        m_stderrEnd = options.stderrOffset;
        m_stderrRead = qMax<qint64>(0, options.stderrOffset);
        m_flowSent = m_stdoutRead + m_stderrRead;
    } else {
        m_stderrEnd = 0;
    }
    if (m_key.isEmpty())
        m_key = generatedKey();
    m_forgetOnExit = !m_detachable;
    registry().insert(m_key, this);
    if (m_attachMode)
        attach();
}

CoreProcess::~CoreProcess()
{
    if (registry().value(m_key) == this)
        registry().remove(m_key);
    if (m_local || !m_client)
        return;
    if (m_state != QProcess::NotRunning && !m_exited) {
        if (m_state == QProcess::Running && keepsRunningAfterQuit())
            return;
        if (m_state == QProcess::Running && !m_stopHandedOff)
            m_client->notify(QStringLiteral("core.agentproc.stop"),
                             keyed({{QStringLiteral("graceMs"), 1500},
                                    {QStringLiteral("forget"), true}}));
        return;
    }
    if (m_exited && !m_forgotten)
        m_client->notify(QStringLiteral("core.agentproc.forget"), keyed());
}

QJsonObject CoreProcess::keyed(QJsonObject params) const
{
    params.insert(QStringLiteral("key"), m_key);
    return params;
}

void CoreProcess::setWorkingDirectory(const QString &directory)
{
    m_workingDirectory = directory;
}

void CoreProcess::setProcessEnvironment(const QProcessEnvironment &environment)
{
    m_environment = environment;
    m_environmentSet = true;
}

void CoreProcess::setProcessChannelMode(QProcess::ProcessChannelMode mode)
{
    m_merge = mode == QProcess::MergedChannels;
}

void CoreProcess::setProgram(const QString &program)
{
    m_program = program;
}

void CoreProcess::setArguments(const QStringList &arguments)
{
    m_arguments = arguments;
}

void CoreProcess::start()
{
    m_scope = QJsonObject();
    m_label.clear();
    launch();
}

void CoreProcess::startContained(const QString &program, const QStringList &arguments,
                                 const QString &label)
{
    m_program = program;
    m_arguments = arguments;
    m_label = label;
    m_scope = QJsonObject();
    const forkmesh::agentresources::Status status = forkmesh::agentresources::status();
    const QString description = QStringLiteral("ForkMesh agent %1").arg(label);
    if (status.available && !program.isEmpty()) {
        m_scope = {{QStringLiteral("systemdRun"), status.systemdRun},
                   {QStringLiteral("contain"), true},
                   {QStringLiteral("slice"), forkmesh::agentresources::sliceName()},
                   {QStringLiteral("memoryHigh"), double(status.budget.agentHighBytes)},
                   {QStringLiteral("memoryMax"), double(status.budget.agentMaxBytes)},
                   {QStringLiteral("oomPolicy"), status.oomPolicy},
                   {QStringLiteral("description"), description}};
    } else {
#ifdef Q_OS_LINUX
        const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
        const QString systemdRun = QStandardPaths::findExecutable(QStringLiteral("systemd-run"));
        if (!systemdRun.isEmpty() && !runtime.isEmpty() &&
            QFileInfo::exists(runtime + QStringLiteral("/systemd")))
            m_scope = {{QStringLiteral("systemdRun"), systemdRun},
                       {QStringLiteral("contain"), false},
                       {QStringLiteral("description"), description}};
#endif
    }
    launch();
}

void CoreProcess::launch()
{
    if (m_state != QProcess::NotRunning || m_attachMode)
        return;
    m_state = QProcess::Starting;
    if (!m_client || !m_client->isConnected()) {
        startLocally();
        return;
    }
    const QProcessEnvironment environment = forkmesh::processreaper::releasedEnvironment(
        m_environmentSet ? m_environment : QProcessEnvironment::systemEnvironment());
    QJsonObject meta = m_meta;
    const QJsonValue secrets = meta.take(QStringLiteral("_secrets"));
    QJsonObject params = keyed({{QStringLiteral("program"), m_program},
                                {QStringLiteral("args"), QJsonArray::fromStringList(m_arguments)},
                                {QStringLiteral("env"),
                                 QJsonArray::fromStringList(environment.toStringList())},
                                {QStringLiteral("cwd"), m_workingDirectory},
                                {QStringLiteral("label"), m_label},
                                {QStringLiteral("detachable"), m_detachable},
                                {QStringLiteral("merge"), m_merge},
                                {QStringLiteral("meta"), meta},
                                {QStringLiteral("window"), double(kFlowWindowBytes)}});
    if (secrets.isArray())
        params.insert(QStringLiteral("secrets"), secrets);
    if (!m_scope.isEmpty())
        params.insert(QStringLiteral("scope"), m_scope);
    if (m_meta.contains(QStringLiteral("parse"))) {
        params.insert(QStringLiteral("skipStdout"), true);
        params.insert(QStringLiteral("skipStderr"), true);
    }
    QPointer<CoreProcess> self(this);
    QPointer<CoreClient> client = m_client;
    const QString key = m_key;
    m_client->request(
        QStringLiteral("core.agentproc.start"), params,
        [self, client, key](bool ok, const QJsonValue &result, const QString &error) {
            if (!self) {
                if (ok && client)
                    client->notify(QStringLiteral("core.agentproc.stop"),
                                   {{QStringLiteral("key"), key},
                                    {QStringLiteral("graceMs"), 1500},
                                    {QStringLiteral("forget"), true}});
                return;
            }
            if (self->m_state != QProcess::Starting)
                return;
            if (!ok) {
                if (error == QLatin1String("core_unavailable"))
                    self->startLocally();
                else
                    self->failToStart(error);
                return;
            }
            self->m_pid = qint64(result.toObject().value(QStringLiteral("pid")).toDouble());
            self->m_parsedByCore = result.toObject().value(QStringLiteral("parsed")).toBool();
            self->m_state = QProcess::Running;
            self->m_live = true;
            emit self->started();
            if (!self)
                return;
            self->flushOps();
            if (!self->m_stdout.isEmpty())
                emit self->readyReadStandardOutput();
            if (self && !self->m_stderr.isEmpty())
                emit self->readyReadStandardError();
            if (self && self->m_exited)
                self->finish(self->m_exitCode, self->m_exitStatus);
        },
        30000);
}

void CoreProcess::startLocally()
{
    m_local = std::make_unique<LocalAgentProcess>();
    LocalAgentProcess *local = m_local.get();
    connect(local, &AgentProcess::started, this, &AgentProcess::started);
    connect(local, &AgentProcess::finished, this, &AgentProcess::finished);
    connect(local, &AgentProcess::errorOccurred, this, &AgentProcess::errorOccurred);
    connect(local, &AgentProcess::readyReadStandardOutput, this,
            &AgentProcess::readyReadStandardOutput);
    connect(local, &AgentProcess::readyReadStandardError, this,
            &AgentProcess::readyReadStandardError);
    if (!m_workingDirectory.isEmpty())
        local->setWorkingDirectory(m_workingDirectory);
    if (m_environmentSet)
        local->setProcessEnvironment(m_environment);
    if (m_merge)
        local->setProcessChannelMode(QProcess::MergedChannels);
    QList<QByteArray> writes;
    bool closeAfter = false;
    for (const auto &op : std::as_const(m_ops)) {
        if (op.first == QLatin1String("core.agentproc.write"))
            writes.append(QByteArray::fromBase64(
                op.second.value(QStringLiteral("data")).toString().toLatin1()));
        else if (op.first == QLatin1String("core.agentproc.closeStdin"))
            closeAfter = true;
    }
    m_ops.clear();
    m_state = QProcess::NotRunning;
    if (!m_label.isEmpty())
        local->startContained(m_program, m_arguments, m_label);
    else
        local->start(m_program, m_arguments);
    for (const QByteArray &data : std::as_const(writes))
        local->write(data);
    if (closeAfter)
        local->closeWriteChannel();
}

void CoreProcess::failToStart(const QString &error)
{
    m_state = QProcess::NotRunning;
    m_live = false;
    m_ops.clear();
    m_error = error.isEmpty() ? QStringLiteral("failed_to_start") : error;
    emit errorOccurred(QProcess::FailedToStart);
}

void CoreProcess::attach()
{
    if (!m_client || !m_client->isConnected())
        return;
    QJsonObject params = keyed({{QStringLiteral("stdoutOffset"), double(m_stdoutEnd)},
                                {QStringLiteral("window"), double(kFlowWindowBytes)}});
    if (m_stderrEnd >= 0)
        params.insert(QStringLiteral("stderrOffset"), double(m_stderrEnd));
    if (m_meta.contains(QStringLiteral("parse"))) {
        params.insert(QStringLiteral("skipStdout"), true);
        params.insert(QStringLiteral("skipStderr"), true);
    }
    QPointer<CoreProcess> self(this);
    m_client->request(QStringLiteral("core.agentproc.attach"), params,
                      [self](bool ok, const QJsonValue &result, const QString &error) {
                          if (!self || self->m_exited)
                              return;
                          if (!ok) {
                              if (error == QLatin1String("unknown_key")) {
                                  self->m_error = QStringLiteral("The agent process is gone.");
                                  self->m_forgotten = true;
                                  self->finish(-1, QProcess::CrashExit);
                              }
                              return;
                          }
                          const qint64 pid = qint64(
                              result.toObject().value(QStringLiteral("pid")).toDouble());
                          if (pid > 0)
                              self->m_pid = pid;
                          self->m_parsedByCore =
                              result.toObject().value(QStringLiteral("parsed")).toBool();
                          self->m_live = true;
                          self->flushOps();
                      });
}

void CoreProcess::sendOp(const QString &method, const QJsonObject &params)
{
    if (m_ops.isEmpty() && m_client && m_client->isConnected() && (m_live || m_exited) &&
        m_client->notify(method, keyed(params)))
        return;
    m_ops.append({method, params});
}

void CoreProcess::flushOps()
{
    while (!m_ops.isEmpty() && m_client && m_client->isConnected()) {
        const auto op = m_ops.first();
        if (!m_client->notify(op.first, keyed(op.second)))
            return;
        m_ops.removeFirst();
    }
}

void CoreProcess::handleConnected(bool connected)
{
    if (m_local)
        return;
    if (!connected) {
        m_live = false;
        return;
    }
    if (m_state == QProcess::Running && !m_exited) {
        attach();
        return;
    }
    if (m_exited)
        flushOps();
}

void CoreProcess::handleEvent(const QString &event, const QJsonObject &data)
{
    if (m_local)
        return;
    const bool out = event == QLatin1String("core.agentproc.stdout");
    if (out || event == QLatin1String("core.agentproc.stderr")) {
        QByteArray bytes =
            QByteArray::fromBase64(data.value(QStringLiteral("data")).toString().toLatin1());
        const qint64 offset = qint64(data.value(QStringLiteral("offset")).toDouble());
        const qint64 end = offset + bytes.size();
        qint64 &known = out ? m_stdoutEnd : m_stderrEnd;
        qint64 &read = out ? m_stdoutRead : m_stderrRead;
        QByteArray &buffer = out ? m_stdout : m_stderr;
        if (known < 0) {
            known = offset;
            read = offset;
        }
        if (end <= known)
            return;
        if (offset < known)
            bytes.remove(0, known - offset);
        else if (offset > known && buffer.isEmpty())
            read = offset;
        buffer.append(bytes);
        known = end;
        if (m_state != QProcess::Running)
            return;
        if (out)
            emit readyReadStandardOutput();
        else
            emit readyReadStandardError();
        return;
    }
    if (event == QLatin1String("core.agentproc.exit")) {
        const bool crashed = data.value(QStringLiteral("crashed")).toBool();
        const int code = data.value(QStringLiteral("code")).toInt();
        if (data.value(QStringLiteral("lost")).toBool())
            m_error = QStringLiteral("The agent process was lost.");
        else if (crashed)
            m_error = QStringLiteral("Process crashed");
        if (m_state == QProcess::Starting) {
            m_exited = true;
            m_exitCode = code;
            m_exitStatus = crashed ? QProcess::CrashExit : QProcess::NormalExit;
            return;
        }
        finish(code, crashed ? QProcess::CrashExit : QProcess::NormalExit);
    }
}

void CoreProcess::finish(int code, QProcess::ExitStatus status)
{
    if (m_state == QProcess::NotRunning && m_exited)
        return;
    m_exited = true;
    m_live = false;
    m_state = QProcess::NotRunning;
    m_exitCode = code;
    m_exitStatus = status;
    QPointer<CoreProcess> self(this);
    if (status == QProcess::CrashExit)
        emit errorOccurred(QProcess::Crashed);
    if (!self)
        return;
    emit finished(code, status);
    if (self && self->m_forgetOnExit)
        self->forget();
}

qint64 CoreProcess::write(const QByteArray &data)
{
    if (m_local)
        return m_local->write(data);
    if (m_state == QProcess::NotRunning || m_stdinClosed)
        return -1;
    sendOp(QStringLiteral("core.agentproc.write"),
           {{QStringLiteral("data"), QString::fromLatin1(data.toBase64())}});
    return data.size();
}

void CoreProcess::closeWriteChannel()
{
    if (m_local) {
        m_local->closeWriteChannel();
        return;
    }
    if (m_stdinClosed || m_state == QProcess::NotRunning)
        return;
    m_stdinClosed = true;
    sendOp(QStringLiteral("core.agentproc.closeStdin"), {});
}

QByteArray CoreProcess::readAllStandardOutput()
{
    if (m_local)
        return m_local->readAllStandardOutput();
    QByteArray out;
    out.swap(m_stdout);
    m_stdoutRead = m_stdoutEnd;
    maybeSendFlow();
    return out;
}

QByteArray CoreProcess::readAllStandardError()
{
    if (m_local)
        return m_local->readAllStandardError();
    QByteArray out;
    out.swap(m_stderr);
    if (m_stderrEnd >= 0)
        m_stderrRead = m_stderrEnd;
    maybeSendFlow();
    return out;
}

void CoreProcess::maybeSendFlow()
{
    const qint64 consumed = m_stdoutRead + qMax<qint64>(0, m_stderrRead);
    if (consumed - m_flowSent < kFlowStepBytes || m_exited)
        return;
    m_flowSent = consumed;
    QJsonObject params{{QStringLiteral("stdoutOffset"), double(m_stdoutRead)}};
    if (m_stderrEnd >= 0)
        params.insert(QStringLiteral("stderrOffset"), double(m_stderrRead));
    sendOp(QStringLiteral("core.agentproc.flow"), params);
}

QProcess::ProcessState CoreProcess::state() const
{
    return m_local ? m_local->state() : m_state;
}

qint64 CoreProcess::processId() const
{
    if (m_local)
        return m_local->processId();
    return m_state == QProcess::NotRunning ? 0 : m_pid;
}

bool CoreProcess::isWritable() const
{
    if (m_local)
        return m_local->isWritable();
    return m_state != QProcess::NotRunning && !m_stdinClosed;
}

void CoreProcess::terminate()
{
    if (m_local) {
        m_local->terminate();
        return;
    }
    if (m_state == QProcess::NotRunning)
        return;
    sendOp(QStringLiteral("core.agentproc.signal"),
           {{QStringLiteral("signal"), QStringLiteral("TERM")}});
}

void CoreProcess::kill()
{
    if (m_local) {
        m_local->kill();
        return;
    }
    if (m_state == QProcess::NotRunning)
        return;
    sendOp(QStringLiteral("core.agentproc.kill"), {});
}

bool CoreProcess::waitForStarted(int msecs)
{
    if (m_local)
        return m_local->waitForStarted(msecs);
    return m_state == QProcess::Running;
}

bool CoreProcess::waitForFinished(int msecs)
{
    if (m_local)
        return m_local->waitForFinished(msecs);
    if (m_state == QProcess::NotRunning)
        return true;
    m_stopHandedOff = true;
    sendOp(QStringLiteral("core.agentproc.stop"),
           {{QStringLiteral("graceMs"), qMax(1, msecs)}, {QStringLiteral("forget"), true}});
    return true;
}

QString CoreProcess::errorString() const
{
    if (m_local)
        return m_local->errorString();
    return m_error.isEmpty() ? QStringLiteral("Unknown error") : m_error;
}

int CoreProcess::exitCode() const
{
    return m_local ? m_local->exitCode() : m_exitCode;
}

QProcess::ExitStatus CoreProcess::exitStatus() const
{
    return m_local ? m_local->exitStatus() : m_exitStatus;
}

bool CoreProcess::coreBacked() const
{
    return !m_local;
}

qint64 CoreProcess::stdoutOffset() const
{
    return m_local ? -1 : m_stdoutRead;
}

qint64 CoreProcess::stderrOffset() const
{
    return m_local ? -1 : qMax<qint64>(0, m_stderrRead);
}

void CoreProcess::acknowledge(qint64 stdoutOffset, qint64 stderrOffset)
{
    if (m_local || m_forgotten)
        return;
    QJsonObject params{{QStringLiteral("stdoutOffset"), double(stdoutOffset)}};
    if (stderrOffset >= 0)
        params.insert(QStringLiteral("stderrOffset"), double(stderrOffset));
    sendOp(QStringLiteral("core.agentproc.ack"), params);
}

void CoreProcess::updateMeta(const QJsonObject &meta)
{
    if (m_local || m_forgotten || meta.isEmpty())
        return;
    for (auto it = meta.begin(); it != meta.end(); ++it)
        m_meta.insert(it.key(), it.value());
    if (m_state == QProcess::Starting || m_state == QProcess::Running || m_exited)
        sendOp(QStringLiteral("core.agentproc.setMeta"), {{QStringLiteral("meta"), meta}});
}

void CoreProcess::forget()
{
    if (m_local || m_forgotten)
        return;
    m_forgotten = true;
    sendOp(QStringLiteral("core.agentproc.forget"), {});
}

void CoreProcess::setIdle(bool idle)
{
    if (m_idle == idle)
        return;
    m_idle = idle;
    if (m_local || !m_detachable)
        return;
    sendOp(QStringLiteral("core.agentproc.setDetachable"),
           {{QStringLiteral("detachable"), !idle}});
}

bool CoreProcess::keepsRunningAfterQuit() const
{
    return !m_local && m_detachable && !m_idle && m_state == QProcess::Running && !m_exited &&
           AgentProcess::desktopQuitting();
}
