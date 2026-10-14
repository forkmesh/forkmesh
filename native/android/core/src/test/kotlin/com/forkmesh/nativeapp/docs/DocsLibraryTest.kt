package com.forkmesh.nativeapp.docs

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File

class DocsLibraryTest {
    private fun page(path: String, title: String, area: String, markdown: String, description: String = "") = mapOf(
        "path" to path, "title" to title, "description" to description, "area" to area,
        "url" to "https://forkmesh.com/docs/${path.removeSuffix(".md")}/", "asset" to false, "markdown" to markdown,
    )

    private fun fixture(): String = Json.stringify(
        mapOf(
            "schema" to "forkmesh-docs-library-v1",
            "webOrigin" to "https://forkmesh.com",
            "sourceTree" to "https://app.forkmesh.com/forkmesh/forkmesh/tree/main/",
            "areas" to listOf(
                mapOf("key" to "guides", "label" to "Guides", "blurb" to "Start here."),
                mapOf("key" to "operations", "label" to "Operations", "blurb" to "Runbooks."),
                mapOf("key" to "dependencies", "label" to "Dependencies", "blurb" to "Open source."),
            ),
            "pages" to listOf(
                page("README.md", "ForkMesh documentation", "guides", "# ForkMesh documentation\n\nWelcome to the library.\n", "Welcome."),
                page(
                    "getting-started.md", "Getting started", "guides",
                    "# Getting started\n\nInstall the desktop app.\n\n## Copies\n\n- **Add a mirror** with `fm mirror add`.\n",
                    "Install ForkMesh and clone a repository.",
                ),
                page("operations/README.md", "Operations", "operations", "# Operations\n\nRunbooks live here.\n"),
                page(
                    "operations/mirror-node.md", "Mirror node", "operations",
                    "# Mirror node\n\nA mirror keeps a copy.\n\n> Mirrors sync every minute.\n",
                    "Run a mirror node.",
                ),
                page("../escape.md", "Escape", "guides", "# Escape"),
            ),
            "dependenciesGenerated" to "2026-10-14",
            "dependencies" to listOf(
                mapOf(
                    "ecosystem" to "python", "name" to "requests", "title" to "Requests", "version" to "2.32", "development" to false,
                    "usedBy" to listOf("app/host/requirements.txt", "/etc/passwd", "../outside.txt"),
                    "description" to "HTTP for Humans.", "repository" to "https://github.com/psf/requests",
                    "authors" to listOf(
                        mapOf("login" to "kennethreitz", "name" to "Kenneth Reitz", "profile" to "https://github.com/kennethreitz",
                            "projects" to "https://github.com/kennethreitz?tab=repositories", "website" to "https://kennethreitz.org",
                            "publicRepos" to 30, "contributions" to 3000),
                        mapOf("login" to "bad login!", "name" to "Bad"),
                        mapOf("login" to "sigmavirus24", "name" to "", "profile" to "https://evil.example/x", "projects" to "",
                            "website" to "javascript:alert(1)", "publicRepos" to 1, "contributions" to 1),
                        mapOf("login" to "nateprewitt", "name" to "Nate Prewitt", "contributions" to 500),
                        mapOf("login" to "fourth", "name" to "Fourth"),
                    ),
                    "authorsNote" to "", "fetched" to "2026-10-14",
                ),
                mapOf(
                    "ecosystem" to "maven", "name" to "androidx.compose:compose-bom", "title" to "Compose BOM", "version" to "",
                    "development" to true, "usedBy" to listOf("native/android/app/build.gradle.kts"), "description" to "Jetpack Compose.",
                    "repository" to "https://gitlab.com/not/github", "authors" to emptyList<Any>(), "authorsNote" to "Not on GitHub.",
                ),
                mapOf("ecosystem" to "python", "name" to "pyyaml", "repository" to "", "authors" to emptyList<Any>()),
                mapOf("ecosystem" to "", "name" to "nameless"),
            ),
        ),
    )

    private fun library(): DocsLibrary = DocsLibrary.parse(fixture())!!

