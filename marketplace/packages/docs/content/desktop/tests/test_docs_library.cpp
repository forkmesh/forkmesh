#include "DocsLibrary.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdio>

using forkmesh::docs::DocsDependency;
using forkmesh::docs::DocsHit;
using forkmesh::docs::DocsLibrary;
using forkmesh::docs::DocsPage;
using forkmesh::docs::DocsTarget;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition) {
        std::printf("PASS: %s\n", message);
    } else {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
}

void write(const QString &path, const QByteArray &body)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly))
        file.write(body);
}

void fixture(const QString &root)
{
    write(root + QStringLiteral("/README.md"),
          "# ForkMesh docs\n\nWelcome to the [library](getting-started.md).\n");
    write(root + QStringLiteral("/getting-started.md"),
          "# Getting started\n\nInstall the **desktop** and [sign in](#sign-in).\n\n"
          "## Install\n\nRead the [wire format](design/protocol.md#wire-format), the "
          "[main source](../../desktop/src/main.cpp), the [site](../public/pricing.html), "
          "[governance](governance/), [the web](https://example.com) and the "
          "[docs page](/docs/protocol/).\n");
    write(root + QStringLiteral("/business-model.md"), "# Business model\n\nFree protocol.\n");
    write(root + QStringLiteral("/zeta.md"), "# Alpha after the curated guides\n\nText.\n");
    write(root + QStringLiteral("/untitled.md"), "No heading here, only prose.\n");
    write(root + QStringLiteral("/design/protocol.md"),
          "# Protocol\n\n> A quote first.\n\nThe wire format carries signed objects.\n\n"
          "```\n# not a title\n[fake](nowhere.md)\n```\n\n## Wire format\n\n"
          "Back to [getting started](../getting-started.md) and [home](../README.md).\n");
    write(root + QStringLiteral("/research/brief.md"), "# Weekly brief\n\nPricing research.\n");
    write(root + QStringLiteral("/governance/README.md"), "# Governance\n\nWho decides.\n");
    write(root + QStringLiteral("/mirror-endpoint.schema.json"),
          "{\"description\": \"A mirror endpoint.\"}");
    write(root + QStringLiteral("/ssh-gateway-config.schema.json"), "{}");
    write(root + QStringLiteral("/.hidden/secret.md"), "# Hidden\n");
}

QStringList paths(const DocsLibrary &library)
{
    QStringList out;
    for (const DocsPage &page : library.pages())
        out.append(page.path);
    return out;
}

void testCatalog(const DocsLibrary &library)
{
    const QStringList order = paths(library);
    check(order.size() == 10, "every markdown and json file is a page, hidden folders are not");
    check(order.value(0) == QStringLiteral("README.md") &&
              order.value(1) == QStringLiteral("getting-started.md") &&
              order.value(2) == QStringLiteral("business-model.md"),
          "curated guides lead in the web library's order");
    check(order.indexOf(QStringLiteral("governance/README.md")) <
              order.indexOf(QStringLiteral("untitled.md")) &&
              order.indexOf(QStringLiteral("untitled.md")) < order.indexOf(QStringLiteral("zeta.md")),
          "other guides sort by path like the web library");
    check(order.indexOf(QStringLiteral("design/protocol.md")) >
                  order.indexOf(QStringLiteral("zeta.md")) &&
              order.indexOf(QStringLiteral("research/brief.md")) >
                  order.indexOf(QStringLiteral("design/protocol.md")) &&
              order.last().endsWith(QLatin1String(".json")),
          "areas follow the web order with schemas last");
    check(library.page(QStringLiteral("research/brief.md"))->area == QStringLiteral("strategy"),
          "research folders file under strategy");
    check(library.page(QStringLiteral("governance/README.md"))->area == QStringLiteral("guides"),
          "unmapped folders file under guides");
    check(library.pagesIn(QStringLiteral("schemas")).size() == 2, "json files are schemas");
    check(library.homePath() == QStringLiteral("README.md"), "the library README is home");
}

