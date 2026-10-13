#include "../src/AgentCliLocator.h"
#include "../src/AgentCapabilityBroker.h"
#include "../src/AgentCapabilityStore.h"
#include "../src/AgentPermissionController.h"
#include "../src/AgentJsonlFramer.h"
#include "../src/AgentRuntimeSession.h"
#include "../src/AgentRuntimeRegistry.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QTemporaryDir>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        qInfo("PASS: %s", message);
    else {
        qCritical("FAIL: %s", message);
        ++failures;
    }
}

void descriptorRoundTrip()
{
    AgentRuntimeDescriptor descriptor{
        QStringLiteral("runtime.test"), QStringLiteral("Test runtime"), 1, 2,
        {AgentRuntimeCapabilities::SessionLifecycle,
         AgentRuntimeCapabilities::ToolLifecycle,
         QStringLiteral("future.capability")}};
    QString error;
    check(descriptor.isValid(&error), "a versioned runtime descriptor is valid");
    check(descriptor.supports(QStringLiteral("future.capability")),
          "unknown capabilities are preserved for forward compatibility");

    AgentRuntimeDescriptor parsed;
    check(AgentRuntimeDescriptor::fromJson(descriptor.toJson(), &parsed, &error),
          "a runtime descriptor round-trips through JSON");
    check(parsed.id == descriptor.id &&
              parsed.capabilities == descriptor.capabilities &&
              parsed.protocolMaximum == 2,
          "the descriptor round trip retains identity, range, and capabilities");

    QJsonObject invalid = descriptor.toJson();
    invalid.insert(QStringLiteral("id"), QStringLiteral("Provider Name"));
    check(!AgentRuntimeDescriptor::fromJson(invalid, &parsed, &error),
          "unstable provider display text cannot become a runtime id");
}

void eventRoundTrip()
{
    const QStringList eventTypes{
        AgentRuntimeEventTypes::SessionStarted,
        AgentRuntimeEventTypes::SessionFinished,
        AgentRuntimeEventTypes::TextDelta,
        AgentRuntimeEventTypes::ReasoningDelta,
        AgentRuntimeEventTypes::ToolStarted,
        AgentRuntimeEventTypes::ToolUpdated,
        AgentRuntimeEventTypes::ToolCompleted,
        AgentRuntimeEventTypes::UsageUpdated,
        AgentRuntimeEventTypes::PermissionRequested,
        AgentRuntimeEventTypes::PermissionResolved,
        AgentRuntimeEventTypes::ArtifactAvailable,
        AgentRuntimeEventTypes::DiagnosticReported,
        AgentRuntimeEventTypes::SubagentUpdated,
        AgentRuntimeEventTypes::TerminalAttached,
        AgentRuntimeEventTypes::RuntimeNotice,
        QStringLiteral("future.event")};

    qint64 sequence = 0;
    for (const QString &type : eventTypes) {
        AgentRuntimeEvent event{1, type, QStringLiteral("session-1"),
                                QStringLiteral("request-1"), sequence++,
                                {{QStringLiteral("value"), type}}};
        AgentRuntimeEvent parsed;
        QString error;
        check(AgentRuntimeEvent::fromJson(event.toJson(), &parsed, &error) &&
                  parsed.type == type && parsed.data == event.data,
              qPrintable(QStringLiteral("event round trip: %1").arg(type)));
    }

    AgentRuntimeEvent invalid;
    QString error;
    check(!AgentRuntimeEvent::fromJson(
              {{QStringLiteral("schemaVersion"), 2},
               {QStringLiteral("type"), AgentRuntimeEventTypes::RuntimeNotice},
               {QStringLiteral("sequence"), 0},
               {QStringLiteral("data"), QJsonObject{}}},
              &invalid, &error),
          "unsupported event schema versions fail closed");
}

