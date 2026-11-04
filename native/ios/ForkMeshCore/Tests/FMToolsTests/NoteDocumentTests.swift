import FMApi
import FMBase
import XCTest
@testable import FMTools

final class NoteDocumentTests: XCTestCase {
    let clock = FixedClock(epochMillis: toolsNowMillis)
    let instant: ToolsSleep = { _ in }
    let utc = TimeZone(identifier: "UTC")!
    let november = Date(timeIntervalSince1970: 1_793_534_400)

    func layout(_ size: NotePrintLayout.Size, _ orientation: NotePrintLayout.Orientation, _ margins: NotePrintLayout.Margins, _ cover: Bool, _ contents: Bool, _ numbers: Bool) -> NotePrintLayout {
        NotePrintLayout(size: size, orientation: orientation, margins: margins, cover: cover, contents: contents, numbers: numbers)
    }

    func testNoLayoutLineUsesTheDefaults() {
        XCTAssertEqual(NoteDocument.layout("# Title\n\nBody"), layout(.a4, .portrait, .normal, false, false, true))
        XCTAssertEqual(NoteDocument.layout(""), .standard)
    }

    func testReadsAPartialLayout() {
        XCTAssertEqual(NoteDocument.layout("<!-- page: size=letter; cover=yes -->\n\nBody"), layout(.letter, .portrait, .normal, true, false, true))
    }

    func testComparesKeysAndValuesLowerCaseWithLooseSpacing() {
        let line = "<!--page:size = LEGAL;orientation=landscape;margins=wide;numbers=no-->"
        XCTAssertEqual(NoteDocument.layout("Intro\n\(line)\nBody"), layout(.legal, .landscape, .wide, false, false, false))
        XCTAssertEqual(NoteDocument.layout("<!-- PAGE: SIZE=Letter -->"), layout(.letter, .portrait, .normal, false, false, true))
    }

    func testUnknownKeysAndValuesUseTheDefaults() {
        XCTAssertEqual(NoteDocument.layout("<!-- page: size=a5; margins=huge; colour=red -->"), .standard)
    }

    func testOnlyTheFirstLayoutLineCounts() {
        let text = "<!-- page: size=legal -->\nBody\n<!-- page: size=letter; cover=yes -->"
        XCTAssertEqual(NoteDocument.layout(text), layout(.legal, .portrait, .normal, false, false, true))
    }

    func testALayoutLineInsideACodeFenceIsIgnored() {
        let text = "```\n<!-- page: size=letter -->\n```\n~~~md\n<!-- page: size=legal -->\n~~~\nBody"
        XCTAssertEqual(NoteDocument.layout(text), .standard)
        XCTAssertEqual(NoteDocument.layout("```\n<!-- page: size=letter -->\n```\n<!-- page: size=legal -->").size, .legal)
    }

    func testWritesTheLayoutAsTheFirstLine() {
        let written = NoteDocument.withLayout("Intro\n<!-- page: size=a4 -->\nBody", layout(.letter, .landscape, .wide, true, false, true))
        XCTAssertEqual(written, "<!-- page: size=letter; orientation=landscape; margins=wide; cover=yes; contents=no; numbers=yes -->\n\nIntro\nBody")
    }

    func testWritesTheDefaultsIntoANoteWithoutALayout() {
        XCTAssertEqual(
            NoteDocument.withLayout("Body", .standard),
            "<!-- page: size=a4; orientation=portrait; margins=normal; cover=no; contents=no; numbers=yes -->\n\nBody")
    }

    func testWritingReplacesOnlyTheFirstLineAndDropsLeadingBlankLines() {
        let text = "<!-- page: size=legal -->\n\n\nBody\n```\n<!-- page: size=letter -->\n```"
        let written = NoteDocument.withLayout(text, layout(.a4, .portrait, .narrow, false, true, false))
        XCTAssertEqual(written, "<!-- page: size=a4; orientation=portrait; margins=narrow; cover=no; contents=yes; numbers=no -->\n\nBody\n```\n<!-- page: size=letter -->\n```")
        XCTAssertEqual(NoteDocument.layout(written), layout(.a4, .portrait, .narrow, false, true, false))
        XCTAssertEqual(NoteDocument.withLayout(written, layout(.a4, .portrait, .narrow, false, true, false)), written)
        XCTAssertEqual(NoteDocument.withLayout("", .standard), "\(NotePrintLayout.standard.line)\n\n")
        XCTAssertEqual(NoteDocument.withLayout("a\r\nb", .standard), "\(NotePrintLayout.standard.line)\n\na\nb")
    }

