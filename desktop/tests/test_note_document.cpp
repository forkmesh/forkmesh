#include "NoteDocument.h"
#include "NoteDocumentRender.h"

#include <QBuffer>
#include <QFile>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTextDocument>

#include <cstdio>

using namespace forkmesh::notes;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

const QString kSvg = QStringLiteral(
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 640 200\">"
    "<rect x=\"10\" y=\"10\" width=\"620\" height=\"180\" fill=\"#0969da\"/>"
    "<circle cx=\"320\" cy=\"100\" r=\"60\" fill=\"#ffffff\"/></svg>");

bool isLayout(const PageLayout &layout, PaperSize size, PaperOrientation orientation,
              PageMargins margins, bool cover, bool contents, bool numbers)
{
    return layout.size == size && layout.orientation == orientation &&
           layout.margins == margins && layout.cover == cover &&
           layout.contents == contents && layout.numbers == numbers;
}

void testLayoutParsing()
{
    check(isLayout(notePageLayout(QStringLiteral("# Title\n\nBody")), PaperSize::A4,
                   PaperOrientation::Portrait, PageMargins::Normal, false, false, true),
          "no layout line gives the defaults");
    check(notePageLayout(QString()) == PageLayout(), "an empty note gives the defaults");
    check(isLayout(notePageLayout(QStringLiteral("<!-- page: size=letter; cover=yes -->\nBody")),
                   PaperSize::Letter, PaperOrientation::Portrait, PageMargins::Normal, true,
                   false, true),
          "size and cover are read, the rest default");
    check(isLayout(notePageLayout(QStringLiteral(
                       "<!--page:size = LEGAL;orientation=landscape;margins=wide;numbers=no-->")),
                   PaperSize::Legal, PaperOrientation::Landscape, PageMargins::Wide, false,
                   false, false),
          "keys and values are compared lower case, spaces around = are allowed");
    check(notePageLayout(QStringLiteral("<!-- page: size=a5; margins=huge; colour=red -->")) ==
              PageLayout(),
          "unknown values and keys use the defaults");
    check(isLayout(notePageLayout(QStringLiteral("<!-- page: size=legal -->\n"
                                                 "<!-- page: size=letter; cover=yes -->")),
                   PaperSize::Legal, PaperOrientation::Portrait, PageMargins::Normal, false,
                   false, true),
          "the first layout line counts");
    check(notePageLayout(QStringLiteral("```\n<!-- page: size=letter -->\n```\nBody")) ==
              PageLayout(),
          "a layout line inside a code fence is ignored");
    check(notePageLayout(QStringLiteral("~~~~\n<!-- page: size=letter -->\n~~~~\n"
                                        "<!-- page: size=legal -->"))
                  .size == PaperSize::Legal,
          "the first layout line after a tilde fence counts");
    check(notePageLayout(QStringLiteral("  <!-- page: contents=yes -->  ")).contents,
          "spaces around the layout line are allowed");
    check(notePageLayout(QStringLiteral("text <!-- page: size=letter -->")) == PageLayout(),
          "a layout comment inside a line is not a layout line");
    check(notePageLayout(QStringLiteral("<!-- page: size=letter -->\r\nBody")).size ==
              PaperSize::Letter,
          "CRLF line ends are accepted");
    check(hasPageLayoutLine(QStringLiteral("<!-- page: -->")),
          "an empty layout line is still a layout line");
}

void testLayoutWriting()
{
    PageLayout layout;
    layout.size = PaperSize::Letter;
    layout.orientation = PaperOrientation::Landscape;
    layout.margins = PageMargins::Wide;
    layout.cover = true;
    layout.contents = false;
    layout.numbers = true;
    check(withLayout(QStringLiteral("Intro\n<!-- page: size=a4 -->\nBody"), layout) ==
              QStringLiteral("<!-- page: size=letter; orientation=landscape; margins=wide; "
                             "cover=yes; contents=no; numbers=yes -->\n\nIntro\nBody"),
          "writing replaces the first layout line and puts it first");
    check(withLayout(QStringLiteral("Body"), PageLayout()) ==
              QStringLiteral("<!-- page: size=a4; orientation=portrait; margins=normal; "
                             "cover=no; contents=no; numbers=yes -->\n\nBody"),
          "writing the defaults adds a full layout line at the top");
    const QString twice = withLayout(withLayout(QStringLiteral("\n\nBody"), layout), layout);
    check(twice == pageLayoutLine(layout) + QStringLiteral("\n\nBody"),
          "writing twice keeps one line and one blank line");
    check(notePageLayout(withLayout(QStringLiteral("Body"), layout)) == layout,
          "a written layout reads back the same");
    check(withLayout(QStringLiteral("```\n<!-- page: size=a4 -->\n```"), layout) ==
              pageLayoutLine(layout) + QStringLiteral("\n\n```\n<!-- page: size=a4 -->\n```"),
          "a layout line inside a fence is left alone");
    check(withoutLayoutLines(QStringLiteral("<!-- page: size=a4 -->\nA\n<!-- page: x -->\nB")) ==
              QStringLiteral("A\nB"),
          "display drops every layout line");
    check(paperMillimetres(layout) == QSizeF(279.4, 215.9),
          "landscape swaps the paper sides");
    check(marginMillimetres(PageMargins::Narrow) == 12.7 &&
              marginMillimetres(PageMargins::Normal) == 25.4 &&
              marginMillimetres(PageMargins::Wide) == 38.1,
          "margins in millimetres");
}

