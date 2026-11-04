#include "NoteComments.h"
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

void testImageOptionLines()
{
    const QString options = QStringLiteral(
        "<!-- image: wrap=behind; align=left; width=80; height=60; rotate=0; margin=3.2; "
        "x=20; y=30; pages=all; recolor=grayscale; transparency=40; brightness=0; "
        "contrast=0 -->");
    check(isImageOptionsLine(options), "an image options line is recognised");
    check(isImageOptionsLine(QStringLiteral("  <!--IMAGE:-->  ")),
          "upper case, no spaces and empty options are still an options line");
    check(isImageOptionsLine(QStringLiteral("<!--  image: width=50  -->")),
          "spaces inside the comment are allowed");
    check(!isImageOptionsLine(QStringLiteral("<!-- image: width=50 --> text")),
          "text after the comment is not an options line");
    check(!isImageOptionsLine(QStringLiteral("text <!-- image: width=50 -->")),
          "an options comment inside a line is not an options line");
    check(!isImageOptionsLine(QStringLiteral("<!-- images: width=50 -->")) &&
              !isImageOptionsLine(QStringLiteral("<!-- page: size=a4 -->")),
          "only the image: word makes an options line");
    check(!isPageLayoutLine(options) && !isPageBreakLine(options),
          "an options line is neither a layout line nor a break");

    const QList<NotePart> parts = noteParts(
        QStringLiteral("Intro\n") + options + QStringLiteral("\n![Cat](cat.png)\nEnd"));
    check(parts.size() == 1 && parts.first().kind == NotePartKind::Markdown &&
              parts.first().markdown == QStringLiteral("Intro\n![Cat](cat.png)\nEnd"),
          "the options line is hidden and the image below it stays");
    check(noteParts(options + QStringLiteral("\r\n![Cat](cat.png)")).value(0).markdown ==
              QStringLiteral("![Cat](cat.png)"),
          "CRLF line ends are accepted");
    const QList<NotePart> fenced = noteParts(
        QStringLiteral("```\n") + options + QStringLiteral("\n```\n~~~~\n") + options +
        QStringLiteral("\n~~~~"));
    check(fenced.size() == 1 && fenced.first().markdown.count(options) == 2,
          "an options line inside a code fence is code");
    check(notePageLayout(options + QStringLiteral("\n<!-- page: size=letter -->")).size ==
              PaperSize::Letter,
          "an options line does not hide the layout line after it");
    const PlaceholderMarkdown out = withObjectPlaceholders(
        options + QStringLiteral("\n![Cat](cat.png)\n<!-- pagebreak -->\nEnd"),
        QStringLiteral("tokx"));
    check(!out.markdown.contains(QStringLiteral("image:")) &&
              out.markdown.contains(QStringLiteral("![Cat](cat.png)")) &&
              out.objects.size() == 1,
          "the preview gets the image without its options line");
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

void testDeferredPreviewFigure()
{
    const QString token = noteObjectToken();
    const PlaceholderMarkdown placeholders = withObjectPlaceholders(
        QStringLiteral("```svg\n") + kSvg + QStringLiteral("\n```"), token);
    QTextDocument doc;
    doc.setMarkdown(placeholders.markdown);
    NoteRenderCache cache;
    QList<QPair<QString, std::function<QImage()>>> deferred;
    cache.defer = [&deferred](const QString &key, std::function<QImage()> make) {
        deferred.append({key, std::move(make)});
    };
    NotePreviewStyle style;
    style.columnWidth = 500;
    applyNotePreviewObjects(&doc, token, placeholders.objects, style, cache);
    check(deferred.size() == 1, "an uncached figure is handed to the worker");
    QString name;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it)
            if (it.fragment().charFormat().intProperty(NoteObjectKind) == int(NotePartKind::Figure))
                name = it.fragment().charFormat().toImageFormat().name();
    const auto centre = [&doc, &name] {
        const QImage card = doc.resource(QTextDocument::ImageResource, QUrl(name)).value<QImage>();
        return card.pixelColor(card.width() / 2, card.height() / 2 - 50);
    };
    check(!name.isEmpty() && centre() == QColor(Qt::white),
          "the preview shows the empty card while the figure draws");
    applyNotePreviewObjects(&doc, token, placeholders.objects, style, cache);
    check(deferred.size() == 1, "a figure already drawing is not queued twice");
    cache.store(deferred.first().first, deferred.first().second());
    applyNotePreviewObjects(&doc, token, placeholders.objects, style, cache);
    check(centre() == QColor(0x09, 0x69, 0xda), "the drawn figure replaces the empty card");
}

