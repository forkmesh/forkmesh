#pragma once

#include <QDateTime>
#include <QString>

namespace forkmesh::usageclock {

QString resetClockLabel(qint64 resetAtMs, const QDateTime &now);

QString noteWithResetClock(const QString &note, qint64 resetAtMs,
                           const QDateTime &now);

}
