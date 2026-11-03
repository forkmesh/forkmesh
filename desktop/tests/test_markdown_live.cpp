#include "DesignTokens.h"
#include "MarkdownLiveHighlighter.h"

#include <QApplication>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>

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

QTextCharFormat formatAt(const QTextBlock &block, int position)
{
    for (const QTextLayout::FormatRange &range : block.layout()->formats()) {
        if (position >= range.start && position < range.start + range.length)
            return range.format;
    }
    return {};
}

bool hidden(const QTextCharFormat &format)
{
    return format.fontPointSize() == 1 && format.foreground().color().alpha() == 0;
}

}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTextDocument doc;
    doc.setPlainText(QStringLiteral(
        "# Title\n"
        "Some **bold** and _it_ with `code` and [link](https://x.test).\n"
        "```\n"
        "**not bold**\n"
        "```\n"
        "- item\n"
        "---\n"
        "> quote"));
    doc.setModified(false);
    const QString original = doc.toPlainText();

    MarkdownLiveHighlighter highlighter;
    highlighter.setActiveBlock(1);
    highlighter.setDocument(&doc);
    highlighter.rehighlight();

    const QTextBlock title = doc.findBlockByNumber(0);
    check(hidden(formatAt(title, 0)) && hidden(formatAt(title, 1)),
          "heading marks are hidden off the caret's line");
    check(formatAt(title, 2).fontWeight() == QFont::Bold &&
              formatAt(title, 2).fontPointSize() > 1.5 * doc.defaultFont().pointSizeF(),
          "a level 1 heading is bold and larger");

    const QTextBlock body = doc.findBlockByNumber(1);
    const QString text = body.text();
    const int bold = int(text.indexOf(QStringLiteral("**bold**")));
    check(!hidden(formatAt(body, bold)) &&
              formatAt(body, bold).foreground().color().alpha() > 0,
          "marks show on the caret's line");
    check(formatAt(body, bold + 2).fontWeight() == QFont::Bold, "bold text is bold");
    check(formatAt(body, int(text.indexOf(QStringLiteral("_it_"))) + 1).fontItalic(),
          "italic text is italic");
    check(formatAt(body, int(text.indexOf(QStringLiteral("`code`"))) + 1).fontFixedPitch(),
          "code spans are monospace");
    check(formatAt(body, int(text.indexOf(QStringLiteral("link")))).fontUnderline() &&
              formatAt(body, int(text.indexOf(QStringLiteral("link")))).foreground().color() ==
                  Theme::color(Theme::Role::Accent),
          "link text is underlined in the accent colour");

    highlighter.setActiveBlock(0);
    check(hidden(formatAt(doc.findBlockByNumber(1), bold)),
          "leaving a line hides its marks");
    check(!hidden(formatAt(doc.findBlockByNumber(0), 0)),
          "the new caret line shows its marks");

    const QTextBlock inside = doc.findBlockByNumber(3);
    check(formatAt(inside, 2).fontFixedPitch() &&
              formatAt(inside, 2).fontWeight() != QFont::Bold &&
              !hidden(formatAt(inside, 0)),
          "fenced code is monospace and not read as Markdown");
    check(doc.findBlockByNumber(2).userState() == 1 &&
              doc.findBlockByNumber(3).userState() == 1 &&
              doc.findBlockByNumber(4).userState() == 0,
          "the fence opens and closes");
    check(formatAt(doc.findBlockByNumber(5), 0).foreground().color() ==
              Theme::color(Theme::Role::Accent),
          "list markers use the accent colour");
    check(hidden(formatAt(doc.findBlockByNumber(6), 0)), "a rule is drawn as a hidden mark");
    check(hidden(formatAt(doc.findBlockByNumber(7), 0)) &&
              formatAt(doc.findBlockByNumber(7), 3).fontItalic(),
          "quotes are italic with the > hidden");

    check(doc.toPlainText() == original && !doc.isModified() && !doc.isUndoAvailable(),
          "styling never edits the text");

    highlighter.setDocument(nullptr);
    check(formatAt(doc.findBlockByNumber(0), 0).fontPointSize() != 1,
          "leaving Live clears the styling");

    if (failures == 0)
        std::puts("markdown live: all checks passed");
    return failures == 0 ? 0 : 1;
}
