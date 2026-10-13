#pragma once

#include "TeammateStore.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

// The "teammates." local control family (WS17; docs/native/DESKTOP_PARITY.md
// section 19): the Team page's teammates. This is the policy half, kept free of widgets so the
// tests can drive it; the requests are answered in
// MainWindowLocalControlTeammates.cpp. Gate 2 creates it empty; WS17 owns it.
namespace forkmesh::localcontrol::teammates {

// The method prefix this family answers.
QString familyPrefix();

QString statusFor(const forkmesh::teammates::Config &config, const QString &sessionStatus,
                  bool nativeInFlight);
bool statusIsWorking(const QString &status);

QJsonObject teammateRow(const forkmesh::teammates::Config &config, const QString &status,
                        const QDateTime &now);
QJsonObject teammateDetail(const forkmesh::teammates::Config &config, const QString &status,
                           const QString &latestOutput, const QDateTime &now);

inline constexpr int kMaxTextField = 200;
inline constexpr int kMaxLongText = 20000;
inline constexpr int kMaxPrompt = 8000;
inline constexpr int kMaxList = 40;
bool applyEdit(forkmesh::teammates::Config *config, const QJsonObject &changes,
               QString *error);
QStringList editableFields();

QString fingerprint(const QString &text);
QString fingerprint(const QJsonObject &object);

QStringList mutatingActions();
QString subjectFor(const QString &action, const QJsonObject &params, QString *error = nullptr);
QString actionSummary(const QString &action, const QString &subject, const QString &name);

struct TaskRecord {
    int session = 0;
    QString teammateId;
    QStringList teammateIds;
    QString title;
    QString status;
    int grade = 0;
    qint64 finishedAtMs = 0;
    double costUsd = 0;
    QString repo;
};
QJsonObject teamStats(const QList<forkmesh::teammates::Config> &teammates,
                      const QList<TaskRecord> &tasks, int maxTasks);

QString boundedOutput(const QString &text, int maxChars);

QJsonObject roomMessage(const QString &id, const QString &sender, const QString &text,
                        qint64 timestampMs, bool self, const QString &threadRootId);

} // namespace forkmesh::localcontrol::teammates
