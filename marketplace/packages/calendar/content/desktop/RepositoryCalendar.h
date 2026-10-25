#pragma once

#include <QDate>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QTime>

namespace forkmesh::ui {

extern const QString kRepositoryCommitsCalendarId;
extern const QString kRepositoryMergesCalendarId;

QJsonArray repositoryCalendarEvents(const QString &gitPath,
                                    const QString &repository,
                                    qint64 startAtMs,
                                    qint64 endAtMs);

struct RepositoryCalendarScan {
    QString gitPath;
    QString label;
    QString key;
    qint64 startAtMs = 0;
    qint64 endAtMs = 0;
};

QList<QPair<qint64, qint64>> repositoryCalendarMonths(const QDate &first,
                                                      const QDate &last);

struct CalendarOccurrence {
    QJsonObject event;
    qint64 startAtMs = 0;
    qint64 endAtMs = 0;
    QDate date;
    QTime time;
};

}
