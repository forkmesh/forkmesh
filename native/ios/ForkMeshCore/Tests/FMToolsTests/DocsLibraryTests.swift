import Foundation
import XCTest
@testable import FMTools

final class DocsLibraryTests: XCTestCase {
    static let sample: [String: Any] = [
        "schema": "forkmesh-docs-library-v1",
        "webOrigin": "https://forkmesh.com",
        "sourceTree": "https://app.forkmesh.com/forkmesh/forkmesh/tree/main/",
        "areas": [
            ["key": "guides", "label": "Guides", "blurb": "Start here."],
            ["key": "operations", "label": "Operations", "blurb": "Runbooks."],
            ["key": "dependencies", "label": "Dependencies", "blurb": "Open source."],
        ],
        "pages": [
            [
                "path": "README.md", "title": "ForkMesh documentation", "description": "The library home.", "area": "guides",
                "url": "https://forkmesh.com/docs/library/", "asset": false,
                "markdown": "# ForkMesh documentation\n\nRead the [mirror guide](operations/mirror-node.md#setup).\n",
            ],
            [
                "path": "getting-started.md", "title": "Getting started", "description": "Install and sign in.", "area": "guides",
                "url": "https://forkmesh.com/docs/getting-started/", "asset": false,
                "markdown": "# Getting started\n\nInstall the desktop app, then open a mirror.\n\n- **Mirror** your first repository with the mirror button.\n",
            ],
            [
                "path": "operations/mirror-node.md", "title": "Mirror node", "description": "Run a mirror node.", "area": "operations",
                "url": "https://forkmesh.com/docs/operations/mirror-node/", "asset": false,
                "markdown": "# Mirror node\n\n## Setup\n\nStart it.\n",
            ],
            [
                "path": "operations/README.md", "title": "Operations", "description": "Runbooks index.", "area": "operations",
                "url": "javascript:alert(1)", "asset": false, "markdown": "# Operations\n",
            ],
            ["path": "../escape.md", "title": "Bad", "area": "guides", "markdown": "x"],
            ["path": "README.md", "title": "Duplicate", "area": "guides", "markdown": "y"],
        ],
        "dependenciesGenerated": "2026-10-14",
        "dependencies": [
            [
                "ecosystem": "python", "name": "orjson", "title": "orjson", "version": "3.12.0", "development": false,
                "usedBy": ["app/host/requirements.txt", "/etc/passwd", "../up.txt"], "description": "Fast JSON.",
                "repository": "https://github.com/ijl/orjson",
                "authors": [
                    ["login": "ijl", "name": "", "profile": "https://evil.example/ijl", "projects": "", "website": "javascript:x", "publicRepos": 5, "contributions": 900],
                    ["login": "bad login", "name": "Nope"],
                    ["login": "b", "name": "Bea", "profile": "https://github.com/b", "projects": "https://github.com/b?tab=repositories", "website": "https://bea.dev", "publicRepos": 2, "contributions": 10],
                    ["login": "c", "name": "Cee", "contributions": 5],
                    ["login": "d", "name": "Dee", "contributions": 1],
                ],
                "authorsNote": "", "fetched": "2026-10-14",
            ],
            [
                "ecosystem": "cpp", "name": "libnghttp3", "title": "nghttp3", "version": "", "development": true,
                "usedBy": ["desktop/CMakeLists.txt"], "description": "HTTP/3 library.", "repository": "https://gitlab.com/x/y",
                "authors": [], "authorsNote": "No GitHub repository.", "fetched": "",
            ],
            [
                "ecosystem": "python", "name": "pytest", "title": "pytest", "version": "8.0", "development": true,
                "usedBy": [], "description": "Testing.", "repository": "https://github.com/pytest-dev/pytest",
                "authors": [["login": "nicoddemus", "name": "Bruno Oliveira", "contributions": 3000]],
            ],
            ["ecosystem": "", "name": "nothing"],
        ],
    ]

    func library(_ object: [String: Any] = DocsLibraryTests.sample) throws -> DocsLibrary {
        try DocsLibrary(data: JSONSerialization.data(withJSONObject: object))
    }