void commandRoundTrip()
{
    const QStringList commandTypes{
        AgentRuntimeCommandTypes::InputSubmit,
        AgentRuntimeCommandTypes::RequestRespond,
        AgentRuntimeCommandTypes::TurnInterrupt,
        AgentRuntimeCommandTypes::SessionStop,
        AgentRuntimeCommandTypes::PermissionSet,
        AgentRuntimeCommandTypes::TerminalInput,
        QStringLiteral("future.command")};
    qint64 sequence = 0;
    for (const QString &type : commandTypes) {
        AgentRuntimeCommand command{1, type, QStringLiteral("request-1"),
                                    sequence++,
                                    {{QStringLiteral("text"),
                                      QStringLiteral("bounded input")}}};
        AgentRuntimeCommand parsed;
        QString error;
        check(AgentRuntimeCommand::fromJson(command.toJson(), &parsed, &error) &&
                  parsed.type == type && parsed.data == command.data,
              qPrintable(QStringLiteral("command round trip: %1").arg(type)));
    }

    AgentRuntimeCommand invalid{1, AgentRuntimeCommandTypes::InputSubmit,
                                QString(), -1, QJsonObject{}};
    QString error;
    check(!invalid.isValid(&error), "negative command sequences fail closed");
}

class FakeRuntime : public AgentRuntimeSession
{
public:
    AgentRuntimeDescriptor runtimeDescriptor() const override
    {
        return {QStringLiteral("fake-runtime"), QStringLiteral("Fake"), 1, 1,
                {AgentRuntimeCapabilities::SessionLifecycle}};
    }
    void startRuntime(const AgentRuntimeLaunch &) override {}
    bool sendUserText(const QString &text) override
    {
        lastText = text;
        return true;
    }
    void stop() override { stopped = true; }
    bool running() const override { return true; }
    bool acceptsInput() const override { return true; }
    qint64 processId() const override { return 1; }
    QString resumeId() const override { return {}; }

    QString lastText;
    bool stopped = false;
};

void commandDispatchContract()
{
    FakeRuntime runtime;
    AgentRuntimeCommand input{
        1,
        AgentRuntimeCommandTypes::InputSubmit,
        QStringLiteral("request-input"),
        1,
        {{QStringLiteral("text"), QStringLiteral("continue safely")}}};
    check(runtime.sendCommand(input) &&
              runtime.lastText == QStringLiteral("continue safely"),
          "the neutral input command dispatches through the runtime contract");

    AgentRuntimeCommand stop{1, AgentRuntimeCommandTypes::SessionStop,
                             QStringLiteral("request-stop"), 2, {}};
    check(runtime.sendCommand(stop) && runtime.stopped,
          "the neutral stop command dispatches through the runtime contract");

    AgentRuntimeCommand unsupported{
        1, AgentRuntimeCommandTypes::TerminalInput,
        QStringLiteral("request-terminal"), 3,
        {{QStringLiteral("text"), QStringLiteral("not attached")}}};
    check(!runtime.sendCommand(unsupported),
          "unsupported neutral commands fail closed at the adapter boundary");
}

void registryContract()
{
    const QList<AgentRuntimeDescriptor> runtimes =
        AgentRuntimeRegistry::builtInRuntimes();
    check(runtimes.size() == 2,
          "both structured adapters register behind the neutral contract");
    for (const AgentRuntimeDescriptor &descriptor : runtimes) {
        check(AgentRuntimeRegistry::protocolCompatible(descriptor),
              qPrintable(QStringLiteral("runtime protocol is compatible: %1")
                             .arg(descriptor.id)));
        AgentRuntimeSession *runtime =
            AgentRuntimeRegistry::create(descriptor.id);
        check(runtime && runtime->runtimeDescriptor().id == descriptor.id,
              qPrintable(QStringLiteral("runtime factory creates: %1")
                             .arg(descriptor.id)));
        delete runtime;
    }
    check(!AgentRuntimeRegistry::create(QStringLiteral("missing-runtime")),
          "unknown runtimes fail closed");

    AgentRuntimeDescriptor future{QStringLiteral("future-runtime"),
                                  QStringLiteral("Future"), 2, 3, {}};
    check(!AgentRuntimeRegistry::protocolCompatible(future),
          "non-overlapping runtime protocol ranges are rejected");
}

