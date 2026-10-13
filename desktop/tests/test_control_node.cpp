#include "CatalogAdminDelete.h"
#include "ControlNode.h"
#include "D1DatabaseAdmin.h"
#include "D1QuerySource.h"
#include "FleetAutoscaleModel.h"
#include "MirrorActionsConfiguration.h"
#include "MirrorActionsSummary.h"
#include "MirrorSyncVerdict.h"
#include "NodeRole.h"
#include "SyncStatusModel.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QLocale>
#include <QMap>
#include <QMutex>
#include <QProcess>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QWaitCondition>

#include <atomic>
#include <cstdio>
#include <thread>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

QString base64Url(const QByteArray &value)
{
    return QString::fromLatin1(
        value.toBase64(QByteArray::Base64UrlEncoding |
                       QByteArray::OmitTrailingEquals));
}

bool runProcess(const QString &program, const QStringList &arguments,
                const QString &workingDirectory = {})
{
    QProcess process;
    if (!workingDirectory.isEmpty())
        process.setWorkingDirectory(workingDirectory);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, arguments);
    return process.waitForStarted(3000) &&
           process.waitForFinished(15000) &&
           process.exitStatus() == QProcess::NormalExit &&
           process.exitCode() == 0;
}

QMap<QString, QVariant> settingsValues(QSettings &settings)
{
    QMap<QString, QVariant> values;
    settings.sync();
    for (const QString &key : settings.allKeys())
        values.insert(key, settings.value(key));
    return values;
}

// The desktop's forkmesh-sync-status-v1 implementation must reproduce the
// shared fixture exactly and never echo the private strings it plants.
void checkSyncStatusFixture()
{
    QFile file(QStringLiteral(FORKMESH_SYNC_STATUS_FIXTURE));
    check(file.open(QIODevice::ReadOnly),
          "the shared sync-status fixture is readable");
    const QJsonObject fixture = QJsonDocument::fromJson(file.readAll()).object();
    QStringList privateStrings;
    for (const QJsonValue &value : fixture.value(QStringLiteral("privateStrings")).toArray())
        privateStrings << value.toString();
    int compared = 0;
    const auto compare = [&](const QString &id, const QJsonObject &got,
                             const QJsonValue &want) {
        ++compared;
        const QByteArray text = QJsonDocument(got).toJson(QJsonDocument::Compact);
        if (got != want.toObject()) {
            std::fprintf(stderr, "FAIL: sync status %s\n  got  %s\n  want %s\n",
                         qPrintable(id), text.constData(),
                         QJsonDocument(want.toObject())
                             .toJson(QJsonDocument::Compact)
                             .constData());
            ++failures;
        }
        for (const QString &secret : std::as_const(privateStrings))
            check(!QString::fromUtf8(text).contains(secret),
                  "sync status never echoes private evidence");
    };
    for (const QJsonValue &value : fixture.value(QStringLiteral("cases")).toArray()) {
        const QJsonObject entry = value.toObject();
        compare(entry.value(QStringLiteral("id")).toString(),
                forkmesh::syncstatus::evaluate(
                    entry.value(QStringLiteral("evidence")).toObject()),
                entry.value(QStringLiteral("expect")));
    }
    for (const QJsonValue &value : fixture.value(QStringLiteral("sequences")).toArray()) {
        const QJsonObject sequence = value.toObject();
        const QJsonArray steps = sequence.value(QStringLiteral("steps")).toArray();
        for (qsizetype i = 0; i < steps.size(); ++i) {
            const QJsonObject step = steps.at(i).toObject();
            compare(sequence.value(QStringLiteral("id")).toString() +
                        QLatin1Char('#') + QString::number(i),
                    forkmesh::syncstatus::evaluate(
                        step.value(QStringLiteral("evidence")).toObject()),
                    step.value(QStringLiteral("expect")));
        }
    }
    for (const QJsonValue &value :
         fixture.value(QStringLiteral("mirrorsPayloadCases")).toArray()) {
        const QJsonObject entry = value.toObject();
        compare(entry.value(QStringLiteral("id")).toString(),
                forkmesh::syncstatus::mirrorsPayloadStatus(
                    entry.value(QStringLiteral("payload")).toObject()),
                entry.value(QStringLiteral("expect")));
    }
    check(compared >= 40, "every shared sync-status fixture entry was compared");
}

void writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(bytes);
}

// Issue #727: a fresh Windows install died at startup with
// "configuration_lock_unavailable". QSettings::fileName() on Windows is a
// registry key ("\HKEY_CURRENT_USER\Software\ForkMesh\ForkMesh"); QFileInfo
// calls that rooted, so the transaction lock went to
// <drive>:\HKEY_CURRENT_USER\Software\ForkMesh, which a standard user often
// cannot create -- and the failure was startup-fatal.
void checkCatalogMirrorLiveness()
{
    namespace sync = forkmesh::mirrorsync;
    const QJsonObject divergedN1 = QJsonDocument::fromJson(
        R"({"node":"fmnext-n1","status":"offline","endpointHealthy":true,)"
        R"("endpointFresh":false,"lastSeen":null,"behind":true,)"
        R"("cloneAvailable":false})").object();
    check(sync::catalogMirrorLiveness(divergedN1, false) ==
              sync::MirrorLiveness::Offline,
          "a core with a lapsed endpoint lease and no other signal is offline");
    check(sync::catalogMirrorLiveness(divergedN1, true) ==
              sync::MirrorLiveness::RelayServing,
          "the relay's serving set keeps a diverged but live core online");
    QJsonObject fresh = divergedN1;
    fresh.insert(QStringLiteral("endpointFresh"), true);
    check(sync::catalogMirrorLiveness(fresh, true) == sync::MirrorLiveness::Endpoint,
          "a fresh healthy endpoint probe still wins over the relay's word");
    fresh.insert(QStringLiteral("status"), QStringLiteral("online"));
    check(sync::catalogMirrorLiveness(fresh, false) == sync::MirrorLiveness::Room,
          "an online catalog status is room presence");
    QJsonObject ssh = divergedN1;
    ssh.insert(QStringLiteral("sshHealthy"), true);
    check(sync::catalogMirrorLiveness(ssh, false) == sync::MirrorLiveness::Ssh,
          "an authenticated SSH snapshot counts as online");
}

void checkNodeRoles()
{
    namespace role = forkmesh::noderole;
    auto classify = [](const QString &name, bool self = false,
                       bool savedHost = false, const QString &mode = {},
                       const QString &declared = {},
                       const role::FleetNames &fleet = {}) {
        role::Facts facts;
        facts.name = name;
        facts.self = self;
        facts.savedHost = savedHost;
        facts.runtimeMode = mode;
        facts.declaredRole = declared;
        return role::classify(facts, fleet);
    };
    check(classify(QStringLiteral("fmnext-n1"), false, false,
                   QStringLiteral("headless")) == role::Role::Core,
          "a Next core node (fmnext-nN) is labelled core, not mirror");
    check(classify(QStringLiteral("FMNEXT-N3")) == role::Role::Core,
          "core names are matched case-insensitively");
    check(classify(QStringLiteral("fmnext-r1")) == role::Role::Runner &&
              classify(QStringLiteral("fmnext-run-2")) == role::Role::Runner,
          "a Next runner is labelled runner");
    check(classify(QStringLiteral("mirror35"), false, true) == role::Role::Mirror,
          "a saved Hosts server is labelled mirror");
    check(classify(QStringLiteral("magnetic-mirror-8594"), false, false,
                   QStringLiteral("desktop")) == role::Role::Desktop,
          "a desktop app whose generated name contains mirror stays a desktop node");
    check(classify(QStringLiteral("unnamed-server"), false, false,
                   QStringLiteral("headless")) == role::Role::Mirror,
          "a headless node outside the Next fleet is a mirror");
    check(classify(QStringLiteral("jett"), true) == role::Role::Desktop,
          "this machine is a desktop node");
    check(classify(QStringLiteral("jairus")) == role::Role::Desktop,
          "a node with no other signal is a desktop node");
    check(classify(QStringLiteral("anything"), false, true, {},
                   QStringLiteral("runner")) == role::Role::Runner,
          "a declared role wins over every inference");
    check(role::allRoles().last() == role::Role::Relay &&
              role::key(role::Role::Relay) == QStringLiteral("relay") &&
              role::label(role::Role::Relay) == QStringLiteral("Relay") &&
              classify(QStringLiteral("app.example.test"), false, false, {},
                       QStringLiteral("relay")) == role::Role::Relay &&
              classify(QStringLiteral("relay-ish")) == role::Role::Desktop,
          "Relay is a role of its own, only ever declared");
    const role::FleetNames fleet = role::parseFleetInstances(
        R"({"fmnext-core-1":{"ip":"192.0.2.1"},"fmnext-core-2":{"ip":"192.0.2.2"},)"
        R"("fmnext-run-1":{"ip":"192.0.2.9"},"other":{"ip":"192.0.2.7"}})");
    check(fleet.core.contains(QStringLiteral("n1")) &&
              fleet.core.contains(QStringLiteral("fmnext-n2")) &&
              fleet.runner.contains(QStringLiteral("r1")) &&
              fleet.runner.contains(QStringLiteral("fmnext-r1")) &&
              !fleet.core.contains(QStringLiteral("other")),
          "the Next fleet file names its core and runner nodes");
    check(classify(QStringLiteral("n2"), false, false, {}, {}, fleet) ==
                  role::Role::Core &&
              classify(QStringLiteral("r1"), false, true, {}, {}, fleet) ==
                  role::Role::Runner,
          "short fleet names classify from the fleet file");
    const role::FleetNames scaled = role::parseFleetInstances(
        R"({"fmnext-auto-1":{"ip":"192.0.2.21"},"fmnext-burst-2":{"ip":"192.0.2.32"}})");
    check(scaled.core.contains(QStringLiteral("a1")) &&
              scaled.core.contains(QStringLiteral("fmnext-a1")) &&
              scaled.runner.contains(QStringLiteral("b2")) &&
              !scaled.runner.contains(QStringLiteral("r2")),
          "autoscaled app replicas are aN cores and burst runners are bN runners");
    check(classify(QStringLiteral("fmnext-auto-3")) == role::Role::Core &&
              classify(QStringLiteral("fmnext-burst-4")) == role::Role::Runner &&
              classify(QStringLiteral("a1"), false, false, {}, {}, scaled) ==
                  role::Role::Core &&
              classify(QStringLiteral("b2"), false, false, {}, {}, scaled) ==
                  role::Role::Runner,
          "autoscaled nodes classify from their labels and the fleet file");
    check(role::label(role::Role::Desktop) == QStringLiteral("Desktop node") &&
              role::label(role::Role::Core) == QStringLiteral("Core") &&
              role::key(role::Role::Mirror) == QStringLiteral("mirror"),
          "roles read core, runner, mirror and desktop node");
}

void checkFleetAutoscaleModel()
{
    namespace as = forkmesh::autoscale;
    const as::Settings defaults = as::defaultSettings();
    check(!defaults.app.enabled && defaults.app.dryRun &&
              !defaults.runner.enabled && defaults.runner.dryRun,
          "both autoscale pools start off and in dry run");
    check(defaults.app.min == 0 && defaults.app.max == 2 &&
              defaults.app.upP95Ms == 500 && defaults.app.upForMinutes == 5 &&
              defaults.app.downForMinutes == 30 &&
              defaults.runner.upQueueWaitMinutes == 2 &&
              defaults.runner.downIdleMinutes == 10 &&
              defaults.app.cooldownMinutes == 10 &&
              defaults.app.plan == QStringLiteral("vc2-1c-1gb") &&
              defaults.app.region == QStringLiteral("ewr") &&
              defaults.app.monthlyCapCents + defaults.runner.monthlyCapCents == 2500,
          "the defaults match docs/fleet/AUTOSCALING.md");
    check(as::validateSettings(defaults).isEmpty(), "the defaults validate");

    as::Settings round = defaults;
    round.app.enabled = true;
    round.app.upRps = 40;
    round.runner.snapshotId = QStringLiteral("0a1b2c3d-1111-2222-3333-444455556666");
    round.app.snapshotId = QStringLiteral("9a1b2c3d-1111-2222-3333-444455556666");
    round.killSwitch = true;
    const as::Settings back = as::settingsFromJson(as::settingsToJson(round));
    check(back.killSwitch && !as::defaultSettings().killSwitch &&
              back.app.snapshotId == round.app.snapshotId,
          "the kill switch and the app snapshot survive a JSON round trip");
    check(back.app.enabled && back.app.upRps == 40 &&
              back.runner.snapshotId == round.runner.snapshotId &&
              back.runner.runner && !back.app.runner &&
              back.app.downFraction == 0.5,
          "settings survive a JSON round trip");
    const QJsonObject wire = as::settingsToJson(defaults);
    check(wire.value(QStringLiteral("app")).toObject().value(QStringLiteral("scaleUp"))
                  .toObject().contains(QStringLiteral("loopLagMs")) &&
              wire.value(QStringLiteral("runner")).toObject()
                  .value(QStringLiteral("scaleDown")).toObject()
                  .contains(QStringLiteral("idleMinutes")) &&
              wire.value(QStringLiteral("app")).toObject()
                  .contains(QStringLiteral("snapshotId")) &&
              wire.value(QStringLiteral("killSwitch")) == QJsonValue(false),
          "each pool writes the scaler's own field names");

    as::Settings bad = defaults;
    bad.app.max = 5;
    check(!as::validateSettings(bad).isEmpty(), "more than 4 nodes is refused");
    bad = defaults;
    bad.runner.min = 3;
    bad.runner.max = 1;
    check(!as::validateSettings(bad).isEmpty(), "min above max is refused");
    bad = defaults;
    bad.app.cooldownMinutes = 2;
    check(!as::validateSettings(bad).isEmpty(), "a short cooldown is refused");
    bad = defaults;
    bad.app.monthlyCapCents = 20000;
    check(!as::validateSettings(bad).isEmpty(), "a cap above $100 is refused");
    bad = defaults;
    bad.app.plan = QStringLiteral("vc2 1c; rm");
    check(!as::validateSettings(bad).isEmpty(), "a malformed plan is refused");
    bad = defaults;
    bad.runner.snapshotId = QStringLiteral("not-an-id");
    check(!as::validateSettings(bad).isEmpty(), "a malformed snapshot id is refused");
    bad = defaults;
    bad.app.upP95Ms = 0;
    bad.app.upLoopLagMs = 0;
    bad.app.upLoadPerCpu = 0;
    bad.app.upRps = 0;
    check(!as::validateSettings(bad).isEmpty(), "an app pool with no signal is refused");

    const QByteArray output = R"(warning line
{"ok":true,"verified":true,"settingsError":"",
 "settings":{"version":1,"app":{"enabled":true,"dryRun":false,"max":3}},
 "service":{"active":true,"unit":"cores","owner":"n2","lastTickMs":1000},
 "pools":{"app":{"nodes":[{"name":"a1","label":"fmnext-auto-1","ip":"192.0.2.21",
   "state":"serving","createdMs":1000,"instanceId":"abc"},
   {"name":"x","label":"fmnext-core-1","ip":"192.0.2.1"}],
   "snapshot":{"id":"snap-1","builtMs":5000},
   "decision":{"action":"up","reason":"p95 812 ms > 500 ms for 5 min","atMs":2000},
   "lastActionMs":1500,"spentCents":1.25,"month":"2026-09",
   "signals":{"p95Ms":812,"loopLagMs":null,"loadPerCpu":0.4,"rps":12.5}},
  "runner":{"nodes":[],"decision":{"action":"none"},"signals":{"queued":0,"oldestWaitS":0}}},
 "recent":[{"ts":3000,"pool":"app","event":"scale_up","detail":"a1 created","ok":true}]})";
    as::Status status = as::parseStatus(output);
    check(status.parsed && status.ok && status.verified && status.serviceActive,
          "the scaler status parses past a leading log line");
    check(status.settings.app.enabled && !status.settings.app.dryRun &&
              status.settings.app.max == 3 && status.settings.app.min == 0,
          "missing settings fields fall back to the defaults");
    check(status.serviceOwner == QStringLiteral("n2") &&
              status.serviceLastTickMs == 1000 &&
              as::serviceLine(status, 41000) ==
                  QStringLiteral("Scaler running on the cores (owner n2, last tick 40 s ago)."),
          "the service line names the owner core and its last tick");
    check(status.app.snapshotId == QStringLiteral("snap-1") &&
              status.app.snapshotBuiltMs == 5000,
          "the pool's boot snapshot parses");
    const QJsonObject instances = as::autoscaledInstancesJson(status);
    check(instances.size() == 1 &&
              instances.value(QStringLiteral("fmnext-auto-1")).toObject() ==
                  QJsonObject{{QStringLiteral("id"), QStringLiteral("abc")},
                              {QStringLiteral("ip"), QStringLiteral("192.0.2.21")}},
          "only autoscaled labels reach autoscaled-instances.json");
    as::Status stopped = status;
    stopped.serviceActive = false;
    stopped.settings.killSwitch = true;
    check(as::serviceLine(stopped, 41000).startsWith(QStringLiteral("KILL SWITCH ON")) &&
              as::serviceLine(stopped, 41000).contains(QStringLiteral("No core has run")),
          "a stopped scaler and the kill switch are called out");
    status.app.nodes.removeLast();
    check(status.app.nodes.size() == 1 &&
              status.app.nodes.first().name == QStringLiteral("a1") &&
              status.app.nodes.first().createdMs == 1000 &&
              status.app.decisionAction == QStringLiteral("up") &&
              status.app.spentCents == 1.25 && status.runner.nodes.isEmpty() &&
              status.recent.size() == 1,
          "pool nodes, decision, spend and recent events parse");
    check(as::formatSignals(false, status.app.signalValues) ==
              QStringLiteral("p95 812 ms · loop lag n/a · load/CPU 0.40 · 12.5 req/s"),
          "missing signals read n/a");
    check(as::decisionLine(status.app, 2000 + 30000).startsWith(
              QStringLiteral("scale up: p95 812 ms")),
          "the decision line names the action and the reason");
    check(!as::parseStatus("Traceback: boom").parsed,
          "a crash with no JSON is not a status");
    check(as::formatAge(0, 5) == QStringLiteral("—") &&
              as::formatAge(0 + 1, 1 + 90 * 60000) == QStringLiteral("1 h 30 min"),
          "node ages read in minutes and hours");
    check(as::scaleUpCostNote(defaults.app).contains(QStringLiteral("$5.00/month")) &&
              as::scaleUpCostNote(defaults.app).contains(QStringLiteral("$15.00")),
          "the scale-up confirmation names the plan rate and the cap");
    check(as::scaleArguments(QStringLiteral("/s/next_autoscale.py"), true, false, true) ==
              QStringList({QStringLiteral("/s/next_autoscale.py"), QStringLiteral("scale"),
                           QStringLiteral("runner"), QStringLiteral("down"),
                           QStringLiteral("--execute")}),
          "manual scale runs the scaler's own scale command");
}

void checkMirrorActionsRecoveryLock()
{
    namespace ma = forkmesh::mirror_actions;
    const QString dataRoot = QStringLiteral("/data/root");
    for (const QString &registryName :
         {QStringLiteral("\\HKEY_CURRENT_USER\\Software\\ForkMesh\\ForkMesh"),
          QStringLiteral("/HKEY_CURRENT_USER/Software/ForkMesh/ForkMesh"),
          QStringLiteral("\\HKEY_LOCAL_MACHINE\\Software\\ForkMesh")}) {
        check(!ma::detail::settingsNameIsFilePath(registryName,
                                                  QSettings::NativeFormat),
              "a Windows registry settings name is never treated as a file");
        check(ma::detail::transactionDirectoryFor(
                  registryName, QSettings::NativeFormat, dataRoot) ==
                  QStringLiteral("/data/root/actions"),
              "registry-backed settings keep the Actions lock in the data root");
    }
    check(ma::detail::transactionDirectoryFor(
              QStringLiteral("/home/u/projects/forkmesh/config/ForkMesh.conf"),
              QSettings::NativeFormat, dataRoot) ==
              QStringLiteral("/home/u/projects/forkmesh/config"),
          "file-backed settings keep the lock beside the settings file");
    check(ma::detail::transactionDirectoryFor(
              {}, QSettings::NativeFormat, dataRoot) ==
              QStringLiteral("/data/root/actions"),
          "settings without a file name fall back to the data root");

    // An uncreatable lock directory (the Windows symptom): startup recovery
    // reports configuration_lock_unavailable, but returns and only suspends
    // mirror Actions; a retry after the cause is gone resumes them.
    ma::detail::resetStartupRecoveryStatusForTesting();
    QTemporaryDir root;
    check(root.isValid(), "recovery-lock fixture root is available");
    const QString blocker = QDir(root.path()).filePath(QStringLiteral("blocked"));
    writeBytes(blocker, "not a directory");
    {
        QSettings blocked(QDir(blocker).filePath(QStringLiteral("settings.ini")),
                          QSettings::IniFormat);
        const ma::StartupRecoveryStatus failed = ma::recoverAtStartup(blocked);
        check(failed.attempted && !failed.ok &&
                  failed.errorCode ==
                      QStringLiteral("configuration_lock_unavailable") &&
                  !failed.lockPath.isEmpty(),
              "an uncreatable lock directory reports configuration_lock_unavailable");
        check(ma::actionsSuspendedByRecoveryFailure() &&
                  ma::startupRecoveryStatus().errorCode == failed.errorCode,
              "startup continues with mirror Actions suspended after a recovery failure");
        QFile::remove(blocker);
        check(QDir().mkpath(blocker), "blocked fixture becomes a directory");
        const ma::StartupRecoveryStatus retried = ma::recoverAtStartup(blocked);
        check(retried.ok && !ma::actionsSuspendedByRecoveryFailure(),
              "retrying recovery once the lock is creatable resumes mirror Actions");
    }

    // A stale lock left by a dead process is taken over at once.
    const QString settingsPath =
        QDir(root.path()).filePath(QStringLiteral("stuck/settings.ini"));
    QDir().mkpath(QFileInfo(settingsPath).absolutePath());
    QSettings settings(settingsPath, QSettings::IniFormat);
    settings.setValue(QStringLiteral("node/machineName"),
                      QStringLiteral("mirror2"));
    settings.sync();
    const QString lockPath = ma::configurationRecoveryLockPath(settings);
    const QString journalPath =
        ma::configurationRecoveryJournalPath(settings);
    writeBytes(lockPath, "4194301\n\n\n");
    check(ma::recoverAtStartup(settings).ok,
          "a lock left by a dead process is treated as stale");

    // An unreadable lock nobody can take: recovery fails (non-fatally) until
    // reset clears exactly the lock and journal files.
    ma::detail::setConfigurationLockTimeoutMsForTesting(200);
    writeBytes(lockPath, "\xff\xfe garbage that is not a QLockFile");
    writeBytes(journalPath, "{\"not\":\"a journal\"}");
    const QString dir = QFileInfo(lockPath).absolutePath();
    const QStringList decoys{
        QDir(dir).filePath(
            QStringLiteral(".forkmesh-actions-configuration-000000000000000000000000.lock")),
        QDir(dir).filePath(
            QStringLiteral(".forkmesh-actions-configuration-000000000000000000000000.json")),
        lockPath + QStringLiteral(".bak"),
        journalPath + QStringLiteral(".keep"),
        QDir(dir).filePath(QStringLiteral("identity.key")),
    };
    for (const QString &decoy : decoys)
        writeBytes(decoy, "keep me");
    const ma::StartupRecoveryStatus stuck = ma::recoverAtStartup(settings);
    check(!stuck.ok && ma::actionsSuspendedByRecoveryFailure(),
          "an unacquirable lock fails recovery without aborting startup");

    // A lock held by a live process is never pulled away by reset.
    QFile::remove(lockPath);
    {
        QLockFile held(lockPath);
        check(held.tryLock(0), "test holds the configuration lock");
        QString busy;
        const QStringList none =
            ma::resetConfigurationRecoveryFiles(settings, &busy);
        check(none.isEmpty() && busy == QStringLiteral("configuration_busy") &&
                  QFileInfo::exists(journalPath),
              "reset refuses while a live process holds the lock");
    }
    writeBytes(lockPath, "\xff\xfe garbage that is not a QLockFile");

    QString resetError;
    QStringList removed =
        ma::resetConfigurationRecoveryFiles(settings, &resetError);
    removed.sort();
    QStringList expected{journalPath, lockPath};
    expected.sort();
    check(resetError.isEmpty() && removed == expected &&
              !QFileInfo::exists(lockPath) && !QFileInfo::exists(journalPath),
          "reset removes exactly the stuck lock and journal files");
    bool decoysKept = QFileInfo::exists(settingsPath);
    for (const QString &decoy : decoys)
        decoysKept = decoysKept && QFileInfo::exists(decoy);
    check(decoysKept, "reset leaves every other file in the folder alone");
    check(ma::recoverAtStartup(settings).ok &&
              !ma::actionsSuspendedByRecoveryFailure(),
          "recovery succeeds after reset");
    ma::detail::setConfigurationLockTimeoutMsForTesting(0);
    ma::detail::resetStartupRecoveryStatusForTesting();
}

} // namespace

