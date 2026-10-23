#pragma once

#include <QDate>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

class QDateTime;
class QEvent;
class QTime;

namespace forkmesh::activity {

inline constexpr int kActivityHours = 24;
inline constexpr int kActivitySlotsPerHour = 12;
inline constexpr int kActivitySlotMinutes = 60 / kActivitySlotsPerHour;
inline constexpr int kActivitySlots = kActivityHours * kActivitySlotsPerHour;

int activitySlotFor(const QTime &time);
QString activitySourcesText(const QString &selfKind, const QStringList &otherKinds);

class UserActivityLog : public QObject
{
public:
    static UserActivityLog *instance();
    static QString defaultPath();
    static bool countsAsInteraction(QObject *watched, QEvent *event);

    explicit UserActivityLog(const QString &path, QObject *parent = nullptr);
    ~UserActivityLog() override;

    void record(const QDateTime &when);
    QVector<int> slotsFor(const QDate &day) const;
    QVector<int> today() const;
    QVector<int> localSlotsFor(const QDate &day) const;
    QString deviceKey() const { return m_deviceKey; }
    quint64 revision() const { return m_revision; }
    void setOthers(const QDate &day, const QVector<int> &counts,
                   const QStringList &kinds);
    QStringList otherKindsFor(const QDate &day) const;
    void clearOthers();
    void flush();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void load();
    QByteArray snapshotBytes() const;
    void flushInBackground();

    QString m_path;
    QDate m_day;
    QVector<int> m_counts;
    QString m_deviceKey;
    QDate m_othersDay;
    QVector<int> m_others;
    QStringList m_otherKinds;
    quint64 m_revision = 0;
    qint64 m_lastSaveMs = 0;
    bool m_writeQueued = false;
    bool m_dirty = false;
};

}