    fun testDecodingKeepsLibraryOrderAndDropsUnsafePages() {
        val library = library()
        assertEq(listOf("README.md", "getting-started.md", "operations/README.md", "operations/mirror-node.md"), library.pages.map { it.path })
        assertEq(listOf("guides", "operations", "dependencies"), library.areas.map { it.key })
        assertEq(listOf("guides", "operations"), library.documentAreas.map { it.key })
        assertEq("Dependencies", library.dependenciesArea?.label)
        assertEq(listOf("operations/README.md", "operations/mirror-node.md"), library.pagesIn("operations").map { it.path })
        assertEq("Mirror node", library.page("operations/mirror-node.md")?.title)
        assertEq(null, library.page("missing.md"))
        assertEq("README.md", library.homePath)
        assertEq(null, library.previous("README.md"))
        assertEq("getting-started.md", library.next("README.md")?.path)
        assertEq("operations/README.md", library.previous("operations/mirror-node.md")?.path)
        assertEq(null, library.next("operations/mirror-node.md"))
        assertEq("2026-10-14", library.dependenciesGenerated)
        assertEq("3 open-source dependencies", library.dependenciesSummary)
    }

    fun testWrongSchemaOrBrokenJsonIsRejected() {
        assertEq(null, DocsLibrary.parse("{\"schema\":\"forkmesh-docs-dependencies-v1\"}"))
        assertEq(null, DocsLibrary.parse("not json"))
        assertEq(null, DocsLibrary.parse("[]"))
        val empty = DocsLibrary.parse("{\"schema\":\"forkmesh-docs-library-v1\"}")!!
        expect(empty.pages.isEmpty() && empty.dependencies.isEmpty())
        assertEq("https://forkmesh.com", empty.webOrigin)
        assertEq(null, empty.dependenciesArea)
    }

    fun testDependenciesAreGroupedAndSanitised() {
        val library = library()
        assertEq(listOf("python", "maven"), library.ecosystems.map { it.key })
        assertEq(listOf("Python", "Android (Gradle)"), library.ecosystems.map { it.label })
        assertEq(listOf("requests", "pyyaml"), library.ecosystems[0].dependencies.map { it.name })
        val requests = library.dependency("python:requests")!!
        assertEq(listOf("app/host/requirements.txt"), requests.usedBy)
        assertEq("psf/requests", requests.repositoryLabel)
        assertEq(listOf("kennethreitz", "sigmavirus24", "nateprewitt"), requests.authors.map { it.login })
        val kenneth = requests.authors[0]
        assertEq("@kennethreitz", kenneth.handle)
        assertEq("3,000 commits", kenneth.commitsLabel)
        assertEq("30 projects", kenneth.projectsLabel)
        assertEq("https://kennethreitz.org", kenneth.website)
        val sigma = requests.authors[1]
        assertEq("sigmavirus24", sigma.name)
        assertEq("", sigma.handle)
        assertEq("https://github.com/sigmavirus24", sigma.profile)
        assertEq("https://github.com/sigmavirus24?tab=repositories", sigma.projects)
        assertEq("", sigma.website)
        assertEq("1 commit", sigma.commitsLabel)
        assertEq("1 project", sigma.projectsLabel)
        assertEq("Projects", requests.authors[2].projectsLabel)
        val bom = library.dependency("maven:androidx.compose:compose-bom")!!
        assertEq("", bom.repository)
        assertEq("unpinned", bom.versionLabel)
        expect(bom.development)
        assertEq("Not on GitHub.", bom.authorsFallback)
        val yaml = library.dependency("python:pyyaml")!!
        assertEq("pyyaml", yaml.title)
        assertEq("Top authors not recorded yet.", yaml.authorsFallback)
        assertEq("Desktop (C++)", DocsLibrary.ecosystemLabel("cpp"))
        assertEq("Swift packages", DocsLibrary.ecosystemLabel("swift"))
        assertEq("JavaScript (npm)", DocsLibrary.ecosystemLabel("npm"))
        assertEq("Bundled web libraries", DocsLibrary.ecosystemLabel("vendored"))
        assertEq("rust", DocsLibrary.ecosystemLabel("rust"))
        assertEq("", DocsLibrary.gitHubLink("https://github.com/"))
        assertEq("", DocsLibrary.webLink("ftp://example.com/x"))
        assertEq("", DocsLibrary.webLink("https:///nohost"))
    }

