#include "CopyFormatting.h"

#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QTextBrowser>
#include <QTextDocument>

#include <iostream>
#include <memory>

namespace {

int failures = 0;

void check(bool condition, const QString &message)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << message.toStdString() << '\n';
    ++failures;
}

void checkEqual(const QString &actual, const QString &expected, const QString &message)
{
    if (actual == expected)
        return;
    std::cerr << "FAIL: " << message.toStdString() << "\n  expected: "
              << expected.toStdString() << "\n  actual:   " << actual.toStdString() << '\n';
    ++failures;
}

void copyAll(QWidget *widget)
{
    widget->show();
    widget->setFocus();
    QKeyEvent selectAll(QEvent::KeyPress, Qt::Key_A, Qt::ControlModifier, QStringLiteral("a"));
    QApplication::sendEvent(widget, &selectAll);
    QKeyEvent copy(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier, QStringLiteral("c"));
    QApplication::sendEvent(widget, &copy);
}

QString formattedClipboardText()
{
    std::unique_ptr<QMimeData> copy(
        forkmesh::copyformat::formattedCopy(QGuiApplication::clipboard()->mimeData()));
    return copy ? copy->text() : QGuiApplication::clipboard()->text();
}

const char *const kMarkdown =
    "# Title\n\n"
    "First paragraph\nstill first.\n\n"
    "Second paragraph.\n\n"
    "- one\n"
    "- two\n"
    "  - nested\n"
    "- three\n\n"
    "1. first\n"
    "2. second\n\n"
    "```\n"
    "int main()\n"
    "    return 0;\n"
    "```\n\n"
    "| Name | Value |\n"
    "|---|---|\n"
    "| a | 1 |\n\n"
    "End.\n";

const char *const kExpected =
    "Title\n\n"
    "First paragraph still first.\n\n"
    "Second paragraph.\n\n"
    "- one\n"
    "- two\n"
    "  - nested\n"
    "- three\n\n"
    "1. first\n"
    "2. second\n\n"
    "int main()\n"
    "    return 0;\n\n"
    "Name\tValue\n"
    "a\t1\n\n"
    "End.";

void checkMarkdownLabel()
{
    QLabel label;
    label.setTextFormat(Qt::MarkdownText);
    label.setText(QString::fromUtf8(kMarkdown));
    label.setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    copyAll(&label);
    checkEqual(formattedClipboardText(), QString::fromUtf8(kExpected),
               QStringLiteral("markdown label copy keeps paragraphs, lists, code and tables"));
}

void checkRichLabel()
{
    QTextDocument source;
    source.setMarkdown(QString::fromUtf8(kMarkdown));
    QLabel label;
    label.setTextFormat(Qt::RichText);
    label.setText(source.toHtml());
    label.setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    copyAll(&label);
    checkEqual(formattedClipboardText(), QString::fromUtf8(kExpected),
               QStringLiteral("rich text label copy keeps paragraphs, lists, code and tables"));
    std::unique_ptr<QMimeData> copy(
        forkmesh::copyformat::formattedCopy(QGuiApplication::clipboard()->mimeData()));
    check(copy && copy->hasHtml() && copy->html().contains(QStringLiteral("nested")),
          QStringLiteral("formatted copy keeps the rich text for apps that paste HTML"));
    check(copy && !forkmesh::copyformat::formattedCopy(copy.get()),
          QStringLiteral("a formatted copy is not formatted again"));
}

void checkLineBreaks()
{
    QTextBrowser browser;
    browser.setHtml(QStringLiteral("<p>Dear team,<br>line two</p><p>Thanks</p>"
                                   "<ul><li>first<br>wrapped</li><li>second</li></ul>"));
    browser.selectAll();
    browser.copy();
    checkEqual(formattedClipboardText(),
               QStringLiteral("Dear team,\nline two\n\nThanks\n\n- first\n  wrapped\n- second"),
               QStringLiteral("line breaks and list item continuations survive a copy"));
}

void checkPlainTextUntouched()
{
    const QString text = QStringLiteral("alpha\n\n\n  beta\tgamma\n- not a list\n");
    QPlainTextEdit edit;
    edit.setPlainText(text);
    edit.selectAll();
    edit.copy();
    std::unique_ptr<QMimeData> copy(
        forkmesh::copyformat::formattedCopy(QGuiApplication::clipboard()->mimeData()));
    check(!copy, QStringLiteral("a plain text editor's copy is left as Qt made it"));
    checkEqual(QGuiApplication::clipboard()->text(), text,
               QStringLiteral("plain text copy is exact"));

    QGuiApplication::clipboard()->setText(QStringLiteral("one\n\ntwo"));
    check(!forkmesh::copyformat::formattedCopy(QGuiApplication::clipboard()->mimeData()),
          QStringLiteral("plain clipboard text is never rewritten"));
}

void checkSingleLineUntouched()
{
    QLabel label;
    label.setTextFormat(Qt::RichText);
    label.setText(QStringLiteral("<ul><li>only item</li></ul>"));
    label.setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    copyAll(&label);
    check(!forkmesh::copyformat::formattedCopy(QGuiApplication::clipboard()->mimeData()),
          QStringLiteral("a one-line selection is copied as is"));
}

void checkInstalledFormatter()
{
    forkmesh::copyformat::installClipboardFormatting(qApp);
    QLabel label;
    label.setTextFormat(Qt::MarkdownText);
    label.setText(QString::fromUtf8(kMarkdown));
    label.setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    copyAll(&label);
    if (!QGuiApplication::clipboard()->ownsClipboard()) {
        std::cout << "skip: this platform does not report clipboard ownership\n";
        return;
    }
    QElapsedTimer timer;
    timer.start();
    while (QGuiApplication::clipboard()->text() != QString::fromUtf8(kExpected) &&
           timer.elapsed() < 3000)
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    checkEqual(QGuiApplication::clipboard()->text(), QString::fromUtf8(kExpected),
               QStringLiteral("the installed formatter rewrites a copy on the clipboard"));
}

}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    checkMarkdownLabel();
    checkRichLabel();
    checkLineBreaks();
    checkPlainTextUntouched();
    checkSingleLineUntouched();
    checkInstalledFormatter();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "copy formatting tests passed\n";
    return 0;
}
