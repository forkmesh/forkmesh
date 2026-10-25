#include "BrowserPrivacy.h"

#include <QCoreApplication>
#include <QProcess>
#include <QUrlQuery>

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

QStringList tokens(const QByteArray &flags)
{
    return QProcess::splitCommand(QString::fromLocal8Bit(flags));
}

QStringList disabledFeatures(const QStringList &switches)
{
    QStringList out;
    for (const QString &token : switches)
        if (token.startsWith(QStringLiteral("--disable-features=")))
            out.append(token);
    return out;
}

void testFlagsFromEmpty()
{
    using namespace forkmesh::browser;
    const QStringList merged = tokens(mergeChromiumFlags(QByteArray()));
    for (const QString &flag : ungoogledSwitches())
        check(merged.contains(flag), "every ungoogled switch is present");
    check(merged.contains(QStringLiteral("--disable-background-networking")),
          "background networking is off");
    check(merged.contains(QStringLiteral("--no-pings")), "hyperlink pings are off");
    const QStringList features = disabledFeatures(merged);
    check(features.size() == 1, "one --disable-features switch");
    check(features.value(0).contains(QStringLiteral("MediaRouter")) &&
              features.value(0).contains(QStringLiteral("OptimizationHints")) &&
              features.value(0).contains(QStringLiteral("Translate")),
          "Google feature list is disabled");
}

void testFlagsKeepUserFlags()
{
    using namespace forkmesh::browser;
    const QStringList merged = tokens(mergeChromiumFlags(
        "--disable-gpu --disable-features=Foo,MediaRouter --no-pings --lang=en-GB"));
    check(merged.contains(QStringLiteral("--disable-gpu")), "user switch kept");
    check(merged.contains(QStringLiteral("--lang=en-GB")), "user value switch kept");
    check(merged.count(QStringLiteral("--no-pings")) == 1, "duplicate switch collapsed");
    const QStringList features = disabledFeatures(merged);
    check(features.size() == 1, "user --disable-features merged into one switch");
    const QStringList list =
        features.value(0).mid(QStringLiteral("--disable-features=").size()).split(QLatin1Char(','));
    check(list.contains(QStringLiteral("Foo")), "user disabled feature kept");
    check(list.count(QStringLiteral("MediaRouter")) == 1, "duplicate feature collapsed");
    check(merged.indexOf(QStringLiteral("--disable-gpu")) >
              merged.indexOf(QStringLiteral("--disable-background-networking")),
          "user switches come after ungoogled switches so they can override");
}

void testInstallRespectsOptOut()
{
    using namespace forkmesh::browser;
    qputenv(kChromiumFlagsVariable, "--disable-gpu");
    qputenv(kKeepGoogleVariable, "1");
    installUngoogledChromiumFlags();
    check(qgetenv(kChromiumFlagsVariable) == "--disable-gpu",
          "opt-out leaves the Chromium flags alone");
    qunsetenv(kKeepGoogleVariable);
    installUngoogledChromiumFlags();
    const QStringList installed = tokens(qgetenv(kChromiumFlagsVariable));
    check(installed.contains(QStringLiteral("--disable-gpu")) &&
              installed.contains(QStringLiteral("--disable-component-update")),
          "install merges ungoogled switches into the environment");
    installUngoogledChromiumFlags();
    check(tokens(qgetenv(kChromiumFlagsVariable)) == installed,
          "installing twice changes nothing");
    qunsetenv(kChromiumFlagsVariable);
}

void testGoogleServiceHosts()
{
    using forkmesh::browser::isGoogleServiceHost;
    check(isGoogleServiceHost(QStringLiteral("safebrowsing.googleapis.com")),
          "Safe Browsing is blocked");
    check(isGoogleServiceHost(QStringLiteral("UPDATE.googleapis.com.")),
          "host match ignores case and a trailing dot");
    check(isGoogleServiceHost(QStringLiteral("beacons.gcp.gvt2.com")),
          "domain reliability beacons are blocked");
    check(isGoogleServiceHost(QStringLiteral("r1---sn-abc.edgedl.me.gvt1.com")),
          "component download mirrors are blocked");
    check(!isGoogleServiceHost(QStringLiteral("www.google.com")),
          "Google search itself still loads");
    check(!isGoogleServiceHost(QStringLiteral("mail.google.com")), "Gmail still loads");
    check(!isGoogleServiceHost(QStringLiteral("notgvt2.com")),
          "suffix match needs a label boundary");
    check(!isGoogleServiceHost(QString()), "empty host is not blocked");
}

void testUserAgent()
{
    using forkmesh::browser::ungoogledUserAgent;
    const QString stock = QStringLiteral(
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
        "QtWebEngine/6.8.2 Chrome/122.0.6261.171 Safari/537.36");
    check(ungoogledUserAgent(stock) ==
              QStringLiteral("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                             "(KHTML, like Gecko) Chrome/122.0.6261.171 Safari/537.36"),
          "QtWebEngine token is removed from the user agent");
    const QString plain = QStringLiteral("Mozilla/5.0 Chrome/122.0 Safari/537.36");
    check(ungoogledUserAgent(plain) == plain, "a user agent without the token is unchanged");
}

void testUrlFromInput()
{
    using forkmesh::browser::urlFromInput;
    check(urlFromInput(QStringLiteral("3000")) == QUrl(QStringLiteral("http://localhost:3000")),
          "bare port opens localhost");
    check(urlFromInput(QStringLiteral(":8080/app")) ==
              QUrl(QStringLiteral("http://localhost:8080/app")),
          "port with path opens localhost");
    check(urlFromInput(QStringLiteral("localhost:5173")) ==
              QUrl(QStringLiteral("http://localhost:5173")),
          "localhost gets http");
    check(urlFromInput(QStringLiteral("example.com")) == QUrl(QStringLiteral("http://example.com")),
          "domain gets a scheme");
    check(urlFromInput(QStringLiteral("https://example.com/a?b=1")) ==
              QUrl(QStringLiteral("https://example.com/a?b=1")),
          "full URL is kept");
    const QUrl search = urlFromInput(QStringLiteral("  qt webengine flags "));
    check(search.host() == QStringLiteral("duckduckgo.com") &&
              QUrlQuery(search).queryItemValue(QStringLiteral("q")) ==
                  QStringLiteral("qt webengine flags"),
          "words search DuckDuckGo");
    check(urlFromInput(QStringLiteral("forkmesh")).host() == QStringLiteral("duckduckgo.com"),
          "a single word searches");
    check(!urlFromInput(QStringLiteral("file:///etc/passwd")).isValid(), "file URLs are refused");
    check(!urlFromInput(QStringLiteral("ftp://example.com")).isValid(),
          "non-web schemes are refused");
    check(!urlFromInput(QStringLiteral("mailto:someone@example.com")).isValid(),
          "mailto is refused");
    check(!urlFromInput(QStringLiteral("   ")).isValid(), "blank input is empty");
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testFlagsFromEmpty();
    testFlagsKeepUserFlags();
    testInstallRespectsOptOut();
    testGoogleServiceHosts();
    testUserAgent();
    testUrlFromInput();
    if (failures) {
        std::fprintf(stderr, "%d browser privacy check(s) failed\n", failures);
        return 1;
    }
    std::printf("browser privacy checks passed\n");
    return 0;
}