QJsonObject activityEvent(const QString &kind, const QString &actor, qint64 at,
                          const QJsonObject &extra = {})
{
    QJsonObject event{{QStringLiteral("kind"), kind},
                      {QStringLiteral("actor"), actor},
                      {QStringLiteral("at"), double(at)}};
    for (auto it = extra.begin(); it != extra.end(); ++it)
        event.insert(it.key(), it.value());
    return event;
}

void testSideColumnActivity()
{
    const qint64 now = 1'800'000'000'000LL;
    const QJsonObject payload{
        {QStringLiteral("counts"),
         QJsonObject{{QStringLiteral("webViews"), 4},
                     {QStringLiteral("webReaders"), 2},
                     {QStringLiteral("webLastViewedAt"), double(now - 500)}}},
        {QStringLiteral("events"),
         QJsonArray{
             activityEvent(QStringLiteral("share"), QStringLiteral("alice"), now,
                           {{QStringLiteral("detail"),
                             QJsonObject{{QStringLiteral("name"), QStringLiteral("bob")},
                                         {QStringLiteral("role"), QStringLiteral("editor")}}}}),
             activityEvent(QStringLiteral("edit"), QStringLiteral("bob"), now - 1000,
                           {{QStringLiteral("version"), 4}}),
             activityEvent(QStringLiteral("edit"), QStringLiteral("bob"), now - 2000,
                           {{QStringLiteral("version"), 3}}),
             activityEvent(QStringLiteral("edit"), QStringLiteral("bob"),
                           now - 2000 - kNoteEditGroupMs - 1,
                           {{QStringLiteral("version"), 2}}),
             activityEvent(QStringLiteral("view"), QStringLiteral("bob"),
                           now - 2 * kNoteEditGroupMs, {{QStringLiteral("count"), 3}}),
             activityEvent(QStringLiteral("restore"), QStringLiteral("alice"),
                           now - 3 * kNoteEditGroupMs,
                           {{QStringLiteral("detail"),
                             QJsonObject{{QStringLiteral("version"), 1},
                                         {QStringLiteral("as"), 5}}}}),
             activityEvent(QString(), QStringLiteral("x"), now),
         }},
    };
    const QList<NoteActivityEntry> entries = noteActivityEntries(payload);
    check(entries.size() == 6, "activity groups close edits and drops kindless events");
    if (entries.size() != 6)
        return;
    check(entries.at(0).kind == QLatin1String("share") &&
              noteActivityText(entries.at(0)) == QLatin1String("shared it with bob · can edit"),
          "a share names the person and the role");
    check(noteActivityActor(entries.at(0), QStringLiteral("Alice")) == QLatin1String("You"),
          "your own events read You");
    check(entries.at(1).kind == QLatin1String("web") &&
              noteActivityText(entries.at(1)) ==
                  QLatin1String("4 reads of the published page by 2 readers") &&
              noteActivityActor(entries.at(1), QString()).isEmpty(),
          "published page reads join the timeline without an actor");
    check(noteActivityText(entries.at(2)) == QLatin1String("edited it · v3–v4, 2 saves"),
          "saves by one person close together fold into one row");
    check(noteActivityText(entries.at(3)) == QLatin1String("edited it · v2"),
          "an older save starts a new row");
    check(noteActivityText(entries.at(4)) == QLatin1String("viewed it 3 times") &&
              noteActivityActor(entries.at(4), QStringLiteral("alice")) == QLatin1String("bob"),
          "views keep their count and the viewer's name");
    check(noteActivityText(entries.at(5)) == QLatin1String("restored version 1 as v5"),
          "a restore names both versions");

    const QList<NoteVersionEntry> versions = noteVersionEntries(QJsonArray{
        QJsonObject{{QStringLiteral("version"), 2},
                    {QStringLiteral("createdAt"), double(now)},
                    {QStringLiteral("author"), QStringLiteral("bob")}},
        QJsonObject{{QStringLiteral("version"), 0}},
        QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("createdAt"), double(now - 9)}},
    });
    check(versions.size() == 2 && versions.at(0).author == QLatin1String("bob") &&
              versions.at(0).createdAt == now && versions.at(1).author.isEmpty(),
          "history keeps numbered versions with who saved them and when");
}

}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    testLayoutParsing();
    testLayoutWriting();
    testFigures();
    testPageBreaks();
    testImageOptionLines();
    testPlaceholders();
    testHeadings();
    testFileNames();
    testPreviewObjects();
    testDeferredPreviewFigure();
    testPdf();
    testSideColumnActivity();
    if (failures) {
        std::fprintf(stderr, "%d note document check(s) failed\n", failures);
        return 1;
    }
    std::printf("note document tests passed\n");
    return 0;
}
