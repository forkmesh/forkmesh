#include "AgentRunner.h"
#include "AgentCliLocator.h"

#include "AgentGuardrails.h"
#include "AgentTokenPricing.h"

#include "AgentJail.h"
#include "AgentScratch.h"
#include "AgentProcess.h"
#include "AgentResources.h"
#include "AgentRuntimeSession.h"
#include "AgentWorktree.h"
#include "SafeDelete.h"
#include "VirtualMachineRuntime.h"
#include "BackgroundActivity.h"
#include "CoreAgentRun.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QtConcurrentRun>

namespace {

const QString kDiscardSuffix = QStringLiteral(".discarded-");

// Move a finished run's directory aside so the path is free the instant we
// return, and hand the moved copy to a worker for the actual delete. A rename
// inside the same parent is one syscall; recursively deleting a checkout plus
// its build output is not, and it ran on the GUI thread.
QString moveAsideForDeletion(const QString &path)
{
    if (path.isEmpty() || !QDir(path).exists())
        return QString();
    const QString aside =
        path + kDiscardSuffix +
        QString::number(QDateTime::currentMSecsSinceEpoch());
    if (QDir().rename(path, aside))
        return aside;
    forkmesh::safedelete::removeTree(path, QFileInfo(path).absolutePath(), nullptr,
                                     forkmesh::safedelete::Allow::OwnWorktree);
    return QString();
}

// Deleting one happens on a worker, so a crash mid-delete strands the renamed
// copy where nothing will look at it again. Sweep the older ones the next time
// a run finishes; "older" keeps a sibling runner's in-flight delete alone.
// code-health: allow(dir-walk-in-loop) only called from the QtConcurrent::run worker in AgentRunner::cleanupWorktree()
void sweepStaleDiscards(const QString &worktreeRoot, qint64 nowMs)
{
    constexpr qint64 kKeepMs = 60 * 60 * 1000;
    const QDir dir(worktreeRoot);
    const QStringList names =
        dir.entryList({QLatin1Char('*') + kDiscardSuffix + QLatin1Char('*')},
                      QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &name : names) {
        bool parsed = false;
        const qint64 movedMs =
            name.section(kDiscardSuffix, -1).toLongLong(&parsed);
        if (!parsed || nowMs - movedMs < kKeepMs)
            continue;
        forkmesh::safedelete::removeTree(dir.absoluteFilePath(name), worktreeRoot, nullptr,
                                         forkmesh::safedelete::Allow::OwnWorktree);
    }
}

QString shellQuote(QString value)
{
    value.replace('\'', QStringLiteral("'\\''"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}

QStringList commandRuntimeFiles(const QString &command)
{
    // A configured provider command may invoke one of ForkMesh's staged
    // scripts outside the worktree. Grant only existing files explicitly
    // named as absolute shell arguments, never their containing directories.
    static const QRegularExpression token(
        QStringLiteral(
            R"REGEX((?:^|\s)(?:'([^']+)'|"([^"]+)"|((?:/|[A-Za-z]:[\\/])\S+)))REGEX"));
    QStringList files;
    QRegularExpressionMatchIterator matches = token.globalMatch(command);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        QString path = match.captured(1);
        if (path.isEmpty())
            path = match.captured(2);
        if (path.isEmpty())
            path = match.captured(3);
        const QFileInfo info(path);
        if (!QDir::isAbsolutePath(path) || !info.isFile())
            continue;
        const QString resolved = info.canonicalFilePath();
        if (!resolved.isEmpty() && !files.contains(resolved))
            files.append(resolved);
    }
    return files;
}

bool runCapture(const QString &dir, const QStringList &args, QByteArray *out,
                QString *error = nullptr)
{
    QProcess process;
    process.start(QStringLiteral("git"), QStringList{QStringLiteral("-C"), dir} + args);
    if (!process.waitForFinished(10000)) {
        process.kill();
        process.waitForFinished(1000);
        if (error)
            *error = QStringLiteral("git timed out");
        return false;
    }
    if (out)
        *out = process.readAllStandardOutput();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error)
            *error = QString::fromUtf8(process.readAllStandardError()).trimmed();
        return false;
    }
    return true;
}

bool runWithInput(const QString &dir, const QStringList &args,
                  const QByteArray &input, QString *error = nullptr)
{
    QProcess process;
    process.start(QStringLiteral("git"), QStringList{QStringLiteral("-C"), dir} + args);
    if (!process.waitForStarted(10000)) {
        if (error)
            *error = process.errorString();
        return false;
    }
    process.write(input);
    process.closeWriteChannel();
    if (!process.waitForFinished(10000)) {
        process.kill();
        process.waitForFinished(1000);
        if (error)
            *error = QStringLiteral("git timed out");
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error)
            *error = QString::fromUtf8(process.readAllStandardError()).trimmed();
        return false;
    }
    return true;
}

bool branchExists(const QString &dir, const QString &branchName)
{
    if (branchName.trimmed().isEmpty())
        return false;
    QByteArray ignored;
    return runCapture(dir,
                      {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                       QStringLiteral("--quiet"),
                       QStringLiteral("refs/heads/%1").arg(branchName)},
                      &ignored, nullptr);
}

QString worktreePathForBranch(const QString &repoPath, const QString &branch)
{
    if (branch.trimmed().isEmpty())
        return QString();
    QByteArray out;
    if (!runCapture(repoPath,
                    {QStringLiteral("worktree"), QStringLiteral("list"),
                     QStringLiteral("--porcelain")},
                    &out, nullptr))
        return QString();
    const QString want = QStringLiteral("refs/heads/%1").arg(branch);
    const int pathPrefix = QStringLiteral("worktree ").size();
    const int branchPrefix = QStringLiteral("branch ").size();
    QString currentPath;
    const QList<QByteArray> lines = out.split('\n');
    for (const QByteArray &raw : lines) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (line.startsWith(QLatin1String("worktree ")))
            currentPath = line.mid(pathPrefix).trimmed();
        else if (line.startsWith(QLatin1String("branch "))) {
            if (line.mid(branchPrefix).trimmed() == want &&
                !currentPath.isEmpty() && currentPath != repoPath)
                return currentPath;
        }
    }
    return QString();
}

QString agentWorktreeParent(const QString &repoPath, const QString &path)
{
    QStringList roots{forkmesh::agentwt::root(repoPath)};
    if (forkmesh::vm::active())
        roots << forkmesh::vm::worktreeRoot();
    for (const QString &root : roots) {
        if (!root.trimmed().isEmpty() &&
            forkmesh::safedelete::refusal(path, root,
                                          forkmesh::safedelete::Allow::OwnWorktree)
                .isEmpty())
            return root;
    }
    return QString();
}

void releaseBranchWorktree(const QString &repoPath, const QString &branch)
{
    const QString leaked = worktreePathForBranch(repoPath, branch);
    const QString parent =
        leaked.isEmpty() ? QString() : agentWorktreeParent(repoPath, leaked);
    if (!parent.isEmpty()) {
        QProcess::execute(QStringLiteral("git"),
                          {QStringLiteral("-C"), repoPath, QStringLiteral("worktree"),
                           QStringLiteral("remove"), QStringLiteral("--force"), leaked});
        forkmesh::safedelete::removeTree(leaked, parent, nullptr,
                                         forkmesh::safedelete::Allow::OwnWorktree);
    }
    QProcess::execute(QStringLiteral("git"),
                      {QStringLiteral("-C"), repoPath, QStringLiteral("worktree"),
                       QStringLiteral("prune")});
}