int main(int argc, char **argv)
{
    // Qt's test mode still writes below ~/.qttest, which is both persistent
    // across runs and unavailable when a developer or CI sandbox mounts the
    // home directory read-only. Keep every standard-path fixture inside one
    // process-owned temporary root instead.
    QTemporaryDir standardPathsRoot;
    check(standardPathsRoot.isValid(),
          "the isolated standard-path root is available");
    const QString standardPathsBase = standardPathsRoot.path();
    qputenv("XDG_DATA_HOME",
            QDir(standardPathsBase).filePath(QStringLiteral("data")).toUtf8());
    qputenv("XDG_CONFIG_HOME",
            QDir(standardPathsBase).filePath(QStringLiteral("config")).toUtf8());
    qputenv("XDG_CACHE_HOME",
            QDir(standardPathsBase).filePath(QStringLiteral("cache")).toUtf8());
    qputenv("XDG_STATE_HOME",
            QDir(standardPathsBase).filePath(QStringLiteral("state")).toUtf8());
    QStandardPaths::setTestModeEnabled(false);
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("ForkMeshTests"));
    app.setApplicationName(QStringLiteral("control-node-security"));

    checkNodeRoles();
    checkCatalogMirrorLiveness();
    checkFleetAutoscaleModel();

    forkmesh::control::CloudflareBootstrapRequest request;
    request.hostname = QStringLiteral("mirror.example.com");
    request.zoneName = QStringLiteral("example.com");
    request.accountId = QStringLiteral("account_123");
    request.nodeName = QStringLiteral("alice-mirror");
    request.relayLabel = QStringLiteral("Alice mirror");
    request.mainRelayUrl = QStringLiteral("https://forkmesh.com");
    check(forkmesh::control::validateCloudflareBootstrapRequest(request).isEmpty(),
          "valid public Cloudflare fields are accepted");

    auto invalid = request;
    invalid.hostname = QStringLiteral("https://mirror.example.com/private");
    check(!forkmesh::control::validateCloudflareBootstrapRequest(invalid).isEmpty(),
          "URL-shaped hostname is rejected");
    invalid = request;
    invalid.hostname = QStringLiteral("mirror.other.example");
    check(!forkmesh::control::validateCloudflareBootstrapRequest(invalid).isEmpty(),
          "hostname outside the zone is rejected");
    invalid = request;
    invalid.mainRelayUrl =
        QStringLiteral("https://user:pass@forkmesh.com/private?token=x");
    check(!forkmesh::control::validateCloudflareBootstrapRequest(invalid).isEmpty(),
          "credentialed/private upstream URL is rejected");
    forkmesh::control::CloudflareBootstrapRequest automaticRequest;
    automaticRequest.mainRelayUrl = QStringLiteral("https://forkmesh.com");
    check(forkmesh::control::validateCloudflareBootstrapRequest(
              automaticRequest, true).isEmpty(),
          "empty topology is accepted only for fail-closed token discovery");
    check(!forkmesh::control::validateCloudflareBootstrapRequest(
               automaticRequest).isEmpty(),
          "empty topology is rejected outside automatic discovery");

    const QString token =
        QStringLiteral("cloudflare-token-that-must-never-enter-argv");
    const auto command =
        forkmesh::control::buildCloudflareBootstrapCommand(
            request, token, QStringLiteral("/repo/tools/cloudflare_bootstrap.py"),
            QStringLiteral("/usr/bin/python3"),
            QStringLiteral("/opt/Fork Mesh/forkmesh"),
            base64Url(QByteArray(32, 'P')));
    check(command.program == QStringLiteral("/usr/bin/python3"),
          "python program is preserved");
    check(!command.arguments.join(QChar(u'\0')).contains(token),
          "Cloudflare token is absent from argv");
    check(command.environment.value(QStringLiteral("CLOUDFLARE_API_TOKEN")) ==
              token,
          "Cloudflare token is passed through the child environment");
    check(!command.arguments.contains(QStringLiteral("--secret-env")),
          "desktop launcher never uploads a Worker secret");
    check(command.arguments.contains(QStringLiteral("--json-stdout")),
          "desktop requests a bounded machine-readable deployment result");
    check(command.arguments.contains(QStringLiteral("--mirror-public-key")),
          "desktop identity public key is passed to manifest bootstrap");
    check(command.arguments.contains(
              QStringLiteral("--manifest-signer-command")),
          "local external manifest signer is configured");
    check(!command.arguments.contains(QStringLiteral("--skip-mirror-manifest")),
          "valid local identity does not skip the signed mirror manifest");
    const int signerIndex =
        command.arguments.indexOf(QStringLiteral("--manifest-signer-command"));
    check(signerIndex >= 0 &&
              command.arguments.value(signerIndex + 1).contains(
                  QStringLiteral("--sign-mirror-manifest")),
          "signer command invokes the dedicated local signer mode");

    const auto automaticCommand =
        forkmesh::control::buildCloudflareBootstrapCommand(
            automaticRequest, token,
            QStringLiteral("/repo/tools/cloudflare_bootstrap.py"),
            QStringLiteral("/usr/bin/python3"), QString(), QString());
    check(automaticCommand.arguments.contains(
              QStringLiteral("--auto-configure")) &&
              !automaticCommand.arguments.contains(
                  QStringLiteral("--hostname")) &&
              !automaticCommand.arguments.contains(
                  QStringLiteral("--zone")),
          "token-only command discovers topology without empty hostname arguments");
    check(!automaticCommand.arguments.join(QChar(u'\0')).contains(token),
          "token-only discovery also keeps the API token out of argv");

    const QJsonObject bootstrapResult{
        {QStringLiteral("ok"), true},
        {QStringLiteral("hostname"),
         QStringLiteral("forkmesh.example.com")},
        {QStringLiteral("zoneName"), QStringLiteral("example.com")},
        {QStringLiteral("accountId"), QStringLiteral("account-1")},
        {QStringLiteral("nodeName"), QStringLiteral("forkmesh-node")},
        {QStringLiteral("directMirrorHostname"),
         QStringLiteral("mirror.example.com")},
    };
    const QByteArray encodedResult =
        QJsonDocument(bootstrapResult)
            .toJson(QJsonDocument::Compact)
            .toBase64(QByteArray::Base64UrlEncoding |
                      QByteArray::OmitTrailingEquals);
    QString resultError;
    const QJsonObject parsedResult =
        forkmesh::control::parseCloudflareBootstrapResult(
            QByteArrayLiteral("human progress\nFORKMESH_BOOTSTRAP_RESULT=") +
                encodedResult + QByteArrayLiteral("\n"),
            &resultError);
    check(parsedResult.value(QStringLiteral("hostname")).toString() ==
              QStringLiteral("forkmesh.example.com") &&
              resultError.isEmpty(),
          "desktop decodes one bounded successful bootstrap result");
    check(forkmesh::control::parseCloudflareBootstrapResult(
              QByteArrayLiteral("FORKMESH_BOOTSTRAP_RESULT=bad\n"
                                "FORKMESH_BOOTSTRAP_RESULT=bad\n"),
              &resultError).isEmpty(),
          "duplicate machine results fail closed");

    const auto tunnelCommand =
        forkmesh::control::buildCloudflareTunnelBootstrapCommand(
            request, QStringLiteral("direct.example.com"), token,
            QStringLiteral("/repo/tools/cloudflare_tunnel_bootstrap.py"),
            QStringLiteral("/usr/bin/python3"),
            QStringLiteral("/opt/Fork Mesh/forkmesh"),
            base64Url(QByteArray(32, 'P')),
            QStringLiteral("/state/gateway/config.json"),
            QStringLiteral("/state/gateway/forkmesh-mirror.json"),
            QStringLiteral("/state/gateway/connector.token"));
    check(!tunnelCommand.arguments.join(QChar(u'\0')).contains(token) &&
              tunnelCommand.environment.value(
                  QStringLiteral("CLOUDFLARE_API_TOKEN")) == token,
          "Tunnel provisioning keeps the Cloudflare API token out of argv");
    check(tunnelCommand.arguments.contains(
              QStringLiteral("--tunnel-token-file")) &&
              tunnelCommand.arguments.contains(
                  QStringLiteral("--manifest-output")) &&
              tunnelCommand.arguments.contains(
                  QStringLiteral("--gateway-config")),
          "Tunnel command provisions restartable local connector state and a signed manifest");
    check(tunnelCommand.arguments.contains(
              QStringLiteral("direct.example.com")),
          "Tunnel command uses a distinct direct mirror hostname");

    const QString redacted = forkmesh::control::redactProcessOutput(
        QStringLiteral("token=%1\nAuthorization: Bearer abcdefghijk\n")
            .arg(token),
        {token});
    check(!redacted.contains(token), "exact API token is redacted from output");
    check(!redacted.contains(QStringLiteral("abcdefghijk")),
          "Bearer credential is redacted from output");
    check(redacted.contains(QStringLiteral("<redacted>")),
          "redaction marker is visible");

    check(forkmesh::control::isValidSolanaPublicAddress(
              QStringLiteral("11111111111111111111111111111111")),
          "32-byte base58 Solana public address is accepted");
    check(forkmesh::control::isValidSolanaPublicAddress(
              QStringLiteral("So11111111111111111111111111111111111111112")),
          "wrapped-SOL public address is accepted");
    check(!forkmesh::control::isValidSolanaPublicAddress(
              QStringLiteral("seed phrase words are not an address")),
          "seed phrase cannot be accepted as a public address");
    check(!forkmesh::control::isValidSolanaPublicAddress(
              QStringLiteral("O0Il")),
          "non-base58 private-looking input is rejected");

    const QUrl world = forkmesh::control::worldUrlForRelay(
        QStringLiteral("wss://relay.example.com/api/room?secret=no"));
    check(world.toString() == QStringLiteral("https://relay.example.com/world/"),
          "websocket relay becomes the HTTPS World portal");

    check(forkmesh::control::worldDevServerUrl(QString()).toString() ==
              QStringLiteral("http://127.0.0.1:8788/world/"),
          "empty World dev setting yields the default local dev URL");
    check(forkmesh::control::worldDevServerUrl(
              QStringLiteral("localhost:9099")).toString() ==
              QStringLiteral("http://localhost:9099/world/"),
          "bare host:port World dev setting is normalized to /world/");
    check(!forkmesh::control::worldDevServerUrl(QStringLiteral("OFF")).isValid(),
          "\"off\" disables the World dev probe");
    check(!forkmesh::control::worldDevServerUrl(
               QStringLiteral("http://evil.example.com/world/")).isValid(),
          "non-loopback World dev URL fails closed");
    check(!forkmesh::control::worldDevServerUrl(
               QStringLiteral("https://127.0.0.1:8788/")).isValid(),
          "the local World dev probe stays plain http");

    const QStringList mirrorOwners =
        forkmesh::control::directMirrorRepositoryOwners(
            QStringLiteral("forkmesh"), QStringLiteral("jett"));
    check(mirrorOwners ==
              QStringList{QStringLiteral("forkmesh"),
                          QStringLiteral("jett")},
          "direct mirror exposes canonical and catalog owner aliases");
    check(forkmesh::control::directMirrorRepositoryOwners(
              QStringLiteral("ForkMesh"), QStringLiteral("forkmesh")) ==
              QStringList{QStringLiteral("forkmesh")},
          "identical normalized direct mirror aliases are emitted once");
    check(forkmesh::control::directMirrorRepositoryOwners(
              QStringLiteral("../private"), QStringLiteral("jett")) ==
              QStringList{QStringLiteral("jett")},
          "invalid direct mirror owner aliases fail closed");

    // A relay-only node leaves the direct gateway settings empty on purpose:
    // background refreshes ask this before rebuilding, so "not configured"
    // has to be reported as such instead of looking like a broken endpoint.
    const QString routerKey = base64Url(QByteArray(32, 'R'));
    check(forkmesh::control::isValidMirrorRouterPublicKey(routerKey),
          "a 32-byte base64url router key is accepted");
    check(!forkmesh::control::isValidMirrorRouterPublicKey(
              base64Url(QByteArray(31, 'R'))),
          "a short router key is rejected");
    check(!forkmesh::control::isValidMirrorRouterPublicKey(
              QString(routerKey).replace(0, 1, QLatin1Char('+'))),
          "a non-base64url router key is rejected");
    check(forkmesh::control::directMirrorGatewayIsConfigured(
              QStringLiteral("mirror1"),
              QStringLiteral("https://mirror1.example.com"), routerKey),
          "a complete direct gateway configuration is recognized");
    check(!forkmesh::control::directMirrorGatewayIsConfigured(
              QString(), QString(), QString()),
          "a relay-only node reports no direct gateway configuration");
    check(!forkmesh::control::directMirrorGatewayIsConfigured(
              QStringLiteral("mirror1"), QString(), routerKey),
          "an unusable mirror hostname leaves the gateway unconfigured");
    check(!forkmesh::control::directMirrorGatewayIsConfigured(
              QStringLiteral("-mirror1"),
              QStringLiteral("https://mirror1.example.com"), routerKey),
          "a node name that is not a DNS label leaves the gateway "
          "unconfigured");
    check(!forkmesh::control::directMirrorGatewayIsConfigured(
              QStringLiteral("mirror1"),
              QStringLiteral("https://mirror1.example.com"),
              QStringLiteral("not-a-router-key")),
          "a malformed router key leaves the gateway unconfigured");

    const QString publicKey = base64Url(QByteArray(32, 'K'));
    const QByteArray payload =
        QJsonDocument(QJsonObject{
                          {QStringLiteral("type"),
                           QStringLiteral("forkmesh.mirror-endpoint")},
                          {QStringLiteral("schemaVersion"), 1},
                      })
            .toJson(QJsonDocument::Compact);
    const QString digest = QString::fromLatin1(
        QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
    QJsonObject signingRequest{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("type"),
         QStringLiteral("forkmesh.mirror-endpoint-signing-request")},
        {QStringLiteral("algorithm"), QStringLiteral("Ed25519")},
        {QStringLiteral("encoding"), QStringLiteral("base64url-no-padding")},
        {QStringLiteral("canonicalization"),
         QStringLiteral("forkmesh-json-sort-v1")},
        {QStringLiteral("publicKey"), publicKey},
        {QStringLiteral("payloadBase64"), base64Url(payload)},
        {QStringLiteral("payloadSha256"), digest},
    };
    QString signingError;
    check(forkmesh::control::mirrorManifestSigningPayload(
              signingRequest, publicKey, &signingError) == payload,
          "valid manifest signing request yields exact canonical payload");
    signingRequest.insert(QStringLiteral("payloadSha256"),
                          QString(64, QLatin1Char('0')));
    check(forkmesh::control::mirrorManifestSigningPayload(
              signingRequest, publicKey, &signingError).isEmpty(),
          "manifest checksum mismatch is rejected");
    signingRequest.insert(QStringLiteral("payloadSha256"), digest);
    check(forkmesh::control::mirrorManifestSigningPayload(
              signingRequest, base64Url(QByteArray(32, 'Z')),
              &signingError).isEmpty(),
          "manifest request for another identity is rejected");

    const QJsonObject canonicalVector{
        {QStringLiteral("z"),
         QString::fromUtf8("snowman \xE2\x98\x83 and \xF0\x9F\x98\x80")},
        {QStringLiteral("a"),
         QJsonArray{0, true, QStringLiteral("line\n")}},
    };
    const QByteArray expectedCanonical =
        QByteArrayLiteral(
            "{\"a\":[0,true,\"line\\n\"],\"z\":\"snowman "
            "\\u2603 and \\ud83d\\ude00\"}");
    check(forkmesh::control::pythonCanonicalJson(
              canonicalVector, &signingError) == expectedCanonical,
          "catalog canonical JSON matches Python sorted ensure-ascii encoding");
    check(forkmesh::control::catalogV2SigningPayload(
              canonicalVector, &signingError) ==
              QByteArrayLiteral(
                  "forkmesh-catalog-v2\n"
                  "2a5d6a869bf7835724b35f3a16660dfc8f683adcc60ca2b367cd669d7fc03e97"),
          "catalog-v2 signing payload matches the cross-language hash vector");

    const QJsonObject boundedTelemetry =
        forkmesh::control::normalizedCatalogHostTelemetry(
            QJsonObject{
                {QStringLiteral("cpuPercent"), 149},
                {QStringLiteral("memUsedBytes"), 900},
                {QStringLiteral("memTotalBytes"), 800},
                {QStringLiteral("diskUsedBytes"), 300},
                {QStringLiteral("diskTotalBytes"), 1000},
            });
    check(boundedTelemetry.value(QStringLiteral("cpuPercent")).toInt() == 100 &&
              boundedTelemetry.value(QStringLiteral("memUsedBytes")).toInt() == 800 &&
              boundedTelemetry.value(QStringLiteral("memTotalBytes")).toInt() == 800 &&
              boundedTelemetry.value(QStringLiteral("diskUsedBytes")).toInt() == 300 &&
              boundedTelemetry.value(QStringLiteral("diskTotalBytes")).toInt() == 1000,
          "catalog telemetry is bounded before canonical signing");
    const QJsonObject unknownTelemetry =
        forkmesh::control::normalizedCatalogHostTelemetry(
            QJsonObject{
                {QStringLiteral("cpuPercent"), -1},
                {QStringLiteral("memUsedBytes"), 100},
                {QStringLiteral("diskUsedBytes"), 10},
                {QStringLiteral("diskTotalBytes"), 0},
            });
    check(unknownTelemetry.value(QStringLiteral("cpuPercent")).isNull() &&
              unknownTelemetry.value(QStringLiteral("memUsedBytes")).isNull() &&
              unknownTelemetry.value(QStringLiteral("memTotalBytes")).isNull() &&
              unknownTelemetry.value(QStringLiteral("diskUsedBytes")).isNull() &&
              unknownTelemetry.value(QStringLiteral("diskTotalBytes")).isNull(),
          "unshared or partial catalog telemetry stays null");

    const QByteArray routePayload =
        forkmesh::control::privateReplicaRouteSigningPayload(
            QStringLiteral("alice"), QStringLiteral("private-project"),
            QStringLiteral("alice-mirror"), QString(64, QLatin1Char('a')),
            QString(64, QLatin1Char('b')), 3, true, 1784800000000LL,
            &signingError);
    check(routePayload ==
              QByteArrayLiteral(
                  "forkmesh-private-route-v1\nalice\nprivate-project\n"
                  "alice-mirror\n"
                  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
                  "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
                  "3\n1\n1784800000000"),
          "private-replica route signing canonical matches Worker contract");
    check(forkmesh::control::privateReplicaRouteSigningPayload(
              QStringLiteral("alice\nmallory"),
              QStringLiteral("private-project"),
              QStringLiteral("alice-mirror"), QString(64, QLatin1Char('a')),
              QString(64, QLatin1Char('b')), 3, true, 1784800000000LL,
              &signingError).isEmpty(),
          "private route canonical rejects delimiter injection");

    check(forkmesh::control::httpsMirrorRegistrationSigningPayload(
              QStringLiteral("alice-mirror"),
              QStringLiteral("https://direct.example.com"),
              publicKey, 1784800000000LL, &signingError) ==
              QByteArrayLiteral(
                  "forkmesh-https-endpoint-v1\nalice-mirror\n"
                  "https://direct.example.com\n") +
                  publicKey.toUtf8() +
                  QByteArrayLiteral("\n1784800000000"),
          "direct endpoint registration canonical matches Worker contract");
    check(forkmesh::control::httpsMirrorRegistrationSigningPayload(
              QStringLiteral("alice-mirror"),
              QStringLiteral("http://direct.example.com/private"),
              publicKey, 1784800000000LL, &signingError).isEmpty(),
          "direct endpoint registration rejects non-HTTPS/private origins");

    QTemporaryDir root;
    check(root.isValid(), "temporary source tree exists");
    const QString client = root.filePath(QStringLiteral("desktop"));
    const QString tools = root.filePath(QStringLiteral("tools"));
    const QString worker = root.filePath(QStringLiteral("app"));
    check(QDir().mkpath(client) && QDir().mkpath(tools) &&
              QDir().mkpath(worker + QStringLiteral("/tools")) &&
              QDir().mkpath(worker + QStringLiteral("/src")) &&
              QDir().mkpath(worker + QStringLiteral("/public")) &&
              QDir().mkpath(worker + QStringLiteral("/migrations")),
          "temporary source directories created");
    const auto writeFixture = [](const QString &path,
                                 const QByteArray &contents) {
        QFile file(path);
        const bool opened =
            file.open(QIODevice::WriteOnly | QIODevice::Truncate);
        check(opened, "temporary deployment fixture created");
        if (opened)
            file.write(contents);
        file.close();
    };
    writeFixture(worker + QStringLiteral("/wrangler.toml"), "[vars]\n");
    writeFixture(worker + QStringLiteral("/pywrangler.sh"), "# pinned\n");
    writeFixture(worker + QStringLiteral("/pyproject.toml"), "[project]\n");
    writeFixture(worker + QStringLiteral("/tools/build_dashboard_assets.py"),
                 "# build\n");
    writeFixture(worker + QStringLiteral("/src/entry.py"), "# worker\n");
    writeFixture(worker + QStringLiteral("/src/public_snapshots.py"),
                 "# snapshots\n");
    QFile script(root.filePath(
        QStringLiteral("tools/cloudflare_bootstrap.py")));
    check(script.open(QIODevice::WriteOnly | QIODevice::Truncate),
          "temporary bootstrap script created");
    script.write("#!/usr/bin/env python3\n");
    script.close();
    check(forkmesh::control::findCloudflareBootstrapScript(client, QString()) ==
              QFileInfo(script).canonicalFilePath(),
          "repository-pinned bootstrap script resolves from Qt source dir");
    check(forkmesh::control::findCloudflareWorkerDirectory(client, QString()) ==
              QFileInfo(worker).canonicalFilePath(),
          "repository-pinned Worker directory resolves with the bootstrapper");
    check(forkmesh::control::findSiteDeployScript(client, QString()).isEmpty(),
          "site deploy button fails closed without app/deploy.sh");
    writeFixture(worker + QStringLiteral("/deploy.sh"),
                 "#!/usr/bin/env bash\n");
    check(forkmesh::control::findSiteDeployScript(client, QString()) ==
              QFileInfo(worker + QStringLiteral("/deploy.sh"))
                  .canonicalFilePath(),
          "site deploy script resolves beside the pinned Worker bundle");
    QTemporaryDir splitDeployRoot;
    const QString splitClient =
        splitDeployRoot.filePath(QStringLiteral("desktop"));
    const QString splitApp =
        splitDeployRoot.filePath(QStringLiteral("app"));
    check(QDir().mkpath(splitClient) && QDir().mkpath(splitApp),
          "split Worker deployment fixture directories created");
    writeFixture(splitApp + QStringLiteral("/deploy.sh"),
                 "#!/usr/bin/env bash\n");
    check(forkmesh::control::findSiteDeployScript(splitClient, QString()) ==
              QFileInfo(splitApp + QStringLiteral("/deploy.sh"))
                  .canonicalFilePath(),
          "site deploy orchestrator resolves from a split top-level app layout");
    const QString deployScript = splitApp + QStringLiteral("/deploy.sh");
    check(forkmesh::control::siteDeployArguments(deployScript,
                                                 QStringLiteral("changed"),
                                                 false) ==
              QStringList({deployScript, QStringLiteral("changed")}),
          "a normal site deployment lets deploy.sh skip unchanged workers");
    check(forkmesh::control::siteDeployArguments(deployScript,
                                                 QStringLiteral("changed"),
                                                 true) ==
              QStringList({deployScript, QStringLiteral("changed"),
                           QStringLiteral("--force")}),
          "the force control appends deploy.sh --force to the same target");
    check(forkmesh::control::siteDeployArguments(deployScript,
                                                 QStringLiteral(" App "), true)
                  .value(1) == QStringLiteral("app") &&
              forkmesh::control::siteDeployArguments(
                  deployScript, QStringLiteral("world"), true)
                      .value(2) == QStringLiteral("--force"),
          "force works for one named Worker, not only the changed sweep");
    check(forkmesh::control::siteDeployArguments(deployScript,
                                                 QStringLiteral("secrets"),
                                                 true)
              .isEmpty() &&
              forkmesh::control::siteDeployArguments(
                  QString(), QStringLiteral("app"), true)
                  .isEmpty(),
          "an unknown target or missing script never reaches deploy.sh");
    QFile tunnelScript(root.filePath(
        QStringLiteral("tools/cloudflare_tunnel_bootstrap.py")));
    QFile gatewayScript(root.filePath(
        QStringLiteral("tools/mirror_gateway.py")));
    check(tunnelScript.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
              gatewayScript.open(QIODevice::WriteOnly |
                                 QIODevice::Truncate),
          "direct mirror tools created");
    tunnelScript.write("#!/usr/bin/env python3\n");
    gatewayScript.write("#!/usr/bin/env python3\n");
    tunnelScript.close();
    gatewayScript.close();
    check(forkmesh::control::findCloudflareTunnelBootstrapScript(
              client, QString()) ==
              QFileInfo(tunnelScript).canonicalFilePath() &&
              forkmesh::control::findMirrorGatewayScript(
                  client, QString()) ==
              QFileInfo(gatewayScript).canonicalFilePath(),
          "repository-pinned Tunnel and gateway tools resolve independently");
    QFile installerScript(root.filePath(
        QStringLiteral("tools/cloudflared_install.py")));
    check(installerScript.open(
              QIODevice::WriteOnly | QIODevice::Truncate),
          "managed cloudflared installer fixture created");
    installerScript.write("#!/usr/bin/env python3\n");
    installerScript.close();
    check(forkmesh::control::findCloudflaredInstallerScript(
              client, QString()) ==
              QFileInfo(installerScript).canonicalFilePath(),
          "repository-pinned managed cloudflared installer resolves");
    QFile autodeployScript(root.filePath(
        QStringLiteral("tools/next_autodeploy.py")));
    check(autodeployScript.open(
              QIODevice::WriteOnly | QIODevice::Truncate),
          "core node deploy tool fixture created");
    autodeployScript.write("#!/usr/bin/env python3\n");
    autodeployScript.close();
    check(forkmesh::control::findNextAutodeployScript(client, QString()) ==
              QFileInfo(autodeployScript).canonicalFilePath(),
          "the core node deploy tool resolves from the checkout's tools");

    QTemporaryDir installedRoot;
    check(installedRoot.isValid(), "temporary installed layout exists");
    const QString installedBin =
        installedRoot.filePath(QStringLiteral("bin"));
    const QString installedResources =
        installedRoot.filePath(QStringLiteral("share/forkmesh"));
    const QString installedTools =
        installedResources + QStringLiteral("/tools");
    const QString installedWorker = installedResources + QStringLiteral("/app");
    check(QDir().mkpath(installedBin) &&
              QDir().mkpath(installedTools) &&
              QDir().mkpath(installedWorker + QStringLiteral("/tools")) &&
              QDir().mkpath(installedWorker + QStringLiteral("/src")) &&
              QDir().mkpath(installedWorker + QStringLiteral("/public")) &&
              QDir().mkpath(installedWorker + QStringLiteral("/migrations")),
          "CMake-style installed resource directories created");
    writeFixture(installedTools + QStringLiteral("/cloudflare_bootstrap.py"),
                 "#!/usr/bin/env python3\n");
    writeFixture(
        installedTools + QStringLiteral("/cloudflare_tunnel_bootstrap.py"),
        "#!/usr/bin/env python3\n");
    writeFixture(installedTools + QStringLiteral("/mirror_gateway.py"),
                 "#!/usr/bin/env python3\n");
    writeFixture(installedTools + QStringLiteral("/cloudflared_install.py"),
                 "#!/usr/bin/env python3\n");
    writeFixture(installedWorker + QStringLiteral("/wrangler.toml"),
                 "[vars]\n");
    writeFixture(installedWorker + QStringLiteral("/pywrangler.sh"),
                 "# pinned\n");
    writeFixture(installedWorker + QStringLiteral("/pyproject.toml"),
                 "[project]\n");
    writeFixture(
        installedWorker + QStringLiteral("/tools/build_dashboard_assets.py"),
        "# build\n");
    writeFixture(installedWorker + QStringLiteral("/src/entry.py"),
                 "# worker\n");
    writeFixture(installedWorker + QStringLiteral("/src/public_snapshots.py"),
                 "# snapshots\n");
    const QString installedBootstrap =
        forkmesh::control::findCloudflareBootstrapScript(
            QString(), installedBin);
    check(installedBootstrap ==
              QFileInfo(installedTools +
                        QStringLiteral("/cloudflare_bootstrap.py"))
                  .canonicalFilePath(),
          "bootstrap resolves from the Linux CMake installed layout without a source checkout");
    check(forkmesh::control::findCloudflareWorkerDirectory(
              QString(), installedBin) ==
              QFileInfo(installedWorker).canonicalFilePath(),
          "installed Worker directory resolves with the bootstrapper");
    writeFixture(installedWorker + QStringLiteral("/deploy.sh"),
                 "#!/usr/bin/env bash\n");
    check(forkmesh::control::findSiteDeployScript(QString(), installedBin) ==
              QFileInfo(installedWorker + QStringLiteral("/deploy.sh"))
                  .canonicalFilePath(),
          "site deploy script resolves from installed resources");
    check(forkmesh::control::findCloudflareTunnelBootstrapScript(
              QString(), installedBin) ==
                  QFileInfo(
                      installedTools +
                      QStringLiteral("/cloudflare_tunnel_bootstrap.py"))
                      .canonicalFilePath() &&
              forkmesh::control::findMirrorGatewayScript(
                  QString(), installedBin) ==
                  QFileInfo(installedTools +
                            QStringLiteral("/mirror_gateway.py"))
                      .canonicalFilePath() &&
              forkmesh::control::findCloudflaredInstallerScript(
                  QString(), installedBin) ==
                  QFileInfo(installedTools +
                            QStringLiteral("/cloudflared_install.py"))
                      .canonicalFilePath(),
          "all direct-mirror tools resolve from installed resources");

    QFile::remove(installedWorker + QStringLiteral("/src/public_snapshots.py"));
    check(forkmesh::control::findCloudflareBootstrapScript(
              QString(), installedBin).isEmpty(),
          "installed Worker bundle without snapshot validation fails closed");
    writeFixture(installedWorker + QStringLiteral("/src/public_snapshots.py"),
                 "# snapshots\n");
    QFile::remove(installedWorker + QStringLiteral("/src/entry.py"));
    check(forkmesh::control::findCloudflareBootstrapScript(
              QString(), installedBin).isEmpty(),
          "incomplete installed Worker bundle fails closed");
    check(forkmesh::control::findCloudflareWorkerDirectory(
              QString(), installedBin).isEmpty(),
          "incomplete installed Worker directory fails closed");

    const QString tailToken =
        QStringLiteral("cf-tail-token-must-never-enter-argv");
    const auto tailCommand =
        forkmesh::control::buildCloudflareTailCommand(
            tailToken, QStringLiteral("account_123"),
            QStringLiteral("/usr/bin/npx"));
    check(tailCommand.program == QStringLiteral("/usr/bin/npx") &&
              tailCommand.arguments ==
                  QStringList({QStringLiteral("--yes"),
                               QStringLiteral("wrangler@4.136.2"),
                               QStringLiteral("tail"),
                               QStringLiteral("--format"),
                               QStringLiteral("json")}),
          "Cloudflare tail uses a direct pinned Wrangler invocation");
    check(!tailCommand.arguments.join(QChar(u'\0')).contains(tailToken) &&
              tailCommand.environment.value(
                  QStringLiteral("CLOUDFLARE_API_TOKEN")) == tailToken &&
              tailCommand.environment.value(
                  QStringLiteral("CLOUDFLARE_ACCOUNT_ID")) ==
                  QStringLiteral("account_123"),
          "Cloudflare tail keeps credentials only in the child environment");
    check(forkmesh::control::buildCloudflareTailCommand(
              tailToken, QStringLiteral("invalid account!"),
              QStringLiteral("/usr/bin/npx")).program.isEmpty(),
          "Cloudflare tail rejects malformed account IDs");

    // --- Node selection (adhoc #1617). Wrangler exits 1 on Node < 22, so the
    // monitor has to find a new enough interpreter itself and put it in front of
    // the child's PATH: npx and the wrangler bin it spawns are both
    // `#!/usr/bin/env node`, and would otherwise pick the old node right back up.
    check(forkmesh::control::nodeMajorVersionFromText(
              QStringLiteral("v22.23.1\n")) == 22 &&
              forkmesh::control::nodeMajorVersionFromText(
                  QStringLiteral("v20.19.2")) == 20 &&
              forkmesh::control::nodeMajorVersionFromText(
                  QStringLiteral("not a version")) == 0,
          "Node versions are read out of `node --version` output");

    QTemporaryDir nodeRoot;
    check(nodeRoot.isValid(), "node toolchain fixture root is valid");
    // A version manager's tree: two installs side by side, only one of which
    // Wrangler will run on.
    const auto makeNodeInstall = [&nodeRoot](const QString &relativeBin,
                                             const QString &version,
                                             bool withNpx) {
        const QString bin = nodeRoot.filePath(relativeBin);
        QDir().mkpath(bin);
        const QString node = QDir(bin).absoluteFilePath(QStringLiteral("node"));
        QFile nodeFile(node);
        nodeFile.open(QIODevice::WriteOnly);
        nodeFile.write(QStringLiteral("#!/bin/sh\necho %1\n")
                           .arg(version)
                           .toUtf8());
        nodeFile.close();
        QFile::setPermissions(node, QFile::ReadOwner | QFile::WriteOwner |
                                        QFile::ExeOwner);
        if (withNpx) {
            const QString npx =
                QDir(bin).absoluteFilePath(QStringLiteral("npx"));
            QFile npxFile(npx);
            npxFile.open(QIODevice::WriteOnly);
            npxFile.write("#!/bin/sh\nexit 0\n");
            npxFile.close();
            QFile::setPermissions(npx, QFile::ReadOwner | QFile::WriteOwner |
                                           QFile::ExeOwner);
        }
        return QFileInfo(bin).canonicalFilePath();
    };
    const QString oldNodeBin = makeNodeInstall(
        QStringLiteral("versions/node/v20.19.2/bin"), QStringLiteral("v20.19.2"),
        true);
    const QString newNodeBin = makeNodeInstall(
        QStringLiteral("versions/node/v22.23.1/bin"), QStringLiteral("v22.23.1"),
        true);
    // A Node install with no npx is not a candidate at all.
    makeNodeInstall(QStringLiteral("versions/node/v24.0.0/bin"),
                    QStringLiteral("v24.0.0"), false);

    const QByteArray savedNvmDir = qgetenv("NVM_DIR");
    const QByteArray savedNodeBin = qgetenv("FORKMESH_NODE_BIN");
    // PATH is searched before any version manager, so a developer machine with
    // a new enough Node already on PATH would be chosen over the fixture. The
    // fixture's launchers are plain /bin/sh scripts and need nothing from PATH.
    const QByteArray savedPath = qgetenv("PATH");
    qputenv("PATH", nodeRoot.path().toUtf8());
    qputenv("NVM_DIR", nodeRoot.path().toUtf8());
    qunsetenv("FORKMESH_NODE_BIN");
    QStringList candidates = forkmesh::control::nodeBinDirectoryCandidates();
    check(candidates.contains(newNodeBin) && candidates.contains(oldNodeBin) &&
              candidates.indexOf(newNodeBin) < candidates.indexOf(oldNodeBin),
          "managed Node installs are offered newest first");
    check(!candidates.contains(
              QFileInfo(nodeRoot.filePath(
                            QStringLiteral("versions/node/v24.0.0/bin")))
                  .canonicalFilePath()),
          "a Node install without npx is not a candidate");

    qputenv("FORKMESH_NODE_BIN", oldNodeBin.toUtf8());
    candidates = forkmesh::control::nodeBinDirectoryCandidates();
    check(!candidates.isEmpty() && candidates.first() == oldNodeBin,
          "an explicit FORKMESH_NODE_BIN override is tried first");

    qunsetenv("FORKMESH_NODE_BIN");
    const auto toolchain = forkmesh::control::findNodeToolchain(22);
    check(toolchain.isValid() && toolchain.binDir == newNodeBin &&
              toolchain.majorVersion == 22 &&
              toolchain.npx ==
                  QDir(newNodeBin).absoluteFilePath(QStringLiteral("npx")),
          "the newest Node that satisfies Wrangler is the one selected");
    check(!forkmesh::control::findNodeToolchain(99).isValid(),
          "a machine with no new enough Node reports no toolchain");

    const auto pinnedTail = forkmesh::control::buildCloudflareTailCommand(
        tailToken, QStringLiteral("account_123"),
        QDir(newNodeBin).absoluteFilePath(QStringLiteral("npx")), newNodeBin);
    check(pinnedTail.environment.value(QStringLiteral("PATH"))
              .startsWith(newNodeBin + QDir::listSeparator()),
          "the tail runs with its own Node at the front of PATH");

    qputenv("PATH", savedPath);
    if (savedNvmDir.isEmpty())
        qunsetenv("NVM_DIR");
    else
        qputenv("NVM_DIR", savedNvmDir);
    if (savedNodeBin.isEmpty())
        qunsetenv("FORKMESH_NODE_BIN");
    else
        qputenv("FORKMESH_NODE_BIN", savedNodeBin);

    // --- The tail ending is normal, not terminal (adhoc #1623). Cloudflare
    // expires a tail session after roughly an hour and Wrangler exits 0 when the
    // server closes it, so a monitor that stops there leaves the Log page with
    // no Worker traffic at all for the rest of the session.
    const auto expired = forkmesh::control::planCloudflareTailRestart(
        0, forkmesh::control::kCloudTailHealthyUptimeMs + 1);
    check(expired.restart && !expired.giveUp && expired.delayMs > 0 &&
              expired.delayMs <= 5000,
          "a tail that had been streaming is replaced almost at once");
    // A tail that had been up for an hour is worth restarting however many
    // short-lived ones preceded it — the counter is about starts that never
    // took, and this one plainly did.
    check(forkmesh::control::planCloudflareTailRestart(
              99, forkmesh::control::kCloudTailHealthyUptimeMs)
              .restart,
          "a long-lived tail restarts whatever came before it");

    const auto firstFailure = forkmesh::control::planCloudflareTailRestart(0, 400);
    const auto secondFailure =
        forkmesh::control::planCloudflareTailRestart(1, 400);
    check(firstFailure.restart && secondFailure.restart &&
              secondFailure.delayMs > firstFailure.delayMs &&
              firstFailure.delayMs >= 1000,
          "a tail that dies young is retried on a widening backoff");
    int attempts = 0;
    while (forkmesh::control::planCloudflareTailRestart(attempts, 400).restart &&
           attempts < 50) {
        ++attempts;
    }
    const auto exhausted =
        forkmesh::control::planCloudflareTailRestart(attempts, 400);
    check(attempts > 1 && attempts < 10 && !exhausted.restart &&
              exhausted.giveUp,
          "a run of tails that never start gives up rather than spinning");

    check(forkmesh::control::cloudflareTailWorkerMissing(QStringLiteral(
              "This Worker does not exist on your account. [code: 10007]")) &&
              !forkmesh::control::cloudflareTailWorkerMissing(QStringLiteral(
                  "Successfully created tail, expires at 2026-09-25")),
          "Cloudflare's missing-Worker answer is recognised");
    {
        QTemporaryDir workerDir;
        QFile toml(workerDir.filePath(QStringLiteral("wrangler.toml")));
        check(toml.open(QIODevice::WriteOnly),
              "wrangler.toml fixture is writable");
        toml.write("# comment\nname = \"forkmesh-next-app\"\n"
                   "main = \"src/entry.py\"\n[[services]]\n"
                   "name = \"other\"\n");
        toml.close();
        check(forkmesh::control::cloudflareWorkerName(workerDir.path()) ==
                  QStringLiteral("forkmesh-next-app") &&
                  forkmesh::control::cloudflareWorkerName(QString()).isEmpty(),
              "the Worker name is the top-level name in wrangler.toml");
    }

    // --- Tail rendering (adhoc #1615): the viewer parses Wrangler's JSON so it
    // can show the user agent behind each hit and tell a failure from a hit.
    const auto okTail = forkmesh::control::parseCloudflareTailLine(
        QStringLiteral(
            "{\"outcome\":\"ok\",\"eventTimestamp\":1000,"
            "\"event\":{\"request\":{\"method\":\"GET\","
            "\"url\":\"https://app.forkmesh.com/api/status\","
            "\"headers\":{\"User-Agent\":\"Mozilla/5.0 ForkMeshBot\"}},"
            "\"response\":{\"status\":200}},"
            "\"logs\":[{\"level\":\"log\",\"message\":[\"served\",7]}],"
            "\"exceptions\":[]}"));
    check(okTail.parsed && !okTail.isError &&
              okTail.userAgent == QStringLiteral("Mozilla/5.0 ForkMeshBot") &&
              okTail.method == QStringLiteral("GET") &&
              okTail.status == 200 &&
              okTail.messages == QStringList({QStringLiteral("served 7")}),
          "a healthy Worker request decodes with its user agent");
    check(okTail.summary.contains(QStringLiteral("UA Mozilla/5.0 ForkMeshBot")) &&
              okTail.summary.contains(QStringLiteral("GET")) &&
              okTail.summary.contains(QStringLiteral("200")) &&
              okTail.summary.contains(
                  QStringLiteral("https://app.forkmesh.com/api/status")),
          "the rendered tail line carries the agent beside the request");

    const auto d1Tail = forkmesh::control::parseCloudflareTailLine(
        QStringLiteral(
            "{\"outcome\":\"ok\",\"eventTimestamp\":1500,"
            "\"event\":{\"request\":{\"method\":\"GET\","
            "\"url\":\"https://app.forkmesh.com/api/status\",\"headers\":{}},"
            "\"response\":{\"status\":200}},"
            "\"logs\":[{\"level\":\"log\",\"message\":[\"D1 query: SELECT id "
            "FROM error_log WHERE id = ? [1 arg]\"]},"
            "{\"level\":\"log\",\"message\":[\"D1 batch of 3: INSERT INTO "
            "nodes VALUES (?)\"]},"
            "{\"level\":\"log\",\"message\":[\"served\"]}],"
            "\"exceptions\":[]}"));
    check(d1Tail.parsed && !d1Tail.isError &&
              d1Tail.d1Statements ==
                  QStringList({QStringLiteral("D1 query: SELECT id FROM "
                                              "error_log WHERE id = ? [1 arg]"),
                               QStringLiteral("D1 batch of 3: INSERT INTO "
                                              "nodes VALUES (?)")}) &&
              d1Tail.messages == QStringList({QStringLiteral("served")}) &&
              !d1Tail.summary.contains(QStringLiteral("SELECT")) &&
              !d1Tail.summary.contains(QStringLiteral("INSERT")),
          "D1 statement console lines are lifted out of the hit's summary");

    const auto originStatement = forkmesh::control::parseD1TailStatement(
        QStringLiteral("D1 primary query: SELECT id FROM pulls WHERE note = "
                       "'(from x)' [2 args] (from repositories.py in "
                       "list_pulls)"));
    const auto legacyStatement = forkmesh::control::parseD1TailStatement(
        QStringLiteral("D1 query: SELECT id FROM error_log WHERE id = ? "
                       "[1 arg]"));
    check(originStatement.kind == QLatin1String("primary query") &&
              originStatement.sql ==
                  QLatin1String("SELECT id FROM pulls WHERE note = '(from x)' "
                                "[2 args]") &&
              originStatement.originFile == QLatin1String("repositories.py") &&
              originStatement.originFunction == QLatin1String("list_pulls") &&
              legacyStatement.kind == QLatin1String("query") &&
              legacyStatement.sql ==
                  QLatin1String("SELECT id FROM error_log WHERE id = ? [1 arg]") &&
              legacyStatement.originFile.isEmpty() &&
              legacyStatement.originFunction.isEmpty(),
          "a D1 console line splits into kind, SQL and the Worker origin");
    const auto callerStatement = forkmesh::control::parseD1TailStatement(
        QStringLiteral("D1 query: SELECT id FROM pulls WHERE repo IN (?) "
                       "[1 arg] (for queue notifications) (from "
                       "repositories.py in list_pulls)"));
    check(callerStatement.sql ==
                  QLatin1String("SELECT id FROM pulls WHERE repo IN (?) [1 arg]") &&
              callerStatement.caller == QLatin1String("queue notifications") &&
              callerStatement.originFile == QLatin1String("repositories.py") &&
              originStatement.caller.isEmpty(),
          "a D1 console line splits off the caller the host names");

    const auto requesterTail = forkmesh::control::parseCloudflareTailLine(
        QStringLiteral(
            "{\"outcome\":\"ok\",\"eventTimestamp\":1500,"
            "\"event\":{\"request\":{\"method\":\"post\","
            "\"url\":\"https://app.forkmesh.com/api/pulls?secret=1\","
            "\"headers\":{\"user-agent\":\"ForkMesh/0.8.6\","
            "\"cf-connecting-ip\":\"203.0.113.9\","
            "\"Origin\":\"https://forkmesh.com\"},"
            "\"cf\":{\"country\":\"DE\",\"asOrganization\":\"Hetzner\"}},"
            "\"response\":{\"status\":200}},\"logs\":[],\"exceptions\":[]}"));
    const QString requester =
        forkmesh::control::cloudflareTailRequester(requesterTail);
    forkmesh::control::CloudflareTailEvent alarmTail;
    alarmTail.kind = QStringLiteral("alarm");
    alarmTail.scriptName = QStringLiteral("forkmesh-app");
    alarmTail.entrypoint = QStringLiteral("ForkMeshNodes");
    check(requester.startsWith(QStringLiteral("POST /api/pulls")) &&
              !requester.contains(QStringLiteral("secret")) &&
              requester.contains(QStringLiteral("UA ForkMesh/0.8.6")) &&
              requester.contains(QStringLiteral("client IP 203.0.113.9")) &&
              requester.contains(QStringLiteral("origin https://forkmesh.com")) &&
              requester.contains(QStringLiteral("from DE")) &&
              requester.contains(QStringLiteral("network Hetzner")) &&
              forkmesh::control::cloudflareTailRequester(alarmTail) ==
                  QString::fromUtf8("alarm \xC2\xB7 ForkMeshNodes"),
          "a D1 line's requester names the agent, address and Origin behind "
          "a fetch, and the object behind a wake-up");

    // The Watch window seats a site visitor off exactly these fields: a
    // plain Worker hit is kind "fetch" under the "default" entrypoint, with
    // the browser's agent, address and place.
    const auto visitorTail = forkmesh::control::parseCloudflareTailLine(
        QStringLiteral(
            "{\"outcome\":\"ok\",\"eventTimestamp\":1600,"
            "\"scriptName\":\"forkmesh-www\",\"entrypoint\":\"default\","
            "\"event\":{\"request\":{\"method\":\"GET\","
            "\"url\":\"https://forkmesh.com/docs?ref=x\","
            "\"headers\":{\"user-agent\":\"Mozilla/5.0 (X11; Linux x86_64) "
            "Firefox/130.0\",\"cf-connecting-ip\":\"198.51.100.4\"},"
            "\"cf\":{\"country\":\"NO\",\"city\":\"Oslo\"}},"
            "\"response\":{\"status\":200}},\"logs\":[],\"exceptions\":[]}"));
    check(visitorTail.parsed && visitorTail.kind == QLatin1String("fetch") &&
              visitorTail.entrypoint == QLatin1String("default") &&
              visitorTail.userAgent.startsWith(QLatin1String("Mozilla/5.0")) &&
              visitorTail.clientAddress == QLatin1String("198.51.100.4") &&
              visitorTail.city == QLatin1String("Oslo") &&
              visitorTail.country == QLatin1String("NO") &&
              visitorTail.status == 200,
          "a browser's hit carries what the Watch window seats a visitor by");

    const auto exceptionTail = forkmesh::control::parseCloudflareTailLine(
        QStringLiteral(
            "{\"outcome\":\"exception\",\"eventTimestamp\":2000,"
            "\"event\":{\"request\":{\"method\":\"POST\","
            "\"url\":\"https://app.forkmesh.com/api/sync\",\"headers\":{}}},"
            "\"exceptions\":[{\"name\":\"Error\",\"message\":\"boom\"}],"
            "\"logs\":[]}"));
    check(exceptionTail.parsed && exceptionTail.isError &&
              exceptionTail.userAgent.isEmpty() &&
              exceptionTail.summary.contains(QStringLiteral("Error: boom")) &&
              exceptionTail.summary.contains(QStringLiteral("UA (none)")),
          "a Worker exception is flagged as an error worth alerting on");
    check(forkmesh::control::parseCloudflareTailLine(
              QStringLiteral(
                  "{\"outcome\":\"ok\",\"event\":{\"request\":{\"method\":"
                  "\"GET\",\"url\":\"https://forkmesh.com/\",\"headers\":{}},"
                  "\"response\":{\"status\":503}}}"))
              .isError,
          "a 5xx reply is an error even when the outcome reads ok");
    const auto expectedBackpressure =
        forkmesh::control::parseCloudflareTailLine(QStringLiteral(
            "{\"outcome\":\"ok\",\"event\":{\"request\":{\"method\":"
            "\"POST\",\"url\":\"https://app.forkmesh.com/api/sync\","
            "\"headers\":{}},\"response\":{\"status\":503}},\"logs\":[{"
            "\"level\":\"log\",\"message\":["
            "\"forkmesh_expected_degraded_response\"]}]}"));
    check(expectedBackpressure.parsed && expectedBackpressure.status == 503 &&
              !expectedBackpressure.isError &&
              expectedBackpressure.summary.contains(
                  QStringLiteral("expected backpressure")),
          "an explicitly marked 503 is expected backpressure, not a Worker "
          "error");
    check(forkmesh::control::parseCloudflareTailLine(QStringLiteral(
              "{\"outcome\":\"exception\",\"event\":{\"response\":{"
              "\"status\":503}},\"logs\":[{\"level\":\"log\",\"message\":["
              "\"forkmesh_expected_degraded_response\"]}],\"exceptions\":[{"
              "\"name\":\"Error\",\"message\":\"boom\"}]}"))
              .isError,
          "the backpressure marker cannot hide a concrete Worker exception");
    check(!forkmesh::control::parseCloudflareTailLine(
               QStringLiteral(
                   "{\"outcome\":\"canceled\",\"event\":{\"request\":{"
                   "\"method\":\"GET\",\"url\":\"https://forkmesh.com/\","
                   "\"headers\":{}},\"response\":{\"status\":200}}}"))
               .isError,
          "a client hanging up mid-request is not a Worker failure");
    // A repository's content is streamed from the desktop that holds it, so a
    // 503 there means that node is offline or not yet attested — the Worker
    // answers it deliberately and skips its own error log. The viewer counted
    // one "Cloud: Worker error" per broken repo-card logo (adhoc #1714).
    const auto offlineNodeTail = forkmesh::control::parseCloudflareTailLine(
        QStringLiteral(
            "{\"outcome\":\"ok\",\"eventTimestamp\":3000,"
            "\"entrypoint\":\"ForkMeshCronRunner\","
            "\"event\":{\"request\":{\"method\":\"GET\",\"url\":"
            "\"https://app.forkmesh.com/api/repo/mirror11/forkmesh/raw"
            "?path=logo.png&ref=84887c21\",\"headers\":{}},"
            "\"response\":{\"status\":503}}}"));
    check(offlineNodeTail.parsed && offlineNodeTail.status == 503 &&
              !offlineNodeTail.isError,
          "an offline node's repository read is not a Worker error");
    check(!forkmesh::control::parseCloudflareTailLine(
               QStringLiteral(
                   "{\"outcome\":\"ok\",\"event\":{\"request\":{\"method\":"
                   "\"GET\",\"url\":\"https://app.forkmesh.com/mirror11/"
                   "forkmesh/info/refs?service=git-upload-pack\",\"headers\":{}"
                   "},\"response\":{\"status\":502}}}"))
               .isError,
          "a clone from an offline node is not a Worker error either");
    check(forkmesh::control::parseCloudflareTailLine(
              QStringLiteral(
                  "{\"outcome\":\"ok\",\"event\":{\"request\":{\"method\":"
                  "\"GET\",\"url\":\"https://app.forkmesh.com/api/repo/"
                  "mirror11/forkmesh/raw?path=logo.png\",\"headers\":{}},"
                  "\"response\":{\"status\":500}}}"))
              .isError,
          "a 500 on the same path is the Worker itself and still alerts");
    check(forkmesh::control::parseCloudflareTailLine(
              QStringLiteral(
                  "{\"outcome\":\"ok\",\"event\":{\"request\":{\"method\":"
                  "\"GET\",\"url\":\"https://app.forkmesh.com/api/repo/"
                  "mainnode/forkmesh/rooms/world-general/ws\",\"headers\":{}},"
                  "\"response\":{\"status\":503}}}"))
              .isError,
          "a room 5xx is the Worker's own Durable Object and stays visible");
    check(forkmesh::control::parseCloudflareTailLine(
              QStringLiteral(
                  "{\"outcome\":\"exception\",\"event\":{\"request\":{"
                  "\"method\":\"GET\",\"url\":\"https://app.forkmesh.com/api/"
                  "repo/mirror11/forkmesh/raw?path=logo.png\",\"headers\":{}},"
                  "\"response\":{\"status\":503}},\"exceptions\":[{\"name\":"
                  "\"Error\",\"message\":\"boom\"}]}"))
              .isError,
          "a repository read that threw is still a Worker error");
    // A WebSocket upgrade's response stream stays open for the life of the
    // socket, so every chat connection that ends reports this outcome. It was
    // being logged as "Cloud: Worker error" and counted against the error chip
    // once per departing client (adhoc #1658).
    const auto disconnectTail = forkmesh::control::parseCloudflareTailLine(
        QStringLiteral(
            "{\"outcome\":\"responseStreamDisconnected\","
            "\"scriptName\":\"forkmesh-relay\",\"entrypoint\":\"ForkMeshRoom\","
            "\"event\":{\"request\":{\"method\":\"GET\",\"url\":"
            "\"https://app.forkmesh.com/api/repo/mainnode/forkmesh/rooms/"
            "world-general/ws\",\"headers\":{}}}}"));
    check(disconnectTail.parsed && !disconnectTail.isError &&
              disconnectTail.summary.contains(
                  QStringLiteral("- Client Disconnected")),
          "a closed WebSocket is the end of a connection, not a Worker error");
    check(forkmesh::control::parseCloudflareTailLine(
              QStringLiteral(
                  "{\"outcome\":\"responseStreamDisconnected\",\"event\":{"
                  "\"request\":{\"method\":\"GET\",\"url\":"
                  "\"https://forkmesh.com/\",\"headers\":{}}},"
                  "\"exceptions\":[{\"name\":\"Error\",\"message\":\"boom\"}]}"))
              .isError,
          "a stream that disconnected because the Worker threw is still an error");
    check(forkmesh::control::parseCloudflareTailLine(
              QStringLiteral(
                  "{\"outcome\":\"ok\",\"event\":{},\"logs\":[{\"level\":"
                  "\"error\",\"message\":[\"db unreachable\"]}]}"))
              .isError,
          "an error-level console log is an error");
    const auto bannerTail = forkmesh::control::parseCloudflareTailLine(
        QStringLiteral("  Connected to forkmesh-app, waiting for logs...  "));
    check(!bannerTail.parsed && !bannerTail.isError &&
              bannerTail.summary ==
                  QStringLiteral("Connected to forkmesh-app, waiting for "
                                 "logs..."),
          "Wrangler's own banner lines pass through untouched");
    check(!forkmesh::control::parseCloudflareTailLine(
               QStringLiteral("{\"hello\":\"world\"}")).parsed,
          "JSON that is not a tail event is not treated as one");
    check(okTail.summary.startsWith(
              QStringLiteral("GET https://app.forkmesh.com/api/status - 200 Ok @")),
          "the tail line opens in Wrangler's own default shape");
    const auto unknownJson = forkmesh::control::parseCloudflareTailLine(
        QStringLiteral("{\n    \"hello\": \"world\",\n    \"n\": 1\n}"));
    check(!unknownJson.parsed &&
              !unknownJson.summary.contains(QLatin1Char('\n')) &&
              unknownJson.summary == QStringLiteral("{ \"hello\": \"world\", "
                                                    "\"n\": 1 }"),
          "JSON nobody understands is still collapsed to a single log line");

    // --- Event shapes (adhoc #1635). Wrangler prints "Unknown Event - Ok" for
    // everything outside its own eight branches, and a hibernated Durable
    // Object WebSocket is outside them — which on the relay is nearly every
    // event, so the cloud log said nothing. Each shape now describes itself.
    check(okTail.kind == QStringLiteral("fetch") &&
              okTail.scriptName.isEmpty(),
          "a request event is classified as a fetch");
    const auto socketTail = forkmesh::control::parseCloudflareTailLine(
        QStringLiteral(
            "{\"outcome\":\"ok\",\"eventTimestamp\":1000,"
            "\"scriptName\":\"forkmesh-relay\",\"entrypoint\":\"ChatRoom\","
            "\"event\":{\"getWebSocketEvent\":{\"webSocketEventType\":"
            "\"message\"}},\"logs\":[],\"exceptions\":[]}"));
    check(socketTail.parsed && !socketTail.isError &&
              socketTail.kind == QStringLiteral("websocket") &&
              socketTail.summary.startsWith(
                  QStringLiteral("WebSocket message - Ok @")),
          "a hibernated WebSocket wake-up says so instead of \"Unknown Event\"");
    check(socketTail.summary.contains(QStringLiteral("forkmesh-relay")) &&
              socketTail.summary.contains(QStringLiteral("ChatRoom")) &&
              !socketTail.summary.contains(QStringLiteral("UA ")),
          "it carries the Worker and Durable Object rather than a phantom agent");

    // --- Cost, build and geography (adhoc #1665). A WebSocket wake-up named
    // only its Worker and Durable Object, which after the shape work above is
    // everything the *event* had to say — the rest of what the trace carries
    // sits beside it, and Wrangler prints none of it.
    const auto costedTail = forkmesh::control::parseCloudflareTailLine(
        QStringLiteral(
            "{\"outcome\":\"ok\",\"eventTimestamp\":1000,"
            "\"scriptName\":\"forkmesh-app\",\"entrypoint\":\"ForkMeshRoom\","
            "\"cpuTime\":0.42,\"wallTime\":118,\"truncated\":true,"
            "\"scriptVersion\":{\"id\":\"6f1d2c3b-aaaa-bbbb-cccc-ddddeeeeffff\","
            "\"message\":\"deploy 44\"},"
            "\"event\":{\"getWebSocketEvent\":{\"webSocketEventType\":"
            "\"message\"}},\"logs\":[],\"exceptions\":[]}"));
    check(costedTail.cpuTimeMs > 0.4 && costedTail.cpuTimeMs < 0.5 &&
              costedTail.wallTimeMs == 118 && costedTail.truncated &&
              costedTail.scriptVersion == QStringLiteral("deploy 44"),
          "a wake-up decodes what it cost, which build ran it, and what was cut");
    check(costedTail.summary.contains(QStringLiteral("cpu 0.42ms, wall 118ms")) &&
              costedTail.summary.contains(QStringLiteral("version deploy 44")) &&
              costedTail.summary.contains(QStringLiteral("logs truncated")),
          "and says all three in the line the cloud log shows");
    // A version with no tag and no message still pins the deploy, and an event
    // that reports no timing must not invent a zero.
    const auto plainTail = forkmesh::control::parseCloudflareTailLine(
        QStringLiteral(
            "{\"outcome\":\"ok\",\"scriptVersion\":{\"id\":"
            "\"6f1d2c3baaaabbbbccccddddeeeeffff\"},\"event\":{}}"));
    check(plainTail.scriptVersion == QStringLiteral("6f1d2c3b") &&
              plainTail.cpuTimeMs < 0 && plainTail.wallTimeMs < 0 &&
              plainTail.summary.contains(QStringLiteral("version 6f1d2c3b")) &&
              !plainTail.summary.contains(QStringLiteral("cpu ")) &&
              !plainTail.summary.contains(QStringLiteral("wall ")) &&
              !plainTail.summary.contains(QStringLiteral("truncated")),
          "an untagged version shortens to its id and absent timing stays absent");
    // Zero CPU is a real measurement — a wake-up that does nothing — so it has
    // to survive the "absent" test rather than being swallowed by it.
    check(forkmesh::control::parseCloudflareTailLine(
              QStringLiteral("{\"outcome\":\"ok\",\"cpuTime\":0,\"wallTime\":3,"
                             "\"event\":{}}"))
                  .summary.contains(QStringLiteral("cpu 0ms, wall 3ms")),
          "a wake-up that burned no CPU reports that rather than nothing");
    const auto locatedTail = forkmesh::control::parseCloudflareTailLine(
        QStringLiteral(
            "{\"outcome\":\"ok\",\"event\":{\"request\":{\"method\":\"GET\","
            "\"url\":\"https://forkmesh.com/\",\"headers\":{"
            "\"cf-connecting-ip\":\"203.0.113.8\"},"
            "\"cf\":{\"colo\":\"LHR\",\"country\":\"GB\","
            "\"city\":\"London\",\"region\":\"England\",\"asn\":64500,"
            "\"asOrganization\":\"Example Access\",\"httpProtocol\":\"HTTP/2\","
            "\"tlsVersion\":\"TLSv1.3\",\"clientTcpRtt\":23}},"
            "\"response\":{\"status\":200}}}"));
    check(locatedTail.colo == QStringLiteral("LHR") &&
              locatedTail.country == QStringLiteral("GB") &&
              locatedTail.city == QStringLiteral("London") &&
              locatedTail.clientAddress == QStringLiteral("203.0.113.8") &&
              locatedTail.summary.contains(QStringLiteral("via LHR from GB")) &&
              locatedTail.summary.contains(QStringLiteral("near London, England")) &&
              locatedTail.summary.contains(QStringLiteral("client IP 203.0.113.8")) &&
              locatedTail.summary.contains(QStringLiteral("AS64500 Example Access")) &&
              locatedTail.summary.contains(QStringLiteral("HTTP/2 TLSv1.3 RTT 23ms")),
          "a request identifies the edge, client location/network and transport");
    const auto ownerTail = forkmesh::control::parseCloudflareTailLine(
        QStringLiteral(
            "{\"outcome\":\"ok\",\"event\":{\"request\":{\"method\":\"GET\","
            "\"url\":\"https://app.forkmesh.com/api/nodes/events?owner=alice"
            "&ts=1&sig=secret\",\"headers\":{\"user-agent\":"
            "\"ForkMeshDesktop/0.7.0 (account=alice; node=alice-laptop)\"}}}}"));
    check(ownerTail.clientIdentity == QStringLiteral("alice") &&
              ownerTail.summary.contains(QStringLiteral("owner alice")) &&
              ownerTail.summary.contains(QStringLiteral("node=alice-laptop")),
          "an authenticated node-event route names its account and desktop client");
    // The same properties have travelled as a JSON string rather than an
    // object, so a string form must decode to the same line.
    check(forkmesh::control::parseCloudflareTailLine(
              QStringLiteral(
                  "{\"outcome\":\"ok\",\"event\":{\"request\":{\"method\":"
                  "\"GET\",\"url\":\"https://forkmesh.com/\",\"headers\":{},"
                  "\"cfJson\":\"{\\\"colo\\\":\\\"CDG\\\"}\"}}}"))
                  .summary.contains(QStringLiteral("via CDG")),
          "cf properties sent as a JSON string decode the same way");
    check(!socketTail.summary.contains(QStringLiteral("via ")) &&
              !socketTail.summary.contains(QStringLiteral("cpu ")) &&
              !socketTail.summary.contains(QStringLiteral("wall ")) &&
              socketTail.summary.endsWith(QStringLiteral("ChatRoom")),
          "an event carrying none of it gains no empty fields");
    const auto closeTail = forkmesh::control::parseCloudflareTailLine(
        QStringLiteral(
            "{\"outcome\":\"ok\",\"event\":{\"getWebSocketEvent\":{"
            "\"webSocketEventType\":\"close\",\"code\":1006,"
            "\"wasClean\":false}}}"));
    check(closeTail.kind == QStringLiteral("websocket") &&
              closeTail.summary.startsWith(
                  QStringLiteral("WebSocket close (code 1006, unclean)")),
          "a close carries the metadata the encrypted frame cannot");
    const auto bareExceptionClose =
        forkmesh::control::parseCloudflareTailLine(QStringLiteral(
            "{\"outcome\":\"exception\",\"scriptName\":\"forkmesh-app\","
            "\"entrypoint\":\"ForkMeshRoom\",\"event\":{"
            "\"getWebSocketEvent\":{\"webSocketEventType\":\"close\","
            "\"code\":1006,\"wasClean\":false}},\"exceptions\":[]}"));
    check(bareExceptionClose.parsed && !bareExceptionClose.isError &&
              !bareExceptionClose.isDeployInterruption,
          "an unclean socket close with no exception detail is not invented "
          "into a Worker fault");
    check(forkmesh::control::parseCloudflareTailLine(QStringLiteral(
              "{\"outcome\":\"exception\",\"event\":{"
              "\"getWebSocketEvent\":{\"webSocketEventType\":\"close\","
              "\"code\":1006,\"wasClean\":false}},\"exceptions\":[{"
              "\"name\":\"TypeError\",\"message\":\"room failed\"}]}"))
              .isError,
          "an unclean socket close carrying a real exception still alerts");
    const auto deployReset = forkmesh::control::parseCloudflareTailLine(
        QStringLiteral(
            "{\"outcome\":\"exception\",\"scriptName\":\"forkmesh-app\","
            "\"entrypoint\":\"ForkMeshNodes\",\"durableObjectId\":"
            "\"0123456789abcdef0123456789abcdef\",\"traceId\":\"trace-77\","
            "\"scriptVersion\":{\"id\":\"c0d1f450aaaaaaaa\"},\"event\":{"
            "\"getWebSocketEvent\":{\"webSocketEventType\":\"close\","
            "\"code\":1006,\"wasClean\":false}},\"exceptions\":[{"
            "\"name\":\"Error\",\"message\":\"Durable Object reset because "
            "its code was updated.\"},{\"name\":\"Error\",\"message\":"
            "\"Durable Object reset because its code was updated.\"}]}"));
    check(deployReset.parsed && deployReset.isDeployInterruption &&
              !deployReset.isError &&
              deployReset.durableObjectId.startsWith(QStringLiteral("012345")) &&
              deployReset.summary.contains(QStringLiteral("object 0123456789abcde")) &&
              deployReset.summary.contains(QStringLiteral("trace trace-77")) &&
              deployReset.summary.contains(
                  QStringLiteral("expected code-update handoff")) &&
              deployReset.summary.contains(
                  QStringLiteral("client identity unavailable")) &&
              !deployReset.summary.contains(
                  QStringLiteral("Durable Object reset because")),
          "a code-update socket reset is a correlated deploy handoff, not a fault");
    check(forkmesh::control::parseCloudflareTailLine(
              QStringLiteral(
                  "{\"outcome\":\"exception\",\"event\":{},\"exceptions\":["
                  "{\"name\":\"Error\",\"message\":\"Durable Object reset "
                  "because its code was updated.\"},{\"name\":\"TypeError\","
                  "\"message\":\"boom\"}]}"))
              .isError,
          "a real exception beside a deploy reset still alerts");
    // The chat relay's hibernated sockets outlive the instances that own them.
    // When the object is already gone the runtime answers the close wake-up
    // with an exception of its own, and the viewer used to raise it as a Worker
    // error complete with an invitation to go and audit module start-up.
    const auto instanceGone = forkmesh::control::parseCloudflareTailLine(
        QStringLiteral(
            "{\"outcome\":\"exception\",\"scriptName\":\"forkmesh-app\","
            "\"entrypoint\":\"ForkMeshRoom\",\"durableObjectId\":"
            "\"5253baddfca6c8a2baac78fe0fb4335f"
            "b6c5fd16d878fece30bddcdb4fe31e18\",\"cpuTime\":0,"
            "\"wallTime\":1,\"scriptVersion\":{\"id\":"
            "\"7855c4f4aaaa2222\"},\"event\":{\"getWebSocketEvent\":{"
            "\"webSocketEventType\":\"close\",\"code\":1006,"
            "\"wasClean\":false}},\"exceptions\":[{\"name\":\"Error\","
            "\"message\":\"Connection closed: this Durable Object instance "
            "is no longer active. Reconnect or retry the request.\"}]}"));
    check(instanceGone.parsed && !instanceGone.isError &&
              !instanceGone.isDeployInterruption &&
              instanceGone.kind == QStringLiteral("websocket"),
          "a close delivered to an object that already ended is not a Worker "
          "fault");
    check(instanceGone.summary.contains(
              QStringLiteral("expected instance handoff")) &&
              instanceGone.summary.contains(
                  QStringLiteral("no longer active")) &&
              !instanceGone.summary.contains(
                  QStringLiteral("faulted before handler code ran")),
          "the line keeps the runtime's own words and stops sending readers to "
          "Worker start-up");
    check(forkmesh::control::parseCloudflareTailLine(
              QStringLiteral(
                  "{\"outcome\":\"exception\",\"event\":{"
                  "\"getWebSocketEvent\":{\"webSocketEventType\":\"close\","
                  "\"code\":1006,\"wasClean\":false}},\"exceptions\":[{"
                  "\"name\":\"Error\",\"message\":\"Connection closed: "
                  "this Durable Object instance is no longer active.\"},{"
                  "\"name\":\"TypeError\",\"message\":\"boom\"}]}"))
              .isError,
          "a real exception beside an ended instance still alerts");
    check(forkmesh::control::parseCloudflareTailLine(
              QStringLiteral(
                  "{\"outcome\":\"exception\",\"event\":{\"request\":{"
                  "\"method\":\"GET\",\"url\":"
                  "\"https://app.forkmesh.com/api/chat/new-member\","
                  "\"headers\":{}}},\"exceptions\":[{\"name\":\"Error\","
                  "\"message\":\"Connection closed: this Durable Object "
                  "instance is no longer active. Reconnect or retry the "
                  "request.\"}]}"))
              .isError,
          "the same text on a request the Worker was serving still alerts");
    // The fault the relay actually sees on a hibernated node socket: the
    // runtime failing underneath the Worker. Every field a reader would need is
    // in the record and the viewer used to drop or shorten all of them, so the
    // line said "Exception Thrown ... internal error" and nothing more.
    const auto platformFault = forkmesh::control::parseCloudflareTailLine(
        QStringLiteral(
            "{\"outcome\":\"exception\",\"scriptName\":\"forkmesh-app\","
            "\"entrypoint\":\"ForkMeshNodes\",\"durableObjectId\":"
            "\"6eaf01267b1b0fa2b0a1c3d4e5f60718"
            "293a4b5c6d7e8f90a1b2c3d4e5f60718\","
            "\"scriptVersion\":{\"id\":\"d4f467fbaaaa1111\"},\"cpuTime\":0,"
            "\"wallTime\":2,\"event\":{\"getWebSocketEvent\":{"
            "\"webSocketEventType\":\"close\",\"code\":1006,"
            "\"wasClean\":false}},\"exceptions\":[{\"name\":\"Error\","
            "\"message\":\"internal error; reference = "
            "veiiufghh4e9irjrg1nlvpus\"}]}"));
    check(platformFault.parsed && platformFault.isError &&
              platformFault.platformFault &&
              platformFault.platformFaultReference ==
                  QStringLiteral("veiiufghh4e9irjrg1nlvpus"),
          "an opaque runtime fault is recognised and its reference kept");
    check(platformFault.summary.contains(
              QStringLiteral("quote reference veiiufghh4e9irjrg1nlvpus")),
          "the fault line carries the one identifier Cloudflare can act on");
    check(platformFault.summary.contains(QStringLiteral(
              "object 6eaf01267b1b0fa2b0a1c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4"
              "e5f60718")),
          "a fault prints the whole object id, not a prefix that cannot be "
          "correlated");
    check(platformFault.summary.contains(
              QStringLiteral("faulted before handler code ran")) &&
              platformFault.summary.contains(
                  QStringLiteral("client identity unavailable")),
          "the line says where the fault was not, so nobody searches the "
          "handler for it");
    // The same shape without the reference id still has to be named: it is what
    // the free-tier duration abort arrives as.
    check(forkmesh::control::parseCloudflareTailLine(
              QStringLiteral(
                  "{\"outcome\":\"exception\",\"event\":{},"
                  "\"exceptions\":[{\"name\":\"Error\",\"message\":"
                  "\"Exceeded allowed duration in Durable Objects free "
                  "tier.\"}]}"))
              .summary.contains(QStringLiteral(
                  "Cloudflare-side fault, not Worker code")),
          "a platform abort with no reference is still named as one");
    // A Worker bug must not be excused as the platform's, and an event that
    // logged its way to the failure did reach handler code.
    const auto workerBug = forkmesh::control::parseCloudflareTailLine(
        QStringLiteral(
            "{\"outcome\":\"exception\",\"cpuTime\":0,\"event\":{},"
            "\"logs\":[{\"level\":\"log\",\"message\":[\"starting\"]}],"
            "\"exceptions\":[{\"name\":\"TypeError\",\"message\":"
            "\"boom\"}]}"));
    check(workerBug.isError && !workerBug.platformFault &&
              !workerBug.summary.contains(
                  QStringLiteral("Cloudflare-side fault")) &&
              !workerBug.summary.contains(
                  QStringLiteral("faulted before handler code ran")),
          "an ordinary Worker exception is not blamed on the platform");
    // Cloudflare's own notes on the invocation outlive a fault that wrote no
    // console output, so they are the only account of what happened.
    check(forkmesh::control::parseCloudflareTailLine(
              QStringLiteral(
                  "{\"outcome\":\"exception\",\"event\":{},"
                  "\"exceptions\":[{\"name\":\"Error\",\"message\":"
                  "\"boom\"}],\"diagnosticsChannelEvents\":[{\"channel\":"
                  "\"workerd\",\"message\":\"isolate restarted\"}]}"))
              .summary.contains(QStringLiteral("workerd: isolate restarted")),
          "diagnostics channel events reach the log instead of being dropped");
    // A deliberate hang-up says which rule ended the socket.
    check(forkmesh::control::parseCloudflareTailLine(
              QStringLiteral(
                  "{\"outcome\":\"ok\",\"event\":{\"getWebSocketEvent\":{"
                  "\"webSocketEventType\":\"close\",\"code\":1009,"
                  "\"wasClean\":true,\"reason\":\"message too large\"}}}"))
              .summary.startsWith(QStringLiteral(
                  "WebSocket close (code 1009, clean, reason \"message too "
                  "large\")")),
          "a close reason names the rule that ended the connection");
    // The shape is matched on what its keys say, not on one exact spelling:
    // Wrangler has no branch for this, so there is no published name to pin to.
    check(forkmesh::control::parseCloudflareTailLine(
              QStringLiteral("{\"outcome\":\"ok\",\"event\":{"
                             "\"hibernatableWebSocket\":{\"type\":\"error\"}}}"))
                  .summary.startsWith(QStringLiteral("WebSocket error -")),
          "a renamed WebSocket shape still reads as a WebSocket");

    check(forkmesh::control::parseCloudflareTailLine(
              QStringLiteral("{\"outcome\":\"ok\",\"event\":{"
                             "\"scheduledTime\":1000}}"))
                  .kind == QStringLiteral("alarm"),
          "a Durable Object alarm is named as one");
    const auto queueTail = forkmesh::control::parseCloudflareTailLine(
        QStringLiteral("{\"outcome\":\"ok\",\"event\":{\"queue\":\"mirror\","
                       "\"batchSize\":3}}"));
    check(queueTail.kind == QStringLiteral("queue") &&
              queueTail.summary.startsWith(
                  QStringLiteral("Queue mirror (3 messages) - Ok")),
          "a queue batch reports its queue and size");
    const auto rpcTail = forkmesh::control::parseCloudflareTailLine(
        QStringLiteral("{\"outcome\":\"ok\",\"entrypoint\":\"Notes\","
                       "\"event\":{\"rpcMethod\":\"append\"}}"));
    check(rpcTail.kind == QStringLiteral("rpc") &&
              rpcTail.summary.startsWith(QStringLiteral("Notes.append() - Ok")),
          "an RPC call names the entrypoint and method it invoked");
    check(forkmesh::control::parseCloudflareTailLine(
              QStringLiteral("{\"outcome\":\"ok\",\"event\":{\"type\":"
                             "\"overload\",\"message\":\"too many events\"}}"))
                  .summary.startsWith(
                      QStringLiteral("Tail overload: too many events")),
          "Cloudflare shedding tail events says so in the log");
    // A shape nobody here has a name for is still described by its own fields,
    // so a future event type degrades to something readable rather than to a
    // line that carries no information at all.
    const auto otherTail = forkmesh::control::parseCloudflareTailLine(
        QStringLiteral("{\"outcome\":\"ok\",\"event\":{\"somethingNew\":{"
                       "\"phase\":\"start\"}}}"));
    check(otherTail.kind == QStringLiteral("other") &&
              otherTail.summary.startsWith(
                  QStringLiteral("Event somethingNew.phase=start - Ok")),
          "an unrecognised event is described from its own keys");
    check(forkmesh::control::parseCloudflareTailLine(
              QStringLiteral("{\"outcome\":\"ok\",\"event\":{}}"))
                  .summary.startsWith(
                      QStringLiteral("Event with no detail - Ok")),
          "an event with nothing in it says that much honestly");

    // --- Record framing (adhoc #1623): Wrangler's --format json is NOT NDJSON.
    // It pretty-prints each event over many lines, so the stream is cut on brace
    // balance, not on newlines — the per-line split showed the raw expanded JSON
    // and never decoded an error.
    QByteArray tailStream =
        "Connected to forkmesh-app, waiting for logs...\n"
        "{\n"
        "    \"outcome\": \"ok\",\n"
        "    \"eventTimestamp\": 1000,\n"
        "    \"event\": {\n"
        "        \"request\": {\n"
        "            \"method\": \"GET\",\n"
        "            \"url\": \"https://forkmesh.com/a{b}\",\n"
        "            \"headers\": {\n"
        "                \"user-agent\": \"Mozilla/5.0 {curly}\"\n"
        "            }\n"
        "        },\n"
        "        \"response\": {\n"
        "            \"status\": 200\n"
        "        }\n"
        "    },\n"
        "    \"logs\": [],\n"
        "    \"exceptions\": []\n"
        "}\n";
    QStringList tailRecords =
        forkmesh::control::takeCloudflareTailRecords(&tailStream);
    check(tailRecords.size() == 2 && tailStream.isEmpty() &&
              tailRecords.constFirst() ==
                  QStringLiteral("Connected to forkmesh-app, waiting for "
                                 "logs...") &&
              tailRecords.constLast().startsWith(QLatin1Char('{')) &&
              tailRecords.constLast().endsWith(QLatin1Char('}')),
          "a pretty-printed event is framed as one record beside plain lines");
    const auto prettyTail =
        forkmesh::control::parseCloudflareTailLine(tailRecords.constLast());
    check(prettyTail.parsed && !prettyTail.isError && prettyTail.status == 200 &&
              prettyTail.userAgent == QStringLiteral("Mozilla/5.0 {curly}") &&
              prettyTail.summary.contains(
                  QStringLiteral("UA Mozilla/5.0 {curly}")),
          "the framed record decodes, braces inside strings and all");

    // A read can land anywhere: the half-arrived record waits in the buffer
    // rather than reaching the parser as fragments.
    QByteArray split = "{\n    \"outcome\": \"exce";
    check(forkmesh::control::takeCloudflareTailRecords(&split).isEmpty() &&
              !split.isEmpty(),
          "an event split across reads is held back until it closes");
    split += "ption\",\n    \"event\": {},\n    \"exceptions\": [\n"
             "        {\n            \"name\": \"TypeError\",\n"
             "            \"message\": \"boom\"\n        }\n    ]\n}\n"
             "{\"outcome\":\"ok\",\"event\":{}}\n";
    const QStringList resumed =
        forkmesh::control::takeCloudflareTailRecords(&split);
    check(resumed.size() == 2 && split.isEmpty() &&
              forkmesh::control::parseCloudflareTailLine(resumed.constFirst())
                  .isError &&
              forkmesh::control::parseCloudflareTailLine(resumed.constFirst())
                  .summary.contains(QStringLiteral("TypeError: boom")),
          "the rest of a split event completes it, compact events still work");
    // stderr is merged into this stream, so a warning carrying a stray brace
    // must not swallow the events behind it.
    QByteArray interleaved =
        "{ WARNING: unbalanced {\n"
        "{\"outcome\":\"ok\",\"event\":{\"request\":{\"method\":\"GET\","
        "\"url\":\"https://forkmesh.com/\",\"headers\":{}}}}\n";
    const QStringList resynced =
        forkmesh::control::takeCloudflareTailRecords(&interleaved);
    check(resynced.size() == 2 && interleaved.isEmpty() &&
              resynced.constFirst() == QStringLiteral("{ WARNING: unbalanced {") &&
              forkmesh::control::parseCloudflareTailLine(resynced.constLast())
                  .parsed,
          "a stray brace on stderr cannot swallow the events behind it");
    QByteArray runaway(3 * 1024 * 1024, '{');
    runaway += '\n';
    check(forkmesh::control::takeCloudflareTailRecords(&runaway).isEmpty() &&
              runaway.isEmpty(),
          "a record that never closes cannot grow the buffer without bound");

    const QMap<QString, QString> storedVariables = {
        {QStringLiteral("cloudflare_api_token"), QStringLiteral("  cf-stored  ")},
        {QStringLiteral("CF_TOKEN"), QStringLiteral("cf-secondary")},
        {QStringLiteral("CLOUDFLARE_ACCOUNT_ID"),
         QStringLiteral("account_from_secrets")},
        {QStringLiteral("PUBLIC_MODE"), QStringLiteral("production")},
    };
    check(forkmesh::control::cloudflareApiTokenFromVariables(
              storedVariables) == QStringLiteral("cf-stored") &&
              forkmesh::control::cloudflareAccountIdFromVariables(
                  storedVariables) ==
                  QStringLiteral("account_from_secrets"),
          "stored action secrets supply the Cloudflare token and account");
    check(forkmesh::control::cloudflareApiTokenFromVariables(
              {{QStringLiteral("CLOUDFLARE_API_TOKEN"),
                QStringLiteral("   ")},
               {QStringLiteral("CF_API_TOKEN"),
                QStringLiteral("cf-fallback")}}) ==
              QStringLiteral("cf-fallback"),
          "blank stored tokens fall through to the next known secret name");
    check(forkmesh::control::cloudflareApiTokenFromVariables(
              {{QStringLiteral("CLOUDFLARE_API_TOKEN"),
                QStringLiteral("cf-line\nCLOUDFLARE_EMAIL=x")}})
              .isEmpty() &&
              forkmesh::control::cloudflareApiTokenFromVariables({}).isEmpty(),
          "multi-line and missing stored tokens are ignored");

    forkmesh::control::MirrorActionsConfigurationRequest actionsRequest;
    actionsRequest.requestId =
        QStringLiteral("0123456789abcdef0123456789abcdef");
    actionsRequest.host = QStringLiteral("mirror2.example.com");
    actionsRequest.sshUser = QStringLiteral("forkmesh");
    actionsRequest.nodeName = QStringLiteral("mirror2");
    actionsRequest.actionsEnabled = true;
    actionsRequest.replaceVariables = true;
    actionsRequest.variables = {
        {QStringLiteral("DEPLOY_TOKEN"),
         QStringLiteral("stdin-only-action-secret")},
        {QStringLiteral("PUBLIC_MODE"), QStringLiteral("production")},
    };
    check(forkmesh::control::validateMirrorActionsConfigurationRequest(
              actionsRequest).isEmpty(),
          "bounded mirror Actions configuration is accepted");
    QString actionsError;
    const QByteArray actionsPayload =
        forkmesh::control::buildMirrorActionsConfigurationPayload(
            actionsRequest, &actionsError);
    const QJsonObject actionsJson =
        QJsonDocument::fromJson(actionsPayload).object();
    check(actionsError.isEmpty() &&
              actionsPayload.size() <= 64 * 1024 &&
              actionsJson.value(QStringLiteral("type")).toString() ==
                  QStringLiteral("forkmesh.mirror-actions-configuration") &&
              actionsJson.value(QStringLiteral("actionsEnabled")).toBool() &&
              actionsJson.value(QStringLiteral("variables"))
                      .toObject()
                      .value(QStringLiteral("values"))
                      .toObject()
                      .value(QStringLiteral("DEPLOY_TOKEN"))
                      .toString() ==
                  QStringLiteral("stdin-only-action-secret"),
          "mirror Actions request serializes only into a bounded stdin document");
    auto stateOnlyActions = actionsRequest;
    stateOnlyActions.replaceVariables = false;
    stateOnlyActions.variables.clear();
    const QJsonObject stateOnlyJson =
        QJsonDocument::fromJson(
            forkmesh::control::buildMirrorActionsConfigurationPayload(
                stateOnlyActions, &actionsError))
            .object();
    check(!stateOnlyJson.contains(QStringLiteral("variables")),
          "state-only Actions request cannot clear or disclose remote variables");

    const QString sshPassword =
        QStringLiteral("ssh-password-that-must-never-enter-argv");
    const auto actionsPasswordCommand =
        forkmesh::control::buildMirrorActionsSshCommand(
            actionsRequest, sshPassword, &actionsError);
    const QString actionsArgv =
        actionsPasswordCommand.arguments.join(QLatin1Char(' '));
    check(actionsPasswordCommand.program == QStringLiteral("sshpass") &&
              actionsPasswordCommand.environment.value(
                  QStringLiteral("SSHPASS")) == sshPassword &&
              actionsPasswordCommand.standardInput == actionsPayload,
          "password-authenticated Actions transport uses environment plus stdin");
    check(!actionsArgv.contains(sshPassword) &&
              !actionsArgv.contains(
                  QStringLiteral("stdin-only-action-secret")) &&
              !actionsArgv.contains(QStringLiteral("DEPLOY_TOKEN")) &&
              !actionsArgv.contains(QStringLiteral("production")),
          "Actions password, variable names, and values are absent from argv");
    check(actionsArgv.contains(
              QStringLiteral("--configure-mirror-actions-stdin")) &&
              actionsArgv.contains(
                  QStringLiteral("StrictHostKeyChecking=accept-new")) &&
              actionsArgv.contains(
                  QStringLiteral("PreferredAuthentications=publickey,password")) &&
              actionsArgv.contains(QStringLiteral("UserKnownHostsFile=")) &&
              !actionsPasswordCommand.arguments.contains(
                  QStringLiteral("UserKnownHostsFile=/dev/null")) &&
              !actionsArgv.contains(
                  QStringLiteral("PubkeyAuthentication=no")),
          "Actions transport invokes only the fixed helper, persists TOFU keys, and prefers public-key auth");

    const auto actionsKeyCommand =
        forkmesh::control::buildMirrorActionsSshCommand(
            actionsRequest, QString(), &actionsError);
    check(actionsKeyCommand.program == QStringLiteral("ssh") &&
              actionsKeyCommand.arguments.contains(
                  QStringLiteral("BatchMode=yes")) &&
              actionsKeyCommand.arguments.contains(
                  QStringLiteral("PreferredAuthentications=publickey")) &&
              actionsKeyCommand.arguments.contains(
                  QStringLiteral("HashKnownHosts=yes")) &&
              actionsKeyCommand.arguments.contains(
                  QStringLiteral("UpdateHostKeys=yes")) &&
              !actionsKeyCommand.arguments.contains(
                  QStringLiteral("IdentitiesOnly=yes")) &&
              !actionsKeyCommand.environment.contains(
                  QStringLiteral("SSHPASS")),
          "empty password selects non-interactive SSH key authentication");

    const auto genericKeyCommand =
        forkmesh::control::buildHostSshCommand(
            QStringLiteral("mirror.example.test"),
            QStringLiteral("forkmesh"), QString(),
            QStringLiteral("exec tail -f node.log"), &actionsError);
    const QString genericKeyArgv =
        genericKeyCommand.arguments.join(QChar(u'\0'));
    check(actionsError.isEmpty() &&
              genericKeyCommand.program == QStringLiteral("ssh") &&
              genericKeyArgv.contains(
                  QStringLiteral("StrictHostKeyChecking=accept-new")) &&
              genericKeyArgv.contains(QStringLiteral("UserKnownHostsFile=")) &&
              !genericKeyArgv.contains(QStringLiteral("/dev/null")) &&
              !genericKeyArgv.contains(
                  QStringLiteral("StrictHostKeyChecking=no")),
          "all controller host operations use a persistent fail-closed TOFU trust store");
    check(forkmesh::control::buildHostSshCommand(
              QStringLiteral("-oProxyCommand=bad"),
              QStringLiteral("forkmesh"), QString(),
              QStringLiteral("true"), &actionsError)
              .program.isEmpty(),
          "generic controller SSH rejects option-shaped hosts");

    // Managed identity keys (adhoc #315): a recorded key file pins
    // authentication to exactly that key; a missing file fails closed instead
    // of silently falling back to the user's default keys.
    QTemporaryDir identityDir;
    const QString identityPath =
        identityDir.filePath(QStringLiteral("vultr_mirror_ed25519"));
    {
        QFile identity(identityPath);
        check(identity.open(QIODevice::WriteOnly) &&
                  identity.write("managed-key-material") > 0,
              "managed identity fixture is writable");
    }
    const auto identityCommand = forkmesh::control::buildHostSshCommand(
        QStringLiteral("203.0.113.10"), QStringLiteral("root"), QString(),
        QStringLiteral("true"), &actionsError, identityPath);
    check(actionsError.isEmpty() &&
              identityCommand.program == QStringLiteral("ssh") &&
              identityCommand.arguments.contains(QStringLiteral("-i")) &&
              identityCommand.arguments.contains(identityPath) &&
              identityCommand.arguments.contains(
                  QStringLiteral("IdentitiesOnly=yes")),
          "a managed identity file is pinned with -i and IdentitiesOnly");
    check(forkmesh::control::buildHostSshCommand(
              QStringLiteral("203.0.113.10"), QStringLiteral("root"),
              QString(), QStringLiteral("true"), &actionsError,
              identityDir.filePath(QStringLiteral("missing_key")))
              .program.isEmpty(),
          "a missing managed identity file fails closed");

    // Shared fleet key: every host now authorizes one ForkMesh key, so the
    // controller keeps a single private half beside its known_hosts store and
    // hands it to every host operation. A device that predates the switch keeps
    // using the per-provider key file the mirrors already trust.
    const QString sharedDir = forkmesh::control::sharedHostKeyDirectory();
    const QString sharedKey =
        QDir(sharedDir).filePath(QStringLiteral("forkmesh_shared_ed25519"));
    const QString legacyKey =
        QDir(sharedDir).filePath(QStringLiteral("vultr_mirror_ed25519"));
    QFile::remove(sharedKey);
    QFile::remove(legacyKey);
    check(!sharedDir.isEmpty() && QFileInfo(sharedDir).isDir() &&
              forkmesh::control::sharedHostKeyPath() == sharedKey &&
              forkmesh::control::existingSharedHostIdentityFile().isEmpty(),
          "the shared host key is named before it exists and is not offered "
          "until it does");
    const auto writeKeyFixture = [](const QString &path) {
        QFile key(path);
        return key.open(QIODevice::WriteOnly) &&
               key.write("shared-key-material") > 0;
    };
    check(writeKeyFixture(legacyKey) &&
              forkmesh::control::sharedHostKeyPath() == legacyKey &&
              forkmesh::control::existingSharedHostIdentityFile() == legacyKey,
          "a pre-existing per-provider key is adopted as the shared fleet key");
    check(writeKeyFixture(sharedKey) &&
              forkmesh::control::sharedHostKeyPath() == sharedKey &&
              forkmesh::control::existingSharedHostIdentityFile() == sharedKey,
          "the shared key wins once it exists");
    const auto sharedCommand = forkmesh::control::buildHostSshCommand(
        QStringLiteral("203.0.113.10"), QStringLiteral("root"),
        QStringLiteral("session-only"), QStringLiteral("true"), &actionsError,
        forkmesh::control::existingSharedHostIdentityFile());
    check(actionsError.isEmpty() &&
              sharedCommand.program == QStringLiteral("sshpass") &&
              sharedCommand.arguments.contains(sharedKey) &&
              sharedCommand.arguments.contains(
                  QStringLiteral("PreferredAuthentications=publickey,password")),
          "a host with both the shared key and a session password tries the "
          "key first and keeps the password as the fallback");
    QFile::remove(sharedKey);
    QFile::remove(legacyKey);

    // Provider sign-in terminal (adhoc #422): the agent-CLI installer copies no
    // tokens, so ForkMesh opens a real interactive shell on the mirror right
    // after it finishes. That needs a forced remote TTY and a shell-safe
    // command line, and the remote command itself must stay credential-free.
    QString loginError;
    const auto loginCommand =
        forkmesh::control::buildHostInteractiveSshCommand(
            QStringLiteral("203.0.113.10"), QStringLiteral("root"), QString(),
            forkmesh::control::buildHostAgentLoginRemoteCommand(), &loginError,
            identityPath);
    check(loginError.isEmpty() &&
              loginCommand.program == QStringLiteral("ssh") &&
              loginCommand.arguments.contains(QStringLiteral("-tt")) &&
              loginCommand.arguments.indexOf(QStringLiteral("-tt")) ==
                  loginCommand.arguments.size() - 3 &&
              loginCommand.arguments.at(loginCommand.arguments.size() - 2) ==
                  QStringLiteral("root@203.0.113.10"),
          "the sign-in shell forces a remote TTY before user@host");
    const QString loginRemote =
        forkmesh::control::buildHostAgentLoginRemoteCommand();
    check(loginRemote.contains(QStringLiteral("$HOME/.local/bin")) &&
              loginRemote.contains(QStringLiteral("forkmesh-node")) &&
              loginRemote.contains(QStringLiteral("codex login")) &&
              loginRemote.contains(QStringLiteral("exec \"${SHELL:-/bin/sh}\"")),
          "the sign-in shell puts the CLI prefixes on PATH and execs a login shell");
    const QString loginLine =
        forkmesh::control::hostSshCommandLine(loginCommand);
    check(loginLine.startsWith(QStringLiteral("'ssh' ")) &&
              loginLine.contains(QStringLiteral("'-tt'")) &&
              loginLine.contains(QStringLiteral("'\\''")),
          "the flattened sign-in command line quotes every word and escapes the "
          "remote command's own quotes");
    QString shellError;
    const auto shellCommand = forkmesh::control::buildHostLoginShellSshCommand(
        QStringLiteral("203.0.113.10"), QStringLiteral("root"), QString(),
        &shellError, identityPath);
    check(shellError.isEmpty() &&
              shellCommand.program == QStringLiteral("ssh") &&
              shellCommand.arguments.last() == QStringLiteral("root@203.0.113.10") &&
              shellCommand.arguments.at(shellCommand.arguments.size() - 2) ==
                  QStringLiteral("-tt") &&
              shellCommand.arguments.contains(QStringLiteral("BatchMode=yes")) &&
              shellCommand.arguments.contains(identityPath) &&
              !shellCommand.arguments.contains(QString()),
          "the host shell is a plain login with a forced TTY and no remote command");
    const auto shellWithPassword = forkmesh::control::buildHostLoginShellSshCommand(
        QStringLiteral("203.0.113.10"), QStringLiteral("pi"), QStringLiteral("hunter2"),
        &shellError);
    check(shellWithPassword.program == QStringLiteral("sshpass") &&
              shellWithPassword.arguments.last() == QStringLiteral("pi@203.0.113.10") &&
              !shellWithPassword.arguments.contains(QStringLiteral("hunter2")) &&
              shellWithPassword.environment.value(QStringLiteral("SSHPASS")) ==
                  QStringLiteral("hunter2"),
          "a password reaches the host shell only through SSHPASS");
    check(forkmesh::control::buildHostLoginShellSshCommand(
              QStringLiteral("-oProxyCommand=bad"), QStringLiteral("root"), QString(),
              &shellError)
              .program.isEmpty(),
          "a rejected host opens no shell");
    check(forkmesh::control::hostSshCommandLine(
              forkmesh::control::buildHostInteractiveSshCommand(
                  QStringLiteral("-oProxyCommand=bad"),
                  QStringLiteral("root"), QString(),
                  QStringLiteral("true"), &loginError))
              .isEmpty(),
          "a rejected sign-in host flattens to no command line at all");

    // Release refs must not become visible on a managed mirror until every
    // signed artifact is there. The controller streams each blob through this
    // fixed receiver, which hashes and atomically installs it into the gateway
    // CAS instead of making the host fetch through the not-yet-ready mesh.
    const QString releaseHash(64, QLatin1Char('a'));
    const QString seedReleaseArtifact =
        forkmesh::control::managedReleaseArtifactSeedCommand(releaseHash);
    check(seedReleaseArtifact.contains(QStringLiteral("cat >\"$tmp\"")) &&
              seedReleaseArtifact.contains(QStringLiteral("sha256sum")) &&
              seedReleaseArtifact.contains(
                  QStringLiteral("runuser -u forkmesh-node")) &&
              seedReleaseArtifact.contains(QStringLiteral(
                  "install -d -o forkmesh-node -g forkmesh-node -m 0700")) &&
              seedReleaseArtifact.indexOf(QStringLiteral("install -d -o")) <
                  seedReleaseArtifact.indexOf(
                      QStringLiteral("exec runuser -u forkmesh-node")) &&
              seedReleaseArtifact.contains(releaseHash) &&
              seedReleaseArtifact.contains(
                  QStringLiteral("forkmesh-releases/sha256/aa/")) &&
              seedReleaseArtifact.contains(
                  QStringLiteral("mv -f -- \"$tmp\" \"$dir/data\"")) &&
              !seedReleaseArtifact.contains(QStringLiteral("curl")),
          "managed release artifacts are verified and installed atomically "
          "from the controller stream even when the CAS shard is root-owned");
    check(forkmesh::control::managedReleaseArtifactSeedCommand(
              QStringLiteral("aa; touch /tmp/injected"))
              .isEmpty(),
          "release artifact seed commands reject non-digest input");
    const QString secondReleaseHash(64, QLatin1Char('b'));
    const QString inventoryReleaseArtifacts =
        forkmesh::control::managedReleaseArtifactInventoryCommand(
            {releaseHash, secondReleaseHash});
    check(inventoryReleaseArtifacts.contains(QStringLiteral("sha256sum")) &&
              inventoryReleaseArtifacts.contains(
                  QStringLiteral("runuser -u forkmesh-node")) &&
              inventoryReleaseArtifacts.contains(releaseHash) &&
              inventoryReleaseArtifacts.contains(secondReleaseHash) &&
              inventoryReleaseArtifacts.contains(
                  QStringLiteral("printf \"%s\\n\" \"$hash\"")) &&
              !inventoryReleaseArtifacts.contains(QStringLiteral("curl")),
          "managed release inventory reports only absent or corrupt CAS blobs");
    check(forkmesh::control::managedReleaseArtifactInventoryCommand(
              {releaseHash, QStringLiteral("aa; touch /tmp/injected")})
              .isEmpty() &&
              forkmesh::control::managedReleaseArtifactInventoryCommand(
                  {releaseHash, releaseHash})
                  .isEmpty(),
          "release artifact inventory rejects unsafe or duplicate digests");
