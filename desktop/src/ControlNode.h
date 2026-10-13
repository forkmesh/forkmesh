#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QProcessEnvironment>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>

class QSettings;

namespace forkmesh::control {

struct CloudflareBootstrapRequest {
    QString hostname;
    QString zoneName;
    QString accountId;
    QString nodeName;
    QString relayLabel;
    QString mainRelayUrl;
    bool dryRun = false;
};

struct CloudflareBootstrapCommand {
    QString program;
    QStringList arguments;
    QProcessEnvironment environment;
};

// Ephemeral request sent by the desktop controller to a saved mirror host.
// Variable values deliberately live only in this in-memory request and the
// SSH stdin payload. They must never be added to argv, QSettings, process
// output, or the mirror's signed public catalog.
struct MirrorActionsConfigurationRequest {
    QString requestId;
    QString host;
    QString sshUser;
    QString nodeName;
    bool actionsEnabled = false;
    bool replaceVariables = false;
    QMap<QString, QString> variables;
};

struct MirrorActionsSshCommand {
    QString program;
    QStringList arguments;
    QProcessEnvironment environment;
    QByteArray standardInput;
};

// A direct controller-to-host SSH command. Host verification always uses
// OpenSSH's persistent known_hosts database in accept-new (TOFU) mode: a first
// key is retained, and a later mismatch fails closed. An empty password selects
// the user's SSH agent/default keys; a non-empty password is exposed only to
// sshpass through SSHPASS and public-key authentication is still attempted
// first.
struct HostSshCommand {
    QString program;
    QStringList arguments;
    QProcessEnvironment environment;
};

QString sharedHostKeyDirectory();

// Path of the one ForkMesh-managed private key every host in the fleet
// authorizes. Devices that still hold the older per-provider key file keep
// using that file, so mirrors provisioned before the switch stay reachable.
// The file itself may not exist yet; sharedHostKeyPath() only names it.
QString sharedHostKeyPath();

QString existingSharedHostIdentityFile();

// Build the common authenticated transport used by install, logs, uninstall,
// and Actions. The caller owns remoteCommand, which must not contain credentials.
// identityFile pins authentication to one ForkMesh-managed private key — the
// shared fleet key, or whatever key a saved host recorded; the path is public
// metadata, the key material never leaves disk.
HostSshCommand buildHostSshCommand(const QString &host,
                                   const QString &sshUser,
                                   const QString &sshPassword,
                                   const QString &remoteCommand,
                                   QString *error = nullptr,
                                   const QString &identityFile = QString());

// Same authenticated transport as buildHostSshCommand, but with a remote TTY
// forced (-tt). ssh only allocates one automatically for a bare login; a call
// that carries a remote command needs this before any interactive remote
// program — a provider sign-in prompt, for instance — can read a keystroke.
HostSshCommand buildHostInteractiveSshCommand(
    const QString &host, const QString &sshUser, const QString &sshPassword,
    const QString &remoteCommand, QString *error = nullptr,
    const QString &identityFile = QString());

HostSshCommand buildHostLoginShellSshCommand(
    const QString &host, const QString &sshUser, const QString &sshPassword,
    QString *error = nullptr, const QString &identityFile = QString());

QString hostSshCommandLine(const HostSshCommand &command);

// Fixed remote receiver used by the release controller before it advances a
// managed mirror's refs. The artifact bytes arrive on stdin and are accepted
// into that mirror's CAS only when their digest matches the signed manifest.
// An empty result means sha256 was not an exact hex digest.
QString managedReleaseArtifactSeedCommand(const QString &sha256);

// Fixed read-only inventory probe for the same managed release CAS. It emits
// one requested digest per line only when that object is absent or corrupt, so
// an ordinary repository reconciliation can repair historical artifacts
// without streaming every large blob on every safety pass. An empty result
// means the input is empty, too large, duplicated, or contains a non-digest.
QString managedReleaseArtifactInventoryCommand(const QStringList &sha256s);

// The interactive shell an operator lands in after ForkMesh installs the agent
// CLIs on a mirror. It puts both user-scoped install prefixes on PATH, prints
// the two provider sign-in commands, and then execs a login shell so
// `claude` /login and `codex login` can be completed by hand — the installer
// deliberately copies no tokens, so this is where credentials are entered. The
// command itself never carries a credential.
QString buildHostAgentLoginRemoteCommand();

// Stable, metadata-only key for a password retained in MainWindow memory for
// this process lifetime. It is never written to QSettings.
QString savedHostCredentialKey(const QString &nodeName, const QString &host,
                               const QString &sshUser);

struct HostDiskEntry {
    QString name;      // basename, as shown in the size map
    QString path;      // absolute remote path
    qint64 bytes = 0;  // disk usage in bytes
    bool directory = false;
};

struct HostDiskUsage {
    QString path;
    qint64 totalBytes = 0;         // disk usage of `path` itself
    QList<HostDiskEntry> entries;  // children, largest first
    QString error;                 // non-empty when the host refused the read
    bool complete = false;         // the end sentinel arrived
};

struct HostMountUsage {
    QString path;                 // absolute mount point, e.g. "/" or "/data"
    qint64 totalBytes = 0;
    qint64 usedBytes = 0;
    qint64 availableBytes = 0;
};

struct HostMountUsageList {
    QList<HostMountUsage> mounts;
    QString error;
    bool complete = false;
};

QString normalizeRemoteDiskPath(const QString &path);

QString buildHostDiskUsageCommand(const QString &path,
                                  QString *error = nullptr);

QString buildHostMountUsageCommand();
HostMountUsageList parseHostMountUsage(const QByteArray &output);

HostDiskUsage parseHostDiskUsage(const QByteArray &output,
                                 const QString &path);

QString formatDiskSize(qint64 bytes);

QString sshConnectionFailureHint(int exitCode, const QString &outputTail,
                                 const QString &host = QString());

// True when a failed SSH run was rejected for its credentials, so asking the
// operator for the host's SSH password and retrying can actually succeed. Only
// the credential rejections qualify: a timeout, a refused port or a changed
// host key all exit 255 too, and no password fixes any of them.
bool sshFailureNeedsPassword(int exitCode, const QString &outputTail);

// Describe the non-routable IPv4 range `host` falls in (RFC 1918 private,
// RFC 6598 carrier-grade NAT, link-local, loopback), or an empty string when it
// is a routable address or not an IPv4 literal at all.
QString nonRoutableAddressNote(const QString &host);

QString sshFailureSummary(int exitCode, const QString &outputTail);

// Load saved host metadata and atomically migrate legacy plaintext password
// fields out of QSettings. When supplied, sessionPasswords receives those
// values in memory so the current app session is not interrupted. saveSavedHosts
// also strips password-like fields defensively before persisting.
QJsonArray loadSavedHosts(QSettings &settings, const QString &settingsKey,
                          QHash<QString, QString> *sessionPasswords = nullptr);
void saveSavedHosts(QSettings &settings, const QString &settingsKey,
                    const QJsonArray &hosts);

// Transitional and failed host records remain visible for recovery, but they
// are not safe targets for unattended mirror synchronization or deployment.
bool savedHostMayBeActive(const QJsonObject &host);

// A provisioning run can report failure (a timeout, a transient verification
// step) after the machine actually finished installing, and nothing else ever
// rewrites that label. Such a record, or an "installing" one abandoned longer
// than any install runs, is worth probing: live health can prove it wrong.
// statusChangedAt (epoch ms) dates the label; a record without one predates
// the stamp, so an "installing" label on it is treated as abandoned.
constexpr qint64 kSavedHostInstallingStaleMs = 2LL * 60 * 60 * 1000;
bool savedHostInstallStatusMayBeStale(const QJsonObject &host, qint64 nowMs);

// Returns savedHosts with every stale-labelled host (see above) whose node the
// mirrors array (catalog /mirrors rows or verified direct health proofs) shows
// online, integrity ok, and endpoint healthy and fresh promoted to
// "installed", stamped with statusChangedAt = nowMs. promotedNodes receives
// the promoted host names; an empty list means nothing changed, so callers
// persist only when it is non-empty.
QJsonArray promoteSavedHostsProvenHealthy(const QJsonArray &savedHosts,
                                          const QJsonArray &mirrors,
                                          qint64 nowMs,
                                          QStringList *promotedNodes);

QString validateMirrorActionsConfigurationRequest(
    const MirrorActionsConfigurationRequest &request);

// Serialize the exact v1 request consumed by a remote ForkMesh Actions helper.
// Secrets appear only in the returned stdin bytes. Callers should overwrite
// and clear that byte array immediately after QProcess::write().
QByteArray buildMirrorActionsConfigurationPayload(
    const MirrorActionsConfigurationRequest &request,
    QString *error = nullptr);

// Build a direct SSH invocation. With a password, sshpass reads it only from
// SSHPASS. Without a password, OpenSSH uses the user's agent/default keys in
// non-interactive public-key mode. The fixed remote helper command and all argv
// fields are secret-free; the configuration payload is standardInput only.
MirrorActionsSshCommand buildMirrorActionsSshCommand(
    const MirrorActionsConfigurationRequest &request,
    const QString &sshPassword,
    QString *error = nullptr,
    const QString &identityFile = QString());

QJsonObject parseMirrorActionsConfigurationResult(
    const QByteArray &output, const QString &expectedRequestId,
    const QString &expectedNodeName, QString *error = nullptr);

QString validateCloudflareBootstrapRequest(
    const CloudflareBootstrapRequest &request,
    bool allowAutomaticTopology = false);

QString findCloudflareBootstrapScript(
    const QString &sourceDir = QString(),
    const QString &applicationDir = QString());

QString findCloudflareWorkerDirectory(
    const QString &sourceDir = QString(),
    const QString &applicationDir = QString());

QString findSiteDeployScript(
    const QString &sourceDir = QString(),
    const QString &applicationDir = QString());

// The bash argument list for one site deployment. deploy.sh skips a target
// whose live fingerprint already matches its local inputs; force appends the
// script's --force flag so the target is uploaded anyway. An unknown target
// yields an empty list, so a bad caller cannot hand deploy.sh a stray word.
QStringList siteDeployArguments(const QString &script, const QString &target,
                                bool force);

// A portable, clear-text recovery/deployment bundle for a complete desktop
// control plane. It deliberately contains every QSettings value, every shared
// Actions variable/secret, the complete application-data tree (including
// identity and SSH keys), the native settings file, and .env.production when
// present. Repository working copies outside application data are referenced
// by settings rather than copied; the installed application binary is also not
// data and must already exist on the destination machine.
//
// The JSON is intentionally human-readable, while each setting also carries an
// exact Qt serialization so QByteArray and other non-JSON QVariant types round
// trip without losing their type. Callers must write it owner-only and warn the
// operator that all secrets are present in clear text.
//
// Entries a JSON file cannot hold at all — FIFOs, sockets, device nodes and
// symbolic links, which an Actions sandbox or an agent worktree can leave
// inside application data — are skipped and listed in the bundle's "skipped"
// array rather than failing the export.
QJsonObject createCompleteSystemBundle(
    QSettings &settings, const QMap<QString, QString> &variables,
    const QString &applicationDataDirectory, const QString &settingsFile,
    const QString &workerEnvironmentFile, const QJsonObject &deployment,
    QString *error = nullptr,
    const QStringList &excludedApplicationDataNames = {});

bool validateCompleteSystemBundle(const QJsonObject &bundle,
                                  QString *error = nullptr);
bool writeCompleteSystemBundle(const QString &path, const QJsonObject &bundle,
                               QString *error = nullptr);
QJsonObject readCompleteSystemBundle(const QString &path,
                                     QString *error = nullptr);
QMap<QString, QString> completeSystemBundleVariables(
    const QJsonObject &bundle, bool *ok = nullptr);
QVariant completeSystemBundleSetting(const QJsonObject &bundle,
                                     const QString &key, bool *ok = nullptr);
// Merge all imported settings into the current store. Existing keys absent
// from the bundle are retained so importing a deployment bundle into a running
// newer app cannot erase newly introduced local-only preferences.
bool mergeCompleteSystemBundleSettings(const QJsonObject &bundle,
                                       QSettings &settings,
                                       QString *error = nullptr);

// One top-level entry of the application-data tree, as the export page lists
// it before anything is written. bytes/fileCount are exactly what the bundle
// would carry for that entry; skippedEntries counts the FIFOs, sockets and
// symbolic links inside it that no self-contained JSON can carry.
struct CompleteSystemDataEntry {
    QString name;
    bool directory = false;
    int fileCount = 0;
    qint64 bytes = 0;
    int skippedEntries = 0;
    bool bulk = false;
};

// True for the top-level application-data names this desktop can rebuild from
// the mesh or from a rerun (served mirrors, archives, CI sandboxes, agent
// scratch, caches). They are surveyed and shown, but left unchecked by default
// so a recovery JSON stays a few megabytes instead of many gigabytes.
bool completeSystemDataIsBulk(const QString &topLevelName);

// Walk application data once and report per-top-level-entry totals. Symbolic
// links are never followed.
QList<CompleteSystemDataEntry> surveyCompleteSystemApplicationData(
    const QString &applicationDataDirectory);

QString findCloudflareTunnelBootstrapScript(
    const QString &sourceDir = QString(),
    const QString &applicationDir = QString());
QString findMirrorGatewayScript(
    const QString &sourceDir = QString(),
    const QString &applicationDir = QString());
QString findMcpServerScript(
    const QString &sourceDir = QString(),
    const QString &applicationDir = QString());
QString findCloudflaredInstallerScript(
    const QString &sourceDir = QString(),
    const QString &applicationDir = QString());
QString findNextAutoscaleScript(
    const QString &sourceDir = QString(),
    const QString &applicationDir = QString());
QString findNextAutodeployScript(
    const QString &sourceDir = QString(),
    const QString &applicationDir = QString());
QString findNextFleetScript(
    const QString &sourceDir = QString(),
    const QString &applicationDir = QString());

// Build the one-click Tunnel/DNS/manifest provisioning command. The API token
// remains in the child environment. The connector token is written only to the
// explicit owner-only local path so Qt can start/stop cloudflared without ever
// putting that credential in argv.
CloudflareBootstrapCommand buildCloudflareTunnelBootstrapCommand(
    const CloudflareBootstrapRequest &request,
    const QString &mirrorHostname,
    const QString &apiToken,
    const QString &scriptPath,
    const QString &pythonProgram,
    const QString &signerProgram,
    const QString &nodePublicKey,
    const QString &gatewayConfigPath,
    const QString &manifestOutputPath,
    const QString &connectorTokenPath);

CloudflareBootstrapCommand buildCloudflareBootstrapCommand(
    const CloudflareBootstrapRequest &request,
    const QString &apiToken,
    const QString &scriptPath,
    const QString &pythonProgram,
    const QString &signerProgram,
    const QString &nodePublicKey);

// Resolve the Cloudflare credentials this node already stores as action
// variables/secrets (Settings > Secrets & Coves) so live tooling can reuse the
// token the deploy workflow authenticates with instead of asking for a fresh
// one. Names are matched case-insensitively; blank or multi-line values are
// ignored so a pasted credential file can never smuggle extra lines into the
// child environment.
QString cloudflareApiTokenFromVariables(
    const QMap<QString, QString> &variables);
QString cloudflareAccountIdFromVariables(
    const QMap<QString, QString> &variables);

constexpr int kWranglerMinimumNodeMajor = 22;

struct NodeToolchain {
    QString npx;     // absolute npx to run
    QString binDir;  // MUST lead the child's PATH: both npx and the wrangler bin
    int majorVersion = 0;
    bool isValid() const { return !npx.isEmpty(); }
};

QStringList nodeBinDirectoryCandidates();

int nodeMajorVersionFromText(const QString &text);

NodeToolchain findNodeToolchain(int minimumMajor);

CloudflareBootstrapCommand buildCloudflareTailCommand(
    const QString &apiToken,
    const QString &accountId,
    const QString &npxProgram,
    const QString &nodeBinDir = QString());

// One decoded event of `wrangler tail --format json`. The viewer renders these
// itself (rather than letting Wrangler pretty-print) because the pretty format
// drops request headers — and the user agent behind each hit is exactly what
// the cloud log is read for. `summary` follows Wrangler's own default line
// where Wrangler has one, so the log reads the way `wrangler tail` does; where
// it has none it describes the event rather than repeating Wrangler's
// "Unknown Event" (adhoc #1635).
struct CloudflareTailEvent {
    bool parsed = false;    // false for banner/plain lines: show them verbatim
    // An exception, a non-ok outcome, a 5xx, or error-level logs — less the
    // shapes that only look like faults. Chief among those: a hibernated
    // socket's close delivered to a Durable Object instance that had already
    // ended, which is the runtime saying so rather than the Worker breaking.
    bool isError = false;
    // Cloudflare necessarily terminates a Durable Object's hibernated sockets
    // when that class is replaced.  It reports the termination as an
    // exception/1006 even though clients are expected to reconnect.  Keep the
    // event visible, but separate it from faults and alert counts.
    bool isDeployInterruption = false;
    QString summary;        // the rendered one-line form for the viewer
    QString method;
    QString url;
    QString userAgent;      // request user-agent header, empty when absent
    QString outcome;
    int status = 0;         // response status, 0 when the event carries none
    QStringList messages;   // console logs and exception text, newest first
    // The Worker's "D1 <kind>: <sql>" console lines (_log_d1_query in
    // app/src/entry.py), kept out of `messages` and the summary so the log can
    // file each statement under its own DB category.
    QStringList d1Statements;
    // What woke the Worker: "fetch", "scheduled", "alarm", "queue", "email",
    // "rpc", "tail", "tail-info", "websocket", or "other" for a shape nobody
    // here has a name for. Wrangler calls everything past its own eight shapes
    // "Unknown Event", which is what a Durable Object's hibernated WebSocket
    // traffic lands on — and that is most of what the relay does (adhoc #1635).
    QString kind;
    QString scriptName;     // the Worker the event ran in, empty when absent
    QString entrypoint;     // the class inside it (a Durable Object, for one)
    // What the event cost and where it ran. Wrangler's pretty printer drops all
    // of these, and they are what a cloud line is actually read for once the
    // shape of the event is known: a median of 5ms beside a one-second outlier
    // is a cold start rather than a hot loop, and a Worker version pins whether
    // the deploy you are looking for is the one that served the hit (#1665).
    double cpuTimeMs = -1;   // Worker CPU time, negative when the event has none
    double wallTimeMs = -1;  // wall time inside the Worker, negative when absent
    QString colo;            // Cloudflare data centre that took a request
    QString country;         // client country, from the request's cf object
    QString city;            // client city, when Cloudflare included it
    QString region;          // client region/state, when included
    QString clientAddress;   // edge-observed client address, when included
    QString clientIdentity;  // authenticated owner inferred from a safe route
    QString requestOrigin;   // request Origin header, empty when absent
    QString asn;              // client network ASN, when included
    QString asOrganization;  // client network operator, when included
    QString httpProtocol;     // HTTP/1.1, HTTP/2, HTTP/3, when included
    QString tlsVersion;       // negotiated TLS version, when included
    int clientTcpRttMs = -1;  // edge-observed RTT, negative when absent
    QString scriptVersion;   // deployed version (tag, message, or short id)
    QString durableObjectId;  // object instance id, shortened only at render
    QString requestId;        // Cloudflare invocation id, when included
    QString traceId;          // trace id for cross-event correlation
    bool truncated = false;  // Cloudflare dropped logs from this event
    // Cloudflare's own fault rather than the Worker's. The runtime raises
    // "internal error; reference = <id>" — and, on the free tier, "Exceeded
    // allowed duration in Durable Objects free tier." — from underneath the
    // Worker: no handler frame produced either, so there is no traceback to go
    // hunting for and no try/except that could have caught it. The relay
    // already classes this exact shape as a transient platform blip wherever
    // it has a caller that can retry (_is_transient_d1_error and
    // log_durable_object_abort in app/src/entry.py); a hibernated WebSocket
    // wake-up has no such caller, so the fault reaches the tail instead and
    // the viewer has to name it or the line reads as an unexplained Worker
    // bug. The reference is the only handle on the incident — it is what
    // Cloudflare support looks it up by — so it is lifted out of the exception
    // text into a field of its own.
    bool platformFault = false;
    QString platformFaultReference;  // empty when the fault carries none
};

QStringList takeCloudflareTailRecords(QByteArray *buffer);

CloudflareTailEvent parseCloudflareTailLine(const QString &line);

// One "D1 <kind>: <sql> [N args] (for <caller>) (from <file>.py in
// <function>)" console line, split apart. The Worker appends the origin clause
// (_d1_query_origin in app/src/entry.py); a build from before it leaves both
// origin fields empty.
struct D1TailStatement {
    QString kind;            // "query", "primary query", "batch", ...
    QString sql;             // statement text, " [N args]" suffix included
    QString originFile;      // Worker module file name, e.g. "repositories.py"
    QString originFunction;  // the function in it that issued the statement
    QString caller;
};

D1TailStatement parseD1TailStatement(const QString &line);

QString takeD1StatementCaller(QString *sql);

// Who asked for the work an event did, for lines filed apart from the hit
// itself (a D1 statement): the request, its agent, client address, Origin,
// owner and country for a fetch; the wake-up kind and the class that ran it
// for everything else. Fields are joined with " · ".
QString cloudflareTailRequester(const CloudflareTailEvent &event);

constexpr qint64 kCloudTailHealthyUptimeMs = 60'000;

struct CloudTailRestartPlan {
    bool restart = false;  // start another tail
    int delayMs = 0;       // after waiting this long
    bool giveUp = false;   // retrying cannot help: stop, and say so once
};

CloudTailRestartPlan planCloudflareTailRestart(int consecutiveFailures,
                                               qint64 uptimeMs);

bool cloudflareTailWorkerMissing(const QString &line);
QString cloudflareWorkerName(const QString &workerDirectory);

// Running per-object aggregation of the CLOUD tail: which Worker, class, and
// Durable Object instance the observed events belong to, with error and cost
// totals. Feeds the log area's Usage tab alongside the account-wide report
// from /api/accounts/admin-usage-report — the tail is what supplies
// object-level granularity, which Cloudflare's analytics API does not expose.
struct CloudUsageLiveEntry {
    QString scriptName;
    QString entrypoint;       // Durable Object class, empty for a plain hit
    QString durableObjectId;  // instance id, empty when the event carries none
    bool overflow = false;    // the bounded tally's "everything else" bucket
    qint64 events = 0;
    qint64 errors = 0;          // isError events; deploy handoffs excluded
    qint64 platformFaults = 0;  // faults Cloudflare raised from underneath
    double wallTimeMs = 0;      // summed where the event carried a figure
    double cpuTimeMs = 0;
    qint64 lastSeenMs = 0;
};

class CloudUsageTally {
public:
    void record(const CloudflareTailEvent &event, qint64 nowMs);
    // Entries ranked busiest-first; the overflow bucket, if any, comes last.
    QList<CloudUsageLiveEntry> ranked(int limit) const;
    qint64 totalEvents() const { return m_totalEvents; }
    qint64 sinceMs() const { return m_sinceMs; }
    bool isEmpty() const { return m_entries.isEmpty(); }
    void clear();

private:
    QHash<QString, CloudUsageLiveEntry> m_entries;
    qint64 m_totalEvents = 0;
    qint64 m_sinceMs = 0;
};

// The identifier-free route label for an /api path: the same grouping
// app/src/api_metrics.py _masked_group applies ("accounts/*", "version",
// "repo/forkmesh/forkmesh/content", "other", ...), so a label reads the same
// on the desktop and in the Worker's endpoint catalog.
QString webRequestRouteGroup(const QString &path);

// Per-route request statistics behind the Network page's Web Requests tab,
// counted from the CLOUD tail's /api fetch events. Nothing about this traffic
// is stored server-side (the Worker's api_metrics_minute table was dropped in
// migration 0190): the numbers exist only in this process, for the last
// kMaxMinutes, and only while the tail runs.
class WebRequestTally {
public:
    static constexpr int kMaxMinutes = 1440;
    // Route labels are masked, so real cardinality is a few dozen; past this
    // cap a new label folds into "other", as the Worker's grouper did.
    static constexpr int kMaxGroups = 200;