QString providerTitle(const QString &provider)
{
    if (provider == QLatin1String("claude-code"))
        return QStringLiteral("Claude Code");
    if (provider == QLatin1String("codex"))
        return QStringLiteral("Codex");
    if (provider == QLatin1String("cloudflare-ai"))
        return QStringLiteral("Cloudflare AI");
    if (provider == QLatin1String("local-llama"))
        return QStringLiteral("Local Llama");
    if (provider.startsWith(QLatin1String("claude")))
        return QStringLiteral("Claude API");
    return QStringLiteral("OpenAI API");
}

} // namespace

AgentRunner::AgentRunner(AgentStore *store, QObject *parent)
    : QObject(parent), m_store(store)
{
}

void AgentRunner::start(const AgentSession &session, const Issue &issue,
                        const QString &repoPath, const Config &config)
{
    if (m_busy || !m_store)
        return;

    m_busy = true;
    m_stopping = false;
    m_attentionRaised = false;
    m_processStartedAtMs = 0;
    m_lastOutputAtMs = 0;
    m_currentProgram.clear();
    m_phase = Phase::Idle;
    m_session = session;
    m_restorePreviousPatch = m_session.startedAtMs > 0 || !m_session.baseRef.isEmpty();
    m_issue = issue;
    m_promptFilePath.clear();
    m_repoPath = repoPath;
    m_config = config;
    m_config.guardrails.provider = m_session.provider;
    m_config.guardrails.repositoryRoot = repoPath;
    const QString guardrailInstruction =
        forkmesh::agentguard::promptInstruction(m_config.guardrails);
    if (!guardrailInstruction.isEmpty())
        m_config.promptPreamble =
            m_config.promptPreamble.trimmed().isEmpty()
                ? guardrailInstruction
                : m_config.promptPreamble + QStringLiteral("\n\n") +
                      guardrailInstruction;
    if (m_config.mode.trimmed().isEmpty())
        m_config.mode = m_session.mode;
    if (m_config.strength.trimmed().isEmpty())
        m_config.strength = m_session.strength;
    if (m_session.mode.trimmed().isEmpty())
        m_session.mode = m_config.mode;
    if (m_session.strength.trimmed().isEmpty())
        m_session.strength = m_config.strength;
    if (m_session.model.trimmed().isEmpty())
        m_session.model = m_config.model;
    m_prompt = buildPrompt();

    // Bank the turn this one replaces before overwriting the stamps beginRun()
    // reads, so a session resumed for a fifth follow-up still reports the wall
    // clock of all five rather than of the one in flight.
    const qint64 runStartMs = QDateTime::currentMSecsSinceEpoch();
    m_session.beginRun(runStartMs);
    m_session.status = AgentStatus::Running;
    m_session.startedAtMs = runStartMs;
    m_session.mergeCandidateHead.clear();
    m_session.contextWindow = m_config.contextWindow;
    m_session.maxOutputTokens = m_config.maxOutputTokens;
    m_session.promptTokens += estimateTokens(m_prompt);
    refreshUsage();
    m_session.spendBeforeUsd = m_session.costUsd;
    m_store->saveSession(m_session);
    emit statusChanged(m_session.id, m_session.status);

    emitLog(QStringLiteral("==> Agent: %1").arg(providerTitle(m_session.provider)));
    emitLog(QStringLiteral("==> Issue: #%1 %2")
                .arg(m_session.issueNumber)
                .arg(m_session.issueTitle));
    emitLog(QStringLiteral("==> Budget: context %1 tokens, max output %2 tokens")
                .arg(m_config.contextWindow)
                .arg(m_config.maxOutputTokens));
    if (!m_config.model.trimmed().isEmpty())
        emitLog(QStringLiteral("==> Model: %1").arg(m_config.model.trimmed()));
    emitLog(QStringLiteral("==> %1")
                .arg(launchIdentityInstruction(m_session.provider, m_config.model,
                                               m_config.mode, m_config.strength)));

    if (m_config.command.trimmed().isEmpty()) {
        complete(false, AgentStatus::Waiting,
                 QStringLiteral("No command is configured for this agent."));
        return;
    }
    if (!QDir(m_repoPath).exists(QStringLiteral(".git")) &&
        !QDir(m_repoPath).exists(QStringLiteral("HEAD"))) {
        complete(false, AgentStatus::Failed,
                 QStringLiteral("No git checkout is available for this repo."));
        return;
    }
    if (forkmesh::vm::active() && !forkmesh::vm::pathIsShared(m_repoPath)) {
        complete(false, AgentStatus::Failed,
                 forkmesh::vm::pathError(m_repoPath));
        return;
    }

    if (m_session.branchName.isEmpty()) {
        m_session.branchName =
            QStringLiteral("agent/issue-%1-%2-%3")
                .arg(m_session.issueNumber)
                .arg(m_session.provider)
                .arg(m_session.id);
    }

    const bool existingSessionBranch = branchExists(m_repoPath, m_session.branchName);
    if (m_session.baseRef.isEmpty()) {
        QByteArray base;
        QString gitError;
        if (!runCapture(m_repoPath, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")},
                        &base, &gitError)) {
            complete(false, AgentStatus::Failed,
                     QStringLiteral("Could not read the base commit: %1").arg(gitError));
            return;
        }
        m_session.baseRef = QString::fromUtf8(base).trimmed();
        QByteArray branch;
        if (runCapture(m_repoPath,
                       {QStringLiteral("rev-parse"), QStringLiteral("--abbrev-ref"),
                        QStringLiteral("HEAD")},
                       &branch, nullptr)) {
            const QString name = QString::fromUtf8(branch).trimmed();
            if (!name.isEmpty() && name != QLatin1String("HEAD"))
                m_session.baseBranch = name;
        }
        if (m_session.baseBranch.isEmpty())
            m_session.baseBranch = m_session.baseRef;
    } else if (m_session.baseBranch.isEmpty()) {
        m_session.baseBranch = m_session.baseRef;
    }
    m_store->saveSession(m_session);

    if (existingSessionBranch && !m_session.baseRef.isEmpty()) {
        const QString leaked = worktreePathForBranch(m_repoPath, m_session.branchName);
        if (!leaked.isEmpty() && QDir(leaked).exists() &&
            m_store->readPatch(m_session).trimmed().isEmpty()) {
            QByteArray wip;
            if (runCapture(leaked,
                           {QStringLiteral("diff"), QStringLiteral("--binary"),
                            m_session.baseRef},
                           &wip, nullptr) &&
                !QString::fromUtf8(wip).trimmed().isEmpty()) {
                m_store->writePatch(m_session, QString::fromUtf8(wip));
                emitLog(QStringLiteral(
                    "==> Recovered in-flight changes from the interrupted run."));
            }
        }
    }
    releaseBranchWorktree(m_repoPath, m_session.branchName);

    const QString worktreeBase = forkmesh::vm::active()
                                     ? forkmesh::vm::worktreeRoot()
                                     : forkmesh::agentwt::root(m_repoPath);
    forkmesh::agentwt::ensureRoot(worktreeBase);
    m_worktree = QDir(worktreeBase)
                     .filePath(m_session.worktreeName.isEmpty()
                                   ? forkmesh::agentwt::dirName(
                                         m_session.id, m_session.issueTitle)
                                   : m_session.worktreeName);
    forkmesh::safedelete::removeTree(m_worktree, worktreeBase, nullptr,
                                     forkmesh::safedelete::Allow::OwnWorktree);
    emitLog(QStringLiteral("==> Creating worktree %1").arg(m_worktree));
    QStringList args{QStringLiteral("-C"), m_repoPath, QStringLiteral("worktree"),
                     QStringLiteral("add")};
    if (existingSessionBranch) {
        args << m_worktree << m_session.branchName;
    } else {
        args << QStringLiteral("-B") << m_session.branchName << m_worktree
             << m_session.baseRef;
    }
    launch(Phase::Worktree, QStringLiteral("git"), args);
}