void testTitles(const DocsLibrary &library)
{
    const DocsPage *protocol = library.page(QStringLiteral("design/protocol.md"));
    check(protocol->title == QStringLiteral("Protocol"), "title is the first heading");
    check(protocol->description == QStringLiteral("A quote first."),
          "a leading blockquote is the description, as on the web");
    check(DocsLibrary::descriptionOf(QStringLiteral("# T\n\nSee <https://x.test/a> and `<id>.json`\n#709).\n")) ==
              QStringLiteral("See https://x.test/a and <id>.json #709)."),
          "autolinks, placeholders and a leading # mid-paragraph stay in the description");
    check(DocsLibrary::descriptionOf(QStringLiteral("<!-- Generated. -->\n# T\n\nBody [`a`](b).\n")) ==
              QStringLiteral("Body a."),
          "comments are skipped and code link labels keep their text");
    check(library.page(QStringLiteral("getting-started.md"))->description ==
              QStringLiteral("Install the desktop and sign in."),
          "description drops inline markdown");
    check(library.page(QStringLiteral("untitled.md"))->title == QStringLiteral("untitled"),
          "a page without a heading is named after its file");
    const DocsPage *mirror = library.page(QStringLiteral("mirror-endpoint.schema.json"));
    check(mirror->asset && mirror->title == QStringLiteral("Mirror endpoint schema") &&
              mirror->description == QStringLiteral("A mirror endpoint."),
          "schemas are titled and described like the web library");
    check(mirror->markdown.contains(QLatin1String("```json")), "schemas render as a json block");
    const DocsPage *ssh = library.page(QStringLiteral("ssh-gateway-config.schema.json"));
    check(ssh->title == QStringLiteral("SSH gateway config schema") &&
              ssh->description == QStringLiteral("Machine-readable data file."),
          "schema acronyms and the fallback description match the web library");
    check(DocsLibrary::titleOf(QStringLiteral("```\n# code\n```\n# Real\n")) ==
              QStringLiteral("Real"),
          "a heading inside fenced code is not the title");
}

void testLinks(const DocsLibrary &library)
{
    const DocsPage *start = library.page(QStringLiteral("getting-started.md"));
    check(start->links.contains(QStringLiteral("design/protocol.md")) &&
              start->links.contains(QStringLiteral("governance/README.md")),
          "outgoing links resolve to pages, folders to their README");
    const DocsPage *protocol = library.page(QStringLiteral("design/protocol.md"));
    check(!protocol->links.contains(QStringLiteral("nowhere.md")) && protocol->links.size() == 2,
          "links inside fenced code are ignored");
    check(library.page(QStringLiteral("getting-started.md"))->backlinks ==
              QStringList({QStringLiteral("README.md"), QStringLiteral("design/protocol.md")}),
          "backlinks list the linking pages in library order");

    DocsTarget target = library.resolve(QStringLiteral("getting-started.md"),
                                        QStringLiteral("design/protocol.md#wire-format"));
    check(target.kind == DocsTarget::Kind::Page && target.path == QStringLiteral("design/protocol.md") &&
              target.fragment == QStringLiteral("wire-format"),
          "a relative document link keeps its heading");
    target = library.resolve(QStringLiteral("getting-started.md"), QStringLiteral("#sign-in"));
    check(target.kind == DocsTarget::Kind::Anchor && target.fragment == QStringLiteral("sign-in"),
          "a same-page anchor scrolls");
    target = library.resolve(QStringLiteral("getting-started.md"),
                             QStringLiteral("../../desktop/src/main.cpp"));
    check(target.kind == DocsTarget::Kind::External &&
              target.url.toString() ==
                  QStringLiteral("https://app.forkmesh.com/forkmesh/forkmesh/tree/main/desktop/src/main.cpp"),
          "a repository file opens in the hosted source tree");
    target = library.resolve(QStringLiteral("getting-started.md"), QStringLiteral("../public/pricing.html"));
    check(target.url.toString() == QStringLiteral("https://forkmesh.com/pricing.html"),
          "a public site file opens on forkmesh.com");
    target = library.resolve(QStringLiteral("getting-started.md"), QStringLiteral("/docs/protocol/"));
    check(target.url.toString() == QStringLiteral("https://forkmesh.com/docs/protocol/"),
          "a site path opens on forkmesh.com");
    target = library.resolve(QStringLiteral("getting-started.md"), QStringLiteral("https://example.com"));
    check(target.kind == DocsTarget::Kind::External, "web links open in the browser");
    target = library.resolve(QStringLiteral("getting-started.md"), QStringLiteral("javascript:alert(1)"));
    check(target.kind == DocsTarget::Kind::None, "other schemes do nothing");
}