std::optional<NoteFigure> onlyFigure(const QString &markdown)
{
    const QList<NotePart> parts = noteParts(markdown);
    for (const NotePart &part : parts) {
        if (part.kind == NotePartKind::Figure)
            return part.figure;
    }
    return std::nullopt;
}

void testFigures()
{
    const auto figure = onlyFigure(QStringLiteral("```svg Figure 1\n") + kSvg +
                                   QStringLiteral("\n```"));
    check(figure.has_value(), "```svg with <svg> is a figure");
    check(figure && figure->caption == QStringLiteral("Figure 1"), "the caption is read");
    check(figure && figure->svg == kSvg, "the figure keeps its markup");
    const auto tilde = onlyFigure(QStringLiteral(
        "~~~SVG\n<?xml version=\"1.0\"?>\n<!-- c --><svg>\n</svg>\n~~~"));
    check(tilde.has_value(), "~~~SVG with a prolog and a comment is a figure");
    check(tilde && tilde->caption.isEmpty(), "no caption is fine");
    check(!onlyFigure(QStringLiteral("```svg\n<div>\n```")),
          "a body that is not <svg> stays a code block");
    check(!onlyFigure(QStringLiteral("```svgfoo\n<svg></svg>\n```")),
          "the info word must be svg");
    check(!onlyFigure(QStringLiteral("```svg\n<svgx></svgx>\n```")),
          "the element must be svg itself");
    const QString open = QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\">");
    const QString close = QStringLiteral("</svg>");
    QString big = open + QString(kSvgFigureMaxBytes - open.size() - close.size() + 1,
                                 QLatin1Char(' ')) + close;
    check(big.toUtf8().size() == kSvgFigureMaxBytes + 1, "the oversized body is 262145 bytes");
    check(!onlyFigure(QStringLiteral("```svg\n") + big + QStringLiteral("\n```")),
          "a 262145-byte body stays a code block");
    big.chop(close.size() + 1);
    big += close;
    check(onlyFigure(QStringLiteral("```svg\n") + big + QStringLiteral("\n```")).has_value(),
          "a 262144-byte body is a figure");
    const QList<NotePart> code = noteParts(QStringLiteral("```svg\n<div>\n```"));
    check(code.size() == 1 && code.first().kind == NotePartKind::Markdown &&
              code.first().markdown == QStringLiteral("```svg\n<div>\n```"),
          "a code block is kept as written");
    const NoteFigureSize box = svgFigureSize(kSvg);
    check(!box.hasWidth() && qAbs(box.aspect - 0.3125) < 1e-9,
          "a viewBox-only diagram fills the column");
    const NoteFigureSize sized = svgFigureSize(
        QStringLiteral("<svg width=\"200px\" height=\"100\" viewBox=\"0 0 10 10\">"));
    check(sized.width == 200 && sized.aspect == 0.5, "width and height set the natural size");
    check(figureDisplaySize(sized, 120) == QSizeF(120, 60), "a figure scales down to the column");
    check(figureDisplaySize(box, 640) == QSizeF(640, 200), "a viewBox figure takes the column");
    check(svgWithoutExternalReferences(QStringLiteral("<image href=\"file:///etc/x.png\"/>")) ==
              QStringLiteral("<image href=\"\"/>"),
          "external references are dropped before drawing");
    check(svgWithoutExternalReferences(QStringLiteral("<use xlink:href='#a'/>")) ==
              QStringLiteral("<use xlink:href='#a'/>"),
          "internal references are kept");
}

