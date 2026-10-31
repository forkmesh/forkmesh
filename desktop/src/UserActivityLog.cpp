#include "UserActivityLog.h"

#include "AppPaths.h"
#include "BackgroundFileWriter.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTime>
#include <QUuid>
#include <QWheelEvent>

namespace forkmesh::activity {

namespace {

constexpr qint64 kSaveIntervalMs = 60 * 1000;

UserActivityLog *g_instance = nullptr;

}

int activitySlotFor(const QTime &time)
{
    if (!time.isValid())
        return -1;
    return time.hour() * kActivitySlotsPerHour +
           time.minute() / kActivitySlotMinutes;
}

QString activitySourcesText(const QString &selfKind, const QStringList &otherKinds)
{
    const auto label = [](const QString &kind) {
        if (kind == QLatin1String("desktop"))
            return QStringLiteral("desktop");
        if (kind == QLatin1String("ios"))
            return QStringLiteral("iOS device");
        if (kind == QLatin1String("android"))
            return QStringLiteral("Android device");
        if (kind == QLatin1String("web"))
            return QStringLiteral("browser");
        return QStringLiteral("other device");
    };
    QStringList parts{selfKind == QLatin1String("desktop") ? QStringLiteral("this desktop")
                      : selfKind == QLatin1String("web")   ? QStringLiteral("this browser")
                                                           : QStringLiteral("this device")};
    QStringList seen;
    for (const QString &kind : otherKinds) {
        if (seen.contains(kind))
            continue;
        seen.append(kind);
        parts.append(kind == selfKind ? QStringLiteral("another ") + label(kind)
                                      : label(kind));
    }
    if (parts.size() == 1)
        return parts.first();
    const QString last = parts.takeLast();
    return parts.join(QStringLiteral(", ")) + QStringLiteral(" and ") + last;
}

UserActivityLog *UserActivityLog::instance()
{
    if (!qApp)
        return nullptr;
    if (!g_instance) {
        g_instance = new UserActivityLog(defaultPath(), qApp);
        qApp->installEventFilter(g_instance);
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, g_instance,
                         [] {
                             if (g_instance)
                                 g_instance->flush();
                         });
    }
    return g_instance;
}

QString UserActivityLog::defaultPath()
{
    if (QStandardPaths::isTestModeEnabled())
        return QString();
    const QString platform = QGuiApplication::platformName();
    if (platform == QLatin1String("offscreen") ||
        platform == QLatin1String("minimal"))
        return QString();
    return forkmesh::paths::stateRoot() +
           QStringLiteral("/user-activity-today.json");
}

bool UserActivityLog::countsAsInteraction(QObject *watched, QEvent *event)
{
    if (!watched || !event || !watched->isWindowType() || !event->spontaneous())
        return false;
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::KeyPress:
    case QEvent::TouchBegin:
    case QEvent::TabletPress:
        return true;
    case QEvent::Wheel: {
        const Qt::ScrollPhase phase = static_cast<QWheelEvent *>(event)->phase();
        return phase == Qt::NoScrollPhase || phase == Qt::ScrollBegin;
    }
    default:
        return false;
    }
}

UserActivityLog::UserActivityLog(const QString &path, QObject *parent)
    : QObject(parent), m_path(path), m_counts(kActivitySlots, 0)
{
    load();
    if (m_deviceKey.isEmpty())
        m_deviceKey = QUuid::createUuid().toString(QUuid::WithoutBraces);
}

UserActivityLog::~UserActivityLog()
{
    flush();
    if (g_instance == this)
        g_instance = nullptr;
}

void UserActivityLog::load()
{
    if (m_path.isEmpty())
        return;
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject object = QJsonDocument::fromJson(file.read(64 * 1024)).object();
    const QString device = object.value(QStringLiteral("device")).toString();
    if (device.size() >= 8 && device.size() <= 64)
        m_deviceKey = device;
    const QDate day =
        QDate::fromString(object.value(QStringLiteral("day")).toString(),
                          Qt::ISODate);
    const QJsonArray counts = object.value(QStringLiteral("counts")).toArray();
    if (!day.isValid() || counts.size() != kActivitySlots)
        return;
    m_day = day;
    for (int i = 0; i < kActivitySlots; ++i)
        m_counts[i] = qMax(0, counts.at(i).toInt());
}

void UserActivityLog::record(const QDateTime &when)
{
    const QDateTime local = when.toLocalTime();
    const int slot = activitySlotFor(local.time());
    if (slot < 0 || slot >= kActivitySlots)
        return;
    if (local.date() != m_day) {
        m_day = local.date();
        m_counts.fill(0);
    }
    ++m_counts[slot];
    ++m_revision;
    m_dirty = true;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs - m_lastSaveMs >= kSaveIntervalMs)
        flushInBackground();
}

QVector<int> UserActivityLog::localSlotsFor(const QDate &day) const
{
    if (day.isValid() && day == m_day)
        return m_counts;
    return QVector<int>(kActivitySlots, 0);
}

QVector<int> UserActivityLog::slotsFor(const QDate &day) const
{
    QVector<int> counts = localSlotsFor(day);
    if (day.isValid() && day == m_othersDay)
        for (int i = 0; i < kActivitySlots && i < m_others.size(); ++i)
            counts[i] += m_others.at(i);
    return counts;
}

void UserActivityLog::setOthers(const QDate &day, const QVector<int> &counts,
                                const QStringList &kinds)
{
    if (!day.isValid() || counts.size() != kActivitySlots)
        return;
    m_othersDay = day;
    m_others = counts;
    for (int &value : m_others)
        value = qMax(0, value);
    m_otherKinds = kinds;
}

void UserActivityLog::clearOthers()
{
    m_othersDay = QDate();
    m_others.clear();
    m_otherKinds.clear();
}

QStringList UserActivityLog::otherKindsFor(const QDate &day) const
{
    return day.isValid() && day == m_othersDay ? m_otherKinds : QStringList();
}

QVector<int> UserActivityLog::today() const
{
    return slotsFor(QDate::currentDate());
}

QByteArray UserActivityLog::snapshotBytes() const
{
    QJsonArray counts;
    for (int value : std::as_const(m_counts))
        counts.append(value);
    const QJsonObject object{
        {QStringLiteral("day"), m_day.toString(Qt::ISODate)},
        {QStringLiteral("device"), m_deviceKey},
        {QStringLiteral("counts"), counts},
    };
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

void UserActivityLog::flush()
{
    m_lastSaveMs = QDateTime::currentMSecsSinceEpoch();
    if ((!m_dirty && !m_writeQueued) || m_path.isEmpty() || !m_day.isValid())
        return;
    if (forkmesh::diskwrite::writeNow(m_path, snapshotBytes())) {
        m_dirty = false;
        m_writeQueued = false;
    }
}

void UserActivityLog::flushInBackground()
{
    m_lastSaveMs = QDateTime::currentMSecsSinceEpoch();
    if (!m_dirty || m_path.isEmpty() || !m_day.isValid())
        return;
    forkmesh::diskwrite::writeLater(m_path, snapshotBytes());
    m_dirty = false;
    m_writeQueued = true;
}

bool UserActivityLog::eventFilter(QObject *watched, QEvent *event)
{
    if (countsAsInteraction(watched, event))
        record(QDateTime::currentDateTime());
    return false;
}

}
