#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <functional>

namespace forkmesh {
namespace corelogs {

struct CoreNode {
    QString name;
    QString address;
    QString role = QStringLiteral("core");
    QString label;
};

struct VultrInventory {
    qint64 fetchedMs = 0;
    qint64 retryAtMs = 0;
    int failures = 0;
    QList<CoreNode> nodes;
};

struct FeedCommand {
    QString program;
    QStringList arguments;
    QString error;
};

struct JournalEntry {
    bool valid = false;
    QString cursor;
    qint64 realtimeMs = 0;
    int priority = 6;
    QString unit;
    QString userUnit;
    QString identifier;
    QString transport;
    QString message;
    QString detail;
};

enum class Severity { Info, Warning, Error };

struct Classified {
    Severity severity = Severity::Info;
    QString unit;
    bool forward = false;
};

struct AccessRecord {
    bool valid = false;
    QString method;
    QString host;
    QString path;
    int status = 0;
    bool expectedDegraded = false;
    qint64 durationMs = 0;
    qint64 bytes = 0;
    qint64 atMs = 0;
    QString clientIp;
    QString edgeIp;
    QString relay;
    QString userAgent;
    QString country;
    QString region;
    QString city;
    QString clientLabel;
    QString network;
};

struct ClientDetails {
    QString country;
    QString label;
    QString network;
};

struct ReaderControl {
    enum Kind { None, Header, Idle, Failure, Vitals };
    Kind kind = None;
    QString node;
    QString role;
    QString error;
    bool resumed = false;
};

struct NodeLinePrefix {
    bool valid = false;
    QString node;
    QString role;
    QString unit;
    QString severity;
};

struct NodeLogFilter {
    QSet<QString> nodes;
    QSet<QString> units;
    bool errorsOnly = false;
    bool active() const
    {
        return !nodes.isEmpty() || !units.isEmpty() || errorsOnly;
    }
    bool accepts(const QString &message) const;
};

struct StatementLine {
    bool valid = false;
    QString kind;
    QString sql;
    QString originFile;
    QString originFunction;
    QString caller;
};

struct NodeVitals {
    bool valid = false;
    qint64 atMs = 0;
    int cpus = 0;
    double cpuBusy = -1.0;
    double load1 = -1.0;
    double load5 = -1.0;
    double load15 = -1.0;
    int procsRunning = -1;
    int procsTotal = -1;
    qint64 memTotal = -1;
    qint64 memAvailable = -1;
    qint64 swapTotal = -1;
    qint64 swapFree = -1;
    qint64 diskTotal = -1;
    qint64 diskFree = -1;
    qint64 inodesTotal = -1;
    qint64 inodesFree = -1;
    qint64 filesOpen = -1;
    qint64 filesMax = -1;
    qint64 uptimeSeconds = -1;
};

struct StatementShape {
    QString verb;
    QString table;
};

struct StatementDone {
    bool valid = false;
    double ms = 0.0;
    qint64 rows = 0;
    qint64 changes = 0;
    QString served;
    bool failed = false;
    QString key;
};

struct CallerShape {
    QString kind;
    QString label;
    QString ip;
    QString user;
    QString session;
};

struct WebRequestRow {
    QString group;
    QString statusClass;
    qint64 count = 0;
    qint64 durationSumMs = 0;
    qint64 durationMaxMs = 0;
};

struct WebRequestBatch {
    bool valid = false;
    qint64 windowStartMs = 0;
    int windowSeconds = 0;
    QList<WebRequestRow> rows;
};

struct RepeatVerdict {
    bool admit = true;
    qint64 foldedRepeats = 0;
};

struct StatementRow {
    QString kind;
    QString sql;
    QString originFile;
    QString originFunction;
    qint64 count = 0;
    qint64 lastMs = 0;
    QMap<QString, qint64> perNode;
};

struct NodeRow {
    QString node;
    bool connected = false;
    QString state;
    qint64 stateMs = 0;
    qint64 lines = 0;
    qint64 forwarded = 0;
    qint64 folded = 0;
    qint64 statements = 0;
    qint64 errors = 0;
    qint64 lastLineMs = 0;
    QMap<QString, qint64> bySource;
};

struct StatementLink {
    QString href;
    QString label;
};

inline constexpr int kFirstConnectReplayLines = 200;
inline constexpr qint64 kRepeatWindowMs = 10 * 60 * 1000;
inline constexpr int kTimelineMinutes = 30;
inline constexpr qint64 kVultrInventoryRefreshMs = 60 * 60 * 1000;
inline constexpr qint64 kVultrInventoryRetryBaseMs = 15 * 60 * 1000;
inline constexpr qint64 kVultrInventoryRetryMaxMs = 6 * 60 * 60 * 1000;
inline constexpr qint64 kVultrInventoryRetryAfterMaxMs = 24 * 60 * 60 * 1000;

QString nextHomeDirectory();
QString fleetConfigDirectory();
QString fleetSshDirectory();
QString fleetLogsDirectory();
QString readerKeyPath();
QString fleetKnownHostsPath();
QString fleetTomlPath();
int fleetSshPort();
QString fleetInstancesPath();
QString autoscaledInstancesPath();
bool writeAutoscaledInstances(const QJsonObject &instances);
QString vultrApiKey();
QList<CoreNode> loadFleetNodes();
QList<CoreNode> parseFleetNodes(const QByteArray &fleetInstancesJson);
QList<CoreNode> parseVultrInstances(const QByteArray &instancesJson);
QList<CoreNode> mergeFleetNodes(const QList<CoreNode> &primary,
                                const QList<CoreNode> &extra);

bool validJournalCursor(const QString &cursor);
QString readerRequest(const QString &afterCursor);
FeedCommand buildFeedCommand(const CoreNode &node, const QString &afterCursor,
                             bool legacyPort = false);
ReaderControl parseReaderControl(const QByteArray &line);
NodeVitals parseNodeVitals(const QByteArray &line);
StatementShape statementShape(const QString &sql);
QString nodeInkColour(const QString &node);
QString readerFailureState(const QString &sshStderrLine);
QHash<QString, QString> loadCursors(const QString &path);
bool saveCursors(const QString &path, const QHash<QString, QString> &cursors);
VultrInventory loadVultrInventory(const QString &path);
bool saveVultrInventory(const QString &path, const VultrInventory &inventory);
qint64 vultrInventoryRetryDelayMs(int failures, qint64 retryAfterMs);
qint64 vultrInventoryDueMs(const VultrInventory &inventory);

JournalEntry parseJournalLine(const QByteArray &line);
QString journalSource(const JournalEntry &entry);
QString unitLabel(const JournalEntry &entry);
QString displayMessage(const JournalEntry &entry);
Classified classifyEntry(const JournalEntry &entry, const QString &source);
bool isErrorEntry(const JournalEntry &entry, const QString &source);
bool isKnownBootNoise(const JournalEntry &entry);
constexpr qint64 kBacklogAlertMs = 15 * 60 * 1000;
bool isBacklogEntry(const JournalEntry &entry, qint64 nowMs);
QString backlogNote(const JournalEntry &entry);
bool isPoolChange(const JournalEntry &entry);
AccessRecord parseAccessRecord(const JournalEntry &entry,
                               const QString &source);
QString accessLogMessage(const AccessRecord &record);
QString countryFlag(const QString &code);
QString accessPlaceName(const QString &value);
QString accessClientLabel(const QString &address, const QList<CoreNode> &fleet);
QStringList lineClientAddresses(const QString &text);
QString withClientDetails(
    const QString &text,
    const std::function<ClientDetails(const QString &)> &describe);
QString compactByteSize(qint64 bytes);
QString withEmojiFont(const QString &html);
QString accessLogCategory(const AccessRecord &record, bool backlog);
QString accessSiteGroup(const AccessRecord &record);
QString statusClassOf(int status);
QString severityWord(Severity severity);
QString nodeLogLine(const CoreNode &node, const QString &unit,
                    const QString &word, const QString &message,
                    qint64 foldedRepeats = 0);
NodeLinePrefix parseNodeLinePrefix(const QString &message);
QString nodeLineBadge(const QString &message, bool errorsWin = true);
QStringList nodeLineBadges();
bool isFleetBadge(const QString &badge);
QString nodeLineHtml(const QString &message, bool dark,
                     const QString &unitAccent = QString());
StatementLine parseStatementLine(const JournalEntry &entry,
                                 const QString &source);
WebRequestBatch parseWebRequestBatch(const JournalEntry &entry,
                                     const QString &source);
QString mainLogStatementLine(const CoreNode &node,
                             const StatementLine &statement);
StatementDone parseStatementDone(const JournalEntry &entry,
                                 const QString &source);
QString statementText(const QString &sql);
int statementArgCount(const QString &sql);
QString statementKey(const QString &sql);
QString callerRoute(const QString &path);
CallerShape callerShape(const QString &caller);
QString feedNoteLine(const CoreNode &node, const QString &note);
QString normalizedRepeatKey(const QString &message);
QString statementLinkKey(const QString &originFile, const QString &sql);

class RepeatFilter
{
public:
    explicit RepeatFilter(int capacity = 4096) : m_capacity(capacity) {}
    RepeatVerdict admit(const QString &node, const QString &source,
                        const QString &message, qint64 nowMs);

private:
    struct Seen {
        qint64 admittedMs = 0;
        qint64 folded = 0;
    };
    QHash<QString, Seen> m_seen;
    int m_capacity;
};

class TracebackAssembler
{
public:
    QList<JournalEntry> feed(const QString &node, const JournalEntry &entry,
                             qint64 nowMs);
    QList<QPair<QString, JournalEntry>> takeStale(qint64 nowMs,
                                                  qint64 quietMs = 2000);
    bool pending() const { return !m_open.isEmpty(); }

private:
    struct Open {
        JournalEntry first;
        QStringList lines;
        qint64 lastMs = 0;
    };
    static JournalEntry finish(const Open &open);
    QHash<QString, Open> m_open;
};

class ActivityTally
{
public:
    explicit ActivityTally(int statementCapacity = 512)
        : m_statementCapacity(statementCapacity)
    {
    }