    func testStripsEveryLayoutLineOutsideFences() {
        let text = "<!-- page: size=legal -->\nBody\n  <!-- page: cover=yes -->  \n```\n<!-- page: size=letter -->\n```"
        XCTAssertEqual(NoteDocument.withoutLayout(text), "Body\n```\n<!-- page: size=letter -->\n```")
    }

    func testFigureWithCaption() {
        let parts = NoteDocument.parts("```svg Figure 1\n<svg viewBox=\"0 0 10 10\"></svg>\n```")
        XCTAssertEqual(parts, [.figure(NoteFigure(svg: "<svg viewBox=\"0 0 10 10\"></svg>", caption: "Figure 1"))])
    }

    func testFigureAfterAPrologAndComment() {
        let body = "<?xml version=\"1.0\"?>\n<!-- c --><svg>"
        let parts = NoteDocument.parts("~~~SVG\n\(body)\n~~~")
        XCTAssertEqual(parts, [.figure(NoteFigure(svg: body, caption: ""))])
        XCTAssertEqual(NoteDocument.figure(info: "  svg   Wide  shot  ", body: "  <svg/>")?.caption, "Wide  shot")
    }

    func testNonSvgBodiesAndOtherInfoWordsStayCode() {
        XCTAssertEqual(NoteDocument.parts("```svg\n<div>\n```"), [.markdown("```svg\n<div>\n```")])
        XCTAssertEqual(NoteDocument.parts("```svgfoo\n<svg></svg>\n```"), [.markdown("```svgfoo\n<svg></svg>\n```")])
        XCTAssertNil(NoteDocument.figure(info: "svg", body: "<svgx>"))
        XCTAssertNil(NoteDocument.figure(info: "svg", body: "<svg"))
        XCTAssertNil(NoteDocument.figure(info: "xml svg", body: "<svg/>"))
        XCTAssertNotNil(NoteDocument.figure(info: "svg", body: "<SVG xmlns=\"http://www.w3.org/2000/svg\"/>"))
    }

    func testTheSizeCapIs262144Bytes() {
        let head = "<svg>"
        let tail = "</svg>"
        let fits = head + String(repeating: "a", count: NoteDocument.svgMaxBytes - head.utf8.count - tail.utf8.count) + tail
        XCTAssertEqual(fits.utf8.count, 262144)
        XCTAssertNotNil(NoteDocument.figure(info: "svg", body: fits))
        let over = head + String(repeating: "a", count: NoteDocument.svgMaxBytes + 1 - head.utf8.count - tail.utf8.count) + tail
        XCTAssertEqual(over.utf8.count, 262145)
        XCTAssertNil(NoteDocument.figure(info: "svg", body: over))
        XCTAssertEqual(NoteDocument.parts("```svg\n\(over)\n```"), [.markdown("```svg\n\(over)\n```")])
    }

    func testPageBreakLines() {
        for line in ["<!-- pagebreak -->", "<!--PAGEBREAK-->", "  <!--  pagebreak  -->  "] {
            XCTAssertTrue(NoteDocument.isPageBreak(line), line)
        }
        XCTAssertFalse(NoteDocument.isPageBreak("<!-- pagebreak --> text"))
        XCTAssertFalse(NoteDocument.isPageBreak("<!-- page break -->"))
        XCTAssertFalse(NoteDocument.isLayoutLine("<!-- pagebreak -->"))
        XCTAssertEqual(NoteDocument.parts("One\n<!-- pagebreak -->\nTwo"), [.markdown("One"), .pageBreak, .markdown("Two")])
        XCTAssertEqual(NoteDocument.parts("One\n<!-- pagebreak --> text"), [.markdown("One\n<!-- pagebreak --> text")])
        XCTAssertEqual(NoteDocument.parts("```\n<!-- pagebreak -->\n```"), [.markdown("```\n<!-- pagebreak -->\n```")])
    }

