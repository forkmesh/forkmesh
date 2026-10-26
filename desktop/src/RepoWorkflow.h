#pragma once

#include "TeammateStore.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

namespace forkmesh::repoworkflows {

inline const QString kFolder = QStringLiteral(".forkmesh/workflows");
inline const QString kLegacyFolder = QStringLiteral(".forkmesh/teammates");
inline const QString kFileKind = QStringLiteral("forkmesh.workflow");

inline const QString kManual = QStringLiteral("workflow.manual");
inline const QString kSchedule = QStringLiteral("workflow.schedule");
inline const QString kMonitor = QStringLiteral("workflow.monitor");
inline const QString kAgent = QStringLiteral("agent.run");
inline const QString kShortcut = QStringLiteral("workflow.shortcut");
inline const QString kOutput = QStringLiteral("workflow.output");
inline const QString kNote = QStringLiteral("workflow.note");

inline const QString kRailTop = QStringLiteral("top");
inline const QString kRailLeft = QStringLiteral("left");
inline const QString kRailRight = QStringLiteral("right");
inline const QString kRailBottom = QStringLiteral("bottom");
inline const QString kAccountRepo = QStringLiteral("@account/workflows");

inline const QString kStatusQueued = QStringLiteral("queued");
inline const QString kStatusRunning = QStringLiteral("running");
inline const QString kStatusSuccess = QStringLiteral("success");
inline const QString kStatusError = QStringLiteral("error");
inline const QString kStatusStopped = QStringLiteral("stopped");
inline const QString kStatusSkipped = QStringLiteral("skipped");

constexpr int kMaxNodes = 60;
constexpr int kMaxConnections = 120;
constexpr int kMaxMonitors = 16;
constexpr int kMaxNodeName = 60;
constexpr int kMaxRunsKept = 30;
constexpr int kMaxPins = 48;
constexpr int kMaxStepOutputChars = 24 * 1024;
constexpr int kMaxInputChars = 12 * 1024;
constexpr int kColumn = 288;
constexpr int kRow = 120;

struct Workflow {
    int version = 1;
    QString id;
    QString name;
    QString description;
    QString icon;
    bool enabled = true;
    qint64 createdAtMs = 0;
    qint64 updatedAtMs = 0;
    QJsonArray nodes;
    QJsonArray connections;

    QJsonObject toJson() const;
    static Workflow fromJson(const QJsonObject &object, QString *error = nullptr);

    int indexOf(const QString &nodeId) const;
    QJsonObject node(const QString &nodeId) const;
    QStringList ordered() const;
    QStringList inputsOf(const QString &nodeId) const;
    QStringList nodesOfType(const QString &type) const;
    QString primaryAgent() const;
    QJsonObject scheduleNode() const;
    bool isSpecialAgent() const { return !primaryAgent().isEmpty(); }
    QString triggerSummary() const;
};

bool looksLikeWorkflowFile(const QJsonObject &object);
bool isTriggerType(const QString &type);
bool isKnownType(const QString &type);
bool isDesktopStepType(const QString &type);
QString desktopStepName(const QString &type);
QString validate(const Workflow &workflow);
QString errorText(const QString &code);
QJsonObject catalog();
QJsonObject nodeSpec(const QString &type);

bool toTeammate(const Workflow &workflow, forkmesh::teammates::Config *config,
                QString *error = nullptr);
QString agentStepId(const Workflow &workflow, const QString &nodeId);
bool runsAgent(const Workflow &workflow, const QString &agentId);
forkmesh::teammates::Config agentConfig(const Workflow &workflow, const QString &nodeId,
                                        QString *error = nullptr);
void applyTeammate(Workflow *workflow, const forkmesh::teammates::Config &config);
Workflow fromTeammate(const forkmesh::teammates::Config &config);
QJsonObject agentParams(const forkmesh::teammates::Config &config);
forkmesh::teammates::Config scheduleConfig(const Workflow &workflow, qint64 lastRunAtMs);
QJsonObject withoutBookkeeping(QJsonObject object);

struct Listing {
    Workflow workflow;
    QString path;
    bool legacy = false;
    QString error;
};

QList<Listing> listRepositoryWorkflows(const QString &checkout);
QString writeWorkflow(const QString &checkout, Workflow workflow, qint64 nowMs, QString *path = nullptr);
bool removeWorkflowFiles(const QString &checkout, const QString &workflowId, QString *error = nullptr);
QList<QPair<QString, QString>> avatarChoices();
QList<QPair<QString, QString>> iconChoices();
bool isIconChoice(const QString &key);

Workflow blank(const QString &id, const QString &name);
Workflow shortcutWorkflow(const QString &id, const QString &name, const QString &shortcutFile);
QString uniqueNodeId(const Workflow &workflow, const QString &base);
QString uniqueNodeName(const Workflow &workflow, const QString &base);
QString safeRepositoryPath(const QString &path);

struct StepRun {
    QString nodeId;
    QString name;
    QString type;
    QString status = kStatusQueued;
    qint64 startedAtMs = 0;
    qint64 finishedAtMs = 0;
    int sessionId = 0;
    QString output;
    QString error;
    QStringList logs;

    QJsonObject toJson() const;
    static StepRun fromJson(const QJsonObject &object);
};

struct Run {
    QString id;
    QString workflowId;
    QString workflowName;
    QString repo;
    QString trigger;
    QString taskType;
    QString assignment;
    QString status = kStatusRunning;
    qint64 startedAtMs = 0;
    qint64 finishedAtMs = 0;
    QList<StepRun> steps;
    QString output;
    QString caption;
    QString outputFile;
    QString error;

    QJsonObject toJson() const;
    static Run fromJson(const QJsonObject &object);
    StepRun *step(const QString &nodeId);
    const StepRun *step(const QString &nodeId) const;
    bool finished() const;
    int sessionId() const;
};

QString runMarkdown(const Run &run);
QString runSummaryText(const Run &run);
QList<Run> loadRuns(const QString &file);
bool saveRun(const QString &file, const Run &run, QStringList *droppedRunIds = nullptr);

struct Pin {
    QString repo;
    QString workflowId;
    QString rail;

    bool operator==(const Pin &other) const
    {
        return repo.compare(other.repo, Qt::CaseInsensitive) == 0 &&
               workflowId == other.workflowId;
    }
};

QStringList rails();
QString railLabel(const QString &rail);
bool isRail(const QString &rail);
QList<Pin> parsePins(const QString &json);
QString serializePins(const QList<Pin> &pins);
QList<Pin> pinWorkflow(QList<Pin> pins, const Pin &pin);
QList<Pin> movePin(QList<Pin> pins, const Pin &pin, int slot);
QList<Pin> unpinWorkflow(QList<Pin> pins, const QString &repo, const QString &workflowId);
QString pinnedRail(const QList<Pin> &pins, const QString &repo, const QString &workflowId);

} // namespace forkmesh::repoworkflows