#ifdef Q_OS_UNIX
    check(runProcess(QStringLiteral("/bin/sh"),
                     {QStringLiteral("-n"), QStringLiteral("-c"),
                      seedReleaseArtifact}),
          "the managed release artifact receiver is valid POSIX shell");
    check(runProcess(QStringLiteral("/bin/sh"),
                     {QStringLiteral("-n"), QStringLiteral("-c"),
                      inventoryReleaseArtifacts}),
          "the managed release artifact inventory is valid POSIX shell");
#endif

    // Host size map (adhoc #390): a read-only `du` browser over the same
    // authenticated SSH channel.
    check(forkmesh::control::normalizeRemoteDiskPath(
              QStringLiteral("/var//lib/./forkmesh/")) ==
                  QStringLiteral("/var/lib/forkmesh") &&
              forkmesh::control::normalizeRemoteDiskPath(
                  QStringLiteral("/var/lib/..")) == QStringLiteral("/var") &&
              forkmesh::control::normalizeRemoteDiskPath(
                  QStringLiteral("/../..")) == QStringLiteral("/") &&
              forkmesh::control::normalizeRemoteDiskPath(QString()) ==
                  QStringLiteral("/"),
          "remote size-map paths collapse to canonical absolute paths");
    check(forkmesh::control::normalizeRemoteDiskPath(
              QStringLiteral("var/lib")).isEmpty() &&
              forkmesh::control::normalizeRemoteDiskPath(
                  QStringLiteral("/var\nrm -rf /")).isEmpty(),
          "relative and newline-bearing size-map paths are rejected");

    QString diskError;
    const QString diskCommand = forkmesh::control::buildHostDiskUsageCommand(
        QStringLiteral("/srv/it's here"), &diskError);
    check(diskError.isEmpty() &&
              diskCommand.startsWith(QStringLiteral("sh -lc '")) &&
              diskCommand.contains(QStringLiteral("du -x -k -a -d 1")) &&
              diskCommand.contains(QStringLiteral("'\\''")) &&
              !diskCommand.contains(QStringLiteral("rm ")) &&
              !diskCommand.contains(QStringLiteral("chmod")),
          "the size-map command is a single quoted read-only du level");
    check(forkmesh::control::buildHostDiskUsageCommand(
              QStringLiteral("relative/path"), &diskError).isEmpty() &&
              !diskError.isEmpty(),
          "the size-map command refuses a non-absolute path");

    const QByteArray diskOutput =
        QByteArray("Welcome to Ubuntu (banner noise)\n") +
        "FORKMESH-DU1 d 2048 " + QByteArray("/var/log").toBase64() + "\n" +
        "FORKMESH-DU1 f 4 " + QByteArray("/var/notes 'x'.txt").toBase64() + "\n" +
        "FORKMESH-DU1 d 8192 " + QByteArray("/var/lib").toBase64() + "\n" +
        "FORKMESH-DU1 T 10244 " + QByteArray("/var").toBase64() + "\n" +
        "FORKMESH-DU1-END\n";
    const auto usage =
        forkmesh::control::parseHostDiskUsage(diskOutput, QStringLiteral("/var"));
    check(usage.complete && usage.error.isEmpty() &&
              usage.totalBytes == 10244LL * 1024 &&
              usage.entries.size() == 3 &&
              usage.entries.at(0).path == QStringLiteral("/var/lib") &&
              usage.entries.at(0).name == QStringLiteral("lib") &&
              usage.entries.at(0).directory &&
              usage.entries.at(0).bytes == 8192LL * 1024 &&
              usage.entries.at(2).name == QStringLiteral("notes 'x'.txt") &&
              !usage.entries.at(2).directory,
          "the size map parses banner-wrapped du sentinels largest first");
    const auto diskFailure = forkmesh::control::parseHostDiskUsage(
        QByteArray("FORKMESH-DU1-ERROR ") + QByteArray("nope").toBase64() + "\n",
        QStringLiteral("/root"));
    check(diskFailure.complete && diskFailure.error == QStringLiteral("nope") &&
              diskFailure.entries.isEmpty(),
          "a host-side size-map refusal surfaces as an error, not an empty tree");
    check(forkmesh::control::parseHostDiskUsage(
              QByteArray("ssh: connect to host port 22: Connection refused\n"),
              QStringLiteral("/"))
              .complete == false,
          "a transport failure never looks like a complete size map");
    // A size map that dies on the credentials asks for the host password and
    // retries; every other 255 is something no password can fix.
    check(forkmesh::control::sshFailureNeedsPassword(
              255, QStringLiteral("root@203.0.113.10: Permission denied "
                                  "(publickey,password).")) &&
              forkmesh::control::sshFailureNeedsPassword(
                  255, QStringLiteral("Authentication failed.")),
          "a rejected SSH credential is worth asking for a password");
    check(!forkmesh::control::sshFailureNeedsPassword(
              255, QStringLiteral("ssh: connect to host port 22: "
                                  "Connection timed out")) &&
              !forkmesh::control::sshFailureNeedsPassword(
                  255, QStringLiteral("Host key verification failed.")) &&
              !forkmesh::control::sshFailureNeedsPassword(
                  1, QStringLiteral("permission denied")),
          "timeouts, host-key mismatches and remote exits never prompt for a "
          "password");
    check(forkmesh::control::formatDiskSize(0) == QStringLiteral("0 B") &&
              forkmesh::control::formatDiskSize(1536) ==
                  QStringLiteral("1.5 KB") &&
              forkmesh::control::formatDiskSize(3LL * 1024 * 1024 * 1024) ==
                  QStringLiteral("3.0 GB"),
          "size-map byte formatting is human readable");
    const QString mountCommand =
        forkmesh::control::buildHostMountUsageCommand();
    check(mountCommand.startsWith(QStringLiteral("sh -lc '")) &&
              mountCommand.contains(QStringLiteral("df -P -k -l")) &&
              mountCommand.contains(QStringLiteral("FORKMESH-MOUNT1")) &&
              !mountCommand.contains(QStringLiteral("rm ")) &&
              !mountCommand.contains(QStringLiteral("chmod")),
          "the mount overview command is read-only and sentinel framed");
    const QByteArray mountOutput =
        QByteArray("login banner\n") +
        "FORKMESH-MOUNT1 102400 25600 76800 " +
        QByteArray("/").toBase64() + "\n" +
        "FORKMESH-MOUNT1 204800 153600 51200 " +
        QByteArray("/mnt/project data").toBase64() + "\n" +
        // Duplicate paths are ignored rather than producing duplicate cards.
        "FORKMESH-MOUNT1 102400 25600 76800 " +
        QByteArray("/").toBase64() + "\n" +
        "FORKMESH-MOUNT1-END\n";
    const auto mounts =
        forkmesh::control::parseHostMountUsage(mountOutput);
    check(mounts.complete && mounts.error.isEmpty() &&
              mounts.mounts.size() == 2 &&
              mounts.mounts.at(0).path == QStringLiteral("/") &&
              mounts.mounts.at(0).usedBytes == 25600LL * 1024 &&
              mounts.mounts.at(1).path ==
                  QStringLiteral("/mnt/project data") &&
              mounts.mounts.at(1).availableBytes == 51200LL * 1024,
          "mount overview parsing is banner-safe, deduplicated and root-first");
    const auto mountFailure = forkmesh::control::parseHostMountUsage(
        QByteArray("FORKMESH-MOUNT1-ERROR ") +
        QByteArray("df unavailable").toBase64() +
        "\nFORKMESH-MOUNT1-END\n");
    check(mountFailure.complete &&
              mountFailure.error == QStringLiteral("df unavailable") &&
              mountFailure.mounts.isEmpty(),
          "a host-side mount overview failure remains actionable");

    // One-click Vultr provisioning helpers (adhoc #315).
    check(forkmesh::control::validateVultrMirrorRequest(
              QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"),
              QStringLiteral("vultr-mirror-1"))
              .isEmpty(),
          "a plausible Vultr key and node name validate");
    check(!forkmesh::control::validateVultrMirrorRequest(
               QStringLiteral("short"), QStringLiteral("vultr-mirror-1"))
               .isEmpty() &&
              !forkmesh::control::validateVultrMirrorRequest(
                   QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"),
                   QStringLiteral("Bad Name"))
                   .isEmpty(),
          "Vultr validation rejects malformed keys and node names");

    // A key saved anywhere is the key every page resolves (adhoc #127): the
    // canonical name wins, the Quick setup alias is honoured, and a value that
    // could not survive an environment or .env line is skipped.
    check(forkmesh::control::vultrApiKeyVariableNames().constFirst() ==
                  QStringLiteral("VULTR_API_KEY") &&
              forkmesh::control::vultrApiKeyVariableNames().contains(
                  QStringLiteral("VULTR_API_TOKEN")),
          "VULTR_API_KEY is canonical and the Quick setup alias resolves too");
    check(forkmesh::control::vultrApiKeyFromVariables(
              {{QStringLiteral("VULTR_API_TOKEN"),
                QStringLiteral("from-quick-setup")}}) ==
                  QStringLiteral("from-quick-setup"),
          "a Vultr key saved by Quick setup alone still provisions mirrors");
    check(forkmesh::control::vultrApiKeyFromVariables(
              {{QStringLiteral("VULTR_API_KEY"), QStringLiteral("canonical")},
               {QStringLiteral("VULTR_API_TOKEN"), QStringLiteral("older")}}) ==
                  QStringLiteral("canonical"),
          "the canonical Vultr variable outranks a stale alias");
    check(forkmesh::control::vultrApiKeyFromVariables(
              {{QStringLiteral("VULTR_API_KEY"), QStringLiteral(" \n")},
               {QStringLiteral("VULTR_TOKEN"), QStringLiteral("usable")}}) ==
                  QStringLiteral("usable"),
          "an empty or multi-line Vultr variable never shadows a usable one");

    const QJsonArray vultrPlans{
        QJsonObject{{QStringLiteral("id"), QStringLiteral("vc2-2c-4gb")},
                    {QStringLiteral("monthly_cost"), 20},
                    {QStringLiteral("ram"), 4096},
                    {QStringLiteral("locations"),
                     QJsonArray{QStringLiteral("ewr")}}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("vc2-1c-1gb")},
                    {QStringLiteral("monthly_cost"), 5},
                    {QStringLiteral("ram"), 1024},
                    {QStringLiteral("locations"),
                     QJsonArray{QStringLiteral("fra"),
                                QStringLiteral("ams")}}},
        // Cheapest IPv4 plan, but too small for the encrypted temporary
        // repository materialization. Automatic provisioning must skip it.
        QJsonObject{{QStringLiteral("id"), QStringLiteral("vc2-1c-0.5gb")},
                    {QStringLiteral("monthly_cost"), 2.5},
                    {QStringLiteral("ram"), 512},
                    {QStringLiteral("locations"),
                     QJsonArray{QStringLiteral("ewr")}}},
        // Cheaper but sold out everywhere: not deployable, must be skipped.
        QJsonObject{{QStringLiteral("id"), QStringLiteral("vc2-old")},
                    {QStringLiteral("monthly_cost"), 3},
                    {QStringLiteral("ram"), 512},
                    {QStringLiteral("locations"), QJsonArray{}}},
        // Same price, more RAM: preferred deterministically.
        QJsonObject{{QStringLiteral("id"), QStringLiteral("vhp-1c-2gb")},
                    {QStringLiteral("monthly_cost"), 5},
                    {QStringLiteral("ram"), 2048},
                    {QStringLiteral("locations"),
                     QJsonArray{QStringLiteral("syd"),
                                QStringLiteral("atl"),
                                QStringLiteral("ewr")}}},
        // Cheapest of all, but IPv6-only: unreachable for the mesh (adhoc #344).
        QJsonObject{{QStringLiteral("id"), QStringLiteral("vc2-1c-0.5gb-v6")},
                    {QStringLiteral("monthly_cost"), 2.5},
                    {QStringLiteral("ram"), 512},
                    {QStringLiteral("locations"),
                     QJsonArray{QStringLiteral("ewr")}}},
    };
    const QJsonObject cheapest =
        forkmesh::control::cheapestVultrPlan(vultrPlans);
    check(cheapest.value(QStringLiteral("id")).toString() ==
              QStringLiteral("vhp-1c-2gb"),
          "cheapest Vultr plan skips undeployable plans and breaks ties on RAM");
    check(forkmesh::control::vultrPlanHasIpv4(cheapest) &&
              !forkmesh::control::vultrPlanHasIpv4(
                  vultrPlans.at(5).toObject()) &&
              !forkmesh::control::vultrPlanHasIpv4(QJsonObject()),
          "IPv6-only Vultr plans are never eligible, however cheap");
    check(forkmesh::control::vultrPlanRegion(cheapest) ==
              QStringLiteral("ewr") &&
              forkmesh::control::vultrPlanRegion(
                  vultrPlans.at(1).toObject()).isEmpty(),
          "Vultr region selection prefers New Jersey and rejects non-US locations");
    check(forkmesh::control::cheapestVultrPlan(QJsonArray()).isEmpty(),
          "an empty Vultr plan list yields no selection");

    // The same rules as choices for the owner's phone (hosts.options), and the
    // phone's pick honoured only inside them (hosts.run "install").
    const QJsonObject choices = forkmesh::control::vultrPlanChoices(vultrPlans);
    const QJsonArray choicePlans = choices.value(QStringLiteral("plans")).toArray();
    check(choicePlans.size() == 2 &&
              choicePlans.at(0).toObject().value(QStringLiteral("id")).toString() ==
                  QStringLiteral("vhp-1c-2gb") &&
              choicePlans.at(1).toObject().value(QStringLiteral("id")).toString() ==
                  QStringLiteral("vc2-2c-4gb"),
          "plan choices are exactly the automatic rule's eligible plans, cheapest first");
    check(choicePlans.at(0).toObject().value(QStringLiteral("regions")).toArray() ==
              QJsonArray({QStringLiteral("ewr"), QStringLiteral("atl")}),
          "plan regions follow the automatic preference and drop non-US locations");
    check(choices.value(QStringLiteral("defaultPlan")).toString() ==
                  QStringLiteral("vhp-1c-2gb") &&
              choices.value(QStringLiteral("defaultRegion")).toString() ==
                  QStringLiteral("ewr"),
          "the default choice is what the desktop picks on its own");
    QString chosenRegion;
    QString choiceError;
    check(forkmesh::control::chooseVultrPlan(vultrPlans, QString(), QString(),
                                             &chosenRegion)
                  .value(QStringLiteral("id"))
                  .toString() == QStringLiteral("vhp-1c-2gb") &&
              chosenRegion == QStringLiteral("ewr"),
          "no pick falls back to the automatic plan and region");
    check(forkmesh::control::chooseVultrPlan(vultrPlans,
                                             QStringLiteral("vhp-1c-2gb"),
                                             QStringLiteral("ATL"), &chosenRegion)
                  .value(QStringLiteral("id"))
                  .toString() == QStringLiteral("vhp-1c-2gb") &&
              chosenRegion == QStringLiteral("atl"),
          "a picked plan and region inside the choices are used");
    check(forkmesh::control::chooseVultrPlan(vultrPlans,
                                             QStringLiteral("vc2-1c-0.5gb"),
                                             QString(), &chosenRegion,
                                             &choiceError)
                  .isEmpty() &&
              !choiceError.isEmpty(),
          "a plan too small for a mirror is refused even when picked");
    check(forkmesh::control::chooseVultrPlan(vultrPlans,
                                             QStringLiteral("vhp-1c-2gb"),
                                             QStringLiteral("syd"), &chosenRegion,
                                             &choiceError)
                  .isEmpty(),
          "a region outside the supported US set is refused");

    const QJsonArray vultrOs{
        QJsonObject{{QStringLiteral("id"), 401},
                    {QStringLiteral("name"), QStringLiteral("Debian 11 x64")},
                    {QStringLiteral("arch"), QStringLiteral("x64")},
                    {QStringLiteral("family"), QStringLiteral("debian")}},
        QJsonObject{{QStringLiteral("id"), 477},
                    {QStringLiteral("name"),
                     QStringLiteral("Debian 12 x64 (bookworm)")},
                    {QStringLiteral("arch"), QStringLiteral("x64")},
                    {QStringLiteral("family"), QStringLiteral("debian")}},
        QJsonObject{{QStringLiteral("id"), 999},
                    {QStringLiteral("name"),
                     QStringLiteral("Ubuntu 24.04 LTS x64")},
                    {QStringLiteral("arch"), QStringLiteral("x64")},
                    {QStringLiteral("family"), QStringLiteral("ubuntu")}},
        QJsonObject{{QStringLiteral("id"), 478},
                    {QStringLiteral("name"),
                     QStringLiteral("Debian 12 i386")},
                    {QStringLiteral("arch"), QStringLiteral("i386")},
                    {QStringLiteral("family"), QStringLiteral("debian")}},
    };
    check(forkmesh::control::latestVultrDebianOs(vultrOs)
                  .value(QStringLiteral("id"))
                  .toInt() == 477,
          "latest Vultr Debian selection picks the newest x64 Debian only");

    // A freshly created mirror has nothing mirroring it yet, so the provisioner
    // must direct-upload this app's binary rather than let the installer look
    // for an online mirror — but only when that binary can actually run there.
    check(forkmesh::control::localBinaryRunsOnVultrMirror(
              QStringLiteral("linux"), QStringLiteral("x86_64")),
          "linux/x86_64 desktop uploads its own binary to a new Vultr mirror");
    check(forkmesh::control::localBinaryRunsOnVultrMirror(
              QStringLiteral("Linux"), QStringLiteral("amd64")),
          "amd64/x64 spellings count as x86_64 for the upload decision");
    check(!forkmesh::control::localBinaryRunsOnVultrMirror(
              QStringLiteral("darwin"), QStringLiteral("x86_64")),
          "macOS desktop does not upload a binary the Debian mirror can't run");
    check(!forkmesh::control::localBinaryRunsOnVultrMirror(
              QStringLiteral("linux"), QStringLiteral("arm64")),
          "arm64 desktop does not upload a binary the x64 mirror can't run");

    const QJsonObject instancePayload =
        forkmesh::control::vultrInstanceCreatePayload(
            QStringLiteral("vultr-mirror-1"), QStringLiteral("vhp-1c-2gb"),
            QStringLiteral("ewr"), 477, QStringLiteral("key-id-1"));
    check(instancePayload.value(QStringLiteral("plan")).toString() ==
                  QStringLiteral("vhp-1c-2gb") &&
              instancePayload.value(QStringLiteral("region")).toString() ==
                  QStringLiteral("ewr") &&
              instancePayload.value(QStringLiteral("os_id")).toInt() == 477 &&
              instancePayload.value(QStringLiteral("sshkey_id")).toArray() ==
                  QJsonArray{QStringLiteral("key-id-1")} &&
              instancePayload.value(QStringLiteral("backups")).toString() ==
                  QStringLiteral("disabled") &&
              instancePayload.value(QStringLiteral("activation_email"))
                      .toBool() == false &&
              instancePayload.value(QStringLiteral("label")).toString() ==
                  QStringLiteral("vultr-mirror-1"),
          "the Vultr instance payload pins plan, region, OS, key and no extras");

    const QJsonObject bootingInstance{
        {QStringLiteral("status"), QStringLiteral("pending")},
        {QStringLiteral("power_status"), QStringLiteral("running")},
        {QStringLiteral("main_ip"), QStringLiteral("0.0.0.0")},
    };
    const QJsonObject readyInstance{
        {QStringLiteral("status"), QStringLiteral("active")},
        {QStringLiteral("power_status"), QStringLiteral("running")},
        {QStringLiteral("main_ip"), QStringLiteral("203.0.113.99")},
    };
    QJsonObject stoppedInstance = readyInstance;
    stoppedInstance.insert(QStringLiteral("power_status"),
                           QStringLiteral("stopped"));
    check(forkmesh::control::vultrInstanceReadyIp(bootingInstance).isEmpty() &&
              forkmesh::control::vultrInstanceReadyIp(stoppedInstance)
                  .isEmpty() &&
              forkmesh::control::vultrInstanceReadyIp(readyInstance) ==
                  QStringLiteral("203.0.113.99"),
          "instance readiness requires active+running and a real IPv4");
    check(instancePayload.value(QStringLiteral("enable_ipv6")).toBool(true) ==
              false,
          "the Vultr instance payload never opts into an IPv6-only address");

    QJsonObject ipv6OnlyInstance = readyInstance;
    ipv6OnlyInstance.insert(QStringLiteral("main_ip"),
                            QStringLiteral("0.0.0.0"));
    ipv6OnlyInstance.insert(QStringLiteral("v6_main_ip"),
                            QStringLiteral("2001:db8::1"));
    QJsonObject dualStackInstance = readyInstance;
    dualStackInstance.insert(QStringLiteral("v6_main_ip"),
                             QStringLiteral("2001:db8::1"));
    check(forkmesh::control::vultrInstanceIsIpv6Only(ipv6OnlyInstance) &&
              !forkmesh::control::vultrInstanceIsIpv6Only(dualStackInstance) &&
              !forkmesh::control::vultrInstanceIsIpv6Only(bootingInstance),
          "an IPv6-only instance is detected instead of polled to a timeout");

    check(forkmesh::control::nextMirrorNodeName(
              {QStringLiteral("mirror1"), QStringLiteral("Mirror4"),
               QStringLiteral("laptop")}) == QStringLiteral("mirror5") &&
              forkmesh::control::nextMirrorNodeName({}) ==
                  QStringLiteral("mirror1") &&
              forkmesh::control::nextMirrorNodeName(
                  {QStringLiteral("mirror2"), QStringLiteral(" mirror3 ")}) ==
                  QStringLiteral("mirror4"),
          "the default mirror name continues the fleet's own numbering");

    QMap<QString, QString> vultrVariables;
    vultrVariables.insert(QStringLiteral("vultr_api_key"),
                          QStringLiteral("STOREDVULTRKEY01234567890"));
    vultrVariables.insert(QStringLiteral("OTHER"),
                          QStringLiteral("unrelated"));
    check(forkmesh::control::vultrApiKeyFromVariables(vultrVariables) ==
                  QStringLiteral("STOREDVULTRKEY01234567890") &&
              forkmesh::control::vultrApiKeyFromVariables({}).isEmpty(),
          "the stored VULTR_API_KEY device variable is resolved case-insensitively");

    // --- Destroying a mirror's server on Vultr (adhoc #24) -----------------
    const QString destroyId =
        QStringLiteral("1f2e3d4c-5b6a-4798-8899-aabbccddeeff");
    check(forkmesh::control::savedHostVultrInstanceId(
              QJsonObject{{QStringLiteral("provider"), QStringLiteral("vultr")},
                          {QStringLiteral("instanceId"), destroyId}}) ==
                  destroyId &&
              forkmesh::control::savedHostVultrInstanceId(
                  QJsonObject{{QStringLiteral("provider"),
                               QStringLiteral("Hetzner")},
                              {QStringLiteral("instanceId"), destroyId}})
                  .isEmpty() &&
              forkmesh::control::savedHostVultrInstanceId(
                  QJsonObject{{QStringLiteral("provider"),
                               QStringLiteral("Vultr")}})
                  .isEmpty(),
          "only a Vultr-provisioned host with a recorded instance id is "
          "destroyable by id");

    const QJsonArray destroyInstances{
        QJsonObject{{QStringLiteral("id"), destroyId},
                    {QStringLiteral("main_ip"), QStringLiteral("203.0.113.7")},
                    {QStringLiteral("label"), QStringLiteral("mirror5")}},
        QJsonObject{{QStringLiteral("id"),
                     QStringLiteral("00000000-1111-2222-3333-444444444444")},
                    {QStringLiteral("main_ip"), QStringLiteral("0.0.0.0")},
                    {QStringLiteral("label"), QStringLiteral("mirror6")}},
    };
    check(forkmesh::control::vultrInstanceIdForAddress(
              destroyInstances, QStringLiteral("203.0.113.7")) == destroyId &&
              forkmesh::control::vultrInstanceIdForAddress(
                  destroyInstances, QStringLiteral("MIRROR5")) == destroyId &&
              forkmesh::control::vultrInstanceIdForAddress(
                  destroyInstances, QStringLiteral("0.0.0.0"))
                  .isEmpty() &&
              forkmesh::control::vultrInstanceIdForAddress(
                  destroyInstances, QStringLiteral("198.51.100.9"))
                  .isEmpty() &&
              forkmesh::control::vultrInstanceIdForAddress(
                  destroyInstances, QString())
                  .isEmpty(),
          "a host is matched to its Vultr instance by address or label, and "
          "an unassigned address never matches");

    const QJsonArray ambiguousInstances{
        QJsonObject{{QStringLiteral("id"), destroyId},
                    {QStringLiteral("main_ip"), QStringLiteral("203.0.113.7")}},
        QJsonObject{{QStringLiteral("id"),
                     QStringLiteral("00000000-1111-2222-3333-444444444444")},
                    {QStringLiteral("label"), QStringLiteral("203.0.113.7")}},
    };
    check(forkmesh::control::vultrInstanceIdForAddress(
              ambiguousInstances, QStringLiteral("203.0.113.7"))
              .isEmpty(),
          "two instances matching one address destroy neither");

    check(forkmesh::control::validateVultrDestroyRequest(
              QStringLiteral("STOREDVULTRKEY01234567890"), destroyId)
                  .isEmpty() &&
              !forkmesh::control::validateVultrDestroyRequest(
                   QStringLiteral("short"), destroyId)
                   .isEmpty() &&
              !forkmesh::control::validateVultrDestroyRequest(
                   QStringLiteral("STOREDVULTRKEY01234567890"),
                   QStringLiteral("not-an-instance"))
                   .isEmpty() &&
              !forkmesh::control::validateVultrDestroyRequest(
                   QStringLiteral("STOREDVULTRKEY01234567890"), QString())
                   .isEmpty(),
          "a destroy call is refused without a plausible API key and instance "
          "id");

    const auto healthyMirror = [](const QString &name) {
        return QJsonObject{
            {QStringLiteral("node"), name},
            {QStringLiteral("status"), QStringLiteral("online")},
            {QStringLiteral("integrity"), QStringLiteral("ok")},
            {QStringLiteral("lastSync"), 123456},
            {QStringLiteral("cloneAvailable"), true},
            {QStringLiteral("endpointHealthy"), true},
            {QStringLiteral("endpointFresh"), true},
        };
    };
    check(!forkmesh::control::mirrorRepositoryProofServesContent(
              QStringLiteral("e3b0c44298fc1c149afbf4c8996fb92427ae41e464"
                             "9b934ca495991b7852b855")) &&
              !forkmesh::control::mirrorRepositoryProofServesContent(
                  QString()) &&
              !forkmesh::control::mirrorRepositoryProofServesContent(
                  QStringLiteral("short")) &&
              forkmesh::control::mirrorRepositoryProofServesContent(
                  QStringLiteral("12ee473574d10e225e2d986166900261513fac51"
                                 "cb0e695f68cd35f18ea1ceac")),
          "a signed proof over zero refs (the empty digest) does not count "
          "as serving content");

    const QStringList seedArguments =
        forkmesh::control::mirrorSeedPushArguments(
            QStringLiteral("/home/user/mirrors/forkmesh-forkmesh.git"),
            QStringLiteral("203.0.113.7"));
    check(seedArguments.size() == 7 &&
              seedArguments.at(0) == QStringLiteral(
                  "--git-dir=/home/user/mirrors/forkmesh-forkmesh.git") &&
              seedArguments.at(1) == QStringLiteral("push") &&
              seedArguments.at(2) == QStringLiteral("--prune") &&
              seedArguments.at(3) == QStringLiteral(
                  "--receive-pack=runuser -u forkmesh-node -- "
                  "git-receive-pack") &&
              seedArguments.at(4) == QStringLiteral(
                  "ssh://root@203.0.113.7/var/lib/forkmesh/repositories/"
                  "forkmesh-forkmesh.git") &&
              seedArguments.contains(
                  QStringLiteral("+refs/heads/*:refs/heads/*")) &&
              seedArguments.contains(
                  QStringLiteral("+refs/tags/*:refs/tags/*")),
          "the mirror seed push mirrors every head and tag with prune and "
          "keeps objects node-owned via the runuser receive-pack");
    check(forkmesh::control::mirrorSeedPushArguments(
              QString(), QStringLiteral("203.0.113.7")).isEmpty() &&
              forkmesh::control::mirrorSeedPushArguments(
                  QStringLiteral("/m.git"), QStringLiteral("  ")).isEmpty(),
          "a seed push without a mirror path or address builds no command");
    check(forkmesh::control::mirrorSeedPushArguments(
              QStringLiteral("/m.git"), QStringLiteral("2001:db8::7"))
                  .at(4) ==
              QStringLiteral("ssh://root@[2001:db8::7]/var/lib/forkmesh/"
                             "repositories/forkmesh-forkmesh.git"),
          "an IPv6 seed target is bracketed in the push URL");
    check(forkmesh::control::mirrorSeedSshCommand(
              QStringLiteral("/keys/it's here/id_ed25519")) ==
              QStringLiteral("ssh -i '/keys/it'\\''s here/id_ed25519' "
                             "-o StrictHostKeyChecking=accept-new "
                             "-o ConnectTimeout=10"),
          "the seed SSH command single-quotes the identity file for the "
          "shell that parses GIT_SSH_COMMAND");

    QJsonObject staleMirror = healthyMirror(QStringLiteral("mirror1"));
    staleMirror.insert(QStringLiteral("endpointFresh"), false);
    check(forkmesh::control::mirrorCatalogEntryIsHealthy(
              healthyMirror(QStringLiteral("mirror1"))) &&
              !forkmesh::control::mirrorCatalogEntryIsHealthy(staleMirror),
          "fleet health requires the complete public traffic health contract");

    // --- Saying why stage 6 is still waiting -------------------------------
    check(forkmesh::control::describeMirrorCatalogEntryGap(
              healthyMirror(QStringLiteral("mirror1")))
                  .isEmpty() &&
              forkmesh::control::describeMirrorCatalogEntryGap(staleMirror)
                  .contains(QStringLiteral("endpoint lease")) &&
              forkmesh::control::describeMirrorCatalogEntryGap({})
                  .contains(QStringLiteral("catalog row")),
          "publication verification can name the health gate a node has not "
          "cleared instead of showing only a poll counter");

    // --- A newly saved host keeps the metadata it was created with ---------
    // The append path used to drop `metadata` entirely, so a freshly
    // provisioned mirror was stored with no instanceId and never joined the
    // managed fleet — the reconciler then created a replacement every cycle.
    const QJsonObject provisionMetadata{
        {QStringLiteral("provider"), QStringLiteral("Vultr")},
        {QStringLiteral("instanceId"), destroyId},
        {QStringLiteral("planType"), QStringLiteral("vc2-1c-1gb")},
    };
    const QJsonObject freshRecord = forkmesh::control::mergeSavedHostRecord(
        {},
        QJsonObject{{QStringLiteral("name"), QStringLiteral("mirror7")},
                    {QStringLiteral("status"), QStringLiteral("vultr booting")}},
        provisionMetadata);
    check(forkmesh::control::savedHostVultrInstanceId(freshRecord) ==
              destroyId,
          "a host saved for the first time keeps its provisioning metadata");

    const QJsonObject statusOnlyUpdate =
        forkmesh::control::mergeSavedHostRecord(
            freshRecord,
            QJsonObject{{QStringLiteral("name"), QStringLiteral("mirror7")},
                        {QStringLiteral("status"),
                         QStringLiteral("installed")}},
            {});
    check(forkmesh::control::savedHostVultrInstanceId(statusOnlyUpdate) ==
              destroyId &&
              statusOnlyUpdate.value(QStringLiteral("status")).toString() ==
                  QStringLiteral("installed"),
          "a later status-only update keeps the recorded Vultr instance");

    // --- Deleted hosts leave the Mirror nodes list at once -----------------
    // The relay keeps a destroyed mirror's last signed catalog row for the
    // length of its endpoint freshness lease, so /mirrors went on reporting a
    // server this app had already destroyed as online, healthy and serving.
    {
        const qint64 nowMs = 1770000000000LL;
        const QJsonObject ledger = forkmesh::control::recordDecommissionedNode(
            {}, QStringLiteral("Mirror22"), nowMs);
        check(ledger.contains(QStringLiteral("mirror22")),
              "a decommissioned node is tombstoned under its lowercased name");
        check(forkmesh::control::mirrorNodeIsDecommissioned(
                  ledger, QStringLiteral("mirror22"),
                  nowMs - 5 * 60 * 1000, nowMs),
              "a catalog row published before the deletion is suppressed");
        check(forkmesh::control::mirrorNodeIsDecommissioned(
                  ledger, QStringLiteral("mirror22"), 0, nowMs),
              "a row with no publication timestamp at all is suppressed");
        check(!forkmesh::control::mirrorNodeIsDecommissioned(
                  ledger, QStringLiteral("mirror9"), 0, nowMs),
              "a node that was never deleted here is left alone");
        check(!forkmesh::control::mirrorNodeIsDecommissioned(
                  ledger, QStringLiteral("mirror22"),
                  nowMs + forkmesh::control::kDecommissionedNodeGraceMs + 1,
                  nowMs + forkmesh::control::kDecommissionedNodeGraceMs + 1),
              "a node that publishes again after the grace window is alive and "
              "stays visible");
        check(!forkmesh::control::mirrorNodeIsDecommissioned(
                  ledger, QStringLiteral("mirror22"), 0,
                  nowMs + forkmesh::control::kDecommissionedNodeTtlMs + 1),
              "the tombstone expires rather than hiding the name forever");
        check(!forkmesh::control::mirrorNodeIsDecommissioned(
                  forkmesh::control::forgetDecommissionedNode(
                      ledger, QStringLiteral("MIRROR22")),
                  QStringLiteral("mirror22"), 0, nowMs),
              "re-saving a host under the same name clears its predecessor's "
              "tombstone, so a re-provisioned server is never hidden");

        QJsonObject crowded;
        for (int i = 0; i < forkmesh::control::kDecommissionedNodeLimit + 20;
             ++i) {
            crowded = forkmesh::control::recordDecommissionedNode(
                crowded, QStringLiteral("mirror%1").arg(i), nowMs + i);
        }
        check(crowded.size() <= forkmesh::control::kDecommissionedNodeLimit &&
                  crowded.contains(QStringLiteral("mirror219")) &&
                  !crowded.contains(QStringLiteral("mirror0")),
              "the ledger is bounded and drops its oldest tombstones first");
    }

    // --- Installing a fresh mirror without a published release (adhoc #408) -
    check(forkmesh::control::localBinaryRunsOnVultrMirror(
              QStringLiteral("linux"), QStringLiteral("x86_64")) &&
              forkmesh::control::localBinaryRunsOnVultrMirror(
                  QStringLiteral("Linux"), QStringLiteral("amd64")) &&
              !forkmesh::control::localBinaryRunsOnVultrMirror(
                  QStringLiteral("linux"), QStringLiteral("arm64")) &&
              !forkmesh::control::localBinaryRunsOnVultrMirror(
                  QStringLiteral("darwin"), QStringLiteral("x86_64")) &&
              !forkmesh::control::localBinaryRunsOnVultrMirror(
                  QStringLiteral("winnt"), QStringLiteral("x86_64")),
          "only a linux/x86_64 controller uploads its own binary to the "
          "Debian x64 instance the flow deploys");

    check(forkmesh::control::vultrInstallNeedsLocalBinary(QStringLiteral(
              "Error: No online ForkMesh node is currently mirroring "
              "'forkmesh'.")) &&
              forkmesh::control::vultrInstallNeedsLocalBinary(QStringLiteral(
                  "Error: No prebuilt ForkMesh binary is published for "
                  "linux/x86_64, and falling back to a source build is "
                  "disabled (FORKMESH_NO_SOURCE_FALLBACK=1, the default on "
                  "headless Linux).")) &&
              forkmesh::control::vultrInstallNeedsLocalBinary(QStringLiteral(
                  "Error: No prebuilt ForkMesh release passed independent "
                  "manifest authentication and SHA-256 verification.")) &&
              !forkmesh::control::vultrInstallNeedsLocalBinary(QStringLiteral(
                  "ssh: connect to host 203.0.113.10 port 22: Connection "
                  "refused")),
          "an install that found nothing to download escalates to the "
          "direct upload, an unreachable host does not");

    // --- Waiting for a fresh instance's SSH (adhoc #48) --------------------
    const QString sshProbe = forkmesh::control::vultrSshProbeRemoteCommand();
    check(!sshProbe.trimmed().isEmpty() &&
              !sshProbe.contains(QLatin1Char(';')) &&
              !sshProbe.contains(QLatin1Char('&')) &&
              !sshProbe.contains(QLatin1Char('>')),
          "the reachability knock is a single side-effect-free command");
    // What the remote shell echoes back: the knock's own last word.
    const QString sshProbeEcho =
        sshProbe.section(QLatin1Char(' '), -1) + QLatin1Char('\n');
    check(forkmesh::control::vultrSshProbeReady(0, sshProbeEcho) &&
              !forkmesh::control::vultrSshProbeReady(
                  255, QStringLiteral("ssh: connect to host 203.0.113.10 port "
                                      "22: Connection refused")) &&
              !forkmesh::control::vultrSshProbeReady(
                  0, QStringLiteral("Debian GNU/Linux banner\n")) &&
              !forkmesh::control::vultrSshProbeReady(255, sshProbeEcho),
          "SSH counts as ready only when the knock exits 0 and its marker "
          "comes back, so a banner-only session never opens the upload");

    // --- Agent CLIs on a fresh mirror (adhoc #418) -------------------------
    forkmesh::control::AgentCliCredentials agentCredentials;
    check(forkmesh::control::agentCliCredentialsAreEmpty(agentCredentials) &&
              forkmesh::control::describeAgentCliCredentials(agentCredentials)
                  .isEmpty(),
          "a device with no agent login has nothing to copy to a new mirror");

    agentCredentials.claudeCredentials =
        QByteArrayLiteral("{\"claudeAiOauth\":{\"accessToken\":\"tok-claude\"}}");
    agentCredentials.env.insert(QStringLiteral("OPENAI_API_KEY"),
                                QStringLiteral("sk-it's-quoted"));
    agentCredentials.env.insert(QStringLiteral("bad name"),
                                QStringLiteral("dropped"));
    agentCredentials.env.insert(QStringLiteral("CONTROL_CHARS"),
                                QStringLiteral("line\nbreak"));
    check(!forkmesh::control::agentCliCredentialsAreEmpty(agentCredentials) &&
              forkmesh::control::describeAgentCliCredentials(
                  agentCredentials) ==
                  QStringLiteral("Claude Code login, OPENAI_API_KEY"),
          "the copy summary names the login and the usable keys only");

    const QByteArray envFile =
        forkmesh::control::agentCliEnvFileContents(agentCredentials.env);
    check(envFile ==
              QByteArrayLiteral("export OPENAI_API_KEY='sk-it'\\''s-quoted'\n"),
          "the sourced env file single-quotes values and drops unusable names");

    QString agentPayloadError;
    const QByteArray agentPayload =
        forkmesh::control::buildAgentCliBootstrapPayload(agentCredentials,
                                                         &agentPayloadError);
    check(agentPayloadError.isEmpty() && !agentPayload.isEmpty() &&
              !agentPayload.contains(QByteArrayLiteral("tok-claude")) &&
              !agentPayload.contains(QByteArrayLiteral("sk-it")) &&
              agentPayload.startsWith(QByteArrayLiteral("claude ")) &&
              agentPayload.contains(QByteArrayLiteral("\nenv ")) &&
              !agentPayload.contains(QByteArrayLiteral("codex ")),
          "the stdin payload carries one base64 line per present section");
    QByteArray decodedClaude;
    QByteArray decodedEnv;
    for (const QByteArray &line : agentPayload.split('\n')) {
        const int space = line.indexOf(' ');
        if (space <= 0)
            continue;
        const QByteArray decoded =
            QByteArray::fromBase64(line.mid(space + 1));
        if (line.startsWith(QByteArrayLiteral("claude ")))
            decodedClaude = decoded;
        else if (line.startsWith(QByteArrayLiteral("env ")))
            decodedEnv = decoded;
    }
    check(decodedClaude == agentCredentials.claudeCredentials.trimmed() &&
              decodedEnv == envFile,
          "each payload section round-trips to the exact file the mirror writes");

    forkmesh::control::AgentCliCredentials malformedCredentials;
    malformedCredentials.codexAuth = QByteArrayLiteral("not json");
    QString malformedError;
    check(forkmesh::control::buildAgentCliBootstrapPayload(
              malformedCredentials, &malformedError).isEmpty() &&
              !malformedError.isEmpty(),
          "a login file that is not a credential document is never sent");
    QString emptyError;
    check(forkmesh::control::buildAgentCliBootstrapPayload(
              forkmesh::control::AgentCliCredentials{}, &emptyError)
                  .isEmpty() &&
              !emptyError.isEmpty(),
          "an empty bundle is refused rather than sent as a blank payload");

    const QString agentInstallOnly =
        forkmesh::control::agentCliBootstrapRemoteCommand(false);
    const QString agentWithLogins =
        forkmesh::control::agentCliBootstrapRemoteCommand(true);
    check(agentInstallOnly.contains(
              QStringLiteral("https://claude.ai/install.sh")) &&
              agentInstallOnly.contains(
                  QStringLiteral("https://chatgpt.com/codex/install.sh")) &&
              !agentInstallOnly.contains(QStringLiteral("credentials.json")) &&
              !agentInstallOnly.contains(QStringLiteral("cat >")),
          "the install-only command reads no stdin and writes no login file");
    check(agentWithLogins.contains(QStringLiteral("cat > \"$tmp\"")) &&
              agentWithLogins.contains(
                  QStringLiteral("$agent_home/.claude/.credentials.json")) &&
              agentWithLogins.contains(
                  QStringLiteral("$agent_home/.codex/auth.json")) &&
              agentWithLogins.contains(
                  QStringLiteral("$agent_home/.forkmesh/agent-env")) &&
              agentWithLogins.contains(
                  QStringLiteral("id forkmesh-node")) &&
              agentWithLogins.contains(
                  QStringLiteral("/usr/local/bin/$program")) &&
              agentWithLogins.contains(QStringLiteral("chmod 600")) &&
              agentWithLogins.contains(QStringLiteral("umask 077")),
          "the credential command writes each login file with private modes");
    check(agentWithLogins.startsWith(QStringLiteral("sh -lc '")) &&
              agentWithLogins.endsWith(QLatin1Char('\'')) &&
              agentWithLogins.count(QLatin1Char('\'')) == 2,
          "the remote command is one shell word, so no section can escape it");
    check(agentInstallOnly.contains(QStringLiteral("\"$target.new\"")) &&
              agentInstallOnly.contains(
                  QStringLiteral("mv -f \"$target.new\" \"$target\"")),
          "the service CLI is swapped in by rename, not rewritten in place");

    // --- Host updates keep the agent CLIs fresh (2026-09-24) ---------------
    const QString refresh = forkmesh::control::agentCliServiceRefreshScript();
    check(!refresh.contains(QLatin1Char('\'')) &&
              refresh.contains(QStringLiteral("test -x /usr/local/bin/$program")) &&
              refresh.contains(QStringLiteral("id forkmesh-node")) &&
              refresh.contains(QStringLiteral("/usr/local/bin/$program.new")) &&
              refresh.contains(QStringLiteral(
                  "mv -f \"/usr/local/bin/$program.new\" \"/usr/local/bin/$program\"")),
          "the refresh only swaps CLIs the service already has, by rename");
    const auto runShell = [](const QString &command) {
        QProcess shell;
        shell.start(QStringLiteral("bash"), {QStringLiteral("-c"), command});
        shell.waitForFinished(20000);
        return shell.exitCode();
    };
    {
        QProcess syntax;
        syntax.start(QStringLiteral("bash"),
                     {QStringLiteral("-n"), QStringLiteral("-c"),
                      forkmesh::control::withAgentCliServiceRefresh(
                          QStringLiteral("set -eu; echo updated"))});
        syntax.waitForFinished(20000);
        check(syntax.exitStatus() == QProcess::NormalExit && syntax.exitCode() == 0,
              "the update-then-refresh command is valid shell");
    }
