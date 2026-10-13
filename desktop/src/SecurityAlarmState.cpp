#include "SecurityAlarmState.h"

#include <QSet>

bool SecurityAlarmState::alarming(const SavedSearch &search)
{
    return search.securityPertinent && search.runs > 0 && search.lastTotal > 0 &&
           !search.resultFingerprint.isEmpty() &&
           search.resultFingerprint != search.acknowledgedFingerprint;
}

QVector<SecurityAlarm>
SecurityAlarmState::activeAlarms(const QVector<SavedSearch> &searches)
{
    QVector<SecurityAlarm> alarms;
    for (const SavedSearch &search : searches) {
        if (!alarming(search))
            continue;
        SecurityAlarm alarm;
        alarm.id = search.id;
        alarm.query = search.query;
        alarm.scope = search.scope;
        alarm.results = search.lastTotal;
        alarm.fingerprint = search.resultFingerprint;
        alarms.append(alarm);
    }
    return alarms;
}

QString SecurityAlarmState::summary(const QVector<SecurityAlarm> &alarms)
{
    if (alarms.isEmpty())
        return {};
    const SecurityAlarm &first = alarms.first();
    QString text =
        QString::fromUtf8("Security alarm: saved search \xE2\x80\x9C%1\xE2\x80\x9D has %2 "
                          "result%3 in %4")
            .arg(first.query)
            .arg(first.results)
            .arg(first.results == 1 ? QString() : QStringLiteral("s"))
            .arg(first.scope.isEmpty() ? QStringLiteral("all repositories")
                                       : first.scope);
    if (alarms.size() > 1)
        text += QStringLiteral(" (and %1 more security-pertinent search%2)")
                    .arg(alarms.size() - 1)
                    .arg(alarms.size() == 2 ? QString() : QStringLiteral("es"));
    return text;
}

SecurityAlarmUpdate SecurityAlarmState::update(const QVector<SavedSearch> &searches)
{
    SecurityAlarmUpdate result;
    result.active = activeAlarms(searches);
    QSet<QString> activeIds;
    for (const SecurityAlarm &alarm : std::as_const(result.active)) {
        activeIds.insert(alarm.id);
        if (m_announced.value(alarm.id) != alarm.fingerprint) {
            result.raised.append(alarm);
            m_announced.insert(alarm.id, alarm.fingerprint);
        }
    }
    for (const SecurityAlarm &alarm : std::as_const(m_active))
        if (!activeIds.contains(alarm.id))
            result.cleared.append(alarm.id);
    for (auto it = m_announced.begin(); it != m_announced.end();) {
        if (activeIds.contains(it.key()))
            ++it;
        else
            it = m_announced.erase(it);
    }
    result.changed = !result.raised.isEmpty() || !result.cleared.isEmpty() ||
                     result.active.size() != m_active.size();
    if (!result.changed)
        for (int i = 0; i < result.active.size(); ++i)
            if (result.active.at(i).results != m_active.at(i).results ||
                result.active.at(i).id != m_active.at(i).id) {
                result.changed = true;
                break;
            }
    m_active = result.active;
    return result;
}
