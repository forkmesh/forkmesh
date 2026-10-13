#pragma once

#include "DesktopPingForward.h"

#include <QList>
#include <QString>

namespace forkmesh::poweralert {

inline constexpr qint64 kConfirmMs = 15 * 1000;
inline constexpr int kPollMs = 5 * 1000;

enum class Source { Unknown, Mains, Battery };
enum class Change { None, LostPower, Restored };

struct Supply {
    QString type;
    QString scope;
    QString online;
    QString status;
    QString capacity;
};

struct Reading {
    Source source = Source::Unknown;
    int percent = -1;
};

QString lostKind();
QString restoredKind();
QString settingKey();
bool isPowerKind(const QString &kind);

Reading fromSupplies(const QList<Supply> &supplies);
Reading combine(int upowerOnBattery, const Reading &supplies);

pingforward::Source lostSource(const QString &node, const Reading &reading,
                               qint64 nowMs);
pingforward::Source restoredSource(const QString &node, qint64 outageMs,
                                   qint64 nowMs);

class Watch
{
public:
    Change observe(Source source, qint64 nowMs);
    qint64 lastOutageMs() const { return m_lastOutageMs; }

private:
    bool m_seenMains = false;
    bool m_alarmed = false;
    qint64 m_batterySinceMs = 0;
    qint64 m_lastOutageMs = 0;
};

} // namespace forkmesh::poweralert