void testWebUrls()
{
    check(DocsLibrary::webUrl(QStringLiteral("README.md")) == QStringLiteral("https://forkmesh.com/docs/library/"),
          "the README is the library page");
    check(DocsLibrary::webUrl(QStringLiteral("design/protocol.md")) ==
              QStringLiteral("https://forkmesh.com/docs/design/protocol/"),
          "a document maps to its folder URL");
    check(DocsLibrary::webUrl(QStringLiteral("plugins/README.md")) ==
              QStringLiteral("https://forkmesh.com/docs/plugins/"),
          "a folder README maps to the folder");
    check(DocsLibrary::webUrl(QStringLiteral("governance/README.md")) ==
              QStringLiteral("https://forkmesh.com/docs/governance/reference/"),
          "a folder README whose slug is a hand-written page moves under reference/");
    check(DocsLibrary::webUrl(QStringLiteral("business-model.md")) ==
              QStringLiteral("https://forkmesh.com/docs/business-model/reference/"),
          "a slug taken by a hand-written page moves under reference/");
    check(DocsLibrary::webUrl(QStringLiteral("terminology.json")) ==
              QStringLiteral("https://forkmesh.com/docs/terminology.json"),
          "data files are served verbatim");
}

void testSearch(const DocsLibrary &library)
{
    QList<DocsHit> hits = library.search(QStringLiteral("protocol"), 10);
    check(!hits.isEmpty() && hits.first().path == QStringLiteral("design/protocol.md"),
          "a title match ranks first");
    hits = library.search(QStringLiteral("signed objects"), 10);
    check(hits.size() == 1 && hits.first().snippet.contains(QLatin1String("signed objects")),
          "body text is searched and quoted");
    hits = library.search(QStringLiteral("wire nosuchword"), 10);
    check(hits.isEmpty(), "every word must match");
    check(library.search(QStringLiteral("  "), 10).isEmpty(), "a blank query finds nothing");
    check(library.search(QStringLiteral("a"), 3).size() == 3, "the limit is honoured");
}

void testPaging(const DocsLibrary &library)
{
    check(library.previousPath(QStringLiteral("README.md")).isEmpty(), "the first page has no previous");
    check(library.nextPath(QStringLiteral("README.md")) == QStringLiteral("getting-started.md"),
          "next follows library order");
    check(library.nextPath(library.pages().last().path).isEmpty(), "the last page has no next");
    check(DocsLibrary::readingMinutes(0) == 0 && DocsLibrary::readingMinutes(10) == 1 &&
              DocsLibrary::readingMinutes(660) == 3,
          "reading time matches the web library");
}

const char *kDependencies = R"json({
  "schema": "forkmesh-docs-dependencies-v1",
  "generated": "2026-10-14",
  "dependencies": [
    {"ecosystem": "cpp", "name": "Qt6", "title": "Qt 6", "version": "6.4+",
     "usedBy": ["desktop/CMakeLists.txt"], "repository": "https://github.com/qt/qtbase",
     "description": "Application *framework*.",
     "authors": [
       {"login": "laknoll", "name": "Lars Knoll", "profile": "https://github.com/laknoll",
        "projects": "https://github.com/laknoll?tab=repositories", "publicRepos": 12,
        "website": "javascript:alert(1)", "contributions": 4321},
       {"login": "second", "name": "", "contributions": 1},
       {"login": "third", "name": "Third Person", "website": "https://third.example"},
       {"login": "fourth", "name": "Fourth Person"}
     ]},
    {"ecosystem": "python", "name": "typing_extensions", "version": "4.16.0",
     "usedBy": ["app/host/requirements.txt", "/etc/passwd", "../outside"],
     "repository": "https://evil.example/python/typing_extensions",
     "authors": [{"login": "bad login!", "name": "Nobody"}],
     "authorsNote": "No GitHub repository is known for this dependency."},
    {"ecosystem": "python", "name": "", "version": "1"},
    {"ecosystem": "maven", "name": "com.android.application", "title": "Android Gradle Plugin",
     "version": "9.4.0", "development": true, "repository": ""}
  ]
})json";