qint64 AgentRunner::processId() const
{
    return m_process && m_process->state() != QProcess::NotRunning
               ? m_process->processId()
               : 0;
}

// code-health: allow(blocking-wait) user stop path; waits at most 1.5 s for the terminated process before killing it
void AgentRunner::stop()
{
    if (!m_busy)
        return;
    m_stopping = true;
    emitLog(QStringLiteral("==> Stop requested by user."));
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->terminate();
        if (!m_process->waitForFinished(1500))
            m_process->kill();
        return;
    }
    complete(false, AgentStatus::Stopped, QStringLiteral("Stopped."));
}

bool AgentRunner::steer(const QString &prompt)
{
    const QString trimmed = prompt.trimmed();
    if (trimmed.isEmpty())
        return true;
    if (!m_busy)
        return false;
    m_session.promptTokens += estimateTokens(trimmed);
    refreshUsage();
    m_store->saveSession(m_session);
    emitLog(QStringLiteral("\n==> User steering prompt\n%1").arg(trimmed));
    if (m_process && m_process->state() == QProcess::Running &&
        m_process->isWritable()) {
        const QString text =
            QStringLiteral("\n\nAdditional user instruction:\n%1\n").arg(trimmed);
        if (m_process->write(text.toUtf8()) >= 0)
            return true;
    }
    emitLog(QStringLiteral("==> Agent process is not accepting input right now; "
                           "the message is held for the next run."));
    return false;
}

QProcessEnvironment AgentRunner::processEnvironment(Phase phase) const
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (m_config.preferApiKeyAuth && !m_config.isolatedHome.isEmpty()) {
        const QString isolatedHome =
            forkmesh::vm::active()
                ? m_worktree + QStringLiteral("-codex-home")
                : m_config.isolatedHome;
        QDir().mkpath(isolatedHome);
        env.insert(QStringLiteral("CODEX_HOME"), isolatedHome);
    }
    if (phase == Phase::Agent && !forkmesh::vm::active()) {
        for (const QString &kv : forkmesh::agentscratch::environment(
                 forkmesh::agentscratch::folderFor(m_worktree))) {
            const int eq = kv.indexOf(QLatin1Char('='));
            env.insert(kv.left(eq), kv.mid(eq + 1));
        }
    }
    if (phase == Phase::Agent &&
        (m_config.jailMemoryMb > 0 || m_config.guardrails.enforced())) {
        const QStringList jailEnv =
            AgentJail::envEntries(m_worktree + QStringLiteral("-jail"));
        for (const QString &kv : jailEnv) {
            const int eq = kv.indexOf(QLatin1Char('='));
            env.insert(kv.left(eq), kv.mid(eq + 1));
        }
    }
    // Provider credentials come only from the selected ForkMesh account/key,
    // never from an unrelated inherited shell variable.
    const QString provider = m_session.provider;
    env.remove(QStringLiteral("ANTHROPIC_API_KEY"));
    env.remove(QStringLiteral("ANTHROPIC_AUTH_TOKEN"));
    env.remove(QStringLiteral("ANTHROPIC_ADMIN_KEY"));
    env.remove(QStringLiteral("CLAUDE_CODE_OAUTH_TOKEN"));
    env.remove(QStringLiteral("CODEX_API_KEY"));
    env.remove(QStringLiteral("OPENAI_API_KEY"));
    env.remove(QStringLiteral("OPENAI_ACCESS_TOKEN"));
    env.remove(QStringLiteral("OPENAI_ADMIN_KEY"));
    if (!m_config.apiKeyName.isEmpty())
        env.remove(m_config.apiKeyName);
    const bool usingConfiguredKey = !m_config.apiKeyName.isEmpty() &&
                                    !m_config.apiKey.trimmed().isEmpty();
    if (usingConfiguredKey) {
        const QString key = m_config.apiKey.trimmed();
        env.insert(m_config.apiKeyName, key);
        if (provider == QLatin1String("openai") ||
            provider == QLatin1String("codex"))
            env.insert(QStringLiteral("OPENAI_API_KEY"), key);
    }
    if (!m_config.model.trimmed().isEmpty())
        env.insert(QStringLiteral("FORKMESH_AGENT_MODEL"),
                   m_config.model.trimmed());
    env.insert(QStringLiteral("FORKMESH_AGENT_PROVIDER"), provider);
    if (!m_config.endpoint.trimmed().isEmpty())
        env.insert(QStringLiteral("FORKMESH_AGENT_ENDPOINT"),
                   m_config.endpoint.trimmed());
    if (!m_config.thinking.trimmed().isEmpty())
        env.insert(QStringLiteral("FORKMESH_AGENT_THINKING"),
                   m_config.thinking.trimmed());
    env.insert(QStringLiteral("FORKMESH_AGENT_CONTEXT_WINDOW"),
               QString::number(m_config.contextWindow));
    env.insert(QStringLiteral("FORKMESH_AGENT_MAX_OUTPUT_TOKENS"),
               QString::number(m_config.maxOutputTokens));
    env.remove(QStringLiteral("FORKMESH_AGENT_ALLOWED_DIRECTORIES"));
    if (m_config.guardrails.enabled) {
        env.insert(
            QStringLiteral("FORKMESH_AGENT_ALLOWED_DIRECTORIES"),
            QString::fromUtf8(
                QJsonDocument(QJsonArray::fromStringList(
                                  m_config.guardrails.allowedDirectories))
                    .toJson(QJsonDocument::Compact)));
    }
    // Same directory set the "is it installed?" check uses, so the runner can
    // never fail to exec a CLI the UI reported as present.
    forkmesh::agentcli::widenPath(env);
    if (phase == Phase::Agent) {
        for (const QString &kv : m_config.extraEnvironment) {
            const int eq = kv.indexOf(QLatin1Char('='));
            if (eq > 0)
                env.insert(kv.left(eq), kv.mid(eq + 1));
            else if (eq < 0 && !kv.isEmpty())
                env.remove(kv);
        }
        forkmesh::vm::applyGuestEnvironmentPolicy(env);
        forkmesh::agentresources::applyEnvironment(env);
    }
    return env;
}

void AgentRunner::launch(Phase phase, const QString &program,
                         const QStringList &args, const QString &workingDir)
{
    m_phase = phase;
    m_processStartedAtMs = 0;
    m_lastOutputAtMs = 0;
    m_currentProgram = program;
    endCoreParse();
    m_coreKey.clear();
    AgentProcess::Options options;
    if (phase == Phase::Agent && forkmesh::coreagentrun::available()) {
        m_coreKey = QStringLiteral("runner-%1-%2")
                        .arg(m_session.id)
                        .arg(QDateTime::currentMSecsSinceEpoch());
        options.key = m_coreKey;
        options.detachable = true;
        options.meta = coreProcessMeta();
        forkmesh::coreagentrun::pushCredentials();
    }
    m_process = AgentProcess::create(this, options);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    if (!workingDir.isEmpty())
        m_process->setWorkingDirectory(workingDir);

    m_process->setProcessEnvironment(processEnvironment(phase));
    connectProcess(phase, program);

    m_process->setProgram(program);
    m_process->setArguments(args);
    m_process->start();
    if (phase == Phase::Agent) {
        if (!m_noOutputTimer) {
            m_noOutputTimer = new QTimer(this);
            m_noOutputTimer->setInterval(15000);
            connect(m_noOutputTimer, &QTimer::timeout, this,
                    &AgentRunner::onNoOutputTimeout);
        }
        emitLog(QStringLiteral("==> Waiting for agent output..."));
        m_noOutputTimer->start();
    }
}

