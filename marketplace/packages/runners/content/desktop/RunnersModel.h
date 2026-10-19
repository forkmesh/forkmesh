#pragma once

#include "RunnerLogin.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace forkmesh::runnerspage {

constexpr qint64 kOnlineWindowMs = 10 * 60 * 1000;

enum class RunnerAction { Pause, Resume, Revoke };

struct AccountChoice {
    QString id;
    QString name;
    bool active = false;
    bool pending = false;
};

struct ProviderRow {
    QString provider;
    QString title;
    QString state;
    QString tone;
    QString account;
    QString signInText;
    bool signInOffered = false;
    bool accountsOffered = false;
    QList<AccountChoice> accounts;
};

struct RunnerRow {
    QString name;
    QString scope;
    QString status;
    QString statusLabel;
    QString statusTone;
    QString detail;
    bool online = false;
    bool revoked = false;
    QList<ProviderRow> providers;
    QList<RunnerAction> actions;
};

bool runnerOnline(const forkmesh::runners::RunnerInfo &runner, qint64 nowMs);
bool runnerUsable(const forkmesh::runners::RunnerInfo &runner);
QString providerTitle(const QString &provider);
QString runnerStatusLabel(const QString &status);
QString runnerStatusTone(const QString &status);
QString scopeLabel(const QString &scope);
QString jobsText(int activeLeases);
QString lastSeenText(qint64 lastSeenAtMs, qint64 nowMs);
QString runnerDetail(const forkmesh::runners::RunnerInfo &runner, qint64 nowMs);

QString providerStateText(const forkmesh::runners::RunnerProviderState &state);
QString providerTone(const forkmesh::runners::RunnerProviderState &state);
bool signInOffered(const forkmesh::runners::RunnerInfo &runner, const QString &provider);
QString signInText(const forkmesh::runners::RunnerInfo &runner, const QString &provider);
bool accountsOffered(const forkmesh::runners::RunnerInfo &runner, const QString &provider);
ProviderRow providerRow(const forkmesh::runners::RunnerInfo &runner, const QString &provider);

QList<RunnerAction> actionsFor(const forkmesh::runners::RunnerInfo &runner);
QList<forkmesh::runners::RunnerInfo> sortedRunners(QList<forkmesh::runners::RunnerInfo> runners);
RunnerRow runnerRow(const forkmesh::runners::RunnerInfo &runner, qint64 nowMs);
QList<RunnerRow> runnerRows(const QJsonObject &payload, qint64 nowMs);
int attentionCount(const QJsonObject &payload);

QString actionName(RunnerAction action);
QString actionLabel(RunnerAction action);
QString actionProgressText(RunnerAction action, const QString &runner);
QString actionDoneText(RunnerAction action, const QString &runner);
QString actionPath(RunnerAction action);
QJsonObject actionBody(const QString &runner);

QString accountsPath(const QString &runner);
QJsonObject accountBody(const QString &provider, const QString &account, const QString &action);
QString accountDoneText(const QString &action, const QString &account, const QString &runner);

QString addRunnerPath();
QString connectPath(const QString &runner, const QString &provider);

QString revokeTitle(const QString &runner);
QString revokeQuestion(const QString &runner);
QString removeAccountQuestion(const QString &account, const QString &runner);

bool signedOutError(const QString &error);
QString loadErrorText(const QString &error);
QString actionErrorText(const QString &error);

}
