#include "CodexRateLimitsProbe.h"

#include "AgentCliLocator.h"
#include "AgentResources.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>

namespace {

constexpr qint64 kInitializeId = 1;
constexpr qint64 kRateLimitsId = 2;
constexpr qsizetype kMaximumFrameBytes = 4 * 1024 * 1024;

QString errorText(const QJsonValue &error)
{
    if (error.isObject()) {
        const QString message =
            error.toObject().value(QStringLiteral("message")).toString();
        if (!message.isEmpty())
            return message;
        return QString::fromUtf8(
            QJsonDocument(error.toObject()).toJson(QJsonDocument::Compact));
    }
    return error.toVariant().toString();
}

}

CodexRateLimitsProbe::CodexRateLimitsProbe(QObject *parent)
    : QObject(parent), m_framer(kMaximumFrameBytes)
{
}

CodexRateLimitsProbe::~CodexRateLimitsProbe()
{
    releaseProcess();
}

void CodexRateLimitsProbe::setAppServerCommand(const QString &program,
                                               const QStringList &arguments)
{
    m_program = program;
    m_arguments = arguments;
}

bool CodexRateLimitsProbe::running() const
{
    return m_proc && m_proc->state() != QProcess::NotRunning;
}

QJsonObject CodexRateLimitsProbe::withLegacyLimit(
    const QJsonObject &rateLimits, const QJsonObject &rateLimitsByLimitId)
{
    QJsonObject byId = rateLimitsByLimitId;
    const QString legacyId = rateLimits.value(QStringLiteral("limitId")).toString();
    if (!legacyId.isEmpty() && !byId.contains(legacyId))
        byId.insert(legacyId, rateLimits);
    return byId;
}

void CodexRateLimitsProbe::start(const QString &codexHome, int timeoutMs)
{
    releaseProcess();
    m_done = false;
    m_framer.reset();
    if (codexHome.trimmed().isEmpty()) {
        QTimer::singleShot(0, this, [this] {
            finish(false, {}, {}, QStringLiteral("No Codex config folder"));
        });
        return;
    }

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("OPENAI_API_KEY"));
    env.remove(QStringLiteral("CODEX_API_KEY"));
    env.insert(QStringLiteral("CODEX_HOME"), QDir::cleanPath(codexHome));
    forkmesh::agentcli::widenPath(env);
    forkmesh::agentresources::applyEnvironment(env);

    QString program = m_program;
    QStringList arguments = m_arguments;
    if (program.isEmpty()) {
#ifdef Q_OS_WIN
        program = QStringLiteral("codex");
        arguments = {QStringLiteral("app-server")};
#else
        program = QStringLiteral("bash");
        arguments = {QStringLiteral("-c"),
                     forkmesh::agentresources::wrapShellCommand(
                         QStringLiteral("exec codex app-server"))};
#endif
    }
    const forkmesh::agentresources::Launch contained =
        forkmesh::agentresources::contain(
            program, arguments,
            forkmesh::agentresources::launchLabel(QStringLiteral("codex usage"),
                                                  env));

    QProcess *proc = new QProcess(this);
    m_proc = proc;
    if (QDir(codexHome).exists())
        proc->setWorkingDirectory(codexHome);
    proc->setProcessEnvironment(env);
    proc->setProcessChannelMode(QProcess::SeparateChannels);
    proc->setStandardErrorFile(QProcess::nullDevice());
    connect(proc, &QProcess::started, this, &CodexRateLimitsProbe::onStarted);
    connect(proc, &QProcess::readyReadStandardOutput, this,
            [this] { onStdout(false); });
    connect(proc, &QProcess::errorOccurred, this,
            [this, proc](QProcess::ProcessError error) {
                if (m_proc != proc || error != QProcess::FailedToStart)
                    return;
                finish(false, {}, {},
                       QStringLiteral("Unable to start Codex app-server: %1")
                           .arg(proc->errorString()));
            });
    connect(proc, &QProcess::finished, this, [this, proc](int exitCode) {
        if (m_proc != proc)
            return;
        onStdout(true);
        finish(false, {}, {},
               QStringLiteral("Codex app-server exited (%1) before reporting usage")
                   .arg(exitCode));
    });

    if (!m_timeout) {
        m_timeout = new QTimer(this);
        m_timeout->setSingleShot(true);
        connect(m_timeout, &QTimer::timeout, this, [this] {
            finish(false, {}, {},
                   QStringLiteral("Codex app-server did not report usage in time"));
        });
    }
    m_timeout->start(qMax(1, timeoutMs));
    proc->start(contained.program, contained.arguments);
}

