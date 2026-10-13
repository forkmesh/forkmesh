#pragma once

#include "SavedSearchStore.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

struct SecurityAlarm {
    QString id;
    QString query;
    QString scope;
    int results = 0;
    QString fingerprint;
};

struct SecurityAlarmUpdate {
    QVector<SecurityAlarm> active;
    QVector<SecurityAlarm> raised;
    QStringList cleared;
    bool changed = false;
};

class SecurityAlarmState
{
public:
    static bool alarming(const SavedSearch &search);
    static QVector<SecurityAlarm> activeAlarms(const QVector<SavedSearch> &searches);
    static QString summary(const QVector<SecurityAlarm> &alarms);

    SecurityAlarmUpdate update(const QVector<SavedSearch> &searches);
    const QVector<SecurityAlarm> &active() const { return m_active; }
    bool isActive() const { return !m_active.isEmpty(); }

private:
    QVector<SecurityAlarm> m_active;
    QHash<QString, QString> m_announced;
};