#ifdef Q_OS_UNIX
    if (::geteuid() != 0) {
        // Not root, so the refresh is a no-op and the update's status passes
        // through; a failed update never reaches the refresh at all.
        check(runShell(forkmesh::control::withAgentCliServiceRefresh(
                  QStringLiteral("set -e; true"))) == 0 &&
                  runShell(forkmesh::control::withAgentCliServiceRefresh(
                      QStringLiteral("set -e; exit 7"))) == 7 &&
                  runShell(forkmesh::control::withAgentCliServiceRefresh(
                      QStringLiteral("false"))) == 1,
              "the host update's exit status is the command's exit status");
    }
#endif

    // --- Cloudflare DNS for a fresh Vultr mirror (adhoc #331) --------------
    QMap<QString, QString> zoneVariables;
    zoneVariables.insert(QStringLiteral("cloudflare_zone"),
                         QStringLiteral("Example.Com"));
    check(forkmesh::control::cloudflareZoneNameFromVariables(zoneVariables) ==
                  QStringLiteral("Example.Com") &&
              forkmesh::control::cloudflareZoneNameFromVariables({}).isEmpty(),
          "the stored CLOUDFLARE_ZONE device variable is resolved");

    check(forkmesh::control::vultrMirrorDnsHostname(
              QStringLiteral("vultr-mirror-1"),
              QStringLiteral(" Example.COM. ")) ==
                  QStringLiteral("vultr-mirror-1.example.com") &&
              forkmesh::control::vultrMirrorDnsHostname(
                  QStringLiteral("vultr-mirror-1"), QStringLiteral("example"))
                  .isEmpty() &&
              forkmesh::control::vultrMirrorDnsHostname(
                  QStringLiteral("bad node"), QStringLiteral("example.com"))
                  .isEmpty() &&
              forkmesh::control::vultrMirrorDnsHostname(
                  QString(), QStringLiteral("example.com")).isEmpty(),
          "the mirror DNS hostname is <node>.<zone> and rejects bad halves");

    const QJsonObject dnsPayload =
        forkmesh::control::vultrMirrorDnsRecordPayload(
            QStringLiteral("vultr-mirror-1.example.com"),
            QStringLiteral("203.0.113.99"));
    check(dnsPayload.value(QStringLiteral("type")).toString() ==
                  QStringLiteral("A") &&
              dnsPayload.value(QStringLiteral("name")).toString() ==
                  QStringLiteral("vultr-mirror-1.example.com") &&
              dnsPayload.value(QStringLiteral("content")).toString() ==
                  QStringLiteral("203.0.113.99") &&
              dnsPayload.value(QStringLiteral("proxied")).toBool() == false &&
              dnsPayload.value(QStringLiteral("ttl")).toInt() == 1,
          "the mirror DNS record is a DNS-only A answer at automatic TTL");
    check(forkmesh::control::vultrMirrorDnsRecordPayload(
              QStringLiteral("vultr-mirror-1.example.com"),
              QStringLiteral("0.0.0.0")).isEmpty() &&
              forkmesh::control::vultrMirrorDnsRecordPayload(
                  QStringLiteral("vultr-mirror-1.example.com"),
                  QStringLiteral("203.0.113.999")).isEmpty() &&
              forkmesh::control::vultrMirrorDnsRecordPayload(
                  QStringLiteral("vultr-mirror-1"),
                  QStringLiteral("203.0.113.99")).isEmpty(),
          "malformed addresses or single-label names never reach Cloudflare");

    const QJsonArray zones{
        QJsonObject{{QStringLiteral("id"), QStringLiteral("zone-other")},
                    {QStringLiteral("name"), QStringLiteral("other.test")}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("zone-example")},
                    {QStringLiteral("name"), QStringLiteral("example.com")}},
    };
    QJsonArray ambiguousZones = zones;
    ambiguousZones.append(
        QJsonObject{{QStringLiteral("id"), QStringLiteral("zone-duplicate")},
                    {QStringLiteral("name"), QStringLiteral("example.com")}});
    check(forkmesh::control::cloudflareZoneId(
              zones, QStringLiteral("Example.com.")) ==
                  QStringLiteral("zone-example") &&
              forkmesh::control::cloudflareZoneId(
                  zones, QStringLiteral("missing.test")).isEmpty() &&
              forkmesh::control::cloudflareZoneId(
                  ambiguousZones, QStringLiteral("example.com")).isEmpty(),
          "the zone id resolves only on an unambiguous exact name match");

    const QJsonArray dnsRecords{
        QJsonObject{
            {QStringLiteral("id"), QStringLiteral("record-cname")},
            {QStringLiteral("name"),
             QStringLiteral("vultr-mirror-1.example.com")},
            {QStringLiteral("type"), QStringLiteral("CNAME")}},
        QJsonObject{
            {QStringLiteral("id"), QStringLiteral("record-a")},
            {QStringLiteral("name"),
             QStringLiteral("vultr-mirror-1.example.com")},
            {QStringLiteral("type"), QStringLiteral("A")}},
    };
    check(forkmesh::control::cloudflareDnsRecordId(
              dnsRecords, QStringLiteral("vultr-mirror-1.example.com"),
              QStringLiteral("A")) == QStringLiteral("record-a") &&
              forkmesh::control::cloudflareDnsRecordId(
                  dnsRecords, QStringLiteral("vultr-mirror-2.example.com"),
                  QStringLiteral("A")).isEmpty(),
          "an existing A record is reused so repeat deploys update in place");

    // --- Cloudflare API token check and rotation (adhoc #108) --------------
    const QList<forkmesh::control::CloudflareTokenRequirement> tokenRequirements =
        forkmesh::control::cloudflareTokenRequirements();
    QSet<QString> tokenKeys;
    bool tokenRequirementsWellFormed = !tokenRequirements.isEmpty();
    int requiredTokenPermissions = 0;
    for (const forkmesh::control::CloudflareTokenRequirement &requirement :
         tokenRequirements) {
        if (requirement.key.isEmpty() || requirement.label.isEmpty() ||
            requirement.purpose.isEmpty() ||
            requirement.groupNames.isEmpty() ||
            tokenKeys.contains(requirement.key) ||
            (requirement.scope != QStringLiteral("account") &&
             requirement.scope != QStringLiteral("zone") &&
             requirement.scope != QStringLiteral("user"))) {
            tokenRequirementsWellFormed = false;
        }
        tokenKeys.insert(requirement.key);
        if (requirement.required)
            ++requiredTokenPermissions;
    }
    check(tokenRequirementsWellFormed && requiredTokenPermissions >= 6 &&
              tokenKeys.contains(QStringLiteral("workers_scripts")) &&
              tokenKeys.contains(QStringLiteral("d1")) &&
              tokenKeys.contains(QStringLiteral("dns")),
          "the API token tab describes every deploy permission exactly once");

    const auto tokenRequirement = [&tokenRequirements](const char *key) {
        for (const forkmesh::control::CloudflareTokenRequirement &requirement :
             tokenRequirements) {
            if (requirement.key == QLatin1String(key))
                return requirement;
        }
        return forkmesh::control::CloudflareTokenRequirement{};
    };
    check(forkmesh::control::cloudflareTokenProbePath(
              tokenRequirement("workers_scripts"),
              QStringLiteral("acct1"), QStringLiteral("zone1")) ==
                  QStringLiteral("/accounts/acct1/workers/scripts") &&
              forkmesh::control::cloudflareTokenProbePath(
                  tokenRequirement("workers_scripts"), QString(),
                  QStringLiteral("zone1")).isEmpty() &&
              forkmesh::control::cloudflareTokenProbePath(
                  tokenRequirement("dns"), QStringLiteral("acct1"),
                  QStringLiteral("../../user/tokens")).isEmpty() &&
              forkmesh::control::cloudflareTokenProbePath(
                  tokenRequirement("api_tokens_write"),
                  QStringLiteral("acct1"), QStringLiteral("zone1")).isEmpty(),
          "capability probes only run against well-formed account/zone ids");

    check(forkmesh::control::isPlausibleCloudflareApiToken(
              QStringLiteral("token-value-for-checks_1")) &&
              !forkmesh::control::isPlausibleCloudflareApiToken(
                  QStringLiteral("short")) &&
              !forkmesh::control::isPlausibleCloudflareApiToken(
                  QStringLiteral("token-value with a space")) &&
              !forkmesh::control::isPlausibleCloudflareApiToken(
                  QStringLiteral("token-value\nsecond-line-token")),
          "a pasted credential file or short string never reaches Cloudflare");

    const QJsonObject tokenDetail{
        {QStringLiteral("result"),
         QJsonObject{
             {QStringLiteral("id"), QStringLiteral("tokenid1")},
             {QStringLiteral("status"), QStringLiteral("active")},
             {QStringLiteral("policies"),
              QJsonArray{
                  QJsonObject{
                      {QStringLiteral("effect"), QStringLiteral("allow")},
                      {QStringLiteral("resources"),
                       QJsonObject{
                           {QStringLiteral("com.cloudflare.api.account.acct1"),
                            QStringLiteral("*")}}},
                      {QStringLiteral("permission_groups"),
                       QJsonArray{
                           QJsonObject{
                               {QStringLiteral("id"), QStringLiteral("pg1")},
                               {QStringLiteral("name"),
                                QStringLiteral("Workers Scripts Write")}},
                           QJsonObject{
                               {QStringLiteral("id"), QStringLiteral("pg2")},
                               {QStringLiteral("name"),
                                QStringLiteral("Account Settings Read")}}}}},
                  QJsonObject{
                      {QStringLiteral("effect"), QStringLiteral("allow")},
                      {QStringLiteral("resources"),
                       QJsonObject{
                           {QStringLiteral("com.cloudflare.api.user.user1"),
                            QStringLiteral("*")}}},
                      {QStringLiteral("permission_groups"),
                       QJsonArray{QJsonObject{
                           {QStringLiteral("id"), QStringLiteral("pg3")},
                           {QStringLiteral("name"),
                            QStringLiteral("API Tokens Write")}}}}},
                  QJsonObject{
                      {QStringLiteral("effect"), QStringLiteral("deny")},
                      {QStringLiteral("resources"),
                       QJsonObject{
                           {QStringLiteral("com.cloudflare.api.account.zone.zone1"),
                            QStringLiteral("*")}}},
                      {QStringLiteral("permission_groups"),
                       QJsonArray{QJsonObject{
                           {QStringLiteral("id"), QStringLiteral("pg4")},
                           {QStringLiteral("name"), QStringLiteral("DNS Write")}}}}},
              }}}}};
    const QStringList grantedGroups =
        forkmesh::control::cloudflareTokenPermissionGroupNames(tokenDetail);
    check(grantedGroups.contains(QStringLiteral("Workers Scripts Write")) &&
              grantedGroups.contains(QStringLiteral("API Tokens Write")) &&
              !grantedGroups.contains(QStringLiteral("DNS Write")),
          "a denied permission group is never reported as granted");
    check(forkmesh::control::cloudflareTokenVerifyStatus(tokenDetail) ==
              QStringLiteral("active") &&
              forkmesh::control::cloudflareTokenVerifyId(tokenDetail) ==
                  QStringLiteral("tokenid1") &&
              forkmesh::control::cloudflareTokenVerifyId(
                  QJsonObject{{QStringLiteral("result"),
                               QJsonObject{{QStringLiteral("id"),
                                            QStringLiteral("../evil")}}}})
                  .isEmpty(),
          "token verification reads the status and only a safe token id");
    check(forkmesh::control::cloudflareTokenGrantsRequirement(
              tokenRequirement("workers_scripts"), grantedGroups) &&
              !forkmesh::control::cloudflareTokenGrantsRequirement(
                  tokenRequirement("dns"), grantedGroups) &&
              forkmesh::control::cloudflareTokenGrantsRequirement(
                  tokenRequirement("zone"),
                  {QStringLiteral("Zone Write")}),
          "a broader group satisfies a read requirement, a missing one does not");
    check(forkmesh::control::cloudflareTokenAccountIds(tokenDetail) ==
              QStringList{QStringLiteral("acct1")} &&
              forkmesh::control::cloudflareTokenUserResourceKey(tokenDetail) ==
                  QStringLiteral("com.cloudflare.api.user.user1"),
          "a token's own policy reveals its account and user resource");

    QJsonArray permissionGroupCatalog;
    for (const forkmesh::control::CloudflareTokenRequirement &requirement :
         tokenRequirements) {
        const QString scope =
            requirement.scope == QStringLiteral("account")
                ? QStringLiteral("com.cloudflare.api.account")
                : requirement.scope == QStringLiteral("zone")
                      ? QStringLiteral("com.cloudflare.api.account.zone")
                      : QStringLiteral("com.cloudflare.api.user");
        permissionGroupCatalog.append(QJsonObject{
            // Cloudflare permission-group ids are hex, and the resolver refuses
            // anything else so a malformed catalog can never reach a policy.
            {QStringLiteral("id"),
             QStringLiteral("pgid") +
                 QString::number(permissionGroupCatalog.size())},
            {QStringLiteral("name"), requirement.groupNames.constFirst()},
            {QStringLiteral("scopes"), QJsonArray{scope}},
        });
    }
    // A same-named group at the wrong scope must not be borrowed.
    permissionGroupCatalog.append(QJsonObject{
        {QStringLiteral("id"), QStringLiteral("pgwrongscope")},
        {QStringLiteral("name"), QStringLiteral("DNS Write")},
        {QStringLiteral("scopes"),
         QJsonArray{QStringLiteral("com.cloudflare.api.user")}},
    });
    QString tokenPayloadError;
    const QJsonObject tokenPayload =
        forkmesh::control::cloudflareTokenCreatePayload(
            QStringLiteral("ForkMesh mirror9 20260731-101500"),
            QStringLiteral("acct1"), QStringLiteral("zone1"),
            QStringLiteral("com.cloudflare.api.user.user1"),
            permissionGroupCatalog, &tokenPayloadError);
    const QJsonArray tokenPolicies =
        tokenPayload.value(QStringLiteral("policies")).toArray();
    QSet<QString> tokenPolicyResources;
    QSet<QString> tokenPolicyGroups;
    for (const QJsonValue &policy : tokenPolicies) {
        const QJsonObject object = policy.toObject();
        check(object.value(QStringLiteral("effect")).toString() ==
                  QStringLiteral("allow"),
              "a minted token only ever carries allow policies");
        const QJsonObject resources =
            object.value(QStringLiteral("resources")).toObject();
        for (auto it = resources.constBegin(); it != resources.constEnd(); ++it)
            tokenPolicyResources.insert(it.key());
        const QJsonArray groups =
            object.value(QStringLiteral("permission_groups")).toArray();
        for (const QJsonValue &group : groups) {
            tokenPolicyGroups.insert(
                group.toObject().value(QStringLiteral("name")).toString());
        }
    }
    check(tokenPayloadError.isEmpty() &&
              tokenPayload.value(QStringLiteral("name")).toString() ==
                  QStringLiteral("ForkMesh mirror9 20260731-101500") &&
              tokenPolicies.size() == 3 &&
              tokenPolicyResources.contains(
                  QStringLiteral("com.cloudflare.api.account.acct1")) &&
              tokenPolicyResources.contains(
                  QStringLiteral("com.cloudflare.api.account.zone.zone1")) &&
              tokenPolicyResources.contains(
                  QStringLiteral("com.cloudflare.api.user.user1")) &&
              tokenPolicyGroups.contains(QStringLiteral("Workers Scripts Write")) &&
              tokenPolicyGroups.contains(QStringLiteral("DNS Write")) &&
              tokenPolicyGroups.contains(QStringLiteral("API Tokens Write")),
          "a minted token is scoped to one account, one zone and this user");

    QJsonArray catalogMissingD1;
    for (const QJsonValue &group : permissionGroupCatalog) {
        if (group.toObject().value(QStringLiteral("name")).toString() !=
            QStringLiteral("D1 Write")) {
            catalogMissingD1.append(group);
        }
    }
    QString missingGroupError;
    check(forkmesh::control::cloudflareTokenCreatePayload(
              QStringLiteral("ForkMesh mirror9 20260731-101500"),
              QStringLiteral("acct1"), QStringLiteral("zone1"),
              QStringLiteral("com.cloudflare.api.user.user1"),
              catalogMissingD1, &missingGroupError).isEmpty() &&
              missingGroupError.contains(QStringLiteral("D1")),
          "a catalog missing a required group fails closed instead of minting");
    QString missingZoneError;
    check(forkmesh::control::cloudflareTokenCreatePayload(
              QStringLiteral("ForkMesh mirror9 20260731-101500"),
              QStringLiteral("acct1"), QString(),
              QStringLiteral("com.cloudflare.api.user.user1"),
              permissionGroupCatalog, &missingZoneError).isEmpty() &&
              !missingZoneError.isEmpty(),
          "minting refuses to guess a zone for the DNS permission");
    QString badNameError;
    check(forkmesh::control::cloudflareTokenCreatePayload(
              QStringLiteral("ForkMesh\nmirror9"), QStringLiteral("acct1"),
              QStringLiteral("zone1"),
              QStringLiteral("com.cloudflare.api.user.user1"),
              permissionGroupCatalog, &badNameError).isEmpty() &&
              !badNameError.isEmpty(),
          "a token name carrying a newline is rejected");
    // Optional user-scoped groups are dropped when the user resource is unknown,
    // so a token that cannot read itself can still mint a deploy token.
    const QJsonObject withoutUserPolicy =
        forkmesh::control::cloudflareTokenCreatePayload(
            QStringLiteral("ForkMesh mirror9 20260731-101500"),
            QStringLiteral("acct1"), QStringLiteral("zone1"), QString(),
            permissionGroupCatalog, nullptr);
    check(withoutUserPolicy.value(QStringLiteral("policies")).toArray().size() ==
              2,
          "an unknown user resource drops only the optional API Tokens policy");

    const QString createdToken =
        forkmesh::control::cloudflareCreatedTokenValue(
            QJsonObject{{QStringLiteral("result"),
                         QJsonObject{{QStringLiteral("value"),
                                      QStringLiteral(
                                          "brand-new-token-value_9")}}}});
    check(createdToken == QStringLiteral("brand-new-token-value_9") &&
              forkmesh::control::cloudflareCreatedTokenValue(
                  QJsonObject{{QStringLiteral("result"),
                               QJsonObject{{QStringLiteral("value"),
                                            QStringLiteral("nope")}}}})
                  .isEmpty(),
          "only a usable token value is adopted from a create response");
    check(forkmesh::control::maskedTokenSuffix(
              QStringLiteral("brand-new-token-value_9"))
                  .endsWith(QStringLiteral("ue_9")) &&
              !forkmesh::control::maskedTokenSuffix(
                   QStringLiteral("brand-new-token-value_9"))
                   .contains(QStringLiteral("brand")),
          "a token is only ever named by its last four characters");

    const QString envBefore = QStringLiteral(
        "# production\r\n"
        "ADMIN_PATH=/secret-admin\r\n"
        "CLOUDFLARE_API_TOKEN=old-token-value\r\n"
        "#CLOUDFLARE_API_TOKEN=commented-example\r\n"
        "OTHER=keepme\r\n"
        "CLOUDFLARE_API_TOKEN = stale-duplicate\r\n");
    const QString envAfter = forkmesh::control::updatedEnvAssignment(
        envBefore, QStringLiteral("CLOUDFLARE_API_TOKEN"),
        QStringLiteral("rotated-token-value"));
    check(envAfter.contains(
              QStringLiteral("CLOUDFLARE_API_TOKEN=rotated-token-value\r\n")) &&
              !envAfter.contains(QStringLiteral("old-token-value")) &&
              !envAfter.contains(QStringLiteral("stale-duplicate")) &&
              envAfter.contains(
                  QStringLiteral("#CLOUDFLARE_API_TOKEN=commented-example")) &&
              envAfter.contains(QStringLiteral("ADMIN_PATH=/secret-admin")) &&
              envAfter.contains(QStringLiteral("OTHER=keepme")) &&
              envAfter.count(QStringLiteral("CLOUDFLARE_API_TOKEN=rotated")) == 1,
          ".env.production rotation replaces the token and keeps every secret");
    const QString envAppended = forkmesh::control::updatedEnvAssignment(
        QStringLiteral("ADMIN_PATH=/secret-admin\n\n"),
        QStringLiteral("CLOUDFLARE_ACCOUNT_ID"), QStringLiteral("acct1"));
    check(envAppended ==
              QStringLiteral("ADMIN_PATH=/secret-admin\n"
                             "CLOUDFLARE_ACCOUNT_ID=acct1\n") &&
              forkmesh::control::updatedEnvAssignment(
                  QStringLiteral("ADMIN_PATH=/secret-admin\n"),
                  QStringLiteral("CLOUDFLARE_API_TOKEN"),
                  QStringLiteral("line\ninjected")) ==
                  QStringLiteral("ADMIN_PATH=/secret-admin\n"),
          "a missing assignment is appended and a multi-line value is refused");

    QTemporaryDir hostSettingsDir;
    const QString hostSettingsPath =
        hostSettingsDir.filePath(QStringLiteral("controller.ini"));
    QSettings hostSettings(hostSettingsPath, QSettings::IniFormat);
    const QString legacyPassword =
        QStringLiteral("legacy-admin-password-must-leave-settings");
    const QJsonArray legacyHosts{
        QJsonObject{
            {QStringLiteral("name"), QStringLiteral("mirror2")},
            {QStringLiteral("ip"), QStringLiteral("mirror2.example.test")},
            {QStringLiteral("user"), QStringLiteral("forkmesh")},
            {QStringLiteral("pass"), legacyPassword},
            {QStringLiteral("status"), QStringLiteral("installed")},
        }};
    hostSettings.setValue(
        QStringLiteral("hosts/list"),
        QString::fromUtf8(
            QJsonDocument(legacyHosts).toJson(QJsonDocument::Compact)));
    hostSettings.sync();
    QHash<QString, QString> sessionHostPasswords;
    QJsonArray migratedHosts = forkmesh::control::loadSavedHosts(
        hostSettings, QStringLiteral("hosts/list"),
        &sessionHostPasswords);
    hostSettings.sync();
    QFile exportedSettings(hostSettingsPath);
    check(exportedSettings.open(QIODevice::ReadOnly),
          "migrated host settings file is readable");
    const QByteArray persistedAfterMigration = exportedSettings.readAll();
    exportedSettings.close();
    const QJsonObject migratedHost = migratedHosts.at(0).toObject();
    const QString migratedCredentialKey =
        forkmesh::control::savedHostCredentialKey(
            QStringLiteral("mirror2"),
            QStringLiteral("mirror2.example.test"),
            QStringLiteral("forkmesh"));
    check(!migratedHost.contains(QStringLiteral("pass")) &&
              sessionHostPasswords.value(migratedCredentialKey) ==
                  legacyPassword &&
              !persistedAfterMigration.contains(
                  legacyPassword.toUtf8()) &&
              !persistedAfterMigration.contains("\"pass\""),
          "legacy plaintext host passwords migrate to memory and are deleted from QSettings");
    check(forkmesh::control::savedHostMayBeActive(migratedHost) &&
              forkmesh::control::savedHostMayBeActive(
                  QJsonObject{{QStringLiteral("status"),
                               QStringLiteral("online")}}) &&
              !forkmesh::control::savedHostMayBeActive(
                  QJsonObject{{QStringLiteral("status"),
                               QStringLiteral("install failed")}}) &&
              !forkmesh::control::savedHostMayBeActive(
                  QJsonObject{{QStringLiteral("status"),
                               QStringLiteral("uninstalling")}}),
          "automated fleet work targets installed hosts but skips transitional and failed records");

    // --- Stale install labels corrected by live mirror health --------------
    {
        const qint64 now = 1'800'000'000'000LL;
        const auto savedHost = [](const QString &name, const QString &status,
                                  qint64 statusChangedAt) {
            QJsonObject host{
                {QStringLiteral("name"), name},
                {QStringLiteral("ip"), QStringLiteral("198.51.100.35")},
                {QStringLiteral("status"), status},
                {QStringLiteral("provider"), QStringLiteral("Vultr")},
            };
            if (statusChangedAt > 0)
                host.insert(QStringLiteral("statusChangedAt"),
                            double(statusChangedAt));
            return host;
        };
        const auto liveMirror = [](const QString &node) {
            return QJsonObject{
                {QStringLiteral("node"), node},
                {QStringLiteral("status"), QStringLiteral("online")},
                {QStringLiteral("integrity"), QStringLiteral("ok")},
                {QStringLiteral("endpoint"),
                 QStringLiteral("https://%1.forkmesh.com").arg(node)},
                {QStringLiteral("endpointHealthy"), true},
                {QStringLiteral("endpointFresh"), true},
            };
        };
        check(forkmesh::control::savedHostInstallStatusMayBeStale(
                  savedHost(QStringLiteral("a"),
                            QStringLiteral("Install Failed"), now),
                  now) &&
                  forkmesh::control::savedHostInstallStatusMayBeStale(
                      savedHost(QStringLiteral("a"),
                                QStringLiteral("installing"), 0),
                      now) &&
                  forkmesh::control::savedHostInstallStatusMayBeStale(
                      savedHost(QStringLiteral("a"),
                                QStringLiteral("installing"),
                                now - forkmesh::control::
                                          kSavedHostInstallingStaleMs -
                                    1),
                      now) &&
                  !forkmesh::control::savedHostInstallStatusMayBeStale(
                      savedHost(QStringLiteral("a"),
                                QStringLiteral("installing"),
                                now - 10 * 60 * 1000),
                      now) &&
                  !forkmesh::control::savedHostInstallStatusMayBeStale(
                      savedHost(QStringLiteral("a"),
                                QStringLiteral("installed"), 0),
                      now) &&
                  !forkmesh::control::savedHostInstallStatusMayBeStale(
                      savedHost(QStringLiteral("a"),
                                QStringLiteral("uninstalled"), 0),
                      now),
              "only install failed and abandoned installing labels are stale "
              "candidates; a running install keeps its label");

        // Mirror nodes sync verdicts (MirrorSyncVerdict.h): a cached mirror
        // observation taken against older source refs is re-requested, never
        // reported as the mirror being out of sync.
        {
            namespace sync = forkmesh::mirrorsync;
            const QString sourceNow(64, QLatin1Char('a'));
            const QString sourceBefore(64, QLatin1Char('b'));
            QJsonObject observation{
                {QStringLiteral("commit"),
                 QStringLiteral("1111111111111111111111111111111111111111")},
            };
            check(sync::observationMatchesSourceRefs(observation, sourceNow),
                  "an unstamped mirror observation keeps the direct comparison");
            observation.insert(sync::observedSourceRefsKey(), sourceBefore);
            check(!sync::observationMatchesSourceRefs(observation, sourceNow),
                  "an observation taken against older source refs is not "
                  "compared with the current source");
            observation.insert(sync::observedSourceRefsKey(),
                               sourceNow.toUpper());
            check(sync::observationMatchesSourceRefs(observation, sourceNow) &&
                      sync::observationMatchesSourceRefs(observation, QString()),
                  "an observation stamped with the current source refs is "
                  "compared; an unknown source digest never hides a row");
            check(sync::refreshIntervalMs(true, true) ==
                          sync::kStaleObservationRefreshMs &&
                      sync::refreshIntervalMs(false, true) ==
                          sync::kBehindRefreshMs &&
                      sync::refreshIntervalMs(false, false) ==
                          sync::kSettledRefreshMs &&
                      sync::kStaleObservationRefreshMs < sync::kBehindRefreshMs &&
                      sync::kBehindRefreshMs < sync::kSettledRefreshMs,
                  "stale observations re-probe first, a behind mirror within "
                  "a minute, a settled fleet on the five-minute cadence");

            const QJsonObject liveDirect{
                {QStringLiteral("node"), QStringLiteral("mirror35")},
                {QStringLiteral("endpointHealthy"), true},
            };
            const QJsonObject deadDirect{
                {QStringLiteral("node"), QStringLiteral("mirror35")},
                {QStringLiteral("endpointHealthy"), false},
            };
            const QJsonObject sshReading{
                {QStringLiteral("node"), QStringLiteral("mirror35")},
                {QStringLiteral("sshHealthy"), false},
            };
            const QJsonObject sshRead{
                {QStringLiteral("node"), QStringLiteral("mirror35")},
                {QStringLiteral("sshHealthy"), true},
            };
            check(!sync::sshSnapshotProvidesRepositoryState(sshReading, liveDirect),
                  "an in-flight or failed SSH read never overrides a live direct "
                  "probe's repository state");
            check(sync::sshSnapshotProvidesRepositoryState(sshRead, liveDirect) &&
                      sync::sshSnapshotProvidesRepositoryState(sshReading,
                                                               deadDirect),
                  "a successful SSH read, or the only view of a node, still "
                  "supplies its repository state");
            check(sync::isRepositoryStateField(QStringLiteral("commit")) &&
                      sync::isRepositoryStateField(QStringLiteral("branchHeads")) &&
                      sync::isRepositoryStateField(sync::observedSourceRefsKey()) &&
                      !sync::isRepositoryStateField(QStringLiteral("cpuPercent")) &&
                      !sync::isRepositoryStateField(QStringLiteral("version")),
                  "repository state fields are separated from machine facts");

            const auto mirrorRow = [](const char *node, const char *status,
                                      bool cloneAvailable) {
                return QJsonObject{
                    {QStringLiteral("node"), QString::fromLatin1(node)},
                    {QStringLiteral("status"), QString::fromLatin1(status)},
                    {QStringLiteral("cloneAvailable"), cloneAvailable},
                };
            };
            check(!sync::clonesRejected(QJsonObject{
                      {QStringLiteral("mirrors"),
                       QJsonArray{mirrorRow("mirror35", "online", true),
                                  mirrorRow("mirror36", "online", false)}}}),
                  "one admitted mirror means clones of the repository are served");
            check(sync::clonesRejected(QJsonObject{
                      {QStringLiteral("mirrors"),
                       QJsonArray{mirrorRow("mirror35", "online", false),
                                  mirrorRow("mirror17", "offline", false)}}}),
                  "online mirrors with none admitted means clones are rejected");
            check(!sync::clonesRejected(QJsonObject{
                      {QStringLiteral("mirrors"),
                       QJsonArray{mirrorRow("mirror17", "offline", false)}}}) &&
                      !sync::clonesRejected(QJsonObject{}),
                  "no online mirror or an unreadable payload never raises the "
                  "integrity alarm");
        }

        const QJsonArray fleet{
            savedHost(QStringLiteral("mirror35"),
                      QStringLiteral("install failed"), 0),
            savedHost(QStringLiteral("mirror36"),
                      QStringLiteral("installed"), 0),
            savedHost(QStringLiteral("mirror37"),
                      QStringLiteral("installing"), now - 60 * 1000),
            savedHost(QStringLiteral("mirror38"),
                      QStringLiteral("install failed"), 0),
            savedHost(QStringLiteral("mirror39"),
                      QStringLiteral("uninstalled"), 0),
            savedHost(QStringLiteral("mirror40"),
                      QStringLiteral("install failed"), 0),
        };
        QJsonObject staleLease = liveMirror(QStringLiteral("mirror38"));
        staleLease.insert(QStringLiteral("endpointFresh"), false);
        QJsonObject brokenIntegrity = liveMirror(QStringLiteral("mirror40"));
        brokenIntegrity.insert(QStringLiteral("integrity"),
                               QStringLiteral("mismatch"));
        const QJsonArray catalog{
            liveMirror(QStringLiteral("MIRROR35")),
            liveMirror(QStringLiteral("mirror36")),
            liveMirror(QStringLiteral("mirror37")),
            staleLease,
            liveMirror(QStringLiteral("mirror39")),
            brokenIntegrity,
        };
        QStringList promoted{QStringLiteral("leftover")};
        const QJsonArray corrected =
            forkmesh::control::promoteSavedHostsProvenHealthy(
                fleet, catalog, now, &promoted);
        const QJsonObject mirror35 = corrected.at(0).toObject();
        check(promoted == QStringList{QStringLiteral("mirror35")} &&
                  mirror35.value(QStringLiteral("status")).toString() ==
                      QStringLiteral("installed") &&
                  mirror35.value(QStringLiteral("statusChangedAt"))
                          .toVariant()
                          .toLongLong() == now &&
                  mirror35.value(QStringLiteral("provider")).toString() ==
                      QStringLiteral("Vultr") &&
                  forkmesh::control::savedHostMayBeActive(mirror35),
              "a healthy catalog row promotes a stale install failed host to "
              "installed, keeping its other fields");
        check(corrected.at(1) == fleet.at(1) && corrected.at(2) == fleet.at(2) &&
                  corrected.at(3) == fleet.at(3) &&
                  corrected.at(4) == fleet.at(4) &&
                  corrected.at(5) == fleet.at(5),
              "installed, freshly installing, uninstalled, stale-lease and "
              "integrity-failed hosts are left untouched");

        const QJsonArray unchanged =
            forkmesh::control::promoteSavedHostsProvenHealthy(
                corrected, catalog, now + 1, &promoted);
        check(promoted.isEmpty() && unchanged == corrected,
              "a second pass over already-corrected hosts reports no change, "
              "so callers skip the settings write");
        forkmesh::control::promoteSavedHostsProvenHealthy(fleet, {}, now,
                                                          &promoted);
        check(promoted.isEmpty(),
              "no mirror evidence promotes nothing");
    }

    // --- SSH connection-failure classification (adhoc #335) ----------------
    check(forkmesh::control::sshConnectionFailureHint(
              255, QStringLiteral("ssh: connect to host 1.2.3.4 port 22: "
                                   "Connection timed out"))
                  .contains(QStringLiteral("firewall"), Qt::CaseInsensitive) &&
              forkmesh::control::sshConnectionFailureHint(
                  255, QStringLiteral("ssh: connect to host 1.2.3.4 port 22: "
                                       "Connection refused"))
                      .contains(QStringLiteral("firewall"),
                                Qt::CaseInsensitive) &&
              forkmesh::control::sshConnectionFailureHint(
                  255,
                  QStringLiteral(
                      "Host key verification failed."))
                      .contains(QStringLiteral("key"), Qt::CaseInsensitive),
          "a 255 ssh exit with a known connection-failure signature yields an "
          "actionable hint");
    // --- Non-routable target diagnosis + attempt summaries (adhoc #342) ----
    const QString cgnatHint = forkmesh::control::sshConnectionFailureHint(
        255,
        QStringLiteral("ssh: connect to host 100.68.82.54 port 22: "
                       "Connection timed out"),
        QStringLiteral("100.68.82.54"));
    check(cgnatHint.contains(QStringLiteral("100.64.0.0/10")) &&
              !cgnatHint.contains(QStringLiteral("security group")) &&
              forkmesh::control::sshConnectionFailureHint(
                  255,
                  QStringLiteral("ssh: connect to host 10.0.0.9 port 22: "
                                 "No route to host"),
                  QStringLiteral("10.0.0.9"))
                  .contains(QStringLiteral("10.0.0.0/8")) &&
              forkmesh::control::sshConnectionFailureHint(
                  255,
                  QStringLiteral("ssh: connect to host 1.2.3.4 port 22: "
                                 "Connection timed out"),
                  QStringLiteral("1.2.3.4"))
                  .contains(QStringLiteral("firewall"), Qt::CaseInsensitive),
          "a timeout against a non-routable address is diagnosed as the "
          "address, not as a firewall");
    check(forkmesh::control::nonRoutableAddressNote(
              QStringLiteral("192.168.1.10")).contains(
              QStringLiteral("192.168.0.0/16")) &&
              forkmesh::control::nonRoutableAddressNote(
                  QStringLiteral("172.16.4.1")).contains(
                  QStringLiteral("172.16.0.0/12")) &&
              forkmesh::control::nonRoutableAddressNote(
                  QStringLiteral("172.32.4.1")).isEmpty() &&
              forkmesh::control::nonRoutableAddressNote(
                  QStringLiteral("100.128.0.1")).isEmpty() &&
              forkmesh::control::nonRoutableAddressNote(
                  QStringLiteral("45.32.1.9")).isEmpty() &&
              forkmesh::control::nonRoutableAddressNote(
                  QStringLiteral("mirror5.example.test")).isEmpty(),
          "only genuinely non-routable IPv4 literals are flagged");
    check(forkmesh::control::sshFailureSummary(
              255,
              QStringLiteral("Warming up\nssh: connect to host 1.2.3.4 port "
                             "22: Connection timed out\n"))
                  .contains(QStringLiteral("Connection timed out")) &&
              forkmesh::control::sshFailureSummary(255, QStringLiteral(""))
                      == QStringLiteral("exit 255") &&
              forkmesh::control::sshFailureSummary(
                  1, QStringLiteral("x").repeated(400)).size() < 200,
          "each attempt gets a bounded one-line failure summary");

    check(forkmesh::control::sshConnectionFailureHint(
              1, QStringLiteral("ssh: connect to host 1.2.3.4 port 22: "
                                 "Connection timed out"))
                  .isEmpty() &&
              forkmesh::control::sshConnectionFailureHint(
                  255, QStringLiteral("some unrelated remote error"))
                      .isEmpty(),
          "a non-255 exit or unrecognized output yields no ssh hint");

    QJsonObject accidentallySecretHost = migratedHost;
    accidentallySecretHost.insert(QStringLiteral("sshPassword"),
                                  legacyPassword);
    forkmesh::control::saveSavedHosts(
        hostSettings, QStringLiteral("hosts/list"),
        QJsonArray{accidentallySecretHost});
    hostSettings.sync();
    exportedSettings.setFileName(hostSettingsPath);
    check(exportedSettings.open(QIODevice::ReadOnly),
          "defensively saved host settings file is readable");
    const QByteArray persistedAfterSave = exportedSettings.readAll();
    check(!persistedAfterSave.contains(legacyPassword.toUtf8()) &&
              !persistedAfterSave.contains("sshPassword"),
          "host settings writer strips password-like fields defensively");
    QJsonObject providerHost = migratedHost;
    providerHost.insert(QStringLiteral("provider"), QStringLiteral("Vultr"));
    providerHost.insert(QStringLiteral("planType"),
                        QStringLiteral("vc2-1c-1gb"));
    providerHost.insert(QStringLiteral("displayName"),
                        QStringLiteral("mirror6"));
    providerHost.insert(QStringLiteral("monthlyCost"), 6.0);
    forkmesh::control::saveSavedHosts(
        hostSettings, QStringLiteral("hosts/list"),
        QJsonArray{providerHost});
    const QJsonObject reloadedProviderHost =
        forkmesh::control::loadSavedHosts(
            hostSettings, QStringLiteral("hosts/list"), nullptr)
            .at(0)
            .toObject();
    check(reloadedProviderHost.value(QStringLiteral("provider")).toString() ==
                  QStringLiteral("Vultr") &&
              reloadedProviderHost.value(QStringLiteral("planType")).toString() ==
                  QStringLiteral("vc2-1c-1gb") &&
              reloadedProviderHost.value(QStringLiteral("displayName")).toString() ==
                  QStringLiteral("mirror6") &&
              reloadedProviderHost.value(QStringLiteral("monthlyCost")).toDouble() ==
                  6.0,
          "saved hosts preserve non-secret provider, plan, display name and cost metadata");

    auto invalidActions = actionsRequest;
    invalidActions.replaceVariables = false;
    check(!forkmesh::control::validateMirrorActionsConfigurationRequest(
               invalidActions).isEmpty(),
          "secret values cannot be silently ignored without replace approval");
    invalidActions = actionsRequest;
    invalidActions.variables.clear();
    invalidActions.variables.insert(QStringLiteral("BAD-NAME"),
                                    QStringLiteral("secret"));
    check(!forkmesh::control::validateMirrorActionsConfigurationRequest(
               invalidActions).isEmpty(),
          "unsafe Actions variable names fail closed");
    invalidActions = actionsRequest;
    invalidActions.host = QStringLiteral("-oProxyCommand=bad");
    check(forkmesh::control::buildMirrorActionsSshCommand(
              invalidActions, sshPassword, &actionsError).program.isEmpty(),
          "option-shaped SSH host input fails closed");
    invalidActions = actionsRequest;
    invalidActions.variables.insert(
        QStringLiteral("TOO_LARGE"), QString(16 * 1024 + 1, QLatin1Char('x')));
    check(forkmesh::control::buildMirrorActionsConfigurationPayload(
              invalidActions, &actionsError).isEmpty(),
          "oversize Actions variable value fails closed before SSH");

    const QJsonObject actionsResult{
        {QStringLiteral("type"),
         QStringLiteral("forkmesh.mirror-actions-configuration-result")},
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("ok"), true},
        {QStringLiteral("requestId"), actionsRequest.requestId},
        {QStringLiteral("node"), actionsRequest.nodeName},
        {QStringLiteral("actionsEnabled"), true},
        {QStringLiteral("variablesReplaced"), true},
        {QStringLiteral("variableCount"), 2},
    };
    const QByteArray encodedActionsResult =
        QJsonDocument(actionsResult)
            .toJson(QJsonDocument::Compact)
            .toBase64(QByteArray::Base64UrlEncoding |
                      QByteArray::OmitTrailingEquals);
    const QByteArray resultOutput =
        QByteArrayLiteral("bounded progress\nFORKMESH_ACTIONS_RESULT=") +
        encodedActionsResult + QByteArrayLiteral("\n");
    check(forkmesh::control::parseMirrorActionsConfigurationResult(
              resultOutput, actionsRequest.requestId,
              actionsRequest.nodeName, &actionsError)
                  .value(QStringLiteral("ok"))
                  .toBool(),
          "one matching bounded Actions helper result confirms the request");
    check(forkmesh::control::parseMirrorActionsConfigurationResult(
              resultOutput + resultOutput, actionsRequest.requestId,
              actionsRequest.nodeName, &actionsError).isEmpty(),
          "duplicate Actions helper results fail closed");
    check(forkmesh::control::parseMirrorActionsConfigurationResult(
              resultOutput, QString(32, QLatin1Char('f')),
              actionsRequest.nodeName, &actionsError).isEmpty(),
          "Actions helper result for another request fails closed");
    QJsonObject leakingActionsResult = actionsResult;
    leakingActionsResult.insert(
        QStringLiteral("variables"),
        QJsonObject{{QStringLiteral("DEPLOY_TOKEN"),
                     QStringLiteral("must-not-be-returned")}});
    const QByteArray leakingResultOutput =
        QByteArrayLiteral("FORKMESH_ACTIONS_RESULT=") +
        QJsonDocument(leakingActionsResult)
            .toJson(QJsonDocument::Compact)
            .toBase64(QByteArray::Base64UrlEncoding |
                      QByteArray::OmitTrailingEquals) +
        QByteArrayLiteral("\n");
    check(forkmesh::control::parseMirrorActionsConfigurationResult(
              leakingResultOutput, actionsRequest.requestId,
              actionsRequest.nodeName, &actionsError).isEmpty(),
          "Actions helper result cannot return variable names or values");
    check(forkmesh::control::parseMirrorActionsConfigurationResult(
              QByteArray(64 * 1024 + 1, 'x'), actionsRequest.requestId,
              actionsRequest.nodeName, &actionsError).isEmpty(),
          "oversize Actions helper output fails closed");

    QTemporaryDir mirrorActionsRoot;
    const QString sourceCheckout =
        mirrorActionsRoot.filePath(QStringLiteral("source"));
    const QString sourceBare =
        mirrorActionsRoot.filePath(QStringLiteral("source.git"));
    check(mirrorActionsRoot.isValid() &&
              QDir().mkpath(sourceCheckout) &&
              runProcess(QStringLiteral("git"),
                         {QStringLiteral("init"),
                          QStringLiteral("--initial-branch=main")},
                         sourceCheckout),
          "mirror Actions helper fixture repository initializes");
    QFile fixtureReadme(
        QDir(sourceCheckout).filePath(QStringLiteral("README.md")));
    check(fixtureReadme.open(QIODevice::WriteOnly) &&
              fixtureReadme.write("safe action fixture\n") > 0,
          "mirror Actions helper fixture writes one source file");
    fixtureReadme.close();
    check(runProcess(
              QStringLiteral("git"),
              {QStringLiteral("-c"), QStringLiteral("user.name=ForkMesh Test"),
               QStringLiteral("-c"),
               QStringLiteral("user.email=forkmesh@example.test"),
               QStringLiteral("add"), QStringLiteral("README.md")},
              sourceCheckout) &&
              runProcess(
                  QStringLiteral("git"),
                  {QStringLiteral("-c"),
                   QStringLiteral("user.name=ForkMesh Test"),
                   QStringLiteral("-c"),
                   QStringLiteral("user.email=forkmesh@example.test"),
                   QStringLiteral("commit"), QStringLiteral("-m"),
                   QStringLiteral("fixture")},
                  sourceCheckout) &&
              runProcess(QStringLiteral("git"),
                         {QStringLiteral("clone"), QStringLiteral("--bare"),
                          sourceCheckout, sourceBare}),
          "mirror Actions helper fixture creates a serving bare repository");
    QDir().mkpath(QDir(sourceBare).filePath(QStringLiteral("hooks")));
    const QString servingHook =
        QDir(sourceBare).filePath(QStringLiteral("hooks/post-receive"));
    QFile hook(servingHook);
    check(hook.open(QIODevice::WriteOnly) &&
              hook.write("#!/bin/sh\n# serving refresh hook\n") > 0,
          "mirror Actions fixture installs a serving refresh hook");
    hook.close();

    const QString gatewayConfig =
        mirrorActionsRoot.filePath(QStringLiteral("mirror-refresh.json"));
    QFile gateway(gatewayConfig);
    const QJsonObject gatewayObject{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("type"),
         QStringLiteral("forkmesh.headless-mirror-refresh")},
        {QStringLiteral("sourceRepository"), sourceBare},
        {QStringLiteral("nodeOwner"), QStringLiteral("mirror2")},
        {QStringLiteral("repositoryName"), QStringLiteral("forkmesh")},
        {QStringLiteral("ownerAliases"),
         QJsonArray{QStringLiteral("mirror2"),
                    QStringLiteral("forkmesh")}},
        {QStringLiteral("catalog"),
         QJsonObject{{QStringLiteral("branch"),
                      QStringLiteral("main")}}},
    };
    check(gateway.open(QIODevice::WriteOnly) &&
              gateway.write(
                  QJsonDocument(gatewayObject)
                      .toJson(QJsonDocument::Compact)) > 0,
          "mirror Actions fixture writes a secret-free gateway configuration");
    gateway.close();
    QFile::setPermissions(gatewayConfig,
                          QFileDevice::ReadOwner |
                              QFileDevice::WriteOwner);
    qputenv("FORKMESH_MIRROR_REFRESH_CONFIG",
            gatewayConfig.toUtf8());

    const QString actionSettingsPath =
        mirrorActionsRoot.filePath(QStringLiteral("settings.ini"));
    QSettings actionSettings(actionSettingsPath, QSettings::IniFormat);
    actionSettings.setValue(QStringLiteral("node/machineName"),
                            QStringLiteral("mirror2"));
    const QJsonObject nodeActionsRequest{
        {QStringLiteral("type"),
         QStringLiteral("forkmesh.mirror-actions-configuration")},
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("requestId"),
         QStringLiteral("abcdef0123456789abcdef0123456789")},
        {QStringLiteral("node"), QStringLiteral("mirror2")},
        {QStringLiteral("actionsEnabled"), true},
        {QStringLiteral("variables"),
         QJsonObject{
             {QStringLiteral("mode"), QStringLiteral("replace")},
             {QStringLiteral("values"),
              QJsonObject{
                  {QStringLiteral("DEPLOY_TOKEN"),
                   QStringLiteral("node-local-only-value")},
                  {QStringLiteral("DEPLOY_ENV"),
                   QStringLiteral("production")},
              }},
         }},
    };
    const QJsonObject applied =
        forkmesh::mirror_actions::applyConfiguration(nodeActionsRequest,
                                                     actionSettings);
    check(applied.value(QStringLiteral("ok")).toBool() &&
              applied.value(QStringLiteral("actionsEnabled")).toBool() &&
              applied.value(QStringLiteral("variablesReplaced")).toBool() &&
              applied.value(QStringLiteral("variableCount")).toInt() == 2 &&
              !QJsonDocument(applied)
                   .toJson(QJsonDocument::Compact)
                   .contains("DEPLOY_TOKEN") &&
              !QJsonDocument(applied)
                   .toJson(QJsonDocument::Compact)
                   .contains("node-local-only-value"),
          "node helper atomically commits local Actions settings without publishing the catalog");
    const QJsonObject savedVariables =
        QJsonDocument::fromJson(
            actionSettings
                .value(QStringLiteral("actions/variables"))
                .toByteArray())
            .object();
    check(savedVariables.value(QStringLiteral("DEPLOY_TOKEN")).toString() ==
                  QStringLiteral("node-local-only-value") &&
              actionSettings
                      .value(QString::fromLatin1(
                          forkmesh::mirror_actions::kEnabledSetting))
                      .toBool() &&
              actionSettings
                      .value(QString::fromLatin1(
                          forkmesh::mirror_actions::kSummaryPathSetting))
                      .toString() ==
                  QDir(QFileInfo(gatewayConfig).absolutePath())
                      .filePath(QStringLiteral("actions-summary.json")) &&
              !QFileInfo::exists(
                  forkmesh::mirror_actions::configurationRecoveryJournalPath(
                      actionSettings)),
          "node helper stores device-only variables and finalizes its local transaction");
    const int repositoryCount = actionSettings.beginReadArray(
        QStringLiteral("repositories/items"));
    actionSettings.setArrayIndex(0);
    const QString isolatedMirror =
        actionSettings.value(QStringLiteral("mirrorPath")).toString();
    check(repositoryCount == 1 &&
              actionSettings
                  .value(QStringLiteral("externallyManagedActions"))
                  .toBool() &&
              actionSettings
                      .value(QStringLiteral("externalActionsSource"))
                      .toString() == sourceBare &&
              isolatedMirror != sourceBare &&
              QFileInfo(QDir(isolatedMirror).filePath(QStringLiteral("HEAD")))
                  .isFile() &&
              !QFileInfo(QDir(isolatedMirror)
                             .filePath(QStringLiteral(
                                 "objects/info/alternates")))
                   .exists(),
          "node helper creates an independent Actions mirror and marks its hook external");
    actionSettings.endArray();
    QFile unchangedHook(servingHook);
    check(unchangedHook.open(QIODevice::ReadOnly) &&
              unchangedHook.readAll().contains("serving refresh hook"),
          "node helper preserves the serving repository post-receive hook");
    unchangedHook.close();
    const QFileInfo settingsInfo(actionSettingsPath);
    check(!(settingsInfo.permissions() &
            (QFileDevice::ReadGroup | QFileDevice::WriteGroup |
             QFileDevice::ReadOther | QFileDevice::WriteOther)),
          "node helper restricts its device-local settings file");

    const QString summaryStoreRoot =
        mirrorActionsRoot.filePath(QStringLiteral("summary-store"));
    ActionStore summaryStore(summaryStoreRoot);
    const qint64 summaryNow = 1784900000000LL;
    for (int index = 1; index <= 25; ++index) {
        ActionRun run;
        run.owner = QStringLiteral("forkmesh");
        run.name = QStringLiteral("forkmesh");
        run.workflowPath =
            QStringLiteral(".forkmesh/private-command-%1.yml").arg(index);
        run.workflowName = QStringLiteral("Build %1").arg(index);
        run.workflowContent =
            QStringLiteral("run: never-publish-command-%1").arg(index);
        run.commit = QStringLiteral("%1").arg(
            index, 40, 16, QLatin1Char('0'));
        run.ref = QStringLiteral("refs/heads/main");
        run.status = index == 25 ? ActionStatus::Running
                                 : ActionStatus::Success;
        run = summaryStore.createRun(run);
        run.createdAtMs = summaryNow - (25 - index) * 1000;
        run.startedAtMs = run.createdAtMs + 10;
        run.finishedAtMs =
            run.status == ActionStatus::Running
                ? 0
                : run.createdAtMs + 500;
        summaryStore.saveRun(run);
        const QString log =
            QString(20 * 1024, QLatin1Char('x')) +
            (index == 25
                 ? QString::fromUtf8(
                       "\nredacted=***\nvalid utf8 \xF0\x9F\x9A\x80")
                 : QStringLiteral("\nalready-redacted run log"));
        summaryStore.appendLog(run, log);
    }
    const QList<ActionRun> summaryRuns = summaryStore.loadAllRuns();
    const QJsonObject summary =
        forkmesh::mirror_actions::buildSummary(
            QStringLiteral("mirror2"), summaryRuns, summaryStore,
            summaryNow);
    QSet<QString> summaryKeys;
    for (auto it = summary.constBegin(); it != summary.constEnd(); ++it)
        summaryKeys.insert(it.key());
    const QSet<QString> expectedSummaryKeys{
        QStringLiteral("schemaVersion"), QStringLiteral("type"),
        QStringLiteral("node"), QStringLiteral("updatedAt"),
        QStringLiteral("expiresAt"), QStringLiteral("runs")};
    const QJsonArray summaryItems =
        summary.value(QStringLiteral("runs")).toArray();
    const QJsonObject latestSummaryRun =
        summaryItems.isEmpty() ? QJsonObject()
                               : summaryItems.at(0).toObject();
    QSet<QString> summaryRunKeys;
    for (auto it = latestSummaryRun.constBegin();
         it != latestSummaryRun.constEnd(); ++it) {
        summaryRunKeys.insert(it.key());
    }
    const QSet<QString> expectedSummaryRunKeys{
        QStringLiteral("id"), QStringLiteral("owner"),
        QStringLiteral("repository"), QStringLiteral("workflow"),
        QStringLiteral("commit"), QStringLiteral("ref"),
        QStringLiteral("status"), QStringLiteral("createdAt"),
        QStringLiteral("startedAt"), QStringLiteral("finishedAt"),
        QStringLiteral("logTail")};
    const QByteArray summaryJson =
        QJsonDocument(summary).toJson(QJsonDocument::Compact);
    check(summaryKeys == expectedSummaryKeys &&
              summary.value(QStringLiteral("type")).toString() ==
                  QStringLiteral("forkmesh.mirror-actions-summary") &&
              summary.value(QStringLiteral("node")).toString() ==
                  QStringLiteral("mirror2") &&
              summary.value(QStringLiteral("expiresAt")).toVariant()
                      .toLongLong() >
                  summary.value(QStringLiteral("updatedAt")).toVariant()
                      .toLongLong() &&
              summary.value(QStringLiteral("expiresAt")).toVariant()
                          .toLongLong() -
                      summary.value(QStringLiteral("updatedAt")).toVariant()
                          .toLongLong() <=
                  15 * 60 * 1000 &&
              summaryItems.size() == 20 &&
              summaryJson.size() + 1 <= 256 * 1024 &&
              summaryRunKeys == expectedSummaryRunKeys &&
              latestSummaryRun.value(QStringLiteral("id")).toInt() == 25 &&
              latestSummaryRun.value(QStringLiteral("status")).toString() ==
                  ActionStatus::Running &&
              latestSummaryRun.value(QStringLiteral("logTail"))
                      .toString()
                      .toUtf8()
                      .size() <=
                  forkmesh::mirror_actions::
                      kMaximumSummaryLogTailBytes &&
              latestSummaryRun.value(QStringLiteral("logTail"))
                  .toString()
                  .contains(QString::fromUtf8("\xF0\x9F\x9A\x80")) &&
              latestSummaryRun.value(QStringLiteral("logTail"))
                  .toString()
                  .contains(QStringLiteral("redacted=***")) &&
              !summaryJson.contains("workflowContent") &&
              !summaryJson.contains("workflowPath") &&
              !summaryJson.contains("never-publish-command") &&
              !summaryJson.contains("private-command"),
          "mirror Actions summary is exact, fresh, bounded, UTF-8, and secret-free");

    const QString summaryDirectory =
        mirrorActionsRoot.filePath(QStringLiteral("summary-public"));
    QDir().mkpath(summaryDirectory);
    QFile::setPermissions(
        summaryDirectory,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner);
    const QString summaryPath =
        QDir(summaryDirectory)
            .filePath(QStringLiteral("actions-summary.json"));
    check(forkmesh::mirror_actions::writeSummaryFile(
              summaryPath, QStringLiteral("mirror2"), summaryRuns,
              summaryStore, summaryNow),
          "mirror Actions summary commits through an owner-only QSaveFile");
    QFile summaryFile(summaryPath);
    check(summaryFile.open(QIODevice::ReadOnly) &&
              QJsonDocument::fromJson(summaryFile.readAll()).isObject() &&
              !(QFileInfo(summaryPath).permissions() &
                (QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                 QFileDevice::ExeGroup | QFileDevice::ReadOther |
                 QFileDevice::WriteOther | QFileDevice::ExeOther)),
          "mirror Actions summary remains strict JSON mode 0600");
    summaryFile.close();
    const QString statePath =
        QDir(summaryDirectory)
            .filePath(QStringLiteral("actions-state.json"));
    check(forkmesh::mirror_actions::writeStateFile(
              statePath, QStringLiteral("mirror2"),
              QStringLiteral("running"), summaryNow),
          "mirror Actions state commits through the protected atomic writer");
    QFile stateFile(statePath);
    const QJsonObject writtenState =
        stateFile.open(QIODevice::ReadOnly)
            ? QJsonDocument::fromJson(stateFile.readAll()).object()
            : QJsonObject();
    const QSet<QString> expectedStateKeys{
        QStringLiteral("schemaVersion"), QStringLiteral("type"),
        QStringLiteral("node"), QStringLiteral("state"),
        QStringLiteral("updatedAt"), QStringLiteral("expiresAt")};
    QSet<QString> writtenStateKeys;
    for (auto it = writtenState.constBegin();
         it != writtenState.constEnd(); ++it) {
        writtenStateKeys.insert(it.key());
    }
    check(writtenStateKeys == expectedStateKeys &&
              writtenState.value(QStringLiteral("type")).toString() ==
                  QStringLiteral("forkmesh.mirror-actions-state") &&
              writtenState.value(QStringLiteral("state")).toString() ==
                  QStringLiteral("running") &&
              !(QFileInfo(statePath).permissions() &
                (QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                 QFileDevice::ExeGroup | QFileDevice::ReadOther |
                 QFileDevice::WriteOther | QFileDevice::ExeOther)),
          "mirror Actions state remains exact bounded JSON mode 0600");
    stateFile.close();
    check(!forkmesh::mirror_actions::writeStateFile(
              statePath, QStringLiteral("mirror2"),
              QStringLiteral("unknown"), summaryNow),
          "mirror Actions state rejects an unknown runtime state");

    using forkmesh::mirror_actions::detail::SummaryWritePolicy;
    const auto policy = [](
                            const QString &path,
                            quint64 effectiveUserId,
                            quint64 parentUserId,
                            quint64 parentGroupId,
                            quint32 parentMode,
                            bool targetExists,
                            bool targetRegular,
                            bool targetSymlink,
                            quint64 targetUserId,
                            quint64 targetGroupId,
                            quint32 targetMode,
                            quint64 targetLinks,
                            bool realParent = true) {
        return forkmesh::mirror_actions::detail::
            classifySummaryWritePolicy(
                path, effectiveUserId, parentUserId, parentGroupId,
                parentMode, realParent, targetExists, targetRegular,
                targetSymlink, targetUserId, targetGroupId, targetMode,
                targetLinks);
    };
    check(policy(
              summaryPath, 1000, 1000, 1000, 0700,
              true, true, false, 1000, 1000, 0600, 1) ==
              SummaryWritePolicy::SameAccount,
          "same-account Actions handoff accepts only owner mode 0600");
    for (const QString &fixedPath : {
             QString::fromLatin1(
                 forkmesh::mirror_actions::kSystemSummaryPath),
             QString::fromLatin1(
                 forkmesh::mirror_actions::kSystemStatePath)}) {
        check(policy(
                  fixedPath, 0, 991, 992, 0700,
                  false, false, false, 0, 0, 0, 0) ==
                  SummaryWritePolicy::RootGatewayHandoff &&
                  policy(
                      fixedPath, 0, 991, 992, 0700,
                      true, true, false, 0, 992, 0640, 1) ==
                  SummaryWritePolicy::RootGatewayHandoff,
              "fixed Actions files accept root-to-gateway group handoff");
    }
    const QString fixedSummary = QString::fromLatin1(
        forkmesh::mirror_actions::kSystemSummaryPath);
    check(policy(
              QStringLiteral("/tmp/actions-summary.json"),
              0, 991, 992, 0700, false, false, false,
              0, 0, 0, 0) == SummaryWritePolicy::Reject &&
              policy(
                  fixedSummary, 0, 991, 992, 0700,
                  true, true, false, 991, 992, 0640, 1) ==
                  SummaryWritePolicy::Reject,
          "root Actions handoff rejects arbitrary paths and service-owned spoofs");
    check(policy(
              fixedSummary, 0, 991, 992, 0700,
              true, true, false, 0, 991, 0640, 1) ==
              SummaryWritePolicy::Reject &&
              policy(
                  fixedSummary, 0, 991, 992, 0700,
                  true, true, false, 0, 992, 0600, 1) ==
                  SummaryWritePolicy::Reject &&
              policy(
                  fixedSummary, 0, 991, 992, 0700,
                  true, true, true, 0, 992, 0640, 1) ==
                  SummaryWritePolicy::Reject &&
              policy(
                  fixedSummary, 0, 991, 992, 0700,
                  true, true, false, 0, 992, 0640, 2) ==
                  SummaryWritePolicy::Reject,
          "root Actions handoff rejects wrong group, mode, symlink, and hardlink");
    check(policy(
              fixedSummary, 0, 991, 992, 0750,
              false, false, false, 0, 0, 0, 0) ==
              SummaryWritePolicy::Reject &&
              policy(
                  fixedSummary, 0, 991, 992, 0700,
                  false, false, false, 0, 0, 0, 0, false) ==
                  SummaryWritePolicy::Reject,
          "root Actions handoff requires an exact mode-0700 real parent");
    check(forkmesh::mirror_actions::buildSummary(
              QStringLiteral("invalid node"), summaryRuns,
              summaryStore, summaryNow)
              .isEmpty(),
          "mirror Actions summary rejects an invalid node identity");

    QJsonObject wrongNodeRequest = nodeActionsRequest;
    wrongNodeRequest.insert(QStringLiteral("node"),
                            QStringLiteral("mirror3"));
    check(!forkmesh::mirror_actions::applyConfiguration(
               wrongNodeRequest, actionSettings)
               .value(QStringLiteral("ok"))
               .toBool(),
          "node helper rejects a request for another mirror identity");

    checkMirrorActionsRecoveryLock();

    // A complete-system deployment JSON preserves typed settings, every
    // variable/secret, and the exact bytes of identity/application files while
    // rejecting path traversal or content tampering before anything is used.
    QTemporaryDir completeBundleRoot;
    const QString completeAppData =
        QDir(completeBundleRoot.path()).filePath(QStringLiteral("app-data"));
    check(QDir().mkpath(QDir(completeAppData).filePath(QStringLiteral("ssh"))) &&
              QDir().mkpath(
                  QDir(completeAppData).filePath(QStringLiteral("empty-dir"))),
          "complete-system test tree is created");
    const QByteArray identityBytes("private-identity-bytes\0binary", 29);
    QFile identityFile(
        QDir(completeAppData).filePath(QStringLiteral("ssh/identity.key")));
    check(identityFile.open(QIODevice::WriteOnly) &&
              identityFile.write(identityBytes) == identityBytes.size(),
          "complete-system identity fixture is written");
    identityFile.close();
    const QString completeSettingsPath =
        QDir(completeBundleRoot.path()).filePath(QStringLiteral("settings.ini"));
    QSettings completeSettings(completeSettingsPath, QSettings::IniFormat);
    completeSettings.setValue(QStringLiteral("plain/name"),
                              QStringLiteral("controller"));
    completeSettings.setValue(QStringLiteral("typed/count"), 42);
    completeSettings.setValue(QStringLiteral("typed/bytes"),
                              QByteArray("\x00\xff\x10", 3));
    completeSettings.setValue(QStringLiteral("typed/list"),
                              QStringList{QStringLiteral("one"),
                                          QStringLiteral("two")});
    completeSettings.sync();
    const QString workerEnvironmentPath =
        QDir(completeBundleRoot.path()).filePath(QStringLiteral(".env.production"));
    QFile workerEnvironment(workerEnvironmentPath);
    check(workerEnvironment.open(QIODevice::WriteOnly) &&
              workerEnvironment.write("CLOUDFLARE_API_TOKEN=secret\n") > 0,
          "complete-system environment fixture is written");
    workerEnvironment.close();
    const QMap<QString, QString> completeVariables{
        {QStringLiteral("CLOUDFLARE_API_TOKEN"), QStringLiteral("cf-secret")},
        {QStringLiteral("VULTR_API_KEY"), QStringLiteral("vultr-secret")},
    };
    QString completeError;
    const QJsonObject completeBundle =
        forkmesh::control::createCompleteSystemBundle(
            completeSettings, completeVariables, completeAppData,
            completeSettingsPath, workerEnvironmentPath,
            QJsonObject{{QStringLiteral("provider"),
                         QStringLiteral("vultr")}},
            &completeError);
    check(!completeBundle.isEmpty() && completeError.isEmpty() &&
              completeBundle.value(QStringLiteral("kind")).toString() ==
                  QStringLiteral("forkmesh-complete-system-v1") &&
              completeBundle.value(QStringLiteral("security"))
                  .toObject()
                  .value(QStringLiteral("clearTextSecrets"))
                  .toBool(),
          "complete-system bundle has a versioned explicit secret-bearing contract");
    bool completeVariablesOk = false;
    check(forkmesh::control::completeSystemBundleVariables(
              completeBundle, &completeVariablesOk) == completeVariables &&
              completeVariablesOk,
          "complete-system bundle preserves every variable and secret");
    bool completeSettingOk = false;
    const QVariant importedBytes =
        forkmesh::control::completeSystemBundleSetting(
            completeBundle, QStringLiteral("typed/bytes"), &completeSettingOk);
    check(completeSettingOk && importedBytes.typeId() == QMetaType::QByteArray &&
              importedBytes.toByteArray() == QByteArray("\x00\xff\x10", 3),
          "complete-system bundle round trips non-JSON setting types exactly");
    bool foundIdentity = false;
    bool foundSettingsFile = false;
    bool foundEnvironmentFile = false;
    for (const QJsonValue &value :
         completeBundle.value(QStringLiteral("files")).toArray()) {
        const QJsonObject file = value.toObject();
        const QString scope = file.value(QStringLiteral("scope")).toString();
        const QString path = file.value(QStringLiteral("path")).toString();
        if (scope == QLatin1String("applicationData") &&
            path == QLatin1String("ssh/identity.key")) {
            foundIdentity =
                QByteArray::fromBase64(
                    file.value(QStringLiteral("dataBase64"))
                        .toString()
                        .toLatin1()) == identityBytes;
        }
        foundSettingsFile |= scope == QLatin1String("settingsFile");
        foundEnvironmentFile |= scope == QLatin1String("workerEnvironment");
    }
    check(foundIdentity && foundSettingsFile && foundEnvironmentFile,
          "complete-system bundle includes keys, native settings, and worker environment");
    const QString completeBundlePath =
        QDir(completeBundleRoot.path()).filePath(QStringLiteral("complete.json"));
    check(forkmesh::control::writeCompleteSystemBundle(
              completeBundlePath, completeBundle, &completeError) &&
              !(QFileInfo(completeBundlePath).permissions() &
                (QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                 QFileDevice::ReadOther | QFileDevice::WriteOther)),
          "complete-system JSON is committed owner-only");
    const QJsonObject rereadBundle =
        forkmesh::control::readCompleteSystemBundle(completeBundlePath,
                                                    &completeError);
    check(!rereadBundle.isEmpty() && completeError.isEmpty(),
          "complete-system JSON reads back after full integrity validation");
    QJsonObject traversalBundle = completeBundle;
    QJsonArray traversalFiles =
        traversalBundle.value(QStringLiteral("files")).toArray();
    QJsonObject traversalFile = traversalFiles.first().toObject();
    traversalFile.insert(QStringLiteral("path"), QStringLiteral("../identity"));
    traversalFiles.replace(0, traversalFile);
    traversalBundle.insert(QStringLiteral("files"), traversalFiles);
    check(!forkmesh::control::validateCompleteSystemBundle(
              traversalBundle, &completeError),
          "complete-system import rejects path traversal");
    QJsonObject tamperedBundle = completeBundle;
    QJsonArray tamperedFiles = tamperedBundle.value(QStringLiteral("files")).toArray();
    QJsonObject tamperedFile = tamperedFiles.first().toObject();
    tamperedFile.insert(QStringLiteral("dataBase64"), QStringLiteral("dGFtcGVy"));
    tamperedFiles.replace(0, tamperedFile);
    tamperedBundle.insert(QStringLiteral("files"), tamperedFiles);
    check(!forkmesh::control::validateCompleteSystemBundle(
              tamperedBundle, &completeError),
          "complete-system import rejects file-content tampering");
    const QString mergedSettingsPath =
        QDir(completeBundleRoot.path()).filePath(QStringLiteral("merged.ini"));
    QSettings mergedSettings(mergedSettingsPath, QSettings::IniFormat);
    mergedSettings.setValue(QStringLiteral("newer/retained"), true);
    check(forkmesh::control::mergeCompleteSystemBundleSettings(
              completeBundle, mergedSettings, &completeError) &&
              mergedSettings.value(QStringLiteral("typed/count")).typeId() ==
                  QMetaType::Int &&
              mergedSettings.value(QStringLiteral("typed/count")).toInt() == 42 &&
              mergedSettings.value(QStringLiteral("typed/list")).toStringList() ==
                  QStringList{QStringLiteral("one"), QStringLiteral("two")} &&
              mergedSettings.value(QStringLiteral("newer/retained")).toBool(),
          "complete-system import restores typed settings without deleting newer keys");

    // An Actions sandbox or agent worktree leaves entries a JSON file cannot
    // carry (FIFOs, sockets, symbolic links) inside application data, and the
    // re-fetchable trees under it are gigabytes. Neither may abort a recovery
    // export: unsupported entries are reported, excluded top-level names are
    // never descended into, and the survey the export page draws from reports
    // both.
    QTemporaryDir sandboxRoot;
    const QString sandboxAppData =
        QDir(sandboxRoot.path()).filePath(QStringLiteral("app-data"));
    check(QDir().mkpath(QDir(sandboxAppData)
                            .filePath(QStringLiteral("actions/sandbox/run/tmp"))) &&
              QDir().mkpath(
                  QDir(sandboxAppData).filePath(QStringLiteral("identity"))),
          "sandbox-shaped application data is created");
    QFile keptFile(
        QDir(sandboxAppData).filePath(QStringLiteral("identity/node.key")));
    check(keptFile.open(QIODevice::WriteOnly) && keptFile.write("k") == 1,
          "sandbox fixture identity file is written");
    keptFile.close();
    QFile sandboxFile(QDir(sandboxAppData)
                          .filePath(QStringLiteral("actions/sandbox/run/tmp/big")));
    check(sandboxFile.open(QIODevice::WriteOnly) && sandboxFile.write("v") == 1,
          "sandbox fixture workspace file is written");
    sandboxFile.close();
    const QString fifoPath =
        QDir(sandboxAppData)
            .filePath(QStringLiteral("identity/GMfifo124"));