QJsonObject AgentRunner::coreProcessMeta() const
{
    QJsonObject meta{{QStringLiteral("runtime"), AgentRuntimeIds::ManagedRunner},
                     {QStringLiteral("parse"), QStringLiteral("log")},
                     {QStringLiteral("sessionId"), QString::number(m_session.id)},
                     {QStringLiteral("storeKey"), m_session.repoKey()},
                     {QStringLiteral("provider"), m_session.provider},
                     {QStringLiteral("repoPath"), m_repoPath},
                     {QStringLiteral("worktree"), m_worktree},
                     {QStringLiteral("promptFile"), m_promptFilePath},
                     {QStringLiteral("program"), m_currentProgram}};
    if (!m_config.apiKey.isEmpty())
        meta.insert(QStringLiteral("_secrets"), QJsonArray{m_config.apiKey});
    return meta;
}

void AgentRunner::handleOutput(const QString &chunk, bool store)
{
    m_lastOutputAtMs = QDateTime::currentMSecsSinceEpoch();
    if (store)
        emitLog(chunk);
    else
        emit logLine(m_session.id, chunk);
    const QString hint = detectAuthIssue(chunk);
    if (!hint.isEmpty()) {
        emitLog(QStringLiteral("==> ") + hint);
        emit needsAttention(m_session.id, hint);
    }
}

void AgentRunner::finishProcess(int exitCode)
{
    if (m_noOutputTimer)
        m_noOutputTimer->stop();
    endCoreParse();
    onProcessFinished(exitCode);
}

void AgentRunner::beginCoreParse()
{
    if (m_coreKey.isEmpty())
        return;
    const QString key = m_coreKey;
    m_coreAttachPending = true;
    QPointer<AgentRunner> guard(this);
    forkmesh::coreagentrun::subscribe(key, this, [guard, key](const QString &event, const QJsonObject &data) {
        if (guard && guard->m_coreKey == key)
            guard->onCoreRunEvent(event, data);
    });
    forkmesh::coreagentrun::attach(
        key, 0, this, [guard, key](bool ok, const QJsonObject &, const QString &) {
            if (!guard || guard->m_coreKey != key)
                return;
            guard->m_coreAttachPending = false;
            guard->m_coreParsed = ok;
            if (ok)
                return;
            forkmesh::coreagentrun::unsubscribe(key, guard);
            if (guard->m_hasDeferredExit) {
                guard->m_hasDeferredExit = false;
                guard->finishProcess(guard->m_deferredExit);
            }
        });
}

void AgentRunner::endCoreParse()
{
    if (!m_coreKey.isEmpty() && (m_coreParsed || m_coreAttachPending)) {
        forkmesh::coreagentrun::unsubscribe(m_coreKey, this);
        forkmesh::coreagentrun::detach(m_coreKey);
    }
    m_coreParsed = false;
    m_coreAttachPending = false;
    m_hasDeferredExit = false;
}

void AgentRunner::onCoreRunEvent(const QString &event, const QJsonObject &data)
{
    if (event == QLatin1String("core.agentrun.reconnected")) {
        if (m_coreParsed) {
            m_coreParsed = false;
            beginCoreParse();
        }
        return;
    }
    if (event == QLatin1String("core.agentrun.exit")) {
        if (!m_coreParsed && !m_coreAttachPending)
            return;
        m_hasDeferredExit = false;
        finishProcess(data.value(QStringLiteral("code")).toInt());
        return;
    }
    if (event != QLatin1String("core.agentrun.event"))
        return;
    const QString text = data.value(QStringLiteral("text")).toString();
    if (!text.isEmpty())
        handleOutput(text, false);
}

bool AgentRunner::attachCoreProcess(const AgentSession &session, const Config &config,
                                    const QString &key, const QJsonObject &meta, qint64 pid)
{
    if (m_busy || key.isEmpty())
        return false;
    AgentProcess::Options options;
    options.attachKey = key;
    options.detachable = true;
    options.pid = pid;
    options.meta = meta;
    AgentProcess *process = AgentProcess::create(this, options);
    if (!process)
        return false;
    m_session = session;
    m_config = config;
    m_issue = Issue();
    m_repoPath = meta.value(QStringLiteral("repoPath")).toString();
    m_worktree = meta.value(QStringLiteral("worktree")).toString();
    m_promptFilePath = meta.value(QStringLiteral("promptFile")).toString();
    m_currentProgram = meta.value(QStringLiteral("program")).toString();
    m_prompt.clear();
    m_restorePreviousPatch = false;
    m_attentionRaised = false;
    m_stopping = false;
    m_busy = true;
    m_phase = Phase::Agent;
    m_process = process;
    m_coreKey = key;
    m_processStartedAtMs = QDateTime::currentMSecsSinceEpoch();
    m_lastOutputAtMs = m_processStartedAtMs;
    connectProcess(Phase::Agent, m_currentProgram);
    beginCoreParse();
    if (!m_noOutputTimer) {
        m_noOutputTimer = new QTimer(this);
        m_noOutputTimer->setInterval(15000);
        connect(m_noOutputTimer, &QTimer::timeout, this, &AgentRunner::onNoOutputTimeout);
    }
    m_noOutputTimer->start();
    emitLog(QStringLiteral("==> Re-attached to the agent process (pid %1) after the ForkMesh restart.")
                .arg(pid));
    return true;
}

void AgentRunner::connectProcess(Phase phase, const QString &program)
{
    connect(m_process, &AgentProcess::readyReadStandardOutput, this, [this] {
        handleOutput(QString::fromUtf8(m_process->readAllStandardOutput()), true);
    });
    connect(m_process, &AgentProcess::finished, this,
            [this](int exitCode, QProcess::ExitStatus) {
                if (m_coreParsed)
                    return;
                if (m_coreAttachPending) {
                    m_hasDeferredExit = true;
                    m_deferredExit = exitCode;
                    return;
                }
                finishProcess(exitCode);
            });
    connect(m_process, &AgentProcess::started, this, [this] {
        m_processStartedAtMs = QDateTime::currentMSecsSinceEpoch();
        m_lastOutputAtMs = m_processStartedAtMs;
        // The follow-up is no longer merely handed to a prospective process:
        // the actual agent command has accepted its prompt file. Clear the
        // durable in-flight copy now. If setup or launch fails before this
        // signal, it remains attached to the session for Continue/app restart.
        if (m_phase == Phase::Agent &&
            !m_session.inFlightSteerMessage.isEmpty()) {
            m_session.inFlightSteerMessage.clear();
            m_store->saveSession(m_session);
        }
        emitLog(QStringLiteral("==> Started process pid %1: %2")
                    .arg(m_process ? m_process->processId() : 0)
                    .arg(m_currentProgram));
        if (m_process && !m_coreKey.isEmpty() && m_process->parsedByCore())
            beginCoreParse();
    });
    connect(m_process, &AgentProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (m_noOutputTimer)
                    m_noOutputTimer->stop();
                if (!m_process)
                    return;
                if (!m_stopping)
                    emitLog(QStringLiteral("!! ") + m_process->errorString());
                if (error == QProcess::FailedToStart)
                    complete(false, AgentStatus::Failed, m_process->errorString());
            });

    const quint64 activity = forkmesh::BackgroundActivity::begin(
        phase == Phase::Agent ? QStringLiteral("agent") : QStringLiteral("git"),
        QStringLiteral("Agent session #%1: %2").arg(m_session.id).arg(program));
    connect(m_process, &QObject::destroyed, this,
            [activity] { forkmesh::BackgroundActivity::end(activity); });
}

