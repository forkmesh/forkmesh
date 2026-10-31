#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace forkmesh::localcontrol::workflows {

QString familyPrefix();

inline constexpr int kMaxWorkflows = 200;
inline constexpr int kMaxRuns = 10;
inline constexpr int kMaxSteps = 60;
inline constexpr int kMaxSummaryChars = 300;
inline constexpr int kMaxRunOutputChars = 2000;
inline constexpr int kMaxStepOutputChars = 1000;
inline constexpr int kMaxErrorChars = 500;
inline constexpr int kMaxLogLines = 10;
inline constexpr int kMaxAssignmentChars = 8000;

QStringList actions();
bool validRepo(const QString &repo);
bool validId(const QString &id);
QString subjectFor(const QString &action, const QJsonObject &params, QString *error = nullptr);
QString actionSummary(const QString &action, const QString &name, const QString &repo);

QJsonObject listRow(const QJsonObject &row);
QJsonArray listRows(const QJsonArray &rows);
QJsonObject runRow(const QJsonObject &run);
QJsonObject detail(const QJsonObject &reply);

} // namespace forkmesh::localcontrol::workflows