#ifdef Q_OS_UNIX
    check(::mkfifo(fifoPath.toLocal8Bit().constData(), 0600) == 0,
          "sandbox fixture FIFO is created");
#endif
    check(QFile::link(QDir(sandboxAppData)
                          .filePath(QStringLiteral("identity/node.key")),
                      QDir(sandboxAppData)
                          .filePath(QStringLiteral("identity/node.link"))),
          "sandbox fixture symbolic link is created");

    QString sandboxError;
    const QJsonObject sandboxBundle =
        forkmesh::control::createCompleteSystemBundle(
            completeSettings, completeVariables, sandboxAppData,
            completeSettingsPath, workerEnvironmentPath,
            QJsonObject{{QStringLiteral("provider"), QStringLiteral("vultr")}},
            &sandboxError, QStringList{QStringLiteral("actions")});
    const QJsonObject sandboxSummary =
        sandboxBundle.value(QStringLiteral("summary")).toObject();
    bool sandboxKeptIdentity = false;
    bool sandboxCarriedExcludedTree = false;
    for (const QJsonValue &value :
         sandboxBundle.value(QStringLiteral("files")).toArray()) {
        const QJsonObject file = value.toObject();
        if (file.value(QStringLiteral("scope")).toString() !=
            QLatin1String("applicationData")) {
            continue;
        }
        const QString path = file.value(QStringLiteral("path")).toString();
        sandboxKeptIdentity |= path == QLatin1String("identity/node.key");
        sandboxCarriedExcludedTree |=
            path.startsWith(QLatin1String("actions/"));
    }
    check(!sandboxBundle.isEmpty() && sandboxError.isEmpty() &&
              sandboxKeptIdentity && !sandboxCarriedExcludedTree,
          "complete-system export survives unsupported entries and skips excluded trees");
    bool sandboxNamedTheFifo = false;
    bool sandboxNamedTheLink = false;
    for (const QJsonValue &value :
         sandboxBundle.value(QStringLiteral("skipped")).toArray()) {
        const QString path =
            value.toObject().value(QStringLiteral("path")).toString();
        sandboxNamedTheFifo |= path == QLatin1String("identity/GMfifo124");
        sandboxNamedTheLink |= path == QLatin1String("identity/node.link");
    }
