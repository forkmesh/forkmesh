#include "DocsLibrary.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdio>

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
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