    func testDecodesPagesInOrderAndDropsBadOnes() throws {
        let docs = try library()
        XCTAssertEqual(docs.pages.map(\.path), ["README.md", "getting-started.md", "operations/mirror-node.md", "operations/README.md"])
        XCTAssertEqual(docs.areas.map(\.key), ["guides", "operations", "dependencies"])
        XCTAssertEqual(docs.documentAreas.map(\.key), ["guides", "operations"])
        XCTAssertEqual(docs.pages(in: "operations").map(\.title), ["Mirror node", "Operations"])
        XCTAssertEqual(docs.page(path: "README.md")?.title, "ForkMesh documentation")
        XCTAssertEqual(docs.page(path: "operations/README.md")?.url, "")
        XCTAssertEqual(docs.previous("getting-started.md")?.path, "README.md")
        XCTAssertNil(docs.previous("README.md"))
        XCTAssertEqual(docs.next("operations/mirror-node.md")?.path, "operations/README.md")
        XCTAssertNil(docs.next("operations/README.md"))
        XCTAssertEqual(docs.homePath, "README.md")
        XCTAssertEqual(docs.dependenciesGenerated, "2026-10-14")
    }

    func testRejectsAWrongSchemaAndAnEmptyLibrary() throws {
        var wrong = Self.sample
        wrong["schema"] = "something-else"
        XCTAssertThrowsError(try library(wrong)) { error in
            XCTAssertEqual(error as? DocsLibraryError, .wrongSchema("something-else"))
        }
        var empty = Self.sample
        empty["pages"] = []
        XCTAssertThrowsError(try library(empty)) { error in
            XCTAssertEqual(error as? DocsLibraryError, .empty)
        }
        XCTAssertThrowsError(try DocsLibrary(data: Data("not json".utf8)))
    }

    func testSanitisesDependencies() throws {
        let docs = try library()
        XCTAssertEqual(docs.dependencies.map(\.name), ["orjson", "libnghttp3", "pytest"])
        let orjson = docs.dependencies[0]
        XCTAssertEqual(orjson.usedBy, ["app/host/requirements.txt"])
        XCTAssertEqual(orjson.repositoryName, "ijl/orjson")
        XCTAssertEqual(orjson.authors.map(\.login), ["ijl", "b", "c"])
        XCTAssertEqual(orjson.authors[0].name, "ijl")
        XCTAssertEqual(orjson.authors[0].profile, "https://github.com/ijl")
        XCTAssertEqual(orjson.authors[0].projects, "https://github.com/ijl?tab=repositories")
        XCTAssertEqual(orjson.authors[0].website, "")
        XCTAssertEqual(orjson.authors[1].website, "https://bea.dev")
        XCTAssertEqual(orjson.authors[0].contributions, 900)
        let nghttp3 = docs.dependencies[1]
        XCTAssertEqual(nghttp3.repository, "")
        XCTAssertTrue(nghttp3.development)
        XCTAssertEqual(nghttp3.authorsNote, "No GitHub repository.")
        XCTAssertEqual(docs.dependencyGroups.map(\.label), ["Python", "Desktop (C++)"])
        XCTAssertEqual(docs.dependencyGroups[0].dependencies.map(\.name), ["orjson", "pytest"])
        XCTAssertEqual(DocsLibrary.ecosystemLabel("maven"), "Android (Gradle)")
        XCTAssertEqual(DocsLibrary.ecosystemLabel("swift"), "Swift packages")
        XCTAssertEqual(DocsLibrary.ecosystemLabel("npm"), "JavaScript (npm)")
        XCTAssertEqual(DocsLibrary.ecosystemLabel("vendored"), "Bundled web libraries")
        XCTAssertEqual(DocsLibrary.ecosystemLabel("zig"), "zig")
    }

    func testLabels() throws {
        let docs = try library()
        let orjson = docs.dependencies[0]
        XCTAssertEqual(orjson.summary, "3.12.0 \u{00B7} 3 top authors")
        XCTAssertEqual(orjson.authors[0].displayName, "ijl")
        XCTAssertEqual(orjson.authors[1].displayName, "Bea (@b)")
        XCTAssertEqual(orjson.authors[0].commitsLabel, "900 commits")
        XCTAssertEqual(orjson.authors[0].projectsLabel, "5 projects")
        XCTAssertEqual(orjson.authors[2].projectsLabel, "Projects")
        let nghttp3 = docs.dependencies[1]
        XCTAssertEqual(nghttp3.versionLabel, "unpinned")
        XCTAssertEqual(nghttp3.summary, "unpinned \u{00B7} development \u{00B7} no GitHub repository")
        XCTAssertEqual(nghttp3.authorsLabel, "No GitHub repository.")
        XCTAssertEqual(docs.dependencies[2].authors[0].commitsLabel, "3,000 commits")
        XCTAssertEqual(docs.dependencies[2].authorsLabel, "1 top author")
        XCTAssertEqual(docs.onGitHub, 2)
        XCTAssertEqual(DocsLibrary.grouped(1234567), "1,234,567")
        XCTAssertEqual(DocsLibrary.grouped(12), "12")
    }

