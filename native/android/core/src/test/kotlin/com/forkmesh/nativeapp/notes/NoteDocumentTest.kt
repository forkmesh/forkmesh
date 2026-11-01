package com.forkmesh.nativeapp.notes

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.time.LocalDate
import java.util.Base64

class NoteDocumentTest {
    private val defaults = NoteLayout(NotePaper.A4, NoteOrientation.Portrait, NoteMargins.Normal, cover = false, contents = false, numbers = true)

    private fun figures(markdown: String): List<NoteFigure> =
        NoteDocument.segments(markdown).filterIsInstance<NoteSegment.Figure>().map { it.figure }

    fun testNoLayoutLineUsesTheDefaults() {
        assertEq(defaults, NoteDocument.layout("# Title\n\nBody"))
        assertEq(defaults, NoteDocument.layout(""))
    }

    fun testLetterWithCover() {
        assertEq(defaults.copy(paper = NotePaper.Letter, cover = true), NoteDocument.layout("<!-- page: size=letter; cover=yes -->\nBody"))
    }

    fun testKeysAndValuesAreComparedLowerCase() {
        assertEq(
            NoteLayout(NotePaper.Legal, NoteOrientation.Landscape, NoteMargins.Wide, cover = false, contents = false, numbers = false),
            NoteDocument.layout("<!--page:size = LEGAL;orientation=landscape;margins=wide;numbers=no-->"),
        )
    }

    fun testUnknownValuesAndKeysUseTheDefaults() {
        assertEq(defaults, NoteDocument.layout("<!-- page: size=a5; margins=huge; colour=red -->"))
    }

    fun testTheFirstLayoutLineCounts() {
        assertEq(defaults.copy(paper = NotePaper.Letter), NoteDocument.layout("<!-- page: size=letter -->\n<!-- page: size=legal; cover=yes -->"))
    }

    fun testLayoutLineInsideACodeFenceIsIgnored() {
        assertEq(defaults, NoteDocument.layout("```\n<!-- page: size=letter -->\n```\nBody"))
        assertEq(defaults.copy(paper = NotePaper.Legal), NoteDocument.layout("~~~\n<!-- page: size=letter -->\n~~~\n<!-- page: size=legal -->"))
    }

    fun testWritingTheLayoutReplacesTheFirstLineAndGoesFirst() {
        val layout = NoteLayout(NotePaper.Letter, NoteOrientation.Landscape, NoteMargins.Wide, cover = true, contents = false, numbers = true)
        assertEq(
            "<!-- page: size=letter; orientation=landscape; margins=wide; cover=yes; contents=no; numbers=yes -->\n\nIntro\nBody",
            NoteDocument.withLayout("Intro\n<!-- page: size=a4 -->\nBody", layout),
        )
    }

    fun testWritingDefaultsIntoANoteWithoutALayout() {
        assertEq(
            "<!-- page: size=a4; orientation=portrait; margins=normal; cover=no; contents=no; numbers=yes -->\n\nBody",
            NoteDocument.withLayout("Body", NoteLayout()),
        )
    }

    fun testWritingTheLayoutTwiceIsStable() {
        val layout = NoteLayout(contents = true)
        val once = NoteDocument.withLayout("Body", layout)
        assertEq(once, NoteDocument.withLayout(once, layout))
        assertEq(layout, NoteDocument.layout(once))
    }

    fun testSvgFenceWithCaptionIsAFigure() {
        val found = figures("```svg Figure 1\n<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 10 10\"></svg>\n```")
        assertEq(1, found.size)
        assertEq("Figure 1", found[0].caption)
        assertEq("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 10 10\"></svg>", found[0].svg)
    }

    fun testTildeFenceUpperCaseWithPrologIsAFigureWithoutCaption() {
        val found = figures("~~~SVG\n<?xml version=\"1.0\"?>\n<!-- c --><svg></svg>\n~~~")
        assertEq(1, found.size)
        assertEq("", found[0].caption)
    }

    fun testSvgFenceWithoutSvgMarkupIsCode() {
        assertEq(null, NoteDocument.figure("svg", "<div></div>"))
        val segments = NoteDocument.segments("```svg\n<div></div>\n```")
        assertEq(listOf<NoteSegment>(NoteSegment.Markdown("```svg\n<div></div>\n```")), segments)
    }