void AgentRunner::onProcessFinished(int exitCode)
{
    if (m_process) {
        const QByteArray tail = m_process->readAllStandardOutput();
        if (!tail.isEmpty())
            emitLog(QString::fromUtf8(tail));
        m_process->deleteLater();
        m_process = nullptr;
    }
    if (m_stopping) {
        complete(false, AgentStatus::Stopped, QStringLiteral("Stopped."));
        return;
    }

    if (m_phase == Phase::Worktree) {
        if (exitCode != 0) {
            complete(false, AgentStatus::Failed,
                     QStringLiteral("Could not create the worktree."));
            return;
        }
        emit worktreeReady(m_session.id, m_worktree);
        if (!restorePreviousPatch())
            return;
        runAgentProcess();
        return;
    }

    if (exitCode != 0) {
        QString message =
            QStringLiteral("Agent command exited with code %1.").arg(exitCode);
        const QString log = m_store ? m_store->readLog(m_session) : QString();
        if (!m_session.provider.startsWith(QLatin1String("claude")) &&
            log.contains(QStringLiteral("401 Unauthorized"), Qt::CaseInsensitive)) {
            message += QStringLiteral(
                " OpenAI rejected the API key; rotate it and re-enter it in Settings.");
        } else if (!m_session.provider.startsWith(QLatin1String("claude")) &&
                   (log.contains(QStringLiteral("quota exceeded"),
                                 Qt::CaseInsensitive) ||
                    log.contains(QStringLiteral("insufficient_quota"),
                                 Qt::CaseInsensitive) ||
                    log.contains(QStringLiteral("billing details"),
                                 Qt::CaseInsensitive))) {
            message += QStringLiteral(
                " OpenAI reports that the API-key-backed account is out of quota. "
                "Use the Codex provider to run your logged-in Codex CLI account, "
                "or update the OpenAI API key/billing project in Settings.");
        }
        complete(false, AgentStatus::Failed, message);
        return;
    }

    // Attribute the commits this run produced to the agent before capturing the
    // patch, so the provenance trailer is carried by the signed commit series.
    stampAgentProvenance();

    QByteArray diff;
    QString diffError;
    if (runCapture(m_worktree,
                   {QStringLiteral("diff"), QStringLiteral("--binary"),
                    m_session.baseRef},
                   &diff, &diffError)) {
        const QString patch = QString::fromUtf8(diff);
        m_store->writePatch(m_session, patch);
        emitLog(patch.trimmed().isEmpty()
                    ? QStringLiteral("==> No code changes were produced.")
                    : QStringLiteral("==> Captured patch for PR creation."));
    } else {
        emitLog(QStringLiteral("!! Could not capture patch: %1").arg(diffError));
    }

    complete(true, AgentStatus::Success, QStringLiteral("Agent session completed."));
}

void AgentRunner::runAgentProcess()
{
    const QString promptPath =
        (m_session.provider == QLatin1String("cloudflare-ai") ||
         m_session.provider == QLatin1String("local-llama"))
            ? QDir::temp().filePath(QStringLiteral(".forkmesh-agent-prompt-") +
                                    QString::number(QDateTime::currentMSecsSinceEpoch()) +
                                    QStringLiteral(".md"))
            : m_worktree + QStringLiteral("/.forkmesh-agent-prompt.md");
    m_promptFilePath = promptPath;
    QFile promptFile(promptPath);
    if (!promptFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        complete(false, AgentStatus::Failed,
                 QStringLiteral("Could not write the prompt file."));
        return;
    }
    promptFile.write(m_prompt.toUtf8());
    promptFile.close();

    if (m_session.provider == QLatin1String("local-llama")) {
        emitLog(QStringLiteral("==> Using the local Llama endpoint %1.")
                    .arg(m_config.endpoint));
    } else if (!m_config.apiKey.isEmpty() && m_config.preferApiKeyAuth &&
        !m_config.isolatedHome.isEmpty()) {
        emitLog(QStringLiteral("==> Using %1 with isolated CLI home for API-key auth.")
                    .arg(m_config.apiKeyName));
    } else if (m_config.apiKey.isEmpty()) {
        emitLog(QStringLiteral("==> No API key set; relying on the local CLI auth if available."));
    }

    const QString command = expandCommand(promptPath);
    emitLog(QStringLiteral("==> Running configured command."));
    if (m_config.guardrails.enabled)
        emitLog(QStringLiteral("==> Repository guardrails: allow only %1.")
                    .arg(forkmesh::agentguard::policyDescription(
                        m_config.guardrails.allowedDirectories)));
#ifdef Q_OS_WIN
    const QString shell = QStringLiteral("cmd");
    const QStringList shellArguments{QStringLiteral("/c"), command};
#else
    if (m_config.jailMemoryMb > 0)
        emitLog(QStringLiteral(
                    "==> Jailed: private scratch env, memory capped at %1 MB.")
                    .arg(m_config.jailMemoryMb));
    const QString shell =
        QFile::exists(QStringLiteral("/bin/bash")) ? QStringLiteral("/bin/bash")
                                                   : QStringLiteral("/bin/sh");
    const QStringList shellArguments{
        QStringLiteral("-lc"),
        forkmesh::agentresources::wrapShellCommand(
            AgentJail::wrapCommand(command, m_config.jailMemoryMb))};
#endif
    const forkmesh::agentguard::LaunchCommand guarded =
        forkmesh::agentguard::restrictedCommand(
            shell, shellArguments, m_worktree, m_config.guardrails,
            processEnvironment(Phase::Agent),
            QStringList{m_promptFilePath} +
                commandRuntimeFiles(m_config.command));
    if (!guarded.error.isEmpty()) {
        complete(false, AgentStatus::Failed, guarded.error);
        return;
    }
    const forkmesh::vm::LaunchCommand launchCommand =
        forkmesh::vm::isolateCommand(guarded.program, guarded.arguments,
                                     m_worktree);
    if (!launchCommand.error.isEmpty()) {
        complete(false, AgentStatus::Failed, launchCommand.error);
        return;
    }
    if (launchCommand.isolated)
        emitLog(QStringLiteral("==> KVM isolated in Lima instance %1.")
                    .arg(forkmesh::vm::instanceName()));
    const forkmesh::agentresources::Launch contained =
        launchCommand.isolated
            ? forkmesh::agentresources::Launch{launchCommand.program,
                                               launchCommand.arguments}
            : forkmesh::agentresources::contain(
                  launchCommand.program, launchCommand.arguments,
                  QStringLiteral("%1 %2").arg(m_session.provider).arg(m_session.id));
    if (contained.contained)
        emitLog(QStringLiteral("==> Memory contained in %1.").arg(contained.unit));
    launch(Phase::Agent, contained.program, contained.arguments, m_worktree);
}