    // Counts a fetch of an /api path handled by the Worker itself (not a
    // Durable Object's internal request). Returns whether it was counted.
    bool record(const CloudflareTailEvent &event, qint64 nowMs);
    bool recordBatch(const QString &group, const QString &statusClass,
                     qint64 count, qint64 durationSumMs, qint64 durationMaxMs,
                     qint64 atMs, qint64 nowMs);
    // The window as the tab renders it: {ok, now, windowMinutes,
    // countingSinceMs, minutes[{t, requests, errors}], classes,
    // groups[{group, requests, errors, avg_ms, dur_ms_max, classes}]}.
    QJsonObject summary(int windowMinutes, qint64 nowMs) const;
    qint64 sinceMs() const { return m_sinceMs; }
    bool isEmpty() const { return m_minutes.isEmpty(); }
    void clear();

private:
    struct Bucket {
        qint64 requests = 0;
        qint64 durationSumMs = 0;
        qint64 durationMaxMs = 0;
    };
    QString admitGroup(QString group);
    void prune(qint64 nowMs);
    // minute start -> "<group>\n<status class>" -> bucket
    QMap<qint64, QHash<QString, Bucket>> m_minutes;
    QSet<QString> m_groups;
    qint64 m_sinceMs = 0;
};

// The Usage tab's document: free-plan budget meters and the per-namespace /
// per-Worker breakdown from the admin usage report (or the reason there is
// none), followed by the live-tail ranking above. Pure of any widget state so
// it can be exercised in tests.
QString renderCloudUsageHtml(const QJsonObject &report,
                             const QList<CloudUsageLiveEntry> &live,
                             qint64 liveSinceMs, qint64 totalLiveEvents,
                             qint64 nowMs, bool dark);

// Where one ranked query is issued from in the checkout, resolved by the
// caller (forkmesh::matchD1QuerySite) so this renderer stays free of disk
// access. `href` is the anchor the hosting view's click handler opens.
struct D1QuerySourceLink {
    QString href;
    QString label; // "app/src/entry.py:4798"
    QString note;  // "+2 more places", or empty
};

// The D1 tab's document: account-wide daily row allowances from Cloudflare
// analytics, a per-database attribution table, and the bound database's direct
// provider storage measurement against its Free ceiling. `querySources` maps a
// ranked query's provider text to the place it is issued from; a query with no
// entry simply carries no link.
QString renderD1UsageHtml(
    const QJsonObject &report, qint64 nowMs, bool dark,
    const QHash<QString, D1QuerySourceLink> &querySources = {});

// The Cloudflare dashboard page that streams D1 activity for a database, or
// the D1 list when no single database is identified. `?to=/:account/...` is
// the account-agnostic deep-link form Cloudflare documents, so the operator's
// signed-in account resolves itself.
QString cloudflareD1DashboardUrl(const QString &databaseId);

// The Build tab's document: the exact compact Python and World-static
// measurements validated during the deployed App Worker build, shown against
// each enforced growth budget with the largest Python modules for attribution.
QString renderWorkerFootprintHtml(const QJsonObject &report, bool dark);

// The Cron tab's document, built from /api/accounts/admin-cron-report: what
// one minute of maintenance costs in Durable Object invocations against what
// it cost before batching, the retained per-tick history, the declared phase
// schedule, and the relay-owned switches that decide what a tick delivers (or
// the reason there is no report). Pure of any widget state, so it can be
// exercised in tests.
QString renderCronActivityHtml(const QJsonObject &report, qint64 nowMs,
                               bool dark);

// Decode the bootstrapper's bounded, non-secret machine result. Human log
// output may surround the sentinel line; malformed or duplicate results fail
// closed.
QJsonObject parseCloudflareBootstrapResult(const QByteArray &output,
                                           QString *error = nullptr);

// Sanitize child-process output before it is shown or copied. Exact in-memory
// secrets are removed first, followed by common credential-shaped assignments.
QString redactProcessOutput(const QString &text,
                            const QStringList &exactSecrets = {});

// A Solana wallet connection in ForkMesh is a public address only. Validation
// decodes base58 and requires exactly 32 bytes; private keys and seed phrases
// therefore cannot be accepted accidentally by this field.
bool isValidSolanaPublicAddress(const QString &address);

QUrl worldUrlForRelay(const QString &relayUrl);

QUrl worldDevServerUrl(const QString &configured);

QStringList directMirrorRepositoryOwners(const QString &canonicalOwner,
                                         const QString &catalogOwner);

// True when `value` is one deployed Worker's mirror-router public key: 43
// base64url characters that decode to exactly 32 bytes.
bool isValidMirrorRouterPublicKey(const QString &value);

// True when this node holds every setting its direct HTTPS mirror gateway
// needs: a DNS-label node name, an https origin for the mirror hostname (as
// returned by the caller's origin normalizer, empty when the hostname is
// unusable), and the deployed Worker's router public key. A node that never
// provisioned a Cloudflare endpoint leaves these empty and serves through the
// relay instead, so an incomplete set is a supported steady state rather than
// a failure worth reporting.
bool directMirrorGatewayIsConfigured(const QString &nodeName,
                                     const QString &mirrorOrigin,
                                     const QString &routerPublicKey);

QByteArray mirrorManifestSigningPayload(const QJsonObject &request,
                                        const QString &expectedPublicKey,
                                        QString *error = nullptr);

// Quote one public command component for Python shlex.split(). This is used only
// for the signer executable path/flag; no secret is ever placed in the string.
QString shlexQuote(const QString &value);

QByteArray pythonCanonicalJson(const QJsonValue &value,
                               QString *error = nullptr);

// Build the exact bytes signed as a private-capable catalog-v2 proof. The input
// must already be the Worker's normalized safe_catalog_record shape.
QByteArray catalogV2SigningPayload(QJsonObject normalizedRecord,
                                   QString *error = nullptr);

// Normalize the optional public CPU/RAM/disk fields embedded in a catalog-v2
// record. Every returned key is present; a JSON null means the operator did not
// share that metric. Numbers are bounded integers so Python and C++ produce the
// same canonical signing bytes.
QJsonObject normalizedCatalogHostTelemetry(const QJsonObject &data);

// Exact owner signature payload for a blind private-replica route.
QByteArray privateReplicaRouteSigningPayload(
    const QString &owner, const QString &repository, const QString &node,
    const QString &opaqueId, const QString &replicaSha256, quint64 keyEpoch,
    bool active, qint64 issuedAtMs, QString *error = nullptr);

QByteArray httpsMirrorRegistrationSigningPayload(
    const QString &node, const QString &baseUrl,
    const QString &publicKey, qint64 issuedAtMs,
    QString *error = nullptr);


QString validateVultrMirrorRequest(const QString &apiKey,
                                   const QString &nodeName);

bool vultrPlanHasIpv4(const QJsonObject &plan);

// From GET /v2/plans: the cheapest plan that can actually run an encrypted
// mirror (at least 1 GiB RAM, monthly_cost > 0, a US location, and IPv4). Ties
// break toward more RAM, then the lexicographically smallest id, so selection
// is deterministic.
QJsonObject cheapestVultrPlan(const QJsonArray &plans);

QString vultrPlanRegion(const QJsonObject &plan);

// The same rules as the two above, as choices a phone can pick from: every
// plan cheapestVultrPlan would accept, cheapest first, each with the US
// regions it deploys in (in vultrPlanRegion's preference order), plus the
// plan and region the desktop picks on its own:
//   {plans:[{id, monthlyCost, ramMb, diskGb, vcpus, regions:[...]}],
//    defaultPlan, defaultRegion}
QJsonObject vultrPlanChoices(const QJsonArray &plans);

// The plan to create: `requestedPlan` when it is one of the choices (else the
// cheapest), in `requestedRegion` when that plan deploys there (else its
// preferred region). An empty object, with *error set, when nothing fits.
QJsonObject chooseVultrPlan(const QJsonArray &plans, const QString &requestedPlan,
                            const QString &requestedRegion, QString *region,
                            QString *error = nullptr);

QJsonObject latestVultrDebianOs(const QJsonArray &osList);

bool localBinaryRunsOnVultrMirror(const QString &kernelType,
                                  const QString &cpuArch);

QJsonObject vultrInstanceCreatePayload(const QString &nodeName,
                                       const QString &planId,
                                       const QString &regionId,
                                       int osId,
                                       const QString &sshKeyId);

QString vultrInstanceReadyIp(const QJsonObject &instance);

bool vultrInstanceIsIpv6Only(const QJsonObject &instance);

QString nextMirrorNodeName(const QStringList &existingNames);

QStringList vultrApiKeyVariableNames();

QString vultrApiKeyFromVariables(const QMap<QString, QString> &variables);

bool vultrInstallNeedsLocalBinary(const QString &installOutput);


QString vultrSshProbeRemoteCommand();

bool vultrSshProbeReady(int exitCode, const QString &outputTail);


QString savedHostVultrInstanceId(const QJsonObject &host);

QJsonObject mergeSavedHostRecord(const QJsonObject &previous,
                                 QJsonObject entry,
                                 const QJsonObject &metadata);

QString vultrInstanceIdForAddress(const QJsonArray &instances,
                                  const QString &address);

QString validateVultrDestroyRequest(const QString &apiKey,
                                    const QString &instanceId);

bool mirrorCatalogEntryIsHealthy(const QJsonObject &mirror);

// A signed repository proof can be honest and still describe an empty
// repository (the refs digest of zero refs). Such a mirror serves nothing:
// it must not pass deploy verification or count as fleet-healthy.
bool mirrorRepositoryProofServesContent(const QString &refsSha256);

// GIT_SSH_COMMAND for seeding a managed mirror over the provisioning SSH
// identity, and the git arguments that push every head and tag from the
// local bare mirror into the node-owned repository on the host. Seeding
// makes provisioning independent of public mesh health: a node created
// during a total outage still starts with a full repository.
QString mirrorSeedSshCommand(const QString &identityFile);
QStringList mirrorSeedPushArguments(const QString &bareMirrorPath,
                                    const QString &address);

QString describeMirrorCatalogEntryGap(const QJsonObject &mirror);

// --- Decommissioned mirror nodes ------------------------------
// Removing a host from the Hosts page (by hand, or by the fleet reconciler
// destroying the server) deletes the machine, but the relay keeps that node's
// last signed catalog row inside its freshness lease. For the ~10 minutes that
// lease runs, /mirrors still reports the node online, healthy and serving, so
// the Mirror nodes panel listed servers this very app had already destroyed.
// This ledger — node name to the millisecond we decommissioned it — is the
// local, authoritative "it is gone" fact the lease cannot know yet.
//
// A node re-published after we deleted it is evidence the deletion did not take
// (or the name was re-provisioned elsewhere), so it stays visible; the grace
// window below only ignores the last heartbeats already in flight when the
// server went away.
constexpr qint64 kDecommissionedNodeGraceMs = 10 * 60 * 1000;   // lease length
constexpr qint64 kDecommissionedNodeTtlMs = 30LL * 24 * 3600 * 1000;
constexpr int kDecommissionedNodeLimit = 200;

// Stamp `node` as decommissioned at `nowMs`, pruning expired/excess entries.
QJsonObject recordDecommissionedNode(const QJsonObject &ledger,
                                     const QString &node, qint64 nowMs);

// Forget `node` — call this when a host with that name is saved again, so a
// re-provisioned server is never hidden by its predecessor's tombstone.
QJsonObject forgetDecommissionedNode(const QJsonObject &ledger,
                                     const QString &node);

// True when `node` was decommissioned here and has published nothing since
// (`lastSeenMs` is the node's own last catalog/advert timestamp, 0 if unknown).
bool mirrorNodeIsDecommissioned(const QJsonObject &ledger, const QString &node,
                                qint64 lastSeenMs, qint64 nowMs);

// --- Agent CLIs on a fresh mirror -----------------------------
// A brand-new mirror can install the Claude Code and Codex CLIs, but until it
// is signed in they cannot run a single session — and there is no browser on a
// headless VPS to sign in with. These helpers copy this device's own logins to
// the new node so it comes up able to run agent sessions on our access.
// Everything sensitive lives in the stdin payload only: the remote helper
// command is fixed and secret-free, exactly like the Actions configuration
// path, so nothing ever reaches argv, the process table or the install log.
struct AgentCliCredentials {
    QByteArray claudeCredentials;  // verbatim ~/.claude/.credentials.json
    QByteArray codexAuth;          // verbatim ~/.codex/auth.json
    QMap<QString, QString> env;
};

bool agentCliCredentialsAreEmpty(const AgentCliCredentials &credentials);

QString describeAgentCliCredentials(const AgentCliCredentials &credentials);

QByteArray agentCliEnvFileContents(const QMap<QString, QString> &env);

// Serialize the credentials into the "<section> <base64>" lines the remote
// helper reads on stdin. Secrets appear only in these bytes; callers should
// overwrite and clear the array right after QProcess::write(). Returns an empty
// array (with *error set) when the bundle is malformed or implausibly large.
QByteArray buildAgentCliBootstrapPayload(const AgentCliCredentials &credentials,
                                         QString *error = nullptr);

// The fixed remote command that installs the official user-scoped Claude Code
// and Codex CLIs. With withCredentials the same command also consumes the
// stdin payload above and writes the login files; without it, no stdin is sent
// and the mirror is left unauthenticated (the per-host button's behaviour).
QString agentCliBootstrapRemoteCommand(bool withCredentials);

// The step that copies the installed Claude Code / Codex CLIs into
// /usr/local/bin for the forkmesh-node service: install to <name>.new, then
// mv -f over the old copy, so running agent jobs are not disturbed.
QString agentCliServicePublishScript();

// Root-only, never-fatal shell fragment that refreshes the service's agent
// CLIs that already exist in /usr/local/bin (official installers, then the
// same .new + mv -f swap). No single quotes, so it embeds in any command.
QString agentCliServiceRefreshScript();

// pipeline, then (when it succeeded) the agent-CLI refresh, exiting with the
// pipeline's status. Host updates use it so a mirror's CLIs never go stale.
QString withAgentCliServiceRefresh(const QString &pipeline);


QString cloudflareZoneNameFromVariables(const QMap<QString, QString> &variables);

QString vultrMirrorDnsHostname(const QString &nodeName,
                               const QString &zoneName);

QJsonObject vultrMirrorDnsRecordPayload(const QString &hostname,
                                        const QString &ip);

QString cloudflareZoneId(const QJsonArray &zones, const QString &zoneName);

QString cloudflareDnsRecordId(const QJsonArray &records,
                              const QString &hostname,
                              const QString &recordType);


struct CloudflareTokenRequirement {
    QString key;             // stable id, also the probe-result map key
    QString label;           // dashboard wording, e.g. "Workers Scripts: Edit"
    QString purpose;         // what stops working without it
    QStringList groupNames;  // acceptable permission-group names, mint-first
    QString scope;           // "account", "zone" or "user"
    QString probePath;       // "{account}"/"{zone}" placeholders, may be empty
    bool required = true;    // false: only this page's own token tooling needs it
};

QList<CloudflareTokenRequirement> cloudflareTokenRequirements();

QString cloudflareTokenProbePath(const CloudflareTokenRequirement &requirement,
                                 const QString &accountId,
                                 const QString &zoneId);

// Loose shape check before a token is put on the wire: Cloudflare issues
// 40-character base62 tokens, but the bound stays wide so a future format keeps
// working. Whitespace and newlines are rejected, which is what a pasted
// credential file looks like.
bool isPlausibleCloudflareApiToken(const QString &token);

QString cloudflareTokenVerifyStatus(const QJsonObject &verifyResult);
QString cloudflareTokenVerifyId(const QJsonObject &verifyResult);

// Distinct permission-group names allowed by GET /user/tokens/<id>, sorted.
// Groups inside a deny policy are not reported as granted.
QStringList cloudflareTokenPermissionGroupNames(const QJsonObject &tokenDetail);

bool cloudflareTokenGrantsRequirement(
    const CloudflareTokenRequirement &requirement,
    const QStringList &grantedGroupNames);

// The account ids and the user resource key a token's own policies name. A
// token that cannot list accounts still reveals which account it belongs to
// this way, and the user key is the only way to scope user-level permission
// groups (API Tokens) on a replacement token.
QStringList cloudflareTokenAccountIds(const QJsonObject &tokenDetail);
QString cloudflareTokenUserResourceKey(const QJsonObject &tokenDetail);

// Exact POST /user/tokens body for a replacement ForkMesh deployment token.
// Permission-group ids come from the account's own
// GET /user/tokens/permission_groups catalog, so no id is hardcoded here.
// Account- and zone-scoped groups become one policy each; user-scoped groups
// are included only when userResourceKey is known. Fails closed with *error
// when a required group has no id or no resource to bind to; optional groups
// are dropped silently.
QJsonObject cloudflareTokenCreatePayload(
    const QString &tokenName, const QString &accountId, const QString &zoneId,
    const QString &userResourceKey, const QJsonArray &permissionGroupCatalog,
    QString *error = nullptr);

// The new secret from POST /user/tokens, validated as a usable token.
QString cloudflareCreatedTokenValue(const QJsonObject &createResult);

QString updatedEnvAssignment(const QString &contents, const QString &name,
                             const QString &value);

QString siteDeployEnvFilePath(const QString &sourceDir = QString(),
                              const QString &applicationDir = QString());

// "…9f3c" — the last four characters of a token, so the page can name which
// credential is in play without ever echoing one.
QString maskedTokenSuffix(const QString &token);

} // namespace forkmesh::control