void boundedJsonlFraming()
{
    AgentJsonlFramer framer(16);
    AgentJsonlFramer::Result result = framer.append(QByteArrayLiteral("{\"a\":"));
    check(result.frames.isEmpty() && framer.bufferedBytes() == 5,
          "a partial JSONL frame is retained until its newline arrives");

    result = framer.append(QByteArrayLiteral("1}\r\n{}\n"));
    check(result.errors.isEmpty() && result.frames.size() == 2 &&
              result.frames.at(0) == QByteArrayLiteral("{\"a\":1}") &&
              result.frames.at(1) == QByteArrayLiteral("{}"),
          "chunked and CRLF JSONL frames are reassembled in order");

    result = framer.append(QByteArray(17, 'x'));
    check(result.frames.isEmpty() && result.errors.size() == 1 &&
              framer.bufferedBytes() == 0,
          "an unterminated oversized frame is bounded and reported once");
    result = framer.append(QByteArrayLiteral("discarded-tail\n{}\n"));
    check(result.errors.isEmpty() && result.frames.size() == 1 &&
              result.frames.first() == QByteArrayLiteral("{}"),
          "framing safely resynchronizes after an oversized chunked frame");

    result = framer.append(QByteArray(17, 'y') + QByteArrayLiteral("\n"));
    check(result.frames.isEmpty() && result.errors.size() == 1,
          "a complete oversized frame is discarded before JSON parsing");

    result = framer.append(QByteArrayLiteral("{\"tail\":true}"), true);
    check(result.frames.size() == 1 &&
              result.frames.first() == QByteArrayLiteral("{\"tail\":true}"),
          "process exit can explicitly flush one bounded final frame");

    AgentJsonlFramer defaultFramer;
    const QByteArray formerlyRejected(
        8 * 1024 * 1024 + 1, 'z');
    result = defaultFramer.append(formerlyRejected);
    check(result.errors.isEmpty() &&
              defaultFramer.bufferedBytes() == formerlyRejected.size(),
          "default framing retains runtime frames above the former 8 MiB limit");
    result = defaultFramer.append(QByteArrayLiteral("\n"));
    check(result.errors.isEmpty() && result.frames.size() == 1 &&
              result.frames.first().size() == formerlyRejected.size(),
          "a large bounded runtime frame reaches the adapter intact");
}

AgentCapabilityRequest request(const QString &capability,
                               const QString &resource,
                               const QString &session = QStringLiteral("s1"),
                               const QString &worktree =
                                   QStringLiteral("/tmp/worktree-a"))
{
    return {capability, resource, session, worktree};
}