bool AgentRunner::restorePreviousPatch()
{
    if (!m_store || !m_restorePreviousPatch)
        return true;

    const QString patch = m_store->readPatch(m_session);
    if (patch.trimmed().isEmpty())
        return true;

    QByteArray currentDiff;
    if (runCapture(m_worktree,
                   {QStringLiteral("diff"), QStringLiteral("--binary"),
                    m_session.baseRef},
                   &currentDiff, nullptr) &&
        !currentDiff.trimmed().isEmpty()) {
        emitLog(QStringLiteral("==> Existing session branch already has changes; skipping saved patch restore."));
        return true;
    }

    const QByteArray patchBytes = patch.toUtf8();
    QString error;
    if (!runWithInput(m_worktree,
                      {QStringLiteral("apply"), QStringLiteral("--check"),
                       QStringLiteral("--binary")},
                      patchBytes, &error)) {
        complete(false, AgentStatus::Failed,
                 QStringLiteral("Could not restore the previous session patch: %1")
                     .arg(error));
        return false;
    }
    if (!runWithInput(m_worktree,
                      {QStringLiteral("apply"), QStringLiteral("--binary")},
                      patchBytes, &error)) {
        complete(false, AgentStatus::Failed,
                 QStringLiteral("Could not restore the previous session patch: %1")
                     .arg(error));
        return false;
    }
    emitLog(QStringLiteral("==> Restored previous session patch."));
    return true;
}

void AgentRunner::complete(bool ok, const QString &status, const QString &message)
{
    if (!m_busy && m_phase == Phase::Idle)
        return;
    endCoreParse();
    if (m_process) {
        if (m_noOutputTimer)
            m_noOutputTimer->stop();
        m_process->disconnect(this);
        m_process->deleteLater();
        m_process = nullptr;
    }
    emitLog((ok ? QStringLiteral("==> SUCCESS: ") : QStringLiteral("==> ")) + message);
    refreshUsage();
    m_session.spendAfterUsd = m_session.costUsd;
    const double runCost = m_session.spendAfterUsd - m_session.spendBeforeUsd;
    emitLog(QStringLiteral(
                "==> Task cost: $%1 (spend before $%2 -> after $%3, this run +$%4)")
                .arg(m_session.costUsd, 0, 'f', 4)
                .arg(m_session.spendBeforeUsd, 0, 'f', 4)
                .arg(m_session.spendAfterUsd, 0, 'f', 4)
                .arg(runCost, 0, 'f', 4));
    cleanupWorktree();
    m_session.status = status;
    m_session.finishedAtMs = QDateTime::currentMSecsSinceEpoch();
    if (!ok)
        m_session.lastError = message;
    m_store->saveSession(m_session);
    m_phase = Phase::Idle;
    m_busy = false;
    emit statusChanged(m_session.id, m_session.status);
    emit finished(m_session.id, ok);
}

// code-health: allow(dir-walk-in-loop) the removeRecursively loop runs inside the QtConcurrent::run worker below
void AgentRunner::cleanupWorktree()
{
    if (m_worktree.isEmpty())
        return;
    // Before anything moves: for cloudflare-ai this file lives in the temp
    // directory rather than under the worktree, so it needs its own removal.
    if (!m_promptFilePath.isEmpty()) {
        QFile::remove(m_promptFilePath);
        m_promptFilePath.clear();
    }
    // The worktree path is derived from the session id, so the next run of this
    // session reuses it — move each directory aside first, then delete off the
    // GUI thread. `worktree prune` (not `remove`) drops git's administrative
    // record, because the checkout is no longer where git recorded it.
    QStringList discarded;
    for (const QString &path : {forkmesh::agentscratch::folderFor(m_worktree), m_worktree,
                                m_worktree + QStringLiteral("-jail"),
                                m_worktree + QStringLiteral("-codex-home")}) {
        const QString aside = moveAsideForDeletion(path);
        if (!aside.isEmpty())
            discarded << aside;
    }
    const QString worktreeRoot = QFileInfo(m_worktree).absolutePath();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    (void)QtConcurrent::run([repoPath = m_repoPath, discarded, worktreeRoot,
                             nowMs] {
        for (const QString &path : discarded)
            forkmesh::safedelete::removeTree(path, worktreeRoot, nullptr,
                                             forkmesh::safedelete::Allow::OwnWorktree);
        forkmesh::agentscratch::sweepOrphans(worktreeRoot);
        QProcess::execute(QStringLiteral("git"),
                          {QStringLiteral("-C"), repoPath,
                           QStringLiteral("worktree"), QStringLiteral("prune")});
        sweepStaleDiscards(worktreeRoot, nowMs);
    });
    m_worktree.clear();
}

QString AgentRunner::agentProvenanceValue() const
{
    QString tool = m_session.provider.trimmed();
    if (tool.isEmpty())
        tool = QStringLiteral("agent");
    const QString model = m_config.model.trimmed();
    QString value = model.isEmpty() ? tool : tool + QLatin1Char('/') + model;
    value.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._+/ -]")),
                  QString());
    return value;
}

void AgentRunner::stampAgentProvenance()
{
    if (m_worktree.isEmpty() || m_session.baseRef.isEmpty())
        return;
    QByteArray dirty;
    if (runCapture(m_worktree, {QStringLiteral("status"), QStringLiteral("--porcelain")},
                   &dirty, nullptr) &&
        !QString::fromUtf8(dirty).trimmed().isEmpty()) {
        emitLog(QStringLiteral(
            "==> Skipping agent provenance trailer: worktree has uncommitted changes."));
        return;
    }
    QByteArray count;
    if (!runCapture(m_worktree,
                    {QStringLiteral("rev-list"), QStringLiteral("--count"),
                     m_session.baseRef + QStringLiteral("..HEAD")},
                    &count, nullptr) ||
        QString::fromUtf8(count).trimmed().toInt() <= 0)
        return;

    const QString value = agentProvenanceValue();
    const QString exec =
        QStringLiteral("git commit --amend --no-edit --trailer 'ForkMesh-Agent: %1'")
            .arg(value);
    QProcess process;
    process.start(QStringLiteral("git"),
                  {QStringLiteral("-C"), m_worktree, QStringLiteral("rebase"),
                   m_session.baseRef, QStringLiteral("--exec"), exec});
    if (!process.waitForFinished(60000) || process.exitCode() != 0) {
        QProcess::execute(QStringLiteral("git"),
                          {QStringLiteral("-C"), m_worktree,
                           QStringLiteral("rebase"), QStringLiteral("--abort")});
        emitLog(QStringLiteral("==> Could not stamp agent provenance trailer (%1).")
                    .arg(QString::fromUtf8(process.readAllStandardError()).trimmed()));
        return;
    }
    emitLog(QStringLiteral("==> Stamped ForkMesh-Agent provenance trailer: %1").arg(value));
}

QString AgentRunner::defaultPromptPreamble()
{
    return QStringLiteral(
        "You are running inside ForkMesh as a coding agent.\n"
        "Use the minimum context and output needed. Inspect only files relevant to the issue.\n"
        "Do not spend extra tokens on broad refactors or unrelated cleanup.\n"
        "When possible, make the smallest patch that satisfies the issue.\n") +
           commitChangesInstruction();
}