    func testPartsHideTheLayoutLineAndKeepOrder() {
        let text = "<!-- page: cover=yes -->\n\n# Intro\n\n```svg A\n<svg/>\n```\n\n<!-- pagebreak -->\n\nEnd"
        XCTAssertEqual(NoteDocument.parts(text), [
            .markdown("\n# Intro\n"), .figure(NoteFigure(svg: "<svg/>", caption: "A")), .pageBreak, .markdown("\nEnd"),
        ])
        XCTAssertEqual(NoteDocument.parts("```svg\n<svg/>"), [.figure(NoteFigure(svg: "<svg/>", caption: ""))])
    }

    func testPdfFileNameUsesTheMarkdownSlug() {
        XCTAssertEqual(NoteDocument.pdfFileName("Project Proposal"), "project-proposal.pdf")
        XCTAssertEqual(NoteDocument.pdfFileName(""), "note.pdf")
        XCTAssertEqual(NoteDocument.pdfFileName("  "), "note.pdf")
        XCTAssertEqual(NoteDocument.pdfFileName("Ünïcode / x"), "n-code-x.pdf")
        XCTAssertEqual(NoteDocument.pdfFileName("U\u{0308}nïcode / x"), "u-n-code-x.pdf")
        XCTAssertEqual(NoteDocument.pdfFileName("Launch plan: Q4!"), "launch-plan-q4.pdf")
        XCTAssertEqual(NoteDocument.pdfFileName("a - b_c.d"), "a---b_c.d.pdf")
        XCTAssertEqual(NoteDocument.pdfFileName("١٢٣ 🚀 ok"), "ok.pdf")
    }

    func testHeadingsForContentsSkipFencesAndDeeperLevels() {
        let text = "# One\n\n```\n# Not a heading\n```\n\n## Two **bold**\n\n### Three\n\n<!-- pagebreak -->\n\n# One"
        XCTAssertEqual(NoteDocument.headings(text), [
            NoteHeading(level: 1, text: "One", anchor: "note-one"),
            NoteHeading(level: 2, text: "Two bold", anchor: "note-two-bold"),
            NoteHeading(level: 1, text: "One", anchor: "note-one-1"),
        ])
    }

    func testFigureSizes() {
        XCTAssertEqual(NoteFigureSize.of("<svg width=\"200\" height=\"100\">"), NoteFigureSize(width: 200, aspect: 0.5))
        XCTAssertEqual(NoteFigureSize.of("<svg xmlns='x' viewBox='0 0 640 200'>"), NoteFigureSize(width: nil, aspect: 0.3125))
        XCTAssertEqual(NoteFigureSize.of("<?xml version=\"1.0\"?><svg width=\"72pt\" viewBox=\"0,0,2,1\">"), NoteFigureSize(width: 96, aspect: 0.5))
        XCTAssertEqual(NoteFigureSize.of("<svg width=\"100%\" height=\"50\">"), NoteFigureSize(width: 300, aspect: 50.0 / 300))
        XCTAssertEqual(NoteFigureSize.of("<svg>"), NoteFigureSize(width: 300, aspect: 0.5))
    }

    func testFigurePageDrawsTheSvgAsAnImageOnly() {
        let figure = NoteFigure(svg: "<svg><script>alert(1)</script></svg>", caption: "")
        let page = figure.page
        XCTAssertTrue(page.contains("default-src 'none'; img-src data:; style-src 'unsafe-inline'"))
        XCTAssertTrue(page.contains("<img alt=\"\" src=\"data:image/svg+xml;base64,\(Data(figure.svg.utf8).base64EncodedString())\">"))
        XCTAssertFalse(page.contains("<script"))
        XCTAssertEqual(figure.label, "Diagram")
    }

    func testPaperAndMarginsInPoints() {
        let a4 = NotePrintLayout.standard.paperPoints
        XCTAssertEqual(a4.width, 595.28, accuracy: 0.01)
        XCTAssertEqual(a4.height, 841.89, accuracy: 0.01)
        let letter = layout(.letter, .landscape, .narrow, false, false, true)
        XCTAssertEqual(letter.paperPoints.width, 792, accuracy: 0.01)
        XCTAssertEqual(letter.paperPoints.height, 612, accuracy: 0.01)
        XCTAssertEqual(letter.marginPoints, 36, accuracy: 0.01)
        XCTAssertEqual(layout(.legal, .portrait, .wide, false, false, true).paperPoints.height, 1008, accuracy: 0.01)
        XCTAssertEqual(NotePrintLayout.standard.marginPoints, 72, accuracy: 0.01)
    }

