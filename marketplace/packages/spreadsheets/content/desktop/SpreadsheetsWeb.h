#pragma once

#include <QString>
#include <QUrl>

namespace forkmesh::spreadsheets {

inline const QString kPagePath = QStringLiteral("/dashboard/spreadsheets");

enum class Navigation { Load, OpenLink, OpenPage, Leave, Block };

QUrl originOf(const QUrl &url);
bool sameOrigin(const QUrl &left, const QUrl &right);
bool ownsPath(const QString &path);
bool ownsUrl(const QUrl &origin, const QUrl &url);
QString pathAndQuery(const QUrl &url);
Navigation navigation(const QUrl &origin, const QUrl &url, bool linkClicked, bool mainFrame);
QString documentScript(const QUrl &origin, const QString &account, const QString &sessionToken,
                       bool dark);
QString themeScript(bool dark);

}
