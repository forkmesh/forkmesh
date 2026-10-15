#include "MainWindow.h"
#include "MainWindowInternal.h"

#include "AgentGuardrailLedger.h"
#include "AgentToolCatalog.h"
#include "AgentToolEfficiencyLedger.h"
#include "AgentToolImprovementLog.h"
#include "BackgroundFileWriter.h"
#include "CoreAgentRun.h"
#include "CoreClient.h"
#include "CoreInsightsHook.h"
#include "LocalControlInsights.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>

#include <utility>

namespace insightctl = forkmesh::localcontrol::insights;
using namespace forkmesh::ui;

namespace {

constexpr int kStateRetryMs = 500;
constexpr int kStateRetries = 40;
constexpr qint64 kMaxShortcutBytes = 64 * 1024;

struct CoreInsightsState {
    QPointer<CoreClient> client;
    bool active = false;
    QString root;
    int retries = 0;
};

CoreInsightsState &insightsState()
{
    static CoreInsightsState state;
    return state;
}

QString cleanAbsolute(const QString &path)
{
    return path.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool coreOwnsPath(const QString &path)
{
    const CoreInsightsState &state = insightsState();
    if (!state.active || state.root.isEmpty() || !state.client || !state.client->isConnected())
        return false;
    const QString clean = cleanAbsolute(path);
    for (const char *name : {"tool-efficiency.json", "tool-improvements.jsonl", "guardrails.json"})
        if (clean == state.root + QLatin1Char('/') + QLatin1String(name))
            return true;
    return false;
}

QString readShortcutFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.read(kMaxShortcutBytes));
}

} // namespace

