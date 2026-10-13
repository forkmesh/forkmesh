#include "TaskMarkdown.h"

#include <QAbstractTextDocumentLayout>
#include <QGuiApplication>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextList>

#include <cstdio>

using namespace forkmesh::taskmarkdown;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

void testTablesHeadingsAndLists()
{
    const QString html = toHtml(QStringLiteral(
        "## Why\n\nThe cores run **hot** and *slow*.\n\n"
        "| Route | Queries |\n|---|---:|\n| /api/tasks | 12 |\n| /api/notes | 3 |\n\n"
        "1. first\n2. second\n\n- bullet\n\n> quoted line\n"));
    check(html.contains(QLatin1String("<h2")), "heading renders as <h2>");
    check(html.contains(QLatin1String("<table")), "table renders as <table>");
    check(html.contains(QLatin1String("/api/tasks")) &&
              html.contains(QLatin1String(">12<")),
          "table cells keep their text");
    check(html.contains(QLatin1String("<ol")), "numbered list renders as <ol>");
    check(html.contains(QLatin1String("<ul")), "bullet list renders as <ul>");
    check(html.contains(QLatin1String("font-weight")), "bold renders");
    check(html.contains(QLatin1String("font-style:italic")), "italic renders");
    check(!html.contains(QLatin1String("| Route |")),
          "table source is not printed verbatim");
    check(!html.contains(QLatin1String("<body")) &&
              !html.contains(QLatin1String("<html")),
          "only the body fragment is returned");
}

void testCode()
{
    const QString html = toHtml(QStringLiteral(
        "Run `ninja -j6` then:\n\n```\nif (a < b && c > d) {}\n```\n"));
    check(html.contains(QLatin1String("ninja -j6")), "inline code text kept");
    check(html.contains(QLatin1String("<pre")), "fenced code renders as <pre>");
    check(html.contains(QLatin1String("a &lt; b &amp;&amp; c &gt; d")),
          "code block text is escaped");
}

void testLinks()
{
    const QString html = toHtml(QStringLiteral(
        "[docs](https://forkmesh.com/docs) and [mail](mailto:ops@forkmesh.com)"));
    check(html.contains(QLatin1String("href=\"https://forkmesh.com/docs\"")),
          "https link kept");
    check(html.contains(QLatin1String("href=\"mailto:ops@forkmesh.com\"")),
          "mailto link kept");
}

void testUnsafeInputIsNeutralised()
{
    const QString html = toHtml(QStringLiteral(
        "[click](javascript:alert(1)) [agent](forkmesh-agent:4) "
        "[img](forkmesh-task-image:abc) [file](file:///etc/passwd)\n\n"
        "<script>alert(1)</script>\n\n<img src=x onerror=alert(1)>\n\n"
        "![pic](file:///etc/passwd)\n"));
    check(!html.contains(QLatin1String("javascript:")), "javascript: link dropped");
    check(!html.contains(QLatin1String("href=\"forkmesh-agent:")),
          "internal agent scheme not linkable");
    check(!html.contains(QLatin1String("href=\"forkmesh-task-image:")),
          "internal image scheme not linkable");
    check(!html.contains(QLatin1String("href=\"file:")), "file: link dropped");
    check(html.contains(QLatin1String("click")) && html.contains(QLatin1String("agent")),
          "unsafe link labels stay as text");
    check(!html.contains(QLatin1String("<script")), "raw script tag not emitted");
    check(html.contains(QLatin1String("&lt;script&gt;")), "raw HTML shown as text");
    check(!html.contains(QLatin1String("<img")), "images never load");
    check(html.contains(QLatin1String("[image: file:///etc/passwd]")),
          "image replaced by a plain label");
}

void testSafeLinkPolicy()
{
    check(isSafeLink(QStringLiteral("https://forkmesh.com")), "https safe");
    check(isSafeLink(QStringLiteral("http://example.com/a?b=c")), "http safe");
    check(isSafeLink(QStringLiteral("mailto:a@b.c")), "mailto safe");
    check(!isSafeLink(QStringLiteral("javascript:alert(1)")), "javascript unsafe");
    check(!isSafeLink(QStringLiteral(" JavaScript:alert(1)")), "mixed-case javascript unsafe");
    check(!isSafeLink(QStringLiteral("data:text/html,x")), "data unsafe");
    check(!isSafeLink(QStringLiteral("file:///etc/passwd")), "file unsafe");
    check(!isSafeLink(QStringLiteral("forkmesh-agent:3")), "internal scheme unsafe");
    check(!isSafeLink(QStringLiteral("/relative")), "relative unsafe");
    check(!isSafeLink(QString()), "empty unsafe");
}