    fun testSearchRanksTitlesFirstAndRequiresEveryToken() {
        val library = library()
        val hits = library.search("mirror", 10)
        assertEq(listOf("operations/mirror-node.md", "getting-started.md"), hits.map { it.path })
        assertEq(200 + 150 + 3, hits[0].score)
        assertEq("Add a mirror with fm mirror add.", hits[1].snippet)
        assertEq("Run a mirror node.", hits[0].snippet)
        assertEq(emptyList<DocsHit>(), library.search("mirror zebra", 10))
        assertEq(listOf("operations/mirror-node.md"), library.search("MIRROR   sync", 10).map { it.path })
        assertEq("Mirrors sync every minute.", library.search("sync every", 10).single().snippet)
        assertEq(emptyList<DocsHit>(), library.search("   ", 10))
        assertEq(1, library.search("mirror", 1).size)
        val path = library.search("operations", 10)
        assertEq("operations/README.md", path.first().path)
        val described = library.search("clone", 10).single()
        assertEq("getting-started.md", described.path)
        assertEq("Install ForkMesh and clone a repository.", described.snippet)
    }

    fun testDependencySearchCoversNamesAndAuthors() {
        val library = library()
        assertEq(listOf("requests"), library.searchDependencies("reitz").map { it.name })
        assertEq(listOf("requests"), library.searchDependencies("Humans http").map { it.name })
        assertEq(listOf("androidx.compose:compose-bom"), library.searchDependencies("compose").map { it.name })
        assertEq(listOf("requests"), library.searchDependencies("nateprewitt").map { it.name })
        assertEq(3, library.searchDependencies("").size)
        assertEq(0, library.searchDependencies("reitz compose").size)
    }

    fun testResolveCoversEveryLinkKind() {
        val library = library()
        assertEq(DocsTarget.Anchor("set up"), library.resolve("README.md", "#set%20up"))
        assertEq(DocsTarget.Page("getting-started.md", ""), library.resolve("README.md", "getting-started.md"))
        assertEq(DocsTarget.Page("getting-started.md", "mirrors"), library.resolve("README.md", "./getting-started.md#mirrors"))
        assertEq(DocsTarget.Page("operations/README.md", ""), library.resolve("README.md", "operations/"))
        assertEq(DocsTarget.Page("operations/README.md", ""), library.resolve("README.md", "operations"))
        assertEq(DocsTarget.Page("operations/mirror-node.md", ""), library.resolve("operations/README.md", "mirror-node.md"))
        assertEq(DocsTarget.Page("README.md", ""), library.resolve("operations/mirror-node.md", "../README.md"))
        assertEq(DocsTarget.Page("README.md", ""), library.resolve("operations/mirror-node.md", "../"))
        assertEq(DocsTarget.Page("operations/mirror-node.md", "x"), library.resolve("operations/mirror-node.md", "mirror%2Dnode.md?raw=1#x"))
        assertEq(DocsTarget.External("https://example.com/a"), library.resolve("README.md", "https://example.com/a"))
        assertEq(DocsTarget.External("http://example.com"), library.resolve("README.md", " http://example.com "))
        assertEq(DocsTarget.External("mailto:hello@forkmesh.com"), library.resolve("README.md", "mailto:hello@forkmesh.com"))
        assertEq(DocsTarget.External("https://cdn.example.com/x.js"), library.resolve("README.md", "//cdn.example.com/x.js"))
        assertEq(DocsTarget.External("https://forkmesh.com/pricing"), library.resolve("operations/mirror-node.md", "/pricing"))
        assertEq(DocsTarget.External("https://forkmesh.com/install.sh"), library.resolve("operations/mirror-node.md", "../../public/install.sh"))
        assertEq(
            DocsTarget.External("https://app.forkmesh.com/forkmesh/forkmesh/tree/main/docs/native/ANDROID.md#13-core"),
            library.resolve("operations/mirror-node.md", "../../../docs/native/ANDROID.md#13-core"),
        )
        assertEq(
            DocsTarget.External("https://app.forkmesh.com/forkmesh/forkmesh/tree/main/www/docs/missing.md"),
            library.resolve("README.md", "missing.md"),
        )
        assertEq(
            DocsTarget.External("https://app.forkmesh.com/forkmesh/forkmesh/tree/main/README.md"),
            library.resolve("README.md", "../../../../README.md"),
        )
        assertEq(null, library.resolve("README.md", "javascript:alert(1)"))
        assertEq(null, library.resolve("README.md", "  "))
        assertEq("a b/é", DocsLibrary.percentDecode("a%20b/%C3%A9"))
        assertEq("100%", DocsLibrary.percentDecode("100%"))
        assertEq("www/README.md", DocsLibrary.normalise("www/docs/./../README.md"))
    }