// Conventions for the tool-efficiency signals measured on real runs: whole-file
// and oversized reads (including saved tool output), re-reads, guessed paths,
// shell probes whose last command fails, parallel batches cancelled by one
// failure, relative paths broken by a persisted cd, self-killing pkill,
// failing heredoc scripts, foreground sleeps and until-loops, whole-disk
// finds, unbounded shell reads by agents without Read (Codex) and whole-file
// rewrites; plus a nudge toward the dedicated, parallel, subagent and deferred
// tools. Appended even when the preamble is custom.
QString AgentRunner::toolUseInstruction()
{
    return QStringLiteral(
        "Tool use:\n"
        "- Prefer dedicated tools: Glob to find files, Grep to find symbols, Read "
        "(absolute path, offset/limit) for only the lines you need, Edit/Write to "
        "change files. Use Bash for git, builds and tests.\n"
        "- Keep a Read range to about 300 lines around the match; page on only if "
        "needed. Read a whole file only when it is under ~300 lines: for a larger "
        "source file, or a docs page you will add to, Grep -n the symbol or its "
        "headings (^#) first and Read that section. Bound command output with "
        "tail, head or grep; when a long result is saved to a tool-results file, "
        "Grep it or Read a range, never all of it.\n"
        "- Without Glob, Grep and Read (Codex), the shell is the reader: find "
        "files with rg -l or rg --files, pipe every search through | head -n 60 "
        "(rg -m counts per file, not in total), add --max-columns 200 and leave "
        "out data, logs and build trees, and print ~150-line sed -n ranges, "
        "~300 lines per call in total. head and sed -n bound lines, not "
        "bytes: cut -c1-300 logs, JSONL and minified files. git show --stat "
        "first, then one path; git status --short | head -50. With several "
        "files, write tail -n N (tail -N fails).\n"
        "- Change files with small anchored edits (Edit, or apply_patch hunks "
        "around the changed lines), not a rewrite of most of the file; Write "
        "refuses an existing file not Read first, so use it for new files. Plan mode "
        "refuses every Edit and Write: read and plan, then ExitPlanMode.\n"
        "- Make independent calls in parallel in one turn; hand broad multi-directory "
        "searches to an Explore subagent via Agent, at most ten subagents at once.\n"
        "- Load deferred tools (Monitor, WebFetch, ForkMesh MCP) with ToolSearch "
        "before calling them.\n"
        "- Do not re-read files already in context or guess paths; issue attachments "
        "exist only where an \"Attached image:\" line points.\n"
        "- Shell probes: a grep with no match, an ls of a missing path and grep -r on "
        "a missing directory all exit non-zero, and 2>/dev/null hides the message but "
        "not the exit code; the last command sets the call's status, so end a probe "
        "chain with || true; probe a tool with command -v before relying on it. "
        "One failing call cancels its parallel siblings. Check "
        "a grep -n line number is non-empty before sed -n; never find / (search the "
        "repo, the build dir or a known prefix).\n"
        "- The working directory persists between Bash calls: a cd in one call moves "
        "every relative path in the next. Use absolute paths, git -C, or (cd dir && "
        "…) in a subshell, never a bare cd.\n"
        "- pkill -f and pgrep -f match the shell running them when the pattern is in "
        "its command line and kill the call (exit 144); bracket the first character "
        "([f]oo) or kill by pid.\n"
        "- Stay in your own area: your worktree and the scratch folder in "
        "$FORKMESH_AGENT_SCRATCH ($TMPDIR already points inside it). Put build "
        "trees, logs, test output, throwaway scripts and any extra checkout or "
        "worktree there, never in /tmp, ~/.cache or a shared builds folder; the "
        "folder is deleted with the agent. The harness refuses rm on a $VAR "
        "path or a relative glob after cd: spell the absolute path, or use "
        "fm_safe_rm_tree <parent> <path> (tools/forkmesh-safe-rm.sh).\n"
        "- Put a throwaway script longer than a few lines in a file under the "
        "scratch folder (Write), run it by path and fix it with Edit; a failing "
        "heredoc costs its whole body again.\n"
        "- Run anything over a minute with Bash run_in_background and wait with "
        "Monitor; the harness rejects a foreground sleep (sleep N; tail … too), and a "
        "foreground until-loop "
        "(timeout N bash -c 'until …' included) blocks the turn just the same; "
        "background waiters survive and notify you; to wait on work you did not start (a "
        "unit, a service, another build), run an until-loop with Bash "
        "run_in_background (one notice) or Monitor (one per event), never a "
        "foreground sleep. Run tests quietly (pytest -q --tb=short, no -v) and "
        "tail -n 80 the output; a test run that fails is a result, not an "
        "error to retry. Do not batch quick probes with a long foreground command.");
}

QString AgentRunner::commitChangesInstruction()
{
    return QStringLiteral(
        "After completing and verifying the requested changes, commit them to the current "
        "agent branch with a concise commit message. Do not push. Do not open a pull "
        "request unless the user explicitly asks for one. Do not create an empty commit "
        "when no repository changes are needed.");
}

QString AgentRunner::directCheckoutInstruction()
{
    return QStringLiteral(
        "You are working directly in the repository's own checkout, not an isolated "
        "agent worktree. Do not create branches or worktrees, and do not switch the "
        "checked-out branch. Do not commit, push, or open a pull request unless the "
        "user explicitly asks for it.");
}

QString AgentRunner::providerDisplayName(const QString &provider)
{
    if (provider == QLatin1String("claude-code"))
        return QStringLiteral("CC");
    if (provider == QLatin1String("codex"))
        return QStringLiteral("Codex");
    if (provider == QLatin1String("cloudflare-ai"))
        return QStringLiteral("Cloudflare AI");
    if (provider == QLatin1String("local-llama"))
        return QStringLiteral("Local Llama");
    if (provider.startsWith(QLatin1String("claude")))
        return QStringLiteral("Claude API");
    return QStringLiteral("OpenAI API");
}

QString AgentRunner::launchIdentityInstruction(const QString &provider,
                                                const QString &model,
                                                const QString &mode,
                                                const QString &strength)
{
    return QStringLiteral("Agent launch identity: provider display name: %1; "
                          "resolved model: %2; permission mode: %3; "
                          "reasoning speed: %4.")
        .arg(providerDisplayName(provider),
             model.trimmed().isEmpty() ? QStringLiteral("provider default")
                                       : model.trimmed(),
             mode.trimmed().isEmpty() ? QStringLiteral("Ask") : mode.trimmed(),
             strength.trimmed().isEmpty() ? QStringLiteral("high")
                                           : strength.trimmed());
}

QString AgentRunner::buildPrompt() const
{
    QStringList prompt;
    QString preamble = m_config.promptPreamble.trimmed();
    if (preamble.isEmpty())
        preamble = defaultPromptPreamble();
    prompt << launchIdentityInstruction(m_session.provider, m_config.model,
                                        m_config.mode, m_config.strength);
    prompt << preamble;
    prompt << toolUseInstruction();
    prompt << QStringLiteral("");
    prompt << QStringLiteral("Repository: %1/%2").arg(m_session.owner, m_session.name);
    const QString task = m_config.taskOverride.trimmed();
    if (!task.isEmpty()) {
        prompt << QStringLiteral("Task: %1").arg(m_session.issueTitle);
        prompt << QStringLiteral("");
        prompt << task;
    } else {
        prompt << QStringLiteral("Issue #%1: %2")
                      .arg(m_session.issueNumber)
                      .arg(m_session.issueTitle);
        prompt << QStringLiteral("");
        prompt << QStringLiteral("Issue thread:");
        for (const IssueEvent &ev : m_issue.events) {
            const QString who = ev.authorName.isEmpty() ? ev.author.left(10) : ev.authorName;
            if (ev.type == QLatin1String("open") || ev.type == QLatin1String("comment")) {
                prompt << QStringLiteral("--- %1 by %2 ---").arg(ev.type, who);
                prompt << ev.body.left(6000);
            } else if (ev.type == QLatin1String("labels")) {
                prompt << QStringLiteral("--- labels: %1 ---").arg(ev.labels.join(", "));
            } else if (ev.type == QLatin1String("assignees")) {
                prompt << QStringLiteral("--- assignees: %1 ---").arg(ev.assignees.join(", "));
            }
        }
    }
    if (m_store && m_session.startedAtMs > 0) {
        const QString transcript = m_store->readLog(m_session).trimmed();
        if (!transcript.isEmpty()) {
            prompt << QStringLiteral(
                "\nPrevious session transcript (most recent part):");
            prompt << transcript.right(12000);
        }
    }
    prompt << QStringLiteral("\nReturn concise progress and final notes.");
    return prompt.join(QLatin1Char('\n'));
}

