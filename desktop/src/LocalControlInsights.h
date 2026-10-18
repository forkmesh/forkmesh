#pragma once

#include "AgentGuardrailLedger.h"
#include "AgentToolCatalog.h"
#include "AgentToolEfficiencyLedger.h"
#include "AgentToolImprovementLog.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

// The "insights." local control family (WS17; docs/native/DESKTOP_PARITY.md
// section 19): agent insights: tool efficiency, guardrails, the announcer. This is the policy half, kept free of widgets so the
// tests can drive it; the requests are answered in
// MainWindowLocalControlInsights.cpp. Gate 2 creates it empty; WS17 owns it.
namespace forkmesh::localcontrol::insights {

// The method prefix this family answers.
QString familyPrefix();

QJsonObject familyRow(const AgentToolFamilyStats &stats, const AgentToolFamily *family,
                      bool disabled, const QString &enforcement);
QJsonObject totalsRow(const AgentToolLedgerTotals &totals);
QJsonObject usageRow(const AgentToolUsageStats &usage);
QJsonObject improvementRow(const AgentToolImprovement &improvement);
QJsonObject guardrailRow(const AgentGuardrailRepoStats &stats, int maxTools, int maxPaths);
QJsonObject guardrailTotalsRow(const AgentGuardrailTotals &totals);

struct SessionUsage {
    int id = 0;
    QString title;
    QString provider;
    QString model;
    QString status;
    qint64 tokens = 0;
    double costUsd = 0;
    int turns = 0;
    qint64 finishedAtMs = 0;
    QString teammateId;
};
QJsonObject usageSummary(const QList<SessionUsage> &sessions, int maxSessions);

struct PeerTraffic {
    int session = 0;
    QString peer;
    quint16 port = 0;
    qint64 sent = 0;
    qint64 received = 0;
};
bool isLoopbackPeer(const QString &peer);
QJsonArray networkRows(const QList<PeerTraffic> &traffic, const QHash<int, QString> &labels,
                       const QHash<QString, QString> &names, int maxPeersPerAgent);

struct ShortcutMeta {
    QString name;
    QString description;
    QString agent;
    QString model;
};
ShortcutMeta parseShortcutMeta(const QString &fileName, const QString &text);
struct ShortcutStep {
    QString name;
    QString shortcut;
};
constexpr int kMaxShortcutSteps = 32;
constexpr int kMaxShortcutStepName = 80;
bool isShortcutStepLine(const QString &line);
QList<ShortcutStep> parseShortcutSteps(const QString &text);
QJsonArray shortcutStepRows(const QList<ShortcutStep> &steps);
QString shortcutPrompt(const QString &text);
QString shortcutKind(const QString &fileName, bool executable);
bool validShortcutFile(const QString &fileName);
QStringList shortcutScopes();

struct ShortcutRun {
    int id = 0;
    QString repo;
    QString file;
    QString name;
    QString output;
    bool running = false;
    bool ok = false;
    QString message;
    qint64 startedAtMs = 0;
    qint64 expectedMs = -1;
};
QString appendShortcutOutput(const QString &output, const QString &chunk, int maxChars);
QJsonObject shortcutRunRow(const ShortcutRun &run, bool withOutput);
QString lastOutputLine(const QString &output, int maxChars = 200);

QStringList mutatingActions();
QString subjectFor(const QString &action, const QJsonObject &params, QString *error = nullptr);
QString actionSummary(const QString &action, const QString &subject, const QString &label,
                      bool script = false);

} // namespace forkmesh::localcontrol::insights