    func testTruncatesLongDescriptions() {
        let long = String(repeating: "word ", count: 60)
        let cut = DocsLibrary.truncated(long, limit: 180)
        XCTAssertLessThanOrEqual(cut.count, 180)
        XCTAssertTrue(cut.hasSuffix("\u{2026}"))
        XCTAssertEqual(DocsLibrary.truncated("Short:", limit: 180), "Short")
    }

    func testSearchRanksTitlesFirstAndRequiresEveryToken() throws {
        let docs = try library()
        let hits = docs.search("mirror")
        XCTAssertEqual(hits.first?.path, "operations/mirror-node.md")
        XCTAssertEqual(Set(hits.map(\.path)), ["operations/mirror-node.md", "getting-started.md", "README.md"])
        XCTAssertEqual(hits.first?.score, 200 + 150 + 1)
        XCTAssertTrue(docs.search("mirror nowhere-to-be-found").isEmpty)
        XCTAssertTrue(docs.search("   ").isEmpty)
        XCTAssertEqual(docs.search("mirror", limit: 1).count, 1)
        XCTAssertEqual(docs.search("runbooks").first?.path, "operations/README.md")
    }

    func testSearchSnippetIsTheMatchingLineWithoutMarkup() throws {
        let docs = try library()
        let hit = try XCTUnwrap(docs.search("first repository").first)
        XCTAssertEqual(hit.path, "getting-started.md")
        XCTAssertEqual(hit.snippet, "Mirror your first repository with the mirror button.")
        let guide = try XCTUnwrap(docs.search("guide").first { $0.path == "README.md" })
        XCTAssertEqual(guide.snippet, "Read the mirror guide.")
        let long = String(repeating: "alpha ", count: 40) + "needle " + String(repeating: "omega ", count: 40)
        let snippet = DocsLibrary.snippet("1. " + long, needle: "needle")
        XCTAssertTrue(snippet.hasPrefix("\u{2026}"))
        XCTAssertTrue(snippet.hasSuffix("\u{2026}"))
        XCTAssertTrue(snippet.contains("needle"))
        XCTAssertEqual(DocsLibrary.snippet("## Heading `code` here", needle: "code"), "Heading code here")
    }

    func testSearchDependenciesMatchesNamesAndAuthors() throws {
        let docs = try library()
        XCTAssertEqual(docs.searchDependencies("bruno").map(\.name), ["pytest"])
        XCTAssertEqual(docs.searchDependencies("nghttp3").map(\.name), ["libnghttp3"])
        XCTAssertEqual(docs.searchDependencies("bea").map(\.name), ["orjson"])
        XCTAssertEqual(docs.searchDependencies("python testing").map(\.name), ["pytest"])
        XCTAssertEqual(docs.searchDependencies("").count, 3)
    }

