#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace forkmesh::bugreports {

inline constexpr int kMinMessage = 4;
inline constexpr int kMaxMessage = 4000;
inline constexpr int kMaxContact = 200;

struct Severity {
    QString value;
    QString label;
};

struct BugReport {
    QString id;
    QString status;
    QString severity;
    QString source;
    QString platform;
    QString appVersion;
    QString osVersion;
    QString device;
    QString account;
    QString contact;
    QString message;
    QString screen;
    QString package;
    qint64 createdAt = 0;
    qint64 confirmedFixedAt = 0;
    int attachments = 0;
};

struct StatusCounts {
    int fresh = 0;
    int triaged = 0;
    int closed = 0;
};

struct ReportList {
    QList<BugReport> reports;
    StatusCounts counts;
    bool more = false;
};

struct Draft {
    QString message;
    QString contact;
    QString severity;
    QString appVersion;
};

const QList<Severity> &severities();
const QStringList &inboxStatuses();
QString inboxStatusLabel(const QString &status);
QString mineStatusLabel(const BugReport &report);
QString statusTone(const QString &status);
QString detailsLine(const BugReport &report);
bool canConfirmFixed(const BugReport &report);

ReportList parseReportList(const QJsonObject &payload);
BugReport parseReport(const QJsonObject &object);
StatusCounts parseCounts(const QJsonObject &object);

bool messageLongEnough(const QString &message);
QString desktopPlatform();
QJsonObject reportPayload(const Draft &draft);
QJsonObject statusPayload(const QString &reportId, const QString &status);
QJsonObject confirmFixedPayload(const QString &reportId);

bool signedOutError(const QString &error);
bool adminRequiredError(const QString &error);
QString sendErrorText(const QString &error);
QString loadErrorText(const QString &error);

}