void capabilityBrokerContract()
{
    AgentCapabilityBroker broker;
    const AgentCapabilityRequest file =
        request(AgentHostCapabilities::FileWrite,
                QStringLiteral("/tmp/worktree-a/src/main.cpp"));
    QString matched;
    check(broker.evaluate(file, &matched) == AgentCapabilityDecision::Prompt &&
              matched.isEmpty(),
          "unknown host capabilities prompt instead of being silently granted");

    QString error;
    const QString once = broker.grant(file, AgentGrantLifetime::Once,
                                      AgentCapabilityDecision::Allow, &error);
    check(!once.isEmpty() && error.isEmpty() &&
              broker.evaluate(file, &matched) == AgentCapabilityDecision::Allow &&
              matched == once &&
              broker.evaluate(file) == AgentCapabilityDecision::Prompt,
          "a once grant authorizes exactly one matching operation");

    const QString sessionGrant =
        broker.grant(file, AgentGrantLifetime::Session,
                     AgentCapabilityDecision::Allow, &error);
    AgentCapabilityRequest otherSession = file;
    otherSession.sessionId = QStringLiteral("s2");
    check(!sessionGrant.isEmpty() &&
              broker.evaluate(file) == AgentCapabilityDecision::Allow &&
              broker.evaluate(otherSession) == AgentCapabilityDecision::Prompt,
          "session grants cannot leak into another agent session");

    AgentCapabilityRequest process =
        request(AgentHostCapabilities::ProcessExecute,
                QStringLiteral("build:test"));
    const QString worktreeGrant =
        broker.grant(process, AgentGrantLifetime::Worktree,
                     AgentCapabilityDecision::Allow, &error);
    AgentCapabilityRequest otherWorktree = process;
    otherWorktree.worktreePath = QStringLiteral("/tmp/worktree-b");
    check(!worktreeGrant.isEmpty() &&
              broker.evaluate(process) == AgentCapabilityDecision::Allow &&
              broker.evaluate(otherWorktree) == AgentCapabilityDecision::Prompt,
          "worktree grants are exact and cannot cross worktree boundaries");
    broker.clearSession(QStringLiteral("s1"));
    broker.clearWorktree(QStringLiteral("/tmp/./worktree-a"));
    check(broker.evaluate(file) == AgentCapabilityDecision::Prompt &&
              broker.evaluate(process) == AgentCapabilityDecision::Prompt,
          "session and normalized-worktree cleanup revoke ephemeral grants");

    const QString unconsumedOnce = broker.grant(
        file, AgentGrantLifetime::Once, AgentCapabilityDecision::Allow, &error);
    check(!unconsumedOnce.isEmpty(),
          "an unconsumed one-shot grant can be staged for cleanup");
    broker.clearSession(QStringLiteral("s1"));
    check(broker.evaluate(file) == AgentCapabilityDecision::Prompt,
          "ending a session revokes an unconsumed one-shot grant");

    AgentCapabilityRequest network =
        request(AgentHostCapabilities::NetworkConnect,
                QStringLiteral("api.example.test:443"));
    const QString persistentAllow =
        broker.grant(network, AgentGrantLifetime::Persistent,
                     AgentCapabilityDecision::Allow, &error);
    AgentCapabilityRequest allNetwork = network;
    allNetwork.resource = QStringLiteral("*");
    const QString persistentDeny =
        broker.grant(allNetwork, AgentGrantLifetime::Persistent,
                     AgentCapabilityDecision::Deny, &error);
    check(!persistentAllow.isEmpty() && !persistentDeny.isEmpty() &&
              broker.evaluate(network) == AgentCapabilityDecision::Deny,
          "a matching wildcard deny fails closed over a specific allow");
    check(broker.revoke(persistentDeny) &&
              broker.evaluate(network) == AgentCapabilityDecision::Allow,
          "revoking a deny restores the remaining explicit grant");

    const QJsonArray saved = broker.persistentGrants();
    AgentCapabilityBroker restored;
    check(restored.loadPersistentGrants(saved, &error) && error.isEmpty() &&
              restored.evaluate(network) == AgentCapabilityDecision::Allow &&
              restored.size() == saved.size(),
          "persistent grants round-trip without ephemeral session state");

    QJsonArray malformed = saved;
    if (!saved.isEmpty())
        malformed.append(saved.first());
    check(!restored.loadPersistentGrants(malformed, &error) &&
              restored.evaluate(network) == AgentCapabilityDecision::Allow,
          "malformed persistent grant updates fail atomically");

    const QStringList capabilityFamilies{
        AgentHostCapabilities::FileRead,
        AgentHostCapabilities::FileWrite,
        AgentHostCapabilities::NetworkConnect,
        AgentHostCapabilities::ProcessExecute,
        AgentHostCapabilities::DeviceControl,
        AgentHostCapabilities::SecretRead,
        AgentHostCapabilities::ToolInvoke,
        AgentHostCapabilities::ResourceRead};
    for (const QString &capability : capabilityFamilies) {
        const QString resource =
            capability == AgentHostCapabilities::FileRead ||
                    capability == AgentHostCapabilities::FileWrite
                ? QStringLiteral("/tmp/opaque-resource")
                : QStringLiteral("opaque-resource");
        check(request(capability, resource).isValid(),
              qPrintable(QStringLiteral("valid host capability: %1")
                             .arg(capability)));
    }
    check(!request(QStringLiteral("file write"),
                   QStringLiteral("opaque-resource"))
               .isValid(),
          "malformed capability identifiers fail closed");
    check(!request(AgentHostCapabilities::FileRead,
                   QStringLiteral("relative/file.cpp"))
               .isValid(),
          "relative file capability resources fail closed");
}