    func testPageNumbersSkipTheCover() {
        XCTAssertEqual(NoteDocument.pageNumber(2, of: 5), "2 / 5")
        let covered = layout(.a4, .portrait, .normal, true, false, true)
        XCTAssertFalse(NoteDocument.showsNumber(onPage: 0, layout: covered))
        XCTAssertTrue(NoteDocument.showsNumber(onPage: 1, layout: covered))
        XCTAssertTrue(NoteDocument.showsNumber(onPage: 0, layout: .standard))
        XCTAssertFalse(NoteDocument.showsNumber(onPage: 1, layout: layout(.a4, .portrait, .normal, false, false, false)))
    }

    func testExportDate() {
        XCTAssertEqual(NoteDocument.exportDate(november, timeZone: utc), "November 1, 2026")
    }

    func testPrintDocumentWithCoverContentsFigureAndPageBreak() {
        let markdown = "<!-- page: size=letter; cover=yes; contents=yes -->\n\n# Plan <b>\n\nSome **bold** & [link](https://example.com/a?b=1&c=2) and [bad](javascript:alert(1)).\n\n## Steps\n\n- one\n  - nested\n- two\n\n```svg Figure 1. Flow\n<svg viewBox=\"0 0 4 2\"><script>x</script></svg>\n```\n\n<!-- pagebreak -->\n\n```js\nlet a = \"<x>\"\n```\n\n| A | B |\n| - | - |\n| `1` | 2 |"
        let html = NoteDocument.printHTML(
            title: " Project \"Proposal\" ", emoji: "🚀", markdown: markdown,
            layout: NoteDocument.layout(markdown), date: november, timeZone: utc)
        XCTAssertTrue(html.hasPrefix("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"))
        XCTAssertTrue(html.contains("<title>project-proposal</title>"))
        XCTAssertTrue(html.contains("@page { size: 215.9mm 279.4mm; margin: 25.4mm; }"))
        XCTAssertTrue(html.contains("<section class=\"note-cover\"><div class=\"note-cover-emoji\">🚀</div><h1>Project &quot;Proposal&quot;</h1><div class=\"note-cover-date\">November 1, 2026</div></section>"))
        XCTAssertTrue(html.contains("<section class=\"note-contents\"><h1>Contents</h1><ol><li class=\"note-contents-1\"><a href=\"#note-plan\">Plan</a></li><li class=\"note-contents-2\"><a href=\"#note-steps\">Steps</a></li></ol></section>"))
        XCTAssertTrue(html.contains("<article><h1 id=\"note-plan\">Plan </h1>"))
        XCTAssertFalse(html.contains("<b>"))
        XCTAssertTrue(html.contains("<strong>bold</strong> &amp; <a href=\"https://example.com/a?b=1&amp;c=2\">link</a> and bad."))
        XCTAssertFalse(html.contains("javascript"))
        XCTAssertTrue(html.contains("<ul><li>one<ul><li>nested</li></ul></li><li>two</li></ul>"))
        let svg = Data("<svg viewBox=\"0 0 4 2\"><script>x</script></svg>".utf8).base64EncodedString()
        XCTAssertTrue(html.contains("<figure class=\"note-figure\"><img src=\"data:image/svg+xml;base64,\(svg)\" alt=\"Figure 1. Flow\"><figcaption>Figure 1. Flow</figcaption></figure>"))
        XCTAssertFalse(html.contains("<script"))
        XCTAssertTrue(html.contains("<div class=\"note-page-break\"></div><pre><code>let a = &quot;&lt;x&gt;&quot;</code></pre>"))
        XCTAssertTrue(html.contains(".note-page-break { break-before: page; page-break-before: always;"))
        XCTAssertTrue(html.contains("<table><thead><tr><th>A</th><th>B</th></tr></thead><tbody><tr><td><code>1</code></td><td>2</td></tr></tbody></table>"))
        XCTAssertFalse(html.contains("<!-- page"))
        XCTAssertFalse(html.contains("pagebreak"))
        XCTAssertTrue(html.contains("background: #fff; color: #1f2328; font: 11pt/1.45"))
        XCTAssertTrue(html.contains("padding-top: 50mm"))
    }