    func testResolvesEveryKindOfLink() throws {
        let docs = try library()
        let from = "operations/mirror-node.md"
        XCTAssertEqual(docs.resolve(from: from, href: "#set%20up"), .anchor("set up"))
        XCTAssertEqual(docs.resolve(from: "README.md", href: "operations/mirror-node.md#setup"), .page(path: "operations/mirror-node.md", fragment: "setup"))
        XCTAssertEqual(docs.resolve(from: from, href: "../getting-started.md"), .page(path: "getting-started.md", fragment: ""))
        XCTAssertEqual(docs.resolve(from: from, href: "./README.md"), .page(path: "operations/README.md", fragment: ""))
        XCTAssertEqual(docs.resolve(from: "README.md", href: "operations/"), .page(path: "operations/README.md", fragment: ""))
        XCTAssertEqual(docs.resolve(from: "README.md", href: "operations"), .page(path: "operations/README.md", fragment: ""))
        XCTAssertEqual(docs.resolve(from: from, href: ".."), .page(path: "README.md", fragment: ""))
        XCTAssertEqual(docs.resolve(from: from, href: "mirror%2Dnode.md?raw=1"), .page(path: "operations/mirror-node.md", fragment: ""))
        XCTAssertEqual(docs.resolve(from: from, href: "https://example.com/a"), .external(URL(string: "https://example.com/a")!))
        XCTAssertEqual(docs.resolve(from: from, href: "mailto:hi@forkmesh.com"), .external(URL(string: "mailto:hi@forkmesh.com")!))
        XCTAssertEqual(docs.resolve(from: from, href: "//cdn.example.com/x.js"), .external(URL(string: "https://cdn.example.com/x.js")!))
        XCTAssertEqual(docs.resolve(from: from, href: "/pricing/"), .external(URL(string: "https://forkmesh.com/pricing/")!))
        XCTAssertEqual(
            docs.resolve(from: from, href: "../../public/install.sh"), .external(URL(string: "https://forkmesh.com/install.sh")!))
        XCTAssertEqual(
            docs.resolve(from: from, href: "runbook.md#step-2"),
            .external(URL(string: "https://app.forkmesh.com/forkmesh/forkmesh/tree/main/www/docs/operations/runbook.md#step-2")!))
        XCTAssertEqual(
            docs.resolve(from: from, href: "../../../app/host/main.py"),
            .external(URL(string: "https://app.forkmesh.com/forkmesh/forkmesh/tree/main/app/host/main.py")!))
        XCTAssertEqual(
            docs.resolve(from: from, href: "../../../../../../a b.md"),
            .external(URL(string: "https://app.forkmesh.com/forkmesh/forkmesh/tree/main/a%20b.md")!))
        XCTAssertNil(docs.resolve(from: from, href: "javascript:alert(1)"))
        XCTAssertNil(docs.resolve(from: from, href: "  "))
    }

    func testLinkURLsRoundTrip() throws {
        let docs = try library()
        let page = try XCTUnwrap(docs.linkURL(from: "README.md", href: "operations/mirror-node.md#setup"))
        XCTAssertEqual(page.scheme, DocsLibrary.linkScheme)
        XCTAssertEqual(DocsLibrary.target(for: page), .page(path: "operations/mirror-node.md", fragment: "setup"))
        let anchor = try XCTUnwrap(docs.linkURL(from: "README.md", href: "#top"))
        XCTAssertEqual(DocsLibrary.target(for: anchor), .anchor("top"))
        let web = try XCTUnwrap(docs.linkURL(from: "README.md", href: "https://github.com/ijl/orjson"))
        XCTAssertEqual(DocsLibrary.target(for: web), .external(web))
        XCTAssertNil(DocsLibrary.target(for: URL(string: "ftp://x/y")!))
    }

    func testHeadingAnchors() {
        XCTAssertEqual(DocsMarkdown.anchor("Getting Started"), "getting-started")
        XCTAssertEqual(DocsMarkdown.anchor("  What's new in 2.0?  "), "whats-new-in-20")
        XCTAssertEqual(DocsMarkdown.anchor("snake_case & dashes-ok"), "snake_case--dashes-ok")
        XCTAssertEqual(DocsMarkdown.anchor("Ünïcode Títle"), "ünïcode-títle")
        let blocks = DocsMarkdown.blocks("# Setup\n\n## Setup\n\n### Setup\n\n## The `code` *way*\n")
        XCTAssertEqual(blocks, [
            .heading(level: 1, text: "Setup", anchor: "setup"),
            .heading(level: 2, text: "Setup", anchor: "setup-1"),
            .heading(level: 3, text: "Setup", anchor: "setup-2"),
            .heading(level: 2, text: "The `code` *way*", anchor: "the-code-way"),
        ])
        XCTAssertEqual(DocsMarkdown.anchorMatching("Setup-1", in: blocks), "setup-1")
        XCTAssertEqual(DocsMarkdown.anchorMatching("The Code Way", in: blocks), "the-code-way")
        XCTAssertNil(DocsMarkdown.anchorMatching("missing", in: blocks))
    }

    func testFencedCodeIsNotParsedAsMarkdown() {
        let blocks = DocsMarkdown.blocks("Intro\n\n```sh\n# not a heading\n- not a list\n\n| a | b |\n```\n\n~~~\nplain\n~~~\nAfter")
        XCTAssertEqual(blocks, [
            .paragraph("Intro"),
            .code(language: "sh", text: "# not a heading\n- not a list\n\n| a | b |"),
            .code(language: "", text: "plain"),
            .paragraph("After"),
        ])
    }

