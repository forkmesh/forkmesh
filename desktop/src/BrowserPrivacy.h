#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace forkmesh::browser {

inline constexpr char kChromiumFlagsVariable[] = "QTWEBENGINE_CHROMIUM_FLAGS";
inline constexpr char kKeepGoogleVariable[] = "FORKMESH_BROWSER_KEEP_GOOGLE";

QStringList ungoogledSwitches();
QStringList ungoogledDisabledFeatures();
QByteArray mergeChromiumFlags(const QByteArray &existing);
void installUngoogledChromiumFlags();

bool isGoogleServiceHost(const QString &host);
QString ungoogledUserAgent(const QString &userAgent);

QUrl searchUrl(const QString &query);
QUrl urlFromInput(const QString &text);

}