void MainWindow::registerCoreInsightsHandlers()
{
    CoreInsightsState &state = insightsState();
    state.client = m_coreClient;
    forkmesh::coreinsights::Hooks &hooks = forkmesh::coreinsights::hooks();
    hooks.owns = [](const QString &path) { return coreOwnsPath(path); };
    hooks.notify = [](const QString &method, const QJsonObject &params) {
        const CoreInsightsState &current = insightsState();
        return current.client && current.client->isConnected() && current.client->notify(method, params);
    };
    hooks.deliveringCoreEvent = [] { return forkmesh::coreagentrun::deliveringAnyStored(); };

    connect(m_coreClient, &CoreClient::coreEvent, this,
            [this](const QString &event, const QJsonObject &data) {
                if (event == QLatin1String("core.insights.changed"))
                    applyCoreInsightsState(data);
            });
    connect(m_coreClient, &CoreClient::connectedChanged, this, [this](bool connected) {
        CoreInsightsState &current = insightsState();
        if (!connected) {
            const bool wasActive = current.active;
            current.active = false;
            if (!wasActive)
                return;
            if (m_agentToolLedger) {
                if (QString error; !m_agentToolLedger->load(&error))
                    qWarning() << "agent tool ledger:" << error;
            }
            if (m_agentToolImprovements)
                m_agentToolImprovements->load();
            if (m_agentGuardrailLedger)
                m_agentGuardrailLedger->load();
            forkmesh::coreinsights::hooks().toolsLocalDirty = false;
            forkmesh::coreinsights::hooks().guardrailsLocalDirty = false;
            if (m_agentToolLedger && m_agentToolLedger->changed)
                m_agentToolLedger->changed();
            if (m_agentGuardrailLedger && m_agentGuardrailLedger->changed)
                m_agentGuardrailLedger->changed();
            return;
        }
        current.retries = 0;
        requestCoreInsightsState(true);
    });
    });

    m_coreClient->handle(
        QStringLiteral("ui.insights.networkRoots"),
        [this](const QJsonObject &, const CoreClient::Responder &respond) {
            QJsonArray roots;
            for (const AgentSession &session : std::as_const(m_agentSessions)) {
                const qint64 pid = agentSessionProcessId(session.id);
                if (pid <= 0)
                    continue;
                QString title = session.issueTitle.trimmed();
                if (title.isEmpty())
                    title = session.prompt.section(QLatin1Char('\n'), 0, 0).trimmed();
                roots.append(QJsonObject{
                    {QStringLiteral("session"), session.id},
                    {QStringLiteral("pid"), double(pid)},
                    {QStringLiteral("label"), title.isEmpty()
                                                  ? QStringLiteral("Agent run #%1").arg(session.id)
                                                  : title.left(160)}});
            }
            respond({{QStringLiteral("roots"), roots}}, QString());
        });
    m_coreClient->handle(
        QStringLiteral("ui.insights.toggleFamily"),
        [this](const QJsonObject &params, const CoreClient::Responder &respond) {
            const QString family = params.value(QStringLiteral("family")).toString();
            if (!agentToolFamily(family)) {
                respond({}, QStringLiteral("bad_family"));
                return;
            }
            setAgentToolFamilyEnabled(family, params.value(QStringLiteral("enabled")).toBool());
            respond({{QStringLiteral("ok"), true}}, QString());
        });
    m_coreClient->handle(
        QStringLiteral("ui.insights.runShortcutScript"),
        [this](const QJsonObject &params, const CoreClient::Responder &respond) {
            const QString repo = params.value(QStringLiteral("repo")).toString();
            const QString file = params.value(QStringLiteral("file")).toString();
            int repoIndex = -1;
            const QString dir = insightctl::validShortcutFile(file)
                                    ? localControlShortcutDir(repo, QStringLiteral("shortcut"), &repoIndex)
                                    : QString();
            const QString path = dir.isEmpty() ? QString() : QDir(dir).filePath(file);
            if (repoIndex < 0 || path.isEmpty() || !QFileInfo(path).isFile()) {
                respond({}, QStringLiteral("unknown_shortcut"));
                return;
            }
            const bool restart = shortcutRunning(path);
            if (!runShortcut(path, repoIndex)) {
                respond({}, QStringLiteral("shortcut_not_started"));
                return;
            }
            respond({{QStringLiteral("run"), m_shortcutRunCounter + (restart ? 1 : 0)},
                     {QStringLiteral("restart"), restart},
                     {QStringLiteral("name"), insightctl::parseShortcutMeta(file, readShortcutFile(path)).name}},
                    QString());
        });
    m_coreClient->handle(
        QStringLiteral("ui.insights.runShortcutAgent"),
        [this](const QJsonObject &params, const CoreClient::Responder &respond) {
            const QString repo = params.value(QStringLiteral("repo")).toString();
            const QString scope = params.value(QStringLiteral("scope")).toString();
            const QString file = params.value(QStringLiteral("file")).toString();
            int repoIndex = -1;
            const QString dir = localControlShortcutDir(repo, scope, &repoIndex);
            const QString path = dir.isEmpty() ? QString() : QDir(dir).filePath(file);
            if (repoIndex < 0 || path.isEmpty() || !QFileInfo(path).isFile()) {
                respond({}, QStringLiteral("unknown_shortcut"));
                return;
            }
            const QString text = readShortcutFile(path);
            const insightctl::ShortcutMeta meta = insightctl::parseShortcutMeta(file, text);
            QString prompt = insightctl::shortcutPrompt(text);
            if (prompt.isEmpty()) {
                respond({}, QStringLiteral("empty_shortcut"));
                return;
            }
            prompt.replace(QStringLiteral("{{appLog}}"), QDir::toNativeSeparators(networkLogPath()));
            const QString stallLog = stallLogPath();
            prompt.replace(QStringLiteral("{{stallLog}}"),
                           stallLog.isEmpty() ? QStringLiteral("(disabled in this build)")
                                              : QDir::toNativeSeparators(stallLog));
            QString provider = meta.agent.isEmpty() ? defaultAgentProvider() : meta.agent;
            if (provider == QLatin1String("claude"))
                provider = QStringLiteral("claude-code");
            if (!meta.agent.isEmpty() && !agentIsCodexProvider(provider) &&
                provider != QLatin1String("claude-code")) {
                respond({}, QStringLiteral("unsupported_agent"));
                return;
            }
            if (!meta.model.isEmpty() && !agentModelMatchesProvider(provider, meta.model)) {
                respond({}, QStringLiteral("model_mismatch"));
                return;
            }
            const int sessionId = startAdHocAgentForRepo(repoIndex, prompt, provider, false,
                                                         meta.model, meta.name, false, false);
            if (sessionId <= 0) {
                respond({}, QStringLiteral("agent_not_started"));
                return;
            }
            if (scope != QLatin1String("shortcut")) {
                if (AgentSession *session = findAgentSession(sessionId)) {
                    session->skillsUsed = {meta.name};
                    if (m_agentStore)
                        m_agentStore->saveSession(*session);
                }
            }
            respond({{QStringLiteral("session"), sessionId}, {QStringLiteral("name"), meta.name}},
                    QString());
        });
}

