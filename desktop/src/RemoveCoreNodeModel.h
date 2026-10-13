#pragma once

#include "AddCoreNodeModel.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace forkmesh::fleet {

inline constexpr char kRemoveCoreEventPrefix[] = "FMNEXT-REMOVE-CORE ";
inline constexpr int kMinimumCores = 3;

struct RemoveCorePlan {
    bool valid = false;
    QString name;
    QString label;
    QString provider;
    QString instanceId;
    QString ip;
    QString leader;
    bool voter = true;
    bool isLeader = false;
    bool allowed = false;
    bool resume = false;
    bool destroyInstance = false;
    bool destroyed = false;
    bool finished = false;
    QString blocker;
    QString error;
    QString quorum;
    QString failedStep;
    QString failedError;
    QStringList votersBefore;
    QStringList votersAfter;
    QStringList healthyAfter;
    QStringList warnings;
    QStringList description;
    int estimateSeconds = 0;
    QList<AddCoreStep> steps;
};

RemoveCorePlan parseRemoveCorePlan(const QByteArray &json);
bool isRemovableCoreName(const QString &name);
QString coreCountBlocker(const QString &name, int coreCount);
bool removeConfirmationMatches(const QString &name, const QString &typed);
QString removeCoreBlocker(const RemoveCorePlan &plan, bool planLoading, bool running,
                          bool destroyInstance, const QString &typed);
QString removeCoreSummaryText(const RemoveCorePlan &plan, bool destroyInstance);
QStringList removeCorePlanArguments(const QString &script, const QString &name,
                                    bool destroyInstance);
QStringList removeCoreArguments(const QString &script, const QString &name,
                                bool destroyInstance);
QString removeCoreUnitName(const QString &stamp);
void configureRemoveRun(AddCoreRun &run);

} // namespace forkmesh::fleet