void capabilityStoreContract()
{
    QTemporaryDir directory;
    check(directory.isValid(), "a temporary capability store is available");
    const QString path = directory.filePath(
        QStringLiteral("private/capability-grants.json"));
    const AgentCapabilityRequest persistentRequest =
        request(AgentHostCapabilities::ResourceRead,
                QStringLiteral("forkmesh:issue:660"));
    const AgentCapabilityRequest ephemeralRequest =
        request(AgentHostCapabilities::ToolInvoke,
                QStringLiteral("forkmesh:diff"));

    AgentCapabilityBroker source;
    QString error;
    source.grant(persistentRequest, AgentGrantLifetime::Persistent,
                 AgentCapabilityDecision::Allow, &error);
    source.grant(ephemeralRequest, AgentGrantLifetime::Session,
                 AgentCapabilityDecision::Allow, &error);
    check(AgentCapabilityStore::save(path, source, &error) && error.isEmpty(),
          "persistent capability grants are saved atomically");
    const QFileInfo savedInfo(path);
    const QFileDevice::Permissions publicBits =
        QFileDevice::ReadGroup | QFileDevice::WriteGroup |
        QFileDevice::ExeGroup | QFileDevice::ReadOther |
        QFileDevice::WriteOther | QFileDevice::ExeOther;
    check(savedInfo.isFile() && !(savedInfo.permissions() & publicBits),
          "the capability grant file is owner-only");

    AgentCapabilityBroker restored;
    check(AgentCapabilityStore::load(path, &restored, &error) &&
              error.isEmpty() &&
              restored.evaluate(persistentRequest) ==
                  AgentCapabilityDecision::Allow &&
              restored.evaluate(ephemeralRequest) ==
                  AgentCapabilityDecision::Prompt,
          "restart restores persistent grants without session authority");

    restored.grant(ephemeralRequest, AgentGrantLifetime::Session,
                   AgentCapabilityDecision::Allow, &error);
    check(AgentCapabilityStore::load(path, &restored, &error) &&
              restored.evaluate(ephemeralRequest) ==
                  AgentCapabilityDecision::Allow,
          "reloading persistent policy does not erase live session grants");

    QFile corrupt(path);
    check(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
              corrupt.write("{not-json") == 9 && corrupt.flush(),
          "the capability fixture can be corrupted");
    corrupt.close();
    check(!AgentCapabilityStore::load(path, &restored, &error) &&
              restored.evaluate(persistentRequest) ==
                  AgentCapabilityDecision::Allow,
          "corrupt persisted policy fails atomically without changing grants");

    check(!AgentCapabilityStore::save(
              QStringLiteral("relative-capability-grants.json"), source,
              &error),
          "relative capability grant paths fail closed");

    const QString publicDirectory = directory.filePath(QStringLiteral("public"));
    check(QDir().mkpath(publicDirectory) &&
              QFile::setPermissions(
                  publicDirectory,
                  QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                      QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                      QFileDevice::ExeGroup),
          "a permissive capability directory fixture is available");
    check(!AgentCapabilityStore::save(
              QDir(publicDirectory)
                  .filePath(QStringLiteral("capability-grants.json")),
              source, &error),
          "saving refuses a permissive directory without changing its mode");
    check(QFileInfo(publicDirectory).permissions() & QFileDevice::ReadGroup,
          "capability persistence does not chmod a caller-owned directory");
}

