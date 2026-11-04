#include "SpreadsheetsWeb.h"

#include <QCoreApplication>
#include <QUrl>

using namespace forkmesh::spreadsheets;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition) {
        qInfo("PASS: %s", message);
    } else {
        qCritical("FAIL: %s", message);
        ++failures;
    }
}

void checkOrigins()
{
    check(originOf(QUrl(QStringLiteral("https://App.ForkMesh.com:443/dashboard?x=1"))) ==
              QUrl(QStringLiteral("https://app.forkmesh.com")),
          "origin drops the path, the query, the case and the default port");
    check(originOf(QUrl(QStringLiteral("http://127.0.0.1:8787/x"))).port() == 8787,
          "origin keeps a non-default port");
    check(!originOf(QUrl(QStringLiteral("file:///etc/passwd"))).isValid(),
          "a file URL has no origin");
    check(sameOrigin(QUrl(QStringLiteral("https://app.forkmesh.com")),
                     QUrl(QStringLiteral("https://app.forkmesh.com:443/dashboard/crm"))),
          "the default port is the same origin");
    check(!sameOrigin(QUrl(QStringLiteral("https://app.forkmesh.com")),
                      QUrl(QStringLiteral("http://app.forkmesh.com/dashboard"))),
          "another scheme is another origin");
    check(!sameOrigin(QUrl(QStringLiteral("https://app.forkmesh.com")),
                      QUrl(QStringLiteral("https://app.forkmesh.com.evil.test/"))),
          "a lookalike host is another origin");
}

void checkPaths()
{
    const QUrl origin(QStringLiteral("https://app.forkmesh.com"));
    check(ownsPath(QStringLiteral("/dashboard/spreadsheets")), "the page path is owned");
    check(ownsPath(QStringLiteral("/dashboard/spreadsheets/")), "a trailing slash is owned");
    check(!ownsPath(QStringLiteral("/dashboard/spreadsheets-evil")), "a longer path is not owned");
    check(!ownsPath(QStringLiteral("/dashboard/crm")), "another page is not owned");
    check(ownsUrl(origin, QUrl(QStringLiteral(
                              "https://app.forkmesh.com/dashboard/spreadsheets?sheet=abc"))),
          "the page with a workbook query is owned");
    check(!ownsUrl(origin, QUrl(QStringLiteral("https://evil.test/dashboard/spreadsheets"))),
          "the page path on another origin is not owned");
    check(pathAndQuery(QUrl(QStringLiteral(
              "https://app.forkmesh.com/dashboard/spreadsheets?crmOrg=acme&crmList=no-pipeline#x"))) ==
              QStringLiteral("/dashboard/spreadsheets?crmOrg=acme&crmList=no-pipeline"),
          "path and query keep the query and drop the fragment");
}

void checkNavigation()
{
    const QUrl origin(QStringLiteral("https://app.forkmesh.com"));
    const QUrl page(QStringLiteral("https://app.forkmesh.com/dashboard/spreadsheets?sheet=1"));
    check(navigation(origin, page, false, true) == Navigation::Load, "the page loads in place");
    check(navigation(origin, page, true, true) == Navigation::Load,
          "a link to the page loads in place");
    check(navigation(origin, QUrl(QStringLiteral("https://example.com/")), true, true) ==
              Navigation::OpenLink,
          "a clicked outside link opens outside");
    check(navigation(origin, QUrl(QStringLiteral("mailto:someone@example.com")), true, true) ==
              Navigation::OpenLink,
          "a clicked mail link opens outside");
    check(navigation(origin, QUrl(QStringLiteral("https://app.forkmesh.com/dashboard/crm")), true,
                     true) == Navigation::OpenLink,
          "a clicked link to another dashboard page opens outside");
    check(navigation(origin, QUrl(QStringLiteral("https://app.forkmesh.com/dashboard/apps")), false,
                     true) == Navigation::OpenPage,
          "a script move to another dashboard page opens that page");
    check(navigation(origin, QUrl(QStringLiteral("https://app.forkmesh.com/login?next=x")), false,
                     true) == Navigation::Leave,
          "a sign-in redirect leaves the page");
    check(navigation(origin, QUrl(QStringLiteral("https://forkmesh.com/")), false, true) ==
              Navigation::Leave,
          "a sign-out redirect to the homepage leaves the page");
    check(navigation(origin, QUrl(QStringLiteral("https://app.forkmesh.com/login?next=x")), true,
                     true) == Navigation::Leave,
          "a script redirect to sign-in, which Qt reports as a link, leaves the page");
    check(navigation(origin, QUrl(QStringLiteral("https://forkmesh.com/")), true, true) ==
              Navigation::Leave,
          "a script sign-out redirect, which Qt reports as a link, leaves the page");
    check(navigation(origin, QUrl(QStringLiteral("https://app.forkmesh.com/verify-pending")), true,
                     true) == Navigation::Leave,
          "the email verification redirect leaves the page");
    check(navigation(origin, QUrl(QStringLiteral("https://forkmesh.com/changelog")), true, true) ==
              Navigation::OpenLink,
          "a link to a public site page opens outside");
    check(navigation(origin, QUrl(QStringLiteral("file:///home/user/x.xlsx")), true, true) ==
              Navigation::Block,
          "a file URL is blocked");
    check(navigation(origin, QUrl(QStringLiteral("file:///tmp/x")), false, false) ==
              Navigation::Block,
          "a file URL is blocked in a frame");
    check(navigation(origin, QUrl(QStringLiteral("about:srcdoc")), false, false) ==
              Navigation::Load,
          "the print frame loads");
    check(navigation(origin, QUrl(QStringLiteral("javascript:alert(1)")), true, true) ==
              Navigation::Block,
          "a script URL in the main frame is blocked");
    check(navigation(origin, QUrl(QStringLiteral("data:text/html,x")), false, true) ==
              Navigation::Block,
          "a data URL in the main frame is blocked");
}

void checkScripts()
{
    const QString script = documentScript(QUrl(QStringLiteral("https://app.forkmesh.com:443/x")),
                                          QStringLiteral("ada\"</script>"),
                                          QStringLiteral("v2.id.secret"), true);
    check(script.contains(QStringLiteral("location.origin !== \"https://app.forkmesh.com\"")),
          "the session script runs only on the host origin");
    check(script.contains(QStringLiteral("\"ada\\\"</script>\"")),
          "the account is a quoted string");
    check(script.contains(QStringLiteral("token = \"v2.id.secret\"")),
          "the session token is a quoted string");
    check(script.contains(QStringLiteral("localStorage.setItem(\"forkmesh.dashboard.theme\", "
                                         "\"dark\")")),
          "the session script sets the desktop theme");
    check(script.contains(QStringLiteral("[data-dashboard-sidebar]")) &&
              script.contains(QStringLiteral("[data-app-header]")),
          "the dashboard header and sidebar are hidden");
    check(themeScript(false).contains(QStringLiteral("const theme = \"light\"")),
          "the theme script names the theme");
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    checkOrigins();
    checkPaths();
    checkNavigation();
    checkScripts();
    if (failures)
        qCritical("%d check(s) failed", failures);
    return failures ? 1 : 0;
}