    fun testHeadingAnchorsFollowGitHub() {
        assertEq("2-add-an-independent-mirror", DocsMarkdown.headingAnchor("2. Add an independent mirror"))
        assertEq("clone-again", DocsMarkdown.headingAnchor(DocsMarkdown.plain("Clone `again`")))
        assertEq("whats-new_v2--today", DocsMarkdown.headingAnchor("What's new_v2 — today"))
        val blocks = DocsMarkdown.blocks("# Setup\n\n## Setup\n\n### Setup\n\n## **Bold** title\n")
        assertEq(
            listOf("setup", "setup-1", "setup-2", "bold-title"),
            blocks.filterIsInstance<DocsBlock.Heading>().map { it.anchor },
        )
        val document = DocsMarkdown.parse("# Intro\n\n<a id=\"apns\"></a>\n\n## Later\n")
        assertEq(1, document.anchorIndex("apns"))
        assertEq(2, document.anchorIndex("later"))
        assertEq(2, document.anchorIndex("Later"))
        assertEq(-1, document.anchorIndex("nowhere"))
    }

    fun testBlocksParseFencesTablesListsAndQuotes() {
        val markdown = """
            # Title

            Intro line one
            continues here.

            ```bash
            # not a heading
            fm mirror add
            ```

            ~~~
            plain
            ~~~

            | Name | Value |
            | :--- | ---: |
            | a \| b | `x|y` |
            | c |

            - one
              wrapped
            - two
              - nested
                1. deep
            - [x] done
            - [ ] open

            3) third

            > **Note** quoted
            > - item in quote

            ---

            <!-- hidden
            comment -->
            <section class="x">
            Text with <b>bold</b> and <node> kept.
        """.trimIndent()
        val blocks = DocsMarkdown.blocks(markdown)
        assertEq(DocsBlock.Heading(1, "Title", "title"), blocks[0])
        assertEq(DocsBlock.Paragraph("Intro line one continues here."), blocks[1])
        assertEq(DocsBlock.Code("bash", "# not a heading\nfm mirror add"), blocks[2])
        assertEq(DocsBlock.Code("", "plain"), blocks[3])
        val table = blocks[4] as DocsBlock.Table
        assertEq(listOf("Name", "Value"), table.header)
        assertEq(listOf(listOf("a | b", "`x|y`"), listOf("c", "")), table.rows)
        assertEq(listOf(DocsAlign.Start, DocsAlign.End), table.align)
        assertEq(DocsBlock.ListItem(0, "•", "one wrapped"), blocks[5])
        assertEq(DocsBlock.ListItem(0, "•", "two"), blocks[6])
        assertEq(DocsBlock.ListItem(1, "•", "nested"), blocks[7])
        assertEq(DocsBlock.ListItem(2, "1.", "deep"), blocks[8])
        expect((blocks[8] as DocsBlock.ListItem).ordered)
        assertEq(DocsBlock.ListItem(0, "•", "done", true), blocks[9])
        assertEq(DocsBlock.ListItem(0, "•", "open", false), blocks[10])
        assertEq(DocsBlock.ListItem(0, "3.", "third"), blocks[11])
        val quote = blocks[12] as DocsBlock.Quote
        assertEq(listOf(DocsBlock.Paragraph("**Note** quoted"), DocsBlock.ListItem(0, "•", "item in quote")), quote.blocks)
        assertEq(DocsBlock.Rule, blocks[13])
        assertEq(DocsBlock.Paragraph("Text with <b>bold</b> and <node> kept."), blocks[14])
        assertEq(15, blocks.size)
        assertEq("Text with bold and <node> kept.", DocsMarkdown.plain((blocks[14] as DocsBlock.Paragraph).text))
    }

    fun testListParagraphsAndFencesInsideItems() {
        val blocks = DocsMarkdown.blocks("1. First\n\n   More about first.\n\n   ```\n   code\n   ```\n2. Second\n\nAfter.\n")
        assertEq(DocsBlock.ListItem(0, "1.", "First"), blocks[0])
        assertEq(DocsBlock.ListItem(0, "", "More about first."), blocks[1])
        expect((blocks[1] as DocsBlock.ListItem).continuation)
        assertEq(DocsBlock.Code("", "code"), blocks[2])
        assertEq(DocsBlock.ListItem(0, "2.", "Second"), blocks[3])
        assertEq(DocsBlock.Paragraph("After."), blocks[4])
        val hard = DocsMarkdown.blocks("line one  \nline two\\\nline three")
        assertEq(listOf<DocsBlock>(DocsBlock.Paragraph("line one\nline two\nline three")), hard)
    }

