#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QtGlobal>

namespace forkmesh {

class PlannedRestarts
{
public:
    static constexpr qint64 kMaxWindowMs = 15LL * 60 * 1000;

    enum class Kind { Deploy, Roll };

    struct Change {
        enum Type { None, Opened, Extended, Closed };
        Type type = None;
        QString node;
        Kind kind = Kind::Deploy;
    };

    struct Expired {
        QString node;
        Kind kind = Kind::Deploy;
        qint64 windowMs = 0;
    };

    Change observe(const QString &line, qint64 nowMs);
    bool quiets(const QString &line, qint64 nowMs, QString *node = nullptr,
                Kind *kind = nullptr) const;
    bool claimAnnouncement(const QString &node);
    QList<Expired> takeExpired(qint64 nowMs);
    bool isOpen(const QString &node) const { return m_windows.contains(node); }
    qint64 nextDeadlineMs() const;
    void clear() { m_windows.clear(); }

    static QString kindWord(Kind kind);
    static bool isRestartNoise(const QString &line, QString *node = nullptr);
    static QString announcementTitle(const QString &node, Kind kind);
    static QString failureLine(const Expired &expired);

private:
    struct Window {
        Kind kind = Kind::Deploy;
        qint64 openedMs = 0;
        qint64 deadlineMs = 0;
        bool announced = false;
    };
    QHash<QString, Window> m_windows;
};

}
