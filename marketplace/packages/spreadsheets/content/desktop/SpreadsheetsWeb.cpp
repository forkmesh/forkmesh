#include "SpreadsheetsWeb.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace forkmesh::spreadsheets {

namespace {

const QString kChromeCss = QStringLiteral(
    "[data-dashboard-skip-link],[data-app-header],[data-dashboard-sidebar],"
    "[data-mobile-sidebar-backdrop],[data-update-notice]{display:none!important}");

QString jsString(const QString &text)
{
    const QByteArray json = QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}

bool webScheme(const QUrl &url)
{
    const QString scheme = url.scheme().toLower();
    return scheme == QLatin1String("http") || scheme == QLatin1String("https");
}

bool signedOutLanding(const QUrl &origin, const QUrl &url)
{
    const QString path = url.path(QUrl::FullyDecoded);
    if (sameOrigin(origin, url))
        return path.isEmpty() || path == QLatin1String("/") ||
               path == QLatin1String("/login") || path.startsWith(QLatin1String("/login/")) ||
               path == QLatin1String("/logout") || path == QLatin1String("/signup") ||
               path == QLatin1String("/verify-pending");
    const QString host = url.host().toLower();
    return (host == QLatin1String("forkmesh.com") || host == QLatin1String("www.forkmesh.com")) &&
           (path.isEmpty() || path == QLatin1String("/"));
}

QString themeName(bool dark)
{
    return dark ? QStringLiteral("dark") : QStringLiteral("light");
}

}

QUrl originOf(const QUrl &url)
{
    if (!url.isValid() || !webScheme(url) || url.host().isEmpty())
        return {};
    QUrl origin;
    origin.setScheme(url.scheme().toLower());
    origin.setHost(url.host().toLower());
    const int standard = origin.scheme() == QLatin1String("https") ? 443 : 80;
    origin.setPort(url.port() == standard ? -1 : url.port());
    return origin;
}

bool sameOrigin(const QUrl &left, const QUrl &right)
{
    const QUrl a = originOf(left);
    return a.isValid() && a == originOf(right);
}

bool ownsPath(const QString &path)
{
    return path == kPagePath || path == kPagePath + QLatin1Char('/');
}

bool ownsUrl(const QUrl &origin, const QUrl &url)
{
    return sameOrigin(origin, url) && ownsPath(url.path(QUrl::FullyDecoded));
}

QString pathAndQuery(const QUrl &url)
{
    QString text = url.path(QUrl::FullyEncoded);
    if (url.hasQuery())
        text += QLatin1Char('?') + url.query(QUrl::FullyEncoded);
    return text;
}

Navigation navigation(const QUrl &origin, const QUrl &url, bool linkClicked, bool mainFrame)
{
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("file"))
        return Navigation::Block;
    if (!mainFrame)
        return Navigation::Load;
    if (!webScheme(url))
        return linkClicked && scheme == QLatin1String("mailto") ? Navigation::OpenLink
                                                                : Navigation::Block;
    if (ownsUrl(origin, url))
        return Navigation::Load;
    if (signedOutLanding(origin, url))
        return Navigation::Leave;
    if (linkClicked)
        return Navigation::OpenLink;
    if (sameOrigin(origin, url) &&
        url.path(QUrl::FullyDecoded).startsWith(QLatin1String("/dashboard/")))
        return Navigation::OpenPage;
    return Navigation::Leave;
}

QString documentScript(const QUrl &origin, const QString &account, const QString &sessionToken,
                       bool dark)
{
    const QUrl clean = originOf(origin);
    return QStringLiteral(
               "(() => {\n"
               "  if (location.origin !== %1) return;\n"
               "  const account = %2, token = %3;\n"
               "  try {\n"
               "    const stored = JSON.parse(localStorage.getItem(\"forkmesh.session\") || "
               "\"null\");\n"
               "    const same = stored && String(stored.nodeName || \"\").toLowerCase() === "
               "account.toLowerCase();\n"
               "    if (!same || (stored.sessionToken !== token && stored.sessionToken !== "
               "\"cookie\"))\n"
               "      localStorage.setItem(\"forkmesh.session\", JSON.stringify({ nodeName: "
               "account, sessionToken: token }));\n"
               "    localStorage.setItem(\"forkmesh.dashboard.theme\", %4);\n"
               "  } catch (_) {}\n"
               "  const css = %5;\n"
               "  try {\n"
               "    const sheet = new CSSStyleSheet();\n"
               "    sheet.replaceSync(css);\n"
               "    document.adoptedStyleSheets = [...document.adoptedStyleSheets, sheet];\n"
               "  } catch (_) {\n"
               "    document.addEventListener(\"DOMContentLoaded\", () => {\n"
               "      const style = document.createElement(\"style\");\n"
               "      style.textContent = css;\n"
               "      document.head.appendChild(style);\n"
               "    });\n"
               "  }\n"
               "})();\n")
        .arg(jsString(clean.toString(QUrl::FullyEncoded)), jsString(account),
             jsString(sessionToken), jsString(themeName(dark)), jsString(kChromeCss));
}

QString themeScript(bool dark)
{
    return QStringLiteral(
               "(() => {\n"
               "  const theme = %1;\n"
               "  try { localStorage.setItem(\"forkmesh.dashboard.theme\", theme); } catch (_) {}\n"
               "  document.documentElement.dataset.dashboardTheme = theme;\n"
               "  document.documentElement.style.colorScheme = theme;\n"
               "})();\n")
        .arg(jsString(themeName(dark)));
}

}
