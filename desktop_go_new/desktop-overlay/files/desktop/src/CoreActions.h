#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

class ActionRunner;
class CoreClient;

namespace forkmesh::coreactions {

void attach(ActionRunner *runner, CoreClient *core);
bool adopt(ActionRunner *runner, const QJsonObject &active);
bool follows(const ActionRunner *runner, int runId);
void applyClaims(const QJsonObject &activeReply);

QJsonObject pullChecksParams(const QString &owner, const QString &name, const QStringList &trusted,
                             bool identity, bool manual, bool force, const QList<int> &numbers);
void noteFinished(const QJsonObject &finished);
bool takeCoreAutoFix(int runId);
bool takeServedLanding(int runId, bool *ok, QString *error);
void followDirect(int runId);
bool followsDirect(int runId);
void dropDirect(int runId);
QList<int> directRuns();
void markFinishHandled(int runId);
bool finishHandled(int runId);

}