    void noteFeedState(const QString &node, bool connected,
                       const QString &state, qint64 nowMs);
    void noteLine(const QString &node, const QString &source, bool forwarded,
                  bool folded, bool error, qint64 nowMs);
    void noteStatement(const QString &node, const StatementLine &statement,
                       qint64 nowMs);

    QList<StatementRow> topStatements(int limit) const;
    QList<NodeRow> nodes() const;
    QList<QPair<qint64, QMap<QString, qint64>>> timeline(qint64 nowMs) const;
    qint64 totalStatements() const { return m_totalStatements; }
    qint64 overflowStatements() const { return m_overflowStatements; }
    qint64 sinceMs() const { return m_sinceMs; }
    int statementCapacity() const { return m_statementCapacity; }

private:
    NodeRow &nodeRow(const QString &node, qint64 nowMs);

    QMap<QString, NodeRow> m_nodes;
    QHash<QString, StatementRow> m_statements;
    QMap<qint64, QMap<QString, qint64>> m_minutes;
    int m_statementCapacity;
    qint64 m_totalStatements = 0;
    qint64 m_overflowStatements = 0;
    qint64 m_sinceMs = 0;
};

QString renderActivityHtml(const ActivityTally &tally, qint64 nowMs, bool dark,
                           const QString &idleReason,
                           const QHash<QString, StatementLink> &links);

} // namespace corelogs
} // namespace forkmesh
