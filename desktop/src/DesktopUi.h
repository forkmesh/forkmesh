#pragma once

#include "Theme.h"

#include <QByteArray>
#include <QColor>
#include <QIcon>
#include <QLatin1String>
#include <QPixmap>
#include <QString>
#include <QStringList>

namespace forkmesh::ui {

const QLatin1String kUserLinkScheme("forkmesh-user:");
const QLatin1String kAgentLinkScheme("forkmesh-agent:");

bool currentThemeIsDark();
qreal iconDevicePixelRatio();
QPixmap crispIconPixmap(int w, int h, qreal dpr);
QPixmap crispIconPixmap(int size, qreal dpr);
QByteArray forkMeshAvatarPng(const QString &seed);
QPixmap roundedAvatar(const QPixmap &src, int side, qreal radiusRatio = 0.28);
QPixmap roundedAvatar(const QByteArray &png, int side, qreal radiusRatio = 0.28);
QPixmap tintedOcticonPixmap(const QString &name, const QColor &color, int size);
QString octiconMarkup(const QString &name, int size,
                      const QColor &color = Theme::remapColor(QColor("#8b949e")));
QPixmap agentRunningFireballPixmap(int size, qreal angleDeg);
QIcon agentFaceIcon(const QString &provider, const QString &model);
QString formatCount(qint64 n);
QString formatShortRelativeTime(qint64 timestampSecs);
QString userProfileLinkHtml(const QString &account, const QString &shown = QString());
bool isVideoAttachmentPath(const QString &path);
QString promptAttachmentLine(const QString &path);
QStringList promptAttachmentPaths(const QString &prompt);
QString promptTextWithoutAttachments(const QString &prompt);

}