void testEmpty()
{
    check(toHtml(QString()).isEmpty(), "empty text renders nothing");
    check(toHtml(QStringLiteral(" \n\n ")).isEmpty(), "blank text renders nothing");
}

void testTaskItemToggle()
{
    const QString source = QStringLiteral(
        "Steps:\n\n- [ ] build\n- [x] test\n\n```\n- [ ] not a box\n```\n\n"
        "1. [ ] numbered\n");
    check(taskItemCount(source) == 3, "fenced code is not a task item");
    check(toggleTaskItem(source, 0) == QString(source).replace(
              QLatin1String("- [ ] build"), QLatin1String("- [x] build")),
          "first box ticks");
    check(toggleTaskItem(source, 1) == QString(source).replace(
              QLatin1String("- [x] test"), QLatin1String("- [ ] test")),
          "ticked box clears");
    check(toggleTaskItem(source, 2) == QString(source).replace(
              QLatin1String("1. [ ] numbered"), QLatin1String("1. [x] numbered")),
          "numbered box ticks past the fence");
    check(toggleTaskItem(source, 3).isNull(), "out-of-range index is refused");
    check(toggleTaskItem(source, -1).isNull(), "negative index is refused");
    check(toggleTaskItem(QStringLiteral("    - [ ] code\n"), 0).isNull(),
          "indented code is refused");
    check(toggleTaskItem(QStringLiteral("> - [ ] quoted\r\n"), 0) ==
              QStringLiteral("> - [x] quoted\n"),
          "quoted box ticks");
    check(taskItemCount(QString()) == 0, "empty text has no boxes");
}

void testTaskItemHitTest()
{
    const QString html = toHtml(QStringLiteral("Intro\n\n- [ ] one\n- [x] two\n- plain\n"));
    check(html.contains(QLatin1String("unchecked")) &&
              html.contains(QLatin1String("checked")),
          "boxes survive the HTML export");
    QTextDocument document;
    document.setHtml(QStringLiteral("<h3>Details</h3><div>%1</div>").arg(html));
    document.setTextWidth(400);
    QList<QTextBlock> boxes;
    QTextBlock plain;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        if (block.textList() &&
            block.blockFormat().marker() != QTextBlockFormat::MarkerType::NoMarker)
            boxes.append(block);
        else if (block.text() == QLatin1String("plain"))
            plain = block;
    }
    check(boxes.size() == 2, "the HTML keeps both boxes");
    auto hitIndex = [&document](const QTextBlock &block) {
        const QRectF rect = document.documentLayout()->blockBoundingRect(block);
        for (qreal x = 0; x < rect.left() + 80; x += 1) {
            const int index = taskItemIndexAt(
                &document, QPointF(x, rect.top() + rect.height() / 2));
            if (index >= 0)
                return index;
        }
        return -1;
    };
    if (boxes.size() == 2) {
        check(hitIndex(boxes.at(0)) == 0, "first box is hit as index 0");
        check(hitIndex(boxes.at(1)) == 1, "second box is hit as index 1");
    }
    check(!plain.isValid() || hitIndex(plain) == -1, "a plain bullet is not a box");
    const QRectF text = document.documentLayout()->blockBoundingRect(document.begin());
    check(taskItemIndexAt(&document, text.center()) == -1, "heading text is not a box");
    check(taskItemIndexAt(nullptr, QPointF()) == -1, "no document, no box");
}

}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    testTablesHeadingsAndLists();
    testCode();
    testLinks();
    testUnsafeInputIsNeutralised();
    testSafeLinkPolicy();
    testEmpty();
    testTaskItemToggle();
    testTaskItemHitTest();
    if (failures) {
        std::fprintf(stderr, "%d task markdown check(s) failed\n", failures);
        return 1;
    }
    std::printf("task markdown tests passed\n");
    return 0;
}