AgentRuntimeEvent permissionEvent(const QString &sessionId,
                                  const QString &requestId,
                                  const QString &capability,
                                  const QString &resource)
{
    return {1,
            AgentRuntimeEventTypes::PermissionRequested,
            sessionId,
            requestId,
            1,
            {{QStringLiteral("capability"), capability},
             {QStringLiteral("resource"), resource}}};
}

void permissionControllerContract()
{
    AgentCapabilityBroker broker;
    AgentPermissionController controller(&broker);
    const AgentRuntimeEvent first = permissionEvent(
        QStringLiteral("session-a"), QStringLiteral("request-a"),
        AgentHostCapabilities::ProcessExecute, QStringLiteral("build:test"));

    AgentPermissionResult result = controller.handleRequest(
        first, QStringLiteral("session-a"), QStringLiteral("/tmp/worktree-a"));
    check(result.status == AgentPermissionStatus::Prompt &&
              result.decision == AgentCapabilityDecision::Prompt &&
              controller.pendingCount() == 1,
          "an unknown normalized permission request becomes a bounded prompt");

    result = controller.resolve(QStringLiteral("session-a"),
                                QStringLiteral("request-a"),
                                AgentGrantLifetime::Once,
                                AgentCapabilityDecision::Allow);
    check(result.status == AgentPermissionStatus::Resolved &&
              result.decision == AgentCapabilityDecision::Allow &&
              result.response.type ==
                  AgentRuntimeCommandTypes::RequestRespond &&
              result.response.requestId == QStringLiteral("request-a") &&
              result.response.data.value(QStringLiteral("decision")).toString() ==
                  QStringLiteral("allow") &&
              result.response.data.value(QStringLiteral("lifetime")).toString() ==
                  QStringLiteral("once") &&
              controller.pendingCount() == 0,
          "a user decision produces a correlated normalized response");

    const AgentRuntimeEvent second = permissionEvent(
        QStringLiteral("session-a"), QStringLiteral("request-b"),
        AgentHostCapabilities::ProcessExecute, QStringLiteral("build:test"));
    check(controller.handleRequest(second, QStringLiteral("session-a"),
                                   QStringLiteral("/tmp/worktree-a"))
                  .status == AgentPermissionStatus::Prompt,
          "a one-shot grant is consumed by the request it resolved");
    result = controller.resolve(QStringLiteral("session-a"),
                                QStringLiteral("request-b"),
                                AgentGrantLifetime::Session,
                                AgentCapabilityDecision::Allow);
    check(result.status == AgentPermissionStatus::Resolved,
          "a session grant resolves its originating request");

    const AgentRuntimeEvent third = permissionEvent(
        QStringLiteral("session-a"), QStringLiteral("request-c"),
        AgentHostCapabilities::ProcessExecute, QStringLiteral("build:test"));
    result = controller.handleRequest(third, QStringLiteral("session-a"),
                                      QStringLiteral("/tmp/worktree-a"));
    check(result.status == AgentPermissionStatus::Resolved &&
              result.decision == AgentCapabilityDecision::Allow &&
              result.response.data.value(QStringLiteral("source")).toString() ==
                  QStringLiteral("stored-grant"),
          "a matching stored grant resolves without prompting again");

    result = controller.handleRequest(third, QStringLiteral("session-a"),
                                      QStringLiteral("/tmp/worktree-a"));
    check(result.status == AgentPermissionStatus::Resolved &&
              result.decision == AgentCapabilityDecision::Deny &&
              result.error.contains(QStringLiteral("replayed")),
          "a replayed runtime request id fails closed");

    const AgentRuntimeEvent spoofed = permissionEvent(
        QStringLiteral("session-b"), QStringLiteral("request-spoofed"),
        AgentHostCapabilities::SecretRead, QStringLiteral("provider:key"));
    result = controller.handleRequest(spoofed, QStringLiteral("session-a"),
                                      QStringLiteral("/tmp/worktree-a"));
    check(result.status == AgentPermissionStatus::Resolved &&
              result.decision == AgentCapabilityDecision::Deny &&
              result.error.contains(QStringLiteral("mismatch")),
          "a runtime cannot borrow another host session's grants");

    const AgentRuntimeEvent malformed = permissionEvent(
        QStringLiteral("session-a"), QStringLiteral("request-malformed"),
        QStringLiteral("file write"), QStringLiteral("/tmp/outside"));
    result = controller.handleRequest(malformed, QStringLiteral("session-a"),
                                      QStringLiteral("/tmp/worktree-a"));
    check(result.status == AgentPermissionStatus::Resolved &&
              result.decision == AgentCapabilityDecision::Deny &&
              result.error.contains(QStringLiteral("invalid capability")),
          "malformed normalized permission data receives a denial response");

    const AgentRuntimeEvent conflicted = permissionEvent(
        QStringLiteral("session-a"), QStringLiteral("request-conflicted"),
        AgentHostCapabilities::SecretRead, QStringLiteral("provider:key"));
    result = controller.handleRequest(conflicted, QStringLiteral("session-a"),
                                      QStringLiteral("/tmp/worktree-a"));
    check(result.status == AgentPermissionStatus::Prompt,
          "a new secret request waits for an explicit decision");
    broker.grant(result.request, AgentGrantLifetime::Persistent,
                 AgentCapabilityDecision::Deny);
    result = controller.resolve(QStringLiteral("session-a"),
                                QStringLiteral("request-conflicted"),
                                AgentGrantLifetime::Session,
                                AgentCapabilityDecision::Allow);
    check(result.status == AgentPermissionStatus::Resolved &&
              result.decision == AgentCapabilityDecision::Deny &&
              result.response.data.value(QStringLiteral("decision")).toString() ==
                  QStringLiteral("deny") &&
              !controller.isPending(QStringLiteral("session-a"),
                                    QStringLiteral("request-conflicted")),
          "a conflicting policy change fails closed and cannot bypass the broker");

    const AgentRuntimeEvent pending = permissionEvent(
        QStringLiteral("session-a"), QStringLiteral("request-pending"),
        AgentHostCapabilities::FileWrite,
        QStringLiteral("/tmp/worktree-a/file.cpp"));
    controller.handleRequest(pending, QStringLiteral("session-a"),
                             QStringLiteral("/tmp/worktree-a"));
    controller.clearSession(QStringLiteral("session-a"));
    check(controller.pendingCount() == 0 &&
              broker.evaluate(request(AgentHostCapabilities::ProcessExecute,
                                      QStringLiteral("build:test"),
                                      QStringLiteral("session-a"))) ==
                  AgentCapabilityDecision::Prompt,
          "ending a session revokes its grants and pending prompts");
}