QString AgentRunner::expandCommand(const QString &promptPath) const
{
    QString command = m_config.command.trimmed();
    const bool hadPromptFile = command.contains(QStringLiteral("{promptFile}"));
    const QString model = m_config.model.trimmed();
    command.replace(QStringLiteral("{modelArg}"),
                    model.isEmpty()
                        ? QString()
                        : QStringLiteral("-m %1").arg(shellQuote(model)));
    command.replace(QStringLiteral("{model}"), shellQuote(model));
    command.replace(QStringLiteral("{promptFile}"), shellQuote(promptPath));
    command.replace(QStringLiteral("{issueNumber}"),
                    QString::number(m_session.issueNumber));
    command.replace(QStringLiteral("{provider}"), shellQuote(m_session.provider));
    command.replace(QStringLiteral("{contextWindow}"),
                    QString::number(m_config.contextWindow));
    command.replace(QStringLiteral("{maxOutputTokens}"),
                    QString::number(m_config.maxOutputTokens));
    if (!hadPromptFile)
        command += QStringLiteral(" < ") + shellQuote(promptPath);
    return command;
}

QString AgentRunner::redact(QString text) const
{
    const QString key = m_config.apiKey.trimmed();
    if (!m_config.apiKey.isEmpty())
        text.replace(m_config.apiKey, QStringLiteral("***"));
    if (!key.isEmpty())
        text.replace(key, QStringLiteral("***"));
    text.replace(QRegularExpression(QStringLiteral("\\bsk-[A-Za-z0-9_-]{20,}")),
                 QStringLiteral("sk-***"));
    text.replace(
        QRegularExpression(
            QStringLiteral(
                "(?i)(authorization\\s*:\\s*bearer\\s+)[A-Za-z0-9._~+/-]{8,}")),
        QStringLiteral("\\1***"));
    text.replace(
        QRegularExpression(
            QStringLiteral(
                "(?i)([\"']?(?:accessToken|refreshToken|apiKey|"
                "anthropicApiKey|openAiApiKey)[\"']?\\s*[:=]\\s*[\"'])"
                "[^\"'\\r\\n]+")),
        QStringLiteral("\\1***"));
    return text;
}

QString AgentRunner::detectAuthIssue(const QString &chunk)
{
    if (m_attentionRaised)
        return QString();
    const QString low = chunk.toLower();
    const bool claude = m_session.provider.startsWith(QLatin1String("claude"));
    const auto has = [&](const char *needle) { return low.contains(QLatin1String(needle)); };
    if (claude &&
        (has("invalid api key") || has("authentication_error") ||
         has("401 unauthorized"))) {
        m_attentionRaised = true;
        return QStringLiteral(
            "Claude rejected the Anthropic API key. Set a valid ANTHROPIC_API_KEY "
            "in Settings, then click Continue.");
    }
    if (claude && has("credit balance is too low")) {
        m_attentionRaised = true;
        return QStringLiteral(
            "Claude reports the credit balance is too low. Top up the Anthropic "
            "account for this API key, then click Continue.");
    }
    if (m_session.provider == QLatin1String("cloudflare-ai")) {
        if (has("cloudflare's edge blocked this request") ||
            has("error code: 1010")) {
            m_attentionRaised = true;
            return QStringLiteral(
                "Cloudflare's edge blocked this node's AI agent request before "
                "it reached the relay, so the account sign-in is fine. Update "
                "ForkMesh to a build that identifies agent turns as ForkMesh, "
                "then restart the session.");
        }
        if (has("not authorized to sign for the account") ||
            has("no signed relay ticket")) {
            m_attentionRaised = true;
            return QStringLiteral(
                "The relay rejected this node's Cloudflare AI run ticket. Sign "
                "in to this node's account, then restart the session.");
        }
        return QString();
    }
    if (m_session.provider == QLatin1String("local-llama") &&
        has("does not support tool calling")) {
        m_attentionRaised = true;
        return QStringLiteral(
            "The selected local model cannot call tools, so it cannot work as "
            "a coding agent. Install a tools-capable model such as Gemma 4 "
            "from the marketplace, select it in Settings, then restart the "
            "session.");
    }
    if (m_session.provider == QLatin1String("local-llama") &&
        (has("connection refused") || has("could not reach") ||
         has("local llama server is unavailable") ||
         has("no local llama model") || has("no local model configured") ||
         has("no local llama model is installed"))) {
        m_attentionRaised = true;
        return QStringLiteral(
            "The local Llama server is unavailable or has no installed model. "
            "Start Ollama/llama.cpp, verify the endpoint and model in Settings, "
            "then restart the session.");
    }
    if (!claude && (has("401 unauthorized") || has("invalid api key"))) {
        m_attentionRaised = true;
        return QStringLiteral(
            "OpenAI rejected the API key. Set a valid OpenAI API key in Settings, "
            "then click Continue.");
    }
    if (!claude &&
        (has("quota exceeded") || has("insufficient_quota") ||
         has("billing details"))) {
        m_attentionRaised = true;
        if (m_session.provider == QLatin1String("codex"))
            return QStringLiteral(
                "Codex is reporting an OpenAI API quota/billing limit. ForkMesh "
                "now runs the Codex provider through your logged-in Codex CLI "
                "account instead of the saved API key; restart this session after "
                "updating to this build.");
        return QStringLiteral(
            "OpenAI reports that this API key or project is out of quota. Update "
            "the OpenAI API key/billing project in Settings, or switch the prompt "
            "agent to Codex to use your logged-in Codex CLI account.");
    }
    return QString();
}

void AgentRunner::emitLog(const QString &text)
{
    const QString safe = redact(text);
    m_store->appendLog(m_session, safe);
    emit logLine(m_session.id, safe);
}

void AgentRunner::onNoOutputTimeout()
{
    if (!m_process || m_phase != Phase::Agent ||
        m_process->state() != QProcess::Running)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 started = m_processStartedAtMs > 0 ? m_processStartedAtMs : now;
    const qint64 last = m_lastOutputAtMs > 0 ? m_lastOutputAtMs : started;
    const qint64 runningSecs = qMax<qint64>(0, (now - started) / 1000);
    const qint64 quietSecs = qMax<qint64>(0, (now - last) / 1000);
    emitLog(QStringLiteral(
                "==> Agent process still running: %1s elapsed, no output for %2s (pid %3, program %4).")
                .arg(runningSecs)
                .arg(quietSecs)
                .arg(m_process->processId())
                .arg(m_currentProgram));
}

void AgentRunner::refreshUsage()
{
    const QString transcript = m_store ? m_store->readLog(m_session) : QString();
    m_session.completionTokens = estimateTokens(transcript);
    m_session.totalTokens = m_session.promptTokens + m_session.completionTokens;
    m_session.contextTokens = m_session.totalTokens;
    m_session.estimatedCredits = (m_session.totalTokens + 999) / 1000;
    m_session.costUsd = estimateCostUsd();
}

double AgentRunner::estimateCostUsd() const
{
    const AgentTokenPrice price =
        agentTokenPriceFor(m_session.provider, m_config.model);
    return (m_session.promptTokens * price.inputPerMillion +
            m_session.completionTokens * price.outputPerMillion) /
           1000000.0;
}

int AgentRunner::estimateTokens(const QString &text)
{
    if (text.isEmpty())
        return 0;
    return qMax(1, text.size() / 4);
}