void testPageBreaks()
{
    check(isPageBreakLine(QStringLiteral("<!-- pagebreak -->")), "<!-- pagebreak --> breaks");
    check(isPageBreakLine(QStringLiteral("<!--PAGEBREAK-->")), "upper case, no spaces breaks");
    check(isPageBreakLine(QStringLiteral("  <!--  pagebreak  -->  ")),
          "spaces inside and around break");
    check(!isPageBreakLine(QStringLiteral("<!-- pagebreak --> text")),
          "text after the comment is not a break");
    check(!isPageBreakLine(QStringLiteral("<!-- page break -->")), "the word is pagebreak");
    const QList<NotePart> parts =
        noteParts(QStringLiteral("A\n<!-- pagebreak -->\nB\n```\n<!-- pagebreak -->\n```"));
    check(parts.size() == 3 && parts.at(1).kind == NotePartKind::PageBreak &&
              parts.at(2).markdown.contains(QStringLiteral("<!-- pagebreak -->")),
          "a break inside a code block is code");
}

void testPlaceholders()
{
    const QString token = QStringLiteral("tokx");
    const PlaceholderMarkdown out = withObjectPlaceholders(
        QStringLiteral("<!-- page: cover=yes -->\nIntro\n\n```svg Cap\n") + kSvg +
            QStringLiteral("\n```\n<!-- pagebreak -->\nEnd"),
        token);
    check(out.objects.size() == 2 && out.objects.at(0).kind == NotePartKind::Figure &&
              out.objects.at(1).kind == NotePartKind::PageBreak,
          "figures and breaks become objects in order");
    check(out.markdown == QStringLiteral("Intro\n\ntokx0\n\ntokx1\n\nEnd"),
          "objects become their own paragraphs and the layout line goes");
    check(notePlaceholderIndex(QStringLiteral("tokx1"), token, 2) == 1, "placeholder index");
    check(notePlaceholderIndex(QStringLiteral("tokx2"), token, 2) == -1, "out of range");
    check(notePlaceholderIndex(QStringLiteral("tokx01"), token, 2) == -1, "not canonical");
    check(notePlaceholderIndex(QStringLiteral("tokxcover"), token, 2) == -1, "not a number");
}

void testHeadings()
{
    const QList<NoteHeading> headings = noteHeadings(QStringLiteral(
        "# One\n\ntext\n\n## Two **bold** ##\n\n### Three\n\n```\n# not\n```\n"
        "Setext\n======\n\nSub [link](https://x)\n---\n\n- item\n---\n"));
    const QList<NoteHeading> expected = {{1, QStringLiteral("One")},
                                         {2, QStringLiteral("Two bold")},
                                         {1, QStringLiteral("Setext")},
                                         {2, QStringLiteral("Sub link")}};
    check(headings == expected, "level 1 and 2 headings outside code, in order");
}

void testFileNames()
{
    check(pdfFileName(QStringLiteral("Project Proposal")) == QStringLiteral("project-proposal.pdf"),
          "Project Proposal");
    check(pdfFileName(QString()) == QStringLiteral("note.pdf"), "empty title");
    check(pdfFileName(QStringLiteral("  ")) == QStringLiteral("note.pdf"), "blank title");
    check(pdfFileName(QString::fromUtf8("Ünïcode / x")) == QStringLiteral("n-code-x.pdf"),
          "the Markdown slug rule");
    check(exportDateLabel(QDate(2026, 11, 1)) == QStringLiteral("November 1, 2026"),
          "export date wording");
    check(pageNumberLabel(2, 5) == QStringLiteral("2 / 5"), "page number wording");
    PageLayout layout;
    layout.cover = true;
    check(!showsPageNumber(0, layout) && showsPageNumber(1, layout),
          "the cover has no page number");
    layout.numbers = false;
    check(!showsPageNumber(1, layout), "numbers=no hides them");
}

int pdfPageObjects(const QByteArray &pdf)
{
    static const QRegularExpression page(QStringLiteral("/Type\\s*/Page(?![a-zA-Z])"));
    int count = 0;
    auto it = page.globalMatch(QString::fromLatin1(pdf));
    while (it.hasNext()) {
        it.next();
        ++count;
    }
    return count;
}

