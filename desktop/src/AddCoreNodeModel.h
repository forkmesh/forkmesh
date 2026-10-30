#pragma once

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

namespace forkmesh::fleet {

inline constexpr char kAddCoreEventPrefix[] = "FMNEXT-ADD-CORE ";

struct AddCorePlan {
    QString id;
    int ramMb = 0;
    int vcpus = 0;
    double monthlyCost = 0.0;
    QStringList locations;
};

struct AddCoreOptions {
    bool valid = false;
    QString provider = QStringLiteral("vultr");
    QStringList providers;
    QString priceNote;
    QString name;
    QString region;
    QString plan;
    QString rev;
    QString replaces;
    bool voter = false;
    QString voterReason;
    int estimateSeconds = 0;
    QList<AddCorePlan> plans;
    QStringList regions;
    QString error;
    QString plansError;
    QString unfinishedName;
    QString unfinishedReplaces;
    QString unfinishedFailedStep;
    QString unfinishedError;
};

struct AddCoreStep {
    enum class Status { Pending, Running, Done, Failed };
    QString id;
    QString title;
    int estimateSeconds = 0;
    Status status = Status::Pending;
    double seconds = 0.0;
    QString error;
};

AddCoreOptions parseAddCoreOptions(const QByteArray &json);
QString stepDisplayText(const AddCoreStep &step);
QList<AddCorePlan> plansInRegion(const AddCoreOptions &options, const QString &region);
const AddCorePlan *findPlan(const AddCoreOptions &options, const QString &planId);
QString formatMonthlyCost(double dollars);
QString formatDuration(int seconds);
QString planLabel(const AddCorePlan &plan);
QString confirmationText(const AddCoreOptions &options, const QString &region,
                         const QString &plan);
QStringList addCoreArguments(const QString &script, const QString &region,
                             const QString &plan, const QString &name, bool resume,
                             const QString &provider = QString(),
                             const QString &replace = QString());
QStringList addCoreOptionsArguments(const QString &script, const QString &provider,
                                    const QString &replace = QString());
QString providerDisplayName(const QString &provider);
bool isFleetInstanceHost(const QString &name, const QString &address,
                         const QStringList &fleetAddresses);
QStringList systemdRunArguments(const QString &unit, const QString &logPath,
                                const QString &workingDirectory, const QString &program,
                                const QStringList &arguments,
                                const QList<QPair<QString, QString>> &environment);
QString addCoreUnitName(const QString &stamp);

struct UnitState {
    bool known = false;
    bool running = false;
    int exitStatus = 0;
};
UnitState parseUnitState(const QByteArray &systemctlShow);

class AddCoreRun {
public:
    enum class State { Idle, Running, Succeeded, Failed };

    void reset();
    void setEventPrefix(const QString &prefix) { m_prefix = prefix; }
    void setVerb(const QString &verb) { m_verb = verb; }
    void setDoneSummary(const QString &summary) { m_doneSummary = summary; }
    void setDefaultSteps(const QList<AddCoreStep> &steps);
    QString consume(const QString &chunk);
    QString flush();
    void markProcessEnded(int exitCode, bool crashed);
    bool loadStatus(const QByteArray &json);

    State state() const { return m_state; }
    const QList<AddCoreStep> &steps() const { return m_steps; }
    QString name() const { return m_name; }
    QString summary() const { return m_summary; }
    QString failedStep() const { return m_failedStep; }
    QString error() const { return m_error; }
    QStringList notes() const { return m_notes; }
    QString branch() const { return m_branch; }
    QString replaces() const { return m_replaces; }
    QString merged() const { return m_merged; }
    QString mergeError() const { return m_mergeError; }
    QString commit() const { return m_commit; }
    int doneCount() const;
    int remainingEstimateSeconds() const;
    QString headline() const;

    static QList<AddCoreStep> stepsFromJson(const QByteArray &json);

private:
    bool handleEvent(const QString &line);
    AddCoreStep *step(const QString &id);

    State m_state = State::Idle;
    QString m_prefix = QString::fromLatin1(kAddCoreEventPrefix);
    QString m_verb = QStringLiteral("Adding");
    QString m_doneSummary = QStringLiteral("%1 is serving, in DNS");
    QList<AddCoreStep> m_steps;
    QString m_pending;
    QString m_name;
    QString m_summary;
    QString m_failedStep;
    QString m_error;
    QStringList m_notes;
    QString m_branch;
    QString m_replaces;
    QString m_merged;
    QString m_mergeError;
    QString m_commit;
    bool m_resultSeen = false;
};

QString fleetLandingText(const AddCoreRun &run, const QString &effect);

} // namespace forkmesh::fleet