    fun testInlineSpansKeepRelativeLinks() {
        val spans = DocsMarkdown.inline("See [the **guide**](../guide.md#setup \"Guide\"), `code *x*` and *soft* ~~old~~ text.")
        assertEq(
            listOf(
                DocsSpan("See "),
                DocsSpan("the ", href = "../guide.md#setup"),
                DocsSpan("guide", bold = true, href = "../guide.md#setup"),
                DocsSpan(", "),
                DocsSpan("code *x*", code = true),
                DocsSpan(" and "),
                DocsSpan("soft", italic = true),
                DocsSpan(" "),
                DocsSpan("old", strike = true),
                DocsSpan(" text."),
            ),
            spans,
        )
        assertEq(
            listOf(DocsSpan("Visit "), DocsSpan("https://forkmesh.com/docs", href = "https://forkmesh.com/docs"), DocsSpan(".")),
            DocsMarkdown.inline("Visit https://forkmesh.com/docs."),
        )
        assertEq(
            listOf(DocsSpan("hello@forkmesh.com", href = "mailto:hello@forkmesh.com")),
            DocsMarkdown.inline("<mailto:hello@forkmesh.com>"),
        )
        assertEq(listOf(DocsSpan("snake_case and 2 * 3 * 4 [draft]")), DocsMarkdown.inline("snake_case and 2 * 3 * 4 [draft]"))
        assertEq(listOf(DocsSpan("a *literal* & <tag>")), DocsMarkdown.inline("a \\*literal\\* &amp; &lt;tag&gt;"))
        assertEq(listOf(DocsSpan("both", bold = true, italic = true)), DocsMarkdown.inline("***both***"))
        assertEq(listOf(DocsSpan("Logo", href = "img/logo.png")), DocsMarkdown.inline("![Logo](img/logo.png)"))
        val document = DocsMarkdown.parse("Read the [spec][s] and [Guide].\n\n[s]: https://example.com/spec\n[guide]: guide.md\n")
        assertEq(1, document.blocks.size)
        assertEq(mapOf("s" to "https://example.com/spec", "guide" to "guide.md"), document.references)
        val linked = DocsMarkdown.inline((document.blocks[0] as DocsBlock.Paragraph).text, document.references)
        assertEq(listOf("https://example.com/spec", "guide.md"), linked.mapNotNull { it.href })
    }

    fun testSnippetStripsMarkupAndClipsLongLines() {
        assertEq("Mirrors sync every minute.", DocsLibrary.snippet("# T\n\n> **Mirrors** sync every minute.\n", "sync"))
        assertEq("Second step", DocsLibrary.snippet("1. Second step\n", "second"))
        val long = "word ".repeat(60) + "needle " + "tail ".repeat(60)
        val clipped = DocsLibrary.snippet(long, "needle")
        expect(clipped.startsWith("…") && clipped.endsWith("…"), clipped)
        expect(clipped.contains("needle"))
        expect(clipped.length <= 142)
        assertEq("", DocsLibrary.snippet("nothing here", "absent"))
    }

    fun testTheBundledLibraryLoads() {
        val candidates = listOf(
            File("../../ios/ForkMesh/Resources/DocsLibrary.json"),
            File("../ios/ForkMesh/Resources/DocsLibrary.json"),
            File("native/ios/ForkMesh/Resources/DocsLibrary.json"),
        )
        val file = candidates.firstOrNull(File::isFile)
        expect(file != null, "bundled DocsLibrary.json")
        val library = DocsLibrary.parse(file!!.readText())
        expect(library != null, "bundle parses")
        library!!
        expect(library.pages.size > 100, "pages: ${library.pages.size}")
        assertEq("README.md", library.pages.first().path)
        assertEq("dependencies", library.areas.last().key)
        expect(library.dependencies.size > 20, "dependencies: ${library.dependencies.size}")
        expect(library.dependencies.all { it.authors.size <= DocsLibrary.TOP_AUTHORS })
        expect(library.dependencies.all { it.repository.isEmpty() || it.repository.startsWith(DocsLibrary.GITHUB) })
        expect(library.search("mirror", 20).isNotEmpty())
        for (page in library.pages) {
            val document = DocsMarkdown.parse(page.markdown)
            expect(document.blocks.isNotEmpty(), "blocks for ${page.path}")
            if (!page.asset) {
                val heading = document.blocks.firstOrNull { it is DocsBlock.Heading } as DocsBlock.Heading?
                expect(heading != null, "heading in ${page.path}")
            }
        }
    }
}