#ifdef Q_OS_UNIX
    check(sandboxNamedTheFifo && sandboxNamedTheLink &&
              sandboxSummary.value(QStringLiteral("skippedCount")).toInt() == 2,
          "complete-system export names every entry JSON cannot carry");
#else
    check(sandboxNamedTheLink,
          "complete-system export names every entry JSON cannot carry");
#endif
    bool sandboxRecordedExclusion = false;
    for (const QJsonValue &value :
         sandboxBundle.value(QStringLiteral("excludedApplicationData")).toArray()) {
        sandboxRecordedExclusion |= value.toString() == QLatin1String("actions");
    }
    check(sandboxRecordedExclusion,
          "complete-system bundle records which application-data trees were left out");

    check(forkmesh::control::completeSystemDataIsBulk(
              QStringLiteral("actions")) &&
              forkmesh::control::completeSystemDataIsBulk(
                  QStringLiteral("mirrors")) &&
              !forkmesh::control::completeSystemDataIsBulk(
                  QStringLiteral("identity")),
          "re-fetchable application-data trees are the ones excluded by default");
    const QList<forkmesh::control::CompleteSystemDataEntry> sandboxSurvey =
        forkmesh::control::surveyCompleteSystemApplicationData(sandboxAppData);
    bool surveyedActions = false;
    bool surveyedIdentity = false;
    for (const forkmesh::control::CompleteSystemDataEntry &entry :
         sandboxSurvey) {
        if (entry.name == QLatin1String("actions")) {
            surveyedActions = entry.bulk && entry.directory &&
                              entry.fileCount == 1 && entry.bytes == 1;
        } else if (entry.name == QLatin1String("identity")) {
#ifdef Q_OS_UNIX
            const int expectedSkips = 2; // the FIFO and the symbolic link
#else
            const int expectedSkips = 1;
#endif
            surveyedIdentity = !entry.bulk && entry.fileCount == 1 &&
                               entry.skippedEntries == expectedSkips;
        }
    }
    check(surveyedActions && surveyedIdentity,
          "the export page survey reports per-tree size and unsupported entries");

    qunsetenv("FORKMESH_MIRROR_REFRESH_CONFIG");

    // --- Usage tab (adhoc #95): "what is using the Durable Objects the most"
    // needs a per-object ranking, which the account-wide analytics API cannot
    // give — its finest dimension is the namespace. The tail can, so the tally
    // groups its events by Worker, class, and object instance.
    {
        // Pin the locale: the tab's grouping separators follow the reader's,
        // and these checks read the formatted numbers back.
        const QLocale savedLocale = QLocale();
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
        auto tailEvent = [](const QString &script, const QString &entrypoint,
                            const QString &objectId, const QString &outcome,
                            double wallMs, double cpuMs) {
            return forkmesh::control::parseCloudflareTailLine(
                QStringLiteral(
                    "{\"outcome\":\"%1\",\"eventTimestamp\":1000,"
                    "\"scriptName\":\"%2\",\"entrypoint\":\"%3\","
                    "\"durableObjectId\":\"%4\",\"wallTime\":%5,"
                    "\"cpuTime\":%6,"
                    "\"event\":{\"request\":{\"method\":\"GET\","
                    "\"url\":\"https://app.forkmesh.com/api/status\"},"
                    "\"response\":{\"status\":200}},"
                    "\"logs\":[],\"exceptions\":[]}")
                    .arg(outcome, script, entrypoint, objectId)
                    .arg(wallMs)
                    .arg(cpuMs));
        };
        const QString busyId(64, QLatin1Char('a'));
        const QString quietId(64, QLatin1Char('b'));
        forkmesh::control::CloudUsageTally tally;
        for (int i = 0; i < 5; ++i)
            tally.record(tailEvent(QStringLiteral("forkmesh"),
                                   QStringLiteral("ForkMeshRoom"), busyId,
                                   QStringLiteral("ok"), 20, 4),
                         10'000 + i);
        tally.record(tailEvent(QStringLiteral("forkmesh"),
                               QStringLiteral("ForkMeshRoom"), quietId,
                               QStringLiteral("exception"), 8, 2),
                     20'000);
        const auto ranked = tally.ranked(10);
        check(ranked.size() == 2 && ranked.at(0).durableObjectId == busyId &&
                  ranked.at(0).events == 5 && ranked.at(0).errors == 0 &&
                  ranked.at(1).durableObjectId == quietId &&
                  ranked.at(1).errors == 1 && tally.totalEvents() == 6,
              "the usage tally ranks Durable Object instances busiest first");
        // Two instances of the same class are the whole point: rolling them up
        // by class would answer a question nobody asked.
        check(ranked.at(0).entrypoint == QLatin1String("ForkMeshRoom") &&
                  ranked.at(1).entrypoint == QLatin1String("ForkMeshRoom") &&
                  ranked.at(0).scriptName == QLatin1String("forkmesh"),
              "instances of one class stay separate rows under it");
        check(qFuzzyCompare(ranked.at(0).wallTimeMs, 100.0) &&
                  qFuzzyCompare(ranked.at(0).cpuTimeMs, 20.0) &&
                  ranked.at(0).lastSeenMs == 10'004,
              "the tally sums the cost figures the tail carried");
        // A relay fronting thousands of rooms must not grow a row per room.
        forkmesh::control::CloudUsageTally bounded;
        for (int i = 0; i < 400; ++i)
            bounded.record(tailEvent(QStringLiteral("forkmesh"),
                                     QStringLiteral("ForkMeshRoom"),
                                     QStringLiteral("%1").arg(i, 64, 10,
                                                              QLatin1Char('0')),
                                     QStringLiteral("ok"), 1, 1),
                           30'000);
        const auto boundedRanked = bounded.ranked(10);
        qint64 boundedShown = 0;
        bool sawOverflow = false;
        for (const auto &entry : boundedRanked) {
            boundedShown += entry.events;
            sawOverflow = sawOverflow || entry.overflow;
        }
        check(boundedRanked.size() == 11 && sawOverflow &&
                  boundedRanked.last().overflow &&
                  boundedRanked.last().events > 0 &&
                  bounded.totalEvents() == 400 && boundedShown < 400,
              "an unbounded object population folds into one overflow row");
        check(!tally.isEmpty() && tally.sinceMs() == 10'000,
              "the tally reports the window its counts cover");
        forkmesh::control::CloudUsageTally cleared = tally;
        cleared.clear();
        check(cleared.isEmpty() && cleared.totalEvents() == 0 &&
                  cleared.ranked(10).isEmpty(),
              "clearing the log clears the usage tally with it");

        // The Network page's Web Requests tab is counted from the same tail:
        // the Worker stores no traffic statistics (migration 0190).
        check(forkmesh::control::webRequestRouteGroup(
                  QStringLiteral("/api/accounts/alice")) ==
                      QLatin1String("accounts/*") &&
                  forkmesh::control::webRequestRouteGroup(
                      QStringLiteral("/api/version")) ==
                      QLatin1String("version") &&
                  forkmesh::control::webRequestRouteGroup(
                      QStringLiteral("/api/repo/ForkMesh/forkmesh/tree")) ==
                      QLatin1String("repo/forkmesh/forkmesh/content") &&
                  forkmesh::control::webRequestRouteGroup(
                      QStringLiteral("/api/repo/alice/site/tree")) ==
                      QLatin1String("repo/*") &&
                  forkmesh::control::webRequestRouteGroup(
                      QStringLiteral("/api/customer-name/private")) ==
                      QLatin1String("other"),
              "web request route groups match the Worker's masked labels");
        const auto fetchEvent = [](const QString &url, int status,
                                   const QString &outcome = QStringLiteral("ok"),
                                   const QString &entrypoint = QString()) {
            return forkmesh::control::parseCloudflareTailLine(
                QStringLiteral(
                    "{\"outcome\":\"%1\",\"scriptName\":\"forkmesh\","
                    "\"entrypoint\":\"%2\",\"wallTime\":30,"
                    "\"event\":{\"request\":{\"method\":\"GET\","
                    "\"url\":\"%3\",\"headers\":{}}%4},"
                    "\"logs\":[],\"exceptions\":[]}")
                    .arg(outcome, entrypoint, url,
                         status > 0
                             ? QStringLiteral(",\"response\":{\"status\":%1}")
                                   .arg(status)
                             : QString()));
        };
        constexpr qint64 kMinute = 60'000;
        const qint64 now = 1'790'000'000'000 / kMinute * kMinute + 5'000;
        forkmesh::control::WebRequestTally web;
        check(web.record(fetchEvent(
                             QStringLiteral("https://app.forkmesh.com/api/version"),
                             200),
                         now - 2 * kMinute) &&
                  web.record(fetchEvent(
                                 QStringLiteral(
                                     "https://app.forkmesh.com/api/version?x=1"),
                                 200),
                             now) &&
                  web.record(fetchEvent(
                                 QStringLiteral(
                                     "https://app.forkmesh.com/api/accounts/bob"),
                                 503),
                             now),
              "the tally counts the Worker's own /api hits");
        check(!web.record(fetchEvent(QStringLiteral("https://forkmesh.com/"), 200),
                          now) &&
                  !web.record(fetchEvent(
                                  QStringLiteral(
                                      "https://app.forkmesh.com/api/status"),
                                  200, QStringLiteral("ok"),
                                  QStringLiteral("ForkMeshRoom")),
                              now) &&
                  !web.record(fetchEvent(
                                  QStringLiteral(
                                      "https://app.forkmesh.com/api/status"),
                                  0, QStringLiteral("canceled")),
                              now),
              "pages, Durable Object hops and unanswered hits are not counted");
        check(web.record(fetchEvent(
                             QStringLiteral("https://app.forkmesh.com/api/status"),
                             0, QStringLiteral("exception")),
                         now),
              "a Worker exception with no response counts as a 5xx");
        const QJsonObject summary = web.summary(60, now);
        const QJsonArray webGroups =
            summary.value(QStringLiteral("groups")).toArray();
        const QJsonObject top = webGroups.at(0).toObject();
        check(webGroups.size() == 3 &&
                  top.value(QStringLiteral("group")).toString() ==
                      QLatin1String("version") &&
                  top.value(QStringLiteral("requests")).toInteger() == 2 &&
                  top.value(QStringLiteral("avg_ms")).toInteger() == 30 &&
                  top.value(QStringLiteral("dur_ms_max")).toInteger() == 30,
              "the summary ranks route groups busiest first with latency");
        check(summary.value(QStringLiteral("classes"))
                          .toObject()
                          .value(QStringLiteral("5xx"))
                          .toInteger() == 2 &&
                  summary.value(QStringLiteral("minutes")).toArray().size() ==
                      2 &&
                  summary.value(QStringLiteral("countingSinceMs"))
                          .toInteger() == now - 2 * kMinute,
              "the summary carries status classes, minutes and its start");
        check(web.summary(1, now)
                      .value(QStringLiteral("groups"))
                      .toArray()
                      .size() == 3 &&
                  web.summary(1, now)
                          .value(QStringLiteral("minutes"))
                          .toArray()
                          .size() == 1,
              "a shorter window drops older minutes");
        web.record(fetchEvent(
                       QStringLiteral("https://app.forkmesh.com/api/version"),
                       200),
                   now + (forkmesh::control::WebRequestTally::kMaxMinutes + 5) *
                             kMinute);
        check(web.summary(forkmesh::control::WebRequestTally::kMaxMinutes,
                          now + (forkmesh::control::WebRequestTally::kMaxMinutes +
                                 5) * kMinute)
                      .value(QStringLiteral("minutes"))
                      .toArray()
                      .size() == 1,
              "minutes older than a day are pruned as the tail streams");
        web.clear();
        check(web.isEmpty() && web.sinceMs() == 0,
              "clearing the web request tally empties it");

        check(web.recordBatch(QStringLiteral("accounts/*"),
                              QStringLiteral("2xx"), 3, 42, 30,
                              now - kMinute + 10'000, now) &&
                  web.recordBatch(QStringLiteral("accounts/*"),
                                  QStringLiteral("5xx"), 1, 500, 500,
                                  now - 1'000, now) &&
                  web.recordBatch(QStringLiteral("version"),
                                  QStringLiteral("2xx"), 2, 10, 6, now - 1'000,
                                  now),
              "a core node's batch rows are counted");
        check(!web.recordBatch(QStringLiteral("version"), QStringLiteral("1xx"),
                               1, 1, 1, now, now) &&
                  !web.recordBatch(QStringLiteral("version"),
                                   QStringLiteral("2xx"), 0, 0, 0, now, now) &&
                  !web.recordBatch(QString(), QStringLiteral("2xx"), 1, 1, 1,
                                   now, now) &&
                  !web.recordBatch(
                      QStringLiteral("version"), QStringLiteral("2xx"), 1, 1, 1,
                      now - (forkmesh::control::WebRequestTally::kMaxMinutes +
                             2) * kMinute,
                      now),
              "unknown classes, empty rows and day-old windows are ignored");
        const QJsonObject batchSummary = web.summary(60, now);
        const QJsonObject batchTop = batchSummary.value(QStringLiteral("groups"))
                                         .toArray()
                                         .at(0)
                                         .toObject();
        check(batchTop.value(QStringLiteral("group")).toString() ==
                      QLatin1String("accounts/*") &&
                  batchTop.value(QStringLiteral("requests")).toInteger() == 4 &&
                  batchTop.value(QStringLiteral("errors")).toInteger() == 1 &&
                  batchTop.value(QStringLiteral("avg_ms")).toInteger() == 135 &&
                  batchTop.value(QStringLiteral("dur_ms_max")).toInteger() ==
                      500,
              "batch rows sum into route groups with latency");
        check(batchSummary.value(QStringLiteral("minutes")).toArray().size() ==
                      2 &&
                  web.sinceMs() == now - kMinute + 10'000,
              "batch rows land in their own window's minute");
        web.clear();

        // The rendered tab: the free-plan meters and the namespace breakdown
        // that say which class is spending the day's allowance.
        QJsonObject report{
            {QStringLiteral("ok"), true},
            {QStringLiteral("day"), QStringLiteral("2026-08-19")},
            {QStringLiteral("sampledAt"), 1'787'176'800'000.0},
            {QStringLiteral("budgets"),
             QJsonArray{QJsonObject{
                 {QStringLiteral("metric"), QStringLiteral("do-duration")},
                 {QStringLiteral("label"),
                  QStringLiteral("Durable Object duration")},
                 {QStringLiteral("unit"), QStringLiteral("GB-s")},
                 {QStringLiteral("allowance"), 13000.0},
                 {QStringLiteral("used"), 11050.0},
                 {QStringLiteral("percent"), 85.0},
                 {QStringLiteral("pingedThreshold"), 80}}}},
            {QStringLiteral("timeline"),
             QJsonArray{
                 QJsonObject{{QStringLiteral("at"),
                              QStringLiteral("2026-08-19T00:00:00Z")},
                             {QStringLiteral("do-duration"), 400.0}},
                 QJsonObject{{QStringLiteral("at"),
                              QStringLiteral("2026-08-19T21:00:00Z")},
                             {QStringLiteral("do-duration"), 650.0}}}},
            {QStringLiteral("durableObjects"),
             QJsonArray{
                 QJsonObject{
                     {QStringLiteral("namespaceId"), QStringLiteral("aaa111")},
                     {QStringLiteral("label"), QStringLiteral("ForkMeshWorld")},
                     {QStringLiteral("script"), QStringLiteral("forkmesh")},
                     {QStringLiteral("gbSeconds"), 9000.0},
                     {QStringLiteral("requests"), 4321.0},
                     {QStringLiteral("errors"), 12.0},
                     {QStringLiteral("websocketIn"), 900.0}},
                 QJsonObject{
                     {QStringLiteral("namespaceId"), QStringLiteral("bbb222")},
                     {QStringLiteral("script"), QStringLiteral("forkmesh")},
                     {QStringLiteral("gbSeconds"), 50.0},
                     {QStringLiteral("requests"), 7.0}}}},
            {QStringLiteral("workers"),
             QJsonArray{QJsonObject{
                 {QStringLiteral("script"), QStringLiteral("forkmesh")},
                 {QStringLiteral("requests"), 8888.0},
                 {QStringLiteral("errors"), 0.0}}}},
            {QStringLiteral("aborts"),
             QJsonObject{{QStringLiteral("aborts24h"), 42.0},
                         {QStringLiteral("durationAborts24h"), 40.0},
                         {QStringLiteral("aborts60m"), 5.0}}}};
        const QString html = forkmesh::control::renderCloudUsageHtml(
            report, ranked, 10'000, tally.totalEvents(),
            1'787'176'860'000, true);
        check(html.contains(QLatin1String("Durable Object duration")) &&
                  html.contains(QLatin1String("85.0")) &&
                  html.contains(QLatin1String("alerted at 80%")),
              "the usage tab shows each free-plan meter and its alert level");
        check(html.contains(QLatin1String("Usage progression and estimate")) &&
                  html.contains(QLatin1String("Hourly trend (UTC)")) &&
                  html.contains(QLatin1String("Estimated at 24:00")) &&
                  html.contains(QLatin1String("00:00")) &&
                  html.contains(QLatin1String("21:00")),
              "the usage tab charts timestamped hourly progress and projects "
              "the UTC-day total");
        check(html.contains(QLatin1String("ForkMeshWorld")) &&
                  html.contains(QLatin1String("9,000.00")) &&
                  html.contains(QLatin1String("4,321")),
              "the usage tab names the class spending the duration allowance");
        check(html.contains(QLatin1String("Raw errors")) &&
                  html.contains(
                      QLatin1String("code-update and instance handoffs")) &&
                  html.contains(QLatin1String("excludes those expected handoffs")),
              "raw namespace errors are distinguished from actionable live errors");
        // A namespace added since the label map was written must still appear,
        // identified by its id rather than dropped for want of a name.
        check(html.contains(QLatin1String("bbb222")),
              "an unlabelled namespace is still listed under its id");
        check(html.contains(QLatin1String("Durable Object abort")) &&
                  html.contains(QLatin1String("42")),
              "mid-request platform aborts are called out, not buried");
        check(html.contains(busyId.left(12)) &&
                  html.contains(QLatin1String("83.3%")),
              "the live tail ranking shows each object's share of traffic");
        // No report is the ordinary state on a non-admin node; the tab has to
        // explain itself rather than look broken.
        const QString denied = forkmesh::control::renderCloudUsageHtml(
            QJsonObject{{QStringLiteral("ok"), false},
                        {QStringLiteral("error"), QStringLiteral("forbidden")}},
            {}, 0, 0, 40'000, false);
        check(denied.contains(QLatin1String("admin")) &&
                  denied.contains(QLatin1String("CLOUD log feed")),
              "a node without admin rights is told why usage is missing");
        // The relay rejecting a signature reads the same way, rather than as
        // an analytics outage the reader would go hunting for.
        check(forkmesh::control::renderCloudUsageHtml(
                  QJsonObject{
                      {QStringLiteral("ok"), false},
                      {QStringLiteral("error"),
                       QStringLiteral("unauthorized")}},
                  {}, 0, 0, 40'000, false)
                  .contains(QLatin1String("admin-only")),
              "a rejected admin signature reads as an auth problem");
        QLocale::setDefault(savedLocale);
    }

    // --- D1 tab: account-wide daily row budgets and direct storage usage.
    {
        const QLocale savedLocale = QLocale();
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
        const QJsonObject report{
            {QStringLiteral("ok"), true},
            {QStringLiteral("day"), QStringLiteral("2026-08-24")},
            {QStringLiteral("sampledAt"), 1'787'608'800'000.0},
            {QStringLiteral("budgets"),
             QJsonArray{
                 QJsonObject{{QStringLiteral("label"),
                              QStringLiteral("Rows read")},
                             {QStringLiteral("metric"),
                              QStringLiteral("rows-read")},
                             {QStringLiteral("unit"), QStringLiteral("rows")},
                             {QStringLiteral("allowance"), 5'000'000.0},
                             {QStringLiteral("used"), 1'250'000.0},
                             {QStringLiteral("percent"), 25.0}},
                 QJsonObject{{QStringLiteral("label"),
                              QStringLiteral("Rows written")},
                             {QStringLiteral("metric"),
                              QStringLiteral("rows-written")},
                             {QStringLiteral("unit"), QStringLiteral("rows")},
                             {QStringLiteral("allowance"), 100'000.0},
                             {QStringLiteral("used"), 50'000.0},
                             {QStringLiteral("percent"), 50.0}}}},
            {QStringLiteral("timeline"),
             QJsonArray{
                 QJsonObject{{QStringLiteral("at"),
                              QStringLiteral("2026-08-24T00:00:00Z")},
                             {QStringLiteral("rows-read"), 100'000.0},
                             {QStringLiteral("rows-written"), 5'000.0}},
                 QJsonObject{{QStringLiteral("at"),
                              QStringLiteral("2026-08-24T21:00:00Z")},
                             {QStringLiteral("rows-read"), 150'000.0},
                             {QStringLiteral("rows-written"), 4'000.0}}}},
            {QStringLiteral("queryInsights"),
             QJsonArray{QJsonObject{
                 {QStringLiteral("query"),
                  QStringLiteral("SELECT value FROM metrics WHERE key=?")},
                 {QStringLiteral("runs"), 42.0},
                 {QStringLiteral("totalRowsRead"), 25'200.0},
                 {QStringLiteral("avgRowsRead"), 600.0},
                 {QStringLiteral("totalRowsWritten"), 0.0},
                 {QStringLiteral("avgDurationMs"), 2.4},
                 {QStringLiteral("efficiency"), 0.125}}}},
            {QStringLiteral("storage"),
             QJsonObject{
                 {QStringLiteral("bytes"), 14'811'136.0},
                 {QStringLiteral("freeDatabaseLimitBytes"), 500'000'000.0},
                 {QStringLiteral("paidDatabaseLimitBytes"),
                  10'000'000'000.0},
                 {QStringLiteral("includedAccountStorageBytes"),
                  5'000'000'000.0}}},
            {QStringLiteral("databases"),
             QJsonArray{QJsonObject{
                 {QStringLiteral("databaseId"),
                  QStringLiteral("2d33ad9e-1ab2-4ca0-9075-6105d0e04d11")},
                 {QStringLiteral("rowsRead"), 1'250'000.0},
                 {QStringLiteral("rowsWritten"), 50'000.0},
                 {QStringLiteral("readQueries"), 1'200.0},
                 {QStringLiteral("writeQueries"), 80.0}}}}};
        const QString html = forkmesh::control::renderD1UsageHtml(
            report, 1'787'608'860'000, true);
        check(html.contains(QLatin1String("Rows read")) &&
                  html.contains(QLatin1String("1,250,000.0")) &&
                  html.contains(QLatin1String("5,000,000")) &&
                  html.contains(QLatin1String("25.0%")),
              "the D1 tab shows today's reads against the 5 million Free limit");
        check(html.contains(QLatin1String("Rows written")) &&
                  html.contains(QLatin1String("100,000")) &&
                  html.contains(QLatin1String("50.0%")),
              "the D1 tab shows writes against the 100,000 Free limit");
        check(html.contains(QLatin1String("14.8 MB")) &&
                  html.contains(QLatin1String("500.0 MB")) &&
                  html.contains(QLatin1String("5.00 GB")),
              "the D1 tab shows provider storage and both Free storage ceilings");
        check(html.contains(QLatin1String("2d33ad9e-1a")) &&
                  html.contains(QLatin1String("1,200")),
              "the D1 tab attributes daily usage to the individual database");
        check(html.contains(QLatin1String("Delayed estimate")) &&
                  html.contains(QLatin1String("account-wide")) &&
                  html.contains(QLatin1String("limit enforcement")),
              "the D1 tab does not present lagged analytics as authoritative");
        check(html.contains(QLatin1String("Usage progression and estimate")) &&
                  html.contains(QLatin1String("00:00")) &&
                  html.contains(QLatin1String("21:00")) &&
                  html.contains(QLatin1String("Estimated at 24:00")),
              "the D1 tab charts timestamped hourly row use and a midnight "
              "projection");
        check(html.contains(QLatin1String("Top D1 queries by rows read")) &&
                  html.contains(QLatin1String(
                      "SELECT value FROM metrics WHERE key=?")) &&
                  html.contains(QLatin1String("25,200")) &&
                  html.contains(QLatin1String("12.5%")),
              "the D1 tab ranks provider query insights with optimization "
              "metrics");
        check(!html.contains(QLatin1String("D1 read guard")),
              "the D1 tab has no guard panel without guard state");

        QJsonObject guarded = report;
        guarded.insert(
            QStringLiteral("readGuard"),
            QJsonObject{
                {QStringLiteral("mode"), QStringLiteral("auto")},
                {QStringLiteral("threshold"), 60.0},
                {QStringLiteral("conserving"), true},
                {QStringLiteral("reason"),
                 QStringLiteral("today's rows read reached 74.7% "
                                "(threshold 60%)")},
                {QStringLiteral("readers"),
                 QJsonArray{QJsonObject{
                     {QStringLiteral("name"),
                      QStringLiteral("users-directory")},
                     {QStringLiteral("label"),
                      QStringLiteral("Users directory joined rows")},
                     {QStringLiteral("normalTtlMs"), 300'000.0},
                     {QStringLiteral("conserveTtlMs"), 3'600'000.0},
                     {QStringLiteral("activeTtlMs"), 3'600'000.0}}}}});
        QJsonArray guardedInsights =
            guarded.value(QStringLiteral("queryInsights")).toArray();
        QJsonObject guardedQuery = guardedInsights.at(0).toObject();
        guardedQuery.insert(QStringLiteral("guard"),
                            QStringLiteral("users-directory"));
        guardedInsights.replace(0, guardedQuery);
        guarded.insert(QStringLiteral("queryInsights"), guardedInsights);
        const QString guardHtml = forkmesh::control::renderD1UsageHtml(
            guarded, 1'787'608'860'000, true);
        check(guardHtml.contains(QLatin1String("D1 read guard")) &&
                  guardHtml.contains(QLatin1String("Conserving")) &&
                  guardHtml.contains(QLatin1String("74.7%")),
              "the D1 tab says the read guard is conserving and why");
        check(guardHtml.contains(QLatin1String(
                  "forkmesh-d1-guard:set?mode=on&amp;threshold=60")) &&
                  guardHtml.contains(QLatin1String(
                      "forkmesh-d1-guard:set?mode=auto&amp;threshold=80")) &&
                  !guardHtml.contains(QLatin1String(
                      "forkmesh-d1-guard:set?mode=auto&amp;threshold=60")),
              "every other guard mode and threshold is a link; the current "
              "one is not");
        check(guardHtml.contains(QLatin1String("Users directory joined rows")) &&
                  guardHtml.contains(QLatin1String("5 min")) &&
                  guardHtml.contains(QLatin1String("1 h")),
              "the D1 tab lists each guarded reader's copy lifetimes");
        check(guardHtml.contains(QLatin1String("guarded: users-directory")),
              "a ranked statement names the guarded reader behind it");

        const QString guardWithoutAnalytics =
            forkmesh::control::renderD1UsageHtml(
                QJsonObject{{QStringLiteral("ok"), false},
                            {QStringLiteral("error"),
                             QStringLiteral("analytics_unconfigured")},
                            {QStringLiteral("readGuard"),
                             guarded.value(QStringLiteral("readGuard"))}},
                1'060'000, false);
        check(guardWithoutAnalytics.contains(
                  QLatin1String("forkmesh-d1-guard:set?mode=off")),
              "the read guard stays controllable without analytics");

        const QString partial = forkmesh::control::renderD1UsageHtml(
            QJsonObject{
                {QStringLiteral("ok"), false},
                {QStringLiteral("error"),
                 QStringLiteral("analytics_unconfigured")},
                {QStringLiteral("storage"),
                 QJsonObject{
                     {QStringLiteral("bytes"), 42.0},
                     {QStringLiteral("freeDatabaseLimitBytes"),
                      500'000'000.0}}}},
            1'060'000, false);
        check(partial.contains(QLatin1String("analytics token")) &&
                  partial.contains(QLatin1String("42 B")),
              "direct D1 storage remains visible without analytics credentials");
        QLocale::setDefault(savedLocale);
    }

    // --- D1 tab: a ranked provider query names the code that issues it.
    {
        const QString source = QString::fromUtf8(R"PY(async def load_repo(env, key):
    """Select the repository row for a key."""
    return await d1_first(
        env,
        "SELECT key_bi, owner_bi, data FROM repositories "
        "WHERE key_bi=? LIMIT 1",
        key,
    )


async def record_report(env, report_id, now):
    """Insert one normalized report and return its id."""
    await d1_run(
        env,
        """INSERT INTO bug_reports
               (report_id, created_at)
           VALUES (?, ?)""",
        report_id, now)
    await d1_run(env, "UPDATE users SET seen_at=42 WHERE user_bi='abc'")
)PY");
        const QList<forkmesh::D1QueryStatement> statements =
            forkmesh::scanD1QueryStatements(
                QStringLiteral("app/src/repositories.py"), source);
        check(statements.size() == 3,
              "only real statements are indexed: a docstring that opens with "
              "a SQL verb is not one");
        check(statements.size() == 3 && statements.at(0).line == 5 &&
                  statements.at(0).normalized ==
                      QLatin1String("select key_bi, owner_bi, data from "
                                    "repositories where key_bi=? limit ?"),
              "implicitly concatenated literals index as one statement at the "
              "line that opens it");
        check(statements.size() == 3 && statements.at(1).line == 15 &&
                  statements.at(1).normalized ==
                      QLatin1String("insert into bug_reports (report_id, "
                                    "created_at) values (?, ?)"),
              "a triple-quoted statement indexes across its lines");
        check(statements.size() == 3 && statements.at(2).line == 19 &&
                  statements.at(2).normalized ==
                      QLatin1String("update users set seen_at=? where "
                                    "user_bi=?"),
              "inline values and numbers collapse the way the provider "
              "collapses them");
        check(forkmesh::normalizeD1Query(
                  forkmesh::normalizeD1Query(
                      QStringLiteral("SELECT a FROM t -- why\n"
                                     "WHERE b='x' AND c=12"))) ==
                  QLatin1String("select a from t where b=? and c=?"),
              "normalization drops comments and is idempotent, so an "
              "already-normalized provider string survives it");

        const forkmesh::D1QuerySite located = forkmesh::matchD1QuerySite(
            QStringLiteral("SELECT key_bi, owner_bi, data FROM repositories "
                           "WHERE key_bi=? LIMIT 1"),
            statements);
        check(located.isValid() &&
                  located.path == QLatin1String("app/src/repositories.py") &&
                  located.line == 5 && located.otherSites == 0,
              "a provider query resolves to the line of the source statement");
        const forkmesh::D1QuerySite truncated = forkmesh::matchD1QuerySite(
            QStringLiteral("SELECT key_bi, owner_bi, data FROM repositories "
                           "WHERE key_b") +
                QString::fromUtf8("\xE2\x80\xA6"),
            statements);
        check(truncated.isValid() && truncated.line == 5,
              "the relay's 360-character cut still resolves, partial word "
              "and all");
        check(!forkmesh::matchD1QuerySite(
                   QStringLiteral("SELECT nothing FROM nowhere WHERE id=?"),
                   statements)
                   .isValid(),
              "a statement this checkout does not contain names no file");

        const QList<forkmesh::D1QueryStatement> rival{
            {QStringLiteral("app/src/a.py"), 10,
             QStringLiteral("select a, b, c from big_table where owner_bi=? "
                            "and state=?")},
            {QStringLiteral("app/src/b.py"), 20,
             QStringLiteral("select a, b, c from big_table where owner_bi=? "
                            "and kinds=?")}};
        check(!forkmesh::matchD1QuerySite(
                   QStringLiteral("SELECT a, b, c FROM big_table WHERE "
                                  "owner_bi=? AND other=?"),
                   rival)
                   .isValid(),
              "two places that fit a partial query equally well name neither");
        const QList<forkmesh::D1QueryStatement> twice{
            {QStringLiteral("app/src/entry.py"), 4798,
             QStringLiteral("select data from users where is_admin=? order by "
                            "user_bi limit ?")},
            {QStringLiteral("app/src/usage_reporting.py"), 321,
             QStringLiteral("select data from users where is_admin=? order by "
                            "user_bi limit ?")}};
        const forkmesh::D1QuerySite duplicated = forkmesh::matchD1QuerySite(
            QStringLiteral("SELECT data FROM users WHERE is_admin=1 ORDER BY "
                           "user_bi LIMIT 20"),
            twice);
        check(duplicated.isValid() &&
                  duplicated.path == QLatin1String("app/src/entry.py") &&
                  duplicated.otherSites == 1,
              "a statement written in two modules names one place and counts "
              "the other");

        QTemporaryDir checkout;
        check(checkout.isValid(), "the scan fixture checkout is available");
        const QString moduleDir =
            QDir(checkout.path()).filePath(QStringLiteral("app/src"));
        check(QDir().mkpath(moduleDir), "the fixture Worker source tree exists");
        QFile module(QDir(moduleDir).filePath(QStringLiteral("repositories.py")));
        check(module.open(QIODevice::WriteOnly), "the fixture module is writable");
        module.write(source.toUtf8());
        module.close();
        const QList<forkmesh::D1QueryStatement> indexed =
            forkmesh::buildD1QuerySourceIndex(
                {QStringLiteral("/nonexistent-checkout"), checkout.path()});
        check(indexed.size() == 3 &&
                  indexed.at(0).path ==
                      QLatin1String("app/src/repositories.py"),
              "the index scans the first root that holds app/src and reports "
              "repository-relative paths");

        const QString query =
            QStringLiteral("SELECT value FROM metrics WHERE key=?");
        QHash<QString, forkmesh::control::D1QuerySourceLink> sources;
        sources.insert(
            query,
            forkmesh::control::D1QuerySourceLink{
                QStringLiteral("fmlogsrc:4798:app/src/entry.py"),
                QStringLiteral("app/src/entry.py:4798"),
                QStringLiteral("+1 more place")});
        const QString linked = forkmesh::control::renderD1UsageHtml(
            QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("queryInsights"),
                 QJsonArray{QJsonObject{
                     {QStringLiteral("query"), query},
                     {QStringLiteral("runs"), 3.0},
                     {QStringLiteral("totalRowsRead"), 300.0}}}}},
            1'787'608'860'000, true, sources);
        check(linked.contains(
                  QLatin1String("href='fmlogsrc:4798:app/src/entry.py'")) &&
                  linked.contains(QLatin1String("app/src/entry.py:4798")) &&
                  linked.contains(QLatin1String("+1 more place")),
              "the ranked query carries an openable link to the code that "
              "issues it");
        check(forkmesh::control::renderD1UsageHtml(
                  QJsonObject{
                      {QStringLiteral("ok"), true},
                      {QStringLiteral("queryInsights"),
                       QJsonArray{QJsonObject{
                           {QStringLiteral("query"), query}}}}},
                  1'787'608'860'000, true)
                  .contains(QLatin1String("fmlogsrc:")) == false,
              "a query with no located source renders without a link");

        check(forkmesh::control::cloudflareD1DashboardUrl(
                  QStringLiteral("2d33ad9e-1ab2-4ca0-9075-6105d0e04d11")) ==
                  QLatin1String("https://dash.cloudflare.com/?to=/:account/"
                                "workers/d1/databases/"
                                "2d33ad9e-1ab2-4ca0-9075-6105d0e04d11"),
              "the live D1 view deep-links to the reported database through "
              "the account-agnostic dashboard form");
        check(forkmesh::control::cloudflareD1DashboardUrl(QString()) ==
                  QLatin1String("https://dash.cloudflare.com/?to=/:account/"
                                "workers/d1") &&
                  forkmesh::control::cloudflareD1DashboardUrl(
                      QStringLiteral("../../account/api-tokens")) ==
                      QLatin1String("https://dash.cloudflare.com/?to=/:account/"
                                    "workers/d1"),
              "an absent or unrecognisable database id opens the D1 list "
              "rather than an attacker-chosen dashboard path");
    }

    // --- Build tab: the static growth budgets that gate deploys.
    {
        const QLocale savedLocale = QLocale();
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
        const QJsonObject report{
            {QStringLiteral("ok"), true},
            {QStringLiteral("measurement"),
             QStringLiteral("uncompressed deployed source bytes")},
            {QStringLiteral("estimatedStartupSourceBytes"), 1'720'000.0},
            {QStringLiteral("attachedPythonBytes"), 3'450'000.0},
            {QStringLiteral("onDemandSourceBytes"), 1'800'000.0},
            {QStringLiteral("moduleCount"), 76.0},
            {QStringLiteral("staticAssetBytes"), 5'070'000.0},
            {QStringLiteral("staticAssetCount"), 53.0},
            {QStringLiteral("initialWorldModuleBytes"), 1'395'000.0},
            {QStringLiteral("initialWorldModuleCount"), 11.0},
            {QStringLiteral("largestStaticAsset"),
             QJsonObject{{QStringLiteral("name"),
                          QStringLiteral("world/public/world/world.js")},
                         {QStringLiteral("bytes"), 900'000.0}}},
            {QStringLiteral("staticLimits"),
             QJsonObject{{QStringLiteral("assetCount"), 20'000.0},
                         {QStringLiteral("maxAssetBytes"), 26'214'400.0},
                         {QStringLiteral("initialWorldModuleBytesSoft"),
                          2'650'000.0}}},
            {QStringLiteral("modules"),
             QJsonArray{QJsonObject{
                 {QStringLiteral("name"), QStringLiteral("entry.py")},
                 {QStringLiteral("bytes"), 1'480'000.0},
                 {QStringLiteral("phase"), QStringLiteral("startup")}}}}};
        const QString html =
            forkmesh::control::renderWorkerFootprintHtml(report, true);
        check(html.contains(QLatin1String("Estimated Python startup source")) &&
                  html.contains(QLatin1String("1.7 MB")) &&
                  !html.contains(QLatin1String("100.0%")),
              "the Build tab reports startup source without a Worker budget");
        check(html.contains(QLatin1String("Initial World module graph")) &&
                  html.contains(QLatin1String("World static asset count")) &&
                  html.contains(QLatin1String("world.js")),
              "the Build tab carries every enforced static growth budget");
        check(html.contains(QLatin1String("entry.py")) &&
                  html.contains(QLatin1String("startup")) &&
                  html.contains(QLatin1String("On-demand Python source")),
              "the Build tab attributes Python footprint and load phase");
        QLocale::setDefault(savedLocale);
    }

    // --- The Cron tab: what the maintenance loop is scheduled to do, what
    // the retained ticks say it did, and what one minute of it costs in
    // Durable Object invocations.
    {
        const QLocale savedLocale = QLocale();
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
        const QJsonObject report{
            {QStringLiteral("ok"), true},
            {QStringLiteral("sampledAt"), 3'600'000.0},
            {QStringLiteral("slot"), 60.0},
            {QStringLiteral("intervalMs"), 60'000.0},
            {QStringLiteral("trigger"),
             QJsonObject{{QStringLiteral("cron"), QStringLiteral("* * * * *")},
                         {QStringLiteral("batchObjects"), 6.0},
                         {QStringLiteral("subrequestCap"), 34.0},
                         {QStringLiteral("retainMs"), 21'600'000.0},
                         {QStringLiteral("phaseCount"), 38.0}}},
            {QStringLiteral("summary"),
             QJsonObject{{QStringLiteral("ticks"), 10.0},
                         {QStringLiteral("failedTicks"), 1.0},
                         {QStringLiteral("batches"), 14.0},
                         {QStringLiteral("phases"), 61.0},
                         {QStringLiteral("subrequests"), 220.0},
                         {QStringLiteral("lastTickAt"), 3'570'000.0},
                         {QStringLiteral("gapMs"), 30'000.0},
                         {QStringLiteral("objectsPerTick"), 1.40},
                         {QStringLiteral("phasesPerTick"), 6.10},
                         {QStringLiteral("unbatchedObjectsPerTick"), 7.10}}},
            {QStringLiteral("phases"),
             QJsonArray{QJsonObject{
                 {QStringLiteral("phase"), QStringLiteral("status-sample")},
                 {QStringLiteral("group"), QStringLiteral("status")},
                 {QStringLiteral("description"),
                  QStringLiteral("Sample every monitored system.")},
                 {QStringLiteral("cadence"), QStringLiteral("every minute")},
                 {QStringLiteral("nextDueSlot"), 61.0},
                 {QStringLiteral("runs"), 10.0},
                 {QStringLiteral("failures"), 1.0},
                 {QStringLiteral("lastFailure"), QStringLiteral("D1 timeout")},
                 {QStringLiteral("lastRunAt"), 3'570'000.0},
                 {QStringLiteral("lastMs"), 240.0},
                 {QStringLiteral("maxSubrequests"), 9.0},
                 {QStringLiteral("notes"),
                  QJsonArray{QStringLiteral("status chain")}}}}},
            {QStringLiteral("ticks"),
             QJsonArray{QJsonObject{
                 {QStringLiteral("slot"), 59.0},
                 {QStringLiteral("ts"), 3'570'000.0},
                 {QStringLiteral("batches"), 2.0},
                 {QStringLiteral("phases"), 7.0},
                 {QStringLiteral("subrequests"), 31.0},
                 {QStringLiteral("ms"), 1'400.0},
                 {QStringLiteral("ran"),
                  QJsonArray{QStringLiteral("status-sample"),
                             QStringLiteral("mirror-health")}},
                 {QStringLiteral("failed"),
                  QJsonArray{QStringLiteral("ap-outbox")}},
                 {QStringLiteral("skipped"),
                  QJsonArray{QStringLiteral("watchdog-completion")}}}}},
            {QStringLiteral("settings"),
             QJsonArray{QJsonObject{
                 {QStringLiteral("key"), QStringLiteral("batching")},
                 {QStringLiteral("label"), QStringLiteral("Phase batching")},
                 {QStringLiteral("value"),
                  QStringLiteral("24 phases per 34 subrequests")},
                 {QStringLiteral("detail"),
                  QStringLiteral("Longest prefix that fits one budget.")}}}}};
        const QString html =
            forkmesh::control::renderCronActivityHtml(report, 3'600'000, true);
        // The number the tab exists for: batching's saving is stated, not
        // left for the reader to divide out.
        check(html.contains(QLatin1String("Alarm invocations per tick")) &&
                  html.contains(QLatin1String("1.40")) &&
                  html.contains(QLatin1String("7.10 unbatched")) &&
                  html.contains(QLatin1String("80% saved")),
              "the cron tab states what a tick costs against the unbatched "
              "shape it replaced");
        check(html.contains(QLatin1String("status-sample")) &&
                  html.contains(QLatin1String("every minute")) &&
                  html.contains(QLatin1String("status chain")) &&
                  html.contains(QLatin1String("D1 timeout")),
              "each scheduled phase shows its cadence, notes and last "
              "failure");
        check(html.contains(QLatin1String("mirror-health")) &&
                  html.contains(QLatin1String("ap-outbox")) &&
                  html.contains(QLatin1String("skipped watchdog-completion")),
              "each retained tick shows what it ran, failed and skipped");
        check(html.contains(QLatin1String("Phase batching")) &&
                  html.contains(QLatin1String("24 phases per 34 "
                                              "subrequests")),
              "the relay-owned cron settings are listed with their values");
        // A phase due next minute reads as a wait, not as a slot number the
        // reader would have to convert.
        check(html.contains(QLatin1String("in 1 min")),
              "the next firing of each phase is shown as a wait");
        // Empty history is the ordinary state right after a deploy, and a
        // non-admin never gets one at all; neither may look like a broken tab.
        check(forkmesh::control::renderCronActivityHtml(
                  QJsonObject{{QStringLiteral("ok"), true},
                              {QStringLiteral("summary"), QJsonObject{}},
                              {QStringLiteral("ticks"), QJsonArray{}}},
                  3'600'000, false)
                  .contains(QLatin1String("No ticks recorded yet")),
              "a relay with no retained ticks says so");
        check(forkmesh::control::renderCronActivityHtml(
                  QJsonObject{{QStringLiteral("ok"), false},
                              {QStringLiteral("error"),
                               QStringLiteral("unauthorized")}},
                  3'600'000, false)
                  .contains(QLatin1String("admin-only")),
              "a rejected cron-report signature reads as an auth problem");
        QLocale::setDefault(savedLocale);
    }

    {
        // The Database tab signs exactly what app/src/admin_database.py
        // verifies; app/tests/test_admin_database.py pins the same bytes.
        forkmesh::D1DatabaseReadRequest read;
        read.op = QStringLiteral("rows");
        read.table = QStringLiteral("widgets");
        read.cursor = QStringLiteral("9");
        read.limit = QStringLiteral("50");
        read.filterColumn = QStringLiteral("label");
        read.filterMode = QStringLiteral("contains");
        read.filterValue = QStringLiteral("a b");
        read.order = QStringLiteral("asc");
        check(forkmesh::d1DatabaseReadCanonical(QStringLiteral("root"),
                                                QStringLiteral("1789400000000"),
                                                read) ==
                  QByteArray("forkmesh-admin-database-read-v1\nroot\n"
                             "1789400000000\nrows\nwidgets\n9\n50\nlabel\n"
                             "contains\na b\nasc"),
              "a database read signs every field in the Worker's order");
        check(forkmesh::d1DatabaseWriteCanonical(
                  QStringLiteral("root"), QStringLiteral("1789400000000"),
                  QStringLiteral("request-0000000001"),
                  QByteArray("{\"op\":\"delete\"}")) ==
                  QByteArray("forkmesh-admin-database-write-v1\nroot\n"
                             "1789400000000\nrequest-0000000001\n") +
                      QCryptographicHash::hash("{\"op\":\"delete\"}",
                                               QCryptographicHash::Sha256)
                          .toHex(),
              "a database write signs the digest of its exact payload bytes");

        read.filterValue = QStringLiteral("a+b&c=d");
        const QByteArray query = forkmesh::d1DatabaseReadQueryString(
            read, QStringLiteral("root"), QStringLiteral("1"),
            QStringLiteral("sig"), true);
        check(query.contains("filterValue=a%2Bb%26c%3Dd") &&
                  query.endsWith("&refresh=1"),
              "a filter value survives the query string byte for byte: a "
              "plus is not decoded as a space");

        const QString requestId = forkmesh::newD1DatabaseRequestId();
        check(QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{16,64}$"))
                      .match(requestId)
                      .hasMatch() &&
                  requestId != forkmesh::newD1DatabaseRequestId(),
              "write request ids are fresh and fit the Worker's pattern");

        check(forkmesh::d1DatabaseEditorValue(QStringLiteral(" 42 "),
                                              QStringLiteral("INTEGER")) ==
                      QJsonValue(qint64(42)) &&
                  forkmesh::d1DatabaseEditorValue(QStringLiteral("4x"),
                                                  QStringLiteral("INTEGER")) ==
                      QJsonValue(QStringLiteral("4x")) &&
                  forkmesh::d1DatabaseEditorValue(QStringLiteral("0012"),
                                                  QStringLiteral("TEXT")) ==
                      QJsonValue(QStringLiteral("0012")) &&
                  forkmesh::d1DatabaseEditorValue(QStringLiteral("1.5"),
                                                  QStringLiteral("REAL")) ==
                      QJsonValue(1.5),
              "edited cells follow SQLite affinity and keep unparsable text");
        check(forkmesh::d1DatabaseCellText(QJsonValue()) ==
                      QLatin1String("NULL") &&
                  forkmesh::d1DatabaseCellText(
                      QJsonValue(1789400000000.0)) ==
                      QLatin1String("1789400000000") &&
                  forkmesh::d1DatabaseCellText(QJsonObject{
                      {QStringLiteral("$bytes"), 12}}) ==
                      QLatin1String("<blob 12 bytes>"),
              "cells show NULL, whole numbers without exponents, and blobs "
              "by size");

        check(forkmesh::d1DatabaseQueryTables(QStringLiteral(
                  "SELECT a FROM users u JOIN \"nodes\" n ON 1 WHERE x IN "
                  "(SELECT y FROM users)")) ==
                  QStringList({QStringLiteral("users"),
                               QStringLiteral("nodes")}),
              "a query names each table it touches once");
        const QJsonObject usage{
            {QStringLiteral("queryInsights"),
             QJsonArray{
                 QJsonObject{{QStringLiteral("query"),
                              QStringLiteral("SELECT data FROM users WHERE "
                                             "user_bi=?")},
                             {QStringLiteral("totalRowsRead"), 10}},
                 QJsonObject{{QStringLiteral("query"),
                              QStringLiteral("UPDATE nodes SET seen=?")},
                             {QStringLiteral("totalRowsRead"), 999}},
                 QJsonObject{{QStringLiteral("query"),
                              QStringLiteral("INSERT INTO users (a) "
                                             "VALUES (?)")},
                             {QStringLiteral("totalRowsRead"), 50}},
             }}};
        const QJsonArray hot =
            forkmesh::d1DatabaseTableQueryInsights(usage, QStringLiteral("Users"));
        check(hot.size() == 2 &&
                  hot.at(0).toObject().value(QStringLiteral("totalRowsRead"))
                          .toInt() == 50,
              "a table's hot queries are the ones touching it, most rows "
              "read first");

        const QString migration = forkmesh::d1DatabaseDropMigration(
            {QStringLiteral("telemetry"), QStringLiteral("commit_inbox"),
             QStringLiteral("telemetry")});
        check(migration.contains(QLatin1String("schema.py")) &&
                  migration.indexOf(QLatin1String(
                      "DROP TABLE IF EXISTS \"commit_inbox\";")) <
                      migration.indexOf(QLatin1String(
                          "DROP TABLE IF EXISTS \"telemetry\";")) &&
                  migration.count(QLatin1String("DROP TABLE")) == 2,
              "the drop migration is one sorted statement per table, headed "
              "by the deploy order the audit requires");
        check(forkmesh::d1DatabaseAuditIsDropCandidate(
                  QStringLiteral("drop-after-readers")) &&
                  !forkmesh::d1DatabaseAuditIsDropCandidate(
                      QStringLiteral("custody")),
              "only drop verdicts are drop candidates");

        const QList<forkmesh::D1DatabaseTable> inventory =
            forkmesh::parseD1DatabaseInventory(QJsonObject{
                {QStringLiteral("tables"),
                 QJsonArray{QJsonObject{
                     {QStringLiteral("name"), QStringLiteral("visitors")},
                     {QStringLiteral("rows"), 7},
                     {QStringLiteral("writable"), false},
                     {QStringLiteral("rowid"), false},
                     {QStringLiteral("gaps"),
                      QJsonArray{QStringLiteral("plaintext secret-named "
                                                "column: deposit_secret")}},
                     {QStringLiteral("columns"),
                      QJsonArray{QJsonObject{
                          {QStringLiteral("name"), QStringLiteral("day")},
                          {QStringLiteral("type"), QStringLiteral("TEXT")},
                          {QStringLiteral("primaryKey"), true}}}},
                 }}}});
        check(inventory.size() == 1 && inventory.first().rows == 7 &&
                  !inventory.first().rowid &&
                  inventory.first().gaps.size() == 1 &&
                  inventory.first().columns.size() == 1 &&
                  inventory.first().columns.first().primaryKey,
              "the inventory parses rows, rowid-less tables, gaps and "
              "columns");
    }

    {
        // An administrator erasing a repository this machine holds no copy of
        // signs exactly what the DELETE branch of catalog_handler verifies;
        // app/tests/test_admin_repo_purge.py pins the same bytes.
        check(forkmesh::catalogAdminDeleteCanonical(
                  QStringLiteral("Root"), QStringLiteral("Alice"),
                  QStringLiteral("Demo"),
                  QStringLiteral("1789400000000")) ==
                  QByteArray("forkmesh-admin-repo-delete-v1\nroot\nalice\n"
                             "demo\n1789400000000"),
              "an admin repository purge signs the administrator, the target "
              "and the timestamp, all lowercased like the relay");
        check(forkmesh::catalogAdminDeleteCanonical(
                  QStringLiteral("root"), QStringLiteral("alice"),
                  QStringLiteral("demo"),
                  QStringLiteral("1789400000000")) !=
                  forkmesh::catalogAdminDeleteCanonical(
                      QStringLiteral("root"), QStringLiteral("alice"),
                      QStringLiteral("other"),
                      QStringLiteral("1789400000000")),
              "a purge proof cannot be re-aimed at another repository");
        check(!forkmesh::catalogAdminDeleteCanonical(
                   QStringLiteral("root"), QStringLiteral("alice"),
                   QStringLiteral("demo"), QStringLiteral("1"))
                   .contains("forkmesh-catalog-delete-v1"),
              "the admin purge is domain-separated from an owner's own "
              "catalog delete");
        // signData() produces base64url without padding, so the signature
        // carries no character a query string would re-spell (a literal '+'
        // would arrive at the Worker's parse_qs as a space).
        const QString query =
            forkmesh::catalogAdminDeleteQuery(
                QStringLiteral("Root"), QStringLiteral("Alice"),
                QStringLiteral("Demo"), QStringLiteral("1789400000000"),
                QStringLiteral("ab-_cd"))
                .toString(QUrl::FullyEncoded);
        check(query == QLatin1String("owner=alice&name=demo&user=root"
                                     "&ts=1789400000000&sig=ab-_cd"),
              "the purge names the account as `user`, lowercases the target "
              "and passes the signature through unchanged");
    }

    if (failures == 0)
        std::fprintf(stdout, "control-node tests passed\n");
    checkSyncStatusFixture();

    return failures == 0 ? 0 : 1;
}