void testPdf()
{
    QTemporaryDir dir;
    check(dir.isValid(), "temporary folder");
    PageLayout layout;
    layout.cover = true;
    layout.contents = true;
    NotePdfInput input;
    input.title = QStringLiteral("Project Proposal");
    input.emoji = QString::fromUtf8("\xF0\x9F\x93\x84");
    input.layout = layout;
    input.date = QDate(2026, 11, 1);
    input.markdown = QStringLiteral(
        "<!-- page: cover=yes; contents=yes -->\n\n# Summary\n\nA short summary with "
        "`code` and a [link](https://forkmesh.com).\n\n```svg Figure 1. Push path\n") +
        kSvg +
        QStringLiteral("\n```\n\n<!-- pagebreak -->\n\n## Budget\n\n| Item | Cost |\n| --- | --- |\n"
                       "| Servers | 10 |\n\n```\nplain code\n```\n");
    const QString path = dir.filePath(pdfFileName(input.title));
    const NotePdfResult result = writeNotePdfFile(path, input);
    check(result.ok, "the PDF is written");
    check(result.pages == 4, "cover, contents, and the body on two pages");
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "the PDF can be read back");
    const QByteArray pdf = file.readAll();
    check(pdf.startsWith("%PDF"), "the file is a PDF");
    check(pdf.size() > 20000, "the PDF has content");
    check(pdfPageObjects(pdf) == result.pages, "the PDF has one page object per page");

    NotePdfInput plain = input;
    plain.layout = PageLayout();
    plain.markdown = QStringLiteral("Body only");
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    const NotePdfResult single = writeNotePdf(&buffer, plain);
    check(single.ok && single.pages == 1, "a short note without cover is one page");
    check(pdfPageObjects(buffer.data()) == 1, "one page object");

    NotePdfInput letter = plain;
    letter.layout.size = PaperSize::Letter;
    letter.layout.orientation = PaperOrientation::Landscape;
    QBuffer wide;
    wide.open(QIODevice::WriteOnly);
    check(writeNotePdf(&wide, letter).ok, "a landscape letter PDF is written");
    check(wide.data().contains("/MediaBox [0 0 792.000000 612.000000]"),
          "the landscape letter page is 11 x 8.5 in");

    check(!writeNotePdfFile(dir.filePath(QStringLiteral("missing/x.pdf")), plain).ok,
          "an unwritable path fails");
}

void testPreviewObjects()
{
    const QString token = noteObjectToken();
    const PlaceholderMarkdown placeholders = withObjectPlaceholders(
        QStringLiteral("Intro\n\n```svg Caption here\n") + kSvg +
            QStringLiteral("\n```\n\n<!-- pagebreak -->\n\nEnd"),
        token);
    QTextDocument doc;
    doc.setMarkdown(placeholders.markdown);
    NoteRenderCache cache;
    NotePreviewStyle style;
    style.columnWidth = 500;
    style.devicePixelRatio = 2;
    applyNotePreviewObjects(&doc, token, placeholders.objects, style, cache);
    const QString text = doc.toPlainText();
    check(!text.contains(token), "no placeholder text is left");
    check(text.contains(QStringLiteral("Caption here")), "the caption is shown as text");
    int images = 0;
    QTextImageFormat figure;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextCharFormat format = it.fragment().charFormat();
            if (!format.isImageFormat())
                continue;
            ++images;
            if (format.intProperty(NoteObjectKind) == int(NotePartKind::Figure)) {
                figure = format.toImageFormat();
                check(block.blockFormat().alignment().testFlag(Qt::AlignHCenter),
                      "the figure is centred");
            }
        }
    }
    check(images == 2, "the figure and the break are images");
    check(qAbs(figure.width() - 500) < 1, "the figure card takes the column width");
    const QImage card = doc.resource(QTextDocument::ImageResource, QUrl(figure.name()))
                            .value<QImage>();
    check(card.width() == 1000 && card.devicePixelRatio() == 2,
          "the card is drawn at the device pixel ratio");
    check(card.pixelColor(card.width() / 2, card.height() / 2) == QColor(Qt::white),
          "the diagram is drawn on the white card");
    check(card.pixelColor(card.width() / 2, card.height() / 2 - 100) == QColor(0x09, 0x69, 0xda),
          "the diagram itself is drawn");
    style.columnWidth = 300;
    applyNotePreviewObjects(&doc, token, placeholders.objects, style, cache);
    bool resized = false;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextCharFormat format = it.fragment().charFormat();
            if (format.intProperty(NoteObjectKind) == int(NotePartKind::Figure) &&
                format.hasProperty(NoteObjectIndex))
                resized = qAbs(format.toImageFormat().width() - 300) < 1;
        }
    }
    check(resized, "a narrower column redraws the figure in place");
}

}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    testLayoutParsing();
    testLayoutWriting();
    testFigures();
    testPageBreaks();
    testPlaceholders();
    testHeadings();
    testFileNames();
    testPreviewObjects();
    testPdf();
    if (failures) {
        std::fprintf(stderr, "%d note document check(s) failed\n", failures);
        return 1;
    }
    std::printf("note document tests passed\n");
    return 0;
}