    func testPrintDocumentWithoutCoverLeadsWithTheTitle() {
        let html = NoteDocument.printHTML(title: "", emoji: "🚀", markdown: "Body", layout: .standard, date: november, timeZone: utc)
        XCTAssertTrue(html.contains("<article><h1>Untitled note</h1><p>Body</p></article>"))
        XCTAssertFalse(html.contains("note-cover\""))
        XCTAssertFalse(html.contains("<section class=\"note-contents\""))
        XCTAssertTrue(html.contains("<title>note</title>"))
        XCTAssertTrue(html.contains("@page { size: 210mm 297mm; margin: 25.4mm; }"))
        let native = NoteDocument.printHTML(title: "x", emoji: "", markdown: "", layout: .standard, date: november, nativeMargins: true)
        XCTAssertTrue(native.contains("@page { margin: 0; }"))
        XCTAssertFalse(native.contains("size: 210mm"))
    }

    func testPrintDocumentImagesAndLandscapeSize() {
        let markdown = "![chart](https://example.com/c.png) [![logo](data:image/png;base64,AAAA)](https://example.com) ![x](javascript:1) `![code](y.png)`\n\n1. a\n2. b\n\n> quoted"
        let html = NoteDocument.printHTML(
            title: "Images", emoji: "", markdown: markdown,
            layout: layout(.a4, .landscape, .wide, false, false, true), date: november, timeZone: utc)
        XCTAssertTrue(html.contains("<p><img src=\"https://example.com/c.png\" alt=\"chart\"> <a href=\"https://example.com\"><img src=\"data:image/png;base64,AAAA\" alt=\"logo\"></a> x <code>![code](y.png)</code></p>"))
        XCTAssertTrue(html.contains("<ol><li>a</li><li>b</li></ol><blockquote><p>quoted</p></blockquote>"))
        XCTAssertTrue(html.contains("@page { size: 297mm 210mm; margin: 38.1mm; }"))
    }

    @MainActor
    func testEditorSavesThePageLayoutLikeAnEdit() async {
        let api = FakeToolsApi { _, _, _ in ["ok": true, "note": .object(noteJSON())] }
        let editor = NoteEditorModel(noteId: noteId, call: api.call, clock: clock, sleep: instant)
        await editor.load()
        XCTAssertEqual(editor.pageLayout, .standard)
        XCTAssertEqual(editor.pdfFileName, "launch-plan.pdf")
        let chosen = layout(.letter, .portrait, .normal, true, true, true)
        editor.savePageLayout(chosen)
        XCTAssertEqual(editor.markdown, "\(chosen.line)\n\n# Launch\n\n- ship it")
        XCTAssertEqual(editor.pageLayout, chosen)
        let html = editor.printHTML(chosen, date: november, timeZone: utc)
        XCTAssertTrue(html.contains("<h1>Launch plan</h1><div class=\"note-cover-date\">November 1, 2026</div>"))
        XCTAssertTrue(html.contains("@page { margin: 0; }"))
    }

    @MainActor
    func testAViewerCannotSaveTheLayout() async {
        let api = FakeToolsApi { _, _, _ in ["ok": true, "note": .object(noteJSON(role: "viewer"))] }
        let editor = NoteEditorModel(noteId: noteId, call: api.call, clock: clock, sleep: instant)
        await editor.load()
        editor.savePageLayout(layout(.legal, .portrait, .normal, false, false, true))
        XCTAssertEqual(editor.markdown, "# Launch\n\n- ship it")
    }

    func testWritesThePdfFile() throws {
        let directory = URL(fileURLWithPath: NSTemporaryDirectory())
        let file = try NoteDocument.write(Data("%PDF-1.4".utf8), fileName: "note.pdf", in: directory)
        XCTAssertEqual(file.lastPathComponent, "note.pdf")
        XCTAssertEqual(try Data(contentsOf: file), Data("%PDF-1.4".utf8))
        try FileManager.default.removeItem(at: file)
    }
}