void CodexRateLimitsProbe::onStarted()
{
    send(QJsonObject{
        {QStringLiteral("method"), QStringLiteral("initialize")},
        {QStringLiteral("id"), kInitializeId},
        {QStringLiteral("params"),
         QJsonObject{
             {QStringLiteral("clientInfo"),
              QJsonObject{{QStringLiteral("name"), QStringLiteral("forkmesh")},
                          {QStringLiteral("title"), QStringLiteral("ForkMesh")},
                          {QStringLiteral("version"),
                           QCoreApplication::applicationVersion().isEmpty()
                               ? QStringLiteral("0")
                               : QCoreApplication::applicationVersion()}}},
             {QStringLiteral("capabilities"),
              QJsonObject{{QStringLiteral("experimentalApi"), true},
                          {QStringLiteral("requestAttestation"), false}}}}}});
}

void CodexRateLimitsProbe::onStdout(bool flushRemainder)
{
    if (!m_proc)
        return;
    const AgentJsonlFramer::Result framed =
        m_framer.append(m_proc->readAllStandardOutput(), flushRemainder);
    for (const QByteArray &line : framed.frames) {
        if (m_done)
            return;
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(line, &error);
        if (error.error == QJsonParseError::NoError && document.isObject())
            handleMessage(document.object());
    }
}

void CodexRateLimitsProbe::handleMessage(const QJsonObject &message)
{
    if (message.contains(QStringLiteral("method")) ||
        !message.contains(QStringLiteral("id")))
        return;
    const qint64 id = message.value(QStringLiteral("id")).toVariant().toLongLong();
    const bool failed = message.contains(QStringLiteral("error"));
    if (id == kInitializeId) {
        if (failed) {
            finish(false, {}, {},
                   QStringLiteral("Codex initialize failed: %1")
                       .arg(errorText(message.value(QStringLiteral("error")))));
            return;
        }
        send(QJsonObject{{QStringLiteral("method"), QStringLiteral("initialized")},
                         {QStringLiteral("params"), QJsonObject()}});
        send(QJsonObject{
            {QStringLiteral("method"), QStringLiteral("account/rateLimits/read")},
            {QStringLiteral("id"), kRateLimitsId},
            {QStringLiteral("params"), QJsonValue(QJsonValue::Null)}});
        return;
    }
    if (id != kRateLimitsId)
        return;
    if (failed) {
        finish(false, {}, {},
               QStringLiteral("Codex account/rateLimits/read failed: %1")
                   .arg(errorText(message.value(QStringLiteral("error")))));
        return;
    }
    const QJsonObject result = message.value(QStringLiteral("result")).toObject();
    const QJsonObject rateLimits =
        result.value(QStringLiteral("rateLimits")).toObject();
    const QJsonObject byId = withLegacyLimit(
        rateLimits, result.value(QStringLiteral("rateLimitsByLimitId")).toObject());
    if (rateLimits.isEmpty() && byId.isEmpty()) {
        finish(false, {}, {},
               QStringLiteral("Codex reported no usage for this account"));
        return;
    }
    finish(true, rateLimits, byId, QString());
}

void CodexRateLimitsProbe::send(const QJsonObject &message)
{
    if (!running())
        return;
    QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact);
    line.append('\n');
    m_proc->write(line);
}

void CodexRateLimitsProbe::finish(bool ok, const QJsonObject &rateLimits,
                                  const QJsonObject &rateLimitsByLimitId,
                                  const QString &error)
{
    if (m_done)
        return;
    m_done = true;
    if (m_timeout)
        m_timeout->stop();
    releaseProcess();
    emit finished(ok, rateLimits, rateLimitsByLimitId, error);
}

void CodexRateLimitsProbe::releaseProcess()
{
    QProcess *proc = m_proc;
    if (!proc)
        return;
    m_proc = nullptr;
    proc->disconnect(this);
    proc->setParent(nullptr);
    if (proc->state() == QProcess::NotRunning) {
        proc->deleteLater();
        return;
    }
    connect(proc, &QProcess::finished, proc, &QObject::deleteLater);
    proc->closeWriteChannel();
    proc->terminate();
    QTimer::singleShot(1500, proc, [proc] {
        if (proc->state() != QProcess::NotRunning)
            proc->kill();
    });
}