    func testTablesListsQuotesRulesAndComments() {
        let markdown = """
            Title
            =====

            <!-- hidden
            still hidden -->
            <p align="center"><img src="x.png"></p>

            | Area | Count |
            | --- | ---: |
            | `a|b` | 1 |
            | Two |

            - one
              continued
            - two
              1. nested
                 - deeper
            - three

            > quoted
            > more
            >
            > second

            ***
            Para line one
            line two
            Sub
            ---
            """
        XCTAssertEqual(DocsMarkdown.blocks(markdown), [
            .heading(level: 1, text: "Title", anchor: "title"),
            .table(header: ["Area", "Count"], rows: [["`a|b`", "1"], ["Two", ""]]),
            .listItem(ordered: false, number: 0, depth: 0, text: "one continued"),
            .listItem(ordered: false, number: 0, depth: 0, text: "two"),
            .listItem(ordered: true, number: 1, depth: 1, text: "nested"),
            .listItem(ordered: false, number: 0, depth: 2, text: "deeper"),
            .listItem(ordered: false, number: 0, depth: 0, text: "three"),
            .quote("quoted more\nsecond"),
            .rule,
            .heading(level: 2, text: "Para line one line two Sub", anchor: "para-line-one-line-two-sub"),
        ])
    }

    func testIndentedCodeAndTaskItems() {
        let blocks = DocsMarkdown.blocks("Text\n\n    let x = 1\n    let y = 2\n\n- [ ] todo\n- [x] done\n")
        XCTAssertEqual(blocks, [
            .paragraph("Text"),
            .code(language: "", text: "let x = 1\nlet y = 2"),
            .listItem(ordered: false, number: 0, depth: 0, text: "\u{2610} todo"),
            .listItem(ordered: false, number: 0, depth: 0, text: "\u{2611} done"),
        ])
    }

    func testInlineRuns() {
        XCTAssertEqual(DocsInline.runs("a **bold** and *it* `co*de` ~~gone~~"), [
            DocsRun(text: "a "), DocsRun(text: "bold", bold: true), DocsRun(text: " and "), DocsRun(text: "it", italic: true),
            DocsRun(text: " "), DocsRun(text: "co*de", code: true), DocsRun(text: " "), DocsRun(text: "gone", strike: true),
        ])
        XCTAssertEqual(DocsInline.runs("see [the **guide**](guide.md \"t\") now"), [
            DocsRun(text: "see "), DocsRun(text: "the ", link: "guide.md"), DocsRun(text: "guide", bold: true, link: "guide.md"),
            DocsRun(text: " now"),
        ])
        XCTAssertEqual(DocsInline.runs("snake_case_name and 2 * 3"), [DocsRun(text: "snake_case_name and 2 * 3")])
        XCTAssertEqual(DocsInline.runs("<https://x.dev/a> &amp; https://y.dev/b."), [
            DocsRun(text: "https://x.dev/a", link: "https://x.dev/a"), DocsRun(text: " & "),
            DocsRun(text: "https://y.dev/b", link: "https://y.dev/b"), DocsRun(text: "."),
        ])
        XCTAssertEqual(DocsInline.runs("![logo](x.png) <b>Hi</b><br>there \\*not\\*"), [DocsRun(text: "logo Hi\nthere *not*")])
        XCTAssertEqual(DocsInline.runs("[ref][1] and [plain]"), [DocsRun(text: "ref and [plain]")])
        XCTAssertEqual(DocsMarkdown.plain("**A**  [b](c)\n`d`"), "A b d")
    }

    func testTheBundledLibraryLoads() throws {
        var url = URL(fileURLWithPath: #filePath)
        for _ in 0..<6 { url.deleteLastPathComponent() }
        url.appendPathComponent("native/ios/ForkMesh/Resources/DocsLibrary.json")
        let docs = try DocsLibrary(data: Data(contentsOf: url))
        XCTAssertGreaterThan(docs.pages.count, 100)
        XCTAssertEqual(docs.pages.first?.path, "README.md")
        XCTAssertEqual(docs.areas.last?.key, DocsLibrary.dependenciesArea)
        XCTAssertGreaterThan(docs.dependencies.count, 20)
        XCTAssertTrue(docs.dependencies.allSatisfy { $0.authors.count <= 3 })
        XCTAssertFalse(docs.search("mirror").isEmpty)
        for page in docs.pages {
            XCTAssertFalse(DocsMarkdown.blocks(page.markdown).isEmpty, page.path)
        }
    }
}