void testDependencies(const QString &root)
{
    QTemporaryDir data;
    const QString file = data.filePath(QStringLiteral("dependencies.json"));
    write(file, kDependencies);
    DocsLibrary library;
    library.load(root);
    const qsizetype documents = library.pages().size();
    check(library.loadDependencies(file), "the dependency snapshot loads");
    check(library.loadDependencies(file) && library.pages().size() == documents + 1,
          "loading the snapshot again replaces the dependencies page");
    check(library.dependencies().size() == 3, "entries without a name are dropped");
    const DocsDependency &qt = library.dependencies().value(0);
    check(qt.repository == QStringLiteral("https://github.com/qt/qtbase"), "a GitHub repository is kept");
    check(qt.authors.size() == 3, "only the top three authors are kept");
    check(qt.authors.value(0).website.isEmpty(), "a non-web author website is dropped");
    check(qt.authors.value(1).name == QStringLiteral("second") &&
              qt.authors.value(1).profile == QStringLiteral("https://github.com/second") &&
              qt.authors.value(1).projects == QStringLiteral("https://github.com/second?tab=repositories"),
          "an author without a name or links falls back to the GitHub login");
    const DocsDependency &typing = library.dependencies().value(1);
    check(typing.repository.isEmpty(), "a repository outside GitHub is dropped");
    check(typing.authors.isEmpty(), "an author with an invalid login is dropped");
    check(typing.usedBy == QStringList{QStringLiteral("app/host/requirements.txt")},
          "used-by paths stay inside the repository");

    const DocsPage *page = library.page(DocsLibrary::dependenciesPath());
    check(page && page->area == QStringLiteral("dependencies") && page->title == QStringLiteral("Dependencies"),
          "the snapshot becomes the Dependencies page");
    check(library.pages().last().path == DocsLibrary::dependenciesPath() &&
              library.pagesIn(QStringLiteral("dependencies")).size() == 1,
          "the Dependencies page closes the library");
    check(DocsLibrary::navigationAreas().last().key == QStringLiteral("dependencies") &&
              DocsLibrary::area(QStringLiteral("dependencies")) &&
              DocsLibrary::areas().size() + 1 == DocsLibrary::navigationAreas().size(),
          "the Dependencies area follows the web library's areas");
    check(DocsLibrary::webUrl(DocsLibrary::dependenciesPath()).isEmpty(),
          "the Dependencies page has no web copy");
    check(DocsLibrary::repositoryPath(DocsLibrary::dependenciesPath()) ==
                  QStringLiteral("marketplace/packages/docs/data/dependencies.json") &&
              DocsLibrary::repositoryPath(QStringLiteral("README.md")) == QStringLiteral("www/docs/README.md"),
          "repository paths name the snapshot and the library");
    const QString markdown = page ? page->markdown : QString();
    check(markdown.contains(QStringLiteral("## Desktop (C++)")) && markdown.contains(QStringLiteral("## Python")) &&
              markdown.contains(QStringLiteral("## Android (Gradle)")),
          "dependencies are grouped by ecosystem");
    check(markdown.contains(QStringLiteral("[Desktop (C++)](#desktop-c)")),
          "the summary table links each ecosystem section");
    check(markdown.contains(QStringLiteral("[qt/qtbase](https://github.com/qt/qtbase)")),
          "each dependency links its GitHub repository");
    check(markdown.contains(QStringLiteral("1. **[Lars Knoll](https://github.com/laknoll)** (@laknoll) · "
                                           "4,321 commits · [12 projects](https://github.com/laknoll?tab=repositories)")),
          "top authors show their name, commits and a link to their projects");
    check(markdown.contains(QStringLiteral("[website](https://third.example)")), "an author website is linked");
    check(!markdown.contains(QStringLiteral("Fourth Person")), "a fourth author is not shown");
    check(markdown.contains(QStringLiteral("Application \\*framework\\*.")) &&
              markdown.contains(QStringLiteral("### typing_extensions")),
          "markdown in snapshot text is escaped, inner underscores are not");
    check(markdown.contains(QStringLiteral("*No GitHub repository is known for this dependency.*")) &&
              markdown.contains(QStringLiteral("- GitHub: none known")),
          "a dependency without GitHub says so");
    check(markdown.contains(QStringLiteral("Version: 9.4.0 · Development only")),
          "development-only dependencies are marked");
    check(markdown.contains(QStringLiteral(
              "[desktop/CMakeLists.txt](https://app.forkmesh.com/forkmesh/forkmesh/tree/main/desktop/CMakeLists.txt)")),
          "used-by manifests link the source tree");
    const QList<DocsHit> hits = library.search(QStringLiteral("lars knoll"), 5);
    check(!hits.isEmpty() && hits.first().path == DocsLibrary::dependenciesPath() &&
              hits.first().snippet.contains(QStringLiteral("Lars Knoll")),
          "search finds dependency authors");
    check(library.resolve(DocsLibrary::dependenciesPath(), QStringLiteral("https://github.com/qt/qtbase")).kind ==
              DocsTarget::Kind::External,
          "repository links open in the browser");

    write(file, R"({"schema": "something-else", "dependencies": [{"ecosystem": "go", "name": "x"}]})");
    check(!library.loadDependencies(file) && !library.page(DocsLibrary::dependenciesPath()) &&
              library.dependencies().isEmpty() && library.pages().size() == documents,
          "a snapshot with another schema removes the page");
    library.load(root);
    check(library.dependencies().isEmpty(), "reloading the library clears the dependencies");
}