// Claude Code and Codex install into user-scoped prefixes — ~/.local/bin from
// the native installer, an npm prefix, the legacy ~/.claude/local launcher —
// that a GUI process launched from a desktop icon does not inherit on PATH.
// Reporting them "not installed on this device" there was a real bug, so the
// locator must look past PATH, and every PATH ForkMesh hands a child must cover
// the same directories it searched.
void cliLocatorContract()
{
    const QByteArray inheritedHome = qgetenv("HOME");
    const QByteArray inheritedPath = qgetenv("PATH");
    QTemporaryDir home;
    check(home.isValid(), "the CLI locator contract has a scratch home");
    qputenv("HOME", home.path().toLocal8Bit());
    qputenv("PATH", QByteArray("/forkmesh-nonexistent-bin"));

    check(forkmesh::agentcli::executable(QString()).isEmpty(),
          "an empty program name resolves to nothing");
    check(!forkmesh::agentcli::installed(QStringLiteral("forkmesh-fake-cli")),
          "a CLI installed nowhere is reported missing");

    const QString binDir = home.filePath(QStringLiteral(".local/bin"));
    QDir().mkpath(binDir);
    const QString installed = binDir + QStringLiteral("/forkmesh-fake-cli");
    QFile launcher(installed);
    check(launcher.open(QIODevice::WriteOnly),
          "the contract can stage a CLI under the scratch home");
    launcher.write("#!/bin/sh\n");
    launcher.close();
    launcher.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                            QFileDevice::ExeOwner);

    check(forkmesh::agentcli::executable(QStringLiteral("forkmesh-fake-cli")) ==
              installed,
          "a CLI under ~/.local/bin is found when PATH omits it");
    check(forkmesh::agentcli::executable(installed) == installed,
          "an absolute path to an installed CLI resolves to itself");
    check(forkmesh::agentcli::executable(binDir).isEmpty(),
          "a directory is never mistaken for an executable");

    QProcessEnvironment env;
    env.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin"));
    forkmesh::agentcli::widenPath(env);
    const QStringList widened =
        env.value(QStringLiteral("PATH")).split(QDir::listSeparator());
    bool coversSearch = widened.contains(QStringLiteral("/usr/bin"));
    const QStringList searched = forkmesh::agentcli::userInstallDirs() +
                                 forkmesh::agentcli::fallbackInstallDirs();
    for (const QString &dir : searched)
        coversSearch = coversSearch && widened.contains(dir);
    check(coversSearch,
          "a widened PATH carries every directory the locator searches");
    check(widened.indexOf(binDir) >= 0 &&
              widened.indexOf(binDir) < widened.indexOf(QStringLiteral("/usr/bin")),
          "user install prefixes outrank the inherited PATH");
    QStringList unique = widened;
    unique.removeDuplicates();
    check(unique.size() == widened.size(),
          "a widened PATH repeats no directory");

    // Startup (main): the process PATH gains the user install directories
    // that exist, in front, and keeps the order of what it had.
    QStringList added;
    const QString startup = forkmesh::agentcli::pathWithUserInstallDirs(
        QStringLiteral("/usr/local/bin:/usr/bin"), &added);
    check(startup == binDir + QStringLiteral(":/usr/local/bin:/usr/bin") &&
              added == QStringList{binDir},
          "startup puts an existing ~/.local/bin in front of a service PATH");
    check(forkmesh::agentcli::pathWithUserInstallDirs(startup, &added) ==
                  startup &&
              added.isEmpty(),
          "startup PATH widening is a no-op once the directory is there");
    check(forkmesh::agentcli::pathWithUserInstallDirs(QString())
              .split(QDir::listSeparator())
              .contains(QStringLiteral("/usr/bin")),
          "an unset PATH keeps the system directories");
    qputenv("PATH", QByteArray("/usr/bin"));
    check(forkmesh::agentcli::addUserInstallDirsToProcessPath() ==
                  QStringList{binDir} &&
              qEnvironmentVariable("PATH") ==
                  binDir + QStringLiteral(":/usr/bin"),
          "the process PATH itself is widened for everything started later");

    qputenv("HOME", inheritedHome);
    qputenv("PATH", inheritedPath);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    descriptorRoundTrip();
    eventRoundTrip();
    commandRoundTrip();
    commandDispatchContract();
    registryContract();
    boundedJsonlFraming();
    capabilityBrokerContract();
    capabilityStoreContract();
    permissionControllerContract();
    cliLocatorContract();
    return failures == 0 ? 0 : 1;
}
