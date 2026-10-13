#pragma once

#include <QString>
#include <QUrl>

namespace forkmesh::mainnode {

inline const QString kDefaultHost = QStringLiteral("app.forkmesh.com");
inline const QString kDefaultRoomName = QStringLiteral("world-general");
inline const QString kLegacyDefaultRoomName = QStringLiteral("general");
inline const QString kRoomPath =
    QStringLiteral("/api/repo/mainnode/forkmesh/rooms/world-general/ws");
inline const QString kLegacyRoomPath =
    QStringLiteral("/api/repo/mainnode/forkmesh/rooms/general/ws");
inline const QString kLocalServerUrl =
    QStringLiteral("ws://127.0.0.1:8787") + kRoomPath;
inline const QString kDefaultServerUrl =
    QStringLiteral("wss://") + kDefaultHost + kRoomPath;

inline bool migrateSavedDefaultRoom(QString *serverUrl, QString *roomName)
{
    if (!serverUrl || !roomName)
        return false;

    QUrl url(serverUrl->trimmed());
    if (!url.isValid() || url.host().isEmpty() ||
        (url.scheme() != QLatin1String("ws") &&
         url.scheme() != QLatin1String("wss")))
        return false;

    const QString room = roomName->trimmed();
    const bool knownDefaultRoom =
        room.isEmpty() ||
        room.compare(kLegacyDefaultRoomName, Qt::CaseInsensitive) == 0 ||
        room.compare(kDefaultRoomName, Qt::CaseInsensitive) == 0;
    const bool knownDefaultPath =
        url.path() == kLegacyRoomPath || url.path() == kRoomPath;
    if (!knownDefaultRoom || !knownDefaultPath)
        return false;

    bool changed = false;
    const QString host = url.host().toLower();
    if (url.scheme() == QLatin1String("wss") && url.port() == -1 &&
        (host == QLatin1String("forkmesh.com") ||
         host == QLatin1String("api.forkmesh.com"))) {
        url.setHost(kDefaultHost);
        changed = true;
    }
    if (url.path() != kRoomPath) {
        url.setPath(kRoomPath);
        changed = true;
    }
    if (changed)
        *serverUrl = url.toString(QUrl::FullyEncoded);
    if (*roomName != kDefaultRoomName) {
        *roomName = kDefaultRoomName;
        changed = true;
    }
    return changed;
}

} // namespace forkmesh::mainnode