void MainWindow::requestCoreInsightsState(bool attaching)
{
    CoreInsightsState &state = insightsState();
    CoreClient *client = state.client;
    if (!client || !client->isConnected() || !client->hasNativeRoute(QStringLiteral("core.insights.state")))
        return;
    forkmesh::coreinsights::Hooks &hooks = forkmesh::coreinsights::hooks();
    const bool dirty = attaching && (hooks.toolsLocalDirty || hooks.guardrailsLocalDirty);
    if (dirty) {
        if (m_agentToolLedger && (m_agentToolLedger->hasSamples() ||
                                  QFile::exists(m_agentToolLedger->persistPath())))
            m_agentToolLedger->save();
        if (m_agentGuardrailLedger && m_agentGuardrailLedger->hasEvents())
            m_agentGuardrailLedger->save();
        forkmesh::diskwrite::flush();
    }
    client->request(dirty ? QStringLiteral("core.insights.reload") : QStringLiteral("core.insights.state"),
                    {}, [this](bool ok, const QJsonValue &result, const QString &) {
                        if (!ok)
                            return;
                        const QJsonObject snapshot = result.toObject();
                        CoreInsightsState &current = insightsState();
                        current.root = cleanAbsolute(snapshot.value(QStringLiteral("root")).toString());
                        current.active = true;
                        forkmesh::coreinsights::hooks().toolsLocalDirty = false;
                        forkmesh::coreinsights::hooks().guardrailsLocalDirty = false;
                        if (!m_agentToolLedger && current.retries++ < kStateRetries) {
                            QTimer::singleShot(kStateRetryMs, this,
                                               [this] { requestCoreInsightsState(false); });
                            return;
                        }
                        applyCoreInsightsState(snapshot);
                        if (m_agentToolLedger)
                            m_agentToolLedger->setDisabledFamilies(m_agentToolLedger->disabledFamilies());
                    });
}

void MainWindow::applyCoreInsightsState(const QJsonObject &state)
{
    if (!insightsState().active)
        return;
    bool toolsChanged = false;
    if (const QJsonValue tools = state.value(QStringLiteral("tools"));
        tools.isObject() && m_agentToolLedger && coreOwnsPath(m_agentToolLedger->persistPath())) {
        if (QString error; m_agentToolLedger->fromJson(tools.toObject(), &error))
            toolsChanged = true;
        else
            qWarning() << "core insights ledger:" << error;
    }
    if (const QJsonValue improvements = state.value(QStringLiteral("improvements"));
        improvements.isObject() && m_agentToolImprovements &&
        coreOwnsPath(m_agentToolImprovements->logPath())) {
        const QJsonObject object = improvements.toObject();
        m_agentToolImprovements->loadSnapshot(
            object.value(QStringLiteral("signals")).toArray(),
            qint64(object.value(QStringLiteral("occurrences")).toDouble()));
        toolsChanged = true;
    }
    bool guardrailsChanged = false;
    if (const QJsonValue guardrails = state.value(QStringLiteral("guardrails"));
        guardrails.isObject() && m_agentGuardrailLedger &&
        coreOwnsPath(m_agentGuardrailLedger->persistPath())) {
        guardrailsChanged = m_agentGuardrailLedger->fromJson(guardrails.toObject());
    }
    if (toolsChanged && m_agentToolLedger && m_agentToolLedger->changed)
        m_agentToolLedger->changed();
    if (guardrailsChanged && m_agentGuardrailLedger->changed)
        m_agentGuardrailLedger->changed();
}
