#pragma once

#include <QtGlobal>

#include <QString>

namespace UsageLimitCalendar {

QString eventText(const QString &providerKey, const QString &windowKey,
                  const QString &providerName, const QString &windowName,
                  qint64 resetMs, qint64 createdMs = 0);

// Atomically write the reminder under the app-data directory and return its
// absolute path. The same provider/window file is updated for later windows.
QString writeEvent(const QString &providerKey, const QString &windowKey,
                   const QString &providerName, const QString &windowName,
                   qint64 resetMs);

} // namespace UsageLimitCalendar