void testRepositoryDependencies()
{
    const QString file = QFileInfo(QStringLiteral(__FILE__))
                             .dir()
                             .absoluteFilePath(QStringLiteral("../../../data/dependencies.json"));
    if (!QFileInfo(file).isFile()) {
        std::printf("FAIL: dependency snapshot missing at %s\n", qPrintable(file));
        ++failures;
        return;
    }
    DocsLibrary library;
    check(library.loadDependencies(QDir::cleanPath(file)), "the repository dependency snapshot loads");
    bool linked = true;
    for (const DocsDependency &dependency : library.dependencies())
        linked = linked && (dependency.repository.isEmpty() || dependency.repository.startsWith(QStringLiteral("https://github.com/")));
    check(library.dependencies().size() > 20 && linked, "the snapshot lists the repository's dependencies");
}

void testRelativeRoot(const QString &root)
{
    const QString previous = QDir::currentPath();
    QDir::setCurrent(QFileInfo(root).absolutePath());
    DocsLibrary library;
    library.load(QFileInfo(root).fileName());
    QDir::setCurrent(previous);
    check(library.page(QStringLiteral("README.md")) && library.homePath() == QStringLiteral("README.md"),
          "a relative root yields paths relative to the library");
}

void testRepositoryLibrary()
{
    const QString root =
        QFileInfo(QStringLiteral(__FILE__)).dir().absoluteFilePath(QStringLiteral("../../../../../../www/docs"));
    if (!QFileInfo(root).isDir()) {
        std::printf("SKIP: repository docs not found at %s\n", qPrintable(root));
        return;
    }
    DocsLibrary library;
    check(library.load(QDir::cleanPath(root)), "the repository library loads");
    check(library.pages().size() > 100, "the repository library has every document");
    check(library.homePath() == QStringLiteral("README.md"), "the repository README is home");
    bool titled = true;
    for (const DocsPage &page : library.pages())
        titled = titled && !page.title.isEmpty();
    check(titled, "every repository document has a title");
    for (const auto &area : DocsLibrary::areas()) {
        if (library.pagesIn(area.key).isEmpty()) {
            std::printf("FAIL: area %s has no documents\n", qPrintable(area.key));
            ++failures;
        }
    }
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir root;
    fixture(root.path());
    DocsLibrary library;
    check(library.load(root.path()), "the fixture library loads");
    check(!DocsLibrary().load(root.filePath(QStringLiteral("missing"))), "a missing folder loads nothing");
    testCatalog(library);
    testTitles(library);
    testLinks(library);
    testWebUrls();
    testSearch(library);
    testPaging(library);
    testRelativeRoot(root.path());
    testRepositoryLibrary();
    testDependencies(root.path());
    testRepositoryDependencies();
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
