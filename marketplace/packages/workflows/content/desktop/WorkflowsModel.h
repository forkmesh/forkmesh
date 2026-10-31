#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace forkmesh::workflows {

constexpr int kColumn = 288;
constexpr int kRow = 120;
constexpr int kMaxNodes = 60;
constexpr int kMaxWorkflows = 50;
constexpr qint64 kDesktopStepTimeoutMs = 20 * 60 * 1000;
inline constexpr char kAccountRepo[] = "@account/workflows";

struct Stats {
    int workflows = 0;
    int active = 0;
    int scheduled = 0;
    int webhooks = 0;
    int manual = 0;
    int steps = 0;
    int runs = 0;
    int succeeded = 0;
    int failed = 0;
    int runsToday = 0;
    qint64 lastRunAt = 0;
    qint64 nextRunAt = 0;
    qint64 averageMs = 0;
    QString lastFailedWorkflow;
};

Stats computeStats(const QJsonArray &workflows, const QJsonArray &runs, qint64 nowMs);
QString successRateText(const Stats &stats);
QString triggerLabel(const QString &type);
QString statusLabel(const QString &status);
QString errorText(const QString &code, const QString &fallback);
QString durationText(qint64 ms);
QString relativeTime(qint64 atMs, qint64 nowMs);
QString everyText(int minutes);
QString humanBytes(double bytes);
QString runOutputText(const QJsonObject &run);
QJsonArray mergeRuns(const QHash<QString, QJsonArray> &byWorkflow,
                     const QHash<QString, QString> &names, int limit);
QJsonArray unifiedWorkflows(const QJsonArray &account, const QHash<QString, QJsonArray> &runsByWorkflow,
                            const QJsonArray &local, const QJsonArray &pins);
bool workflowMatches(const QJsonObject &entry, const QString &needle, const QString &scope);
void sortWorkflows(QJsonArray &entries, const QString &order);
QJsonArray statsWorkflows(const QJsonArray &entries);
QString workflowStateText(const QJsonObject &entry);

class Catalog
{
public:
    void load(const QJsonObject &catalog);
    bool isEmpty() const { return m_types.isEmpty(); }
    QJsonObject spec(const QString &type) const;
    QString name(const QString &type) const;
    QString description(const QString &type) const;
    QString group(const QString &type) const;
    bool isTrigger(const QString &type) const;
    bool isApp(const QString &type) const;
    bool isDesktop(const QString &type) const;
    QString desktopStep(const QString &type) const;
    QStringList outputs(const QString &type) const;
    QJsonArray operations(const QString &type) const;
    QJsonArray fields(const QString &type, const QString &operation) const;
    QJsonArray credentialFields(const QString &integration) const;
    QStringList triggerTypes() const;
    QStringList stepTypes() const;
    QStringList integrationKeys() const;
    QJsonArray operators() const { return m_operators; }
    QString summary(const QJsonObject &node) const;

private:
    QHash<QString, QJsonObject> m_types;
    QStringList m_order;
    QJsonArray m_operators;
};

class Draft
{
public:
    static Draft blank();
    static Draft fromJson(const QJsonObject &workflow);
    static Draft fromLocal(const QJsonObject &workflow, const QString &repo);
    QJsonObject localJson() const;
    bool isLocal() const { return !repo.isEmpty(); }
    QString addTrigger(const Catalog &catalog, const QString &type);
    void retype(const Catalog &catalog, const QString &nodeId, const QString &type);
    bool hasAgentStep() const;

    QJsonObject graph() const;
    QJsonObject body() const;
    int indexOf(const QString &id) const;
    QJsonObject node(const QString &id) const;
    QJsonObject trigger(const Catalog &catalog) const;
    QStringList ordered() const;
    QJsonArray inputs(const QString &id) const;
    QJsonArray outputsOf(const QString &id) const;
    QString uniqueName(const QString &base, const QString &except = QString()) const;
    QString newId(const QString &type) const;
    bool nameTaken(const QString &name, const QString &except) const;
    QString addStep(const Catalog &catalog, const QString &type, const QString &parent,
                    const QString &output = QStringLiteral("main"));
    void setTrigger(const Catalog &catalog, const QString &type);
    void remove(const QString &id);
    bool updateNode(const QJsonObject &node);
    bool reaches(const QString &from, const QString &to) const;
    bool canConnect(const Catalog &catalog, const QString &from, const QString &output,
                    const QString &to) const;
    bool connectSteps(const Catalog &catalog, const QString &from, const QString &output,
                      const QString &to);
    void disconnectSteps(const QString &from, const QString &output, const QString &to);
    bool hasDesktopSteps(const Catalog &catalog) const;
    QString inputText(const QJsonObject &connection) const;
    QString inputsText(const QString &id) const;

    QString id;
    QString name;
    bool active = false;
    QJsonArray nodes;
    QJsonArray connections;
    QString hookPath;
    QString repo;
    QString icon;
    QString description;
    qint64 createdAtMs = 0;
};

QString localWorkflowId(const QString &name);
QString localStatusLabel(const QString &status);

QStringList freeDiskSpaceArguments(const QString &checkout, const QString &step);
QJsonObject freeDiskSpaceReply(const QString &step, const QByteArray &output, qint64 ms);
QString freeDiskSpaceFailure(const QByteArray &output);

} // namespace forkmesh::workflows