    fun testTheWordMustBeSvg() {
        assertEq(null, NoteDocument.figure("svgfoo", "<svg></svg>"))
        assertEq(0, figures("```svgfoo\n<svg></svg>\n```").size)
        assertEq(null, NoteDocument.figure("xml", "<svg></svg>"))
    }

    fun testSvgOverTheCapIsCode() {
        val prefix = "<svg>"
        val suffix = "</svg>"
        val atCap = prefix + "a".repeat(NoteDocument.MAX_SVG_BYTES - prefix.length - suffix.length) + suffix
        val over = prefix + "a".repeat(NoteDocument.MAX_SVG_BYTES + 1 - prefix.length - suffix.length) + suffix
        assertEq(262144, atCap.toByteArray().size)
        assertEq(262145, over.toByteArray().size)
        expect(NoteDocument.figure("svg", atCap) != null)
        assertEq(null, NoteDocument.figure("svg", over))
        assertEq(0, figures("```svg\n$over\n```").size)
    }

    fun testPageBreakLines() {
        expect(NoteDocument.isPageBreak("<!-- pagebreak -->"))
        expect(NoteDocument.isPageBreak("<!--PAGEBREAK-->"))
        expect(NoteDocument.isPageBreak("  <!--  pagebreak  -->  "))
        expect(!NoteDocument.isPageBreak("<!-- pagebreak --> text"))
        assertEq(
            listOf(NoteSegment.Markdown("One"), NoteSegment.PageBreak, NoteSegment.Markdown("Two")),
            NoteDocument.segments("One\n<!-- pagebreak -->\nTwo"),
        )
        assertEq(
            listOf<NoteSegment>(NoteSegment.Markdown("One <!-- pagebreak --> text")),
            NoteDocument.segments("One <!-- pagebreak --> text"),
        )
    }

    fun testPageBreakInsideAFenceStaysCode() {
        val text = "```\n<!-- pagebreak -->\n```"
        assertEq(listOf<NoteSegment>(NoteSegment.Markdown(text)), NoteDocument.segments(text))
    }

    fun testSegmentsDropLayoutLines() {
        assertEq(listOf<NoteSegment>(NoteSegment.Markdown("\nBody")), NoteDocument.segments("<!-- page: size=letter -->\n\nBody"))
    }

    fun testPdfFileNames() {
        assertEq("project-proposal.pdf", NoteDocument.pdfFileName("Project Proposal"))
        assertEq("note.pdf", NoteDocument.pdfFileName(""))
        assertEq("note.pdf", NoteDocument.pdfFileName("  "))
        assertEq("n-code-x.pdf", NoteDocument.pdfFileName("Ünïcode / x"))
    }

    fun testHeadingsForContentsSkipFencesAndDeeperLevels() {
        val headings = NoteDocument.headings("# One\n## Two\n### Three\n```\n# Not a heading\n```\n# Four")
        assertEq(listOf("One" to 1, "Two" to 2, "Four" to 1), headings.map { it.text to it.level })
        assertEq(listOf("note-h-1", "note-h-2", "note-h-4"), headings.map { it.id })
    }

    fun testFigureBoxFromSizeAndViewBox() {
        assertEq(NoteFigureBox(200.0, 0.5), NoteDocument.figureBox("<svg width=\"200\" height=\"100\"></svg>"))
        assertEq(NoteFigureBox(null, 0.25), NoteDocument.figureBox("<svg viewBox=\"0 0 640 160\"></svg>"))
        assertEq(NoteFigureBox(96.0, 0.5), NoteDocument.figureBox("<?xml version=\"1.0\"?><svg width='1in' viewBox='0,0,20,10' stroke-width=\"3\"></svg>"))
        assertEq(NoteFigureBox(null, 0.5), NoteDocument.figureBox("<svg width=\"100%\" viewBox=\"0 0 20 10\"></svg>"))
        assertEq(NoteFigureBox(300.0, 0.5), NoteDocument.figureBox("<svg></svg>"))
    }

    fun testFigurePreviewIsAnImageWithNoScript() {
        val html = NoteDocument.figurePreviewHtml(NoteFigure("<svg><script>alert(1)</script></svg>", "A \"quoted\" <b>"))
        expect(html.contains("<img src=\"data:image/svg+xml;base64,"))
        expect(!html.contains("<script"))
        expect(html.contains("alt=\"A &quot;quoted&quot; &lt;b&gt;\""))
    }

    private val sample = """
<!-- page: size=letter; cover=yes; contents=yes -->

# Intro

Some **bold** text and a [link](https://forkmesh.com) and <script>alert(1)</script>.

```svg Figure 1. Flow
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"><script>alert(2)</script></svg>
```

<!-- pagebreak -->

## Details

- one
- [x] done

| A | B |
| - | -: |
| 1 | 2 |

![Logo](data:image/png;base64,AAAA) ![Bad](javascript:alert(3)) [Evil](javascript:alert(4))

```js
<script>code()</script>
```
""".trimStart()

    fun testPrintHtmlCarriesCoverContentsFiguresAndBreaks() {
        val layout = NoteDocument.layout(sample)
        val html = NoteDocument.printHtml("Project <Proposal>", "📝", sample, layout, LocalDate.of(2026, 11, 1))
        expect(html.startsWith("<!doctype html>"))
        expect(html.contains("<title>project-proposal</title>"))
        expect(html.contains("<section class=\"note-cover\"><div class=\"note-cover-emoji\">📝</div><h1>Project &lt;Proposal&gt;</h1><div class=\"note-cover-date\">November 1, 2026</div></section>"))
        expect(html.contains("<section class=\"note-contents\"><h1>Contents</h1><ol><li class=\"note-contents-1\"><a href=\"#note-h-1\">Intro</a></li><li class=\"note-contents-2\"><a href=\"#note-h-2\">Details</a></li></ol></section>"))
        expect(html.contains("<h1 id=\"note-h-1\">Intro</h1>"))
        expect(html.contains("<h2 id=\"note-h-2\">Details</h2>"))
        expect(!html.contains("<article><h1>Project"))
        val svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 10 10\"><script>alert(2)</script></svg>"
        val encoded = Base64.getEncoder().encodeToString(svg.toByteArray())
        expect(html.contains("<figure class=\"note-figure\"><img src=\"data:image/svg+xml;base64,$encoded\" alt=\"Figure 1. Flow\"><figcaption>Figure 1. Flow</figcaption></figure>"))
        expect(html.contains("<div class=\"note-page-break\"></div>"))
        expect(html.contains("break-before: page"))
        expect(html.contains("<strong>bold</strong>"))
        expect(html.contains("<a href=\"https://forkmesh.com\">link</a>"))
        expect(html.contains("<img src=\"data:image/png;base64,AAAA\" alt=\"Logo\">"))
        expect(!html.contains("javascript:"))
        expect(!html.contains("<script"))
        expect(html.contains("&lt;script&gt;code()&lt;/script&gt;"))
        expect(html.contains("<th>A</th><th style=\"text-align:right\">B</th>"))
        expect(html.contains("☑"))
        expect(!html.contains("page:"))
        expect(!html.contains("pagebreak"))
    }

    fun testPrintHtmlPageRules() {
        val html = NoteDocument.printHtml("Notes", "", "Body", NoteLayout(NotePaper.Letter, NoteOrientation.Landscape, NoteMargins.Narrow), LocalDate.of(2026, 11, 1))
        expect(html.contains("@page { size: 279.4mm 215.9mm; margin: 12.7mm; @bottom-center { content: counter(page) \" / \" counter(pages);"))
        expect(!html.contains("@page :first"))
        expect(!html.contains("note-cover\""))
        expect(!html.contains("note-contents\""))
        expect(html.contains("<article><h1>Notes</h1><p>Body</p></article>"))
        val covered = NoteDocument.printHtml("", "", "Body", NoteLayout(cover = true), LocalDate.of(2026, 1, 5))
        expect(covered.contains("@page :first { @bottom-center { content: none; } }"))
        expect(covered.contains("<h1>Untitled note</h1><div class=\"note-cover-date\">January 5, 2026</div>"))
        expect(covered.contains("<title>note</title>"))
        val plain = NoteDocument.printHtml("T", "", "Body", NoteLayout(numbers = false), LocalDate.of(2026, 1, 5))
        expect(!plain.contains("counter(page)"))
        expect(plain.contains("background: #fff"))
    }
}
